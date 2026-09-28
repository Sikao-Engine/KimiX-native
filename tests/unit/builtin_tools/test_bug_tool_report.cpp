// test_bug_tool_report.cpp - Reproductions for the tool-behaviour defects in
// bug_tool.md ("Bug Tool Report - 工具行为测试发现").
//
// Every test below pins the *documented* tool contract (the registered
// schema/description) that the report found broken:
//   1. bash       - execute/interactive must surface stdout + exit code and a
//                   usable task_id; long commands with literal \n escapes must
//                   run instead of being refused as "malformed parameters".
//   2. edit       - the registered file-based contract (file_path +
//                   old_string/new_string, mode auto/replace/sloppy with
//                   default auto) must edit the file on disk.
//   4. web_search - a query must produce results (or a *visible* diagnostic),
//                   never an empty ERROR.
//   5. fetch_url  - a url must be fetched to markdown (optionally saved to
//                   output_path); errors must be visible.
//   6. read_image - file_path alone must read the image from disk and return
//                   metadata + a data_url (never "Tool output is empty").
//   7. grep       - output_mode=files_with_matches must list the files.
//   8. read       - the char window slices the rendered output as documented.
//   9. write      - a new file says "created", an existing one "overwritten".
//  10. subagent   - run_in_background=true returns immediately with a session
//                   id; the settled outcome reaches the parent at the next turn.
//  11. interrupt_agent - interrupting an already-finished session is graceful.
//  12. retrieve   - the BM25 ranking assigns non-zero relevance to the best hit.
//  14. plan       - a session with the plan tools enabled carries a usable
//                   default plan_writing_path (cli_default_plan_path).
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "agent/tool_argument_repair.h"
#include "builtin_tools/agent_tool.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/edit_tool.h"
#include "builtin_tools/fetch_url_tool.h"
#include "builtin_tools/grep_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/web_search_tool.h"
#include "builtin_tools/write_tool.h"
#include "builtin_tools/read_image_tool.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/plan_tool.h"
#include "builtin_tools/job_output_tool.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>

#include "cli/cli_common.h"

using namespace boost::ut;
using namespace boost::ut::literals;
namespace bt = kimix::builtin_tools;

namespace {

// 1x1 red PNG (generated with zlib; validated by the read_image test).
constexpr const char *k_red_png_b64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/"
    "pLvAAAAAElFTkSuQmCC";

bool base64_decode(const std::string &in, std::string &out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

void write_file(const kimix::string &path, const kimix::string &content) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    expect(f != nullptr);
    if (f != nullptr) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

kimix::string read_file(const kimix::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    expect(f != nullptr);
    if (f == nullptr) return {};
    kimix::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// A field of the serialized result payload as a string ("" when absent).
kimix::string payload_field(const bt::Tool &tool, const char *key) {
    kimix::vector<char> out;
    tool.result_json(out);
    kimix::string json(out.data(), out.size());
    yyjson_doc *doc = yyjson_read_opts(json.data(), json.size(),
                                       YYJSON_READ_STOP_WHEN_DONE,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    kimix::string value;
    if (doc != nullptr) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root != nullptr && yyjson_is_obj(root)) {
            yyjson_val *v = yyjson_obj_get(root, key);
            if (v != nullptr && yyjson_is_str(v)) {
                value.assign(yyjson_get_str(v),
                             static_cast<size_t>(yyjson_get_len(v)));
            }
        }
        yyjson_doc_free(doc);
    }
    return value;
}

kimix::string payload_raw(const bt::Tool &tool) {
    kimix::vector<char> out;
    tool.result_json(out);
    return kimix::string(out.data(), out.size());
}

bool payload_has(const bt::Tool &tool, const char *needle) {
    return payload_raw(tool).find(needle) != kimix::string::npos;
}

bt::ToolParams params_of(
    std::initializer_list<std::pair<const char *, bt::ValueElement>> kv) {
    bt::ToolParams p;
    for (const auto &e : kv) {
        p.values[e.first] = e.second;
    }
    return p;
}

// Minimal always-ok chat backend for soul-level tests.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &,
         const kimix::llm::AbortCheck *) override {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

// Extract the subagent session id from a spawn payload ("Session ID: <id>").
kimix::string session_id_of(const bt::Tool &tool) {
    const kimix::string text =
        payload_field(tool, "output") + "\n" + payload_field(tool, "message");
    const size_t b = text.find("Session ID: ");
    if (b == kimix::string::npos) {
        return {};
    }
      size_t e = b + 12;
      while (e < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[e])) ||
              text[e] == '-' || text[e] == '_')) {
          ++e;
      }
      return text.substr(b + 12, e - (b + 12));
}

bool bash_available() { return !bt::bash::Bash::detect_bash_path().empty(); }

} // namespace

