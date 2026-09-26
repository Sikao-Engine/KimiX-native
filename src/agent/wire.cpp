// agent/wire.cpp - WireWriter implementation (see wire.h).

#include "agent/wire.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <core/clock.h>
#include <random>

#include "llm/common.h"       // add_json_fragment
#include "llm/yyjson_alc.h"

namespace kimix::agent {

namespace {

// Record line: {"timestamp": <float>, "message": {"type": <name>, "payload":
// {...}}}\n (wire/file.py _dump_line + wire/types.py WireMessageEnvelope).
// Payload key order follows the pydantic field declaration order of
// wire/types.py; absent Optionals serialize as explicit nulls (model_dump
// without exclude_none).
void add_str_or_null(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                     kimix::string_view key, kimix::string_view value) {
    if (value.empty()) {
        yyjson_mut_obj_add_null(doc, obj, key.data());
    } else {
        yyjson_mut_obj_add_strn(doc, obj, key.data(), value.data(), value.size());
    }
}

void add_int_or_null(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                     kimix::string_view key, int64_t value) {
    if (value < 0) {
        yyjson_mut_obj_add_null(doc, obj, key.data());
    } else {
        yyjson_mut_obj_add_sint(doc, obj, key.data(), value);
    }
}

double default_now_seconds() {
    return kimix::Clock::now_ms() / 1000.0;
}

kimix::string default_compaction_id() {
    // uuid.uuid4().hex analogue: 16 random bytes as 32 lowercase hex chars.
    std::random_device rd;
    kimix::string out;
    out.reserve(32);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        const unsigned b = static_cast<unsigned>(rd()) & 0xFFu;
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

} // namespace

kimix::string new_compaction_id() { return default_compaction_id(); }

bool WireWriter::open(const kimix::string &path, kimix::string &error) {
    close();
    error.clear();
    // Create the parent directories like WireFile.open (wire/file.py:133-134).
    const kimix::filesystem::path fs_path{path};
    const auto parent = fs_path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        kimix::filesystem::create_directories(parent, ec);
        if (ec) {
            error = kimix::string("cannot create ") + kimix::to_string(parent) +
                    ": " + ec.message().c_str();
            return false;
        }
    }
    // Append mode; the header is written only when the file is missing or
    // empty so reopening a session preserves its existing stream.
    const bool needs_header = [&]() {
        std::error_code size_ec;
        const auto size = kimix::filesystem::file_size(fs_path, size_ec);
        return size_ec || size == 0;
    }();
    errno = 0;
    std::FILE *f = std::fopen(path.c_str(), "ab");
    if (f == nullptr) {
        error = "cannot open " + path + " for appending";
        return false;
    }
    if (needs_header) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
        if (doc == nullptr) {
            std::fclose(f);
            error = "cannot allocate the wire header";
            return false;
        }
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "type", "metadata");
        yyjson_mut_obj_add_str(doc, root, "protocol_version", kProtocolVersion);
        const char *header = yyjson_mut_write(doc, 0, nullptr);
        if (header == nullptr) {
            yyjson_mut_doc_free(doc);
            std::fclose(f);
            error = "cannot serialize the wire header";
            return false;
        }
        std::fwrite(header, 1, std::strlen(header), f);
        std::fputc('\n', f);
        free(const_cast<char *>(header));
        yyjson_mut_doc_free(doc);
    }
    std::fflush(f);
    {
        std::lock_guard<std::mutex> g(_mutex);
        _file = f;
        _path = path;
    }
    return true;
}

void WireWriter::close() noexcept {
    std::FILE *f = nullptr;
    {
        std::lock_guard<std::mutex> g(_mutex);
        f = _file;
        _file = nullptr;
    }
    if (f != nullptr) {
        std::fflush(f);
        std::fclose(f);
    }
}

double WireWriter::now_seconds() const {
    return now ? now() : default_now_seconds();
}

kimix::string WireWriter::new_compaction_id() const {
    return compaction_id_gen ? compaction_id_gen() : default_compaction_id();
}

