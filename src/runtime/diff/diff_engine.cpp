/*
 * diff_engine.cpp — Diff kernel implementation.
 */

#include <runtime/diff/diff_engine.h>

#include <runtime/common/utf8.h>

#include <core/kimix_core.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace kimix {
namespace runtime {
namespace diff {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 decoding with surrogatepass semantics.
// ---------------------------------------------------------------------------

kimix::vector<uint32_t> decode_utf8_surrogatepass(kimix::string_view s) {
    kimix::vector<uint32_t> out;
    out.reserve(s.size());
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
            out.push_back(c);
            ++p;
        } else if ((c & 0xe0) == 0xc0) {
            if (p + 1 >= end || (static_cast<unsigned char>(p[1]) & 0xc0) != 0x80) {
                out.push_back(0xfffd);
                ++p;
                continue;
            }
            const uint32_t cp = ((c & 0x1f) << 6) | (static_cast<unsigned char>(p[1]) & 0x3f);
            out.push_back(cp);
            p += 2;
        } else if ((c & 0xf0) == 0xe0) {
            if (p + 2 >= end ||
                (static_cast<unsigned char>(p[1]) & 0xc0) != 0x80 ||
                (static_cast<unsigned char>(p[2]) & 0xc0) != 0x80) {
                out.push_back(0xfffd);
                ++p;
                continue;
            }
            const uint32_t cp = ((c & 0x0f) << 12) |
                                ((static_cast<unsigned char>(p[1]) & 0x3f) << 6) |
                                (static_cast<unsigned char>(p[2]) & 0x3f);
            out.push_back(cp);
            p += 3;
        } else if ((c & 0xf8) == 0xf0) {
            if (p + 3 >= end ||
                (static_cast<unsigned char>(p[1]) & 0xc0) != 0x80 ||
                (static_cast<unsigned char>(p[2]) & 0xc0) != 0x80 ||
                (static_cast<unsigned char>(p[3]) & 0xc0) != 0x80) {
                out.push_back(0xfffd);
                ++p;
                continue;
            }
            const uint32_t cp = ((c & 0x07) << 18) |
                                ((static_cast<unsigned char>(p[1]) & 0x3f) << 12) |
                                ((static_cast<unsigned char>(p[2]) & 0x3f) << 6) |
                                (static_cast<unsigned char>(p[3]) & 0x3f);
            out.push_back(cp);
            p += 4;
        } else {
            out.push_back(0xfffd);
            ++p;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Generic LCS / diff (equivalent to SequenceMatcher.get_opcodes).
// ---------------------------------------------------------------------------

struct match {
    size_t i = 0;
    size_t j = 0;
    size_t n = 0;
};

struct string_hash {
    size_t operator()(const kimix::string& s) const noexcept {
        return kimix::hash64(s.data(), s.size());
    }
};

template <typename T, typename Hash>
void find_longest_match(const kimix::vector<T>& a,
                        size_t alo,
                        size_t ahi,
                        const kimix::unordered_map<T, kimix::vector<size_t>, Hash>& b2j,
                        size_t blo,
                        size_t bhi,
                        match& out) {
    // j2len rows (SequenceMatcher's per-row j2len dicts) flattened to two
    // arrays over the [blo, bhi) window, preserving the dict semantics
    // exactly: each row reads only the *previous* row (prev), writes into
    // *curr*, then swaps them; a row whose line is not indexed in b produces
    // an empty previous row. Only positions actually written in a row are
    // cleared before the array is reused, so the per-row cost stays O(window)
    // with array indexing instead of a hash-map insert/lookup per (i, j)
    // pair. Tie-breaks are unchanged (strict `>` over ascending j).
    const size_t w = bhi - blo;
    kimix::vector<size_t> prev;
    kimix::vector<size_t> curr;
    kimix::vector<size_t> prev_touched;
    kimix::vector<size_t> curr_touched;
    if (w > 0) {
        prev.assign(w, 0);
        curr.assign(w, 0);
        prev_touched.reserve(w);
        curr_touched.reserve(w);
    }
    size_t best_i = alo;
    size_t best_j = blo;
    size_t best_n = 0;

    for (size_t i = alo; i < ahi; ++i) {
        // Clear the row currently occupying `curr` (it was swapped out of
        // `prev` at the end of the previous row's iteration).
        for (size_t p : curr_touched) {
            curr[p] = 0;
        }
        curr_touched.clear();
        auto it = b2j.find(a[i]);
        if (it == b2j.end()) {
            prev.swap(curr);
            prev_touched.swap(curr_touched);
            continue;
        }
        for (size_t j : it->second) {
            if (j < blo || j >= bhi) {
                continue;
            }
            size_t k = 1;
            if (j > blo) {
                const size_t v = prev[j - 1 - blo];
                if (v != 0) {
                    k = v + 1;
                }
            }
            const size_t pj = j - blo;
            curr[pj] = k;
            curr_touched.push_back(pj);
            if (k > best_n) {
                best_i = i + 1 - k;
                best_j = j + 1 - k;
                best_n = k;
            }
        }
        prev.swap(curr);
        prev_touched.swap(curr_touched);
    }

    out = {best_i, best_j, best_n};
}

template <typename T, typename Hash>
void find_matching_blocks(const kimix::vector<T>& a,
                          size_t alo,
                          size_t ahi,
                          const kimix::unordered_map<T, kimix::vector<size_t>, Hash>& b2j,
                          size_t blo,
                          size_t bhi,
                          kimix::vector<match>& out) {
    match m;
    find_longest_match<T, Hash>(a, alo, ahi, b2j, blo, bhi, m);
    if (m.n == 0) {
        return;
    }
    if (alo < m.i && blo < m.j) {
        find_matching_blocks<T, Hash>(a, alo, m.i, b2j, blo, m.j, out);
    }
    out.push_back(m);
    if (m.i + m.n < ahi && m.j + m.n < bhi) {
        find_matching_blocks<T, Hash>(a, m.i + m.n, ahi, b2j, m.j + m.n, bhi, out);
    }
}

// Hash selector: use the generic kimix::hash for arithmetic types (uint32_t),
// and a custom hasher for kimix::string.
template <typename T>
struct hash_selector {
    using type = kimix::hash<T>;
};

template <>
struct hash_selector<kimix::string> {
    using type = string_hash;
};

template <typename T>
void compute_opcodes_impl(const kimix::vector<T>& a,
                          const kimix::vector<T>& b,
                          kimix::vector<opcode>& out) {
    out.clear();
    using Hash = typename hash_selector<T>::type;
    // Index b once for the whole call (mirrors SequenceMatcher.__chain_b).
    // Recursive find_longest_match calls restrict by index range instead of
    // rebuilding the index per recursion level.
    kimix::unordered_map<T, kimix::vector<size_t>, Hash> b2j;
    b2j.reserve(b.size());
    for (size_t j = 0; j < b.size(); ++j) {
        b2j[b[j]].push_back(j);
    }
    kimix::vector<match> matches;
    find_matching_blocks<T, Hash>(a, 0, a.size(), b2j, 0, b.size(), matches);
    matches.push_back({a.size(), b.size(), 0}); // sentinel

    size_t i1 = 0;
    size_t j1 = 0;
    for (const auto& m : matches) {
        const size_t i2 = m.i;
        const size_t j2 = m.j;
        if (i1 < i2 || j1 < j2) {
            const char* tag = (i1 < i2 && j1 < j2) ? "replace"
                              : (i1 < i2)          ? "delete"
                                                   : "insert";
            out.push_back({tag, i1, i2, j1, j2});
        }
        if (m.n > 0) {
            out.push_back({"equal", i2, i2 + m.n, j2, j2 + m.n});
        }
        i1 = i2 + m.n;
        j1 = j2 + m.n;
    }
}

// ---------------------------------------------------------------------------
// Helpers for unified diff output.
// ---------------------------------------------------------------------------

kimix::string format_range(size_t start, size_t len) {
    if (len == 1) {
        return kimix::format("{}", start);
    }
    if (len == 0) {
        return kimix::format("{},0", start - 1);
    }
    return kimix::format("{},{}", start, len);
}

bool ends_with_newline(const kimix::string& s) {
    return !s.empty() && s.back() == '\n';
}

} // namespace

// ---------------------------------------------------------------------------
// Public: line splitting.
// ---------------------------------------------------------------------------

kimix::vector<kimix::string> split_lines(kimix::string_view text, bool keepends) {
    kimix::vector<kimix::string> result;
    const size_t n = text.size();
    // Cheap line-count estimate for reserve: every '\n' or '\r' terminates at
    // least one line (a '\r\n' pair counts two — over-estimating is fine).
    size_t estimate = 1;
    for (size_t k = 0; k < n; ++k) {
        const char c = text[k];
        if (c == '\n' || c == '\r') {
            ++estimate;
        }
    }
    result.reserve(estimate);
    size_t start = 0;
    size_t i = 0;

    while (i < n) {
        size_t boundary_len = 0;
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1e)) {
            boundary_len = 1;
            if (c == '\r' && i + 1 < n && static_cast<unsigned char>(text[i + 1]) == '\n') {
                boundary_len = 2;
            }
        } else if (c == 0xc2 && i + 1 < n) {
            const unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
            if (c2 == 0x85) {
                boundary_len = 2; // U+0085 NEXT LINE
            }
        } else if (c == 0xe2 && i + 2 < n) {
            const unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
            if (c2 == 0x80 && (c3 == 0xa8 || c3 == 0xa9)) {
                boundary_len = 3; // U+2028 LINE SEPARATOR / U+2029 PARAGRAPH SEPARATOR
            }
        }

