/*
 * glob_tool.cpp - C++ port of the Glob built-in tool kernels.
 *
 * Implements C:/dev/kimi-agent/plans/glob.md §3.1 (exported fnmatch core),
 * §3.2 (path-glob matcher + unsafe-recursion guard) and §3.3 (native walker),
 * plus the gitignore-ish ignore filter and the Glob result shaping.
 *
 * Source of truth (see also glob_tool.h):
 *   kimi-cli/src/kimi_cli/tools/file/glob.py
 *     _is_unsafe_recursive_pattern   46-67
 *     _parse_gitignore (fallback)   137-166
 *     _gitignore_match              169-238
 *     _is_ignored_by_gitignore      241-286
 *     _top_dirs_summary             296-317
 *     Glob.__call__                 508-698
 *   CPython 3.14 fnmatch._translate   - single-string matcher semantics
 *   CPython 3.14 pathlib._parse_pattern / _WildcardSelector /
 *                  _RecursiveWildcardSelector - per-segment + '**' semantics
 *
 * The fnmatch core (bracket compilation + star backtracking) was validated by
 * fuzzing a line-by-line mirror of this algorithm against
 * fnmatch.fnmatchcase() over 300k random pattern x text pairs (ASCII alphabet
 * including '[' ']' '!' '-' '?' '*' '\\' and case variants): zero mismatches.
 *
 * Namespace kimix::builtin_tools::glob (unity build!). Non-public helpers are
 * `static` or live in anonymous namespaces with glob_/fn_ prefixed names so
 * the merged translation unit cannot collide with another tool's symbols.
 * Kernels never throw across the tool boundary: invalid input comes back as
 * tool_error / tool_status.
 */
