// test_todo_tool.cpp - Unit tests for the built-in todo tools (todo_tool.h):
// status/mode parsing, tree kernels, fuzzy scorer, write/read/update flows
// (merge, regressions, single-in_progress auto-fix, replace/clear guards,
// archiving, depth caps, rendering), state JSON round-trip, file persistence
// (atomic save, corrupt-file defaults, unknown-key merge), session-scoped
// load/save through builtin_tools::Session and kimix::agent::AgentSession,
// and the ToolRegistry entries.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access.
// Python source of truth: kimi-cli/src/kimi_cli/tools/todo/__init__.py.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

#include <cstdio>
#include <exception>
#include <cstring>
#include <stdexcept>

namespace {

using namespace boost::ut;

using kimix::builtin_tools::Session;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;
namespace todo = kimix::builtin_tools::todo;

// No exceptions (kimix is built with kimix_enable_exception=false), so a
// failed parse of a test literal is reported through Boost.UT instead of by
// throwing std::runtime_error.
ToolParams parse_json(const kimix::string &json) {
    ToolParams p;
    kimix::string err;
    const bool ok =
        p.try_deserialize(kimix::span<char const>(json.data(), json.size()),
                          err);
    expect(ok) << "parse_json: " << err;
    return p;
}

kimix::string tmp_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

kimix::string read_file_text(const kimix::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    kimix::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

todo::todo_item mk(kimix::string content, todo::todo_status status,
                   kimix::optional<kimix::string> notes = std::nullopt) {
    todo::todo_item it;
    it.content = std::move(content);
    it.status = status;
    it.notes = std::move(notes);
    return it;
}

kimix::span<const todo::todo_item> span_of(
    const kimix::vector<todo::todo_item> &v) {
    return kimix::span<const todo::todo_item>(v.data(), v.size());
}

// Run a tool with JSON args and return the parsed result envelope.
ToolParams run_tool(kimix::builtin_tools::Tool &t, const kimix::string &json) {
    ToolParams p = parse_json(json);
    t(&p);
    kimix::vector<char> out;
    t.result_json(out);
    ToolParams r;
    const kimix::string s(out.data(), out.size());
    r.deserialize(kimix::span<char const>(s.data(), s.size()));
    return r;
}

kimix::string res_str(const ToolParams &r, const char *key) {
    const ValueElement *el = r.get(key);
    return (el != nullptr && el->is_string()) ? el->as_string()
                                              : kimix::string();
}

bool res_err(const ToolParams &r) {
    const ValueElement *el = r.get("is_error");
    return el != nullptr && el->is_bool() && el->as_bool();
}

bool has(const kimix::string &hay, kimix::string_view needle) {
    return hay.find(kimix::string(needle)) != kimix::string::npos;
}

} // namespace

int main() {
    using namespace boost::ut;

    // =======================================================================
    // Status / mode vocabulary
    // =======================================================================
    "todo_status_parsing"_test = [] {
        todo::todo_status st;
        expect(todo::parse_status("pending", st));
        expect(st == todo::todo_status::pending);
        expect(todo::parse_status("in_progress", st));
        expect(st == todo::todo_status::in_progress);
        expect(todo::parse_status("done", st));
        expect(st == todo::todo_status::done);
        expect(todo::parse_status("completed", st)); // report spelling
        expect(st == todo::todo_status::done);
        expect(todo::parse_status("  IN-PROGRESS  ", st)); // strip/lower/dash
        expect(st == todo::todo_status::in_progress);
        expect(!todo::parse_status("bogus", st));
        expect(!todo::parse_status("", st));
        expect(eq(kimix::string(todo::status_name(todo::todo_status::pending)),
                  kimix::string("pending")));
        expect(eq(kimix::string(
                     todo::status_name(todo::todo_status::in_progress)),
                  kimix::string("in_progress")));
        expect(eq(kimix::string(todo::status_name(todo::todo_status::done)),
                  kimix::string("done")));
    };

    "todo_write_mode_parsing"_test = [] {
        todo::write_mode m;
        bool ffm = false;
        expect(todo::parse_write_mode("append", m, ffm));
        expect(m == todo::write_mode::append);
        expect(!ffm);
        expect(todo::parse_write_mode(" REPLACE ", m, ffm));
        expect(m == todo::write_mode::replace);
        expect(todo::parse_write_mode("overwrite", m, ffm)); // legacy spelling
        expect(m == todo::write_mode::replace);
        expect(!ffm);
        expect(todo::parse_write_mode("clear", m, ffm));
        expect(m == todo::write_mode::clear);
        expect(todo::parse_write_mode("force_overwrite", m, ffm));
        expect(m == todo::write_mode::replace);
        expect(ffm);
        expect(todo::parse_write_mode("Force Override", m, ffm)); // spaces
        expect(m == todo::write_mode::replace);
        expect(ffm);
        expect(todo::parse_write_mode("FORCE", m, ffm));
        expect(m == todo::write_mode::replace);
        expect(ffm);
        expect(!todo::parse_write_mode("bogus", m, ffm));
        expect(!todo::parse_write_mode("", m, ffm));
    };

    // =======================================================================
    // Fuzzy scorer (rapidfuzz token_sort_ratio port)
    // =======================================================================
    "todo_fuzzy_scorer"_test = [] {
        expect(todo::token_sort_ratio("hello world", "hello world") > 99.9);
        // Case-insensitive (processor=str.lower).
        expect(todo::token_sort_ratio("Hello WORLD", "hello world") > 99.9);
        // Token-order insensitive (sorted tokens).
        expect(todo::token_sort_ratio("bug login fix", "fix login bug") > 99.9);
        // Near match clears both cut-offs.
        const double near_score = todo::token_sort_ratio("Fix login bug",
                                                         "fix login bugs");
        expect(near_score >= todo::k_fuzzy_warning_cutoff) << near_score;
        // Unrelated titles stay below the title cut-off.
        const double far_score =
            todo::token_sort_ratio("write docs", "run the tests");
        expect(far_score < todo::k_fuzzy_title_cutoff) << far_score;
        // Empty vs empty / empty vs text.
        expect(todo::token_sort_ratio("  ", "") > 99.9);
        expect(todo::token_sort_ratio("", "x") < 0.1);

        kimix::vector<kimix::string> cands = {"Write design doc",
                                              "Fix login bug", "Deploy"};
        const auto hit = todo::find_nearest_title(
            "fix login bugs",
            kimix::span<const kimix::string>(cands.data(), cands.size()),
            todo::k_fuzzy_title_cutoff);
        expect(hit.has_value());
        if (hit.has_value()) {
            expect(eq(hit->choice, kimix::string("Fix login bug")));
            expect(eq(hit->index, int32_t(1)));
        }
        const auto no_hit = todo::find_nearest_title(
            "zzzz qqqq xyzzy",
            kimix::span<const kimix::string>(cands.data(), cands.size()),
            todo::k_fuzzy_title_cutoff);
        expect(!no_hit.has_value());
    };

    // =======================================================================
    // Tree kernels
    // =======================================================================
    "todo_tree_kernels"_test = [] {
        //        A(pending)
        //        ├── A1(done)
        //        └── A2(in_progress)
        //        B(done)
        kimix::vector<todo::todo_item> tree;
        tree.push_back(mk("A", todo::todo_status::pending));
        tree[0].children.push_back(mk("A1", todo::todo_status::done));
        tree[0].children.push_back(
            mk("A2", todo::todo_status::in_progress, kimix::string("note")));
        tree.push_back(mk("B", todo::todo_status::done));

        expect(eq(todo::count_all(span_of(tree)), int32_t(4)));
        const todo::status_count c = todo::status_counts(span_of(tree));
        expect(eq(c.pending, int32_t(1)));
        expect(eq(c.in_progress, int32_t(1)));
        expect(eq(c.done, int32_t(2)));
        expect(eq(todo::max_tree_depth(span_of(tree)), int32_t(2)));

        const kimix::vector<kimix::string> titles =
            todo::collect_titles(span_of(tree));
        expect(eq(titles.size(), size_t(4)));
        expect(eq(titles[0], kimix::string("A")));   // DFS pre-order
        expect(eq(titles[1], kimix::string("A1")));
        expect(eq(titles[2], kimix::string("A2")));
        expect(eq(titles[3], kimix::string("B")));

        kimix::vector<int32_t> path;
        expect(todo::find_path(span_of(tree), "A2", path));
        expect(eq(path.size(), size_t(2)));
        expect(eq(path[0], int32_t(0)));
        expect(eq(path[1], int32_t(1)));
        const todo::todo_item *node = todo::node_at_path(
            tree, kimix::span<const int32_t>(path.data(), path.size()));
        expect(node != nullptr);
        if (node != nullptr) {
            expect(eq(node->content, kimix::string("A2")));
            expect(node->notes.has_value());
        }
        path.clear();
        expect(!todo::find_path(span_of(tree), "nope", path));
        const int32_t bad[] = {7};
        expect(todo::node_at_path(
                   tree, kimix::span<const int32_t>(bad, 1)) == nullptr);

        // Duplicates (root level, sorted).
        kimix::vector<todo::todo_item> dups;
        dups.push_back(mk("b", todo::todo_status::pending));
        dups.push_back(mk("a", todo::todo_status::pending));
        dups.push_back(mk("b", todo::todo_status::pending));
        const kimix::vector<kimix::string> d =
            todo::find_duplicate_titles(span_of(dups));
        expect(eq(d.size(), size_t(1)));
        expect(eq(d[0], kimix::string("b")));
        expect(todo::find_duplicate_titles(span_of(tree)).empty());

        // Single in_progress + auto-fix (keep LAST in DFS pre-order).
        expect(todo::find_in_progress_conflicts(span_of(tree)).empty());
        tree[0].status = todo::todo_status::in_progress; // A + A2 both active
        const kimix::vector<kimix::string> conflicts =
            todo::find_in_progress_conflicts(span_of(tree));
        expect(eq(conflicts.size(), size_t(2)));
        expect(eq(conflicts[0], kimix::string("A")));
        expect(eq(conflicts[1], kimix::string("A2")));
        kimix::vector<kimix::string> warnings;
        todo::auto_fix_in_progress(tree, warnings);
        expect(eq(warnings.size(), size_t(1)));
        expect(has(warnings[0], "Auto-fixed \"A\""));
        expect(has(warnings[0], "keeping \"A2\" in_progress"));
        expect(tree[0].status == todo::todo_status::done);
        expect(tree[0].children[1].status == todo::todo_status::in_progress);

        // mark_subtree_done + unfinished descendants.
        todo::mark_subtree_done(tree[0]);
        expect(tree[0].status == todo::todo_status::done);
        expect(tree[0].children[0].status == todo::todo_status::done);
        expect(tree[0].children[1].status == todo::todo_status::done);
        expect(eq(todo::count_unfinished_descendants(tree[0]), int32_t(0)));
    };

    // =======================================================================
    // Rendering
    // =======================================================================
    "todo_render_helpers"_test = [] {
        kimix::vector<todo::todo_item> tree;
        tree.push_back(mk("A", todo::todo_status::in_progress,
                          kimix::string("doing A")));
        tree[0].children.push_back(mk("A1", todo::todo_status::pending));
        tree.push_back(mk("B", todo::todo_status::done));
        tree.push_back(mk("C", todo::todo_status::pending));

        // format_todos: root-level pending/in_progress only, "in progress"
        // display name + Notes suffix for the active item.
        const kimix::string active = todo::format_todos(span_of(tree));
        expect(eq(active, kimix::string("- [in progress] A  Notes: doing A\n"
                                        "- [pending] C")));

        // render_read_tree: all statuses, canonical names, 2-space indent.
        const kimix::string rendered = todo::render_read_tree(span_of(tree));
        expect(eq(rendered, kimix::string("- [in_progress] A  Notes: doing A\n"
                                          "  - [pending] A1\n"
                                          "- [done] B\n"
                                          "- [pending] C")));
        // max_lines truncation (DFS stop).
        const kimix::string short_render =
            todo::render_read_tree(span_of(tree), 2);
        expect(eq(short_render, kimix::string("- [in_progress] A  Notes: doing A\n"
                                              "  - [pending] A1")));

        // Display items: flattened DFS with depth.
        const kimix::vector<todo::display_item> items =
            todo::build_display_items(span_of(tree));
        expect(eq(items.size(), size_t(4)));
        expect(eq(items[0].title, kimix::string("A")));
        expect(eq(items[0].depth, int32_t(0)));
        expect(eq(items[1].title, kimix::string("A1")));
        expect(eq(items[1].depth, int32_t(1)));
        expect(items[2].title == "B" && items[2].depth == 0);

        // truncate_prompt: >200 code points -> 100 + "..." + 100.
        kimix::string long_prompt(250, 'x');
        const kimix::string trunc = todo::truncate_prompt(long_prompt);
        expect(eq(trunc.size(), size_t(203)));
        expect(has(trunc, "xxx...xxx"));
        const kimix::string short_prompt = "keep me";
        expect(eq(todo::truncate_prompt(short_prompt), short_prompt));
        // UTF-8 aware: 250 two-byte code points.
        kimix::string utf(250 * 2, '\0');
        for (size_t i = 0; i < 250; ++i) {
            utf[2 * i] = char(0xC3);
            utf[2 * i + 1] = char(0xA9);
        }
        const kimix::string utf_trunc = todo::truncate_prompt(utf);
        expect(eq(kimix::builtin_tools::utf8_code_point_count(utf_trunc),
                  size_t(203)));
    };

    // =======================================================================
    // Parameter parsing
    // =======================================================================
    "todo_parse_write_params"_test = [] {
        todo::tool_response err;
        { // canonical + aliases
            todo::write_params p;
            ToolParams args = parse_json(
                R"JSON({"todos":[{"content":"A","status":"pending"}],"mode":"replace","force":true,"auto_fix":false})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(p.has_todos);
            expect(eq(p.todos.size(), size_t(1)));
            expect(p.mode == todo::write_mode::replace);
            expect(p.force);
            expect(!p.auto_fix);
        }
        { // items alias + title alias + completed status + description notes
            todo::write_params p;
            ToolParams args = parse_json(
                R"JSON({"items":[{"title":"A","status":"completed","description":"  dn  "}]})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(p.has_todos);
            expect(eq(p.todos.size(), size_t(1)));
            expect(eq(p.todos[0].content, kimix::string("A")));
            expect(p.todos[0].status == todo::todo_status::done);
            expect(p.todos[0].notes.has_value());
            expect(eq(*p.todos[0].notes, kimix::string("dn")));
            expect(!p.todos[0].children_provided);
        }
        { // single object + explicit children
            todo::write_params p;
            ToolParams args = parse_json(
                R"JSON({"todos":{"content":"A","status":"pending","children":[{"content":"A1","status":"done"}]}})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(eq(p.todos.size(), size_t(1)));
            expect(p.todos[0].children_provided);
            expect(eq(p.todos[0].children.size(), size_t(1)));
        }
        { // embedded JSON string (repair path)
            todo::write_params p;
            ToolParams args = parse_json(
                R"JSON({"todos":"[{\"content\":\"A\",\"status\":\"pending\"},]"})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(eq(p.todos.size(), size_t(1)));
            expect(eq(p.todos[0].content, kimix::string("A")));
        }
        { // mode aliases: {"replace": true}
            todo::write_params p;
            ToolParams args = parse_json(
                R"JSON({"todos":[{"content":"A","status":"pending"}],"replace":true})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(p.mode == todo::write_mode::replace);
        }
        { // legacy force mode
            todo::write_params p;
            ToolParams args = parse_json(R"JSON({"mode":"force_overwrite"})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(p.mode == todo::write_mode::replace);
            expect(p.force);
        }
        { // absent todos -> read mode
            todo::write_params p;
            ToolParams args = parse_json(R"JSON({"mode":"append"})JSON");
            expect(todo::parse_write_params(&args, p, err));
            expect(!p.has_todos);
            expect(todo::parse_write_params(nullptr, p, err));
            expect(!p.has_todos);
        }
        { // errors
            todo::write_params p;
            ToolParams bad_mode = parse_json(R"JSON({"mode":"bogus"})JSON");
            expect(!todo::parse_write_params(&bad_mode, p, err));
            expect(err.is_error);
            expect(has(err.output, "Invalid mode 'bogus'"));
            expect(has(err.output, "\nHint: "));
            ToolParams bad_status = parse_json(
                R"JSON({"todos":[{"content":"A","status":"bogus"}]})JSON");
            expect(!todo::parse_write_params(&bad_status, p, err));
            expect(has(err.output, "Invalid status 'bogus'"));
            ToolParams blank = parse_json(
                R"JSON({"todos":[{"content":"   ","status":"pending"}]})JSON");
            expect(!todo::parse_write_params(&blank, p, err));
            expect(has(err.output, "Title cannot be empty"));
            ToolParams missing = parse_json(
                R"JSON({"todos":[{"content":"A"}]})JSON");
            expect(!todo::parse_write_params(&missing, p, err));
            expect(has(err.output, "Invalid todo at index 0: Field required"));
            ToolParams not_obj = parse_json(R"JSON({"todos":[5]})JSON");
            expect(!todo::parse_write_params(&not_obj, p, err));
            expect(has(err.output, "Invalid todo at index 0"));
            ToolParams bad_type = parse_json(R"JSON({"todos":5})JSON");
            expect(!todo::parse_write_params(&bad_type, p, err));
            expect(has(err.output,
                       "todos must be a list of todos, a single todo"));
        }
    };

    "todo_parse_update_params"_test = [] {
        todo::tool_response err;
        { // single top-level with aliases
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"task":"A","status":"completed","notes":"n","parent":"P","fuzzy":false,"force":true,"complete":true})JSON");
            expect(todo::parse_update_params(&args, p, err));
            expect(eq(p.ops.size(), size_t(1)));
            const todo::update_op &op = p.ops[0];
            expect(eq(op.title, kimix::string("A")));
            expect(op.has_status && op.status == todo::todo_status::done);
            expect(op.has_notes && op.notes == "n");
            expect(op.has_parent && op.parent == "P");
            expect(!op.fuzzy);
            expect(op.force);
            expect(op.complete);
        }
        { // batch + common parent applied only to ops without one
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"updates":[{"title":"A","status":"done"},{"title":"B","parent":"Q"}],"parent":"P"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            expect(eq(p.ops.size(), size_t(2)));
            expect(p.ops[0].has_parent && p.ops[0].parent == "P");
            expect(p.ops[1].has_parent && p.ops[1].parent == "Q");
        }
        { // updates aliases: todos / edits
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"edits":[{"title":"A"}]})JSON");
            expect(todo::parse_update_params(&args, p, err));
            expect(eq(p.ops.size(), size_t(1)));
            ToolParams args2 =
                parse_json(R"JSON({"todos":{"title":"A"}})JSON");
            expect(todo::parse_update_params(&args2, p, err));
            expect(eq(p.ops.size(), size_t(1)));
        }
        { // mixed top-level + updates
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"updates":[{"title":"A"}],"title":"B","status":"done"})JSON");
            expect(!todo::parse_update_params(&args, p, err));
            expect(has(err.output, "Cannot mix top-level ['status', 'title']"));
        }
        { // missing title
            todo::update_params p;
            ToolParams args = parse_json(R"JSON({"status":"done"})JSON");
            expect(!todo::parse_update_params(&args, p, err));
            expect(eq(err.message, kimix::string("No todo title provided.")));
            expect(has(err.output, "title is required when updates is not provided"));
            expect(!todo::parse_update_params(nullptr, p, err));
        }
        { // rename_to whitespace -> absent
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"A","rename_to":"   "})JSON");
            expect(todo::parse_update_params(&args, p, err));
            expect(!p.ops[0].has_rename);
        }
    };

    // =======================================================================
    // write flow
    // =======================================================================
    "todo_write_read_empty"_test = [] {
        todo::todo_state st;
        const todo::tool_response r = todo::read_todos(st);
        expect(!r.is_error);
        expect(has(r.output, "Todo list is empty."));
        expect(has(r.output, "Next: todo_update to edit one or more items"));
        expect(eq(r.message, kimix::string("Todo list is empty.")));
        expect(r.display.empty());

        st.archived_todos.push_back(mk("old", todo::todo_status::done));
        const todo::tool_response r2 = todo::read_todos(st);
        expect(has(r2.output, "Archived: 1 completed todo(s)."));
    };

    "todo_write_append_new"_test = [] {
        todo::todo_state st;
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"in_progress","notes":"na"},{"content":"B","status":"pending"},{"content":"C","status":"pending"}]})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(!cr.response.is_error) << cr.response.output;
        expect(cr.commit);
        expect(has(cr.response.output,
                   "Todo list appended (3 total: 0 done, 1 in progress, 2 "
                   "pending)"));
        expect(has(cr.response.output, "- [in progress] A  Notes: na"));
        expect(has(cr.response.output, "- [pending] B"));
        expect(has(cr.response.output, "\nNext: "));
        expect(eq(cr.response.message, kimix::string("Todo list appended.")));
        expect(eq(cr.todos.size(), size_t(3)));
        expect(eq(cr.response.display.size(), size_t(3)));
    };

    "todo_write_append_merge"_test = [] {
        todo::todo_state st;
        // old: A(pending, notes "oldnote", child C1), B(in_progress)
        st.todos.push_back(mk("A", todo::todo_status::pending,
                              kimix::string("oldnote")));
        st.todos[0].children.push_back(mk("C1", todo::todo_status::pending));
        st.todos[0].children_provided = true;
        st.todos.push_back(mk("B", todo::todo_status::in_progress));

        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"done"},{"content":"N","status":"pending"}]})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(!cr.response.is_error) << cr.response.output;
        // Order preserved: A, B, then appended N.
        expect(eq(cr.todos.size(), size_t(3)));
        expect(eq(cr.todos[0].content, kimix::string("A")));
        expect(eq(cr.todos[1].content, kimix::string("B")));
        expect(eq(cr.todos[2].content, kimix::string("N")));
        // Status updated; notes + children kept (omitted in the new item).
        expect(cr.todos[0].status == todo::todo_status::done);
        expect(cr.todos[0].notes.has_value());
        expect(eq(*cr.todos[0].notes, kimix::string("oldnote")));
        expect(eq(cr.todos[0].children.size(), size_t(1)));
        // Recursive counts: A(done) C1(pending) B(in_progress) N(pending).
        expect(has(cr.response.output,
                   "Todo list appended (4 total: 1 done, 1 in progress, 2 "
                   "pending)"));

        // Explicit notes replace; explicit empty children clear.
        todo::todo_state st2;
        st2.todos.push_back(mk("A", todo::todo_status::pending,
                               kimix::string("oldnote")));
        st2.todos[0].children.push_back(mk("C1", todo::todo_status::pending));
        todo::write_params p2;
        ToolParams args2 = parse_json(
            R"JSON({"todos":[{"content":"A","status":"pending","notes":" new ","children":[]}]})JSON");
        expect(todo::parse_write_params(&args2, p2, err));
        todo::commit_result cr2 = todo::write_todos(st2, p2);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(eq(*cr2.todos[0].notes, kimix::string("new")));
        expect(cr2.todos[0].children.empty());
        // Empty append list is a no-op.
        todo::write_params p3;
        ToolParams args3 = parse_json(R"JSON({"todos":[]})JSON");
        expect(todo::parse_write_params(&args3, p3, err));
        todo::commit_result cr3 = todo::write_todos(st2, p3);
        expect(!cr3.response.is_error);
        expect(!cr3.commit);
        expect(has(cr3.response.output, "Todo list unchanged; no todos provided"));
        expect(eq(cr3.response.message,
                  kimix::string("No todos provided; todo list unchanged.")));
    };

    "todo_write_append_warnings"_test = [] {
        // Fuzzy near-duplicate warning (non-blocking).
        todo::todo_state st;
        st.todos.push_back(mk("Fix login bugs", todo::todo_status::pending));
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"Fix login bug","status":"pending"}]})JSON");

        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(!cr.response.is_error) << cr.response.output;
        expect(has(cr.response.message,
                   "\"Fix login bug\" looks like existing \"Fix login bugs\""));

        // Scope-duplicate warning: title exists deeper in the tree.
        todo::todo_state st2;
        st2.todos.push_back(mk("Deploy", todo::todo_status::pending));
        st2.todos[0].children.push_back(mk("Build", todo::todo_status::pending));
        todo::write_params p2;
        ToolParams args2 = parse_json(
            R"JSON({"todos":[{"content":"Build","status":"pending"}]})JSON");
        expect(todo::parse_write_params(&args2, p2, err));
        todo::commit_result cr2 = todo::write_todos(st2, p2);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(has(cr2.response.message,
                   "\"Build\" already exists in the tree (under \"Deploy\")"));
        expect(has(cr2.response.message,
                   "todo_update(parent=\"Deploy\", title=\"Build\")"));
        expect(eq(cr2.todos.size(), size_t(2))); // appended as a new root
    };


    "todo_write_duplicate_titles_error"_test = [] {
        todo::todo_state st;
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"pending"},{"content":"A","status":"done"}]})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output, "Duplicate todo titles found: ['A']"));
        expect(has(cr.response.output, "\nHint: todo_update(parent=...)"));
        expect(!cr.commit);
    };

    "todo_write_replace_guard_and_archive"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::pending));
        st.todos.push_back(mk("B", todo::todo_status::done));
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"N","status":"pending"}],"mode":"replace"})JSON");
        expect(todo::parse_write_params(&args, p, err));
        // Guard: unfinished old todos block the replace.
        todo::commit_result cr = todo::write_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output,
                   "Cannot replace todos while old todos are not all done"));
        expect(has(cr.response.output, "Unfinished:\nA"));
        expect(has(cr.response.output, "force=True"));

        // force=true bypasses the guard and archives dropped done items.
        todo::write_params pf;
        ToolParams args_f = parse_json(
            R"JSON({"todos":[{"content":"N","status":"pending"}],"mode":"replace","force":true})JSON");
        expect(todo::parse_write_params(&args_f, pf, err));
        todo::commit_result cr2 = todo::write_todos(st, pf);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(eq(cr2.todos.size(), size_t(1)));
        expect(eq(cr2.archived.size(), size_t(1))); // B archived, A not (pending)
        expect(eq(cr2.archived[0].content, kimix::string("B")));
        expect(has(cr2.response.message,
                   "Warning: force=True bypassed the all-done guard"));
    };

    "todo_write_clear_guard"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::pending));
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(R"JSON({"mode":"clear"})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output,
                   "Cannot clear todos while old todos are not all done"));
        expect(has(cr.response.output, "Unfinished:\nA"));

        // clear + todos is always a mistake.
        todo::write_params pc;
        ToolParams args_c = parse_json(
            R"JSON({"mode":"clear","todos":[{"content":"X","status":"pending"}]})JSON");
        expect(todo::parse_write_params(&args_c, pc, err));
        todo::commit_result crc = todo::write_todos(st, pc);
        expect(crc.response.is_error);
        expect(has(crc.response.output,
                   "mode='clear' cannot be combined with todos"));

        // All done -> clear succeeds and archives.
        todo::todo_state st2;
        st2.todos.push_back(mk("A", todo::todo_status::done));
        todo::commit_result cr2 = todo::write_todos(st2, p);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(cr2.todos.empty());
        expect(eq(cr2.archived.size(), size_t(1)));
        expect(has(cr2.response.output,
                   "Todo list cleared (0 total: 0 done, 0 in progress, 0 "
                   "pending)"));
        expect(!has(cr2.response.output, "Next:")); // 0-total: no hint
        expect(eq(cr2.response.message, kimix::string("Todo list cleared.")));
    };

    "todo_write_regression_guard"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::done));
        st.todos.push_back(mk("B", todo::todo_status::in_progress));
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"pending"},{"content":"B","status":"in_progress"}]})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output,
                   "Cannot regress completed todos back to "
                   "pending/in_progress: A"));
        expect(has(cr.response.output, "force=True"));

        // force=true allows the restart.
        todo::write_params pf;
        ToolParams args_f = parse_json(
            R"JSON({"todos":[{"content":"A","status":"pending"},{"content":"B","status":"in_progress"}],"force":true})JSON");
        expect(todo::parse_write_params(&args_f, pf, err));
        todo::commit_result cr2 = todo::write_todos(st, pf);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(cr2.todos[0].status == todo::todo_status::pending);
    };

    "todo_write_single_in_progress"_test = [] {
        todo::todo_state st;
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"in_progress"},{"content":"B","status":"in_progress"}],"auto_fix":false})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output,
                   "Multiple items are in_progress: ['A', 'B']"));
        expect(has(cr.response.output, "auto_fix=True"));

        // auto_fix (default): keep the LAST item, demote earlier ones.
        todo::write_params pa;
        ToolParams args_a = parse_json(
            R"JSON({"todos":[{"content":"A","status":"in_progress"},{"content":"B","status":"in_progress"}]})JSON");
        expect(todo::parse_write_params(&args_a, pa, err));
        todo::commit_result cr2 = todo::write_todos(st, pa);
        expect(!cr2.response.is_error) << cr2.response.output;
        expect(cr2.todos[0].status == todo::todo_status::done);
        expect(cr2.todos[1].status == todo::todo_status::in_progress);
        expect(has(cr2.response.message, "Auto-fixed \"A\""));
        expect(has(cr2.response.message, "keeping \"B\" in_progress"));
    };

    "todo_write_depth_cap"_test = [] {
        todo::todo_state st;
        todo::write_params p;
        todo::tool_response err;
        // depth == max_layers + 1 == 5 -> allowed.
        ToolParams ok_args = parse_json(
            R"JSON({"todos":[{"content":"L1","status":"pending","children":[{"content":"L2","status":"pending","children":[{"content":"L3","status":"pending","children":[{"content":"L4","status":"pending","children":[{"content":"L5","status":"pending"}]}]}]}]}]})JSON");
        expect(todo::parse_write_params(&ok_args, p, err));
        todo::commit_result cr = todo::write_todos(st, p);
        expect(!cr.response.is_error) << cr.response.output;
        // depth 6 -> blocked.
        todo::write_params p6;
        ToolParams bad_args = parse_json(
            R"JSON({"todos":[{"content":"L1","status":"pending","children":[{"content":"L2","status":"pending","children":[{"content":"L3","status":"pending","children":[{"content":"L4","status":"pending","children":[{"content":"L5","status":"pending","children":[{"content":"L6","status":"pending"}]}]}]}]}]}]})JSON");
        expect(todo::parse_write_params(&bad_args, p6, err));
        todo::commit_result cr6 = todo::write_todos(st, p6);
        expect(cr6.response.is_error);
        expect(has(cr6.response.output,
                   "Todo tree exceeds maximum nesting depth of 5 levels"));
        expect(has(cr6.response.output, "todo_max_layers=4"));
    };

    "todo_write_all_done_reminder"_test = [] {
        todo::todo_state st;
        todo::write_params p;
        todo::tool_response err;
        ToolParams args = parse_json(
            R"JSON({"todos":[{"content":"A","status":"completed"}],"mode":"replace"})JSON");
        expect(todo::parse_write_params(&args, p, err));
        todo::commit_result cr = todo::write_todos(st, p, "the original ask");
        expect(!cr.response.is_error) << cr.response.output;
        expect(has(cr.response.output, "All todos are done."));
        expect(has(cr.response.output, "Original prompt:\n\nthe original ask"));
        expect(has(cr.response.message, "All todos are done."));
    };

    // =======================================================================
    // read flow
    // =======================================================================
    "todo_read_rendering"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::done));
        st.todos.push_back(mk("B", todo::todo_status::in_progress,
                              kimix::string("nb")));
        st.todos[1].children.push_back(mk("B1", todo::todo_status::pending));
        st.archived_todos.push_back(mk("old", todo::todo_status::done));

        const todo::tool_response r = todo::read_todos(st);
        expect(!r.is_error);
        expect(has(r.output, "Current todo list:"));
        expect(has(r.output, "- [done] A"));
        expect(has(r.output, "- [in_progress] B  Notes: nb"));
        expect(has(r.output, "  - [pending] B1"));
        expect(has(r.output, "Archived: 1 completed todo(s)."));
        expect(has(r.output, "Next: todo_update"));
        expect(eq(r.message, kimix::string("Current todo list displayed.")));
        expect(r.display.empty()); // read mode has no display block

        // All done -> reminder in output AND message.
        todo::todo_state st2;
        st2.todos.push_back(mk("A", todo::todo_status::done));
        const todo::tool_response r2 = todo::read_todos(st2);
        expect(has(r2.output, "All todos are done."));
        expect(has(r2.message, "All todos are done."));

        // >100 items: truncated rendering + overflow line.
        todo::todo_state st3;
        for (int i = 0; i < 105; ++i) {
            st3.todos.push_back(
                mk(kimix::format("item {}", i), todo::todo_status::pending));
        }
        const todo::tool_response r3 = todo::read_todos(st3);
        expect(has(r3.output,
                   "... and 5 more (105 pending, 0 in_progress, 0 done total)"));
        expect(!has(r3.output, "item 104")); // rendering stopped at 100 lines
        expect(has(r3.output, "item 99"));
    };

    // =======================================================================
    // update flow
    // =======================================================================
    "todo_update_single_ops"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::pending));
        st.todos.push_back(mk("B", todo::todo_status::pending));
        todo::tool_response err;

        { // status
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"A","status":"in_progress"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(cr.todos[0].status == todo::todo_status::in_progress);
            expect(has(cr.response.output,
                       "Updated \"A\" (status=in_progress)."));
            expect(eq(cr.response.message, kimix::string("Updated \"A\".")));
            expect(has(cr.response.output, "Current todo list:"));
            expect(has(cr.response.output,
                       "Next: todo_update to edit another item"));
            st = todo::todo_state{};
            st.todos = cr.todos;
        }
        { // notes set + clear
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"A","notes":"  hello  "})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(cr.todos[0].notes.has_value());
            expect(eq(*cr.todos[0].notes, kimix::string("hello")));
            expect(has(cr.response.output, "notes updated"));
            st.todos = cr.todos;

            todo::update_params p2;
            ToolParams args2 = parse_json(R"JSON({"title":"A","notes":""})JSON");
            expect(todo::parse_update_params(&args2, p2, err));
            todo::commit_result cr2 = todo::update_todos(st, p2);
            expect(!cr2.response.is_error);
            expect(!cr2.todos[0].notes.has_value());
            st.todos = cr2.todos;
        }
        { // rename + no-op update
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"A","rename_to":"Z"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(eq(cr.todos[0].content, kimix::string("Z")));
            expect(has(cr.response.output, "renamed to \"Z\""));
            st.todos = cr.todos;

            todo::update_params p2;
            ToolParams args2 = parse_json(R"JSON({"title":"Z"})JSON");
            expect(todo::parse_update_params(&args2, p2, err));
            todo::commit_result cr2 = todo::update_todos(st, p2);
            expect(has(cr2.response.output, "Updated \"Z\" (no changes)."));
        }
        { // rename collision
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"Z","rename_to":"B"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "Cannot rename \"Z\" to \"B\": title already exists"));
            expect(!cr.commit);
        }
    };

    "todo_update_complete_subtree"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::in_progress));
        st.todos[0].children.push_back(mk("A1", todo::todo_status::pending));
        st.todos[0].children.push_back(mk("A2", todo::todo_status::pending));
        st.todos[0].children[0].children.push_back(
            mk("A1a", todo::todo_status::pending));
        todo::tool_response err;
        todo::update_params p;
        ToolParams args = parse_json(R"JSON({"title":"A","complete":true})JSON");
        expect(todo::parse_update_params(&args, p, err));
        todo::commit_result cr = todo::update_todos(st, p);
        expect(!cr.response.is_error) << cr.response.output;
        expect(has(cr.response.output,
                   "Updated \"A\" (completed with 4 sub-todos marked done)."));
        expect(cr.todos[0].status == todo::todo_status::done);
        expect(cr.todos[0].children[0].status == todo::todo_status::done);
        expect(cr.todos[0].children[0].children[0].status ==
               todo::todo_status::done);

        // complete + pending status is ambiguous -> error.
        todo::update_params p2;
        ToolParams args2 = parse_json(
            R"JSON({"title":"A","complete":true,"status":"pending"})JSON");
        expect(todo::parse_update_params(&args2, p2, err));
        todo::commit_result cr2 = todo::update_todos(st, p2);
        expect(cr2.response.is_error);
        expect(has(cr2.response.output,
                   "complete=True cannot be combined with status=\"pending\""));
    };

    "todo_update_regression_guard"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::done));
        todo::tool_response err;
        todo::update_params p;
        ToolParams args =
            parse_json(R"JSON({"title":"A","status":"in_progress"})JSON");
        expect(todo::parse_update_params(&args, p, err));
        todo::commit_result cr = todo::update_todos(st, p);
        expect(cr.response.is_error);
        expect(has(cr.response.output,
                   "Cannot regress completed todo \"A\" back to in_progress"));
        expect(has(cr.response.output, "force=True"));

        todo::update_params pf;
        ToolParams args_f = parse_json(
            R"JSON({"title":"A","status":"in_progress","force":true})JSON");
        expect(todo::parse_update_params(&args_f, pf, err));
        todo::commit_result crf = todo::update_todos(st, pf);
        expect(!crf.response.is_error) << crf.response.output;
        expect(crf.todos[0].status == todo::todo_status::in_progress);
    };

    "todo_update_fuzzy_match"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("Fix login bug", todo::todo_status::pending));
        todo::tool_response err;
        { // fuzzy default: near-miss title matches + warning
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"fix login bugs","status":"done"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(has(cr.response.output,
                       "Fuzzy matched \"fix login bugs\" to \"Fix login bug\""));
            expect(has(cr.response.output, "Updated \"Fix login bug\""));
            expect(cr.todos[0].status == todo::todo_status::done);
        }
        { // fuzzy=false: exact-only -> not found
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"fix login bugs","status":"done","fuzzy":false})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "No todo titled \"fix login bugs\" found"));
            expect(has(cr.response.output, "fuzzy=True"));
        }
        { // nothing close -> "No todo matching"
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"zzzz qqqq xyzzy"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "No todo matching \"zzzz qqqq xyzzy\" found"));
        }
        { // empty tree, no parent -> "No todos exist."
            todo::todo_state empty;
            todo::update_params p;
            ToolParams args = parse_json(R"JSON({"title":"A"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(empty, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output, "No todos exist."));
            expect(eq(cr.response.message,
                      kimix::string("No todos to update.")));
        }
    };

    "todo_update_parent_scope"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("Deploy", todo::todo_status::pending));
        st.todos[0].children.push_back(mk("Build", todo::todo_status::pending));
        todo::tool_response err;
        { // update an existing child
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"Build","parent":"Deploy","status":"done"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(cr.todos[0].children[0].status == todo::todo_status::done);
            expect(has(cr.response.output, "Updated \"Build\""));
        }
        { // create a new child
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"Test","parent":"Deploy","status":"in_progress","notes":"nt"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(eq(cr.todos[0].children.size(), size_t(2)));
            expect(has(cr.response.output,
                       "Created \"Test\" under \"Deploy\"."));
            expect(cr.todos[0].children[1].notes.has_value());
            st.todos = cr.todos;
        }
        { // create a root item with parent=""
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"Docs","parent":""})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(eq(cr.todos.size(), size_t(2)));
            expect(has(cr.response.output, "Created \"Docs\" under \"root\"."));
            st.todos = cr.todos;
        }
        { // parent not found (fuzzy off / no near match)
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"X","parent":"nope","fuzzy":false})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "No parent todo titled \"nope\" found"));
            todo::update_params p2;
            ToolParams args2 =
                parse_json(R"JSON({"title":"X","parent":"nope"})JSON");
            expect(todo::parse_update_params(&args2, p2, err));
            todo::commit_result cr2 = todo::update_todos(st, p2);
            expect(cr2.response.is_error);
            expect(has(cr2.response.output,
                       "No parent todo matching \"nope\" found"));
        }
        { // fuzzy parent match
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"check","parent":"Deployy"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(has(cr.response.output,
                       "Fuzzy matched parent \"Deployy\" to \"Deploy\""));
            expect(has(cr.response.output, "Created \"check\" under \"Deploy\""));
            st.todos = cr.todos;
        }
        { // rename / complete a non-existent child
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"Ghost","parent":"Deploy","rename_to":"X"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "\"Ghost\" does not exist under \"Deploy\""));
            expect(eq(cr.response.message,
                      kimix::string("Cannot rename non-existent todo \"Ghost\".")));
            todo::update_params p2;
            ToolParams args2 = parse_json(
                R"JSON({"title":"Ghost","parent":"Deploy","complete":true})JSON");
            expect(todo::parse_update_params(&args2, p2, err));
            todo::commit_result cr2 = todo::update_todos(st, p2);
            expect(cr2.response.is_error);
            expect(has(cr2.response.output,
                       "\"Ghost\" does not exist under \"Deploy\""));
            expect(eq(cr2.response.message,
                      kimix::string(
                          "Cannot complete non-existent todo \"Ghost\".")));
        }
    };

    "todo_update_depth_guard"_test = [] {
        // Build a depth-5 chain L1..L5 through the kernel.
        todo::todo_state st;
        st.todos.push_back(mk("L1", todo::todo_status::pending));
        todo::todo_item *n = &st.todos[0];
        for (int i = 2; i <= 5; ++i) {
            n->children.push_back(mk(kimix::format("L{}", i),
                                     todo::todo_status::pending));
            n = &n->children.back();
        }
        todo::tool_response err;
        { // child under L4 (depth 4) -> new depth 5, allowed
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"L4b","parent":"L4"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(has(cr.response.output, "Created \"L4b\" under \"L4\"."));
        }
        { // child under L5 (depth 5 > max_layers 4) -> blocked
            todo::update_params p;
            ToolParams args =
                parse_json(R"JSON({"title":"L6","parent":"L5"})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(has(cr.response.output,
                       "\"L5\" is at depth 5; children would exceed the "
                       "maximum depth (5)"));
            expect(has(cr.response.output,
                       "Cannot add children deeper than 5 layers."));
        }
    };

    "todo_update_batch_and_autofix"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::in_progress));
        st.todos.push_back(mk("B", todo::todo_status::pending));
        todo::tool_response err;
        { // batch: complete A + start B (no auto-fix conflict afterwards)
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"updates":[{"title":"A","status":"done"},{"title":"B","status":"in_progress"}]})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(eq(cr.response.message,
                      kimix::string("Updated \"A\".; Updated \"B\".")));
            expect(cr.todos[0].status == todo::todo_status::done);
            expect(cr.todos[1].status == todo::todo_status::in_progress);
            st.todos = cr.todos;
        }
        { // conflict -> automatic fix with a warning in the OUTPUT
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"title":"A","status":"in_progress","force":true})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(!cr.response.is_error) << cr.response.output;
            expect(cr.todos[0].status == todo::todo_status::done);
            expect(cr.todos[1].status == todo::todo_status::in_progress);
            expect(has(cr.response.output, "Auto-fixed \"A\": set to done"));
            expect(has(cr.response.output, "keeping \"B\" in_progress"));
        }
        { // error mid-batch aborts without committing
            todo::update_params p;
            ToolParams args = parse_json(
                R"JSON({"updates":[{"title":"A","status":"done"},{"title":"Ghost","fuzzy":false}]})JSON");
            expect(todo::parse_update_params(&args, p, err));
            todo::commit_result cr = todo::update_todos(st, p);
            expect(cr.response.is_error);
            expect(!cr.commit);
            expect(has(cr.response.output, "No todo titled \"Ghost\" found"));
        }
    };

    // =======================================================================
    // State serialization + persistence
    // =======================================================================
    "todo_state_json_roundtrip"_test = [] {
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::in_progress,
                              kimix::string("na")));
        st.todos[0].children.push_back(mk("A1", todo::todo_status::pending));
        st.todos.push_back(mk("B", todo::todo_status::done));
        st.archived_todos.push_back(mk("old", todo::todo_status::done,
                                       kimix::string("arch")));

        const kimix::string json = todo::serialize_state(st);
        expect(has(json, "\"todos\""));
        expect(has(json, "\"archived_todos\""));
        expect(has(json, "\"title\":\"A\""));
        expect(has(json, "\"status\":\"in_progress\""));
        expect(has(json, "\"notes\":null"));

        todo::todo_state back;
        kimix::string err;
        expect(todo::deserialize_state(json, back, err)) << err;
        expect(eq(back.todos.size(), size_t(2)));
        expect(eq(back.todos[0].content, kimix::string("A")));
        expect(back.todos[0].status == todo::todo_status::in_progress);
        expect(back.todos[0].notes.has_value());
        expect(eq(*back.todos[0].notes, kimix::string("na")));
        expect(eq(back.todos[0].children.size(), size_t(1)));
        expect(eq(back.todos[0].children[0].content, kimix::string("A1")));
        expect(back.todos[1].status == todo::todo_status::done);
        expect(eq(back.archived_todos.size(), size_t(1)));
        expect(eq(back.archived_todos[0].content, kimix::string("old")));

        // Lenient loading: malformed items are skipped, good ones survive.
        todo::todo_state lenient;
        const kimix::string mixed =
            R"JSON({"todos":[{"title":"good","status":"pending"},{"status":"pending"},{"title":"","status":"pending"},{"title":"bad","status":"nope"},{"title":"ok2","status":"completed"}]})JSON";
        expect(todo::deserialize_state(mixed, lenient, err)) << err;
        expect(eq(lenient.todos.size(), size_t(2)));
        expect(eq(lenient.todos[0].content, kimix::string("good")));
        expect(eq(lenient.todos[1].content, kimix::string("ok2")));
        expect(lenient.todos[1].status == todo::todo_status::done);

        // Invalid envelope.
        todo::todo_state bad;
        expect(!todo::deserialize_state("not json", bad, err));
        expect(!err.empty());
        expect(!todo::deserialize_state("[]", bad, err));
    };

    "todo_state_file_persistence"_test = [] {
        const kimix::string dir = tmp_dir("kimix_todo_state_test");
        const kimix::string path = todo::state_file_path(dir);
        expect(has(path, "state.json"));

        // Missing file -> true with an empty state.
        todo::todo_state loaded;
        kimix::string err;
        expect(todo::load_state_file(path, loaded, err)) << err;
        expect(loaded.todos.empty());

        // Save + load round-trip; parent dirs are created.
        todo::todo_state st;
        st.todos.push_back(mk("A", todo::todo_status::pending));
        st.archived_todos.push_back(mk("Z", todo::todo_status::done));
        expect(todo::save_state_file(path, st, err)) << err;
        expect(kimix::filesystem::exists(kimix::filesystem::path(path)));
        expect(todo::load_state_file(path, loaded, err)) << err;
        expect(eq(loaded.todos.size(), size_t(1)));
        expect(eq(loaded.archived_todos.size(), size_t(1)));

        // Unknown sibling keys survive a save (subagent-state merge parity).
        {
            std::FILE *f = std::fopen(path.c_str(), "wb");
            const char *with_extra =
                R"({"custom_key":42,"todos":[{"title":"A","status":"pending","notes":null,"children":[]}],"archived_todos":[{"title":"Z","status":"done","notes":null,"children":[]}]})";
            std::fwrite(with_extra, 1, std::strlen(with_extra), f);
            std::fclose(f);
        }
        todo::todo_state st2;
        st2.todos.push_back(mk("B", todo::todo_status::done));
        expect(todo::save_state_file(path, st2, err)) << err;
        const kimix::string text = read_file_text(path);
        expect(has(text, "custom_key"));
        expect(has(text, "\"title\":\"B\""));
        expect(!has(text, "\"title\":\"A\"")); // todos key replaced

        // Corrupt file -> load fails; session falls back to defaults.
        {
            std::FILE *f = std::fopen(path.c_str(), "wb");
            const char *junk = "{not json!!";
            std::fwrite(junk, 1, std::strlen(junk), f);
            std::fclose(f);
        }
        todo::todo_state st3;
        expect(!todo::load_state_file(path, st3, err));
        expect(!err.empty());

        Session session;
        session.state_dir = dir;
        kimix::string warning;
        todo::todo_state &mem =
            todo::session_todos(session, false, &warning);
        expect(mem.todos.empty()); // corrupt -> defaults
        expect(has(warning, "Corrupted todo state"));

        // No temp files left behind.
        std::error_code ec;
        expect(!kimix::filesystem::exists(kimix::filesystem::path(path + ".tmp"),
                                          ec));
    };

    // =======================================================================
    // Tool interface + session persistence
    // =======================================================================
    "todo_tool_write_read_update_flow"_test = [] {
        const kimix::string dir = tmp_dir("kimix_todo_tool_test");
        Session session;
        session.state_dir = dir;

        todo::TodoWrite w(&session);
        const ToolParams r1 = run_tool(
            w,
            R"JSON({"todos":[{"content":"Design","status":"in_progress","notes":"d"},{"content":"Build","status":"pending"},{"content":"Test","status":"pending"}]})JSON");
        expect(eq(res_str(r1, "status"), kimix::string("ok")));
        expect(!res_err(r1));
        expect(has(res_str(r1, "output"), "Todo list appended (3 total"));
        const ValueElement *todos_el = r1.get("todos");
        expect(todos_el != nullptr && todos_el->is_array());
        if (todos_el != nullptr) {
            expect(eq(todos_el->as_array().size(), size_t(3)));
            const ToolParams *first = todos_el->as_array()[0].as_object();
            expect(first != nullptr);
            if (first != nullptr) {
                expect(eq(first->get("title")->as_string(),
                          kimix::string("Design")));
                expect(eq(first->get("depth")->as_int(), int64_t(0)));
            }
        }
        // The state file exists and holds the tree.
        const kimix::string state_text =
            read_file_text(todo::state_file_path(dir));
        expect(has(state_text, "\"title\":\"Design\""));

        // Read mode (no todos key).
        const ToolParams r2 = run_tool(w, R"JSON({})JSON");
        expect(!res_err(r2));
        expect(has(res_str(r2, "output"), "Current todo list:"));
        expect(has(res_str(r2, "output"), "- [in_progress] Design  Notes: d"));

        // A FRESH session on the same state_dir reloads the list from disk
        // (load-with-session).
        Session session2;
        session2.state_dir = dir;
        todo::TodoWrite w2(&session2);
        const ToolParams r3 = run_tool(w2, R"JSON({})JSON");
        expect(has(res_str(r3, "output"), "Design"));
        expect(has(res_str(r3, "output"), "Test"));

        // TodoUpdate on session2 marks Design done; the file follows.
        todo::TodoUpdate u(&session2);
        const ToolParams r4 =
            run_tool(u, R"JSON({"title":"Design","status":"done"})JSON");
        expect(!res_err(r4)) << res_str(r4, "output");
        expect(has(res_str(r4, "output"), "Updated \"Design\" (status=done)."));
        todo::todo_state persisted;
        kimix::string perr;
        expect(todo::load_state_file(todo::state_file_path(dir), persisted,
                                     perr))
            << perr;
        expect(eq(persisted.todos.size(), size_t(3)));
        if (!persisted.todos.empty()) {
            expect(persisted.todos[0].status == todo::todo_status::done);
        }

        // Errors surface through the envelope.
        const ToolParams r5 = run_tool(u, R"JSON({"status":"done"})JSON");
        expect(res_err(r5));
        expect(eq(res_str(r5, "status"), kimix::string("invalid_input")));
        expect(has(res_str(r5, "output"), "title is required"));

        // Null session -> unsupported, no crash.
        todo::TodoWrite nosess(nullptr);
        const ToolParams r6 = run_tool(nosess, R"JSON({})JSON");
        expect(res_err(r6));
        expect(eq(res_str(r6, "status"), kimix::string("unsupported")));
    };

    "todo_agent_session_state"_test = [] {
        const kimix::string ws = tmp_dir("kimix_todo_agent_ws");
        const kimix::string dir = ws + "/session_state";

        { // Session #1: drive the tool through an AgentSession tool_session.
            kimix::agent::AgentSession as(ws);
            as.set_state_dir(dir);
            expect(eq(as.state_dir(), dir));
            expect(eq(as.tool_session().state_dir, dir));

            todo::TodoWrite w(&as.tool_session());
            const ToolParams r = run_tool(
                w,
                R"JSON({"todos":[{"content":"Step one","status":"in_progress"},{"content":"Step two","status":"pending"}]})JSON");
            expect(!res_err(r)) << res_str(r, "output");
            // The tool persists on success; save_state() re-persists the same
            // in-memory state (idempotent).
            kimix::string err;
            expect(as.save_state(err)) << err;
            expect(kimix::filesystem::exists(
                kimix::filesystem::path(todo::state_file_path(dir))));
        }
        { // Session #2: load_state() restores the todo list.
            kimix::agent::AgentSession as2(ws);
            as2.set_state_dir(dir);
            kimix::string err;
            expect(as2.load_state(err)) << err;
            const todo::todo_state *st = as2.tool_session().todo_state.get();
            expect(st != nullptr);
            if (st != nullptr) {
                expect(eq(st->todos.size(), size_t(2)));
                expect(eq(st->todos[0].content, kimix::string("Step one")));
                expect(st->todos[0].status == todo::todo_status::in_progress);
            }
            // A tool bound to the reloaded session sees the same tree.
            todo::TodoWrite w(&as2.tool_session());
            const ToolParams r = run_tool(w, R"JSON({})JSON");
            expect(has(res_str(r, "output"), "Step one"));
            expect(has(res_str(r, "output"), "Step two"));
            // ... and continues to persist updates.
            todo::TodoUpdate u(&as2.tool_session());
            const ToolParams r2 = run_tool(
                u, R"JSON({"title":"Step one","status":"done"})JSON");
            expect(!res_err(r2)) << res_str(r2, "output");
            expect(has(read_file_text(todo::state_file_path(dir)),
                       "\"status\":\"done\""));
        }
        { // Without a state_dir everything stays in memory (no files).
            kimix::agent::AgentSession as3(ws);
            kimix::string err;
            expect(!as3.save_state(err));
            expect(!err.empty());
            expect(!as3.load_state(err));
            todo::TodoWrite w(&as3.tool_session());
            const ToolParams r = run_tool(
                w, R"JSON({"todos":[{"content":"mem","status":"pending"}]})JSON");
            expect(!res_err(r));
            expect(!kimix::filesystem::exists(
                kimix::filesystem::path(todo::state_file_path(
                    as3.tool_session().state_dir))));
        }
    };

    // =======================================================================
    // Registry integration
    // =======================================================================
    "todo_registry_entries"_test = [] {
        auto &reg = kimix::builtin_tools::ToolRegistry::instance();
        for (const char *name : {"TodoWrite", "TodoUpdate"}) {
            const auto *meta = reg.find(kimix::string_view(name));
            expect(meta != nullptr) << name;
            if (meta != nullptr) {
                expect(eq(meta->name, kimix::string(name)));
                expect(!meta->description.empty());
                expect(meta->parameters_json.find("properties") !=
                       kimix::string::npos);
            }
        }
        // Case-insensitive lookup + factory.
        Session session;
        auto tool = reg.create("todowrite", &session);
        expect(tool != nullptr);
        auto tool2 = reg.create("TodoUpdate", &session);
        expect(tool2 != nullptr);
        // Schemas advertise the documented parameter names.
        const auto *wm = reg.find("TodoWrite");
        if (wm != nullptr) {
            expect(has(wm->parameters_json, "\"todos\""));
            expect(has(wm->parameters_json, "\"mode\""));
            expect(has(wm->parameters_json, "auto_fix"));
        }
        const auto *um = reg.find("TodoUpdate");
        if (um != nullptr) {
            expect(has(um->parameters_json, "\"updates\""));
            expect(has(um->parameters_json, "rename_to"));
            expect(has(um->parameters_json, "complete"));
        }
    };
}
