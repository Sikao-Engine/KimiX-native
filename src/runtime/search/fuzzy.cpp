/*
 * fuzzy.cpp — implementation of SymmetricDeleteIndex (see header).
 */

#include <runtime/search/fuzzy.h>

#include <runtime/search/distance.h>

#include <algorithm>

namespace kimix {
namespace runtime {
namespace search {

namespace {

// Exact port of retrieval.py::_generate_deletes (lines 240-260).
// - max_edits == 0 or empty term -> {term}
// - max_edits == 1 -> {term} + all single-char deletions
// - otherwise iterative: repeat max_edits times, extending by one deletion.
void generate_deletes_impl(kimix::string_view term, uint32_t max_edits,
                           SymmetricDeleteIndex::StringSet& out) {
    if (max_edits == 0 || term.empty()) {
        out.insert(kimix::string(term));
        return;
    }
    const size_t n = term.size();
    if (max_edits == 1) {
        out.reserve(n + 1);
        out.insert(kimix::string(term));
        for (size_t i = 0; i < n; ++i) {
            kimix::string v;
            v.reserve(n - 1);
            v.append(term.data(), i);
            v.append(term.data() + i + 1, n - i - 1);
            out.insert(std::move(v));
        }
        return;
    }
    SymmetricDeleteIndex::StringSet deletes;
    deletes.insert(kimix::string(term));
    for (uint32_t step = 0; step < max_edits; ++step) {
        SymmetricDeleteIndex::StringSet new_deletes;
        for (const auto& t : deletes) {
            const size_t tl = t.size();
            for (size_t i = 0; i < tl; ++i) {
                kimix::string v;
                v.reserve(tl - 1);
                v.append(t.data(), i);
                v.append(t.data() + i + 1, tl - i - 1);
                new_deletes.insert(std::move(v));
            }
        }
        for (auto& v : new_deletes) {
            deletes.insert(std::move(v));
        }
    }
    for (auto& v : deletes) {
        out.insert(std::move(v));
    }
}

} // namespace

void SymmetricDeleteIndex::generate_deletes(kimix::string_view term,
                                             uint32_t max_edits,
                                             StringSet& out) {
    generate_deletes_impl(term, max_edits, out);
}

void SymmetricDeleteIndex::add_term(kimix::string_view term, uint32_t max_edits) {
    const uint32_t id = static_cast<uint32_t>(_terms.size());
    _terms.emplace_back(term);
    _term_set.insert(_terms.back());
    // Build the 1..max_edits delete levels (the reference builds both level 1
    // and level 2 in _build_symmetric_delete_index).
    for (uint32_t me = 1; me <= max_edits && me <= 2; ++me) {
        StringSet variants;
        generate_deletes(term, me, variants);
        auto* map = (me == 1) ? &_deletes1 : &_deletes2;
        for (const auto& v : variants) {
            if (v != term) {
                (*map)[v].push_back(id);
            }
        }
    }
    _cache.clear();
}

void SymmetricDeleteIndex::expand(kimix::string_view query, uint32_t max_edits,
                                  kimix::vector<fuzzy_candidate>& out,
                                  uint32_t max_expansions) const {
    out.clear();
    if (max_edits == 0 || query.empty()) {
        // Reference: max_edits == 0 is short-circuited by the caller
        // (_expand_token returns the exact term). Empty query matches nothing.
        if (!query.empty() && has_term(query)) {
            out.push_back({kimix::string(query), 1.0});
        }
        return;
    }
    if (max_expansions == 0) {
        return;
    }

    std::string cache_key;
    cache_key.reserve(query.size() + 8);
    cache_key.append(query.data(), query.size());
    cache_key.push_back('\x1f');
    cache_key += std::to_string(max_edits);
    cache_key.push_back('\x1f');
    cache_key += std::to_string(max_expansions);
    if (auto cached = _cache.get(cache_key)) {
        out = std::move(*cached);
        return;
    }

    kimix::vector<fuzzy_candidate> result;
    const size_t pattern_len = query.size();

    // Candidates: union over the query's own deletes of the variant's term
    // lists, plus the query itself when indexed.
    kimix::unordered_set<uint32_t> candidate_ids;
    const auto* map = (max_edits == 1) ? &_deletes1 : &_deletes2;
    // Reference: `sd = sd_index.get(max_edits) or sd_index.get(1, {})` — an
    // EMPTY max_edits level falls back to the level-1 map.
    if (max_edits == 2 && map->empty()) {
        map = &_deletes1;
    }
    StringSet query_deletes;
    generate_deletes(query, max_edits, query_deletes);
    for (const auto& variant : query_deletes) {
        const auto it = map->find(variant);
        if (it != map->end()) {
            for (uint32_t id : it->second) {
                candidate_ids.insert(id);
            }
        }
    }
    if (has_term(query)) {
        // id of the exact query term (scan is fine: the index is small and
        // this is a cold path; the reference uses dictionary.has_term).
        for (size_t i = 0; i < _terms.size(); ++i) {
            if (_terms[i] == query) {
                candidate_ids.insert(static_cast<uint32_t>(i));
                break;
            }
        }
    }

    // Verify each candidate with the reference gates (match(), lines 930-941).
    for (uint32_t id : candidate_ids) {
        if (result.size() >= max_expansions) {
            break;
        }
        const kimix::string& term = _terms[id];
        const size_t term_len = term.size();
        if (term_len > pattern_len ? term_len - pattern_len > max_edits
                                   : pattern_len - term_len > max_edits) {
            continue; // |len diff| > max_edits
        }
        if (term_len >= 1 && term[0] != query[0]) {
            continue; // prefix_length == 1 first-char match
        }
        // has_freq_filter = len(pattern) <= 64
        if (pattern_len <= 64 && freq_lower_bound(query, term) > static_cast<int32_t>(max_edits)) {
            continue;
        }
        const int32_t dl = damerau_levenshtein(query, term, static_cast<int32_t>(max_edits));
        if (dl <= static_cast<int32_t>(max_edits)) {
            result.push_back({term, 1.0 / (1.0 + static_cast<double>(dl))});
        }
    }

    // Deterministic ordering: (score desc, term asc). The reference returns
    // arbitrary set order — documented deviation.
    std::sort(result.begin(), result.end(),
              [](const fuzzy_candidate& a, const fuzzy_candidate& b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  return a.term < b.term;
              });
    if (result.size() > max_expansions) {
        result.resize(max_expansions);
    }

    _cache.put(cache_key, result);
    out = std::move(result);
}

size_t SymmetricDeleteIndex::term_count() const noexcept {
    return _terms.size();
}

bool SymmetricDeleteIndex::has_term(kimix::string_view term) const noexcept {
    return _term_set.find(term) != _term_set.end();
}

void SymmetricDeleteIndex::reset() {
    _terms.clear();
    _term_set.clear();
    _deletes1.clear();
    _deletes2.clear();
    _cache.clear();
}

} // namespace search
} // namespace runtime
} // namespace kimix
