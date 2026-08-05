#include <core/stl/filesystem.h>

namespace kimix {

kimix::string to_string(const kimix::filesystem::path &path) {
    return path.string<char, std::char_traits<char>, kimix::allocator<char>>();
}

} // namespace kimix
