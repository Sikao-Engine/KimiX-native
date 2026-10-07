// grep_engine.cpp - Implementation of the native grep search engine.
// See grep_engine.h for the design notes (ripgrep-inspired: whole-buffer
// zero-copy line scan, literal fast path, single-pass Teddy-style any-of
// scan for literal alternations, per-worker regexes over a shared size-
// ordered claim queue merged back into walk order, 64 KiB NUL binary sniff).
// Semantics mirror the previous inline
// native_io branch of Grep::operator() byte-for-byte where observable.
//
// Threading model: the engine NEVER creates a scheduler, and it only touches
// fiber when the file list is big enough to fan out (>= 8 files) - a small
// scan runs inline on the calling thread with no bind cost. When fan-out IS
// required it always runs over kimix::fiber: a calling thread already bound to
// a pool (the host binds one at process/thread start - see cli_main() and the
// background sub-agent worker in agent_tool.cpp) spreads its chunks over that
// AMBIENT pool; an unbound caller transiently binds the process-wide shared
// pool (kimix::fiber::shared_scheduler()) for the duration of the call and
// unbinds it again on the way out (ge::fiber_bind_guard below - the same
// pattern as fiber::schedule_background() and the Python host's binding
// guard). Binding is not creating: the pool is the host's, never the engine's.
// Only where fiber cannot be made available at all does the search fail, via
// tool_status::unsupported (run_grep never throws).
//
// Compiled into the kimix-llm static library with a unity (jumbo) batch, so
// all file-local helpers live inside an anonymous namespace and carry
// grep-engine-specific names (no file-scope same-named globals).

#include "builtin_tools/grep_engine.h"

#include "builtin_tools/grep_tool.h" // fnmatch_ascii (shared with grep_tool.cpp)
#include "builtin_tools/regex_lite.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

// x64 SIMD for the single-pass multi-literal any-of scan (Teddy-style).
#if defined(_M_X64) || defined(__x86_64__)
#define KIMIX_GREP_ENGINE_X64_SIMD 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h> // __cpuid for the SSSE3 runtime check
#endif
#else
#define KIMIX_GREP_ENGINE_X64_SIMD 0
#endif


#include <core/fiber.h> // the file chunks are fanned out over kimix::fiber

