// mcp/mcp_client.h - F7 (audit G08, toolset.py 1747-2166 + mcp/client.py +
// mcp_cmd.py): a minimal MCP **stdio** client for the native CLI.
//
// Scope (documented):
//   * stdio servers only ({"command": ..., "args": [...], "env": {...}});
//     remote ("url") servers are parsed and listed but connecting to them
//     fails with an explicit reason.
//   * tools are bridged into the existing ToolRegistry as EXTERNAL tools
//     (register_external_tool); resources/prompts discovery is surfaced as
//     per-server status only - nothing is registered for them.
//   * OAuth (mcp_oauth.py) has no counterpart: a native client has no
//     browser/auth storage, and remote servers are out of scope anyway.
//
// Transport: JSON-RPC 2.0 over NEWLINE-DELIMITED UTF-8 JSON on the child's
// stdin/stdout. That is what the reference stack speaks: kimi_cli's
// fastmcp.Client wraps the `mcp` SDK whose stdio_client writes
// `(json + "\n")` and splits the child's stdout on "\n"
// (mcp/client/stdio/__init__.py stdout_reader/stdin_writer). There are no
// LSP-style Content-Length headers anywhere in that path. Lines that do not
// parse as JSON-RPC (e.g. a server logging to stderr, which the runner merges
// into the same capture stream) are skipped, exactly like the SDK's reader.
//
// Handshake (mcp/client/session.py start): `initialize` request ->
// `notifications/initialized` notification -> tools/list (page cursor
// followed) -> best-effort resources/list + prompts/list.
//
// Failure isolation: every child is a background task of the existing
// process_runner registry (start_task/send_task/read_task/stop_task); every
// wait is bounded. A dead/hung server degrades to a per-server "failed"
// status at startup and, for tool calls, to an error tool-result payload - the
// agent loop is never blocked indefinitely.
//
// Portability note: the runner EXTENDS the parent environment with the
// config's `env` entries (REPROC_ENV_EXTEND). Overriding a variable that the
// parent process already exports is therefore best-effort (the reference
// builds `{**default_env, **server.env}` instead); keys absent from the parent
// behave exactly like the reference.
//
// Rules (see .agents/skills/cpp): namespace kimix::mcp, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_registry.h"

