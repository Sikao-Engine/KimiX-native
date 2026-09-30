#include <core/clock.h>
#include <core/platform.h>
#include <core/stl/string.h>
#include <core/stl/filesystem.h>

#include <cstdio>

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
#include <csignal>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")
#include <DbgHelp.h>

#ifdef KIMIX_DISABLE_WIN_MESSAGE_BOX
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace kimix {

void *aligned_alloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void aligned_free(void *p) noexcept {
    _aligned_free(p);
}

#ifdef KIMIX_DISABLE_WIN_MESSAGE_BOX
// Disable-message-box design (ported from LuisaCompute's platform.cpp): a
// static initializer that redirects CRT asserts/runtime errors and Win32
// critical-error dialogs to stderr, so headless runs (tests, CI, agent
// subprocesses) never hang on a hidden pop-up message box. Enabled via the
// kimix_disable_win_message_box xmake option (default on), which defines
// KIMIX_DISABLE_WIN_MESSAGE_BOX on the kimix-core target.
struct DisableMessageBoxInit {
    DisableMessageBoxInit() noexcept {
#ifndef NDEBUG
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
        _set_error_mode(_OUT_TO_STDERR);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
} disable_message_box;
#endif

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
        std::fprintf(stderr, "[kimix][warning] Failed to load dynamic module '%s', reason: %s (%s:%d)\n",
                     path_string.c_str(), detail::win32_last_error_message().c_str(), __FILE__, __LINE__);
    }
    return module;
}

void dynamic_module_destroy(void *handle) noexcept {
    if (handle != nullptr) { FreeLibrary(reinterpret_cast<HMODULE>(handle)); }
}

void *dynamic_module_find_symbol(void *handle, const char *name) noexcept {
    auto symbol = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    if (symbol == nullptr) [[unlikely]] {
        std::fprintf(stderr, "[kimix][warning] Failed to load symbol '%s'.\n", name);
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
        std::fprintf(stderr, "[kimix][error] Failed to get current executable path. (%s:%d)\n", __FILE__, __LINE__);
    }
    // Convert wide to narrow
    auto wstr = std::wstring_view(path, nchar);
    kimix::string result(wstr.begin(), wstr.end());
    return result;
}

kimix::vector<TraceItem> backtrace() {
    void *stack[100];
    auto process = GetCurrentProcess();
    static bool sym_initialized = false;
    if (!sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        sym_initialized = SymInitialize(process, nullptr, TRUE);
    }
    auto frame_count = CaptureStackBackTrace(0, 100, stack, nullptr);

    struct Symbol : SYMBOL_INFO {
        char name_storage[1023];
    } symbol{};
    symbol.MaxNameLen = 1024;
    symbol.SizeOfStruct = sizeof(SYMBOL_INFO);
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
    kimix::vector<TraceItem> trace;
    trace.reserve(frame_count - 1u);
    for (auto i = 1u; i < frame_count; i++) {
        auto address = reinterpret_cast<uint64_t>(stack[i]);
        auto displacement = 0ull;
        if (SymFromAddr(process, address, &displacement, &symbol)) {
            TraceItem item{};
            if (SymGetModuleInfo64(process, symbol.ModBase, &module)) {
                item.module = module.ModuleName;
            } else {
                item.module = "???";
            }
            item.symbol = symbol.Name;
            item.address = address;
            item.offset = displacement;
            trace.emplace_back(std::move(item));
        } else {
            std::cerr << kimix::format("Failed to get stacktrace at 0x{:012}: {}\n", address, detail::win32_last_error_message());
        }
    }
    return trace;
}

namespace platform_detail {

void print_stack_trace() {
    auto trace = backtrace();
    std::cerr << "----- Stack Trace (" << trace.size() << " frames) -----\n";
    for (size_t i = 0; i < trace.size(); ++i) {
        std::cerr << "  [" << std::setw(2) << i << "] ";
        if (!trace[i].symbol.empty()) {
            std::cerr << trace[i].symbol;
        } else {
            std::cerr << "0x" << std::hex << trace[i].address << std::dec;
        }
        if (!trace[i].module.empty() && trace[i].module != "???") {
            std::cerr << "  at " << trace[i].module << "+0x" << std::hex << trace[i].offset << std::dec;
        }
        std::cerr << "\n";
    }
    std::cerr << "----- End Stack Trace -----\n";
}

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS * /*exc*/) {
    std::cerr << "!!! Unhandled structured exception !!!\n";
    print_stack_trace();
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
    std::cerr << "!!! std::terminate called (uncaught exception) !!!\n";
    print_stack_trace();
    ExitProcess(1);
}

void OnSigAbort(int) {
    std::cerr << "!!! SIGABRT / std::abort() called !!!\n";
    print_stack_trace();
    _exit(3);
}

struct StackTracerInit {
    StackTracerInit() noexcept {
        SetUnhandledExceptionFilter(UnhandledExceptionFilter);
        std::set_terminate(OnTerminate);
        std::signal(SIGABRT, OnSigAbort);
    }
} stack_tracer_init;

}// namespace platform_detail


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