namespace kimix::builtin_tools::grep {

namespace {

namespace ge {

constexpr uint64_t k_max_file_bytes = 4ull * 1024 * 1024;
constexpr size_t k_binary_sniff_bytes = 64 * 1024;

// One line of the file buffer: a view (start offset + length with one
// trailing '\r' already stripped). Zero-copy: no per-line string allocs.
struct line_view {
    size_t start = 0;
    size_t len = 0;
};

// One searchable file collected by the walk: the display path (the exact
// kimix::to_string(entry) spelling the results report) plus the native
// fs::path it came from, so scan_file never re-decodes the UTF-8 string and
// never re-checks the file type (collect_files already established it is a
// regular file).
struct walk_entry {
    kimix::string display;
    kimix::filesystem::path native;
      // Byte size as the walk saw it (0 when the size query failed). A BALANCING
      // HINT only - see ge::claim_order: a file may grow, shrink or change type
    // between the walk and the scan, so scan_file re-stats and re-applies the
    // 4 MiB cap itself. Cost: the directory-entry attributes carry the size on
    // Windows (a recursive walk already reads them for the file type), while a
    // POSIX std::filesystem directory_entry has no size cache and pays a stat
    // per entry here, so run_grep only asks for hints during the walk when the
    // bound pool is already wider than one worker (fan-out possible); an
    // unbound caller cannot know the list size beforehand, and when its list
    // turns out big enough to fan out run_grep pays one deferred stat pass
    // instead (see the LPT call site). Measured on this tree a walk-time query
    // is ~1-3 us per collected file (~+2-4 ms on a 2000-file walk, against
    // ~15 ms saved on the skewed one) - at most one stat per file, which is
    // what the balance buys back many times over when the sizes skew.
    uintmax_t size_hint = 0;
};

// RAII fiber binding for the calling thread: makes the engine's fan-out
// possible from ANY thread without the engine ever owning a scheduler. A
// thread already bound to a pool keeps its AMBIENT pool and this guard does
// nothing (marl allows exactly one scheduler per thread); an unbound thread
// transiently binds the process-wide shared pool, which is intentionally
// never destroyed, so the destructor only ever UNBINDS what the constructor
// bound and a thread detached here cannot hang. Mirrors fiber_binding_guard
// in src/runtime/py/module.cpp and the transient self-binding of
// kimix::fiber::schedule_background().
class fiber_bind_guard {
public:
    // `enable` false: the caller already knows no fan-out will happen (a file
    // list below the fan-out threshold), so an unbound thread is left unbound
    // and the scan runs inline - binding would spawn a marl worker thread for
    // nothing. `enable` true: an unbound thread gets the shared pool, because
    // the chunks must spread over fiber (that is the contract this guard
    // implements).
    explicit fiber_bind_guard(bool enable) noexcept {
        if (enable && !kimix::fiber::is_bound()) {
            kimix::fiber::shared_scheduler().bind();
            _bound = kimix::fiber::is_bound(); // unbind only what we bound
        }
    }
    fiber_bind_guard(const fiber_bind_guard &) = delete;
    fiber_bind_guard &operator=(const fiber_bind_guard &) = delete;
    ~fiber_bind_guard() noexcept {
        if (_bound) {
            kimix::fiber::shared_scheduler().unbind();
        }
    }
    // True when the calling thread can submit fiber work (its own ambient
    // pool, or the shared pool bound above). False either because `enable`
    // was false (nothing was bound; inline scan only) or on a host where
    // fiber is unavailable - run_grep only errors on the latter.
    [[nodiscard]] bool bound() const noexcept { return kimix::fiber::is_bound(); }

private:
    bool _bound = false;
};

// One MATCHED file's output, addressable by the file's position in the
// walk-order list. The shared claim queue hands a worker arbitrary files in
// size order, so the merge cannot assume "worker order == walk order": it
// sorts the gathered blocks by this index before emitting anything.
struct file_block {
    uint32_t index = 0;                  // position in the walk-order file list
    grep_file_result res;                // display path + matching-line count
    kimix::vector<kimix::string> lines;  // this file's rendered lines (content/count)
    kimix::vector<uint8_t> line_match;   // parallel to lines
};

// Per-worker search output. A worker produces a block ONLY for a file that
// matched (unmatched files add nothing, as before); its blocks come out in
// claim order, so they are NOT walk-ordered - the merge re-sorts the gathered
// blocks by file_block.index. No mutexes: every worker appends to its own
// chunk_output and to nothing else.
struct chunk_output {
    kimix::vector<file_block> blocks;
    int64_t total_matches = 0;
};

char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// -- literal fast path ------------------------------------------------------

// True when `pat` is a pure literal: no unescaped metacharacters among
// .^$*+?()[]{}|. Escapes: \d \D \w \W \s \S \b \B and hex/unicode escapes are
// NOT literal; \n \t \r \f \v ARE literal control chars; escaped punctuation
// is the literal char. Unknown alphabetic escapes and digit escapes (\0 is
// the NUL character, \1-\9 are back-references) are NOT literal either: only
// the regex engine assigns them meaning, so they go to the regex path and the
// validator decides - the fast path must never re-interpret them as plain
// digits (that would diverge from the inline-regex semantics run_grep
// mirrors). Unescapes into `lit` (also set on the false paths that consume a
// prefix - callers ignore it then).
bool extract_literal(kimix::string_view pat, kimix::string &lit) noexcept {
    lit.clear();
    for (size_t i = 0; i < pat.size(); ++i) {
        const char c = pat[i];
        switch (c) {
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
            return false;
        case '\\':
            if (++i >= pat.size()) {
                return false; // dangling backslash: let the regex validator reject it
            }
            switch (pat[i]) {
            case 'd':
            case 'D':
            case 'w':
            case 'W':
            case 's':
            case 'S':
            case 'b':
            case 'B': // classes / anchors: regex path
            case 'x':
            case 'u':
            case 'U': // hex / unicode escapes: regex path
                return false;
            case 'n':
                lit.push_back('\n');
                break;
            case 't':
                lit.push_back('\t');
                break;
            case 'r':
                lit.push_back('\r');
                break;
            case 'f':
                lit.push_back('\f');
                break;
            case 'v':
                lit.push_back('\v');
                break;
            default:
                if (is_alpha(pat[i]) || (pat[i] >= '0' && pat[i] <= '9')) {
                    return false; // unknown alphabetic/digit escape: regex decides
                }
                lit.push_back(pat[i]); // escaped punctuation is the literal char
                break;
            }
            break;
        default:
            lit.push_back(c);
            break;
        }
    }
    return true;
}

// Direct substring search for the literal fast path and for the regex prefilter
// gate: memchr on a probe byte + memcmp; ASCII folding byte compare under
// ignore_case (matches the regex_lite A-Za-z fold, see rx_fold in
// regex_lite.cpp: it maps 'A'-'Z' and nothing else, exactly like lower_ascii).
//
// Probe byte: memchr skips haystack runs at SIMD speed only when the probed
// byte is RARE in the text, so instead of the first needle byte we pick the
// RAREST one (static code/English frequency table below): any position, not
// just the first. A byte that never needs folding is required for the raw
// memchr path - the fold only touches A-Za-z - so under ignore_case only
// non-letter needle bytes are candidates, and a needle made entirely of
// letters (the common -i case, "hit") still falls back to the per-byte folded
// scan. Ties resolve to the earliest position (deterministic). Verified
// candidate starts advance one byte at a time (off+1), so an occurrence that
// starts right after a failed candidate is still found.
//
// Static byte-frequency table (0 = rarest .. 255 = most common), tuned for
// code/English haystacks: ' ' and e/t/a lead; `_` ranks high because code is
// full of snake_case; backtick, #, $, @, ~ and friends rank as the rarest
// probe bytes, so a needle carrying one of them skips nearly the whole
// haystack in a single memchr.
constexpr uint8_t k_probe_byte_freq[256] = {
    // 0x00-0x0F: controls (tab/newline/CR get real text frequencies)
    40, 40, 40, 40, 40, 40, 40, 40, 40, 180, 200, 40, 40, 170, 40, 40,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    // 0x20-0x2F: space .. '/'
    250, 60, 170, 30, 35, 55, 65, 120, 150, 150, 60, 70, 160, 140, 190, 130,
    // 0x30-0x3F: digits, ':', ';', '<', '=', '>', '?'
    150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 145, 150, 75, 140, 75, 85,
    // 0x40-0x4F: '@', A-O
    25, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165,
    // 0x50-0x5F: P-Z, '[', '\', ']', '^', '_', '`'
    165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 165, 110, 50, 110, 45, 175,
    // 0x60-0x6F: '`', a-o
    20, 230, 152, 190, 192, 240, 172, 160, 195, 222, 35, 80, 205, 178, 220, 225,
    // 0x70-0x7F: p-z, '{', '|', '}', '~', DEL
    168, 40, 212, 215, 235, 185, 95, 158, 65, 155, 30, 70, 72, 70, 22, 40,
    // 0x80-0xFF: non-ASCII (never a probe candidate for ASCII needles)
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
};

bool literal_in_line(kimix::string_view hay, kimix::string_view needle,
                     bool fold_case) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    const size_t n = needle.size();
    const size_t last = hay.size() - n; // last start offset a needle can have in hay
    // Rarest probeable byte: any position when case-sensitive, any non-letter
    // position under -i (probe == n means "all letters": folded scan below).
    size_t probe = n;
    uint8_t best_freq = 0;
    for (size_t i = 0; i < n; ++i) {
        if (fold_case && is_alpha(needle[i])) {
            continue; // a letter's raw byte never matches its folded forms
        }
        const uint8_t f = k_probe_byte_freq[static_cast<unsigned char>(needle[i])];
        if (probe == n || f < best_freq) {
            best_freq = f;
            probe = i;
            if (f == 0) {
                break; // cannot get rarer than this
            }
        }
    }
    if (probe < n) {
        // Exact-byte probe (folding off: any rare byte; folding on: the rarest
        // non-letter). Candidate start = hit - probe.
        const char want = needle[probe];
        size_t pos = 0;
        while (pos <= last) {
            const void *hit =
                std::memchr(hay.data() + pos + probe, want,
                            static_cast<size_t>(last - pos) + 1u);
            if (hit == nullptr) {
                return false;
            }
            const size_t off = static_cast<size_t>(
                                   static_cast<const char *>(hit) - hay.data()) -
                               probe;
            bool eq = true;
            if (!fold_case) {
                // Bulk compare: memcmp is SIMD-optimized (the probe byte itself
                // already matches, comparing it again costs nothing).
                eq = std::memcmp(hay.data() + off, needle.data(), n) == 0;
            } else {
                for (size_t k = 0; k < n; ++k) {
                    if (lower_ascii(hay[off + k]) != lower_ascii(needle[k])) {
                        eq = false;
                        break;
                    }
                }
            }
            if (eq) {
                return true;
            }
            pos = off + 1u;
        }
        return false;
    }
    // All-needle letters under -i: no byte can be probed raw, scan folded.
    const char want = lower_ascii(needle[0]);
    size_t pos = 0;
    while (pos <= last) {
        size_t off = pos;
        while (off <= last && lower_ascii(hay[off]) != want) {
            ++off;
        }
        if (off > last) {
            return false;
        }
        bool eq = true;
        for (size_t k = 1; k < n; ++k) {
            if (lower_ascii(hay[off + k]) != lower_ascii(needle[k])) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return true;
        }
        pos = off + 1u;
    }
    return false;
}

// True when `s` contains any of the four UTF-8 lead bytes whose OVERLONG form
// regex_lite's lenient decoder folds back into an ASCII code point (2-byte
// leads 0xC0/0xC1 cover cp < 0x80, 3-byte 0xE0 and 4-byte 0xF0 reach the same
// with their continuation bytes at 0x80 - e.g. bytes C1 81 decode to 'A').
// Every other lead byte decodes to a code point >= 0x80, and an ASCII byte is
// never consumed as a continuation, so on a string WITHOUT such a lead byte the
// engine's code-point matching of an ASCII needle is exactly equivalent to the
// byte-level literal_in_line test. That equivalence is what lets the prefilter
// below skip the regex engine without changing a single observable result.
bool may_hide_ascii_cp(kimix::string_view s) noexcept {
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        if (b >= 0xC0u && (b == 0xC0u || b == 0xC1u || b == 0xE0u || b == 0xF0u)) {
            return true;
        }
    }
    return false;
}

// Same question for a whole read buffer, answered with four SIMD memchr passes
// instead of a byte loop: only reached on the "no needle anywhere" path, where
// the alternative is scanning every line anyway.
bool buffer_may_hide_ascii_cp(kimix::string_view s) noexcept {
    static const unsigned char k_leads[4] = {0xC0u, 0xC1u, 0xE0u, 0xF0u};
    for (const unsigned char lead : k_leads) {
        if (std::memchr(s.data(), static_cast<char>(lead), s.size()) != nullptr) {
            return true;
        }
    }
    return false;
}

// -- required-literal extraction (ripgrep-style prefilter) -------------------

// Number of bytes regex_lite::decode_utf8 consumes for the code point starting
// at `i` - the SAME fallbacks (invalid lead byte, truncated sequence, or a
// non-continuation next byte all collapse to a one-byte step), so a pattern
// walk stays in step with what the engine compiles.
size_t decoded_char_len(kimix::string_view pat, size_t i) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(pat[i]);
    size_t extra = 0;
    if (b0 < 0x80u) {
        return 1;
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        extra = 1;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        extra = 2;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        extra = 3;
    } else {
        return 1; // invalid lead: single-byte fallback
    }
    if (i + extra >= pat.size()) {
        return 1; // truncated sequence
    }
    for (size_t k = 1; k <= extra; ++k) {
        if ((static_cast<unsigned char>(pat[i + k]) & 0xC0u) != 0x80u) {
            return 1;
        }
    }
    return extra + 1;
}

