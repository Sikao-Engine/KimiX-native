// agent/soul.h - KimiSoul: a C++ port of kimi_cli/soul/kimisoul.py's core
// responsibilities - manage a conversation session, run agent turns (chat ->
// tool calls -> tool results -> repeat), parse/repair tool-call JSON, and
// compact the context through an LLM summarization prompt.
//
// Scope (deliberately minimal vs. the 2.5k-line Python reference):
//   * turn loop with a max-steps bound and tool-call execution through the
//     static ToolRegistry (src/builtin_tools/tool_registry.h)
//   * empty-input guard: a blank/whitespace-only prompt never starts a turn
//     (kimi_cli/soul/__init__.py::_user_input_is_empty + run_soul's guard)
//   * tool-call argument parsing with kimix::repair (json_repair) +
//     ToolParams::try_deserialize; malformed arguments become an error tool
//     message instead of killing the turn (mirrors check_message/repair flow)
//   * auto-compaction: should_auto_compact() on the estimated token count
//     before every step (fed max_tokens / tool_call_buffer_tokens like the
//     reference's _tool_call_buffer_tokens); manual compaction with an optional
//     custom instruction and a compaction prompt (compact.md port)
//   * compaction boundary + summary message exactly as the reference computes
//     them: compact::resolve_preserve_split (balanced tool pairing + Phase-6
//     primacy re-insertion) and compaction.py:626-653's summary message
//   * loop resilience (Phase 1): provider-anchored token ledger feeding the
//     trigger, [loop_control]-driven trigger inputs, per-step retry with
//     _RateLimitAwareWait + Retry-After, think-only output-budget escalation,
//     context-overflow detection with force-compact recovery, and bounded
//     automatic session restarts (agent/token_ledger.h, agent/step_retry.h,
//     agent/context_overflow.h, agent/errors.h)
// * loop detectors (Phase 1): per-turn repeated-tool-call guard with graded
// <system-reminder> reminders and force-stop (agent/tool_loop_guard.h),
// reasoning-present detector reset, the loop-recovery gate (<=3 plain-text
// recovery prompts, then a synthesized final answer so a turn never ends
// tool-only) and the verification gate (unfinished todos / unverified
// edits -> nudge + extra step, agent/verification_gate.h)
// * dynamic injections (G9): per-step provider collection with error
//   isolation, the <system-reminder> wrap + stale-reminder strip +
//   normalize_history, and the five reference providers behind their
//   loop_control gates (agent/dynamic_injection.h, agent/dynamic_injections/)
// * context pruning (Phase 4 part 2, rows D1/C14/D11): the ContextPruner
//   engine (agent/context_pruning.h), its per-step auto pass over the
//   LLM-visible history with prune_N archiving into the history index (D5),
//   the prune-before-compact arbitration (C14), and step-1 auto-retrieval
//   memory injection (agent/auto_retrieve.h)
// Not ported: hooks engine, wire protocol, notifications (Python-side
// orchestration around the same kernels).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/approval.h"
#include "agent/btw.h"
#include "agent/cancel.h"
#include "agent/compaction_ledger.h"
#include "agent/context_pruning.h"
#include "agent/dynamic_injection.h"
#include "agent/errors.h"
#include "agent/hooks_engine.h"
#include "agent/llm_recorder.h"
#include "agent/loop_control.h"
#include "agent/steer.h"
#include "agent/system_prompt.h"
#include "agent/token_ledger.h"
#include "agent/tool_loop_guard.h"
#include "agent/verification_gate.h"
#include "agent/wire.h"
#include "builtin_tools/compact_tool.h"
#include "builtin_tools/tool.h"
#include "llm/llm.h"
#include "runtime/index/history_index.h"
#include "runtime/index/sqlite_history_index.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// C12 - compaction style modes (compaction.py _MODE_GUIDANCE)
// ---------------------------------------------------------------------------

// The guidance text the compaction prompt carries for a user-facing style
// mode ("auto" / "retentive" / "balanced" / "aggressive" / "technical").
//
// NOTE (audit C12): the guidance TEXT lives in the compact kernel
// (builtin_tools::compact::mode_guidance, compact_tool.cpp - outside the
// agent module's scope) and is appended to the prompt by
// build_compaction_prompt, exactly like the reference's _build_prompt_text
// (compaction.py:397-432). This adapter is the src/agent-side mode -> guidance
// mapping so agent-side callers never re-derive it: "auto" and the empty
// string are the balanced default (compaction.py CompactMode.BALANCED, the
// CompactionOptions default), anything else resolves through the kernel's
// parse_compact_mode. An unknown mode yields the balanced guidance (never
// empty text) so a style request can never silently drop the guidance block.
kimix::string_view compaction_style_guidance(kimix::string_view mode) noexcept;


// ---------------------------------------------------------------------------
// Chat backend abstraction (so the soul is unit-testable without network)
// ---------------------------------------------------------------------------

