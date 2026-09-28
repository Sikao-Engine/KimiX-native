// Test for the CLI display line of the built-in tools
// (builtin_tools/tool.h: Tool::operator()'s `display_str` out-parameter).
//
// The terminal prints ONE short line per tool call instead of the tool's full
// output (src/cli/cli_app.cpp forwards the line to
// stream_renderer::on_tool_result). This test pins that contract:
// - the universal rules every call must satisfy, whatever it returns: a single
//   line, at most kToolDisplayMaxChars code points (+ the "..." marker), and
//   non-empty (an unanswered display_str is filled from the tool's own result
//   payload by tool_display_scope);
// - every registered built-in, driven with no parameters at all (the shortest
//   path through each operator()), answers with such a line;
// - the tool-specific lines: read/write name the file, glob/grep report the
//   match counts, todo reports the tree size, the plan tools name the plan
//   file, bash reports the captured-output size - and none of them leaks the
//   payload's raw output into the line.
#include "ut/ut.hpp"

#include "builtin_tools/agent_tool.h"
#include "builtin_tools/bash_tool.h"
#include "builtin_tools/compact_tool.h"
#include "builtin_tools/context_prune_tool.h"
#include "builtin_tools/edit_tool.h"
#include "builtin_tools/fetch_url_tool.h"
#include "builtin_tools/glob_tool.h"
#include "builtin_tools/grep_tool.h"
#include "builtin_tools/job_output_tool.h"
#include "builtin_tools/plan_tool.h"
#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/read_image_tool.h"
#include "builtin_tools/read_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/web_search_tool.h"
#include "builtin_tools/workflow_tool.h"
#include "builtin_tools/write_tool.h"

#include <core/kimix_core.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;

