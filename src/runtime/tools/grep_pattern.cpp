/*
 * grep_pattern.cpp - Newline-aware grep pattern kernels implementation.
 *
 * Exact port of kimi-cli grep_local.py _pattern_has_regex_newline and
 * _multiline_pattern (commit 0582e09).  The odd-backslash ``\n`` escape test
 * replicates re.search(r"(?<!\\)(?:\\\\)*\\n", pattern): at each 'n' the
 * run of immediately-preceding backslashes is maximal, so the match exists
 * iff that run has odd length (the negative lookbehind is then automatic).
 *
 * _multiline_pattern:
 *   1. p = pattern.replace("\r\n", "\n")
 *   2. p = regex-sub every odd-backslash \n escape with the literal 5-char
 *      r"\r?\n" (backslash r ? backslash n) -- non-overlapping, resume after
 *      each match end
 *   3. p = p.replace("\n", r"\r?\n") for real newlines
 * The inserted ``\r?\n`` contains no real newline, so step 3 is order-safe.
 */

#include <runtime/tools/grep_pattern.h>

namespace kimix {
namespace runtime {
namespace tools {

namespace {

// True when the pattern contains a regex ``\n`` escape: a run of an ODD
// number of backslashes immediately followed by 'n'.
bool has_regex_newline_escape(kimix::string_view p) noexcept {
    size_t i = 0;
    const size_t n = p.size();
    while (i < n) {
        if (p[i] == '\\') {
            size_t j = i;
            while (j < n && p[j] == '\\') {
                ++j;
            }
            const size_t run = j - i;
            if (j < n && p[j] == 'n' && (run % 2) == 1) {
                return true;
            }
            i = j; // skip the run (the 'n' cannot start a match itself)
        } else {
            ++i;
        }
    }
    return false;
}

// Find the next odd-backslash \n escape at/after `from`; returns (start, end)
// or false.  Matches are non-overlapping; scanning resumes after match end.
bool next_regex_newline(kimix::string_view p, size_t from, size_t& start,
                        size_t& end) noexcept {
    const size_t n = p.size();
    size_t i = from;
    while (i < n) {
        if (p[i] == '\\') {
            size_t j = i;
            while (j < n && p[j] == '\\') {
                ++j;
            }
            const size_t run = j - i;
            if (j < n && p[j] == 'n' && (run % 2) == 1) {
                start = i;
                end = j + 1;
                return true;
            }
            i = j;
        } else {
            ++i;
        }
    }
    return false;
}

} // namespace

bool pattern_has_regex_newline(kimix::string_view pattern) {
    return pattern.find('\n') != kimix::string_view::npos ||
           has_regex_newline_escape(pattern);
}

kimix::string multiline_pattern(kimix::string_view pattern) {
    if (pattern.find('\n') == kimix::string_view::npos &&
        !has_regex_newline_escape(pattern)) {
        return kimix::string(pattern);
    }
    kimix::string p;
    p.reserve(pattern.size() + 16);

    // 1. p = pattern.replace("\r\n", "\n")
    {
        size_t i = 0;
        while (i < pattern.size()) {
            if (i + 1 < pattern.size() && pattern[i] == '\r' &&
                pattern[i + 1] == '\n') {
                p.push_back('\n');
                i += 2;
            } else {
                p.push_back(pattern[i]);
                ++i;
            }
        }
    }

    // 2. Replace every regex \n escape with the literal 5-char r"\r?\n".
    {
        kimix::string out;
        out.reserve(p.size() + 16);
        size_t pos = 0;
        size_t s = 0, e = 0;
        while (next_regex_newline(p, pos, s, e)) {
            out.append(p.data() + pos, s - pos);
            out.append("\\r?\\n"); // literal backslash r ? backslash n
            pos = e;
        }
        out.append(p.data() + pos, p.size() - pos);
        p = std::move(out);
    }

    // 3. p = p.replace("\n", r"\r?\n") for real newlines.
    {
        kimix::string out;
        out.reserve(p.size() + 16);
        size_t pos = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            if (p[i] == '\n') {
                out.append(p.data() + pos, i - pos);
                out.append("\\r?\\n");
                pos = i + 1;
            }
        }
        out.append(p.data() + pos, p.size() - pos);
        p = std::move(out);
    }
    return p;
}

} // namespace tools
} // namespace runtime
} // namespace kimix