// What one atom of a flat pattern demands.
enum class atom_kind : uint8_t {
    literal,   // these exact bytes (always a single ASCII byte)
    opaque,    // consumes one code point whose bytes we cannot name
    zerowidth, // '^' / '$': a position test, consumes nothing
    bail,      // anything the concat-only reasoning below does not cover
};

// How many times the atom just read must occur.
enum class quant_kind : uint8_t { none, star, plus, quest, bail };

// Consume the atom at `i` (advancing `i`) and report what it demands. Escape
// table is the one ge::extract_literal uses: \d \D \w \W \s \S \b \B, hex and
// unicode escapes and unknown alphabetic/digit escapes bail; \n \t \r \f \v and
// escaped punctuation are literal chars. A multi-byte pattern code point (or an
// escaped byte >= 0x80) is OPAQUE, never a required byte: the engine matches
// code points, so requiring its raw bytes could skip a line that spells the same
// code point differently.
atom_kind read_atom(kimix::string_view pat, size_t &i, kimix::string &bytes) noexcept {
    bytes.clear();
    const char c = pat[i];
    switch (c) {
    case '(':
    case ')':
    case '|': // groups and alternation: no single required literal
    case '{':
    case '}': // counted quantifier / stray brace: refused, see below
    case '*':
    case '+':
    case '?': // a quantifier with no operand (regex_lite rejects these too)
        return atom_kind::bail;
    case '^':
    case '$':
        ++i;
        return atom_kind::zerowidth;
    case '.':
        ++i;
        return atom_kind::opaque;
    case '[': {
        // Character class: consume the whole [...] . A '\]' inside is escaped,
        // and (as in parse_class_body) a ']' in the FIRST position is a member.
        size_t j = i + 1;
        if (j < pat.size() && (pat[j] == '^' || pat[j] == '!')) {
            ++j;
        }
        if (j < pat.size() && pat[j] == ']') {
            ++j;
        }
        while (j < pat.size()) {
            if (pat[j] == '\\') {
                j += 2;
                continue;
            }
            if (pat[j] == ']') {
                i = j + 1;
                return atom_kind::opaque;
            }
            ++j;
        }
        return atom_kind::bail; // unterminated class: the validator decides
    }
    case '\\': {
        if (i + 1 >= pat.size()) {
            return atom_kind::bail; // dangling backslash
        }
        const char e = pat[i + 1];
        switch (e) {
        case 'd':
        case 'D':
        case 'w':
        case 'W':
        case 's':
        case 'S':
        case 'b':
        case 'B':
        case 'x':
        case 'u':
        case 'U':
            return atom_kind::bail;
        case 'n':
            bytes.push_back('\n');
            i += 2;
            return atom_kind::literal;
        case 't':
            bytes.push_back('\t');
            i += 2;
            return atom_kind::literal;
        case 'r':
            bytes.push_back('\r');
            i += 2;
            return atom_kind::literal;
        case 'f':
            bytes.push_back('\f');
            i += 2;
            return atom_kind::literal;
        case 'v':
            bytes.push_back('\v');
            i += 2;
            return atom_kind::literal;
        default:
            if (is_alpha(e) || (e >= '0' && e <= '9')) {
                return atom_kind::bail; // unknown alphabetic/digit escape
            }
            i += 2;
            if (static_cast<unsigned char>(e) >= 0x80u) {
                return atom_kind::opaque; // escaped raw byte: never required
            }
            bytes.push_back(e); // escaped punctuation is the literal char
            return atom_kind::literal;
        }
    }
    default: {
        const size_t clen = decoded_char_len(pat, i);
        if (clen == 1u && static_cast<unsigned char>(c) < 0x80u) {
            bytes.push_back(c);
            ++i;
            return atom_kind::literal;
        }
        i += clen;
        return atom_kind::opaque;
    }
    }
}

// Consume the quantifier (if any) after an atom, including its lazy '?' marker.
// '{' bails by fiat: reasoning about {n,m} optionality is not worth the risk.
quant_kind read_quantifier(kimix::string_view pat, size_t &i) noexcept {
    if (i >= pat.size()) {
        return quant_kind::none;
    }
    const char q = pat[i];
    if (q == '{') {
        return quant_kind::bail;
    }
    if (q != '*' && q != '+' && q != '?') {
        return quant_kind::none;
    }
    ++i;
    if (i < pat.size() && pat[i] == '?') {
        ++i; // lazy marker
    } else if (i < pat.size() && pat[i] == '+') {
        return quant_kind::bail; // possessive: regex_lite rejects the pattern
    }
    if (i < pat.size()) {
        const char t = pat[i];
        if (t == '*' || t == '+' || t == '?' || t == '{') {
            return quant_kind::bail; // stacked quantifiers: refuse to reason
        }
    }
    switch (q) {
    case '*':
        return quant_kind::star;
    case '+':
        return quant_kind::plus;
    default:
        return quant_kind::quest;
    }
}

// Finds a byte string EVERY match of the FLAT pattern `pat` must contain, so a
// line - or a whole file buffer - without it cannot match and the regex engine
// never runs there. Returns false whenever no such reasoning is safe.
//
// Why the runs it produces are required: a flat pattern is a concatenation (no
// '(' ')' '|'), so every match traverses every element in order and the matched
// text is the concatenation of what the elements consume. '^'/'$' consume
// nothing, so they leave their neighbours adjacent; '.' and a class consume one
// code point whose bytes are unknown, so they SPLIT a run; a literal char under
// '*' or '?' may be absent, so it is not required and it also splits; a literal
// char under '+' is required but may repeat, which is handled by keeping the run
// that ENDS at its first occurrence and restarting the run AT that char, so the
// run continues with what follows the last occurrence - "fo+bar" therefore
// yields "obar", NOT "fobar" (which "foobar" does not contain). Counted
// quantifiers, groups, alternation and the regex-lite-special escapes bail out:
// a missing prefilter only costs speed, a wrong one costs correctness.
bool extract_required_literal(kimix::string_view pat, kimix::string &lit) noexcept {
    lit.clear();
    kimix::string run;
    kimix::string bytes;
    size_t i = 0;
    while (i < pat.size()) {
        const atom_kind a = read_atom(pat, i, bytes);
        if (a == atom_kind::bail) {
            return false;
        }
        const quant_kind q = read_quantifier(pat, i);
        if (q == quant_kind::bail) {
            return false;
        }
        if (a == atom_kind::zerowidth) {
            continue; // anchor (possibly quantified): consumes nothing, run intact
        }
        if (a == atom_kind::opaque || q == quant_kind::star || q == quant_kind::quest) {
            // Either an unknown code point or an optional char: nothing on the
            // far side of it is adjacent to the run built so far.
            if (run.size() > lit.size()) {
                lit = run;
            }
            run.clear();
            continue;
        }
        if (q == quant_kind::plus) {
            run.append(bytes); // the FIRST occurrence continues the run...
            if (run.size() > lit.size()) {
                lit = run;
            }
            run.assign(bytes.data(), bytes.size()); // ... the LAST one starts the next
            continue;
        }
        run.append(bytes); // exactly-once literal char
    }
    if (run.size() > lit.size()) {
        lit = run;
    }
    return !lit.empty();
}

