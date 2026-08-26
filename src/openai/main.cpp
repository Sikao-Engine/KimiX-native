// openai_chat_demo - OpenAI-compatible chat completion streaming workflow demo.
//
// Uses a real LLM backend configured by a JSON file (default C:/dev/ds_flash.json,
// same shape as kimi-cli's ds_flash config). The workflow mirrors
// openai_legacy.py's dev main:
//   1. "hello" greeting turn     -> streams reasoning_content + content
//   2. dummy tool call turn      -> model calls get_weather, we feed back a
//                                   fake tool result, then stream the answer
// Every turn is logged to stdout.

#include <cstdio>
#include <string>
#include <vector>

#include "openai/openai_chat.h"

using namespace openai;

namespace {

void log_line(const char *tag, const std::string &text) {
    if (text.empty()) {
        return;
    }
    std::printf("[%s] %s\n", tag, text.c_str());
}

void print_chunk(const ChatChunk &chunk) {
    if (!chunk.reasoning_content.empty()) {
        std::printf("\033[90m(reasoning) %s\033[0m", chunk.reasoning_content.c_str());
        std::fflush(stdout);
    }
    if (chunk.has_content && !chunk.content.empty()) {
        std::printf("%s", chunk.content.c_str());
        std::fflush(stdout);
    }
    for (const auto &tc : chunk.tool_calls) {
        if (!tc.id.empty() || !tc.name.empty()) {
            std::printf("\n  [tool-call] id=%s name=%s args:", tc.id.c_str(), tc.name.c_str());
        } else {
            std::printf("%s", tc.arguments.c_str());
        }
        std::fflush(stdout);
    }
    if (!chunk.finish_reason.empty()) {
        std::printf("\n  [finish] %s\n", chunk.finish_reason.c_str());
    }
}

void log_result(const char *label, const ChatResult &r) {
    std::printf("\n--- %s ---\n", label);
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
    std::printf("usage: prompt=%lld completion=%lld total=%lld\n",
                (long long)r.prompt_tokens, (long long)r.completion_tokens,
                (long long)r.total_tokens);
}

} // namespace

int main(int argc, char *argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "C:/dev/ds_flash.json";
    LlmConfig cfg;
    if (!load_config(config_path, cfg)) {
        std::fprintf(stderr, "failed to load LLM config from %s\n", config_path.c_str());
        return 1;
    }
    std::printf("config: model=%s url=%s thinking_effort=%s\n",
                cfg.model.c_str(), cfg.url.c_str(), cfg.thinking_effort.c_str());

    std::vector<ChatMessage> messages;
    messages.push_back({"system",
                        "You are a helpful assistant. Greet the user when they say hello.",
                        "", {}});

    // ---- 1) hello greeting turn ------------------------------------------
    messages.push_back({"user", "hello", "", {}});
    std::printf("\n>>> hello greeting (streaming)\n");
    ChatResult hello = chat_completion_stream(cfg, messages, {}, print_chunk);
    log_result("hello greeting", hello);
    if (!hello.ok) {
        return 1;
    }
    messages.push_back({"assistant", hello.content, "", hello.tool_calls});

    // ---- 2) dummy tool call turn -----------------------------------------
    Tool weather_tool;
    weather_tool.name = "get_weather";
    weather_tool.description = "Get the current weather for a city.";
    weather_tool.parameters_json = R"({"type":"object","properties":{"city":{"type":"string","description":"The city to look up."}},"required":["city"]})";
    const std::vector<Tool> tools = {weather_tool};

    messages.push_back({"user", "What's the weather in Beijing?", "", {}});
    std::printf("\n>>> dummy tool call (streaming)\n");
    ChatResult call = chat_completion_stream(cfg, messages, tools, print_chunk);
    log_result("tool call turn", call);
    if (!call.ok) {
        return 1;
    }
    if (call.tool_calls.empty()) {
        std::fprintf(stderr, "expected a get_weather tool call, but the model returned none\n");
        return 1;
    }

    // Feed the assistant's tool call and a dummy tool result back to the model.
    messages.push_back({"assistant", call.content, "", call.tool_calls});
    for (const auto &tc : call.tool_calls) {
        const std::string dummy_result = R"({"city":"Beijing","temperature":20,"condition":"sunny"})";
        log_line("tool_result", tc.name + " " + tc.arguments + " => " + dummy_result);
        messages.push_back({"tool", dummy_result, tc.id, {}});
    }

    // ---- 3) final answer after the tool result ---------------------------
    std::printf("\n>>> final answer after tool result (streaming)\n");
    ChatResult final = chat_completion_stream(cfg, messages, tools, print_chunk);
    log_result("final answer", final);

    std::printf("\n>>> workflow complete\n");
    return final.ok ? 0 : 1;
}
