// test_wire.cpp - Unit tests for the live wire.jsonl event stream
// (src/agent/wire.*, gap B7/G11).
//
// Coverage:
// * the file layout: metadata header line on creation, one
//   {"timestamp","message":{"type","payload"}} record per line afterwards;
// * the exact record byte shapes of every emitted kind, ported from
//   kimi_cli/wire/types.py's pydantic field order (absent Optionals serialize
//   as explicit nulls - model_dump without exclude_none);
// * per-record timestamps through the injected clock;
// * append-after-reopen: reopening a session appends to the existing stream
//   and never rewrites the header;
// * the protocol version constant matching kimi_cli/wire/protocol.py.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/wire.h>

#include <cmath>
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
                                  "kimix_wire_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

// Read a whole file as one string ("" when missing/unreadable).
std::string read_all(const kimix::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Split into lines, dropping the trailing empty entry after the last newline.
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

} // namespace

int main() {
    using kimix::agent::WireWriter;

    "metadata_header_on_creation"_test = [] {
        const auto dir = test_dir("header");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        WireWriter w;
        w.now = [] { return 1000.5; };
        kimix::string error;
        expect(w.open(path, error));
        expect(w.is_open());
        w.close();
        const std::vector<std::string> lines =
            lines_of(read_all(dir / "wire.jsonl"));
        expect(eq(lines.size(), size_t{1}));
        expect(eq(lines[0],
                  std::string("{\"type\":\"metadata\",\"protocol_version\":\"1."
                              "11\"}")));
    };

    "record_byte_shapes"_test = [] {
        const auto dir = test_dir("shapes");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        WireWriter w;
        w.now = [] { return 42.25; };
        kimix::string error;
        expect(w.open(path, error));
        w.wire_turn_begin("hello");
        w.wire_turn_end();
        w.wire_step_begin(3);
        w.wire_step_interrupted();
        w.wire_steer_input(" steer me ");
        w.wire_step_retry(2, 3, 5, 1.5, "APIStatusError", 429);
        w.wire_step_retry(1, 2, 5, 0.3, "APIConnectionError", 0);
        w.wire_status_update(0.5, 1234, 128000, 100, 20, 5, 2);
        w.wire_status_update(0.0, -1, -1, -1, -1, -1, -1);
        w.wire_compaction_begin("cid123", "auto");
        w.wire_compaction_end("cid123", "auto", 999, 321, "");
        w.wire_compaction_end("cid123", "manual", -1, -1, "boom");
        w.close();
        const std::vector<std::string> lines =
            lines_of(read_all(dir / "wire.jsonl"));
        expect(eq(lines.size(), size_t{13}));
        if (lines.size() != 14) {
            return;
        }
        expect(eq(lines[1],
                  std::string("{\"timestamp\":42.25,\"message\":{\"type\":\"TurnBe"
                              "gin\",\"payload\":{\"user_input\":\"hello\"}}}")));
        expect(eq(lines[2],
                  std::string("{\"timestamp\":42.25,\"message\":{\"type\":\"TurnEn"
                              "d\",\"payload\":{}}}")));
        expect(eq(lines[3],
                  std::string("{\"timestamp\":42.25,\"message\":{\"type\":\"StepBe"
                              "gin\",\"payload\":{\"n\":3}}}")));
        expect(eq(lines[4],
                  std::string("{\"timestamp\":42.25,\"message\":{\"type\":\"StepIn"
                              "terrupted\",\"payload\":{}}}")));
        expect(eq(lines[5],
                  std::string("{\"timestamp\":42.25,\"message\":{\"type\":\"SteerIn"
                              "put\",\"payload\":{\"user_input\":\" steer me \"}}}"
                              )));
        expect(eq(lines[6],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"StepRetry\","
                      "\"payload\":{\"n\":2,\"next_attempt\":3,\"max_attempts\":5,"
                      "\"wait_s\":1.5,\"error_type\":\"APIStatusError\","
                      "\"status_code\":429}}}")));
        // status_code <= 0 serializes as null (the pydantic default None).
        expect(eq(lines[7],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"StepRetry\","
                      "\"payload\":{\"n\":1,\"next_attempt\":2,\"max_attempts\":5,"
                      "\"wait_s\":0.3,\"error_type\":\"APIConnectionError\","
                      "\"status_code\":null}}}")));
        // kosong TokenUsage field order: input_other, output,
        // input_cache_read, input_cache_creation.
        expect(eq(lines[8],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"StatusUpdate\","
                      "\"payload\":{\"context_usage\":0.5,\"context_tokens\":1234,"
                      "\"max_context_tokens\":128000,\"token_usage\":{"
                      "\"input_other\":100,\"output\":20,\"input_cache_read\":5,"
                      "\"input_cache_creation\":2},\"message_id\":null,"
                      "\"mcp_status\":null}}}")));
        // No provider usage at all: token_usage is null; absent context
        // numbers are null too.
        expect(eq(lines[9],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"StatusUpdate\","
                      "\"payload\":{\"context_usage\":0.0,\"context_tokens\":null,"
                      "\"max_context_tokens\":null,\"token_usage\":null,"
                      "\"message_id\":null,\"mcp_status\":null}}}")));
        expect(eq(lines[10],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"CompactionBegi"
                      "n\",\"payload\":{\"compaction_id\":\"cid123\",\"trigger\":"
                      "\"auto\",\"shadowed_tokens\":null}}}")));
        expect(eq(lines[11],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"CompactionEnd\","
                      "\"payload\":{\"compaction_id\":\"cid123\",\"trigger\":"
                      "\"auto\",\"shadowed_tokens\":999,"
                      "\"estimated_token_count\":321,\"error\":null}}}")));
        expect(eq(lines[12],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"CompactionEnd\","
                      "\"payload\":{\"compaction_id\":\"cid123\",\"trigger\":"
                      "\"manual\",\"shadowed_tokens\":null,"
                      "\"estimated_token_count\":null,\"error\":\"boom\"}}}")));
    };

    "g10_llm_request_and_snapshot_shapes"_test = [] {
        // The G10 request-trace records (llm_request_recorder.py): full
        // LLMRequest field order with the recorder-filled hashes, the
        // content-addressed LLMToolsSnapshot, MCPToolsDiscovered, the
        // ApprovalRequest/Response pair and the BtwBegin/BtwEnd pair - all
        // byte-pinned like the reference's pydantic field order (absent
        // Optionals serialize as null).
        const auto dir = test_dir("g10_shapes");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        WireWriter w;
        w.now = [] { return 42.25; };
        kimix::string error;
        expect(w.open(path, error));
        kimix::agent::llm_request_record rec;
        rec.kind = "loop";
        rec.provider = "openai_legacy";
        rec.model = "kimi-test";
        rec.system_prompt_hash = "abc123";
        rec.system_prompt = "the prompt";
        rec.tools_hash = "fed999";
        rec.message_count = 7;
        rec.turn_step = 2;
        rec.attempt = 1;
        w.wire_llm_request(rec);
        rec.turn_step = -1;
        rec.attempt = 3;
        rec.dropped_count = 5;
        rec.system_prompt.clear(); // second occurrence: inline prompt omitted
        w.wire_llm_request(rec);
        kimix::vector<kimix::llm::Tool> tools;
        kimix::llm::Tool tool;
        tool.name = "read";
        tool.description = "Reads a file.";
        tool.parameters_json = "{\"type\":\"object\"}";
        tools.push_back(tool);
        w.wire_llm_tools_snapshot("snap42", tools);
        w.wire_mcp_tools_discovered("srv", "mh", tools, {"a"}, {"b"});
        w.wire_approval_request("rid1", "call_9", "edit", "edit file",
                                "Edit file `x.txt`");
        w.wire_approval_response("rid1", "approve_for_session", "");
        w.wire_btw_begin("b1", "what is this?");
        w.wire_btw_end("b1", "an answer", "");
        w.wire_btw_end("b2", "", "No response received.");
        w.close();
        const std::vector<std::string> lines =
            lines_of(read_all(dir / "wire.jsonl"));
        expect(eq(lines.size(), size_t{10}));
        if (lines.size() != 10) {
            return;
        }
        expect(eq(lines[1],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"LLMRequest\","
                      "\"payload\":{\"kind\":\"loop\",\"provider\":\"openai_legacy\","
                      "\"model\":\"kimi-test\",\"thinking_effort\":null,"
                      "\"temperature\":null,\"top_p\":null,\"max_tokens\":null,"
                      "\"system_prompt_hash\":\"abc123\",\"system_prompt\":\"the "
                      "prompt\",\"tools_hash\":\"fed999\",\"message_count\":7,"
                      "\"turn_step\":2,\"attempt\":1,\"dropped_count\":null}}}")));
        expect(eq(lines[2],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"LLMRequest\","
                      "\"payload\":{\"kind\":\"loop\",\"provider\":\"openai_legacy\","
                      "\"model\":\"kimi-test\",\"thinking_effort\":null,"
                      "\"temperature\":null,\"top_p\":null,\"max_tokens\":null,"
                      "\"system_prompt_hash\":\"abc123\",\"system_prompt\":null,"
                      "\"tools_hash\":\"fed999\",\"message_count\":7,"
                      "\"turn_step\":null,\"attempt\":3,\"dropped_count\":5}}}")));
        expect(eq(lines[3],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"LLMToolsSnapsho"
                      "t\",\"payload\":{\"hash\":\"snap42\",\"tools\":[{\"name\":"
                      "\"read\",\"description\":\"Reads a file.\",\"parameters\":{"
                      "\"type\":\"object\"}}]}}}")));
        expect(eq(lines[4],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"MCPToolsDiscovered"
                      "\",\"payload\":{\"server_name\":\"srv\",\"hash\":\"mh\","
                      "\"tools\":[{\"name\":\"read\",\"description\":\"Reads a "
                      "file.\",\"parameters\":{\"type\":\"object\"}}],"
                      "\"enabled_names\":[\"a\"],\"collisions\":[\"b\"]}}}")));
        expect(eq(lines[5],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"ApprovalRequest\","
                      "\"payload\":{\"id\":\"rid1\",\"tool_call_id\":\"call_9\","
                      "\"sender\":\"edit\",\"action\":\"edit file\",\"description\":"
                      "\"Edit file `x.txt`\",\"source_kind\":\"foreground_turn\","
                      "\"source_id\":null,\"agent_id\":null,\"subagent_type\":null,"
                      "\"source_description\":null,\"display\":[]}}}")));
        expect(eq(lines[6],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"ApprovalResponse\","
                      "\"payload\":{\"request_id\":\"rid1\",\"response\":\"approve_"
                      "for_session\",\"feedback\":\"\"}}}")));
        expect(eq(lines[7],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"BtwBegin\","
                      "\"payload\":{\"id\":\"b1\",\"question\":\"what is this?\"}}}")));
        expect(eq(lines[8],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"BtwEnd\","
                      "\"payload\":{\"id\":\"b1\",\"response\":\"an answer\","
                      "\"error\":null}}}")));
        expect(eq(lines[9],
                  std::string(
                      "{\"timestamp\":42.25,\"message\":{\"type\":\"BtwEnd\","
                      "\"payload\":{\"id\":\"b2\",\"response\":null,"
                      "\"error\":\"No response received.\"}}}")));
    };

    "per_record_timestamps"_test = [] {
        const auto dir = test_dir("timestamps");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        double clock = 10.0;
        WireWriter w;
        w.now = [&clock] { return clock; };
        kimix::string error;
        expect(w.open(path, error));
        w.wire_turn_begin("a");
        clock = 10.5;
        w.wire_turn_end();
        w.close();
        const std::vector<std::string> lines =
            lines_of(read_all(dir / "wire.jsonl"));
        expect(eq(lines.size(), size_t{3}));
        expect(lines[1].find("\"timestamp\":10.0") != std::string::npos);
        expect(lines[2].find("\"timestamp\":10.5") != std::string::npos);
    };

    "append_after_reopen"_test = [] {
        const auto dir = test_dir("reopen");
        const kimix::string path = kimix::to_string(dir / "wire.jsonl");
        double clock = 1.0;
        {
            WireWriter w;
            w.now = [&clock] { return clock; };
            kimix::string error;
            expect(w.open(path, error));
            w.wire_turn_begin("first session");
            w.close();
        }
        clock = 2.0;
        {
            WireWriter w; // the resumed session: appends, no new header
            w.now = [&clock] { return clock; };
            kimix::string error;
            expect(w.open(path, error));
            w.wire_turn_begin("second session");
            w.close();
        }
        const std::vector<std::string> lines =
            lines_of(read_all(dir / "wire.jsonl"));
        expect(eq(lines.size(), size_t{3}));
        expect(lines[0].find("\"protocol_version\":\"1.11\"") !=
               std::string::npos);
        expect(lines[1].find("first session") != std::string::npos);
        expect(lines[2].find("second session") != std::string::npos);
        expect(lines[2].find("\"timestamp\":2.0") != std::string::npos);
    };

    "compaction_id_shape"_test = [] {
        const kimix::string id = kimix::agent::new_compaction_id();
        expect(eq(id.size(), size_t{32}));
        bool lower_hex = true;
        for (char c : id) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            lower_hex = lower_hex && ok;
        }
        expect(lower_hex);
    };

    "open_failure_reports_error"_test = [] {
    #if !defined(KIMIX_PLATFORM_WINDOWS)
     printf("[skip] wire open-failure pin uses a Windows separator\n");
     return;
    #endif

        WireWriter w;
        kimix::string error;
        // A path whose parent cannot be created (a file blocks the dir name).
        const auto dir = test_dir("open_fail");
        const kimix::string blocker = kimix::to_string(dir / "blocker");
        {
            std::ofstream f(blocker.c_str());
            f << "x";
        }
        expect(!w.open(blocker + "\\wire.jsonl", error));
        expect(!error.empty());
        expect(!w.is_open());
        // Emits on a closed writer are silent no-ops.
        w.wire_turn_begin("nobody");
    };
}
