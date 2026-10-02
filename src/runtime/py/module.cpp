// module.cpp -- Python bindings for the Kimix runtime (module: runtime_py).
//
// Thin pybind11 wrapper around the runtime C-FFI / C++ API. The runtime
// kernels in src/runtime/<domain>/ are compiled into the same module (no
// separate runtime.dll). Built with no unity build so the TU containing
// PYBIND11_MODULE stays isolated from Python.h.
//
// GIL policy: every kernel call made from this file releases the GIL via
// kimix::runtime::common::gil_scoped_release (see common/gil.h). Kernels
// below the binding layer never touch Python.
//
// This is the ONLY file containing PYBIND11_MODULE (one TU per extension).
//
// Fiber policy: this module is the "root main" of the Python host. Runtime
// kernels compiled into it (the grep engine's chunk fan-out, via kimix-llm)
// submit fiber work over whatever pool the CALLING thread is bound to and
// never create one themselves - so the module init binds the importing
// thread (in practice Python's main thread) to the process-wide shared pool
// for as long as the extension is loaded. Without this binding every Python
// call would fall back to serial inline scans. The shared pool is
// intentionally never destroyed (a .pyd must not hang on DLL unload), so the
// binding guard's destructor only UNBINDS the thread. Kernel calls from
// OTHER Python threads were never bound: they keep the graceful inline
// fallback (blocking parallel() forms) or schedule_background()'s transient
// self-binding.

#include <pybind11/pybind11.h>

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>      // fiber_binding_guard below

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/common/utf8.h>

#include <cctype>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace py = pybind11;

namespace {

// Read an environment variable. On Windows we use GetEnvironmentVariableA
// (Win32 process-environment block) instead of std::getenv: this extension is
// built with the debug CRT (MDd) while python.exe uses the release CRT (MD),
// and each CRT instance keeps its own getenv table -- so std::getenv would not
// see changes made by Python (os.environ) after the DLL was loaded.
// GetEnvironmentVariable reads the single PEB-backed process environment.
bool env_is_zero(const char* name) {
#ifdef _WIN32
    const DWORD len = GetEnvironmentVariableA(name, nullptr, 0);
    if (len == 0) {
        return false; // not set
    }
    std::string value(len, '\0');
    if (GetEnvironmentVariableA(name, value.data(), len) == 0) {
        return false;
    }
    value.resize(len - 1); // drop the trailing null terminator
    return value == "0";
#else
    const char* v = std::getenv(name);
    return v != nullptr && std::string(v) == "0";
#endif
}

// C++ mirror of the python/kimix_native shim `use_native(kernel)` toggle:
//   - returns false when env KIMIX_NATIVE == "0"
//   - returns false when env KIMIX_NATIVE_<KERNEL.upper()> == "0"
//   - returns true otherwise
bool use_native(const std::string& kernel) {
    if (env_is_zero("KIMIX_NATIVE")) {
        return false;
    }
    std::string key = "KIMIX_NATIVE_" + kernel;
    for (char& c : key) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return !env_is_zero(key.c_str());
}

// RAII fiber binding for the Python host (see the file header). Constructed
// in PYBIND11_MODULE (binds the importing thread to the shared pool),
// destroyed at extension unload (unbinds the same thread). A no-op when the
// importing thread already has a scheduler bound - e.g. a host that bound its
// own pool before importing runtime_py; marl allows exactly one scheduler per
// thread, and the ambient pool stays in charge there.
struct fiber_binding_guard {
    fiber_binding_guard() noexcept {
        if (!kimix::fiber::is_bound()) {
            kimix::fiber::shared_scheduler().bind();
            _bound = true;
        }
    }
    ~fiber_binding_guard() noexcept {
        // Unbind only what this guard bound. The shared pool itself is never
        // destroyed, so a thread detached here at unload cannot hang.
        if (_bound) {
            kimix::fiber::shared_scheduler().unbind();
        }
    }
    fiber_binding_guard(const fiber_binding_guard &) = delete;
    fiber_binding_guard &operator=(const fiber_binding_guard &) = delete;

private:
    bool _bound = false;
};

} // namespace

// Submodule registration entry points (defined in py_text.cpp / py_stream.cpp /
// py_index.cpp / py_search.cpp / py_codec.cpp / py_parse.cpp / py_tools.cpp /
// py_diff.cpp / py_glob.cpp, compiled into this target by the recursive glob
// ../runtime/py/**).
void py_register_text(py::module_& m);
void py_register_stream(py::module_& m);
void py_register_index(py::module_& m);
void py_register_history(py::module_& m);
void py_register_search(py::module_& m);
void py_register_codec(py::module_& m);
void py_register_parse(py::module_& m);
void py_register_tools(py::module_& m);
void py_register_diff(py::module_& m);
void py_register_glob(py::module_& m);
void py_register_grep(py::module_& m);
void py_register_print(py::module_& m);
void py_register_builtin_shell(py::module_& m);
void py_register_builtin_file(py::module_& m);
void py_register_builtin_web(py::module_& m);
void py_register_builtin_python(py::module_& m);

