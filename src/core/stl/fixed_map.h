#pragma once

#include <array>
#include <utility>
#include <cstddef>
#include <optional>

namespace kimix {

// ---------------------------------------------------------------------------
// fixed_map — a fixed-capacity map backed by std::array
// ---------------------------------------------------------------------------

template <typename Key, typename Value, size_t N>
class fixed_map {
public:
    using value_type = std::pair<Key, Value>;
    using container_type = std::array<value_type, N>;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;

    fixed_map() = default;

    Value& operator[](const Key& key) {
        for (size_t i = 0; i < _count; ++i) {
            if (_data[i].first == key) { return _data[i].second; }
        }
        _data[_count] = value_type{key, Value{}};
        return _data[_count++].second;
    }

    iterator find(const Key& key) {
        for (size_t i = 0; i < _count; ++i) {
            if (_data[i].first == key) { return _data.begin() + i; }
        }
        return end();
    }

    const_iterator find(const Key& key) const {
        for (size_t i = 0; i < _count; ++i) {
            if (_data[i].first == key) { return _data.begin() + i; }
        }
        return end();
    }

    iterator begin() noexcept { return _data.begin(); }
    const_iterator begin() const noexcept { return _data.begin(); }
    iterator end() noexcept { return _data.begin() + _count; }
    const_iterator end() const noexcept { return _data.begin() + _count; }

    size_t size() const noexcept { return _count; }
    static constexpr size_t capacity() noexcept { return N; }
    bool empty() const noexcept { return _count == 0; }
    bool full() const noexcept { return _count == N; }

    void clear() noexcept { _count = 0; }

private:
    container_type _data{};
    size_t _count = 0;
};

} // namespace kimix
