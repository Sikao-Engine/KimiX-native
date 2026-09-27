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

#include "llm/stream_filter.h" // E9: reusable empty-part filter

#include <core/json_repair.h>

#include "llm/openai/openai_chat.h"
#include "llm/openai_responses/responses_chat.h"
#include "llm/anthropic/anthropic_chat.h"

#include <utility>

namespace kimix::llm {

namespace detail {

// Map a provider transport error onto the unified taxonomy.
ChatErrorKind to_unified_error_kind(TransportErrorKind kind) {
    switch (kind) {
    case TransportErrorKind::connection:
        return ChatErrorKind::connection;
    case TransportErrorKind::timeout:
        return ChatErrorKind::timeout;
    case TransportErrorKind::empty_response:
        return ChatErrorKind::empty_response;
    case TransportErrorKind::http:
        return ChatErrorKind::http;
    default:
        return ChatErrorKind::none;
    }
}

// Repair a JSON string received from the LLM backend (e.g. tool-call
// arguments) that the model may have hallucinated into invalid JSON.
// Mirrors kimi_cli.tools.utils.repair_json_string: only strings that look
// like JSON (start with '{' or '[' after trimming) are candidates, and
// already-valid / unrepairable input is returned unchanged (kimix::repair
// returns the empty string for those, which must not clobber valid JSON).
kimix::string repair_backend_json(kimix::string json) {
    size_t i = 0;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                               json[i] == '\n' || json[i] == '\r'))
        i++;
    if (i == json.size() || (json[i] != '{' && json[i] != '[')) return json;
    kimix::string repaired = kimix::repair(json);
    return repaired.empty() ? std::move(json) : std::move(repaired);
}

