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

// Build one segment from an ordered term->postings map (per-term postings
// must already be sorted by doc_id). term_keys are sorted alphabetically.
InvertedIndex::Segment InvertedIndex::build_segment_from_terms(const PostingsMap& terms) {
    Segment seg;
    const size_t n = terms.size();
    seg.term_keys.reserve(n);
    seg.term_offsets.reserve(n + 1);
    seg.term_offsets.push_back(0);
    seg.term_index.reserve(n * 2);
    seg.postings.reserve(n); // will grow with actual postings

    for (const auto& kv : terms) {
        seg.term_keys.push_back(kv.first);
    }
    std::sort(seg.term_keys.begin(), seg.term_keys.end());
    for (size_t i = 0; i < seg.term_keys.size(); ++i) {
        const auto& key = seg.term_keys[i];
        const auto it = terms.find(key);
        const auto& pl = it->second;
        // Owned key: the segment is moved into _segments later (vector<string>
        // relocates SSO strings on move), so views into term_keys would dangle.
        seg.term_index.emplace(key, static_cast<uint32_t>(i));
        seg.term_offsets.push_back(static_cast<uint32_t>(seg.postings.size() + pl.size()));
        seg.postings.insert(seg.postings.end(), pl.begin(), pl.end());
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
            _delta[kimix::string(kv.first)].postings.push_back({doc_id, kv.second});
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
            _delta[kimix::string(kv.first)].postings.push_back({doc_id, kv.second});
        }
    } else {
        // Segment-resident doc re-added: append duplicates to the delta
        // (matches Python's PRE-finalize duplicate accumulation; documented
        // edge case — compact() does not dedupe these).
        _sum_doc_lengths += static_cast<uint64_t>(new_len) - meta_it->second.length;
        meta_it->second.length = new_len;
        for (const auto& kv : counter) {
            _delta[kimix::string(kv.first)].postings.push_back({doc_id, kv.second});
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

    // Count how many segments hold the term (0, 1, or many).
    uint32_t match_count = 0;
    uint32_t seg_row = 0;
    const Segment* seg = nullptr;
    for (const auto& s : _segments) {
        const auto it = s.term_index.find(term);
        if (it != s.term_index.end()) {
            ++match_count;
            seg = &s;
            seg_row = it->second;
        }
    }

    if (!in_delta && match_count == 0) {
        return {};
    }
    if (in_delta && match_count == 0) {
        // Delta-only: ensure sorted by doc_id (appends are in add order).
        auto& pl = dit->second.postings;
        if (pl.size() > 1 &&
            !std::is_sorted(pl.begin(), pl.end(), posting_less)) {
            std::sort(pl.begin(), pl.end(), posting_less);
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
    // Group postings by term across all segments (per-term postings are each
    // sorted by doc_id -> k-way merge per term, then one flat segment).
    PostingsMap merged_terms;
    for (const auto& seg : _segments) {
        for (size_t t = 0; t < seg.term_keys.size(); ++t) {
            const uint32_t start = seg.term_offsets[t];
            const uint32_t end = seg.term_offsets[t + 1];
            auto& dst = merged_terms[seg.term_keys[t]];
            dst.insert(dst.end(), seg.postings.begin() + start, seg.postings.begin() + end);
        }
    }
    for (auto& kv : merged_terms) {
        std::sort(kv.second.begin(), kv.second.end(), posting_less);
    }
    _segments.clear();
    _segments.push_back(build_segment_from_terms(merged_terms));
    invalidate_cache();
}

void InvertedIndex::save_to(kimix::string& blob) const {
    // Like Python's save(): an unfinalized index is finalized first.
    InvertedIndex* self = const_cast<InvertedIndex*>(this);
    if (!_delta.empty()) {
        self->finalize();
    }
    blob.clear();
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
            // Owned keys (see build_segment_from_terms): safe across moves.
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

    // Collect sorted sources: delta entry (sorted on demand) + segment rows.
    kimix::vector<kimix::span<const postings_entry>> sources;
    auto dit = _delta.find(term);
    if (dit != _delta.end()) {
        auto& pl = dit->second.postings;
        if (pl.size() > 1 && !std::is_sorted(pl.begin(), pl.end(), posting_less)) {
            std::sort(pl.begin(), pl.end(), posting_less);
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
    PostingsMap terms;
    terms.reserve(_delta.size() * 2);
    for (auto& kv : _delta) {
        auto& pl = kv.second.postings;
        if (pl.size() > 1) {
            std::sort(pl.begin(), pl.end(), posting_less);
        }
        terms.emplace(kv.first, std::move(pl));
    }
    _segments.push_back(build_segment_from_terms(terms));
    _delta.clear();
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
