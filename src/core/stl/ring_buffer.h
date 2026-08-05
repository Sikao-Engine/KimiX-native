#pragma once

#include "vector.h"
#include <cstddef>
#include <optional>

namespace kimix {

// ---------------------------------------------------------------------------
// Simple ring buffer (circular buffer) backed by kimix::vector
// ---------------------------------------------------------------------------

template <typename T>
class ring_buffer {
public:
    explicit ring_buffer(size_t capacity) : _data(capacity), _capacity(capacity) {}

    void push(const T& value) {
        if (_count < _capacity) {
            _data[_tail] = value;
            _tail = (_tail + 1) % _capacity;
            ++_count;
        } else {
            _data[_head] = value;
            _head = (_head + 1) % _capacity;
            _tail = _head;
        }
    }

    void push(T&& value) {
        if (_count < _capacity) {
            _data[_tail] = std::move(value);
            _tail = (_tail + 1) % _capacity;
            ++_count;
        } else {
            _data[_head] = std::move(value);
            _head = (_head + 1) % _capacity;
            _tail = _head;
        }
    }

    std::optional<T> pop() {
        if (_count == 0) { return std::nullopt; }
        T value = std::move(_data[_head]);
        _head = (_head + 1) % _capacity;
        --_count;
        return value;
    }

    T& front() { return _data[_head]; }
    const T& front() const { return _data[_head]; }

    T& back() {
        size_t idx = (_tail == 0) ? _capacity - 1 : _tail - 1;
        return _data[idx];
    }

    const T& back() const {
        size_t idx = (_tail == 0) ? _capacity - 1 : _tail - 1;
        return _data[idx];
    }

    size_t size() const noexcept { return _count; }
    size_t capacity() const noexcept { return _capacity; }
    bool empty() const noexcept { return _count == 0; }
    bool full() const noexcept { return _count == _capacity; }

    void clear() noexcept { _count = 0; _head = 0; _tail = 0; }

private:
    vector<T> _data;
    size_t _capacity;
    size_t _head = 0;
    size_t _tail = 0;
    size_t _count = 0;
};

} // namespace kimix