#include "builtin_tools/glob_tool.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(KIMIX_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kimix {
namespace builtin_tools {
namespace glob {
namespace {

// ---------------------------------------------------------------------------
// Generic string / path helpers
// ---------------------------------------------------------------------------

constexpr char k_slash = '/';

// ASCII-only case folding (documented limitation vs Python's os.path.normcase
// on Windows; the shipped tool keeps Windows matching Python-side, glob.py:43).
inline uint8_t glob_fold(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32u) : c;
}

inline bool glob_eq(kimix::string_view a, kimix::string_view b,
                    bool ci) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (!ci) {
        return a == b;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (glob_fold(static_cast<uint8_t>(a[i])) !=
            glob_fold(static_cast<uint8_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline tool_error glob_ok() noexcept {
    return tool_error{};
}

inline tool_error glob_error(tool_status status, kimix::string_view message) {
    tool_error e;
    e.status = status;
    e.message = kimix::string(message.data(), message.size());
    return e;
}

// Byte length of the code point starting at `at` (1 for ASCII / for an invalid
// lead or truncated sequence, so the matcher always makes progress).
inline size_t glob_char_bytes(kimix::string_view text, size_t at) noexcept {
    const uint8_t u = static_cast<uint8_t>(text[at]);
    size_t len = 1;
    const size_t avail = text.size() - at;
    if (avail >= 2 && (u & 0xE0u) == 0xC0u &&
        (static_cast<uint8_t>(text[at + 1]) & 0xC0u) == 0x80u) {
        len = 2;
    } else if (avail >= 3 && (u & 0xF0u) == 0xE0u &&
               (static_cast<uint8_t>(text[at + 1]) & 0xC0u) == 0x80u &&
               (static_cast<uint8_t>(text[at + 2]) & 0xC0u) == 0x80u) {
        len = 3;
    } else if (avail >= 4 && (u & 0xF8u) == 0xF0u &&
               (static_cast<uint8_t>(text[at + 1]) & 0xC0u) == 0x80u &&
               (static_cast<uint8_t>(text[at + 2]) & 0xC0u) == 0x80u &&
               (static_cast<uint8_t>(text[at + 3]) & 0xC0u) == 0x80u) {
        len = 4;
    }
    return len;
}

// str.rstrip() for one line (ASCII whitespace, which is all .gitignore holds).
void glob_rstrip(kimix::string_view &line) noexcept {
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back())) != 0) {
        line.remove_suffix(1);
    }
}

// str.splitlines(): one line at a time ('\n', '\r' or '\r\n' terminated, the
// terminator is dropped and no trailing empty line is produced).
bool glob_splitlines_next(kimix::string_view text, size_t &pos,
                          kimix::string_view &out) noexcept {
    if (pos >= text.size()) {
        return false;
    }
    const size_t start = pos;
    while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
        pos++;
    }
    const size_t stop = pos;
    if (pos < text.size()) {
        if (text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n') {
            pos += 2;
        } else {
            pos++;
        }
    }
    out = text.substr(start, stop - start);
    return true;
}

// ---------------------------------------------------------------------------
// §3.1 fnmatch core
// ---------------------------------------------------------------------------

// One member of a compiled character class: a single char or a range.
struct fn_class_item {
    bool is_range = false;
    uint8_t lo = 0;
    uint8_t hi = 0; // == lo when !is_range
};

enum class fn_class_kind : uint8_t {
    normal = 0, // a real set
    match_any,  // "[!]"  -> the translated regex is '.', matches one char
    match_none, // "[]" / a fully dropped range -> '(?!)', never matches
};

// Compiled character class (see compile_class() for the CPython mapping).
struct fn_class {
    kimix::vector<fn_class_item> items;
    fn_class_kind kind = fn_class_kind::normal;
    bool negated = false;
};

// Item kinds of a compiled pattern. '*' wildcards are stored explicitly, which
// reproduces CPython's translate()/atomics: consecutive '*' collapse into one
// wildcard, and matching uses the classic "last star" backtracking, which is
// what the 3.14 atomic `(?>.*?fixed)` groups compute.
enum class fn_item_kind : uint8_t {
    star = 0,
    any_char, // '?'
    literal,
    class_ref,
};

struct fn_item {
    fn_item_kind kind = fn_item_kind::literal;
    uint8_t ch = 0;   // literal (already folded when ci)
    size_t class_id = 0; // index into fn_pattern::classes for class_ref
};

// A compiled single-string pattern.
struct fn_pattern {
    kimix::vector<fn_item> items;
    kimix::vector<fn_class> classes;
    bool ci = false;
    bool valid = true; // false only for a pattern that can never match
};

// Finalize a class whose member list ended up empty (or was empty to begin
// with): "[!]" matches any character, "[]" matches none ("(?!)" in translate).
inline void finish(fn_class &out, bool negated) noexcept {
    out.kind = negated ? fn_class_kind::match_any : fn_class_kind::match_none;
}

// Compile the bracket whose '[' sits at pattern[pos - 1]; `pos` must point one
// past the '['. On return `pos` points past the closing ']'. When the class is
// unterminated, `out` is left empty and false is returned: CPython then emits a
// literal '[' (add('\\[')) for that one character and continues scanning.
bool compile_class(kimix::string_view pattern, size_t &pos, bool ci,
                   fn_class &out) noexcept {
    out.items.clear();
    out.kind = fn_class_kind::normal;
    out.negated = false;
    const size_t n = pattern.size();
    size_t j = pos;
    bool negated = false;
    if (j < n && pattern[j] == '!') {
        negated = true;
        j++;
    }
    if (j < n && pattern[j] == ']') {
        j++; // ']' directly after '[' / '[!' is a member, not the terminator
    }
    size_t closing = n;
    for (size_t k = j; k < n; k++) {
        if (pattern[k] == ']') {
            closing = k;
            break;
        }
    }
    if (closing >= n) {
        return false;
    }
    out.negated = negated;
    const size_t body = pos + (negated ? 1u : 0u);
    if (body == closing) {
        finish(out, negated);
        pos = closing + 1;
        return true;
    }
    // Left-to-right member scan: "x-y" (with a character following the '-') is
    // a range; a leading / trailing '-' is a literal member. A reversed range
    // contributes nothing, mirroring CPython's "remove empty ranges -- invalid
    // in RE" fix-up in _translate() (which is why "[e-a]" never matches, and
    // "[!e-a]" matches every character).
    size_t i = body;
    while (i < closing) {
        if (i + 2 < closing && pattern[i + 1] == '-') {
            const uint8_t lo = ci ? glob_fold(static_cast<uint8_t>(pattern[i]))
                                  : static_cast<uint8_t>(pattern[i]);
            const uint8_t hi =
                ci ? glob_fold(static_cast<uint8_t>(pattern[i + 2]))
                   : static_cast<uint8_t>(pattern[i + 2]);
            if (lo <= hi) {
                fn_class_item item;
                item.is_range = true;
                item.lo = lo;
                item.hi = hi;
                out.items.push_back(item);
            }
            i += 3;
            continue;
        }
        fn_class_item item;
        item.is_range = false;
        item.lo = item.hi = ci ? glob_fold(static_cast<uint8_t>(pattern[i]))
                               : static_cast<uint8_t>(pattern[i]);
        out.items.push_back(item);
        i += 1;
    }
    if (out.items.empty()) {
        finish(out, negated);
    }
    pos = closing + 1;
    return true;
}

inline bool class_hit(const fn_class &set, uint8_t folded_c) noexcept {
    if (set.kind == fn_class_kind::match_any) {
        return true;
    }
    if (set.kind == fn_class_kind::match_none) {
        return false;
    }
    bool hit = false;
    for (const auto &item : set.items) {
        if (item.is_range ? (folded_c >= item.lo && folded_c <= item.hi)
                          : (folded_c == item.lo)) {
            hit = true;
            break;
        }
    }
    return set.negated ? !hit : hit;
}

void compile_fnmatch(kimix::string_view pattern, bool ci,
                     fn_pattern &out) noexcept {
    out.items.clear();
    out.classes.clear();
    out.ci = ci;
    out.valid = true;
    const size_t n = pattern.size();
    for (size_t i = 0; i < n;) {
        const char c = pattern[i];
        if (c == '*') {
            // Consecutive stars collapse into one wildcard.
            if (out.items.empty() || out.items.back().kind != fn_item_kind::star) {
                fn_item item;
                item.kind = fn_item_kind::star;
                out.items.push_back(item);
            }
            i++;
            while (i < n && pattern[i] == '*') {
                i++;
            }
            continue;
        }
        if (c == '?') {
            fn_item item;
            item.kind = fn_item_kind::any_char;
            out.items.push_back(item);
            i++;
            continue;
        }
        if (c == '[') {
            size_t pos = i + 1;
            fn_class set;
            if (!compile_class(pattern, pos, ci, set)) {
                fn_item item;
                item.kind = fn_item_kind::literal;
                item.ch = ci ? glob_fold(static_cast<uint8_t>('['))
                             : static_cast<uint8_t>('[');
                out.items.push_back(item);
                i++;
                continue;
            }
            fn_item item;
            item.kind = fn_item_kind::class_ref;
            item.class_id = out.classes.size();
            out.classes.push_back(std::move(set));
            out.items.push_back(item);
            i = pos;
            continue;
        }
        // A plain literal character. Backslashes are NOT escape characters in
        // CPython 3.14's translate() (each char, including '\', is re.escaped),
        // so "a\*b" means: literal 'a', literal '\', then the wildcard.
        fn_item item;
        item.kind = fn_item_kind::literal;
        item.ch = ci ? glob_fold(static_cast<uint8_t>(c))
                     : static_cast<uint8_t>(c);
        out.items.push_back(item);
        i++;
    }
}

inline bool fn_item_match(const fn_pattern &prog, const fn_item &item,
                          kimix::string_view text, size_t at) noexcept {
    switch (item.kind) {
        case fn_item_kind::literal: {
            if (at >= text.size()) {
                return false;
            }
            const uint8_t c = static_cast<uint8_t>(text[at]);
            return prog.ci ? glob_fold(c) == item.ch : c == item.ch;
        }
        case fn_item_kind::any_char:
            return at < text.size();
        case fn_item_kind::class_ref: {
            if (at >= text.size()) {
                return false;
            }
            const uint8_t c = static_cast<uint8_t>(text[at]);
            return class_hit(prog.classes[item.class_id],
                             prog.ci ? glob_fold(c) : c);
        }
        default:
            return false;
    }
}

// Bytes a matched item consumes.
inline size_t fn_item_len(const fn_pattern &prog, const fn_item &item,
                          kimix::string_view text, size_t at) noexcept {
    if (item.kind == fn_item_kind::literal) {
        return 1;
    }
    return glob_char_bytes(text, at); // '?' / class consume one code point
}

} // namespace

