// anthropic_chat.cpp - Anthropic Messages API streaming workflow.
//
// HTTPS transport uses WinHTTP (Windows-native, system proxy + cert store) so
// the target has no OpenSSL dependency. SSE bytes are fed into
// anthropic/stream_parser.h exactly like the OpenAI demo feeds its parser.

#include "anthropic/anthropic_chat.h"

#ifndef _WIN32
#error "anthropic_chat_demo currently requires Windows (WinHTTP transport)"
#endif

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "yyjson.h"

namespace anthropic {

namespace {

// Map an effort level to a legacy thinking budget (mirrors anthropic.py).
int thinking_budget(const std::string &effort) {
    if (effort == "low") {
        return 1024;
    }
    if (effort == "medium") {
        return 4096;
    }
    if (effort == "xhigh") {
        return 64'000;
    }
    if (effort == "max") {
        return 128'000;
    }
    return 32'000; // high and default
}

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

std::string last_error_string(const char *what, DWORD err) {
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf), nullptr);
    return std::string(what) + " failed (error " + std::to_string(err) + "): "
           + std::string(buf);
}

} // namespace

bool load_config(const std::string &path, AnthropicConfig &cfg) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        return false;
    }
    std::vector<char> buf((size_t)size);
    size_t rd = std::fread(buf.data(), 1, (size_t)size, fp);
    std::fclose(fp);
    if (rd == 0) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(buf.data(), rd, 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        auto get_str = [&](const char *key) -> std::string {
            yyjson_val *v = yyjson_obj_get(root, key);
            if (yyjson_is_str(v)) {
                return std::string(yyjson_get_str(v), yyjson_get_len(v));
            }
            return {};
        };
        cfg.model = get_str("model");
        cfg.url = get_str("url");
        cfg.api_key = get_str("api_key");
        cfg.type = get_str("type");
        cfg.thinking_effort = get_str("thinking_effort");
        if (cfg.thinking_effort.empty()) {
            cfg.thinking_effort = "high";
        }
        yyjson_val *v = yyjson_obj_get(root, "max_tokens");
        if (yyjson_is_int(v)) {
            cfg.max_tokens = (int)yyjson_get_int(v);
        }
        v = yyjson_obj_get(root, "max_context_size");
        if (yyjson_is_int(v)) {
            cfg.max_context_size = (int)yyjson_get_int(v);
        }
        ok = !cfg.model.empty() && !cfg.url.empty();
    }
    yyjson_doc_free(doc);
    return ok;
}

