/*
 * inverted_index.h — Incremental inverted index (kimix::runtime::index).
 *
 * Plan 004: native port of `kimix/retrieval.py::InvertedIndex` with the
 * report's headline design change — a TRUE incremental index that never
 * needs a full rebuild after finalize():
 *
 *   - add_document(doc_id, tokens) appends into a small DELTA buffer and
 *     NEVER touches finalized segments: O(tokens) always, even after
 *     finalize() (the Python bug — add_document raises after finalize and
 *     history_index.py rebuilds all turns because of it — is fixed here).
 *   - finalize() flushes the delta into a new immutable segment (sorted
 *     packed postings, CSR-like flat arrays). Cheap: only the delta is
 *     sorted. When the segment count exceeds a threshold, finalize()
 *     triggers compact() (amortized).
 *   - get_postings(term) returns the merged postings: delta only -> delta
 *     view; exactly one segment -> a span into the segment (no copy);
 *     otherwise a lazily-built per-term merge cache (k-way merge).
 *   - compact() merges all segments into one (allows segment count to stay
 *     bounded in the 500-turn turn-per-finalize workload).
 *
 * Semantics parity with the reference (verified against retrieval.py):
 *   - Per-term postings are sorted by (doc_id, tf) exactly like Python's
 *     finalize() `list(zip(...)) + sort(key=doc_id)`.
 *   - tf is the per-(doc, term) count (Python Counter).
 *   - save_to()/load_from() round-trip byte-identically (own KNIDX1 blob;
 *     the Python msgspec/numpy format is not replicated — the blob format
 *     is shared with plan 006 instead).
 *
 * Documented deviations from the reference:
 *   - Python finalize(stop_threshold=0.5) drops n-grams whose df > N/2 and
 *     pure-punctuation n-grams. In the incremental model the global df is
 *     not stable across finalize() calls, so the native index keeps ALL
 *     terms. The Python shim's _compat mirror follows the native contract
 *     (no pruning), so native/_compat parity holds.
 *   - Re-adding a doc_id that was already flushed into a segment appends a
 *     second (doc_id, tf) entry to the delta (matching Python's PRE-finalize
 *     duplicate-posting accumulation). Re-adding a doc that is still in the
 *     delta replaces its postings atomically (matching Python's finalize
 *     "rebuild from scratch handles overwrites" behavior, localized to the
 *     delta). New docs can always be added after finalize — the headline fix.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 * Style: kimix containers, noexcept, no RTTI.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace index {

// One posting: document id + term frequency in that document.
struct postings_entry {
    uint32_t doc_id;
    uint32_t tf;
};

class KIMIX_RUNTIME_API InvertedIndex {
public:
    // Number of segments at/beyond which finalize() auto-compacts.
    static constexpr uint32_t kMaxSegmentsBeforeCompact = 8;
    // Total segment postings at/beyond which finalize() auto-compacts.
    static constexpr uint32_t kMaxPostingsBeforeCompact = 1u << 18; // 262144

    InvertedIndex() noexcept = default;
    ~InvertedIndex() = default;
    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;

    // Add a document's tokens. O(unique tokens) — NEVER touches finalized
    // segments, regardless of finalize() history. Replaces the postings of a
    // doc that is still in the delta; appends (Python pre-finalize style)
    // for a doc already flushed into a segment.
    void add_document(uint32_t doc_id, kimix::span<const kimix::string_view> tokens);

    // Flush the delta into a new immutable segment (sorts only the delta).
    // Auto-compacts when segments grow past the thresholds above. Cheap in
    // the common (small delta) case.
    void finalize();

    bool finalized() const noexcept { return _finalized; }

    // Merged postings for `term` (delta + segments, sorted by doc_id).
    // Returns an empty span when the term is absent (use has_term() to
    // distinguish). The returned span stays valid until the next mutating
    // call (finalize/compact/load_from/reset) — callers that keep it across
    // mutations must copy.
    kimix::span<const postings_entry> get_postings(kimix::string_view term);

    // True when the term has at least one posting (delta or segment).
    bool has_term(kimix::string_view term) const;

    // Number of DISTINCT documents added (Python len(_doc_lengths)).
    uint32_t doc_count() const noexcept;

    // Highest doc_id added + 1 — Python's `self._N` (idf denominator).
    uint32_t max_doc_id() const noexcept;

    // Token count of a document (0 when unknown). Used by BM25 scoring.
    uint32_t doc_length(uint32_t doc_id) const noexcept;

    // Sum of all document lengths (Python _sum_doc_lengths).
    uint64_t sum_doc_lengths() const noexcept;

    // avgdl = sum_doc_lengths / doc_count (Python _avgdl; 0 when no docs).
    double avg_doc_len() const noexcept;

    // Raw total postings across delta + segments (Python-style "total
    // postings" blob statistic).
    uint32_t total_postings() const noexcept;

    // Number of immutable segments currently held.
    uint32_t segment_count() const noexcept;

    // Merge all segments into one. Called automatically by finalize() past
    // the thresholds; exposed for tests / plan 006.
    void compact();

    // Serialize the index into `blob` (KNIDX1 format, see .cpp). Implicitly
    // finalizes first when the delta is non-empty (mirrors Python save()).
    void save_to(kimix::string& blob) const;

    // Deserialize from a KNIDX1 blob produced by save_to(). Replaces the
    // current state. Returns false on malformed input (state reset).
    bool load_from(kimix::string_view blob);

    // Reset to an empty, unfinalized index.
    void reset();

    // Heterogeneous hash/equality: lets maps keyed by kimix::string be looked
    // up with a borrowed kimix::string_view (no temporary allocation) — the
    // hot path of get_postings (O(1) lookup with a caller-owned view).
    // PUBLIC (tests / plan 006 reuse them; also kimix::hash has no
    // specialization for kimix::string, so these are REQUIRED for any
    // kimix::string-keyed container in this class).
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

private:
    // Immutable segment produced by finalize(). Term keys sorted
    // alphabetically (deterministic blob); postings sorted by doc_id.
    struct Segment {
        kimix::vector<kimix::string> term_keys;       // owns term bytes (sorted)
        // key -> row. Keys are OWNED copies (NOT views into term_keys): a
        // segment is moved when _segments grows, and moving a vector<string>
        // relocates SSO strings — views into them would dangle. Transparent
        // hash keeps string_view lookups allocation-free (one compare on hit).
        // Terms are n-grams (<= 15 bytes), so these copies stay in the SSO
        // buffer — no heap allocation per term.
        kimix::unordered_map<kimix::string, uint32_t,
                             TransparentStringHash, TransparentStringEq>
            term_index;
        kimix::vector<postings_entry> postings;       // packed (term-major, doc asc)
        kimix::vector<uint32_t> term_offsets;         // CSR row starts, size terms+1
        uint32_t total_postings() const noexcept { return term_offsets.empty() ? 0 : term_offsets.back(); }
    };

    // Delta buffer entry: owns the term string (map key == term, shared).
    // `sorted` caches whether postings are doc-ordered so get_postings does
    // not re-run an O(n) is_sorted scan on every read between mutations.
    struct DeltaEntry {
        kimix::vector<postings_entry> postings; // append-order; sorted at finalize/read
        bool sorted = true;                     // false after any push_back
    };
    using TermMap = kimix::unordered_map<kimix::string, DeltaEntry,
                                         TransparentStringHash, TransparentStringEq>;
    using CacheMap = kimix::unordered_map<kimix::string, kimix::vector<postings_entry>,
                                          TransparentStringHash, TransparentStringEq>;

    // ---- helpers ----
    const Segment* find_in_segments(kimix::string_view term, uint32_t* out_row) const;
    // k-way merge of delta + all segments for `term` into `_merge_cache[term]`.
    kimix::span<const postings_entry> merge_postings(kimix::string_view term);
    void build_segment_from_delta();
    void invalidate_cache();
    // Build one segment from (key, doc-sorted postings) rows. Keys are moved
    // into the segment (row order is arbitrary; the builder sorts by key).
    // Per-term postings must already be sorted by doc_id.
    static Segment build_segment_from_rows(
        kimix::vector<std::pair<kimix::string, kimix::vector<postings_entry>>>& rows);
    // Exact serialized KNIDX1 size (after finalize-when-dirty, mirroring
    // save_to's implicit finalize) — lets history save reserve once and append
    // the index blob directly instead of staging a full copy.
    size_t save_blob_size() const;
    // Append the KNIDX1 payload to `blob` (no clear, no intermediate buffer).
    void append_save_to(kimix::string& blob) const;
    // Shared KNIDX1 payload writer (appends; the caller controls clear/reserve).
    void write_blob(kimix::string& blob) const;
    // Write doc lengths into a caller-owned dense array (indexed by doc_id;
    // entries without a length keep their prior value). Avoids N individual
    // doc_length() hash lookups during HistoryIndex::search (one map walk).
    void fill_doc_lengths(uint32_t* out, uint32_t count) const noexcept {
        for (const auto& kv : _doc_meta) {
            if (kv.first < count) {
                out[kv.first] = kv.second.length;
            }
        }
    }
    friend class HistoryIndex;

    // ---- state ----
    TermMap _delta;
    // doc_id -> (doc_length, in_segment) for lengths/avgdl; in_segment marks
    // docs already flushed (re-adds then append duplicates to the delta).
    struct DocMeta {
        uint32_t length = 0;
        bool in_segment = false;
        // Terms this doc contributed — kept ONLY while the doc is delta-
        // resident, so a re-add can remove the old (term, doc) postings.
        // Cleared when the doc is flushed into a segment.
        kimix::vector<kimix::string> terms;
    };
    kimix::unordered_map<uint32_t, DocMeta> _doc_meta;
    kimix::vector<Segment> _segments;
    CacheMap _merge_cache;
    uint32_t _max_doc_id = 0;       // Python's N = max_doc_id + 1
    uint64_t _sum_doc_lengths = 0;
    bool _finalized = false;
};

} // namespace index
} // namespace runtime
} // namespace kimix
