// test_stream_filter.cpp - E9 (audit row 722): the reusable empty-part stream
// filter. The OpenAI Chat accumulator is structurally immune to blank deltas
// (pinned elsewhere); the filter exists so EVERY provider path - and any
// future on_chunk consumer - shares one guaranteed-blank-free stream.

#include "ut/ut.hpp"

#include <llm/stream_filter.h>

using namespace boost::ut;

int main() {
    "chunk_is_empty_classifies_blank_deltas"_test = [] {
        kimix::llm::Chunk chunk;
        expect(kimix::llm::chunk_is_empty(chunk));
        // Any real payload makes the chunk non-empty.
        chunk.content = "x";
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.content = "";
        chunk.reasoning = "t";
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.reasoning = "";
        chunk.role = "assistant";
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.role = "";
        chunk.finish_reason = "stop";
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.finish_reason = "";
        chunk.done = true;
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.done = false;
        chunk.has_usage = true;
        expect(!kimix::llm::chunk_is_empty(chunk));
        chunk.has_usage = false;
        chunk.tool_calls.push_back(kimix::llm::ToolCall{});
        expect(!kimix::llm::chunk_is_empty(chunk));
    };

    "filter_forwards_everything_but_blanks"_test = [] {
        kimix::vector<kimix::llm::Chunk> seen;
        kimix::llm::ChunkCallback collect = [&seen](const kimix::llm::Chunk &c) {
            seen.push_back(c);
        };
        kimix::llm::EmptyPartFilter filter(collect);

        kimix::llm::Chunk blank;
        filter(blank);
        kimix::llm::Chunk text;
        text.content = "hello";
        filter(text);
        kimix::llm::Chunk tool;
        tool.tool_calls.push_back(kimix::llm::ToolCall{});
        tool.tool_calls[0].name = "bash";
        filter(tool);
        kimix::llm::Chunk usage;
        usage.has_usage = true;
        usage.prompt_tokens = 11;
        filter(usage);
        filter(blank); // blanks interleaved anywhere stay dropped

        expect(eq(seen.size(), size_t(3)));
        if (seen.size() == 3) {
            expect(seen[0].content == kimix::string("hello"));
            expect(eq(seen[1].tool_calls.size(), size_t(1)));
            expect(seen[1].tool_calls[0].name == kimix::string("bash"));
            expect(seen[2].has_usage);
        }
    };
}