namespace {

namespace bt = kimix::builtin_tools;

kimix::string td_display_of(Tool &tool, const ToolParams *params) {
    kimix::string display;
    tool(params, display);
    // The universal contract, checked for every call in this file.
    expect(display.find('\n') == kimix::string::npos)
        << "display line must be single-line: " << display;
    expect(display.find('\r') == kimix::string::npos)
        << "display line must be single-line: " << display;
    expect(display.size() <= kToolDisplayMaxChars + 3) << display;
    return display;
}

bool td_contains(const kimix::string &haystack, kimix::string_view needle) {
    return kimix::string_view(haystack).find(needle) != kimix::string_view::npos;
}

// A temp work dir for the native_io tools (created by the caller that needs
// real files; the pure-kernel calls never touch it).
kimix::filesystem::path td_work_dir(const char *tag) {
    std::error_code ec;
    const kimix::string name = kimix::string("kimix_display_") + tag;
    const kimix::filesystem::path dir =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    if (ec) {
        return {};
    }
    return dir;
}

bool td_write_file(const kimix::filesystem::path &path, const char *text) {
    std::FILE *f = std::fopen(kimix::to_string(path).c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t n = std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
    return n == std::strlen(text);
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ── every registry tool answers with a display line ─────────────────────
    "every_registered_tool_fills_display_str"_test = [] {
        bt::Session session;
        session.work_dir = ".";
        session.native_io = false;
        session.plan_enabled = true;
        session.swarm_enabled = true;
        session.state_dir = "";

        const kimix::vector<ToolMeta> metas = ToolRegistry::instance().all();
        expect(!metas.empty()) << "the tool registry is populated";
        size_t checked = 0;
        for (const ToolMeta &meta : metas) {
            if (meta.external || !meta.factory) {
                continue; // host-answered tools are not built-in operator()s
            }
            kimix::unique_ptr<Tool> tool = meta.factory(&session);
            if (tool == nullptr) {
                continue;
            }
            ++checked;
            // No parameters: the shortest path through every operator(). The
            // display line still has to be there, because the terminal prints
            // it for a failed call exactly as for a successful one.
            const kimix::string display = td_display_of(*tool, nullptr);
            expect(!display.empty()) << meta.name.c_str()
                                     << " answered with no display line";
        }
        expect(checked > 15) << checked << " tools probed";
    };

    // ── read: the file it read, not its content ─────────────────────────────
    "read_display_names_the_file"_test = [] {
        read::Read tool(nullptr);
        ToolParams p;
        p.values["content"] =
            ValueElement::make_string(kimix::string("alpha\nbeta needle\ngamma\n"));
        p.values["display_path"] = ValueElement::make_string(kimix::string("notes.txt"));
        const kimix::string display = td_display_of(tool, &p);
        expect(td_contains(display, "notes.txt")) << display;
        expect(td_contains(display, "lines")) << display;
        // The file content itself never reaches the terminal line.
        expect(!td_contains(display, "beta needle")) << display;
    };

    // ── write: the file and the byte count ──────────────────────────────────
    "write_display_names_the_file"_test = [] {
        write::Write tool(nullptr);
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string(kimix::string("out.txt"));
        p.values["content"] =
            ValueElement::make_string(kimix::string("one\ntwo\nthree\n"));
        const kimix::string display = td_display_of(tool, &p);
        expect(td_contains(display, "out.txt")) << display;
        expect(td_contains(display, "bytes")) << display;
    };

    // ── edit: the file and how many replacements landed ─────────────────────
    "edit_display_counts_replacements"_test = [] {
        const kimix::filesystem::path dir = td_work_dir("edit");
        if (dir.empty()) {
            return;
        }
        bt::Session session;
        session.work_dir = kimix::to_string(dir);
        session.native_io = true;
        expect(td_write_file(dir / "target.txt", "alpha\nbeta\ngamma\n"));
        edit::Edit tool(&session);
        ToolParams p;
        p.values["file_path"] =
            ValueElement::make_string(kimix::string("target.txt"));
        ValueElement::Array edits;
        ToolParams one;
        one.values["old_string"] =
            ValueElement::make_string(kimix::string("beta\n"));
        one.values["new_string"] =
            ValueElement::make_string(kimix::string("BETA\n"));
        edits.push_back(ValueElement::make_object(
            kimix::shared_ptr<ToolParams>(new ToolParams(std::move(one)))));
        p.values["edits"] = ValueElement::make_array(std::move(edits));
        const kimix::string display = td_display_of(tool, &p);
        expect(td_contains(display, "target.txt")) << display;
        expect(td_contains(display, "replacement")) << display;
        std::error_code ec;
        kimix::filesystem::remove_all(dir, ec);
    };

    // ── glob / grep: counts, not the hit list ───────────────────────────────
    "glob_and_grep_display_count_the_hits"_test = [] {
        const kimix::filesystem::path dir = td_work_dir("scan");
        if (dir.empty()) {
            return;
        }
        expect(td_write_file(dir / "a.txt", "needle here\n"));
        expect(td_write_file(dir / "b.txt", "nothing\n"));
        bt::Session session;
        session.work_dir = kimix::to_string(dir);
        session.native_io = true;

        glob::Glob g(&session);
        ToolParams gp;
        gp.values["pattern"] = ValueElement::make_string(kimix::string("*.txt"));
        gp.values["path"] = ValueElement::make_string(kimix::to_string(dir));
        const kimix::string gdisplay = td_display_of(g, &gp);
        expect(td_contains(gdisplay, "*.txt")) << gdisplay;
        expect(td_contains(gdisplay, "files")) << gdisplay;

        grep::Grep r(&session);
        ToolParams rp;
        rp.values["pattern"] = ValueElement::make_string(kimix::string("needle"));
        rp.values["path"] = ValueElement::make_string(kimix::to_string(dir));
        const kimix::string rdisplay = td_display_of(r, &rp);
        expect(td_contains(rdisplay, "needle")) << rdisplay;
        expect(!td_contains(rdisplay, "needle here"))
            << "the matched line stays out of the display: " << rdisplay;

        std::error_code ec;
        kimix::filesystem::remove_all(dir, ec);
    };

    // ── todo: the shape of the tree ─────────────────────────────────────────
    "todo_display_counts_the_tree"_test = [] {
        bt::Session session;
        todo::TodoList tool(&session);
        ToolParams p;
        ValueElement::Array items;
        {
            auto item = kimix::shared_ptr<ToolParams>(new ToolParams());
            item->values["title"] = ValueElement::make_string(kimix::string("Design"));
            item->values["status"] = ValueElement::make_string(kimix::string("pending"));
            items.push_back(ValueElement::make_object(std::move(item)));
        }
        {
            auto item = kimix::shared_ptr<ToolParams>(new ToolParams());
            item->values["title"] = ValueElement::make_string(kimix::string("Build"));
            item->values["status"] = ValueElement::make_string(kimix::string("pending"));
            items.push_back(ValueElement::make_object(std::move(item)));
        }
        p.values["todos"] = ValueElement::make_array(std::move(items));
        const kimix::string write_display = td_display_of(tool, &p);
        expect(!write_display.empty()) << write_display;
        // The read flow names the tree size.
        ToolParams read;
        const kimix::string read_display = td_display_of(tool, &read);
        expect(td_contains(read_display, "todos")) << read_display;
    };

    // ── plan tools: the plan file they touched ──────────────────────────────
    "plan_display_names_the_plan_file"_test = [] {
        const kimix::filesystem::path dir = td_work_dir("plan");
        if (dir.empty()) {
            return;
        }
        bt::Session session;
        session.work_dir = kimix::to_string(dir);
        session.native_io = true;
        session.plan_enabled = true;
        session.plan_path = kimix::to_string(dir / "PLAN.md");

        plan::WritePlan w(&session);
        ToolParams wp;
        wp.values["content"] =
            ValueElement::make_string(kimix::string("# Plan\n\n- step\n"));
        const kimix::string wdisplay = td_display_of(w, &wp);
        expect(td_contains(wdisplay, "PLAN.md")) << wdisplay;

        plan::ReadPlan r(&session);
        ToolParams rp;
        const kimix::string rdisplay = td_display_of(r, &rp);
        expect(!rdisplay.empty()) << rdisplay;

        plan::EditPlan e(&session);
        ToolParams ep;
        ValueElement::Array edits;
        {
            auto one = kimix::shared_ptr<ToolParams>(new ToolParams());
            one->values["old_string"] =
                ValueElement::make_string(kimix::string("- step\n"));
            one->values["new_string"] =
                ValueElement::make_string(kimix::string("- step one\n"));
            edits.push_back(ValueElement::make_object(std::move(one)));
        }
        ep.values["edits"] = ValueElement::make_array(std::move(edits));
        const kimix::string edisplay = td_display_of(e, &ep);
        expect(td_contains(edisplay, "PLAN.md")) << edisplay;

        std::error_code ec;
        kimix::filesystem::remove_all(dir, ec);
    };

    // ── bash: the outcome of the run, never the captured stream ─────────────
    "bash_display_summarizes_the_run"_test = [] {
        if (bash::Bash::detect_bash_path().empty()) {
            return; // no shell on this host
        }
        const kimix::filesystem::path dir = td_work_dir("bash");
        if (dir.empty()) {
            return;
        }
        bt::Session session;
        session.work_dir = kimix::to_string(dir);
        session.native_io = true;
        bash::Bash tool(&session);
        ToolParams p;
        p.values["cmd"] = ValueElement::make_string(
            kimix::string("printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n%.0s' 1 2 3 30"));
        p.values["timeout"] = ValueElement::make_int(60);
        const kimix::string display = td_display_of(tool, &p);
        expect(!display.empty()) << display;
        expect(td_contains(display, "lines")) << display;
        // 900 captured bytes of 'a' must NOT be printed on the terminal line.
        expect(display.size() < 300) << display;
        std::error_code ec;
        kimix::filesystem::remove_all(dir, ec);
    };

    // ── the kernel-only tools still answer through the payload default ──────
    "compact_and_retrieve_display_come_from_the_payload"_test = [] {
        compact::Compact compact(nullptr);
        ToolParams cp;
        const kimix::string cdisplay = td_display_of(compact, &cp);
        expect(!cdisplay.empty()) << cdisplay;

        retrieve::Retrieve retrieve(nullptr);
        ToolParams rp;
        const kimix::string rdisplay = td_display_of(retrieve, &rp);
        expect(!rdisplay.empty()) << rdisplay;
    };

    // ── a failing call still prints something useful ────────────────────────
    "failure_paths_keep_a_display_line"_test = [] {
        read::Read read_tool(nullptr);
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string(kimix::string("nope.txt"));
        p.values["content"] = ValueElement::make_string(kimix::string(""));
        // "content" empty is an invalid_input path: the status is the line.
        const kimix::string display = td_display_of(read_tool, &p);
        expect(!display.empty()) << display;

        write::Write write_tool(nullptr);
        const kimix::string nodisplay = td_display_of(write_tool, nullptr);
        expect(td_contains(nodisplay, "invalid_input")) << nodisplay;
    };

    return 0;
}
