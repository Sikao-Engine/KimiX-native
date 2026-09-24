// web_search_tool.h - Pure kernels for the web_search built-in agent tool.
//
// Plan: C:/dev/kimi-agent/plans/web_search.md (MISSING at port time — the
// plans/ directory was empty; the API below reconstructs the plan's §3 scope
// from the task brief and the Python source of truth, see
// src/builtin_tools/reports/web_search.md).
//
// Python source of truth (kimi-cli, note: the brief's path
// `src/kimix/tools/web/` does not exist — the modules live under
// `kimi-cli/src/kimi_cli/tools/web/`):
//   content.py  convert_base64_images_to_links (38-65)
//   content.py  store_full_text (75-104) — only the pure parts: cache file
//               name construction (host slug + xxh64 digest) and the
//               MAX_STORED_TEXT_CHARS cap marker; the directory comes from
//               config in Python, so the C++ kernel takes it as an argument.
//   content.py  truncate_with_footer (107-163) — the char-budget cut + footer.
//               The Python body calls store_full_text() (filesystem + config);
//               the C++ kernel injects that side effect as a callback so unit
//               tests are deterministic.
//   content.py  get_extract_char_limit (166-186) — clamp [2000, 500000],
//               default 15000.
//   search.py   Params.limit ge=1, le=20 (27-38); SearchResult (41-49);
//               SearchWeb.__call__ rendering (112-125) — mirrored by
//               build_search_output().
//   providers.py _resolve / _SEARCH_LEGACY_PREFERENCE /
//               _EXTRACT_LEGACY_PREFERENCE (232-300) — engine routing decision
//               table, mirrored by resolve_active_provider().
//
// NOT ported here (owned by fetch_url): url_safety.py symbols
// (normalize_url / sensitive_query_param_name / is_safe_url / ...). Do NOT
// declare any of them in this namespace.
//
// Byte-exactness: make_cache_slug uses XXH64 from the vendored xxhash.h
// (`xxhash.xxh64(url.encode("utf-8")).hexdigest()[:10]`, seed 0). It MUST NOT
// use kimix::hash64, which is XXH3 — a silent cache-key break (plan §8).
//
// Pure CPU kernels: no Python includes; filesystem effects are isolated in
// store_full_text() and injected everywhere else.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/tool.h"

