/*
 * export_builder.cpp - see export_builder.h (plan 016).
 *
 * Byte-exact port of kimi_cli/utils/export.py::build_export_markdown plus
 * its helpers (_build_overview, _group_into_turns, _format_turn_md,
 * _format_tool_call_md, _format_tool_result_md, _extract_tool_call_hint,
 * _format_content_part_md, message_stringify). Tool-call argument JSON is
 * re-serialized with the shared orjson OPT_INDENT_2-compatible printer.
 * All helper names are prefixed `exp_` to stay unique across unity batches.
 */

#include <runtime/tools/export_builder.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

#include <cstdio>
#include <cstdlib>

#include <runtime/common/json_pretty.h>
#include <runtime/common/text_util.h>
#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace tools {
namespace {

constexpr const char* kExpHintKeys[] = {"path", "file_path", "command",
                                        "query", "url",       "name",
                                        "pattern"};

// Parsed JSON fragment carried by non-text/think content parts.
struct exp_frag {
    kimix::string type;
    kimix::string audio_id;
    bool has_audio_id = false;
};

exp_frag exp_parse_frag(kimix::string_view frag) noexcept {
    exp_frag out;
    yyjson_doc* doc = yyjson_read_opts((char*)frag.data(), frag.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return out;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val* t = yyjson_obj_get(root, "type");
        if (t != nullptr && yyjson_is_str(t)) {
            out.type.assign(yyjson_get_str(t),
                            static_cast<size_t>(yyjson_get_len(t)));
        }
        yyjson_val* au = yyjson_obj_get(root, "audio_url");
        if (au != nullptr && yyjson_is_obj(au)) {
            yyjson_val* id = yyjson_obj_get(au, "id");
            if (id != nullptr && yyjson_is_str(id)) {
                out.audio_id.assign(yyjson_get_str(id),
                                    static_cast<size_t>(yyjson_get_len(id)));
                out.has_audio_id = true;
            }
        }
    }
    yyjson_doc_free(doc);
    return out;
}

// _format_content_part_md(part) when stringify_mode=false; message_stringify
// when true. Returns "" for parts that render to nothing (whitespace text,
// empty think) -- callers re-check with empty_after_trim like Python.
void exp_part_md(const soul::part_view& p, bool stringify,
                 kimix::string& out) noexcept {
    switch (p.kind) {
    case soul::part_kind::TEXT:
        out.append(p.text.data(), p.text.size());
        return;
    case soul::part_kind::THINK:
        if (stringify) {
            out += "[think]"; // message_stringify's else branch: f"[{part.type}]"
            return;
        }
        if (common::empty_after_trim(p.text)) {
            return;
        }
        out += "<details><summary>Thinking</summary>\n\n";
        out.append(p.text.data(), p.text.size());
        out += "\n\n</details>";
        return;
    case soul::part_kind::IMAGE:
        out += "[image]";
        return;
    case soul::part_kind::AUDIO:
        if (stringify) {
            exp_frag f = exp_parse_frag(p.text);
            out += "[audio";
            if (f.has_audio_id) {
                out += ':';
                out += f.audio_id;
            }
            out += ']';
        } else {
            out += "[audio]";
        }
        return;
    default: {
        // FILE / OTHER / TOOL_CALL: the fragment carries the part dict.
        exp_frag f = exp_parse_frag(p.text);
        if (f.type == "video_url") {
            out += "[video]"; // kimi_cli VideoURLPart
            return;
        }
        out += '[';
        if (!f.type.empty()) {
            out += f.type;
        } else {
            out += "unknown";
        }
        out += ']';
        return;
    }
    }
}

// message_stringify(msg): concatenated per-part renderings.
kimix::string exp_message_stringify(const soul::message_view& m) noexcept {
    kimix::string out;
    for (const soul::part_view& p : m.parts) {
        exp_part_md(p, true, out);
    }
    return out;
}

// _extract_tool_call_hint(args_json): well-known keys first, then the first
// short string value; "" when nothing useful (or doc unparsable).  Operates on
// an already-parsed doc so hint extraction and the pretty printer can share ONE
// yyjson parse per tool call.
kimix::string exp_extract_hint_doc(yyjson_doc* doc) noexcept {
    if (doc == nullptr) {
        return kimix::string();
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        return kimix::string();
    }
    kimix::string hint;
    for (const char* key : kExpHintKeys) {
        yyjson_val* v = yyjson_obj_get(root, key);
        if (v != nullptr && yyjson_is_str(v)) {
            kimix::string_view sv(yyjson_get_str(v),
                                  static_cast<size_t>(yyjson_get_len(v)));
            if (!common::empty_after_trim(sv)) {
                // Copy BEFORE the caller frees the doc (yyjson_read strings
                // live in the doc's str pool -- T6 lesson); shorten_utf8
                // already returns an owning copy.
                hint = common::shorten_utf8(sv, 60);
                return hint;
            }
        }
    }
    // Fallback: first string value with 0 < len <= 80.
    size_t idx = 0;
    size_t max = 0;
    yyjson_val* key = nullptr;
    yyjson_val* val = nullptr;
    yyjson_obj_foreach(root, idx, max, key, val) {
        if (yyjson_is_str(val)) {
            kimix::string_view sv(yyjson_get_str(val),
                                  static_cast<size_t>(yyjson_get_len(val)));
            const size_t len = common::utf8_code_point_count(sv);
            if (len > 0 && len <= 80) {
                hint = common::shorten_utf8(sv, 60);
                return hint;
            }
        }
    }
    return hint;
}

// One-pass markdown line sink: every logical "line" is separated by exactly
// one '\n' (matching "\n".join(lines) in the reference), but bytes are
// appended straight into the final output buffer instead of being staged in
// per-turn/top-level string vectors and joined later.
struct exp_md_writer {
    kimix::string& out;
    bool first = true;