// Convert one unified Message into an OpenAI chat-completion message.
// Tool-call arguments are sanitized: history may contain arguments from a
// glitching gateway (e.g. duplicated chunks merged into "{}{}") that strict
// backends 400 on when echoed back (scnet/Qwen code 10013).
openai::ChatMessage to_openai_message(const Message &m) {
    openai::ChatMessage wm;
    wm.role = m.role;
    wm.content = m.content;
    // E1/E2: media adjunct on user/assistant messages - the parts serialize
    // as the wire content array. Tool messages flatten to their text backbone
    // (openai_legacy's ToolMessageConversion == "extract_text" drops the
    // media parts of a tool result).
    if (m.role != "tool") {
        wm.parts = m.parts;
    }
    wm.tool_call_id = m.tool_call_id;
    for (const auto &tc : m.tool_calls) {
        openai::ToolCall wtc;
        wtc.id = tc.id;
        wtc.type = tc.type;
        wtc.name = tc.name;
        wtc.arguments = sanitize_tool_arguments(tc.arguments);
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
    r.error_kind = to_unified_error_kind(raw.error_kind);
    r.error_status = raw.error_status;
    r.retry_after_seconds = raw.retry_after_seconds;
    return r;
}

// Convert one unified Message into one or more Responses input items.
// The mapping mirrors openai_responses/main.cpp's round-trip:
//   system/user -> message items; assistant -> message (+ reasoning + each
//   function_call); tool -> function_call_output.
void append_responses_input(const Message &m,
                            kimix::vector<openai_responses::InputItem> &input) {
    const auto push_item = [&input](kimix::string_view type,
                                    kimix::string_view role,
                                    kimix::string_view content) {
        openai_responses::InputItem item;
        item.type.assign(type.data(), type.size());
        item.role.assign(role.data(), role.size());
        item.content.assign(content.data(), content.size());
        input.push_back(std::move(item));
    };
    if (m.role == "system") {
        push_item("message", "system", m.content);
    } else if (m.role == "user") {
        // E1/E2: media parts ride along on the message item; the provider
        // serializes them as an input content block array.
        if (m.parts.empty()) {
            push_item("message", "user", m.content);
        } else {
            openai_responses::InputItem item;
            item.type = "message";
            item.role = "user";
            item.content = m.content;
            item.parts = m.parts;
            input.push_back(std::move(item));
        }
    } else if (m.role == "assistant") {
        if (!m.content.empty()) {
            push_item("message", "assistant", m.content);
        }
        if (!m.thinking.empty()) {
            openai_responses::InputItem item;
            item.type = "reasoning";
            item.content = m.thinking;
            input.push_back(std::move(item));
        }
        for (const auto &tc : m.tool_calls) {
            openai_responses::InputItem item;
            item.type = "function_call";
            item.call_id = tc.id;
            item.name = tc.name;
            item.arguments = sanitize_tool_arguments(tc.arguments);
            input.push_back(std::move(item));
        }
    } else if (m.role == "tool") {
        // E1/E2: a tool result with media parts serializes its output as a
        // content item list (the reference's
        // _message_content_to_function_output_items).
        if (m.parts.empty()) {
            openai_responses::InputItem item;
            item.type = "function_call_output";
            item.content = m.content;
            item.call_id = m.tool_call_id;
            input.push_back(std::move(item));
        } else {
            openai_responses::InputItem item;
            item.type = "function_call_output";
            item.content = m.content;
            item.call_id = m.tool_call_id;
            item.parts = m.parts;
            input.push_back(std::move(item));
        }
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
    r.error_kind = to_unified_error_kind(raw.error_kind);
    r.error_status = raw.error_status;
    r.retry_after_seconds = raw.retry_after_seconds;
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
        // E1/E2: media parts -> content blocks (the text part replaces the
        // plain-string content; the parts carry the same backbone).
        if (!m.parts.empty()) {
            wm.parts = m.parts;
            wm.text = m.content;
        }
    } else if (m.role == "tool") {
        // The reference flattens tool-message content to text
        // (ToolMessageConversion == "extract_text"), so media parts never
        // reach an Anthropic tool_result block - the text backbone is sent.
        wm.role = "user";
        wm.tool_result_id = m.tool_call_id;
        wm.tool_result_content = m.content;
    } else if (m.role == "assistant") {
        wm.role = "assistant";
        wm.text = m.content;
        wm.thinking = m.thinking;
        wm.thinking_signature = m.thinking_signature;
        // E1/E2: parts carry the text blocks; `text` is skipped on the wire
        // to avoid doubling the backbone.
        if (!m.parts.empty()) {
            wm.parts = m.parts;
        }
        for (const auto &tc : m.tool_calls) {
            anthropic::ToolUse tu;
            tu.id = tc.id;
            tu.name = tc.name;
            // Sanitized like the other providers: without this, the strict
            // re-parse in anthropic_chat.cpp would silently drop a poisoned
            // argument to {} instead of keeping the repairable prefix.
            tu.input_json = sanitize_tool_arguments(tc.arguments);
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
    // The anthropic cache-creation input was parsed but dropped before; the
    // token ledger adds it back so the recorded usage matches the reference's
    // TokenUsage.input (input + cache_read + cache_creation).
    r.cache_creation_tokens = raw.cache_creation_input_tokens;
    r.total_tokens = raw.input_tokens + raw.output_tokens;
    r.error_kind = to_unified_error_kind(raw.error_kind);
    r.error_status = raw.error_status;
    r.retry_after_seconds = raw.retry_after_seconds;
    return r;
}

} // namespace detail

// ---------------------------------------------------------------------------
// E4: tool-call id normalization
// (kosong/contrib/chat_provider/common.py normalize_tool_call_ids)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kToolCallIdMaxLength = 64;
const char *const kEmptyToolCallId = "tool_call";

// _sanitize_tool_call_id: characters strict backends reject become '_', then
// the id is truncated to the 64-character budget.
kimix::string sanitize_tool_call_id(kimix::string_view id) {
    kimix::string out;
    out.reserve(id.size());
    for (const char c : id) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(safe ? c : '_');
    }
    if (out.size() > kToolCallIdMaxLength) {
        out.resize(kToolCallIdMaxLength);
    }
    return out;
}

// _make_unique_tool_call_id: `normalized` (possibly empty) truncated to the
// budget; on collision append "_2", "_3", ... re-truncating the base so the
// whole id stays within 64 characters.
kimix::string make_unique_tool_call_id(const kimix::string &normalized,
                                       kimix::unordered_set<kimix::string, kimix::string_hash> &used) {
    const kimix::string &base = normalized.empty() ? kimix::string(kEmptyToolCallId)
                                                   : normalized;
    kimix::string candidate = base.substr(0, kToolCallIdMaxLength);
    if (used.find(candidate) == used.end()) {
        return candidate;
    }
    for (int64_t index = 2;; ++index) {
        const kimix::string suffix = kimix::format("_{}", index);
        candidate = base.substr(0, kToolCallIdMaxLength - suffix.size()) + suffix;
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
}

} // namespace

kimix::vector<Message> normalize_tool_call_ids(const kimix::vector<Message> &history) {
    // First pass: collect every distinct raw id in order of appearance.
    // The C++ message model has no None tool_call_id: an EMPTY tool_call_id on
    // a tool-role message is repaired to "tool_call" (the reference's
    // _EMPTY_TOOL_CALL_ID path for an id of ""), while an empty id on any
    // other role means "no id" (Python None) and is left out entirely.
    const auto has_result_id = [](const Message &m) {
        return m.role == "tool" || !m.tool_call_id.empty();
    };
    kimix::vector<kimix::string> raw_ids;
    kimix::unordered_set<kimix::string, kimix::string_hash> seen;
    for (const Message &m : history) {
        for (const ToolCall &tc : m.tool_calls) {
            if (seen.find(tc.id) == seen.end()) {
                seen.insert(tc.id);
                raw_ids.push_back(tc.id);
            }
        }
        if (has_result_id(m) && seen.find(m.tool_call_id) == seen.end()) {
            seen.insert(m.tool_call_id);
            raw_ids.push_back(m.tool_call_id);
        }
    }
    if (raw_ids.empty()) {
        return history;
    }

    // Ids that already satisfy the contract keep their value (first mapping
    // pass), so only genuinely invalid ids are rewritten (second pass) - and
    // the valid ones still block their spelling in the `used` set.
    kimix::unordered_map<kimix::string, kimix::string, kimix::string_hash> mapped;
    kimix::unordered_set<kimix::string, kimix::string_hash> used;
    for (const kimix::string &raw : raw_ids) {
        const kimix::string normalized = sanitize_tool_call_id(raw);
        if (normalized == raw && !normalized.empty()) {
            mapped[raw] = normalized;
            used.insert(normalized);
        }
    }
    for (const kimix::string &raw : raw_ids) {
        if (mapped.find(raw) != mapped.end()) {
            continue;
        }
        kimix::string unique = make_unique_tool_call_id(sanitize_tool_call_id(raw), used);
        mapped[raw] = unique;
        used.insert(std::move(unique));
    }

    bool all_identity = true;
    for (const kimix::string &raw : raw_ids) {
        if (mapped[raw] != raw) {
            all_identity = false;
            break;
        }
    }
    if (all_identity) {
        return history;
    }

    kimix::vector<Message> out;
    out.reserve(history.size());
    for (const Message &m : history) {
        Message copy = m;
        for (ToolCall &tc : copy.tool_calls) {
            tc.id = mapped[tc.id];
        }
        if (has_result_id(m)) {
            copy.tool_call_id = mapped[copy.tool_call_id];
        }
        out.push_back(std::move(copy));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Provider wire seams (normalize + convert), shared with the tests
// ---------------------------------------------------------------------------

kimix::vector<openai::ChatMessage> openai_wire_messages(
    const kimix::vector<Message> &messages) {
    // normalize_tool_call_ids runs for every provider (the reference applies
    // it defensively in openai_legacy, openai_responses and anthropic alike).
    const kimix::vector<Message> normalized = normalize_tool_call_ids(messages);
    kimix::vector<openai::ChatMessage> wire_messages;
    wire_messages.reserve(normalized.size());
    for (const Message &m : normalized) {
        wire_messages.push_back(detail::to_openai_message(m));
    }
    return wire_messages;
}

kimix::vector<openai_responses::InputItem> responses_wire_input(
    const kimix::vector<Message> &messages) {
    const kimix::vector<Message> normalized = normalize_tool_call_ids(messages);
    kimix::vector<openai_responses::InputItem> input;
    for (const Message &m : normalized) {
        detail::append_responses_input(m, input);
    }
    return input;
}

AnthropicWireRequest anthropic_wire_request(const kimix::vector<Message> &messages) {
    AnthropicWireRequest req;
    // normalize_tool_call_ids first (anthropic.py generate()), then convert.
    // Per the Anthropic spec the tool_result blocks of one assistant turn must
    // live in a single user message, so consecutive tool-result-only user
    // messages are merged (anthropic.py generate() lines 384-401). The
    // reference does NOT drop orphan tool_use/tool_result pairs - a tool
    // message without an id becomes the text error block returned by
    // check_tool_call_id; here the empty id is repaired to "tool_call" by the
    // normalization pass instead (the C++ message model has no None id).
    const kimix::vector<Message> normalized = normalize_tool_call_ids(messages);
    for (const Message &m : normalized) {
        if (m.role == "system") {
            // Anthropic has no system role in the message list; system-role
            // content becomes the request-level `system` string (joined with
            // "\n" when multiple).
            if (!m.content.empty()) {
                if (!req.system.empty()) {
                    req.system += '\n';
                }
                req.system += m.content;
            }
            continue;
        }
        anthropic::ChatMessage wm = detail::to_anthropic_message(m);
        const bool tool_result_only =
            wm.role == "user" && (!wm.tool_results.empty() ||
                                  !wm.tool_result_id.empty());
        if (tool_result_only && !req.messages.empty()) {
            anthropic::ChatMessage &prev = req.messages.back();
            const bool prev_tool_result_only =
                prev.role == "user" && prev.text.empty() &&
                prev.tool_uses.empty() &&
                (!prev.tool_results.empty() || !prev.tool_result_id.empty());
            if (prev_tool_result_only) {
                // Merge into the previous user message.
                if (prev.tool_results.empty() && !prev.tool_result_id.empty()) {
                    prev.tool_results.push_back(
                        {std::move(prev.tool_result_id),
                         std::move(prev.tool_result_content)});
                    prev.tool_result_id.clear();
                    prev.tool_result_content.clear();
                }
                if (wm.tool_results.empty()) {
                    prev.tool_results.push_back({std::move(wm.tool_result_id),
                                                 std::move(wm.tool_result_content)});
                } else {
                    for (anthropic::ToolResult &tr : wm.tool_results) {
                        prev.tool_results.push_back(std::move(tr));
                    }
                }
                continue;
            }
        }
        req.messages.push_back(std::move(wm));
    }
    return req;
}

// ---------------------------------------------------------------------------
// OpenAIChatProvider - OpenAI Chat Completions backend
// ---------------------------------------------------------------------------
class OpenAIChatProvider : public ChatProvider {
public:
    explicit OpenAIChatProvider(kimix::shared_ptr<Config> config)
        : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_->model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk,
                      const AbortCheck *abort) const override {
        // E4: normalize_tool_call_ids + conversion (shared seam, unit-tested).
        kimix::vector<openai::ChatMessage> wire_messages =
            openai_wire_messages(messages);
        kimix::vector<openai::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_openai_tool(t));
        }

        openai::ChunkCallback wrapper;
        if (on_chunk) {
            EmptyPartFilter filtered(on_chunk); // E9: drop blank deltas
            wrapper = [on_chunk, filtered](const openai::ChatChunk &raw) mutable {
                filtered(detail::to_unified_chunk(raw));
                };
        }
        return detail::to_unified_result(
            openai::chat_completion_stream(*config_, wire_messages, wire_tools,
                                             wrapper, abort));
    }

private:
    // Shared with the LLM wrapper: the think-only escalation
    // (LLM::set_output_token_budget) mutates this object and the provider
    // reads its max_tokens from the very same instance.
    kimix::shared_ptr<Config> config_;
};

