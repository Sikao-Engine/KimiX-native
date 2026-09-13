// test_agent.cpp - Unit tests for the tool registry, regex_lite, the native
// (reproc) process runner, the native Read/Write/Grep/Bash execution paths
// and the KimiSoul agent turn loop (with a scripted fake chat backend).
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: the soul tests
// drive IChatBackend with canned ChatResults.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/glob_tool.h"
#include "builtin_tools/grep_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/regex_lite.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/write_tool.h"

#include <cstdio>

#include <core/clock.h>

namespace {

using namespace boost::ut;

using kimix::builtin_tools::Session;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// Scripted chat backend: pops one canned result per chat() call.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    size_t index = 0;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)tools;
        (void)on_chunk;
        requests.push_back(messages);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace() {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / "kimix_agent_test_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

ToolParams parse_json(const kimix::string &json) {
    ToolParams p;
    kimix::string err;
    const bool ok =
        p.try_deserialize(kimix::span<char const>(json.data(), json.size()), err);
    if (!ok) {
        throw std::runtime_error(err.c_str());
    }
    return p;
}

kimix::string result_string(const kimix::vector<char> &buf) {
    return kimix::string(buf.data(), buf.size());
}

} // namespace

int main() {
    using namespace boost::ut;

    // =======================================================================
    // Tool registry
    // =======================================================================
    "registry_has_all_builtin_tools"_test = [] {
        auto &reg = kimix::builtin_tools::ToolRegistry::instance();
        expect(reg.size() >= 12u);
        for (const char *name :
             {"Bash", "Read", "Write", "Grep", "Glob", "Compact", "Edit",
              "Pwsh", "FetchUrl", "WebSearch", "ReadImage", "Retrieve",
              "Python"}) {
            const auto *meta = reg.find(kimix::string_view(name));
            expect(meta != nullptr) << name;
            if (meta != nullptr) {
                expect(eq(meta->name, kimix::string(name)));
                expect(!meta->description.empty());
                expect(meta->parameters_json.find("properties") !=
                       kimix::string::npos)
                    << name;
            }
        }
    };

    "registry_case_insensitive_create"_test = [] {
        auto &reg = kimix::builtin_tools::ToolRegistry::instance();
        Session session;
        auto tool = reg.create("bash", &session);
        expect(tool != nullptr);
        auto none = reg.create("no_such_tool", &session);
        expect(none == nullptr);
    };

    // =======================================================================
    // regex_lite
    // =======================================================================
    "regex_lite_basic"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("foo", false, err));
        size_t b = 0, e = 0;
        expect(re.search("xfooy", b, e));
        expect(eq(b, size_t(1)));
        expect(eq(e, size_t(4)));
        expect(!re.search("bar", b, e));

        expect(re.compile("[a-z]+\\d{2,3}", false, err));
        expect(re.search("ab12", b, e));
        expect(re.search("xy999", b, e));
        expect(!re.search("ab1", b, e));

