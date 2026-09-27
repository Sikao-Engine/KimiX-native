// mcp/mcp_client.cpp - F7: the minimal MCP stdio client (see mcp_client.h).
//
// Process layer note: children are background tasks of the existing reproc
// wrapper (src/builtin_tools/process_runner.h). The runner merges the child's
// stderr into the same capture stream as stdout, so protocol lines are parsed
// tolerantly: anything that is not a JSON-RPC message is skipped, exactly like
// the reference SDK's stdout_reader.

#include "mcp/mcp_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "builtin_tools/process_runner.h"
#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::mcp {

namespace {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::proc::query_task;
using kimix::builtin_tools::proc::read_task;
using kimix::builtin_tools::proc::send_task;
using kimix::builtin_tools::proc::start_task;
using kimix::builtin_tools::proc::stop_task;
using kimix::builtin_tools::proc::task_handle;
using kimix::builtin_tools::proc::task_status_info;
using kimix::builtin_tools::proc::wait_task;

// Steady milliseconds (bounded waits must not jump with the wall clock).
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Parse one JSON text (STOP_WHEN_DONE tolerates trailing whitespace/newline).
yyjson_doc *parse_json(kimix::string_view text) {
    kimix::string buffer(text);
    return yyjson_read_opts(buffer.data(), buffer.size(),
                            YYJSON_READ_STOP_WHEN_DONE, &kimix::llm::kYYJsonAlcMi,
                            nullptr);
}

// Serialize a sub-tree of an immutable document to compact JSON.
kimix::string val_to_json(const yyjson_val *val) {
    if (val == nullptr) {
        return kimix::string();
    }
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, val);
    yyjson_mut_doc_set_root(mdoc, root);
    kimix::string out;
    if (root != nullptr) {
        size_t len = 0;
        char *json =
            yyjson_mut_write_opts(mdoc, 0, &kimix::llm::kYYJsonAlcMi, &len, nullptr);
        if (json != nullptr) {
            out.assign(json, len);
            mi_free(json);
        }
    }
    yyjson_mut_doc_free(mdoc);
    return out;
}

// A string member of an object ("" when absent or not a string).
kimix::string obj_str(yyjson_val *obj, const char *key) {
    if (obj == nullptr) {
        return kimix::string();
    }
    const yyjson_val *v = yyjson_obj_get(obj, key);
    if (v == nullptr || !yyjson_is_str(v)) {
        return kimix::string();
    }
    return kimix::string(yyjson_get_str(v), static_cast<size_t>(yyjson_get_len(v)));
}

// Serialize one frame (root of a mutable doc) into a newline-terminated line.
kimix::string write_frame(yyjson_mut_doc *mdoc) {
    size_t len = 0;
    char *json = yyjson_mut_write_opts(mdoc, 0, &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    kimix::string out;
    if (json != nullptr) {
        out.assign(json, len);
        mi_free(json);
    }
    yyjson_mut_doc_free(mdoc);
    if (!out.empty()) {
        out.push_back('\n');
    }
    return out;
}

// Copy a raw-JSON fragment as a member value of `root`; false when the
// fragment does not parse (the member is then omitted).
bool add_raw_member(yyjson_mut_doc *mdoc, yyjson_mut_val *root,
                    const char *key, kimix::string_view raw_json) {
    if (raw_json.empty()) {
        return false;
    }
    yyjson_doc *doc = parse_json(raw_json);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *value = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (value == nullptr) {
        return false;
    }
    yyjson_mut_obj_add(root, yyjson_mut_str(mdoc, key), value);
    return true;
}

// The dispatcher's tool-result payload contract (soul.cpp's payload parser):
// {"status": "ok"|"error"|<other>, "message", "output", "runtime"}. Status
// "error" is the runtime-failure channel; any other non-"ok" status renders
// the plain <system>ERROR: ...</system> envelope (the reference's ToolError).
kimix::string make_payload(kimix::string_view status, kimix::string_view message,
                           kimix::string_view output) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add(root, yyjson_mut_str(mdoc, "status"),
                       yyjson_mut_strncpy(mdoc, status.data(), status.size()));
    if (!message.empty()) {
        yyjson_mut_obj_add(root, yyjson_mut_str(mdoc, "message"),
                           yyjson_mut_strncpy(mdoc, message.data(), message.size()));
    }
    if (!output.empty()) {
        yyjson_mut_obj_add(root, yyjson_mut_str(mdoc, "output"),
                           yyjson_mut_strncpy(mdoc, output.data(), output.size()));
    }
    kimix::string frame = write_frame(mdoc);
    // Drop the trailing newline write_frame added - this is a payload, not a
    // protocol frame.
    if (!frame.empty() && frame.back() == '\n') {
        frame.pop_back();
    }
    return frame;
}

