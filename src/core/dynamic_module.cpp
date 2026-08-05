#include <core/dynamic_module.h>
#include <core/platform.h>
#include <core/clock.h>
#include <core/stl/filesystem.h>
#include <core/stl/format.h>

#include <cstdio>

#ifdef KIMIX_PLATFORM_WINDOWS
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// DynamicModule non-inline helpers
// ---------------------------------------------------------------------------

// Update search paths from the process environment
void dynamic_module_update_search_paths() noexcept {
#ifdef KIMIX_PLATFORM_WINDOWS
    // Add the executable directory to the DLL search path
    auto exe_path = current_executable_path();
    auto exe_dir = kimix::filesystem::path(exe_path).parent_path().string();
    DynamicModule::add_search_path(exe_dir);

    // Also add common subdirectories
    auto bin_dir = kimix::filesystem::path(exe_dir) / "bin";
    if (kimix::filesystem::exists(bin_dir)) {
        DynamicModule::add_search_path(bin_dir.string());
    }

    // Use AddDllDirectory for extended search on Windows
    for (const auto &p : DynamicModule::get_search_paths()) {
        auto wide_path = std::wstring(p.begin(), p.end());
        AddDllDirectory(wide_path.c_str());
    }
#else
    // On Unix, the search paths are managed via LD_LIBRARY_PATH or rpath
    // The dynamic linker handles this automatically.
    auto exe_path = current_executable_path();
    auto exe_dir = kimix::filesystem::path(exe_path).parent_path().string();
    DynamicModule::add_search_path(exe_dir);
#endif
}

// Remove all DLL directories (Windows-specific cleanup)
void dynamic_module_clear_search_paths() noexcept {
#ifdef KIMIX_PLATFORM_WINDOWS
    // Windows manages DLL directories per-process; we track them in DynamicModule
#endif
    DynamicModule::reset_search_paths();
}

// Factory: load a module with full error reporting
bool dynamic_module_load_with_log(string_view folder, string_view name, void **out_handle) noexcept {
    kimix::Clock clock;
    kimix::filesystem::path full_path = kimix::filesystem::path(folder) / name;
    auto *handle = dynamic_module_load(full_path);
    if (handle) {
        std::fprintf(stderr, "[kimix][info] Loaded dynamic module '%s' in %.2f ms.\n",
                     full_path.string().c_str(), clock.toc());
        if (out_handle) { *out_handle = handle; }
        return true;
    }
    std::fprintf(stderr, "[kimix][warning] Failed to load dynamic module '%s' after %.2f ms. (%s:%d)\n",
                 full_path.string().c_str(), clock.toc(), __FILE__, __LINE__);
    return false;
}

} // namespace kimix