void WireWriter::emit(
    kimix::string_view type,
    const kimix::function<void(yyjson_mut_doc *, yyjson_mut_val *)>
        &build_payload) {
    std::lock_guard<std::mutex> g(_mutex);
    if (_file == nullptr) {
        return;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_real(doc, root, "timestamp", now_seconds());
    yyjson_mut_val *message = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strn(doc, message, "type", type.data(), type.size());
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    if (build_payload) {
        build_payload(doc, payload);
    }
    yyjson_mut_obj_add_val(doc, message, "payload", payload);
    yyjson_mut_obj_add_val(doc, root, "message", message);
    const char *line = yyjson_mut_write(doc, 0, nullptr);
    if (line != nullptr) {
        // One record per line, a single write per record (atomic append).
        std::fwrite(line, 1, std::strlen(line), _file);
        std::fputc('\n', _file);
        std::fflush(_file);
        free(const_cast<char *>(line));
    }
    yyjson_mut_doc_free(doc);
}

void WireWriter::wire_turn_begin(kimix::string_view user_input) {
    emit("TurnBegin", [user_input](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
        yyjson_mut_obj_add_strn(doc, payload, "user_input", user_input.data(),
                                user_input.size());
    });
}

void WireWriter::wire_turn_end() {
    emit("TurnEnd", nullptr);
}

void WireWriter::wire_step_begin(int32_t n) {
    emit("StepBegin", [n](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
        yyjson_mut_obj_add_sint(doc, payload, "n", n);
    });
}

void WireWriter::wire_step_interrupted() {
    emit("StepInterrupted", nullptr);
}

void WireWriter::wire_steer_input(kimix::string_view user_input) {
    emit("SteerInput", [user_input](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
        yyjson_mut_obj_add_strn(doc, payload, "user_input", user_input.data(),
                                user_input.size());
    });
}

void WireWriter::wire_step_retry(int32_t n, int32_t next_attempt,
                                 int32_t max_attempts, double wait_s,
                                 kimix::string_view error_type,
                                 int32_t status_code) {
    emit("StepRetry",
         [n, next_attempt, max_attempts, wait_s, error_type,
          status_code](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_sint(doc, payload, "n", n);
             yyjson_mut_obj_add_sint(doc, payload, "next_attempt", next_attempt);
             yyjson_mut_obj_add_sint(doc, payload, "max_attempts", max_attempts);
             yyjson_mut_obj_add_real(doc, payload, "wait_s", wait_s);
             yyjson_mut_obj_add_strn(doc, payload, "error_type",
                                     error_type.data(), error_type.size());
             add_int_or_null(doc, payload, "status_code",
                             status_code > 0 ? status_code : -1);
         });
}

void WireWriter::wire_status_update(double context_usage, int64_t context_tokens,
                                    int64_t max_context_tokens, int64_t input_other,
                                    int64_t output, int64_t input_cache_read,
                                    int64_t input_cache_creation) {
    emit("StatusUpdate",
         [context_usage, context_tokens, max_context_tokens, input_other, output,
          input_cache_read,
          input_cache_creation](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_real(doc, payload, "context_usage", context_usage);
             add_int_or_null(doc, payload, "context_tokens", context_tokens);
             add_int_or_null(doc, payload, "max_context_tokens",
                             max_context_tokens);
             // kosong TokenUsage field order (chat_provider/__init__.py:96-106);
             // a missing provider usage serializes as a null token_usage.
             if (input_other < 0 && output < 0 && input_cache_read < 0 &&
                 input_cache_creation < 0) {
                 yyjson_mut_obj_add_null(doc, payload, "token_usage");
             } else {
                 yyjson_mut_val *usage = yyjson_mut_obj(doc);
                 add_int_or_null(doc, usage, "input_other", input_other);
                 add_int_or_null(doc, usage, "output", output);
                 add_int_or_null(doc, usage, "input_cache_read",
                                 input_cache_read);
                 add_int_or_null(doc, usage, "input_cache_creation",
                                 input_cache_creation);
                 yyjson_mut_obj_add_val(doc, payload, "token_usage", usage);
             }
             yyjson_mut_obj_add_null(doc, payload, "message_id");
             yyjson_mut_obj_add_null(doc, payload, "mcp_status");
         });
}

void WireWriter::wire_compaction_begin(kimix::string_view compaction_id,
                                       kimix::string_view trigger) {
    emit("CompactionBegin",
         [compaction_id, trigger](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             add_str_or_null(doc, payload, "compaction_id", compaction_id);
             add_str_or_null(doc, payload, "trigger", trigger);
             yyjson_mut_obj_add_null(doc, payload, "shadowed_tokens");
         });
}

void WireWriter::wire_compaction_end(kimix::string_view compaction_id,
                                     kimix::string_view trigger,
                                     int64_t shadowed_tokens,
                                     int64_t estimated_token_count,
                                     kimix::string_view error) {
    emit("CompactionEnd",
         [compaction_id, trigger, shadowed_tokens, estimated_token_count,
          error](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             add_str_or_null(doc, payload, "compaction_id", compaction_id);
             add_str_or_null(doc, payload, "trigger", trigger);
             add_int_or_null(doc, payload, "shadowed_tokens", shadowed_tokens);
             add_int_or_null(doc, payload, "estimated_token_count",
                             estimated_token_count);
             add_str_or_null(doc, payload, "error", error);
         });
}

void WireWriter::wire_llm_request(const llm_request_record &record) {
    // The G10 request-trace record (soul/llm_request_recorder.py), field order
    // per wire/types.py LLMRequest; absent Optionals serialize as null.
    emit("LLMRequest",
         [&record](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             add_str_or_null(doc, payload, "kind", record.kind);
             add_str_or_null(doc, payload, "provider", record.provider);
             add_str_or_null(doc, payload, "model", record.model);
             add_str_or_null(doc, payload, "thinking_effort", record.thinking_effort);
             if (record.has_temperature) {
                 yyjson_mut_obj_add_real(doc, payload, "temperature",
                                         record.temperature);
             } else {
                 yyjson_mut_obj_add_null(doc, payload, "temperature");
             }
             if (record.has_top_p) {
                 yyjson_mut_obj_add_real(doc, payload, "top_p", record.top_p);
             } else {
                 yyjson_mut_obj_add_null(doc, payload, "top_p");
             }
             add_int_or_null(doc, payload, "max_tokens", record.max_tokens);
             add_str_or_null(doc, payload, "system_prompt_hash",
                             record.system_prompt_hash);
             add_str_or_null(doc, payload, "system_prompt", record.system_prompt);
             add_str_or_null(doc, payload, "tools_hash", record.tools_hash);
             yyjson_mut_obj_add_sint(doc, payload, "message_count",
                                     record.message_count);
             add_int_or_null(doc, payload, "turn_step", record.turn_step);
             yyjson_mut_obj_add_sint(doc, payload, "attempt", record.attempt);
             add_int_or_null(doc, payload, "dropped_count", record.dropped_count);
         });
}

void WireWriter::wire_llm_tools_snapshot(
    kimix::string_view hash, const kimix::vector<kimix::llm::Tool> &tools) {
    // LLMToolsSnapshot {hash, tools: [{name, description, parameters}]}; the
    // parameters schema embeds as parsed JSON (an unparseable schema degrades
    // to {} so the record always stays valid).
    emit("LLMToolsSnapshot",
         [&hash, &tools](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_strn(doc, payload, "hash", hash.data(), hash.size());
             yyjson_mut_val *arr = yyjson_mut_arr(doc);
             for (const kimix::llm::Tool &tool : tools) {
                 yyjson_mut_val *entry = yyjson_mut_obj(doc);
                 yyjson_mut_obj_add_strn(doc, entry, "name", tool.name.data(),
                                         tool.name.size());
                 yyjson_mut_obj_add_strn(doc, entry, "description",
                                         tool.description.data(),
                                         tool.description.size());
                 if (!kimix::llm::add_json_fragment(doc, entry, "parameters",
                                        tool.parameters_json)) {
                     yyjson_mut_obj_add_val(doc, entry, "parameters",
                                            yyjson_mut_obj(doc));
                 }
                 yyjson_mut_arr_add_val(arr, entry);
             }
             yyjson_mut_obj_add_val(doc, payload, "tools", arr);
         });
}

void WireWriter::wire_mcp_tools_discovered(
    kimix::string_view server_name, kimix::string_view hash,
    const kimix::vector<kimix::llm::Tool> &tools,
    const kimix::vector<kimix::string> &enabled_names,
    const kimix::vector<kimix::string> &collisions) {
    // MCPToolsDiscovered {server_name, hash, tools, enabled_names, collisions}.
    emit("MCPToolsDiscovered",
         [&server_name, &hash, &tools, &enabled_names,
          &collisions](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_strn(doc, payload, "server_name",
                                     server_name.data(), server_name.size());
             yyjson_mut_obj_add_strn(doc, payload, "hash", hash.data(), hash.size());
             yyjson_mut_val *arr = yyjson_mut_arr(doc);
             for (const kimix::llm::Tool &tool : tools) {
                 yyjson_mut_val *entry = yyjson_mut_obj(doc);
                 yyjson_mut_obj_add_strn(doc, entry, "name", tool.name.data(),
                                         tool.name.size());
                 yyjson_mut_obj_add_strn(doc, entry, "description",
                                         tool.description.data(),
                                         tool.description.size());
                 if (!kimix::llm::add_json_fragment(doc, entry, "parameters",
                                        tool.parameters_json)) {
                     yyjson_mut_obj_add_val(doc, entry, "parameters",
                                            yyjson_mut_obj(doc));
                 }
                 yyjson_mut_arr_add_val(arr, entry);
             }
             yyjson_mut_obj_add_val(doc, payload, "tools", arr);
             yyjson_mut_val *enabled = yyjson_mut_arr(doc);
             for (const kimix::string &name : enabled_names) {
                 yyjson_mut_arr_add_strn(doc, enabled, name.data(), name.size());
             }
             yyjson_mut_obj_add_val(doc, payload, "enabled_names", enabled);
             yyjson_mut_val *collide = yyjson_mut_arr(doc);
             for (const kimix::string &name : collisions) {
                 yyjson_mut_arr_add_strn(doc, collide, name.data(), name.size());
             }
             yyjson_mut_obj_add_val(doc, payload, "collisions", collide);
         });
}

void WireWriter::wire_approval_request(kimix::string_view id,
                                       kimix::string_view tool_call_id,
                                       kimix::string_view sender,
                                       kimix::string_view action,
                                       kimix::string_view description) {
    // ApprovalRequest (approval_runtime/runtime.py _publish_wire_request);
    // payload order per wire/types.py. The synchronous foreground port always
    // reports source_kind "foreground_turn" with no agent/subagent fields, and
    // an empty display list (the CLI prompt renders `description` itself).
    emit("ApprovalRequest",
         [id, tool_call_id, sender, action,
          description](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_strn(doc, payload, "id", id.data(), id.size());
             yyjson_mut_obj_add_strn(doc, payload, "tool_call_id", tool_call_id.data(),
                                     tool_call_id.size());
             yyjson_mut_obj_add_strn(doc, payload, "sender", sender.data(),
                                     sender.size());
             yyjson_mut_obj_add_strn(doc, payload, "action", action.data(),
                                     action.size());
             yyjson_mut_obj_add_strn(doc, payload, "description",
                                     description.data(), description.size());
             yyjson_mut_obj_add_str(doc, payload, "source_kind", "foreground_turn");
             yyjson_mut_obj_add_null(doc, payload, "source_id");
             yyjson_mut_obj_add_null(doc, payload, "agent_id");
             yyjson_mut_obj_add_null(doc, payload, "subagent_type");
             yyjson_mut_obj_add_null(doc, payload, "source_description");
             yyjson_mut_obj_add_val(doc, payload, "display", yyjson_mut_arr(doc));
         });
}

void WireWriter::wire_approval_response(kimix::string_view request_id,
                                        kimix::string_view response,
                                        kimix::string_view feedback) {
    // ApprovalResponse {request_id, response, feedback}; feedback defaults to
    // "" in the reference (a plain string, never null).
    emit("ApprovalResponse",
         [request_id, response,
          feedback](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
             yyjson_mut_obj_add_strn(doc, payload, "request_id", request_id.data(),
                                     request_id.size());
             yyjson_mut_obj_add_strn(doc, payload, "response", response.data(),
                                     response.size());
             yyjson_mut_obj_add_strn(doc, payload, "feedback", feedback.data(),
                                     feedback.size());
         });
}

void WireWriter::wire_btw_begin(kimix::string_view id,
                                kimix::string_view question) {
    emit("BtwBegin", [id, question](yyjson_mut_doc *doc, yyjson_mut_val *payload) {
        yyjson_mut_obj_add_strn(doc, payload, "id", id.data(), id.size());
        yyjson_mut_obj_add_strn(doc, payload, "question", question.data(),
                                question.size());
    });
}

void WireWriter::wire_btw_end(kimix::string_view id, kimix::string_view response,
                              kimix::string_view error) {
    emit("BtwEnd", [id, response, error](yyjson_mut_doc *doc,
                                         yyjson_mut_val *payload) {
        yyjson_mut_obj_add_strn(doc, payload, "id", id.data(), id.size());
        add_str_or_null(doc, payload, "response", response);
        add_str_or_null(doc, payload, "error", error);
    });
}

} // namespace kimix::agent
