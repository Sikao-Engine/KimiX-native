// anthropic_chat_demo - Anthropic Messages API streaming workflow demo.
//
// Uses a real LLM backend configured by a JSON file (default
// C:/dev/backup_ds_flash.json, an Anthropic-compatible DeepSeek endpoint).
// The workflow mirrors anthropic.py's stream handling:
//   1. "hello" greeting turn     -> streams thinking + text content blocks
//   2. dummy tool call turn      -> model emits a tool_use block; we pass the
//                                   assistant message (with thinking signature)
//                                   and a fake tool_result back, then stream
//                                   the final answer
// Every turn is logged to stdout.

#include <cstdio>
#include <string>
#include <vector>

#include "anthropic/anthropic_chat.h"

using namespace anthropic;

namespace {

void log_line(const char *tag, const std::string &text) {
    if (text.empty()) {
        return;
    }
    std::printf("[%s] %s\n", tag, text.c_str());
}

void print_event(const StreamEvent &ev) {
    if (ev.type == "content_block_start") {
        if (ev.block_type == "tool_use") {
            std::printf("\n  [tool_use] id=%s name=%s input:", ev.block_id.c_str(),
                        ev.block_name.c_str());
            std::fflush(stdout);
        } else if (ev.block_type == "thinking") {
            std::printf("\033[90m(thinking) \033[0m");
            std::fflush(stdout);
        } else if (ev.block_type == "text") {
            // no marker needed; text deltas print inline
        }
    } else if (ev.type == "content_block_delta") {
        if (ev.delta_type == "text_delta") {
            std::printf("%s", ev.text.c_str());
            std::fflush(stdout);
        } else if (ev.delta_type == "thinking_delta") {
            std::printf("\033[90m%s\033[0m", ev.text.c_str());
            std::fflush(stdout);
        } else if (ev.delta_type == "input_json_delta") {
            std::printf("%s", ev.text.c_str());
            std::fflush(stdout);
        }
    } else if (ev.type == "message_delta") {
        if (!ev.stop_reason.empty()) {
            std::printf("\n  [stop_reason] %s\n", ev.stop_reason.c_str());
        }
    }
}

void log_result(const char *label, const ChatResult &r) {
    std::printf("\n--- %s ---\n", label);
    if (!r.ok) {
        std::printf("error: %s\n", r.error.c_str());
        return;
    }
    if (!r.thinking.empty()) {
        log_line("thinking", r.thinking);
    }
    if (!r.text.empty()) {
        log_line("response", r.text);
    }
    for (const auto &tu : r.tool_uses) {
        log_line("tool_use", tu.name + "(" + tu.input_json + ") id=" + tu.id);
    }
    std::printf("stop_reason: %s\n", r.stop_reason.c_str());
    std::printf("usage: input=%lld output=%lld cache_read=%lld cache_creation=%lld\n",
                (long long)r.input_tokens, (long long)r.output_tokens,
                (long long)r.cache_read_input_tokens,
                (long long)r.cache_creation_input_tokens);
}

} // namespace

int main(int argc, char *argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "C:/dev/backup_ds_flash.json";
    AnthropicConfig cfg;
    if (!load_config(config_path, cfg)) {
        std::fprintf(stderr, "failed to load Anthropic config from %s\n", config_path.c_str());
        return 1;
    }
    std::printf("config: model=%s url=%s thinking_effort=%s max_tokens=%d\n",
                cfg.model.c_str(), cfg.url.c_str(), cfg.thinking_effort.c_str(),
                cfg.max_tokens);

    const std::string system =
        "You are a helpful assistant. Greet the user when they say hello. "
        "Use the get_weather tool when asked about weather.";

    std::vector<ChatMessage> messages;

    // ---- 1) hello greeting turn ------------------------------------------
    messages.push_back({"user", "hello", "", "", {}, "", ""});
    std::printf("\n>>> hello greeting (streaming)\n");
    ChatResult hello = chat_completion_stream(cfg, system, messages, {}, print_event);
    log_result("hello greeting", hello);
    if (!hello.ok) {
        return 1;
    }
    messages.push_back({"assistant", hello.text, hello.thinking, hello.signature,
                        hello.tool_uses, "", ""});

    // ---- 2) dummy tool call turn -----------------------------------------
    Tool weather_tool;
    weather_tool.name = "get_weather";
    weather_tool.description = "Get the current weather for a city.";
    weather_tool.input_schema_json = R"({"type":"object","properties":{"city":{"type":"string","description":"The city to look up."}},"required":["city"]})";
    const std::vector<Tool> tools = {weather_tool};

    messages.push_back({"user", "What's the weather in Beijing?", "", "", {}, "", ""});
    std::printf("\n>>> dummy tool call (streaming)\n");
    ChatResult call = chat_completion_stream(cfg, system, messages, tools, print_event);
    log_result("tool call turn", call);
    if (!call.ok) {
        return 1;
    }
    if (call.tool_uses.empty()) {
        std::fprintf(stderr, "expected a get_weather tool_use, but the model returned none\n");
        return 1;
    }

    // Feed the assistant's thinking + tool_use and a dummy tool_result back.
    messages.push_back({"assistant", call.text, call.thinking, call.signature,
                        call.tool_uses, "", ""});
    for (const auto &tu : call.tool_uses) {
        const std::string dummy_result = R"({"city":"Beijing","temperature":20,"condition":"sunny"})";
        log_line("tool_result", tu.name + " " + tu.input_json + " => " + dummy_result);
        messages.push_back({"user", "", "", "", {}, tu.id, dummy_result});
    }

    // ---- 3) final answer after the tool result ---------------------------
    std::printf("\n>>> final answer after tool result (streaming)\n");
    ChatResult final = chat_completion_stream(cfg, system, messages, tools, print_event);
    log_result("final answer", final);

    std::printf("\n>>> workflow complete\n");
    return final.ok ? 0 : 1;
}
