// todo_tool.h - C++ port of the kimi-cli todo list tools (todo_write +
// todo_update) with session-scoped persistence.
//
// Python source of truth:
//   C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/todo/__init__.py
//     Todo model + aliases                 133-222
//     Params (todo_write)                  224-340
//     TodoList._enforce_single_in_progress 389-407
//     TodoList._auto_fix_in_progress       409-442
//     TodoList._write_todos                444-588
//     TodoList._find_duplicate_titles      611-621
//     TodoList._format_todos               623-650
//     TodoList._find_nearest_titles        663-712
//     TodoList._merge_todos (+helpers)     714-879
//     TodoList._check_regressions          881-914
//     TodoList._build_success_response     916-979
//     TodoList._status_counts/_count_all   981-1003
//     TodoList._build_display_block        1027-1048
//     TodoList._render_read_tree/_read     1052-1134
//     TodoList persistence                 1136-1287
//     TodoUpdateItem / TodoUpdateParams    1291-1504
//     todo_update.__call__                 1539-1592
//     todo_update._normalize_update_items  1594-1631
//     todo_update._apply_one_update        1633-1661
//     todo_update._update_global_in_memory 1663-1701
//     todo_update._update_or_create_under_parent_in_memory 1703-1798
//     todo_update._apply_update_to_tree    1800-1895
//     todo_update tree helpers             1897-1961
//   C:/dev/kimi-agent/kimi-cli/src/kimi_cli/session_state.py
//     TodoItemState (title/status/notes/children)  25-32
//     STATE_FILE_NAME = "state.json"               16
//   C:/dev/kimi-agent/kimi-cli/src/kosong/tooling/__init__.py
//     FIELD_ALIASES_TODO / FIELD_ALIASES_TODO_UPDATE  499-533
//     _repair_dict_for_model (alias-when-missing semantics) 2046+
//
// Design (project conventions):
//   * namespace kimix::builtin_tools::todo (unity-build safe: every symbol is
//     namespaced, TU-local helpers use the td_ prefix).
//   * kimix:: containers only; no RTTI; errors are data (tool_error /
//     tool_response), never thrown across the tool boundary.
//   * The tree-mutation kernels (write_todos / update_todos / read_todos) are
//     pure: they take the old state by const reference and return the new
//     lists inside commit_result; the Tool wrapper commits + persists exactly
//     once on success (mirrors "_save_todos ... saving exactly once").
//   * Persistence mirrors the kimi-cli session state file: <state_dir>/
//     state.json with {"todos": [...], "archived_todos": [...]} items shaped
//     {"title","status","notes","children"}. Unknown sibling keys in an
//     existing state.json are preserved on save (subagent-state merge
//     behaviour). The file write is atomic (temp file + rename).
//   * Session binding: builtin_tools::Session carries `state_dir` ("" ==
//     in-memory only) and a lazily created shared `todo_state` cache. Two
//     tool instances of the same session share the cache; a fresh session
//     pointed at the same state_dir reloads the list from disk.
//   * Fuzzy matching ports rapidfuzz token_sort_ratio(processor=str.lower):
//     lower -> split on whitespace -> sort tokens -> join -> normalized indel
//     similarity * 100 (2*LCS/(len1+len2)). Cut-offs: 60 for title matching,
//     75 for append-mode near-duplicate warnings (Python parity).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::todo {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// ---------------------------------------------------------------------------
// Status vocabulary
// ---------------------------------------------------------------------------

enum class todo_status : uint8_t {
    pending = 0,
    in_progress,
    done,
};

// Canonical spelling: "pending" / "in_progress" / "done".
kimix::string_view status_name(todo_status s) noexcept;

// _canonical_status: strip -> lower -> '-' -> '_' -> {pending, in_progress,
// done, completed->done}. Returns false when `v` is not a status.
bool parse_status(kimix::string_view v, todo_status &out) noexcept;

// ---------------------------------------------------------------------------
// Todo tree
// ---------------------------------------------------------------------------

struct todo_item {
    kimix::string content; // title, stripped, non-empty
    todo_status status = todo_status::pending;
    kimix::optional<kimix::string> notes; // nullopt == absent; never empty
    kimix::vector<todo_item> children;
    // Parser bookkeeping (merge_one parity): true when the input JSON
    // explicitly carried a `children` key. Ignored by (de)serialization.
    bool children_provided = false;
};

