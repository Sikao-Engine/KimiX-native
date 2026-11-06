/*
 * request_builder.cpp - the provider request-body builder over the arena.
 * See request_builder.h for the pipeline and the per-provider envelope
 * contract, which is pinned byte-for-byte by the C++ suite and by the shim's
 * `_compat_build_request` mirror.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers. Strings are sanitized (invalid UTF-8 -> U+FFFD) before they reach
 * yyjson, because agent text can carry lone surrogates through the shim's
 * surrogatepass encoding and yyjson refuses to write invalid UTF-8.
 */

#include <core/kimix_core.h>

#include <runtime/common/json_out.h>
#include <runtime/soul/request_builder.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_request_builder {

using kimix::llm::kYYJsonAlcMi;
using kimix::runtime::common::json_add_fragment;
using kimix::runtime::common::json_add_str;
using kimix::runtime::common::json_make_fragment;
using kimix::runtime::common::json_make_str;
using kimix::runtime::common::json_write;

inline void set_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                    kimix::string_view value) {
    json_add_str(doc, obj, key, value);
}

inline bool add_fragment(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                         kimix::string_view raw) {
    return json_add_fragment(doc, obj, key, raw);
}

inline yyjson_mut_val *make_fragment(yyjson_mut_doc *doc, kimix::string_view raw) {
    return json_make_fragment(doc, raw);
}

inline yyjson_mut_val *make_str(yyjson_mut_doc *doc, kimix::string_view value) {
    return json_make_str(doc, value);
}

inline bool write_doc(yyjson_mut_doc *doc, kimix::string &out) noexcept {
    return json_write(doc, out);
}

// ── message classification (the reference predicates) ────────────────────────

inline bool single_text_starts_with(kimix::span<const part_view> parts,
                                    kimix::string_view prefix) noexcept {
    if (parts.size() != 1 || parts[0].kind != part_kind::TEXT) {
        return false;
    }
    kimix::string_view text = parts[0].text;
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\n' || text[i] == '\r' ||
                               text[i] == '\f' || text[i] == '\v')) {
        ++i;
    }
    return text.substr(i).substr(0, prefix.size()) == prefix;
}

inline bool is_system_reminder(const message_view &mv) noexcept {
    return mv.role == kRoleUser &&
           single_text_starts_with(mv.parts, "<system-reminder>");
}

inline bool is_notification(const message_view &mv) noexcept {
    return mv.role == kRoleUser &&
           single_text_starts_with(mv.parts, "<notification ");
}

inline kimix::string_view rb_role_name(uint8_t role) noexcept {
    switch (role) {
    case kRoleSystem:
        return "system";
    case kRoleAssistant:
        return "assistant";
    case kRoleTool:
        return "tool";
    default:
        return "user";
    }
}

// Visible (non-think) part count / index helpers.
inline bool visible_part(const part_view &p) noexcept {
    return p.kind != part_kind::THINK;
}

// ── pipeline stages ──────────────────────────────────────────────────────────