// ---------------------------------------------------------------------------
// ResponsesChatProvider - OpenAI Responses API backend
// ---------------------------------------------------------------------------
class ResponsesChatProvider : public ChatProvider {
public:
    explicit ResponsesChatProvider(kimix::shared_ptr<Config> config)
        : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_->model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk,
                      const AbortCheck *abort) const override {
        // E4: normalize_tool_call_ids + conversion (shared seam, unit-tested).
        kimix::vector<openai_responses::InputItem> input =
            responses_wire_input(messages);
        kimix::vector<openai_responses::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_responses_tool(t));
        }

        openai_responses::EventCallback wrapper;
        if (on_chunk) {
            EmptyPartFilter filtered(on_chunk); // E9: drop blank deltas
            wrapper = [on_chunk, filtered](const openai_responses::StreamEvent &ev) mutable {
                filtered(detail::to_unified_chunk(ev));
                };
        }
        return detail::to_unified_result(
            openai_responses::responses_completion_stream(*config_, input,
                                                            wire_tools, wrapper,
                                                            abort));
    }

private:
    // Shared with the LLM wrapper: the think-only escalation
    // (LLM::set_output_token_budget) mutates this object and the provider
    // reads its max_tokens from the very same instance.
    kimix::shared_ptr<Config> config_;
};

