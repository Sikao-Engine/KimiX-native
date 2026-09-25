// agent_tool.h - C++ port of the kimi-agent sub-agent tools: `subagent`,
// `send_message`, `list_agents` and `interrupt_agent`.
//
// Python source of truth (C:/dev/kimi-agent/src/kimix/tools/agent/):
//   store.py ConversationTurn                            11-16
//   store.py AgentSessionEntry                           19-46
//   store.py AgentSessionStore (MAX_SESSIONS=10, get/put/close/list_active/
//            evict_lru_if_needed)                        49-85
//   __init__.py _agent_entries / _agent_sessions maps    25-45
//   __init__.py _register/_get/_unregister_agent_session 32-45
//   __init__.py _cli_session_id / _session_work_dir      47-88
//   __init__.py _pending_messages + cap 50               100-121
//   __init__.py _queue_pending_message                   112-118
//   __init__.py _drain_pending_messages                  121-124
//   __init__.py _pending_message_count                   127-129
//   __init__.py _format_pending_messages                 132-143
//   __init__.py _queued_message_output                   146-158
//   __init__.py _resolve_prompt (@path)                  161-181
//   __init__.py _prompt_saved_message                    184-195
//   __init__.py SubAgentParams                           221-295
//   __init__.py _get_store                               298-303
//   __init__.py AskAgentParams / AskAgent.__call__ /
//               AskAgent._resolve_target                 389-521
//   __init__.py Agent.name/description/__call__          523-700
//   __init__.py Agent._build_extras / _format_history    701-780
//   __init__.py Agent._resolve_session                   781-870
//   __init__.py AgentListParams / AgentList.__call__     992-1029
//   __init__.py AgentCloseParams / AgentClose.__call__   1030-1076
//
// Design notes (project conventions):
// * namespace kimix::builtin_tools::agents; TU-local helpers use the `ag_`
//   prefix (kimix-llm builds src/builtin_tools/*.cpp as one unity TU).
// * kimix:: containers only; no RTTI; kernels never throw across the tool
//   boundary.
// * Running a sub-agent needs a chat backend, which lives above this layer
//   (src/agent/soul.h). Exactly like retrieve::HistoryIndexView, the registry
//   exposes an INJECTABLE `subagent_runner`; when it is unset the tools answer
//   tool_status::unsupported so the host can fall back. src/agent/agent_host
//   installs a KimiSoul-backed runner. Every other kernel here is pure and
//   unit-testable offline.
// * Background execution uses one std::thread per in-flight run (the Python
//   tool uses asyncio tasks). The registry joins every worker in its
//   destructor, and interrupt_agent sets the run's cancel flag - the C++
//   counterpart of `Steer`/`close_session_async`.
// * The module-level Python maps (_agent_entries, _agent_sessions,
//   _pending_messages) are process-wide there because one process hosts one
//   session tree; here they live inside the session-scoped agent_registry,
//   which Session::agents owns. Cross-session lookup by id is preserved
//   through the registry's live-session set.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::agents {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// store.py AgentSessionStore.MAX_SESSIONS
inline constexpr int32_t k_max_sessions = 10;
// __init__.py _MAX_PENDING_MESSAGES_PER_TARGET
inline constexpr size_t k_max_pending_messages = 50;
// Agent.__call__: prompts above 100 KiB are offloaded to a temp file.
inline constexpr size_t k_prompt_offload_bytes = 100 * 1024;
// Agent.__init__: asyncio.Semaphore(8)
inline constexpr int32_t k_max_concurrent_subagents = 8;

// ---------------------------------------------------------------------------
// Conversation turns (store.py ConversationTurn)
// ---------------------------------------------------------------------------
struct conversation_turn {
    kimix::string role; // user | assistant | system | tool | error
    kimix::string content;
    double timestamp = 0.0; // time.time() seconds
    kimix::string type; // metadata["type"]: text|think|tool_call|tool_result|error
};