// Minimal chat interface the soul needs. kimix::llm::LLM satisfies it through
// LLMBackend below; tests inject a scripted fake.
class IChatBackend {
public:
    virtual kimix::string model_name() const = 0;
    // G8: `abort` is the check THIS call polls while streaming (the turn's
    // CancelToken OR'd with the steer wake event); it is scoped to the call,
    // never stored on the backend. The host must guarantee the pointer stays
    // valid until chat() returns - KimiSoul passes its turn-local composite,
    // so a background sub-agent's turn can never invalidate the check an
    // in-flight parent request is polling (the old set_abort_check() mutated
    // shared backend state and let the child soul's destruction dangle the
    // parent's stream mid-poll: 0xC0000005). nullptr == never abort.
    // Scripted test backends are synchronous and ignore it.
    virtual kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck *abort = nullptr) = 0;
    virtual int64_t max_context_size() const = 0;
    // G10: the provider identity fields the request recorder logs. Defaults
    // answer "unknown" (the scripted test backends have no config); LLMBackend
    // forwards its Config. `provider` mirrors the reference's
    // str(getattr(chat_provider, "name", ...)) - the config type, e.g.
    // "openai_legacy" | "anthropic".
    virtual kimix::string provider_name() const { return {}; }
    virtual kimix::string thinking_effort() const { return {}; }
    // temperature/top_p have no native Config knobs yet (the reference reads
    // _generation_kwargs); false == serialize as null.
    virtual bool generation_temperature_top_p(double &temperature,
                                              double &top_p) const {
        (void)temperature;
        (void)top_p;
        return false;
    }
      // The output-token budget the provider sends (the reference's
    // chat_provider._generation_kwargs['max_tokens']). The think-only retry
    // escalation (A8) raises it x1.5 from 8192; the default no-op backends
    // (test fakes) simply record it.
    virtual void set_output_token_budget(int64_t tokens) {
        (void)tokens;
    }
    virtual int64_t output_token_budget() const { return 0; }

    // ── A7: connection/auth recovery (kimisoul.py
    // _run_with_connection_recovery:2442-2523) ─────────────────────────────
    // Invoked ONCE per failed step, after the retry budget is exhausted, when
    // the error is retryable (connection / timeout / empty / 429 / 5xx) and
    // before the final give-up: the reference's RetryableChatProvider
    // .on_retryable_error(error) hook (reconnect the transport). Default
    // no-op (scripted test backends and providers with nothing to re-arm).
    virtual void on_retryable_error(const kimix::llm::ChatResult &error) {
        (void)error;
    }
    // The RetryableChatProvider marker analogue: only a backend that answers
    // true gets the one extra recovered attempt after on_retryable_error()
    // (the reference raises when the provider is not retryable). Default
    // false - the native providers open a fresh connection per request and
    // have nothing to re-arm, so the hook alone cannot change the outcome.
    virtual bool supports_retryable_recovery() const noexcept { return false; }
    // Invoked ONCE per failed step on a 401/403 to re-arm the credentials
    // (the reference's oauth.ensure_fresh(force=True) branch). True when the
    // credentials were refreshed and the failed step is retried once more
    // OUTSIDE the retry budget; false leaves the failure standing. Default
    // false: plain API-key providers have nothing to refresh.
    virtual bool refresh_auth() { return false; }
};

// Adapter over the concrete kimix::llm::LLM facade (create_llm_from_file).
class LLMBackend : public IChatBackend {
public:
    explicit LLMBackend(kimix::unique_ptr<kimix::llm::LLM> llm);
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck *abort = nullptr) override;
    int64_t max_context_size() const override;
    kimix::string model_name() const override;
    void set_output_token_budget(int64_t tokens) override;
    int64_t output_token_budget() const override;
    kimix::string provider_name() const override;
    kimix::string thinking_effort() const override;
    bool generation_temperature_top_p(double &temperature, double &top_p) const override;
    // A7: re-arms the credentials through the Config-level refresh callback
    // (kimix::llm::Config::auth_refresh, unset by default - no real OAuth
    // flows are implemented; a host installs the callback when its provider
    // uses OAuth).
    bool refresh_auth() override;

private:
    kimix::unique_ptr<kimix::llm::LLM> _llm;
};

// ---------------------------------------------------------------------------
// Agent session
// ---------------------------------------------------------------------------

// One conversation session: identity, working directory, message history and
// the builtin_tools::Session the tools receive (native_io enabled).
class AgentSession {
public:
    AgentSession(); // generates a random hex id
    explicit AgentSession(kimix::string work_dir);
    ~AgentSession(); // closes the durable history index when open

    const kimix::string &id() const { return _id; }
    const kimix::string &work_dir() const { return _tool_session.work_dir; }
    void set_work_dir(kimix::string dir);

    // Directory of the persisted session state (state.json). Empty ==
    // in-memory only. Setting it lets the todo tools save/load their list
    // with the session (builtin_tools::todo::session_todos).
    const kimix::string &state_dir() const { return _tool_session.state_dir; }
    void set_state_dir(kimix::string dir);

    // Persist the todo state to <state_dir>/state.json (kimi_cli
    // session.save_state parity). False + error when state_dir is unset or
    // the write fails.
    bool save_state(kimix::string &error) const;
    // (Re)load the todo state from <state_dir>/state.json into the in-memory
    // cache. A missing file loads an empty state; a corrupt file fails.
    bool load_state(kimix::string &error);

    kimix::vector<kimix::llm::Message> &history() { return _history; }
    const kimix::vector<kimix::llm::Message> &history() const { return _history; }

    // Append one message to the history AND feed the session's in-memory
    // history index - the reference's context._on_append -> HistoryIndex
    // .index_messages wiring (kimisoul.py:420-423, history_index.py:295+).
    // Only user/assistant/tool messages with non-blank text become turns.
    void append_history(const kimix::llm::Message &message);

    // Post-compaction re-index: mark every indexed turn compacted and index
    // the current history verbatim - the reference's mark_compacted() +
    // append_message(compaction_result.messages) sequence, which re-indexes
    // the summary + preserved tail as fresh turns.
    void on_history_compacted();

    // Feed every current history message to the index without touching the
    // history vector - used when a persisted history is loaded into a FRESH
    // session (the CLI's /resume rebind) so retrieve sees the resumed turns.
    void reindex_history();

    // The session's in-memory history index backing the retrieve tool.
    // Active only when no durable index is open (the fallback for hosts and
    // tests that never set a history.db path).
    kimix::runtime::index::HistoryIndex &history_index() { return _history_index; }
    const kimix::runtime::index::HistoryIndex &history_index() const {
        return _history_index;
    }

    // D4(durable): open the durable SQLite FTS5 history index (rows
    // D3/D6/D7/D8/D9 of report.md section D). `db_path` is the history.db
    // path (typically <session work dir>/history.db); the file and parent
    // directories are created on demand. On success every append_history /
    // on_history_compacted / reindex_history call feeds the durable index
    // and the retrieve tool's view is served from SQLite; the in-memory
    // index stays empty as the no-path fallback. On failure the session
    // keeps running on the in-memory index and false is returned.
    bool open_history_index(kimix::string_view db_path, kimix::string &error);
    // Durable mode is active (open_history_index succeeded and not closed).
    bool has_durable_history_index() const noexcept {
        return _sqlite_history_index != nullptr;
    }
    // Save + close the durable index (session teardown). No-op when the
    // in-memory fallback is in use. Safe to call twice.
    void close_history_index() noexcept;