// ---------------------------------------------------------------------------
// AnthropicChatProvider - Anthropic Messages API backend
// ---------------------------------------------------------------------------
class AnthropicChatProvider : public ChatProvider {
public:
    explicit AnthropicChatProvider(kimix::shared_ptr<Config> config)
        : config_(std::move(config)) {}

    kimix::string model_name() const override { return config_->model; }

    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools,
                    const ChunkCallback &on_chunk,
                      const AbortCheck *abort) const override {
        // E4: normalize_tool_call_ids + conversion + the merge of consecutive
        // tool-result-only user messages (shared seam, unit-tested). System
        // messages become the request-level `system` string.
        const AnthropicWireRequest wire = anthropic_wire_request(messages);
        const kimix::string &system = wire.system;
        const kimix::vector<anthropic::ChatMessage> &wire_messages = wire.messages;
        kimix::vector<anthropic::Tool> wire_tools;
        wire_tools.reserve(tools.size());
        for (const auto &t : tools) {
            wire_tools.push_back(detail::to_anthropic_tool(t));
        }

        anthropic::EventCallback wrapper;
        if (on_chunk) {
            EmptyPartFilter filtered(on_chunk); // E9: drop blank deltas
            wrapper = [on_chunk, filtered](const anthropic::StreamEvent &ev) mutable {
                filtered(detail::to_unified_chunk(ev));
                };
        }
        return detail::to_unified_result(
              anthropic::chat_completion_stream(*config_, system, wire_messages,
                                                wire_tools, wrapper, abort));
    }