        expect(re.compile("^foo|bar$", false, err));
        expect(re.search("foobar", b, e));
        expect(re.search("xxbar", b, e));
        expect(!re.search("xxbarx", b, e));
    };

    "regex_lite_groups_case_insensitive"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("(\\w+)@(\\w+)\\.(com|org)", true, err));
        size_t b = 0, e = 0;
        expect(re.search("mail Bob@Example.COM ok", b, e));
        size_t gb = 0, ge = 0;
        expect(re.last_group(1, gb, ge));
        const kimix::string subj = "mail Bob@Example.COM ok";
        expect(eq(kimix::string("Bob"),
                  kimix::string(subj.substr(gb, ge - gb))));
        expect(eq(re.group_count(), size_t(3)));

        // Bad patterns must be rejected, not crash.
        Regex bad;
        expect(!bad.compile("(foo", false, err));
        expect(!err.empty());
        expect(!bad.compile("a{2,1}", false, err));
        expect(!bad.compile("*x", false, err));
        expect(!bad.compile("a\\1", false, err));
    };

    "regex_lite_utf8"_test = [] {
        using kimix::builtin_tools::regex_lite::Regex;
        kimix::string err;
        Regex re;
        expect(re.compile("caf.", false, err));
        size_t b = 0, e = 0;
        const kimix::string s = "x caf\xC3\xA9 y";
        expect(re.search(s, b, e));
        expect(eq(kimix::string(s.substr(b, e - b)),
                  kimix::string("caf\xC3\xA9")));
    };

    // =======================================================================
    // process_runner (reproc spawn/poll/drain/kill)
    // =======================================================================
    "process_runner_echo"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            // No bash on this machine: skip (the e2e covers Windows+Git Bash).
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-c",
                     "echo hello_world; echo err_msg >&2; exit 3"};
        opts.timeout_ms = 20000;
        const proc::run_result rr = proc::run_process(opts);
        expect(rr.spawn_error.empty()) << rr.spawn_error;
        expect(rr.exit_code.has_value());
        expect(eq(*rr.exit_code, int64_t(3)));
        expect(rr.output.find("hello_world") != kimix::string::npos);
        expect(rr.output.find("err_msg") != kimix::string::npos);
    };

    "process_runner_timeout_kill"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-c", "sleep 30"};
        opts.timeout_ms = 1000;
        const int64_t t0 = static_cast<int64_t>(kimix::Clock::now_ms());
        const proc::run_result rr = proc::run_process(opts);
        const int64_t elapsed = kimix::Clock::now_ms() - t0;
        expect(rr.killed);
        expect(elapsed < 10000) << elapsed;
    };

    "process_runner_interactive_task"_test = [] {
        const kimix::string bash =
            kimix::builtin_tools::bash::Bash::detect_bash_path();
        if (bash.empty()) {
            return;
        }
        namespace proc = kimix::builtin_tools::proc;
        proc::run_options opts;
        opts.argv = {bash, "--noprofile", "--norc", "-i"};
        proc::task_handle h;
        const auto terr = proc::start_task(opts, h);
        expect(!terr.failed()) << terr.message;
        expect(!h.task_id.empty());
        expect(!proc::send_task(h.task_id, "echo interactive_ok", true).failed());
        const auto tw = proc::wait_task(h.task_id, "interactive_ok", 15000);
        expect(tw.matched);
        kimix::string out;
        expect(!proc::read_task(h.task_id, out).failed());
        expect(out.find("interactive_ok") != kimix::string::npos);
        expect(!proc::stop_task(h.task_id).failed());
        expect(!proc::query_task(h.task_id).exists);
        proc::stop_all_tasks();
    };

    // =======================================================================
    // Native Read / Write / Grep / Bash through the Tool interface
    // =======================================================================
    "native_write_then_read"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;

        kimix::builtin_tools::write::Write w(&session);
        ToolParams wp = parse_json(R"JSON({"file_path":"out/note.txt","content":"alpha\nbeta\ngamma\n","mode":"overwrite","mkdir":true})JSON");
        w(&wp);
        const auto &wr = w.last_result().values;
        expect(eq(wr.at("status").as_string(), kimix::string("ok")))
            << wr.at("message").as_string();
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "out" / "note.txt", ec));

        kimix::builtin_tools::read::Read r(&session);
        ToolParams rp = parse_json(R"JSON({"file_path":"out/note.txt"})JSON");
        r(&rp);
        ToolParams rr;
        const kimix::string rs = result_string(r.serialized_result());
        rr.deserialize(kimix::span<char const>(rs.data(), rs.size()));
        expect(eq(rr.get("status")->as_string(), kimix::string("ok")));
        expect(rr.get("output")->as_string().find("beta") != kimix::string::npos);
    };

    "native_grep_search"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;
        kimix::builtin_tools::write::Write w(&session);
        ToolParams w1 = parse_json(R"JSON({"file_path":"a.cpp","content":"int needle_here = 1;\n","mkdir":true})JSON");
        w(&w1);
        ToolParams w2 = parse_json(R"JSON({"file_path":"b.txt","content":"nothing useful\nneedle too\n","mkdir":true})JSON");
        w(&w2);

        kimix::builtin_tools::grep::Grep g(&session);
        ToolParams gp;
        gp.values["pattern"] = ValueElement::make_string(kimix::string("needle"));
        gp.values["paths"] = ValueElement::make_string(kimix::string("."));
        gp.values["output_mode"] =
            ValueElement::make_string(kimix::string("files_with_matches"));
        g(&gp);
        ToolParams gr;
        const kimix::string gs = result_string(g.serialized_result());
        gr.deserialize(kimix::span<char const>(gs.data(), gs.size()));
        expect(eq(gr.get("status")->as_string(), kimix::string("ok")));
        expect(eq(gr.get("file_count")->as_int(), int64_t(2)));

        // Content mode with a regex.
        ToolParams gp2;
        gp2.values["pattern"] =
            ValueElement::make_string(kimix::string("needle_\\w+"));
        gp2.values["paths"] = ValueElement::make_string(kimix::string("."));
        gp2.values["output_mode"] =
            ValueElement::make_string(kimix::string("content"));
        g(&gp2);
        ToolParams gr2;
        const kimix::string gs2 = result_string(g.serialized_result());
        gr2.deserialize(kimix::span<char const>(gs2.data(), gs2.size()));
        expect(eq(gr2.get("status")->as_string(), kimix::string("ok")));
        expect(gr2.get("output")->as_string().find("needle_here") !=
               kimix::string::npos);
    };

    "native_glob_and_bash"_test = [] {
        const kimix::string ws = tmp_workspace();
        Session session;
        session.work_dir = ws;
        session.native_io = true;
        kimix::builtin_tools::write::Write w(&session);
        ToolParams w1 = parse_json(R"JSON({"file_path":"src/main.cpp","content":"int main(){}\n","mkdir":true})JSON");
        w(&w1);

        auto reg_tool =
            kimix::builtin_tools::ToolRegistry::instance().create("Glob", &session);
        expect(reg_tool != nullptr);
        ToolParams gp = parse_json(R"JSON({"pattern":"**/*.cpp","path":"src"})JSON");
        (*reg_tool)(&gp);
        kimix::vector<char> out;
        reg_tool->result_json(out);
        const kimix::string os(out.data(), out.size());
        expect(os.find("main.cpp") != kimix::string::npos) << os;

        // Bash: real command through reproc (skip without bash).
        if (kimix::builtin_tools::bash::Bash::detect_bash_path().empty()) {
            return;
        }
        auto bash_tool =
            kimix::builtin_tools::ToolRegistry::instance().create("Bash", &session);
        expect(bash_tool != nullptr);
        ToolParams bp2;
        bp2.values["cmd"] = ValueElement::make_string(
            kimix::string("cd '") + ws + "' && ls src && echo E2E_OK");
        bp2.values["timeout"] = ValueElement::make_int(30);
        (*bash_tool)(&bp2);
        kimix::vector<char> bout;
        bash_tool->result_json(bout);
        const kimix::string bs(bout.data(), bout.size());
        expect(bs.find("E2E_OK") != kimix::string::npos) << bs;
        expect(bs.find("main.cpp") != kimix::string::npos) << bs;
    };

    // =======================================================================
    // KimiSoul
    // =======================================================================
    "soul_tool_call_with_json_repair"_test = [] {
        const kimix::string ws = tmp_workspace();
        kimix::agent::AgentSession session(ws);
        expect(eq(session.tool_session().work_dir, ws));
        expect(session.tool_session().native_io);
        expect(!session.id().empty());

        FakeBackend backend;
        // Step 1: the model emits a Write tool call with BROKEN JSON
        // (trailing comma + unquoted key) -> the soul must repair it.
        kimix::llm::ChatResult step1;
        step1.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call_1";
        tc.name = "Write";
        tc.arguments = R"JSON({file_path: "soul_note.txt", "content": "written by soul",})JSON";
        step1.tool_calls.push_back(tc);
        // Step 2: a Read tool call to verify.
        kimix::llm::ChatResult step2;
        step2.ok = true;
        kimix::llm::ToolCall tc2;
        tc2.id = "call_2";
        tc2.name = "read"; // case-insensitive registry lookup
        tc2.arguments = R"JSON({"file_path":"soul_note.txt"})JSON";
        step2.tool_calls.push_back(tc2);
        // Step 3: final text.
        kimix::llm::ChatResult step3;
        step3.ok = true;
        step3.content = "All done.";
        backend.scripted = {step1, step2, step3};

        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::TurnResult tr = soul.turn("write and read a note");
        expect(tr.ok) << tr.error;
        expect(eq(tr.content, kimix::string("All done.")));
        expect(eq(tr.steps, 3));

        // The tool results landed in the history.
        bool saw_write_ok = false;
        bool saw_read_ok = false;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool" && m.tool_call_id == "call_1") {
                saw_write_ok = m.content.find("\"status\":\"ok\"") != kimix::string::npos;
            }
            if (m.role == "tool" && m.tool_call_id == "call_2") {
                saw_read_ok = m.content.find("written by soul") != kimix::string::npos;
            }
        }
        expect(saw_write_ok);
        expect(saw_read_ok);
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "soul_note.txt", ec));
    };

    "soul_unknown_tool_and_bad_json"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        kimix::string err;
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::string out = soul.execute_tool_call("NoSuchTool", "{}", err);
        expect(!err.empty());
        expect(out.find("unknown tool") != kimix::string::npos);

        // Unrepairable JSON surfaces as an error message, not a crash.
        err.clear();
        const kimix::string out2 =
            soul.execute_tool_call("Read", "]]not json[[", err);
        expect(!err.empty());
        expect(out2.find("invalid tool arguments") != kimix::string::npos);
    };

    "soul_compact_context"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        // The compaction LLM call returns a canned summary.
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};

        kimix::agent::KimiSoul soul(session, backend);
        // Build a long history: user/assistant/tool triples.
        auto &h = session.history();
        for (int i = 0; i < 8; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = kimix::format("question {} with some padding text to "
                                      "make the token estimate grow", i);
            h.push_back(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = kimix::format("answer {} plus a longer explanation "
                                      "that should be compacted away", i);
            kimix::llm::ToolCall tc;
            tc.id = kimix::format("c{}", i);
            tc.name = "Bash";
            tc.arguments = "{\"cmd\":\"ls\"}";
            a.tool_calls.push_back(tc);
            h.push_back(a);
            kimix::llm::Message t;
            t.role = "tool";
            t.tool_call_id = tc.id;
            t.content = "file1\nfile2\n";
            h.push_back(t);
        }
        const size_t before = h.size();
        kimix::string err;
        expect(soul.compact_context("keep the tests", err)) << err;
        expect(h.size() < before);
        expect(eq(soul.compaction_count(), 1));
        // First message is the summary; the tail must not start with an orphan
        // tool message.
        expect(h.front().content.find("compacted") != kimix::string::npos);
        expect(h[1].role != "tool");
        // The compacted tail still contains the most recent exchange.
        expect(h.back().content.find("file1") != kimix::string::npos ||
               h[h.size() - 2].content.find("file1") != kimix::string::npos);
    };

    "soul_auto_compact_in_turn"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul::options opts;
        opts.auto_compact = true;
        opts.reserved_context = 8192;
        // Force compaction by using a tiny context window.
        class TinyBackend : public kimix::agent::IChatBackend {
        public:
            kimix::vector<kimix::llm::ChatResult> scripted;
            size_t index = 0;
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &messages,
                 const kimix::vector<kimix::llm::Tool> &tools,
                 const kimix::llm::ChunkCallback &on_chunk) override {
                (void)tools;
                (void)on_chunk;
                (void)messages;
                if (index < scripted.size()) {
                    return scripted[index++];
                }
                kimix::llm::ChatResult r;
                r.ok = true;
                r.content = "final";
                return r;
            }
            int64_t max_context_size() const override { return 200; }
            kimix::string model_name() const override { return "tiny"; }
        };
        TinyBackend tiny;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>summarized</current_focus>";
        kimix::llm::ChatResult final_text;
        final_text.ok = true;
        final_text.content = "wrapped up";
        tiny.scripted = {summary, final_text};

        kimix::agent::KimiSoul soul(session, tiny, opts);
        // Seed a long history so the estimate exceeds the tiny window.
        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = kimix::string(500, 'x');
            h.push_back(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = kimix::string(500, 'y');
            h.push_back(a);
        }
        const kimix::agent::TurnResult tr = soul.turn("continue please");
        expect(tr.ok) << tr.error;
        expect(tr.compacted);
        expect(eq(soul.compaction_count(), 1));
        expect(eq(tr.content, kimix::string("wrapped up")));
    };

    "soul_tool_definitions_from_registry"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const auto defs = soul.tool_definitions();
        expect(defs.size() >= 12u);
        bool found_bash = false;
        for (const auto &d : defs) {
            if (d.name == "Bash") {
                found_bash = true;
                expect(d.parameters_json.find("\"cmd\"") != kimix::string::npos);
            }
        }
        expect(found_bash);
    };

    kimix::builtin_tools::proc::stop_all_tasks();
    return 0;
}
