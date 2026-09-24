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
// Not ported: hooks engine, wire protocol, notifications, loop detectors,
// dynamic injections, context pruning, overflow recovery (all Python-side
// orchestration around the same kernels).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "llm/llm.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// Chat backend abstraction (so the soul is unit-testable without network)
// ---------------------------------------------------------------------------

// Minimal chat interface the soul needs. kimix::llm::LLM satisfies it through
// LLMBackend below; tests inject a scripted fake.
class IChatBackend {
public:
    virtual ~IChatBackend() = default;
    virtual kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) = 0;
    virtual int64_t max_context_size() const = 0;
    virtual kimix::string model_name() const = 0;
};

// Adapter over the concrete kimix::llm::LLM facade (create_llm_from_file).
class LLMBackend : public IChatBackend {
public:
    explicit LLMBackend(kimix::unique_ptr<kimix::llm::LLM> llm);
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override;
    int64_t max_context_size() const override;
    kimix::string model_name() const override;

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

    // The builtin_tools::Session passed to every tool instance.
    builtin_tools::Session &tool_session() { return _tool_session; }
    const builtin_tools::Session &tool_session() const { return _tool_session; }

private:
    kimix::string _id;
    kimix::vector<kimix::llm::Message> _history;
    builtin_tools::Session _tool_session; // work_dir + native_io = true
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
    // The input carried no sendable content (empty / whitespace-only) so no turn
    // was started: the history was not touched and no LLM call was made. Mirrors
    // kimi_cli.soul.run_soul's empty-input guard (soul/__init__.py:329-341);
    // without it the soul appends an empty `user` message and the model answers
    // a spurious "you sent an empty message" turn.
    bool ignored = false;
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
        int32_t max_steps = 32;         // per-turn LLM round-trip bound
        bool auto_compact = true;       // compact when the context grows
        double auto_compact_ratio = 0.75;
        int64_t reserved_context = 8192;
        // Mirrors kimi_cli's LoopControl.max_tokens / _tool_call_buffer_tokens():
        // both feed should_auto_compact's reserved-output boundary, so leaving
        // them at 0 under-fires the reserved-based trigger.
        int64_t max_tokens = 0;              // model output budget (None -> 0)
        int64_t tool_call_buffer_tokens = 0; // dynamic per-tool output budget
        // Preserve-depth bounds for the compaction boundary, mirroring
        // LoopControl.min_preserved_messages (1) / max_preserved_messages (2).
        // The reference resolves its SimpleCompaction preserve_depth through
        // adaptive_preserve_depth(msgs, min_preserved=1, max_preserved=2).
        int32_t min_preserved_turns = 1;
        int32_t max_preserved_turns = 2;
        kimix::vector<kimix::string> enabled_tools; // empty == all registered
    };

    KimiSoul(AgentSession &session, IChatBackend &backend);
    KimiSoul(AgentSession &session, IChatBackend &backend, options opts);

    const AgentSession &session() const { return _session; }
    AgentSession &session() { return _session; }

    // Tools offered to the model, derived from the static ToolRegistry
    // (class name -> name). Rebuilt on demand; cheap.
    kimix::vector<kimix::llm::Tool> tool_definitions() const;

    // Run one user turn: append the user message, then loop
    // chat -> execute tool calls -> append tool results until the model
    // answers with plain text or max_steps is hit. `on_event` receives the
    // streamed chunks (may be empty).
    TurnResult turn(kimix::string_view user_input,
                    const SoulEventCallback &on_event = {});

    // Compact the conversation history through the backend: slice off the
    // preserved tail (balanced on tool-call pairs), build the compaction
    // prompt (compact:: kernels + the ported compact.md body), summarize with
    // one LLM call and replace the head with the summary. Returns false and
    // fills `error` when there is nothing to compact or the call fails.
    bool compact_context(kimix::string_view custom_instruction,
                         kimix::string &error);

    // Estimated token count of the current history (compact:: estimator).
    int64_t estimated_tokens() const;

    // Number of compactions performed so far.
    int32_t compaction_count() const { return _compactions; }

    // Execute one tool call by name with raw JSON arguments (repair +
    // parse + dispatch). Exposed for tests; returns the tool message content.
    kimix::string execute_tool_call(kimix::string_view name,
                                    kimix::string_view arguments_json,
                                    kimix::string &error);

private:
    AgentSession &_session;
    IChatBackend &_backend;
    options _opts;
    int32_t _compactions = 0;
    kimix::unordered_map<kimix::string, kimix::unique_ptr<builtin_tools::Tool>,
                         kimix::string_hash>
        _tools; // cached instances by class name

    builtin_tools::Tool *get_tool(kimix::string_view name);
    kimix::string effective_system_prompt() const;
};

} // namespace kimix::agent
