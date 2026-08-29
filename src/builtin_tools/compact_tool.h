// compact_tool.h - C++ port of the compact context-compaction kernels.
//
// Plan: D:/KimiX-native/plans/compact.md
// Python source of truth (D:/kimi-agent/kimi-cli/src/kimi_cli/soul/):
//   compaction.py
//     CompactMode / _MODE_GUIDANCE (37-58)
//     CompactionOptions (61-73)
//     SummarizationInput (111-124)
//     SurfaceFingerprint / _surface_fingerprint (127-134 / 182-189)
//     CompactionResult / estimated_token_count* (136-179)
//     _detect_cascade_depth (223-231)
//     SAFETY_MARGIN_TOKENS / should_auto_compact (234-277)
//     adaptive_preserve_depth (280-329)
//     _build_prompt_text / _resolve_preserve_depth (392-441)
//     SimpleCompaction.compact (443-677) - orchestration stays in Python
//     PrepareResult / prepare (679-796) - slicing/prompt assembly ported
//   utils/tokens.py
//     count_tokens / _estimate_chars_tokens / count_message_tokens (42-98)
//   packages/kosong/src/kosong/message.py
//     Message / TextPart / ThinkPart / ToolCall / ToolCallPart (16-303)
//
// What is ported to C++:
//   * should_auto_compact
//   * adaptive_preserve_depth
//   * detect_cascade_depth
//   * build_compaction_prompt
//   * build_compact_message_text
//   * prepare_compaction_input
//   * compute_surface_fingerprint
//   * estimate_message_tokens / estimate_text_tokens
//
// What stays in Python (per plan §4):
//   * LLM summarization call, durable ledger, stability/shrink checks,
//     context-overflow retry loop, balanced tool-pairing boundary computation,
//     tiktoken exact counting, Pydantic validation, tool orchestration.
//
// There is no CompactTool class; compact is a kernel library used by the Python
// compact tool and SimpleCompaction. A thin kimix::builtin_tools::Tool subclass
// named Compact is exposed so the Python binding can drive it like other tools.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::compact {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// ── Message representation (Kosong Message minimal subset) ───────────────────

struct content_part {
    kimix::string type; // "text", "think", or "other"
    kimix::string text; // non-empty for "text" / "think"
};

struct message {
    kimix::string role; // "system", "user", "assistant", "tool"
    kimix::vector<content_part> content; // in-order content parts
};

// Join type == "text" parts with `sep`.
kimix::string extract_text(const message &msg,
                          kimix::string_view sep = "") noexcept;

// True if any part has type == "think".
bool has_think_part(const message &msg) noexcept;

// True for role == "user" or "assistant".
bool is_user_or_assistant(const message &msg) noexcept;

// ── CompactMode and options ────────────────────────────────────────────────

enum class CompactMode : uint8_t {
    retentive = 0,
    balanced,
    aggressive,
    technical,
};

struct compaction_options {
    bool avoid_cascade = false;
    CompactMode mode = CompactMode::balanced;
    int32_t todos_max_items = 20; // carried for Python; not used by C++ kernels
    int32_t preserve_depth_override = -1; // -1 == none
    bool decision_section_enabled = false;
};

// Converts user-facing mode string to enum; defaults to retentive on unknown.
CompactMode parse_compact_mode(kimix::string_view mode) noexcept;

// Guidance text for the mode; invalid mode returns empty view.
kimix::string_view mode_guidance(CompactMode mode) noexcept;

// ── Auto-compaction trigger ────────────────────────────────────────────────

struct compaction_trigger_config {
    double trigger_ratio = 0.75;
    int64_t max_context_size = 128000;
    int64_t reserved_context_size = 8192;
    int64_t max_tokens = 0; // 0 when absent
    int64_t tool_call_buffer_tokens = 0;
    int64_t safety_margin_tokens = 1024;
};

// Returns true when the token count crosses the ratio or reserved boundary.
bool should_auto_compact(int64_t token_count,
                         const compaction_trigger_config &cfg) noexcept;

// ── Adaptive preserve depth ────────────────────────────────────────────────