// The per-server stderr note the reference logs when a server dies
// ("Failed to connect MCP server: {name}, error: {error}" is logger-side; the
// native client surfaces the same text through the status line instead).

} // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool parse_mcp_config_text(kimix::string_view text,
                           kimix::vector<server_config> &out,
                           kimix::vector<kimix::string> &warnings) {
    out.clear();
    yyjson_doc *doc = parse_json(text);
    if (doc == nullptr) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    const yyjson_val *servers = yyjson_obj_get(root, "mcpServers");
    if (servers != nullptr && !yyjson_is_obj(servers)) {
        warnings.push_back("mcpServers must contain a JSON object.");
        yyjson_doc_free(doc);
        return true;
    }
    if (servers == nullptr) {
        yyjson_doc_free(doc);
        return true; // no servers configured
    }
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *key = nullptr;
    yyjson_val *entry = nullptr;
    yyjson_obj_foreach(servers, idx, max, key, entry) {
        kimix::string name;
        if (key != nullptr && yyjson_is_str(key)) {
            name.assign(yyjson_get_str(key), static_cast<size_t>(yyjson_get_len(key)));
        }
        if (!yyjson_is_obj(entry)) {
            warnings.push_back("mcpServers." + name +
                               " must contain a JSON object.");
            continue;
        }
        server_config cfg;
        cfg.name = name;
        cfg.command = obj_str(entry, "command");
        cfg.url = obj_str(entry, "url");
        if (cfg.command.empty() && cfg.url.empty()) {
            warnings.push_back("mcpServers." + name +
                               " names neither command nor url.");
            continue;
        }
        if (!cfg.command.empty()) {
            const yyjson_val *args = yyjson_obj_get(entry, "args");
            if (args != nullptr && yyjson_is_arr(args)) {
                size_t ai = 0;
                yyjson_val *item = nullptr;
                size_t amax = 0;
                yyjson_arr_foreach(args, ai, amax, item) {
                    if (yyjson_is_str(item)) {
                        cfg.args.emplace_back(
                            yyjson_get_str(item),
                            static_cast<size_t>(yyjson_get_len(item)));
                    }
                }
            }
            const yyjson_val *env = yyjson_obj_get(entry, "env");
            if (env != nullptr && yyjson_is_obj(env)) {
                size_t ei = 0;
                size_t emax = 0;
                yyjson_val *ek = nullptr;
                yyjson_val *ev = nullptr;
                yyjson_obj_foreach(env, ei, emax, ek, ev) {
                    if (ek == nullptr || !yyjson_is_str(ek) || !yyjson_is_str(ev)) {
                        continue;
                    }
                    kimix::string kv;
                    if (ek != nullptr && yyjson_is_str(ek)) {
                        kv.assign(yyjson_get_str(ek),
                                  static_cast<size_t>(yyjson_get_len(ek)));
                    }
                    kv += "=";
                    if (ev != nullptr && yyjson_is_str(ev)) {
                        kv.append(yyjson_get_str(ev),
                                  static_cast<size_t>(yyjson_get_len(ev)));
                    }
                    cfg.env.push_back(std::move(kv));
                }
            }
        } else {
            cfg.transport = obj_str(entry, "transport");
            if (cfg.transport.empty()) {
                cfg.transport = "http";
            }
        }
        cfg.raw_json = val_to_json(entry);
        out.push_back(std::move(cfg));
    }
    yyjson_doc_free(doc);
    return true;
}