        if (boundary_len > 0) {
            const size_t end = i + boundary_len;
            if (keepends) {
                result.emplace_back(text.substr(start, end - start));
            } else {
                result.emplace_back(text.substr(start, i - start));
            }
            start = end;
            i = end;
        } else {
            ++i;
        }
    }

    if (start < n) {
        result.emplace_back(text.substr(start));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Public: opcodes and grouping.
// ---------------------------------------------------------------------------

void compute_opcodes(const kimix::vector<kimix::string>& old_lines,
                     const kimix::vector<kimix::string>& new_lines,
                     kimix::vector<opcode>& out) {
    compute_opcodes_impl(old_lines, new_lines, out);
}

void group_opcodes(const kimix::vector<opcode>& opcodes,
                   size_t n,
                   kimix::vector<kimix::vector<opcode>>& out) {
    out.clear();
    kimix::vector<opcode> codes = opcodes;

    if (codes.empty()) {
        codes.push_back({"equal", 0, 1, 0, 1});
    }

    if (codes.front().tag == "equal") {
        opcode& first = codes.front();
        const size_t old_len = first.old_end - first.old_start;
        const size_t new_len = first.new_end - first.new_start;
        first.old_start = (old_len > n) ? (first.old_end - n) : first.old_start;
        first.new_start = (new_len > n) ? (first.new_end - n) : first.new_start;
    }

    if (codes.back().tag == "equal") {
        opcode& last = codes.back();
        last.old_end = std::min(last.old_end, last.old_start + n);
        last.new_end = std::min(last.new_end, last.new_start + n);
    }

    const size_t nn = n + n;
    kimix::vector<opcode> group;

    for (opcode& op : codes) {
        if (op.tag == "equal" && (op.old_end - op.old_start) > nn) {
            opcode tail = op;
            tail.old_end = op.old_start + n;
            tail.new_end = op.new_start + n;
            group.push_back(tail);
            out.push_back(std::move(group));
            group.clear();
            op.old_start = op.old_end - n;
            op.new_start = op.new_end - n;
        }
        group.push_back(op);
    }

    if (!group.empty() && !(group.size() == 1 && group.front().tag == "equal")) {
        out.push_back(std::move(group));
    }
}

// ---------------------------------------------------------------------------
// Public: unified diff.
// ---------------------------------------------------------------------------

kimix::string unified_diff(kimix::string_view old_text,
                           kimix::string_view new_text,
                           kimix::string_view path,
                           bool include_file_header,
                           kimix::string_view lineterm) {
    kimix::vector<kimix::string> old_lines = split_lines(old_text, true);
    kimix::vector<kimix::string> new_lines = split_lines(new_text, true);

    if (!old_lines.empty() && !ends_with_newline(old_lines.back())) {
        old_lines.back().push_back('\n');
    }
    if (!new_lines.empty() && !ends_with_newline(new_lines.back())) {
        new_lines.back().push_back('\n');
    }

    kimix::vector<opcode> opcodes;
    compute_opcodes(old_lines, new_lines, opcodes);

    kimix::vector<kimix::vector<opcode>> groups;
    group_opcodes(opcodes, 3, groups);

    if (groups.empty()) {
        return {};
    }

    const kimix::string fromfile = path.empty() ? "a/file" : ("a/" + kimix::string(path));
    const kimix::string tofile = path.empty() ? "b/file" : ("b/" + kimix::string(path));

    kimix::string result;
    result.reserve(fromfile.size() + tofile.size() + old_text.size() + new_text.size() + 64);

    if (include_file_header) {
        result += "--- ";
        result += fromfile;
        result.append(lineterm.data(), lineterm.size());
        result += "+++ ";
        result += tofile;
        result.append(lineterm.data(), lineterm.size());
    }

    for (const auto& group : groups) {
        const opcode& first = group.front();
        const opcode& last = group.back();

        const size_t old_start = first.old_start + 1;
        const size_t new_start = first.new_start + 1;
        const size_t old_len = last.old_end - first.old_start;
        const size_t new_len = last.new_end - first.new_start;

        result += "@@ -";
        result += format_range(old_start, old_len);
        result += " +";
        result += format_range(new_start, new_len);
        result += " @@";
        result.append(lineterm.data(), lineterm.size());

        for (const opcode& op : group) {
            if (op.tag == "equal") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += ' ';
                    result += old_lines[i];
                }
            } else if (op.tag == "delete") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += '-';
                    result += old_lines[i];
                }
            } else if (op.tag == "insert") {
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    result += '+';
                    result += new_lines[j];
                }
            } else if (op.tag == "replace") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    result += '-';
                    result += old_lines[i];
                }
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    result += '+';
                    result += new_lines[j];
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Public: diff_hunks.
// ---------------------------------------------------------------------------

