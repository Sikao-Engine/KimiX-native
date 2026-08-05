#pragma once

#include "unordered_map.h"
#include "list.h"

#include <utility>
#include <cstddef>
#include <optional>

namespace kimix {

// ---------------------------------------------------------------------------
// Simple LRU (Least Recently Used) cache
// ---------------------------------------------------------------------------

template <typename Key, typename Value>
class lru_cache {
public:
    using key_value_pair = std::pair<Key, Value>;
    using list_iterator = typename list<key_value_pair>::iterator;

    explicit lru_cache(size_t max_size) : _max_size(max_size) {}

    void put(const Key& key, const Value& value) {
        auto it = _cache.find(key);
        if (it != _cache.end()) {
            _items.erase(it->second);
            _cache.erase(it);
        }

        _items.push_front({key, value});
        _cache[key] = _items.begin();

        if (_cache.size() > _max_size) {
            auto last = _items.end();
            --last;
            _cache.erase(last->first);
            _items.pop_back();
        }
    }

    std::optional<Value> get(const Key& key) {
        auto it = _cache.find(key);
        if (it == _cache.end()) {
            return std::nullopt;
        }
        _items.splice(_items.begin(), _items, it->second);
        return it->second->second;
    }

    bool contains(const Key& key) const {
        return _cache.find(key) != _cache.end();
    }

    void erase(const Key& key) {
        auto it = _cache.find(key);
        if (it != _cache.end()) {
            _items.erase(it->second);
            _cache.erase(it);
        }
    }

    void clear() {
        _items.clear();
        _cache.clear();
    }

    size_t size() const noexcept { return _cache.size(); }
    bool empty() const noexcept { return _cache.empty(); }

private:
    size_t _max_size;
    list<key_value_pair> _items;
    unordered_map<Key, list_iterator> _cache;
};

} // namespace kimix