kimix::vector<server_config> merge_server_lists(
    const kimix::vector<server_config> &global,
    const kimix::vector<server_config> &project) {
    // mcp/config.py merge_mcp_configs: {**global, **project} - a project
    // entry replaces the same-named global entry in place, new names append.
    kimix::vector<server_config> merged = global;
    for (const server_config &p : project) {
        bool replaced = false;
        for (server_config &g : merged) {
            if (g.name == p.name) {
                g = p;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.push_back(p);
        }
    }
    return merged;
}

// ---------------------------------------------------------------------------
// Protocol layer (pure)
// ---------------------------------------------------------------------------

namespace proto {

bool parse_message(kimix::string_view line, message &out) {
    out = message{};
    yyjson_doc *doc = parse_json(line);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    // The reference SDK validates jsonrpc == "2.0"; a frame without the
    // member is tolerated, one with a different value is skipped.
    const yyjson_val *version = yyjson_obj_get(root, "jsonrpc");
    if (version != nullptr && !yyjson_is_str(version)) {
        yyjson_doc_free(doc);
        return false;
    }
    const yyjson_val *id = yyjson_obj_get(root, "id");
    const kimix::string method = obj_str(root, "method");
    if (!method.empty()) {
        out.method = method;
        out.params_json = val_to_json(yyjson_obj_get(root, "params"));
        if (id != nullptr) {
            out.type = message::kind::request;
            out.id = val_to_json(id);
        } else {
            out.type = message::kind::notification;
        }
    } else if (id != nullptr) {
        out.type = message::kind::response;
        out.id = val_to_json(id);
        out.result_json = val_to_json(yyjson_obj_get(root, "result"));
        out.error_json = val_to_json(yyjson_obj_get(root, "error"));
    }
    yyjson_doc_free(doc);
    return out.type != message::kind::none;
}

kimix::string make_request(uint64_t id, kimix::string_view method,
                           kimix::string_view params_json) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add_str(mdoc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_uint(mdoc, root, "id", id);
    yyjson_mut_obj_add_strcpy(mdoc, root, "method",
                              kimix::string(method).c_str());
    add_raw_member(mdoc, root, "params", params_json);
    return write_frame(mdoc);
}

kimix::string make_notification(kimix::string_view method,
                                kimix::string_view params_json) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add_str(mdoc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(mdoc, root, "method",
                              kimix::string(method).c_str());
    add_raw_member(mdoc, root, "params", params_json);
    return write_frame(mdoc);
}

kimix::string make_response(kimix::string_view raw_id_json,
                            kimix::string_view result_json) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add_str(mdoc, root, "jsonrpc", "2.0");
    if (!add_raw_member(mdoc, root, "id", raw_id_json)) {
        yyjson_mut_obj_add_null(mdoc, root, "id");
    }
    if (!add_raw_member(mdoc, root, "result", result_json)) {
        yyjson_mut_obj_add_null(mdoc, root, "result");
    }
    return write_frame(mdoc);
}

kimix::string make_response_error(kimix::string_view raw_id_json, int32_t code,
                                  kimix::string_view message) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (mdoc == nullptr) {
        return kimix::string();
    }
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add_str(mdoc, root, "jsonrpc", "2.0");
    if (!add_raw_member(mdoc, root, "id", raw_id_json)) {
        yyjson_mut_obj_add_null(mdoc, root, "id");
    }
    yyjson_mut_val *err = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_int(mdoc, err, "code", code);
    yyjson_mut_obj_add_strcpy(mdoc, err, "message",
                              kimix::string(message).c_str());
    yyjson_mut_obj_add(root, yyjson_mut_str(mdoc, "error"), err);
    return write_frame(mdoc);
}

kimix::string initialize_request(uint64_t id) {
    // mcp/client/session.py start: protocol version, empty capabilities and
    // the client identity. The reference negotiates the version with the
    // server's response; this client accepts whatever the server answers.
    const char *params =
        R"JSON({"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"kimix-cli","version":")JSON"
        KIMIX_CORE_VERSION R"JSON("}})JSON";
    return make_request(id, "initialize", params);
}

