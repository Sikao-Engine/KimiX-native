/*
 * message_rows.cpp - history persistence rows over the arena.
 * See message_rows.h for the row contract and the packed layout.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers. One yyjson document per ROW (not per message list) keeps the
 * transient to the row itself; the FTS text is produced in the same pass so a
 * message never produces a second Python string.
 */

#include <core/kimix_core.h>

#include <runtime/common/json_out.h>
#include <runtime/common/le_bytes.h>
#include <runtime/index/message_rows.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

namespace kimix {
namespace runtime {
namespace index {

namespace src_runtime_index_message_rows {

using kimix::llm::kYYJsonAlcMi;
using kimix::runtime::common::json_add_str;
using kimix::runtime::common::json_make_fragment;
using kimix::runtime::common::json_write;
using kimix::runtime::common::put_le32;
using kimix::runtime::common::read_le32;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

inline kimix::string_view mr_role_name(uint8_t role) noexcept {
    switch (role) {
    case kimix::runtime::soul::kRoleSystem:
        return "system";
    case kimix::runtime::soul::kRoleAssistant:
        return "assistant";
    case kimix::runtime::soul::kRoleTool:
        return "tool";
    default:
        return "user";
    }
}

inline uint8_t mr_role_from_name(kimix::string_view name) noexcept {
    if (name == "system") {
        return kimix::runtime::soul::kRoleSystem;
    }
    if (name == "assistant") {
        return kimix::runtime::soul::kRoleAssistant;
    }
    if (name == "tool") {
        return kimix::runtime::soul::kRoleTool;
    }
    return kimix::runtime::soul::kRoleUser;
}

// part type string -> part_kind (the shim's _PART_TYPE_KIND table).
inline part_kind mr_kind_from_type(kimix::string_view type) noexcept {
    if (type == "text") {
        return part_kind::TEXT;
    }
    if (type == "think") {
        return part_kind::THINK;
    }
    if (type == "tool_call") {
        return part_kind::TOOL_CALL;
    }
    if (type == "image_url") {
        return part_kind::IMAGE;
    }
    if (type == "audio_url") {
        return part_kind::AUDIO;
    }
    if (type == "video_url" || type == "file") {
        return part_kind::FILE;
    }
    return part_kind::OTHER;
}

// One part as model_dump() renders it inside a content list.
void add_part_dump(yyjson_mut_doc *doc, yyjson_mut_val *content,
                   const part_view &p) {
    if (p.kind == part_kind::TEXT) {
        yyjson_mut_val *block = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(content, block);
        yyjson_mut_obj_add_str(doc, block, "type", "text");
        json_add_str(doc, block, "text", p.text);
        return;
    }
    if (p.kind == part_kind::THINK) {
        yyjson_mut_val *block = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(content, block);
        yyjson_mut_obj_add_str(doc, block, "type", "think");
        json_add_str(doc, block, "think", p.text);
        // model_dump keeps the None default of `encrypted`.
        yyjson_mut_obj_add_null(doc, block, "encrypted");
        return;
    }
    if (yyjson_mut_val *frag = json_make_fragment(doc, p.text)) {
        yyjson_mut_arr_append(content, frag);
        return;
    }
    yyjson_mut_val *block = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(content, block);
    yyjson_mut_obj_add_str(doc, block, "type", "text");
    json_add_str(doc, block, "text", p.text);
}

// One tool call as ToolCall.model_dump(exclude_none=True) renders it.
void add_tool_call(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                   const tool_call_view &tc) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(arr, obj);
    yyjson_mut_obj_add_str(doc, obj, "type", "function");
    json_add_str(doc, obj, "id", tc.id);
    yyjson_mut_val *fn = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "function", fn);
    json_add_str(doc, fn, "name", tc.name);
    if (tc.arguments.data() != nullptr) {
        json_add_str(doc, fn, "arguments", tc.arguments);
    }
}

// "\n".join of the TEXT parts (fts5_search.extract_text_from_content).
void fts_text(kimix::span<const part_view> parts, kimix::string &out) {
    out.clear();
    bool first = true;
    for (const part_view &p : parts) {
        if (p.kind != part_kind::TEXT) {
            continue;
        }
        if (!first) {
            out.push_back('\n');
        }
        out.append(p.text.data(), p.text.size());
        first = false;
    }
}

bool build_row(const message_view &mv, kimix::string &json) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    json_add_str(doc, root, "role", mr_role_name(mv.role));