    // History search dispatch used by the retrieve tool view: serves from
    // the durable SQLite index when open, otherwise the in-memory kernel.
    // Returned turns carry the raw verbatim text (D7) and the bm25 score.
    kimix::vector<kimix::runtime::index::turn_meta>
    history_search(kimix::string_view query, uint32_t top_k);
    // D11 auto-retrieval dispatch: recency-boosted search (the durable index
    // implements search_with_recency; the in-memory fallback applies the same
    // boost formula over a top_k*3 pool). Each turn carries the raw bm25
    // score and the boosted value in turn_meta::boosted_score.
    kimix::vector<kimix::runtime::index::turn_meta>
    history_search_with_recency(kimix::string_view query, uint32_t top_k,
                                double recency_weight);
    // D11: ids of every indexed turn that is NOT marked compacted (the
    // reference's _history_index._turns scan behind the last-2 exclusion).
    kimix::vector<uint32_t> non_compacted_turn_ids() const;
    // get_by_id dispatch (the "prune_N" prefix strip is the caller's job).
    kimix::optional<kimix::runtime::index::turn_meta>
    history_get_by_id(uint32_t turn_id) const;

    // D5 - prune_N turn-id authority: reserve the next history-index turn id
    // for an elided-original archive row (the id the context-elided stub
    // names). In-memory fallback: the session's own counter.
    uint32_t reserve_elided_turn_id() noexcept;
    // D5: write one archived elided original under `turn_id` (reserved via
    // reserve_elided_turn_id()) so retrieve id=prune_N resolves it. The role
    // is the wire role string ("tool"); blank originals are skipped.
    void archive_elided_original(uint32_t turn_id, kimix::string_view role,
                                 kimix::string_view text);

    // The builtin_tools::Session passed to every tool instance.
    builtin_tools::Session &tool_session() { return _tool_session; }
    const builtin_tools::Session &tool_session() const { return _tool_session; }

private:
    // Feed a single message to the history index (index_messages).
    void index_history_message(const kimix::llm::Message &message);

    kimix::string _id;
    kimix::vector<kimix::llm::Message> _history;
    builtin_tools::Session _tool_session; // work_dir + native_io = true
    kimix::runtime::index::HistoryIndex _history_index;
    // D4(durable): non-null while the durable SQLite history index is open;
    // it OWNS the turn-id counter (re-synced from MAX(turn_id)+1 on open).
    kimix::unique_ptr<kimix::runtime::index::SqliteHistoryIndex>
        _sqlite_history_index;
    uint32_t _next_turn_id = 0; // history_index.py _doc_id_counter
                                // (in-memory fallback only; the durable
                                // index assigns ids itself)
};

// ---------------------------------------------------------------------------
// KimiSoul
// ---------------------------------------------------------------------------

// Outcome of one full turn (one user input -> final assistant text).
struct TurnResult {
    bool ok = false;
    kimix::string content;       // final assistant text
    kimix::string error;         // set when the turn aborted
    int32_t steps = 0;           // LLM round-trips performed
    bool compacted = false;      // a compaction happened during the turn
    // Typed failure taxonomy (agent/errors.h): which reference exception this
    // failure ports. `none` on success (or on an ignored empty input).
    TurnErrorKind error_kind = TurnErrorKind::none;
    // Set for max_steps_reached: the cap that was hit, and the reference
    // wording "Max number of steps reached: {n}" in `error`.
    int32_t max_steps = 0;
    // Set for session_restart_exhausted: how many automatic session restarts
    // were performed before giving up (kimisoul.py / _session.py).
    int32_t session_restarts = 0;
    // The input carried no sendable content (empty / whitespace-only) so no turn
    // was started: the history was not touched and no LLM call was made. Mirrors
    // kimi_cli.soul.run_soul's empty-input guard (soul/__init__.py:329-341);
    // without it the soul appends an empty `user` message and the model answers
    // a spurious "you sent an empty message" turn.
    bool ignored = false;
    // G8: the turn was cancelled mid-flight (TurnErrorKind::cancelled). The
    // step aborted at the next step boundary, the streaming request was
    // interrupted, no partial assistant message was grown into the history,
    // and the session is kept - the caller surfaces the reference's
    // "Keyboard Interrupt." warning.
    bool cancelled = false;
};

// True when a user turn input carries no sendable content, i.e. it is empty or
// whitespace-only. Port of kimi_cli/soul/__init__.py::_user_input_is_empty
// (which also guards KimiSoul.steer / request_steer); the C++ soul takes plain
// text, so only the string branch applies. ASCII whitespace + the vertical-tab /
// form-feed pair Python's str.strip() also treats as whitespace.
bool agent_user_input_is_empty(kimix::string_view user_input) noexcept;

// Streaming callback: text deltas / reasoning deltas / tool-call notices.
using SoulEventCallback = kimix::function<void(const kimix::llm::Chunk &)>;