std::string build_messages_body(const AnthropicConfig &cfg,
                                const std::string &system,
                                const std::vector<ChatMessage> &messages,
                                const std::vector<Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", cfg.model.c_str());
    yyjson_mut_obj_add_int(doc, root, "max_tokens", cfg.max_tokens);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    if (!system.empty()) {
        yyjson_mut_obj_add_str(doc, root, "system", system.c_str());
    }

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, obj, "role", m.role.c_str());
        if (m.role == "user" && !m.tool_result_id.empty()) {
            // User tool_result content block.
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "tool_result");
            yyjson_mut_obj_add_str(doc, block, "tool_use_id", m.tool_result_id.c_str());
            yyjson_mut_obj_add_str(doc, block, "content", m.tool_result_content.c_str());
        } else if (m.role == "assistant") {
            // Assistant content is a block list: thinking (required by some
            // backends, e.g. DeepSeek, when thinking mode is on), then text,
            // then tool_use blocks.
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            if (!m.thinking.empty() || !m.thinking_signature.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "thinking");
                yyjson_mut_obj_add_str(doc, block, "thinking", m.thinking.c_str());
                if (!m.thinking_signature.empty()) {
                    yyjson_mut_obj_add_str(doc, block, "signature", m.thinking_signature.c_str());
                }
            }
            if (!m.text.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "text");
                yyjson_mut_obj_add_str(doc, block, "text", m.text.c_str());
            }
            for (const auto &tu : m.tool_uses) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "tool_use");
                yyjson_mut_obj_add_str(doc, block, "id", tu.id.c_str());
                yyjson_mut_obj_add_str(doc, block, "name", tu.name.c_str());
                yyjson_doc *idoc = yyjson_read(tu.input_json.data(), tu.input_json.size(), 0);
                yyjson_mut_val *input = nullptr;
                if (idoc) {
                    input = yyjson_val_mut_copy(doc, yyjson_doc_get_root(idoc));
                    yyjson_doc_free(idoc);
                }
                if (!input) {
                    input = yyjson_mut_obj(doc);
                }
                yyjson_mut_obj_add_val(doc, block, "input", input);
            }
        } else {
            // Plain user text.
            yyjson_mut_obj_add_str(doc, obj, "content", m.text.c_str());
        }
        yyjson_mut_arr_append(msg_arr, obj);
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            yyjson_mut_obj_add_str(doc, tool_obj, "name", t.name.c_str());
            yyjson_mut_obj_add_str(doc, tool_obj, "description", t.description.c_str());
            yyjson_doc *sdoc = yyjson_read(t.input_schema_json.data(), t.input_schema_json.size(), 0);
            yyjson_mut_val *schema = nullptr;
            if (sdoc) {
                schema = yyjson_val_mut_copy(doc, yyjson_doc_get_root(sdoc));
                yyjson_doc_free(sdoc);
            }
            if (!schema) {
                schema = yyjson_mut_obj(doc);
            }
            yyjson_mut_obj_add_val(doc, tool_obj, "input_schema", schema);
        }
    }

    // Legacy budget-based thinking (maps thinking_effort to a token budget,
    // mirroring anthropic.py's budgets table).
    yyjson_mut_val *thinking = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
    yyjson_mut_obj_add_str(doc, thinking, "type", "enabled");
    yyjson_mut_obj_add_int(doc, thinking, "budget_tokens", thinking_budget(cfg.thinking_effort));

    char *json = yyjson_mut_write(doc, 0, nullptr);
    std::string body = json ? std::string(json) : std::string();
    std::free(json);
    yyjson_mut_doc_free(doc);
    return body;
}

