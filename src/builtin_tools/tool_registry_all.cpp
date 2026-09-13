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

} // namespace kimix::builtin_tools
