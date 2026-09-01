/*
 * distance.cpp — implementation of the distance/similarity kernels (see header).
 *
 * All functions are exact ports of the verified retrieval.py source; float
 * computations use double in the same operation order as the Python float64
 * expressions so results are bit-identical.
 */

#include <runtime/search/distance.h>

#include <runtime/common/utf8.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace kimix {
namespace runtime {
namespace search {

namespace {

// Code-point sequence: raw bytes when pure ASCII (each byte == one code
// point), else a decoded vector<uint32_t> (owned when !ascii and bytes is
// null). ASCII handling avoids the UTF-8 decode entirely on the fast path.
struct CpSeq {
    const char* bytes = nullptr; // valid when ascii == true
    const uint32_t* cps = nullptr; // valid when ascii == false
    size_t len = 0;
    bool ascii = true;

    uint32_t at(size_t i) const noexcept {
        return ascii ? static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) : cps[i];
    }
};

// Build a CpSeq from a string_view: decodes UTF-8 only when non-ASCII.
CpSeq make_seq(kimix::string_view sv, kimix::vector<uint32_t>& scratch) {
    if (common::is_ascii(sv)) {
        CpSeq s;
        s.bytes = sv.data();
        s.len = sv.size();
        s.ascii = true;
        return s;
    }
    const char* it = sv.data();
    const char* end = it + sv.size();
    while (it < end) {
        scratch.push_back(common::decode_cp(it, end));
    }
    CpSeq s;
    s.cps = scratch.data();
    s.len = scratch.size();
    s.ascii = false;
    return s;
}

// Core OSAbL Damerau-Levenshtein DP over code-point sequences. Exact port of
// retrieval.py lines 845-884 (the swap so len(s) >= len(t), the n==0 / n==1 /
// m==2&&n==2 fast paths, and the three-row DP with the restricted
// transposition rule). When max_dist >= 0 the DP may early-exit returning
// max_dist + 1: the minimum over a DP row is a valid lower bound on the
// final distance for any edit model with non-negative op costs (incl. OSAbL
// transpositions), so once every cell in the current row exceeds max_dist
// the true distance is > max_dist.
int32_t dl_dp(const CpSeq& s, const CpSeq& t, int32_t max_dist) noexcept {
    // s is the longer string (m >= n), like Python after the swap.
    const size_t m = s.len;
    const size_t n = t.len;
    if (n == 0) {
        const int32_t d = static_cast<int32_t>(m);
        return (max_dist >= 0 && d > max_dist) ? max_dist + 1 : d;
    }
    if (n == 1) {
        // Reference fast path (retrieval.py:852-853): returns 0/1 based only
        // on the first char — preserved exactly, including the m > 1 quirk.
        const int32_t d = s.at(0) == t.at(0) ? 0 : 1;
        return (max_dist >= 0 && d > max_dist) ? max_dist + 1 : d;
    }
    if (m == 2 && n == 2) {
        // Reference fast path (retrieval.py:854-861).
        int32_t d = 2;
        if (s.at(0) == t.at(0) && s.at(1) == t.at(1)) {
            d = 0;
        } else if (s.at(0) == t.at(0) || s.at(1) == t.at(1)) {
            d = 1;
        } else if (s.at(0) == t.at(1) && s.at(1) == t.at(0)) {
            d = 1;
        }
        return (max_dist >= 0 && d > max_dist) ? max_dist + 1 : d;
    }

    // Small-buffer optimization: DP rows of up to kStackRow int32s live on
    // the stack (no heap allocation per call); longer rows fall back to the
    // heap. The DP arithmetic is unchanged.
    constexpr size_t kStackRow = 256;
    const size_t row_len = n + 1;
    int32_t st_prev_prev[kStackRow];
    int32_t st_prev[kStackRow];
    int32_t st_curr[kStackRow];
    kimix::vector<int32_t> heap_prev_prev, heap_prev, heap_curr;
    if (row_len > kStackRow) {
        heap_prev_prev.resize(row_len);
        heap_prev.resize(row_len);
        heap_curr.resize(row_len);
    }
    int32_t* prev_prev = row_len <= kStackRow ? st_prev_prev : heap_prev_prev.data();
    int32_t* prev = row_len <= kStackRow ? st_prev : heap_prev.data();
    int32_t* curr = row_len <= kStackRow ? st_curr : heap_curr.data();
    for (size_t j = 0; j <= n; ++j) {
        prev_prev[j] = static_cast<int32_t>(j);
        prev[j] = static_cast<int32_t>(j);
    }
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int32_t>(i);
        const uint32_t si_1 = s.at(i - 1);
        for (size_t j = 1; j <= n; ++j) {
            const int32_t cost = (si_1 == t.at(j - 1)) ? 0 : 1;
            int32_t v = curr[j - 1] + 1;            // insertion
            if (prev[j] + 1 < v) {
                v = prev[j] + 1;                    // deletion
            }
            if (prev[j - 1] + cost < v) {
                v = prev[j - 1] + cost;             // substitution
            }
            if (i > 1 && j > 1 && si_1 == t.at(j - 2) && s.at(i - 2) == t.at(j - 1)) {
                const int32_t trans = prev_prev[j - 2] + 1;
                if (trans < v) {
                    v = trans;                      // transposition
                }
            }
            curr[j] = v;
        }
        if (max_dist >= 0) {
            // Row-minimum lower bound: all cells above max_dist -> answer > max_dist.
            int32_t row_min = curr[0];
            for (size_t j = 1; j <= n; ++j) {
                if (curr[j] < row_min) {
                    row_min = curr[j];
                }
            }
            if (row_min > max_dist) {
                return max_dist + 1;
            }
        }
        std::swap(prev_prev, prev);
        std::swap(prev, curr);
    }
    const int32_t d = prev[n];
    return (max_dist >= 0 && d > max_dist) ? max_dist + 1 : d;
}

} // namespace

