/*
 * magic_enum.h — the Luisa magic_enum door (third-party leg only).
 *
 * STANDALONE (no KIMIX_EXTERNAL_THIRDPARTY): this header defines nothing.
 * kimix-core does not reflect over enums — an enum is turned into text by an
 * explicit table at the call site — so there is no kimix equivalent to map onto,
 * and the standalone umbrella header (core/kimix_core.h) does NOT include this
 * file. Including it directly is harmless (it expands to nothing); a call site
 * that has to compile in both legs must guard its calls with the same macro.
 *
 * THIRD-PARTY LEG: LC vendors magic_enum and exposes it through
 * <luisa/core/magic_enum.h>, which configures it with luisa::string /
 * luisa::string_view / luisa::optional first, so its results are kimix::string &
 * friends too (same types in this leg — see stl/string.h, stl/optional.h). Use
 * that ONE copy; this door adds no second include of the magic_enum headers.
 *
 * What this file adds:
 * kimix::to_string(value) — the enum name as a kimix::string_view
 * (magic_enum::enum_name's return type), empty when the value
 * is not a named enumerator (luisa::to_string re-exported;
 * the other luisa::to_string overloads — filesystem
 *                                      paths, Vector/Matrix — come with it).
 * What comes along from the header without a re-export:
 *   magic_enum::enum_name / enum_names / enum_values / enum_entries /
 *   enum_contains / enum_cast / enum_integer / enum_type_name / enum_count /
 *   enum_index ... in the global magic_enum namespace, and
 *   LUISA_MAGIC_ENUM_RANGE(E, E_MIN, E_MAX) to widen the scanned range of a large
 *   enum (magic_enum only scans [-128, 128] by default).
 */
#pragma once

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

#include <luisa/core/magic_enum.h>

namespace kimix {

using luisa::to_string;

} // namespace kimix

#endif // KIMIX_EXTERNAL_THIRDPARTY