kimix::string initialized_notification() {
    return make_notification("notifications/initialized", kimix::string_view());
}

kimix::string tools_list_request(uint64_t id, kimix::string_view cursor) {
    kimix::string params;
    if (!cursor.empty()) {
        params = "{\"cursor\":\"";
        params.append(cursor.data(), cursor.size());
        params += "\"}";
    }
    return make_request(id, "tools/list", params);
}

// tools/list, resources/list and prompts/list share the cursor-param shape.
kimix::string paged_list_request(uint64_t id, kimix::string_view method,
                                 kimix::string_view cursor) {
    kimix::string params;
    if (!cursor.empty()) {
        params = "{\"cursor\":\"";
        params.append(cursor.data(), cursor.size());
        params += "\"}";
    }
    return make_request(id, method, params);
}

kimix::string tools_call_request(uint64_t id, kimix::string_view name,
                                 kimix::string_view arguments_json) {
    kimix::string params = "{\"name\":\"";
    params.append(name.data(), name.size());
    params += "\",\"arguments\":";
    if (!arguments_json.empty()) {
        params.append(arguments_json.data(), arguments_json.size());
    } else {
        params += "{}";
    }
    params += "}";
    return make_request(id, "tools/call", params);
}

kimix::string resources_list_request(uint64_t id, kimix::string_view cursor) {
    return paged_list_request(id, "resources/list", cursor);
}

kimix::string prompts_list_request(uint64_t id, kimix::string_view cursor) {
    return paged_list_request(id, "prompts/list", cursor);
}