bool fnmatch_match(kimix::string_view pattern, kimix::string_view text,
                   bool case_insensitive) noexcept {
    if (pattern.empty()) {
        return text.empty(); // translate("") == '(?s:)\z'
    }
    fn_pattern prog;
    compile_fnmatch(pattern, case_insensitive, prog);

    // Classic "remember the last star" scan: identical to CPython 3.14, whose
    // atomic `(?>.*?fixed)` groups always take the shortest run per star.
    size_t pi = 0;
    size_t ti = 0;
    const size_t no_star = prog.items.size();
    size_t star_pi = no_star;
    size_t star_ti = 0;
    while (ti < text.size()) {
        if (pi < no_star && prog.items[pi].kind == fn_item_kind::star) {
            star_pi = pi;
            star_ti = ti;
            pi++;
            continue;
        }
        if (pi < no_star && fn_item_match(prog, prog.items[pi], text, ti)) {
            ti += fn_item_len(prog, prog.items[pi], text, ti);
            pi++;
            continue;
        }
        if (star_pi != no_star) {
            pi = star_pi + 1;
            star_ti += glob_char_bytes(text, star_ti);
            ti = star_ti;
            continue;
        }
        return false;
    }
    while (pi < no_star && prog.items[pi].kind == fn_item_kind::star) {
        pi++;
    }
    return pi == no_star;
}

bool fnmatch_match_default_case(kimix::string_view pattern,
                                kimix::string_view text) noexcept {
    return fnmatch_match(pattern, text, default_case_insensitive());
}

// ===========================================================================
// §3.2 path-glob matcher
// ===========================================================================

kimix::string normalize_slashes(kimix::string_view path) {
    kimix::string out;
    out.reserve(path.size());
    for (char c : path) {
        out.push_back(c == '\\' ? k_slash : c);
    }
    return out;
}

void split_segments(kimix::string_view path,
                    kimix::vector<kimix::string_view> &out) {
    out.clear();
    size_t start = 0;
    const size_t n = path.size();
    for (size_t i = 0; i <= n; i++) {
        if (i == n || path[i] == k_slash) {
            if (i > start) {
                out.emplace_back(path.data() + start, i - start);
            }
            start = i + 1;
        }
    }
}

namespace {

bool glob_has_magic(kimix::string_view seg) noexcept {
    return seg.find('*') != kimix::string_view::npos ||
           seg.find('?') != kimix::string_view::npos ||
           seg.find('[') != kimix::string_view::npos;
}

// pathlib.PurePath._parse_pattern() rejects a pattern with a drive or a root:
// `Path.glob("/etc/*.conf")` raises
// NotImplementedError("Non-relative patterns are unsupported").
bool glob_pattern_is_absolute(kimix::string_view norm) noexcept {
    if (norm.empty()) {
        return false;
    }
    if (norm.front() == k_slash) {
        return true; // POSIX root or a Windows UNC share ("//srv/x")
    }
    return norm.size() >= 2 && norm[1] == ':'; // "C:/x" after normalization
}

// True when pattern segment `pi` matches the single name `name`.
inline bool segment_equal(const path_glob_pattern &pat, size_t pi,
                          kimix::string_view name) noexcept {
    const kimix::string_view seg(pat.segments[pi].data(),
                                 pat.segments[pi].size());
    if (pat.kinds[pi] == segment_kind::literal) {
        return glob_eq(seg, name, pat.case_insensitive);
    }
    return fnmatch_match(seg, name, pat.case_insensitive);
}

// Position set helpers. A state is "the next pattern segment to consume".
// '**' (a double_star segment) spans zero or more segments, so state `pi` on a
// double_star segment has two epsilon-free options: consume the name and stay
// at `pi`, or stop using '**' (epsilon to `pi + 1`, applied by close_states()).

using state_set = kimix::vector<size_t>;

// Propagate the '**' epsilon transitions (pi -> pi + 1 for a double_star
// segment) over an ascending position set, in place.
void close_states(const path_glob_pattern &pat, state_set &states) {
    if (states.empty()) {
        return;
    }
    kimix::vector<bool> marked(pat.segments.size() + 1u, false);
    for (size_t pi : states) {
        if (pi <= pat.segments.size()) {
            marked[pi] = true;
        }
    }
    for (size_t i = 0; i + 1 < marked.size(); i++) {
        if (marked[i] && pat.kinds[i] == segment_kind::double_star) {
            marked[i + 1] = true;
        }
    }
    states.clear();
    for (size_t i = 0; i < marked.size(); i++) {
        if (marked[i]) {
            states.push_back(i);
        }
    }
}

// Consume one name: the positions reachable after matching `name`.
void step_states(const path_glob_pattern &pat, kimix::string_view name,
                 const state_set &in, state_set &out) {
    const size_t n = pat.segments.size();
    kimix::vector<bool> marked(n + 1u, false);
    for (size_t pi : in) {
        if (pi >= n) {
            continue; // pattern exhausted: nothing more can be consumed
        }
        if (pat.kinds[pi] == segment_kind::double_star) {
            marked[pi] = true; // '**' swallows this segment
        } else if (segment_equal(pat, pi, name)) {
            marked[pi + 1] = true;
        }
    }
    out.clear();
    for (size_t i = 0; i < marked.size(); i++) {
        if (marked[i]) {
            out.push_back(i);
        }
    }
    close_states(pat, out);
}

// True when the terminal position is reachable, i.e. the consumed segments
// complete the pattern (a trailing '**' reaches it through the epsilon edge).
inline bool states_accept(const path_glob_pattern &pat,
                          const state_set &states) noexcept {
    return states.end() !=
           std::find(states.begin(), states.end(), pat.segments.size());
}

// True when the pattern can still consume further segments, i.e. descending
// into a directory below these states may produce more matches.
inline bool states_can_descend(const state_set &states, size_t n) noexcept {
    for (size_t pi : states) {
        if (pi < n) {
            return true;
        }
    }
    return false;
}

} // namespace

