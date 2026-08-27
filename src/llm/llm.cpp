// llm.cpp - Unified LLM interface implementation.
//
// Implements the three concrete ChatProviders (OpenAI Chat Completions,
// OpenAI Responses, Anthropic Messages), the LLM wrapper, and the
// create_llm / create_llm_from_file factories. Each provider adapts the
// unified Message/Tool/Chunk/ChatResult types to the wire types of the
// underlying provider library in src/llm/<provider>/.
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; unity build merges these TUs).

#include <httplib.h>

#include "llm/llm.h"

#include "llm/openai/openai_chat.h"
#include "llm/openai_responses/responses_chat.h"
#include "llm/anthropic/anthropic_chat.h"

#include <utility>

namespace kimix::llm {

namespace detail {

// Convert one unified Message into an OpenAI chat-completion message.
openai::ChatMessage to_openai_message(const Message &m) {
    openai::ChatMessage wm;
    wm.role = m.role;
    wm.content = m.content;
    wm.tool_call_id = m.tool_call_id;
    for (const auto &tc : m.tool_calls) {
        openai::ToolCall wtc;
        wtc.id = tc.id;
        wtc.type = tc.type;
        wtc.name = tc.name;
        wtc.arguments = tc.arguments;
        wm.tool_calls.push_back(std::move(wtc));
    }
    return wm;
}

// Convert one unified Tool into an OpenAI chat-completion tool.
openai::Tool to_openai_tool(const Tool &t) {
    openai::Tool wt;
    wt.name = t.name;
    wt.description = t.description;
    wt.parameters_json = t.parameters_json;
    return wt;
}

// Convert one unified ToolCall into an OpenAI tool-call delta.
ToolCall to_unified_tool_call(const openai::ToolCall &tc) {
    ToolCall utc;
    utc.id = tc.id;
    utc.type = tc.type;
    utc.name = tc.name;
    utc.arguments = tc.arguments;
    return utc;
}

// Convert one OpenAI streaming chunk into a unified Chunk.
Chunk to_unified_chunk(const openai::ChatChunk &raw) {
    Chunk c;
    c.ok = raw.ok;
    c.done = raw.done;
    c.role = raw.role;
    c.content = raw.content;
    c.reasoning = raw.reasoning_content;
    for (const auto &tcd : raw.tool_calls) {
        ToolCall tc;
        tc.id = tcd.id;
        tc.type = tcd.type;
        tc.name = tcd.name;
        tc.arguments = tcd.arguments;
        c.tool_calls.push_back(std::move(tc));
    }
    c.finish_reason = raw.finish_reason;
    c.has_usage = raw.has_usage;
    c.prompt_tokens = raw.prompt_tokens;
    c.completion_tokens = raw.completion_tokens;
    c.total_tokens = raw.total_tokens;
    return c;
}

// Convert an OpenAI chat-completion result into a unified ChatResult.
ChatResult to_unified_result(const openai::ChatResult &raw) {
    ChatResult r;
    r.ok = raw.ok;
    r.error = raw.error;
    r.content = raw.content;
    r.reasoning = raw.reasoning;
    for (const auto &tc : raw.tool_calls) {
        r.tool_calls.push_back(to_unified_tool_call(tc));
    }
    r.prompt_tokens = raw.prompt_tokens;
    r.completion_tokens = raw.completion_tokens;
    r.total_tokens = raw.total_tokens;
    return r;
}

// Convert one unified Message into one or more Responses input items.
// The mapping mirrors openai_responses/main.cpp's round-trip:
//   system/user -> message items; assistant -> message (+ reasoning + each
//   function_call); tool -> function_call_output.
void append_responses_input(const Message &m,
                            kimix::vector<openai_responses::InputItem> &input) {
    if (m.role == "system") {
        input.push_back({"message", "system", m.content, "", "", "", ""});
    } else if (m.role == "user") {
        input.push_back({"message", "user", m.content, "", "", "", ""});
    } else if (m.role == "assistant") {
        if (!m.content.empty()) {
            input.push_back({"message", "assistant", m.content, "", "", "", ""});
        }
        if (!m.thinking.empty()) {
            input.push_back({"reasoning", "", m.thinking, "", "", "", ""});
        }
        for (const auto &tc : m.tool_calls) {
            input.push_back({"function_call", "", "", "", tc.id, tc.name,
                             tc.arguments});
        }
    } else if (m.role == "tool") {
        input.push_back({"function_call_output", "", m.content, "", m.tool_call_id,
                         "", ""});
    }
}

// Convert one unified Tool into a Responses tool.
openai_responses::Tool to_responses_tool(const Tool &t) {
    openai_responses::Tool wt;
    wt.name = t.name;
    wt.description = t.description;
    wt.parameters_json = t.parameters_json;
    return wt;
}

// Convert one Responses FunctionCall into a unified ToolCall.
ToolCall to_unified_tool_call(const openai_responses::FunctionCall &fc) {
    ToolCall tc;
    tc.id = fc.call_id;
    tc.name = fc.name;
    tc.arguments = fc.arguments;
    return tc;
}

// Convert one Responses stream event into a unified Chunk.
Chunk to_unified_chunk(const openai_responses::StreamEvent &ev) {
    Chunk c;
    if (ev.type == "response.output_text.delta") {
        c.ok = true;
        c.content = ev.delta;
    } else if (ev.type == "response.reasoning_text.delta"
               || ev.type == "response.reasoning_summary_text.delta") {
        c.ok = true;
        c.reasoning = ev.delta;
    } else if (ev.type == "response.output_item.added") {
        if (ev.item_type == "function_call") {
            c.ok = true;
            ToolCall tc;
            tc.id = ev.call_id;
            tc.name = ev.name;
            tc.arguments = ev.arguments;
            c.tool_calls.push_back(std::move(tc));
        }
    } else if (ev.type == "response.function_call_arguments.delta") {
        c.ok = true;
        ToolCall tc;
        tc.arguments += ev.delta;
        c.tool_calls.push_back(std::move(tc));
    } else if (ev.type == "response.completed" && ev.usage.has) {
        c.ok = true;
        c.has_usage = true;
        c.prompt_tokens = ev.usage.input_tokens;
        c.completion_tokens = ev.usage.output_tokens;
        c.total_tokens = ev.usage.total_tokens;
    }
    return c;
}

// Convert a Responses result into a unified ChatResult.
ChatResult to_unified_result(const openai_responses::ChatResult &raw) {
    ChatResult r;
    r.ok = raw.ok;
    r.error = raw.error;
    r.content = raw.text;
    r.reasoning = raw.reasoning;
    for (const auto &fc : raw.tool_calls) {
        r.tool_calls.push_back(to_unified_tool_call(fc));
    }
    r.prompt_tokens = raw.input_tokens;
    r.completion_tokens = raw.output_tokens;
    r.cached_tokens = raw.cached_tokens;
    r.total_tokens = raw.total_tokens;
    return r;
}

// Convert one unified Message into an Anthropic messages message. System
// messages are handled by the caller (they become the separate `system`
// string), so they are skipped here.
anthropic::ChatMessage to_anthropic_message(const Message &m) {
    anthropic::ChatMessage wm;
    if (m.role == "user") {
        wm.role = "user";
        wm.text = m.content;
    } else if (m.role == "tool") {
        wm.role = "user";
        wm.tool_result_id = m.tool_call_id;
        wm.tool_result_content = m.content;
    } else if (m.role == "assistant") {
        wm.role = "assistant";
        wm.text = m.content;
        wm.thinking = m.thinking;
        wm.thinking_signature = m.thinking_signature;
        for (const auto &tc : m.tool_calls) {
            anthropic::ToolUse tu;
            tu.id = tc.id;
            tu.name = tc.name;
            tu.input_json = tc.arguments;
            wm.tool_uses.push_back(std::move(tu));
        }
    }
    return wm;
}

// Convert one unified Tool into an Anthropic tool.
anthropic::Tool to_anthropic_tool(const Tool &t) {
    anthropic::Tool wt;
    wt.name = t.name;
    wt.description = t.description;
    wt.input_schema_json = t.parameters_json;
    return wt;
}

// Convert one Anthropic stream event into a unified Chunk.
Chunk to_unified_chunk(const anthropic::StreamEvent &ev) {
    Chunk c;
    if (ev.type == "content_block_delta") {
        if (ev.delta_type == "text_delta") {
            c.ok = true;
            c.content = ev.text;
        } else if (ev.delta_type == "thinking_delta") {
            c.ok = true;
            c.reasoning = ev.text;
        } else if (ev.delta_type == "input_json_delta") {
            c.ok = true;
            ToolCall tc;
            tc.arguments += ev.text;
            c.tool_calls.push_back(std::move(tc));
        }
    } else if (ev.type == "content_block_start") {
        if (ev.block_type == "tool_use") {
            c.ok = true;
            ToolCall tc;
            tc.id = ev.block_id;
            tc.name = ev.block_name;
            c.tool_calls.push_back(std::move(tc));
        }
    } else if (ev.type == "message_delta" && !ev.stop_reason.empty()) {
        c.ok = true;
        c.finish_reason = ev.stop_reason;
    }
    return c;
}

// Convert an Anthropic result into a unified ChatResult.
ChatResult to_unified_result(const anthropic::ChatResult &raw) {
    ChatResult r;
    r.ok = raw.ok;
    r.error = raw.error;
    r.content = raw.text;
    r.reasoning = raw.thinking;
    for (const auto &tu : raw.tool_uses) {
        ToolCall tc;
        tc.id = tu.id;
        tc.name = tu.name;
        tc.arguments = tu.input_json;
        r.tool_calls.push_back(std::move(tc));
    }
    r.finish_reason = raw.stop_reason;
    r.signature = raw.signature;
    r.prompt_tokens = raw.input_tokens;
    r.completion_tokens = raw.output_tokens;
    r.cached_tokens = raw.cache_read_input_tokens;
    r.total_tokens = raw.input_tokens + raw.output_tokens;
    return r;
}

} // namespace detail

// ---------------------------------------------------------------------------
// OpenAIChatProvider - OpenAI Chat Completions backend
// ---------------------------------------------------------------------------
class OpenAIChatProvider : public ChatProvider {
public:
    explicit OpenAIChatProvider(Config config) : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_.model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk) const override {
        kimix::vector<openai::ChatMessage> wire_messages;
        wire_messages.reserve(messages.size());
        for (const auto &m : messages) {
            wire_messages.push_back(detail::to_openai_message(m));
        }
        kimix::vector<openai::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_openai_tool(t));
        }

        openai::ChunkCallback wrapper;
        if (on_chunk) {
            wrapper = [on_chunk](const openai::ChatChunk &raw) {
                on_chunk(detail::to_unified_chunk(raw));
            };
        }
        return detail::to_unified_result(
            openai::chat_completion_stream(config_, wire_messages, wire_tools, wrapper));
    }