bool parse_tools_list(kimix::string_view result_json,
                      kimix::vector<tool_info> &out, kimix::string &next_cursor,
                      kimix::string &error) {
    out.clear();
    next_cursor.clear();
    error.clear();
    yyjson_doc *doc = parse_json(result_json);
    if (doc == nullptr) {
        error = "malformed tools/list result";
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const yyjson_val *tools =
        (root != nullptr && yyjson_is_obj(root)) ? yyjson_obj_get(root, "tools")
                                                 : nullptr;
    if (tools == nullptr || !yyjson_is_arr(tools)) {
        yyjson_doc_free(doc);
        error = "malformed tools/list result";
        return false;
    }
    size_t idx = 0;
    size_t idxmax = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(tools, idx, idxmax, item) {
        if (item == nullptr || !yyjson_is_obj(item)) {
            continue;
        }
        tool_info info;
        info.name = obj_str(item, "name");
        if (info.name.empty()) {
            continue; // a tool without a name cannot be addressed
        }
        info.description = obj_str(item, "description");
        const kimix::string schema = val_to_json(yyjson_obj_get(item, "inputSchema"));
        info.input_schema_json =
            !schema.empty() ? schema : kimix::string("{}");
        out.push_back(std::move(info));
    }
    next_cursor = obj_str(root, "nextCursor");
    yyjson_doc_free(doc);
    return true;
}

// The truncation note appended when the shared budget is exhausted
// (toolset.py convert_mcp_tool_result).
kimix::string truncation_note() {
    kimix::string note = "\n\n[Output truncated: exceeded ";
    note += std::to_string(static_cast<long long>(kMaxOutputChars));
    note += " character limit. Use pagination or more specific queries to get "
            "remaining content.]";
    return note;
}

kimix::string timeout_error_text(kimix::string_view tool_name) {
    kimix::string text = "Timeout while calling MCP tool `";
    text.append(tool_name.data(), tool_name.size());
    text += "`. You may explain to the user that the timeout config is set too "
            "low.";
    return text;
}

kimix::string mcp_tool_description(kimix::string_view server_name,
                                   kimix::string_view tool_description) {
    // toolset.py MCPTool.__init__ (2105-2110): the prefix is `or`-ed with the
    // reference's fallback description.
    kimix::string text = "This is an MCP (Model Context Protocol) tool from MCP "
                         "server `";
    text.append(server_name.data(), server_name.size());
    text += "`.\n\n";
    if (tool_description.empty()) {
        text += "No description provided.";
    } else {
        text.append(tool_description.data(), tool_description.size());
    }
    return text;
}

void extract_tool_result(kimix::string_view result_json, tool_result &out) {
    out = tool_result{};
    yyjson_doc *doc = parse_json(result_json);
    if (doc == nullptr) {
        out.is_error = true;
        out.text = "[Unsupported content: malformed tool result]";
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const yyjson_val *content =
        (root != nullptr && yyjson_is_obj(root)) ? yyjson_obj_get(root, "content")
                                                 : nullptr;
    if (content != nullptr && !yyjson_is_arr(content)) {
        content = nullptr;
    }
    size_t budget = kMaxOutputChars;
    if (content != nullptr) {
        size_t idx = 0;
        size_t idxmax = 0;
        yyjson_val *part = nullptr;
        yyjson_arr_foreach(content, idx, idxmax, part) {
            if (part == nullptr || !yyjson_is_obj(part)) {
                continue;
            }
            const kimix::string type = obj_str(part, "type");
            if (type == "text") {
                kimix::string text = obj_str(part, "text");
                if (budget <= 0) {
                    out.truncated = true;
                    continue;
                }
                if (text.size() > budget) {
                    text.resize(budget);
                    out.truncated = true;
                }
                budget -= text.size();
                if (!out.text.empty()) {
                    out.text += "\n";
                }
                out.text += text;
                continue;
            }
            // The native external-tool result channel carries one string, so
            // media parts cannot be represented - they degrade to the
            // reference's "[Unsupported content: ...]" placeholder (the
            // reference's convert_mcp_content ValueError wording).
            ++out.unsupported;
            kimix::string placeholder = "[Unsupported content: ";
            const kimix::string mime = obj_str(part, "mimeType");
            if (!mime.empty() && type != "text") {
                placeholder += "Unsupported mime type: ";
                placeholder += mime;
            } else {
                placeholder += "Unsupported MCP tool result part: {\"type\":\"";
                placeholder += type;
                placeholder += "\"}";
            }
            placeholder += "]";
            if (budget <= 0) {
                out.truncated = true;
                continue;
            }
            if (placeholder.size() > budget) {
                placeholder.resize(budget);
                out.truncated = true;
            }
            budget -= placeholder.size();
            if (!out.text.empty()) {
                out.text += "\n";
            }
            out.text += placeholder;
        }
    }
    const yyjson_val *is_err = yyjson_obj_get(root, "isError");
    out.is_error = yyjson_is_bool(is_err) && yyjson_get_bool(is_err);
    if (out.truncated) {
        out.text += truncation_note();
    }
    yyjson_doc_free(doc);
}

} // namespace proto

// ---------------------------------------------------------------------------
// Client (process layer)
// ---------------------------------------------------------------------------

McpClient::McpClient(server_config config, client_options options)
    : _config(std::move(config)), _options(options) {
    _status.name = _config.name;
}

McpClient::~McpClient() {
    shutdown();
}

bool McpClient::spawn(kimix::string &error) {
    kimix::builtin_tools::proc::run_options opts;
    opts.argv.push_back(_config.command);
    for (const kimix::string &a : _config.args) {
        opts.argv.push_back(a);
    }
    for (const kimix::string &e : _config.env) {
        opts.extra_env.push_back(e);
    }
    // A background task has no total-time bound: the server lives until
    // stop_task. Every protocol wait is bounded by the client options.
    opts.timeout_ms = 0;
    opts.inactivity_timeout_ms = 0;
    opts.output_cap_chars = _options.output_cap_chars;
    // The runner uniquifies collisions with the same "_<n>" suffix the
    // Python allocator adds (process_runner.h run_options.requested_task_id).
    opts.requested_task_id = "mcp_" + _config.name;
    task_handle handle;
    const tool_error err = start_task(opts, handle);
    if (err.failed()) {
        error = "spawn failed: " + err.message;
        return false;
    }
    _task_id = handle.task_id;
    return true;
}

bool McpClient::send_frame(kimix::string_view frame, kimix::string &error) {
    if (!_task_id.has_value()) {
        error = "server is not running";
        return false;
    }
    // Frames are newline-terminated already; the runner's add_newline stays
    // off so the wire bytes are exactly what parse_message expects.
    const tool_error err = send_task(*_task_id, frame, false);
    if (err.failed()) {
        error = "send failed: " + err.message;
        return false;
    }
    return true;
}

void McpClient::answer_server_request(const proto::message &msg) {
    // The SDK's default handlers while a real request is in flight: ping
    // gets a result, everything else the JSON-RPC "method not found" error.
    kimix::string error;
    if (msg.method == "ping") {
        (void)send_frame(proto::make_response(msg.id, "{}"), error);
    } else {
        (void)send_frame(
            proto::make_response_error(msg.id, -32601, "Method not found"),
            error);
    }
}

bool McpClient::drain_pending(const kimix::string &want_id,
                              proto::message &out) {
    size_t start = 0;
    while (true) {
        const size_t nl = _pending.find('\n', start);
        if (nl == kimix::string::npos) {
            break;
        }
        kimix::string_view line(_pending.data() + start, nl - start);
        start = nl + 1;
        // CRLF tolerance: a server writing /r/n (or logging) still parses.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        proto::message msg;
        if (!proto::parse_message(line, msg)) {
            continue; // stderr noise / partial line: skipped like the SDK
        }
        if (msg.type == proto::message::kind::response && msg.id == want_id) {
            _pending.erase(0, start);
            out = std::move(msg);
            return true;
        }
        if (msg.type == proto::message::kind::request) {
            answer_server_request(msg);
        }
        // notifications and stale responses are dropped
    }
    _pending.erase(0, start);
    return false;
}

bool McpClient::request_frame(kimix::string_view frame, uint64_t id,
                              int64_t timeout_ms, proto::message &out,
                              kimix::string &error) {
    if (!send_frame(frame, error)) {
        return false;
    }
    const kimix::string want_id(
        std::to_string(static_cast<long long>(id)).c_str());
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (drain_pending(want_id, out)) {
            return true;
        }
        if (now_ms() >= deadline) {
            error = "timed out waiting for a response";
            return false;
        }
        const task_status_info st = query_task(*_task_id);
        if (st.exists && st.exited) {
            // Drain whatever the child still buffered before declaring death.
            kimix::string tail;
            (void)read_task(*_task_id, tail);
            _pending += tail;
            if (drain_pending(want_id, out)) {
                return true;
            }
            error = "server exited before responding";
            return false;
        }
        (void)wait_task(*_task_id, "\n", 100);
        kimix::string chunk;
        (void)read_task(*_task_id, chunk);
        _pending += chunk;
    }
}