    // Emit the separator before a block that appends directly to `out`.
    void next() noexcept {
        if (!first) {
            out += '\n';
        }
        first = false;
    }

    void line(kimix::string_view s) noexcept {
        next();
        out.append(s.data(), s.size());
    }
};

// _format_tool_call_md(tool_call).  `doc` was parsed by the caller from
// tc.arguments (may be null when unparsable): the hint and the pretty-printed
// argument JSON both come from this one doc, so a tool call is parsed only
// once.  Returns the hint (used for the paired tool-result header).
kimix::string exp_format_tool_call_md(const soul::tool_call_view& tc,
                                      yyjson_doc* doc,
                                      kimix::string& out,
                                      exp_md_writer& w) noexcept {
    const kimix::string hint = exp_extract_hint_doc(doc);
    const kimix::string_view args_sv =
        tc.arguments.empty() ? kimix::string_view("{}") : tc.arguments;

    w.next();
    out += "#### Tool Call: ";
    out.append(tc.name.data(), tc.name.size());
    if (!hint.empty()) {
        out += " (`";
        out += hint;
        out += "`)";
    }
    out += "\n<!-- call_id: ";
    out.append(tc.id.data(), tc.id.size());
    out += " -->\n```json\n";

    // args_formatted = orjson.dumps(parsed, OPT_INDENT_2) or args_raw.
    if (doc != nullptr) {
        // pretty_write_doc appends; never empty for a parsed doc (root is
        // always a value), so the old args_raw fallback is unreachable here.
        common::pretty_write_doc(doc, out);
    } else {
        out.append(args_sv.data(), args_sv.size());
    }
    out += "\n```";
    return hint;
}

// _format_tool_result_md(msg, tool_name, hint).  result_parts are rendered
// directly into `out` (only non-blank renderings, joined with "\n"), avoiding
// per-part temporary strings.
void exp_format_tool_result_md(const soul::message_view& msg,
                               kimix::string_view tool_name,
                               kimix::string_view hint,
                               kimix::string& out,
                               exp_md_writer& w) noexcept {
    w.next();
    const kimix::string_view call_id =
        msg.tool_call_id.empty() ? kimix::string_view("unknown")
                                 : msg.tool_call_id;

    out += "<details><summary>Tool Result: ";
    out.append(tool_name.data(), tool_name.size());
    if (!hint.empty()) {
        out += " (`";
        out.append(hint.data(), hint.size());
        out += "`)";
    }
    out += "</summary>\n\n<!-- call_id: ";
    out.append(call_id.data(), call_id.size());
    out += " -->\n";

    bool first = true;
    for (const soul::part_view& p : msg.parts) {
        // TEXT/THINK render to "" (and are skipped) exactly when the raw text
        // is blank; every other kind renders a non-empty placeholder, so the
        // empty_after_trim check on the rendered bytes is equivalent to this.
        const bool may_render_empty =
            (p.kind == soul::part_kind::TEXT || p.kind == soul::part_kind::THINK);
        if (may_render_empty && common::empty_after_trim(p.text)) {
            continue;
        }
        if (!first) {
            out += '\n';
        }
        first = false;
        exp_part_md(p, false, out);
    }

    out += "\n\n</details>";
}

// export.py::_is_internal_user_message.
bool exp_is_internal_user(const soul::message_view& m) noexcept {
    return soul::is_checkpoint_msg(m) || soul::is_system_reminder_msg(m) ||
           soul::is_notification_msg(m);
}

// _group_into_turns: logical turns, each starting at a real user message.
void exp_group_turns(kimix::span<const soul::message_view> msgs,
                     kimix::vector<kimix::vector<size_t>>& turns) noexcept {
    kimix::vector<size_t> current;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (exp_is_internal_user(msgs[i])) {
            continue;
        }
        if (msgs[i].role == soul::kRoleUser && !current.empty()) {
            turns.push_back(current);
            current.clear();
        }
        current.push_back(i);
    }
    if (!current.empty()) {
        turns.push_back(current);
    }
}