int32_t damerau_levenshtein(kimix::string_view a, kimix::string_view b, int32_t max_dist) noexcept {
    kimix::vector<uint32_t> a_scratch, b_scratch;
    CpSeq sa = make_seq(a, a_scratch);
    CpSeq tb = make_seq(b, b_scratch);
    // Python: if len(s) < len(t): s, t = t, s  (so s is the longer string)
    if (sa.len < tb.len) {
        std::swap(sa, tb);
    }
    return dl_dp(sa, tb, max_dist);
}

int32_t freq_lower_bound(kimix::string_view pattern, kimix::string_view term) noexcept {
    // Exact port of retrieval.py::_freq_lower_bound (lines 886-905).
    // The term_len <= 32 branch uses term.count(c) per pattern char; the
    // longer branch counts via a dict. Both compute the same totals; we use
    // one code-point scan. The FORMULA is what matters for parity:
    //   total = sum over pattern chars c of |pc - tc| (only when pc != tc)
    //   matched = sum over pattern chars c of tc
    //   total += len(term) - matched
    //   return (total + 1) // 2
    // where pc = count of c in pattern, tc = count of c in term.
    kimix::vector<uint32_t> pv_scratch, tv_scratch;
    CpSeq pv = make_seq(pattern, pv_scratch);
    CpSeq tv = make_seq(term, tv_scratch);

    // ASCII fast path: bytes == code points, so fixed 256-slot count arrays
    // on the stack replace the two per-call hash maps below (identical
    // totals — pure integer arithmetic, same formula).
    if (pv.ascii && tv.ascii) {
        int32_t pcounts[256] = {0};
        int32_t tcounts[256] = {0};
        for (size_t i = 0; i < pv.len; ++i) {
            ++pcounts[static_cast<unsigned char>(pv.bytes[i])];
        }
        for (size_t i = 0; i < tv.len; ++i) {
            ++tcounts[static_cast<unsigned char>(tv.bytes[i])];
        }
        int32_t total = 0;
        int32_t matched = 0;
        for (int c = 0; c < 256; ++c) {
            const int32_t pc = pcounts[c];
            if (pc == 0) {
                continue;
            }
            const int32_t tc = tcounts[c];
            matched += tc;
            if (pc != tc) {
                total += pc > tc ? (pc - tc) : (tc - pc);
            }
        }
        total += static_cast<int32_t>(tv.len) - matched;
        return (total + 1) / 2;
    }

    // Count pattern chars and term chars once (one pass each). The reference
    // counts per unique pattern char by scanning the whole term; precomputing
    // the term counts yields identical totals in O(len(term)) instead of
    // O(unique_pattern_chars * len(term)).
    kimix::unordered_map<uint32_t, int32_t> pcounts;
    for (size_t i = 0; i < pv.len; ++i) {
        ++pcounts[pv.at(i)];
    }
    kimix::unordered_map<uint32_t, int32_t> tcounts;
    tcounts.reserve(tv.len);
    for (size_t i = 0; i < tv.len; ++i) {
        ++tcounts[tv.at(i)];
    }

    int32_t total = 0;
    int32_t matched = 0;
    for (const auto& kv : pcounts) {
        const uint32_t c = kv.first;
        const int32_t pc = kv.second;
        // tc = count of c in term (0 when absent).
        const auto it = tcounts.find(c);
        const int32_t tc = it != tcounts.end() ? it->second : 0;
        matched += tc;
        if (pc != tc) {
            total += pc > tc ? (pc - tc) : (tc - pc);
        }
    }
    total += static_cast<int32_t>(tv.len) - matched;
    return (total + 1) / 2;
}