// Session-persisted state (kimi_cli SessionState.todos / .archived_todos).
struct todo_state {
    kimix::vector<todo_item> todos;          // active tree
    kimix::vector<todo_item> archived_todos; // done items dropped by replace/clear
};

// Harness limits (Python parity).
inline constexpr int32_t k_max_todos = 4096;
inline constexpr int32_t k_max_archived_todos = 500;
inline constexpr int32_t k_max_read_items = 100;
inline constexpr int32_t k_default_max_layers = 4; // deepest tree = max_layers + 1
inline constexpr size_t k_max_title_chars = 65536;
inline constexpr size_t k_max_notes_chars = 65536;

// rapidfuzz cut-offs * 100 scale.
inline constexpr double k_fuzzy_title_cutoff = 60.0;
inline constexpr double k_fuzzy_warning_cutoff = 75.0;

// Cross-tool hint lines (Python parity).
inline constexpr kimix::string_view k_success_hint =
    "todo_update to edit one or more items, or todo_write to read the tree.";
inline constexpr kimix::string_view k_update_next_hint =
    "todo_update to edit another item, todo_update(parent=...) to add a child, "
    "or todo_write to read the tree.";
inline constexpr kimix::string_view k_default_error_hint =
    "todo_write to read the tree, or todo_update to edit one or more items.";
inline constexpr kimix::string_view k_all_done_reminder =
    "All todos are done. Please review the requirements again to ensure "
    "nothing is left unfinished.";

// ---------------------------------------------------------------------------
// Tree kernels (pure, recursive where Python is recursive)
// ---------------------------------------------------------------------------

struct status_count {
    int32_t pending = 0;
    int32_t in_progress = 0;
    int32_t done = 0;
};

// _count_all: recursive total item count.
int32_t count_all(kimix::span<const todo_item> items) noexcept;
// _status_counts: recursive per-status counts.
status_count status_counts(kimix::span<const todo_item> items) noexcept;
// _max_tree_depth: root items == depth 1; empty list == 0.
int32_t max_tree_depth(kimix::span<const todo_item> items) noexcept;
// _find_duplicate_titles: ROOT-level duplicates, sorted; empty when unique.
kimix::vector<kimix::string> find_duplicate_titles(kimix::span<const todo_item> items);
// _collect_titles: every title, depth-first pre-order.
kimix::vector<kimix::string> collect_titles(kimix::span<const todo_item> items);
// _find_path: index path to the FIRST node titled `title` (DFS pre-order).
// Returns false when absent.
bool find_path(kimix::span<const todo_item> items, kimix::string_view title,
               kimix::vector<int32_t> &path);
// _node_at_path: null when the path is invalid.
todo_item *node_at_path(kimix::vector<todo_item> &items,
                        kimix::span<const int32_t> path) noexcept;
const todo_item *node_at_path(const kimix::vector<todo_item> &items,
                              kimix::span<const int32_t> path) noexcept;
// _mark_subtree_done.
void mark_subtree_done(todo_item &node) noexcept;
// _count_unfinished_descendants (children and deeper).
int32_t count_unfinished_descendants(const todo_item &node) noexcept;
// _enforce_single_in_progress: returns every in_progress title (DFS pre-order)
// when more than one exists; empty when the invariant holds.
kimix::vector<kimix::string> find_in_progress_conflicts(
    kimix::span<const todo_item> items);
// _auto_fix_in_progress: keeps the LAST in_progress node (DFS pre-order) and
// demotes every earlier one to done; appends one warning per demotion.
void auto_fix_in_progress(kimix::vector<todo_item> &items,
                          kimix::vector<kimix::string> &warnings);

// ---------------------------------------------------------------------------
// Fuzzy matching (rapidfuzz token_sort_ratio port, processor=str.lower)
// ---------------------------------------------------------------------------

// Normalized indel similarity of the whitespace-split, sorted, lowercased
// tokens of `a` and `b`, scaled to [0, 100].
double token_sort_ratio(kimix::string_view a, kimix::string_view b) noexcept;

struct fuzzy_hit {
    kimix::string choice; // original (unprocessed) candidate
    double score = 0.0;
    int32_t index = -1;
};

