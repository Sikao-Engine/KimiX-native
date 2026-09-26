// test_llm_recorder.cpp - Unit tests for the LLM request recorder
// (src/agent/llm_recorder.*, gap G10).
//
// Coverage:
// * sha256 over canonicalized (sorted-keys) JSON matches CPython's
//   hashlib.sha256(orjson.dumps(obj, OPT_SORT_KEYS)) for a known payload;
// * record(): one LLMToolsSnapshot per unique tools hash, the LLMRequest
//   carrying provider identity + hashes, the system prompt inlined only on
//   the first occurrence of its hash;
// * record(): repeated calls with identical content emit NO further
//   snapshots and keep system_prompt null (content-addressed dedup);
// * restore_from(): seeding from an existing wire.jsonl suppresses the
//   durable re-log on a resumed session (both snapshot and inline prompt);
// * record_mcp_discovery(): dedup per (server_name, hash), the API kept for
//   the not-yet-ported MCP layer.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/llm_recorder.h>
#include <agent/wire.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_recorder_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

std::string read_all(const kimix::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

// Records what the recorder emitted.
struct recorder_sink : kimix::agent::WireSink {
    int snapshots = 0;
    int requests = 0;
    int mcp = 0;
    kimix::string last_tools_hash;
    kimix::agent::llm_request_record last;
    void wire_turn_begin(kimix::string_view) override {}
    void wire_turn_end() override {}
    void wire_step_begin(int32_t) override {}
    void wire_step_interrupted() override {}
    void wire_steer_input(kimix::string_view) override {}
    void wire_step_retry(int32_t, int32_t, int32_t, double, kimix::string_view,
                         int32_t) override {}
    void wire_status_update(double, int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t) override {}
    void wire_compaction_begin(kimix::string_view, kimix::string_view) override {}
    void wire_compaction_end(kimix::string_view, kimix::string_view, int64_t,
                             int64_t, kimix::string_view) override {}
    void wire_llm_request(const kimix::agent::llm_request_record &rec) override {
        ++requests;
        last = rec;
    }
    void wire_llm_tools_snapshot(kimix::string_view hash,
                                 const kimix::vector<kimix::llm::Tool> &) override {
        ++snapshots;
        last_tools_hash = kimix::string(hash);
    }
    void wire_mcp_tools_discovered(kimix::string_view, kimix::string_view,
                                   const kimix::vector<kimix::llm::Tool> &,
                                   const kimix::vector<kimix::string> &,
                                   const kimix::vector<kimix::string> &) override {
        ++mcp;
    }
    void wire_approval_request(kimix::string_view, kimix::string_view,
                               kimix::string_view, kimix::string_view,
                               kimix::string_view) override {}
    void wire_approval_response(kimix::string_view, kimix::string_view,
                                kimix::string_view) override {}
    void wire_btw_begin(kimix::string_view, kimix::string_view) override {}
    void wire_btw_end(kimix::string_view, kimix::string_view,
                      kimix::string_view) override {}
};

kimix::vector<kimix::llm::Tool> sample_tools() {
    kimix::vector<kimix::llm::Tool> tools;
    kimix::llm::Tool tool;
    tool.name = "read";
    tool.description = "Reads a file.";
    // Deliberately unsorted keys: the hash must be order-insensitive.
    tool.parameters_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}";
    tools.push_back(tool);
    return tools;
}

kimix::agent::recorder_provider_fields sample_identity() {
    kimix::agent::recorder_provider_fields id;
    id.provider = "openai_legacy";
    id.model = "kimi-test";
    id.thinking_effort = "high";
    id.max_tokens = 4096;
    return id;
}

} // namespace