namespace kimix::mcp {

// ---------------------------------------------------------------------------
// Configuration (.kimix/mcp.json / the global share dir's mcp.json)
// ---------------------------------------------------------------------------

// One entry of "mcpServers". stdio servers carry command/args/env; remote
// servers carry url/transport (and are never connected to in this scope).
struct server_config {
    kimix::string name;
    kimix::string command;             // stdio: executable
    kimix::vector<kimix::string> args; // stdio: command arguments
    kimix::vector<kimix::string> env;  // stdio: "KEY=VALUE" additions
    kimix::string url;                 // remote: endpoint (not supported here)
    kimix::string transport;           // remote: "http" | "streamable-http" | "sse"
    kimix::string raw_json;            // the entry's original JSON (compact)
};

// Parse a `{"mcpServers": {...}}` JSON text. Entries that are not JSON
// objects, or that name neither "command" nor "url", are SKIPPED and reported
// through `warnings` (the reference's fastmcp validation would reject the
// whole config; the native client degrades per server instead). Returns false
// only when the text itself is not a JSON object - the caller then reports
// the reference's "must contain a JSON object." warning.
bool parse_mcp_config_text(kimix::string_view text,
                           kimix::vector<server_config> &out,
                           kimix::vector<kimix::string> &warnings);

// merge_mcp_configs (mcp/config.py 54-76): `project` entries win over `global`
// entries with the same name; both inputs are untouched.
kimix::vector<server_config> merge_server_lists(
    const kimix::vector<server_config> &global,
    const kimix::vector<server_config> &project);

// ---------------------------------------------------------------------------
// Protocol layer (pure; unit-tested without any process)
// ---------------------------------------------------------------------------

namespace proto {

// convert_mcp_tool_result's budget (toolset.py MCP_MAX_OUTPUT_CHARS).
constexpr size_t kMaxOutputChars = 100000;

// One parsed JSON-RPC message (a line of the child's stdout).
struct message {
    enum class kind : uint8_t { none, response, request, notification };
    kind type = kind::none;
    kimix::string id;          // response/request id as a RAW JSON token
    kimix::string method;      // request/notification method
    kimix::string result_json; // response "result" member (raw JSON)
    kimix::string error_json;  // response "error" member (raw JSON, "" = none)
    kimix::string params_json; // request/notification "params" (raw JSON)
};

// Classify + parse one framed line. Unparseable input (garbage, stderr noise)
// yields kind::none - the caller skips it like the SDK's stdout_reader.
bool parse_message(kimix::string_view line, message &out);

// Frames (each returned string is one complete newline-terminated wire line).
// `raw_id_json` is the id as a RAW JSON token (e.g. "3" or "\"abc\""): a
// response must echo the request's id exactly, and a server->client request
// may use any id value.
kimix::string make_request(uint64_t id, kimix::string_view method,
                           kimix::string_view params_json);
kimix::string make_notification(kimix::string_view method,
                                kimix::string_view params_json);
kimix::string make_response(kimix::string_view raw_id_json,
                            kimix::string_view result_json);
kimix::string make_response_error(kimix::string_view raw_id_json, int32_t code,
                                  kimix::string_view message);

// The client-side messages of the startup handshake + discovery
// (mcp/client/session.py start + tools/call).
kimix::string initialize_request(uint64_t id);
kimix::string initialized_notification();
kimix::string tools_list_request(uint64_t id, kimix::string_view cursor);
kimix::string tools_call_request(uint64_t id, kimix::string_view name,
                                 kimix::string_view arguments_json);
kimix::string resources_list_request(uint64_t id, kimix::string_view cursor);
kimix::string prompts_list_request(uint64_t id, kimix::string_view cursor);

// One entry of a tools/list result.
struct tool_info {
    kimix::string name;
    kimix::string description;       // "" when absent
    kimix::string input_schema_json; // "{}" when absent
};

// Parse a tools/list result into `out`; `next_cursor` is "" when the server
// has no further page. False + `error` for a malformed result.
bool parse_tools_list(kimix::string_view result_json,
                      kimix::vector<tool_info> &out, kimix::string &next_cursor,
                      kimix::string &error);

// convert_mcp_tool_result (toolset.py 2085-2166) reduced to the text channel:
// every "text" content part is appended (a shared 100 000-character budget
// truncates in place), every other part becomes the reference's
// "[Unsupported content: ...]" placeholder text (the native external-tool
// result channel carries one string, not ContentParts), and a truncated
// result gets the reference's truncation note appended. `is_error` mirrors the
// result's "isError" flag.
struct tool_result {
    kimix::string text;
    bool is_error = false;
    bool truncated = false;
    size_t unsupported = 0;
};
void extract_tool_result(kimix::string_view result_json, tool_result &out);

// The truncation note appended after an exhausted budget
// ("\n\n[Output truncated: exceeded 100000 character limit. ...]").
kimix::string truncation_note();

// MCPTool.__call__'s timeout ToolError message (toolset.py 2029-2035).
kimix::string timeout_error_text(kimix::string_view tool_name);

// MCPTool's description prefix (toolset.py 2105-2110, `or` the fallback).
kimix::string mcp_tool_description(kimix::string_view server_name,
                                   kimix::string_view tool_description);

} // namespace proto

// ---------------------------------------------------------------------------
// Per-server status
// ---------------------------------------------------------------------------

enum class server_state : uint8_t {
    not_started = 0,
    connecting,
    connected,
    failed,
};

struct server_status {
    kimix::string name;
    server_state state = server_state::not_started;
    kimix::string error;                     // failed: the reason
    kimix::vector<kimix::string> tools;      // registered tool names
    kimix::vector<kimix::string> collisions; // skipped: name already owned
    kimix::vector<kimix::string> skipped;    // skipped: other reason
    kimix::vector<kimix::string> resources;  // discovered URIs (status only)
    kimix::vector<kimix::string> prompts;    // discovered names (status only)
};

// ---------------------------------------------------------------------------
// Client (process layer): one stdio server child
// ---------------------------------------------------------------------------

struct client_options {
    // The reference passes one client-wide timeout
    // (runtime.config.mcp.client.tool_call_timeout_ms, default 60 s) to
    // every call. Connecting is bounded separately (and tighter) so a hung
    // server cannot stall the boot: the reference fans the connects out with
    // asyncio.gather, the native loop is sequential.
    int64_t connect_timeout_ms = 10000;
    int64_t call_timeout_ms = 60000; // reference tool_call_timeout_ms default
    // The runner's bounded-append capture cap; the client drains continuously,
    // the bound only exists so a chatty server cannot grow the buffer forever.
    int64_t output_cap_chars = 4194304;
};

class McpClient {
public:
    McpClient(server_config config, client_options options);
    ~McpClient();
    McpClient(const McpClient &) = delete;
    McpClient &operator=(const McpClient &) = delete;