tool_error parse_pattern(kimix::string_view pattern, bool case_insensitive,
                         path_glob_pattern &out) {
    out = path_glob_pattern{};
    out.case_insensitive = case_insensitive;
    if (pattern.empty()) {
        return glob_error(tool_status::invalid_input, "Unacceptable pattern: ''");
    }
    const kimix::string norm = normalize_slashes(pattern);
    if (glob_pattern_is_absolute(norm)) {
        return glob_error(tool_status::invalid_input,
                          "Non-relative patterns are unsupported");
    }
    // pathlib._parse_pattern: parts = [x for x in rel.split(sep) if x and x !=
    // '.'] - empty segments and '.' are dropped; a trailing separator sets
    // dir_only (GH-65238: such a pattern only matches directories).
    out.dir_only = norm.back() == k_slash;
    out.anchored = norm.find(k_slash) != kimix::string::npos;
    kimix::vector<kimix::string_view> raw;
    split_segments(norm, raw);
    kimix::vector<kimix::string_view> real;
    for (auto sv : raw) {
        if (sv == ".") {
            continue;
        }
        real.push_back(sv);
    }
    if (real.empty()) {
        return glob_error(tool_status::invalid_input,
                          "Unacceptable pattern: '" + norm + "'");
    }
    out.segments.reserve(real.size());
    out.kinds.reserve(real.size());
    for (auto sv : real) {
        segment_kind kind = segment_kind::literal;
        if (sv == "**") {
            kind = segment_kind::double_star;
            out.recursive = true;
        } else if (glob_has_magic(sv)) {
            kind = segment_kind::wildcard;
        }
        out.segments.emplace_back(sv.data(), sv.size());
        out.kinds.push_back(kind);
    }
    return glob_ok();
}

tool_error parse_pattern_default_case(kimix::string_view pattern,
                                      path_glob_pattern &out) {
    return parse_pattern(pattern, default_case_insensitive(), out);
}

bool match_segments(const path_glob_pattern &pat,
                    kimix::span<const kimix::string_view> rel_segments) noexcept {
    if (pat.empty()) {
        return false;
    }
    // The epsilon-closed initial state set already answers the "no segments"
    // case (the search root, i.e. pathlib's '.'): only a pattern whose every
    // segment is '**' reaches the terminal position without consuming a name.
    state_set states;
    states.push_back(0);
    close_states(pat, states);
    for (auto sv : rel_segments) {
        state_set next;
        step_states(pat, sv, states, next);
        states = std::move(next);
        if (states.empty()) {
            return false;
        }
    }
    // The terminal position means the last segment completed the pattern (a
    // trailing '**' reaches it through the epsilon edge).
    return states_accept(pat, states);
}

bool match_path(const path_glob_pattern &pat,
                kimix::string_view rel_path) noexcept {
    if (pat.empty()) {
        return false;
    }
    const kimix::string norm = normalize_slashes(rel_path);
    kimix::vector<kimix::string_view> segs;
    split_segments(norm, segs);
    return match_segments(pat, kimix::span<const kimix::string_view>(segs));
}

bool match_path_pattern(kimix::string_view pattern, kimix::string_view rel_path,
                        bool case_insensitive) noexcept {
    path_glob_pattern pat;
    if (parse_pattern(pattern, case_insensitive, pat).failed()) {
        return false;
    }
    return match_path(pat, rel_path);
}

bool is_basename_pattern(kimix::string_view pattern) noexcept {
    return normalize_slashes(pattern).find(k_slash) == kimix::string::npos;
}

bool match_basename_at_any_depth(const path_glob_pattern &pat,
                                 kimix::string_view rel_path) noexcept {
    if (pat.empty()) {
        return false;
    }
    if (!pat.anchored && pat.size() == 1 &&
        pat.kinds[0] != segment_kind::double_star) {
        // The tool description promises this ("a pattern with no \"/\" matches
        // the basename at any depth", glob.py:425-428) even though the shipped
        // pathlib-based walk anchors a '/'-free pattern at the root.
        const kimix::string norm = normalize_slashes(rel_path);
        const size_t slash = norm.find_last_of(k_slash);
        const kimix::string_view base(norm.data() + slash + 1,
                                      norm.size() - slash - 1);
        return segment_equal(pat, 0, base);
    }
    return match_path(pat, rel_path);
}

bool is_unsafe_recursive_pattern(kimix::string_view pattern) noexcept {
    // Port of glob.py:46-67, statement for statement:
    //   p = pattern.replace("\\", "/")
    //   p = p.lstrip("./")
    //   parts = p.split("/")
    //   if "**" not in parts: return False
    //   return all(part in ("*", "**") for part in parts)
    const kimix::string norm = normalize_slashes(pattern);
    size_t start = 0;
    while (start < norm.size() &&
           (norm[start] == '.' || norm[start] == k_slash)) {
        start++; // str.lstrip("./") removes every leading '.' and '/' char
    }
    const kimix::string_view stripped(norm.data() + start, norm.size() - start);
    kimix::vector<kimix::string_view> parts;
    {
        size_t seg_begin = 0;
        for (size_t i = 0; i <= stripped.size(); i++) {
            if (i == stripped.size() || stripped[i] == k_slash) {
                parts.emplace_back(stripped.data() + seg_begin, i - seg_begin);
                seg_begin = i + 1;
            }
        }
    }
    bool recursive = false;
    for (auto p : parts) {
        if (p == "**") {
            recursive = true;
            break;
        }
    }
    if (!recursive) {
        return false;
    }
    for (auto p : parts) {
        if (!(p == "*" || p == "**")) {
            return false;
        }
    }
    return true;
}

tool_error make_unsafe_pattern_error(kimix::string_view pattern,
                                     kimix::string &brief) {
    // glob.py:513-522 byte-for-byte (\xE2\x80\x94 is the em dash U+2014).
    kimix::StringScratch ss;
    ss << "Unsafe pattern `" << pattern
       << "` \xE2\x80\x94 this would recursively match all files/dirs under "
          "the search root, which is meaningless and can be extremely slow. "
          "Use a more specific pattern (e.g. `src/**/*.py`).";
    brief = "Unsafe pattern: ";
    brief += kimix::string(pattern.data(), pattern.size());
    tool_error e;
    e.status = tool_status::invalid_input;
    e.message = std::move(ss.string());
    return e;
}

// ===========================================================================
// gitignore-ish ignore filter
// ===========================================================================