double jaro_similarity(kimix::string_view a, kimix::string_view b) noexcept {
    kimix::vector<uint32_t> sa_scratch, tb_scratch;
    CpSeq sa = make_seq(a, sa_scratch);
    CpSeq tb = make_seq(b, tb_scratch);
    if (sa.len == tb.len) {
        bool equal = true;
        for (size_t i = 0; i < sa.len; ++i) {
            if (sa.at(i) != tb.at(i)) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return 1.0;
        }
    }
    const size_t len_s = sa.len;
    const size_t len_t = tb.len;
    if (len_s == 0 || len_t == 0) {
        return 0.0;
    }
    // match_distance = max(len_s, len_t) // 2 - 1  (Python floor division)
    const size_t match_distance = (len_s > len_t ? len_s : len_t) / 2 - 1;
    // uint8_t flags instead of std::vector<bool>: the latter is bit-packed
    // (each access costs shift/mask), the flags here are plain true/false.
    kimix::vector<uint8_t> s_matches(len_s, 0);
    kimix::vector<uint8_t> t_matches(len_t, 0);
    size_t matches = 0;
    for (size_t i = 0; i < len_s; ++i) {
        const size_t start = i > match_distance ? i - match_distance : 0;
        const size_t end = (i + match_distance + 1) < len_t ? (i + match_distance + 1) : len_t;
        for (size_t j = start; j < end; ++j) {
            if (t_matches[j] || sa.at(i) != tb.at(j)) {
                continue;
            }
            s_matches[i] = true;
            t_matches[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0) {
        return 0.0;
    }
    size_t transpositions = 0;
    size_t k = 0;
    for (size_t i = 0; i < len_s; ++i) {
        if (!s_matches[i]) {
            continue;
        }
        while (!t_matches[k]) {
            ++k;
        }
        if (sa.at(i) != tb.at(k)) {
            ++transpositions;
        }
        ++k;
    }
    // (matches/len_s + matches/len_t + (matches - transpositions/2)/matches)/3.0
    // in double, same order as Python.
    const double a1 = static_cast<double>(matches) / static_cast<double>(len_s);
    const double a2 = static_cast<double>(matches) / static_cast<double>(len_t);
    const double a3 = (static_cast<double>(matches) - static_cast<double>(transpositions) / 2.0) /
                      static_cast<double>(matches);
    return (a1 + a2 + a3) / 3.0;
}

double jaro_winkler(kimix::string_view a, kimix::string_view b, double prefix_scale) noexcept {
    const double jaro = jaro_similarity(a, b);
    kimix::vector<uint32_t> sa_scratch, tb_scratch;
    CpSeq sa = make_seq(a, sa_scratch);
    CpSeq tb = make_seq(b, tb_scratch);
    size_t prefix = 0;
    const size_t limit = (std::min)((std::min)(size_t{4}, sa.len), tb.len);
    for (size_t i = 0; i < limit; ++i) {
        if (sa.at(i) == tb.at(i)) {
            ++prefix;
        } else {
            break;
        }
    }
    return jaro + static_cast<double>(prefix) * prefix_scale * (1.0 - jaro);
}

double sorensen_dice(kimix::string_view a, kimix::string_view b) noexcept {
    kimix::vector<uint32_t> sa_scratch, tb_scratch;
    CpSeq sa = make_seq(a, sa_scratch);
    CpSeq tb = make_seq(b, tb_scratch);
    const bool a_empty = sa.len == 0;
    const bool b_empty = tb.len == 0;
    if (a_empty && b_empty) {
        return 1.0;
    }
    if (a_empty || b_empty) {
        return 0.0;
    }
    // ASCII fast path: bigrams are byte pairs; fixed stack arrays with
    // sort+unique+intersection replace the per-call heap sets below (same
    // set-count semantics -> identical double result, no allocation).
    if (sa.ascii && tb.ascii) {
        constexpr size_t kStackGrams = 128; // strings up to 129 code points
        if (sa.len <= kStackGrams + 1 && tb.len <= kStackGrams + 1) {
            uint64_t ag[kStackGrams];
            uint64_t bg[kStackGrams];
            size_t an = 0, bn = 0;
            const auto pair_of = [](uint32_t x, uint32_t y) {
                return (static_cast<uint64_t>(x) << 32) | y;
            };
            for (size_t i = 0; i + 1 < sa.len; ++i) {
                ag[an++] = pair_of(static_cast<unsigned char>(sa.bytes[i]),
                                   static_cast<unsigned char>(sa.bytes[i + 1]));
            }
            for (size_t i = 0; i + 1 < tb.len; ++i) {
                bg[bn++] = pair_of(static_cast<unsigned char>(tb.bytes[i]),
                                   static_cast<unsigned char>(tb.bytes[i + 1]));
            }
            std::sort(ag, ag + an);
            an = static_cast<size_t>(std::unique(ag, ag + an) - ag);
            std::sort(bg, bg + bn);
            bn = static_cast<size_t>(std::unique(bg, bg + bn) - bg);
            size_t intersection = 0;
            size_t ia = 0, ib = 0;
            while (ia < an && ib < bn) {
                if (ag[ia] < bg[ib]) {
                    ++ia;
                } else if (bg[ib] < ag[ia]) {
                    ++ib;
                } else {
                    ++intersection;
                    ++ia;
                    ++ib;
                }
            }
            const size_t denom = an + bn;
            if (denom == 0) {
                return 0.0;
            }
            return 2.0 * static_cast<double>(intersection) / static_cast<double>(denom);
        }
        // Longer ASCII strings fall through to the generic code-point path.
    }
    // Bigram sets (code-point pairs), like Python's {s[i:i+2] for ...}.
    kimix::unordered_set<uint64_t> a_grams;
    kimix::unordered_set<uint64_t> b_grams;
    const auto pair_of = [](uint32_t x, uint32_t y) { return (static_cast<uint64_t>(x) << 32) | y; };
    for (size_t i = 0; i + 1 < sa.len; ++i) {
        a_grams.insert(pair_of(sa.at(i), sa.at(i + 1)));
    }
    for (size_t i = 0; i + 1 < tb.len; ++i) {
        b_grams.insert(pair_of(tb.at(i), tb.at(i + 1)));
    }
    size_t intersection = 0;
    for (uint64_t g : a_grams) {
        if (b_grams.count(g)) {
            ++intersection;
        }
    }
    const size_t denom = a_grams.size() + b_grams.size();
    if (denom == 0) {
        return 0.0;
    }
    return 2.0 * static_cast<double>(intersection) / static_cast<double>(denom);
}

double ngram_overlap(kimix::string_view a, kimix::string_view b, uint32_t n) noexcept {
    kimix::vector<uint32_t> sa_scratch, tb_scratch;
    CpSeq sa = make_seq(a, sa_scratch);
    CpSeq tb = make_seq(b, tb_scratch);
    if (sa.len == 0 || tb.len == 0) {
        return 0.0;
    }
    if (n == 0) {
        return 0.0; // degenerate; reference would produce empty slices
    }

    // ASCII fast path: bytes == code points, so grams are plain byte spans of
    // the input buffer. Fixed stack arrays with sort+unique+intersection
    // replace the per-call unordered_set<string_view> (same set-count
    // semantics -> identical double result; no heap allocation). Both inputs
    // must be pure ASCII for the byte==code-point identity to hold.
    if (sa.ascii && tb.ascii) {
        constexpr size_t kStackGrams = 128;
        const auto fits = [n](const CpSeq& v) noexcept {
            return v.len < n || (v.len - n + 1 <= kStackGrams);
        };
        if (fits(sa) && fits(tb)) {
            kimix::string_view ag[kStackGrams];
            kimix::string_view bg[kStackGrams];
            size_t an = 0, bn = 0;
            auto make_ascii_grams = [n](const CpSeq& v, kimix::string_view* out,
                                        size_t& cnt) {
                if (v.len < n) {
                    // len < n collapses the whole string into a single gram.
                    out[cnt++] = kimix::string_view(v.bytes, v.len);
                    return;
                }
                for (size_t i = 0; i + n <= v.len; ++i) {
                    out[cnt++] = kimix::string_view(v.bytes + i, n);
                }
            };
            make_ascii_grams(sa, ag, an);
            make_ascii_grams(tb, bg, bn);
            std::sort(ag, ag + an);
            an = static_cast<size_t>(std::unique(ag, ag + an) - ag);
            std::sort(bg, bg + bn);
            bn = static_cast<size_t>(std::unique(bg, bg + bn) - bg);
            size_t intersection = 0;
            size_t ia = 0, ib = 0;
            while (ia < an && ib < bn) {
                if (ag[ia] < bg[ib]) {
                    ++ia;
                } else if (bg[ib] < ag[ia]) {
                    ++ib;
                } else {
                    ++intersection;
                    ++ia;
                    ++ib;
                }
            }
            const size_t union_size = an + bn - intersection;
            if (union_size == 0) {
                return 0.0;
            }
            return static_cast<double>(intersection) / static_cast<double>(union_size);
        }
        // Longer ASCII inputs fall through to the generic code-point path.
    }
    // {s[i:i+n]} when len >= n else {s} — grams are n-code-point sequences.
    // kimix::hash has no specialization for vector<uint32_t>, so a custom
    // hash over the gram bytes is used (XXH3-64, like kimix::hash64).
    struct vec_u32_hash {
        size_t operator()(const kimix::vector<uint32_t>& v) const noexcept {
            return static_cast<size_t>(kimix::hash64(v.data(), v.size() * sizeof(uint32_t)));
        }
    };
    kimix::unordered_set<kimix::vector<uint32_t>, vec_u32_hash> a_grams;
    kimix::unordered_set<kimix::vector<uint32_t>, vec_u32_hash> b_grams;
    auto make_grams = [n](const CpSeq& v,
                          kimix::unordered_set<kimix::vector<uint32_t>, vec_u32_hash>& out) {
        if (v.len < n) {
            kimix::vector<uint32_t> whole;
            whole.reserve(v.len);
            for (size_t i = 0; i < v.len; ++i) {
                whole.push_back(v.at(i));
            }
            out.insert(std::move(whole));
            return;
        }
        for (size_t i = 0; i + n <= v.len; ++i) {
            kimix::vector<uint32_t> gram;
            gram.reserve(n);
            for (uint32_t k = 0; k < n; ++k) {
                gram.push_back(v.at(i + k));
            }
            out.insert(std::move(gram));
        }
    };
    make_grams(sa, a_grams);
    make_grams(tb, b_grams);
    size_t intersection = 0;
    for (const auto& g : a_grams) {
        if (b_grams.count(g)) {
            ++intersection;
        }
    }
    const size_t union_size = a_grams.size() + b_grams.size() - intersection;
    if (union_size == 0) {
        return 0.0;
    }
    return static_cast<double>(intersection) / static_cast<double>(union_size);
}

} // namespace search
} // namespace runtime
} // namespace kimix
