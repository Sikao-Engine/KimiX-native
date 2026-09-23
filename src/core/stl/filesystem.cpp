#include <core/stl/filesystem.h>

#include <climits>
#include <string>

#if defined(KIMIX_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif

namespace kimix {

kimix::string to_string(const kimix::filesystem::path &path) {
    return path.string<char, std::char_traits<char>, kimix::allocator<char>>();
}

bool path_from_narrow(kimix::string_view text,
                      kimix::filesystem::path &out) noexcept {
    out.clear();
#if defined(KIMIX_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const int len = static_cast<int>(text.size());
    // CP_ACP + MB_ERR_INVALID_CHARS mirrors what the STL's narrow path
    // conversion does: a byte that cannot be represented makes the conversion
    // fail instead of being silently replaced with a placeholder.
    const int needed = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                                             text.data(), len, nullptr, 0);
    if (needed <= 0) {
        // MultiByteToWideChar reports 0 both for an empty input (which is a
        // valid empty path) and for invalid input; only the latter is an error.
        return len == 0;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.data(), len,
                              wide.data(), needed) != needed) {
        return false;
    }
    // The wide constructor stores the native string as-is: no conversion, so
    // it cannot fail.
    out = kimix::filesystem::path(wide);
    return true;
#else
    // POSIX paths are plain byte strings; construction cannot fail.
    out = kimix::filesystem::path(kimix::string(text));
    return true;
#endif
}

} // namespace kimix
