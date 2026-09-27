// test_mcp_client.cpp - F7: the minimal MCP stdio client.
//
// Layered like the feature itself: the PROTOCOL layer (frame parse/serialize,
// tools/list + tool-result parsing, config parsing/merging) is tested purely;
// one integration test drives the full client against a SCRIPTED python child
// speaking newline-delimited JSON-RPC (skipped cleanly when no python is
// available on PATH, like the other interpreter-dependent suites).

#include "ut/ut.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "builtin_tools/process_runner.h"
#include "mcp/mcp_client.h"

using namespace boost::ut;

namespace {

// A stable per-suite path for the scripted server: removed and rewritten on
// every run (no tmpnam - the MSVC secure-CRT flags it).
kimix::string server_script_path() {
    std::error_code ec;
    const kimix::filesystem::path p =
        kimix::filesystem::temp_directory_path(ec) / "kimix_mcp_server.py";
    return kimix::to_string(p);
}

bool python_available() {
    static const bool available = [] {
        kimix::builtin_tools::proc::run_options opts;
#ifdef KIMIX_PLATFORM_WINDOWS
        opts.argv = {kimix::string("python"), kimix::string("--version")};
#else
        opts.argv = {kimix::string("python3"), kimix::string("--version")};
#endif
        opts.timeout_ms = 10000;
        const kimix::builtin_tools::proc::run_result r =
            kimix::builtin_tools::proc::run_process(opts);
        return r.status == kimix::builtin_tools::tool_status::ok;
    }();
    return available;
}

#ifdef KIMIX_PLATFORM_WINDOWS
constexpr const char *kPythonExe = "python";
#else
constexpr const char *kPythonExe = "python3";
#endif

const char *kServerScript = R"PY(import sys, json
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
    except Exception:
        continue
    m = req.get("method")
    if m == "initialize":
        print(json.dumps({"jsonrpc": "2.0", "id": req["id"],
                          "result": {"protocolVersion": "2024-11-05",
                                     "serverInfo": {"name": "scripted"}}}),
              flush=True)
    elif m == "tools/list":
        print(json.dumps({"jsonrpc": "2.0", "id": req["id"],
                          "result": {"tools": [
                              {"name": "echo", "description": "Echo tool",
                               "inputSchema": {"type": "object",
                                               "properties": {"text": {"type": "string"}}}},
                              {"name": "read", "description": "A tool that collides",
                               "inputSchema": {"type": "object"}},
                              {"name": "slow", "description": "Never answers in time",
                               "inputSchema": {"type": "object"}}]}}),
              flush=True)
    elif m == "tools/call" and req.get("params", {}).get("name") == "slow":
        import time
        time.sleep(5)
        print(json.dumps({"jsonrpc": "2.0", "id": req["id"],
                          "result": {"content": [{"type": "text",
                                                  "text": "finally"}],
                                     "isError": False}}),
              flush=True)
    elif m == "tools/call":
        print(json.dumps({"jsonrpc": "2.0", "id": req["id"],
                          "result": {"content": [{"type": "text",
                                                  "text": "hello from mcp"}],
                                     "isError": False}}),
              flush=True)
    elif m == "ping":
        print(json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": {}}),
              flush=True)
    # unknown methods are deliberately never answered (timeout coverage)
)PY";

} // namespace

