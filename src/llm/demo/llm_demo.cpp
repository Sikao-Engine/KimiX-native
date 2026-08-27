// kimix_llm_demo - Unified LLM interface demo.
//
// Uses the kimix::llm::LLM facade: config.type decides which provider backend
// is used (openai/openai_legacy -> Chat Completions, openai_responses ->
// Responses API, anthropic -> Messages API), so one binary exercises all three
// with the same code path.
//
// Run with: xmake run kimix_llm_demo [config.json]  (default C:/dev/ds_flash.json)

#include <cstdio>

#include <core/kimix_core.h>

#include "llm/llm.h"

static void print_chunk(const kimix::llm::Chunk &chunk) {
    if (!chunk.reasoning.empty()) {
        std::printf("\033[90m(reasoning) %s\033[0m", chunk.reasoning.c_str());
        std::fflush(stdout);
    }
    if (!chunk.content.empty()) {
        std::printf("%s", chunk.content.c_str());
        std::fflush(stdout);
    }
    for (const auto &tc : chunk.tool_calls) {
        if (!tc.id.empty() || !tc.name.empty()) {
            std::printf("\n  [tool-call] id=%s name=%s args:", tc.id.c_str(),
                        tc.name.c_str());
        } else {
            std::printf("%s", tc.arguments.c_str());
        }
        std::fflush(stdout);
    }
    if (!chunk.finish_reason.empty()) {
        std::printf("\n  [finish] %s\n", chunk.finish_reason.c_str());
    }
}

static void log_line(const char *tag, const kimix::string &text) {
    if (text.empty()) {
        return;
    }
    std::printf("[%s] %s\n", tag, text.c_str());
}

static void log_result(const kimix::llm::ChatResult &r) {
    std::printf("\n--- chat result ---\n");
    if (!r.ok) {
        std::printf("error: %s\n", r.error.c_str());
        return;
    }
    if (!r.reasoning.empty()) {
        log_line("reasoning", r.reasoning);
    }
    if (!r.content.empty()) {
        log_line("response", r.content);
    }
    for (const auto &tc : r.tool_calls) {
        log_line("tool_call", tc.name + "(" + tc.arguments + ")");
    }
    std::printf("usage: prompt=%lld completion=%lld cached=%lld total=%lld\n",
                (long long)r.prompt_tokens, (long long)r.completion_tokens,
                (long long)r.cached_tokens, (long long)r.total_tokens);
}

int main(int argc, char *argv[]) {
    const kimix::string config_path = argc > 1 ? argv[1] : "C:/dev/ds_flash.json";

    auto llm = kimix::llm::create_llm_from_file(config_path);
    if (!llm) {
        std::fprintf(stderr, "failed to load LLM config / create LLM from %s\n",
                     config_path.c_str());
        return 1;
    }

    std::printf("provider: model=%s type=%s max_context_size=%d\n",
                llm->model_name().c_str(), llm->config().type.c_str(),
                llm->max_context_size());

    kimix::vector<kimix::llm::Message> messages;
    messages.push_back({"system",
                        "You are a helpful assistant. Greet the user when they say hello.",
                        "", {}, "", ""});
    messages.push_back({"user", "hello", "", {}, "", ""});

    std::printf("\n>>> hello greeting (streaming)\n");
    kimix::llm::ChatResult result =
        llm->chat(messages, {}, print_chunk);
    log_result(result);

    std::printf("\n>>> workflow complete\n");
    return result.ok ? 0 : 1;
}
