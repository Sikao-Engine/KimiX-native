/*
 * kimix_core.h — KimixBase Core umbrella header.
 *
 * Includes all core headers (STL wrappers, basic types, concepts,
 * clock, memory management, file I/O, and more).
 *
 * Legacy kimix namespace (compatibility):
 *   kimix::add(a, b)       — add two ints
 *   kimix::multiply(a, b)  — multiply two ints
 *   kimix::version_string  — version identifier
 *
 * Just include this single header to access all core functionality:
 *   #include <core/kimix_core.h>
 */
#pragma once

// KimixBase Core — umbrella header.
// Includes all core headers.

// Windows: winsock2.h MUST precede windows.h (windows.h otherwise pulls
// winsock.h, and a later <llm/http_client.h> re-including winsock2.h then
// collides in unity batches — windows.h-before-winsock2.h breaks ws2tcpip.h).
// Core headers
// or consumer TUs pull <windows.h> in many places, so the umbrella sets the
// winsock order once, unconditionally (no-op on non-Windows).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// STL wrappers
#include "stl.h"

// DLL export macros
#include "dll_export.h"

// Win32 message-box suppression (no-op on non-Windows / when disabled)
#include "win_message_box_suppression.h"

// Basic types and traits
#include "basic_traits.h"
#include "basic_types.h"

// Concepts, constants, mathematics
#include "concepts.h"
#include "constants.h"
#include "mathematics.h"

// Clock, spin mutex, thread safety
#include "clock.h"
#include "spin_mutex.h"
#include "thread_safety.h"

// Platform
#include "platform.h"

// Memory management
#include "pool.h"
#include "first_fit.h"

// File I/O
#include "binary_file_stream.h"
#include "binary_io.h"

// Dynamic module loading
#include "dynamic_module.h"

// String utilities
#include "string_scratch.h"
// Three-way compare, MD5, GUID, hash map
#include "compare.h"
#include "md5.h"
#include "guid.h"
#include "hash_map.h"

// Third-party leg only: the Luisa logging and magic_enum doors (core/logging.h,
// core/magic_enum.h). Standalone kimix-core has neither a logging backend nor
// enum reflection to map onto, so those two headers define nothing there and are
// deliberately NOT part of the standalone umbrella — see each header's comment.
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include "logging.h"
#include "magic_enum.h"
#endif

// Version (generated at build time from the version.txt config file)
#include "kimix_version.h"

// ---------------------------------------------------------------------------
// Legacy kimix namespace (compatibility)
// ---------------------------------------------------------------------------

namespace kimix {

inline constexpr auto version_string = "kimix " KIMIX_CORE_VERSION;

auto add(int a, int b) -> int;
auto multiply(int a, int b) -> int;

} // namespace kimix