struct exp_tc_info {
    kimix::string name;
    kimix::string hint;
};

// _format_turn_md(messages, turn_number): writes the turn directly into the
// shared document writer (same logical line sequence as "\n".join(lines)).
void exp_format_turn_md(kimix::span<const soul::message_view> msgs,
                        const kimix::vector<size_t>& turn,
                        size_t turn_number,
                        kimix::string& out,
                        exp_md_writer& w) noexcept {
    char buf[64];
    const int header_len = std::snprintf(
        buf, sizeof(buf), "## Turn %llu",
        static_cast<unsigned long long>(turn_number));
    w.line(kimix::string_view(buf, static_cast<size_t>(header_len)));
    w.line("");

    kimix::unordered_map<kimix::string, exp_tc_info, kimix::string_hash> tool_call_info;
    bool assistant_header_written = false;

    const auto append_parts = [&](kimix::span<const soul::part_view> parts) {
        for (const soul::part_view& p : parts) {
            if (p.kind == soul::part_kind::TEXT) {
                // Rendered text == p.text; skip when it trims to nothing.
                if (common::empty_after_trim(p.text)) {
                    continue;
                }
                w.line(p.text);
                w.line("");
                continue;
            }
            kimix::string text;
            exp_part_md(p, false, text);
            if (!common::empty_after_trim(text)) {
                w.line(text);
                w.line("");
            }
        }
    };

    for (size_t idx : turn) {
        const soul::message_view& msg = msgs[idx];
        if (exp_is_internal_user(msg)) {
            continue;
        }
        if (msg.role == soul::kRoleUser) {
            w.line("### User");
            w.line("");
            append_parts(msg.parts);
        } else if (msg.role == soul::kRoleAssistant) {
            if (!assistant_header_written) {
                w.line("### Assistant");
                w.line("");
                assistant_header_written = true;
            }
            append_parts(msg.parts);
            for (const soul::tool_call_view& tc : msg.tool_calls) {
                // Parse the arguments ONCE; hint + pretty JSON share the doc.
                const kimix::string_view args_sv =
                    tc.arguments.empty() ? kimix::string_view("{}")
                                         : tc.arguments;
                yyjson_doc* doc = yyjson_read_opts(
                    const_cast<char*>(args_sv.data()), args_sv.size(), 0,
                    &kimix::llm::kYYJsonAlcMi, nullptr);
                const kimix::string hint =
                    exp_format_tool_call_md(tc, doc, out, w);
                yyjson_doc_free(doc);
                exp_tc_info info;
                info.name.assign(tc.name.data(), tc.name.size());
                info.hint = hint;
                tool_call_info[kimix::string(tc.id)] = std::move(info);
                w.line("");
            }
        } else if (msg.role == soul::kRoleTool) {
            kimix::string_view tc_id = msg.tool_call_id;
            kimix::string_view name("unknown");
            kimix::string_view hint;
            auto it = tool_call_info.find(kimix::string(tc_id));
            if (it != tool_call_info.end()) {
                name = it->second.name;
                hint = it->second.hint;
            }
            exp_format_tool_result_md(msg, name, hint, out, w);
            w.line("");
        } else if (msg.role == soul::kRoleSystem) {
            w.line("### System");
            w.line("");
            append_parts(msg.parts);
        }
    }
}