// Repair pairing into `out`. The synthetic placeholder texts and their single
// part_view live in DEQUES: both are addressed by span/pointer from the views in
// `out`, and a deque never relocates an existing element on push_back (a string
// / vector would, and the spans would read freed memory).
void repair_into(kimix::span<const message_view> in,
                 kimix::vector<message_view> &out,
                 kimix::deque<kimix::string> &synth,
                 kimix::deque<part_view> &synth_parts) {
    out.clear();
    out.reserve(in.size());

    // tool_call_id -> queue of input indices (ordered, so duplicates are seen).
    kimix::vector<kimix::string_view> ids;
    kimix::vector<kimix::vector<uint32_t>> at;
    bool any_calls = false;
    for (const message_view &mv : in) {
        if (mv.role == kRoleTool && mv.tool_call_id.data() != nullptr) {
            bool found = false;
            for (size_t k = 0; k < ids.size(); ++k) {
                if (ids[k] == mv.tool_call_id) {
                    at[k].push_back(static_cast<uint32_t>(&mv - in.data()));
                    found = true;
                    break;
                }
            }
            if (!found) {
                ids.push_back(mv.tool_call_id);
                kimix::vector<uint32_t> q;
                q.push_back(static_cast<uint32_t>(&mv - in.data()));
                at.push_back(std::move(q));
            }
        }
        if (!mv.tool_calls.empty()) {
            any_calls = true;
        }
    }

    bool has_tool_ids = false;
    for (const message_view &mv : in) {
        if (mv.role == kRoleTool && mv.tool_call_id.data() != nullptr) {
            has_tool_ids = true;
            break;
        }
    }
    if (!has_tool_ids && !any_calls) {
        for (const message_view &mv : in) {
            out.push_back(mv);
        }
        return;
    }

    kimix::vector<uint32_t> cursor;
    cursor.assign(ids.size(), 0);

    for (const message_view &mv : in) {
        if (mv.role == kRoleTool) {
            continue; // reclaimed by its claiming assistant message
        }
        out.push_back(mv);
        if (mv.tool_calls.empty()) {
            continue;
        }
        for (const tool_call_view &tc : mv.tool_calls) {
            int32_t bucket = -1;
            for (size_t k = 0; k < ids.size(); ++k) {
                if (ids[k] == tc.id) {
                    bucket = static_cast<int32_t>(k);
                    break;
                }
            }
            if (bucket < 0 || cursor[static_cast<size_t>(bucket)] >=
                                  at[static_cast<size_t>(bucket)].size()) {
                // Interrupted step: synthesize the placeholder result.
                synth.push_back(kimix::format(
                    "[Tool result missing: the call to '{}' was interrupted "
                    "before its result was recorded. Treat the call as "
                    "failed.]",
                    tc.name));
                part_view pv;
                pv.kind = part_kind::TEXT;
                pv.text = kimix::string_view(synth.back());
                synth_parts.push_back(pv);
                message_view fake;
                fake.role = kRoleTool;
                fake.parts = kimix::span<const part_view>(&synth_parts.back(), 1);
                fake.tool_call_id = tc.id;
                out.push_back(fake);
                continue;
            }
            const uint32_t input_index =
                at[static_cast<size_t>(bucket)][cursor[static_cast<size_t>(bucket)]++];
            out.push_back(in[input_index]);
        }
    }
}

// Merge adjacent user messages into `out`, appending their parts to
// `parts_store` (reserved by the caller so the spans stay valid).
void normalize_views(kimix::span<const message_view> in,
                     kimix::vector<message_view> &out,
                     kimix::vector<part_view> &parts_store) {
    out.clear();
    out.reserve(in.size());
    kimix::vector<uint32_t> first;
    kimix::vector<uint32_t> count;
    first.reserve(in.size());
    count.reserve(in.size());
    for (const message_view &mv : in) {
        const bool merge = !out.empty() && out.back().role == kRoleUser &&
                           mv.role == kRoleUser && !is_notification(out.back()) &&
                           !is_notification(mv) && !is_system_reminder(out.back()) &&
                           !is_system_reminder(mv);
        if (merge) {
            for (const part_view &p : mv.parts) {
                parts_store.push_back(p);
            }
            count.back() += static_cast<uint32_t>(mv.parts.size());
            continue;
        }
        out.push_back(mv);
        first.push_back(static_cast<uint32_t>(parts_store.size()));
        const size_t start = parts_store.size();
        for (const part_view &p : mv.parts) {
            parts_store.push_back(p);
        }
        count.push_back(static_cast<uint32_t>(parts_store.size() - start));
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].parts = kimix::span<const part_view>(parts_store.data() + first[i],
                                                    count[i]);
    }
}

} // namespace src_runtime_soul_request_builder
using namespace src_runtime_soul_request_builder;

// ── provider / option parsing ────────────────────────────────────────────────

bool parse_provider(kimix::string_view name, provider_kind &out) noexcept {
    if (name == "openai_legacy" || name == "openai" || name == "openai_chat") {
        out = provider_kind::openai_legacy;
        return true;
    }
    if (name == "openai_responses" || name == "responses") {
        out = provider_kind::openai_responses;
        return true;
    }
    if (name == "anthropic") {
        out = provider_kind::anthropic;
        return true;
    }
    if (name == "kimi") {
        out = provider_kind::kimi;
        return true;
    }
    return false;
}

kimix::string_view provider_name(provider_kind kind) noexcept {
    switch (kind) {
    case provider_kind::openai_responses:
        return "openai_responses";
    case provider_kind::anthropic:
        return "anthropic";
    case provider_kind::kimi:
        return "kimi";
    default:
        return "openai_legacy";
    }
}

