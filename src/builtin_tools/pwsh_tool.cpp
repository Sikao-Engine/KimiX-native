// pwsh_tool.cpp - Self-kill guard kernels (pwsh tool namespace).
//
// Exact port of C:/dev/kimi-agent/src/kimix/tools/file/bash/safety.py
// (self-kill guard, lines 48-70 + 272-806; see pwsh_tool.h for the
// function-by-function map). The helper scanners below replicate the
// `regex` module semantics for the reference patterns: leftmost match,
// finditer resuming after each match end, re.split(..., maxsplit=1) taking
// the earliest separator position, and ASCII `\b` = [A-Za-z0-9_].
//
// ASCII-only contract: non-ASCII input and pkill patterns with regex
// metacharacters are signalled through tool_status::unsupported so the shim
// falls back to the Python mirror; std::regex is never used.

#include "builtin_tools/pwsh_tool.h"

#include <algorithm>
#include <cstdint>

namespace kimix::builtin_tools::pwsh {
namespace {

// ---- ASCII character classes ------------------------------------------------
inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
inline bool is_ascii(char c) noexcept { return static_cast<unsigned char>(c) < 0x80u; }
// ASCII \w = [A-Za-z0-9_].
inline bool is_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
inline bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}
inline bool has_alnum(kimix::string_view s) noexcept {
    for (char c : s) {
        if (is_alpha(c) || is_digit(c)) {
            return true;
        }
    }
    return false;
}

bool is_ascii_text(kimix::string_view s) noexcept {
    for (char c : s) {
        if (!is_ascii(c)) {
            return false;
        }
    }
    return true;
}

kimix::string lower_copy(kimix::string_view s) {
    kimix::string out(s.data(), s.size());
    for (char &c : out) {
        c = lower_ascii(c);
    }
    return out;
}

// Strip whitespace, then strip any combination of leading/trailing quotes
// (Python str.strip(chars)).
kimix::string_view strip_quotes(kimix::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) {
        ++b;
    }
    while (e > b && is_space(s[e - 1])) {
        --e;
    }
    while (b < e && (s[b] == '"' || s[b] == '\'')) {
        ++b;
    }
    while (e > b && (s[e - 1] == '"' || s[e - 1] == '\'')) {
        --e;
    }
    return s.substr(b, e - b);
}

// ---- _segment_text / _segment_tokens (safety.py 438-440, 73-81) ------------
//
// re.split(r";|\|\||&&|\||\n", tail, maxsplit=1)[0]: the split index is the
// earliest position of ';', "||", "&&", '|' or '\n' (the "||" alternative
// implies a '|' at the same position; match length is irrelevant because
// only the pre-split text is used). A single '&' does NOT split.
size_t segment_split_pos(kimix::string_view text, size_t start) noexcept {
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ';' || c == '|' || c == '\n') {
            return i;
        }
        if (c == '&' && i + 1 < text.size() && text[i + 1] == '&') {
            return i;
        }
    }
    return text.size();
}

kimix::string_view segment_text(kimix::string_view text, size_t start) noexcept {
    return text.substr(start, segment_split_pos(text, start) - start);
}

// Whitespace-separated token views of text[start : split_pos).
kimix::vector<kimix::string_view> segment_tokens(kimix::string_view text,
                                                 size_t start) {
    kimix::vector<kimix::string_view> toks;
    const size_t e = segment_split_pos(text, start);
    size_t i = start;
    while (i < e) {
        while (i < e && is_space(text[i])) {
            ++i;
        }
        if (i >= e) {
            break;
        }
        size_t j = i;
        while (j < e && !is_space(text[j])) {
            ++j;
        }
        toks.push_back(text.substr(i, j - i));
        i = j;
    }
    return toks;
}