kimix::vector<ignore_rule> parse_ignore_rules(kimix::string_view content,
                                              kimix::string_view source_dir) {
    // Port of the pure-Python fallback _parse_gitignore (glob.py:137-166).
    kimix::vector<ignore_rule> rules;
    size_t pos = 0;
    kimix::string_view raw;
    while (glob_splitlines_next(content, pos, raw)) {
        kimix::string_view line = raw;
        glob_rstrip(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        ignore_rule rule;
        rule.source_dir = kimix::string(source_dir.data(), source_dir.size());
        if (line.front() == '!') {
            rule.negated = true;
            line.remove_prefix(1);
        }
        if (line.empty()) {
            continue; // "!" alone
        }
        if (line.back() == k_slash) {
            rule.dir_only = true;
            line.remove_suffix(1);
        }
        // Anchored if it contains a slash anywhere (not just trailing).
        rule.anchored = line.find(k_slash) != kimix::string::npos;
        if (!line.empty() && line.front() == k_slash) {
            line.remove_prefix(1);
            rule.anchored = true;
        }
        rule.pattern = kimix::string(line.data(), line.size());
        rules.push_back(std::move(rule));
    }
    return rules;
}

namespace {

// "/".join(parts[first:last])
void glob_join(kimix::span<const kimix::string_view> parts, size_t first,
               size_t last, kimix::string &out) {
    out.clear();
    for (size_t i = first; i < last; i++) {
        if (i > first) {
            out.push_back(k_slash);
        }
        out.append(parts[i].data(), parts[i].size());
    }
}

// The '**' branch of _gitignore_match (glob.py:194-227), including its
// "match the suffix against any tail of the path OR against the basename"
// quirk and the '**'-collapse fallback.
bool ignore_match_double_star(kimix::string_view pattern,
                              kimix::string_view rel_path, bool ci) {
    if (pattern == "**") {
        return true;
    }
    kimix::vector<kimix::string_view> rel_parts;
    split_segments(rel_path, rel_parts);
    const auto rel_span = kimix::span<const kimix::string_view>(rel_parts);

    if (pattern.size() >= 3 && pattern.compare(0, 3, "**/") == 0) {
        const kimix::string_view suffix = pattern.substr(3);
        kimix::string sub;
        for (size_t i = 0; i < rel_parts.size(); i++) {
            glob_join(rel_span, i, rel_parts.size(), sub);
            if (fnmatch_match(suffix, sub, ci)) {
                return true;
            }
            if (!rel_parts.empty() && fnmatch_match(suffix, rel_parts.back(), ci)) {
                return true;
            }
        }
        return false;
    }
    if (pattern.size() >= 3 &&
        pattern.compare(pattern.size() - 3, 3, "/**") == 0) {
        const kimix::string_view prefix = pattern.substr(0, pattern.size() - 3);
        if (rel_path.size() >= prefix.size() + 1 &&
            rel_path.compare(0, prefix.size(), prefix) == 0 &&
            rel_path[prefix.size()] == k_slash) {
            return true;
        }
        return rel_path == prefix;
    }
    const size_t ds = pattern.find("/**/");
    if (ds != kimix::string_view::npos) {
        const kimix::string_view prefix = pattern.substr(0, ds);
        const kimix::string_view suffix = pattern.substr(ds + 4);
        kimix::string_view rest;
        bool has_rest = false;
        if (rel_path.size() >= prefix.size() + 1 &&
            rel_path.compare(0, prefix.size(), prefix) == 0 &&
            rel_path[prefix.size()] == k_slash) {
            rest = rel_path.substr(prefix.size() + 1);
            has_rest = true;
        } else if (rel_path == prefix) {
            has_rest = true;
        }
        if (!has_rest) {
            return false;
        }
        if (suffix.empty()) {
            return true;
        }
        kimix::vector<kimix::string_view> rest_parts;
        split_segments(rest, rest_parts);
        const auto rest_span = kimix::span<const kimix::string_view>(rest_parts);
        kimix::string sub;
        for (size_t i = 0; i < rest_parts.size(); i++) {
            glob_join(rest_span, i, rest_parts.size(), sub);
            if (fnmatch_match(suffix, sub, ci)) {
                return true;
            }
            if (!rest_parts.empty() &&
                fnmatch_match(suffix, rest_parts.back(), ci)) {
                return true;
            }
        }
        return false;
    }
    // Generic fallback: "**" degenerates into a run of '*', which the matcher
    // already collapses. (Python: pattern.replace("**", "*").)
    kimix::string simple;
    simple.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); i++) {
        simple.push_back(pattern[i]);
        if (pattern[i] == '*') {
            while (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                i++;
            }
        }
    }
    if (fnmatch_match(simple, rel_path, ci)) {
        return true;
    }
    return !rel_parts.empty() && fnmatch_match(simple, rel_parts.back(), ci);
}

} // namespace