constexpr size_t k_max_alt_literals = 16; // beyond this the any-of scan loses

// -- single-pass multi-literal any-of scan (Teddy-style) ---------------------

// "Does the haystack contain ANY of the alternation's literals" answered in
// ONE pass instead of one literal_in_line scan per branch. Ripgrep's answer to
// the same question is the Teddy multi-string matcher; this is a small
// Teddy-style two-byte fingerprint filter built once per scan plan:
//
//   Each alt contributes its first two bytes. The low NIBBLE of an ASCII
//   letter is case-folding invariant ('a'==0x61 and 'A'==0x41 both end in
//   nibble 1), and every alt is pure ASCII (extract_literal_alternation
//   enforces it), so the nibble fingerprint needs no fold handling at all:
//   mask_lo[b0 & 0xF] names the alts whose first byte has that nibble and
//   mask_hi[b1 & 0xF] the alts whose second byte has it; a position is a
//   candidate iff the two masks intersect. Candidates are verified with the
//   exact folded/exact compare literal_in_line uses, so a fingerprint hit
//   that is not a real occurrence costs one short memcmp, never a wrong
//   answer. The candidate set is a SUPERSET of every real occurrence (the
//   true start position has both fingerprint nibbles present), which is what
//   keeps the any-of answer exactly equivalent to the per-alt scans it
//   replaces.
struct any_of_scanner {
    static constexpr size_t k_max = 16; // == k_max_alt_literals
    uint16_t mask_lo[16] = {}; // alt-bit set when (alt[0] & 0xF) == index
    uint16_t mask_hi[16] = {}; // alt-bit set when (alt[1] & 0xF) == index
    // The same masks split into bytes for the pshufb (SSSE3) block loop.
    alignas(16) uint8_t pshuf[4][16] = {};

    void build(const kimix::vector<kimix::string> &alts) noexcept {
        for (size_t nib = 0; nib < 16u; ++nib) {
            mask_lo[nib] = 0;
            mask_hi[nib] = 0;
        }
        const size_t n = alts.size() < k_max ? alts.size() : k_max;
        for (size_t i = 0; i < n; ++i) {
            const uint16_t bit = static_cast<uint16_t>(1u << i);
            mask_lo[static_cast<unsigned char>(alts[i][0]) & 0xFu] |= bit;
            mask_hi[static_cast<unsigned char>(alts[i][1]) & 0xFu] |= bit;
        }
        for (size_t nib = 0; nib < 16u; ++nib) {
            pshuf[0][nib] = static_cast<uint8_t>(mask_lo[nib] & 0xFFu);
            pshuf[1][nib] = static_cast<uint8_t>(mask_lo[nib] >> 8);
            pshuf[2][nib] = static_cast<uint8_t>(mask_hi[nib] & 0xFFu);
            pshuf[3][nib] = static_cast<uint8_t>(mask_hi[nib] >> 8);
        }
    }
};

inline unsigned any_of_ctz16(uint16_t v) noexcept {
    unsigned n = 0;
    while ((v & 1u) == 0u) {
        v = static_cast<uint16_t>(v >> 1);
        ++n;
    }
    return n;
}

// Whole-alt compare at a candidate start: exact memcmp, or the same folded
// byte compare literal_in_line applies under ignore_case.
bool any_of_alt_at(const char *hay, kimix::string_view alt, bool fold_case) noexcept {
    if (!fold_case) {
        return std::memcmp(hay, alt.data(), alt.size()) == 0;
    }
    for (size_t k = 0; k < alt.size(); ++k) {
        if (lower_ascii(hay[k]) != lower_ascii(alt[k])) {
            return false;
        }
    }
    return true;
}

// Verify every alt whose fingerprint covers the two bytes at `pos`.
bool any_of_verify_at(const char *d, size_t n, size_t pos,
                      const kimix::vector<kimix::string> &alts, const any_of_scanner &sc,
                      bool fold_case) noexcept {
    const unsigned b0 = static_cast<unsigned char>(d[pos]) & 0xFu;
    const unsigned b1 = static_cast<unsigned char>(d[pos + 1]) & 0xFu;
    uint32_t bits = static_cast<uint32_t>(sc.mask_lo[b0] & sc.mask_hi[b1]);
    while (bits != 0u) {
        const unsigned b = any_of_ctz16(static_cast<uint16_t>(bits));
        bits &= bits - 1u;
        const kimix::string &alt = alts[b];
        if (pos + alt.size() <= n && any_of_alt_at(d + pos, alt, fold_case)) {
            return true;
        }
    }
    return false;
}

#if KIMIX_GREP_ENGINE_X64_SIMD

// Runtime SSSE3 probe (cached). Teddy's block step needs pshufb (SSSE3, not
// plain SSE2); CPUs without it take the scalar nibble path below.
bool any_of_have_ssse3() noexcept {
#if defined(_MSC_VER)
    static const bool have = []() noexcept {
        int info[4] = {0, 0, 0, 0};
        __cpuid(info, 1);
        return (info[2] & (1 << 9)) != 0; // ECX bit 9 = SSSE3
    }();
    return have;
#else
    static const bool have = __builtin_cpu_supports("ssse3") != 0;
    return have;
#endif
}

#if defined(__GNUC__)
__attribute__((target("ssse3")))
#endif
// One 16-byte Teddy step: returns a 16-bit map of candidate start positions
// in the block at `p` (byte j of the block is set when the fingerprint of the
// byte pair starting at j can begin one of the alts).
uint32_t
any_of_teddy16(const char *p, const any_of_scanner &sc) noexcept {
    const __m128i low_nib = _mm_set1_epi8(0x0F);
    const __m128i a =
        _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p)), low_nib);
    const __m128i b = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 1)), low_nib);
    const __m128i lo_a = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(sc.pshuf[0])), a);
    const __m128i hi_a = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(sc.pshuf[1])), a);
    const __m128i lo_b = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(sc.pshuf[2])), b);
    const __m128i hi_b = _mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(sc.pshuf[3])), b);
    const __m128i res =
        _mm_or_si128(_mm_and_si128(lo_a, lo_b), _mm_and_si128(hi_a, hi_b));
    const __m128i zero =
        _mm_cmpeq_epi8(res, _mm_setzero_si128()); // 0xFF where NO candidate
    return static_cast<uint32_t>(_mm_movemask_epi8(zero)) ^ 0xFFFFu;
}

#endif // KIMIX_GREP_ENGINE_X64_SIMD

// Single-pass "is ANY alt present in hay". Replaces the per-branch
// literal_in_line loop of the alternation fast path in both the per-line
// gate and the whole-buffer early-out; the answer is identical because the
// fingerprint candidate set is a superset of all real occurrences and every
// candidate is verified with literal_in_line's own compare.
bool any_of_scan(kimix::string_view hay, const kimix::vector<kimix::string> &alts,
                 const any_of_scanner &sc, bool fold_case) noexcept {
    const size_t n = hay.size();
    if (n < 2u) {
        return false; // every alt is at least 2 bytes (extractor's contract)
    }
    const char *d = hay.data();
    size_t i = 0;
#if KIMIX_GREP_ENGINE_X64_SIMD
    if (any_of_have_ssse3()) {
        // 16 candidate bytes + 1 look-ahead byte per step.
        for (; i + 17u <= n; i += 16u) {
            uint32_t cand = any_of_teddy16(d + i, sc);
            while (cand != 0u) {
                const unsigned j = any_of_ctz16(static_cast<uint16_t>(cand));
                cand &= cand - 1u;
                if (any_of_verify_at(d, n, i + j, alts, sc, fold_case)) {
                    return true;
                }
            }
        }
    }
#endif
    for (; i + 2u <= n; ++i) {
        if (any_of_verify_at(d, n, i, alts, sc, fold_case)) {
            return true;
        }
    }
    return false;
}