    // Spawn the child and run the handshake + discovery. On failure the child
    // is stopped again, `error` carries the reason and status() reports
    // server_state::failed. Never throws, never blocks longer than the
    // connect bounds in `options`.
    bool connect(kimix::string &error);

    // Stop the child (idempotent).
    void shutdown();

    bool connected() const { return _status.state == server_state::connected; }

    const server_config &config() const { return _config; }
    const server_status &status() const { return _status; }
    const kimix::vector<proto::tool_info> &tools() const { return _tools; }

    // Forward `tools/call` for a discovered tool. ALWAYS returns true and
    // fills `payload_json` with the tool-result payload JSON the dispatcher
    // renders ({"status": "ok"|"error", "message"/"brief"/"output", ...}):
    // a timeout produces the reference's timeout ToolError text, a dead
    // server the reference's runtime-error text, an isError result the
    // reference's "Tool returned an error." mapping. `error` is only set for
    // caller mistakes (unknown tool / malformed arguments).
    bool call_tool(kimix::string_view name, kimix::string_view arguments_json,
                   kimix::string &payload_json, kimix::string &error);

private:
    // Send a pre-built frame and wait (bounded) for the response with `id`.
    // Server->client requests are answered inline while waiting (ping gets a
    // result, everything else a -32601 error, like the SDK's default
    // handlers); notifications are dropped.
    bool request_frame(kimix::string_view frame, uint64_t id,
                       int64_t timeout_ms, proto::message &out,
                       kimix::string &error);

    // Consume every complete line buffered in `_pending`, answering
    // server->client requests inline. True when the response with `want_id`
    // arrived (moved into `out`).
    bool drain_pending(const kimix::string &want_id, proto::message &out);
    // Answer one server->client request (ping -> result, else -32601).
    void answer_server_request(const proto::message &msg);

    // Best-effort resources/list or prompts/list discovery: any failure is
    // silent and leaves `out` untouched (the reference logs it at debug).
    void best_effort_list(bool resources, int64_t deadline);

    bool send_frame(kimix::string_view frame, kimix::string &error);
    bool spawn(kimix::string &error);

    server_config _config;
    client_options _options;
    server_status _status;
    kimix::vector<proto::tool_info> _tools;
    kimix::optional<kimix::string> _task_id; // process_runner registry id
    kimix::string _pending;                  // child bytes not yet framed
    uint64_t _next_id = 1;
};

// ---------------------------------------------------------------------------
// Manager: every configured server of this process
// ---------------------------------------------------------------------------

class McpManager {
public:
    static McpManager &instance();

    // Connect every configured server and register the discovered tools
    // (name-collided tools are skipped and recorded with the reference's
    // "tool name conflicts with existing tool" message). `status_lines`
    // receives one "MCP server <name>: connected (N tools)" /
    // "MCP server <name>: failed: <reason>" line per server, in config
    // order. Returns true when at least one server reached connected state.
    bool start_all(const kimix::vector<server_config> &configs,
                   kimix::vector<kimix::string> &status_lines,
                   kimix::string &error);

    // Unregister every tool this manager registered, then stop the children.
    // Idempotent; statuses stay readable afterwards.
    void shutdown();

    bool running() const { return !_clients.empty(); }

    const kimix::vector<server_status> &statuses() const { return _statuses; }

private:
    McpManager() = default;
    McpManager(const McpManager &) = delete;
    McpManager &operator=(const McpManager &) = delete;

    // Register the client's discovered tools (collision skip included),
    // filling the status' tools/collisions/skipped lists.
    void register_tools(McpClient &client, server_status &st);

    kimix::vector<kimix::unique_ptr<McpClient>> _clients;
    kimix::vector<server_status> _statuses;
    kimix::vector<kimix::string> _registered_tools;
};

} // namespace kimix::mcp