// f"{value:,}" -- comma-grouped decimal.
void exp_append_comma_uint(kimix::string& out, uint64_t value) noexcept {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%llu",
                                static_cast<unsigned long long>(value));
    for (int i = 0; i < n; ++i) {
        out.push_back(buf[i]);
        const int remaining = n - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out.push_back(',');
        }
    }
}

// _build_overview(history, turns, token_count).
kimix::string exp_build_overview(kimix::span<const soul::message_view> msgs,
                                 size_t turn_count, uint64_t token_count) noexcept {
    // Topic: first real user message, shorten(message_stringify(msg), 80).
    kimix::string topic;
    for (const soul::message_view& m : msgs) {
        if (m.role == soul::kRoleUser && !exp_is_internal_user(m)) {
            topic = common::shorten_utf8(exp_message_stringify(m), 80);
            break;
        }
    }
    // n_tool_calls across all messages.
    size_t n_tool_calls = 0;
    for (const soul::message_view& m : msgs) {
        n_tool_calls += m.tool_calls.size();
    }

    kimix::string out;
    out += "## Overview\n\n";
    if (!topic.empty()) {
        out += "- **Topic**: ";
        out += topic;
    } else {
        out += "- **Topic**: (empty)";
    }
    out += "\n- **Conversation**: ";
    out += std::to_string(turn_count);
    out += " turns | ";
    out += std::to_string(n_tool_calls);
    out += " tool calls | ";
    exp_append_comma_uint(out, token_count);
    out += " tokens\n\n---";
    return out;
}

} // namespace

void build_export_markdown(kimix::span<const soul::message_view> msgs,
                           const export_options& opts,
                           kimix::string& out) noexcept {
    out.clear();

    // One-pass write with a rough reserve so the full document grows in a
    // single buffer instead of being staged in string vectors and joined.
    size_t estimate = 4096;
    for (const soul::message_view& m : msgs) {
        estimate += 128;
        for (const soul::part_view& p : m.parts) {
            estimate += p.text.size();
        }
        for (const soul::tool_call_view& tc : m.tool_calls) {
            estimate += tc.name.size() + 96;
            estimate += tc.arguments.size() * 2; // pretty JSON expands
        }
    }
    out.reserve(estimate);

    exp_md_writer w{out};
    const auto add_header_line = [&](kimix::string_view prefix,
                                     kimix::string_view value) noexcept {
        w.next();
        out.append(prefix.data(), prefix.size());
        out.append(value.data(), value.size());
    };

    w.line("---");
    add_header_line("session_id: ", opts.session_id);
    add_header_line("exported_at: ", opts.exported_at);
    add_header_line("work_dir: ", opts.work_dir);
    add_header_line("message_count: ", std::to_string(msgs.size()));
    add_header_line("token_count: ", std::to_string(opts.token_count));
    w.line("---");
    w.line("");
    w.line("# Kimi Session Export");
    w.line("");

    kimix::vector<kimix::vector<size_t>> turns;
    exp_group_turns(msgs, turns);
    const kimix::string overview =
        exp_build_overview(msgs, turns.size(), opts.token_count);
    w.line(overview);
    w.line("");

    size_t turn_number = 1;
    for (const kimix::vector<size_t>& turn : turns) {
        exp_format_turn_md(msgs, turn, turn_number, out, w);
        ++turn_number;
    }
}

} // namespace tools
} // namespace runtime
} // namespace kimix
