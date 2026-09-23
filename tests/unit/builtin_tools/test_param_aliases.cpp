// Cross-tool tests for fuzzy alias matching of tool parameters
// (builtin_tools/tool.h: `ToolParams::alias_map` + the per-tool `param_alias`
// literal tables).
//
// Every built-in tool declares the alternate argument names it accepts, so a
// hallucinated-but-reasonable name (`command` for `cmd`, `ShellCommand` for
// `cmd`, `file` for `file_path`) parses like the canonical one. This file drives
// the REAL tool entry points (`operator()` or the public `parse_*` function)
// with alias-only parameter objects and asserts:
//   * a declared alias (including case/separator-folded spellings) is accepted,
//   * the canonical name still wins when both are present,
//   * an undeclared name is still rejected exactly as before.
//
// The ToolParams machinery itself (alias_map / add_alias / get_exact) is covered
// by test_tool.cpp; per-tool behaviour lives in the test_<tool>_tool.cpp files.

#include "ut/ut.hpp"

#include "builtin_tools/agent_tool.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/compact_tool.h"
#include "builtin_tools/edit_tool.h"
#include "builtin_tools/fetch_url_tool.h"
#include "builtin_tools/glob_tool.h"
#include "builtin_tools/grep_tool.h"
#include "builtin_tools/job_output_tool.h"
#include "builtin_tools/plan_tool.h"
#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/read_image_tool.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/run_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/web_search_tool.h"
#include "builtin_tools/workflow_tool.h"
#include "builtin_tools/write_tool.h"

#include <cstdint>

namespace {

using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// Serialized tool results are JSON objects: small readers for the fields this
// test asserts on.
kimix::string_view sv_of(const kimix::vector<char> &buf) {
    return kimix::string_view(buf.data(), buf.size());
}

bool has_text(const kimix::vector<char> &buf, kimix::string_view needle) {
    return sv_of(buf).find(needle) != kimix::string_view::npos;
}

ToolParams parse_result(const kimix::vector<char> &buf) {
    ToolParams result;
    kimix::string error;
    result.try_deserialize(kimix::span<char const>(buf.data(), buf.size()), error);
    return result;
}

kimix::string string_field(const kimix::vector<char> &buf, kimix::string_view key) {
    const ToolParams result = parse_result(buf);
    const ValueElement *el = result.get(key);
    return (el != nullptr && el->is_string()) ? el->as_string() : kimix::string();
}

} // namespace