// Top-level pure-literal alternation fast path: when `pat` is A|B|C... and every
// branch is a pure literal, "the line matches" IS "the line contains one of
// these needles", so the regex engine is skipped entirely (the same trick as the
// single-literal use_literal path, with several needles). Refused when: any
// branch is empty (an empty alternative matches every line - keep the engine),
// any branch is a single byte (a 1-byte any-of prefilter is barely cheaper than
// the engine), any branch carries a '\n' (per-line matching can never reach it,
// same rule as use_literal), any branch is not pure ASCII (see may_hide_ascii_cp
// - the byte test must be exactly equivalent to the code-point engine), or a
// '(' / ')' shows up anywhere outside a class (nested alternation is unsound).
bool extract_literal_alternation(kimix::string_view pat,
                                kimix::vector<kimix::string> &lits) noexcept {
    lits.clear();
    auto take_branch = [&](kimix::string_view branch) noexcept {
        kimix::string lit;
        if (!extract_literal(branch, lit)) {
            return false; // metacharacters, unknown escapes, dangling backslash
        }
        if (lit.size() < 2u || lit.find('\n') != kimix::string::npos) {
            return false;
        }
        for (const char c : lit) {
            if (static_cast<unsigned char>(c) >= 0x80u) {
                return false;
            }
        }
        lits.push_back(std::move(lit));
        return lits.size() <= k_max_alt_literals;
    };
    size_t i = 0;
    size_t start = 0;
    for (;;) {
        while (i < pat.size()) {
            const char c = pat[i];
            if (c == '\\') {
                i += 2; // an escaped char is never a separator
                continue;
            }
            if (c == '[') {
                kimix::string tmp;
                if (read_atom(pat, i, tmp) != atom_kind::opaque) {
                    lits.clear();
                    return false;
                }
                continue; // '|' inside a class is class content, not a separator
            }
            if (c == '(' || c == ')') {
                lits.clear();
                return false;
            }
            if (c == '|') {
                break;
            }
            ++i;
        }
        // A dangling backslash walks `i` past the end: clamp it, so the branch
        // view never reaches outside the pattern (extract_literal then rejects
        // the trailing backslash and the whole fast path is refused).
        if (i > pat.size()) {
            i = pat.size();
        }
        if (!take_branch(kimix::string_view(pat.data() + start, i - start))) {
            lits.clear();
            return false;
        }
        if (i >= pat.size()) {
            break;
        }
        ++i; // step over the '|'
        start = i;
    }
    return lits.size() >= 2u && lits.size() <= k_max_alt_literals;
}

// -- per-file match plan -----------------------------------------------------

// How scan_file answers "does this line match", decided ONCE by run_grep on the
// calling thread (extraction is pure, so every chunk may read it) and then used
// read-only by all workers.
struct scan_plan {
    kimix::string literal;                // the pattern's own bytes (use_literal)
    bool use_literal = false;             // pure literal: substring search, no engine
    kimix::vector<kimix::string> alts;    // top-level pure-literal alternation
    bool use_alts = false;                // ... : any-of substring search, no engine
    any_of_scanner alt_scan;              // ... : nibble-fingerprint tables for the single-pass any-of scan
    kimix::string prefilter;              // longest required run of a regex pattern
    bool has_prefilter = false;           // ... : gates the engine per line / buffer
};

// Per-worker regex engine, compiled ON DEMAND.  The pure-literal plan
// normally never touches the engine at all, but its byte-level answer is
// provably equal to the code-point engine ONLY on lines that cannot hide an
// ASCII code point behind an overlong UTF-8 spelling (may_hide_ascii_cp -
// e.g. bytes C1 81 decode to 'A' under regex_lite's lenient decoder, so the
// line "\xC1\x81pple" DOES match the literal pattern "App").  The first
// such line compiles the engine then; the common all-ASCII corpus never pays
// the compile.  Plans that are not pure-literal keep their upfront compile
// (validated once by run_grep, so compile() cannot fail there).
struct lazy_regex {
    regex_lite::Regex re;
    kimix::string_view pattern;
    bool fold_case = false;
    bool compiled = false;

    bool compile() noexcept {
        if (!compiled) {
            kimix::string err;
            compiled = re.compile(pattern, fold_case, err);
        }
        return compiled;
    }
};

// Line-level answer of the plan. The engine is consulted for every line the
// prefilter cannot already decide, which is exactly the prefilter's contract:
// a byte needle that is ABSENT from a line that cannot spell it overlong means
// the regex cannot match that line.  Every byte-level fast path (literal,
// alternation, required-literal prefilter) has the same escape hatch: when the
// line could hide an ASCII code point overlong, the code-point engine decides.
bool line_matches(kimix::string_view line, const scan_plan &plan, lazy_regex &lz) noexcept {
    if (plan.use_literal) {
        if (literal_in_line(line, plan.literal, lz.fold_case)) {
            return true;
        }
        if (!may_hide_ascii_cp(line) || !lz.compile()) {
            return false; // no bytewise hit and none can hide overlong
        }
    } else if (plan.use_alts) {
        if (any_of_scan(line, plan.alts, plan.alt_scan, lz.fold_case)) {
            return true;
        }
        if (!may_hide_ascii_cp(line) || !lz.compile()) {
            return false; // no branch present bytewise and none can hide overlong
        }
    } else if (plan.has_prefilter && !literal_in_line(line, plan.prefilter, lz.fold_case) &&
               !may_hide_ascii_cp(line)) {
        return false; // the required run is absent: the engine could not match
    }
    size_t mb = 0;
    size_t me = 0;
    return lz.re.search(line, mb, me);
}

// Whole-buffer early-out: when nothing the plan requires occurs anywhere in the
// read buffer, no line of the file can match, so the line enumeration (and the
// engine) never runs. Sound because every line view is a contiguous subrange of
// the buffer. The pure-literal path is deliberately NOT gated here: for it the
// per-line scan already IS the prefilter, and a folded all-letter needle would
// make this an extra full pass over bytes that get scanned again below.
bool buffer_cannot_match(kimix::string_view text, const scan_plan &plan,
                         bool fold_case) noexcept {
    if (plan.use_alts) {
        if (any_of_scan(text, plan.alts, plan.alt_scan, fold_case)) {
            return false;
        }
    } else if (plan.has_prefilter) {
        if (literal_in_line(text, plan.prefilter, fold_case)) {
            return false;
        }
    } else {
        return false;
    }
    return !buffer_may_hide_ascii_cp(text);
}

// -- zero-copy line iteration ------------------------------------------------

// Stream every line of the raw byte range [text, text+size) through
// `fn(line_view)` without materializing the views: split at '\n', strip one
// trailing '\r' per line, emit a trailing partial line (no closing '\n'),
// and no extra empty line after a buffer that ends in '\n' - the exact
// enumeration ge::collect_lines produces. Takes a pointer + size (not a
// kimix::string&) so both the memory-mapped view and the fread fallback
// buffer share the one line-splitting implementation.
template <typename Fn>
void for_each_line(const char *text, size_t size, Fn &&fn) {
    size_t start = 0;
    while (start <= size) {
        const char *nl_hit = static_cast<const char *>(
            std::memchr(text + start, '\n', size - start));
        if (nl_hit == nullptr) {
            if (start < size) {
                size_t len = size - start;
                if (text[size - 1] == '\r') {
                    --len;
                }
                fn(line_view{start, len});
            }
            break;
        }
        const size_t nl = static_cast<size_t>(nl_hit - text);
        size_t len = nl - start;
        if (len > 0 && text[nl - 1] == '\r') {
            --len;
        }
        fn(line_view{start, len});
        start = nl + 1u;
    }
}

// Split the byte range into line views (same rules as for_each_line; used by
// the content mode, where context rendering needs random access to the lines).
void collect_lines(const char *text, size_t size, kimix::vector<line_view> &out) {
    out.clear();
    for_each_line(text, size,
                  [&](const line_view &lv) noexcept { out.push_back(lv); });
}

