/*
 * fts5_query.h — FTS5 query sanitizing + CJK routing helpers
 * (kimix::runtime::index).
 *
 * D8: native port of kimi-cli/src/kimi_cli/soul/fts5_search.py (read in
 * full). The helpers are header-only inline functions so both kimix-llm and
 * runtime_py (and their tests) can use them without extra build wiring.
 *
 * Ported verbatim:
 *   - MAX_FTS5_QUERY_CHARS = 2048 cap (codepoint-accurate truncation; the
 *     Python slices codepoints, not bytes).
 *   - _FTS5_SPECIAL_CHARS = '+{}():"^@/#&|~[]<>,;!?$=\\\'' (% deliberately
 *     excluded — the CJK LIKE fallback needs it as a literal).
 *   - sanitize_fts5_query: protect balanced quoted phrases via \x00Q<n>\x00
 *     placeholders, strip specials, %-strip only when no CJK, collapse *,
 *     strip dangling AND/OR/NOT, re-quote dotted/hyphenated tokens, restore
 *     placeholders. Step 5's Python regex is \b(\w+(?:[._-]\w+)+)\b — note
 *     '_' is a \w char, so only '.'/'-' act as separators ("my_app" is NOT
 *     quoted; "my-app.config.ts" is).
 *   - contains_cjk / count_cjk / has_lone_cjk_run / trigram_eligible_tokens
 *     with the 7 CJK codepoint ranges (fts5_search.py:40-50).
 *   - escape_like (% / _ / \\) and quote_fts_tokens (per-token quoting that
 *     preserves AND/OR/NOT).
 *
 * Documented deviations:
 *   - Word-char definition for step 5: ASCII alnum + '_' + any byte >= 0x80
 *     (Python's Unicode \w also excludes CJK punctuation; treating all
 *     non-ASCII bytes as word chars only matters for exotic tokens that
 *     contain both CJK punctuation and '.'/'-', and merely changes whether an
 *     edge-case token gets quoted).
 *   - Whitespace = the ASCII set; Python's str.split()/strip() also honours
 *     Unicode whitespace, which search queries essentially never contain.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace index {

// fts5_search.py:25 — cap user-controlled FTS input before any processing.
inline constexpr size_t kMaxFts5QueryChars = 2048;

// True for CJK Unified Ideographs, extensions, symbols, kana, hangul
// (fts5_search.py:40-50, the 7 ranges).
inline bool is_cjk_codepoint(uint32_t cp) noexcept {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) ||   // CJK Unified Ideographs
           (cp >= 0x3400u && cp <= 0x4DBFu) ||   // CJK Extension A
           (cp >= 0x20000u && cp <= 0x2A6DFu) || // CJK Extension B
           (cp >= 0x3000u && cp <= 0x303Fu) ||   // CJK Symbols and Punctuation
           (cp >= 0x3040u && cp <= 0x309Fu) ||   // Hiragana
           (cp >= 0x30A0u && cp <= 0x30FFu) ||   // Katakana
           (cp >= 0xAC00u && cp <= 0xD7AFu);     // Hangul Syllables
}

inline bool contains_cjk(kimix::string_view text) noexcept {
    size_t i = 0;
    while (i < text.size()) {
        const auto b = static_cast<uint8_t>(text[i]);
        if (b < 0x80u) {
            ++i;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 0;
        if ((b & 0xE0u) == 0xC0u) {
            cp = b & 0x1Fu;
            len = 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            cp = b & 0x0Fu;
            len = 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            cp = b & 0x07u;
            len = 4;
        } else {
            ++i; // stray continuation
            continue;
        }
        if (i + len > text.size()) {
            break;
        }
        for (size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3Fu);
        }
        if (is_cjk_codepoint(cp)) {
            return true;
        }
        i += len;
    }
    return false;
}

// has_lone_cjk_run (fts5_search.py:63-79): true when any maximal CJK run in
// the query is a single char (trigram needs >=3 CJK chars per token).
inline bool has_lone_cjk_run(kimix::string_view query) noexcept {
    size_t run = 0;
    size_t i = 0;
    while (i < query.size()) {
        const auto b = static_cast<uint8_t>(query[i]);
        uint32_t cp = b;
        size_t len = 1;
        if (b >= 0x80u) {
            if ((b & 0xE0u) == 0xC0u) {
                cp = b & 0x1Fu;
                len = 2;
            } else if ((b & 0xF0u) == 0xE0u) {
                cp = b & 0x0Fu;
                len = 3;
            } else if ((b & 0xF8u) == 0xF0u) {
                cp = b & 0x07u;
                len = 4;
            } else {
                ++i;
                continue;
            }
            if (i + len > query.size()) {
                break;
            }
            for (size_t k = 1; k < len; ++k) {
                cp = (cp << 6) | (static_cast<uint8_t>(query[i + k]) & 0x3Fu);
            }
        }
        if (is_cjk_codepoint(cp)) {
            ++run;
        } else {
            if (run == 1) {
                return true;
            }
            run = 0;
        }
        i += len;
    }
    return run == 1;
}

inline bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

// ASCII case-insensitive equality (Python's (?i) flag over ASCII input).
inline bool iequals_ascii(kimix::string_view a, kimix::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') {
            x = static_cast<char>(x - 'a' + 'A');
        }
        if (y >= 'a' && y <= 'z') {
            y = static_cast<char>(y - 'a' + 'A');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}



// Append a size_t as decimal digits (std::to_string returns std::string,
// which kimix::string cannot consume across allocators).
inline void append_decimal(kimix::string &out, size_t v) {
    char buf[24];
    size_t n = 0;
    do {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        out.push_back(buf[--n]);
    }
}

inline bool is_boolean_operator(kimix::string_view tok) noexcept {
    return tok == "AND" || tok == "OR" || tok == "NOT" || tok == "and" ||
           tok == "or" || tok == "not" || tok == "And" || tok == "Or" ||
           tok == "Not" || tok == "aND" || tok == "oR" || tok == "nOT";
}

// trigram_eligible_tokens (fts5_search.py:81-95): every non-operator token
// must be >=3 chars (the trigram tokenizer emits no tokens for shorter ones,
// and FTS5's implicit AND would then match nothing).
inline bool trigram_eligible_tokens(kimix::string_view query) noexcept {
    kimix::string stripped;
    // Python: query.strip('"').strip() — strip ALL leading/trailing '"'
    // then whitespace.
    size_t b = 0, e = query.size();
    while (b < e && query[b] == '"') {
        ++b;
    }
    while (e > b && query[e - 1] == '"') {
        --e;
    }
    while (b < e && is_ascii_space(query[b])) {
        ++b;
    }
    while (e > b && is_ascii_space(query[e - 1])) {
        --e;
    }
    stripped.assign(query.data() + b, e - b);
    bool any = false;
    size_t i = 0;
    while (i <= stripped.size()) {
        size_t j = i;
        while (j < stripped.size() && !is_ascii_space(stripped[j])) {
            ++j;
        }
        if (j > i) {
            const kimix::string_view tok(stripped.data() + i, j - i);
            if (!is_boolean_operator(tok)) {
                any = true;
                if (tok.size() < 3) {
                    return false;
                }
            }
        }
        i = j + 1;
    }
    return any;
}

// escape_like (fts5_search.py:98-104): escape % / _ / \ for the LIKE
// fallback; pair with ESCAPE '\' in the clause.
inline kimix::string escape_like(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\\' || c == '%' || c == '_') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

namespace detail {

// Characters FTS5's query grammar rejects outside a quoted phrase
// (fts5_search.py:36). '%' deliberately excluded.
inline bool is_fts5_special(char c) noexcept {
    switch (c) {
        case '+': case '{': case '}': case '(': case ')': case ':': case '"':
        case '^': case '@': case '/': case '#': case '&': case '|': case '~':
        case '[': case ']': case '<': case '>': case ',': case ';': case '!':
        case '?': case '$': case '=': case '\\': case '\'':
            return true;
        default:
            return false;
    }
}

inline bool is_word_char(char c) noexcept {
    const auto b = static_cast<uint8_t>(c);
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || c == '_' || b >= 0x80u;
}

} // namespace detail

// sanitize_fts5_query (fts5_search.py:107-182). Never raises: arbitrary user
// input must not produce an FTS5 grammar error inside MATCH.
inline kimix::string sanitize_fts5_query(kimix::string_view query) {
    // Step 0: 2048-CODEPOINT cap (Python slices str codepoints).
    {
        size_t i = 0, cps = 0;
        while (i < query.size() && cps < kMaxFts5QueryChars) {
            const auto b = static_cast<uint8_t>(query[i]);
            i += b < 0x80u ? 1 : ((b & 0xE0u) == 0xC0u)   ? 2
                                 : ((b & 0xF0u) == 0xE0u) ? 3
                                 : ((b & 0xF8u) == 0xF0u) ? 4
                                                          : 1;
            ++cps;
        }
        query = query.substr(0, i);
    }

    // Step 1: protect balanced double-quoted phrases with placeholders.
    kimix::vector<kimix::string> quoted_parts;
    kimix::string sanitized;
    sanitized.reserve(query.size() + 16);
    {
        size_t i = 0;
        while (i < query.size()) {
            const char ch = query[i];
            if (ch != '"') {
                sanitized.push_back(ch);
                ++i;
                continue;
            }
            size_t end = i + 1;
            while (end < query.size() && query[end] != '"') {
                ++end;
            }
            if (end == query.size()) {
                // Unmatched quote: replace with whitespace.
                sanitized.push_back(' ');
                ++i;
                continue;
            }
            quoted_parts.emplace_back(query.data() + i, end - i + 1);
            sanitized += "\x00Q";
            append_decimal(sanitized, quoted_parts.size() - 1);
            sanitized += "\x00";
            i = end + 1;
        }
    }

    // Step 2: strip remaining (unquoted) FTS5-special characters.
    for (char &c : sanitized) {
        if (detail::is_fts5_special(c)) {
            c = ' ';
        }
    }

    // Step 2b: '%' is only kept for the CJK LIKE-fallback path; a non-CJK
    // query never reaches it, so strip it there.
    if (sanitized.find('%') != kimix::string::npos &&
        !contains_cjk(sanitized)) {
        for (char &c : sanitized) {
            if (c == '%') {
                c = ' ';
            }
        }
    }

    // Step 3: collapse repeated '*' into one; drop leading/space-*.
    {
        kimix::string out;
        out.reserve(sanitized.size());
        bool prev_star = false;
        for (size_t i = 0; i < sanitized.size(); ++i) {
            const char c = sanitized[i];
            if (c == '*') {
                const bool at_start = i == 0 || is_ascii_space(sanitized[i - 1]);
                if (at_start) {
                    continue; // (^\s)* — drop
                }
                if (prev_star) {
                    continue; // collapse
                }
                prev_star = true;
                out.push_back(c);
            } else {
                prev_star = false;
                out.push_back(c);
            }
        }
        sanitized = std::move(out);
    }

    // Step 4: remove dangling boolean operators at start/end.
    {
        // trim whitespace first (Python .strip())
        size_t b = 0, e = sanitized.size();
        while (b < e && is_ascii_space(sanitized[b])) {
            ++b;
        }
        while (e > b && is_ascii_space(sanitized[e - 1])) {
            --e;
        }
        kimix::string_view sv(sanitized.data() + b, e - b);
        // leading ^(AND|OR|NOT)\b\s*
        for (kimix::string_view op : {kimix::string_view("AND"),
                                      kimix::string_view("OR"),
                                      kimix::string_view("NOT")}) {
            if (sv.size() > op.size() && iequals_ascii(sv.substr(0, op.size()), op) &&
                is_ascii_space(sv[op.size()])) {
                sv = sv.substr(op.size());
                while (!sv.empty() && is_ascii_space(sv.front())) {
                    sv = sv.substr(1);
                }
                break;
            }
            if (sv == op) {
                sv = kimix::string_view();
                break;
            }
        }
        // trailing \s+(AND|OR|NOT)$
        for (kimix::string_view op : {kimix::string_view("AND"),
                                      kimix::string_view("OR"),
                                      kimix::string_view("NOT")}) {
            if (sv.size() > op.size() &&
                is_ascii_space(sv[sv.size() - op.size() - 1]) &&
                iequals_ascii(sv.substr(sv.size() - op.size()), op)) {
                sv = sv.substr(0, sv.size() - op.size());
                break;
            }
        }
        sanitized.assign(sv.data(), sv.size());
    }

    // Step 5: wrap unquoted dotted/hyphenated tokens in double quotes
    // (Python \b(\w+(?:[._-]\w+)+)\b — '.'/'-' are the only separators;
    // '_' is a word char and must NOT trigger quoting).
    {
        kimix::string out;
        out.reserve(sanitized.size() + 8);
        size_t i = 0;
        while (i < sanitized.size()) {
            if (!detail::is_word_char(sanitized[i])) {
                out.push_back(sanitized[i]);
                ++i;
                continue;
            }
            // Start of a word run. Greedily consume run (sep run)* where sep
            // is '.' or '-'; if at least one separator was seen, the whole
            // span matches the Python pattern.
            size_t span_end = i;
            size_t seps = 0;
            for (;;) {
                size_t run_end = span_end;
                while (run_end < sanitized.size() &&
                       detail::is_word_char(sanitized[run_end])) {
                    ++run_end;
                }
                span_end = run_end;
                if (span_end < sanitized.size() &&
                    (sanitized[span_end] == '.' || sanitized[span_end] == '-')) {
                    // A separator must be followed by a word char to belong
                    // to the pattern ("\w+(?:[._-]\w+)+"); otherwise the run
                    // ends before it.
                    if (span_end + 1 < sanitized.size() &&
                        detail::is_word_char(sanitized[span_end + 1])) {
                        ++seps;
                        ++span_end; // consume the separator
                        continue;
                    }
                }
                break;
            }
            if (seps > 0) {
                out.push_back('"');
                out.append(sanitized, i, span_end - i);
                out.push_back('"');
            } else {
                out.append(sanitized, i, span_end - i);
            }
            i = span_end;
        }
        sanitized = std::move(out);
    }

    // Step 6: restore the preserved quoted phrases.
    for (size_t i = 0; i < quoted_parts.size(); ++i) {
        kimix::string ph("\x00Q");
        append_decimal(ph, i);
        ph += "\x00";
        size_t pos = 0;
        while ((pos = sanitized.find(ph, pos)) != kimix::string::npos) {
            sanitized.replace(pos, ph.size(), quoted_parts[i]);
            pos += quoted_parts[i].size();
        }
    }

    // Final strip (Python .strip()).
    size_t b = 0, e = sanitized.size();
    while (b < e && is_ascii_space(sanitized[b])) {
        ++b;
    }
    while (e > b && is_ascii_space(sanitized[e - 1])) {
        --e;
    }
    return kimix::string(sanitized.substr(b, e - b));
}

// quote_fts_tokens (fts5_search.py:185-197): quote each non-operator token
// (inner quotes doubled), preserving AND/OR/NOT between them.
inline kimix::string quote_fts_tokens(kimix::string_view raw_query) {
    kimix::string out;
    size_t i = 0;
    bool first = true;
    while (i <= raw_query.size()) {
        size_t j = i;
        while (j < raw_query.size() && !is_ascii_space(raw_query[j])) {
            ++j;
        }
        if (j > i) {
            const kimix::string_view tok(raw_query.data() + i, j - i);
            if (!first) {
                out.push_back(' ');
            }
            first = false;
            if (is_boolean_operator(tok)) {
                out.append(tok.data(), tok.size());
            } else {
                out.push_back('"');
                for (char c : tok) {
                    if (c == '"') {
                        out.push_back('"');
                    }
                    out.push_back(c);
                }
                out.push_back('"');
            }
        }
        if (j >= raw_query.size()) {
            break;
        }
        i = j + 1;
    }
    return out;
}

} // namespace index
} // namespace runtime
} // namespace kimix
