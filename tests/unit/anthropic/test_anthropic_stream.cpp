// Test for the Anthropic Messages API SSE stream parser
// (anthropic/stream_parser.h). Covers: message_start usage, thinking blocks
// with signature, text deltas, tool_use with input_json deltas, message_delta
// stop_reason, ping, CRLF and partial feeds.

#include "ut/ut.hpp"

#include "anthropic/stream_parser.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "anthropic_hello_stream"_test = [] {
        anthropic::StreamParser parser;
        const std::string sse =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":84,\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0,\"output_tokens\":0}}}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"We\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\" need\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig123\"}}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"!\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":84,\"output_tokens\":12}}\n\n"
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n\n"
            "event: ping\n"
            "data: {\"type\":\"ping\"}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 12u));
        expect(events[0].type == "message_start");
        expect(events[0].message_id == "msg_1");
        expect(events[0].usage.has);
        expect(eq(events[0].usage.input_tokens, 84));
        expect(events[1].type == "content_block_start");
        expect(events[1].block_type == "thinking");
        expect(events[2].delta_type == "thinking_delta");
        expect(events[2].text == "We");
        expect(events[3].text == " need");
        expect(events[4].delta_type == "signature_delta");
        expect(events[4].signature == "sig123");
        expect(events[5].block_type == "text");
        expect(events[6].delta_type == "text_delta");
        expect(events[6].text == "Hello");
        expect(events[7].text == "!");
        expect(events[8].type == "content_block_stop");
        expect(events[9].type == "message_delta");
        expect(events[9].stop_reason == "end_turn");
        expect(eq(events[9].usage.output_tokens, 12));
        expect(events[10].type == "message_stop");
        expect(events[11].type == "ping");
        expect(parser.finish().empty());
    };

    "anthropic_tool_use_stream"_test = [] {
        anthropic::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_00_abc\",\"name\":\"get_weather\",\"input\":{}}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"city\\\":\\\"Bei\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"jing\\\"}\"}}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":63}}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 4u));
        expect(events[0].type == "content_block_start");
        expect(events[0].block_type == "tool_use");
        expect(events[0].block_id == "call_00_abc");
        expect(events[0].block_name == "get_weather");
        expect(events[1].delta_type == "input_json_delta");
        expect(events[1].text == "{\"city\":\"Bei");
        expect(events[2].text == "jing\"}");
        expect(events[3].stop_reason == "tool_use");
        expect(eq(events[3].usage.output_tokens, 63));

        // Caller accumulates partial_json fragments into the full tool input.
        std::string accumulated;
        for (const auto &ev : events) {
            if (ev.delta_type == "input_json_delta") {
                accumulated += ev.text;
            }
        }
        expect(accumulated == "{\"city\":\"Beijing\"}");
    };

    "anthropic_partial_feed_crlf"_test = [] {
        anthropic::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\r\n\r\n";
        auto first = parser.feed(sse.data(), 10);
        expect(first.empty()) << "partial feed must buffer, not emit events";
        auto rest = parser.feed(sse.data() + 10, sse.size() - 10);
        expect(eq(rest.size(), 1u));
        expect(rest[0].delta_type == "text_delta");
        expect(rest[0].text == "Hi");
        expect(parser.finish().empty());
    };

    "anthropic_ping_ignored"_test = [] {
        anthropic::StreamParser parser;
        const std::string sse = "event: ping\ndata: {\"type\":\"ping\"}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 1u));
        expect(events[0].type == "ping");
    };

    "anthropic_invalid_json"_test = [] {
        anthropic::StreamParser parser;
        const std::string bad = "data: {not json}\n\n";
        auto events = parser.feed(bad.data(), bad.size());
        expect(eq(events.size(), 1u));
        expect(events[0].type.empty());
    };
}