// -- per-file scan -----------------------------------------------------------

// Render the content-mode lines for ONE matched file into its output block:
// for each hit line the
// context run [li-B, li+A] clamped to the file bounds, overlapping runs
// merged, "--" between non-adjacent runs; match line "path:LN:text", context
// line "path-LN:text" (LN is 1-based). Same last_emitted logic as the old
// inline branch.
void render_content(const grep_options &opts, kimix::string_view path,
                    kimix::string_view text, const kimix::vector<line_view> &lines,
                    const kimix::vector<int64_t> &hit_lines, file_block &blk) {
    const int64_t line_count = static_cast<int64_t>(lines.size());
    int64_t last_emitted = -1000;
    for (const int64_t li : hit_lines) {
        const int64_t lo = std::max<int64_t>(0, li - static_cast<int64_t>(opts.ctx_before));
        const int64_t hi = std::min<int64_t>(line_count - 1,
                                             li + static_cast<int64_t>(opts.ctx_after));
        if (lo > last_emitted + 1 && last_emitted > -999) {
            blk.lines.push_back("--");
            blk.line_match.push_back(0);
        }
        for (int64_t l = lo; l <= hi; ++l) {
            if (l <= last_emitted) {
                continue;
            }
            const line_view &lv = lines[static_cast<size_t>(l)];
            const kimix::string_view text_view(text.data() + lv.start, lv.len);
            const char sep = (l == li) ? ':' : '-';
            blk.lines.push_back(
                kimix::format("{}{}{}{}{}", path, sep, l + 1, sep, text_view));
            blk.line_match.push_back(l == li ? 1 : 0);
            last_emitted = l;
        }
        last_emitted = std::max(last_emitted, hi);
    }
}

// The shared scan over an already-read byte range: whole-buffer prefilter
// gate, then the per-mode line pass over the read buffer.
void scan_text(const walk_entry &entry, uint32_t file_index, const grep_options &opts,
               lazy_regex &lz, const scan_plan &plan, const char *text,
               size_t text_size, chunk_output &out) {
    const kimix::string &path = entry.display;
    // Whole-buffer prefilter early-out (all modes): no required byte anywhere
    // means no line can match, so neither the line enumeration nor the engine
    // runs at all. Purely an answer-preserving shortcut - see
    // buffer_cannot_match for why the overlong-encoding case still falls through.
    if (buffer_cannot_match(kimix::string_view(text, text_size), plan,
                            opts.ignore_case)) {
        return;
    }

    if (opts.mode == grep_output_mode::content) {
        // Content mode needs random line access for the -B/-A context runs.
        kimix::vector<line_view> lines;
        collect_lines(text, text_size, lines);
        kimix::vector<int64_t> hit_lines;
        for (size_t li = 0; li < lines.size(); ++li) {
            const line_view &lv = lines[li];
            const kimix::string_view line(text + lv.start, lv.len);
            if (line_matches(line, plan, lz)) {
                hit_lines.push_back(static_cast<int64_t>(li));
            }
        }
        if (hit_lines.empty()) {
            return;
        }
        const int64_t file_matches = static_cast<int64_t>(hit_lines.size());
        out.total_matches += file_matches;
        file_block blk;
        blk.index = file_index;
        blk.res = grep_file_result{path, file_matches};
        render_content(opts, path, kimix::string_view(text, text_size), lines,
                       hit_lines, blk);
        out.blocks.push_back(std::move(blk)); // vectors move, the lines are not re-copied
        return;
    }
    // files_with_matches / count_matches: stream the lines, never materialize
    // the views (the per-line scan result is identical).
    int64_t file_matches = 0;
    for_each_line(text, text_size, [&](const line_view &lv) noexcept {
        const kimix::string_view line(text + lv.start, lv.len);
        if (line_matches(line, plan, lz)) {
            ++file_matches;
        }
    });
    if (file_matches == 0) {
        return;
    }
    out.total_matches += file_matches;
    file_block blk;
    blk.index = file_index;
    blk.res = grep_file_result{path, file_matches};
    if (opts.mode == grep_output_mode::count_matches) {
        blk.lines.push_back(kimix::format("{}:{}", path, file_matches));
        blk.line_match.push_back(1);
    }
      // files_with_matches renders nothing here: the merge step derives the
      // (head_limit-capped) lines from the complete files[] array.
      out.blocks.push_back(std::move(blk));
  }

  // Search one already-collected file (index = its position in the walk-order
  // list, stamped on the block so the walk-order merge can find it again).
  // Silently skipped on: stat size (or the growing read buffer) > 4 MiB,
  // open/read errors, or a NUL in the first 64 KiB of the bytes (rg binary
  // convention). The include-glob and the regular-file checks already ran
  // during the walk (collect_files), so this only sees candidate regular
  // files whose name matches. `plan` is the read-only match plan run_grep
  // built before the fan-out (literal fast path / multi-literal alternation /
  // required-literal prefilter for the regex path); `lz` is only consulted
  // where the plan cannot answer on its own. A block is appended to `out`
  // ONLY when the file matched - a no-match file contributes nothing, exactly
  // as before.
  void scan_file(const walk_entry &entry, uint32_t file_index, const grep_options &opts,
                 lazy_regex &lz, const scan_plan &plan, chunk_output &out) {
      const kimix::string &path = entry.display;
      const kimix::filesystem::path &file = entry.native;
      std::error_code ec;
      // Re-stat, never trust the walk's balancing hint: a file may have grown,
      // shrunk or changed type since the walk. A failed stat returns (uintmax)-1,
      // which exceeds the cap: the file is skipped, exactly like the previous
      // scan-side is_regular/file_size pair.
      const uintmax_t stat_size = kimix::filesystem::file_size(file, ec);
      if (stat_size > k_max_file_bytes) {
          return;
      }
      // fopen takes the DISPLAY string directly - the old chain was
      // fopen(to_string(path_from_utf8/narrow(display))) and kimix::to_string
      // round-trips its own output (ACP-first, UTF-8 fallback - see
      // stl/filesystem.cpp), so the byte string handed to fopen is identical
      // without the per-file path re-decode + re-encode.
      std::FILE *f = std::fopen(path.c_str(), "rb");
      if (f == nullptr) {
          return;
      }
      // Whole-buffer read with one reserve from the stat size (the append loop
      // below no longer grows the string geometrically for multi-KiB files).
      kimix::string text;
      if (stat_size <= k_max_file_bytes) {
          text.reserve(static_cast<size_t>(stat_size));
      }
      char buf[65536];
      // Binary sniff on the FIRST 64 KiB of the read buffer (the rg convention
      // the old whole-file read implemented): collect at least 64 KiB (or EOF),
      // sniff, and only then tail-read the rest. Identical sniff set, but a
      // binary blob is skipped after one 64 KiB read instead of up to 4 MiB.
      bool sniffed = false;
      size_t n = 0;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
          text.append(buf, n);
          if (text.size() > k_max_file_bytes) {
              std::fclose(f);
              return;
          }
          if (!sniffed && text.size() >= k_binary_sniff_bytes) {
              if (std::memchr(text.data(), '\0', k_binary_sniff_bytes) != nullptr) {
                  std::fclose(f);
                  return;
              }
              sniffed = true;
          }
      }
      std::fclose(f);
      if (!sniffed) {
          // The file was shorter than 64 KiB: sniff the whole buffer. A NUL past
          // 64 KiB does NOT skip (same rule as before).
          if (std::memchr(text.data(), '\0', text.size()) != nullptr) {
              return;
          }
      }
      scan_text(entry, file_index, opts, lz, plan, text.data(), text.size(), out);
  }

// -- single-threaded walk ----------------------------------------------------

