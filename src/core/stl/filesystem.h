#pragma once

#include <filesystem>
#include "string.h"
#include "../dll_export.h"

namespace kimix {

namespace filesystem = std::filesystem;

// Convert a filesystem path to a kimix::string
KIMIX_CORE_API string to_string(const filesystem::path& path);

} // namespace kimix