class KimiSoul {
public:
    struct options {
        kimix::string system_prompt;    // "" -> default agent prompt
        // Default-prompt plumbing (agent/system_prompt.h): which role's prompt
        // to build and the inputs of build_system_prompt(). prompt_role::worker
        // + yolo=true + shell_tool="bash" reproduce the Python reference's
        // defaults; skills_text maps to the pre-formatted KIMI_SKILLS block.
        system_prompt_role prompt_role = system_prompt_role::worker;
        bool yolo = true;
        kimix::string skills_text;
        kimix::string shell_tool = "bash";
        int32_t max_steps = 15000;      // per-turn LLM round-trip bound
                                        // (LoopControl.max_steps_per_turn default)
        bool auto_compact = true;       // compact when the context grows
        double auto_compact_ratio = 0.8; // LoopControl.compaction_trigger_ratio default
        int64_t reserved_context = 75000; // LoopControl.reserved_context_size default
        // Mirrors kimi_cli's LoopControl.max_tokens / _tool_call_buffer_tokens():
        // both feed should_auto_compact's reserved-output boundary, so leaving
        // them at 0 under-fires the reserved-based trigger.
        int64_t max_tokens = 0;              // model output budget (None -> 0)
        int64_t tool_call_buffer_tokens = 0; // dynamic per-tool output budget
        // Extra tokens reserved for unforeseen growth in the compaction
        // trigger (compaction.py SAFETY_MARGIN_TOKENS). No LoopControl field
        // exists for it in the reference either - it is a module constant.
        int64_t safety_margin_tokens = 1024;
        // Test hooks for the step-retry policy (agent/step_retry.h): replace
        // the sleeper (tests record delays instead of really waiting) and pin
        // the jitter RNG for deterministic _RateLimitAwareWait values. An
        // empty sleep function keeps the real sleeper.
        kimix::function<void(double seconds)> step_retry_sleep;
        uint64_t step_retry_jitter_seed = 0x9E3779B97F4A7C15ull;
        // Preserve-depth bounds for the compaction boundary, mirroring
        // LoopControl.min_preserved_messages (1) / max_preserved_messages (2).
        // The reference resolves its SimpleCompaction preserve_depth through
        // adaptive_preserve_depth(msgs, min_preserved=1, max_preserved=2).
    int32_t min_preserved_turns = 1;
    int32_t max_preserved_turns = 2;
    // C13 (compaction.py Phase 2, SimpleCompaction.compact's
    // aligned_system_prompt): when set, the compaction LLM call replays the
    // conversation's real system prompt (the same serialized prompt as the
    // live turns), the real tool schemas and the contiguous to_compact region
    // VERBATIM, appending only the compaction instruction as the final user
    // message - so the provider's cacheable request prefix is identical to the
    // main loop's up to the compaction point. false falls back to the legacy
    // flattened transport (one user message, generic system prompt, no tools).
    // The reference uses the aligned path for every KimiToolset soul, so the
    // default here is on.
    bool compact_aligned_transport = true;
        // The parsed [loop_control] section (agent/loop_control.h), threaded
        // in by the CLI. This is the AUTHORITATIVE source the loop reads
        // (retry budget, compaction trigger ratio / reserved context /
        // overflow recovery); the legacy numeric fields above are reconciled
        // against it in the KimiSoul constructor so older callers that set
        // them directly (tests, demos) keep working: a legacy value left at
        // its LoopControl default defers to loop_control, an explicitly
        // changed legacy value wins and is written back into loop_control.
        LoopControl loop_control;
        kimix::vector<kimix::string> enabled_tools; // empty == all registered
        // Read-only mode (the reference's runtime.read_only, toolset.py:93-105
        // + 1347-1362): the 10 mutating tools of _READ_ONLY_BLOCKED_TOOLS are
        // refused at dispatch with the reference's exact refusal text.  Tools
        // that only read (read/grep/glob/fetch_url/retrieve/...) still run.
        bool read_only = false;
    };

    KimiSoul(AgentSession &session, IChatBackend &backend);
    ~KimiSoul();
    KimiSoul(AgentSession &session, IChatBackend &backend, options opts);

    const AgentSession &session() const { return _session; }
    AgentSession &session() { return _session; }

    // Tools offered to the model, derived from the static ToolRegistry
    // (registry key -> definition). Rebuilt on demand; cheap.
    //
    // Validity gate: every candidate is constructed through the instance cache
    // and asked Tool::valid(); a tool that answers false is skipped here, so
    // its definition never reaches the LLM backend (no python interpreter ->
    // no `python` tool, no Git Bash on Windows -> no `bash` tool). When the
    // bash tool is dropped, tool_offered() adopts pwsh in its place even if the
    // manifest did not list it: the agent must keep one usable shell.
    kimix::vector<kimix::llm::Tool> tool_definitions() const;

    // The registry names the manifest asked for but this environment cannot
    // run - i.e. the tools tool_definitions() dropped because their
    // Tool::valid() answered false - in registry order. Nothing the model sees
    // depends on it; it exists so a host can explain a missing tool instead of
    // silently dropping it.
    kimix::vector<kimix::string> unavailable_tools() const;

    // Run one user turn: append the user message, then loop
    // chat -> execute tool calls -> append tool results until the model
    // answers with plain text or max_steps is hit. `on_event` receives the
    // streamed chunks (may be empty).
    TurnResult turn(kimix::string_view user_input,
                    const SoulEventCallback &on_event = {});
    // G8: the same turn loop driven by a caller-owned cancellation token (the
    // reference's run_soul(cancel_event)): cancel() from another thread (a
    // Ctrl-C handler owns the token's raw flag) aborts the turn at the next
    // step boundary AND interrupts the in-flight streaming request through
    // the provider's abort hook. The result reports cancelled=true and the
    // history keeps everything up to the last completed step (the cancelled
    // step's partial output is never appended).
    TurnResult turn(kimix::string_view user_input,
                    const SoulEventCallback &on_event, const CancelToken &cancel);
      // Sub-agent settle notices (bug_tool.md item 10: the tool description
      // promises "when that run settles, the runtime sends the parent a notice
      // containing its outcome" - nothing did). drain_finished_subagent_notices
      // collects every settled background run from the session's agent registry
      // (applying each run's close_session choice) and queues a formatted
      // notice; turn() flushes the queue as user messages before the next user
      // input. Returns the notices queued by THIS call. Hosts may call it
      // between turns to poll for outcomes.
      kimix::vector<kimix::string> drain_finished_subagent_notices();


    // G7 steering (kimi_cli/soul/steer.py + kimisoul.py:1007-1030).
    // Queue a steer message for injection into the current turn. Step-boundary
    // only: consumed between steps (or before turn end), never mid-stream.
    // Blank content is dropped. When no turn is running the steer is queued
    // and discarded as stale at the next turn init (the reference behavior).
    void steer(kimix::string_view content);
    // Interrupting steer: additionally wakes the loop so the currently
    // streaming step is interrupted; the steers are injected as follow-up
    // user messages and a fresh step starts with them in context
    // (kimisoul.py:1276-1305). Never interrupts between turns.
    void request_steer(kimix::string_view content);
    // Steer::push_sync backing: enqueue + block the calling thread until the
    // steer is consumed or the running turn ends, or `timeout_s` elapses
    // (<= 0 == no timeout). True when consumed.
    bool push_steer_sync(kimix::string_view content, double timeout_s);
    size_t pending_steers() const;
    void clear_steers();
    // True while a turn is running (steer.py's soul.is_running()).
    bool is_running() const noexcept { return _turn_cancel != nullptr; }