// Append every non-hidden REGULAR file entry under `root_str` to `out`, in
// walk order, already filtered through the include-glob when one is set.
// Relative roots resolve against `work_dir`. Hidden entries (name starting
// with '.') are skipped at every depth and hidden directories are not
// descended. Directories and other non-regular entries never enter the list
// (the old scan-side is_regular_file check skipped them after a full path
// re-decode and two stats; the directory_iterator's cached entry type answers
// the same question without the extra syscalls, following symlinks like
// std::filesystem::is_regular_file did). Only error_code overloads are used;
// any iterator error ends the walk for that root.
void collect_files(const kimix::string &root_str, kimix::string_view work_dir,
                   const kimix::string &include_glob, bool want_size,
                   kimix::vector<walk_entry> &out) {
    namespace fs = kimix::filesystem;
    // No narrow path constructor: it converts through the ANSI code page and
    // THROWS std::system_error on bytes it cannot represent (fatal with C++
    // exceptions disabled). Tool arguments arrive UTF-8, work_dir follows the
    // CLI's ANSI/lossy convention; a conversion failure reports "missing".
    fs::path rp;
    if (!kimix::path_from_utf8(root_str, rp)) {
        if (!kimix::path_from_narrow(root_str, rp)) {
            return;
        }
    }
    if (rp.is_relative() && !work_dir.empty()) {
        fs::path wd;
        if (kimix::path_from_narrow(work_dir, wd) ||
            kimix::path_from_utf8(work_dir, wd)) {
            rp = wd / rp;
        }
    }
    std::error_code ec;
    if (!fs::exists(rp, ec)) {
        return;
    }
    if (fs::is_regular_file(rp, ec)) {
        // The include-glob selects by file name for file roots too (the old
        // scan-side check silently skipped a mismatched direct root).
        if (!include_glob.empty()) {
            const kimix::string name = kimix::to_string(rp.filename());
            if (!fnmatch_ascii(name, include_glob, false)) {
                return;
            }
        }
        walk_entry entry;
        entry.display = kimix::to_string(rp);
        entry.native = std::move(rp);
        if (want_size) {
            // Direct-file root: one stat for the balancing hint (0 when it
            // fails, which also means scan_file's own stat will fail and skip
            // the file).
            entry.size_hint = fs::file_size(entry.native, ec);
            if (ec) {
                entry.size_hint = 0;
                ec.clear(); // never let a hint query confuse the walk below
            }
        }
        out.push_back(std::move(entry));
        return;
    }
      // Canonical cross-platform walk order: recursive_directory_iterator
      // follows the raw OS enumeration order (NTFS ~alphabetical, POSIX
      // creation/hash order), which made the walk-order merge - and the
      // determinism the tests pin - OS-dependent. Enumerate each directory,
      // sort its entries by path, then descend in that order, so every
      // platform walks byte-identical order (the NTFS order the suites were
      // written against: a subdirectory's contents yield at its sorted
      // position, before later siblings).
      auto walk = [&](auto &&self, const fs::path &dir) -> void {
          kimix::vector<fs::path> entries;
          fs::directory_iterator dit(dir, fs::directory_options::none, ec);
          if (ec) {
              ec.clear();
              return;
          }
          const fs::directory_iterator d_end;
          for (; dit != d_end; dit.increment(ec)) {
              if (ec) {
                  ec.clear();
                  return;
              }
              entries.push_back(dit->path());
          }
          std::sort(entries.begin(), entries.end());
          for (const fs::path &entry_path : entries) {
              // Skip hidden dirs (.git etc.) and hidden files at every depth.
              const kimix::string fname = kimix::to_string(entry_path.filename());
              if (!fname.empty() && fname[0] == '.' && fname != "." && fname != "..") {
                  continue;
              }
              std::error_code t_ec;
              if (fs::is_directory(entry_path, t_ec)) {
                  self(self, entry_path);
                  continue;
              }
              // Only regular files are searchable.
              if (!fs::is_regular_file(entry_path, t_ec)) {
                  continue;
              }
              if (!include_glob.empty() && !fnmatch_ascii(fname, include_glob, false)) {
                  continue;
              }
              walk_entry entry;
              entry.display = kimix::to_string(entry_path);
              entry.native = entry_path;
              if (want_size) {
                  // Size hint for the LPT balance (see walk_entry): the
                  // error_code overload, so an unreadable entry costs nothing
                  // but a zero hint.
                  entry.size_hint = fs::file_size(entry_path, t_ec);
                  if (t_ec) {
                      entry.size_hint = 0;
                      t_ec.clear();
                  }
              }
              out.push_back(std::move(entry));
          }
      };
      walk(walk, rp);
  }

// -- dynamic work distribution -----------------------------------------------

// One file as the work-order sort sees it: its walk-order index and the size
// the walk hinted for it.
struct sized_index {
    uintmax_t size = 0;
    uint32_t index = 0;
};

// The ONE shared claim order for the whole file list: size hints DESCENDING,
// walk index ascending as the tiebreak, so the order itself is deterministic.
// This is the LPT greedy order turned dynamic. The old static assignment
// (lpt_assign) pinned every file to a chunk before the scan, so a chunk stuck
// with a slow/huge/match-dense file blocked its worker while the neighbours
// idled - and the size hints that drive the balance do not even predict the
// real per-file cost (content mode is render-bound, not size-bound; the
// prefilter settles a 4 MiB file faster than an unmatched 8 KiB one). Here no
// file belongs to anyone: every worker loops `order[claim.fetch_add(1)]` until
// the queue drains, so a worker that gets a cheap file automatically takes the
// next-largest remaining one. The LPT property (largest job claimed first)
// holds at every claim instant, and a wrong or zero hint degrades gracefully -
// the order stays a pure heuristic, never a correctness input (scan_file
// re-stats and re-caps every file itself).
//
// Complexity O(n log n) once per call, single-threaded, before the fan-out.
void claim_order(const kimix::vector<walk_entry> &files,
                 kimix::vector<uint32_t> &order) {
    const size_t n = files.size();
    order.resize(n);
    kimix::vector<sized_index> sized(n);
    for (size_t i = 0; i < n; ++i) {
        sized[i].size = files[i].size_hint;
        sized[i].index = static_cast<uint32_t>(i);
    }
    std::sort(sized.begin(), sized.end(),
              [](const sized_index &a, const sized_index &b) noexcept {
                  if (a.size != b.size) {
                      return a.size > b.size; // longest job first
                  }
                  return a.index < b.index; // ... deterministically
              });
    for (size_t i = 0; i < n; ++i) {
        order[i] = sized[i].index;
    }
}

} // namespace ge

} // namespace