private:
    Config config_;
};

// ---------------------------------------------------------------------------
// ResponsesChatProvider - OpenAI Responses API backend
// ---------------------------------------------------------------------------
class ResponsesChatProvider : public ChatProvider {
public:
    explicit ResponsesChatProvider(Config config) : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_.model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk) const override {
        kimix::vector<openai_responses::InputItem> input;
        for (const auto &m : messages) {
            detail::append_responses_input(m, input);
        }
        kimix::vector<openai_responses::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_responses_tool(t));
        }

        openai_responses::EventCallback wrapper;
        if (on_chunk) {
            wrapper = [on_chunk](const openai_responses::StreamEvent &ev) {
                on_chunk(detail::to_unified_chunk(ev));
            };
        }
        return detail::to_unified_result(
            openai_responses::responses_completion_stream(config_, input, wire_tools, wrapper));
    }

private:
    Config config_;
};

// ---------------------------------------------------------------------------
// AnthropicChatProvider - Anthropic Messages API backend
// ---------------------------------------------------------------------------
class AnthropicChatProvider : public ChatProvider {
public:
    explicit AnthropicChatProvider(Config config) : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_.model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk) const override {
        // System-role messages become a single `system` string (joined with
        // "\n" when multiple); they are not part of the messages array.
        kimix::string system;
        kimix::vector<anthropic::ChatMessage> wire_messages;
        for (const auto &m : messages) {
            if (m.role == "system") {
                if (!m.content.empty()) {
                    if (!system.empty()) {
                        system += '\n';
                    }
                    system += m.content;
                }
                continue;
            }
            wire_messages.push_back(detail::to_anthropic_message(m));
        }
        kimix::vector<anthropic::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_anthropic_tool(t));
        }

        anthropic::EventCallback wrapper;
        if (on_chunk) {
            wrapper = [on_chunk](const anthropic::StreamEvent &ev) {
                on_chunk(detail::to_unified_chunk(ev));
            };
        }
        return detail::to_unified_result(
            anthropic::chat_completion_stream(config_, system, wire_messages,
                                              wire_tools, wrapper));
    }