void McpClient::best_effort_list(bool resources, int64_t deadline_ms) {
    // resources/prompts discovery is status-only; any failure is silent (the
    // reference logs it at debug level and keeps booting).
    proto::message resp;
    kimix::string error;
    const uint64_t id = _next_id++;
    const kimix::string frame =
        resources ? proto::resources_list_request(id, kimix::string_view())
                  : proto::prompts_list_request(id, kimix::string_view());
    const int64_t bounded =
        std::max<int64_t>(1, std::min<int64_t>(deadline_ms - now_ms(), 2000));
    if (!request_frame(frame, id, bounded, resp, error) ||
        resp.result_json.empty()) {
        return;
    }
    yyjson_doc *doc = parse_json(resp.result_json);
    if (doc == nullptr) {
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *key = resources ? "resources" : "prompts";
    const yyjson_val *arr =
        (root != nullptr && yyjson_is_obj(root)) ? yyjson_obj_get(root, key)
                                                 : nullptr;
    if (arr != nullptr && yyjson_is_arr(arr)) {
        size_t idx = 0;
        size_t idxmax = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(arr, idx, idxmax, item) {
            if (item == nullptr || !yyjson_is_obj(item)) {
                continue;
            }
            const kimix::string label =
                resources ? obj_str(item, "uri") : obj_str(item, "name");
            if (label.empty()) {
                continue;
            }
            if (resources) {
                _status.resources.push_back(label);
            } else {
                _status.prompts.push_back(label);
            }
        }
    }
    yyjson_doc_free(doc);
}

void McpClient::shutdown() {
    if (_task_id.has_value()) {
        kimix::string final_output;
        (void)stop_task(*_task_id, final_output);
        _task_id.reset();
    }
    _pending.clear();
    if (_status.state == server_state::connected ||
        _status.state == server_state::connecting) {
        _status.state = server_state::not_started;
    }
}

bool McpClient::connect(kimix::string &error) {
    if (_status.state == server_state::connected) {
        return true;
    }
    if (_config.command.empty()) {
        error = _config.url.empty()
                    ? "server names neither command nor url"
                    : "remote servers are not supported by the native client "
                      "(url: " +
                          _config.url + ")";
        _status.state = server_state::failed;
        _status.error = error;
        return false;
    }
    _status.state = server_state::connecting;
    if (!spawn(error)) {
        _status.state = server_state::failed;
        _status.error = error;
        return false;
    }
    const int64_t deadline = now_ms() + _options.connect_timeout_ms;
    auto fail = [this](kimix::string &error) {
        shutdown();
        _status.state = server_state::failed;
        _status.error = error;
        return false;
    };
    proto::message resp;
    const uint64_t init_id = _next_id++;
    if (!request_frame(proto::initialize_request(init_id), init_id,
                       std::max<int64_t>(1, deadline - now_ms()), resp, error)) {
        return fail(error);
    }
    if (resp.result_json.empty() || resp.result_json[0] != '{') {
        error = "malformed initialize result";
        return fail(error);
    }
    kimix::string send_error;
    if (!send_frame(proto::initialized_notification(), send_error)) {
        error = send_error;
        return fail(error);
    }
    // tools/list with cursor paging (a bounded page count guards against a
    // server that never empties its cursor).
    kimix::string cursor;
    for (int page = 0; page < 100; ++page) {
        proto::message tl;
        const uint64_t list_id = _next_id++;
        if (!request_frame(proto::tools_list_request(list_id, cursor), list_id,
                           std::max<int64_t>(1, deadline - now_ms()), tl,
                           error)) {
            return fail(error);
        }
        kimix::string next;
        if (!proto::parse_tools_list(tl.result_json, _tools, next, error)) {
            return fail(error);
        }
        if (next.empty()) {
            break;
        }
        cursor = next;
    }
    best_effort_list(/*resources=*/true, deadline);
    best_effort_list(/*resources=*/false, deadline);
    _status.state = server_state::connected;
    _status.error.clear();
    return true;
}

bool McpClient::call_tool(kimix::string_view name,
                          kimix::string_view arguments_json,
                          kimix::string &payload_json, kimix::string &error) {
    if (!connected()) {
        // Dead/hung server: the reference's raised exception becomes the
        // runtime-error channel ("MCP tool call failed: {tool}: {error}").
        payload_json = make_payload(
            "error", "MCP tool call failed: " + kimix::string(name) +
                         ": server is not connected.",
            kimix::string_view());
        return true;
    }
    bool known = false;
    for (const proto::tool_info &t : _tools) {
        if (t.name == name) {
            known = true;
            break;
        }
    }
    if (!known) {
        error = "Tool `" + kimix::string(name) + "` not found on server `" +
                _config.name + "`.";
        return false;
    }
    const uint64_t id = _next_id++;
    proto::message resp;
    if (!request_frame(proto::tools_call_request(id, name, arguments_json), id,
                       _options.call_timeout_ms, resp, error)) {
        kimix::string lowered(error);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (lowered.find("timed out") != kimix::string::npos ||
            lowered.find("timeout") != kimix::string::npos) {
            // toolset.py 2029-2035: the timeout ToolError (message + brief).
            payload_json = make_payload("tool_error",
                                        proto::timeout_error_text(name),
                                        kimix::string_view());
            return true;
        }
        payload_json = make_payload("error",
                                    "MCP tool call failed: " +
                                        kimix::string(name) + ": " + error,
                                    kimix::string_view());
        return true;
    }
    if (!resp.error_json.empty()) {
        // A JSON-RPC error response: the reference re-raises, the dispatcher
        // renders the runtime-error wording.
        yyjson_doc *doc = parse_json(resp.error_json);
        kimix::string detail;
        if (doc != nullptr) {
            detail = obj_str(yyjson_doc_get_root(doc), "message");
            yyjson_doc_free(doc);
        }
        payload_json = make_payload(
            "error",
            "Error running tool: " +
                (detail.empty() ? kimix::string("server returned an error")
                                : detail),
            kimix::string_view());
        return true;
    }
    proto::tool_result result;
    proto::extract_tool_result(resp.result_json, result);
    if (result.is_error) {
        // toolset.py convert_mcp_tool_result: ToolError(output=content,
        // message="Tool returned an error. ...").
        payload_json =
            make_payload("tool_error",
                         "Tool returned an error. The output may be error "
                         "message or incomplete output",
                         result.text);
        return true;
    }
    payload_json = make_payload("ok", kimix::string_view(), result.text);
    return true;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

McpManager &McpManager::instance() {
    static McpManager manager;
    return manager;
}

void McpManager::register_tools(McpClient &client, server_status &st) {
    kimix::builtin_tools::ToolRegistry &reg =
        kimix::builtin_tools::ToolRegistry::instance();
    for (const proto::tool_info &t : client.tools()) {
        if (reg.find(t.name) != nullptr) {
            // toolset.py register (collision branch): skipped with the
            // reference's message.
            st.collisions.push_back(t.name);
            continue;
        }
        // The client pointer is stable (unique_ptr in _clients); the lambda
        // only outlives the manager's own shutdown order (tools are
        // unregistered before the children are stopped).
        McpClient *c = &client;
        const kimix::string tool_name = t.name;
        kimix::builtin_tools::ExternalToolCall call =
            [c, tool_name](kimix::string_view arguments_json,
                           kimix::string &result_json, kimix::string &error) {
                return c->call_tool(tool_name, arguments_json, result_json,
                                    error);
            };
        kimix::string error;
        if (!reg.register_external_tool(
                t.name,
                proto::mcp_tool_description(client.config().name, t.description),
                t.input_schema_json, call, error)) {
            st.skipped.push_back(t.name + ": " + error);
            continue;
        }
        st.tools.push_back(t.name);
        _registered_tools.push_back(t.name);
    }
}

bool McpManager::start_all(const kimix::vector<server_config> &configs,
                           kimix::vector<kimix::string> &status_lines,
                           kimix::string &error) {
    status_lines.clear();
    error.clear();
    bool any = false;
    for (const server_config &cfg : configs) {
        auto client = std::make_unique<McpClient>(cfg, client_options{});
        kimix::string connect_error;
        const bool ok = client->connect(connect_error);
        server_status st = client->status();
        if (ok) {
            register_tools(*client, st);
            _clients.push_back(std::move(client));
            any = true;
            kimix::string line = "MCP server " + cfg.name + ": connected (";
            line += std::to_string(static_cast<long long>(st.tools.size()));
            line += " tools)";
            status_lines.push_back(std::move(line));
        } else {
            status_lines.push_back("MCP server " + cfg.name +
                                   ": failed: " + connect_error);
        }
        _statuses.push_back(std::move(st));
    }
    if (!any && error.empty()) {
        error = "no MCP server connected";
    }
    return any;
}

void McpManager::shutdown() {
    if (!_registered_tools.empty()) {
        kimix::builtin_tools::ToolRegistry &reg =
            kimix::builtin_tools::ToolRegistry::instance();
        for (const kimix::string &name : _registered_tools) {
            (void)reg.unregister_tool(name);
        }
        _registered_tools.clear();
    }
    for (const auto &client : _clients) {
        client->shutdown();
    }
    _clients.clear();
}

} // namespace kimix::mcp