tool_status run_grep(const grep_options &opts, kimix::span<const kimix::string> roots,
                     kimix::string_view work_dir, grep_result &out) {
    out = grep_result{};
    // Match plan, built ONCE here (single-threaded, next to the compile
    // validation below) and read by every chunk worker afterwards:
    //   1. pure literal            -> substring search, no engine at all;
    //   2. literal A|B|C alternation -> any-of substring search, no engine;
    //   3. anything else           -> the engine, but gated per line (and per
    //      buffer) by the longest run of bytes every match must contain.
    // A literal containing '\n' must NOT take (1) - per-line matching can never
    // span lines, exactly like the regex scan.
    ge::scan_plan plan;
    plan.use_literal = ge::extract_literal(opts.pattern, plan.literal);
    if (plan.use_literal && plan.literal.find('\n') != kimix::string::npos) {
        plan.use_literal = false;
    }
    // Compile validation BEFORE the walk (single-threaded), like the old
    // inline branch: an invalid pattern is a parameter error, not a search
    // result. Literal patterns compile by construction and skip this.
    if (!plan.use_literal) {
        regex_lite::Regex re;
        kimix::string re_error;
        if (!re.compile(opts.pattern, opts.ignore_case, re_error)) {
            out.status = tool_status::invalid_input;
            out.message = "invalid pattern: " + re_error;
            return out.status;
        }
        // (2) then (3): only for patterns that really go the engine's way, and
        // (2) wins because it never touches the engine at all.
        if (ge::extract_literal_alternation(opts.pattern, plan.alts)) {
            plan.use_alts = true;
            plan.alt_scan.build(plan.alts); // once, on the calling thread
        } else {
            plan.has_prefilter = ge::extract_required_literal(opts.pattern, plan.prefilter);
        }
    }
    // Fan-out threshold: dispatching the chunk jobs costs more than it saves
    // on very small file lists (a micro-benchmark of a 3-file tree runs
    // ~2x faster through the single inline chunk than through fiber::parallel
    // over the ambient pool). Only spread from >= k_min_fanout files. Known
    // before the walk: it also decides whether the walk collects size hints
    // and whether an unbound caller will bind the shared pool.
    constexpr size_t k_min_fanout = 8u;
    const bool fanout_possible =
        kimix::fiber::is_bound() && kimix::fiber::worker_thread_count() > 1u;
    kimix::vector<ge::walk_entry> files;
    for (const kimix::string &root : roots) {
        ge::collect_files(root, work_dir, opts.include_glob, fanout_possible,
                          files);
    }
    const size_t n_files = files.size();
    // Threading: the engine never creates a scheduler, and it only touches
    // fiber when the list is big enough to fan out - a small scan runs inline
    // on the calling thread, bound or not, with no bind cost at all. When
    // fan-out IS required the chunks must spread over kimix::fiber: a bound
    // caller keeps its AMBIENT pool, an unbound caller transiently binds the
    // process-wide shared pool for the rest of the call and gets the same
    // fan-out a host thread gets. The guard outlives the scan, so the shared
    // pool is unbound only once every chunk job has drained (parallel()
    // blocks until then anyway).
    const ge::fiber_bind_guard fiber_binding(n_files >= k_min_fanout);
    if (n_files >= k_min_fanout && !fiber_binding.bound()) {
        // Defensive: with marl the shared pool is created on first use and
        // bind() cannot fail, so this branch is unreachable today - it is the
        // contract's escape hatch for a host without fiber support. Report it
        // (run_grep never throws) instead of silently degrading to a serial
        // scan that would not scale on a foreign thread.
        out.status = tool_status::unsupported;
        out.message = "grep engine requires kimix::fiber support";
        return out.status;
    }
    const uint32_t pool_workers =
        fiber_binding.bound() ? kimix::fiber::worker_thread_count() : 1u;
    size_t num_workers = 1;
    if (pool_workers > 1u && n_files >= k_min_fanout) {
        num_workers = std::min<size_t>(pool_workers, n_files);
    }
    if (num_workers > 1 && !fanout_possible) {
        // The walk skipped the size query because the caller was unbound (the
        // list size - and thus whether fan-out would happen - was unknowable
        // then); pay the deferred stat pass now. The hints it buys order the
        // shared claim queue largest-first, which is what keeps a huge file
        // from starting last and deciding the wall time.
        std::error_code ec;
        for (ge::walk_entry &e : files) {
            e.size_hint = kimix::filesystem::file_size(e.native, ec);
            if (ec) {
                e.size_hint = 0;
                ec.clear();
            }
        }
    }
    kimix::vector<ge::chunk_output> worker_out(num_workers);
    // Shared claim queue over the single descending-size order list: one
    // atomic per file, no static pinning (see ge::claim_order).
    kimix::vector<uint32_t> order;
    ge::claim_order(files, order);
    std::atomic<uint32_t> claim{0};
    // One job per pool worker; each carries its OWN lazily-compiled
    // regex_lite engine (Regex is not thread-safe, see ge::lazy_regex) and
    // appends blocks to its own output. Every plan that is not the pure-
    // literal path compiles upfront, since the alternation any-of test and
    // the prefilter gate fall back to the engine for the lines that could
    // hide an ASCII code point overlong; the pure-literal path compiles on
    // the first such line instead, so an all-ASCII corpus never pays it.
    auto worker = [&](size_t wi) noexcept {
        ge::lazy_regex lz;
        lz.pattern = opts.pattern;
        lz.fold_case = opts.ignore_case;
        if (!plan.use_literal) {
            kimix::string err;
            lz.compiled = lz.re.compile(opts.pattern, opts.ignore_case, err);
            if (!lz.compiled) {
                return; // validated upfront; cannot fail
            }
        }
        ge::chunk_output &mine = worker_out[wi];
        for (;;) {
            const uint32_t oi = claim.fetch_add(1u, std::memory_order_relaxed);
            if (oi >= order.size()) {
                break; // queue drained
            }
            const uint32_t idx = order[oi];
            ge::scan_file(files[idx], idx, opts, lz, plan, mine);
        }
    };
     if (num_workers <= 1) {
     worker(0);
     } else {
     // The jobs run on the CALLING thread's pool: its own ambient pool, or
     // the process-wide shared pool the guard bound for this call
     // (num_workers <= worker_thread_count() of it either way). Because the
     // queue is shared there is no per-chunk makespan to get wrong: the
     // scan ends when the last file is claimed, whichever worker held the
     // slow ones. Only the CLAIM TIMING varies with scheduling - never the
     // answers (see the index sort in the merge below).
     kimix::fiber::parallel(
         static_cast<uint32_t>(num_workers),
         [&](uint32_t wi) noexcept { worker(wi); },
         /*internal_jobs=*/1u);
     }

    // Walk-order merge. A worker's blocks come out in CLAIM order (size-
    // sorted), not walk order, so every block is gathered and sorted once by
    // the file's walk-order index; the result is then emitted strictly index-
    // ascending - exactly the set of matched files in walk order, independent
    // of which worker scanned what (indices are unique, so the sort is fully
    // deterministic). Totals are order-independent, summed per worker first.
    // Block storage is dead after its turn, so the rendered lines and file
    // records MOVE into the result (no second round of string allocations).
    for (size_t wi = 0; wi < num_workers; ++wi) {
        out.total_matches += worker_out[wi].total_matches;
    }
    size_t n_blocks = 0;
    for (size_t wi = 0; wi < num_workers; ++wi) {
        n_blocks += worker_out[wi].blocks.size();
    }
    kimix::vector<ge::file_block> merged;
    merged.reserve(n_blocks);
    for (size_t wi = 0; wi < num_workers; ++wi) {
        for (ge::file_block &blk : worker_out[wi].blocks) {
            merged.push_back(std::move(blk));
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const ge::file_block &a, const ge::file_block &b) noexcept {
                  return a.index < b.index;
              });
    for (ge::file_block &blk : merged) {
        if (opts.mode == grep_output_mode::files_with_matches) {
            // fwm lines are the paths, capped as the merge grows (the old
            // branch's behaviour, and the old chunk-contiguous cap's exact
            // semantics: the count is over rendered lines, in output order);
            // the files[] array stays complete.
            if (opts.head_limit <= 0 ||
                static_cast<int64_t>(out.lines.size()) < opts.head_limit) {
                out.lines.push_back(blk.res.path);
                out.line_match.push_back(1);
            }
        }
        out.files.push_back(std::move(blk.res));
        if (opts.mode != grep_output_mode::files_with_matches) {
            out.lines.insert(out.lines.end(),
                             std::make_move_iterator(blk.lines.begin()),
                             std::make_move_iterator(blk.lines.end()));
            out.line_match.insert(out.line_match.end(), blk.line_match.begin(),
                                  blk.line_match.end());
        }
    }
    out.message = kimix::format("{} match(es) in {} file(s)", out.total_matches,
                                out.files.size());
    return out.status;
}

} // namespace kimix::builtin_tools::grep

#undef KIMIX_GREP_ENGINE_X64_SIMD // unity-batch hygiene: do not leak the arch macro