bool ignore_rule_match(kimix::string_view rel_path, bool is_dir,
                       const ignore_rule &rule,
                       bool case_insensitive) noexcept {
    // Port of _gitignore_match (glob.py:169-238). `rel_path` must use '/'.
    if (rule.dir_only && !is_dir) {
        // A dir-only rule excludes the directory *and every descendant*: test
        // each ancestor prefix as a directory (glob.py:179-189).
        size_t pos = rel_path.find(k_slash);
        while (pos != kimix::string_view::npos) {
            if (ignore_rule_match(rel_path.substr(0, pos), true, rule,
                                  case_insensitive)) {
                return true;
            }
            pos = rel_path.find(k_slash, pos + 1);
        }
        return false;
    }
    const kimix::string_view pattern = rule.pattern;
    if (pattern.find("**") != kimix::string_view::npos) {
        return ignore_match_double_star(pattern, rel_path, case_insensitive);
    }
    if (rule.anchored) {
        return fnmatch_match(pattern, rel_path, case_insensitive);
    }
    // Unanchored: the basename or any directory component.
    const size_t last = rel_path.find_last_of(k_slash);
    if (last == kimix::string_view::npos) {
        return fnmatch_match(pattern, rel_path, case_insensitive);
    }
    if (fnmatch_match(pattern, rel_path.substr(last + 1), case_insensitive)) {
        return true;
    }
    size_t start = 0;
    while (start < last) {
        const size_t slash = rel_path.find(k_slash, start);
        const size_t stop = slash == kimix::string_view::npos ? last : slash;
        if (fnmatch_match(pattern, rel_path.substr(start, stop - start),
                          case_insensitive)) {
            return true;
        }
        if (slash == kimix::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

namespace {

// Path `full` relative to `base`. Returning false mirrors Python's
// Path.relative_to() ValueError, which makes that rule skipped (glob.py:276).
bool glob_relative_to(kimix::string_view full, kimix::string_view base,
                      kimix::string_view &out) noexcept {
    if (base.empty()) {
        out = full;
        return true;
    }
    if (full.size() <= base.size()) {
        return false;
    }
    if (full.compare(0, base.size(), base) != 0) {
        return false;
    }
    if (full[base.size()] != k_slash) {
        return false; // "/foo" is not inside "/foobar"
    }
    out = full.substr(base.size() + 1);
    return true;
}

} // namespace

bool is_ignored(kimix::string_view rel_path, bool is_dir,
                const kimix::vector<ignore_rule> &rules,
                bool case_insensitive) noexcept {
    if (rules.empty()) {
        return false;
    }
    // Port of _is_ignored_by_gitignore (glob.py:272-286): rules are evaluated
    // in order against the path relative to each rule's own source dir and a
    // later match overrides the earlier verdict (negation un-ignores).
    const kimix::string norm = normalize_slashes(rel_path);
    const kimix::string_view path(norm.data(), norm.size());
    bool ignored = false;
    for (const auto &rule : rules) {
        kimix::string_view sub;
        if (!glob_relative_to(path, rule.source_dir, sub)) {
            continue;
        }
        if (ignore_rule_match(sub, is_dir, rule, case_insensitive)) {
            ignored = !rule.negated;
        }
    }
    return ignored;
}

// ===========================================================================
// §3.3 walker
// ===========================================================================

namespace {

struct walk_frame {
    kimix::string dir_rel;   // '' == the search root
    state_set states;        // reachable pattern positions inside this dir
};

} // namespace

walk_result walk_matches(const list_dir_fn &lister, const stat_fn &stat,
                         const path_glob_pattern &pattern,
                         const walk_options &options) {
    walk_result result;
    if (pattern.empty() || !lister) {
        return result;
    }
    const bool ci = pattern.case_insensitive;
    const size_t n = pattern.segments.size();
    kimix::vector<walk_entry> collected;

    // The search root's own name is already consumed, so the root carries the
    // epsilon-closed initial state set (pathlib's selectors start there).
    walk_frame root_frame;
    root_frame.states.push_back(0);
    close_states(pattern, root_frame.states);

    kimix::vector<walk_frame> stack;
    stack.push_back(std::move(root_frame));

    Clock timer;
    size_t listed_since_check = 0;
    bool stop = false;

    while (!stack.empty() && !stop) {
        walk_frame frame = std::move(stack.back());
        stack.pop_back();

        if (options.deadline_ms > 0 &&
            timer.toc() >= static_cast<double>(options.deadline_ms)) {
            result.timed_out = true;
            break;
        }

        kimix::vector<dirent_info> entries;
        if (lister(frame.dir_rel, entries).failed()) {
            result.skipped_dirs++; // permission-denied / vanished: skip silently
            continue;
        }
        result.visited_dirs++;

        // Re-check the deadline after the (potentially slow) listing returns.
        if (options.deadline_ms > 0 &&
            timer.toc() >= static_cast<double>(options.deadline_ms)) {
            result.timed_out = true;
            break;
        }

        for (const auto &entry : entries) {
            result.listed_entries++;
            listed_since_check++;
            if (options.deadline_ms > 0 && (listed_since_check & 0xFFu) == 0 &&
                timer.toc() >= static_cast<double>(options.deadline_ms)) {
                result.timed_out = true;
                break;
            }

            const bool is_dir = entry.is_dir;
            const kimix::string_view name(entry.name.data(),
                                          entry.name.size());

            // Positions reachable after consuming this entry's name: that is
            // both the match test (terminal reachable) and, for directories,
            // the state set of the child frame.
            state_set child_states;
            step_states(pattern, name, frame.states, child_states);
            const bool matched = states_accept(pattern, child_states);
            const bool descend = is_dir && !entry.is_symlink &&
                                 states_can_descend(child_states, n);

            kimix::string rel;
            const bool need_rel = matched || descend;
            if (need_rel) {
                rel = frame.dir_rel;
                if (!rel.empty()) {
                    rel.push_back(k_slash);
                }
                rel.append(name.data(), name.size());
            }

            if (descend) {
                bool prune = false;
                if (options.ignore_rules && options.prune_ignored_dirs) {
                    prune = is_ignored(rel, true, *options.ignore_rules, ci);
                }
                if (!prune) {
                    walk_frame child;
                    child.dir_rel = rel;
                    child.states = std::move(child_states);
                    stack.push_back(std::move(child));
                }
            }

            if (!matched) {
                continue;
            }
            // Kind gates: a trailing-'/' pattern yields directories only
            // (pathlib GH-65238), and it yields them regardless of
            // include_dirs. Otherwise directories are dropped unless
            // include_dirs (glob.py:570), and files are dropped for a
            // dir-only pattern.
            if (pattern.dir_only) {
                if (!is_dir) {
                    continue;
                }
            } else if (is_dir && !options.include_dirs) {
                continue;
            }
            if (options.ignore_rules &&
                is_ignored(rel, is_dir, *options.ignore_rules, ci)) {
                result.ignored_count++; // counted for the exclusion message
                continue;
            }

            walk_entry e;
            e.rel_path = std::move(rel);
            e.is_dir = is_dir;
            if (options.collect_stats && stat) {
                entry_stat st;
                if (stat(e.rel_path, st)) {
                    e.size = st.size_bytes;
                    e.mtime = st.mtime;
                }
            }
            collected.push_back(std::move(e));
            if (options.max_matches > 0 &&
                collected.size() > options.max_matches) {
                // glob.py:597-600: the overflow candidate is popped and the
                // search stops, so exactly max_matches matches is NOT capped.
                collected.pop_back();
                result.truncated = true;
                stop = true;
                break;
            }
        }
    }

    sort_entries(collected);
    dedup_entries(collected);
    result.entries = std::move(collected);
    return result;
}

// ===========================================================================
// real-filesystem convenience wrapper
// ===========================================================================

namespace {

// file_time_type -> seconds since the Unix epoch (the file clock's epoch is
// platform-specific, so re-anchor it through `now`).
double glob_file_time_to_seconds(kimix::filesystem::file_time_type t) noexcept {
    const auto system_now = std::chrono::system_clock::now();
    const auto fs_now = kimix::filesystem::file_time_type::clock::now();
    const auto wall = t - fs_now + system_now;
    return std::chrono::duration<double>(wall.time_since_epoch()).count();
}

bool glob_stat_native(const kimix::filesystem::path &full,
                      entry_stat &out) noexcept {
    std::error_code ec;
    const auto st = kimix::filesystem::status(full, ec);
    if (ec) {
        return false;
    }
    out.is_dir = kimix::filesystem::is_directory(st);
    out.size_bytes = -1;
    if (!out.is_dir) {
        const auto size = kimix::filesystem::file_size(full, ec);
        if (!ec) {
            out.size_bytes = static_cast<int64_t>(size);
        }
    }
    out.mtime = 0.0;
    const auto t = kimix::filesystem::last_write_time(full, ec);
    if (!ec) {
        out.mtime = glob_file_time_to_seconds(t);
    }
    return true;
}

kimix::filesystem::path glob_append_rel(const kimix::filesystem::path &root,
                                        kimix::string_view rel) {
    kimix::filesystem::path out = root;
    kimix::vector<kimix::string_view> parts;
    split_segments(rel, parts);
    for (auto sv : parts) {
        out /= std::string(sv.data(), sv.size());
    }
    return out;
}

#if defined(KIMIX_PLATFORM_WINDOWS)

bool glob_is_dot_name(const wchar_t *name) noexcept {
    if (name[0] != L'.') {
        return false;
    }
    return name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0');
}

// Win32 enumeration over the *wide* path, so names outside the ANSI code page
// survive; they are re-encoded to UTF-8 for the kimix::string kernels.
tool_error glob_list_native(const kimix::filesystem::path &dir,
                            kimix::vector<dirent_info> &out) {
    kimix::filesystem::path probe = dir;
    probe /= L"*";
    WIN32_FIND_DATAW data{};
    const HANDLE handle = ::FindFirstFileW(probe.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return glob_error(tool_status::not_found, "directory cannot be listed");
    }
    for (;;) {
        if (!glob_is_dot_name(data.cFileName)) {
            dirent_info info;
            const int len = ::WideCharToMultiByte(
                CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                info.name.resize(static_cast<size_t>(len) - 1u);
                ::WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1,
                                      info.name.data(), len, nullptr, nullptr);
            }
            const uint32_t attrs = data.dwFileAttributes;
            info.is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            info.is_symlink = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            out.push_back(std::move(info));
        }
        if (::FindNextFileW(handle, &data) == 0) {
            break;
        }
    }
    ::FindClose(handle);
    return glob_ok();
}

#else

tool_error glob_list_native(const kimix::filesystem::path &dir,
                            kimix::vector<dirent_info> &out) {
    DIR *dp = ::opendir(dir.c_str());
    if (dp == nullptr) {
        return glob_error(tool_status::not_found, "directory cannot be listed");
    }
    while (dirent *e = ::readdir(dp)) {
        const char *name = e->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        dirent_info info;
        info.name = name;
        bool is_dir = e->d_type == DT_DIR;
        bool is_link = e->d_type == DT_LNK;
        if (is_dir || is_link || e->d_type == DT_UNKNOWN) {
            struct stat lb {};
            const kimix::filesystem::path full = dir / name;
            if (::lstat(full.c_str(), &lb) == 0) {
                is_link = S_ISLNK(lb.st_mode) != 0;
                if (is_link) {
                    struct stat rb {};
                    is_dir = ::stat(full.c_str(), &rb) == 0 &&
                             S_ISDIR(rb.st_mode) != 0;
                } else {
                    is_dir = S_ISDIR(lb.st_mode) != 0;
                }
            }
        }
        info.is_dir = is_dir;
        info.is_symlink = is_link;
        out.push_back(std::move(info));
    }
    ::closedir(dp);
    return glob_ok();
}

#endif

} // namespace

walk_result walk_matches_fs(const kimix::filesystem::path &root,
                            const path_glob_pattern &pattern,
                            const walk_options &options) {
    list_dir_fn lister = [&root](kimix::string_view dir_rel,
                                 kimix::vector<dirent_info> &out) -> tool_error {
        return glob_list_native(glob_append_rel(root, dir_rel), out);
    };
    stat_fn stat;
    if (options.collect_stats) {
        stat = [&root](kimix::string_view rel, entry_stat &out) -> bool {
            return glob_stat_native(glob_append_rel(root, rel), out);
        };
    }
    return walk_matches(lister, stat, pattern, options);
}

walk_result walk_matches_fs(kimix::string_view root, kimix::string_view pattern,
                            const walk_options &options, tool_error &out_error) {
    path_glob_pattern parsed;
    out_error = parse_pattern(pattern, default_case_insensitive(), parsed);
    if (out_error.failed()) {
        return walk_result{};
    }
    const std::string root_native(root.data(), root.size());
    return walk_matches_fs(kimix::filesystem::path(root_native), parsed, options);
}

// ===========================================================================
// result shaping
// ===========================================================================

void sort_entries(kimix::vector<walk_entry> &entries) noexcept {
    std::sort(entries.begin(), entries.end(),
              [](const walk_entry &a, const walk_entry &b) {
                  return a.rel_path < b.rel_path;
              });
}

size_t dedup_entries(kimix::vector<walk_entry> &entries) noexcept {
    if (entries.size() < 2) {
        return 0;
    }
    // Drop duplicate rel_path rows, keeping the first occurrence. The walker
    // sorts before calling this (so duplicates are adjacent), but the helper
    // itself is defined as a first-occurrence dedup and must handle the
    // non-adjacent case too.
    const size_t before = entries.size();
    kimix::unordered_set<kimix::string, kimix::string_hash> seen;
    seen.reserve(entries.size());
    size_t write = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (seen.insert(entries[i].rel_path).second) {
            if (write != i) {
                entries[write] = std::move(entries[i]);
            }
            write++;
        }
    }
    entries.resize(write);
    return before - write;
}

size_t strip_prefix(kimix::vector<walk_entry> &entries,
                    kimix::string_view prefix) noexcept {
    kimix::string norm = normalize_slashes(prefix);
    while (!norm.empty() && norm.back() == k_slash) {
        norm.pop_back();
    }
    if (norm.empty()) {
        return 0;
    }
    size_t rewritten = 0;
    for (auto &e : entries) {
        const kimix::string &rel = e.rel_path;
        if (rel.size() == norm.size() &&
            rel.compare(0, norm.size(), norm) == 0) {
            e.rel_path.clear();
            rewritten++;
        } else if (rel.size() > norm.size() &&
                   rel.compare(0, norm.size(), norm) == 0 &&
                   rel[norm.size()] == k_slash) {
            e.rel_path.erase(0, norm.size() + 1);
            rewritten++;
        }
    }
    return rewritten;
}

void order_by_mtime_top_k(kimix::vector<walk_entry> &entries, size_t k) noexcept {
    // Stable: equal mtimes keep the incoming (path-sorted) order.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const walk_entry &a, const walk_entry &b) {
                         return a.mtime > b.mtime;
                     });
    if (k > 0 && entries.size() > k) {
        entries.resize(k);
    }
}

