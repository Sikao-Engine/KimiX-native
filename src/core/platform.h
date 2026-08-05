#pragma once

#include "dll_export.h"
#include "stl/filesystem.h"
#include "stl/string.h"
#include "stl/vector.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kimix {

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------

#if defined(_WIN32) || defined(_WIN64)
    #ifndef KIMIX_PLATFORM_WINDOWS
        #define KIMIX_PLATFORM_WINDOWS
    #endif
#elif defined(__APPLE__)
    #ifndef KIMIX_PLATFORM_APPLE
        #define KIMIX_PLATFORM_APPLE
    #endif
    #ifndef KIMIX_PLATFORM_UNIX
        #define KIMIX_PLATFORM_UNIX
    #endif
#elif defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    #ifndef KIMIX_PLATFORM_UNIX
        #define KIMIX_PLATFORM_UNIX
    #endif
#endif

// ---------------------------------------------------------------------------
// Aligned memory allocation (platform-specific)
// ---------------------------------------------------------------------------

KIMIX_CORE_API void* aligned_alloc(size_t alignment, size_t size) noexcept;
KIMIX_CORE_API void aligned_free(void* ptr) noexcept;
KIMIX_CORE_API size_t pagesize() noexcept;

// ---------------------------------------------------------------------------
// Dynamic module loading
// ---------------------------------------------------------------------------

KIMIX_CORE_API void* dynamic_module_load(const filesystem::path& path) noexcept;
KIMIX_CORE_API void dynamic_module_destroy(void* handle) noexcept;
KIMIX_CORE_API void* dynamic_module_find_symbol(void* handle, const char* name) noexcept;
KIMIX_CORE_API string dynamic_module_name(string_view name) noexcept;

// ---------------------------------------------------------------------------
// Stack trace
// ---------------------------------------------------------------------------

struct TraceItem {
    string module;
    string function;
    string file;
    uint32_t line = 0;
};

KIMIX_CORE_API vector<TraceItem> backtrace();

// ---------------------------------------------------------------------------
// System information
// ---------------------------------------------------------------------------

KIMIX_CORE_API string cpu_name() noexcept;
KIMIX_CORE_API string current_executable_path() noexcept;
KIMIX_CORE_API char env_separator() noexcept;

// ---------------------------------------------------------------------------
// Debug break
// ---------------------------------------------------------------------------

inline void debug_break() {
#if defined(KIMIX_PLATFORM_WINDOWS)
    __debugbreak();
#elif defined(__has_builtin)
    #if __has_builtin(__builtin_debugtrap)
    __builtin_debugtrap();
    #else
    __builtin_trap();
    #endif
#else
    __builtin_trap();
#endif
}

} // namespace kimix
