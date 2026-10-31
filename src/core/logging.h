/*
 * logging.h — the Luisa logging door (third-party leg only).
 *
 * STANDALONE (no KIMIX_EXTERNAL_THIRDPARTY): this header defines nothing.
 * kimix-core has no logging backend of its own — core code reports through
 * return values, deliberate stderr lines and kimix::allocation_failure(), and
 * there is no kimix::log_* to bind to — so there is nothing to map onto, and the
 * standalone umbrella header (core/kimix_core.h) does NOT include this file.
 * Including it directly is harmless (it expands to nothing); a call site that has
 * to compile in both legs must guard its calls with the same macro.
 *
 * THIRD-PARTY LEG: the process already carries LC's spdlog-backed logger (LC's
 * own core code logs through it), so kimix code uses that ONE backend through
 * this door instead of inventing a second one. What you get is luisa::log_*
 * re-exported under kimix:::
 *
 *   kimix::log_verbose(...)   kimix::log_info(...)
 *   kimix::log_warning(...)   kimix::log_error(...)   [[noreturn]]
 *   kimix::log_flush()
 *   kimix::log_level_verbose() / log_level_info() / log_level_warning() /
 *   log_level_error()
 *   kimix::logger  (the spdlog::logger type) and kimix::log_level
 *                  (spdlog::level::level_enum)
 *
 * The macros are LC's and are the intended spelling at a call site:
 * LUISA_INFO / LUISA_VERBOSE / LUISA_WARNING / LUISA_ERROR and the
 * *_WITH_LOCATION / LUISA_NOT_IMPLEMENTED / LUISA_ASSERT family. They take a
 * single {} format string plus arguments; LUISA_ERROR logs a backtrace and then
 * aborts, which is kimix's exceptions-off "fatal" path.
 *
 * There is no kimix-specific log level, sink, file or rotation knob: those belong
 * to whoever configured the LC logger (luisa::log_level_* above, or LC's
 * spdlog sinks), and duplicating them here would be the second backend this door
 * exists to avoid.
 */
#pragma once

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

#include <luisa/core/logging.h>

namespace kimix {

using luisa::logger;
using luisa::log_level;

using luisa::log_error;
using luisa::log_flush;
using luisa::log_info;
using luisa::log_level_error;
using luisa::log_level_info;
using luisa::log_level_verbose;
using luisa::log_level_warning;
using luisa::log_verbose;
using luisa::log_warning;

} // namespace kimix

#endif // KIMIX_EXTERNAL_THIRDPARTY