private:
    // Shared with the LLM wrapper: the think-only escalation
    // (LLM::set_output_token_budget) mutates this object and the provider
    // reads its max_tokens from the very same instance.
    kimix::shared_ptr<Config> config_;
};

// ---------------------------------------------------------------------------
// LLM
// ---------------------------------------------------------------------------
LLM::LLM(kimix::unique_ptr<ChatProvider> provider, Config config)
    : provider_(std::move(provider)),
      config_(kimix::shared_ptr<Config>(new Config(std::move(config)))) {}

LLM::LLM(kimix::unique_ptr<ChatProvider> provider,
         kimix::shared_ptr<Config> config)
    : provider_(std::move(provider)), config_(std::move(config)) {}

kimix::string LLM::model_name() const { return provider_->model_name(); }

const Config &LLM::config() const { return *config_; }

int32_t LLM::max_context_size() const { return config_->max_context_size; }

ChatResult LLM::chat(const kimix::vector<Message> &messages,
                     const kimix::vector<Tool> &tools,
                     const ChunkCallback &on_chunk,
                     const AbortCheck *abort) const {
    // Capability pre-flight (soul/message.py check_message -> LLMNotSupported):
    // refuse BEFORE the request is sent when the history carries parts the
    // model cannot consume, instead of letting the provider 400.
    const ModelCapabilities needed = message_required_capabilities(messages);
    ModelCapabilities missing;
    missing.image_in = needed.image_in && !config_->capabilities.image_in;
    missing.video_in = needed.video_in && !config_->capabilities.video_in;
    missing.thinking = needed.thinking && !config_->capabilities.thinking;
    missing.always_thinking = needed.always_thinking &&
                              !config_->capabilities.always_thinking;
    if (missing.image_in || missing.video_in || missing.thinking ||
        missing.always_thinking) {
        ChatResult refused;
        refused.ok = false;
        refused.error_kind = ChatErrorKind::not_supported;
        refused.error = capability_error_text(model_name(), missing);
        return refused;
    }
    ChatResult result = provider_->chat(messages, tools, on_chunk, abort);
    if (abort != nullptr && abort->aborted() && !result.ok) {
        // G8: the caller cancelled mid-stream - classify the failure as an
        // abort (never retryable, never a session restart) no matter what
        // transport error the provider observed after cancelling the read.
        result.error_kind = ChatErrorKind::aborted;
        if (result.error.empty()) {
            result.error = "request aborted";
        }
    }
    // Tool-call arguments are the JSON the backend produced; models often
    // hallucinate trailing commas, truncated objects or unquoted keys, so
    // repair each argument before the caller parses it. Streaming chunk
    // deltas are NOT repaired here: they are partial fragments that only make
    // sense once accumulated (which the provider already did for `result`).
    for (auto &tc : result.tool_calls) {
        tc.arguments = detail::repair_backend_json(std::move(tc.arguments));
        // Guarantee the arguments are strict-valid JSON before the caller
        // persists them into history — lenient repair alone can leave
        // gateway-poisoned strings (e.g. duplicated chunks merged into
        // "{}{}") that a strict backend rejects with a 400 on the next turn.
        tc.arguments = sanitize_tool_arguments(tc.arguments);
    }
    // Safety net (the providers already flag this): a successful result that
    // carries nothing at all means the backend's 200 body never parsed
    // (garbage JSON, HTML error page, empty stream). Surface it as an error
    // instead of letting the caller treat the turn as a successful empty
    // reply.
    if (result.ok && result.content.empty() && result.reasoning.empty()
        && result.tool_calls.empty() && result.signature.empty()) {
        result.ok = false;
        result.error_kind = ChatErrorKind::empty_response;
        result.error = "backend returned an empty response (invalid JSON or "
                       "empty stream)";
    }
    return result;
}