    // G7 sub-agent wiring: an external steer source polled at every step
    // boundary alongside the internal queue - the production sub-agent runner
    // installs registry->drain_steer(session_id) here so send_message pushes
    // actually reach a running child turn.
    void set_external_steers(
        kimix::function<kimix::vector<kimix::string>()> source) {
        _external_steers = std::move(source);
    }

    // B7: attach the live wire.jsonl event stream (the session's WireWriter).
    // Not owned; cleared by the caller before the writer is destroyed. Null ==
    // no stream (the soul works exactly as before). The approval gate's sink
    // follows it so request/response records join the same stream.
    void set_wire_sink(WireSink *sink) noexcept;

    // G1-G4: the approval gate consulted by execute_tool_call before every
    // gated (edit/write-family) tool runs. Borrowed, not owned - the CLI owns
    // it across soul rebinds (so approve-for-session grants and yolo/afk
    // survive /resume); null == dispatch is ungated (tests, hosts that do not
    // install a gate). The gate also receives the soul's wire sink so
    // ApprovalRequest/ApprovalResponse records pair up on the stream.
    void set_approval(Approval *approval) noexcept;
    Approval *approval() const noexcept { return _approval; }

    // G10: the request-trace recorder (llm_request_recorder.py). Wired into
    // every outbound request (loop steps + compaction) once a wire sink
    // exists; restore_from() re-seeds its dedup sets from a resumed
    // session's wire.jsonl.
    LLMRequestRecorder &request_recorder() noexcept { return _recorder; }
    const LLMRequestRecorder &request_recorder() const noexcept { return _recorder; }
    void restore_request_recorder(kimix::string_view wire_jsonl_path) noexcept {
        _recorder.restore_from(wire_jsonl_path);
    }

    // G11: answer a /btw side question (btw.py execute_side_question): a
    // separate lightweight conversation over the same system prompt +
    // normalized history with the real tool table declared (cache stability)
    // but every call denied. Nothing is written to the main history; the
    // BtwBegin/BtwEnd wire pair brackets the run. `on_text` receives the
    // streamed answer text deltas.
    SideQuestionResult run_side_question(kimix::string_view question,
                                         const SoulEventCallback &on_text = {});

    // Compact the conversation history through the backend: slice off the
    // preserved tail (balanced on tool-call pairs), build the compaction
    // prompt (compact:: kernels + the ported compact.md body), summarize with
    // one LLM call and replace the head with the summary. Returns false and
    // fills `error` when there is nothing to compact or the call fails.
    //
    // Phase 3 transaction (compaction.py / kimisoul.py): the conversation
    // surface is fingerprinted around the LLM call - a mid-call mutation
    // (SurfaceChangedError analogue) retries the whole attempt once, then
    // fails with the classified "changed" error (manual) or a plain report
    // (auto).  A summary that is not smaller than the region it replaces is
    // discarded and the history is left verbatim (a no-op compaction, not an
    // error).  `manual` mirrors the reference's compact_context(manual=...):
    // true for /compact-style user requests, false for the auto trigger.
    bool compact_context(kimix::string_view custom_instruction,
                         kimix::string &error, bool manual);

    // Overflow-recovery variant (context_overflow.py / kimisoul.py:1815-1882):
    // force-compact the context after a provider-confirmed overflow, bypassing
    // the should_auto_compact trigger entirely - AGGRESSIVE mode with a fixed
    // preserve depth (context_overflow_preserve_depth) instead of the adaptive
    // one. `trigger` labels the attempt for diagnostics (the reference passes
    // trigger_override="overflow" into the compaction ledger).
    bool compact_context(kimix::string_view custom_instruction,
                         kimix::string &error, bool manual,
                         builtin_tools::compact::CompactMode mode,
                         int32_t preserve_depth_override,
                         kimix::string_view trigger);

    // Estimated token count of the full next-request input. Prefers the
    // provider-measured usage recorded in the token ledger
    // (token_count_with_pending: recorded usage + the incremental estimate of
    // messages appended since, mirroring context.py); falls back to the char
    // heuristic (history text + effective system prompt + every offered
    // tool's name/description/parameters) while nothing has been recorded.
    // Drives both the CLI context usage and the should_auto_compact trigger.
    int64_t estimated_tokens() const;

    // The session's provider-anchored token ledger (B1): recorded usage after
    // every successful step plus the pending estimate of in-flight growth.
    const TokenLedger &token_ledger() const { return _ledger; }

    // Resume seeding (context.py restore(): `self._token_count = latest_usage`):
    // a resumed session anchors its recorded count on the persisted _usage
    // snapshot until the next provider response supersedes it, so the context
    // usage readout and the compaction trigger start from the real measurement
    // instead of falling back to the char heuristic. No pending estimate is
    // reconstructed (the native store keeps usage rows separately, so the
    // messages-after-last-usage boundary the reference walks is not carried);
    // the next successful step re-anchors the pair.
    void seed_token_ledger(int64_t recorded_usage_input) noexcept {
        _ledger.update_token_count(recorded_usage_input);
    }

    // Number of compactions performed so far.
    int32_t compaction_count() const { return _compactions; }

    // C10: the durable compaction transaction ledger
    // (kimisoul._compaction_ledger). Built in the constructor from the
    // session directory (state_dir, else work_dir) behind
    // [loop_control] compaction_ledger_enabled; a disabled ledger or an
    // unusable directory degrades to the no-op ledger (path empty).
    const CompactionLedger &compaction_ledger() const noexcept {
        return _compaction_ledger;
    }
    // C10: the ledger records loaded on session open (telemetry view; the
    // reference's ledger is write-only, this is the read-back the audit asks
    // for). Snapshotted at construction; compaction appends update it.
    const kimix::vector<CompactionRecord> &compaction_records() const noexcept {
        return _compaction_records;
    }

