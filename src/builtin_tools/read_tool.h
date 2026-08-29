// read_tool.h - Pure-CPU kernels for the `read` built-in agent tool.
//
// Plan: C:/dev/kimi-agent/plans/read.md (§3 C++ design phases 1 & 3, §7 tests).
// Python sources of truth (mirrored byte-exactly, see reports/read.md):
//   - kimi-cli/src/kimi_cli/tools/file/read.py
//       _apply_char_window (349-385), _render_forward (1528-1578),
//       _render_tail (1580-1648), _render_result (1650-1697);
//       constants MAX_LINES=5000 (81), MAX_LINE_LENGTH=4000 (82),
//       MAX_BYTES=100<<10 (85-87), Params._validate_value (278-294).
//   - kimi-cli/src/kimi_cli/tools/file/read_profiles.py
//       render_cpu_profile (194-261), render_sample_profile (387-430).
//   - kimi-cli/src/kimi_cli/tools/file/read_markit.py
//       markdown_to_text (215-254).
//   - kimi-cli/src/kimi_cli/tools/file/hash_line.py
//       compute_line_hash (55-87), _cumulative_hashes (90-109).
//
// Everything here is pure CPU: kernels take bytes and return data; no file
// system, no exceptions across the boundary (tool_error / bool instead).
//
// NOT ported here (missing third-party libraries — see issue/read.md):
// PDF rendering, DOCX/XLSX/XLS/PPTX/IPYNB extraction, archive member
// extraction, image rendering, HTML->text (markdownify, owned by fetch_url).
// Conflict-marker scanning is owned by the write tool (see README ownership
// map) and is intentionally absent.

