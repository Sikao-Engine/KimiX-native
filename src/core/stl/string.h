/*
 * string.h — kimix::string and related string types (mimalloc-backed).
 *
 * kimix::string is std::basic_string<char, ..., kimix::allocator<char>>.
 * Other types: kimix::u8string, u16string, u32string, wstring.
 * String-view types: kimix::string_view (alias for std::string_view).
 *
 * Construction:
 *   default, from literal, from std::string, copy, move
 *
 * Operations:
 *   append(), operator+=, push_back()
 *   substr(pos, count)
 *   find(needle) — returns position or kimix::string::npos
 *   Comparison: ==, !=, <, >, <=, >=
 *   c_str() — null-terminated C string
 *   Conversion to/from std::string via constructor
 *
 * String hashing:
 *   kimix::string_hash, kimix::basic_string_hash<Char>
 *   kimix::hash specializations for char*, const char*, char[N],
 *   std::string, std::string_view, and kimix::string variants.
 *
 * Type traits:
 *   kimix::is_char<T>, kimix::is_char_v<T>
 *
 * Example:
 *   kimix::string s = "hello";
 *   s += " world";
 *   auto pos = s.find("world");  // 6
 *   kimix::string sub = s.substr(0, 5);  // "hello"
 */
#pragma once

#include "memory.h"
#include "hash_fwd.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <cstdint>

namespace kimix {

// ---------------------------------------------------------------------------
// Basic string types with mimalloc allocator
// ---------------------------------------------------------------------------

using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;
using u8string = std::basic_string<char8_t, std::char_traits<char8_t>, allocator<char8_t>>;
using u16string = std::basic_string<char16_t, std::char_traits<char16_t>, allocator<char16_t>>;
using u32string = std::basic_string<char32_t, std::char_traits<char32_t>, allocator<char32_t>>;
using wstring = std::basic_string<wchar_t, std::char_traits<wchar_t>, allocator<wchar_t>>;

// ---------------------------------------------------------------------------
// String view types (no allocator)
// ---------------------------------------------------------------------------

using string_view = std::string_view;
using u8string_view = std::u8string_view;
using u16string_view = std::u16string_view;
using u32string_view = std::u32string_view;
using wstring_view = std::wstring_view;

// ---------------------------------------------------------------------------
// Basic string hash
// ---------------------------------------------------------------------------

template <typename Char, typename CharTraits = std::char_traits<Char>>
struct basic_string_hash {
    uint64_t operator()(std::basic_string_view<Char, CharTraits> s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s.data(), s.size() * sizeof(Char), seed);
    }
    uint64_t operator()(const Char* s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s, CharTraits::length(s) * sizeof(Char), seed);
    }
};

using string_hash = basic_string_hash<char>;
using u8string_hash = basic_string_hash<char8_t>;
using u16string_hash = basic_string_hash<char16_t>;
using u32string_hash = basic_string_hash<char32_t>;
using wstring_hash = basic_string_hash<wchar_t>;

// ---------------------------------------------------------------------------
// is_char type traits
// ---------------------------------------------------------------------------

template <typename T>
struct is_char : std::false_type {};

template <> struct is_char<char> : std::true_type {};
template <> struct is_char<wchar_t> : std::true_type {};
template <> struct is_char<char8_t> : std::true_type {};
template <> struct is_char<char16_t> : std::true_type {};
template <> struct is_char<char32_t> : std::true_type {};

template <typename T>
inline constexpr bool is_char_v = is_char<T>::value;

// ---------------------------------------------------------------------------
// hash specializations for strings
// ---------------------------------------------------------------------------

template <>
struct hash<char*> {
    uint64_t operator()(const char* s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s, std::char_traits<char>::length(s), seed);
    }
};

template <>
struct hash<const char*> {
    uint64_t operator()(const char* s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s, std::char_traits<char>::length(s), seed);
    }
};

template <size_t N>
struct hash<char[N]> {
    uint64_t operator()(const char (&s)[N], uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s, N - 1, seed);
    }
};

template <>
struct hash<std::string> {
    uint64_t operator()(const std::string& s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s.data(), s.size(), seed);
    }
};

template <>
struct hash<std::string_view> {
    uint64_t operator()(const std::string_view& s, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(s.data(), s.size(), seed);
    }
};

} // namespace kimix
