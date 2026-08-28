// web_search_tool.cpp - Pure kernels for the web_search built-in agent tool.
//
// Exact ports of the Python reference (kimi-cli/src/kimi_cli/tools/web/):
//   content.py  convert_base64_images_to_links (38-65)
//   content.py  store_full_text (75-104)
//   content.py  truncate_with_footer (107-163)
//   content.py  get_extract_char_limit (166-186)
//   search.py   Params.limit (27-38) / SearchResult (41-49) /
//               SearchWeb.__call__ rendering (112-125)
//   providers.py _resolve (232-300)
//
// See web_search_tool.h for the plan/source-of-truth notes and the missing
// plan file (C:/dev/kimi-agent/plans/web_search.md) reconstruction.
//
// Unity-build safety: every helper is static inside an anonymous namespace in
// this TU (or in the tool namespace declared by the header); no file-scope
// `using namespace`.

#include "builtin_tools/web_search_tool.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <core/kimix_core.h>
#include <core/stl/filesystem.h>

#include "builtin_tools/utf8_util.h"

// Vendored xxHash — XXH64 (NOT kimix::hash64, which is XXH3; the plan flags
// the XXH3-vs-XXH64 mix-up as a silent cache-key break). The macro makes the
// header self-contained (the same pattern the xxhash unit test uses).
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace kimix {
namespace builtin_tools {
namespace web_search {

namespace {

// ---- ASCII helpers ---------------------------------------------------------

bool ws_is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool ws_is_base64_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

bool ws_has_at(kimix::string_view text, size_t pos, kimix::string_view lit) noexcept {
    return pos + lit.size() <= text.size() &&
           text.substr(pos, lit.size()) == lit;
}

// Python-style thousands separator ("{n:,}").
kimix::string ws_thousands(int64_t n) {
    kimix::StringScratch ss;
    ss << static_cast<long long>(n);
    kimix::string digits = std::move(ss.string());
    kimix::string out;
    out.reserve(digits.size() + digits.size() / 3u + 1u);
    for (size_t i = 0; i < digits.size(); ++i) {
        out.push_back(digits[i]);
        const size_t remaining = digits.size() - i - 1u;
        if (remaining > 0u && remaining % 3u == 0u) {
            out.push_back(',');
        }
    }
    return out;
}

size_t ws_count_newlines(kimix::string_view s) noexcept {
    size_t n = 0;
    for (const char c : s) {
        if (c == '\n') {
            ++n;
        }
    }
    return n;
}

// ---- convert_base64_images_to_links scanner --------------------------------
//
// One left-to-right pass that, at each position, tries the three reference
// `regex` patterns in order:
//   1. !\[(?P<alt>[^\]]*)\]\(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\)
//   2. \(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\)
//   3. data:image/[^;]+;base64,[A-Za-z0-9+/=]+
// The first chars are disjoint ('!', '(', 'd'), so at most one matches per
// position and the result is identical to the three sequential re.sub passes
// of the reference (verified against the Python goldens).

struct ws_match {
    bool is_md = false;
    size_t begin = 0; // == scan position
    size_t end = 0;   // exclusive
    size_t alt_begin = 0;
    size_t alt_end = 0;
    size_t payload_begin = 0;
    size_t payload_end = 0;
};

// Shared blob matcher: text[start..] must begin with "data:image/". Mirrors
// `[^;]+` (one+ chars up to the first ';'), the literal ";base64,", and the
// payload class `[A-Za-z0-9+/=]` (+ `\s` when allow_space). When
// require_close_paren, the char immediately after the payload run must be ')'
// (the payload class never contains ')', so greedy + backtrack collapses to
// "run followed by ')'").
bool ws_match_data_blob(kimix::string_view text, size_t start,
                        bool require_close_paren, bool allow_space,
                        size_t &payload_begin, size_t &payload_end,
                        size_t &match_end) noexcept {
    size_t i = start + 12u; // "data:image/"
    const size_t type_start = i;
    while (i < text.size() && text[i] != ';') {
        ++i;
    }
    if (i == type_start) {
        return false; // [^;]+ needs at least one char
    }
    if (!ws_has_at(text, i, ";base64,")) {
        return false;
    }
    i += 8u; // ";base64,"
    payload_begin = i;
    while (i < text.size() &&
           (ws_is_base64_char(text[i]) ||
            (allow_space && ws_is_space(text[i])))) {
        ++i;
    }
    if (i == payload_begin) {
        return false; // payload class is '+' — must be non-empty
    }
    if (require_close_paren) {
        if (i >= text.size() || text[i] != ')') {
            return false;
        }
        match_end = i + 1u;
    } else {
        match_end = i;
    }
    payload_end = i;
    return true;
}

bool ws_find_md_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (from + 2u > text.size() || text[from] != '!' || text[from + 1u] != '[') {
        return false;
    }
    const size_t alt_begin = from + 2u;
    size_t alt_end = alt_begin;
    while (alt_end < text.size() && text[alt_end] != ']') {
        ++alt_end;
    }
    if (alt_end >= text.size()) {
        return false; // unterminated alt
    }
    if (alt_end + 1u >= text.size() || text[alt_end] != ']' ||
        text[alt_end + 1u] != '(') {
        return false;
    }
    size_t i = alt_end + 2u;
    while (i < text.size() && ws_is_space(text[i])) {
        ++i;
    }
    if (!ws_has_at(text, i, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, i, /*require_close_paren=*/true,
                            /*allow_space=*/true, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = true;
    m.begin = from;
    m.end = match_end;
    m.alt_begin = alt_begin;
    m.alt_end = alt_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

bool ws_find_paren_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (text[from] != '(') {
        return false;
    }
    size_t i = from + 1u;
    while (i < text.size() && ws_is_space(text[i])) {
        ++i;
    }
    if (!ws_has_at(text, i, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, i, /*require_close_paren=*/true,
                            /*allow_space=*/true, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = false;
    m.begin = from;
    m.end = match_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

bool ws_find_bare_base64(kimix::string_view text, size_t from, ws_match &m) noexcept {
    if (!ws_has_at(text, from, "data:image/")) {
        return false;
    }
    size_t payload_begin = 0;
    size_t payload_end = 0;
    size_t match_end = 0;
    if (!ws_match_data_blob(text, from, /*require_close_paren=*/false,
                            /*allow_space=*/false, payload_begin, payload_end,
                            match_end)) {
        return false;
    }
    m.is_md = false;
    m.begin = from;
    m.end = match_end;
    m.payload_begin = payload_begin;
    m.payload_end = payload_end;
    return true;
}

kimix::string ws_replacement(kimix::string_view text, const ws_match &m) {
    if (!m.is_md) {
        return "[IMAGE]";
    }
    size_t b = m.alt_begin;
    size_t e = m.alt_end;
    while (b < e && ws_is_space(text[b])) {
        ++b;
    }
    while (e > b && ws_is_space(text[e - 1u])) {
        --e;
    }
    if (b == e) {
        return "[IMAGE]";
    }
    kimix::string out = "[IMAGE: ";
    out.append(text.data() + b, e - b);
    out += "]";
    return out;
}

kimix::string ws_scan_base64(kimix::string_view text,
                            kimix::vector<kimix::string> *payloads) {
    kimix::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        ws_match m;
        bool matched = false;
        if (text[pos] == '!') {
            matched = ws_find_md_base64(text, pos, m);
        } else if (text[pos] == '(') {
            matched = ws_find_paren_base64(text, pos, m);
        } else {
            matched = ws_find_bare_base64(text, pos, m);
        }
        if (matched) {
            out += ws_replacement(text, m);
            if (payloads != nullptr) {
                payloads->emplace_back(text.data() + m.payload_begin,
                                       m.payload_end - m.payload_begin);
            }
            pos = m.end;
        } else {
            out.push_back(text[pos]);
            ++pos;
        }
    }
    return out;
}

// ---- URL hostname (content.py store_full_text host extraction) -------------

kimix::string ws_url_hostname(kimix::string_view url) {
    const size_t scheme = url.find("://");
    if (scheme == kimix::string_view::npos) {
        return {}; // urlparse: no scheme -> hostname None -> "page"
    }
    size_t start = scheme + 3u;
    size_t end = start;
    while (end < url.size() && url[end] != '/' && url[end] != '?' &&
           url[end] != '#') {
        ++end;
    }
    kimix::string_view netloc = url.substr(start, end - start);
    const size_t at = netloc.rfind('@');
    if (at != kimix::string_view::npos) {
        netloc = netloc.substr(at + 1u);
    }
    if (!netloc.empty() && netloc[0] == '[') {
        const size_t rb = netloc.find(']');
        if (rb == kimix::string_view::npos) {
            return {};
        }
        return kimix::string(netloc.substr(1u, rb - 1u));
    }
    const size_t colon = netloc.find(':');
    if (colon != kimix::string_view::npos) {
        netloc = netloc.substr(0u, colon);
    }
    return kimix::string(netloc);
}

} // namespace

// ---------------------------------------------------------------------------
// search.py Params.limit + content.py get_extract_char_limit
// ---------------------------------------------------------------------------

int32_t clamp_search_limit(int64_t limit) {
    if (limit < k_min_search_limit) {
        return k_min_search_limit;
    }
    if (limit > k_max_search_limit) {
        return k_max_search_limit;
    }
    return static_cast<int32_t>(limit);
}

int64_t clamp_extract_char_limit(int64_t char_limit) {
    if (char_limit < k_min_extract_char_limit) {
        return k_min_extract_char_limit;
    }
    if (char_limit > k_max_extract_char_limit) {
        return k_max_extract_char_limit;
    }
    return char_limit;
}

// ---------------------------------------------------------------------------
// providers.py _resolve engine routing decision table
// ---------------------------------------------------------------------------

kimix::optional<kimix::string>
resolve_active_provider(kimix::string_view configured,
                        search_capability capability,
                        kimix::span<const web_provider_info> providers) {
    const auto capable = [capability](const web_provider_info &p) {
        return capability == search_capability::search ? p.supports_search
                                                       : p.supports_extract;
    };

    // Rule 1: explicit config wins (registered + capable, availability ignored).
    if (!configured.empty()) {
        for (const auto &p : providers) {
            if (p.name == configured && capable(p)) {
                return p.name;
            }
        }
    }

    // Rule 2: exactly one eligible provider.
    const web_provider_info *single = nullptr;
    size_t single_count = 0;
    for (const auto &p : providers) {
        if (capable(p) && p.available) {
            single = &p;
            ++single_count;
        }
    }
    if (single_count == 1u) {
        return single->name;
    }

    // Rule 3: legacy preference walk.
    static constexpr kimix::string_view k_search_pref[] = {"kimi", "ddgs", "local"};
    static constexpr kimix::string_view k_extract_pref[] = {"local", "kimi"};
    const kimix::span<const kimix::string_view> pref =
        capability == search_capability::search
            ? kimix::span<const kimix::string_view>(k_search_pref, 3u)
            : kimix::span<const kimix::string_view>(k_extract_pref, 2u);
    for (const kimix::string_view name : pref) {
        for (const auto &p : providers) {
            if (p.name == name && capable(p) && p.available) {
                return p.name;
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// content.py convert_base64_images_to_links (38-65)
// ---------------------------------------------------------------------------

kimix::string convert_base64_images_to_links(kimix::string_view text) {
    return ws_scan_base64(text, nullptr);
}

kimix::string convert_base64_images_to_links(
    kimix::string_view text, kimix::vector<kimix::string> &payloads) {
    return ws_scan_base64(text, &payloads);
}

// ---------------------------------------------------------------------------
// content.py store_full_text (75-104) — cache file naming + write
// ---------------------------------------------------------------------------

kimix::string make_cache_slug(kimix::string_view url) {
    const uint64_t h = XXH64(url.data(), url.size(), 0);
    char buf[16];
    for (int i = 15; i >= 0; --i) {
        const unsigned nib = static_cast<unsigned>((h >> (4u * static_cast<unsigned>(i))) & 0xFu);
        buf[15 - i] = nib < 10u ? static_cast<char>('0' + nib)
                                : static_cast<char>('a' + nib - 10u);
    }
    // xxhash.xxh64(...).hexdigest()[:10]
    return kimix::string(buf, 10u);
}

kimix::string make_cache_file_name(kimix::string_view url) {
    kimix::string host = ws_url_hostname(url);
    // content.py: host = (urlparse(url).hostname or "page").replace(":", "_")
    for (auto &c : host) {
        if (c == ':') {
            c = '_';
        }
    }
    // re.sub(r"[^A-Za-z0-9._-]", "-", host)[:60]
    kimix::string slug;
    slug.reserve(60u);
    for (const char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-';
        if (slug.size() < 60u) {
            slug.push_back(ok ? c : '-');
        }
    }
    // .strip("-")
    size_t b = 0;
    size_t e = slug.size();
    while (b < e && slug[b] == '-') {
        ++b;
    }
    while (e > b && slug[e - 1u] == '-') {
        --e;
    }
    kimix::string out;
    if (b == e) {
        out = "page";
    } else {
        out.assign(slug.data() + b, e - b);
    }
    out += "-";
    out += make_cache_slug(url);
    out += ".md";
    return out;
}

bool store_full_text(kimix::string_view url, kimix::string_view content,
                     kimix::string_view cache_dir, kimix::string &out_path) {
    out_path.clear();
    try {
        const kimix::string dir(cache_dir);
        std::error_code ec;
        kimix::filesystem::create_directories(kimix::filesystem::path(dir), ec);
        const kimix::filesystem::path file_path =
            kimix::filesystem::path(dir) / make_cache_file_name(url);

        kimix::string stored;
        const size_t total_cp = utf8_code_point_count(content);
        if (total_cp > k_max_stored_text_chars) {
            stored.assign(
                content.data(),
                utf8_byte_offset_of_code_point(content, k_max_stored_text_chars));
            kimix::StringScratch ss;
            ss << "\n\n[... stored copy truncated at "
               << ws_thousands(static_cast<int64_t>(k_max_stored_text_chars))
               << " chars of " << ws_thousands(static_cast<int64_t>(total_cp))
               << "; re-extract a more specific URL for the rest ...]";
            stored += ss.string();
        } else {
            stored.assign(content.data(), content.size());
        }

        FILE *file = fopen(file_path.string().c_str(), "wb");
        if (file == nullptr) {
            return false;
        }
        const size_t written = fwrite(stored.data(), 1u, stored.size(), file);
        fclose(file);
        if (written != stored.size()) {
            return false;
        }
        out_path = kimix::to_string(file_path);
        return true;
    } catch (...) {
        // Best-effort, matching Python's try/except -> None.
        return false;
    }
}

// ---------------------------------------------------------------------------
// content.py truncate_with_footer (107-163)
// ---------------------------------------------------------------------------

truncate_with_footer_result truncate_with_footer(
    kimix::string_view content, kimix::string_view url, int64_t char_limit,
    const store_full_text_fn &store) {
    truncate_with_footer_result r;
    const size_t total_cp = utf8_code_point_count(content);
    if (total_cp <= static_cast<size_t>(char_limit)) {
        r.text.assign(content.data(), content.size());
        r.was_truncated = false;
        return r;
    }
    r.was_truncated = true;

    // int(char_limit * 0.75); 0.75 is exact in binary so integer arithmetic
    // matches Python for the clamped range.
    const int64_t head_budget = char_limit * 3 / 4;
    const int64_t tail_budget = char_limit - head_budget;

    const size_t head_cp = static_cast<size_t>(head_budget);
    // Python `content[-tail_budget:]`; note content[-0:] == content[0:] (whole).
    const size_t tail_start_cp =
        (tail_budget <= 0) ? 0u : total_cp - static_cast<size_t>(tail_budget);

    kimix::string head(content.data(),
                       utf8_byte_offset_of_code_point(content, head_cp));
    const size_t tail_byte_begin =
        utf8_byte_offset_of_code_point(content, tail_start_cp);
    kimix::string tail(content.data() + tail_byte_begin,
                       content.size() - tail_byte_begin);

    // Snap the head cut back to the last newline (code-point index semantics:
    // Python head.rfind("\n") then head[:nl]).
    const size_t head_nl_byte = head.rfind('\n');
    if (head_nl_byte != kimix::string::npos) {
        const int64_t head_nl = static_cast<int64_t>(utf8_code_point_count(
            kimix::string_view(head).substr(0u, head_nl_byte)));
        if (head_nl * 2 > head_budget) {
            head.assign(head.data(), head_nl_byte);
        }
    }

    // Snap the tail cut forward to the next newline.
    const size_t tail_nl_byte = tail.find('\n');
    if (tail_nl_byte != kimix::string::npos) {
        const int64_t tail_nl = static_cast<int64_t>(utf8_code_point_count(
            kimix::string_view(tail).substr(0u, tail_nl_byte)));
        if (tail_nl * 2 < tail_budget) {
            tail.assign(tail.data() + tail_nl_byte + 1u,
                        tail.size() - tail_nl_byte - 1u);
        }
    }

    const int64_t total = static_cast<int64_t>(total_cp);
    const kimix::optional<kimix::string> stored_path = store(url, content);

    kimix::vector<kimix::string> footer_lines;
    footer_lines.emplace_back(); // leading empty line
    footer_lines.push_back(
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 [TRUNCATED] "
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
    {
        kimix::StringScratch ss;
        ss << "Showing "
           << ws_thousands(static_cast<int64_t>(utf8_code_point_count(head)))
           << " chars (head) + "
           << ws_thousands(static_cast<int64_t>(utf8_code_point_count(tail)))
           << " chars (tail) of " << ws_thousands(total)
           << " total clean characters.";
        footer_lines.push_back(std::move(ss.string()));
    }
    if (stored_path) {
        kimix::StringScratch ss;
        ss << "Full text saved to: " << *stored_path;
        footer_lines.push_back(std::move(ss.string()));
        kimix::StringScratch ss2;
        ss2 << "To read the omitted middle: read_file path=\"" << *stored_path
            << "\" offset=" << static_cast<unsigned long long>(
                   ws_count_newlines(head) + 2u)
            << " limit=200  (the file is the complete page; raise/lower "
               "offset to page through it).";
        footer_lines.push_back(std::move(ss2.string()));
    } else {
        footer_lines.push_back(
            "Full text could not be stored; re-run web_extract on a more "
            "specific URL for the complete page.");
    }
    footer_lines.push_back(
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");

    kimix::string model_text;
    model_text.reserve(head.size() + tail.size() + 256u);
    model_text += head;
    // "\n\n[... middle omitted — see footer ...]\n\n" (U+2014 em dash)
    model_text += "\n\n[... middle omitted \xE2\x80\x94 see footer ...]\n\n";
    model_text += tail;
    model_text += "\n";
    for (size_t i = 0; i < footer_lines.size(); ++i) {
        if (i > 0u) {
            model_text += "\n";
        }
        model_text += footer_lines[i];
    }
    r.text = std::move(model_text);
    return r;
}

// ---------------------------------------------------------------------------
// search.py SearchWeb.__call__ rendering (112-125) — pure renderer
// ---------------------------------------------------------------------------

build_search_output_result
build_search_output(kimix::span<const web_item> items,
                    const build_search_output_options &opts) {
    build_search_output_result r;

    // De-duplicate by URL (first occurrence wins, input order preserved).
    kimix::vector<const web_item *> kept;
    kept.reserve(items.size());
    kimix::unordered_set<kimix::string_view> seen;
    for (const auto &it : items) {
        const kimix::string_view url(it.url.data(), it.url.size());
        if (seen.insert(url).second) {
            kept.push_back(&it);
        }
    }
    r.omitted_items = items.size() - kept.size();

    // Per-item blocks, rendered exactly like search.py.
    kimix::vector<kimix::string> blocks;
    blocks.reserve(kept.size());
    for (const auto *it : kept) {
        kimix::string block;
        block.reserve(it->title.size() + it->date.size() + it->url.size() +
                      it->snippet.size() + it->content.size() + 32u);
        block += "Title: ";
        block += it->title;
        block += "\nDate: ";
        block += it->date;
        block += "\nURL: ";
        block += it->url;
        block += "\nSummary: ";
        block += it->snippet;
        block += "\n\n";
        if (opts.include_content && !it->content.empty()) {
            if (opts.max_content_chars > 0u) {
                block.append(
                    it->content.data(),
                    utf8_byte_offset_of_code_point(it->content,
                                                   opts.max_content_chars));
            } else {
                block += it->content;
            }
            block += "\n\n";
        }
        blocks.push_back(std::move(block));
    }

    kimix::string out;
    if (opts.summary && !opts.summary->empty()) {
        out = *opts.summary;
        out += "\n\n";
    }

    size_t dropped_at_cap = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const size_t sep = (i == 0u) ? 0u : 5u; // "---\n\n"
        const size_t add = sep + blocks[i].size();
        if (out.size() + add > opts.max_output_bytes) {
            r.truncated = true;
            dropped_at_cap = blocks.size() - i;
            break;
        }
        if (sep != 0u) {
            out += "---\n\n";
        }
        out += blocks[i];
    }
    if (dropped_at_cap > 0u) {
        kimix::StringScratch ss;
        ss << "\n\xE2\x80\xA6 (" << static_cast<unsigned long long>(dropped_at_cap)
           << " item(s) omitted \xE2\x80\x94 output byte cap) \xE2\x80\xA6";
        kimix::string note = std::move(ss.string());
        if (out.size() + note.size() <= opts.max_output_bytes) {
            out += note;
        }
        r.omitted_items += dropped_at_cap;
    }

    r.text = std::move(out);
    return r;
}

} // namespace web_search
} // namespace builtin_tools
} // namespace kimix