    size_t visible = 0;
    const part_view *only = nullptr;
    for (const part_view &p : mv.parts) {
        ++visible;
        only = &p;
    }
    if (visible == 1 && only != nullptr && only->kind == part_kind::TEXT) {
        // Message._serialize_content: a lone TextPart collapses to its string.
        json_add_str(doc, root, "content", only->text);
    } else {
        yyjson_mut_val *content = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "content", content);
        for (const part_view &p : mv.parts) {
            add_part_dump(doc, content, p);
        }
    }
    if (!mv.tool_calls.empty()) {
        yyjson_mut_val *calls = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tool_calls", calls);
        for (const tool_call_view &tc : mv.tool_calls) {
            add_tool_call(doc, calls, tc);
        }
    }
    if (mv.tool_call_id.data() != nullptr) {
        json_add_str(doc, root, "tool_call_id", mv.tool_call_id);
    }
    return json_write(doc, json);
}

// Re-serialize one content part object into `out` (the arena form for kinds
// that are not text/think).
bool dump_fragment(yyjson_val *val, kimix::string &out) {
    char *text = yyjson_val_write_opts(val, 0, &kYYJsonAlcMi, nullptr, nullptr);
    if (text == nullptr) {
        return false;
    }
    out.assign(text, strlen(text));
    kimix::deallocate(text);
    return true;
}

} // namespace src_runtime_index_message_rows
using namespace src_runtime_index_message_rows;

void context_rows(kimix::span<const kimix::runtime::soul::message_view> msgs,
                  kimix::string &packed) noexcept {
    kimix::vector<kimix::string> jsons;
    kimix::vector<kimix::string> ftss;
    jsons.reserve(msgs.size());
    ftss.reserve(msgs.size());
    uint64_t total = 0;
    for (const kimix::runtime::soul::message_view &mv : msgs) {
        kimix::string json;
        kimix::string text;
        if (!build_row(mv, json)) {
            packed.clear();
            return;
        }
        fts_text(mv.parts, text);
        total += 8 + json.size() + text.size();
        jsons.push_back(std::move(json));
        ftss.push_back(std::move(text));
    }
    packed.resize(4 + total);
    char *p = packed.data();
    put_le32(p, static_cast<uint32_t>(jsons.size()));
    p += 4;
    for (size_t i = 0; i < jsons.size(); ++i) {
        put_le32(p, static_cast<uint32_t>(jsons[i].size()));
        put_le32(p + 4, static_cast<uint32_t>(ftss[i].size()));
        p += 8;
        if (!jsons[i].empty()) {
            memcpy(p, jsons[i].data(), jsons[i].size());
        }
        p += jsons[i].size();
        if (!ftss[i].empty()) {
            memcpy(p, ftss[i].data(), ftss[i].size());
        }
        p += ftss[i].size();
    }
}

namespace src_runtime_index_message_rows {

// Decode one stored row JSON back into arena spans, then append it.
bool append_row(kimix::string_view json, kimix::runtime::soul::HistoryStore &out,
                kimix::vector<part_view> &parts,
                kimix::vector<tool_call_view> &calls,
                kimix::deque<kimix::string> &fragments) {
    kimix::string fixed;
    const char *data = json.data();
    size_t len = json.size();
    if (!kimix::runtime::common::json_utf8_valid(json)) {
        fixed = kimix::runtime::common::json_utf8_lossy(json);
        data = fixed.data();
        len = fixed.size();
    }
    yyjson_doc *doc =
        yyjson_read_opts(const_cast<char *>(data), len, 0, &kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    parts.clear();
    calls.clear();
    fragments.clear();

    yyjson_val *role = yyjson_obj_get(root, "role");
    const uint8_t role_id =
        role != nullptr && yyjson_is_str(role)
            ? mr_role_from_name(kimix::string_view(yyjson_get_str(role), yyjson_get_len(role)))
            : static_cast<uint8_t>(kimix::runtime::soul::kRoleUser);

    yyjson_val *content = yyjson_obj_get(root, "content");
    if (content != nullptr && yyjson_is_str(content)) {
        part_view pv;
        pv.kind = part_kind::TEXT;
        pv.text = kimix::string_view(yyjson_get_str(content), yyjson_get_len(content));
        parts.push_back(pv);
    } else if (content != nullptr && yyjson_is_arr(content)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_iter iter = yyjson_arr_iter_with(content);
        (void)idx;
        (void)max;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!yyjson_is_obj(item)) {
                yyjson_doc_free(doc);
                return false;
            }
            yyjson_val *type = yyjson_obj_get(item, "type");
            const kimix::string_view type_view =
                type != nullptr && yyjson_is_str(type)
                    ? kimix::string_view(yyjson_get_str(type), yyjson_get_len(type))
                    : kimix::string_view();
            part_view pv;
            pv.kind = mr_kind_from_type(type_view);
            if (pv.kind == part_kind::TEXT) {
                yyjson_val *text = yyjson_obj_get(item, "text");
                pv.text = text != nullptr && yyjson_is_str(text)
                              ? kimix::string_view(yyjson_get_str(text),
                                                   yyjson_get_len(text))
                              : kimix::string_view();
            } else if (pv.kind == part_kind::THINK) {
                yyjson_val *think = yyjson_obj_get(item, "think");
                pv.text = think != nullptr && yyjson_is_str(think)
                              ? kimix::string_view(yyjson_get_str(think),
                                                   yyjson_get_len(think))
                              : kimix::string_view();
            } else {
                // Any other kind is stored as its serialized JSON object.
                kimix::string frag;
                if (!dump_fragment(item, frag)) {
                    yyjson_doc_free(doc);
                    return false;
                }
                // deque: push_back never invalidates the views of earlier
                // fragments (a vector would reallocate under them).
                fragments.push_back(std::move(frag));
                pv.text = kimix::string_view(fragments.back());
            }
            parts.push_back(pv);
        }
    }