// ── public stages ────────────────────────────────────────────────────────────

void normalize_history(kimix::span<const message_view> in,
                       kimix::string &structure, kimix::string &history) noexcept {
    kimix::vector<message_view> merged;
    kimix::vector<part_view> parts_store;
    size_t total_parts = 0;
    for (const message_view &mv : in) {
        total_parts += mv.parts.size();
    }
    parts_store.reserve(total_parts);
    normalize_views(in, merged, parts_store);
    kmb2_build(merged, structure, history);
}

void repair_tool_pairing(kimix::span<const message_view> in,
                         kimix::string &structure, kimix::string &history) noexcept {
    kimix::vector<message_view> repaired;
    kimix::deque<kimix::string> synth;
    kimix::deque<part_view> synth_parts;
    repair_into(in, repaired, synth, synth_parts);
    kmb2_build(repaired, structure, history);
}

// ── provider envelopes ───────────────────────────────────────────────────────

namespace src_runtime_soul_request_builder {

// Append the content of one message: string form for a single visible TEXT
// part, else an array of part blocks. Returns the value to put under "content".
yyjson_mut_val *make_openai_content(yyjson_mut_doc *doc,
                                    kimix::span<const part_view> parts,
                                    bool has_tool_calls) {
    size_t visible = 0;
    const part_view *only = nullptr;
    for (const part_view &p : parts) {
        if (visible_part(p)) {
            ++visible;
            only = &p;
        }
    }
    if (visible == 0) {
        if (has_tool_calls) {
            return yyjson_mut_null(doc);
        }
        return make_str(doc, "");
    }
    if (visible == 1 && only != nullptr && only->kind == part_kind::TEXT) {
        return make_str(doc, only->text);
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (const part_view &p : parts) {
        if (!visible_part(p)) {
            continue;
        }
        if (p.kind == part_kind::TEXT) {
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(arr, block);
            yyjson_mut_obj_add_str(doc, block, "type", "text");
            set_str(doc, block, "text", p.text);
            continue;
        }
        // Non-text parts carry their serialized JSON object verbatim.
        if (yyjson_mut_val *frag = make_fragment(doc, p.text)) {
            yyjson_mut_arr_append(arr, frag);
        } else {
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(arr, block);
            set_str(doc, block, "type", "text");
            set_str(doc, block, "text", p.text);
        }
    }
    return arr;
}

void add_openai_tool_calls(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                           kimix::span<const tool_call_view> calls) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, obj, "tool_calls", arr);
    for (const tool_call_view &tc : calls) {
        yyjson_mut_val *tco = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(arr, tco);
        set_str(doc, tco, "id", tc.id);
        yyjson_mut_obj_add_str(doc, tco, "type", "function");
        yyjson_mut_val *fn = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, tco, "function", fn);
        set_str(doc, fn, "name", tc.name);
        if (tc.arguments.data() != nullptr) {
            set_str(doc, fn, "arguments", tc.arguments);
        }
    }
}