kimix::vector<hunk> diff_hunks(kimix::string_view old_text,
                               kimix::string_view new_text,
                               size_t context_lines) {
    kimix::vector<kimix::string> old_lines = split_lines(old_text, false);
    kimix::vector<kimix::string> new_lines = split_lines(new_text, false);

    kimix::vector<opcode> opcodes;
    compute_opcodes(old_lines, new_lines, opcodes);

    kimix::vector<kimix::vector<opcode>> groups;
    group_opcodes(opcodes, context_lines, groups);

    kimix::vector<hunk> result;
    result.reserve(groups.size());

    for (const auto& group : groups) {
        hunk h;
        h.old_start = group.front().old_start + 1;
        h.new_start = group.front().new_start + 1;

        for (const opcode& op : group) {
            if (op.tag == "equal") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    h.old_lines.push_back(old_lines[i]);
                }
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    h.new_lines.push_back(new_lines[j]);
                }
            } else if (op.tag == "delete") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    h.old_lines.push_back(old_lines[i]);
                }
            } else if (op.tag == "insert") {
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    h.new_lines.push_back(new_lines[j]);
                }
            } else if (op.tag == "replace") {
                for (size_t i = op.old_start; i < op.old_end; ++i) {
                    h.old_lines.push_back(old_lines[i]);
                }
                for (size_t j = op.new_start; j < op.new_end; ++j) {
                    h.new_lines.push_back(new_lines[j]);
                }
            }
        }

        result.push_back(std::move(h));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Public: inline diff ranges.
