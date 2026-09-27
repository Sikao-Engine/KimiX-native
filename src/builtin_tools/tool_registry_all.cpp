// tool_registry_all.cpp - Static registration of the remaining built-in tool
// classes (Bash / Read / Write / Grep register themselves in their own .cpp).
//
// The registrar objects below use static-initialization constructors (the
// RoboCute module_register.h pattern) so every Tool subclass is discoverable
// through ToolRegistry by its lowercase registry key ("glob", "compact", ...)
// before main() runs. The JSON schemas mirror the agent-facing parameter
// shapes used by the kimi-cli Python tools.

#include "builtin_tools/compact_tool.h"
#include "builtin_tools/context_prune_tool.h"
#include "builtin_tools/edit_tool.h"
#include "builtin_tools/fetch_url_tool.h"
#include "builtin_tools/glob_tool.h"
#include "builtin_tools/pwsh_tool.h"
#include "builtin_tools/read_image_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/web_search_tool.h"

namespace kimix::builtin_tools {

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    context_prune::ContextPrune, "context_prune",
    "Prune old session content (reasoning, tool results, stale messages) to "
    "save tokens. Recent turns and tool-call pairs are always preserved. "
    "Modes: 'prune' (smart elision), 'compact' (full compaction), "
    "'strip_reasoning' (remove old thinking content). "
    "Use dry_run=True to preview changes.",
    R"JSON({"type":"object","properties":{"mode":{"type":"string","enum":["prune","compact","strip_reasoning"],"description":"Strategy: prune stale content, compact old turns, or strip old reasoning."},"target_token_count":{"type":"integer","description":"Target max tokens after pruning (>= 1000)."},"remove_reasoning":{"type":"boolean","description":"Remove old reasoning/thinking content."},"remove_tool_results":{"type":"boolean","description":"Remove old tool-result messages."},"keep_recent_turns":{"type":"integer","description":"Recent user/assistant turns to keep (1-20)."},"dry_run":{"type":"boolean","description":"Report what would be removed without changing the session."}}})JSON",
    "ContextPrune prune_context contextprune");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    glob::Glob, "glob",
    "Find files by glob pattern (e.g. **/*.ts). Returns paths in "
    "modification-time order; respects .gitignore by default.",
    R"JSON({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern to match file paths against (e.g. src/**/*.cpp)"},"path":{"type":"string","description":"Directory to search in (default: session work dir)"},"include_dirs":{"type":"boolean","description":"Include directories in results"},"respect_gitignore":{"type":"boolean","description":"Skip files matched by .gitignore (default true)"},"verbose":{"type":"boolean","description":"Include size/mtime per match"},"timeout":{"type":"integer","description":"Search timeout seconds"}},"required":["pattern"]})JSON",
    "Glob find_files");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    compact::Compact, "compact",
    "Compact / summarize conversation history to reduce token usage. Slices "
    "the message list, assembles the compaction prompt and returns the "
    "preserved tail; the actual summarization is one LLM call by the soul.",
    R"JSON({"type":"object","properties":{"messages":{"type":"array","description":"Conversation messages [{role, content:[{type,text}]}]"},"preserve_start_index":{"type":"integer","description":"Index where the preserved tail starts"},"options":{"type":"object","description":"{avoid_cascade, mode, preserve_depth_override}"},"custom_instruction":{"type":"string"},"prompt_compact":{"type":"string"},"prompt_compact_cascade":{"type":"string"}},"required":["messages"]})JSON",
    "Compact compaction");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    edit::Edit, "edit",
    "Edit an existing UTF-8 text file by replacing literal text (exact or "
    "fuzzy match). Refuses files with unresolved git conflict markers unless "
    "allowed.",
    R"JSON({"type":"object","properties":{"file_path":{"type":"string","description":"Path to edit"},"old_string":{"type":"string","description":"Literal text to replace"},"new_string":{"type":"string","description":"Literal replacement text"},"replace_all":{"type":"boolean","description":"Replace all occurrences"}},"required":["file_path","old_string","new_string"]})JSON",
    "Edit str_replace");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    pwsh::Pwsh, "pwsh",
    "Execute a PowerShell command (Windows-native shell). Modes: 'execute' "
    "(default for a native session: run now, bounded by timeout), 'send' (run "
    "in the background / write to a running task), 'interactive' (persistent "
    "REPL). The analysis kernels are selected by name: 'transform', 'fix', "
    "'hardline', 'rtk_rewrite', 'self_kill_hint'.",
    R"JSON({"type":"object","properties":{"command":{"type":"string","description":"PowerShell command or path to a .ps1 script"},"mode":{"type":"string","enum":["execute","send","interactive","transform","fix","hardline","rtk_rewrite","self_kill_hint"],"description":"execute: run now (default when a native session passes no mode); send: background or continue task_id; interactive: persistent REPL"},"timeout":{"type":"integer","description":"Timeout in seconds, 1-900 (default 30)"},"workdir":{"type":"string","description":"Working directory for this command (default: session workspace)"},"task_id":{"type":"string","description":"Existing task to continue (send) or label a new one"},"wait_for_pattern":{"type":"string","description":"Stop waiting when this literal appears in output"},"max_lines":{"type":"integer","description":"Max output lines to return (default 500)"},"agent_pid":{"type":"integer","description":"Agent process id for the self-kill guard"},"protected_pids":{"type":"array","description":"Pids the self-kill guard must never target"},"image_names":{"type":"array","description":"Process images the self-kill guard must never target"},"cmdline":{"type":"string","description":"Agent command line (pkill -f haystack)"},"token_kill":{"type":"boolean","description":"rtk: deduplicate repeated output lines"},"rtk_available":{"type":"boolean","description":"rtk: binary present (enables the rewrite)"},"rtk_binary_path":{"type":"string","description":"rtk: executable path"},"exclude_read":{"type":"boolean","description":"rtk: skip read-style commands"}},"required":["command"]})JSON",
    "Pwsh powershell PowerShell");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    fetch_url::FetchUrl, "fetch_url",
    "Fetch a web page and convert it to markdown text. Rejects non-http(s) "
    "schemes and private/loopback addresses.",
    R"JSON({"type":"object","properties":{"url":{"type":"string","description":"URL to fetch"},"output_path":{"type":"string","description":"Optional file to save the markdown to"}},"required":["url"]})JSON",
    "FetchUrl fetchurl fetch");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    web_search::WebSearch, "web_search",
    "Search the web for current information. Returns a summary answer and a "
    "list of source URLs.",
    R"JSON({"type":"object","properties":{"query":{"type":"string","description":"The search query"},"limit":{"type":"integer","description":"Number of results (default 5)"},"include_content":{"type":"boolean","description":"Include full page content"}},"required":["query"]})JSON",
    "WebSearch websearch search_web");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    read_image::ReadImage, "read_image",
    "Read a raster image (png/jpeg/webp/gif/bmp/ico) and return its metadata "
    "plus a downscaled re-encode suitable for model input.",
    R"JSON({"type":"object","properties":{"file_path":{"type":"string","description":"Path to the image file"},"pdf_page":{"type":"integer","description":"(PDFs) render this page as an image"}},"required":["file_path"]})JSON",
    "ReadImage view_image");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    retrieve::Retrieve, "retrieve",
    "Search past conversation history (BM25 with recency boost) or fetch a "
    "specific turn by id.",
    R"JSON({"type":"object","properties":{"query":{"type":"string","description":"Natural-language search query"},"id":{"type":"string","description":"Fetch a specific turn by id"},"k":{"type":"integer","description":"Max turns to return (default 3)"}}})JSON",
    "Retrieve memory");

KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    todo::TodoList, "todo_list",
    "Read or write the todo plan - one tool, one item shape, every "
    "operation. The plan is persisted with the session.\n\n"
    "Dispatch:\n"
    "- `todos` omitted -> read the current tree.\n"
    "- `todos=[...]` present -> write flow. mode='append' (default, also "
    "spelled 'merge') merges root-level todos by exact title and appends "
    "new ones; mode='replace' replaces the whole list; mode='clear' "
    "empties it. replace/clear need all existing todos done unless "
    "force=true.\n"
    "- otherwise a single edit (title=..., status=...) or an updates=[...] "
    "batch -> update flow: edit, rename (rename_to), complete a subtree "
    "(complete=true), or create a child (parent=\"<title>\") without "
    "resending the tree.\n\n"
    "Notes:\n"
    "- Statuses: pending, in_progress, done (or completed).\n"
    "- Keep exactly one item in_progress at a time; auto_fix=true resolves "
    "conflicts by keeping the last listed item.\n"
    "- done items never move back to pending/in_progress unless force=true.",
    R"JSON({"type":"object","properties":{"todos":{"type":"array","description":"The todo list to write (read mode when omitted). Each item: `content` (string, short imperative line; aliases title/task/todo/item/name) and `status` (enum: pending/in_progress/done). Also accepted as `items`/`list`/`tasks`/`entries`. Passing an empty list [] is a no-op (use mode='clear' to empty the list).","items":{"type":"object","properties":{"content":{"type":"string","description":"Title (report item shape: `content`)."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"Status"},"notes":{"type":"string","description":"Notes. MUST write, be comprehensively, detailed."},"children":{"type":"array","description":"Sub todos (children). Leave empty for a leaf. Each child has the same fields as a todo (`content`/`status`/`notes`).","items":{"type":"object","properties":{"content":{"type":"string","description":"Title"},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"Status"},"notes":{"type":"string","description":"Notes"}},"required":["content","status"]}}},"required":["content","status"]}},"updates":{"type":"array","description":"One or more update operations (update flow; only when `todos` is omitted). Each item has the shape (title, status?, notes?, rename_to?, parent?, fuzzy?, force?, complete?). Batches several lightweight edits in one call. When provided, top-level title/status/notes/rename_to/complete must not be used.","items":{"type":"object","properties":{"title":{"type":"string","description":"Title of the todo to update or create. Exact match is tried first; fuzzy match is used when enabled and exact match fails."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"New status. Omit to keep the current status (new items default to pending)."},"notes":{"type":"string","description":"New notes. Omit to keep current notes; pass an empty string to clear notes."},"rename_to":{"type":"string","description":"Rename the matched todo to this title."},"parent":{"type":"string","description":"Parent todo title that scopes the lookup and creation. When provided, the title is searched only under that parent. If the title does not exist there, a new child is created. Use an empty string for the root scope (creation allowed); omit to search globally and update only."},"fuzzy":{"type":"boolean","description":"When true and the exact title is not found, use fuzzy matching to find the nearest title."},"force":{"type":"boolean","description":"Allow regressing a 'done' item back to pending/in_progress, or allow renaming that would collide with a done item."},"complete":{"type":"boolean","description":"When true, mark the matched todo and all of its sub-todos done (one-call subtree finish). Cannot be combined with status='pending'/'in_progress'."}},"required":["title"]}},"title":{"type":"string","description":"Title of the todo to update or create when using a single top-level update. Use `updates` to batch multiple edits. `content` is accepted as an alias for compatibility with todo-write items."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"New status for the single top-level update. Ignored when `updates` is provided."},"notes":{"type":"string","description":"New notes for the single top-level update. Ignored when `updates` is provided."},"rename_to":{"type":"string","description":"Rename for the single top-level update. Ignored when `updates` is provided."},"parent":{"type":"string","description":"Optional common parent title applied to items in `updates` that do not specify their own parent. Also usable as a top-level parent for a single update."},"fuzzy":{"type":"boolean","description":"Fuzzy matching setting for the single top-level update. Ignored when `updates` is provided."},"force":{"type":"boolean","description":"Force setting for the single top-level update. Ignored when `updates` is provided."},"complete":{"type":"boolean","description":"When true, mark the matched todo and all of its sub-todos done. Ignored when `updates` is provided."},"mode":{"type":"string","enum":["append","merge","replace","clear"],"description":"Write mode: 'append' (default; 'merge' is the same upsert) merges the provided todos into the existing list (existing root titles are updated, new titles are appended; empty list is a no-op); 'replace' replaces the existing todo list only when every existing todo is done (errors otherwise); 'clear' empties the list (errors unless every old todo is done). Set force=true to replace or clear even with unfinished todos."},"auto_fix":{"type":"boolean","description":"When true (default) and multiple items are in_progress, automatically mark the extra items as done before applying the update, keeping the LAST in_progress item. Set false to get an error instead."}}})JSON",
    "TodoList todolist todo todo_write todowrite TodoWrite todo_update "
    "todoupdate TodoUpdate");

} // namespace kimix::builtin_tools