bool build_openai_like(kimix::span<const message_view> msgs,
                       const request_options &opts, kimix::string &out) noexcept {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    set_str(doc, root, "model", opts.model);
    yyjson_mut_obj_add_bool(doc, root, "stream", opts.stream);
    if (opts.stream) {
        yyjson_mut_val *stream_options = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "stream_options", stream_options);
        yyjson_mut_obj_add_bool(doc, stream_options, "include_usage", true);
    }
    if (opts.max_tokens > 0) {
        const char *key = opts.provider == provider_kind::kimi
                              ? "max_completion_tokens"
                              : "max_tokens";
        yyjson_mut_obj_add_int(doc, root, key, opts.max_tokens);
    }
    if (opts.has_temperature) {
        yyjson_mut_obj_add_real(doc, root, "temperature", opts.temperature);
    }

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", arr);
    for (const message_view &mv : msgs) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(arr, obj);
        set_str(doc, obj, "role", rb_role_name(mv.role));
        if (mv.role == kRoleAssistant) {
            if (yyjson_mut_val *c =
                    make_openai_content(doc, mv.parts, !mv.tool_calls.empty())) {
                yyjson_mut_obj_add_val(doc, obj, "content", c);
            }
        } else {
            if (yyjson_mut_val *c = make_openai_content(doc, mv.parts, false)) {
                yyjson_mut_obj_add_val(doc, obj, "content", c);
            }
        }
        if (mv.tool_call_id.data() != nullptr) {
            set_str(doc, obj, "tool_call_id", mv.tool_call_id);
        }
        if (!mv.tool_calls.empty()) {
            add_openai_tool_calls(doc, obj, mv.tool_calls);
        }
        if (opts.preserve_thinking && !opts.reasoning_key.empty() &&
            mv.role == kRoleAssistant) {
            kimix::string thinking;
            bool first = true;
            for (const part_view &p : mv.parts) {
                if (p.kind != part_kind::THINK) {
                    continue;
                }
                if (!first) {
                    thinking.push_back('\n');
                }
                thinking.append(p.text.data(), p.text.size());
                first = false;
            }
            if (!first) {
                const kimix::string key(opts.reasoning_key);
                set_str(doc, obj, key.c_str(), thinking);
            }
        }
    }

    if (!opts.tools_json.empty()) {
        if (!add_fragment(doc, root, "tools", opts.tools_json)) {
            yyjson_mut_obj_add_null(doc, root, "tools");
        }
    }
    if (!opts.tool_choice.empty()) {
        if (!add_fragment(doc, root, "tool_choice", opts.tool_choice)) {
            set_str(doc, root, "tool_choice", opts.tool_choice);
        }
    }
    if (!opts.reasoning_effort.empty()) {
        set_str(doc, root, "reasoning_effort", opts.reasoning_effort);
    }
    return write_doc(doc, out);
}

bool build_anthropic(kimix::span<const message_view> msgs,
                     const request_options &opts, kimix::string &out) noexcept {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    set_str(doc, root, "model", opts.model);
    yyjson_mut_obj_add_bool(doc, root, "stream", opts.stream);
    if (opts.max_tokens > 0) {
        yyjson_mut_obj_add_int(doc, root, "max_tokens", opts.max_tokens);
    }
    if (opts.has_temperature) {
        yyjson_mut_obj_add_real(doc, root, "temperature", opts.temperature);
    }

    // System: the explicit prompt plus any system-role history message (folded
    // into one string, matching the reference's <system> mapping).
    kimix::string system;
    if (!opts.system_prompt.empty()) {
        system.assign(opts.system_prompt.data(), opts.system_prompt.size());
    }
    for (const message_view &mv : msgs) {
        if (mv.role != kRoleSystem) {
            continue;
        }
        kimix::string text;
        bool first = true;
        for (const part_view &p : mv.parts) {
            if (p.kind != part_kind::TEXT) {
                continue;
            }
            if (!first) {
                text.push_back('\n');
            }
            text.append(p.text.data(), p.text.size());
            first = false;
        }
        if (text.empty()) {
            continue;
        }
        if (!system.empty()) {
            system.push_back('\n');
        }
        system.append(text);
    }
    if (!system.empty()) {
        set_str(doc, root, "system", system);
    }

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", arr);
    for (const message_view &mv : msgs) {
        if (mv.role == kRoleSystem) {
            continue;
        }
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(arr, obj);
        const bool is_tool = mv.role == kRoleTool;
        set_str(doc, obj, "role", is_tool ? "user"
                                          : (mv.role == kRoleAssistant ? "assistant"
                                                                       : "user"));
        yyjson_mut_val *content = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, obj, "content", content);

        if (is_tool) {
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "tool_result");
            set_str(doc, block, "tool_use_id", mv.tool_call_id);
            yyjson_mut_val *inner = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, block, "content", inner);
            for (const part_view &p : mv.parts) {
                if (p.kind != part_kind::TEXT) {
                    continue;
                }
                yyjson_mut_val *tb = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(inner, tb);
                yyjson_mut_obj_add_str(doc, tb, "type", "text");
                set_str(doc, tb, "text", p.text);
            }
            continue;
        }

        for (const part_view &p : mv.parts) {
            if (p.kind == part_kind::TEXT) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "text");
                set_str(doc, block, "text", p.text);
            } else if (p.kind == part_kind::THINK && opts.preserve_thinking) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "thinking");
                set_str(doc, block, "thinking", p.text);
            } else if (p.kind != part_kind::THINK &&
                       p.kind != part_kind::TOOL_CALL) {
                // image / audio / file / other: embed the serialized block.
                if (yyjson_mut_val *frag = make_fragment(doc, p.text)) {
                    yyjson_mut_arr_append(content, frag);
                }
            }
        }
        for (const tool_call_view &tc : mv.tool_calls) {
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "tool_use");
            set_str(doc, block, "id", tc.id);
            set_str(doc, block, "name", tc.name);
            if (tc.arguments.data() != nullptr) {
                if (!add_fragment(doc, block, "input", tc.arguments)) {
                    yyjson_mut_val *empty = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_val(doc, block, "input", empty);
                }
            } else {
                yyjson_mut_val *empty = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, block, "input", empty);
            }
        }
    }

    if (!opts.tools_json.empty()) {
        if (!add_fragment(doc, root, "tools", opts.tools_json)) {
            yyjson_mut_obj_add_null(doc, root, "tools");
        }
    }
    return write_doc(doc, out);
}

