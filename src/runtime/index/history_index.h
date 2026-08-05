/*
 * history_index.h — Turn-metadata layer over the Plan 004 InvertedIndex
 * (kimix::runtime::index). Plan 006: native port of
 * `kimi_cli/soul/history_index.py` with the report's design changes:
 *
 *   - append_turns() is ALWAYS incremental — O(text) per turn, even after
 *     prior searches. The Python bug (full re-tokenize/re-add of <=500 turns
 *     whenever the index was finalized by a search) is structurally
 *     impossible here: there is no rebuild path.
 *   - save()/load() use a single-pass binary blob (KNHIX1): turn metadata
 *     + the Plan 004 KNIDX1 index blob. load_from() performs NO tokenization
 *     (postings are serialized) and is atomic: a malformed blob leaves the
 *     object reset.
 *   - Eviction at MAX_TURNS=500 pops the oldest turn in O(1) (kimix::deque;
 *     Python `_turns.pop(0)` is O(n)). Stale doc_ids stay in the index and
 *     are harmless — get_by_id() returns nullptr and search() skips them,
 *     exactly like the reference (which never prunes the index).
 *
 * Doc-id contract (PARITY-CRITICAL): doc_id == turn_id, like the reference
 * (`self._index.add_document(turn["turn_id"], tokens)`). N = max_doc_id + 1
 * therefore includes evicted turns and BM25 scores match the Python index
 * bit-for-bit (same N, same doc lengths, same avgdl — the index is never
 * pruned on eviction). The task's "doc_id = turn index in deque" reading is
 * deliberately NOT used: deque positions shift on pop_front, so index
 * postings would no longer map to deque slots after eviction.
 *
 * Text contract: the caller (Python shim) passes ALREADY-NORMALIZED +
 * STRIPPED UTF-8 (lowercased + NFKC'd via the shim's unicodedata composition
 * — the kernel's normalize() is the documented ASCII fast path only, see
 * ngram_tokenizer.h). The kernel applies normalize() (idempotent on
 * pre-normalized input), detect_n() and tokenize(), mirroring
 * retrieval.py::NgramTokenizer.tokenize on the normalized text.
 *
 * role encoding: 0=user 1=assistant 2=tool 3=other (u8). is_compacted flags
 * in the blob: bit 0.
 *
 * Pure C++ kernel: no Python includes; the GIL is released in the binding
 * layer (src/runtime/py/py_history.cpp). Style: kimix containers, noexcept,
 * no RTTI.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/index/ngram_tokenizer.h>
#include <runtime/index/inverted_index.h>

namespace kimix {
namespace runtime {
namespace index {

// One conversation turn. `score` is transient (filled by search(); never
// serialized — the KNHIX1 blob stores the other fields only).
struct turn_meta {
    uint32_t turn_id = 0;
    double timestamp = 0.0;    // time.time() seconds
    uint8_t role = 3;          // 0=user 1=assistant 2=tool 3=other
    bool is_compacted = false;
    kimix::string text;        // pre-normalized + stripped UTF-8 (see above)
    double score = 0.0;        // BM25 score, set only by search()
};

class KIMIX_RUNTIME_API HistoryIndex {
public:
    static constexpr uint32_t MAX_TURNS = 500;

    HistoryIndex();
    ~HistoryIndex() = default;
    HistoryIndex(const HistoryIndex&) = delete;
    HistoryIndex& operator=(const HistoryIndex&) = delete;

    // Append turns (incremental, always). O(total text). Evicts the oldest
    // turns beyond MAX_TURNS (O(1) deque front-pop); evicted docs stay in the
    // index (stale doc_ids are harmless — reference semantics).
    void append_turns(kimix::span<const turn_meta> turns);

    // Set is_compacted = true on ALL currently-indexed turns (reference
    // `mark_compacted` iterates every turn; verified against history_index.py
    // lines 86-89).
    void mark_compacted();

    // Optional persist path, stored for the Python wrapper.
    void set_persist_path(kimix::string_view path);

    // BM25 top-k over the query. Returns copies of the matching turns with
    // `score` filled, ordered (score desc, turn_id/doc_id asc) — the
    // reference's ordering. Evicted doc_ids that still have postings are
    // skipped (get_by_id returns nullptr). Tokenizes the query only.
    kimix::vector<turn_meta> search(kimix::string_view query, uint32_t top_k);

    // Linear scan over the deque (<=500 entries — reference get_by_id is the
    // same loop). Returns nullptr when the turn was evicted or never added.
    const turn_meta* get_by_id(uint32_t turn_id) const;

    // Serialize: magic "KNHIX1" | u32 turn_count | u32 doc_count |
    //   per turn (u32 turn_id, f64 timestamp, u8 role, u8 flags, u32 text_len,
    //             text bytes) | KNIDX1 index blob.
    // Implicitly finalizes an unfinalized index (Python save() parity).
    void save_to(kimix::string& blob) const;

    // Deserialize a KNHIX1 blob in a single pass; NO re-tokenization (the
    // postings are serialized). Returns true on success. Atomic: parses into
    // locals and commits only on success; a malformed blob leaves the object
    // reset and returns false.
    bool load_from(kimix::string_view blob);

    uint32_t turn_count() const noexcept;

    // O(1) deque front-pop (exposed for the Python wrapper's eviction path).
    void pop_front();

    // Clear all turns + index. The tokenize counter is reset too.
    void reset();

    // Test instrumentation: total tokenize() invocations since construction /
    // last load/reset (per appended turn + per search query; load performs
    // none — asserted by test_history_index.cpp).
    uint64_t tokenize_call_count() const noexcept { return _tokenize_calls; }

private:
    kimix::deque<turn_meta> _turns;   // oldest first (matches _turns list order)
    InvertedIndex _index;             // doc_id == turn_id (reference semantics)
    NgramTokenizer _tokenizer{2};     // reference NgramTokenizer(n=2)
    kimix::string _persist_path;
    uint64_t _tokenize_calls = 0;
};

} // namespace index
} // namespace runtime
} // namespace kimix