// ---------------------------------------------------------------------------
// Injectable sub-agent execution
// ---------------------------------------------------------------------------
struct subagent_request {
    kimix::string session_id; // "" == caller allocates a new id
    kimix::string prompt; // fully resolved effective prompt
    kimix::string work_dir;
    kimix::string description;
    bool resume = false; // an existing session is being continued
    bool inherit_context = false;
    bool close_session = true;
    bool background = false;
    // Swarm/agent type ("coder" | "explore" | "plan" | custom) - the runner
    // maps it onto the child system prompt (Python _SUBAGENT_TYPE_MAP).
    kimix::string subagent_type;
    // Set by the registry for background runs; the runner should poll it
    // between steps and stop early (interrupt_agent).
    std::atomic<bool> *cancel = nullptr;
};

struct subagent_run_result {
    bool ok = false;
    kimix::string output; // final assistant text ("(no text output)" when empty)
    kimix::string error;
    kimix::vector<conversation_turn> turns;
    // Set when the sub-agent asked the parent a question (state becomes
    // "awaiting_response" and the session stays open).
    kimix::optional<kimix::string> pending_question;
    bool cancelled = false;
};

// Injected sub-agent runner. It must NOT throw (kimix is built without
// exceptions): failures are reported as data - `subagent_run_result::ok ==
// false` plus `::error` - exactly what the registry's background worker
// observes.
using subagent_runner =
    kimix::function<subagent_run_result(const subagent_request &)>;

// File access hooks so the prompt/context kernels stay fixture-free.
using read_file_fn = kimix::function<bool(kimix::string_view path,
                                          kimix::string &out)>;
using save_prompt_fn = kimix::function<kimix::string(kimix::string_view text,
                                                     kimix::string_view ext)>;

// ---------------------------------------------------------------------------
// Registry (store.py AgentSessionStore + the module-level maps)
// ---------------------------------------------------------------------------
struct agent_entry {
    kimix::string session_id;
    double created_at = 0.0;
    double last_accessed = 0.0;
    kimix::vector<conversation_turn> conversation_history;
    int32_t total_turns = 0;
    bool is_active = true;
    kimix::optional<kimix::string> pending_question;
    kimix::string state = "running"; // running | awaiting_response | completed
};

// list_active() row - the exact dict shape AgentList serializes.
struct agent_list_item {
    kimix::string session_id;
    double created_at = 0.0;
    double last_accessed = 0.0;
    int32_t total_turns = 0;
    kimix::string state;
    bool is_active = true;
};

// Live background run bookkeeping.
struct agent_run {
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    subagent_run_result result;
    kimix::string prompt;
    double started_at = 0.0;
};

class agent_registry {
public:
    agent_registry() = default;
    ~agent_registry(); // joins every worker
    agent_registry(const agent_registry &) = delete;
    agent_registry &operator=(const agent_registry &) = delete;

    // Injected execution. Empty == the agent tools report unsupported.
    subagent_runner runner;
    // Optional file hooks (defaults: real file system / .kimix_cache temp dir).
    read_file_fn read_file;
    save_prompt_fn save_prompt;
    // Clock injection for deterministic tests (defaults to wall clock).
    kimix::function<double()> now;

    // ---- store (AgentSessionStore) -------------------------------------
    // get(): null when absent. The returned pointer stays valid until the next
    // put/close on the same id (entries are held by unique_ptr).
    const agent_entry *get(kimix::string_view session_id) const;
    agent_entry *get(kimix::string_view session_id);
    void put(agent_entry entry);
    bool close(kimix::string_view session_id);
    kimix::vector<agent_list_item> list_active() const;
    size_t size() const;
    // evict_lru_if_needed(): while size() >= MAX_SESSIONS drop the least
    // recently accessed entry (is_active = false + close_session hook).
    kimix::vector<kimix::string> evict_lru_if_needed();

    // ---- live session map (_agent_sessions) ----------------------------
    void register_session(kimix::string_view session_id);
    void unregister_session(kimix::string_view session_id);
    bool has_session(kimix::string_view session_id) const;

    // ---- pending message queue (_pending_messages) ---------------------
    void queue_pending_message(kimix::string_view target_id,
                               kimix::string_view message);
    kimix::vector<kimix::string> drain_pending_messages(
        kimix::string_view session_id);
    size_t pending_message_count(kimix::string_view session_id) const;