void LLM::set_output_token_budget(int32_t tokens) const {
    config_->max_tokens = tokens;
}

// ---------------------------------------------------------------------------
// E1/E2: content parts (kosong Message.content) - the sync helpers
// ---------------------------------------------------------------------------

kimix::vector<ContentPart> message_parts(const Message &msg) {
    if (msg.parts.empty()) {
        // Plain-text message: the single implicit TextPart backbone
        // (Message._coerce_none_content coerces a str content to one
        // TextPart).
        kimix::vector<ContentPart> parts;
        if (!msg.content.empty()) {
            ContentPart part;
            part.kind = ContentPart::Kind::text;
            part.text = msg.content;
            parts.push_back(std::move(part));
        }
        return parts;
    }
    return msg.parts;
}

void message_set_parts(Message &msg, kimix::vector<ContentPart> parts) {
    // The text backbone: every text part joined in order (Message.extract_text
    // with the default sep=""). Think parts mirror into the thinking
    // round-trip field when they are not already carried there.
    kimix::string text;
    kimix::string thinking;
    kimix::string signature;
    for (const ContentPart &part : parts) {
        switch (part.kind) {
        case ContentPart::Kind::text:
            text += part.text;
            break;
        case ContentPart::Kind::think:
            thinking += part.text;
            break;
        default:
            break; // media parts carry no text
        }
    }
    msg.parts = std::move(parts);
    msg.content = std::move(text);
    if (!thinking.empty()) {
        msg.thinking = std::move(thinking);
    }
    if (!signature.empty()) {
        msg.thinking_signature = std::move(signature);
    }
}