    // C13 (compaction.py:141-176 CompactionResult.estimated_token_count_for_model):
    // post-compaction token estimate. When the compaction LLM call reported a
    // usage (`usage_output` set) and there is at least one message, the exact
    // generated-summary token count is combined with an estimate of the
    // preserved tail; otherwise every message is estimated from its text.
    static int64_t estimated_token_count_for_model(
        const kimix::vector<kimix::llm::Message> &messages,
        kimix::optional<int64_t> usage_output);

    // G9: register an additional dynamic injection provider
    // (kimisoul.add_injection_provider, kimisoul.py:688-690). The five
    // reference providers are registered by the constructor when their
    // loop_control.*_enabled gates are on; this is the extension point.
    void add_injection_provider(kimix::unique_ptr<DynamicInjectionProvider> provider);

    // G9: notify providers that afk mode changed (kimisoul.notify_afk_changed,
    // kimisoul.py:899-910). The native CLI has no /afk yet (Phase 3) - the
    // hook is exposed so the call site can land later.
    void notify_afk_changed(bool enabled);

    // G9: the per-session turn counter (providers' turn-id analogue).
    uint64_t turn_sequence() const noexcept { return _turn_seq; }

    // ── Phase 4 (part 2): context pruning engine (rows D1/C14/D11) ───────────
    // D1: the loop_control-configured context pruner (kimisoul._pruner). The
    // /prune command and the context_prune tool drive it through here; the
    // turn loop runs its auto pass and the prune-before-compact arbitration
    // through the private wiring.
    ContextPruner &pruner() noexcept { return _pruner; }
    const ContextPruner &pruner() const noexcept { return _pruner; }
    // D12: the current step number of the running turn (0 between turns;
    // kimisoul._current_step_no, fed to the pruner's cooldown check).
    int32_t current_step_no() const noexcept { return _current_step_no; }
    // The backend's context window (llm.max_context_size).
    int64_t max_context_size() const { return _backend.max_context_size(); }
    // D2: true when the active provider requires the reasoning back-pass
    // (the reference's soul.thinking flag; strip_reasoning keeps empty
    // thinking parts when this is true).
    bool thinking_active() const { return !_backend.thinking_effort().empty(); }
    // The authoritative [loop_control] section (the /prune command reads
    // context_pruning_enabled / prune_subagents through this).
    const LoopControl &loop_control() const noexcept { return _opts.loop_control; }
    // D2: apply a pruned history from the context_prune tool: replaces the
    // history, archives the elided originals (D5), reanchors the token
    // ledger and emits the StatusUpdate wire refresh (G41) when a wire is
    // attached. `elided` may be empty (strip_reasoning mode).
    void apply_pruned_history(kimix::vector<kimix::llm::Message> messages,
                              const kimix::vector<elided_record> &elided);
    // D5: reserve the next history-index turn id and return the "prune_N"
    // reference string naming it. Public for the context_prune tool, which
    // produces its own refs when driving prune_with_policy.
    kimix::string alloc_prune_ref();

    // G13 (toolset.py hide/unhide 1154-1163): drop a tool from the LLM tool
    // list while keeping it callable - a capability negotiated mid-session
    // (the reference hides/unhides AskUserQuestion per client support).
    // True when the tool exists (the reference returns bool from hide()).
    bool hide_tool(kimix::string_view name);
    // Restore a hidden tool to the LLM tool list (no-op when absent).
    void unhide_tool(kimix::string_view name);

    // F6 (toolset.py set_hook_engine 1064-1066): replace the lifecycle hook
    // engine (null resets to the built-in empty one). The engine is consulted
    // around every execute_tool_call (PreToolUse blocking, PostToolUse /
    // PostToolUseFailure fire-and-forget).
    void set_hook_engine(kimix::shared_ptr<hooks::HookEngine> engine) noexcept {
        _hook_engine = std::move(engine);
    }
    hooks::HookEngine &hook_engine() noexcept { return *_hook_engine; }
    const hooks::HookEngine &hook_engine() const noexcept { return *_hook_engine; }