    // ---- background runs ------------------------------------------------
    // Start `runner` on a worker thread. Returns false when no runner is set.
    bool start_background(kimix::string_view session_id,
                          const subagent_request &request);
    // True while a worker thread is running for this id.
    bool is_running(kimix::string_view session_id) const;
    // Push a message into the running turn's steer queue (send_message). True
    // when the target is running and accepted it.
    bool push_steer(kimix::string_view session_id, kimix::string_view message);
    kimix::vector<kimix::string> drain_steer(kimix::string_view session_id);
    // Request cancellation (interrupt_agent). True when a run was signalled.
    bool request_cancel(kimix::string_view session_id);
    // Wait for a background run to settle and take its result. False when
    // there is no run for this id.
    bool join_run(kimix::string_view session_id, subagent_run_result &out);
    // Non-blocking settle check.
    bool run_finished(kimix::string_view session_id) const;
    // Drop the run bookkeeping after the result has been collected.
    void clear_run(kimix::string_view session_id);

    // Session this registry belongs to ("" for a standalone registry).
    kimix::string owner_session_id;

    // Injectable clock read (time.time() seconds). Public so the tool
    // wrappers stamp entries with the same clock the registry uses.
    double clock_now() const;

private:
    struct slot {
        kimix::unique_ptr<agent_entry> entry;
        kimix::unique_ptr<agent_run> run;
        kimix::vector<kimix::string> steer;
    };
    // Results parked by close() (interrupt_agent / eviction) so join_run() and
    // run_finished() can still report the outcome after the session slot was
    // dropped. Consumed by join_run(), dropped by clear_run().
    kimix::unordered_map<kimix::string, subagent_run_result, kimix::string_hash>
        _finished;
    slot *find_locked(kimix::string_view session_id);
    const slot *find_locked(kimix::string_view session_id) const;
    double now_seconds() const;

    mutable kimix::spin_mutex _mutex;
    kimix::unordered_map<kimix::string, slot, kimix::string_hash> _slots;
    kimix::vector<kimix::string> _order; // Python dict insertion order
    kimix::unordered_map<kimix::string, kimix::vector<kimix::string>,
                         kimix::string_hash>
        _pending;
    kimix::unordered_set<kimix::string, kimix::string_hash> _live_sessions;
};

// Lazily create/attach Session::agents (port of _get_store).
agent_registry &session_registry(kimix::builtin_tools::Session *session);

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------
// _format_pending_messages (132-143).
kimix::string format_pending_messages(
    kimix::span<const kimix::string> messages);

// _queued_message_output (146-158).
kimix::string queued_message_output(kimix::string_view target_id,
                                    kimix::string_view reason);

// _resolve_prompt (161-181): `@path` indirection with the work-dir -> CWD
// fallback. Returns false and fills `error` with
// "prompt file not found: {rel}" when the file is missing.
bool resolve_prompt(kimix::string_view prompt, kimix::string_view base_dir,
                    const read_file_fn &read_file, kimix::string &out,
                    kimix::string &error);

// _prompt_saved_message (184-195). `saved_display` is the temp path as shown
// to the model; "" (or an empty prompt) yields "".
kimix::string prompt_saved_message(kimix::string_view prompt,
                                   kimix::string_view saved_display);

// Agent.__call__ context block (context_files / context_data).
kimix::string build_context_block(
    kimix::span<const kimix::string> context_files,
    kimix::string_view context_data_json, const read_file_fn &read_file,
    kimix::string_view base_dir);

// Agent.__call__ response injection (the deprecated `response` parameter).
kimix::string inject_response(kimix::string_view prompt,
                              kimix::string_view question,
                              kimix::string_view response);

// Agent.__call__: prompts above k_prompt_offload_bytes become
// "Please read the task from `{display}` and execute it."
kimix::string offload_long_prompt(kimix::string_view prompt,
                                  kimix::string_view saved_display);

// Agent._format_history (745-780): "json" | "markdown" | "summary".
// `json` returns the serialized array as a string; the Tool wrapper embeds it
// as a real JSON array. markdown/summary return plain text.
kimix::string format_history_markdown(
    kimix::span<const conversation_turn> turns);
kimix::string format_history_summary(
    kimix::span<const conversation_turn> turns);
// One turn as a JSON object element (role/content/timestamp/metadata).
ValueElement turn_to_value(const conversation_turn &turn);

