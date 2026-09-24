// tool_types.h - Shared vocabulary for the built-in agent tool kernels.
//
// This directory (src/builtin_tools) holds the C++ ports of the kimi-agent
// built-in tools described in C:/dev/kimi-agent/plans/*.md (bash, pwsh,
// python, glob, grep, read, read_image, write, edit, fetch_url, web_search).
// Each tool owns exactly one <tool>_tool.h / <tool>_tool.cpp pair plus one
// Boost.UT test file under tests/unit/builtin_tools/.
//
// Design rules (mirrors the plans' "3. C++ design" sections and
// .agents/skills/cpp/SKILL.md):
//   * namespace kimix::builtin_tools
//   * kimix::string / kimix::string_view / kimix::vector / kimix::optional /
//     kimix::StringScratch - never std:: equivalents (mimalloc-backed).
//   * Fixed-width integers (int32_t / uint32_t / int64_t / size_t).
//   * No RTTI: no dynamic_cast / typeid (project builds with kimix_rtti=false).
//   * Pure CPU kernels: no file-system, no subprocess, no socket access inside
//     a kernel unless the plan explicitly says so; OS/IO effects are injected
//     through kimix::function callbacks or plain data arguments so every kernel
//     is deterministically unit-testable.
//   * Errors are returned as data (tool_error / optional), never thrown across
//     the tool boundary.
//
// Everything here is compiled into the `kimix-llm` static library target
// (see src/xmake.lua), which is linked statically: no dll export macro is needed
// (same convention as src/llm/*.h).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::builtin_tools {

// Status code shared by every tool kernel entry point. `ok` means the result
// payload is valid; every other value means the payload is unspecified and
// `message` carries a human-readable, byte-exact diagnostic.
enum class tool_status : uint8_t {
    ok = 0,
    invalid_input,   // parameter failed validation (mirrors a Python ValueError)
    not_found,       // path / member / anchor missing
    no_change,       // idempotent edit, nothing applied
    ambiguous,       // multiple fuzzy matches, caller must disambiguate
    blocked,         // safety guard refused (sensitive file, hardline command, ...)
    too_large,       // byte / line budget exceeded before any work happened
    unsupported,     // input outside the native subset -> caller must use the
                     // Python fallback mirror (ASCII gate, regex-only feature, ...)
    external_library // the remaining work needs a third-party library that is not
                     // vendored in src/ext (see issue/*.md reports)
};

// Generic failure envelope returned by kernels that produce a string payload.
struct tool_error {
    tool_status status = tool_status::ok;
    kimix::string message;

    bool failed() const { return status != tool_status::ok; }
};

// A (start, end) byte range into a UTF-8 buffer. `end` is exclusive.
struct byte_range {
    uint64_t begin = 0;
    uint64_t end = 0;

    uint64_t size() const { return end - begin; }
};

// A 1-based inclusive line range, matching the kimi-agent line-selector
// grammar (`file.py:10-20`, `file.py:5+10`, `file.py:301-`).
struct line_range {
    uint32_t start_line = 1;
    kimix::optional<uint32_t> end_line; // nullopt == open ended to EOF

    bool operator==(const line_range &) const = default;
};

// Key/value pair used for parameters, env blocks and result metadata.
struct named_value {
    kimix::string name;
    kimix::string value;
};

// Common cap constants shared by the output pipeline kernels. Values are taken
// from the kimi-agent references so the native and Python sides agree:
//   k_max_output_bytes  glob.py / grep_local.py / hash_line.py `MAX_BYTES`
//                       (100 << 10) and read.py `_DEFAULT_READ_MAX_BYTES`.
//   k_record_cap        grep_recorder.py `RECORDER_CAP` (500).
//   k_max_head_limit    grep's default head_limit budget (500).
//   k_max_lines_fold    native fold budget (500).  Note this is NOT
//                       output_utils.DEFAULT_MAX_LINES (200): a caller that
//                       wants the Python default fold must pass 200 (the
//                       ported tools compute it themselves, see
//                       glob_tool.cpp shape_output).  output_utils
//                       DEFAULT_MAX_LINE_LEN == 500 matches k_max_head_limit
//                       and truncate_line's own default.
inline constexpr size_t k_max_output_bytes = 100u * 1024u;    // tool output byte cap
inline constexpr size_t k_max_lines_fold = 500u;              // native fold budget
inline constexpr size_t k_max_head_limit = 500u;              // default head_limit
inline constexpr size_t k_record_cap = 500u;                  // grep file recorder
inline constexpr uint32_t k_invalid_node = 0xFFFFFFFFu;       // arena sentinel

// Truncate `text` to at most `max_len` code points, appending the
// "... [+K chars]" marker used throughout the tool output pipeline.
void truncate_line(kimix::string_view text, size_t max_len,
                                 kimix::string &out);

// Join `lines` with '\n' under a byte budget and stop at the line that reaches
// it (port of grep_local._join_with_byte_limit, grep_local.py 618-632; glob.py
// 631-637 runs the same loop inline). The crossing line IS kept and `truncated`
// is set even when nothing follows it; `omitted` counts the input lines after
// the crossing one (a native extension - Python returns only the bool).
// The caller must pass valid UTF-8 (Python measures len(line.encode("utf-8"))).
void join_with_byte_limit(kimix::span<const kimix::string> lines,
                                        size_t max_bytes, kimix::string &out,
                                        bool &truncated, size_t &omitted);

// Keep the first `head` and last `tail` lines of `lines`, replacing the middle
// with the "... (N lines omitted)" marker (port of output_utils.fold_lines).
// `head` / `tail` are the values the Python call site would compute
// (head = max(1, max_lines // 2), tail = max_lines - head) because the Python
// defaults are keyword-only and live in its callers.
void fold_lines(kimix::span<const kimix::string> lines,
                              size_t max_lines, size_t head, size_t tail,
                              kimix::vector<kimix::string> &out,
                              size_t &omitted);

// Collapse consecutive duplicate lines (port of output_utils.dedup_lines):
// runs of `min_repeats`+ identical lines keep a single copy. `saved` reports
// how many lines were dropped.
void dedup_lines(kimix::span<const kimix::string> lines,
                               size_t min_repeats,
                               kimix::vector<kimix::string> &out, size_t &saved);

} // namespace kimix::builtin_tools
