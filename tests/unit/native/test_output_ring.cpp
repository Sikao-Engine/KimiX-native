// Test for src/runtime/tools/output_ring.{h,cpp} — the bounded background-job
// output buffer (report candidate C1).
//
// The ring must reproduce background/utils.py::bounded_append exactly: head
// int(cap*0.4) code points, the reference marker line, tail cap-head code
// points, all measured in CODE POINTS (Python str slicing). It must also stay
// amortised: appending to an already-truncated ring only touches the tail.

#include "ut/ut.hpp"

#include <runtime/common/utf8.h>
#include <runtime/tools/output_ring.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::tools::OutputRing;

namespace tests_unit_native_test_output_ring {

// The reference (Python) implementation of bounded_append, for parity.
void reference_append(kimix::string& content, kimix::string_view text, int64_t cap,
                      bool& truncated) {
    kimix::string full;
    full.append(content.data(), content.size());
    full.append(text.data(), text.size());
    const uint64_t chars =
        kimix::runtime::common::utf8_code_point_count(full);
    if (static_cast<int64_t>(chars) <= cap) {
        content = std::move(full);
        truncated = false;
        return;
    }
    const int64_t head_len = static_cast<int64_t>(static_cast<double>(cap) * 0.4);
    const int64_t tail_len = cap - head_len;
    // Byte offsets of the code-point indices.
    const auto offset_of = [&full](uint64_t index) {
        const char* it = full.data();
        const char* end = it + full.size();
        uint64_t i = 0;
        while (it < end && i < index) {
            kimix::runtime::common::decode_cp(it, end);
            ++i;
        }
        return static_cast<size_t>(it - full.data());
    };
    const size_t head_bytes = offset_of(static_cast<uint64_t>(head_len));
    const size_t tail_bytes = offset_of(chars - static_cast<uint64_t>(tail_len));
    kimix::string marker = kimix::format(
        "\n[... (output truncated, keeping first {} and last {} chars)]\n", head_len,
        tail_len);
    content.clear();
    content.append(full.data(), head_bytes);
    content.append(marker.data(), marker.size());
    content.append(full.data() + tail_bytes, full.size() - tail_bytes);
    truncated = true;
}

} // namespace tests_unit_native_test_output_ring
using namespace tests_unit_native_test_output_ring;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "output_ring_parity"_test = [] {
        const int64_t cap = 40;
        OutputRing ring(cap);
        kimix::string reference;
        const kimix::string_view chunks[] = {
            "0123456789",
            "abcdefghij",
            "KLMNOPQRST",
            "",
            "uvwxyz",
            "\xE4\xB8\x96\xE7\x95\x8C\xE4\xB8\x96\xE7\x95\x8C", // 4 CJK cp
            "0123456789012345678901234567890123456789",
        };
        for (kimix::string_view chunk : chunks) {
            bool ref_truncated = false;
            reference_append(reference, chunk, cap, ref_truncated);
            const bool truncated = ring.append(chunk);
            expect(truncated == ref_truncated);
            kimix::string snapshot;
            ring.snapshot(snapshot);
            expect(snapshot == reference);
        }
        expect(ring.truncated());
        expect(ring.char_count() ==
               kimix::runtime::common::utf8_code_point_count(reference));
        expect(ring.byte_count() == reference.size());
        expect(ring.bytes_owned() >= reference.size());
    };

    "output_ring_fast_path"_test = [] {
        OutputRing ring(1000);
        expect(!ring.append("hello "));
        expect(!ring.append("world"));
        kimix::string out;
        ring.snapshot(out);
        expect(out == "hello world");
        expect(!ring.truncated());
        expect(ring.char_count() == 11u);
    };

    "output_ring_read"_test = [] {
        OutputRing ring(10);
        ring.append("0123456789ABC"); // truncates: head 4, tail 6
        kimix::string full;
        ring.snapshot(full);
        expect(ring.truncated());
        kimix::string slice;
        ring.read(0, 4, slice);
        expect(slice == kimix::string_view(full.data(), 4));
        ring.read(4, 1000, slice);
        expect(slice.size() == full.size() - 4);
        ring.read(full.size() + 10, 5, slice);
        expect(slice.empty());
        ring.read(0, 0, slice);
        expect(slice.empty());
    };

    "output_ring_multibyte_boundaries"_test = [] {
        // 4-byte code points: a read must never split one.
        OutputRing ring(8);
        ring.append("\xF0\x9F\x98\x80\xF0\x9F\x98\x80"); // 2 emoji
        ring.append("\xF0\x9F\x98\x80");
        ring.append("\xF0\x9F\x98\x80");
        kimix::string out;
        ring.snapshot(out);
        for (size_t i = 0; i < ring.char_count(); ++i) {
            kimix::string one;
            ring.read(i, 1, one);
            expect(one.size() == 4u || one.size() == 1u); // 4-byte cp or a marker byte
        }
    };

    "output_ring_clear_and_cap"_test = [] {
        OutputRing ring(4);
        ring.append("0123456789");
        expect(ring.truncated());
        ring.clear();
        expect(!ring.truncated());
        expect(ring.char_count() == 0u);
        expect(ring.byte_count() == 0u);
        kimix::string out;
        ring.snapshot(out);
        expect(out.empty());
        expect(ring.cap_chars() == 4u);
        expect(!ring.append("abc"));
    };

    "output_ring_zero_cap"_test = [] {
        OutputRing ring(0);
        expect(ring.append("x")); // truncation happens immediately
        kimix::string out;
        ring.snapshot(out);
        // cap 0 -> head 0, tail 0 -> just the marker.
        expect(out.find("output truncated") != kimix::string::npos);
    };

    "bench_output_ring_append"_test = [] {
        OutputRing ring(2000000);
        kimix::string chunk(1024, 'x');
        for (int i = 0; i < 2000; ++i) {
            ring.append(chunk);
        }
        expect(ring.truncated());
        expect(ring.char_count() <= 2000000u + 200u);
        kimix::string out;
        ring.snapshot(out);
        expect(!out.empty());
    };

    return 0;
}
