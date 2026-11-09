/*
 * runtime.h — Kimix Runtime module.
 *
 * High-level runtime library built on top of kimix-core. Exposes a small
 * C-FFI surface (kimix_runtime_version / kimix_runtime_core_version) plus a
 * C++ API (kimix::runtime::core_version). The runtime kernels are compiled
 * into the static kimix-runtime archive (src/xmake.lua), which both the
 * Python extension module runtime_py.pyd and the C++ kernel tests link.
 *
 * KIMIX_RUNTIME_EXPORT_DLL (public on kimix-llm) makes the exported symbols
 * below __declspec(dllexport) in every image that links the archive; a
 * consumer linking the archive therefore sees the same plain declarations.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {

// Semantic version string of the runtime module itself.
// The version literal lives only in the version.txt config file; xmake
// generates <build>/gen/kimix_version.h with KIMIX_RUNTIME_VERSION.
inline constexpr auto version_string = "kimix-runtime " KIMIX_RUNTIME_VERSION;

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
