// Test for the OpenAI-compatible SSE stream parser (openai/sse_parser.h).
// Covers: hello content delta, reasoning_content delta, tool-call argument
// accumulation, finish reasons, [DONE], usage chunk, CRLF and partial feeds.

#include "ut/ut.hpp"

#include "openai/sse_parser.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "sse_hello_stream"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"Think hard\"},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"!\"},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        auto chunks = parser.feed(sse.data(), sse.size());
        expect(eq(chunks.size(), 5u));
        expect(chunks[0].ok);
        expect(chunks[0].role == "assistant");
        expect(chunks[0].reasoning_content == "Think hard");
        expect(!chunks[0].has_content);
        expect(chunks[1].has_content);
        expect(chunks[1].content == "Hello");
        expect(chunks[2].content == "!");
        expect(chunks[3].finish_reason == "stop");
        expect(chunks[4].done);
        expect(parser.finish().empty());
    };

    "sse_tool_call_deltas"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"city\\\":\\\"Bei\"}}]},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"jing\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
        auto chunks = parser.feed(sse.data(), sse.size());
        expect(eq(chunks.size(), 4u));
        expect(eq(chunks[0].tool_calls.size(), 1u));
        expect(chunks[0].tool_calls[0].id == "call_1");
        expect(chunks[0].tool_calls[0].type == "function");
        expect(chunks[0].tool_calls[0].name == "get_weather");
        expect(chunks[0].tool_calls[0].arguments.empty());
        expect(eq(chunks[1].tool_calls.size(), 1u));
        expect(chunks[1].tool_calls[0].arguments == "{\"city\":\"Bei");
        expect(chunks[2].tool_calls[0].arguments == "jing\"}");
        expect(chunks[3].finish_reason == "tool_calls");

        // The caller accumulates argument fragments; verify full JSON.
        std::string accumulated;
        for (const auto &c : chunks) {
            if (!c.tool_calls.empty()) {
                accumulated += c.tool_calls[0].arguments;
            }
        }
        expect(accumulated == "{\"city\":\"Beijing\"}");
    };

    "sse_usage_chunk"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"id\":\"x\",\"object\":\"chat.completion.chunk\",\"model\":\"m\",\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":8,\"total_tokens\":20}}\n\n";
        auto chunks = parser.feed(sse.data(), sse.size());
        expect(eq(chunks.size(), 1u));
        expect(chunks[0].ok);
        expect(chunks[0].has_usage);
        expect(eq(chunks[0].prompt_tokens, 12));
        expect(eq(chunks[0].completion_tokens, 8));
        expect(eq(chunks[0].total_tokens, 20));
    };

    "sse_partial_feed_crlf"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi\"},\"finish_reason\":null}]}\r\n\r\n";
        auto first = parser.feed(sse.data(), 10);
        expect(first.empty()) << "partial feed must buffer, not emit events";
        auto rest = parser.feed(sse.data() + 10, sse.size() - 10);
        expect(eq(rest.size(), 1u));
        expect(rest[0].content == "Hi");
        expect(parser.finish().empty());
    };

    "sse_invalid_json"_test = [] {
        openai::SseParser parser;
        const std::string bad = "data: {not json}\n\n";
        auto chunks = parser.feed(bad.data(), bad.size());
        expect(eq(chunks.size(), 1u));
        expect(!chunks[0].ok);
        expect(!chunks[0].done);
    };

    "sse_done_without_blank_line"_test = [] {
        openai::SseParser parser;
        const std::string tail = "data: [DONE]\n";
        auto chunks = parser.feed(tail.data(), tail.size());
        expect(chunks.empty());
        auto flushed = parser.finish();
        expect(eq(flushed.size(), 1u));
        expect(flushed[0].done);
    };
}