PYBIND11_MODULE(runtime_py, m) {
    // The extension's "root main": binds the importing thread to the
    // process-wide fiber pool for as long as the module is loaded (see the
    // file header and fiber_binding_guard above).
    static const fiber_binding_guard fiber_binding;

    m.doc() = "Kimix runtime Python bindings (built on kimix-core)";

    m.def("version", []() { return kimix::runtime::version_string; },
          "Returns the runtime module version string");
    m.def("core_version", &kimix::runtime::core_version,
          "Returns the underlying kimix-core version string");
    m.def("c_version", []() { return kimix_runtime_version(); },
          "Returns the runtime version string via the C-FFI entry point");
    m.def("use_native", &use_native,
          "C++ mirror of the shim use_native(kernel) toggle: false when "
          "KIMIX_NATIVE=0 or KIMIX_NATIVE_<KERNEL>=0, true otherwise",
          py::arg("kernel"));

    m.attr("version_string") = kimix::runtime::version_string;

    // ------------------------------------------------------------------
    // Submodule skeletons -- later plans (001+) fill each domain.
    // ------------------------------------------------------------------
    {
        auto text = m.def_submodule("text", "Text kernels (heuristic token count, sanitizer, ANSI strip, line stream).");
        py_register_text(text);
    }
    {
        auto index = m.def_submodule(
            "index", "Index kernels (ngram tokenizer, incremental inverted index, history blob I/O).");
        py_register_index(index);
        py_register_history(index);
    }
    {
        auto search = m.def_submodule(
            "search", "Search kernels (BM25 scorer, fuzzy matching, line hash / string find / grep).");
        py_register_search(search);
    }
    {
        auto codec = m.def_submodule(
            "codec", "Codec kernels (wire envelope + merge buffer, JSON-RPC/jsonl frames, TCP recv buffer, SSE frames).");
        py_register_codec(codec);
    }
    {
        auto stream = m.def_submodule("stream", "Stream kernels (single-pass line stream processor).");
        py_register_stream(stream);
    }
    {
        auto parse = m.def_submodule(
            "parse", "Parse kernels (comment parsers, bash/pwsh command scanners).");
        py_register_parse(parse);
    }
    {
        auto tools = m.def_submodule(
            "tools", "Tool kernels (line hashing, string find, grep line scan, export markdown).");
        py_register_tools(tools);
    }
    {
        auto diff = m.def_submodule(
            "diff", "Diff kernels (unified diff, hunk extraction, inline diff ranges).");
        py_register_diff(diff);
    }
    {
        auto glob = m.def_submodule(
            "glob", "Glob kernels (gitignore parsing/matching, path filtering, git ls-files parser).");
        py_register_glob(glob);
    }
    {
        auto grep = m.def_submodule(
            "grep", "Grep kernels (native grep_engine content scan + regex_lite pattern probe).");
        py_register_grep(grep);
    }
    {
        auto print = m.def_submodule(
            "print", "Async print stream (concurrent queue + background worker thread).");
        py_register_print(print);
    }

    // ------------------------------------------------------------------
    // builtin_tools -- C++ ports of the kimi-agent built-in tools.
    // ------------------------------------------------------------------
    {
    auto builtin_tools = m.def_submodule(
        "builtin_tools",
        "Built-in agent tool kernels (shell/file/web/python).");
    py_register_builtin_shell(builtin_tools);
    py_register_builtin_file(builtin_tools);
    py_register_builtin_web(builtin_tools);
    py_register_builtin_python(builtin_tools);
    }

    // ------------------------------------------------------------------
    // common -- shared low-level kernels (GIL released during calls).
    // ------------------------------------------------------------------
    auto common = m.def_submodule("common",
                                  "Shared low-level kernels (GIL released during calls).");

    common.def("is_ascii", [](py::bytes data) -> bool {
        char* buf = nullptr;
        Py_ssize_t len = 0;
        if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
            throw py::error_already_set();
        }
        kimix::string_view view(buf, static_cast<size_t>(len));
        kimix::runtime::common::gil_scoped_release release;
        return kimix::runtime::common::is_ascii(view);
    }, "True when every byte is < 0x80 (pure ASCII).", py::arg("data"));

    common.def("utf8_code_point_count", [](py::bytes data) -> size_t {
        char* buf = nullptr;
        Py_ssize_t len = 0;
        if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
            throw py::error_already_set();
        }
        kimix::string_view view(buf, static_cast<size_t>(len));
        kimix::runtime::common::gil_scoped_release release;
        return kimix::runtime::common::utf8_code_point_count(view);
    }, "Count UTF-8 code points in a bytes object.", py::arg("data"));
}
