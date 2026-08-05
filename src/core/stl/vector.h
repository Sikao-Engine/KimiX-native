/*
 * vector.h — kimix::vector<T> (mimalloc-backed std::vector).
 *
 * kimix::vector<T> is std::vector<T, kimix::allocator<T>>.
 *
 * Operations:
 *   push_back(), emplace_back()
 *   resize(n, val), reserve(n)
 *   size(), capacity(), empty()
 *   operator[] for element access
 *   begin()/end() for iteration (range-for compatible)
 *   Copy construction and assignment
 *   Move construction and assignment
 *   Initializer-list construction: kimix::vector<int> v = {1,2,3};
 *
 * Helper functions:
 *   kimix::enlarge_by(vec, n)     — resize(vec.size() + n)
 *   kimix::size_bytes(vec)        — vec.size() * sizeof(T)
 *   kimix::vector_resize(vec, n)  — vec.resize(n)
 *
 * Other vector types:
 *   kimix::bitvector — alias for std::vector<bool>
 *   kimix::fixed_vector<T, N> — alias for kimix::vector<T>
 *
 * Example:
 *   kimix::vector<int> v;
 *   v.push_back(10);
 *   v.push_back(20);
 *   for (auto& x : v) { /* ... *\/ }
 */
#pragma once

#include "memory.h"

#include <vector>
#include <cstddef>
#include <initializer_list>

namespace kimix {

// ---------------------------------------------------------------------------
// Standard vector with mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T>
using vector = std::vector<T, allocator<T>>;

// ---------------------------------------------------------------------------
// bitvector — vector<bool> with default allocator (specialized)
// ---------------------------------------------------------------------------

using bitvector = std::vector<bool>;

// ---------------------------------------------------------------------------
// fixed_vector — aliased to vector (no fixed-size optimization)
// ---------------------------------------------------------------------------

template <typename T, size_t N>
using fixed_vector = vector<T>;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

template <typename T>
inline void enlarge_by(vector<T>& vec, size_t size) {
    vec.resize(vec.size() + size);
}

template <typename T>
inline size_t size_bytes(const vector<T>& vec) {
    return vec.size() * sizeof(T);
}

template <typename T>
inline void vector_resize(vector<T>& vec, size_t size) {
    vec.resize(size);
}

} // namespace kimix