// ---------------------------------------------------------------------------

namespace {

kimix::vector<uint32_t> expand_tabs(const kimix::vector<uint32_t>& cps,
                                    size_t tab_size,
                                    kimix::vector<size_t>& offset_map) {
    kimix::vector<uint32_t> out;
    offset_map.clear();
    offset_map.reserve(cps.size() + 1);
    size_t col = 0;

    for (uint32_t cp : cps) {
        offset_map.push_back(col);
        if (cp == '\t') {
            const size_t spaces = tab_size - (col % tab_size);
            for (size_t k = 0; k < spaces; ++k) {
                out.push_back(' ');
            }
            col += spaces;
        } else if (cp == '\n' || cp == '\r') {
            out.push_back(cp);
            col = 0;
        } else {
            out.push_back(cp);
            col += 1;
        }
    }
    offset_map.push_back(col);
    return out;
}

} // namespace

std::tuple<kimix::vector<offset_range>, kimix::vector<offset_range>>
inline_diff_ranges(kimix::string_view old_line,
                   kimix::string_view new_line,
                   double min_ratio,
                   size_t tab_size) {
    const kimix::vector<uint32_t> old_cps = decode_utf8_surrogatepass(old_line);
    const kimix::vector<uint32_t> new_cps = decode_utf8_surrogatepass(new_line);

    kimix::vector<opcode> opcodes;
    compute_opcodes_impl(old_cps, new_cps, opcodes);

    size_t matches = 0;
    for (const opcode& op : opcodes) {
        if (op.tag == "equal") {
            matches += op.old_end - op.old_start;
        }
    }

    const size_t total = old_cps.size() + new_cps.size();
    const double ratio = (total == 0) ? 1.0 : (2.0 * static_cast<double>(matches)) / static_cast<double>(total);
    if (ratio < min_ratio) {
        return {kimix::vector<offset_range>(), kimix::vector<offset_range>()};
    }

    kimix::vector<size_t> old_map;
    kimix::vector<size_t> new_map;
    expand_tabs(old_cps, tab_size, old_map);
    expand_tabs(new_cps, tab_size, new_map);

    kimix::vector<offset_range> deletes;
    kimix::vector<offset_range> inserts;

    for (const opcode& op : opcodes) {
        if (op.tag == "delete" || op.tag == "replace") {
            deletes.push_back({old_map[op.old_start], old_map[op.old_end]});
        }
        if (op.tag == "insert" || op.tag == "replace") {
            inserts.push_back({new_map[op.new_start], new_map[op.new_end]});
        }
    }

    return {std::move(deletes), std::move(inserts)};
}

