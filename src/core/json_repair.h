/*
 * json_repair.h -- repair malformed JSON.
 *
 * kimix::repair(json):
 *   - returns an empty vector when the input is already valid JSON
 *   - otherwise returns the repaired, strictly valid JSON as a byte buffer.
 *     When the result is non-empty its last element is the NUL terminator
 *     ('\0'), so data() can be passed to C-string APIs; the JSON text itself
 *     is the first size() - 1 bytes (see repaired_view() for a convenient
 *     string_view without the terminator).
 *
 * See json_repair.cpp for the list of handled malformations.
 */
#pragma once
#include <core/stl/string.h>
#include <core/stl/vector.h>
namespace kimix {
vector<char> repair(string_view json);

// View of a non-empty repair() result excluding its trailing '\0'.
// Returns the empty view when `repaired` is empty.
inline string_view repaired_view(const vector<char> &repaired) noexcept {
    return repaired.empty()
               ? string_view{}
               : string_view{repaired.data(), repaired.size() - 1};
}
} // namespace kimix