// ---- _looks_like_flag (safety.py 84-91) -------------------------------------
bool looks_like_flag(kimix::string_view token) noexcept {
    if (token.size() <= 1) {
        return false;
    }
    if (token[0] == '-') {
        return true;
    }
    if (token[0] == '/') {
        for (size_t i = 1; i < token.size(); ++i) {
            if (!is_alpha(token[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// ---- word scanner ------------------------------------------------------------
// Leftmost `word` occurrences in `text` starting at `from`, with ASCII word
// boundaries on both sides (re.finditer(r"\b<word>\b", text)). `text` is
// already lowercased; `word` must be lowercase.
struct word_match {
    size_t begin = 0;
    size_t end = 0;
};

kimix::vector<word_match> find_word(kimix::string_view text,
                                    kimix::string_view word, size_t from = 0) {
    kimix::vector<word_match> out;
    size_t pos = from;
    while (pos + word.size() <= text.size()) {
        pos = text.find(word, pos);
        if (pos == kimix::string_view::npos) {
            break;
        }
        const bool lb = pos == 0 || !is_word(text[pos - 1]);
        const size_t end = pos + word.size();
        const bool rb = end == text.size() || !is_word(text[end]);
        if (lb && rb) {
            out.push_back({pos, end});
            pos = end == pos ? end + 1 : end; // finditer resumes at match end
        } else {
            pos += 1;
        }
    }
    return out;
}

// find_word for the `\b<word>(?:\.exe)?\b` family: the greedy `(?:\.exe)?`
// suffix is consumed when ".exe" follows and a word boundary holds after it;
// otherwise the plain word match stands (regex backtracking on \b).
kimix::vector<word_match> find_word_opt_exe(kimix::string_view text,
                                            kimix::string_view word) {
    kimix::vector<word_match> matches = find_word(text, word);
    for (word_match &m : matches) {
        if (m.end + 4 <= text.size() && text.substr(m.end, 4) == ".exe" &&
            (m.end + 4 == text.size() || !is_word(text[m.end + 4]))) {
            m.end += 4;
        }
    }
    return matches;
}

// ---- collapse + lowercase (safety.py 629: " ".join(command.split()).lower()) -
kimix::string collapse_lower(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    bool first = true;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        if (!first) {
            out.push_back(' ');
        }
        first = false;
        while (i < s.size() && !is_space(s[i])) {
            out.push_back(lower_ascii(s[i]));
            ++i;
        }
    }
    return out;
}

// ---- _numeric_pid_targets (safety.py 443-460) --------------------------------
// Non-flag tokens; comma-split; strip whitespace+quotes+parens; plain ASCII
// digits; PowerShell expression style ^(\d+)[).]. Python's int() is
// unbounded, so an ASCII digit run longer than 18 digits can never equal an
// int64_t protected PID and is skipped (faithful: Python finds no hit either).
kimix::vector<int64_t> numeric_pid_targets(kimix::span<const kimix::string_view> tokens) {
    kimix::vector<int64_t> pids;
    for (kimix::string_view token : tokens) {
        if (looks_like_flag(token)) {
            continue;
        }
        size_t begin = 0;
        while (true) {
            size_t comma = token.find(',', begin);
            if (comma == kimix::string_view::npos) {
                comma = token.size();
            }
            kimix::string_view part = token.substr(begin, comma - begin);
            begin = comma + 1;
            // part.strip().strip("\"'()")
            size_t b = 0;
            size_t e = part.size();
            while (b < e && is_space(part[b])) {
                ++b;
            }
            while (e > b && is_space(part[e - 1])) {
                --e;
            }
            while (b < e && (part[b] == '"' || part[b] == '\'' || part[b] == '(' || part[b] == ')')) {
                ++b;
            }
            while (e > b && (part[e - 1] == '"' || part[e - 1] == '\'' ||
                             part[e - 1] == '(' || part[e - 1] == ')')) {
                --e;
            }
            part = part.substr(b, e - b);
            if (part.empty()) {
                if (comma >= token.size()) {
                    break;
                }
                continue;
            }
            size_t digits = 0;
            while (digits < part.size() && is_digit(part[digits])) {
                ++digits;
            }
            if (digits == part.size()) {
                if (digits <= 18) {
                    int64_t value = 0;
                    for (size_t k = 0; k < digits; ++k) {
                        value = value * 10 + (part[k] - '0');
                    }
                    pids.push_back(value);
                }
                // >18 digits: Python int() keeps it, but it can never equal
                // an int64_t protected PID — skipped.
            } else if (digits > 0 && digits < part.size() &&
                       (part[digits] == ')' || part[digits] == '.')) {
                // ^(\d+)[).] — leading digits followed by ')' or '.'
                if (digits <= 18) {
                    int64_t value = 0;
                    for (size_t k = 0; k < digits; ++k) {
                        value = value * 10 + (part[k] - '0');
                    }
                    pids.push_back(value);
                }
            }
            if (comma >= token.size()) {
                break;
            }
        }
    }
    return pids;
}

// ---- _loop_pid_sources (safety.py 469-505) -----------------------------------
// Loop headers that bind a variable to a literal PID list. Variable names are
// lowercased; entries containing $ * ? [ ` ~ are skipped (unresolvable); only
// plain-digit parts are kept (deduped per variable). Insertion order is
// preserved because the hit descriptions enumerate the bound PIDs.
//
// bash:      \bfor\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([^;]+)   (list ends at ';')
// PowerShell:\bforeach\s*\(\s*\$([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([^)]+)\)
using loop_sources = kimix::vector<std::pair<kimix::string, kimix::vector<int64_t>>>;

kimix::vector<int64_t> &loop_source_get(loop_sources &sources,
                                        kimix::string_view var) {
    for (auto &entry : sources) {
        if (entry.first == var) {
            return entry.second;
        }
    }
    sources.emplace_back(kimix::string(var.data(), var.size()),
                         kimix::vector<int64_t>{});
    return sources.back().second;
}

const kimix::vector<int64_t> *loop_source_find(const loop_sources &sources,
                                               kimix::string_view var) {
    for (const auto &entry : sources) {
        if (entry.first == var) {
            return &entry.second;
        }
    }
    return nullptr;
}

void loop_source_add_pids(loop_sources &sources, kimix::string_view var,
                          kimix::string_view list) {
    auto &pids = loop_source_get(sources, var);
    // Whitespace-tokenize the list, then comma-split each token.
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && is_space(list[i])) {
            ++i;
        }
        if (i >= list.size()) {
            break;
        }
        size_t j = i;
        while (j < list.size() && !is_space(list[j])) {
            ++j;
        }
        const kimix::string_view token = list.substr(i, j - i);
        i = j;
        bool unresolvable = false;
        for (char c : token) {
            if (c == '$' || c == '*' || c == '?' || c == '[' || c == '`' || c == '~') {
                unresolvable = true;
                break;
            }
        }
        if (unresolvable) {
            continue;
        }
        size_t begin = 0;
        while (true) {
            size_t comma = token.find(',', begin);
            if (comma == kimix::string_view::npos) {
                comma = token.size();
            }
            kimix::string_view part = token.substr(begin, comma - begin);
            begin = comma + 1;
            part = strip_quotes(part);
            bool all_digits = !part.empty();
            int64_t value = 0;
            for (char c : part) {
                if (!is_digit(c)) {
                    all_digits = false;
                    break;
                }
                value = value * 10 + (c - '0');
            }
            if (all_digits) {
                bool seen = false;
                for (int64_t pid : pids) {
                    if (pid == value) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    pids.push_back(value);
                }
            }
            if (comma >= token.size()) {
                break;
            }
        }
    }
}

loop_sources build_loop_pid_sources(kimix::string_view text) {
    loop_sources sources;
    // bash/POSIX: for <var> in <list up to ';'>
    for (const word_match &m : find_word(text, "for")) {
        size_t i = m.end;
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        size_t vb = i;
        while (i < text.size() &&
               ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_')) {
            ++i;
        }
        if (i == vb) {
            continue;
        }
        size_t ve = i;
        while (i < text.size() &&
               ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z') ||
                (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) {
            ++i;
        }
        size_t k = i;
        while (k < text.size() && is_space(text[k])) {
            ++k;
        }
        if (k + 2 > text.size() || text[k] != 'i' || text[k + 1] != 'n' ||
            (k + 2 < text.size() && is_word(text[k + 2]))) {
            continue;
        }
        k += 2;
        while (k < text.size() && is_space(text[k])) {
            ++k;
        }
        size_t le = k;
        while (le < text.size() && text[le] != ';') {
            ++le;
        }
        // `text` is already lowercased; keep a stable copy (never bind a
        // string_view to the temporary returned by lower_copy).
        const kimix::string var = lower_copy(text.substr(vb, ve - vb));
        loop_source_add_pids(sources, var, text.substr(k, le - k));
    }
    // PowerShell: foreach ($<var> in <list up to ')'>)
    for (const word_match &m : find_word(text, "foreach")) {
        size_t i = m.end;
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        if (i >= text.size() || text[i] != '(') {
            continue;
        }
        ++i;
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        if (i >= text.size() || text[i] != '$') {
            continue;
        }
        ++i;
        size_t vb = i;
        while (i < text.size() &&
               ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_')) {
            ++i;
        }
        if (i == vb) {
            continue;
        }
        size_t ve = i;
        while (i < text.size() &&
               ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z') ||
                (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) {
            ++i;
        }
        size_t k = i;
        while (k < text.size() && is_space(text[k])) {
            ++k;
        }
        if (k + 2 > text.size() || text[k] != 'i' || text[k + 1] != 'n' ||
            (k + 2 < text.size() && is_word(text[k + 2]))) {
            continue;
        }
        k += 2;
        while (k < text.size() && is_space(text[k])) {
            ++k;
        }
        size_t le = k;
        while (le < text.size() && text[le] != ')') {
            ++le;
        }
        if (le >= text.size()) {
            continue;
        }
        const kimix::string var = lower_copy(text.substr(vb, ve - vb));
        loop_source_add_pids(sources, var, text.substr(k, le - k));
    }
    return sources;
}

// ---- _split_image_name (safety.py 388-398) -----------------------------------
struct image_name_parts {
    kimix::string base;
    kimix::string stem;
};

bool executable_suffix(kimix::string_view ext) noexcept {
    return ext == "exe" || ext == "com" || ext == "bat" || ext == "cmd" ||
           ext == "py" || ext == "sh";
}

image_name_parts split_image_name(kimix::string_view name) {
    // name.strip().strip("\"'")
    kimix::string_view s = strip_quotes(name);
    // basename: last component after '/' or '\'
    size_t slash = s.size();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/' || s[i] == '\\') {
            slash = i;
        }
    }
    image_name_parts parts;
    parts.base = lower_copy(slash < s.size() ? s.substr(slash + 1) : s);
    // rpartition("."): stem drops a known executable suffix only
    size_t dot = parts.base.rfind('.');
    if (dot != kimix::string::npos && dot > 0 && dot + 1 < parts.base.size() &&
        executable_suffix(kimix::string_view(parts.base).substr(dot + 1))) {
        parts.stem = parts.base.substr(0, dot);
    } else {
        parts.stem = parts.base;
    }
    return parts;
}

// ---- _name_kill_hit (safety.py 541-561) ---------------------------------------
// Returns the matched agent image name, or empty. `names` is the shim-provided
// set iterated in Python's unspecified set order; the kernel uses ascending
// order (documented deviation) so the result is deterministic.
kimix::string name_kill_hit(kimix::string_view token,
                            const kimix::vector<kimix::string> &names) {
    const image_name_parts parts = split_image_name(token);
    if (parts.base.empty() || !has_alnum(parts.base)) {
        return {};
    }
    if (!parts.base.empty() && parts.base.back() == '*') {
        const kimix::string_view prefix(parts.base.data(), parts.base.size() - 1);
        if (prefix.size() < 3) {
            return {};
        }
        for (const kimix::string &name : names) {
            if (name.size() >= prefix.size() &&
                kimix::string_view(name).substr(0, prefix.size()) == prefix) {
                return name;
            }
        }
        return {};
    }
    for (const kimix::string &name : names) {
        if (parts.base == name || parts.stem == name) {
            return name;
        }
    }
    return {};
}

// ---- _pattern_kill_hit, plain-substring subset (safety.py 564-584) ------------
// Python: re.search(pattern, haystack, re.IGNORECASE) with substring fallback.
// The kernel implements only the plain case-insensitive substring subset; the
// metachar gate (below) routes everything else to Python.
kimix::string_view pattern_kill_hit(kimix::string_view pattern,
                                    kimix::span<const kimix::string> haystacks) {
    const kimix::string_view p = strip_quotes(pattern);
    if (p.empty() || !has_alnum(p)) {
        return {};
    }
    for (const kimix::string &haystack : haystacks) {
        if (haystack.empty()) {
            continue;
        }
        // case-insensitive substring search over ASCII text
        if (haystack.size() >= p.size()) {
            for (size_t i = 0; i + p.size() <= haystack.size(); ++i) {
                bool ok = true;
                for (size_t k = 0; k < p.size(); ++k) {
                    if (lower_ascii(haystack[i + k]) != lower_ascii(p[k])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    return haystack;
                }
            }
        }
    }
    return {};
}

bool is_regex_metachar(char c) noexcept {
    switch (c) {
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '+':
    case '?':
    case '|':
    case '^':
    case '$':
    case '\\':
    case '.':
        return true;
    default:
        return false;
    }
}

// ---- _pkill_full_match (safety.py 587-595) ------------------------------------
bool pkill_full_match(kimix::span<const kimix::string_view> tokens) noexcept {
    for (kimix::string_view token : tokens) {
        if (token == "--full") {
            return true;
        }
        if (token.size() >= 2 && token[0] == '-' && token[1] != '-') {
            for (size_t i = 1; i < token.size(); ++i) {
                if (token[i] == 'f') {
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- gate: pkill patterns with regex metacharacters ----------------------------
// Any non-flag pkill pattern token containing a regex metacharacter routes the
// whole call to the Python mirror (plan §8): the kernel's substring subset
// would diverge from re.search on such patterns. Uses the same word scanner
// (with the optional ".exe" suffix) as detector 4 so the token stream agrees.
bool pkill_needs_python(kimix::string_view text) {
    for (const word_match &m : find_word_opt_exe(text, "pkill")) {
        for (kimix::string_view token : segment_tokens(text, m.end)) {
            if (looks_like_flag(token)) {
                continue;
            }
            const kimix::string_view p = strip_quotes(token);
            for (char c : p) {
                if (is_regex_metachar(c)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- description builders (byte-exact ports of the Python f-strings) ----------
kimix::string pid_hit_description(int64_t pid, kimix::string_view via) {
    kimix::string out = "targets PID ";
    out += kimix::format("{}", pid);
    out += " via ";
    out += via;
    out += ", which is the agent process or one of its parent processes";
    return out;
}

kimix::string loop_pid_list(kimix::span<const int64_t> pids) {
    kimix::string out;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += kimix::format("{}", pids[i]);
    }
    return out;
}

} // namespace

const char *const k_self_kill_guidance =
    "If you meant to stop a different process, re-check its PID first "
    "(`tasklist` / `Get-Process` / `ps aux`) and retry with a PID that does "
    "not belong to the agent. If the target merely shares the agent's image "
    "name, terminate that specific PID instead of a name/pattern match. If "
    "you really intend to stop or restart the agent itself, ask the user to "
    "do it from outside this session.";

void command_detection_variants(kimix::string_view command,
                                kimix::vector<kimix::string> &out) {
    out.clear();
    if (command.empty()) {
        return;
    }
    bool only_space = true;
    for (char c : command) {
        if (!is_space(c)) {
            only_space = false;
            break;
        }
    }
    if (only_space) {
        return;
    }
    // collapsed = " ".join(command.split()) — whitespace collapse, case kept.
    kimix::string collapsed;
    collapsed.reserve(command.size());
    {
        bool first = true;
        size_t i = 0;
        while (i < command.size()) {
            while (i < command.size() && is_space(command[i])) {
                ++i;
            }
            if (i >= command.size()) {
                break;
            }
            if (!first) {
                collapsed.push_back(' ');
            }
            first = false;
            while (i < command.size() && !is_space(command[i])) {
                collapsed.push_back(command[i]);
                ++i;
            }
        }
    }
    // deobfuscated = re.sub(r"[\\'\"]", "", collapsed).lower()
    kimix::string deobfuscated;
    deobfuscated.reserve(collapsed.size());
    for (char c : collapsed) {
        if (c != '\\' && c != '\'' && c != '"') {
            deobfuscated.push_back(lower_ascii(c));
        }
    }
    // lowered = collapsed.lower()
    kimix::string lowered = lower_copy(collapsed);
    auto push_unique = [&](const kimix::string &v) {
        if (v.empty()) {
            return;
        }
        for (const kimix::string &existing : out) {
            if (existing == v) {
                return;
            }
        }
        out.push_back(v);
    };
    push_unique(collapsed);
    push_unique(deobfuscated);
    push_unique(lowered);
}

namespace {

// Ordered-detector state shared between the optional<> and struct APIs.
struct detect_state {
    kimix::string_view text;
    const kimix::unordered_set<int64_t> *protected_pids = nullptr;
    kimix::vector<kimix::string> names; // lowercased + sorted (deterministic)
    kimix::string_view cmdline;
    loop_sources loops;
    tool_status status = tool_status::ok;
};

bool pid_protected(const detect_state &st, int64_t pid) {
    return st.protected_pids->find(pid) != st.protected_pids->end();
}

// _pid_hit over numeric targets of `tokens`.
kimix::optional<kimix::string> pid_hit(detect_state &st,
                                       kimix::span<const kimix::string_view> tokens,
                                       kimix::string_view via) {
    const kimix::vector<int64_t> pids = numeric_pid_targets(tokens);
    for (int64_t pid : pids) {
        if (pid_protected(st, pid)) {
            return pid_hit_description(pid, via);
        }
    }
    return {};
}

// _variable_pid_hit (safety.py 508-538): `$var` / `${var}` tokens resolved
// through the loop sources.
kimix::optional<kimix::string> variable_pid_hit(
    detect_state &st, kimix::span<const kimix::string_view> tokens,
    kimix::string_view via) {
    for (kimix::string_view token : tokens) {
        const kimix::string_view stripped = strip_quotes(token);
        // fullmatch \$\{?([A-Za-z_][A-Za-z0-9_]*)\}?
        if (stripped.empty() || stripped[0] != '$') {
            continue;
        }
        size_t i = 1;
        bool open_brace = false;
        if (i < stripped.size() && stripped[i] == '{') {
            open_brace = true;
            ++i;
        }
        const size_t vb = i;
        while (i < stripped.size() &&
               ((stripped[i] >= 'a' && stripped[i] <= 'z') ||
                (stripped[i] >= 'A' && stripped[i] <= 'Z') || stripped[i] == '_')) {
            ++i;
        }
        if (i == vb) {
            continue;
        }
        size_t ve = i;
        while (i < stripped.size() &&
               ((stripped[i] >= 'a' && stripped[i] <= 'z') ||
                (stripped[i] >= 'A' && stripped[i] <= 'Z') ||
                (stripped[i] >= '0' && stripped[i] <= '9') || stripped[i] == '_')) {
            ++i;
        }
        // \}? is optional and independent of the leading brace —
        // ``${pid``, ``${pid}`` and ``$pid}`` all fullmatch.
        if (i < stripped.size() && stripped[i] == '}') {
            ++i;
        }
        if (i != stripped.size()) {
            continue;
        }
        const kimix::string var = lower_copy(stripped.substr(vb, ve - vb));
        const kimix::vector<int64_t> *pids = loop_source_find(st.loops, var);
        if (pids == nullptr || pids->empty()) {
            continue;
        }
        for (int64_t pid : *pids) {
            if (pid_protected(st, pid)) {
                const kimix::string_view var_original = stripped.substr(vb, ve - vb);
                kimix::string out = "kills PID ";
                out += kimix::format("{}", pid);
                out += " via `";
                out += via;
                out += "` through loop variable `$";
                out += var_original;
                out += "` (bound to PIDs ";
                out += loop_pid_list(kimix::span<const int64_t>(pids->data(), pids->size()));
                out += "), which is the agent process or one of its parent processes";
                return out;
            }
        }
    }
    return {};
}

} // namespace

self_kill_result detect_self_kill_ex(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline) {
    self_kill_result result;
    result.status = tool_status::ok;
    result.hit = false;

    // ASCII gate (plan §8): Python's str methods and \b are Unicode-aware.
    if (!is_ascii_text(command) || !is_ascii_text(cmdline)) {
        for (const kimix::string &name : image_names) {
            if (!is_ascii_text(name)) {
                result.status = tool_status::unsupported;
                return result;
            }
        }
        result.status = tool_status::unsupported;
        return result;
    }

    // safety.py 618-619: empty / whitespace-only command
    bool only_space = true;
    for (char c : command) {
        if (!is_space(c)) {
            only_space = false;
            break;
        }
    }
    if (command.empty() || only_space) {
        return result;
    }
    // safety.py 626-627: no protected pids -> None
    if (protected_pids.empty()) {
        return result;
    }

    detect_state st;
    st.protected_pids = &protected_pids;
    st.cmdline = cmdline;
    // image_names lowercased + sorted (Python: {n.lower() ...} set order is
    // unspecified; ascending order keeps wildcard first-match deterministic).
    {
        kimix::vector<kimix::string> names;
        names.reserve(image_names.size());
        for (const kimix::string &name : image_names) {
            if (!name.empty()) {
                names.push_back(lower_copy(name));
            }
        }
        std::sort(names.begin(), names.end());
        st.names = std::move(names);
    }

    const kimix::string text = collapse_lower(command);
    st.text = text;
    st.loops = build_loop_pid_sources(text);

    // Metachar gate: pkill patterns are full regexes in Python.
    if (pkill_needs_python(text)) {
        result.status = tool_status::unsupported;
        return result;
    }

    auto finish = [&](kimix::optional<kimix::string> desc, const char *rule) -> bool {
        if (desc.has_value()) {
            result.hit = true;
            result.description = std::move(desc);
            result.rule_id = rule;
            return true;
        }
        return false;
    };

    // 1. POSIX kill / Windows tskill (safety.py 644-657).
    static const char *k_kill_skip[] = {"docker", "podman", "kubectl", "compose"};
    {
        kimix::vector<word_match> matches = find_word_opt_exe(text, "kill");
        for (const word_match &m : find_word_opt_exe(text, "tskill")) {
            matches.push_back(m);
        }
        std::sort(matches.begin(), matches.end(),
                  [](const word_match &a, const word_match &b) {
                      return a.begin < b.begin;
                  });
        // finditer resumes after each match end; overlapping alternation
        // starts inside a consumed span are dropped (kill.exe starts inside
        // the "kill" match span only when kill matched plain - it does not,
        // because \b fails, so kill.exe matches are kept).
        for (const word_match &m : matches) {
            // `text` is a kimix::string; take subviews of its buffer directly
            // (text.substr would bind a view to a temporary -> dangling).
            const kimix::string_view word(text.data() + m.begin, m.end - m.begin);
            if (word.size() >= 4 && word.substr(0, 4) == "kill") {
                // skip docker/podman/kubectl/compose kill: the word before
                // the match (after trailing whitespace) is one of the skip
                // words. Work on sizes so the const view needs no mutation.
                const kimix::string_view before(text.data(), m.begin);
                size_t be = before.size();
                while (be > 0 && is_space(before[be - 1])) {
                    --be;
                }
                const kimix::string_view trimmed = before.substr(0, be);
                size_t wb = trimmed.size();
                while (wb > 0 && is_word(trimmed[wb - 1])) {
                    --wb;
                }
                const kimix::string_view prev = trimmed.substr(wb);
                bool skip = false;
                for (const char *s : k_kill_skip) {
                    if (prev == s) {
                        skip = true;
                        break;
                    }
                }
                if (skip) {
                    continue;
                }
            }
            const kimix::vector<kimix::string_view> tokens = segment_tokens(text, m.end);
            const kimix::string via = kimix::string("`") + kimix::string(word.data(), word.size()) + "`";
            if (finish(pid_hit(st, tokens, via), "kill")) {
                return result;
            }
            if (st.status != tool_status::ok) {
                result.status = st.status;
                return result;
            }
            if (finish(variable_pid_hit(st, tokens, via), "kill")) {
                return result;
            }
        }
    }

    // 2. taskkill (safety.py 660-675).
    {
        kimix::vector<word_match> matches = find_word_opt_exe(text, "taskkill");
        std::sort(matches.begin(), matches.end(),
                  [](const word_match &a, const word_match &b) {
                      return a.begin < b.begin;
                  });
        for (const word_match &m : matches) {
            const kimix::vector<kimix::string_view> tokens =
                segment_tokens(segment_text(text, m.end), 0);
            if (finish(pid_hit(st, tokens, "`taskkill`"), "taskkill")) {
                return result;
            }
            if (st.status != tool_status::ok) {
                result.status = st.status;
                return result;
            }
            if (finish(variable_pid_hit(st, tokens, "`taskkill`"), "taskkill")) {
                return result;
            }
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (tokens[i] == "/im") {
                    const kimix::string name_hit = name_kill_hit(tokens[i + 1], st.names);
                    if (!name_hit.empty()) {
                        kimix::string desc = "kills by image name `";
                        desc += name_hit;
                        desc += "` via `taskkill /IM`, which also matches the agent process";
                        if (finish(kimix::optional<kimix::string>(std::move(desc)), "taskkill")) {
                            return result;
                        }
                    }
                }
            }
        }
    }

    // 3. Stop-Process -Id / -Name, Get-Process piped into a kill (safety.py 677-714).
    {
        for (const word_match &m : find_word(text, "stop-process")) {
            const kimix::vector<kimix::string_view> tokens = segment_tokens(text, m.end);
            if (finish(pid_hit(st, tokens, "`Stop-Process`"), "stop-process")) {
                return result;
            }
            if (st.status != tool_status::ok) {
                result.status = st.status;
                return result;
            }
            if (finish(variable_pid_hit(st, tokens, "`Stop-Process`"), "stop-process")) {
                return result;
            }
            for (kimix::string_view token : tokens) {
                if (looks_like_flag(token)) {
                    continue;
                }
                const kimix::string name_hit = name_kill_hit(token, st.names);
                if (!name_hit.empty()) {
                    kimix::string desc = "kills by process name `";
                    desc += name_hit;
                    desc += "` via `Stop-Process`, which also matches the agent process";
                    if (finish(kimix::optional<kimix::string>(std::move(desc)), "stop-process")) {
                        return result;
                    }
                }
            }
        }
    }
    if (text.find("stop-process") != kimix::string_view::npos ||
        text.find("| kill") != kimix::string_view::npos ||
        text.find(".kill()") != kimix::string_view::npos) {
        for (const word_match &m : find_word(text, "get-process")) {
            const kimix::vector<kimix::string_view> tokens = segment_tokens(text, m.end);
            if (finish(pid_hit(st, tokens, "`Get-Process` piped to a kill"), "get-process")) {
                return result;
            }
            if (st.status != tool_status::ok) {
                result.status = st.status;
                return result;
            }
            if (finish(variable_pid_hit(st, tokens, "`Get-Process` piped to a kill"),
                       "get-process")) {
                return result;
            }
            for (kimix::string_view token : tokens) {
                if (looks_like_flag(token)) {
                    continue;
                }
                const kimix::string name_hit = name_kill_hit(token, st.names);
                if (!name_hit.empty()) {
                    kimix::string desc = "kills by process name `";
                    desc += name_hit;
                    desc += "` via `Get-Process` piped to a kill, which also matches the agent process";
                    if (finish(kimix::optional<kimix::string>(std::move(desc)), "get-process")) {
                        return result;
                    }
                }
            }
        }
    }

    // 4. pkill / killall (safety.py 716-742).
    {
        struct tagged {
            word_match match;
            bool is_pkill;
        };
        kimix::vector<tagged> matches;
        for (const word_match &m : find_word_opt_exe(text, "pkill")) {
            matches.push_back({m, true});
        }
        for (const word_match &m : find_word_opt_exe(text, "killall")) {
            matches.push_back({m, false});
        }
        std::sort(matches.begin(), matches.end(),
                  [](const tagged &a, const tagged &b) {
                      return a.match.begin < b.match.begin;
                  });
        for (const tagged &t : matches) {
            const kimix::vector<kimix::string_view> tokens = segment_tokens(text, t.match.end);
            const bool full = t.is_pkill && pkill_full_match(
                                                 kimix::span<const kimix::string_view>(tokens.data(), tokens.size()));
            for (kimix::string_view token : tokens) {
                if (looks_like_flag(token)) {
                    continue;
                }
                if (t.is_pkill) {
                    kimix::vector<kimix::string> haystacks = st.names; // sorted
                    if (full && !st.cmdline.empty()) {
                        haystacks.push_back(kimix::string(st.cmdline.data(), st.cmdline.size()));
                    }
                    const kimix::string_view hit = pattern_kill_hit(
                        token, kimix::span<const kimix::string>(haystacks.data(), haystacks.size()));
                    if (!hit.empty()) {
                        const kimix::string_view display = strip_quotes(token);
                        kimix::string desc = "kills processes matching `";
                        desc += display;
                        desc += "` via `pkill";
                        if (full) {
                            desc += " -f";
                        }
                        desc += "`, which also matches the agent process";
                        if (finish(kimix::optional<kimix::string>(std::move(desc)), "pkill")) {
                            return result;
                        }
                    }
                } else {
                    const kimix::string name_hit = name_kill_hit(token, st.names);
                    if (!name_hit.empty()) {
                        kimix::string desc = "kills by process name `";
                        desc += name_hit;
                        desc += "` via `killall`, which also matches the agent process";
                        if (finish(kimix::optional<kimix::string>(std::move(desc)), "killall")) {
                            return result;
                        }
                    }
                }
            }
        }
    }

    // 5. wmic (safety.py 744-768).
    {
        const kimix::vector<word_match> matches = find_word_opt_exe(text, "wmic");
        for (const word_match &m : matches) {
            const kimix::string_view segment = segment_text(text, m.end);
            // \b(?:delete|terminate)\b must be present in the segment
            if (find_word(segment, "delete").empty() &&
                find_word(segment, "terminate").empty()) {
                continue;
            }
            // First re.search: processid\s*=\s*(\d+)
            // Second re.search: processid\s*=\s*\$\{?var\}?
            // They are independent scans (Python runs the var search even when
            // the numeric search matched but the PID was not protected).
            bool numeric_hit = false;
            {
                size_t pos = 0;
                while (pos < segment.size()) {
                    pos = segment.find("processid", pos);
                    if (pos == kimix::string_view::npos) {
                        break;
                    }
                    size_t i = pos + 9;
                    while (i < segment.size() && is_space(segment[i])) {
                        ++i;
                    }
                    if (i >= segment.size() || segment[i] != '=') {
                        pos += 9;
                        continue;
                    }
                    ++i;
                    while (i < segment.size() && is_space(segment[i])) {
                        ++i;
                    }
                    size_t db = i;
                    while (i < segment.size() && is_digit(segment[i])) {
                        ++i;
                    }
                    if (i == db) {
                        pos += 9; // not numeric; keep searching for another
                        continue;
                    }
                    const kimix::string_view digits = segment.substr(db, i - db);
                    // Python int() is unbounded; a run longer than 18 digits
                    // never equals an int64_t protected PID, so skip it and
                    // keep scanning for a later processid= token.
                    if (digits.size() <= 18) {
                        int64_t pid = 0;
                        for (char c : digits) {
                            pid = pid * 10 + (c - '0');
                        }
                        if (pid_protected(st, pid)) {
                            kimix::string desc = "targets PID ";
                            desc += kimix::string(digits.data(), digits.size());
                            desc += " via `wmic`, which is the agent process or one of its parent processes";
                            if (finish(kimix::optional<kimix::string>(std::move(desc)), "wmic")) {
                                return result;
                            }
                        }
                        numeric_hit = true; // re.search stops at first match
                        break;
                    }
                    pos += 9;
                }
            }
            if (!numeric_hit) {
                size_t pos = 0;
                while (pos < segment.size()) {
                    pos = segment.find("processid", pos);
                    if (pos == kimix::string_view::npos) {
                        break;
                    }
                    size_t i = pos + 9;
                    while (i < segment.size() && is_space(segment[i])) {
                        ++i;
                    }
                    if (i >= segment.size() || segment[i] != '=') {
                        pos += 9;
                        continue;
                    }
                    ++i;
                    while (i < segment.size() && is_space(segment[i])) {
                        ++i;
                    }
                    if (i >= segment.size() || segment[i] != '$') {
                        pos += 9;
                        continue;
                    }
                    ++i;
                    if (i < segment.size() && segment[i] == '{') {
                        ++i;
                    }
                    size_t vb = i;
                    while (i < segment.size() &&
                           ((segment[i] >= 'a' && segment[i] <= 'z') ||
                            (segment[i] >= 'A' && segment[i] <= 'Z') || segment[i] == '_')) {
                        ++i;
                    }
                    if (i == vb) {
                        pos += 9;
                        continue;
                    }
                    size_t ve = i;
                    while (i < segment.size() &&
                           ((segment[i] >= 'a' && segment[i] <= 'z') ||
                            (segment[i] >= 'A' && segment[i] <= 'Z') ||
                            (segment[i] >= '0' && segment[i] <= '9') ||
                            segment[i] == '_')) {
                        ++i;
                    }
                    // \}? optional, like the Python pattern
                    const kimix::string var = lower_copy(segment.substr(vb, ve - vb));
                    const kimix::vector<int64_t> *pids = loop_source_find(st.loops, var);
                    if (pids != nullptr) {
                        for (int64_t pid : *pids) {
                            if (pid_protected(st, pid)) {
                                const kimix::string_view var_original = segment.substr(vb, ve - vb);
                                kimix::string desc = "targets PID ";
                                desc += kimix::format("{}", pid);
                                desc += " via `wmic` through loop variable `$";
                                desc += var_original;
                                desc += "` (bound to PIDs ";
                                desc += loop_pid_list(
                                    kimix::span<const int64_t>(pids->data(), pids->size()));
                                desc += "), which is the agent process or one of its parent processes";
                                if (finish(kimix::optional<kimix::string>(std::move(desc)), "wmic")) {
                                    return result;
                                }
                            }
                        }
                    }
                    break; // first var match wins, like re.search
                }
            }
        }
    }

    return result;
}

kimix::optional<kimix::string> detect_self_kill(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline,
    tool_status &status) {
    const self_kill_result r =
        detect_self_kill_ex(command, protected_pids, image_names, cmdline);
    status = r.status;
    if (r.status != tool_status::ok) {
        return {};
    }
    return r.description;
}

kimix::optional<kimix::string> self_kill_hint(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline,
    int64_t agent_pid,
    tool_status &status) {
    status = tool_status::ok;
    bool only_space = true;
    for (char c : command) {
        if (!is_space(c)) {
            only_space = false;
            break;
        }
    }
    if (command.empty() || only_space) {
        return {};
    }
    kimix::vector<kimix::string> variants;
    command_detection_variants(command, variants);
    for (const kimix::string &variant : variants) {
        tool_status variant_status = tool_status::ok;
        const kimix::optional<kimix::string> desc = detect_self_kill(
            variant, protected_pids, image_names, cmdline, variant_status);
        if (variant_status == tool_status::unsupported) {
            status = tool_status::unsupported;
            return {};
        }
        if (desc.has_value()) {
            kimix::string out = "The command ";
            out += *desc;
            out += ". Executing it would terminate this agent session (current agent PID: ";
            out += kimix::format("{}", agent_pid);
            out += "). ";
            out += k_self_kill_guidance;
            return out;
        }
    }
    return {};
}

// ===========================================================================
// PowerShell 7.x -> 5.1 transform, fixer, hardline floor, RTK rewrite
// ===========================================================================

namespace {

inline bool pwsh_is_ascii(kimix::string_view s) noexcept {
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80u) {
            return false;
        }
    }
    return true;
}

const char *fix_warning_for_code(int code) {
    switch (code) {
    case 1:
        return "The command has an unclosed double-quoted string; appended a closing `\"` at the end to make it a legal PowerShell command.";
    case 2:
        return "The command has an unclosed single-quoted string; appended a closing `'` at the end to make it a legal PowerShell command.";
    case 3:
        return "The command has an unclosed double-quoted here-string; appended a newline and `\"@` at the end to close it.";
    case 4:
        return "The command has an unclosed single-quoted here-string; appended a newline and `'@` at the end to close it.";
    case 5:
        return "The command has an unclosed block comment `<#`; appended `#>` at the end to close it.";
    case 6:
        return "The command ends with a line comment; appended a newline so the trailing comment does not swallow the try/catch wrapper used to execute the command.";
    case 7:
        return "The command ends with the `--%` stop-parsing marker; appended a newline so the wrapper is not passed literally to the native command.";
    case 8:
        return "The command contains only comments; appended a newline and a no-op `$null` statement so the try/catch wrapper has a statement to execute.";
    case 9:
        return "The command ends with a backtick line-continuation; appended a newline so the continuation does not join with the try/catch wrapper used to execute the command.";
    default:
        return "";
    }
}

} // namespace

transform_result pwsh_transform(kimix::string_view code) {
    transform_result result;
    if (!pwsh_is_ascii(code)) {
        result.status = tool_status::unsupported;
        return result;
    }
    kimix::vector<kimix::runtime::parse::edit> edits;
    scan_shell(kimix::runtime::parse::shell_dialect::PWSH_TRANSFORM, code,
               edits, &result.command, nullptr, nullptr, nullptr,
               &result.warnings);
    result.status = tool_status::ok;
    return result;
}

fix_result fix_pwsh_command(kimix::string_view command) {
    fix_result result;
    if (!pwsh_is_ascii(command)) {
        result.valid = false;
        result.changed = false;
        return result;
    }
    kimix::vector<kimix::runtime::parse::edit> edits;
    kimix::string transformed;
    int warning_code = 0;
    scan_shell(kimix::runtime::parse::shell_dialect::PWSH_FIX, command,
               edits, &transformed, nullptr, nullptr, &warning_code, nullptr);
    if (warning_code == -1) {
        result.valid = false;
        result.changed = false;
        return result;
    }
    result.valid = true;
    result.command = std::move(transformed);
    result.changed = (warning_code != 0);
    if (result.changed) {
        result.warning = fix_warning_for_code(warning_code & 0x0F);
    }
    return result;
}

hardline_result check_hardline_blocked(kimix::string_view command) {
    hardline_result result;
    if (!pwsh_is_ascii(command)) {
        return result;
    }
    const kimix::runtime::tools::hardline_result hr =
        kimix::runtime::tools::check_hardline_blocked(command);
    result.blocked = hr.blocked;
    if (hr.description.has_value()) {
        result.description = hr.description.value();
    }
    return result;
}

kimix::builtin_tools::bash::rewrite_result
maybe_rewrite_with_rtk(kimix::string_view command,
                       bool token_kill,
                       bool rtk_available,
                       kimix::string_view rtk_binary_path,
                       bool exclude_read) {
    return kimix::builtin_tools::bash::maybe_rewrite_shell_command_with_rtk(
        command, token_kill, rtk_available, rtk_binary_path, exclude_read,
        /*pwsh=*/true);
}

Pwsh::Pwsh(kimix::builtin_tools::Session *session)
    : Tool(session) {}

void Pwsh::operator()(const kimix::builtin_tools::ToolParams *parameters) {
    using namespace kimix::builtin_tools;
    _last_result.clear();
    ToolParams result;
    if (parameters == nullptr) {
        result.values["status"] = ValueElement::make_string(
            kimix::string("invalid_input"));
        result.values["message"] = ValueElement::make_string(
            kimix::string("no parameters provided"));
        result.serialize(_last_result);
        return;
    }

    const auto *mode_val = parameters->get("mode");
    kimix::string mode = "transform";
    if (mode_val != nullptr && mode_val->is_string()) {
        mode = mode_val->as_string();
    }

    const auto *cmd_val = parameters->get("command");
    if (cmd_val == nullptr || !cmd_val->is_string()) {
        result.values["status"] = ValueElement::make_string(
            kimix::string("invalid_input"));
        result.values["message"] = ValueElement::make_string(
            kimix::string("missing or invalid 'command'"));
        result.serialize(_last_result);
        return;
    }
    const kimix::string_view command = cmd_val->as_string();

    if (mode == "transform") {
        const transform_result tr = pwsh_transform(command);
        result.values["status"] = ValueElement::make_string(
            tr.status == tool_status::ok ? "ok" : "unsupported");
        result.values["command"] = ValueElement::make_string(tr.command);
        kimix::vector<ValueElement> warns;
        warns.reserve(tr.warnings.size());
        for (const kimix::string &w : tr.warnings) {
            warns.push_back(ValueElement::make_string(w));
        }
        result.values["warnings"] = ValueElement::make_array(std::move(warns));
    } else if (mode == "fix") {
        const fix_result fr = fix_pwsh_command(command);
        result.values["status"] = ValueElement::make_string(
            fr.valid ? "ok" : "error");
        result.values["valid"] = ValueElement::make_bool(fr.valid);
        result.values["changed"] = ValueElement::make_bool(fr.changed);
        result.values["command"] = ValueElement::make_string(fr.command);
        result.values["warning"] = ValueElement::make_string(fr.warning);
    } else if (mode == "hardline") {
        const hardline_result hr = check_hardline_blocked(command);
        result.values["status"] = ValueElement::make_string("ok");
        result.values["blocked"] = ValueElement::make_bool(hr.blocked);
        result.values["description"] = ValueElement::make_string(hr.description);
    } else if (mode == "rtk_rewrite") {
        bool token_kill = true;
        bool rtk_available = false;
        kimix::string rtk_binary_path;
        bool exclude_read = false;
        const auto *token_kill_val = parameters->get("token_kill");
        if (token_kill_val != nullptr && token_kill_val->is_bool()) {
            token_kill = token_kill_val->as_bool();
        }
        const auto *rtk_available_val = parameters->get("rtk_available");
        if (rtk_available_val != nullptr && rtk_available_val->is_bool()) {
            rtk_available = rtk_available_val->as_bool();
        }
        const auto *rtk_path_val = parameters->get("rtk_binary_path");
        if (rtk_path_val != nullptr && rtk_path_val->is_string()) {
            rtk_binary_path = rtk_path_val->as_string();
        }
        const auto *exclude_read_val = parameters->get("exclude_read");
        if (exclude_read_val != nullptr && exclude_read_val->is_bool()) {
            exclude_read = exclude_read_val->as_bool();
        }
        const auto rr = maybe_rewrite_with_rtk(command, token_kill, rtk_available,
                                             rtk_binary_path, exclude_read);
        result.values["status"] = ValueElement::make_string("ok");
        result.values["command"] = ValueElement::make_string(rr.segment);
        result.values["changed"] = ValueElement::make_bool(rr.changed);
    } else if (mode == "self_kill_hint") {
        int64_t agent_pid = 0;
        kimix::unordered_set<int64_t> protected_pids;
        kimix::unordered_set<kimix::string, kimix::string_hash> image_names;
        kimix::string cmdline;
        const auto *agent_pid_val = parameters->get("agent_pid");
        if (agent_pid_val != nullptr && agent_pid_val->is_int()) {
            agent_pid = agent_pid_val->as_int();
        }
        const auto *pids_val = parameters->get("protected_pids");
        if (pids_val != nullptr && pids_val->is_array()) {
            for (const ValueElement &v : pids_val->as_array()) {
                if (v.is_int()) {
                    protected_pids.insert(v.as_int());
                }
            }
        }
        const auto *names_val = parameters->get("image_names");
        if (names_val != nullptr && names_val->is_array()) {
            for (const ValueElement &v : names_val->as_array()) {
                if (v.is_string()) {
                    image_names.insert(v.as_string());
                }
            }
        }
        const auto *cmdline_val = parameters->get("cmdline");
        if (cmdline_val != nullptr && cmdline_val->is_string()) {
            cmdline = cmdline_val->as_string();
        }
        tool_status status = tool_status::ok;
        const auto hint = self_kill_hint(command, protected_pids, image_names,
                                         cmdline, agent_pid, status);
        result.values["status"] = ValueElement::make_string(
            status == tool_status::ok ? "ok" : "unsupported");
        result.values["blocked"] = ValueElement::make_bool(hint.has_value());
        if (hint.has_value()) {
            result.values["description"] = ValueElement::make_string(*hint);
        }
    } else {
        result.values["status"] = ValueElement::make_string(
            kimix::string("invalid_input"));
        result.values["message"] = ValueElement::make_string(
            kimix::string("unknown mode"));
    }
    result.serialize(_last_result);
}

} // namespace kimix::builtin_tools::pwsh
