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
    // The exe path is user-machine input: its narrow bytes may be
    // unrepresentable in the ANSI code page, which would make the path
    // narrow constructor terminate the process - treat that as absent.
    kimix::filesystem::path exe_path_obj;
    if (!kimix::path_from_narrow(exe_path, exe_path_obj)) { return; }
    const auto exe_dir_path = exe_path_obj.parent_path();
    DynamicModule::add_search_path(kimix::to_string(exe_dir_path));

    // Also add common subdirectories
    const auto bin_dir = exe_dir_path / "bin";
    std::error_code exists_ec;
    if (kimix::filesystem::exists(bin_dir, exists_ec) && !exists_ec) {
        DynamicModule::add_search_path(kimix::to_string(bin_dir));
    }

    // Use AddDllDirectory for extended search on Windows
    for (const auto &p : DynamicModule::get_search_paths()) {
        // Widen with CP_ACP: a per-byte wchar_t widening mangles any
        // non-ASCII directory and makes AddDllDirectory silently fail.
        const int needed = MultiByteToWideChar(CP_ACP, 0, p.data(),
                                               static_cast<int>(p.size()), nullptr, 0);
        if (needed <= 0) { continue; }
        std::wstring wide_path(static_cast<size_t>(needed), L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, p.data(), static_cast<int>(p.size()),
                                wide_path.data(), needed) != needed) {
            continue;
        }
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
    // folder/name can carry caller-supplied bytes (config paths, user dirs):
    // build the path without throwing; unrepresentable bytes mean the
    // module simply cannot be addressed, so report a failed load.
    kimix::filesystem::path full_path;
    kimix::filesystem::path name_path;
    if (!kimix::path_from_narrow(folder, full_path) ||
        !kimix::path_from_narrow(name, name_path)) {
        std::fprintf(stderr, "[kimix][warning] Failed to load dynamic module '%.*s%.*s' after %.2f ms. (%s:%d)\n",
                     static_cast<int>(folder.size()), folder.data(),
                     static_cast<int>(name.size()), name.data(),
                     clock.toc(), __FILE__, __LINE__);
        return false;
    }
    full_path /= name_path;
    auto *handle = dynamic_module_load(full_path);
    if (handle) {
        std::fprintf(stderr, "[kimix][info] Loaded dynamic module '%s' in %.2f ms.\n",
                     kimix::to_string(full_path).c_str(), clock.toc());
        if (out_handle) { *out_handle = handle; }
        return true;
    }
    std::fprintf(stderr, "[kimix][warning] Failed to load dynamic module '%s' after %.2f ms. (%s:%d)\n",
                 kimix::to_string(full_path).c_str(), clock.toc(), __FILE__, __LINE__);
    return false;
}

} // namespace kimix
