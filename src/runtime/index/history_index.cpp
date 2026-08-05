/*
 * history_index.cpp — implementation of HistoryIndex (see header).
 *
 * KNHIX1 blob format (little-endian; deterministic — save/load round-trips
 * byte-identically):
 *
 *   "KNHIX1"                  6 bytes magic
 *   u32  turn_count           turns in the deque (oldest first)
 *   u32  doc_count            index doc_count (KNIDX1 validation)
 *   per turn (turn_count times):
 *     u32 turn_id
 *     f64 timestamp
 *     u8  role                (0=user 1=assistant 2=tool 3=other)
 *     u8  flags               (bit 0 = is_compacted)
 *     u32 text_len, text_len bytes (pre-normalized UTF-8)
 *   index blob                the raw KNIDX1 payload (InvertedIndex.save_to)
 *
 * load_from() parses the turns into locals and the index into a fresh
 * InvertedIndex::load_from() call; only when EVERYTHING parsed does it commit
 * (deque move + index load) — a malformed blob leaves the object reset.
 */

#include <runtime/index/history_index.h>

#include <runtime/search/bm25.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace index {

namespace {

// NOTE: these helpers are named with the History* prefix because the runtime
// target builds with unity (jumbo) compilation — inverted_index.cpp already
// defines kMagic/Writer/Reader in this anonymous namespace, and unity merges
// both TUs into one, so the names must not collide.
constexpr char kHistoryMagic[6] = {'K', 'N', 'H', 'I', 'X', '1'};

struct HistoryWriter {
    kimix::string& out;
    void u32(uint32_t v) {
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void f64(double v) {
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void u8(uint8_t v) {
        out.push_back(static_cast<char>(v));
    }
    void bytes(kimix::string_view b) {
        out.append(b.data(), b.size());
    }
};

struct HistoryReader {
    kimix::string_view data;
    size_t pos = 0;
    bool ok = true;

    bool u32(uint32_t& v) {
        if (!ok || pos + 4 > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(&v, data.data() + pos, 4);
        pos += 4;
        return true;
    }
    bool f64(double& v) {
        if (!ok || pos + 8 > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(&v, data.data() + pos, 8);
        pos += 8;
        return true;
    }
    bool u8(uint8_t& v) {
        if (!ok || pos + 1 > data.size()) {
            ok = false;
            return false;
        }
        v = static_cast<uint8_t>(data[pos]);
        pos += 1;
        return true;
    }
    bool take(size_t n, kimix::string_view& v) {
        if (!ok || pos + n > data.size()) {
            ok = false;
            return false;
        }
        v = kimix::string_view(data.data() + pos, n);
        pos += n;
        return true;
    }
};

} // namespace

HistoryIndex::HistoryIndex() = default;

void HistoryIndex::append_turns(kimix::span<const turn_meta> turns) {
    for (const auto& t : turns) {
        _turns.push_back(t);
        // Tokenize the (pre-normalized + stripped) text and index it with
        // doc_id == turn_id (reference semantics). norm must outlive the
        // token views — add_document copies them into its own storage.
        const kimix::string norm = _tokenizer.normalize(t.text);
        ++_tokenize_calls;
        const uint32_t n = _tokenizer.detect_n(norm);
        kimix::vector<kimix::string_view> grams;
        _tokenizer.tokenize(norm, n, grams);
        _index.add_document(t.turn_id, grams);
    }
    // Enforce the size bound — drop oldest turns. The index is NOT touched
    // (stale doc_ids are harmless; reference `_turns.pop(0)` behavior).
    while (_turns.size() > MAX_TURNS) {
        _turns.pop_front();
    }
}

void HistoryIndex::mark_compacted() {
    for (auto& t : _turns) {
        t.is_compacted = true;
    }
}

void HistoryIndex::set_persist_path(kimix::string_view path) {
    _persist_path.assign(path.data(), path.size());
}

kimix::vector<turn_meta> HistoryIndex::search(kimix::string_view query,
                                              uint32_t top_k) {
    kimix::vector<turn_meta> out;
    if (_turns.empty() || top_k == 0) {
        return out;
    }
    // Reference `search` finalizes an unfinalized index before searching.
    if (!_index.finalized()) {
        _index.finalize();
    }

    // Tokenize the query (pre-normalized + stripped by the caller).
    const kimix::string norm = _tokenizer.normalize(query);
    ++_tokenize_calls;
    const uint32_t n = _tokenizer.detect_n(norm);
    kimix::vector<kimix::string_view> query_terms;
    _tokenizer.tokenize(norm, n, query_terms);

    // Per-term merged postings + idf. doc_count = N = max_doc_id + 1
    // (sparse-safe; includes evicted docs, matching the reference index).
    const uint32_t doc_count = _index.max_doc_id() + 1;
    kimix::vector<kimix::span<const postings_entry>> postings;
    kimix::vector<double> idf;
    postings.reserve(query_terms.size());
    idf.reserve(query_terms.size());
    for (kimix::string_view term : query_terms) {
        auto pl = _index.get_postings(term);
        postings.push_back(pl);
        idf.push_back(search::bm25_idf(doc_count, static_cast<uint32_t>(pl.size()),
                                       1.2, 0.75));
    }

    // doc_lengths indexed by doc_id (0 for unknown — Python zeros array).
    kimix::vector<uint32_t> doc_lengths(doc_count, 0);
    for (uint32_t i = 0; i < doc_count; ++i) {
        doc_lengths[i] = _index.doc_length(i);
    }

    kimix::vector<double> scores;
    search::Bm25Scorer scorer;
    scorer.score(postings, idf, doc_lengths, _index.avg_doc_len(), doc_count,
                 scores);

    kimix::vector<uint32_t> top_docs;
    search::top_k(scores, top_k, top_docs);

    // Map doc_id (== turn_id) back to the turn; skip evicted ids (reference
    // `for turn in self._turns: if turn["turn_id"] == doc_id` loop).
    for (uint32_t doc : top_docs) {
        const turn_meta* t = get_by_id(doc);
        if (t == nullptr) {
            continue;
        }
        turn_meta copy = *t;
        copy.score = scores[doc];
        out.push_back(std::move(copy));
    }
    return out;
}

const turn_meta* HistoryIndex::get_by_id(uint32_t turn_id) const {
    for (const auto& t : _turns) {
        if (t.turn_id == turn_id) {
            return &t;
        }
    }
    return nullptr;
}

void HistoryIndex::save_to(kimix::string& blob) const {
    // Index blob first (finalizes an unfinalized index — Python save()
    // parity; the KNIDX1 payload is the flat, deterministic buffer).
    kimix::string index_blob;
    _index.save_to(index_blob);

    blob.clear();
    HistoryWriter w{blob};
    w.bytes(kimix::string_view(kHistoryMagic, sizeof(kHistoryMagic)));
    w.u32(static_cast<uint32_t>(_turns.size()));
    w.u32(_index.doc_count());
    for (const auto& t : _turns) {
        w.u32(t.turn_id);
        w.f64(t.timestamp);
        w.u8(t.role);
        w.u8(t.is_compacted ? 1u : 0u);
        w.u32(static_cast<uint32_t>(t.text.size()));
        w.bytes(t.text);
    }
    w.bytes(index_blob);
}

bool HistoryIndex::load_from(kimix::string_view blob) {
    // Parse everything into locals FIRST; commit only on success. A malformed
    // blob leaves the object reset (spec) and returns false.
    kimix::deque<turn_meta> turns;
    uint32_t header_doc_count = 0;
    kimix::string_view index_blob_view;
    {
        HistoryReader r{blob};
        kimix::string_view magic;
        if (!r.take(sizeof(kHistoryMagic), magic) ||
            magic != kimix::string_view(kHistoryMagic, sizeof(kHistoryMagic))) {
            reset();
            return false;
        }
        uint32_t turn_count = 0;
        if (!r.u32(turn_count) || !r.u32(header_doc_count)) {
            reset();
            return false;
        }
        for (uint32_t i = 0; i < turn_count; ++i) {
            turn_meta t;
            uint32_t text_len = 0;
            uint8_t flags = 0;
            kimix::string_view text;
            if (!r.u32(t.turn_id) || !r.f64(t.timestamp) || !r.u8(t.role) ||
                !r.u8(flags) || !r.u32(text_len) || !r.take(text_len, text)) {
                reset();
                return false;
            }
            t.is_compacted = (flags & 1u) != 0;
            t.text.assign(text.data(), text.size());
            turns.push_back(std::move(t));
        }
        // The remainder is the KNIDX1 index blob.
        if (!r.take(r.data.size() - r.pos, index_blob_view)) {
            reset();
            return false;
        }
    }

    // Parse the index only after the turns succeeded. InvertedIndex::load_from
    // is itself atomic (parses into locals, commits on success). On any
    // failure reset() clears both _turns and _index (spec: malformed blob
    // leaves the object reset).
    if (!_index.load_from(index_blob_view) ||
        _index.doc_count() != header_doc_count) {
        reset();
        return false;
    }
    // Commit (atomic): the deque is moved in only after everything parsed.
    _turns = std::move(turns);
    _tokenize_calls = 0; // load performs no tokenization
    return true;
}

uint32_t HistoryIndex::turn_count() const noexcept {
    return static_cast<uint32_t>(_turns.size());
}

void HistoryIndex::pop_front() {
    if (!_turns.empty()) {
        _turns.pop_front();
    }
}

void HistoryIndex::reset() {
    _turns.clear();
    _index.reset();
    _tokenize_calls = 0;
}

} // namespace index
} // namespace runtime
} // namespace kimix