    // Execute one tool call by name with raw JSON arguments (repair +
    // parse + dispatch). Exposed for tests; returns the tool message content.
    // `tool_call_id` is the wire id of the in-flight call (empty for direct
    // test invocations); it labels the ApprovalRequest record when the call
    // is approval-gated.
    kimix::string execute_tool_call(kimix::string_view name,
                                    kimix::string_view arguments_json,
                                    kimix::string &error,
                                    kimix::string_view tool_call_id = {}) {
        return execute_tool_call(name, arguments_json, error, tool_call_id,
                                 nullptr);
    }
    // The same dispatch with an out-parameter describing what the dispatcher
    // decided (F8 resolution, F11 canonical key, F10 pure rejection). The
    // turn loop uses it for the same-step duplicate short-circuit and the
    // rejection-stops-turn rule.
    struct ToolDispatchInfo {
        kimix::string resolved_name;  // canonical registry name after F8
        kimix::string canonical_args; // sorted-key canonical argument JSON
        bool corrected = false;       // F8 auto-corrected the name
        bool external = false;        // F14 host-answered tool
        // F10 (kimisoul.py:1983-1993): an approval rejection with no user
        // feedback - a root soul stops the turn with stop_reason
        // "tool_rejected" (sub-agents continue so the model can retry).
        bool pure_rejection = false;
    };
    kimix::string execute_tool_call(kimix::string_view name,
                                    kimix::string_view arguments_json,
                                    kimix::string &error,
                                    kimix::string_view tool_call_id,
                                    ToolDispatchInfo *info);
    // The same dispatch collecting any media parts the tool produced
    // (E1/E2 media out: read_image's data_url becomes a real ContentPart
    // image_url part riding on the tool message).
    kimix::string execute_tool_call(kimix::string_view name,
                                    kimix::string_view arguments_json,
                                    kimix::string &error,
                                    kimix::string_view tool_call_id,
                                    ToolDispatchInfo *info,
                                    kimix::vector<kimix::llm::ContentPart> *media_parts);

private:
    AgentSession &_session;
    IChatBackend &_backend;
    options _opts;
    int32_t _compactions = 0;
    // G8/G7: the running turn's outside handles (null between turns).
    // `_turn_abort` is what the LLM backend polls while streaming: the caller's
    // cancel token OR'd with the steer-queue wake flag, so both Ctrl-C
    // cancellation and a mid-stream steer stop the HTTP read promptly.
    const CancelToken *_turn_cancel = nullptr;
    struct TurnAbortCheck final : kimix::llm::AbortCheck {
        const CancelToken *cancel = nullptr;
        const SteerQueue *queue = nullptr;
        bool aborted() const noexcept override {
            return (cancel != nullptr && cancel->cancelled()) ||
                   (queue != nullptr && queue->wake_peek());
        }
    };
    TurnAbortCheck _turn_abort;
    // G7: the soul-side steer queue (thread-safe; external threads push, the
    // turn drains it between steps / before turn end).
    SteerQueue _steer_queue;
      // G7: optional external steer source (the sub-agent registry drain).
      kimix::function<kimix::vector<kimix::string>()> _external_steers;
      // Settled sub-agent outcomes waiting for the next turn (formatted by
      // drain_finished_subagent_notices, flushed by turn() as user messages).
      kimix::vector<kimix::string> _pending_subagent_notices;
    // B7: the live wire.jsonl sink (not owned; null == no stream).
    WireSink *_wire = nullptr;
    // G1-G4: the approval gate (borrowed; null == ungated dispatch).
    Approval *_approval = nullptr;
    // G10: the outbound-request trace recorder (observability only).
    LLMRequestRecorder _recorder;
    // Provider-anchored token accounting (context.py _token_count +
    // _pending_token_estimate); see token_ledger.h and estimated_tokens().
    TokenLedger _ledger;
    // A1: per-turn repeated-tool-call loop detectors (toolset.py's KimiToolset
    // detector state machine). Consulted after every tool result; a trip
    // becomes stop_reason "tool_call_repeat".
    ToolLoopGuard _loop_guard;
    // A4: verification gate (verification_gate.py): unfinished todos /
    // unverified edits at a no_tool_calls stop produce a nudge user message
    // and one more step, bounded by loop_control.verification_gate_max_nudges.
    VerificationGate _verification_gate;
    // G9: dynamic-injection framework (agent/dynamic_injection.h + the five
    // providers in agent/dynamic_injections/): registered by the constructor
    // per the loop_control gates, collected before every step, and notified
    // on compaction / afk changes.
    InjectionRegistry _injections;
    // G9: per-session turn counter, bumped at every turn() start; the C++
    // turn-id analogue for providers with per-turn state (budget, churn).
    uint64_t _turn_seq = 0;
    // Phase 4 (part 2): context pruning engine (rows D1/C14/D11).
    // D1: the loop_control-configured pruner; its auto pass runs per step in
    // the request build, its estimate drives the prune-before-compact
    // arbitration (C14), and reset_cooldown() fires after a compaction.
    ContextPruner _pruner;
    // D12: the running turn's step number (kimisoul._current_step_no).
    int32_t _current_step_no = 0;
    // D11: the auto-retrieval dedup set (kimisoul._recently_retrieved_turn_ids),
    // capped at 10 by the auto-retrieval pass.
    kimix::set<uint32_t> _recently_retrieved_turn_ids;
    // C8 (kimisoul.py's compact_export_path): set when a compaction commits -
    // the deterministic pre-compaction export slot
    // <work_dir>/.kimix_cache/context_compacted.md. Advertised in the system
    // prompt (the "Pre-compaction context exported to:" item) until the
    // session ends; empty when the export failed or no compaction happened.
    kimix::string _compact_export_path;
    // The is_compacting companion flag (the Python prompt's is_compacting arg;
    // only compact_export_path changes the rendered text, this stays for API
    // parity with system_prompt_input).
    bool _compact_export_pending = false;
    // C10: the durable compaction transaction ledger + its open-time snapshot.
    CompactionLedger _compaction_ledger;
    kimix::vector<CompactionRecord> _compaction_records;
    // Tool instance cache (registry key -> instance). Mutable because it is
    // memoisation behind a const query: tool_definitions() has to construct a
    // tool to ask it whether it is valid. Instances of tools that answer
    // valid() == false are NOT cached, so a dependency that shows up later (a
    // sub-agent runner injected into the session, an interpreter installed
    // mid-session) is picked up by the next rebuild.
    mutable kimix::unordered_map<kimix::string, kimix::unique_ptr<builtin_tools::Tool>,
                                 kimix::string_hash>
        _tools; // cached instances by registry key
    // G13: the hidden set (toolset.py _hidden_tools) - filtered from
    // tool_definitions() only, never from dispatch.
    kimix::set<kimix::string> _hidden_tools;
    // F6: the lifecycle hook engine (a default empty one until replaced).
    kimix::shared_ptr<hooks::HookEngine> _hook_engine{
        kimix::shared_ptr<hooks::HookEngine>(new hooks::HookEngine())};

    // The cached instance for `name` (fuzzy resolution), created on demand,
    // or null when the name is unknown or the tool is not valid here.
    builtin_tools::Tool *get_tool(kimix::string_view name) const;
    // The registry names dispatch/the LLM tool list may see: enabled by the
    // manifest, valid() here, plus the adopted shell fallback and every
    // runtime-registered external tool (the reference's _tool_dict keys).
    kimix::vector<kimix::string> offered_tool_names() const;