// _find_nearest_titles(top_k=1): best candidate scoring >= cutoff (ties keep
// the earliest candidate). nullopt when nothing clears the cutoff.
kimix::optional<fuzzy_hit>
find_nearest_title(kimix::string_view query,
                   kimix::span<const kimix::string> candidates,
                   double cutoff = k_fuzzy_title_cutoff);

// ---------------------------------------------------------------------------
// Rendering (Python-parity text)
// ---------------------------------------------------------------------------

// _format_todos: dense ROOT-level summary of pending/in_progress items
// ("- [in progress] X  Notes: ..."); "" when none selected.
kimix::string format_todos(kimix::span<const todo_item> items);
// _render_read_tree: all statuses, children indented 2 spaces per depth,
// stops after max_lines lines (DFS).
kimix::string render_read_tree(kimix::span<const todo_item> items,
                               int32_t max_lines = k_max_read_items);
// _build_display_block: flattened DFS items with depth (root == 0).
struct display_item {
    kimix::string title;
    todo_status status = todo_status::pending;
    kimix::optional<kimix::string> notes;
    int32_t depth = 0;
};
kimix::vector<display_item> build_display_items(kimix::span<const todo_item> items);
// _truncate_prompt: >200 code points -> first 100 + "..." + last 100.
kimix::string truncate_prompt(kimix::string_view text, size_t max_len = 200);

// ---------------------------------------------------------------------------
// State (de)serialization + file persistence
// ---------------------------------------------------------------------------

// {"todos":[...],"archived_todos":[...]} with items shaped
// {"title","status","notes","children"} (TodoItemState.model_dump parity).
kimix::string serialize_state(const todo_state &state);
// Strict on the JSON envelope (root must be an object); lenient on items:
// malformed entries (missing/blank title, bad status, wrong types) are
// skipped like the Python loader's "Skipping malformed todo item".
bool deserialize_state(kimix::string_view json, todo_state &out,
                       kimix::string &error);

// <state_dir>/state.json (STATE_FILE_NAME parity).
kimix::string state_file_path(kimix::string_view state_dir);
// Atomic write (temp + rename); preserves unknown keys of an existing JSON
// object. Creates parent directories.
bool save_state_file(kimix::string_view path, const todo_state &state,
                     kimix::string &error);
// Missing file -> true with an empty state. Corrupt JSON -> false + error.
bool load_state_file(kimix::string_view path, todo_state &out,
                     kimix::string &error);

// Session binding -----------------------------------------------------------

// Get (lazily create) the shared todo state of `session`. When the cache is
// fresh and session.state_dir is set, loads <state_dir>/state.json first
// (missing or corrupt file -> start empty, corrupt sets *warning).
// force_reload re-reads the file even when the cache is populated.
todo_state &session_todos(builtin_tools::Session &session,
                          bool force_reload = false,
                          kimix::string *warning = nullptr);
// Persist the cached state; no-op (ok) when state_dir is empty.
tool_error persist_session_todos(builtin_tools::Session &session);

// ---------------------------------------------------------------------------
// todo_write parameters
// ---------------------------------------------------------------------------

enum class write_mode : uint8_t {
    append = 0,
    replace,
    clear,
};

// _validate_mode + _translate_legacy_force_modes: normalizes
// strip/lower/'-'->'_' (plus ' '->'_' for the legacy spellings);
// "overwrite" -> replace; {force_overwrite, force_override, force, forcewrite,
// forceoverride} -> replace + force_from_mode = true. False when invalid.
bool parse_write_mode(kimix::string_view v, write_mode &mode,
                      bool &force_from_mode) noexcept;

struct write_params {
    bool has_todos = false; // todos/items present and not null (else: read)
    kimix::vector<todo_item> todos;
    write_mode mode = write_mode::append;
    bool force = false;
    bool auto_fix = true;
    int32_t max_layers = k_default_max_layers; // injected (loop_control parity)
};

struct update_op {
    kimix::string title;
    bool has_status = false;
    todo_status status = todo_status::pending;
    bool has_notes = false;
    kimix::string notes; // raw; "" (or whitespace) clears on apply
    bool has_rename = false;
    kimix::string rename_to; // stripped, non-empty
    bool has_parent = false;
    kimix::string parent; // stripped; "" == root scope
    bool fuzzy = true;
    bool force = false;
    bool complete = false;
};

