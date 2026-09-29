// test_reflection_error_log.cpp - Reflection mode (KimiSoul::options::
// reflection): when on, every built-in tool call whose result payload is NOT
// status "ok" is appended as one JSONL line to
// <work_dir>/.kimix_cache/error_log/<session_id>.jsonl - original arguments,
// kimix::Clock-measured elapsed time, message and output. When off (the
// default) nothing is written.
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <cstdio>

#include <core/kimix_core.h>

#include "agent/soul.h"

namespace {

using namespace boost::ut;

// Minimal backend: execute_tool_call never chats, but KimiSoul needs one.
class SilentBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "silent";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "silent"; }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

kimix::string log_path(const kimix::string &ws, const char *stem) {
    return kimix::to_string(kimix::filesystem::path(ws) / ".kimix_cache" /
                            "error_log" / (kimix::string(stem) + ".jsonl"));
}

kimix::string read_text_file(const kimix::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    kimix::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

size_t line_count(const kimix::string &text) {
    if (text.empty()) {
        return 0;
    }
    size_t n = 1;
    for (char c : text) {
        if (c == '\n') {
            ++n;
        }
    }
    return text.back() == '\n' ? n - 1 : n;
}

} // namespace

int main() {
    using namespace boost::ut;
    using namespace kimix::agent;

    "reflection_mode_logs_a_failed_tool_call"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_refl_log_ws");
        SilentBackend backend;
        AgentSession session(ws);
        session.tool_session().session_id = "refl-test";
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.reflection = true;
        KimiSoul soul(session, backend, opts);
        kimix::string err;
        const kimix::string out = soul.execute_tool_call(
            "read", R"({"file_path":"missing.txt"})", err);
        expect(out.find("<system>ERROR") == 0) << out;
        // One JSONL line in the session's error_log file.
        const kimix::string log = read_text_file(log_path(ws, "refl-test"));
        expect(!log.empty()) << "the session's jsonl was created";
        expect(line_count(log) == 1_i) << log;
        // The recorded fields: tool, ORIGINAL arguments (as embedded JSON),
        // the Clock-measured time and the returned message + output.
        expect(log.find("\"tool\":\"read\"") != kimix::string::npos) << log;
        expect(log.find("\"arguments\":{\"file_path\":\"missing.txt\"}") !=
               kimix::string::npos)
            << log;
        expect(log.find("\"elapsed_ms\":") != kimix::string::npos) << log;
        expect(log.find("\"message\":") != kimix::string::npos) << log;
        expect(log.find("\"output\":") != kimix::string::npos) << log;
        // The failure text the model is told about is in the record.
        expect(log.find("missing.txt") != kimix::string::npos) << log;
    };

    "reflection_mode_appends_one_line_per_failure"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_refl_log_ws2");
        SilentBackend backend;
        AgentSession session(ws);
        session.tool_session().session_id = "refl-multi";
        KimiSoul::options opts;
        opts.reflection = true;
        KimiSoul soul(session, backend, opts);
        for (int i = 0; i < 3; ++i) {
            kimix::string err;
            const kimix::string args = kimix::format(
                "{{\"file_path\":\"gone_{}.txt\"}}", i);
            soul.execute_tool_call("read", args, err);
        }
        const kimix::string log = read_text_file(log_path(ws, "refl-multi"));
        expect(line_count(log) == 3_i) << log;
    };

    "successful_tool_calls_are_not_logged"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_refl_log_ws3");
        SilentBackend backend;
        AgentSession session(ws);
        session.tool_session().session_id = "refl-ok";
        KimiSoul::options opts;
        opts.reflection = true;
        KimiSoul soul(session, backend, opts);
        kimix::string err;
        const kimix::string out = soul.execute_tool_call(
            "write", R"({"file_path":"ok.txt","content":"hi"})", err);
        expect(out.find("<system>ERROR") != 0) << out;
        // The error_log directory may not even exist: nothing failed.
        const kimix::string log = read_text_file(log_path(ws, "refl-ok"));
        expect(log.empty()) << log;
    };

    "reflection_mode_off_by_default_writes_nothing"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_refl_log_ws4");
        SilentBackend backend;
        AgentSession session(ws);
        session.tool_session().session_id = "refl-off";
        KimiSoul::options opts; // reflection defaults to false
        expect(!opts.reflection);
        KimiSoul soul(session, backend, opts);
        kimix::string err;
        soul.execute_tool_call("read", R"({"file_path":"missing.txt"})", err);
        const kimix::string log = read_text_file(log_path(ws, "refl-off"));
        expect(log.empty()) << log;
    };

    return 0;
}
