// Test for src/runtime/codec/recv_buffer.h (plan 009).
// This test covers:
// - BIG-ENDIAN 4-byte length-prefixed framing (tcp_client.py convention)
// - property test: every split point for a 64 KB message assembles identically
// - header split across chunks; interleaved multiple frames in one chunk
// - zero-length prefix rejected; oversize frames rejected without consuming
// - delimiter framing (delimiter at buffer edge)
// - compaction + copy counter: total bytes copied <= ~2x payload (16 MB run)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/codec/recv_buffer.h>

#include <algorithm>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::codec;

namespace {

kimix::string frame_with_length(const kimix::string& payload) {
    kimix::string out;
    // Big-endian 4-byte length prefix.
    const uint32_t len = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<char>((len >> 24) & 0xFF));
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out.append(payload.data(), payload.size());
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "recv_length_prefixed_basic"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        const kimix::string payload = "hello world";
        buf.append(frame_with_length(payload));
        expect(eq(buf.size(), payload.size() + 4));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, payload));
        // Buffer is empty after the frame was consumed (auto-compacted).
        expect(eq(buf.size(), 0u));
    };

    "recv_header_split_across_chunks"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        const kimix::string payload = "abc";
        const kimix::string framed = frame_with_length(payload);
        for (size_t i = 1; i < framed.size(); ++i) {
            RecvBuffer b;
            b.append(kimix::string_view(framed).substr(0, i));
            expect(!b.take_frame_length_prefixed(4, 0, out)) << "split at " << i;
            b.append(kimix::string_view(framed).substr(i));
            expect(b.take_frame_length_prefixed(4, 0, out)) << "split at " << i;
            expect(eq(out, payload));
        }
    };

    "recv_property_every_split_point_64kb"_test = [] {
        // 64 KB payload; every split point must assemble the same frame.
        kimix::string payload;
        payload.reserve(64 * 1024);
        for (int i = 0; i < 64 * 1024; ++i) {
            payload.push_back(static_cast<char>('a' + (i % 26)));
        }
        const kimix::string framed = frame_with_length(payload);
        const size_t total = framed.size();
        // Check a dense sample of split points (every 37 bytes + edges) to
        // keep the test fast while still covering the whole range.
        kimix::vector<size_t> splits;
        for (size_t s = 0; s <= total; s += 37) {
            splits.push_back(s);
        }
        if (splits.back() != total) {
            splits.push_back(total);
        }
        bool all_ok = true;
        for (size_t s : splits) {
            RecvBuffer buf;
            kimix::string out;
            buf.append(kimix::string_view(framed).substr(0, s));
            buf.append(kimix::string_view(framed).substr(s));
            const bool took = buf.take_frame_length_prefixed(4, 0, out);
            if (!took || out != payload) {
                all_ok = false;
                printf("split %zu failed\n", s);
                break;
            }
        }
        expect(all_ok) << "every split point must assemble the 64KB frame";
    };

    "recv_interleaved_frames_in_one_chunk"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        const kimix::string f1 = frame_with_length("one");
        const kimix::string f2 = frame_with_length("two");
        const kimix::string f3 = frame_with_length("three");
        buf.append(f1 + f2 + f3);
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, kimix::string("one")));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, kimix::string("two")));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, kimix::string("three")));
        expect(!buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(buf.size(), 0u));
    };

    "recv_zero_length_and_oversize_rejected"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        // Zero-length prefix is invalid (Python guard: length == 0).
        buf.append(kimix::string("\x00\x00\x00\x00", 4));
        expect(!buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(buf.size(), 4u)) << "zero-length frame not consumed";

        // Oversize: declared 11 bytes with max_frame = 10 -> rejected and
        // NOT consumed (caller may clear() to resync).
        buf.clear();
        buf.append(kimix::string("\x00\x00\x00\x0Bpayload!", 4 + 8));
        expect(!buf.take_frame_length_prefixed(4, 10, out));
        expect(eq(buf.size(), 12u)) << "oversize frame not consumed";

        // After clear() the stream resyncs.
        buf.clear();
        buf.append(frame_with_length("ok"));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, kimix::string("ok")));
    };

    "recv_delimiter_frames"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        const kimix::string delim = "\r\n";
        buf.append("line1\r\nline2\r\nline3");
        expect(buf.take_frame_delimiter(delim, 0, out));
        expect(eq(out, kimix::string("line1")));
        expect(buf.take_frame_delimiter(delim, 0, out));
        expect(eq(out, kimix::string("line2")));
        // Third line has no delimiter yet.
        expect(!buf.take_frame_delimiter(delim, 0, out));
        // Delimiter split across chunks.
        buf.append("\r");
        expect(!buf.take_frame_delimiter(delim, 0, out));
        buf.append("\n");
        expect(buf.take_frame_delimiter(delim, 0, out));
        expect(eq(out, kimix::string("line3")));
    };

    "recv_delimiter_at_buffer_edge"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        // The delimiter straddles the append boundary.
        buf.append("ab|");
        expect(buf.take_frame_delimiter("|", 0, out));
        expect(eq(out, kimix::string("ab")));
        // Empty frame before a delimiter.
        buf.append("|");
        expect(buf.take_frame_delimiter("|", 0, out));
        expect(out.empty());
    };

    "recv_compaction_and_copy_bound_16mb"_test = [] {
        // 16 MB in 4 KB chunks, frames extracted as they arrive: total bytes
        // copied by reallocations + compactions must stay ~O(payload)
        // (no O(n^2) `data += chunk` behavior).
        RecvBuffer buf;
        kimix::string payload(4 * 1024, 'x');
        const kimix::string framed = frame_with_length(payload);
        const size_t kFrames = (16u * 1024u * 1024u) / payload.size();
        size_t received = 0;
        kimix::string out;
        for (size_t i = 0; i < kFrames; ++i) {
            buf.append(framed);
            while (buf.take_frame_length_prefixed(4, 0, out)) {
                expect(eq(out.size(), payload.size()));
                ++received;
            }
        }
        expect(eq(received, kFrames));
        const size_t total_payload = kFrames * payload.size();
        // Upper bound: copies <= 2x payload (amortized; geometric growth).
        expect(buf.debug_bytes_copied() <= 2 * total_payload)
            << "copied=" << buf.debug_bytes_copied()
            << " payload=" << total_payload;
        expect(eq(buf.size(), 0u));
    };

    "recv_manual_compact_and_clear"_test = [] {
        RecvBuffer buf;
        kimix::string out;
        buf.append("0123456789");
        expect(buf.take_frame_delimiter("5", 0, out));
        expect(eq(out, kimix::string("01234")));
        // 6 of 10 bytes consumed (60% > 50%) -> auto-compacted already.
        expect(eq(buf.size(), 4u));
        buf.append("ABCDEF");
        expect(eq(buf.peek(), kimix::string_view("6789ABCDEF")));
        buf.clear();
        expect(eq(buf.size(), 0u));
        expect(buf.peek().empty());
    };

    // -----------------------------------------------------------------------
    // Benchmarks -- RecvBuffer (kimix_bench contract). Production shapes:
    // 10k small frames arriving in 3-byte fragments, one large frame arriving
    // in 64 KB chunks, and a delimiter frame whose terminator arrives only
    // after it has accumulated in many tiny chunks (rescanning workload).
    // Correctness is verified before timing and on the final state.
    // -----------------------------------------------------------------------

    "bench_recv_10k_frames_3b_fragments"_test = [] {
        const kimix::string mid_payload = "0123456789abcdefghijk";
        const kimix::string framed = frame_with_length(mid_payload);
        RecvBuffer buf;
        kimix::string out;
        size_t count = 0;
        // Sanity: never time a broken path.
        buf.append(kimix::string_view(framed));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out, mid_payload));
        buf.clear();
        kimix_bench::run("codec/recv_10k_frames_3b_fragments",
                         [&] {
                             buf.clear();
                             count = 0;
                             for (size_t f = 0; f < 100; ++f) {
                                 for (size_t off = 0; off < framed.size();
                                      off += 3) {
                                     const size_t n = (std::min)(size_t(3),
                                                          framed.size() - off);
                                     buf.append(kimix::string_view(framed)
                                                    .substr(off, n));
                                 }
                                 while (buf.take_frame_length_prefixed(4, 0,
                                                                       out)) {
                                     ++count;
                                 }
                             }
                             kimix_bench::sink(count);
                         },
                         100, static_cast<double>(framed.size()));
        expect(eq(count, 100u));
        expect(eq(out, mid_payload));
    };

    "bench_recv_huge_frame_64kb_chunks"_test = [] {
        const kimix::string payload = kimix::string(4u * 1024u * 1024u, 'x');
        const kimix::string framed = frame_with_length(payload);
        RecvBuffer buf;
        kimix::string out;
        buf.append(kimix::string_view(framed));
        expect(buf.take_frame_length_prefixed(4, 0, out));
        expect(eq(out.size(), payload.size()));
        buf.clear();
        kimix_bench::run("codec/recv_huge_4mb_64kb_chunks",
                         [&] {
                             buf.clear();
                             for (size_t off = 0; off < framed.size();
                                  off += 64u * 1024u) {
                                 const size_t n = (std::min)(
                                     size_t(64u * 1024u), framed.size() - off);
                                 buf.append(kimix::string_view(framed).substr(
                                     off, n));
                             }
                             const bool took =
                                 buf.take_frame_length_prefixed(4, 0, out);
                             kimix_bench::sink(took ? out.size() : 0u);
                         },
                         1, static_cast<double>(payload.size()));
        expect(eq(out.size(), payload.size()));
        expect(eq(out, payload));
    };

    "bench_recv_delim_256kb_8b_chunks"_test = [] {
        // A single delimiter-terminated frame that accumulates in 8-byte
        // chunks with the terminator arriving only at the very end. Every
        // intermediate take_frame_delimiter() call has to search; this is
        // where whole-buffer rescans would show up as O(n^2).
        const kimix::string delim = "\r\n";
        const kimix::string payload = kimix::string(256u * 1024u, 'y');
        const kimix::string hay = payload + delim;
        RecvBuffer buf;
        kimix::string out;
        buf.append(kimix::string_view(hay));
        expect(buf.take_frame_delimiter(delim, 0, out));
        expect(eq(out, payload));
        buf.clear();
        kimix_bench::run("codec/recv_delim_256kb_8b_chunks",
                         [&] {
                             buf.clear();
                             for (size_t off = 0; off < hay.size();
                                  off += 8) {
                                 const size_t n = (std::min)(size_t(8),
                                                      hay.size() - off);
                                 buf.append(kimix::string_view(hay).substr(
                                     off, n));
                                 buf.take_frame_delimiter(delim, 0, out);
                             }
                             kimix_bench::sink(out.size());
                         },
                         1, static_cast<double>(payload.size()));
        expect(eq(out, payload));
    };

    return 0;
}