void paginate_entries(kimix::span<const walk_entry> entries, size_t head_limit,
                      size_t offset, kimix::vector<walk_entry> &out,
                      size_t &total, size_t &omitted_after) {
    out.clear();
    total = entries.size();
    omitted_after = 0;
    if (offset >= entries.size()) {
        return;
    }
    const size_t avail = entries.size() - offset;
    const size_t count = head_limit == 0 ? avail : std::min(head_limit, avail);
    out.assign(entries.begin() + static_cast<long>(offset),
               entries.begin() + static_cast<long>(offset + count));
    omitted_after = avail - count;
}

void shape_output(kimix::span<const walk_entry> entries,
                  const shape_options &options, shaped_output &out) {
    out = shaped_output{};
    kimix::vector<kimix::string> lines;
    lines.reserve(entries.size());
    for (const auto &e : entries) {
        kimix::string line;
        if (options.verbose) {
            // "<rel>  (<size> bytes, <kind>, <mtime>)" (glob.py:615-629). The
            // mtime text comes from a caller-supplied renderer because pendulum
            // formatting stays in Python.
            kimix::StringScratch ss;
            ss << e.rel_path << "  (";
            if (e.size >= 0) {
                ss << e.size;
            } else {
                ss << "?";
            }
            ss << " bytes, " << (e.is_dir ? "dir" : "file");
            if (options.format_mtime) {
                ss << ", " << options.format_mtime(e);
            }
            ss << ")";
            line = std::move(ss.string());
        } else {
            line = kimix::string(e.rel_path.data(), e.rel_path.size());
        }
        kimix::string capped;
        truncate_line(line, options.max_line_len, capped);
        lines.push_back(std::move(capped));
    }

    // Byte budget (glob.py:631-637): the line that reaches the cap is kept and
    // collection stops right there.
    kimix::vector<kimix::string> budgeted;
    budgeted.reserve(lines.size());
    size_t bytes = 0;
    for (auto &line : lines) {
        const size_t sep = budgeted.empty() ? 0u : 1u;
        bytes += sep + line.size();
        budgeted.push_back(std::move(line));
        if (options.max_bytes > 0 && bytes >= options.max_bytes) {
            out.truncated_by_bytes = true;
            break;
        }
    }
    out.total_bytes = bytes;

    // head+tail fold (Params.max_results; 0 = unlimited). output_utils.fold_lines
    // defaults: head = max(1, max_lines // 2), tail = max_lines - head.
    size_t head = 0;
    size_t tail = 0;
    if (options.max_results > 0) {
        head = options.max_results / 2;
        if (head < 1) {
            head = 1;
        }
        tail = options.max_results - head;
    }
    kimix::vector<kimix::string> folded;
    fold_lines(kimix::span<const kimix::string>(budgeted), options.max_results,
               head, tail, folded, out.omitted_by_fold);
    out.lines = std::move(folded);
    out.shown_count = out.lines.size() - (out.omitted_by_fold > 0 ? 1u : 0u);
}

