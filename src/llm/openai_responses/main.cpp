// openai_responses_demo - OpenAI Responses API streaming workflow demo.
//
// Usage: openai_responses_demo <config.json>
// The config path is NOT hard-coded: pass any OpenAI-compatible config file
// (e.g. C:/dev/backup_ds_flash.json). The workflow mirrors openai_responses.py:
//   1. "hello" greeting turn      -> streams reasoning_text + output_text
//   2. dummy tool call turn       -> model emits a function_call; we feed the
//                                    reasoning item + function_call back with a
//                                    function_call_output, then stream the final
//                                    answer
// Every turn is logged to stdout.

#include <cstdio>

#include <core/kimix_core.h>

#include "llm/openai_responses/responses_chat.h"

using namespace kimix::llm::openai_responses;

static void log_line(const char *tag, const kimix::string &text) {
    if (text.empty()) {
        return;
    }
    std::printf("[%s] %s\n", tag, text.c_str());
}

static void print_event(const StreamEvent &ev) {
    if (ev.type == "response.reasoning_text.delta") {
        std::printf("\033[90m(reasoning) %s\033[0m", ev.delta.c_str());
        std::fflush(stdout);
    } else if (ev.type == "response.output_text.delta") {
        std::printf("%s", ev.delta.c_str());
        std::fflush(stdout);
    } else if (ev.type == "response.output_item.added") {
        if (ev.item_type == "function_call") {
            std::printf("\n  [function_call] id=%s name=%s args:", ev.call_id.c_str(),
                        ev.name.c_str());
            std::fflush(stdout);
        }
    } else if (ev.type == "response.function_call_arguments.delta") {
        std::printf("%s", ev.delta.c_str());
        std::fflush(stdout);
    }
}

static void log_result(const char *label, const ChatResult &r) {
    std::printf("\n--- %s ---\n", label);
    if (!r.ok) {
        std::printf("error: %s\n", r.error.c_str());
        return;
    }
    if (!r.reasoning.empty()) {
        log_line("reasoning", r.reasoning);
    }
    if (!r.text.empty()) {
        log_line("response", r.text);
    }
    for (const auto &tc : r.tool_calls) {
        log_line("function_call", tc.name + "(" + tc.arguments + ") id=" + tc.call_id);
    }
    std::printf("usage: input=%lld output=%lld cached=%lld total=%lld\n",
                (long long)r.input_tokens, (long long)r.output_tokens,
                (long long)r.cached_tokens, (long long)r.total_tokens);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: openai_responses_demo <config.json>\n");
        return 2;
    }
    const kimix::string config_path = argv[1];

    kimix::llm::Config cfg;
    if (!kimix::llm::load_config(config_path, cfg)) {
        std::fprintf(stderr, "failed to load config from %s\n", config_path.c_str());
        return 1;
    }
    std::printf("config: model=%s url=%s thinking_effort=%s max_tokens=%d\n",
                cfg.model.c_str(), cfg.url.c_str(), cfg.thinking_effort.c_str(),
                cfg.max_tokens);

    const kimix::string system =
        "You are a helpful assistant. Greet the user when they say hello. "
        "Use the get_weather tool when asked about weather.";

    kimix::vector<InputItem> input;
    input.push_back({"message", "system", system, "", "", "", ""});

    // ---- 1) hello greeting turn ------------------------------------------
    input.push_back({"message", "user", "hello", "", "", "", ""});
    std::printf("\n>>> hello greeting (streaming)\n");
    ChatResult hello = responses_completion_stream(cfg, input, {}, print_event);
    log_result("hello greeting", hello);
    if (!hello.ok) {
        return 1;
    }
    if (!hello.text.empty()) {
        input.push_back({"message", "assistant", hello.text, "", "", "", ""});
    }
    if (!hello.reasoning.empty()) {
        input.push_back({"reasoning", "", hello.reasoning, hello.reasoning_item_id, "", "", ""});
    }

    // ---- 2) dummy tool call turn -----------------------------------------
    Tool weather_tool;
    weather_tool.name = "get_weather";
    weather_tool.description = "Get the current weather for a city.";
    weather_tool.parameters_json = R"({"type":"object","properties":{"city":{"type":"string","description":"The city to look up."}},"required":["city"]})";
    const kimix::vector<Tool> tools = {weather_tool};

    input.push_back({"message", "user", "What's the weather in Beijing?", "", "", "", ""});
    std::printf("\n>>> dummy tool call (streaming)\n");
    ChatResult call = responses_completion_stream(cfg, input, tools, print_event);
    log_result("tool call turn", call);
    if (!call.ok) {
        return 1;
    }
    if (call.tool_calls.empty()) {
        std::fprintf(stderr, "expected a get_weather function_call, but the model returned none\n");
        return 1;
    }

    // Feed the assistant message, reasoning item and function_call back, then
    // the dummy function_call_output (mirrors openai_responses.py round-trip).
    if (!call.text.empty()) {
        input.push_back({"message", "assistant", call.text, "", "", "", ""});
    }
    if (!call.reasoning.empty()) {
        input.push_back({"reasoning", "", call.reasoning, call.reasoning_item_id, "", "", ""});
    }
    for (const auto &tc : call.tool_calls) {
        const kimix::string dummy_output = R"({"city":"Beijing","temperature":20,"condition":"sunny"})";
        log_line("function_call_output", tc.name + " " + tc.arguments + " => " + dummy_output);
        input.push_back({"function_call", "", "", "", tc.call_id, tc.name, tc.arguments});
        input.push_back({"function_call_output", "", dummy_output, "", tc.call_id, "", ""});
    }

    // ---- 3) final answer after the tool result ---------------------------
    std::printf("\n>>> final answer after function_call_output (streaming)\n");
    ChatResult final = responses_completion_stream(cfg, input, tools, print_event);
    log_result("final answer", final);

    std::printf("\n>>> workflow complete\n");
    return final.ok ? 0 : 1;
}