int main() {
    using namespace boost::ut;
    using namespace boost::ut::literals;

    // ──────────────────────────────────────────────────────────────────────────
    // bash: `shell_command` for `cmd`, folding, canonical-wins, strict reject
    // ──────────────────────────────────────────────────────────────────────────
    "bash_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["shell_command"] = ValueElement::make_string("ls -la");
        p.values["max_output_lines"] = ValueElement::make_int(12);
        p.values["wait_pattern"] = ValueElement::make_string("done");
        kimix::builtin_tools::bash::bash_params out;
        const tool_error err =
            kimix::builtin_tools::bash::parse_bash_params(&p, out);
        expect(!err.failed()) << "declared aliases are accepted";
        expect(out.cmd == kimix::string("ls -la"));
        expect(out.max_lines.has_value() && *out.max_lines == 12_i);
        expect(out.wait_for_pattern.has_value() &&
               *out.wait_for_pattern == kimix::string("done"));
    };

    "bash_alias_matches_folded_spelling"_test = [] {
        ToolParams p;
        p.values["ShellCommand"] = ValueElement::make_string("pwd");
        kimix::builtin_tools::bash::bash_params out;
        expect(!kimix::builtin_tools::bash::parse_bash_params(&p, out).failed());
        expect(out.cmd == kimix::string("pwd"))
            << "case and '_'/'-' are ignored when matching an alias";
    };

    "bash_canonical_name_wins_over_alias"_test = [] {
        ToolParams p;
        p.values["cmd"] = ValueElement::make_string("canonical");
        p.values["shell_command"] = ValueElement::make_string("aliased");
        kimix::builtin_tools::bash::bash_params out;
        expect(!kimix::builtin_tools::bash::parse_bash_params(&p, out).failed());
        expect(out.cmd == kimix::string("canonical"));
    };

    "bash_undeclared_name_is_still_rejected"_test = [] {
        ToolParams p;
        p.values["execute"] = ValueElement::make_string("ls");
        kimix::builtin_tools::bash::bash_params out;
        const tool_error err =
            kimix::builtin_tools::bash::parse_bash_params(&p, out);
        expect(err.failed());
        expect(err.status == tool_status::invalid_input);
    };

    // ──────────────────────────────────────────────────────────────────────────
    // run / job_output / retrieve / plan: parse-level tools
    // ──────────────────────────────────────────────────────────────────────────
    "run_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["command_line"] = ValueElement::make_string("echo hi");
        p.values["timeout_seconds"] = ValueElement::make_int(7);
        p.values["use_shell"] = ValueElement::make_bool(true);
        p.values["background"] = ValueElement::make_bool(true);
        kimix::builtin_tools::run::run_params out;
        expect(!kimix::builtin_tools::run::parse_params(&p, out).failed());
        expect(out.command == kimix::string("echo hi"));
        expect(out.timeout_seconds == 7_i);
        expect(out.shell);
        expect(out.run_in_background);
    };

    "job_output_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["id"] = ValueElement::make_string("bash_7");
        p.values["force_kill"] = ValueElement::make_bool(true);
        p.values["output"] = ValueElement::make_string("out.txt");
        kimix::builtin_tools::job_output::job_output_params out;
        expect(!kimix::builtin_tools::job_output::parse_params(&p, out).failed());
        expect(out.job_id.has_value() && *out.job_id == kimix::string("bash_7"));
        // `kill: true` is normalised to action='kill'.
        expect(out.action == kimix::string("kill"));
        expect(out.output_path.has_value() &&
               *out.output_path == kimix::string("out.txt"));
    };

    "retrieve_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["q"] = ValueElement::make_string("how do aliases work");
        p.values["top_k"] = ValueElement::make_int(5);
        kimix::builtin_tools::retrieve::retrieve_params out;
        tool_error err;
        expect(kimix::builtin_tools::retrieve::parse_params(&p, out, err) ==
               tool_status::ok);
        expect(out.query == kimix::string("how do aliases work"));
        expect(out.k == 5);
    };

    "plan_read_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["offset"] = ValueElement::make_int(3);
        p.values["lines"] = ValueElement::make_int(4);
        p.values["char_limit"] = ValueElement::make_int(64);
        kimix::builtin_tools::plan::read_plan_params out;
        expect(!kimix::builtin_tools::plan::parse_read_params(&p, out).failed());
        expect(out.line_offset == 3_i);
        expect(out.n_lines == 4_i);
        expect(out.max_char == 64_i);
    };

    "plan_write_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["plan_text"] = ValueElement::make_string("the plan");
        kimix::builtin_tools::plan::write_plan_params out;
        expect(!kimix::builtin_tools::plan::parse_write_params(&p, out).failed());
        expect(out.content == kimix::string("the plan"));
    };

    "plan_edit_alias_fresh_name"_test = [] {
        using VE = ValueElement;
        using TP = ToolParams;
        kimix::shared_ptr<TP> item(new TP());
        item->values["old"] = VE::make_string("a");
        item->values["new"] = VE::make_string("A");
        VE::Array edits;
        edits.push_back(VE::make_object(std::move(item)));
        ToolParams p;
        p.values["edit_items"] = VE::make_array(std::move(edits));
        kimix::builtin_tools::plan::edit_plan_params out;
        expect(!kimix::builtin_tools::plan::parse_edit_params(&p, out).failed());
        expect(out.edits.size() == size_t(1));
        expect(out.edits[0].old_text == kimix::string("a"));
        expect(out.edits[0].new_text == kimix::string("A"));
    };

    // ──────────────────────────────────────────────────────────────────────────
    // agent tools: alias-only parameter objects
    // ──────────────────────────────────────────────────────────────────────────
    "subagent_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["instruction"] = ValueElement::make_string("do the thing");
        p.values["desc"] = ValueElement::make_string("brief");
        p.values["resume_session_id"] = ValueElement::make_string("sess_1");
        p.values["background"] = ValueElement::make_bool(false);
        kimix::builtin_tools::agents::subagent_params out;
        expect(!kimix::builtin_tools::agents::parse_subagent_params(&p, out).failed());
        expect(out.prompt == kimix::string("do the thing"));
        expect(out.description.has_value() &&
               *out.description == kimix::string("brief"));
        expect(out.session_id.has_value() &&
               *out.session_id == kimix::string("sess_1"));
        expect(!out.run_in_background);
    };

    "interrupt_agent_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["target"] = ValueElement::make_string("agent_9");
        kimix::builtin_tools::agents::interrupt_agent_params out;
        expect(!kimix::builtin_tools::agents::parse_interrupt_params(&p, out).failed());
        expect(out.agent_id == kimix::string("agent_9"));
    };

    "send_message_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["content"] = ValueElement::make_string("hello there");
        p.values["target"] = ValueElement::make_string("agent_2");
        kimix::builtin_tools::agents::send_message_params out;
        expect(!kimix::builtin_tools::agents::parse_send_message_params(&p, out).failed());
        expect(out.message == kimix::string("hello there"));
        expect(out.subagent_id.has_value() &&
               *out.subagent_id == kimix::string("agent_2"));
    };

    "list_agents_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["filter"] = ValueElement::make_string("descendants");
        kimix::builtin_tools::agents::list_agents_params out;
        expect(!kimix::builtin_tools::agents::parse_list_agents_params(&p, out).failed());
        expect(out.scope == kimix::string("descendants"));
    };

    // ──────────────────────────────────────────────────────────────────────────
    // todo: alias-only write mode / update field
    // ──────────────────────────────────────────────────────────────────────────
    "todo_write_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["write_mode"] = ValueElement::make_string("clear");
        kimix::builtin_tools::todo::write_params out;
        kimix::builtin_tools::todo::tool_response resp;
        expect(kimix::builtin_tools::todo::parse_write_params(&p, out, resp));
        expect(out.mode == kimix::builtin_tools::todo::write_mode::clear);
    };

    "todo_update_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["content"] = ValueElement::make_string("My todo");
        p.values["state"] = ValueElement::make_string("done");
        kimix::builtin_tools::todo::update_params out;
        kimix::builtin_tools::todo::tool_response resp;
        expect(kimix::builtin_tools::todo::parse_update_params(&p, out, resp));
        expect(out.ops.size() == size_t(1));
    };

    // ──────────────────────────────────────────────────────────────────────────
    // workflow: alias-only parse
    // ──────────────────────────────────────────────────────────────────────────
    "workflow_alias_fresh_names"_test = [] {
        using VE = ValueElement;
        ToolParams p;
        p.values["desc"] = VE::make_string("swarm task");
        p.values["sample_count"] = VE::make_int(3);
        VE::Array items;
        items.push_back(VE::make_string("a"));
        items.push_back(VE::make_string("b"));
        p.values["inputs"] = VE::make_array(std::move(items));
        p.values["prefix"] = VE::make_string("do ");
        kimix::builtin_tools::workflow::workflow_params out;
        const tool_error err =
            kimix::builtin_tools::workflow::parse_params(&p, out);
        expect(!err.failed()) << err.message;
        expect(out.description == kimix::string("swarm task"));
        expect(out.sample_n.has_value() && *out.sample_n == 3);
        expect(out.items.size() == size_t(2)) << "'inputs' is accepted for items";
        expect(out.prompt_prefix.has_value() &&
               *out.prompt_prefix == kimix::string("do "));
    };

    // ──────────────────────────────────────────────────────────────────────────
    // write / read / edit: alias-only calls through operator()
    // ──────────────────────────────────────────────────────────────────────────
    "write_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["file"] = ValueElement::make_string("/tmp/alias_test.txt");
        p.values["text"] = ValueElement::make_string("new\ncontent\n");
        kimix::builtin_tools::write::Write tool(nullptr);
        tool(&p);
        const ToolParams &res = tool.last_result();
        const ValueElement *status = res.get("status");
        expect(status != nullptr && status->is_string());
        expect(status->as_string() == kimix::string("ok"));
        expect(res.get("new_text")->as_string() == kimix::string("new\ncontent\n"));
    };

    "read_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["text"] = ValueElement::make_string("a\nb\n");
        p.values["display"] = ValueElement::make_string("x.txt");
        kimix::builtin_tools::read::Read tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.serialized_result();
        expect(!buf.empty());
        expect(string_field(buf, "status") == kimix::string("ok"))
            << "'text'/'display' are accepted for content/display_path";
    };

    "edit_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["edit_mode"] = ValueElement::make_string("replace");
        p.values["content"] = ValueElement::make_string("a\nb\n");
        p.values["old_string"] = ValueElement::make_string("a");
        p.values["new_string"] = ValueElement::make_string("A");
        kimix::builtin_tools::edit::Edit tool(nullptr);
        tool(&p);
        const ToolParams &res = tool.last_result();
        expect(res.get("status")->as_string() == kimix::string("ok"))
            << "'edit_mode' is accepted for mode";
        expect(res.get("replacements")->as_int() == 1_i);
        expect(res.get("content")->as_string() == kimix::string("A\nb\n"));
    };

    // ──────────────────────────────────────────────────────────────────────────
    // tools whose kernel parses inline in operator()
    // ──────────────────────────────────────────────────────────────────────────
    "glob_alias_fresh_names"_test = [] {
        ToolParams p;
        p.values["glob"] = ValueElement::make_string("*.py");
        p.values["directory"] =
            ValueElement::make_string("/this/path/does/not/exist/for/glob");
        kimix::builtin_tools::Session session;
        kimix::builtin_tools::glob::Glob tool(&session);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(!has_text(buf, "Missing or invalid 'pattern'"))
            << "'glob' is accepted for pattern";
        expect(has_text(buf, "does not exist"))
            << "the walk runs and reports the missing directory";
    };

    "grep_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["regex"] = ValueElement::make_string("needle");
        p.values["paths"] =
            ValueElement::make_string("/this/path/does/not/exist/for/grep");
        kimix::builtin_tools::grep::Grep tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.serialized_result();
        expect(!buf.empty());
        expect(!has_text(buf, "missing required field: pattern"))
            << "'regex' is accepted for pattern";
    };

    "pwsh_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["script"] = ValueElement::make_string("Get-ChildItem");
        kimix::builtin_tools::pwsh::Pwsh tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(string_field(buf, "status") == kimix::string("ok"))
            << "'script' is accepted for command";
    };

    "compact_alias_fresh_name"_test = [] {
        using VE = ValueElement;
        using TP = ToolParams;
        kimix::shared_ptr<TP> part(new TP());
        part->values["type"] = VE::make_string("text");
        part->values["text"] = VE::make_string("hello");
        VE::Array content;
        content.push_back(VE::make_object(std::move(part)));
        kimix::shared_ptr<TP> msg(new TP());
        msg->values["role"] = VE::make_string("user");
        msg->values["content"] = VE::make_array(std::move(content));
        VE::Array messages;
        messages.push_back(VE::make_object(std::move(msg)));

        ToolParams p;
        p.values["history"] = VE::make_array(std::move(messages));
        p.values["preserve_index"] = VE::make_int(0);
        p.values["base_prompt"] = VE::make_string("BASE");
        kimix::builtin_tools::compact::Compact tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(!has_text(buf, "missing or invalid 'messages' array"))
            << "'history' is accepted for messages";
    };

    "fetch_url_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["page_content"] = ValueElement::make_string("<p>hi</p>");
        kimix::builtin_tools::fetch_url::FetchUrl tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(has_text(buf, "\"ok\":true"))
            << "'page_content' is accepted for html";
    };

    "web_search_alias_fresh_name"_test = [] {
        using VE = ValueElement;
        using TP = ToolParams;
        kimix::shared_ptr<TP> item(new TP());
        item->values["title"] = VE::make_string("T");
        item->values["url"] = VE::make_string("https://example.com");
        item->values["snippet"] = VE::make_string("S");
        VE::Array items;
        items.push_back(VE::make_object(std::move(item)));

        ToolParams p;
        p.values["results"] = VE::make_array(std::move(items));
        kimix::builtin_tools::web_search::WebSearch tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(has_text(buf, "\"ok\":true"))
            << "'results' is accepted for items";
    };

    "read_image_alias_fresh_name"_test = [] {
        ToolParams p;
        p.values["file"] =
            ValueElement::make_string("/this/path/does/not/exist/x.png");
        kimix::builtin_tools::read_image::ReadImage tool(nullptr);
        tool(&p);
        const kimix::vector<char> &buf = tool.last_result();
        expect(!buf.empty());
        expect(!has_text(buf, "missing or invalid path parameter"))
            << "'file' is accepted for path";
    };

    return 0;
}
