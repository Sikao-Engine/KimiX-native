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
    uint64_t address;
    string symbol;
    size_t offset;
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

namespace platform {

// ---------------------------------------------------------------------------
// Narrow <-> wide string conversion primitives over the ANSI code page
// (CP_ACP). Windows-only; POSIX builds keep paths as plain byte strings and
// the owning helpers below never call these.
//
// These primitives NEVER allocate: they only measure or fill caller-provided
// buffers, which makes them safe to export from kimix-core even for modules
// that link a different allocator instance (see the note on the owning
// helpers below).
//
// narrow_to_wide_acp:
//   Query:  dst == nullptr -> returns the required wchar_t capacity, or 0
//            when `text` is empty or is not representable in the ANSI code
//            page (MB_ERR_INVALID_CHARS).
//   Fill:   writes up to `capacity` wchar_t (including the null terminator
//            when it fits) and returns the number of wchar_t written.
// wide_to_narrow_acp:
//   Same contract over bytes. `strict` adds WC_ERR_INVALID_CHARS |
//   WC_NO_BEST_FIT_CHARS so an unrepresentable character fails (0 for
//   non-empty input) instead of becoming the default replacement character.
//
// For BOTH primitives: whether the returned count includes the null
// terminator varies between Windows builds (documented as included;
// observed as excluded), so callers should size the buffer with the query
// result and strip trailing nulls from the string instead of trusting the
// count.
// ---------------------------------------------------------------------------
#if defined(KIMIX_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
[[nodiscard]] KIMIX_CORE_API size_t narrow_to_wide_acp(string_view text, wchar_t *dst, size_t capacity) noexcept;
[[nodiscard]] KIMIX_CORE_API size_t wide_to_narrow_acp(wstring_view text, char *dst, size_t capacity, bool strict) noexcept;
#endif

// Owning path <-> narrow-string conversions with the same semantics as
// kimix::path_from_narrow() and kimix::to_string(const filesystem::path&):
// the ANSI code page, never throwing (empty or undecodable input becomes an
// empty result), and path_to_narrow() degrades lossily (replacement
// characters) when the strict conversion fails.
//
// THESE HELPERS ARE INLINE ON PURPOSE. Standalone kimix-core is a static
// archive whose objects are linked into several modules (kimix-llm, kimix-cli,
// runtime_py, kimix_api, the test binaries), and each standalone module has its
// own private mimalloc instance. An owning
// std::filesystem::path / kimix::string must be allocated AND freed inside
// the same module: a core-exported factory handing such an object to another
// module transfers a block across allocator instances and can crash at
// destruction (the failure luisa-compute observed as an access violation
// inside rpmalloc's free on the CUDA backend's first shader-cache lookup,
// fixed upstream in commit d80d45d). Being inline gives every caller module
// its own copy; the allocation-free primitives above do the heavy lifting
// and are the only part that crosses the module boundary.
[[nodiscard]] inline filesystem::path path_from_narrow(string_view text) noexcept {
    filesystem::path result;
#if defined(KIMIX_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (const size_t needed = narrow_to_wide_acp(text, nullptr, 0u); needed > 1u) {
        wstring wide(needed, L'\0');
        if (narrow_to_wide_acp(text, &wide[0], needed) != 0u) {
            while (!wide.empty() && wide.back() == L'\0') { wide.pop_back(); }
            result = filesystem::path{wide};
        }
    }
#else
    // POSIX paths are plain byte strings; construction cannot fail.
    result = filesystem::path{text};
#endif
    return result;
}

[[nodiscard]] inline string path_to_narrow(const filesystem::path &path) noexcept {
#if defined(KIMIX_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    const std::wstring_view wide{path.native()};
    if (wide.empty()) { return {}; }
    // Strict CP_ACP first (byte-identical to path.string<char>() for every
    // representable path); lossy CP_ACP second, mirroring how the rest of
    // Windows resolves unrepresentable names.
    for (const bool strict : {true, false}) {
        if (const size_t needed = wide_to_narrow_acp(wide, nullptr, 0u, strict); needed > 1u) {
            string out(needed, '\0');
            if (wide_to_narrow_acp(wide, &out[0], needed, strict) != 0u) {
                while (!out.empty() && out.back() == '\0') { out.pop_back(); }
                return out;
            }
        }
    }
    return {};
#else
    return string{path.native()};
#endif
}

}// namespace platform

} // namespace kimix