// ---------------------------------------------------------------------------
// Public: build_offset_map.
// ---------------------------------------------------------------------------

void build_offset_map(kimix::string_view raw,
                      kimix::string_view rendered,
                      int tab_size,
                      kimix::vector<int>& out) {
    out.clear();

    // Defensive: guard the modulo below (col % tab_size) against a zero or
    // negative tab_size. The app contract assumes tab_size >= 1; for invalid
    // input the kernel stays defined (Python would raise ZeroDivisionError).
    const int tsize = (tab_size >= 1) ? tab_size : 1;

    // 1. raw == rendered: identity map of length len(raw) + 1 (code points).
    // UTF-8 byte equality is equivalent to code-point equality because every
    // code point has exactly one valid UTF-8 encoding.
    if (raw == rendered) {
        const size_t n = common::utf8_code_point_count(raw);
        out.reserve(n + 1);
        for (size_t i = 0; i <= n; ++i) {
            out.push_back(static_cast<int>(i));
        }
        return;
    }

    // 2. Walk raw code point by code point, replicating the column-aware tab
    // expansion Python's str.expandtabs defines.
    const size_t rendered_len = common::utf8_code_point_count(rendered);
    int64_t col = 0;
    const char* it = raw.data();
    const char* end = it + raw.size();
    while (it < end) {
        out.push_back(static_cast<int>(col));
        const uint32_t ch = common::decode_cp(it, end);
        if (ch == '\t') {
            col += tsize - (col % tsize);
        } else {
            col += 1;
        }
    }
    out.push_back(static_cast<int>(col));

    // 3. The highlighter transformed the text in a way we didn't expect:
    // return a bounded, monotonic best-effort map so inline stylizing can
    // proceed without crashing or producing out-of-range offsets.
    if (col != static_cast<int64_t>(rendered_len)) {
        out.clear();
        const size_t raw_len = common::utf8_code_point_count(raw);
        if (raw_len == 0) {
            out.push_back(static_cast<int>(rendered_len));
            return;
        }
        out.reserve(raw_len + 1);
        for (size_t i = 0; i < raw_len; ++i) {
            const int64_t v = (static_cast<int64_t>(i) * static_cast<int64_t>(rendered_len)) /
                              static_cast<int64_t>(raw_len);
            out.push_back(static_cast<int>(v));
        }
        out.push_back(static_cast<int>(rendered_len));
        return;
    }
    // 4. Otherwise the offsets list is already the answer.
}

} // namespace diff
} // namespace runtime
} // namespace kimix