bool build_responses(kimix::span<const message_view> msgs,
                     const request_options &opts, kimix::string &out) noexcept {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    set_str(doc, root, "model", opts.model);
    yyjson_mut_obj_add_bool(doc, root, "stream", opts.stream);
    if (!opts.system_prompt.empty()) {
        set_str(doc, root, "instructions", opts.system_prompt);
    }
    if (opts.max_tokens > 0) {
        yyjson_mut_obj_add_int(doc, root, "max_output_tokens", opts.max_tokens);
    }
    if (opts.has_temperature) {
        yyjson_mut_obj_add_real(doc, root, "temperature", opts.temperature);
    }

    yyjson_mut_val *input = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", input);
    for (const message_view &mv : msgs) {
        if (mv.role == kRoleTool) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(input, item);
            yyjson_mut_obj_add_str(doc, item, "type", "function_call_output");
            set_str(doc, item, "call_id", mv.tool_call_id);
            kimix::string text;
            bool first = true;
            for (const part_view &p : mv.parts) {
                if (p.kind != part_kind::TEXT) {
                    continue;
                }
                if (!first) {
                    text.push_back('\n');
                }
                text.append(p.text.data(), p.text.size());
                first = false;
            }
            set_str(doc, item, "output", text);
            continue;
        }
        if (mv.role == kRoleAssistant && !mv.tool_calls.empty()) {
            for (const tool_call_view &tc : mv.tool_calls) {
                yyjson_mut_val *item = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(input, item);
                yyjson_mut_obj_add_str(doc, item, "type", "function_call");
                set_str(doc, item, "call_id", tc.id);
                set_str(doc, item, "name", tc.name);
                set_str(doc, item, "arguments",
                        tc.arguments.data() == nullptr ? kimix::string_view("{}")
                                                       : tc.arguments);
            }
        }
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(input, item);
        set_str(doc, item, "role", mv.role == kRoleAssistant ? "assistant" : "user");
        yyjson_mut_val *content = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, item, "content", content);
        const char *text_type =
            mv.role == kRoleAssistant ? "output_text" : "input_text";
        for (const part_view &p : mv.parts) {
            if (p.kind != part_kind::TEXT) {
                continue;
            }
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", text_type);
            set_str(doc, block, "text", p.text);
        }
    }
    if (!opts.tools_json.empty()) {
        if (!add_fragment(doc, root, "tools", opts.tools_json)) {
            yyjson_mut_obj_add_null(doc, root, "tools");
        }
    }
    return write_doc(doc, out);
}

} // namespace src_runtime_soul_request_builder
using namespace src_runtime_soul_request_builder;

bool build_request(kimix::span<const message_view> in, const request_options &opts,
                   kimix::string &out) noexcept {
    out.clear();
    kimix::vector<message_view> repaired;
    kimix::deque<kimix::string> synth;
    kimix::deque<part_view> synth_parts;
    repair_into(in, repaired, synth, synth_parts);

    kimix::vector<message_view> merged;
    kimix::vector<part_view> merged_parts;
    size_t total_parts = 0;
    for (const message_view &mv : repaired) {
        total_parts += mv.parts.size();
    }
    merged_parts.reserve(total_parts);
    normalize_views(repaired, merged, merged_parts);

    switch (opts.provider) {
    case provider_kind::anthropic:
        return build_anthropic(merged, opts, out);
    case provider_kind::openai_responses:
        return build_responses(merged, opts, out);
    default:
        return build_openai_like(merged, opts, out);
    }
}

} // namespace soul
} // namespace runtime
} // namespace kimix