int main() {
    // =======================================================================
    // Protocol layer (pure)
    // =======================================================================
    "parse_message_classifies_frames"_test = [] {
        kimix::mcp::proto::message msg;
        expect(kimix::mcp::proto::parse_message(
            R"({"jsonrpc":"2.0","id":7,"result":{"tools":[]}})", msg));
        expect(msg.type == kimix::mcp::proto::message::kind::response);
        expect(msg.id == kimix::string("7"));
        expect(msg.result_json == kimix::string(R"({"tools":[]})"));

        expect(kimix::mcp::proto::parse_message(
            R"({"jsonrpc":"2.0","id":3,"method":"ping","params":{}})", msg));
        expect(msg.type == kimix::mcp::proto::message::kind::request);
        expect(msg.method == kimix::string("ping"));

        expect(kimix::mcp::proto::parse_message(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})", msg));
        expect(msg.type == kimix::mcp::proto::message::kind::notification);

        expect(!kimix::mcp::proto::parse_message("not json at all", msg));
        expect(msg.type == kimix::mcp::proto::message::kind::none);
        // stderr noise with embedded quotes: skipped, never a crash
        expect(!kimix::mcp::proto::parse_message("[WARN] \"quoted\" noise", msg));
    };

    "frames_roundtrip"_test = [] {
        const kimix::string req =
            kimix::mcp::proto::make_request(5, "tools/list", "");
        expect(req == kimix::string(
                          R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})" "\n"));
        const kimix::string notif =
            kimix::mcp::proto::make_notification("notifications/initialized", "");
        expect(notif.find("\"id\"") == kimix::string::npos);
        const kimix::string resp =
            kimix::mcp::proto::make_response("\"abc\"", "{}");
        expect(resp.find(R"("id":"abc")") != kimix::string::npos);
        const kimix::string err =
            kimix::mcp::proto::make_response_error("9", -32601, "nope");
        expect(err.find(R"("code":-32601)") != kimix::string::npos);
        expect(err.find("\"nope\"") != kimix::string::npos);
    };

    "parse_tools_list_pages"_test = [] {
        kimix::vector<kimix::mcp::proto::tool_info> tools;
        kimix::string cursor;
        kimix::string error;
        expect(kimix::mcp::proto::parse_tools_list(
            R"({"tools":[{"name":"echo","description":"Echo","inputSchema":{"type":"object"}},
                          {"name":"bad"},{"description":"no name"}],
                          "nextCursor":"page2"})",
            tools, cursor, error));
        expect(eq(tools.size(), size_t(2)));
        expect(tools[0].name == kimix::string("echo"));
        expect(tools[0].description == kimix::string("Echo"));
        expect(cursor == kimix::string("page2"));

        expect(!kimix::mcp::proto::parse_tools_list("42", tools, cursor, error));
        expect(!error.empty());
    };

    "extract_tool_result_budget_and_placeholders"_test = [] {
        kimix::mcp::proto::tool_result result;
        kimix::mcp::proto::extract_tool_result(
            R"({"content":[{"type":"text","text":"hello"},
                            {"type":"image","data":"QUJD","mimeType":"image/png"}],
                            "isError":false})",
            result);
        expect(!result.is_error);
        expect(result.text == kimix::string("hello\n[Unsupported content: "
                                            "Unsupported mime type: image/png]"));
        expect(eq(result.unsupported, size_t(1)));

        // Over-budget text is truncated in place and gets the reference note.
        kimix::string huge = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        huge += kimix::string(100001 + 80, 'x');
        huge += "\"}]}";
        kimix::mcp::proto::extract_tool_result(huge, result);
        expect(result.truncated);
        expect(result.text.find('x') != kimix::string::npos);
        expect(result.text.find(
                   "[Output truncated: exceeded 100000 character limit. Use "
                   "pagination or more specific queries to get remaining "
                   "content.]") != kimix::string::npos);

        // isError maps to the reference's ToolError message.
        kimix::mcp::proto::extract_tool_result(
            R"({"content":[{"type":"text","text":"boom"}],"isError":true})",
            result);
        expect(result.is_error);
        expect(result.text == kimix::string("boom"));
    };

    "reference_wordings"_test = [] {
        expect(kimix::mcp::proto::timeout_error_text(kimix::string("echo")) ==
               kimix::string("Timeout while calling MCP tool `echo`. You may "
                             "explain to the user that the timeout config is "
                             "set too low."));
        expect(kimix::mcp::proto::mcp_tool_description(kimix::string("srv"),
                                                       kimix::string("")) ==
               kimix::string("This is an MCP (Model Context Protocol) tool from "
                             "MCP server `srv`.\n\nNo description provided."));
        expect(kimix::mcp::proto::mcp_tool_description(kimix::string("srv"),
                                                       kimix::string("Echo.")) ==
               kimix::string("This is an MCP (Model Context Protocol) tool from "
                             "MCP server `srv`.\n\nEcho."));
    };

    "config_parse_and_merge"_test = [] {
        kimix::vector<kimix::mcp::server_config> servers;
        kimix::vector<kimix::string> warnings;
        expect(!kimix::mcp::parse_mcp_config_text("[1,2]", servers, warnings));
        expect(kimix::mcp::parse_mcp_config_text(
            R"({"mcpServers":{
                  "fs": {"command":"npx","args":["-y","fs"],"env":{"K":"V"}},
                  "remote": {"url":"https://x","transport":"streamable-http"},
                  "broken": {"args":[]}
                }})",
            servers, warnings));
        expect(eq(servers.size(), size_t(2)));
        expect(warnings.size() == 1); // "broken" names neither command nor url
        expect(servers[0].name == kimix::string("fs"));
        expect(servers[0].command == kimix::string("npx"));
        expect(servers[0].args.size() == 2);
        expect(servers[0].env[0] == kimix::string("K=V"));
        expect(servers[1].name == kimix::string("remote"));
        expect(servers[1].transport == kimix::string("streamable-http"));

        // merge_mcp_configs: project wins, order kept, new names append.
        kimix::vector<kimix::mcp::server_config> global;
        kimix::mcp::server_config g;
        g.name = "fs";
        g.command = "global-fs";
        global.push_back(g);
        g.name = "only-global";
        global.push_back(g);
        const kimix::vector<kimix::mcp::server_config> merged =
            kimix::mcp::merge_server_lists(global, servers);
        expect(eq(merged.size(), size_t(3)));
        expect(merged[0].name == kimix::string("fs"));
        expect(merged[0].command == kimix::string("npx")); // project won
        // {**global, **project}: the global order is kept, the project's new
        // name appends after the untouched global names.
        expect(merged[1].name == kimix::string("only-global"));
        expect(merged[2].name == kimix::string("remote"));
    };

    // =======================================================================
    // Integration: the scripted stdio server
    // =======================================================================
    "client_against_scripted_server"_test = [] {
        if (!python_available()) {
            printf("[skip] no python on PATH - the MCP integration test needs "
                   "a scripted interpreter child\n");
            return;
        }
        // Write the server script to a temp file.
        const kimix::string path = server_script_path();
        {
            std::FILE *f = std::fopen(path.c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                std::fwrite(kServerScript, 1, std::strlen(kServerScript), f);
                std::fclose(f);
            }
        }
        kimix::mcp::server_config cfg;
        cfg.name = "scripted";
        cfg.command = kPythonExe;
        cfg.args.push_back(path);
        kimix::mcp::client_options options;
        options.connect_timeout_ms = 15000;
        kimix::mcp::McpClient client(cfg, options);
        kimix::string error;
        const bool ok = client.connect(error);
        expect(ok) << error;
        if (ok) {
            expect(client.status().state ==
                   kimix::mcp::server_state::connected);
            expect(eq(client.tools().size(), size_t(3)));
            expect(client.tools()[0].name == kimix::string("echo"));
            expect(eq(client.tools().size(), size_t(3)));

            // A successful call comes back as the ok payload with the text.
            kimix::string payload;
            expect(client.call_tool("echo", "{\"text\":\"hi\"}", payload, error));
            expect(payload.find("\"status\":\"ok\"") != kimix::string::npos);
            expect(payload.find("hello from mcp") != kimix::string::npos);

            // An unknown tool is a caller mistake (false + error), not a
            // payload.
            kimix::string payload2;
            expect(!client.call_tool("nope", "{}", payload2, error));
            expect(error.find("not found") != kimix::string::npos);
        }
        client.shutdown();
        expect(client.status().state != kimix::mcp::server_state::connected);
        std::remove(path.c_str());
    };

    "call_timeout_yields_the_reference_tool_error"_test = [] {
        if (!python_available()) {
            return; // covered by the skip note above
        }
        const kimix::string path = server_script_path();
        {
            std::FILE *f = std::fopen(path.c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                std::fwrite(kServerScript, 1, std::strlen(kServerScript), f);
                std::fclose(f);
            }
        }
        kimix::mcp::server_config cfg;
        cfg.name = "scripted-slow";
        cfg.command = kPythonExe;
        cfg.args.push_back(path);
        kimix::mcp::client_options options;
        options.connect_timeout_ms = 15000;
        options.call_timeout_ms = 300; // the scripted server never answers
        kimix::mcp::McpClient client(cfg, options);
        kimix::string error;
        if (client.connect(error)) {
                kimix::string payload;
                expect(client.call_tool("slow", "{}", payload, error));
                expect(payload.find("\"status\":\"tool_error\"") !=
                       kimix::string::npos);
                expect(payload.find("Timeout while calling MCP tool `slow`.") !=
                       kimix::string::npos);
        } else {
            printf("[skip] scripted server failed to start: %s\n",
                   error.c_str());
        }
        client.shutdown();
        std::remove(path.c_str());
    };
}