int main() {
    // -----------------------------------------------------------------------
    // 1. bash: execute surfaces stdout / exit code; interactive returns a
    //    usable task_id.
    // -----------------------------------------------------------------------
    "bug_bash_execute_surfaces_output"_test = [] {
        if (!bash_available()) {
            expect(true); // no shell on this machine: nothing to assert
            return;
        }
        bt::Session session;
        session.native_io = true;
        session.work_dir = tmp_workspace("kimix_bug_bash");
        bt::bash::Bash tool(&session);
        expect(tool.valid());
        bt::ToolParams p = params_of(
            {{"cmd", bt::ValueElement::make_string(
                         kimix::string("echo hello from bash"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_has(tool, "\"status\":\"ok\""));
        // The model-visible output MUST carry the captured stdout (bug: the
        // payload only had the internal "output_block" key).
        const kimix::string output = payload_field(tool, "output");
        expect(!output.empty()) << "bash execute returned no visible output";
        expect(output.find("hello from bash") != kimix::string::npos) << output;
        expect(output.find("status: completed") != kimix::string::npos) << output;
        expect(output.find("exit_code: 0") != kimix::string::npos) << output;
    };

    "bug_bash_interactive_returns_task_id"_test = [] {
        if (!bash_available()) {
            expect(true);
            return;
        }
        bt::Session session;
        session.native_io = true;
        session.work_dir = tmp_workspace("kimix_bug_bash2");
        bt::bash::Bash tool(&session);
        bt::ToolParams start = params_of(
            {{"cmd", bt::ValueElement::make_string(kimix::string("cat"))},
             {"mode", bt::ValueElement::make_string(kimix::string("interactive"))}});
        kimix::builtin_tools::tool_invoke(tool, &start);
        expect(payload_has(tool, "\"status\":\"ok\""));
        // The task id must be visible to the model: in the message or the
        // output (bug: neither carried it -> "Tool output is empty.").
        kimix::string task_id;
        for (const char *key : {"task_id", "message", "output"}) {
            const kimix::string v = payload_field(tool, key);
            const size_t b = v.find("task_");
            if (b != kimix::string::npos) {
                size_t e = b;
                while (e < v.size() &&
                       (std::isalnum(static_cast<unsigned char>(v[e])) ||
                        v[e] == '_')) {
                    ++e;
                }
                task_id = v.substr(b, e - b);
                break;
            }
        }
        expect(!task_id.empty()) << "interactive bash exposed no task_id";
        // The task must actually exist and be manageable through job_output.
        expect(bt::proc::query_task(task_id).exists);
        // Clean up: close the REPL politely, then force-stop the task.
        bt::ToolParams send = params_of(
            {{"cmd", bt::ValueElement::make_string(kimix::string("exit"))},
             {"mode", bt::ValueElement::make_string(kimix::string("send"))},
             {"task_id", bt::ValueElement::make_string(task_id)}});
        kimix::builtin_tools::tool_invoke(tool, &send);
        bt::proc::stop_task(task_id);
    };

    // -----------------------------------------------------------------------
    // 1b. bash: a long command whose \n escapes were double-encoded must run
    //     (the dispatch repairs it in place) instead of refusing with
    //     "Parameters appear to be in the wrong format".
    // -----------------------------------------------------------------------
    "bug_bash_escaped_newline_command_is_repaired_not_refused"_test = [] {
        // 40 long lines joined by LITERAL backslash-n (> 200 chars).
        kimix::string cmd;
        for (int i = 0; i < 40; ++i) {
            cmd +=
                "echo some rather long command line segment that keeps "
                "going\\n";
        }
        const kimix::string arguments = kimix::format(R"({{"cmd":"{}"}})", cmd);
        kimix::agent::AgentSession session(tmp_workspace("kimix_bug_bash3"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        const kimix::string out = soul.execute_tool_call("bash", arguments, err);
        expect(out.find("Parameters appear to be in the wrong format") ==
               kimix::string::npos)
            << out;
        // The repaired command reached the tool (echo ran, or bash is absent
        // on this machine).
        expect(out.find("some rather long command line segment") !=
                   kimix::string::npos ||
               out.find("not available in this environment") !=
                   kimix::string::npos)
            << out;
    };

    // The repair kernel itself: literal \n sequences become real newlines.
    "bug_escaped_newline_repair_kernel"_test = [] {
        const kimix::string arguments =
            R"({"cmd":"line one\\nline two\\nline three"})";
        kimix::string repaired;
        expect(kimix::agent::unescape_escaped_newline_params(
            arguments, "bash", repaired));
        // The re-serialized arguments decode to real newlines (0x0A).
        bt::ToolParams round_trip;
        kimix::string perr;
        expect(round_trip.try_deserialize(
            kimix::span<char const>(repaired.data(), repaired.size()), perr));
        const bt::ValueElement *cmd = round_trip.get("cmd");
        expect(cmd != nullptr && cmd->is_string());
        if (cmd != nullptr) {
            expect(cmd->as_string() == kimix::string("line one\nline two\nline three"))
                << cmd->as_string();
        }
        // A value with real newlines already is left alone.
        kimix::string untouched;
        expect(!kimix::agent::unescape_escaped_newline_params(
            R"({"cmd":"line one\nline two"})", "bash", untouched));
        // A JSON-shaped value keeps the reference extraction flow.
        expect(!kimix::agent::unescape_escaped_newline_params(
            R"({"cmd":"[\"line one\",\"line two\"]"})", "bash", untouched));
        // Non-long-content params are never touched.
        expect(!kimix::agent::unescape_escaped_newline_params(
            R"({"file_path":"a\\nb.txt"})", "bash", untouched));
    };

    // -----------------------------------------------------------------------
    // 2. edit: the registered file-based contract (file_path/old/new, mode
    //    auto default) edits the file on disk.
    // -----------------------------------------------------------------------
    "bug_edit_file_mode_replaces_text"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_edit");
        const kimix::string file = ws + "\\a.txt";
        write_file(file, "alpha line\nunique_alpha line\nomega line\n");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::edit::Edit tool(&session);
        expect(tool.valid());
        // NO mode at all (the schema has no required mode): must default to
        // auto/replace and edit the file (bug: "Missing or invalid required
        // parameter: mode").
        bt::ToolParams p = params_of(
            {{"file_path", bt::ValueElement::make_string(file)},
             {"old_string",
              bt::ValueElement::make_string(kimix::string("unique_alpha line"))},
             {"new_string",
              bt::ValueElement::make_string(kimix::string("EDITED_ALPHA line"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "ok")
            << payload_field(tool, "status") << " / "
            << payload_field(tool, "message");
        expect(read_file(file).find("EDITED_ALPHA line") != kimix::string::npos);
        expect(read_file(file).find("unique_alpha line") == kimix::string::npos);
    };

    "bug_edit_explicit_modes_and_edits_array"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_edit2");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::edit::Edit tool(&session);

        // mode=replace, replace_all.
        const kimix::string f1 = ws + "\\b.txt";
        write_file(f1, "x x x\n");
        bt::ToolParams p1 = params_of(
            {{"file_path", bt::ValueElement::make_string(f1)},
             {"mode", bt::ValueElement::make_string(kimix::string("replace"))},
             {"old_string", bt::ValueElement::make_string(kimix::string("x"))},
             {"new_string", bt::ValueElement::make_string(kimix::string("y"))},
             {"replace_all", bt::ValueElement::make_bool(true)}});
        kimix::builtin_tools::tool_invoke(tool, &p1);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(read_file(f1) == "y y y\n");

        // mode omitted + edits array (multi edit).
        const kimix::string f2 = ws + "\\c.txt";
        write_file(f2, "one\ntwo\nthree\n");
        bt::ToolParams edit_item;
        edit_item.values["old_string"] =
            bt::ValueElement::make_string(kimix::string("two"));
        edit_item.values["new_string"] =
            bt::ValueElement::make_string(kimix::string("TWO"));
        bt::ValueElement::Array edits;
        edits.push_back(bt::ValueElement::make_object(
            kimix::shared_ptr<bt::ToolParams>(new bt::ToolParams(edit_item))));
        bt::ToolParams p2 =
            params_of({{"file_path", bt::ValueElement::make_string(f2)}});
        p2.values["edits"] = bt::ValueElement::make_array(std::move(edits));
        kimix::builtin_tools::tool_invoke(tool, &p2);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(read_file(f2) == "one\nTWO\nthree\n");

        // mode=sloppy (section-sign grammar) applies to the file.
        const kimix::string f3 = ws + "\\d.txt";
        write_file(f3, "MATCH\n");
        bt::ToolParams p3 = params_of(
            {{"file_path", bt::ValueElement::make_string(f3)},
             {"mode", bt::ValueElement::make_string(kimix::string("sloppy"))},
             {"input", bt::ValueElement::make_string(kimix::string(
                           "\xC2\xA7" "d.txt\nMATCH\n\xC2\xBB\nREWRITE\n"))}});
        kimix::builtin_tools::tool_invoke(tool, &p3);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(read_file(f3) == "REWRITE\n");
    };

    "bug_edit_guards_conflict_markers_and_autogenerated"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_edit3");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::edit::Edit tool(&session);

        // Unresolved conflict markers are refused until allow_conflicts=true.
        const kimix::string fc = ws + "\\conflict.txt";
        write_file(fc, "start\n<<<<<<< ours\nmine\n=======\ntheirs\n>>>>>>> "
                       "theirs\nend\n");
        bt::ToolParams pc = params_of(
            {{"file_path", bt::ValueElement::make_string(fc)},
             {"old_string", bt::ValueElement::make_string(kimix::string("start"))},
             {"new_string", bt::ValueElement::make_string(kimix::string("END"))}});
        kimix::builtin_tools::tool_invoke(tool, &pc);
        expect(payload_field(tool, "status") != "ok");
        expect(payload_field(tool, "message").find("Conflict markers detected") !=
               kimix::string::npos)
            << payload_field(tool, "message");
        bt::ToolParams pc2 = params_of(
            {{"file_path", bt::ValueElement::make_string(fc)},
             {"old_string", bt::ValueElement::make_string(kimix::string("start"))},
             {"new_string", bt::ValueElement::make_string(kimix::string("END"))},
             {"allow_conflicts", bt::ValueElement::make_bool(true)}});
        kimix::builtin_tools::tool_invoke(tool, &pc2);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(read_file(fc).find("END") != kimix::string::npos);

        // Auto-generated files are refused unless allow_auto_generated=true.
        const kimix::string fg = ws + "\\zz_generated.go";
        write_file(fg, "// Code generated by gen. DO NOT EDIT.\npackage gen\n");
        bt::ToolParams pg = params_of(
            {{"file_path", bt::ValueElement::make_string(fg)},
             {"old_string",
              bt::ValueElement::make_string(kimix::string("package gen"))},
             {"new_string",
              bt::ValueElement::make_string(kimix::string("package gen2"))}});
        kimix::builtin_tools::tool_invoke(tool, &pg);
        expect(payload_field(tool, "status") != "ok");
        expect(payload_field(tool, "message").find("auto-generated") !=
               kimix::string::npos)
            << payload_field(tool, "message");
        bt::ToolParams pg2 = params_of(
            {{"file_path", bt::ValueElement::make_string(fg)},
             {"old_string",
              bt::ValueElement::make_string(kimix::string("package gen"))},
             {"new_string",
              bt::ValueElement::make_string(kimix::string("package gen2"))},
             {"allow_auto_generated", bt::ValueElement::make_bool(true)}});
        kimix::builtin_tools::tool_invoke(tool, &pg2);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(read_file(fg).find("package gen2") != kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // 4. web_search: a query renders results behind the injectable search
    //    hook; failures are VISIBLE.
    // -----------------------------------------------------------------------
    "bug_web_search_query_renders_results"_test = [] {
        bt::Session session;
        bt::web_search::WebSearch tool(&session);
        bt::web_search::WebSearch::tool_config cfg;
        cfg.search = [](kimix::string_view query, int32_t limit,
                           bool /*include_content*/,
                           kimix::vector<bt::web_search::web_item> &items,
                           kimix::string &error) -> bool {
            (void)query;
            (void)limit;
            error.clear();
            bt::web_search::web_item item;
            item.title = "Python 3.13 release notes";
            item.url = "https://docs.python.org/3.13/";
            item.snippet = "Python 3.13 official release";
            items.push_back(item);
            return true;
        };
        tool.configure(cfg);
        bt::ToolParams p = params_of(
            {{"query",
              bt::ValueElement::make_string(kimix::string("Python 3.13 release notes"))},
             {"limit", bt::ValueElement::make_int(3)}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        const kimix::string output = payload_field(tool, "output");
        expect(output.find("Python 3.13 release notes") != kimix::string::npos)
            << output;
        expect(output.find("https://docs.python.org/3.13/") != kimix::string::npos)
            << output;
    };

    "bug_web_search_failure_is_visible"_test = [] {
        bt::Session session;
        bt::web_search::WebSearch tool(&session);
        bt::web_search::WebSearch::tool_config cfg;
        cfg.search = [](kimix::string_view, int32_t, bool,
                           kimix::vector<bt::web_search::web_item> &,
                           kimix::string &error) -> bool {
            error = "search provider unreachable";
            return false;
        };
        tool.configure(cfg);
        bt::ToolParams p = params_of(
            {{"query",
              bt::ValueElement::make_string(kimix::string("capital of France"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        // The failure must be reported with status=error AND a non-empty
        // message (bug: empty ERROR).
        expect(payload_field(tool, "status") == "error") << payload_field(tool, "status");
        expect(!payload_field(tool, "message").empty()) << "empty error message";
        expect(payload_field(tool, "message").find("search provider unreachable") !=
               kimix::string::npos)
            << payload_field(tool, "message");
    };

    // The native DuckDuckGo parser on a synthetic result page (offline pin for
    // the default keyless backend; the real endpoint sometimes serves a bot
    // challenge, which the provider chain reports as an actionable error).
    "bug_web_search_ddg_parser"_test = [] {
        const kimix::string html =
            "<html><body>"
            "<a rel=\"nofollow\" class=\"result__a\" "
            "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fdocs.python.org%2F3.13%2F"
            "&amp;rut=abc\">Python 3.13 release</a>"
            "<a class=\"result__snippet\" href=\"#\">What's new in <b>3.13</b></a>"
            "<a rel=\"nofollow\" class=\"result__a\" "
            "href=\"https://example.com/second\">Second result</a>"
            "<a class=\"result__snippet\" href=\"#\">Another snippet</a>"
            "</body></html>";
        kimix::vector<bt::web_search::web_item> items;
        bt::web_search::WebSearch::parse_ddg_html(html, items);
        expect(items.size() == 2u) << items.size();
        if (items.size() == 2u) {
            expect(items[0].title == "Python 3.13 release") << items[0].title;
            // The uddg redirect is decoded to the real target.
            expect(items[0].url == "https://docs.python.org/3.13/") << items[0].url;
            expect(items[0].snippet.find("What's new in 3.13") != kimix::string::npos)
                << items[0].snippet;
            expect(items[1].url == "https://example.com/second");
            expect(items[1].title == "Second result");
        }
    };

    // -----------------------------------------------------------------------
    // 5. fetch_url: the registered url contract (fetch -> markdown ->
    //    output_path), with injectable transport and visible errors.
    // -----------------------------------------------------------------------
    "bug_fetch_url_downloads_and_saves"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_fetch");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::fetch_url::FetchUrl tool(&session);
        bt::fetch_url::FetchUrl::tool_config cfg;
        cfg.fetch = [](kimix::string_view /*url*/, kimix::string &html,
                          kimix::string &error) -> bool {
            (void)error;
            html = "<html><head><title>t</title></head><body><h1>Hello</h1>"
                   "<p>world</p></body></html>";
            return true;
        };
        tool.configure(cfg);
        const kimix::string out_path = ws + "\\fetched.md";
        bt::ToolParams p = params_of(
            {{"url",
              bt::ValueElement::make_string(kimix::string("https://example.com"))},
             {"output_path", bt::ValueElement::make_string(out_path)}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        const kimix::string output = payload_field(tool, "output");
        expect(!output.empty()) << "fetch_url returned no visible output";
        // The markdown landed in output_path too.
        std::error_code ec;
        expect(kimix::filesystem::exists(kimix::filesystem::path(out_path), ec));
        expect(read_file(out_path).find("Hello") != kimix::string::npos);
    };

    "bug_fetch_url_errors_are_visible"_test = [] {
        bt::Session session;
        session.native_io = true; // the url contract needs the native transport
        bt::fetch_url::FetchUrl tool(&session);
        bt::fetch_url::FetchUrl::tool_config cfg;
        cfg.fetch = [](kimix::string_view, kimix::string &,
                          kimix::string &error) -> bool {
            error = "connection refused";
            return false;
        };
        tool.configure(cfg);
        bt::ToolParams p = params_of(
            {{"url",
              bt::ValueElement::make_string(kimix::string("https://example.com"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "error") << payload_field(tool, "status");
        expect(!payload_field(tool, "message").empty()) << "empty error message";
    };

    // -----------------------------------------------------------------------
    // 5b. Live-network check (opt-in via KIMIX_TEST_LIVE_NET=1): the REAL
    // http_fetch transport + the registered fetch_url contract end to end.
    // -----------------------------------------------------------------------
    "bug_fetch_url_live_network_when_enabled"_test = [] {
        const char *enabled = std::getenv("KIMIX_TEST_LIVE_NET");
        if (enabled == nullptr || kimix::string(enabled) != "1") {
            expect(true); // opt-in only
            return;
        }
        bt::Session session;
        session.native_io = true;
        session.work_dir = tmp_workspace("kimix_bug_fetch_live");
        bt::fetch_url::FetchUrl tool(&session);
        bt::ToolParams p = params_of(
            {{"url", bt::ValueElement::make_string(
                         kimix::string("https://example.com"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        const kimix::string status = payload_field(tool, "status");
        if (status != "ok") {
            // Offline machines fail cleanly and VISIBLY - also acceptable.
            expect(!payload_field(tool, "message").empty())
                << payload_field(tool, "message");
            return;
        }
        expect(payload_field(tool, "output").find("Example Domain") !=
               kimix::string::npos)
            << payload_field(tool, "output");
    };

    // -----------------------------------------------------------------------
    // 6. read_image: file_path alone reads the image from disk.
    // -----------------------------------------------------------------------
    "bug_read_image_reads_file_from_disk"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_img");
        std::string png_bytes;
        expect(base64_decode(k_red_png_b64, png_bytes));
        const kimix::string file = ws + "\\red.png";
        write_file(file, kimix::string(png_bytes.data(), png_bytes.size()));

        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::read_image::ReadImage tool(&session);
        expect(tool.valid());
        bt::ToolParams p =
            params_of({{"file_path", bt::ValueElement::make_string(file)}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        expect(payload_field(tool, "kind") == "image") << payload_field(tool, "kind");
        expect(payload_field(tool, "mime_type") == "image/png");
        // Metadata sniffed from the real file header.
        const kimix::string json = payload_raw(tool);
        expect(json.find("\"width\":1") != kimix::string::npos) << json;
        expect(json.find("\"height\":1") != kimix::string::npos) << json;
        // The media payload is present and model-visible.
        expect(payload_field(tool, "data_url").find("data:image/png;base64,") == 0);
        const kimix::string vis = payload_field(tool, "output");
        expect(!vis.empty()) << "read_image returned no visible output";
        expect(!payload_field(tool, "message").empty());
    };

    // -----------------------------------------------------------------------
    // 7. grep: files_with_matches lists the files.
    // -----------------------------------------------------------------------
  "bug_grep_files_mode_lists_files"_test = [] {
      const kimix::string ws = tmp_workspace("kimix_bug_grep");
      // Portable separators: a backslash is a literal filename character on
      // Linux, which would scatter the corpus files into the temp root
      // instead of `ws` (grep then finds nothing there). Forward slashes
      // work for both the fopen write and the tool's directory search.
      write_file(ws + "/a.txt", "ALPHA line\n");
      write_file(ws + "/notes.md", "x\nSECRET note\n");
      write_file(ws + "/secret.log", "SECRET log\n");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::grep::Grep tool(&session);
        expect(tool.valid());
        bt::ToolParams p = params_of(
            {{"pattern",
              bt::ValueElement::make_string(kimix::string("ALPHA|SECRET"))},
             {"path", bt::ValueElement::make_string(ws)},
             {"output_mode",
              bt::ValueElement::make_string(kimix::string("files_with_matches"))}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        expect(payload_field(tool, "status") == "ok") << payload_field(tool, "message");
        const kimix::string output = payload_field(tool, "output");
        expect(output.find("a.txt") != kimix::string::npos) << output;
        expect(output.find("notes.md") != kimix::string::npos) << output;
        expect(output.find("secret.log") != kimix::string::npos) << output;
    };

    // -----------------------------------------------------------------------
    // 8. read: the char window slices the rendered output as documented.
    // -----------------------------------------------------------------------
    "bug_read_char_window_is_a_char_window"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_read");
        const kimix::string file = ws + "\\w.txt";
        write_file(file, "alpha\nbeta\ngamma\n");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::read::Read tool(&session);
        bt::ToolParams p = params_of(
            {{"file_path", bt::ValueElement::make_string(file)},
             {"char_offset", bt::ValueElement::make_int(6)},
             {"max_char", bt::ValueElement::make_int(11)}});
      kimix::builtin_tools::tool_invoke(tool, &p);
      expect(payload_field(tool, "status") == "ok");
      // The window is a CODE-POINT slice over the rendered (line-numbered)
      // output: "     1\talpha\n     2\tbeta\n     3\tgamma\n"[6..17) =
      // '\t' + "alpha" + '\n' + four of the five spaces before line 2.
      const kimix::string rendered = "     1\talpha\n     2\tbeta\n     3\tgamma\n";
      expect(payload_field(tool, "output") == kimix::string(rendered.substr(6, 11)))
          << "[" << payload_field(tool, "output") << "]";
      expect(payload_field(tool, "message").find("chars 6..17") !=
             kimix::string::npos)
          << payload_field(tool, "message");
    };

    // -----------------------------------------------------------------------
    // 9. write: a NEW file says "created", an existing one "overwritten".
    // -----------------------------------------------------------------------
    "bug_write_new_file_says_created"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_write");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::write::Write tool(&session);

      const kimix::string fresh = ws + "\\new.txt";
      // The temp workspace persists across runs: start from a clean slate so
      // this run really creates the file.
      std::error_code rm_ec;
      kimix::filesystem::remove(kimix::filesystem::path(fresh), rm_ec);
      bt::ToolParams p1 = params_of(
            {{"file_path", bt::ValueElement::make_string(fresh)},
             {"content", bt::ValueElement::make_string(kimix::string("body\n"))}});
        kimix::builtin_tools::tool_invoke(tool, &p1);
        expect(payload_field(tool, "status") == "ok");
        expect(payload_field(tool, "message").find("File successfully created") !=
               kimix::string::npos)
            << payload_field(tool, "message");

        // Second write to the SAME path: overwritten.
        bt::ToolParams p2 = params_of(
            {{"file_path", bt::ValueElement::make_string(fresh)},
             {"content", bt::ValueElement::make_string(kimix::string("body2\n"))}});
        kimix::builtin_tools::tool_invoke(tool, &p2);
        expect(payload_field(tool, "status") == "ok");
        expect(payload_field(tool, "message").find("File successfully overwritten") !=
               kimix::string::npos)
            << payload_field(tool, "message");
    };

    // -----------------------------------------------------------------------
    // 10. subagent: run_in_background=true returns immediately.
    // -----------------------------------------------------------------------
    "bug_subagent_background_returns_immediately"_test = [] {
        bt::Session session;
        session.session_id = "parent";
        bt::agents::agent_registry &registry =
            bt::agents::session_registry(&session);
        registry.runner =
            [](const bt::agents::subagent_request &)
            -> bt::agents::subagent_run_result {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bt::agents::subagent_run_result r;
            r.ok = true;
            r.output = "child done";
            return r;
        };
        bt::agents::Subagent tool(&session);
        expect(tool.valid());
        bt::ToolParams p = params_of(
            {{"prompt", bt::ValueElement::make_string(kimix::string("sleep task"))},
             {"run_in_background", bt::ValueElement::make_bool(true)}});
        const auto t0 = std::chrono::steady_clock::now();
        kimix::builtin_tools::tool_invoke(tool, &p);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        expect(elapsed < 200)
            << "background subagent blocked for " << elapsed << "ms";
        expect(payload_field(tool, "status") == "ok");
        // The durable session id is visible to the model.
        const kimix::string visible =
            payload_field(tool, "output") + payload_field(tool, "message");
        expect(visible.find("Session ID:") != kimix::string::npos) << visible;
    };

    // -----------------------------------------------------------------------
    // 11. interrupt_agent: an already-finished session closes gracefully.
    // -----------------------------------------------------------------------
    "bug_interrupt_finished_agent_is_graceful"_test = [] {
        bt::Session session;
        session.session_id = "parent";
        bt::agents::agent_registry &registry =
            bt::agents::session_registry(&session);
        registry.runner = [](const bt::agents::subagent_request &)
            -> bt::agents::subagent_run_result {
            bt::agents::subagent_run_result r;
            r.ok = true;
            r.output = "finished already";
            return r;
        };
        // Start + let it finish (a background run that settled while the
        // parent was busy).
        bt::agents::Subagent spawner(&session);
        bt::ToolParams spawn = params_of(
            {{"prompt", bt::ValueElement::make_string(kimix::string("quick"))},
             {"run_in_background", bt::ValueElement::make_bool(true)}});
        kimix::builtin_tools::tool_invoke(spawner, &spawn);
        const kimix::string id = session_id_of(spawner);
        expect(!id.empty()) << payload_raw(spawner);
        for (int i = 0; i < 50 && !registry.run_finished(id); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        expect(registry.run_finished(id));
        // The parent collected the settled outcome (turn-start drain); the
        // session bookkeeping is gone (close_session default true) but the
        // result was parked.
        bt::agents::subagent_run_result settled;
        expect(registry.join_run(id, settled));
        registry.clear_run(id);

        bt::agents::InterruptAgent tool(&session);
        bt::ToolParams p = params_of(
            {{"agent_id", bt::ValueElement::make_string(id)}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        // Documented: "interrupting an agent that already finished still
        // closes its session (no error)".
        expect(payload_field(tool, "status") == "ok")
            << payload_field(tool, "status") << " / "
            << payload_field(tool, "message");
    };

    // -----------------------------------------------------------------------
    // 10b. the settled background outcome reaches the parent's next turn.
    // -----------------------------------------------------------------------
    "bug_background_settle_notice_reaches_parent"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace("kimix_bug_notice"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        bt::agents::agent_registry &registry =
            bt::agents::session_registry(&session.tool_session());
        registry.runner = [](const bt::agents::subagent_request &)
            -> bt::agents::subagent_run_result {
            bt::agents::subagent_run_result r;
            r.ok = true;
            r.output = "research result: 42";
            return r;
        };
        kimix::string err;
        const kimix::string spawned = soul.execute_tool_call(
            "subagent", R"({"prompt":"do research","run_in_background":true})",
            err);
        expect(err.empty()) << err;
        expect(spawned.find("background") != kimix::string::npos) << spawned;
        // Wait until the child settles, then collect the notice.
        bool noticed = false;
        for (int i = 0; i < 100 && !noticed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            noticed = !soul.drain_finished_subagent_notices().empty();
        }
        expect(noticed) << "no settle notice was produced";
        // The NEXT turn delivers the notice before the user input.
        const kimix::agent::TurnResult tr = soul.turn("continue", {});
        expect(tr.ok) << tr.error;
        bool saw_notice = false;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "user" &&
                m.content.find("research result: 42") != kimix::string::npos) {
                saw_notice = true;
            }
        }
        expect(saw_notice)
            << "the settled sub-agent outcome never reached the parent";
    };

    // -----------------------------------------------------------------------
    // 12. retrieve: the native BM25 index ranks the relevant turn first with
    //     a non-zero relevance.
    // -----------------------------------------------------------------------
    "bug_retrieve_ranks_relevant_turn_first"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace("kimix_bug_retrieve"));
        for (int i = 0; i < 8; ++i) {
            kimix::llm::Message filler;
            filler.role = "user";
            filler.content = kimix::format(
                "filler turn {} about unrelated filesystems and buffers", i);
            session.append_history(filler);
        }
        kimix::llm::Message relevant;
        relevant.role = "user";
        relevant.content =
            "how do I configure the bash tool timeout for long builds";
        session.append_history(relevant);

        const auto found = session.history_search("bash tool timeout long builds", 3);
        expect(found.size() > 0);
        if (!found.empty()) {
            expect(found[0].text == relevant.content) << found[0].text;
            expect(found[0].score > 0.0) << "relevance stayed 0.00";
        }
    };

    // -----------------------------------------------------------------------
    // 14. plan tools: the CLI session wiring provides a default plan path.
    // -----------------------------------------------------------------------
    "bug_plan_session_gets_default_plan_path"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_plan");
        // The CLI helper the session wiring uses (bug: plan_enabled was set
        // without a plan_writing_path, so every call answered
        // "no plan_writing_path set").
        const kimix::string plan_path = kimix::cli::cli_default_plan_path(ws);
        expect(!plan_path.empty());
        expect(plan_path.find(".kimix_cache") != kimix::string::npos) << plan_path;
        expect(plan_path.find("plan_") != kimix::string::npos) << plan_path;

        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        session.plan_enabled = true;
        session.plan_path = plan_path; // what the fixed wiring now sets
        bt::plan::WritePlan write_tool(&session);
        bt::ToolParams wp = params_of(
            {{"content",
              bt::ValueElement::make_string(kimix::string("# The Plan\n- step 1\n"))}});
        kimix::builtin_tools::tool_invoke(write_tool, &wp);
        expect(payload_field(write_tool, "status") == "ok")
            << payload_field(write_tool, "message");
        expect(read_file(plan_path).find("step 1") != kimix::string::npos);

        bt::plan::ReadPlan read_tool(&session);
        bt::ToolParams rp = params_of({});
        kimix::builtin_tools::tool_invoke(read_tool, &rp);
        expect(payload_field(read_tool, "status") == "ok");
        expect(payload_field(read_tool, "output").find("step 1") !=
               kimix::string::npos)
            << payload_field(read_tool, "output");
    };

    // -----------------------------------------------------------------------
    // 15. read_image (bug_tool.md item 1): every failure mode must carry a
    // non-empty status + message (never an empty ERROR / empty output), and
    // the failure payload must expose the path and size metadata.
    // -----------------------------------------------------------------------
    "bug_read_image_missing_file_reports_reasonable_error"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_img_err");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::read_image::ReadImage tool(&session);
        bt::ToolParams p = params_of({{"file_path",
                                      bt::ValueElement::make_string(
                                          ws + "\\nope.png")}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        const kimix::string json = payload_raw(tool);
        const kimix::string status = payload_field(tool, "status");
        expect(status != "ok") << json;
        expect(status != "error")
            << "a missing file must not use the generic runtime-failure "
               "status (it earns the misleading 'unexpected error' suffix)"
            << json;
        expect(!payload_field(tool, "message").empty())
            << "missing-file error carried no reason" << json;
        expect(payload_field(tool, "message").find("nope.png") !=
               kimix::string::npos)
            << payload_field(tool, "message");
        // Metadata survives into the failure payload (bug: none was returned).
        expect(json.find("\"path\"") != kimix::string::npos) << json;
        expect(json.find("\"size\"") != kimix::string::npos) << json;
    };

    "bug_read_image_non_image_file_reports_unsupported"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_bug_img_err2");
        const kimix::string file = ws + "\\notes.md";
        write_file(file, "# not an image\n");
        bt::Session session;
        session.native_io = true;
        session.work_dir = ws;
        bt::read_image::ReadImage tool(&session);
        bt::ToolParams p =
            params_of({{"file_path", bt::ValueElement::make_string(file)}});
        kimix::builtin_tools::tool_invoke(tool, &p);
        const kimix::string json = payload_raw(tool);
        const kimix::string status = payload_field(tool, "status");
        expect(status != "ok") << json;
        expect(status != "error")
            << "a wrong-type file is an expected validation failure" << json;
        // Bug: this case produced a completely empty tool result (no message,
        // no output): the model saw "Tool output is empty.".
        const bool visible = !payload_field(tool, "message").empty() ||
                             !payload_field(tool, "output").empty();
        expect(visible) << "non-image file produced no visible diagnostic"
                        << json;
        expect(json.find("\"size\"") != kimix::string::npos) << json;
    };

    // -----------------------------------------------------------------------
    // 16. job_output action='kill' (bug_tool.md item 2): killing a live
    // process must report SUCCESS with a real message (never a bare
    // "(7.05s)" timing value as the error text), and the kill must be
    // recorded (a later read from the history must not be an error, and
    // list must not keep the killed id as running).
    // -----------------------------------------------------------------------
    "bug_job_output_kill_reports_success_not_raw_elapsed"_test = [] {
        if (!bash_available()) {
            expect(true);
            return;
        }
        using namespace kimix::builtin_tools::job_output;
        // A long-running task: a live `sleep` via start_task.
        const kimix::string bash_path =
            bt::bash::Bash::detect_bash_path();
        bt::proc::run_options opts;
        opts.argv.push_back(bash_path);
        opts.argv.push_back("--noprofile");
        opts.argv.push_back("--norc");
        opts.argv.push_back("-c");
        opts.argv.push_back("sleep 30");
        opts.timeout_ms = 0;
        bt::proc::task_handle handle;
        const bt::tool_error start = bt::proc::start_task(opts, handle);
        expect(!start.failed()) << start.message;

        bt::Session session;
        session.native_io = true;
        session.work_dir = tmp_workspace("kimix_bug_jobkill");
        clear_finished_tasks();
        JobOutput tool(&session);
        bt::ToolParams kill_p = params_of(
            {{"job_id", bt::ValueElement::make_string(handle.task_id)},
             {"action", bt::ValueElement::make_string(kimix::string("kill"))}});
        kimix::builtin_tools::tool_invoke(tool, &kill_p);
        const kimix::string json = payload_raw(tool);
        // The kill itself is a SUCCESS: status ok, never the generic
        // runtime-failure status (which the soul renders as
        // "ERROR: <message>" with the misleading unexpected-error suffix,
        // bug: message was the bare timing "(7.05s)").
        expect(payload_field(tool, "status") == "ok") << json;
        expect(json.find("\"runtime\":true") == kimix::string::npos) << json;
        expect(json.find("killed") != kimix::string::npos) << json;
        // The kill result must be self-describing somewhere in the payload:
        // the reference's message is the timing-only string, so the brief
        // carries the wording (and the message must never be the bare
        // timing on its own).
        const kimix::string msg = payload_field(tool, "message");
        const bool descriptive =
            msg.find("killed") != kimix::string::npos ||
            payload_field(tool, "brief").find("killed") != kimix::string::npos;
        expect(descriptive) << msg;

        // The kill is recorded: the id left the running registry...
        expect(!bt::proc::query_task(handle.task_id).exists);
        // ...and a later read serves the recorded result as a SUCCESS
        // (bug: it re-served "ERROR: (7.05s)" forever).
        bt::ToolParams get_p = params_of(
            {{"job_id", bt::ValueElement::make_string(handle.task_id)}});
        kimix::builtin_tools::tool_invoke(tool, &get_p);
        const kimix::string json2 = payload_raw(tool);
        expect(payload_field(tool, "status") == "ok") << json2;
        // The killed state is visible in the history view (bug_tool.md item
        // 2: "there is no way to see a task was killed").
        expect(json2.find("\"status_text\":\"killed\"") != kimix::string::npos)
            << json2;
        clear_finished_tasks();
    };

    // -----------------------------------------------------------------------
    // 17. bash interactive (bug_tool.md item 3): startup output is captured,
    // exported env persists across sends, and a send returns promptly (no
    // fixed ~30 s wait).
    // -----------------------------------------------------------------------
    "bug_bash_interactive_repl_semantics"_test = [] {
        if (!bash_available()) {
            expect(true);
            return;
        }
        bt::Session session;
        session.native_io = true;
        session.work_dir = tmp_workspace("kimix_bug_bashrepl");
        bt::bash::Bash tool(&session);

        // (a) Start with a command: its output must be VISIBLE in the start
        // result (bug: only "interactive bash started (pid ...)" was).
        bt::ToolParams start = params_of(
            {{"cmd", bt::ValueElement::make_string(
                         kimix::string("echo REPLSTART-42"))},
             {"mode",
              bt::ValueElement::make_string(kimix::string("interactive"))}});
        const auto t0 = std::chrono::steady_clock::now();
        kimix::builtin_tools::tool_invoke(tool, &start);
        const auto start_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        expect(payload_field(tool, "status") == "ok");
        const kimix::string start_out = payload_field(tool, "output");
        expect(start_out.find("REPLSTART-42") != kimix::string::npos)
            << "startup command output was lost: " << start_out;
        // Extract the task id: prefer the dedicated payload field.
        kimix::string task_id = payload_field(tool, "task_id");
        if (task_id.empty()) {
            const kimix::string text = payload_field(tool, "output") +
                                       "\n" +
                                       payload_field(tool, "message");
            const size_t b = text.find("task_");
            expect(b != kimix::string::npos) << text;
            size_t e = b;
            while (e < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[e])) ||
                    text[e] == '_')) {
                ++e;
            }
            task_id = text.substr(b, e - b);
        }
        expect(!task_id.empty());
        // The start must also be prompt (bug: fixed 30 s waits).
        expect(start_ms < 15000)
            << "interactive start blocked for " << start_ms << "ms";

        // (b) Exported environment persists across sends (bug: every send ran
        // in a fresh env so `export X` never stuck).
        bt::ToolParams exp_p = params_of(
            {{"cmd", bt::ValueElement::make_string(
                         kimix::string("export MYREPLVAR=hello42"))},
             {"mode", bt::ValueElement::make_string(kimix::string("send"))},
             {"task_id", bt::ValueElement::make_string(task_id)}});
        kimix::builtin_tools::tool_invoke(tool, &exp_p);
        bt::ToolParams echo_p = params_of(
            {{"cmd", bt::ValueElement::make_string(
                         kimix::string("echo GOT[$MYREPLVAR]"))},
             {"mode", bt::ValueElement::make_string(kimix::string("send"))},
             {"task_id", bt::ValueElement::make_string(task_id)}});
        const auto t1 = std::chrono::steady_clock::now();
        kimix::builtin_tools::tool_invoke(tool, &echo_p);
        const auto echo_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t1)
                .count();
        const kimix::string echo_out = payload_field(tool, "output");
        expect(echo_out.find("GOT[hello42]") != kimix::string::npos)
            << "exported env did not persist across sends: " << echo_out;
        // (c) The send returned promptly, not after a fixed ~30 s wait.
        expect(echo_ms < 15000)
            << "send blocked for " << echo_ms << "ms (fixed-wait regression)";

        // Close the session cleanly.
        bt::ToolParams exit_p = params_of(
            {{"cmd", bt::ValueElement::make_string(kimix::string("exit"))},
             {"mode", bt::ValueElement::make_string(kimix::string("send"))},
             {"task_id", bt::ValueElement::make_string(task_id)}});
        kimix::builtin_tools::tool_invoke(tool, &exit_p);
        bt::proc::stop_task(task_id);
    };

    // -----------------------------------------------------------------------
    // 18. fetch_url (bug_tool.md item 4): expected safety/validation
    // rejections must NOT use the generic runtime-failure status (the soul
    // adds "This is an unexpected error and the tool is probably not
    // working." for it). Real transport failures keep that status.
    // -----------------------------------------------------------------------
    "bug_fetch_url_expected_rejections_are_not_runtime_errors"_test = [] {
        bt::Session session;
        session.native_io = true;
        bt::fetch_url::FetchUrl tool(&session);
        const kimix::string soul_runtime_suffix =
            "unexpected error and the tool is probably not working";

        // ftp:// scheme: rejected with the non-runtime status.
        bt::ToolParams ftp = params_of({{"url", bt::ValueElement::make_string(
                                                   kimix::string("ftp://example.com/file"))}});
        kimix::builtin_tools::tool_invoke(tool, &ftp);
        const kimix::string ftp_status = payload_field(tool, "status");
        expect(ftp_status != "ok") << payload_raw(tool);
        expect(ftp_status != "error")
            << "a scheme rejection is an expected validation failure, not a "
               "runtime error: " << payload_raw(tool);
        expect(payload_field(tool, "message").find("http") !=
               kimix::string::npos)
            << payload_field(tool, "message");

        // SSRF loopback: rejected with the non-runtime status.
        bt::ToolParams loop = params_of({{"url", bt::ValueElement::make_string(
                                                    kimix::string("http://127.0.0.1/x"))}});
        kimix::builtin_tools::tool_invoke(tool, &loop);
        const kimix::string loop_status = payload_field(tool, "status");
        expect(loop_status != "ok") << payload_raw(tool);
        expect(loop_status != "error")
            << "an SSRF refusal is expected safety behavior, not a runtime "
               "error: " << payload_raw(tool);

        // DNS failure (guaranteed-unresolvable .invalid TLD): expected
        // rejection, not the runtime error the model reads as a broken tool.
        bt::ToolParams dns = params_of({{"url", bt::ValueElement::make_string(
                                                   kimix::string("https://kimix-nonexistent-host.invalid/"))}});
        kimix::builtin_tools::tool_invoke(tool, &dns);
        const kimix::string dns_status = payload_field(tool, "status");
        expect(dns_status != "ok") << payload_raw(tool);
        expect(dns_status != "error")
            << "an unresolvable host is an expected refusal: "
            << payload_raw(tool);

        // A transport failure (injected fetch hook) keeps the runtime status:
        // there the tool really could not do its job on a valid URL.
        bt::fetch_url::FetchUrl::tool_config cfg;
        cfg.fetch = [](kimix::string_view, kimix::string &, kimix::string &error) {
            error = "connection reset by peer";
            return false;
        };
        tool.configure(cfg);
        bt::ToolParams transport = params_of({{"url", bt::ValueElement::make_string(
                                                        kimix::string("https://example.com"))}});
        kimix::builtin_tools::tool_invoke(tool, &transport);
        expect(payload_field(tool, "status") == "error") << payload_raw(tool);
        expect(payload_field(tool, "message").find("connection reset") !=
               kimix::string::npos)
            << payload_field(tool, "message");
    };

    return 0;
}
