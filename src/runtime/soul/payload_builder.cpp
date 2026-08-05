/*
 * payload_builder.cpp - see payload_builder.h (plan 014).
 *
 * Uses the vendored kimix-yyjson (trimmed 0.12 fork) the same way
 * wire_envelope.cpp does: immutable yyjson_read for embedded JSON fragments
 * (non-text parts), the yyjson_mut_* builder API for the output doc, write
 * buffers allocated with the default (malloc) allocator and freed with
 * free(). All functions are noexcept.
 */

#include <runtime/soul/payload_builder.h>

#include <yyjson.h>

#include <cstdlib>

#include <runtime/common/text_util.h>
#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace soul {
namespace {

// is_effectively_empty_content_parts(visible): every visible part is a
// TextPart whose text strips to empty (vacuous true for an empty list).
bool effectively_empty(const kimix::vector<const part_view*>& visible) noexcept {
    for (const part_view* p : visible) {
        if (p->kind != part_kind::TEXT) {
            return false;
        }
        if (!common::empty_after_trim(p->text)) {
            return false;
        }
    }
    return true;
}

// Add one content-part dict to `arr`. TEXT/THINK are handled natively;
// every other kind carries a pre-serialized JSON object in `p.text` which
// is parsed once and embedded verbatim (falls back to a text part when the
// fragment is not valid JSON).
void append_content_part(yyjson_mut_doc* doc, yyjson_mut_val* arr,
                         const part_view& p) noexcept {
    yyjson_mut_val* part = nullptr;
    if (p.kind == part_kind::TEXT) {
        part = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strncpy(doc, part, "type", "text", 4);
        yyjson_mut_obj_add_strncpy(doc, part, "text", p.text.data(), p.text.size());
    } else if (p.kind != part_kind::THINK) {
        // Raw JSON object fragment (image_url / audio_url / video_url / ...).
        yyjson_doc* parsed = yyjson_read(p.text.data(), p.text.size(), 0);
        if (parsed != nullptr) {
            yyjson_val* root = yyjson_doc_get_root(parsed);
            if (root != nullptr && yyjson_is_obj(root)) {
                part = yyjson_val_mut_copy(doc, root);
            }
            yyjson_doc_free(parsed);
        }
        if (part == nullptr) {
            part = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strncpy(doc, part, "type", "text", 4);
            yyjson_mut_obj_add_strncpy(doc, part, "text", p.text.data(), p.text.size());
        }
    }
    if (part != nullptr) {
        yyjson_mut_arr_append(arr, part);
    }
}

void append_tool_call(yyjson_mut_doc* doc, yyjson_mut_val* arr,
                      const tool_call_view& tc) noexcept {
    yyjson_mut_val* call = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strncpy(doc, call, "type", "function", 8);
    yyjson_mut_obj_add_strncpy(doc, call, "id", tc.id.data(), tc.id.size());
    yyjson_mut_val* fn = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strncpy(doc, fn, "name", tc.name.data(), tc.name.size());
    if (!tc.arguments.empty()) {
        // exclude_none=True: None arguments drop the key entirely.
        yyjson_mut_obj_add_strncpy(doc, fn, "arguments", tc.arguments.data(),
                                   tc.arguments.size());
    }
    yyjson_mut_obj_add_val(doc, call, "function", fn);
    yyjson_mut_arr_append(arr, call);
}

// Build one ChatCompletionMessageParam dict. Returns false on allocation
// failure (out stays untouched by this message).
bool append_message(yyjson_mut_doc* doc, yyjson_mut_val* root,
                    const message_view& msg, bool preserved_thinking) noexcept {
    // Reasoning split: ThinkPart text is concatenated into reasoning_content;
    // visible keeps the remaining parts in original order.
    kimix::string reasoning;
    kimix::vector<const part_view*> visible;
    visible.reserve(msg.parts.size());
    bool has_reasoning_part = false;
    for (const part_view& p : msg.parts) {
        if (p.kind == part_kind::THINK) {
            has_reasoning_part = true;
            reasoning.append(p.text.data(), p.text.size());
        } else {
            visible.push_back(&p);
        }
    }

    yyjson_mut_val* m = yyjson_mut_obj(doc);
    if (m == nullptr) {
        return false;
    }
    const char* role = role_name(msg.role);
    const size_t role_len = std::char_traits<char>::length(role);
    yyjson_mut_obj_add_strncpy(doc, m, "role", role, role_len);

    // Content: single visible TextPart -> plain string (pydantic field
    // serializer); otherwise a list of part dicts; dropped entirely when the
    // Kimi compat quirk applies (assistant + tool_calls + empty content).
    const bool drop_content =
        msg.role == kRoleAssistant && !msg.tool_calls.empty() &&
        effectively_empty(visible);
    if (!drop_content) {
        if (visible.size() == 1 && visible[0]->kind == part_kind::TEXT) {
            yyjson_mut_obj_add_strncpy(doc, m, "content", visible[0]->text.data(),
                                       visible[0]->text.size());
        } else {
            yyjson_mut_val* content = yyjson_mut_arr(doc);
            for (const part_view* p : visible) {
                append_content_part(doc, content, *p);
            }
            yyjson_mut_obj_add_val(doc, m, "content", content);
        }
    }

    if (!msg.tool_calls.empty()) {
        yyjson_mut_val* calls = yyjson_mut_arr(doc);
        for (const tool_call_view& tc : msg.tool_calls) {
            append_tool_call(doc, calls, tc);
        }
        yyjson_mut_obj_add_val(doc, m, "tool_calls", calls);
    }

    if (!msg.tool_call_id.empty()) {
        yyjson_mut_obj_add_strncpy(doc, m, "tool_call_id", msg.tool_call_id.data(),
                                   msg.tool_call_id.size());
    }

    if (has_reasoning_part ||
        (preserved_thinking && msg.role == kRoleAssistant)) {
        yyjson_mut_obj_add_strncpy(doc, m, "reasoning_content", reasoning.data(),
                                   reasoning.size());
    }

    return yyjson_mut_arr_append(root, m);
}

} // namespace

void build_payload(kimix::span<const message_view> msgs,
                   bool preserved_thinking_enabled,
                   kimix::string& out_json) noexcept {
    out_json.clear();
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (doc == nullptr) {
        return;
    }
    yyjson_mut_val* root = yyjson_mut_arr(doc);
    if (root == nullptr) {
        yyjson_mut_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(doc, root);

    for (const message_view& msg : msgs) {
        if (!append_message(doc, root, msg, preserved_thinking_enabled)) {
            yyjson_mut_doc_free(doc);
            out_json.clear();
            return;
        }
    }

    size_t len = 0;
    char* text = yyjson_mut_write(doc, 0, &len);
    if (text != nullptr) {
        out_json.assign(text, len);
        free(text); // default allocator -> malloc/free
    }
    yyjson_mut_doc_free(doc);
}

} // namespace soul
} // namespace runtime
} // namespace kimix
