// tool_registry_all.cpp - Static registration of the remaining built-in tool
// classes (Bash / Read / Write / Grep register themselves in their own .cpp).
//
// The registrar objects below use static-initialization constructors (the
// RoboCute module_register.h pattern) so every Tool subclass is discoverable
// through ToolRegistry by its class name before main() runs. The JSON schemas
// mirror the agent-facing parameter shapes used by the kimi-cli Python tools.

#include "builtin_tools/compact_tool.h"
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

KIMIX_REGISTER_TOOL(
    glob::Glob,
    "Find files by glob pattern (e.g. **/*.ts). Returns paths in "
    "modification-time order; respects .gitignore by default.",
    R"JSON({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern to match file paths against (e.g. src/**/*.cpp)"},"path":{"type":"string","description":"Directory to search in (default: session work dir)"},"include_dirs":{"type":"boolean","description":"Include directories in results"},"respect_gitignore":{"type":"boolean","description":"Skip files matched by .gitignore (default true)"},"verbose":{"type":"boolean","description":"Include size/mtime per match"},"timeout":{"type":"integer","description":"Search timeout seconds"}},"required":["pattern"]})JSON");

KIMIX_REGISTER_TOOL(
    compact::Compact,
    "Compact / summarize conversation history to reduce token usage. Slices "
    "the message list, assembles the compaction prompt and returns the "
    "preserved tail; the actual summarization is one LLM call by the soul.",
    R"JSON({"type":"object","properties":{"messages":{"type":"array","description":"Conversation messages [{role, content:[{type,text}]}]"},"preserve_start_index":{"type":"integer","description":"Index where the preserved tail starts"},"options":{"type":"object","description":"{avoid_cascade, mode, preserve_depth_override}"},"custom_instruction":{"type":"string"},"prompt_compact":{"type":"string"},"prompt_compact_cascade":{"type":"string"}},"required":["messages"]})JSON");

KIMIX_REGISTER_TOOL(
    edit::Edit,
    "Edit an existing UTF-8 text file by replacing literal text (exact or "
    "fuzzy match). Refuses files with unresolved git conflict markers unless "
    "allowed.",
    R"JSON({"type":"object","properties":{"file_path":{"type":"string","description":"Path to edit"},"old_string":{"type":"string","description":"Literal text to replace"},"new_string":{"type":"string","description":"Literal replacement text"},"replace_all":{"type":"boolean","description":"Replace all occurrences"}},"required":["file_path","old_string","new_string"]})JSON");

KIMIX_REGISTER_TOOL(
    pwsh::Pwsh,
    "Execute a PowerShell command (Windows-native shell). Use when PowerShell "
    "cmdlets are required; prefer Bash for POSIX syntax.",
    R"JSON({"type":"object","properties":{"command":{"type":"string","description":"PowerShell command or script"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"}},"required":["command"]})JSON");

KIMIX_REGISTER_TOOL(
    fetch_url::FetchUrl,
    "Fetch a web page and convert it to markdown text. Rejects non-http(s) "
    "schemes and private/loopback addresses.",
    R"JSON({"type":"object","properties":{"url":{"type":"string","description":"URL to fetch"},"output_path":{"type":"string","description":"Optional file to save the markdown to"}},"required":["url"]})JSON");

KIMIX_REGISTER_TOOL(
    web_search::WebSearch,
    "Search the web for current information. Returns a summary answer and a "
    "list of source URLs.",
    R"JSON({"type":"object","properties":{"query":{"type":"string","description":"The search query"},"limit":{"type":"integer","description":"Number of results (default 5)"},"include_content":{"type":"boolean","description":"Include full page content"}},"required":["query"]})JSON");

KIMIX_REGISTER_TOOL(
    read_image::ReadImage,
    "Read a raster image (png/jpeg/webp/gif/bmp/ico) and return its metadata "
    "plus a downscaled re-encode suitable for model input.",
    R"JSON({"type":"object","properties":{"file_path":{"type":"string","description":"Path to the image file"},"pdf_page":{"type":"integer","description":"(PDFs) render this page as an image"}},"required":["file_path"]})JSON");

KIMIX_REGISTER_TOOL(
    retrieve::Retrieve,
    "Search past conversation history (BM25 with recency boost) or fetch a "
    "specific turn by id.",
    R"JSON({"type":"object","properties":{"query":{"type":"string","description":"Natural-language search query"},"id":{"type":"string","description":"Fetch a specific turn by id"},"k":{"type":"integer","description":"Max turns to return (default 3)"}}})JSON");