ChatResult chat_completion_stream(const AnthropicConfig &cfg,
                                  const std::string &system,
                                  const std::vector<ChatMessage> &messages,
                                  const std::vector<Tool> &tools,
                                  const EventCallback &on_event) {
    ChatResult result;
    const std::string body = build_messages_body(cfg, system, messages, tools);
    if (body.empty()) {
        result.error = "failed to build request body";
        return result;
    }

    // Split the config URL into scheme / host[:port] / path prefix.
    std::string rest = cfg.url;
    std::string scheme = "http";
    if (rest.rfind("https://", 0) == 0) {
        scheme = "https";
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    std::string host = rest;
    std::string path_prefix = "/";
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path_prefix = rest.substr(slash);
    }
    int port = scheme == "https" ? 443 : 80;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }
    std::string path = path_prefix;
    if (path.empty() || path.back() != '/') {
        path += '/';
    }
    path += "v1/messages";

    const std::wstring host_w = utf8_to_wide(host);
    const std::wstring path_w = utf8_to_wide(path);
    const std::wstring headers_w = L"Content-Type: application/json\r\n"
                                   L"Accept: text/event-stream\r\n"
                                   L"x-api-key: " + utf8_to_wide(cfg.api_key)
                                   + L"\r\n"
                                     L"anthropic-version: 2023-06-01\r\n";
    const DWORD secure_flag = scheme == "https" ? WINHTTP_FLAG_SECURE : 0;

    // Transient failures (403/408/429/5xx, dropped connections) are retried a
    // couple of times with a short pause.
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // Reset accumulators for this attempt.
        result.text.clear();
        result.thinking.clear();
        result.signature.clear();
        result.tool_uses.clear();
        result.input_tokens = 0;
        result.output_tokens = 0;
        result.cache_creation_input_tokens = 0;
        result.cache_read_input_tokens = 0;

        StreamParser parser;
        std::vector<ToolUse> acc_tool_uses;
        // Anthropic indexes ALL content blocks (thinking, text, tool_use)
        // sequentially, so a tool_use block may sit at any index. Map each
        // tool_use block index to its position in acc_tool_uses.
        std::map<int, size_t> tool_block_pos;
        std::string thinking, signature;

        const auto consume = [&](const StreamEvent &ev) {
            if (on_event) {
                on_event(ev);
            }
            if (ev.type == "message_start") {
                if (ev.usage.has) {
                    result.input_tokens = ev.usage.input_tokens;
                    result.cache_creation_input_tokens = ev.usage.cache_creation_input_tokens;
                    result.cache_read_input_tokens = ev.usage.cache_read_input_tokens;
                }
            } else if (ev.type == "content_block_start") {
                if (ev.block_type == "tool_use") {
                    size_t pos = acc_tool_uses.size();
                    tool_block_pos[ev.index] = pos;
                    acc_tool_uses.push_back({ev.block_id, ev.block_name, ev.text});
                }
            } else if (ev.type == "content_block_delta") {
                if (ev.delta_type == "text_delta") {
                    result.text += ev.text;
                } else if (ev.delta_type == "thinking_delta") {
                    thinking += ev.text;
                } else if (ev.delta_type == "signature_delta") {
                    signature += ev.signature;
                } else if (ev.delta_type == "input_json_delta") {
                    auto it = tool_block_pos.find(ev.index);
                    if (it != tool_block_pos.end()) {
                        acc_tool_uses[it->second].input_json += ev.text;
                    } else if (!acc_tool_uses.empty()) {
                        acc_tool_uses.back().input_json += ev.text;
                    }
                }
            } else if (ev.type == "message_delta") {
                if (!ev.stop_reason.empty()) {
                    result.stop_reason = ev.stop_reason;
                }
                if (ev.usage.has) {
                    result.output_tokens = ev.usage.output_tokens;
                    if (ev.usage.input_tokens > 0) {
                        result.input_tokens = ev.usage.input_tokens;
                    }
                    result.cache_creation_input_tokens = ev.usage.cache_creation_input_tokens;
                    result.cache_read_input_tokens = ev.usage.cache_read_input_tokens;
                }
            }
        };

        HINTERNET hSession = WinHttpOpen(L"KimixBase/1.0",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            result.error = last_error_string("WinHttpOpen", GetLastError());
            return result;
        }
        WinHttpSetTimeouts(hSession, 30'000, 30'000, 300'000, 30'000);

        HINTERNET hConnect = WinHttpConnect(hSession, host_w.c_str(),
                                            (INTERNET_PORT)port, 0);
        if (!hConnect) {
            result.error = last_error_string("WinHttpConnect", GetLastError());
            WinHttpCloseHandle(hSession);
            return result;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path_w.c_str(),
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                secure_flag);
        if (!hRequest) {
            result.error = last_error_string("WinHttpOpenRequest", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        BOOL ok = WinHttpSendRequest(hRequest, headers_w.c_str(), (DWORD)-1,
                                     (LPVOID)body.data(), (DWORD)body.size(),
                                     (DWORD)body.size(), 0);
        if (ok) {
            ok = WinHttpReceiveResponse(hRequest, nullptr);
        }
        if (!ok) {
            result.error = last_error_string("WinHttpSendRequest/ReceiveResponse",
                                             GetLastError());
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX);

        char buf[8192];
        DWORD bytes_read = 0;
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytes_read)
               && bytes_read > 0) {
            for (const auto &ev : parser.feed(buf, bytes_read)) {
                consume(ev);
            }
            bytes_read = 0;
        }
        for (const auto &ev : parser.finish()) {
            consume(ev);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        const bool retriable = status == 0 || status == 403 || status == 408
                               || status == 429 || status >= 500;
        if (retriable && attempt < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
            continue;
        }
        if (status == 0) {
            result.error = "no HTTP status received";
            return result;
        }
        if (status != 200) {
            result.error = "http status " + std::to_string(status);
            return result;
        }

        result.thinking = std::move(thinking);
        result.signature = std::move(signature);
        result.tool_uses = std::move(acc_tool_uses);
        result.ok = true;
        return result;
    }

    result.error = "exhausted retries";
    return result;
}

} // namespace anthropic