    yyjson_val *tc_arr = yyjson_obj_get(root, "tool_calls");
    if (tc_arr != nullptr && yyjson_is_arr(tc_arr)) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(tc_arr);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!yyjson_is_obj(item)) {
                yyjson_doc_free(doc);
                return false;
            }
            tool_call_view tc;
            yyjson_val *id = yyjson_obj_get(item, "id");
            if (id != nullptr && yyjson_is_str(id)) {
                tc.id = kimix::string_view(yyjson_get_str(id), yyjson_get_len(id));
            }
            yyjson_val *fn = yyjson_obj_get(item, "function");
            if (fn != nullptr && yyjson_is_obj(fn)) {
                yyjson_val *name = yyjson_obj_get(fn, "name");
                if (name != nullptr && yyjson_is_str(name)) {
                    tc.name = kimix::string_view(yyjson_get_str(name),
                                                 yyjson_get_len(name));
                }
                yyjson_val *args = yyjson_obj_get(fn, "arguments");
                if (args != nullptr && yyjson_is_str(args)) {
                    tc.arguments = kimix::string_view(yyjson_get_str(args),
                                                      yyjson_get_len(args));
                }
            }
            calls.push_back(tc);
        }
    }

    yyjson_val *tcid = yyjson_obj_get(root, "tool_call_id");
    kimix::string_view tool_call_id;
    if (tcid != nullptr && yyjson_is_str(tcid)) {
        tool_call_id = kimix::string_view(yyjson_get_str(tcid), yyjson_get_len(tcid));
    }
    const bool appended = out.append(role_id, parts, calls, tool_call_id) != UINT32_MAX;
    yyjson_doc_free(doc);
    return appended;
}

} // namespace src_runtime_index_message_rows
using namespace src_runtime_index_message_rows;

uint32_t parse_rows(kimix::string_view packed,
                    kimix::runtime::soul::HistoryStore &out) noexcept {
    if (packed.size() < 4) {
        return 0;
    }
    const uint32_t count = read_le32(packed.data());
    const char *p = packed.data() + 4;
    const char *end = packed.data() + packed.size();

    kimix::vector<part_view> parts;
    kimix::vector<tool_call_view> calls;
    kimix::deque<kimix::string> fragments;
    const uint32_t before = out.size();
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(end - p) < 8) {
            return out.size() - before;
        }
        const uint32_t json_len = read_le32(p);
        const uint32_t fts_len = read_le32(p + 4);
        p += 8;
        if (static_cast<size_t>(end - p) <
            static_cast<size_t>(json_len) + fts_len) {
            return out.size() - before;
        }
        const kimix::string_view json(p, json_len);
        if (!append_row(json, out, parts, calls, fragments)) {
            return out.size() - before;
        }
        p += static_cast<size_t>(json_len) + fts_len;
    }
    return out.size() - before;
}

} // namespace index
} // namespace runtime
} // namespace kimix