    // ── F11 dispatch split ─────────────────────────────────────────────
    // execute_tool_call = prepare + finish. The turn loop splits them so a
    // same-step duplicate (same resolved name + canonical args) reuses the
    // in-flight/last result BEFORE any hook / approval / tool run, exactly
    // like the reference's _current_step_tasks check (toolset.py:1409-1419).
    struct ToolDispatchPlan;
    // Resolution + repairs + side-effect-free refusals. False: `content` is
    // the enveloped refusal and `error` its message.
    bool prepare_tool_dispatch(kimix::string_view name,
                               kimix::string_view arguments_json,
                               kimix::string_view tool_call_id,
                               ToolDispatchPlan &plan, kimix::string &content,
                               kimix::string &error);
    // Hooks + approval gate + run + result post-processing.
    kimix::string finish_tool_dispatch(ToolDispatchPlan &plan,
                                       kimix::string_view tool_call_id,
                                       kimix::string &error);
    // The same dispatch collecting the tool's media parts (E1/E2 media out).
    kimix::string finish_tool_dispatch(ToolDispatchPlan &plan,
                                       kimix::string_view tool_call_id,
                                       kimix::string &error,
                                       kimix::vector<kimix::llm::ContentPart> *media_parts);
    // A9: the step's tool calls dispatched on up to
    // loop_control.dispatch_concurrency worker threads (each through the FULL
    // pipeline), results attached in original call order; a cancel or pure
    // rejection stops starting new tools (skipped calls keep the pairing).
    void dispatch_tool_calls_parallel(
        const kimix::vector<kimix::llm::ToolCall> &tool_calls,
        kimix::map<std::pair<kimix::string, kimix::string>, kimix::string>
            &step_results,
        bool &step_pure_rejection);
    // True when `name` resolves to a tool that may be used: registered, not
    // filtered out by options::enabled_tools, and valid().
    bool tool_available(kimix::string_view name) const;
    // True when `name` ends up in tool_definitions(): either tool_available()
    // or the shell fallback adopted there (pwsh in place of a bash tool this
    // environment cannot run, even when the manifest did not list pwsh).
    bool tool_offered(kimix::string_view name) const;
    // The shell the default system prompt names (see options::shell_tool):
    // the configured one when it is available, else the other shell tool when
    // that one is - the bash <-> pwsh fallback.
    kimix::string effective_shell_tool() const;
    kimix::string effective_system_prompt() const;

    // Reconcile the legacy numeric option fields with the authoritative
    // [loop_control] section (see the options::loop_control comment).
    static options reconcile_options(options opts);

    // Append one message to the history AND add its incremental token
    // estimate to the ledger (context.py append_message: pending += est).
    void append_history_with_ledger(const kimix::llm::Message &message);

    // Post-compaction ledger anchor (kimisoul.py:2223-2232): recorded =
    // estimate(history) + estimate(system prompt), pending = 0.
    void reanchor_ledger();

    // C10: finalize the compaction ledger transaction opened by
    // compact_context_attempt (compaction_ledger.py record_end). Both are
    // failure-isolated: a ledger error is reported on stderr and never fails
    // the compaction. The telemetry snapshot is refreshed after each write.
    void ledger_end(kimix::string_view compaction_id, int64_t summary_tokens,
                    bool shrank);
    void ledger_end_failure(kimix::string_view compaction_id,
                            kimix::string_view message);

    // G9 (kimisoul.py:1630-1650, the 2e.2 DYNAMIC INJECTION block): strip
    // stale <system-reminder> messages from live history, run the D11
    // auto-retrieval (step 1 only), collect fresh injections from every
    // registered provider, and append ONE combined reminder user message
    // (auto-retrieval first, mirroring the reference's prepend). Called once
    // per step before the request build.
    void apply_dynamic_injections(int32_t step_no, kimix::string_view turn_user_text);

    // D1 (kimisoul.py:1655-1704, 2e.3 CONTEXT PRUNING): run the pruner's auto
    // pass over the LLM-visible history (non-destructive; storage intact) and
    // return the pruned request view. Archives Tier-B/Tier-C elided originals
    // into the history index (D5) and logs the pass telemetry when a change
    // applied. Returns the input unchanged when pruning is disabled, gated
    // off (subagent without prune_subagents), in cooldown, or frees nothing.
    kimix::vector<kimix::llm::Message>
    prune_history_for_request(const kimix::vector<kimix::llm::Message> &history,
                              int32_t step_no);

    // C14 (kimisoul.py:1534-1548): the cache-depth floor for a history of
    // `history_len` - loop_control.prune_min_cache_prefix_depth when set (>0),
    // else len-(recent+8); 0 disables the floor (nullopt).
    kimix::optional<int32_t> cache_depth_floor(int64_t history_len) const noexcept;

    // The dynamic per-tool output budget in tokens
    // (kimisoul.py _tool_call_buffer_tokens / toolset.py
    // estimate_tool_output_token_budget): the more restrictive of 50% of the
    // model window, 90% of the remaining tokens and a 128KiB ceiling -
    // NOT the old static max_tokens/4.
    int64_t tool_call_buffer_tokens() const;

    // One chat attempt with the full step-retry policy applied
    // (agent/step_retry.h): retryable errors (connection/timeout/empty +
    // 429/5xx) retry up to loop_control.max_retries_per_step with the
    // _RateLimitAwareWait schedule; an empty/think-only failure escalates the
    // output budget x1.5 from 8192 on every retry (kimisoul.py
    // _before_step_retry_sleep). Retries do NOT count against max_steps.
    // `step_no` drives the wire StepRetry/LLMRequest records; `cancel`
    // (nullable) stops the retry loop and the in-flight request.
    kimix::llm::ChatResult chat_with_step_retry(
        const kimix::vector<kimix::llm::Message> &messages,
        const kimix::vector<kimix::llm::Tool> &tools,
        const SoulEventCallback &on_event, int32_t step_no,
        const CancelToken *cancel);
      // G7 (kimisoul.py:1036-1050 _consume_pending_steers): drain the internal
      // queue + the external steer source, inject every steer as a follow-up
      // user message and emit one SteerInput wire record per steer. Returns the
      // number injected.
      size_t consume_steers();
    // G8: true when the outside asked this turn to stop (the caller's token).
    bool cancel_requested() const noexcept {
        return _turn_cancel != nullptr && _turn_cancel->cancelled();
    }
    // compact_context body without the wire CompactionBegin/End transaction
    // envelope; fills `shadowed_out` with the estimated tokens the summary
    // replaces (-1 when unknown). `compaction_id` names the transaction on
    // the wire and in the compaction ledger (C10).
    bool compact_context_attempt(kimix::string_view custom_instruction,
                                 kimix::string &error, bool manual,
                                 builtin_tools::compact::CompactMode mode,
                                 int32_t preserve_depth_override,
                                 kimix::string_view trigger,
                                 int64_t &shadowed_out,
                                 kimix::string_view compaction_id);
};

} // namespace kimix::agent
