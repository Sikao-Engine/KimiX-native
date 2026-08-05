#include <core/clock.h>
#include <core/platform.h>
#include <core/logging.h>
#include <core/stl/string.h>
#include <core/stl/filesystem.h>

static_assert(sizeof(void *) == 8 && sizeof(int) == 4 && sizeof(char) == 1,
              "illegal pointer and integer sizes.");

#ifdef KIMIX_PLATFORM_WINDOWS

#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <windows.h>
#include <intrin.h>

namespace kimix {

void *aligned_alloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void aligned_free(void *p) noexcept {
    _aligned_free(p);
}

size_t pagesize() noexcept {
    static thread_local auto page_size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
    }();
    return page_size;
}

// Win32 last error helper
namespace detail {
[[nodiscard]] kimix::string win32_last_error_message() {
    void *buffer = nullptr;
    auto err_code = GetLastError();
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer, 0, nullptr);
    auto value = static_cast<char *>(buffer);
    kimix::string err_msg{value};
    LocalFree(buffer);
    return err_msg;
}
} // namespace detail

void *dynamic_module_load(const kimix::filesystem::path &path) noexcept {
    auto path_string = path.string();
    auto module = LoadLibraryA(path_string.c_str());
    if (module == nullptr) [[unlikely]] {
        KIMIX_WARNING_WITH_LOCATION(
            "Failed to load dynamic module '{}', reason: {}.",
            path_string, detail::win32_last_error_message());
    }
    return module;
}

void dynamic_module_destroy(void *handle) noexcept {
    if (handle != nullptr) { FreeLibrary(reinterpret_cast<HMODULE>(handle)); }
}

void *dynamic_module_find_symbol(void *handle, const char *name) noexcept {
    auto symbol = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    if (symbol == nullptr) [[unlikely]] {
        KIMIX_WARNING("Failed to load symbol '{}'.", name);
    }
    return reinterpret_cast<void *>(symbol);
}

kimix::string dynamic_module_name(kimix::string_view name) noexcept {
    kimix::string s{name};
    s.append(".dll");
    return s;
}

kimix::string cpu_name() noexcept {
    int32_t brand[12];
    __cpuid(&brand[0], static_cast<int>(0x80000002u));
    __cpuid(&brand[4], static_cast<int>(0x80000003u));
    __cpuid(&brand[8], static_cast<int>(0x80000004u));
    return reinterpret_cast<const char *>(brand);
}

kimix::string current_executable_path() noexcept {
    constexpr auto max_path_length = 4096;
    wchar_t path[max_path_length] = {};
    auto nchar = GetModuleFileNameW(nullptr, path, max_path_length);
    if (nchar == 0 || (nchar == max_path_length && GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
        KIMIX_ERROR_WITH_LOCATION("Failed to get current executable path.");
    }
    // Convert wide to narrow
    auto wstr = std::wstring_view(path, nchar);
    kimix::string result(wstr.begin(), wstr.end());
    return result;
}

kimix::vector<TraceItem> backtrace() {
    // TODO: implement stack trace on Windows using CaptureStackBackTrace / DbgHelp
    return {};
}

char env_separator() noexcept {
    return ';';
}

} // namespace kimix

#else
// Unix fallback stubs
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>

namespace kimix {

void *aligned_alloc(size_t alignment, size_t size) noexcept {
    return ::aligned_alloc(alignment, size);
}
void aligned_free(void *p) noexcept { free(p); }

size_t pagesize() noexcept {
    static thread_local auto page_size = sysconf(_SC_PAGESIZE);
    return static_cast<size_t>(page_size);
}

void *dynamic_module_load(const kimix::filesystem::path &path) noexcept {
    auto p = path.string();
    return dlopen(p.c_str(), RTLD_LAZY);
}

void dynamic_module_destroy(void *handle) noexcept {
    if (handle) dlclose(handle);
}

void *dynamic_module_find_symbol(void *handle, const char *name) noexcept {
    return dlsym(handle, name);
}

kimix::string dynamic_module_name(kimix::string_view name) noexcept {
    kimix::string s{"lib"};
    s.append(name).append(".so");
    return s;
}

kimix::string cpu_name() noexcept {
    return "Unknown CPU";
}

kimix::string current_executable_path() noexcept {
    char buf[4096] = {};
    auto len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) return kimix::string(buf, static_cast<size_t>(len));
    return "";
}

kimix::vector<TraceItem> backtrace() {
    // TODO: implement backtrace on Unix using backtrace() / dladdr
    return {};
}

char env_separator() noexcept { return ':'; }

} // namespace kimix
#endif