// AgentList.__call__: orjson.dumps(sessions, option=OPT_INDENT_2).
kimix::string list_active_json(kimix::span<const agent_list_item> items);

// AskAgent._resolve_target (494-521).
struct send_target {
    kimix::optional<kimix::string> target_id;
    bool has_live_session = false;
    kimix::string reason; // "" on success
};
send_target resolve_send_target(const agent_registry &registry,
                                bool caller_is_sub_agent,
                                kimix::string_view parent_session_id,
                                kimix::string_view requested_id);

// "Message from agent '{caller_id}':\n{message}" (only when caller_id != "").
kimix::string prefix_sender(kimix::string_view caller_id,
                            kimix::string_view message);

// Agent._resolve_session: an existing ACTIVE entry is reused as-is.
bool is_resumable(const agent_registry &registry, kimix::string_view session_id);

// ---------------------------------------------------------------------------
// Parameter models
// ---------------------------------------------------------------------------
struct subagent_params {
    kimix::optional<kimix::string> description;
    kimix::string prompt; // alias `task`
    bool run_in_background = true;
    kimix::optional<kimix::string> session_id; // alias `session`
    bool close_session = true;
    bool return_history = false;
    kimix::string history_format = "json"; // json | markdown | summary
    kimix::optional<kimix::string> response; // deprecated
    kimix::vector<kimix::string> context_files;
    kimix::optional<kimix::string> context_data_json; // serialized as-is
    bool inherit_context = false;
};
tool_error parse_subagent_params(const ToolParams *params, subagent_params &out);

struct send_message_params {
    kimix::string message; // alias `question`
    kimix::optional<kimix::string> subagent_id; // alias `id`
};
tool_error parse_send_message_params(const ToolParams *params,
                                     send_message_params &out);

struct list_agents_params {
    kimix::string scope = "children";
};
tool_error parse_list_agents_params(const ToolParams *params,
                                    list_agents_params &out);

struct interrupt_agent_params {
    kimix::string agent_id; // aliases: session, session_id
};
tool_error parse_interrupt_params(const ToolParams *params,
                                  interrupt_agent_params &out);

// ---------------------------------------------------------------------------
// Tool classes
// ---------------------------------------------------------------------------
// Shared serialized envelope:
//   ok / status / message / output / brief / extras{session_id,status,
//   turn_count[,question][,conversation_history]}
class Subagent : public kimix::builtin_tools::Tool {
public:
    explicit Subagent(kimix::builtin_tools::Session *session);
    // Spawning a sub-agent needs an injected runner in the session's agent
    // registry (the host owns the nested turn loop). Without one every call
    // answers unsupported, so the tool is not offered; a runner installed
    // later re-enables it on the next definition rebuild.
    bool valid() const override;
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

private:
    kimix::vector<char> _result;
};

// NOTE: the class is spelled SendMessageTool because Windows' <winuser.h>
// does `#define SendMessage SendMessageW`, which rewrites any identifier
// spelled SendMessage in a TU that also sees windows.h (process_runner.cpp
// does). It is REGISTERED under the name "send_message" - the lowercase
// form of the agent-facing tool ("SendMessage" and "sendmessage" are
// declared aliases).
class SendMessageTool : public kimix::builtin_tools::Tool {
public:
    explicit SendMessageTool(kimix::builtin_tools::Session *session);
    // Needs the session whose id / parent id the message routing reads; the
    // agent registry itself is optional (a message to a session that is not
    // live yet is queued, which is a valid outcome).
    bool valid() const override;
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

private:
    kimix::vector<char> _result;
};

class ListAgents : public kimix::builtin_tools::Tool {
public:
    explicit ListAgents(kimix::builtin_tools::Session *session);
    // Reads the session's agent registry (an empty list is a valid answer),
    // so only a missing session can make the tool unusable.
    bool valid() const override;
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

private:
    kimix::vector<char> _result;
};

class InterruptAgent : public kimix::builtin_tools::Tool {
public:
    explicit InterruptAgent(kimix::builtin_tools::Session *session);
    // Same requirement as ListAgents: it drives the session's registry.
    bool valid() const override;
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

private:
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::agents
