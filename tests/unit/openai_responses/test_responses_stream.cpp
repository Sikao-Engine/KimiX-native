// Test for the OpenAI Responses API SSE stream parser
// (openai_responses/stream_parser.h). Covers: reasoning_text deltas,
// output_text deltas, function_call output_item.added + arguments deltas/done,
// reasoning output_item.done fallback, completed usage, ignore events, CRLF
// and partial feeds.

#include "ut/ut.hpp"

#include "openai_responses/stream_parser.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "responses_hello_stream"_test = [] {
        openai_responses::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n"
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"rsn_1\",\"type\":\"reasoning\"}}\n\n"
            "data: {\"type\":\"response.reasoning_text.delta\",\"item_id\":\"rsn_1\",\"delta\":\"We\"}\n\n"
            "data: {\"type\":\"response.reasoning_text.delta\",\"item_id\":\"rsn_1\",\"delta\":\" need\"}\n\n"
            "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"delta\":\"Hello\"}\n\n"
            "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"delta\":\"!\"}\n\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"usage\":{\"input_tokens\":97,\"input_tokens_details\":{\"cached_tokens\":0},\"output_tokens\":20,\"total_tokens\":117}}}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 7u));
        expect(events[0].type == "response.created");
        expect(events[0].item_type.empty());
        expect(events[1].type == "response.output_item.added");
        expect(events[1].item_id == "rsn_1");
        expect(events[1].item_type == "reasoning");
        expect(events[2].type == "response.reasoning_text.delta");
        expect(events[2].delta == "We");
        expect(events[2].item_ref == "rsn_1");
        expect(events[3].delta == " need");
        expect(events[4].type == "response.output_text.delta");
        expect(events[4].delta == "Hello");
        expect(events[5].delta == "!");
        expect(events[6].type == "response.completed");
        expect(events[6].usage.has);
        expect(eq(events[6].usage.input_tokens, 97));
        expect(eq(events[6].usage.output_tokens, 20));
        expect(eq(events[6].usage.cached_tokens, 0));
        expect(eq(events[6].usage.total_tokens, 117));
        expect(parser.finish().empty());
    };

    "responses_tool_call_stream"_test = [] {
        openai_responses::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"call_id\":\"call_00_abc\",\"name\":\"get_weather\",\"arguments\":\"\"}}\n\n"
            "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"{\\\"city\\\":\\\"Bei\"}\n\n"
            "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"jing\\\"}\"}\n\n"
            "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_1\",\"arguments\":\"{\\\"city\\\":\\\"Beijing\\\"}\"}\n\n"
            "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"rsn_1\",\"type\":\"reasoning\",\"content\":[{\"type\":\"reasoning_text\",\"text\":\"fallback text\"}]}}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 5u));
        expect(events[0].item_type == "function_call");
        expect(events[0].call_id == "call_00_abc");
        expect(events[0].name == "get_weather");
        expect(events[1].delta == "{\"city\":\"Bei");
        expect(events[1].item_ref == "fc_1");
        expect(events[2].delta == "jing\"}");
        expect(events[3].type == "response.function_call_arguments.done");
        expect(events[3].text == "{\"city\":\"Beijing\"}");
        expect(events[4].item_type == "reasoning");
        expect(events[4].text == "fallback text");
    };

    "responses_partial_feed_crlf"_test = [] {
        openai_responses::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"delta\":\"Hi\"}\r\n\r\n";
        auto first = parser.feed(sse.data(), 10);
        expect(first.empty()) << "partial feed must buffer, not emit events";
        auto rest = parser.feed(sse.data() + 10, sse.size() - 10);
        expect(eq(rest.size(), 1u));
        expect(rest[0].delta == "Hi");
        expect(parser.finish().empty());
    };

    "responses_ignore_events"_test = [] {
        openai_responses::StreamParser parser;
        const std::string sse =
            "data: {\"type\":\"response.in_progress\"}\n\n"
            "data: {\"type\":\"response.output_text.done\",\"item_id\":\"m\"}\n\n";
        auto events = parser.feed(sse.data(), sse.size());
        expect(eq(events.size(), 2u));
        expect(events[0].type == "response.in_progress");
        expect(events[1].type == "response.output_text.done");
    };

    "responses_invalid_json"_test = [] {
        openai_responses::StreamParser parser;
        const std::string bad = "data: {not json}\n\n";
        auto events = parser.feed(bad.data(), bad.size());
        expect(eq(events.size(), 1u));
        expect(events[0].type.empty());
    };
}
