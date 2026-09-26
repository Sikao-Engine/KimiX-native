// agent/llm_recorder.h - Request-trace recorder for outbound LLM requests (G10).
//
// Port of kimi_cli/soul/llm_request_recorder.py (read in full) onto the wire
// sink the native soul already emits into:
//   * one LLMRequest wire record per outbound request (loop step or
//     compaction) with provider identity, the system-prompt hash (+ the inline
//     prompt once per unique hash), the tools hash, message_count, turn_step,
//     attempt and the compaction-only dropped_count;
//   * LLMToolsSnapshot: content-addressed tool table (sha256 over the
//     canonicalized - sorted-keys - JSON of the {name, description,
//     parameters} schemas), emitted once per unique hash; LLMRequest records
//     reference it by tools_hash;
//   * restore_from(wire.jsonl): seed the dedup sets from an existing stream
//     on a resumed session so nothing durable is re-logged (the reference
//     reads raw payload fields by type string, never re-validating);
//   * record_mcp_discovery(): the verbatim MCP tools/list record API, kept
//     even though the native tree has no MCP yet (nothing calls it today).
//
// All emission paths never raise and never touch the request itself: the
// recorder is observability-only (persisted, never replayed to UI clients).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/wire.h"

namespace kimix::agent {

// The provider identity fields the recorder derives defensively from the chat
// backend (llm_request_recorder.py _provider_fields): the native LLM Config
// has no temperature/top_p knobs, so those stay absent (has_* == false) until
// a provider grows them.
struct recorder_provider_fields {
    kimix::string provider;        // Config.type, "" when unknown (-> null)
    kimix::string model;
    kimix::string thinking_effort; // "" when unknown (-> null)
    bool has_temperature = false;
    double temperature = 0.0;
    bool has_top_p = false;
    double top_p = 0.0;
    int64_t max_tokens = -1;       // < 0 -> null (the _KIMI_DEFAULT_MAX_TOKENS
                                   // fallback has no native equivalent: the
                                   // backend always knows its Config.max_tokens)
};

class LLMRequestRecorder {
public:
    LLMRequestRecorder() = default;

    // Seed the dedup sets from an existing wire.jsonl (a resumed session).
    // Reads each record's raw payload fields by "type" string and never
    // fails: a missing/corrupt file leaves the sets empty. `wire_path` may be
    // empty (no-op).
    void restore_from(kimix::string_view wire_path) noexcept;

    // Record one outbound LLM request (llm_request_recorder.py record(),
    // :106-135): emits the LLMToolsSnapshot when the tools hash is new, then
    // the LLMRequest (system_prompt inlined only on the first occurrence of
    // its hash). `system_prompt` is the EFFECTIVE prompt sent with `messages`;
    // `tools` the offered tool table; `turn_step` < 0 == null (compaction);
    // `dropped_count` < 0 == null (loop). Never raises; a null sink drops the
    // record silently (the request still goes out).
    void record(WireSink *sink, const recorder_provider_fields &provider,
                kimix::string_view system_prompt,
                const kimix::vector<kimix::llm::Tool> &tools, int32_t message_count,
                kimix::string_view kind = "loop", int32_t turn_step = -1,
                int32_t attempt = 1, int32_t dropped_count = -1) noexcept;

    // The verbatim MCP tools/list record (llm_request_recorder.py
    // record_mcp_discovery, :196-229): deduped per (server_name, hash), where
    // the hash covers tools + enabled_names + collisions. Kept as API even
    // though MCP is not ported (the task's G10 scope); never raises.
    void record_mcp_discovery(WireSink *sink, kimix::string_view server_name,
                              const kimix::vector<kimix::llm::Tool> &tools,
                              const kimix::vector<kimix::string> &enabled_names,
                              const kimix::vector<kimix::string> &collisions =
                                  {}) noexcept;

    // Test/introspection hooks: the dedup set sizes.
    size_t seen_tools_hashes() const noexcept { return _seen_tools_hashes.size(); }
    size_t seen_prompt_hashes() const noexcept { return _seen_prompt_hashes.size(); }
    size_t seen_mcp_discoveries() const noexcept { return _seen_mcp_discoveries.size(); }

private:
    kimix::unordered_set<kimix::string, kimix::string_hash> _seen_tools_hashes;
    kimix::unordered_set<kimix::string, kimix::string_hash> _seen_prompt_hashes;
    kimix::unordered_set<kimix::string, kimix::string_hash> _seen_mcp_discoveries;
};

// sha256 over canonicalized (sorted-keys) JSON - llm_request_recorder.py
// _hash_json. Implemented locally (recursive key sort + the vendored mbedtls
// SHA-256) so the agent lib needs no runtime-codec dependency.
kimix::string recorder_hash_json(kimix::string_view json);
kimix::string recorder_hash_text(kimix::string_view text);

} // namespace kimix::agent
