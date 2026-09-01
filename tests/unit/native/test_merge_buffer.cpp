// Test for src/runtime/codec/merge_buffer.h + args_buffer.h (plans 007/010).
// This test covers:
// - WireMergeBuffer: first-append accepts any kind; same-kind text/args
//   parts merge; kind change flushes (append returns false); snapshot /
//   reset / empty semantics
// - ArgsBuffer: append / snapshot / delta_since watermark advancement;
//   reset clears bytes and the caller's watermark

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/codec/merge_buffer.h>
#include <runtime/codec/args_buffer.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::codec;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "wire_merge_same_kind_text_merges"_test = [] {
        WireMergeBuffer buf;
        expect(buf.empty());
        expect(buf.append("text", "Hello "));
        expect(!buf.empty());
        expect(eq(buf.kind(), kimix::string_view("text")));
        expect(buf.append("text", "World"));
        expect(eq(buf.snapshot(), kimix::string_view("Hello World")));
    };

    "wire_merge_kind_change_flushes"_test = [] {
        WireMergeBuffer buf;
        expect(buf.append("text", "part1"));
        // Same-kind merge OK...
        expect(buf.append("text", " part2"));
        // ...kind change -> false, buffer untouched.
        expect(!buf.append("args", "{\"k\":1}"));
        expect(eq(buf.snapshot(), kimix::string_view("part1 part2")));
        // Caller flushes + resets, then the new kind is accepted.
        buf.reset();
        expect(buf.empty());
        expect(buf.append("args", "{\"k\":1}"));
        expect(buf.append("args", ",\"j\":2}"));
        expect(eq(buf.snapshot(), kimix::string_view("{\"k\":1},\"j\":2}")));
    };

    "wire_merge_unknown_kind_never_merges"_test = [] {
        WireMergeBuffer buf;
        // First part of any kind starts the group.
        expect(buf.append("think", "a"));
        // A second "think" part is NOT mergeable (only text/args merge).
        expect(!buf.append("think", "b"));
        expect(eq(buf.snapshot(), kimix::string_view("a")));
        buf.reset();
        expect(buf.append("think", "b"));
        expect(eq(buf.snapshot(), kimix::string_view("b")));
    };

    "wire_merge_reset_and_empty"_test = [] {
        WireMergeBuffer buf;
        expect(buf.empty());
        expect(buf.append("text", "x"));
        expect(!buf.empty());
        buf.reset();
        expect(buf.empty());
        expect(buf.snapshot().empty());
        expect(buf.kind().empty());
        // After reset, a different kind starts fresh.
        expect(buf.append("args", "{}"));
    };

    "args_buffer_append_snapshot_delta"_test = [] {
        ArgsBuffer buf;
        size_t wm = 0;
        expect(buf.empty());
        buf.append("{\"a\":");
        buf.append("1}");
        expect(eq(buf.snapshot(), kimix::string_view("{\"a\":1}")));
        expect(eq(buf.size(), 7u));

        // delta_since returns everything since the last call.
        kimix::string_view d1 = buf.delta_since(wm);
        expect(eq(d1, kimix::string_view("{\"a\":1}")));
        expect(eq(wm, 7u));

        buf.append(", \"b\": [1,2]");
        kimix::string_view d2 = buf.delta_since(wm);
        expect(eq(d2, kimix::string_view(", \"b\": [1,2]")));
        expect(eq(wm, buf.size()));

        // No new data -> empty delta.
        kimix::string_view d3 = buf.delta_since(wm);
        expect(d3.empty());

        // reset clears bytes; the caller resets its watermark alongside.
        buf.reset();
        wm = 0;
        expect(buf.empty());
        expect(eq(buf.size(), 0u));
        buf.append("x");
        expect(eq(buf.delta_since(wm), kimix::string_view("x")));
    };

    "args_buffer_amortized_appends"_test = [] {
        // 10k small appends must not corrupt the accumulated bytes.
        ArgsBuffer buf;
        size_t wm = 0;
        kimix::string expected;
        for (int i = 0; i < 10000; ++i) {
            kimix::string part;
            part += "p";
            part += std::to_string(i);
            part += ',';
            buf.append(part);
            expected += part;
        }
        expect(eq(buf.size(), expected.size()));
        expect(eq(buf.snapshot(), kimix::string_view(expected)));
        // Deltas reconstruct the same bytes in order.
        kimix::string rebuilt;
        kimix::string_view d;
        while (!(d = buf.delta_since(wm)).empty()) {
            rebuilt.append(d.data(), d.size());
        }
        expect(eq(rebuilt, expected));
    };

    // -----------------------------------------------------------------------
    // Benchmarks -- WireMergeBuffer / ArgsBuffer (kimix_bench contract).
    // Production shape: LLM streaming where every streamed chunk is appended
    // and flushed per merge group / tool-call part. every case verifies the
    // accumulated bytes match an independently built reference.
    // -----------------------------------------------------------------------

    "bench_wire_merge_100k_chunks"_test = [] {
        // 100k small text chunks (per-stream chunk sizes), 100 distinct parts.
        kimix::vector<kimix::string> chunks;
        chunks.reserve(100);
        for (int i = 0; i < 100; ++i) {
            kimix::string part;
            part += "chunk_";
            part += std::to_string(i);
            part += ":";
            part += kimix::string(28, 'x');
            part += ",";
            chunks.push_back(std::move(part));
        }
        WireMergeBuffer buf;
        kimix::string expected;
        expected.reserve(100000 * chunks[0].size() + 64);
        for (size_t i = 0; i < 100000; ++i) {
            expected.append(chunks[i % 100].data(), chunks[i % 100].size());
        }
        // Sanity: one group of a few chunks merges correctly.
        buf.append("text", chunks[0]);
        buf.append("text", chunks[1]);
        kimix::string probe = chunks[0] + chunks[1];
        expect(eq(buf.snapshot(), kimix::string_view(probe)));
        kimix_bench::run("codec/wire_merge_100k_chunks",
                         [&] {
                             buf.reset();
                             for (size_t i = 0; i < 100000; ++i) {
                                 buf.append("text", chunks[i % 100]);
                             }
                         },
                         100000, static_cast<double>(chunks[0].size()));
        expect(eq(buf.kind(), kimix::string_view("text")));
        expect(eq(buf.snapshot(), kimix::string_view(expected)));
    };

    "bench_wire_merge_10k_frames"_test = [] {
        // 10k merge groups (frames), 4 args chunks each -- the ACP
        // tool-call stream shape.
        const kimix::string p0 = "{\"type\":\"args\",\"data\":\"";
        const kimix::string p1 = "tool_call_part_";
        const kimix::string p2 = "42\",\"seq\":";
        const kimix::string p3 = "7}";
        const kimix::string expected_frame = p0 + p1 + p2 + p3;
        WireMergeBuffer buf;
        kimix_bench::run("codec/wire_merge_10k_frames",
                         [&] {
                             for (size_t f = 0; f < 10000; ++f) {
                                 buf.reset();
                                 buf.append("args", p0);
                                 buf.append("args", p1);
                                 buf.append("args", p2);
                                 buf.append("args", p3);
                             }
                             kimix_bench::sink(buf.snapshot().size());
                         },
                         10000, static_cast<double>(expected_frame.size()));
        expect(eq(buf.snapshot(), kimix::string_view(expected_frame)));
    };

    "bench_args_buffer_encode_loop"_test = [] {
        // Append + delta_since per chunk, like _send_tool_call_part does.
        kimix::vector<kimix::string> chunks;
        chunks.reserve(100);
        for (int i = 0; i < 100; ++i) {
            kimix::string part;
            part += "\"k_";
            part += std::to_string(i);
            part += "\":";
            part += std::to_string(i * 7);
            part += ",";
            chunks.push_back(std::move(part));
        }
        ArgsBuffer buf;
        size_t wm = 0;
        kimix::string expected;
        expected.reserve(10000 * chunks[0].size() + 64);
        for (size_t i = 0; i < 10000; ++i) {
            expected.append(chunks[i % 100].data(), chunks[i % 100].size());
        }
        kimix_bench::run("codec/args_buffer_encode_10k",
                         [&] {
                             buf.reset();
                             wm = 0;
                             for (size_t i = 0; i < 10000; ++i) {
                                 buf.append(chunks[i % 100]);
                                 kimix::string_view d = buf.delta_since(wm);
                                 kimix_bench::sink(d.size());
                             }
                         },
                         10000, static_cast<double>(chunks[0].size()));
        expect(eq(buf.snapshot(), kimix::string_view(expected)));
    };

    return 0;
}
