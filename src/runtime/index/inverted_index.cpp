/*
 * inverted_index.cpp — implementation of InvertedIndex (see header).
 *
 * Blob format (KNIDX1, little-endian; deterministic — save/load round-trips
 * byte-identically):
 *
 *   "KNIDX1"                    6 bytes magic
 *   u32  doc_count              distinct documents
 *   u32  max_doc_id             Python's N-1 (N = max_doc_id + 1)
 *   u64  sum_doc_lengths
 *   u32  doc_lengths_count      == doc_count; then per doc:
 *     u32 doc_id, u32 length
 *   u32  segment_count
 *   per segment:
 *     u32  term_count
 *     term table:               term_count * (u32 byte_len, bytes)
 *     term_offsets:             (term_count + 1) * u32  (CSR row starts)
 *     u32  postings_count       == term_offsets.back()
 *     postings:                 postings_count * (u32 doc_id, u32 tf)
 */

#include <runtime/index/inverted_index.h>

#include <algorithm>
#include <cstring>

namespace kimix {
namespace runtime {
namespace index {

namespace {

constexpr char kMagic[6] = {'K', 'N', 'I', 'D', 'X', '1'};

struct Writer {
    kimix::string& out;
    void u32(uint32_t v) {
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void u64(uint64_t v) {
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void bytes(kimix::string_view b) {
        out.append(b.data(), b.size());
    }
};

struct Reader {
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
    bool u64(uint64_t& v) {
        if (!ok || pos + 8 > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(&v, data.data() + pos, 8);
        pos += 8;
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

bool posting_less(const postings_entry& a, const postings_entry& b) {
    return a.doc_id < b.doc_id;
}

} // namespace

// Build one segment from (key, doc-sorted postings) rows. Row order is
// arbitrary — the builder sorts keys alphabetically (deterministic blob) and
// moves the key strings in: the old path copied every key into an
// intermediate term->postings map AND into term_keys before re-copying it
// into term_index; here each key is copied once (SSO for n-grams) and moved
// once. Per-term postings must already be sorted by doc_id.
InvertedIndex::Segment InvertedIndex::build_segment_from_rows(
    kimix::vector<std::pair<kimix::string, kimix::vector<postings_entry>>>& rows) {
    Segment seg;
    const size_t n = rows.size();
    seg.term_keys.reserve(n);
    seg.term_offsets.reserve(n + 1);
    seg.term_offsets.push_back(0);
    seg.term_index.reserve(n * 2);
    size_t total_pl = 0;
    for (const auto& r : rows) {
        total_pl += r.second.size();
    }
    seg.postings.reserve(total_pl);

    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& r : rows) {
        const uint32_t row = static_cast<uint32_t>(seg.term_keys.size());
        seg.term_index.emplace(r.first, row); // SSO copy for short n-grams
        seg.term_keys.push_back(std::move(r.first));
        seg.term_offsets.push_back(
            static_cast<uint32_t>(seg.postings.size() + r.second.size()));
        seg.postings.insert(seg.postings.end(), r.second.begin(), r.second.end());
    }
    return seg;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void InvertedIndex::add_document(uint32_t doc_id, kimix::span<const kimix::string_view> tokens) {
    // Count term frequencies (Python Counter). Views into the caller-owned
    // token buffer are fine — they are only read during this call.
    kimix::unordered_map<kimix::string_view, uint32_t> counter;
    counter.reserve(tokens.size());
    for (kimix::string_view t : tokens) {
        ++counter[t];
    }
    const uint32_t new_len = static_cast<uint32_t>(tokens.size());

    // Append (doc_id, tf) under a term VIEW: the transient hash lookup is
    // allocation-free (transparent string_view key), and an owned string key
    // is created only when the term is actually new. The old code built a
    // temporary kimix::string for every term on every add — a heap allocation
    // per token even when the term already existed in the delta.
    const auto push_posting = [this](kimix::string_view term, uint32_t id, uint32_t tf) {
        auto dit = _delta.find(term);
        if (dit == _delta.end()) {
            dit = _delta.emplace(kimix::string(term), DeltaEntry{}).first;
        }
        dit->second.postings.push_back({id, tf});
        dit->second.sorted = false;
    };

    auto meta_it = _doc_meta.find(doc_id);
    if (meta_it == _doc_meta.end()) {
        // New document.
        DocMeta meta;
        meta.length = new_len;
        meta.in_segment = false;
        meta.terms.reserve(counter.size());
        meta_it = _doc_meta.emplace(doc_id, std::move(meta)).first;
        _sum_doc_lengths += new_len;
        for (const auto& kv : counter) {
            meta_it->second.terms.emplace_back(kv.first);
            push_posting(kv.first, doc_id, kv.second);
        }
    } else if (!meta_it->second.in_segment) {
        // Delta-resident doc re-added: replace its postings atomically
        // (Python finalize's "rebuild from scratch handles overwrites",
        // localized to the delta). _doc_meta stores the previous terms.
        _sum_doc_lengths += static_cast<uint64_t>(new_len) - meta_it->second.length;
        meta_it->second.length = new_len;
        kimix::vector<kimix::string> old_terms = std::move(meta_it->second.terms);
        for (const auto& t : old_terms) {
            auto dit = _delta.find(t);
            if (dit == _delta.end()) {
                continue;
            }
            auto& pl = dit->second.postings;
            pl.erase(std::remove_if(pl.begin(), pl.end(),
                                    [doc_id](const postings_entry& e) { return e.doc_id == doc_id; }),
                     pl.end());
            if (pl.empty()) {
                _delta.erase(dit); // also frees the term key
            }
        }
        meta_it->second.terms.clear();
        meta_it->second.terms.reserve(counter.size());
        for (const auto& kv : counter) {
            meta_it->second.terms.emplace_back(kv.first);
            push_posting(kv.first, doc_id, kv.second);
        }
    } else {
        // Segment-resident doc re-added: append duplicates to the delta
        // (matches Python's PRE-finalize duplicate accumulation; documented
        // edge case — compact() does not dedupe these).
        _sum_doc_lengths += static_cast<uint64_t>(new_len) - meta_it->second.length;
        meta_it->second.length = new_len;
        for (const auto& kv : counter) {
            push_posting(kv.first, doc_id, kv.second);
        }
    }

    if (doc_id > _max_doc_id) {
        _max_doc_id = doc_id;
    }
}

void InvertedIndex::finalize() {
    if (!_delta.empty()) {
        build_segment_from_delta();
        // Auto-compaction: keep the segment count bounded for the
        // turn-per-finalize workload (plan 004 500-turn stress).
        uint32_t seg_postings = 0;
        for (const auto& seg : _segments) {
            seg_postings += seg.total_postings();
        }
        if (_segments.size() > kMaxSegmentsBeforeCompact ||
            seg_postings > kMaxPostingsBeforeCompact) {
            compact();
        }
    }
    _finalized = true;
}

kimix::span<const postings_entry> InvertedIndex::get_postings(kimix::string_view term) {
    auto dit = _delta.find(term);
    const bool in_delta = dit != _delta.end();

    // Count how many segments hold the term (0, 1, or many). Stop scanning
    // once two are found: both remaining outcomes (merge, and merge-with-
    // delta) go through merge_postings anyway.
    uint32_t match_count = 0;
    uint32_t seg_row = 0;
    const Segment* seg = nullptr;
    for (const auto& s : _segments) {
        const auto it = s.term_index.find(term);
        if (it != s.term_index.end()) {
            ++match_count;
            if (match_count == 1) {
                seg = &s;
                seg_row = it->second;
            } else {
                break; // >= 2 sources -> merge path
            }
        }
    }

    if (!in_delta && match_count == 0) {
        return {};
    }
    if (in_delta && match_count == 0) {
        // Delta-only: sort once after a mutation (is_sorted check cached in
        // the entry's `sorted` flag; appends are in add order).
        auto& pl = dit->second.postings;
        if (!dit->second.sorted) {
            if (pl.size() > 1) {
                std::sort(pl.begin(), pl.end(), posting_less);
            }
            dit->second.sorted = true;
        }
        return pl;
    }
    if (!in_delta && match_count == 1) {
        // Single segment: return a span into the immutable packed arrays.
        const uint32_t start = seg->term_offsets[seg_row];
        const uint32_t end = seg->term_offsets[seg_row + 1];
        return kimix::span<const postings_entry>(seg->postings.data() + start, end - start);
    }
    // Multi-source (delta + segments, or 2+ segments): k-way merge (lazily
    // cached per term).
    return merge_postings(term);
}

bool InvertedIndex::has_term(kimix::string_view term) const {
    if (_delta.find(term) != _delta.end()) {
        return true;
    }
    uint32_t row = 0;
    return find_in_segments(term, &row) != nullptr;
}

uint32_t InvertedIndex::doc_count() const noexcept {
    return static_cast<uint32_t>(_doc_meta.size());
}

uint32_t InvertedIndex::max_doc_id() const noexcept {
    return _max_doc_id;
}

uint32_t InvertedIndex::doc_length(uint32_t doc_id) const noexcept {
    const auto it = _doc_meta.find(doc_id);
    return it == _doc_meta.end() ? 0u : it->second.length;
}

uint64_t InvertedIndex::sum_doc_lengths() const noexcept {
    return _sum_doc_lengths;
}

double InvertedIndex::avg_doc_len() const noexcept {
    const uint32_t n = doc_count();
    return n == 0 ? 0.0 : static_cast<double>(_sum_doc_lengths) / static_cast<double>(n);
}

uint32_t InvertedIndex::total_postings() const noexcept {
    uint32_t total = 0;
    for (const auto& kv : _delta) {
        total += static_cast<uint32_t>(kv.second.postings.size());
    }
    for (const auto& seg : _segments) {
        total += seg.total_postings();
    }
    return total;
}

uint32_t InvertedIndex::segment_count() const noexcept {
    return static_cast<uint32_t>(_segments.size());
}

void InvertedIndex::compact() {
    if (_segments.size() <= 1) {
        return;
    }
    // Collect (term -> postings span) references across all segments. Each
    // segment's per-term postings are already doc-sorted, so after grouping
    // equal keys a k-way merge produces globally doc-sorted postings in O(m)
    // — the old code concatenated into a string-keyed map and re-sorted every
    // term's postings (O(m log m)) plus copied every term key twice.
    struct TermRef {
        kimix::string_view key;
        kimix::span<const postings_entry> pl;
    };
    size_t total_refs = 0;
    for (const auto& seg : _segments) {
        total_refs += seg.term_keys.size();
    }
    kimix::vector<TermRef> refs;
    refs.reserve(total_refs);
    for (const auto& seg : _segments) {
        for (size_t t = 0; t < seg.term_keys.size(); ++t) {
            const uint32_t start = seg.term_offsets[t];
            const uint32_t end = seg.term_offsets[t + 1];
            refs.push_back({seg.term_keys[t],
                            kimix::span<const postings_entry>(seg.postings.data() + start,
                                                              end - start)});
        }
    }
    std::sort(refs.begin(), refs.end(),
              [](const TermRef& a, const TermRef& b) { return a.key < b.key; });

    kimix::vector<std::pair<kimix::string, kimix::vector<postings_entry>>> rows;
    rows.reserve(refs.size());
    size_t i = 0;
    while (i < refs.size()) {
        size_t j = i + 1;
        while (j < refs.size() && refs[j].key == refs[i].key) {
            ++j;
        }
        const size_t k = j - i;
        if (k == 1) {
            rows.emplace_back(
                kimix::string(refs[i].key),
                kimix::vector<postings_entry>(refs[i].pl.begin(), refs[i].pl.end()));
        } else {
            // k-way merge of doc-sorted spans (k <= segment count, small).
            kimix::vector<postings_entry> merged;
            size_t total = 0;
            for (size_t m = i; m < j; ++m) {
                total += refs[m].pl.size();
            }
            merged.reserve(total);
            kimix::vector<size_t> cursors(k, 0);
            for (;;) {
                size_t best = k;
                uint32_t best_doc = 0;
                for (size_t m = 0; m < k; ++m) {
                    if (cursors[m] < refs[i + m].pl.size() &&
                        (best == k || refs[i + m].pl[cursors[m]].doc_id < best_doc)) {
                        best = m;
                        best_doc = refs[i + m].pl[cursors[m]].doc_id;
                    }
                }
                if (best == k) {
                    break;
                }
                merged.push_back(refs[i + best].pl[cursors[best]]);
                ++cursors[best];
            }
            rows.emplace_back(kimix::string(refs[i].key), std::move(merged));
        }
        i = j;
    }
    _segments.clear(); // refs (views into segment keys) are no longer used
    _segments.push_back(build_segment_from_rows(rows));
    invalidate_cache();
}

size_t InvertedIndex::save_blob_size() const {
    // Exact KNIDX1 size (see the format comment at the top of this file).
    // Like save_to/append_save_to, an unfinalized delta is flushed first so
    // the count reflects what will actually be serialized.
    if (!_delta.empty()) {
        const_cast<InvertedIndex*>(this)->finalize();
    }
    size_t size = 6;                       // magic
    size += 4 + 4 + 8 + 4;                 // doc_count, max_doc_id, sum, meta_count
    size += static_cast<size_t>(_doc_meta.size()) * 8;
    size += 4;                             // segment_count
    for (const auto& seg : _segments) {
        size += 4;                         // term_count
        for (const auto& key : seg.term_keys) {
            size += 4 + key.size();        // u32 byte_len + bytes
        }
        size += static_cast<size_t>(seg.term_keys.size() + 1) * 4; // term_offsets
        size += 4;                         // postings_count
        size += static_cast<size_t>(seg.total_postings()) * 8;     // postings
    }
    return size;
}

void InvertedIndex::save_to(kimix::string& blob) const {
    // Like Python's save(): an unfinalized index is finalized first.
    if (!_delta.empty()) {
        const_cast<InvertedIndex*>(this)->finalize();
    }
    blob.clear();
    blob.reserve(save_blob_size());
    write_blob(blob);
}

void InvertedIndex::append_save_to(kimix::string& blob) const {
    // Same payload as save_to() but appended to an existing buffer so
    // HistoryIndex::save_to can lay out its own header first and avoid
    // staging a full copy of the index blob. Caller must have reserved.
    if (!_delta.empty()) {
        const_cast<InvertedIndex*>(this)->finalize();
    }
    write_blob(blob);
}

void InvertedIndex::write_blob(kimix::string& blob) const {
    Writer w{blob};
    w.bytes(kimix::string_view(kMagic, sizeof(kMagic)));
    w.u32(doc_count());
    w.u32(_max_doc_id);
    w.u64(_sum_doc_lengths);
    w.u32(static_cast<uint32_t>(_doc_meta.size()));
    // Deterministic doc order for byte-identical round-trips: sort doc ids.
    kimix::vector<uint32_t> doc_ids;
    doc_ids.reserve(_doc_meta.size());
    for (const auto& kv : _doc_meta) {
        doc_ids.push_back(kv.first);
    }
    std::sort(doc_ids.begin(), doc_ids.end());
    for (uint32_t id : doc_ids) {
        w.u32(id);
        w.u32(_doc_meta.at(id).length);
    }
    w.u32(static_cast<uint32_t>(_segments.size()));
    for (const auto& seg : _segments) {
        w.u32(static_cast<uint32_t>(seg.term_keys.size()));
        for (const auto& key : seg.term_keys) {
            w.u32(static_cast<uint32_t>(key.size()));
            w.bytes(key);
        }
        for (uint32_t off : seg.term_offsets) {
            w.u32(off);
        }
        const uint32_t pc = seg.total_postings();
        w.u32(pc);
        for (const auto& e : seg.postings) {
            w.u32(e.doc_id);
            w.u32(e.tf);
        }
    }
}

bool InvertedIndex::load_from(kimix::string_view blob) {
    // Parse into locals FIRST and only commit to members on success — a
    // malformed blob must leave the index fully reset (doc_count == 0).
    kimix::unordered_map<uint32_t, DocMeta> doc_meta;
    kimix::vector<Segment> segments;
    uint32_t max_doc_id = 0;
    uint64_t sum_lengths = 0;
    { // scope for the reader
        Reader r{blob};
        kimix::string_view magic;
        if (!r.take(sizeof(kMagic), magic) ||
            magic != kimix::string_view(kMagic, sizeof(kMagic))) {
            return false;
        }
        uint32_t doc_count = 0, seg_count = 0;
        if (!r.u32(doc_count) || !r.u32(max_doc_id) || !r.u64(sum_lengths)) {
            return false;
        }
        uint32_t meta_count = 0;
        if (!r.u32(meta_count) || meta_count != doc_count) {
            return false;
        }
        doc_meta.reserve(meta_count);
        for (uint32_t i = 0; i < meta_count; ++i) {
            uint32_t doc_id = 0, len = 0;
            if (!r.u32(doc_id) || !r.u32(len)) {
                return false;
            }
            DocMeta meta;
            meta.length = len;
            meta.in_segment = true; // loaded docs are part of the immutable blob
            doc_meta.emplace(doc_id, meta);
        }
        if (!r.u32(seg_count)) {
            return false;
        }
        segments.reserve(seg_count);
        for (uint32_t s = 0; s < seg_count; ++s) {
            uint32_t term_count = 0;
            if (!r.u32(term_count)) {
                return false;
            }
            Segment seg;
            seg.term_keys.reserve(term_count);
            seg.term_offsets.reserve(term_count + 1);
            seg.term_index.reserve(term_count * 2);
            for (uint32_t t = 0; t < term_count; ++t) {
                uint32_t len = 0;
                kimix::string_view bytes;
                if (!r.u32(len) || !r.take(len, bytes)) {
                    return false;
                }
                seg.term_keys.emplace_back(bytes);
            }
            uint32_t prev_off = 0;
            for (uint32_t t = 0; t <= term_count; ++t) {
                uint32_t off = 0;
                if (!r.u32(off) || off < prev_off) {
                    return false;
                }
                prev_off = off;
                seg.term_offsets.push_back(off);
            }
            uint32_t postings_count = 0;
            if (!r.u32(postings_count) || postings_count != seg.term_offsets.back()) {
                return false;
            }
            seg.postings.reserve(postings_count);
            for (uint32_t i = 0; i < postings_count; ++i) {
                uint32_t doc_id = 0, tf = 0;
                if (!r.u32(doc_id) || !r.u32(tf)) {
                    return false;
                }
                seg.postings.push_back({doc_id, tf});
            }
            // Owned keys (see build_segment_from_rows): safe across moves.
            for (size_t t = 0; t < seg.term_keys.size(); ++t) {
                seg.term_index.emplace(seg.term_keys[t], static_cast<uint32_t>(t));
            }
            segments.push_back(std::move(seg));
        }
    } // reader scope

    // Commit.
    reset();
    _doc_meta = std::move(doc_meta);
    _segments = std::move(segments);
    _max_doc_id = max_doc_id;
    _sum_doc_lengths = sum_lengths;
    _finalized = true;
    return true;
}

void InvertedIndex::reset() {
    _delta.clear();
    _doc_meta.clear();
    _segments.clear();
    invalidate_cache();
    _max_doc_id = 0;
    _sum_doc_lengths = 0;
    _finalized = false;
}

// ---------------------------------------------------------------------------
// private helpers
// ---------------------------------------------------------------------------

const InvertedIndex::Segment* InvertedIndex::find_in_segments(
    kimix::string_view term, uint32_t* out_row) const {
    for (const auto& seg : _segments) {
        const auto it = seg.term_index.find(term);
        if (it != seg.term_index.end()) {
            *out_row = it->second;
            return &seg;
        }
    }
    return nullptr;
}

kimix::span<const postings_entry> InvertedIndex::merge_postings(kimix::string_view term) {
    // Check the cache first.
    auto cit = _merge_cache.find(term);
    if (cit != _merge_cache.end()) {
        return cit->second;
    }

    // Collect sorted sources: delta entry (sorted on demand, cached in the
    // entry's flag) + segment rows.
    kimix::vector<kimix::span<const postings_entry>> sources;
    auto dit = _delta.find(term);
    if (dit != _delta.end()) {
        auto& pl = dit->second.postings;
        if (!dit->second.sorted) {
            if (pl.size() > 1) {
                std::sort(pl.begin(), pl.end(), posting_less);
            }
            dit->second.sorted = true;
        }
        sources.push_back(pl);
    }
    for (const auto& seg : _segments) {
        const auto it = seg.term_index.find(term);
        if (it != seg.term_index.end()) {
            const uint32_t row = it->second;
            const uint32_t start = seg.term_offsets[row];
            const uint32_t end = seg.term_offsets[row + 1];
            sources.push_back(kimix::span<const postings_entry>(
                seg.postings.data() + start, end - start));
        }
    }

    kimix::vector<postings_entry> merged;
    size_t total = 0;
    for (const auto& src : sources) {
        total += src.size();
    }
    merged.reserve(total);
    kimix::vector<size_t> cursors(sources.size(), 0);
    while (true) {
        size_t best = sources.size();
        uint32_t best_doc = 0;
        for (size_t i = 0; i < sources.size(); ++i) {
            if (cursors[i] < sources[i].size() &&
                (best == sources.size() || sources[i][cursors[i]].doc_id < best_doc)) {
                best = i;
                best_doc = sources[i][cursors[i]].doc_id;
            }
        }
        if (best == sources.size()) {
            break;
        }
        merged.push_back(sources[best][cursors[best]]);
        ++cursors[best];
    }

    // Insert into the cache (owns the term key — node-stable).
    auto inserted = _merge_cache.emplace(kimix::string(term), std::move(merged));
    return inserted.first->second;
}

void InvertedIndex::build_segment_from_delta() {
    // Move each delta entry's postings into rows; the key is copied once (the
    // dense map owns its keys, so they cannot be moved out) and then moved
    // again into the segment by build_segment_from_rows — the old path copied
    // every key into an intermediate PostingsMap AND into term_keys AND into a
    // string-keyed term_index map.
    kimix::vector<std::pair<kimix::string, kimix::vector<postings_entry>>> rows;
    rows.reserve(_delta.size());
    for (auto& kv : _delta) {
        auto& de = kv.second;
        if (!de.sorted && de.postings.size() > 1) {
            std::sort(de.postings.begin(), de.postings.end(), posting_less);
        }
        de.sorted = true;
        rows.emplace_back(kv.first, std::move(de.postings));
    }
    _delta.clear();
    _segments.push_back(build_segment_from_rows(rows));
    // Docs just flushed are now segment-resident.
    for (auto& kv : _doc_meta) {
        if (!kv.second.in_segment) {
            kv.second.in_segment = true;
            kv.second.terms.clear();
            kv.second.terms.shrink_to_fit();
        }
    }
    invalidate_cache();
}

void InvertedIndex::invalidate_cache() {
    _merge_cache.clear();
}

} // namespace index
} // namespace runtime
} // namespace kimix