ModelCapabilities
message_required_capabilities(const kimix::vector<Message> &messages) noexcept {
    // soul/message.py check_message: an ImageURLPart requires `image_in`, a
    // VideoURLPart `video_in`, and a ThinkPart (the C++ message model's
    // assistant `thinking` / `thinking_signature` round-trip fields)
    // `thinking`. Start from a zeroed set (the struct default is
    // thinking-capable).
    ModelCapabilities needed{false, false, false, false};
    for (const Message &m : messages) {
        if (!m.thinking.empty() || !m.thinking_signature.empty()) {
            needed.thinking = true;
        }
        for (const ContentPart &part : m.parts) {
            if (part.kind == ContentPart::Kind::image_url) {
                needed.image_in = true;
            } else if (part.kind == ContentPart::Kind::video_url) {
                needed.video_in = true;
            }
        }
    }
    return needed;
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
namespace {

kimix::unique_ptr<ChatProvider> make_provider(const kimix::shared_ptr<Config> &shared) {
    if (shared->type == "openai" || shared->type == "openai_legacy") {
        return kimix::unique_ptr<ChatProvider>(new OpenAIChatProvider(shared));
    }
    if (shared->type == "openai_responses") {
        return kimix::unique_ptr<ChatProvider>(new ResponsesChatProvider(shared));
    }
    if (shared->type == "anthropic") {
        return kimix::unique_ptr<ChatProvider>(new AnthropicChatProvider(shared));
    }
    return nullptr;
}

} // namespace

kimix::string create_llm_error_text(CreateLlmError kind,
                                    kimix::string_view provider_type) {
    // kimi_cli/soul/__init__.py LLMNotSet: super().__init__("LLM not set").
    // That is the exact user-visible wording the reference surfaces when the
    // LLM could not be created (kimi_cli/llm.py create_llm logs
    // "Cannot create LLM: missing base_url or model (provider_type=...)" and
    // returns None; the enum alone tells the two failure kinds apart).
    kimix::string out = "LLM not set";
    if (kind == CreateLlmError::unknown_provider_type) {
        out += " (unknown provider type '";
        out.append(provider_type.data(), provider_type.size());
        out += "')";
    }
    return out;
}

CreateLlmError create_llm(const Config &config, kimix::unique_ptr<LLM> &out,
                          kimix::string *error) {
    out.reset();
    // One shared Config: the provider reads its request parameters (model,
    // max_tokens, capabilities) from the same instance LLM::
    // set_output_token_budget mutates for the think-only escalation. The
    // KIMI_* env fallback chain applies BEFORE the model/url validation (the
    // reference augments the config first and create_llm then refuses when
    // base_url or model is still missing), so an env-only setup works for a
    // programmatically built Config too; it is a no-op once fields are filled.
    kimix::shared_ptr<Config> shared(new Config(config));
    apply_env_overrides(*shared);
    if (shared->model.empty() || shared->url.empty()) {
        if (error != nullptr) {
            *error = create_llm_error_text(CreateLlmError::llm_not_set, shared->type);
        }
        return CreateLlmError::llm_not_set;
    }
    kimix::unique_ptr<ChatProvider> provider = make_provider(shared);
    if (!provider) {
        if (error != nullptr) {
            *error = create_llm_error_text(CreateLlmError::unknown_provider_type,
                                           shared->type);
        }
        return CreateLlmError::unknown_provider_type;
    }
    out.reset(new LLM(std::move(provider), std::move(shared)));
    return CreateLlmError::none;
}

kimix::unique_ptr<LLM> create_llm(Config config) {
    kimix::unique_ptr<LLM> llm;
    kimix::string unused_error;
    create_llm(config, llm, &unused_error);
    return llm;
}

kimix::unique_ptr<LLM> create_llm_from_file(const kimix::string &path) {
    Config cfg;
    if (!load_config(path, cfg)) {
        return nullptr;
    }
    return create_llm(std::move(cfg));
}

} // namespace kimix::llm