namespace kimix {
namespace builtin_tools {
namespace web_search {

// ---------------------------------------------------------------------------
// search.py Params / SearchResult (27-49)
// ---------------------------------------------------------------------------

// One search result item. Field names follow search.py's SearchResult model;
// providers return the snippet under the "description" key, which the
// renderer prints as "Summary:" (the binding layer maps that key onto
// `snippet`).
struct web_item {
    kimix::string site_name;
    kimix::string title;
    kimix::string url;
    kimix::string snippet;
    kimix::string content; // full page content (provider include_content request)
    kimix::string date;
    kimix::string icon;
    kimix::string mime;
};

// search.py Params.limit: ge=1, le=20, default 5. Pure clamp so a caller that
// skips pydantic validation can still match the Python bounds.
inline constexpr int32_t k_min_search_limit = 1;
inline constexpr int32_t k_max_search_limit = 20;
inline constexpr int32_t k_default_search_limit = 5;

// Clamp `limit` into [k_min_search_limit, k_max_search_limit]. Documented
// deviation: pydantic *rejects* an out-of-range limit (ValidationError) and
// only falls back to 5 when the field is missing; the kernel clamps so callers
// that skip validation still stay inside the reference bounds.
int32_t clamp_search_limit(int64_t limit);

// content.py get_extract_char_limit (166-186): floor 2000, cap 500000,
// default 15000.
inline constexpr int64_t k_min_extract_char_limit = 2000;
inline constexpr int64_t k_max_extract_char_limit = 500000;
inline constexpr int64_t k_default_extract_char_limit = 15000;

// Clamp `char_limit` into [k_min_extract_char_limit, k_max_extract_char_limit]
// (Python returns the default when config is absent / unparsable; the C++
// caller resolves that decision and calls this only with a parsed value).
int64_t clamp_extract_char_limit(int64_t char_limit);

// ---------------------------------------------------------------------------
// providers.py engine routing decision table (232-300)
// ---------------------------------------------------------------------------

enum class search_capability : uint8_t {
    search,
    extract,
};

// One entry of the provider registry snapshot. `available` is the precomputed
// result of the Python provider's is_available() (which may touch config/env —
// resolved by the binding layer, never inside this kernel).
struct web_provider_info {
    kimix::string name;
    bool supports_search = false;
    bool supports_extract = false;
    bool available = false;
};

// Mirror of providers.py _resolve(): explicit configured backend wins (when
// registered and capable), else the single eligible provider, else the legacy
// preference walk (search: kimi -> firecrawl -> parallel -> tavily -> exa ->
// searxng -> brave-free -> xai -> ddgs -> local; extract: local -> kimi ->
// firecrawl -> parallel -> tavily -> exa — the reference order).
// Returns the resolved provider NAME, or nullopt when nothing qualifies.
kimix::optional<kimix::string>
resolve_active_provider(kimix::string_view configured,
                        search_capability capability,
                        kimix::span<const web_provider_info> providers);

// ---------------------------------------------------------------------------
// content.py convert_base64_images_to_links (38-65)
// ---------------------------------------------------------------------------

// Replace inline base64 image blobs with labeled markdown placeholders:
//   ![alt](data:image/...;base64,...) -> [IMAGE: alt] (alt stripped)
//   (data:image/...;base64,...)       -> [IMAGE]
//   bare data:image/...;base64,...    -> [IMAGE]
// Real http(s) image links are left untouched. Byte-exact port of the three
// `regex` substitutions, including their whitespace classes: the payload
// classes and the optional space after '(' / '](' use the reference `regex`
// module's Unicode \s (25 code points, NO \x1c-\x1f), while the markdown-alt
// strip uses `str.strip()` (i.e. `str.isspace()`, 29 code points, which DOES
// include \x1c-\x1f). Both tables are pinned in the tests.
kimix::string convert_base64_images_to_links(kimix::string_view text);

// Same replacement text; additionally appends the raw base64 payload of every
// replaced data URL to `payloads` in document order (an extension — the
// Python reference only returns the text; provided so callers can inspect /
// store the extracted blobs without re-scanning).
kimix::string convert_base64_images_to_links(
    kimix::string_view text, kimix::vector<kimix::string> &payloads);

// ---------------------------------------------------------------------------
// content.py store_full_text (75-104) — cache file naming + write
// ---------------------------------------------------------------------------

// Hard ceiling on the stored full-text file (content.py MAX_STORED_TEXT_CHARS,
// 2_000_000 chars).
inline constexpr size_t k_max_stored_text_chars = 2'000'000u;

// content.py line 89: xxhash.xxh64(url.encode("utf-8")).hexdigest()[:10].
// XXH64 (vendored xxhash.h, seed 0) — NOT kimix::hash64 (XXH3).
kimix::string make_cache_slug(kimix::string_view url);

// content.py lines 87-90: "<host-slug>-<digest>.md" where host-slug is the
// URL hostname (ASCII-lower-cased like urlparse().hostname) with every code
// point outside [A-Za-z0-9._-] replaced by '-', truncated to 60 code points,
// stripped of leading/trailing '-' (fallback "page").
kimix::string make_cache_file_name(kimix::string_view url);

// content.py store_full_text (75-104): write `content` to
// <cache_dir>/<make_cache_file_name(url)> (UTF-8), creating <cache_dir>.
// Content longer than k_max_stored_text_chars is capped on a code-point
// boundary with the reference marker. Best-effort like Python: returns false
// (and leaves `out_path` empty) on any failure; `out_path` receives the
// absolute path on success.
bool store_full_text(kimix::string_view url, kimix::string_view content,
                     kimix::string_view cache_dir, kimix::string &out_path);

// ---------------------------------------------------------------------------
// content.py truncate_with_footer (107-163)
// ---------------------------------------------------------------------------

struct truncate_with_footer_result {
    kimix::string text;
    bool was_truncated = false;
};

// Side effect injected by the binding layer (Python store_full_text, or the
// native store_full_text above). Returns the stored absolute path, or nullopt
// when storage failed.
using store_full_text_fn = kimix::function<kimix::optional<kimix::string>(
    kimix::string_view url, kimix::string_view content)>;

// content.py truncate_with_footer (107-163): pages at or under `char_limit`
// code points are returned whole. Larger pages get a head+tail window (~75%
// head / ~25% tail, cut on a markdown line boundary where possible) plus the
// reference footer. `char_limit` is measured in code points (Python
// len(str)); byte slicing always lands on UTF-8 boundaries.
truncate_with_footer_result truncate_with_footer(
    kimix::string_view content, kimix::string_view url, int64_t char_limit,
    const store_full_text_fn &store);

// ---------------------------------------------------------------------------
// search.py SearchWeb.__call__ rendering (112-125) — pure renderer
// ---------------------------------------------------------------------------

// search.py builds its output through
// ``ToolResultBuilder(max_line_length=None)`` (tools/utils.py), i.e.
// ``max_chars = DEFAULT_MAX_CHARS`` and no per-line limit. The builder caps the
// whole tool result at 50,000 characters (code points), shortens the line that
// crosses the cap to ``truncate_line(line, remaining, "[...truncated]")`` and
// stops appending once full. `build_search_output` reproduces that builder
// exactly (see build_search_output_options::max_output_chars).
inline constexpr size_t k_tool_result_max_chars = 50'000u;
inline constexpr kimix::string_view k_tool_result_truncation_marker =
    "[...truncated]";

struct build_search_output_options {
    // Per-item content cap in code points (0 = no cap). Extension: the
    // reference renderer never caps a single item's content block.
    size_t max_content_chars = 0;
    // Whole-result cap in code points, i.e. ToolResultBuilder.max_chars
    // (default DEFAULT_MAX_CHARS = 50,000; 0 = unlimited). The cut is
    // byte-exact with ToolResultBuilder.write(): the overflowing line is
    // shortened to `remaining` code points ending in "[...truncated]" and the
    // remaining chunks are dropped.
    size_t max_output_chars = k_tool_result_max_chars;
    // Drop later items whose URL was already rendered (first occurrence wins).
    // Extension, OFF by default: search.py renders every item it is given.
    bool dedup_urls = false;
    // Leading summary/answer block. Extension (search.py has no summary); the
    // reference tool result is produced when this is unset.
    kimix::optional<kimix::string> summary;
};

struct build_search_output_result {
    kimix::string text;
    // Mirrors ToolResultBuilder's "Output truncated." flag: true when a line was
    // shortened by the cap (a clean cut exactly on a chunk boundary that drops
    // whole items without shortening a line leaves this false, exactly like the
    // reference).
    bool truncated = false;
    // Extension (no reference equivalent): items dropped by dedup_urls plus
    // items that rendered zero characters because the cap was already full.
    size_t omitted_items = 0;
};

// Render search results exactly like search.py SearchWeb.__call__:
//   [<summary>\n\n]                                      (when summary set)
//   Title: <title>\nDate: <date>\nURL: <url>\nSummary: <snippet>\n\n
//   <content>\n\n                                        (non-empty content)
//   ---\n\n                                              (between items)
// `snippet` is the provider's "description" value (search.py reads
// item["description"]); the binding layer maps the key. Item content is
// rendered whenever it is non-empty — search.py has no include_content gate in
// the renderer (`include_content` only asks the provider for content).
// The whole result is capped through the ToolResultBuilder emulation described
// in build_search_output_options.
build_search_output_result
build_search_output(kimix::span<const web_item> items,
                    const build_search_output_options &opts);

// ---------------------------------------------------------------------------
// Tool class wrapper (CallableTool2-style binding entry point)
// ---------------------------------------------------------------------------

// Concrete built-in tool implementation used by the binding layer. The
// provider/HTTP path stays in Python; this wrapper receives the pre-built
// search-result items and renders them through build_search_output.
//
// Accepted parameters:
//   items              required array of objects             web search results
//     site_name        optional string
//     title            optional string
//     url              optional string
//     snippet          optional string   (the provider's "description")
//     description      optional string   (alias of `snippet`; provider key)
//     content          optional string
//     date             optional string
//     icon             optional string
//     mime             optional string
//   include_content    optional bool     accepted, no effect on the rendered
//                                        text (search.py's renderer prints any
//                                        non-empty content; the flag only asks
//                                        the provider for content)
//   summary            optional string   extension (no reference equivalent)
//   max_content_chars  optional int/uint default 0 (no cap)     — extension
//   max_output_chars   optional int/uint default k_tool_result_max_chars
//   dedup_urls         optional bool     default false           — extension
//
// Serialized result fields:
//   ok                 bool
//   text               string                      rendered output
//   truncated          bool
//   omitted_items      int64_t
//   error              string                      when not ok
class WebSearch : public kimix::builtin_tools::Tool {
public:
    explicit WebSearch(kimix::builtin_tools::Session *session);
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &last_result() const { return _last_result; }
    void result_json(kimix::vector<char> &out) const override { out = _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace web_search
} // namespace builtin_tools
} // namespace kimix
