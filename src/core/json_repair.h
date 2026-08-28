/*
 * json_repair.h -- repair malformed JSON.
 *
 * kimix::repair(json):
 *   - returns the empty string when the input is already valid JSON
 *   - otherwise returns the repaired, strictly valid JSON string
 *
 * See json_repair.cpp for the list of handled malformations.
 */
#pragma once
#include <core/stl/string.h>
namespace kimix {
string repair(string_view json);
} // namespace kimix
