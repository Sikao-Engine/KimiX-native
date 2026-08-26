/*
 * compress.h - Native micro-compression kernels for text (plan 016).
 *
 * Pure C++ implementations of selected stages from
 * kimi-cli/src/kimi_cli/tools/file/micro_compress.py.  All kernels operate on
 * UTF-8 bytes; the Python binding layer handles str<->bytes conversion with
 * surrogatepass.
 *
 * Public API:
 *   compress_intra_line_dedup     - Stage 7 (annotated)
 *   compress_collapse_whitespace    - Stage 3 (lossless + annotated)
 *   compress_renumber_lines         - Stage 5 (lossless)
 *   compress_strip_control_noise    - Stage 2 (lossless)
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

// Intra-line repeating-unit dedup.
//   text       : input UTF-8 bytes
//   threshold  : minimum line length to attempt dedup
//   max_unit   : maximum repeating-unit length to consider
// Mirrors intra_line_dedup(text, kind="log", config) where
// config.intra_line_dedup=True and config.intra_line_dedup_len=threshold.
KIMIX_RUNTIME_API kimix::string compress_intra_line_dedup(kimix::string_view text,
                                                        int threshold,
                                                        int max_unit);

// Whitespace collapse.
// Mirrors collapse_whitespace(text, kind, config) with the corresponding
// config fields.  The prefix_fold argument is kept for API symmetry but is
// not used by this stage (it belongs to fold_per_line_prefix).
KIMIX_RUNTIME_API kimix::string compress_collapse_whitespace(kimix::string_view text,
                                                             kimix::string_view kind,
                                                             bool lossless_only,
                                                             bool strip_trailing_ws,
                                                             int blank_line_collapse,
                                                             bool common_indent_factor,
                                                             bool prefix_fold);

// Compact fixed-width leading line numbers ("  42\t" -> "42\t").
// Mirrors renumber_lines(text).
KIMIX_RUNTIME_API kimix::string compress_renumber_lines(kimix::string_view text);

// Strip ANSI/OSC/DCS escape sequences and collapse CR progress-bar chains.
// Mirrors strip_control_noise(text).  Reuses stream::strip_ansi for the ANSI
// strip step.
KIMIX_RUNTIME_API kimix::string compress_strip_control_noise(kimix::string_view text);

} // namespace tools
} // namespace runtime
} // namespace kimix
