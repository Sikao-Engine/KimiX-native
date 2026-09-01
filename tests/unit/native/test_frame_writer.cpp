// Test for src/runtime/codec/frame_writer.h (plan 007).
// This test covers:
// - JsonRpcFrameWriter: payload + "\n" (wire/server.py framing)
// - JsonlRecorder: frame + "\n" (wire/file.py _dump_line)
// - empty inputs; multi-sink single-serialize pattern

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/codec/frame_writer.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::codec;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "jsonrpc_frame_newline_delimited"_test = [] {
        JsonRpcFrameWriter w;
        kimix::string frame;
        const char* payload = R"({"jsonrpc":"2.0","id":1,"method":"event"})";
        w.write(kimix::string_view(payload), frame);
        expect(eq(frame, kimix::string(payload) + "\n"));
        // Empty payload -> a single newline.
        w.write(kimix::string_view(""), frame);
        expect(eq(frame, kimix::string("\n")));
    };

    "jsonrpc_span_overload"_test = [] {
        JsonRpcFrameWriter w;
        kimix::string frame;
        const char* payload = "abc";
        w.write(kimix::span<const char>(payload, 3), frame);
        expect(eq(frame, kimix::string("abc\n")));
    };

    "jsonl_recorder_appends_newline"_test = [] {
        JsonlRecorder r;
        kimix::string out;
        const char* record = R"({"timestamp":1.5,"message":{"type":"StepBegin","payload":{}}})";
        r.record(record, out);
        expect(eq(out, kimix::string(record) + "\n"));
        r.record("", out);
        expect(eq(out, kimix::string("\n")));
    };

    "single_serialize_multi_sink"_test = [] {
        // The plan's "serialize once, write to N sinks" pattern: one frame
        // built from the payload is written to both a socket frame and a
        // jsonl line without re-serialization.
        JsonRpcFrameWriter socket_writer;
        JsonlRecorder recorder;
        const char* envelope = R"({"type":"StepBegin","payload":{"n":1}})";
        kimix::string socket_frame;
        kimix::string file_line;
        socket_writer.write(envelope, socket_frame);
        recorder.record(envelope, file_line);
        expect(eq(socket_frame, kimix::string(envelope) + "\n"));
        expect(eq(file_line, kimix::string(envelope) + "\n"));
        // Both came from the same envelope bytes (no re-encode).
        expect(file_line.find("StepBegin") != kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // Benchmarks -- frame writers (kimix_bench contract). Production shape:
    // one frame/line per message, 100k+ per session; the same payload string
    // is framed again and again (reused output buffer, like the socket/file
    // fan-out loops). Byte-exact framing asserted before and after timing.
    // -----------------------------------------------------------------------

    "bench_jsonrpc_100k_frames"_test = [] {
        const kimix::string payload =
            R"({"jsonrpc":"2.0","id":123,"method":"event","params":)"
            R"({"type":"StepBegin","session_id":"sess_123","n":1}})";
        JsonRpcFrameWriter w;
        kimix::string frame;
        w.write(kimix::string_view(payload), frame);
        expect(eq(frame, payload + "\n"));
        kimix_bench::run("codec/jsonrpc_frame_write",
                         [&] {
                             w.write(kimix::string_view(payload), frame);
                             kimix_bench::sink(frame.size());
                         },
                         1, static_cast<double>(payload.size()) + 1.0);
        expect(eq(frame, payload + "\n"));
    };

    "bench_jsonl_100k_records"_test = [] {
        const kimix::string record =
            R"({"timestamp":1.5,"message":{"type":"ToolCall","payload":)"
            R"({"tool_name":"search","args":{"q":"wire jsonl test"}}}})";
        JsonlRecorder r;
        kimix::string out;
        r.record(kimix::string_view(record), out);
        expect(eq(out, record + "\n"));
        kimix_bench::run("codec/jsonl_record_write",
                         [&] {
                             r.record(kimix::string_view(record), out);
                             kimix_bench::sink(out.size());
                         },
                         1, static_cast<double>(record.size()) + 1.0);
        expect(eq(out, record + "\n"));
    };

    return 0;
}