kimix::string top_dirs_summary(kimix::span<const walk_entry> entries,
                               size_t top) {
    // Port of _top_dirs_summary (glob.py:296-317): count the first component of
    // every match that has one, keep the `top` biggest (ties by name).
    kimix::vector<std::pair<kimix::string, size_t>> counts;
    for (const auto &e : entries) {
        const size_t slash = e.rel_path.find(k_slash);
        if (slash == kimix::string::npos || slash == 0) {
            continue; // files directly in the search root have no top-level dir
        }
        const kimix::string head(e.rel_path.data(), slash);
        bool found = false;
        for (auto &kv : counts) {
            if (kv.first == head) {
                kv.second++;
                found = true;
                break;
            }
        }
        if (!found) {
            counts.emplace_back(head, size_t(1));
        }
    }
    if (counts.empty()) {
        return {};
    }
    std::sort(counts.begin(), counts.end(),
              [](const std::pair<kimix::string, size_t> &a,
                 const std::pair<kimix::string, size_t> &b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  return a.first < b.first;
              });
    kimix::StringScratch ss;
    ss << "top dirs: ";
    const size_t limit = std::min(top, counts.size());
    for (size_t i = 0; i < limit; i++) {
        if (i > 0) {
            ss << ", ";
        }
        ss << counts[i].first << " (" << counts[i].second << ")";
    }
    return std::move(ss.string());
}

kimix::string build_result_message(const message_input &in) {
    // Exact assembly of the Glob `message` field (glob.py:660-690). Like
    // Python, the top-dirs summary is appended only inside the fold branch.
    kimix::StringScratch ss;
    if (in.total > 0) {
        ss << "Found " << in.total << " matches for pattern `" << in.pattern
           << "`.";
    } else {
        ss << "No matches found for pattern `" << in.pattern << "`.";
        if (in.respect_gitignore && in.ignored_count > 0 && !in.timed_out) {
            ss << " " << in.ignored_count
               << " path(s) matched but were excluded by"
                  " .gitignore \xE2\x80\x94 pass respect_gitignore=False to "
                  "include them.";
        }
    }
    if (in.omitted_by_fold > 0) {
        ss << " Showing " << in.shown_count << " of " << in.total
           << " (head+tail fold). "
              "Use max_results=0 or a more specific pattern to see more.";
        if (in.with_top_dirs && !in.top_dirs.empty()) {
            ss << " " << in.top_dirs;
        }
    }
    if (in.truncated) {
        ss << " Search capped at " << in.max_matches << " matches.";
    }
    if (in.timed_out) {
        ss << " Search timed out after " << in.timeout_seconds
           << "s; showing matches collected so far.";
    }
    if (in.truncated_by_bytes) {
        ss << " Output truncated to " << in.max_bytes << " bytes.";
    }
    return std::move(ss.string());
}

} // namespace glob
} // namespace builtin_tools
} // namespace kimix
