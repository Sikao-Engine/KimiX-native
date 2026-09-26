// agent/wire.h - The live wire.jsonl event stream (B7).
//
// Port of kimi_cli/wire/file.py + wire/types.py's record shapes onto an
// append-only native writer. The reference's wire.jsonl is an append-only
// stream: a {"type":"metadata","protocol_version":...} header line, then one
// {"timestamp": <float>, "message": {"type": <name>, "payload": {...}}} record
// per wire event AS IT HAPPENS (wire/file.py:155 _dump_line,
// WireMessageRecord.from_wire_message). The native CLI previously regenerated
// the whole file from history at save time with a single timestamp
// (cli_session.cpp; gap G11/B7) - this module replaces that with the live
// stream: the producers below emit the reference record kinds the native loop
// can produce natively (TurnBegin/TurnEnd, StepBegin, StepInterrupted,
// SteerInput, StepRetry, StatusUpdate, CompactionBegin/End and the LLMRequest
// observability hook for the later request recorder).
//
// Byte shapes: the payload objects follow the pydantic field order of
// wire/types.py with None fields serialized as null (model_dump without
// exclude_none), so records stay byte-compatible with the Python reader.
//
// WireSink is the soul's view of the stream (so tests can capture records
// without a file); WireWriter is the append-only file sink: one record per
// line, a single fwrite per record, per-record wall-clock timestamps,
// optional fields as explicit nulls, and append-after-reopen semantics (the
// existing header/protocol version is preserved like WireFile.__post_init__).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>

#include <core/kimix_core.h>

#include "llm/llm.h" // kimix::llm::Tool (G10/G11 record payloads)
#include "yyjson.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// G10: the LLMRequest observability record (wire/types.py LLMRequest)
// ---------------------------------------------------------------------------

// One outbound LLM request trace. Optional reference fields use sentinels:
// an empty string serializes as null (provider/thinking_effort/system_prompt),
// max_tokens/dropped_count/turn_step < 0 serialize as null. `system_prompt` is
// the inline copy, emitted only on the first occurrence of
// `system_prompt_hash` (the recorder's dedup); empty == null.
struct llm_request_record {
    kimix::string kind;                // "loop" | "compaction"
    kimix::string provider;            // the chat provider name, e.g. "kimi" ("" == null)
    kimix::string model;
    kimix::string thinking_effort;     // "" == null
    bool has_temperature = false;
    double temperature = 0.0;
    bool has_top_p = false;
    double top_p = 0.0;
    int64_t max_tokens = -1;           // < 0 == null (the effective output cap)
    kimix::string system_prompt_hash;  // sha256 hex of the system prompt
    kimix::string system_prompt;       // inline once per unique hash ("" == null)
    kimix::string tools_hash;          // references the LLMToolsSnapshot
    int32_t message_count = 0;
    int32_t turn_step = -1;            // < 0 == null (None for compaction)
    int32_t attempt = 1;               // 1-based across retries of one step
    int32_t dropped_count = -1;        // < 0 == null (compaction only)
};

// ---------------------------------------------------------------------------
// WireSink - what KimiSoul emits into
// ---------------------------------------------------------------------------

class WireSink {
public:
    virtual ~WireSink() = default;
    // TurnBegin {user_input: str}
    virtual void wire_turn_begin(kimix::string_view user_input) = 0;
    // TurnEnd {}
    virtual void wire_turn_end() = 0;
    // StepBegin {n: int}
    virtual void wire_step_begin(int32_t n) = 0;
    // StepInterrupted {}
    virtual void wire_step_interrupted() = 0;
    // SteerInput {user_input: str}
    virtual void wire_steer_input(kimix::string_view user_input) = 0;
    // StepRetry {n, next_attempt, max_attempts, wait_s, error_type,
    // status_code?} - status_code <= 0 serializes as null.
    virtual void wire_step_retry(int32_t n, int32_t next_attempt,
                                 int32_t max_attempts, double wait_s,
                                 kimix::string_view error_type,
                                 int32_t status_code) = 0;
    // StatusUpdate: context usage snapshot + this step's provider-measured
    // token usage (kosong TokenUsage field order: input_other, output,
    // input_cache_read, input_cache_creation). Token counts < 0 serialize as
    // null/absent fields per the reference snapshot semantics.
    virtual void wire_status_update(double context_usage, int64_t context_tokens,
                                    int64_t max_context_tokens, int64_t input_other,
                                    int64_t output, int64_t input_cache_read,
                                    int64_t input_cache_creation) = 0;
    // CompactionBegin {compaction_id, trigger, shadowed_tokens?} (-1 == null).
    virtual void wire_compaction_begin(kimix::string_view compaction_id,
                                       kimix::string_view trigger) = 0;
    // CompactionEnd {compaction_id, trigger, shadowed_tokens?,
    // estimated_token_count?, error?} (-1 == null, empty error == null).
    virtual void wire_compaction_end(kimix::string_view compaction_id,
                                     kimix::string_view trigger,
                                     int64_t shadowed_tokens,
                                     int64_t estimated_token_count,
                                     kimix::string_view error) = 0;
    // G10: LLMRequest - one record per outbound request (loop step or
    // compaction) with the recorder-filled provider identity + hashes.
    virtual void wire_llm_request(const llm_request_record &record) = 0;
    // G10: LLMToolsSnapshot - content-addressed tool table, emitted once per
    // unique `hash` (the recorder's dedup decides).
    virtual void wire_llm_tools_snapshot(kimix::string_view hash,
                                         const kimix::vector<kimix::llm::Tool> &tools) = 0;
    // G10: MCPToolsDiscovered - verbatim MCP tools/list result (kept API; the
    // native tree has no MCP, so nothing calls this yet).
    virtual void
    wire_mcp_tools_discovered(kimix::string_view server_name, kimix::string_view hash,
                              const kimix::vector<kimix::llm::Tool> &tools,
                              const kimix::vector<kimix::string> &enabled_names,
                              const kimix::vector<kimix::string> &collisions) = 0;
    // G1/G2: ApprovalRequest on creation (approval_runtime/runtime.py
    // _publish_wire_request). The foreground synchronous port always carries
    // source_kind "foreground_turn"; the display block list is [] (the native
    // prompt renders the description line instead).
    virtual void wire_approval_request(kimix::string_view id,
                                       kimix::string_view tool_call_id,
                                       kimix::string_view sender,
                                       kimix::string_view action,
                                       kimix::string_view description) = 0;
    // G1/G3: ApprovalResponse on resolution (response: "approve" |
    // "approve_for_session" | "reject"; feedback is "" when none was given).
    virtual void wire_approval_response(kimix::string_view request_id,
                                        kimix::string_view response,
                                        kimix::string_view feedback) = 0;
    // G11: BtwBegin/BtwEnd pair bracketing one /btw side question.
    virtual void wire_btw_begin(kimix::string_view id,
                                kimix::string_view question) = 0;
    virtual void wire_btw_end(kimix::string_view id, kimix::string_view response,
                              kimix::string_view error) = 0;
};