#pragma once

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools {
namespace read {

// ── Constants (read.py 81-87) ──────────────────────────────────────────────

inline constexpr int64_t k_max_lines = 5000;         // MAX_LINES
inline constexpr int64_t k_max_line_length = 4000;   // MAX_LINE_LENGTH (code points)
inline constexpr uint64_t k_max_bytes = 100u << 10;  // MAX_BYTES = 102400
inline constexpr uint32_t k_max_files = 32;          // MAX_FILES
inline constexpr uint64_t k_max_profile_summary_bytes = 32u * 1024u * 1024u;

// ── Parameter validation (Params._validate_value, read.py 278-294) ─────────

// Validates one scalar option exactly like Params._validate_value.
// `name` must be "offset" | "limit" | "max_char" | "char_offset".
// Returns ok status on success; invalid_input with the byte-exact Python
// ValueError message on failure.
tool_error validate_int_option(kimix::string_view name, int64_t value);

// ── Truncation (kimi_cli.tools.utils.truncate_line, tools/utils.py 113-126) ─

// read.py imports THIS truncate_line (tools/utils.py, marker="..."), not the
// output_utils one. A trailing line-break run ([\r\n]+$) is preserved and the
// max budget is raised to at least len(marker+linebreak). The output can
// therefore exceed max_len for very small max_len values — same as Python.
void truncate_line_read(kimix::string_view text, int64_t max_len,
                        kimix::string &out);

// ── Line splitting (aiofiles universal newlines semantics) ─────────────────

// Splits `bytes` exactly like Python text-mode iteration: LF and lone CR are
// line ends; CRLF becomes a single LF; each yielded line keeps its trailing
// '\n' except the last when the input does not end with a line break.
// Invalid UTF-8 decodes to U+FFFD (errors="replace"), matching the
// KaosPath.read_lines(errors="replace") source path.
kimix::vector<kimix::string> split_lines(kimix::string_view bytes);

// ── Render engine (read.py 1528-1697) ──────────────────────────────────────

// One budgeted, optionally numbered line ready for output.
struct rendered_line {
    int64_t line_no = 0;      // 1-based physical line number in the file
    kimix::string text;       // truncated line content (line break kept)
    bool was_truncated = false;
    uint64_t byte_len = 0;    // UTF-8 byte length of `text`
};

// Full render result — mirrors the (ToolOk, window_lines, start_line) triple
// returned by _render_forward / _render_tail, with the output/message split
// out so callers can still apply the char window.
struct render_result {
    kimix::string output;                       // "".join(lines_with_no)
    kimix::string message;                      // byte-exact status message
    kimix::vector<kimix::string> window_lines;  // raw (rstripped) lines shown
    int64_t start_line = 1;                     // for the char-window/conflict pass
    int64_t total_lines = -1;                   // -1 == unknown (mid-file stop)
    bool max_lines_reached = false;
    bool max_bytes_reached = false;
    bool end_of_file = false;
    kimix::vector<int64_t> truncated_line_numbers;
};

// Positive line_offset path (_render_forward).
// `lines` already carry their (normalized) trailing '\n' where applicable —
// typically produced by split_lines. Budgets: min(n_lines, MAX_LINES) lines,
// MAX_BYTES bytes. `note` is appended to the message (document-extraction
// notice); `display_path` closes the message (" Path: ...").
render_result render_forward(kimix::span<const kimix::string> lines,
                             kimix::string_view display_path,
                             int64_t line_offset, int64_t n_lines,
                             bool show_line_numbers = true,
                             kimix::string_view note = "");

// Negative line_offset path (_render_tail): keeps the last abs(line_offset)
// lines via a bounded ring buffer (O(n) — the Python list.pop(0) was O(n²)),
// then applies the n_lines/MAX_LINES cap from the head of the window and the
// byte budget by reverse-scanning for the newest lines that fit.
render_result render_tail(kimix::span<const kimix::string> lines,
                          kimix::string_view display_path,
                          int64_t line_offset, int64_t n_lines,
                          bool show_line_numbers = true,
                          kimix::string_view note = "");

// ── Character window (_apply_char_window, read.py 349-385) ─────────────────

struct char_window {
    kimix::string output; // sliced output (code points)
    kimix::string note;   // empty when nothing is hidden, else the NOTE text
};

// Slices `output` to [char_offset, char_offset + max_char) in code points and
// builds the " NOTE: output window shows ..." notice when content is hidden.
char_window apply_char_window(kimix::string_view output, int64_t char_offset,
                              int64_t max_char);

// ── Repeated-line collapse (read's dedup mode) ─────────────────────────────

// Chained per-line xxHash32 low bytes — the read-side line-hash helper used
// for dedup identification (port of hash_line.py::compute_line_hash +
// _cumulative_hashes). Recipe per line: strip one trailing '\r'; keep only
// non-whitespace code points (Python str.isspace set); seed = chained nibble
// decode of the previous hash, else 0 when the line has any alnum char,
// else the 1-based line number; value = xxh32(filtered, seed) & 0xFF.
// Lines are split on '\n' (trailing '\r' handled by the strip step).
kimix::vector<uint32_t> compute_line_hashes(kimix::string_view content);

// Renders the chained hashes as the 2-char nibble strings
// (NIBBLE_STR = "ZPMQVRWSNKTXJBYH"), matching Python _cumulative_hashes.
kimix::vector<kimix::string> compute_line_hash_strings(kimix::string_view content);

// Non-chained per-line hash for repeated-line identification in read's dedup
// mode: the same recipe with no previous hash and line_num=1, so identical
// lines always produce the same value (chained hashes differ by position).
uint32_t line_hash_independent(kimix::string_view line);

// Collapses runs of >= min_repeats identical hash values into a single entry,
// producing the collapse annotation used by read's dedup mode:
// "<line>  (K repeats)" appended to the first line of each collapsed run.
// Pass hashes from line_hash_independent so identical lines compare equal.
// `saved` reports how many lines were dropped.
void collapse_repeated_lines(kimix::span<const uint32_t> hashes,
                             kimix::span<const kimix::string> lines,
                             size_t min_repeats,
                             kimix::vector<kimix::string> &out, size_t &saved);

// ── Profile summarizers (read_profiles.py) ─────────────────────────────────

// Renders a V8 .cpuprofile JSON summary, or returns false when the input is
// not a valid profile (Python returns None -> caller falls back to raw text).
// Uses the vendored yyjson parser with a mimalloc-backed allocator; never
// throws. `out` holds the rendered text when true is returned.
bool render_cpu_profile(kimix::string_view json_text, kimix::string &out);

// Renders a macOS `sample` profile summary, or returns false when the input
// is not recognized (no preamble / no samples).
bool render_sample_profile(kimix::string_view text, kimix::string &out);

// ── Markdown to text (read_markit.py::markdown_to_text) ────────────────────

// Deterministic scanner for the nine regex passes: fenced code blocks
// ("[code block: N lines]"), inline code placeholders, bold/italic/underscore
// emphasis (underscore is word-boundary guarded), links "text (url)", images
// "[image: url]", heading markers, horizontal rules, and blank-run collapse.
// Operates on UTF-8 bytes; non-ASCII is handled via code-point classes
// (\w == Unicode L*/N*/Pc/Mark per Python regex \w, ASCII fast path).
kimix::string markdown_to_text(kimix::string_view md);

// ---------------------------------------------------------------------------
// Tool class wrapper (CallableTool2-style binding entry point)
// ---------------------------------------------------------------------------
// The native read tool performs the deterministic text-rendering kernels on
// caller-supplied bytes.  Path resolution, safety guards, archive/SQLite/
// document extraction, PDF rendering, HTML->text, conflict scanning, and the
// top-level async dispatcher remain in Python per plans/read.md §4.
//
// Expected input parameters (all strings/ints/bools as JSON values):
//   content           string   file bytes/text to render (required)
//   display_path      string   path shown in messages (required)
//   mode              string   "text" | "markdown" | "cpu_profile" | "sample_profile"
//                            (default "text"; lets Python route rich formats)
//   offset            int      line_offset for text mode (default 1)
//   limit             int      n_lines for text mode (default 2000)
//   max_char          int      char-window budget for text mode (default 16000)
//   char_offset       int      char-window start for text mode (default 0)
//   show_line_numbers bool     prefix "%6d\t" lines in text mode (default true)
//   note              string   appended to the message in text mode (default "")
//
// The serialized result object always contains:
//   status            string   "ok" | "invalid_input" | "unsupported"
//   output            string   rendered text
//   message           string   status message
//   brief             string   "Read file"
// For status "ok" the result may also contain:
//   start_line        int      first rendered line number
//   total_lines       int      total lines in file (-1 if unknown)
//   max_lines_reached bool
//   max_bytes_reached bool
//   end_of_file       bool
//   truncated_line_numbers  array of ints
class Read : public kimix::builtin_tools::Tool {
public:
    explicit Read(kimix::builtin_tools::Session *session);

    // Validate parameters, dispatch to the native kernels, and serialize the
    // result into an internal buffer.  Never throws across the tool boundary.
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &serialized_result() const { return _result; }

private:
    kimix::vector<char> _result;
};

} // namespace read
} // namespace kimix::builtin_tools
