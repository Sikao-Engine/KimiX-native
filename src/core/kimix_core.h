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

// ---------------------------------------------------------------------------
// Legacy kimix namespace (compatibility)
// ---------------------------------------------------------------------------

namespace kimix {

inline constexpr auto version_string = "kimix 0.1.0";

auto add(int a, int b) -> int;
auto multiply(int a, int b) -> int;

} // namespace kimix
