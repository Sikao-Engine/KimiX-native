#pragma once

#include "vector.h"
#include "algorithm.h"

#include <utility>
#include <algorithm>

namespace kimix {

// ---------------------------------------------------------------------------
// vector_map — a simple flat map stored in a sorted vector of pairs
// ---------------------------------------------------------------------------

template <typename Key, typename Value>
class vector_map {
public:
    using value_type = std::pair<Key, Value>;
    using container_type = vector<value_type>;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;

    vector_map() = default;

    Value& operator[](const Key& key) {
        auto it = find(key);
        if (it != _data.end()) {
            return it->second;
        }
        auto pos = std::lower_bound(_data.begin(), _data.end(), key,
            [](const value_type& p, const Key& k) { return p.first < k; });
        auto inserted = _data.insert(pos, value_type{key, Value{}});
        return inserted->second;
    }

    iterator find(const Key& key) {
        auto it = std::lower_bound(_data.begin(), _data.end(), key,
            [](const value_type& p, const Key& k) { return p.first < k; });
        if (it != _data.end() && it->first == key) { return it; }
        return _data.end();
    }

    const_iterator find(const Key& key) const {
        auto it = std::lower_bound(_data.begin(), _data.end(), key,
            [](const value_type& p, const Key& k) { return p.first < k; });
        if (it != _data.end() && it->first == key) { return it; }
        return _data.end();
    }

    iterator begin() noexcept { return _data.begin(); }
    const_iterator begin() const noexcept { return _data.begin(); }
    iterator end() noexcept { return _data.end(); }
    const_iterator end() const noexcept { return _data.end(); }
    size_t size() const noexcept { return _data.size(); }
    bool empty() const noexcept { return _data.empty(); }
    void clear() noexcept { _data.clear(); }
    void reserve(size_t n) { _data.reserve(n); }

    container_type& data() noexcept { return _data; }
    const container_type& data() const noexcept { return _data; }

private:
    container_type _data;
};

} // namespace kimix
