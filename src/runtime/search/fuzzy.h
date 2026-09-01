/*
 * fuzzy.h — Symmetric Delete fuzzy term expansion (kimix::runtime::search).
 *
 * Plan 005: native port of retrieval.py's symmetric-delete machinery:
 *   - _generate_deletes             (lines 240-260, static, lru_cached)
 *   - _build_symmetric_delete_index (lines 262-282 — a TWO-level index:
 *                                    max_edits=1 deletes and max_edits=2
 *                                    deletes, each mapping a delete variant
 *                                    to the terms that produce it)
 *   - LevenshteinAutomaton.match    (lines 907-942 — the candidate walk used
 *                                    for prefix_length == 1: gather candidate
 *                                    terms via the query's own deletes, then
 *                                    verify with length/prefix/freq/DL gates)
 *
 * The kernel is byte-based: deletes slice bytes. For pure-ASCII terms — the
 * only terms ever queried through expand() in the reference (Searcher only
 * fuzzy-expands Latin tokens, retrieval.py:1061-1064) — byte slicing is
 * identical to Python's char slicing. Non-ASCII terms produce byte-sliced
 * variants that are never looked up by an ASCII query, so behavior is
 * unchanged in practice (documented).
 *
 * Determinism (documented deviation): Python gathers candidates into a
 * `set` and returns them in arbitrary set-iteration order. The kernel sorts
 * the output deterministically by (score desc, term asc), where
 * score = 1.0 / (1.0 + dl(query, term)) — the reference has no score at
 * all; the score is added by plan 005's API (fuzzy_candidate{term, score})
 * so the candidate SET matches the reference while the ORDER is stable.
 *
 * The expansion cache is bounded (LRU, 4096) — the fix for the reference's
 * unbounded _expand_cache. add_term() invalidates the cache.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace search {

// One fuzzy expansion candidate: matched term + similarity score.
struct fuzzy_candidate {
    kimix::string term;
    double score;
};

class KIMIX_RUNTIME_API SymmetricDeleteIndex {
public:
    // LRU size for the expansion cache (the fix for the unbounded
    // `_expand_cache` in the reference Searcher).
    static constexpr size_t kCacheSize = 4096;
    // Maximum number of candidates returned by one expand() call — the
    // reference `match(max_expansions=50)` cap.
    static constexpr uint32_t kDefaultMaxExpansions = 50;

    // Heterogeneous hash/equality for string_view lookups into string-keyed
    // containers. PUBLIC: kimix::hash has no specialization for kimix::string
    // (mimalloc-allocated basic_string), so every kimix::string-keyed
    // container here must use these. Also required because a string_view
    // query must find a kimix::string key without a temporary allocation.
    struct TransparentStringHash {
        using is_transparent = void;
        size_t operator()(kimix::string_view sv) const noexcept {
            return static_cast<size_t>(kimix::hash64(sv.data(), sv.size()));
        }
        size_t operator()(const kimix::string& s) const noexcept {
            return static_cast<size_t>(kimix::hash64(s.data(), s.size()));
        }
    };
    struct TransparentStringEq {
        using is_transparent = void;
        bool operator()(kimix::string_view a, kimix::string_view b) const noexcept {
            return a == b;
        }
    };
    // kimix::string-keyed set with the transparent hash (see above).
    using StringSet = kimix::unordered_set<kimix::string, TransparentStringHash,
                                           TransparentStringEq>;

    SymmetricDeleteIndex() = default;
    ~SymmetricDeleteIndex() = default;
    SymmetricDeleteIndex(const SymmetricDeleteIndex&) = delete;
    SymmetricDeleteIndex& operator=(const SymmetricDeleteIndex&) = delete;

    // Add a term to the index: generates all delete variants for max_edits
    // 1..max_edits (excluding the term itself, like _build_symmetric_delete_
    // index) and maps variant -> term id. Invalidates the expansion cache.
    void add_term(kimix::string_view term, uint32_t max_edits = 2);

    // Expand `query` within `max_edits` edits. Mirrors the reference
    // candidate walk: candidates = union over query-deletes of the variant's
    // term lists (+ the query itself if indexed), gated by |len diff| <=
    // max_edits, first-char prefix match (prefix_length == 1), freq_lower_
    // bound <= max_edits, and damerau_levenshtein <= max_edits. Results are
    // sorted by (score desc, term asc) and capped at max_expansions. The
    // result is cached (LRU, keyed by query|max_edits|max_expansions).
    void expand(kimix::string_view query, uint32_t max_edits,
                kimix::vector<fuzzy_candidate>& out,
                uint32_t max_expansions = kDefaultMaxExpansions) const;

    // Number of indexed terms.
    size_t term_count() const noexcept;

    // True when `term` was added to the index.
    bool has_term(kimix::string_view term) const noexcept;

    // Drop all terms (and the cache).
    void reset();

private:
    // Generate all unique strings obtainable by deleting up to max_edits
    // chars — exact port of retrieval.py::_generate_deletes (byte-based).
    static void generate_deletes(kimix::string_view term, uint32_t max_edits,
                                 StringSet& out);

    kimix::vector<kimix::string> _terms;
    // term -> id: fast exact-match test (transparent hash so string_view
    // lookups work) AND O(1) id lookup (the candidate walk needs the id of
    // the exact query term; a scan over _terms would be O(n) per expand).
    kimix::unordered_map<kimix::string, uint32_t,
                         TransparentStringHash, TransparentStringEq>
        _term_ids;
    // variant -> term ids (two levels like the reference sd_index{1,2}).
    kimix::unordered_map<kimix::string, kimix::vector<uint32_t>,
                         TransparentStringHash, TransparentStringEq>
        _deletes1;
    kimix::unordered_map<kimix::string, kimix::vector<uint32_t>,
                         TransparentStringHash, TransparentStringEq>
        _deletes2;
    // bounded cache (mutable: expand() is const but updates the cache).
    // Keyed by std::string: kimix::hash has no specialization for
    // kimix::string, so the core lru_cache (which uses kimix::hash<Key>)
    // cannot key on kimix::string.
    mutable kimix::lru_cache<std::string, kimix::vector<fuzzy_candidate>> _cache{kCacheSize};
};

} // namespace search
} // namespace runtime
} // namespace kimix
