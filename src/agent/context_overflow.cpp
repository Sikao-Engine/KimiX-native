// agent/context_overflow.cpp - Overflow detection implementation
// (see context_overflow.h). Port of context_overflow.py:18-55.

#include "agent/context_overflow.h"

namespace kimix::agent {

namespace {

// ASCII lower-casing containment check (the markers are pure ASCII, so a
// locale-free fold is enough; the reference lowercases the whole message with
// str.lower() and does substring matching).
bool contains_ascii_ci(kimix::string_view haystack,
                       kimix::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            ++j;
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_context_overflow_error(kimix::string_view message,
                               int32_t status) noexcept {
    // classify_api_error precedence: auth (401/403) and rate_limit (429) win
    // over overflow; anything outside [400, 500) is not an API 4xx.
    if (status < 400 || status >= 500) {
        return false;
    }
    if (status == 401 || status == 403 || status == 429) {
        return false;
    }
    for (kimix::string_view marker : k_context_overflow_markers) {
        if (contains_ascii_ci(message, marker)) {
            return true;
        }
    }
    return false;
}

} // namespace kimix::agent
