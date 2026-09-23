#pragma once

#include <filesystem>
#include "string.h"
#include "../dll_export.h"

namespace kimix {

namespace filesystem = std::filesystem;

// Convert a filesystem path to a kimix::string
KIMIX_CORE_API string to_string(const filesystem::path& path);

// Build a filesystem::path from narrow bytes without ever throwing.
//
// std::filesystem::path's narrow constructor converts the bytes to the native
// encoding on Windows and throws std::system_error when they cannot be
// represented ("No mapping for the Unicode character exists in the target
// multi-byte code page").  kimix is compiled without C++ exceptions
// (kimix_enable_exception=false), where that failure would terminate the
// process, so this helper reports the condition through its return value:
//
//   * returns true and fills `out` when the bytes are representable;
//   * returns false and leaves `out` cleared when they are not - callers treat
//     such a path as "does not exist" instead of as a hard error.
//
// On POSIX the bytes are stored verbatim, so the conversion cannot fail.
KIMIX_CORE_API bool path_from_narrow(string_view text,
                                     filesystem::path& out) noexcept;

} // namespace kimix
