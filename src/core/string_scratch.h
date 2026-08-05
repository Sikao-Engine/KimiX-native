/*
 * string_scratch.h — kimix::StringScratch efficient string builder.
 *
 * A fast string builder with operator<< for various types.
 * Default initial capacity is 512 bytes; can be set via explicit constructor.
 *
 * Supported argument types:
 *   int, unsigned, long, long long, unsigned long long (covers size_t)
 *   float, double, bool
 *   char, const char*, kimix::string, kimix::string_view
 *
 * Methods:
 *   string()       — returns const kimix::string& (built content)
 *   string_view()  — returns kimix::string_view of content
 *   c_str()        — returns null-terminated const char*
 *   clear()        — reset buffer
 *   reserve(n)     — pre-allocate capacity
 *   size()         — current content length
 *   empty()        — true if empty
 *   <<             — append (chainable)
 *
 * Example:
 *   kimix::StringScratch ss;
 *   ss << "hello " << 42 << " " << true;
 *   // ss.string_view() == "hello 42 true"
 */
#pragma once

#include "stl/string.h"
#include "stl/sstream.h"

#include <string>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <string_view>

namespace kimix {

// ---------------------------------------------------------------------------
// StringScratch — efficient string builder
// ---------------------------------------------------------------------------

class StringScratch {
public:
    StringScratch() {
        _buffer.reserve(512);
    }

    explicit StringScratch(size_t initial_capacity) {
        _buffer.reserve(initial_capacity);
    }

    // Append a string_view
    // NOTE: types are spelled kimix::-qualified throughout this class because the
    // string()/string_view() accessors below would otherwise change the meaning
    // of the unqualified type names (gcc/clang reject that; MSVC accepts it).
    StringScratch& operator<<(kimix::string_view sv) {
        _buffer.append(sv);
        return *this;
    }

    // Append a string
    StringScratch& operator<<(const kimix::string& s) {
        _buffer.append(s);
        return *this;
    }

    // Append a C string
    StringScratch& operator<<(const char* s) {
        _buffer.append(s);
        return *this;
    }

    // Append a char
    StringScratch& operator<<(char c) {
        _buffer.push_back(c);
        return *this;
    }

    // Append an int
    StringScratch& operator<<(int v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append an unsigned int
    StringScratch& operator<<(unsigned int v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append a long
    StringScratch& operator<<(long v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append an unsigned long (covers size_t on Linux, where
    // size_t is unsigned long rather than unsigned long long)
    StringScratch& operator<<(unsigned long v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append a long long
    StringScratch& operator<<(long long v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append an unsigned long long (also covers size_t on 64-bit)
    StringScratch& operator<<(unsigned long long v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append a float
    StringScratch& operator<<(float v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append a double
    StringScratch& operator<<(double v) {
        _buffer += std::to_string(v);
        return *this;
    }

    // Append a bool
    StringScratch& operator<<(bool v) {
        _buffer.append(v ? "true" : "false");
        return *this;
    }

    // Get the built string
    const kimix::string& string() const noexcept { return _buffer; }
    kimix::string_view string_view() const noexcept { return _buffer; }
    const char* c_str() const noexcept { return _buffer.c_str(); }

    // Clear the buffer
    void clear() noexcept { _buffer.clear(); }

    // Reserve capacity
    void reserve(size_t capacity) { _buffer.reserve(capacity); }

    // Access the underlying buffer
    size_t size() const noexcept { return _buffer.size(); }
    bool empty() const noexcept { return _buffer.empty(); }

private:
    kimix::string _buffer;
};

} // namespace kimix
