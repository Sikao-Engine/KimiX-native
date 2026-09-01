/*
 * find_str.cpp - Case-insensitive substring search per file line (plan 013).
 *
 * Exact port of find_str.py::find_in_file semantics; BMH search with on-the-
 * fly ASCII folding (see the header for the folding note and the empty-needle
 * contract).
 */

#include <runtime/tools/find_str.h>

namespace kimix {
namespace runtime {
namespace tools {

namespace {

inline uint8_t fold_ascii(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

} // namespace

void find_in_file(kimix::string_view content, kimix::string_view needle,
                  bool case_insensitive, kimix::vector<find_match>& out) {
    out.clear();
    const size_t n = content.size();

    // For a non-empty needle a line never yields more overlapping matches
    // than its byte length, so content.size() slots is an exact upper bound
    // that removes most geometric-growth reallocations on match-dense inputs.
    // The reservation is capped so sparse searches over very large files do
    // not balloon memory. (The empty-needle case needs n + #lines, which the
    // cap still under-covers by design; growth there is amortized.)
    if (n > 0) {
        out.reserve(out.size() + std::min<size_t>(n, (size_t)1 << 20));
    }

    // Fold the needle once.
    kimix::vector<uint8_t> nf;
    nf.reserve(needle.size());
    for (unsigned char c : needle) {
        nf.push_back(case_insensitive ? fold_ascii(c) : c);
    }
    const size_t m = nf.size();

    // BMH bad-character table over the folded needle (last occurrence wins;
    // the final character is excluded so a match at the end shifts by 1).
    uint32_t badchar[256];
    if (m > 0) {
        for (uint32_t& b : badchar) {
            b = static_cast<uint32_t>(m);
        }
        for (size_t i = 0; i + 1 < m; ++i) {
            badchar[nf[i]] = static_cast<uint32_t>(m - 1 - i);
        }
    }

    // readlines() semantics: lines keep their terminator; "" yields no lines.
    size_t line_start = 0;
    uint32_t line_index = 0;
    while (line_start < n) {
        const size_t nl = content.find('\n', line_start);
        const size_t line_end = (nl == kimix::string_view::npos) ? n : nl + 1;
        const size_t line_len = line_end - line_start;
        const char* const base = content.data() + line_start;

        if (m == 0) {
            // Reference find("") returns start while start <= len(line):
            // matches at every column 0..line_len inclusive.
            for (size_t c = 0; c <= line_len; ++c) {
                out.push_back(find_match{line_index, static_cast<uint32_t>(c), 0});
            }
        } else {
            size_t start = 0;
            while (start + m <= line_len) {
                // BMH scan for the next occurrence from `start`.
                size_t pos = start;
                size_t shift = 1;
                bool found = false;
                while (pos + m <= line_len) {
                    // compare from the end
                    size_t k = m;
                    while (k > 0) {
                        const uint8_t lc =
                            case_insensitive
                                ? fold_ascii(static_cast<uint8_t>(base[pos + k - 1]))
                                : static_cast<uint8_t>(base[pos + k - 1]);
                        if (lc != nf[k - 1]) {
                            break;
                        }
                        --k;
                    }
                    if (k == 0) {
                        found = true;
                        break;
                    }
                    // shift by the bad-char rule on the char at pos + m - 1
                    const uint8_t bc =
                        case_insensitive
                            ? fold_ascii(static_cast<uint8_t>(base[pos + m - 1]))
                            : static_cast<uint8_t>(base[pos + m - 1]);
                    shift = badchar[bc];
                    pos += (shift == 0) ? 1 : shift;
                }
                if (!found) {
                    break;
                }
                out.push_back(find_match{line_index, static_cast<uint32_t>(pos),
                                        static_cast<uint32_t>(m)});
                // overlapping: resume at pos + 1 (reference start = idx + 1)
                start = pos + 1;
            }
        }

        line_start = line_end;
        ++line_index;
    }
}

} // namespace tools
} // namespace runtime
} // namespace kimix