struct update_params {
    kimix::vector<update_op> ops; // normalized (single update -> one op)
    int32_t max_layers = k_default_max_layers;
};

// Tool-response envelope (ToolReturnValue parity: is_error + output +
// message + flattened display items).
struct tool_response {
    tool_status status = tool_status::ok;
    bool is_error = false;
    kimix::string output;
    kimix::string message;
    kimix::vector<display_item> display;
};

// Kernel outcome: on success (is_error == false && commit) the wrapper moves
// `todos` / `archived` into the session state and persists once.
struct commit_result {
    tool_response response;
    bool commit = false;
    kimix::vector<todo_item> todos;
    kimix::vector<todo_item> archived;
};

// Parameter parsing. Alias handling mirrors _repair_dict_for_model +
// AliasChoices: top-level {items,list,tasks,entries,todo_list,task_list} ->
// todos, {replace,override,overwrite,append,merge,update} -> mode (only when
// the canonical key is missing); item keys {content,title,task,todo,item,
// name} -> content, description -> notes. Unknown keys are ignored (the
// Python repair strips them before validation). On failure returns false with
// `err` fully populated (output carries the "Error: ..." + "\nHint: ..."
// text).
bool parse_write_params(const ToolParams *params, write_params &out,
                        tool_response &err);
bool parse_update_params(const ToolParams *params, update_params &out,
                         tool_response &err);

// ---------------------------------------------------------------------------
// Flows (pure kernels over todo_state)
// ---------------------------------------------------------------------------

// TodoList.__call__ write branch (_write_todos). `current_prompt` mirrors
// Runtime.current_prompt (may be empty).
commit_result write_todos(const todo_state &old, const write_params &params,
                          kimix::string_view current_prompt = {});
// TodoList.__call__ read branch (_read_todos).
tool_response read_todos(const todo_state &state,
                         kimix::string_view current_prompt = {});
// todo_update.__call__ (normalize already done by parse_update_params).
commit_result update_todos(const todo_state &old, const update_params &params);

// ---------------------------------------------------------------------------
  // Tool classes (registry names: "todo_write", "todo_update")
// ---------------------------------------------------------------------------

// Shared implementation of the load -> run -> commit -> save cycle.
class TodoToolBase : public kimix::builtin_tools::Tool {
public:
    explicit TodoToolBase(kimix::builtin_tools::Session *session)
        : kimix::builtin_tools::Tool(session) {}

    // Serializes `_result` (status/is_error/output/message/todos).
    void result_json(kimix::vector<char> &out) const override;
    ToolParams const &last_result() const noexcept { return _result; }

    // Runtime context (Runtime.current_prompt parity): appended to the
    // all-done reminder when non-empty.
    kimix::string current_prompt;
    // loop_control.todo_max_layers parity.
    int32_t max_layers = k_default_max_layers;

protected:
    // Commit + persist-on-success + fill _result. `save_hint` is the Hint:
    // line used when persistence fails (write and update differ in Python).
    void run_flow(commit_result &cr, kimix::string_view save_hint);
    // Read/flow response without a commit.
    void run_read(const tool_response &resp);
    // Error shortcut: output should already carry "Error: ..." (+ hint).
    void set_error(tool_status st, kimix::string_view output,
                   kimix::string_view message);
    void set_response(const tool_response &resp);
    // Session guard: null -> sets the unsupported error result, returns null.
    builtin_tools::Session *require_session();

    ToolParams _result;
};

class TodoWrite : public TodoToolBase {
public:
    explicit TodoWrite(kimix::builtin_tools::Session *session);
    // The list lives in the session (todo_state, persisted with it), so a
    // tool without one cannot keep anything - the same guard `require_session`
    // applies to every call.
    bool valid() const override;
    // todos absent/null -> read mode; otherwise the write flow.
    void operator()(ToolParams const *parameters) override;
};

class TodoUpdate : public TodoToolBase {
public:
    explicit TodoUpdate(kimix::builtin_tools::Session *session);
    // Same session-owned list state as TodoWrite (see TodoWrite::valid()).
    bool valid() const override;
    void operator()(ToolParams const *parameters) override;
};

// tool_status -> registry string ("ok", "invalid_input", ...).
kimix::string_view td_status_string(tool_status s) noexcept;

} // namespace kimix::builtin_tools::todo