int32_t adaptive_preserve_depth(kimix::span<const message> messages,
                                int32_t min_preserved = 1,
                                int32_t max_preserved = 10) noexcept;

// ── Cascade depth detection ────────────────────────────────────────────────

int32_t detect_cascade_depth(kimix::span<const message> messages) noexcept;

// ── Prompt assembly ────────────────────────────────────────────────────────

struct compaction_prompt_input {
    kimix::span<const message> to_compact; // messages that will be compacted
    compaction_options options;
    kimix::string_view custom_instruction; // may be empty
    // Static prompt bodies, passed in so C++ does not hard-code the prose.
    kimix::string_view prompt_compact;
    kimix::string_view prompt_compact_cascade;
};

struct compaction_prompt_output {
    kimix::string prompt_text;
    int32_t cascade_depth = 0;
};

// Assembles the compaction instruction text.
tool_error build_compaction_prompt(const compaction_prompt_input &in,
                                   compaction_prompt_output &out);

struct compact_message_request {
    kimix::span<const message> to_compact; // already-determined compacted region
    kimix::string_view prompt_text; // output of build_compaction_prompt
};

// Legacy flattened compact_message text.
kimix::string build_compact_message_text(
    const compact_message_request &req) noexcept;

// ── Prepare compaction input (CPU-bound slice + prompt) ──────────────────────

struct prepare_request {
    kimix::span<const message> messages; // full conversation history
    size_t preserve_start_index = 0; // balanced by Python; 0..messages.size()
    compaction_options options;
    kimix::string_view custom_instruction;
    kimix::string_view prompt_compact;
    kimix::string_view prompt_compact_cascade;
};

struct prepare_result {
    kimix::vector<message> to_compact;
    kimix::vector<message> to_preserve;
    kimix::string compact_message_text; // legacy flattened message
    kimix::string prompt_text; // final instruction text
    int32_t cascade_depth = 0;
};

// Slices history, builds the legacy compact_message text, and assembles the
// prompt. The preserve_start_index is assumed to already respect balanced tool
// pairing (computed by Python).
tool_error prepare_compaction_input(const prepare_request &req,
                                    prepare_result &out);

// ── Surface fingerprint ──────────────────────────────────────────────────────

struct surface_fingerprint {
    uint32_t history_len = 0;
    int64_t token_count = 0;
    kimix::optional<kimix::string> last_message_text; // nullopt when history empty
};

// Cheap surface snapshot used by the stability check.
// token_counter may be null; when null, estimate_message_tokens is used.
surface_fingerprint compute_surface_fingerprint(
    kimix::span<const message> messages,
    kimix::function<int64_t(kimix::span<const message>)> token_counter = {});

// ── Token estimation ─────────────────────────────────────────────────────────

// Language-aware heuristic matching Python _estimate_chars_tokens.
int64_t estimate_text_tokens(kimix::string_view text) noexcept;

// Sums estimate_text_tokens over all type == "text" parts.
int64_t estimate_message_tokens(kimix::span<const message> messages) noexcept;

// ── Result shaping helpers ───────────────────────────────────────────────────

// Result wrapper returned to Python (mirrors CompactionResult fields that are
// derivable without an LLM call).
struct compaction_result {
    kimix::vector<message> messages; // compacted summary + preserved tail
    int64_t estimated_token_count = 0; // sum of estimate_message_tokens(messages)
    kimix::string compaction_id; // empty when no-op; generated by Python
    int64_t shadowed_tokens = 0; // tokens of the replaced region
};

// ── Tool class and standard integration ──────────────────────────────────────

// Thin Tool subclass so the Python orchestration layer can call the kernels
// through the same binding path as other built-in tools. Compact is not a
// self-contained agent tool; operator() deserializes the Kosong-style message
// array, runs the kernels, and serializes a result payload into `_last_result`.
class Compact : public kimix::builtin_tools::Tool {
public:
    explicit Compact(kimix::builtin_tools::Session *session);
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &last_result() const { return _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace kimix::builtin_tools::compact
