/*
 * runtime.h — Kimix Runtime module.
 *
 * High-level runtime library built on top of kimix-core. Exposes a small
 * C-FFI surface (kimix_runtime_version / kimix_runtime_core_version) plus a
 * C++ API (kimix::runtime::core_version). The Python bindings and the runtime
 * kernels are built together into the single runtime_py extension module.
 *
 * KIMIX_RUNTIME_EXPORT_DLL is defined privately by the build so the exported
 * symbols below get __declspec(dllexport); consumers see dllimport instead.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {

// Semantic version string of the runtime module itself.
inline constexpr auto version_string = "kimix-runtime 0.2.0";

// Returns the underlying kimix-core version string.
// Exported so the Python bindings (runtime_py) can link it from the DLL.
KIMIX_RUNTIME_API const char* core_version() noexcept;

} // namespace runtime
} // namespace kimix

// ---------------------------------------------------------------------------
// C-FFI — stable ABI entry points exported from the runtime shared library.
// ---------------------------------------------------------------------------
extern "C" {

// Returns the runtime module version string.
KIMIX_RUNTIME_API const char* kimix_runtime_version(void);

// Returns the underlying kimix-core version string.
KIMIX_RUNTIME_API const char* kimix_runtime_core_version(void);

} // extern "C"
