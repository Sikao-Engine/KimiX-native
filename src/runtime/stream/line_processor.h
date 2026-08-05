/*
 * line_processor.h — Single-pass line stream processor (kimix::runtime::stream).
 *
 * Plan 003: native port of the subprocess-output post-processing chain —
 * ANSI stripping (`filter_output`), CRLF normalization, line splitting,
 * dedup (`_dedup_output` counter + block modes), column folding and byte /
 * code-point accounting. Today this runs per 4096-byte chunk with regex
 * passes and O(n²) list ops; here it is one stateful processor.
 *
 * Semantics (verified against src/kimix/tools/common.py):
 * - feed(chunk): ANSI-strip (streaming, chunk boundaries safe), normalize
 *   CRLF / lone-CR to LF, split on LF. Completed lines are processed.
 * - Dedup mode 0 (off): completed lines are emitted on feed (after fold and
 *   budget checks) — streaming.
 * - Dedup mode 1 (counter): EXACT mirror of `_dedup_output(output,
 *   threshold, max_block_lines=1)` — Counter total counts, first-occurrence
 *   annotated `"{line}  ({cnt} repeats)"`. Requires total counts, so lines
 *   are buffered and emitted at flush().
 * - Dedup mode 2 (block): EXACT mirror of `_dedup_output(output, threshold,
 *   max_block_lines=block_window)` — greedy largest-block-first contiguous
 *   run detection with `consumed` marking. Also buffered until flush().
 * - fold_col > 0: wrap every line at fold_col CODE POINT boundaries (a line
 *   of length L produces ceil(L/fold_col) output lines; no mid-codepoint
 *   splits). fold_col == 0 disables folding.
 * - max_lines / max_bytes > 0: output budgets. Once exhausted, no further
 *   output is produced (truncation happens at line boundaries). 0 = unlimited.
 * - bytes_written(): UTF-8 bytes of emitted line content (terminators not
 *   counted); code_points_written(): code points of emitted line content;
 *   lines_written(): number of emitted lines. Counters are cumulative across
 *   feed/flush until reset().
 *
 * Line splitting matches `str.splitlines()` for LF / CRLF / lone-CR input
 * (which is all filter_output produces); other Unicode line boundaries
 * (U+0085, U+2028, …) are deliberately NOT split — the real pipeline
 * normalizes to LF before this processor runs.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/stream/ansi.h>

namespace kimix {
namespace runtime {
namespace stream {

struct process_options {
    bool strip_ansi = true;
    uint32_t dedup_mode = 0;    // 0=off, 1=counter, 2=block
    uint32_t threshold = 3;     // collapse when repeat count > threshold
    uint32_t block_window = 3;  // max block height h (block mode)
    size_t max_bytes = 0;       // 0 = unlimited output budget (line-boundary truncation)
    size_t max_lines = 0;       // 0 = unlimited line count
    size_t fold_col = 0;        // 0 = no folding; > 0 wraps lines at fold_col cps
};

class KIMIX_RUNTIME_API LineProcessor {
public:
    explicit LineProcessor(const process_options& opts = process_options());
    ~LineProcessor() = default;

    LineProcessor(const LineProcessor&) = delete;
    LineProcessor& operator=(const LineProcessor&) = delete;
    LineProcessor(LineProcessor&&) noexcept = default;
    LineProcessor& operator=(LineProcessor&&) noexcept = default;

    // Process one chunk; completed lines are appended to `out_lines` when
    // provided (dedup modes buffer and only emit at flush()).
    void feed(kimix::string_view chunk, kimix::vector<kimix::string>* out_lines = nullptr);

    // End of stream: resolve pending ANSI/CR state, finalize the last line,
    // run dedup (modes 1/2) and append every emitted line to `out_lines`.
    void flush(kimix::vector<kimix::string>* out_lines = nullptr);

    // Clear all state: buffers, counters, ANSI stripper.
    void reset();

    uint64_t bytes_written() const noexcept { return bytes_written_; }
    uint64_t code_points_written() const noexcept { return code_points_written_; }
    uint64_t lines_written() const noexcept { return lines_written_; }

private:
    // CRLF-normalize + split the cleaned bytes; completed lines go through
    // process_line (mode 0) or the line buffer (modes 1/2).
    void process_cleaned(kimix::string_view cleaned, kimix::vector<kimix::string>* out_lines);
    void finalize_line(kimix::vector<kimix::string>* out_lines);
    void emit_segment(kimix::string_view segment, kimix::vector<kimix::string>* out_lines);
    void fold_and_emit(kimix::string_view line, kimix::vector<kimix::string>* out_lines);
    void emit_deduped(kimix::vector<kimix::string>* out_lines);

    process_options opts_;
    AnsiStripper ansi_;

    kimix::string line_buf_;   // current (possibly partial) line, no terminator
    bool pending_cr_ = false;  // saw '\r', waiting to see if '\n' follows

    kimix::vector<kimix::string> lines_; // buffered raw lines for dedup modes
    bool budget_exhausted_ = false;

    uint64_t bytes_written_ = 0;
    uint64_t code_points_written_ = 0;
    uint64_t lines_written_ = 0;
};

// One-shot convenience: strip ANSI + normalize CRLF/CR to LF
// (exact `filter_output` from tools/common.py). No line splitting.
KIMIX_RUNTIME_API kimix::string filter_output(kimix::string_view utf8);

} // namespace stream
} // namespace runtime
} // namespace kimix
