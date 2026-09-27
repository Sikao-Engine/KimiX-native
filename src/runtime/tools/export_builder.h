/*
 * export_builder.h - session export markdown builder (kimix::runtime::tools).
 *
 * Plan 016: one-pass string builder for kimi_cli/utils/export.py
 * build_export_markdown (165-320, verified against source): metadata header,
 * Overview section (topic from the first real user message via
 * shorten(message_stringify(msg), 80), comma-grouped token count), logical
 * turns grouped at real user messages, per-turn markdown with tool-call
 * blocks (hint extraction, orjson OPT_INDENT_2 argument JSON) and
 * collapsible tool-result blocks.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace tools {

struct export_options {
    kimix::string_view session_id;
    kimix::string_view work_dir;
    kimix::string_view exported_at; // ISO-8601 with seconds precision
    uint64_t token_count = 0;
    // Reserved: the reference build_export_markdown renders no per-message
    // timestamps (accepted for API stability; currently unused).
    bool include_timestamps = false;
};

// B9 (kimi_cli/soul/context_records.py::ExportedContext): the STRUCTURED
// export of a session - the system prompt, the messages, the checkpoint ids
// and the usage snapshots. The store (src/cli/cli_session.h) produces it in
// one read; this builder renders it to the byte-exact export markdown.
//
// The reference's build_export_markdown renders the history only (the meta
// fields are the structured projection, never extra markdown), so the output
// of build_export_context_markdown is byte-identical to
// build_export_markdown over ctx.messages.
struct export_context {
    bool has_system_prompt = false;
    kimix::string_view system_prompt;
    kimix::span<const soul::message_view> messages;
    kimix::span<const int64_t> checkpoints;
    kimix::span<const int64_t> usages;
};

// Render the full export markdown for `msgs` into `out` (LF line endings,
// byte-compatible with the reference when the same metadata is supplied).
KIMIX_RUNTIME_API void build_export_markdown(
    kimix::span<const soul::message_view> msgs,
    const export_options& opts,
    kimix::string& out) noexcept;

// B9: the structured-export entry point (see export_context).
KIMIX_RUNTIME_API void build_export_context_markdown(
    const export_context& ctx,
    const export_options& opts,
    kimix::string& out) noexcept;

} // namespace tools
} // namespace runtime
} // namespace kimix