private:
    Config config_;
};

// ---------------------------------------------------------------------------
// LLM
// ---------------------------------------------------------------------------
LLM::LLM(kimix::unique_ptr<ChatProvider> provider, Config config)
    : provider_(std::move(provider)), config_(std::move(config)) {}

kimix::string LLM::model_name() const { return provider_->model_name(); }

const Config &LLM::config() const { return config_; }

int32_t LLM::max_context_size() const { return config_.max_context_size; }

ChatResult LLM::chat(const kimix::vector<Message> &messages,
                     const kimix::vector<Tool> &tools,
                     const ChunkCallback &on_chunk) const {
    return provider_->chat(messages, tools, on_chunk);
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
kimix::unique_ptr<LLM> create_llm(Config config) {
    if (config.model.empty() || config.url.empty()) {
        return nullptr;
    }
    kimix::unique_ptr<ChatProvider> provider;
    if (config.type == "openai" || config.type == "openai_legacy") {
        provider = kimix::unique_ptr<ChatProvider>(
            new OpenAIChatProvider(config));
    } else if (config.type == "openai_responses") {
        provider = kimix::unique_ptr<ChatProvider>(
            new ResponsesChatProvider(config));
    } else if (config.type == "anthropic") {
        provider = kimix::unique_ptr<ChatProvider>(
            new AnthropicChatProvider(config));
    } else {
        return nullptr;
    }
    return kimix::unique_ptr<LLM>(
        new LLM(std::move(provider), std::move(config)));
}

kimix::unique_ptr<LLM> create_llm_from_file(const kimix::string &path) {
    Config cfg;
    if (!load_config(path, cfg)) {
        return nullptr;
    }
    return create_llm(std::move(cfg));
}

} // namespace kimix::llm