int main() {
    using kimix::agent::LLMRequestRecorder;

    "hash_json_matches_python_orjson_sorted"_test = [] {
        // CPython: hashlib.sha256(orjson.dumps(
        //   [{"name":"read","description":"Reads a file.",
        //     "parameters":{"type":"object","properties":{"path":{"type":"string"}}}}],
        //   option=orjson.OPT_SORT_KEYS)).hexdigest()
        const kimix::string hash = kimix::agent::recorder_hash_json(
            "[{\"name\":\"read\",\"description\":\"Reads a file.\","
            "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\"}}}}]");
        expect(hash == "c337e452b620a51cf31cb27241615f68679ff0a0397831c5e2e5edff1"
                       "7e4a87d");
        expect(kimix::agent::recorder_hash_text("hello system prompt") ==
               "553e3d434b8c8237d9f08502c8c4817e3a9985ffa6e8d23bf4eb2f234d3770ad");
    };

    "record_emits_snapshot_once_and_inlines_prompt_once"_test = [] {
        LLMRequestRecorder recorder;
        recorder_sink sink;
        const kimix::vector<kimix::llm::Tool> tools = sample_tools();
        const kimix::agent::recorder_provider_fields id = sample_identity();
        recorder.record(&sink, id, "the system prompt", tools, 5, "loop", 1, 1);
        expect(sink.snapshots == 1);
        expect(sink.requests == 1);
        expect(sink.last.kind == "loop");
        expect(sink.last.provider == "openai_legacy");
        expect(sink.last.model == "kimi-test");
        expect(sink.last.thinking_effort == "high");
        expect(sink.last.max_tokens == 4096);
        expect(sink.last.message_count == 5);
        expect(sink.last.turn_step == 1);
        expect(sink.last.attempt == 1);
        expect(sink.last.system_prompt == "the system prompt")
            << "first occurrence inlines the prompt";
        expect(sink.last.system_prompt_hash ==
               "d7ed66b8bcaf0fd105306430101cd22f5a10cee77d0a7c97ea845c68ae229bc7");
        // Same content again: no snapshot, no inline prompt.
        recorder.record(&sink, id, "the system prompt", tools, 7, "loop", 2, 1);
        expect(sink.snapshots == 1);
        expect(sink.requests == 2);
        expect(sink.last.message_count == 7);
        expect(sink.last.turn_step == 2);
        expect(sink.last.system_prompt.empty()) << "hash already seen: no inline copy";
        expect(sink.last.tools_hash == sink.last_tools_hash)
            << "the request references the snapshot by hash";
        // A changed tool table re-emits the snapshot.
        kimix::vector<kimix::llm::Tool> changed = tools;
        changed.front().description = "Reads a file. (v2)";
        recorder.record(&sink, id, "the system prompt", changed, 7, "loop", 3, 1);
        expect(sink.snapshots == 2);
    };

    "restore_from_suppresses_durable_relog"_test = [] {
        const auto dir = test_dir("restore");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        const kimix::vector<kimix::llm::Tool> tools = sample_tools();
        const kimix::agent::recorder_provider_fields id = sample_identity();
        // A previous process recorded into this session's wire.jsonl.
        {
            LLMRequestRecorder first;
            kimix::agent::WireWriter w;
            kimix::string error;
            expect(w.open(path, error)) << error;
            first.record(&w, id, "the system prompt", tools, 5, "loop", 1, 1);
            w.close();
        }
        const int prior_lines = static_cast<int>(lines_of(read_all(dir / "wire.jsonl")).size());
        expect(prior_lines >= 3) << "header + snapshot + request";

        // The resumed session seeds from the file: nothing durable re-logs.
        LLMRequestRecorder resumed;
        resumed.restore_from(path);
        expect(resumed.seen_tools_hashes() >= 1u);
        expect(resumed.seen_prompt_hashes() >= 1u);
        recorder_sink sink;
        resumed.record(&sink, id, "the system prompt", tools, 9, "loop", 1, 1);
        expect(sink.snapshots == 0) << "the snapshot hash was restored";
        expect(sink.requests == 1);
        expect(sink.last.system_prompt.empty()) << "the prompt hash was restored";
    };

    "restore_from_tolerates_missing_and_garbage"_test = [] {
        LLMRequestRecorder recorder;
        recorder.restore_from(""); // no-op
        const auto dir = test_dir("restore_garbage");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        {
            std::ofstream f(kimix::to_string(path).c_str(), std::ios::binary);
            f << "not json at all\n{\"message\":{\"type\":\"LLMRequest\","
                 "\"payload\":{}}}\n";
        }
        recorder.restore_from(path); // skips garbage, keeps going
        recorder.restore_from(kimix::to_string(dir / "absent.jsonl")); // missing: fine
        expect(recorder.seen_tools_hashes() == 0u);
        expect(recorder.seen_prompt_hashes() == 0u);
    };

    "record_mcp_discovery_dedups_per_server_and_hash"_test = [] {
        LLMRequestRecorder recorder;
        recorder_sink sink;
        const kimix::vector<kimix::llm::Tool> tools = sample_tools();
        recorder.record_mcp_discovery(&sink, "fs", tools, {"read"}, {});
        expect(sink.mcp == 1);
        recorder.record_mcp_discovery(&sink, "fs", tools, {"read"}, {});
        expect(sink.mcp == 1) << "identical discovery is deduped";
        recorder.record_mcp_discovery(&sink, "other", tools, {"read"}, {});
        expect(sink.mcp == 2) << "a different server re-logs";
        recorder.record_mcp_discovery(nullptr, "fs", tools, {"read"}, {});
        expect(sink.mcp == 2) << "a null sink drops the record but never raises";
    };
}
