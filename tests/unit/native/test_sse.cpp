// Test for src/runtime/codec/sse.h (plan 008).
// This test covers:
// - byte-exact reference frame: "event: message\nid: <id>\ndata: <json>\n\n"
//   (src/kimix/server/bus.py BusEvent.to_sse)
// - multi-line payload -> repeated data: lines
// - event: line only when name non-empty; id: line only when id != 0
// - empty payload

#include "ut/ut.hpp"
#include <runtime/codec/sse.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::codec;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "sse_reference_frame_bytes"_test = [] {
        // Reference to_sse: f"event: message\nid: {id}\ndata: {json}\n\n"
        kimix::string out;
        build_sse_frame("message", R"({"id":"evt_1","type":"text","properties":{}})",
                        1, out);
        expect(eq(out, kimix::string(
            "event: message\n"
            "id: 1\n"
            "data: {\"id\":\"evt_1\",\"type\":\"text\",\"properties\":{}}\n"
            "\n")));
    };

    "sse_field_order_event_id_data"_test = [] {
        kimix::string out;
        build_sse_frame("text", "{\"x\":1}", 42, out);
        expect(out.find("event: text\n") == 0) << "event line first";
        const size_t id_pos = out.find("id: 42\n");
        expect(id_pos != kimix::string::npos);
        expect(id_pos > out.find("event:"));
        const size_t data_pos = out.find("data: {\"x\":1}\n");
        expect(data_pos != kimix::string::npos);
        expect(data_pos > id_pos);
        // Frame ends with a blank line.
        expect(out.size() >= 2);
        expect(eq(out.substr(out.size() - 2), kimix::string("\n\n")));
    };

    "sse_multiline_data_repeats_data_lines"_test = [] {
        kimix::string out;
        build_sse_frame("message", "line1\nline2\nline3", 7, out);
        expect(eq(out, kimix::string(
            "event: message\n"
            "id: 7\n"
            "data: line1\n"
            "data: line2\n"
            "data: line3\n"
            "\n")));
    };

    "sse_empty_event_name_omits_event_line"_test = [] {
        kimix::string out;
        build_sse_frame("", "{\"x\":1}", 5, out);
        expect(out.find("event:") == kimix::string::npos);
        expect(out.find("id: 5\n") != kimix::string::npos);
        expect(eq(out, kimix::string("id: 5\ndata: {\"x\":1}\n\n")));
    };

    "sse_zero_id_omits_id_line"_test = [] {
        kimix::string out;
        build_sse_frame("message", "data", 0, out);
        expect(out.find("id:") == kimix::string::npos);
        expect(eq(out, kimix::string("event: message\ndata: data\n\n")));
    };

    "sse_empty_payload"_test = [] {
        kimix::string out;
        build_sse_frame("message", "", 0, out);
        expect(eq(out, kimix::string("event: message\ndata: \n\n")));
    };

    "sse_large_id"_test = [] {
        kimix::string out;
        build_sse_frame("", "x", 18446744073709551615ull, out);
        expect(out.find("id: 18446744073709551615\n") != kimix::string::npos);
    };

    return 0;
}