KIMIX_REGISTER_TOOL(
    todo::TodoWrite,
    "Read or write the whole todo tree (persisted with the session). Omit "
    "`todos` to read the current tree; send the complete list to set the "
    "plan. For targeted single/batch edits (status, notes, rename, or "
    "children via parent=...) use TodoUpdate.\n\n"
    "Write modes:\n"
    "- append (default): merges root-level todos by exact title; new titles "
    "are appended.\n"
    "- replace: replaces the whole list; only allowed when all existing "
    "todos are done (use force=true to override).\n"
    "- clear: empties the list; only allowed when all todos are done (use "
    "force=true to override).\n\n"
    "Notes:\n"
    "- Send the complete list each write; there are no partial edits.\n"
    "- Keep exactly one item in_progress at a time; auto_fix=true resolves "
    "conflicts by keeping the last listed item.\n"
    "- Statuses: pending, in_progress, done (or completed).",
    R"JSON({"type":"object","properties":{"todos":{"type":"array","description":"The COMPLETE task list, replacing any previous list. Each item: `content` (string, short imperative line) and `status` (enum: pending/in_progress/done). Passing an empty list [] is a no-op (use mode='clear' to empty the list). Accepts `todos` or `items` parameter.","items":{"type":"object","properties":{"content":{"type":"string","description":"Title (report item shape: `content`)."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"Status"},"notes":{"type":"string","description":"Notes. MUST write, be comprehensively, detailed."},"children":{"type":"array","description":"Sub todos (children). Leave empty for a leaf. Each child has the same fields as a todo (`content`/`status`/`notes`).","items":{"type":"object","properties":{"content":{"type":"string","description":"Title"},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"Status"},"notes":{"type":"string","description":"Notes"}},"required":["content","status"]}}},"required":["content","status"]}},"mode":{"type":"string","enum":["append","replace","clear"],"description":"Write mode: 'append' merges the provided todos into the existing list (existing root titles are updated, new titles are appended; empty list is a no-op); 'replace' replaces the existing todo list only when every existing todo is done (errors otherwise); 'clear' empties the list (errors unless every old todo is done). Set force=true to replace or clear even with unfinished todos."},"force":{"type":"boolean","description":"When true, mode='replace' and mode='clear' bypass the all-done guard (and skip regression and single-in_progress checks)."},"auto_fix":{"type":"boolean","description":"When true (default) and multiple items are in_progress, automatically mark the extra items as done before applying the update, keeping the LAST in_progress item. Set false to get an error instead."}}})JSON");

KIMIX_REGISTER_TOOL(
    todo::TodoUpdate,
    "Create, update, rename, or complete one or more todos by title - no "
    "need to resend the whole tree. Pass a single edit directly (title=..., "
    "status=...), or pass updates=[...] to batch several edits in one "
    "call.\n"
    "- title: the todo to update or create. `content` is accepted as an "
    "alias for title, so TodoWrite-style items ({content, status, notes}) "
    "may be reused here.\n"
    "- parent: scope the lookup/creation - omit to search the whole tree "
    "(update only), \"\" for the root scope, or a parent title to "
    "create/update a child under it.\n"
    "- status: pending/in_progress/done; omit keeps the current status (new "
    "items default to pending).\n"
    "- notes: replace notes (\"\" clears, omit keeps).\n"
    "- rename_to: rename the matched todo.\n"
    "- complete: true marks the matched todo and all its sub-todos done "
    "(one call finishes a subtree).\n"
    "- force: allow reopening a done item or renaming over a done item.\n"
    "- fuzzy: default true - match near-miss titles when the exact title is "
    "not found.",
    R"JSON({"type":"object","properties":{"title":{"type":"string","description":"Title of the todo to update or create when using a single top-level update. Use `updates` to batch multiple edits. `content` is accepted as an alias for compatibility with TodoWrite items."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"New status for the single top-level update. Ignored when `updates` is provided."},"notes":{"type":"string","description":"New notes for the single top-level update. Ignored when `updates` is provided."},"rename_to":{"type":"string","description":"Rename for the single top-level update. Ignored when `updates` is provided."},"parent":{"type":"string","description":"Optional common parent title applied to items in `updates` that do not specify their own parent. Also usable as a top-level parent for a single update."},"fuzzy":{"type":"boolean","description":"Fuzzy matching setting for the single top-level update. Ignored when `updates` is provided."},"force":{"type":"boolean","description":"Force setting for the single top-level update. Ignored when `updates` is provided."},"complete":{"type":"boolean","description":"When true, mark the matched todo and all of its sub-todos done. Ignored when `updates` is provided."},"updates":{"type":"array","description":"One or more update operations. Each item has the same shape as a single TodoUpdate call (title, status, notes, rename_to, parent, fuzzy, force, complete). Use this to batch multiple lightweight edits in one call. When provided, top-level title/status/notes/rename_to/complete must not be used.","items":{"type":"object","properties":{"title":{"type":"string","description":"Title of the todo to update or create. Exact match is tried first; fuzzy match is used when enabled and exact match fails."},"status":{"type":"string","enum":["pending","in_progress","done","completed"],"description":"New status. Omit to keep the current status (new items default to pending)."},"notes":{"type":"string","description":"New notes. Omit to keep current notes; pass an empty string to clear notes."},"rename_to":{"type":"string","description":"Rename the matched todo to this title."},"parent":{"type":"string","description":"Parent todo title that scopes the lookup and creation. When provided, the title is searched only under that parent. If the title does not exist there, a new child is created. Use an empty string for the root scope (creation allowed); omit to search globally and update only."},"fuzzy":{"type":"boolean","description":"When true and the exact title is not found, use fuzzy matching to find the nearest title."},"force":{"type":"boolean","description":"Allow regressing a 'done' item back to pending/in_progress, or allow renaming that would collide with a done item."},"complete":{"type":"boolean","description":"When true, mark the matched todo and all of its sub-todos done (one-call subtree finish). Cannot be combined with status='pending'/'in_progress'."}},"required":["title"]}}}})JSON");

} // namespace kimix::builtin_tools