// A fresh compaction transaction id (uuid.uuid4().hex analogue: 32 lowercase
// hex chars), generated before the compaction LLM call and pairing the
// CompactionBegin/End records (kimisoul.py:2164).
kimix::string new_compaction_id();

// ---------------------------------------------------------------------------
// WireWriter - append-only wire.jsonl file
// ---------------------------------------------------------------------------

class WireWriter : public WireSink {
public:
    // The wire protocol version written into a fresh file's metadata header
    // (kimi_cli/wire/protocol.py::WIRE_PROTOCOL_VERSION).
    static constexpr const char *kProtocolVersion = "1.11";

    WireWriter() = default;
    ~WireWriter() override { close(); }
    WireWriter(const WireWriter &) = delete;
    WireWriter &operator=(const WireWriter &) = delete;

    // Open `path` for appending: creates parent directories and writes the
    // metadata header when the file is missing or empty (WireFile.open,
    // wire/file.py:132-140). False + `error` on failure.
    bool open(const kimix::string &path, kimix::string &error);
    void close() noexcept;
    bool is_open() const noexcept { return _file != nullptr; }
    // The file this writer appends to ("" when closed/not opened).
    const kimix::string &path() const noexcept { return _path; }

    // Test hooks: pin the wall clock (per-record timestamps) and the
    // compaction-id generator (uuid4().hex analogue, 32 lowercase hex chars).
    kimix::function<double()> now;
    kimix::function<kimix::string()> compaction_id_gen;

    // WireSink implementation.
    void wire_turn_begin(kimix::string_view user_input) override;
    void wire_turn_end() override;
    void wire_step_begin(int32_t n) override;
    void wire_step_interrupted() override;
    void wire_steer_input(kimix::string_view user_input) override;
    void wire_step_retry(int32_t n, int32_t next_attempt, int32_t max_attempts,
                         double wait_s, kimix::string_view error_type,
                         int32_t status_code) override;
    void wire_status_update(double context_usage, int64_t context_tokens,
                            int64_t max_context_tokens, int64_t input_other,
                            int64_t output, int64_t input_cache_read,
                            int64_t input_cache_creation) override;
    void wire_compaction_begin(kimix::string_view compaction_id,
                               kimix::string_view trigger) override;
    void wire_compaction_end(kimix::string_view compaction_id,
                             kimix::string_view trigger, int64_t shadowed_tokens,
                             int64_t estimated_token_count,
                             kimix::string_view error) override;
    void wire_llm_request(const llm_request_record &record) override;
    void wire_llm_tools_snapshot(kimix::string_view hash,
                                 const kimix::vector<kimix::llm::Tool> &tools) override;
    void wire_mcp_tools_discovered(kimix::string_view server_name,
                                   kimix::string_view hash,
                                   const kimix::vector<kimix::llm::Tool> &tools,
                                   const kimix::vector<kimix::string> &enabled_names,
                                   const kimix::vector<kimix::string> &collisions) override;
    void wire_approval_request(kimix::string_view id, kimix::string_view tool_call_id,
                               kimix::string_view sender, kimix::string_view action,
                               kimix::string_view description) override;
    void wire_approval_response(kimix::string_view request_id,
                                kimix::string_view response,
                                kimix::string_view feedback) override;
    void wire_btw_begin(kimix::string_view id, kimix::string_view question) override;
    void wire_btw_end(kimix::string_view id, kimix::string_view response,
                      kimix::string_view error) override;

private:
    void emit(kimix::string_view type,
              const kimix::function<void(yyjson_mut_doc *, yyjson_mut_val *)>
                  &build_payload);
    double now_seconds() const;
    kimix::string new_compaction_id() const;

    std::mutex _mutex;
    std::FILE *_file = nullptr;
    kimix::string _path;
};

} // namespace kimix::agent
