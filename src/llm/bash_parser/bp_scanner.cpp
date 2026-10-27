/* bp_scanner.cpp — the compatibility scanner: line-for-line port of
 * kimi-agent shell_compat/bash_core.py::_BashFixScanner (`_scan_range_inner`
 * scan flow + helper methods + `fix()` orchestration), translator.py's
 * `render_splices`, and bash_core.py's `_opaque_inline_expansions`.
 *
 * DEPTH MODEL (deviation, documented in the header): the reference recurses
 * synchronously (`_scan_range` / `_find_matching` per substitution level) and
 * relies on Python's RecursionError (~1000 frames; observed cutoff at nesting
 * depth ~330 for the `$(...)*N` shape) being caught in `fix()` as a
 * whole-command pass-through.  This port keeps the exact synchronous DFS
 * structure (so report discovery order matches byte-for-byte) and the
 * _MAX_NESTING_DEPTH (1024) budget, but `scan_all` runs the scan on a
 * dedicated thread with a large stack (64 MiB) so adversarial nesting within
 * the budget can never overflow the native stack — including when the caller
 * runs on a 1 MiB marl fiber stack.  Practical consequence: inputs nested
 * between the reference's Python-frame cutoff (~330) and 1024 are scanned
 * here but passed through by the reference; the reference suite's robustness
 * cases (250-level rewrite, 2,000-level pass-through) agree byte-for-byte.
 */
#include "llm/bash_parser/bp_scanner.h"

#ifdef KIMIX_PLATFORM_WINDOWS
    #include <windows.h>
    #include <process.h>
#else
    #include <pthread.h>
#endif

#include "llm/bash_parser/bp_cmd.h"
#include "llm/bash_parser/bp_core.h"
#include "llm/bash_parser/bp_fallback.h"
#include "llm/bash_parser/bp_heredoc.h"
#include "llm/bash_parser/bp_paths.h"
#include "llm/bash_parser/bp_wrappers.h"

namespace kimix::llm::bash_parser {

namespace src_llm_bash_parser_bp_scanner {


// `_HSPACE_RE.match(s, i, end)` end position.
inline size_t hspace_end(kimix::string_view s, size_t i, size_t end) {
    while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
        i += 1;
    }
    return i;
}

// Python str.find(ch, i, end): bounded find, npos when absent.
inline size_t bounded_find(kimix::string_view s, char ch, size_t i, size_t end) {
    if (i >= end || i >= s.size()) {
        return kimix::string_view::npos;
    }
    const size_t found = s.find(ch, i);
    if (found == kimix::string_view::npos || found >= end) {
        return kimix::string_view::npos;
    }
    return found;
}

// Next position at/after i holding one of `chars`, bounded by end.
inline size_t bounded_find_any(kimix::string_view s, kimix::string_view chars, size_t i, size_t end) {
    const size_t limit = end < s.size() ? end : s.size();
    for (size_t j = i; j < limit; ++j) {
        for (const char c : chars) {
            if (s[j] == c) {
                return j;
            }
        }
    }
    return kimix::string_view::npos;
}

// `render_splices(source, edits)` (translator.py): sorted one-pass splice;
// with no edits the input bytes are returned unchanged.  The reference sorts
// by the full (start, end, replacement) tuple; sorting by (start, end) is
// equivalent for the disjoint edit sets the scanner records.
kimix::string render_splices(kimix::string_view source, const kimix::vector<ScanReports::Edit> &edits) {
    if (edits.empty()) {
        return kimix::string(source);
    }
    kimix::vector<const ScanReports::Edit *> ordered;
    ordered.reserve(edits.size());
    for (const ScanReports::Edit &edit : edits) {
        ordered.push_back(&edit);
    }
    // Stable insertion sort by (start, end); edit counts are modest per scan.
    for (size_t i = 1; i < ordered.size(); ++i) {
        const ScanReports::Edit *key = ordered[i];
        size_t j = i;
        while (j > 0 && (ordered[j - 1]->start > key->start || (ordered[j - 1]->start == key->start && ordered[j - 1]->end > key->end))) {
            ordered[j] = ordered[j - 1];
            j -= 1;
        }
        ordered[j] = key;
    }
    kimix::string out;
    out.reserve(source.size() + 64);
    size_t previous = 0;
    for (const ScanReports::Edit *edit : ordered) {
        const size_t start = edit->start < source.size() ? edit->start : source.size();
        const size_t end = edit->end < source.size() ? edit->end : source.size();
        if (start > previous) {
            out.append(source.substr(previous, start - previous));
        }
        out.append(edit->replacement);
        if (end > previous) {
            previous = end;
        }
    }
    if (previous < source.size()) {
        out.append(source.substr(previous));
    }
    return out;
}

// `_opaque_inline_expansions(text)` (bash_core.py): copy inline-script text
// with every runtime expansion made opaque.  Returns (sanitized,
// substitutions).
struct OpaqueInline {
    kimix::string sanitized;
    kimix::vector<kimix::string> substitutions;
};

OpaqueInline opaque_inline_expansions(kimix::string_view text) {
    static constexpr kimix::string_view kPlaceholder = "__kimix_operand__";
    const size_t n = text.size();
    OpaqueInline result;
    kimix::string &out = result.sanitized;
    size_t i = 0;
    bool in_squote = false;
    while (i < n) {
        const char ch = text[i];
        if (in_squote) {
            out.push_back(ch);
            if (ch == '\'') {
                in_squote = false;
            }
            i += 1;
            continue;
        }
        if (ch == '\'') {
            in_squote = true;
            out.push_back(ch);
            i += 1;
            continue;
        }
        if (ch == '\\') {
            out.push_back(ch);
            if (i + 1 < n) {
                out.push_back(text[i + 1]);
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }
        if (ch == '"') {
            out.push_back(ch);
            i += 1;
            continue;
        }
        if (ch == '`') {
            const int64_t close = find_backtick_end(text, static_cast<int64_t>(i + 1));
            if (close < 0) {
                out.append(text.substr(i));
                break;
            }
            const size_t e = static_cast<size_t>(close);
            result.substitutions.emplace_back(text.substr(i + 1, e - 1 - (i + 1)));
            out.append(kPlaceholder);
            i = e;
            continue;
        }
        if (ch != '$' || i + 1 >= n) {
            out.push_back(ch);
            i += 1;
            continue;
        }
        const char nxt = text[i + 1];
        if (nxt == '(') {
            size_t end = 0;
            if (bp_starts_with(text.substr(i), "$((")) {
                const auto m = match_arithmetic_span(text, i + 3);
                if (!m.has_value()) {
                    out.append(text.substr(i));
                    break;
                }
                end = *m;
            } else {
                const int64_t close = find_matching_paren(text, static_cast<int64_t>(i + 1));
                if (close < 0) {
                    out.append(text.substr(i));
                    break;
                }
                const size_t c = static_cast<size_t>(close);
                result.substitutions.emplace_back(text.substr(i + 2, c - (i + 2)));
                end = c + 1;
            }
            out.append(kPlaceholder);
            i = end;
            continue;
        }
        if (nxt == '{') {
            const auto m = match_parameter_braces(text, i + 2);
            if (!m.has_value()) {
                out.append(text.substr(i));
                break;
            }
            out.append(kPlaceholder);
            i = *m;
            continue;
        }
        if (nxt == '\'') {
            const int64_t end = find_ansi_c_end(text, static_cast<int64_t>(i + 2));
            if (end < 0) {
                out.append(text.substr(i));
                break;
            }
            out.append(kPlaceholder);
            i = static_cast<size_t>(end);
            continue;
        }
        if (nxt == '"') {
            const int64_t end = find_dq_end(text, static_cast<int64_t>(i + 2));
            if (end < 0) {
                out.append(text.substr(i));
                break;
            }
            out.append(kPlaceholder);
            i = static_cast<size_t>(end);
            continue;
        }
        if (is_path_alpha(nxt) || nxt == '_') {
            size_t j = i + 2;
            while (j < n && (is_path_alpha(text[j]) || text[j] == '_' || is_ascii_digit(text[j]))) {
                j += 1;
            }
            out.append(kPlaceholder);
            i = j;
            continue;
        }
        // _SPECIAL_PARAM_CHARS = "0123456789?-$!#*@"
        if (is_ascii_digit(nxt) || nxt == '?' || nxt == '-' || nxt == '$' || nxt == '!' || nxt == '#' || nxt == '*' || nxt == '@') {
            out.append(kPlaceholder);
            i += 2;
            continue;
        }
        // A `$` starting no expansion is a literal dollar sign.
        out.push_back(ch);
        i += 1;
    }
    return result;
}

bool string_blank(kimix::string_view s) {
    // Python `str.strip()` emptiness: whitespace " \t\n\r\f\v".
    for (const char ch : s) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') {
            return false;
        }
    }
    return true;
}

// `_BashWrapper` port: the reference dataclass has exactly the same fields as
// wrappers.WrapperState, so the scan flow uses that type directly (the
// wrapper_step / wrapper_path_inline_replacement rules take it by reference).
using BashWrapper = WrapperState;

// `_BashHereDoc` port.
struct BashHereDoc {
    kimix::optional<kimix::string> delimiter;
    bool strip_tabs = false;
    bool expands = true;
};

// Convert the scan-flow's BashHereDoc list to the public PendingHeredoc list
// consumed by skip_heredoc_bodies.
inline kimix::vector<Scanner::PendingHeredoc> to_pending(const kimix::vector<BashHereDoc> &docs) {
    kimix::vector<Scanner::PendingHeredoc> out;
    out.reserve(docs.size());
    for (const BashHereDoc &doc : docs) {
        out.push_back(Scanner::PendingHeredoc{doc.delimiter, doc.strip_tabs, doc.expands});
    }
    return out;
}

// Dedicated large-stack runner: the synchronous scan recursion (bounded by
// _MAX_NESTING_DEPTH per chain) runs here so adversarial nesting cannot
// overflow a small caller stack (marl fibers use 1 MiB).
constexpr size_t kScanStackSize = 64u * 1024u * 1024u;

void run_on_big_stack(const kimix::function<void()> &body) {
#ifdef KIMIX_PLATFORM_WINDOWS
    struct Ctx {
        const kimix::function<void()> *fn;
    };
    Ctx ctx{&body};
    const uintptr_t handle = _beginthreadex(
        nullptr, static_cast<unsigned>(kScanStackSize),
        [](void *arg) -> unsigned {
            auto *c = static_cast<Ctx *>(arg);
            (*c->fn)();
            return 0;
        },
        &ctx, 0, nullptr);
    if (handle != 0) {
        ::WaitForSingleObject(reinterpret_cast<void *>(handle), INFINITE);
        ::CloseHandle(reinterpret_cast<void *>(handle));
        return;
    }
    body(); // fallback: run inline (thread creation failed)
#else
    pthread_attr_t attr;
    pthread_t thread;
    bool started = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, kScanStackSize) == 0) {
            struct Ctx {
                const kimix::function<void()> *fn;
            };
            Ctx ctx{&body};
            started = pthread_create(&thread, &attr, [](void *arg) -> void * {
                auto *c = static_cast<Ctx *>(arg);
                (*c->fn)();
                return nullptr;
            }, &ctx) == 0;
        }
        pthread_attr_destroy(&attr);
    }
    if (started) {
        pthread_join(thread, nullptr);
    } else {
        body();
    }
#endif
}

} // namespace src_llm_bash_parser_bp_scanner
using namespace src_llm_bash_parser_bp_scanner;

// ── Scanner::Impl ────────────────────────────────────────────────────────────

struct Scanner::Impl {
    int32_t nest_depth = 0; // `_scan_range`/`_find_matching` budget counter
};

Scanner::Scanner(kimix::string_view command) : Scanner(command, kimix::string_view()) {}

Scanner::Scanner(kimix::string_view command, kimix::string_view temp_dir)
    : impl_(std::make_unique<Impl>()), owned_(command), s_(owned_), n_(owned_.size()) {
    if (temp_dir.empty()) {
        temp_dir_ = windows_temp_dir();
    } else {
        temp_dir_ = kimix::string(temp_dir);
        for (char &ch : temp_dir_) {
            if (ch == '\\') {
                ch = '/';
            }
        }
    }
}

Scanner::~Scanner() = default;
Scanner::Scanner(Scanner &&) noexcept = default;
Scanner &Scanner::operator=(Scanner &&) noexcept = default;

int32_t Scanner::nest_depth() const { return impl_ ? impl_->nest_depth : 0; }

// ── report recorders ─────────────────────────────────────────────────────────

void Scanner::record_edit(size_t start, size_t end, kimix::string replacement) {
    reports_.edits.push_back(ScanReports::Edit{start, end, std::move(replacement)});
}

void Scanner::record_name(const kimix::string &name) { reports_.names.push_back(name); }
void Scanner::record_path(const kimix::string &raw) { reports_.path_notes.push_back(raw); }
void Scanner::record_shell(const kimix::string &name) { reports_.shell_notes.push_back(name); }
void Scanner::record_nul(const kimix::string &raw) { reports_.nul_fixes.push_back(raw); }
void Scanner::record_cmd_conditional(const kimix::string &note) { reports_.cmd_conditional_notes.push_back(note); }

bool Scanner::record_unsupported(kimix::string_view raw, const kimix::optional<kimix::string> &decoded, bool decoded_valid) {
    // `_record_unsupported(raw, name=_UNDECODED)`: the name is the decoded
    // literal when pre-decoded (nullopt included), else decoded here through
    // `_literal_unsupported_name` (which maps non-table values to None).
    kimix::optional<kimix::string> name;
    if (decoded_valid) {
        name = decoded;
    } else {
        const auto value = literal_word_value(raw);
        if (value.has_value() && unsupported_reason(*value).has_value()) {
            name = value;
        }
    }
    if (!name.has_value() || !unsupported_reason(*name).has_value()) {
        return false;
    }
    for (const kimix::string &existing : reports_.unsupported) {
        if (existing == *name) {
            return true; // first-occurrence dedup
        }
    }
    reports_.unsupported.push_back(*name);
    return true;
}

void Scanner::merge_inner(const Scanner &inner) {
    const ScanReports &in = inner.reports_;
    auto dedup_extend = [](kimix::vector<kimix::string> &target, const kimix::vector<kimix::string> &source) {
        for (const kimix::string &name : source) {
            bool seen = false;
            for (const kimix::string &existing : target) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                target.push_back(name);
            }
        }
    };
    dedup_extend(reports_.names, in.names);
    reports_.path_notes.insert(reports_.path_notes.end(), in.path_notes.begin(), in.path_notes.end());
    dedup_extend(reports_.shell_notes, in.shell_notes);
    reports_.nul_fixes.insert(reports_.nul_fixes.end(), in.nul_fixes.begin(), in.nul_fixes.end());
    reports_.cmd_conditional_notes.insert(reports_.cmd_conditional_notes.end(), in.cmd_conditional_notes.begin(), in.cmd_conditional_notes.end());
    dedup_extend(reports_.unsupported, in.unsupported);
}

// ── reader primitives ────────────────────────────────────────────────────────

kimix::optional<kimix::string> Scanner::literal_word_value(kimix::string_view raw) {
    return bash_parser::literal_word_value(raw);
}

bool Scanner::redirection_after_fd_at(size_t i, size_t end) const {
    return redirection_after_fd(s_, i, end);
}

std::pair<kimix::string_view, size_t> Scanner::read_shell_control_operator_at(size_t i, size_t end) const {
    kimix::string_view op;
    const size_t after = read_shell_control_operator(s_, i, end, &op);
    return {op, after};
}

kimix::optional<size_t> Scanner::empty_parentheses_end(size_t i, size_t end) const {
    i = hspace_end(s_, i, end);
    if (i >= end || s_[i] != '(') {
        return std::nullopt;
    }
    i += 1;
    i = hspace_end(s_, i, end);
    if (i < end && s_[i] == ')') {
        return i + 1;
    }
    return std::nullopt;
}

kimix::optional<size_t> Scanner::function_declaration_end(kimix::string_view raw, size_t i, size_t end) const {
    if (raw.empty() || !(is_path_alpha(raw[0]) || raw[0] == '_')) {
        return std::nullopt;
    }
    if (!is_name_word(raw)) {
        return std::nullopt;
    }
    return empty_parentheses_end(i, end);
}

bool Scanner::coproc_name_before_compound(kimix::string_view raw, size_t i, size_t end) const {
    if (!is_name_word(raw)) {
        return false;
    }
    i = hspace_end(s_, i, end);
    if (i >= end) {
        return false;
    }
    const kimix::string_view rest = s_.substr(i, end - i);
    if (bp_starts_with(rest, "{") || bp_starts_with(rest, "(") || bp_starts_with(rest, "[[")) {
        return true;
    }
    static const char *const kKeywords[] = {"case", "for", "if", "select", "until", "while"};
    for (const char *const keyword : kKeywords) {
        const size_t klen = kimix::string_view(keyword).size();
        if (!bp_starts_with(rest, keyword)) {
            continue;
        }
        if (i + klen >= end) {
            return true;
        }
        const char after = s_[i + klen];
        if (after == ' ' || after == '\t' || after == '\r' || after == '\n' || after == ';' || after == '&' || after == '|' || after == '(' || after == ')' || after == '<' || after == '>' || after == '{' || after == '}') {
            return true;
        }
    }
    return false;
}

kimix::optional<kimix::string> Scanner::path_replacement_for(kimix::string_view raw) const {
    return path_replacement(raw, temp_dir_);
}

kimix::optional<kimix::string_view> Scanner::classify_pattern_option_for(kimix::string_view family, kimix::string_view word) {
    return classify_pattern_option(family, word);
}

// ── word reader + region skippers ────────────────────────────────────────────

size_t Scanner::skip_single_quote(size_t i, size_t end) const {
    const size_t close = bounded_find(s_, '\'', i, end);
    return close == kimix::string_view::npos ? end : close + 1;
}

size_t Scanner::skip_ansi_quote(size_t i, size_t end) const {
    while (i < end) {
        if (s_[i] == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (s_[i] == '\'') {
            return i + 1;
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::scanner_find_backtick_end(size_t i, size_t end) const {
    // Bounded `_find_backtick_end` method: returns the index OF the closing
    // backtick, or `end` when the region is unterminated in the range.
    static constexpr kimix::string_view kSpecial = "\\`";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        if (s_[i] == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else {
            return i;
        }
    }
    return end;
}

size_t Scanner::skip_double_quote_scan(size_t i, size_t end) {
    // `_skip_double_quote`: closes the region, scanning $()/backtick
    // substitution contents as command contexts.
    static constexpr kimix::string_view kSpecial = "\\\"`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '\\') {
            if (i + 1 < end && (s_[i + 1] == '$' || s_[i + 1] == '`' || s_[i + 1] == '"' || s_[i + 1] == '\\' || s_[i + 1] == '\n')) {
                i += 2;
            } else {
                i += 1;
            }
        } else if (ch == '"') {
            return i + 1;
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            scan_range(i + 1, close);
            i = close < end ? close + 1 : end;
        } else if (bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (bp_starts_with(s_.substr(i, end - i), "${")) {
            i = skip_parameter(i + 2, end);
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::skip_double_quote_for_matching(size_t i, size_t end) {
    static constexpr kimix::string_view kSpecial = "\\\"`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '\\') {
            if (i + 1 < end && (s_[i + 1] == '$' || s_[i + 1] == '`' || s_[i + 1] == '"' || s_[i + 1] == '\\' || s_[i + 1] == '\n')) {
                i += 2;
            } else {
                i += 1;
            }
        } else if (ch == '"') {
            return i + 1;
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            i = close < end ? close + 1 : end;
        } else if (bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            i = close < end ? close + 1 : end;
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::skip_parameter_literal(size_t i, size_t end) const {
    int32_t depth = 1;
    static constexpr kimix::string_view kSpecial = "\\\'\"{}";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        if (s_[i] == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (s_[i] == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (s_[i] == '"') {
            // The reference calls the *const* matching variant here.
            size_t j = i + 1;
            static constexpr kimix::string_view kDq = "\\\"`$";
            while (j < end) {
                const size_t dm = bounded_find_any(s_, kDq, j, end);
                if (dm == kimix::string_view::npos) {
                    j = end;
                    break;
                }
                j = dm;
                const char ch = s_[j];
                if (ch == '\\') {
                    if (j + 1 < end && (s_[j + 1] == '$' || s_[j + 1] == '`' || s_[j + 1] == '"' || s_[j + 1] == '\\' || s_[j + 1] == '\n')) {
                        j += 2;
                    } else {
                        j += 1;
                    }
                } else if (ch == '"') {
                    j += 1;
                    break;
                } else if (ch == '`') {
                    const size_t close = scanner_find_backtick_end(j + 1, end);
                    j = close < end ? close + 1 : end;
                } else if (bp_starts_with(s_.substr(j, end - j), "$(") && !bp_starts_with(s_.substr(j, end - j), "$((")) {
                    // `_skip_double_quote_for_matching` calls _find_matching;
                    // from this const context we mirror it non-recursively via
                    // the module oracle on the full string, bounded to end.
                    const int64_t close = find_matching_paren(s_, static_cast<int64_t>(j + 1));
                    j = (close >= 0 && static_cast<size_t>(close) < end) ? static_cast<size_t>(close) + 1 : end;
                } else {
                    j += 1;
                }
            }
            i = j;
        } else if (s_[i] == '{') {
            depth += 1;
            i += 1;
        } else if (s_[i] == '}') {
            depth -= 1;
            i += 1;
            if (depth == 0) {
                return i;
            }
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::skip_parameter(size_t i, size_t end) {
    int32_t depth = 1;
    static constexpr kimix::string_view kSpecial = "\\$\'\"{}";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (ch == '"') {
            i = skip_double_quote_scan(i + 1, end);
        } else if (ch == '{') {
            depth += 1;
            i += 1;
        } else if (ch == '}') {
            depth -= 1;
            i += 1;
            if (depth == 0) {
                return i;
            }
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::skip_arithmetic(size_t i, size_t end) {
    int32_t depth = 1;
    static constexpr kimix::string_view kSpecial = "\\()'\"$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (ch == '(' && bp_starts_with(s_.substr(i, end - i), "((")) {
            depth += 1;
            i += 2;
        } else if (ch == ')' && bp_starts_with(s_.substr(i, end - i), "))")) {
            depth -= 1;
            i += 2;
            if (depth == 0) {
                return i;
            }
        } else if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (ch == '"') {
            i = skip_double_quote_scan(i + 1, end);
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::skip_conditional(size_t i, size_t end) {
    // `_skip_conditional`: skip a [[ ... ]] expression, scanning its
    // substitutions.
    static constexpr kimix::string_view kSpecial = "\\]'\"`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (ch == ']' && bp_starts_with(s_.substr(i, end - i), "]]")) {
            return i + 2;
        }
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$'")) {
            i = skip_ansi_quote(i + 2, end);
        } else if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (ch == '"') {
            i = skip_double_quote_scan(i + 1, end);
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            scan_range(i + 1, close);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$((")) {
            i = skip_arithmetic(i + 3, end);
        } else {
            i += 1;
        }
    }
    return end;
}

void Scanner::scan_expansions(size_t i, size_t end) {
    // `_scan_expansions`: scan executable substitutions in a region whose
    // plain words are data.
    static constexpr kimix::string_view kSpecial = "\\\'\"`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$'")) {
            i = skip_ansi_quote(i + 2, end);
        } else if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (ch == '"') {
            i = skip_double_quote_scan(i + 1, end);
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            scan_range(i + 1, close);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$((")) {
            i = skip_arithmetic(i + 3, end);
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "${")) {
            i = skip_parameter(i + 2, end);
        } else {
            i += 1;
        }
    }
}

void Scanner::scan_heredoc_expansions(size_t i, size_t end) {
    // `_scan_heredoc_expansions`: quotes are literal in heredoc bodies; only
    // a backslash can suppress the expansion introducers.
    static constexpr kimix::string_view kSpecial = "\\`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return;
        }
        i = m;
        const char ch = s_[i];
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            scan_range(i + 1, close);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$(") && !bp_starts_with(s_.substr(i, end - i), "$((")) {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$((")) {
            i = skip_arithmetic(i + 3, end);
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "${")) {
            i = skip_parameter(i + 2, end);
        } else {
            i += 1;
        }
    }
}

void Scanner::scan_array_words(size_t i, size_t end) {
    // `_scan_array_words`: array-literal elements are data words; still
    // rewritten as paths and their substitutions scanned.
    while (i < end) {
        const char ch = s_[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            i += 1;
            continue;
        }
        if (ch == '\\' && i + 1 < end && s_[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (ch == '#' && comment_starts(s_, i, 0)) {
            const size_t newline = bounded_find(s_, '\n', i + 1, end);
            i = newline == kimix::string_view::npos ? end : newline;
            continue;
        }
        const size_t word_end = read_word(i, end, true);
        if (word_end <= i) {
            i += 1;
            continue;
        }
        const kimix::string_view raw = s_.substr(i, word_end - i);
        const auto replacement = path_replacement_for(raw);
        if (replacement.has_value()) {
            record_edit(i, word_end, *replacement);
            record_path(kimix::string(raw));
        }
        i = word_end;
    }
}

size_t Scanner::find_matching_impl(size_t i, size_t end) {
    // `_find_matching(i, end, ')')`: depth-budgeted wrapper.
    if (i >= end) {
        return end;
    }
    if (impl_->nest_depth >= kMaxNestingDepth) {
        return end;
    }
    impl_->nest_depth += 1;
    const size_t result = find_matching_inner(i, end);
    impl_->nest_depth -= 1;
    return result;
}

size_t Scanner::find_matching_inner(size_t i, size_t end) {
    // `_find_matching_inner`: the flat paren matcher (heredoc + case aware).
    int32_t depth = 0;
    kimix::vector<BashHereDoc> pending_heredocs;
    kimix::vector<kimix::string> case_stack;
    while (i < end) {
        const char ch = s_[i];
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
        } else if (ch == '\n') {
            i += 1;
            if (!pending_heredocs.empty()) {
                i = skip_heredoc_bodies(i, end, to_pending(pending_heredocs), false);
                pending_heredocs.clear();
            }
        } else if (ch == '$' && bp_starts_with(s_.substr(i, end - i), "$((")) {
            i = skip_arithmetic(i + 3, end);
            if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            }
        } else if (ch == '<' && bp_starts_with(s_.substr(i, end - i), "<<") && !bp_starts_with(s_.substr(i, end - i), "<<<")) {
            const bool strip_tabs = bp_starts_with(s_.substr(i, end - i), "<<-");
            size_t delimiter_start = i + (strip_tabs ? 3 : 2);
            delimiter_start = hspace_end(s_, delimiter_start, end);
            const size_t delimiter_end = read_word(delimiter_start, end, false);
            const auto hd = heredoc_delimiter(s_.substr(delimiter_start, delimiter_end > delimiter_start ? delimiter_end - delimiter_start : 0));
            if (hd.has_value()) {
                pending_heredocs.push_back(BashHereDoc{hd->delimiter, strip_tabs, hd->expands});
            }
            i = delimiter_end > delimiter_start ? delimiter_end : delimiter_start;
        } else if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
        } else if (ch == '"') {
            i = skip_double_quote_for_matching(i + 1, end);
        } else if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            i = close < end ? close + 1 : end;
            if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            }
        } else if (ch == '#' && comment_starts(s_, i, 0)) {
            const size_t newline = bounded_find(s_, '\n', i + 1, end);
            i = newline == kimix::string_view::npos ? end : newline;
        } else if (ch == ';' && bp_starts_with(s_.substr(i, end - i), ";;&")) {
            if (!case_stack.empty()) {
                case_stack.back() = "patterns";
            }
            i += 3;
        } else if (ch == ';' && (bp_starts_with(s_.substr(i, end - i), ";;") || bp_starts_with(s_.substr(i, end - i), ";&"))) {
            if (!case_stack.empty()) {
                case_stack.back() = "patterns";
            }
            i += 2;
        } else if (!in_word_end_chars(ch)) {
            const size_t word_end = read_word(i, end, false);
            if (word_end <= i) {
                i += 1;
                continue;
            }
            const kimix::string_view word = s_.substr(i, word_end - i);
            if (word == "case") {
                case_stack.push_back("word");
            } else if (!case_stack.empty() && (case_stack.back() == "patterns" || case_stack.back() == "body") && word == "esac") {
                case_stack.pop_back();
            } else if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            } else if (!case_stack.empty() && case_stack.back() == "await-in" && word == "in") {
                case_stack.back() = "patterns";
            }
            i = word_end;
        } else if (ch == '(') {
            depth += 1;
            i += 1;
        } else if (ch == ')') {
            if (!case_stack.empty() && case_stack.back() == "patterns") {
                case_stack.back() = "body";
                i += 1;
            } else if (depth == 0) {
                return i;
            } else {
                depth -= 1;
                i += 1;
            }
        } else {
            i += 1;
        }
    }
    return end;
}

size_t Scanner::find_matching(size_t i, size_t end) {
    return find_matching_impl(i, end);
}

void Scanner::read_redirection_at(size_t i, size_t end, kimix::string_view *op_out, size_t *after_out) const {
    *after_out = read_redirection_operator(s_, i, end, op_out);
}

// ── heredoc ──────────────────────────────────────────────────────────────────

kimix::optional<Scanner::HeredocDelimiter> Scanner::heredoc_delimiter(kimix::string_view raw) {
    // `_heredoc_delimiter(raw)`: returns nullopt for an empty word; else the
    // (delimiter|nullopt, expands) pair.  A malformed ANSI-C delimiter yields
    // a nullopt `delimiter` (unmatchable: no body is claimed).
    if (raw.empty()) {
        return std::nullopt;
    }
    kimix::string result;
    bool quoted = false;
    bool matchable = true;
    size_t i = 0;
    while (i < raw.size()) {
        const char ch = raw[i];
        if (bp_starts_with(raw.substr(i), "$'")) {
            quoted = true;
            // `_read_ansi_c_delimiter(raw, i+2)` -> (value, next_i, valid)
            size_t j = i + 2;
            kimix::string value;
            bool valid = true;
            bool nul_seen = false;
            while (j < raw.size()) {
                if (raw[j] == '\'') {
                    j += 1;
                    break;
                }
                if (raw[j] != '\\' || j + 1 >= raw.size()) {
                    if (!nul_seen) {
                        value.push_back(raw[j]);
                    }
                    j += 1;
                    continue;
                }
                const char esc = raw[j + 1];
                char simple = 0;
                bool has_simple = true;
                switch (esc) {
                case 'a': simple = '\a'; break;
                case 'b': simple = '\b'; break;
                case 'e':
                case 'E': simple = '\x1b'; break;
                case 'f': simple = '\f'; break;
                case 'n': simple = '\n'; break;
                case 'r': simple = '\r'; break;
                case 't': simple = '\t'; break;
                case 'v': simple = '\v'; break;
                case '\\': simple = '\\'; break;
                case '\'': simple = '\''; break;
                case '"': simple = '"'; break;
                case '?': simple = '?'; break;
                default: has_simple = false; break;
                }
                if (has_simple) {
                    if (!nul_seen) {
                        value.push_back(simple);
                    }
                    j += 2;
                    continue;
                }
                if (esc >= '0' && esc <= '7') {
                    size_t k = j + 1;
                    const size_t lim = raw.size() < j + 4 ? raw.size() : j + 4;
                    while (k < lim && raw[k] >= '0' && raw[k] <= '7') {
                        k += 1;
                    }
                    int32_t code = 0;
                    for (size_t d = j + 1; d < k; ++d) {
                        code = code * 8 + (raw[d] - '0');
                    }
                    if (code == 0) {
                        nul_seen = true;
                    } else if (!nul_seen) {
                        value.push_back(static_cast<char>(code));
                    }
                    j = k;
                    continue;
                }
                if (esc == 'x' || esc == 'X' || esc == 'u' || esc == 'U') {
                    const size_t width = (esc == 'x' || esc == 'X') ? 2 : (esc == 'u' ? 4 : 8);
                    size_t k = j + 2;
                    size_t lim = raw.size() < k + width ? raw.size() : k + width;
                    auto is_hex = [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                    };
                    while (k < lim && is_hex(raw[k])) {
                        k += 1;
                    }
                    if (k > j + 2) {
                        uint32_t value32 = 0;
                        for (size_t d = j + 2; d < k; ++d) {
                            const char c = raw[d];
                            uint32_t v = 0;
                            if (c >= '0' && c <= '9') {
                                v = static_cast<uint32_t>(c - '0');
                            } else if (c >= 'a' && c <= 'f') {
                                v = static_cast<uint32_t>(c - 'a' + 10);
                            } else {
                                v = static_cast<uint32_t>(c - 'A' + 10);
                            }
                            value32 = value32 * 16 + v;
                        }
                        if (value32 == 0) {
                            nul_seen = true;
                        } else if (value32 <= 0x10FFFF && !(value32 >= 0xD800 && value32 <= 0xDFFF)) {
                            if (!nul_seen) {
                                // Encode the code point as UTF-8 (Python chr()).
                                if (value32 < 0x80) {
                                    value.push_back(static_cast<char>(value32));
                                } else if (value32 < 0x800) {
                                    value.push_back(static_cast<char>(0xC0 | (value32 >> 6)));
                                    value.push_back(static_cast<char>(0x80 | (value32 & 0x3F)));
                                } else if (value32 < 0x10000) {
                                    value.push_back(static_cast<char>(0xE0 | (value32 >> 12)));
                                    value.push_back(static_cast<char>(0x80 | ((value32 >> 6) & 0x3F)));
                                    value.push_back(static_cast<char>(0x80 | (value32 & 0x3F)));
                                } else {
                                    value.push_back(static_cast<char>(0xF0 | (value32 >> 18)));
                                    value.push_back(static_cast<char>(0x80 | ((value32 >> 12) & 0x3F)));
                                    value.push_back(static_cast<char>(0x80 | ((value32 >> 6) & 0x3F)));
                                    value.push_back(static_cast<char>(0x80 | (value32 & 0x3F)));
                                }
                            }
                        } else {
                            valid = false;
                            value.append(raw.substr(j, k - j));
                        }
                        j = k;
                        continue;
                    }
                }
                if (!nul_seen) {
                    value.push_back('\\');
                    value.push_back(esc);
                }
                j += 2;
            }
            result.append(value);
            matchable = matchable && valid;
            i = j;
        } else if (ch == '\'') {
            quoted = true;
            const size_t close = raw.find('\'', i + 1);
            if (close == kimix::string_view::npos) {
                result.append(raw.substr(i + 1));
                i = raw.size();
            } else {
                result.append(raw.substr(i + 1, close - i - 1));
                i = close + 1;
            }
        } else if (ch == '"' || bp_starts_with(raw.substr(i), "$\"")) {
            quoted = true;
            i += (ch != '"') ? 2 : 1;
            while (i < raw.size() && raw[i] != '"') {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    const char escaped = raw[i + 1];
                    if (escaped == '$' || escaped == '`' || escaped == '"' || escaped == '\\' || escaped == '\n') {
                        if (escaped != '\n') {
                            result.push_back(escaped);
                        }
                        i += 2;
                        continue;
                    }
                }
                result.push_back(raw[i]);
                i += 1;
            }
            if (i < raw.size()) {
                i += 1;
            }
        } else if (ch == '\\' && i + 1 < raw.size()) {
            quoted = true;
            result.push_back(raw[i + 1]);
            i += 2;
        } else {
            result.push_back(ch);
            i += 1;
        }
    }
    HeredocDelimiter hd;
    hd.delimiter = matchable ? kimix::optional<kimix::string>(result) : std::nullopt;
    hd.expands = !quoted;
    return hd;
}

// (to_pending helper moved to the anonymous namespace above.)

size_t Scanner::skip_heredoc_bodies(size_t i, size_t end, const kimix::vector<PendingHeredoc> &documents, bool scan_expansions) {
    // `_skip_heredoc_bodies`: claim each pending heredoc body; append the
    // single (redir_line_end, terminator_end) event for the group.
    int64_t redir_line_end = -1;
    for (const PendingHeredoc &document : documents) {
        const size_t body_start = i;
        if (redir_line_end < 0) {
            redir_line_end = static_cast<int64_t>(body_start) - 1;
        }
        kimix::string logical_line;
        size_t logical_start = i;
        bool terminated = false;
        while (i < end) {
            const size_t newline = bounded_find(s_, '\n', i, end);
            const size_t line_end = newline == kimix::string_view::npos ? end : newline;
            // rstrip("\r")
            size_t le = line_end;
            while (le > i && s_[le - 1] == '\r') {
                le -= 1;
            }
            kimix::string_view line = s_.substr(i, le - i);
            kimix::string_view compare = line;
            if (document.strip_tabs) {
                size_t t = 0;
                while (t < compare.size() && compare[t] == '\t') {
                    t += 1;
                }
                compare = compare.substr(t);
            }
            if (logical_line.empty()) {
                logical_start = i;
            }
            const bool continues = heredoc_line_continues(compare);
            if (document.expands && continues) {
                logical_line.append(compare.substr(0, compare.size() - 1));
                i = newline == kimix::string_view::npos ? end : newline + 1;
                continue;
            }
            logical_line.append(compare);
            if (document.delimiter.has_value() && logical_line == *document.delimiter) {
                if (scan_expansions && document.expands) {
                    scan_heredoc_expansions(body_start, logical_start);
                }
                i = newline == kimix::string_view::npos ? end : newline + 1;
                terminated = true;
                break;
            }
            logical_line.clear();
            i = newline == kimix::string_view::npos ? end : newline + 1;
        }
        if (!terminated && scan_expansions && document.expands) {
            scan_heredoc_expansions(body_start, end);
        }
    }
    if (redir_line_end >= 0) {
        reports_.heredoc_events.emplace_back(redir_line_end, static_cast<int64_t>(i));
    }
    return i;
}

// ── word reader ──────────────────────────────────────────────────────────────

size_t Scanner::read_word(size_t start, size_t end, bool scan_substitutions) {
    size_t i = start;
    static constexpr kimix::string_view kWordSpecial = ";&|()<>\n \t\r#\\'\"`$";
    while (i < end) {
        const size_t m = bounded_find_any(s_, kWordSpecial, i, end);
        if (m == kimix::string_view::npos) {
            return end;
        }
        i = m;
        const char ch = s_[i];
        if (in_word_end_chars(ch)) {
            break;
        }
        if (ch == '#') {
            if (i == start) {
                break;
            }
            i += 1;
            continue;
        }
        if (ch == '\\') {
            i += (i + 1 < end) ? 2 : 1;
            continue;
        }
        if (ch == '\'') {
            i = skip_single_quote(i + 1, end);
            continue;
        }
        if (ch == '"') {
            i = scan_substitutions ? skip_double_quote_scan(i + 1, end) : skip_double_quote_for_matching(i + 1, end);
            continue;
        }
        if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            if (scan_substitutions) {
                scan_range(i + 1, close);
            }
            i = close < end ? close + 1 : end;
            continue;
        }
        // ch == '$'
        const char nxt = i + 1 < end ? s_[i + 1] : '\0';
        if (nxt == '(') {
            if (bp_starts_with(s_.substr(i, end - i), "$((")) {
                i = skip_arithmetic(i + 3, end);
                continue;
            }
            const size_t close = find_matching_impl(i + 2, end);
            if (scan_substitutions) {
                scan_range(i + 2, close < end ? close : end);
            }
            i = close < end ? close + 1 : end;
            continue;
        }
        if (nxt == '{') {
            i = scan_substitutions ? skip_parameter(i + 2, end) : skip_parameter_literal(i + 2, end);
            continue;
        }
        if (nxt == '\'') {
            i = skip_ansi_quote(i + 2, end);
            continue;
        }
        i += 1;
    }
    return i;
}

// ── nested-script repair entry points ────────────────────────────────────────

std::optional<std::pair<size_t, bool>> Scanner::handle_shell_wrapper(const kimix::string &shell_name, size_t word_start, size_t i, size_t end, bool assignment_prefix, bool wrapped) {
    // `_handle_shell_wrapper`: unwrap/fix_script/keep decision via
    // wrappers.unwrap_decision; the edit/note/merge tail lives here.
    const UnwrapDecision decision = unwrap_decision(*this, s_, i, end, assignment_prefix, wrapped);
    if (decision.action == UnwrapAction::kKeep) {
        if (decision.harvest && decision.script_span.has_value()) {
            harvest_nonliteral_inline_script(s_.substr(decision.script_span->first, decision.script_span->second - decision.script_span->first));
        }
        return std::nullopt;
    }
    if (decision.action == UnwrapAction::kUnwrap) {
        const size_t j = *decision.scan_index;
        record_edit(word_start, j, kimix::string());
        record_shell(shell_name);
        return std::make_pair(j, wrapped);
    }
    // fix_script
    const size_t k = decision.script_span->first;
    const size_t script_end = decision.script_span->second;
    const auto script_opt = literal_word_value(s_.substr(k, script_end - k));
    if (!script_opt.has_value()) {
        return std::make_pair(script_end, wrapped);
    }
    const kimix::string &script = *script_opt;
    Scanner inner(script, temp_dir_);
    inner.scan_all();
    const kimix::string fixed = inner.build_fixed_source(inner.build_source(), inner.reports_.edits.empty());
    merge_inner(inner);
    if (!inner.reports_.edits.empty()) {
        record_edit(k, script_end, single_quote(fixed));
    }
    return std::make_pair(script_end, wrapped);
}

kimix::optional<size_t> Scanner::fix_shell_c_script_in_place(size_t i, size_t end) {
    // `_fix_shell_c_script_in_place`: locate the -c script operand and repair
    // it in place; returns the index after the script word, or nullopt.
    const auto found = shell_c_script_range(*this, s_, i, end);
    if (!found.has_value()) {
        return std::nullopt;
    }
    const size_t script_start = found->first;
    const size_t script_end = found->second;
    const kimix::string_view script_raw = s_.substr(script_start, script_end - script_start);
    const auto script_opt = literal_word_value(script_raw);
    if (!script_opt.has_value()) {
        harvest_nonliteral_inline_script(script_raw);
        return script_end;
    }
    const kimix::string &script = *script_opt;
    Scanner inner(script, temp_dir_);
    inner.scan_all();
    const ScanReports &in = inner.reports_;
    if (in.names.empty() && in.unsupported.empty() && in.nul_fixes.empty() && in.path_notes.empty() && in.cmd_conditional_notes.empty()) {
        return script_end;
    }
    merge_inner(inner);
    if (!in.edits.empty()) {
        record_edit(script_start, script_end, single_quote(inner.build_fixed_source(inner.build_source(), inner.reports_.edits.empty())));
    }
    return script_end;
}

bool Scanner::inline_script_operand(size_t word_start, size_t word_end, kimix::string_view raw) {
    // `_inline_script_operand`: repair a quoted runtime-reparsed operand.
    if (!is_inline_script_word(raw)) {
        return false;
    }
    const auto script_opt = literal_word_value(raw);
    if (!script_opt.has_value()) {
        harvest_nonliteral_inline_script(raw);
        return true;
    }
    const kimix::string &script = *script_opt;
    Scanner inner(script, temp_dir_);
    inner.scan_all();
    const kimix::string fixed = inner.build_fixed_source(inner.build_source(), inner.reports_.edits.empty());
    merge_inner(inner);
    if (!inner.reports_.edits.empty()) {
        record_edit(word_start, word_end, single_quote(fixed));
    }
    return true;
}

void Scanner::harvest_nonliteral_inline_script(kimix::string_view raw) {
    // `_harvest_nonliteral_inline_script`: register fallbacks from an
    // expansion-bearing inline script; only names/shell/unsupported merged.
    kimix::string_view inner_text = raw;
    if (raw.size() >= 2 && (raw[0] == '\'' || raw[0] == '"') && raw.back() == raw[0]) {
        inner_text = raw.substr(1, raw.size() - 2);
    }
    const OpaqueInline opaque = opaque_inline_expansions(inner_text);
    kimix::vector<kimix::string_view> sources;
    sources.push_back(kimix::string_view(opaque.sanitized));
    for (const kimix::string &sub : opaque.substitutions) {
        sources.push_back(kimix::string_view(sub));
    }
    for (const kimix::string_view source : sources) {
        if (string_blank(source)) {
            continue;
        }
        Scanner inner(source, temp_dir_);
        inner.scan_all();
        // Only names/shell_notes/unsupported are merged (dedup); path/nul/cmd
        // are NOT (the operand text stays byte-for-byte).
        for (const kimix::string &name : inner.reports_.names) {
            bool seen = false;
            for (const kimix::string &existing : reports_.names) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                reports_.names.push_back(name);
            }
        }
        for (const kimix::string &name : inner.reports_.shell_notes) {
            bool seen = false;
            for (const kimix::string &existing : reports_.shell_notes) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                reports_.shell_notes.push_back(name);
            }
        }
        for (const kimix::string &name : inner.reports_.unsupported) {
            bool seen = false;
            for (const kimix::string &existing : reports_.unsupported) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                reports_.unsupported.push_back(name);
            }
        }
    }
}

kimix::optional<Scanner::AtPrefixResult> Scanner::try_at_prefix(size_t word_start, size_t word_end, size_t end) {
    // `_try_at_prefix`: decision via cmd_translator.at_prefix_rule; the
    // edit/note apply lives here.
    const auto decision = at_prefix_rule(*this, s_, word_start, word_end, end);
    if (!decision.has_value()) {
        return std::nullopt;
    }
    record_edit(decision->edit_start, decision->edit_end, decision->edit_replacement);
    if (decision->note.has_value()) {
        record_cmd_conditional(*decision->note);
    }
    AtPrefixResult result;
    result.index = decision->index;
    result.statement = decision->statement;
    return result;
}

kimix::optional<size_t> Scanner::try_cmd_if(size_t if_start, size_t if_end, size_t end) {
    // `_try_cmd_if`: validate + note + commit.
    const auto match = validate_cmd_if(*this, s_, if_start, if_end, end);
    if (!match.has_value()) {
        return std::nullopt;
    }
    record_cmd_conditional(kimix::string(s_.substr(if_start, match->segments[0].op_end - if_start)));
    return commit_cmd_if(*this, s_, *match);
}

kimix::optional<size_t> Scanner::try_cmd_for(size_t for_start, size_t for_end, size_t end) {
    // `_try_cmd_for`: validate + note + commit.
    const auto match = validate_cmd_for(*this, s_, for_start, for_end, end);
    if (!match.has_value()) {
        return std::nullopt;
    }
    record_cmd_conditional(kimix::string(s_.substr(for_start, match->close_pos + 1 - for_start)));
    return commit_cmd_for(*this, s_, *match);
}

bool Scanner::record_cmd_jump(kimix::string_view raw, const kimix::optional<kimix::string> &literal, bool literal_valid, size_t word_end, size_t end) {
    // `_record_cmd_jump`: refusal key + dedup-first-wins append.
    const auto key = cmd_jump_record(*this, s_, raw, literal, literal_valid, word_end, end);
    if (!key.has_value()) {
        return false;
    }
    for (const kimix::string &existing : reports_.unsupported) {
        if (existing == *key) {
            return true;
        }
    }
    reports_.unsupported.emplace_back(*key);
    return true;
}

kimix::optional<kimix::string> Scanner::peek_literal_word(size_t pos, size_t end) {
    return bash_parser::peek_literal_word(*this, s_, pos, end);
}

void Scanner::drop_cmd_cd_flag(size_t i, size_t end) {
    // `_drop_cmd_cd_flag`: the /d flag span via paths.cd_flag_span_after.
    WordReader reader;
    reader.read_word = [this](size_t a, size_t b) { return read_word(a, b, false); };
    reader.redirection_after_fd = [this](size_t a, size_t b) { return redirection_after_fd_at(a, b); };
    const auto span = cd_flag_span_after(reader, s_, i, end);
    if (!span.has_value()) {
        return;
    }
    record_edit(span->first, span->second, kimix::string());
    record_path("cd /d");
}

// ── scan flow ────────────────────────────────────────────────────────────────

void Scanner::scan_range(size_t start, size_t end) {
    // `_scan_range`: depth-budgeted command-context scan.  Empty ranges are
    // skipped without touching the budget.
    if (start >= end) {
        return;
    }
    if (impl_->nest_depth >= kMaxNestingDepth) {
        return;
    }
    impl_->nest_depth += 1;
    scan_range_inner(start, end);
    impl_->nest_depth -= 1;
}

void Scanner::scan_range_inner(size_t start, size_t end) {
    // `_scan_range_inner`: the linear word/redirection walk.  A line-for-line
    // port; every rule decision delegates to bp_paths / bp_wrappers / bp_cmd.
    const kimix::string_view s = s_;
    size_t i = start;
    bool command_expected = true;
    bool redirect_expected = false;
    bool redirect_resume = true;
    kimix::string redirect_op;
    bool redirect_op_valid = false;
    kimix::optional<BashWrapper> wrapper;
    kimix::string heredoc_operator;
    bool heredoc_operator_valid = false;
    bool herestring_flag = false;
    kimix::vector<BashHereDoc> pending_heredocs;
    kimix::vector<kimix::string> case_stack;
    bool for_arith_header = false;
    bool for_brace_body = false;
    bool function_name_expected = false;
    bool function_body_expected = false;
    bool assignment_prefix = false;
    int32_t reparse_operands = 0;
    bool reparse_leading = false;
    kimix::optional<kimix::string> command_word;
    kimix::optional<kimix::string> pattern_family;
    kimix::optional<kimix::string> pattern_pending;
    int32_t pattern_bare = 0;
    bool subst_just_closed = false;

    auto heredoc_operator_is_doc = [&]() {
        return heredoc_operator_valid && (heredoc_operator == "<<" || heredoc_operator == "<<-");
    };

    while (i < end) {
        const char ch = s[i];
        const bool suppress_hash_comment = subst_just_closed;
        subst_just_closed = false;

        if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (i + 1 < end && (s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\r')) {
                i = hspace_end(s, i, end);
            } else {
                i += 1;
            }
            continue;
        }
        if (ch == '\\' && i + 1 < end && s[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (ch == '\n') {
            i += 1;
            if (!pending_heredocs.empty()) {
                i = skip_heredoc_bodies(i, end, to_pending(pending_heredocs), true);
                pending_heredocs.clear();
            }
            command_expected = true;
            redirect_expected = false;
            redirect_op_valid = false;
            redirect_op.clear();
            heredoc_operator_valid = false;
            heredoc_operator.clear();
            herestring_flag = false;
            wrapper = std::nullopt;
            assignment_prefix = false;
            reparse_operands = 0;
            command_word = std::nullopt;
            pattern_family = std::nullopt;
            pattern_pending = std::nullopt;
            pattern_bare = 0;
            for_arith_header = false;
            for_brace_body = false;
            continue;
        }
        if (ch == '#' && comment_starts_at(i, start) && !suppress_hash_comment) {
            const size_t newline = bounded_find(s, '\n', i + 1, end);
            i = newline == kimix::string_view::npos ? end : newline;
            continue;
        }

        const bool process_substitution = in_redirection_start(ch) && i + 1 < end && s[i + 1] == '(';
        if (!process_substitution &&
            (in_redirection_start(ch) ||
             (ch == '&' && i + 1 < end && s[i + 1] == '>') ||
             (is_ascii_digit(ch) && redirection_after_fd_at(i, end)))) {
            size_t op_start = i;
            if (is_ascii_digit(ch)) {
                while (i < end && is_ascii_digit(s[i])) {
                    i += 1;
                }
            }
            kimix::string_view op;
            size_t after = i;
            read_redirection_at(i, end, &op, &after);
            i = after;
            if (!op.empty()) {
                redirect_resume = command_expected;
                redirect_expected = true;
                redirect_op = kimix::string(op);
                redirect_op_valid = true;
                herestring_flag = (op == "<<<");
                if (op == "<<" || op == "<<-") {
                    // delimiter captured when the following word is read
                } else {
                    op_start = static_cast<size_t>(-1);
                }
                if (op_start != static_cast<size_t>(-1)) {
                    heredoc_operator = kimix::string(op);
                    heredoc_operator_valid = true;
                }
                continue;
            }
            i = op_start;
        }

        if (redirect_expected) {
            size_t word_end = 0;
            if (in_redirection_start(ch) && i + 1 < end && s[i + 1] == '(') {
                const size_t close = find_matching_impl(i + 2, end);
                scan_range(i + 2, close < end ? close : end);
                word_end = close < end ? close + 1 : end;
                subst_just_closed = true;
            } else {
                const bool scan_substitutions = !heredoc_operator_is_doc();
                word_end = read_word(i, end, scan_substitutions);
            }
            if (word_end <= i) {
                i += 1;
                continue;
            }
            if (heredoc_operator_is_doc()) {
                const auto hd = heredoc_delimiter(s.substr(i, word_end - i));
                if (hd.has_value()) {
                    pending_heredocs.push_back(BashHereDoc{hd->delimiter, heredoc_operator == "<<-", hd->expands});
                }
            } else if (!herestring_flag) {
                const kimix::string_view raw_word = s.substr(i, word_end - i);
                if (redirect_op_valid && !is_nul_forbidden_operator(redirect_op) && is_effective_nul_word(raw_word)) {
                    record_edit(i, word_end, kimix::string(kNulReplacement));
                    record_nul(kimix::string(raw_word));
                } else {
                    const auto replacement = path_replacement_for(raw_word);
                    if (replacement.has_value()) {
                        record_edit(i, word_end, *replacement);
                        record_path(kimix::string(raw_word));
                    }
                }
            }
            i = word_end;
            command_expected = redirect_resume;
            redirect_expected = false;
            redirect_op_valid = false;
            redirect_op.clear();
            heredoc_operator_valid = false;
            heredoc_operator.clear();
            continue;
        }

        if (ch == '[' && i + 1 < end && s[i + 1] == '[') {
            function_body_expected = false;
            i = skip_conditional(i + 2, end);
            command_expected = false;
            for_brace_body = false;
            continue;
        }
        if (ch == '(' && i + 1 < end && s[i + 1] == '(') {
            function_body_expected = false;
            i = skip_arithmetic(i + 2, end);
            command_expected = false;
            for_brace_body = for_arith_header;
            for_arith_header = false;
            continue;
        }
        if (ch == '$' && i + 1 < end && s[i + 1] == '(' && !(i + 2 < end && s[i + 2] == '(')) {
            const size_t close = find_matching_impl(i + 2, end);
            const size_t inner_end = close < end ? close : end;
            scan_range(i + 2, inner_end);
            i = close < end ? close + 1 : end;
            subst_just_closed = true;
            for_brace_body = false;
            if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            }
            command_expected = false;
            continue;
        }
        if (ch == '`') {
            const size_t close = scanner_find_backtick_end(i + 1, end);
            scan_range(i + 1, close);
            i = close < end ? close + 1 : end;
            for_brace_body = false;
            if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            }
            command_expected = false;
            continue;
        }
        if (in_redirection_start(ch) && i + 1 < end && s[i + 1] == '(') {
            const size_t close = find_matching_impl(i + 2, end);
            scan_range(i + 2, close < end ? close : end);
            i = close < end ? close + 1 : end;
            subst_just_closed = true;
            for_brace_body = false;
            if (!case_stack.empty() && case_stack.back() == "word") {
                case_stack.back() = "await-in";
            }
            command_expected = false;
            continue;
        }

        kimix::string_view op;
        size_t op_end = i;
        if (in_control_op_starts(ch)) {
            op_end = read_control_operator(s, i, end, &op);
        } else {
            op = kimix::string_view();
            op_end = i;
        }
        if (!op.empty()) {
            i = op_end;
            for_brace_body = false;
            if (op == "(" && function_body_expected) {
                function_body_expected = false;
                command_expected = true;
            } else if (op == "(") {
                command_expected = true;
            } else if (op == ")") {
                if (!case_stack.empty() && case_stack.back() == "patterns") {
                    case_stack.back() = "body";
                    command_expected = true;
                } else {
                    command_expected = false;
                }
            } else if (op == ";;" || op == ";&" || op == ";;&") {
                if (!case_stack.empty()) {
                    case_stack.back() = "patterns";
                    command_expected = false;
                } else {
                    command_expected = true;
                }
            } else {
                command_expected = true;
            }
            redirect_expected = false;
            heredoc_operator_valid = false;
            heredoc_operator.clear();
            wrapper = std::nullopt;
            assignment_prefix = false;
            reparse_operands = 0;
            command_word = std::nullopt;
            pattern_family = std::nullopt;
            pattern_pending = std::nullopt;
            pattern_bare = 0;
            continue;
        }

        const size_t word_start = i;
        const bool scan_substitutions = !heredoc_operator_is_doc();
        const size_t word_end = read_word(i, end, scan_substitutions);
        if (word_end <= i) {
            i += 1;
            continue;
        }
        const kimix::string_view raw = s.substr(word_start, word_end - word_start);
        i = word_end;
        const bool brace_body_open = for_brace_body && raw == "{";
        for_arith_header = false;
        for_brace_body = false;

        if (function_name_expected) {
            function_name_expected = false;
            function_body_expected = true;
            command_expected = false;
            const auto declaration_end = empty_parentheses_end(i, end);
            if (declaration_end.has_value()) {
                i = *declaration_end;
            }
            continue;
        }

        if (function_body_expected) {
            function_body_expected = false;
            if (raw == "{") {
                command_expected = true;
                continue;
            }
            command_expected = true;
        }

        if (!case_stack.empty() && case_stack.back() == "word") {
            case_stack.back() = "await-in";
            command_expected = false;
            continue;
        }
        if (!case_stack.empty() && case_stack.back() == "await-in" && raw == "in") {
            case_stack.back() = "patterns";
            command_expected = false;
            continue;
        }
        if (!case_stack.empty() && case_stack.back() == "patterns") {
            if (raw == "esac") {
                case_stack.pop_back();
            }
            command_expected = false;
            continue;
        }

        if (!command_expected) {
            if (command_word.has_value() && in_exec_operand_commands(*command_word)) {
                const auto shell_name_val = literal_word_value(raw);
                if (shell_name_val.has_value() && in_shell_wrappers(*shell_name_val)) {
                    const auto script_end = fix_shell_c_script_in_place(i, end);
                    if (script_end.has_value()) {
                        i = *script_end;
                        continue;
                    }
                }
            }
            if (reparse_operands != 0 && !in_command_end_keywords(raw)) {
                if (reparse_operands > 0) {
                    reparse_operands -= 1;
                }
                if (inline_script_operand(word_start, word_end, raw)) {
                    reparse_leading = false;
                    continue;
                }
                if (reparse_leading) {
                    reparse_leading = false;
                    const auto operand = literal_word_value(raw);
                    if (operand.has_value() && !operand->empty()) {
                        if (is_fallback_name(*operand)) {
                            record_name(*operand);
                        } else {
                            record_unsupported(raw, operand, true);
                        }
                    }
                }
            }
            if (raw == "then" || raw == "do" || raw == "else" || raw == "elif") {
                command_expected = true;
            } else if (raw == "esac" && !case_stack.empty()) {
                case_stack.pop_back();
            } else if (brace_body_open) {
                command_expected = true;
            } else if (pattern_family.has_value()) {
                const auto value = literal_word_value(raw);
                const kimix::string_view word = value.has_value() ? kimix::string_view(*value) : raw;
                bool path_ok = false;
                if (pattern_pending.has_value()) {
                    const kimix::string pending = *pattern_pending;
                    pattern_pending = std::nullopt;
                    path_ok = (pending == "path");
                } else if (!word.empty() && word[0] == '-' && word != "-") {
                    const auto option = classify_pattern_option(*pattern_family, word);
                    path_ok = false;
                    if (option.has_value()) {
                        if (*option == "data-pending") {
                            pattern_pending = kimix::string("data");
                        } else if (*option == "path-pending") {
                            pattern_pending = kimix::string("path");
                        } else if (*option == "value-pending") {
                            pattern_pending = kimix::string("value");
                        }
                    }
                } else if (pattern_bare == 0) {
                    pattern_bare += 1;
                    path_ok = false;
                } else {
                    pattern_bare += 1;
                    path_ok = true;
                }
                if (path_ok) {
                    const auto replacement = path_replacement_for(raw);
                    if (replacement.has_value()) {
                        record_edit(word_start, word_end, *replacement);
                        record_path(kimix::string(raw));
                    }
                }
            } else {
                const auto replacement = path_replacement_for(raw);
                if (replacement.has_value()) {
                    record_edit(word_start, word_end, *replacement);
                    record_path(kimix::string(raw));
                }
            }
            if (raw.find('=') != kimix::string_view::npos && is_assignment_word(raw) && i < end && s[i] == '(') {
                const size_t close = find_matching_impl(i + 1, end);
                scan_array_words(i + 1, close < end ? close : end);
                i = close < end ? close + 1 : end;
            }
            continue;
        }

        if (!raw.empty() && raw[0] == '@') {
            const auto at = try_at_prefix(word_start, word_end, end);
            if (at.has_value()) {
                i = at->index;
                if (at->statement) {
                    command_expected = false;
                }
                continue;
            }
        }
        if (raw == "function") {
            function_name_expected = true;
            command_expected = true;
            continue;
        }
        {
            const auto declaration_end = function_declaration_end(raw, i, end);
            if (declaration_end.has_value()) {
                i = *declaration_end;
                function_body_expected = true;
                command_expected = false;
                continue;
            }
        }
        if (raw == "if") {
            const auto consumed = try_cmd_if(word_start, word_end, end);
            if (consumed.has_value()) {
                i = *consumed;
                command_expected = false;
                continue;
            }
        }
        if (raw == "for") {
            const auto consumed = try_cmd_for(word_start, word_end, end);
            if (consumed.has_value()) {
                i = *consumed;
                command_expected = false;
                continue;
            }
        }
        if (in_command_start_keywords(raw)) {
            command_expected = true;
            continue;
        }
        if (in_command_end_keywords(raw)) {
            if (raw == "esac" && !case_stack.empty()) {
                case_stack.pop_back();
            }
            command_expected = false;
            continue;
        }
        if (in_list_keywords(raw)) {
            if (raw == "case") {
                case_stack.push_back("word");
            } else if (raw == "for") {
                for_arith_header = true;
            }
            command_expected = false;
            continue;
        }
        if (raw.find('=') != kimix::string_view::npos && is_assignment_word(raw)) {
            if (i < end && s[i] == '(') {
                const size_t close = find_matching_impl(i + 1, end);
                scan_array_words(i + 1, close < end ? close : end);
                i = close < end ? close + 1 : end;
            }
            command_expected = true;
            assignment_prefix = true;
            continue;
        }

        if (raw == "cd") {
            drop_cmd_cd_flag(i, end);
        }

        const bool executable_wrapper = wrapper.has_value() && !in_same_shell_wrappers(wrapper->kind);
        if (wrapper.has_value() && wrapper->kind == "coproc") {
            if (coproc_name_before_compound(raw, i, end)) {
                wrapper = std::nullopt;
                command_expected = true;
                continue;
            }
        }
        bool inline_consumed = false;
        if (wrapper.has_value() && (wrapper->kind == "env" || wrapper->kind == "sudo" || wrapper->kind == "time" || wrapper->kind == "xargs")) {
            const auto inline_repl = wrapper_path_inline_replacement(*wrapper, raw, *this);
            if (inline_repl.has_value()) {
                if (!inline_repl->empty()) {
                    // The arm fired WITH a path change: emit the rewrite.
                    // wrapper_path_inline_replacement already reset
                    // skip_next/path_value; record the edit + note.
                    record_edit(word_start, word_end, *inline_repl);
                    record_path(kimix::string(raw));
                }
                wrapper->skip_next = false;
                wrapper->path_value = false;
                command_expected = true;
                inline_consumed = true;
            }
        }
        if (inline_consumed) {
            continue;
        }
        if (wrapper.has_value()) {
            const bool path_option_value = wrapper->path_value && wrapper->skip_next;
            const WrapperAction action = wrapper_step(*wrapper, raw);
            if (action == WrapperAction::kSkip) {
                if (path_option_value) {
                    const auto replacement = path_replacement_for(raw);
                    if (replacement.has_value()) {
                        record_edit(word_start, word_end, *replacement);
                        record_path(kimix::string(raw));
                    }
                }
                command_expected = true;
                continue;
            }
            if (action == WrapperAction::kInspect) {
                command_expected = false;
                wrapper = std::nullopt;
                continue;
            }
            if (wrapper.has_value() && wrapper->kind == "watch") {
                // `watch_reparse_operand`: watch re-executes its command in
                // this shell, so a quoted operand is an inline script.
                const bool consumed = inline_script_operand(word_start, word_end, raw);
                wrapper = std::nullopt;
                if (consumed) {
                    command_expected = false;
                    continue;
                }
            }
        }

        // Leading-backslash / quoted wrapper-name normalization.
        kimix::string_view unescaped = raw;
        if (bp_starts_with(raw, "\\") && !in_shell_keywords(raw.substr(1))) {
            unescaped = raw.substr(1);
        }
        kimix::optional<kimix::string> literal_unescaped;
        if (unescaped != raw || (!raw.empty() && (raw[0] == '\'' || raw[0] == '"' || raw[0] == '$'))) {
            const auto decoded = literal_word_value(raw);
            if (decoded.has_value() && !in_shell_keywords(*decoded)) {
                literal_unescaped = decoded;
            }
        }
        kimix::string wrapper_key(unescaped);
        const bool key_is_fallback_wrapper = fallback_command_wrapper_kind(wrapper_key).has_value();
        const bool key_is_command_wrapper = in_command_wrappers(wrapper_key);
        if (!key_is_fallback_wrapper && !key_is_command_wrapper) {
            wrapper_key = literal_unescaped.has_value() ? *literal_unescaped : wrapper_key;
        }
        const auto fallback_wrapper = fallback_command_wrapper_kind(wrapper_key);
        if (fallback_wrapper.has_value()) {
            record_name(wrapper_key);
            if (executable_wrapper) {
                record_edit(word_start, word_end, wrapper_runner(wrapper_key));
            }
            BashWrapper w;
            w.kind = kimix::string(*fallback_wrapper);
            w.operands = wrapper_operand_count(*fallback_wrapper);
            wrapper = w;
            command_expected = true;
            continue;
        }
        if (in_command_wrappers(wrapper_key)) {
            BashWrapper w;
            w.kind = wrapper_key;
            w.operands = wrapper_operand_count(wrapper_key);
            wrapper = w;
            command_expected = true;
            const auto cw = literal_word_value(raw);
            command_word = cw.has_value() && !cw->empty() ? cw : kimix::optional<kimix::string>(kimix::string(raw));
            continue;
        }

        const auto literal = literal_word_value(raw);

        const kimix::optional<kimix::string> shell_name_val =
            (literal.has_value() && in_shell_wrappers(*literal)) ? literal : std::nullopt;
        if (shell_name_val.has_value()) {
            const auto handled = handle_shell_wrapper(*shell_name_val, word_start, i, end, assignment_prefix, wrapper.has_value());
            if (handled.has_value()) {
                i = handled->first;
                const bool keep_wrapper = handled->second;
                command_expected = true;
                redirect_expected = false;
                heredoc_operator_valid = false;
                heredoc_operator.clear();
                if (!keep_wrapper) {
                    wrapper = std::nullopt;
                }
                assignment_prefix = false;
                continue;
            }
            const auto script_end = fix_shell_c_script_in_place(i, end);
            if (script_end.has_value()) {
                i = *script_end;
                command_expected = false;
                wrapper = std::nullopt;
                reparse_operands = 0;
                continue;
            }
        }

        command_word = literal.has_value() ? literal : kimix::optional<kimix::string>(kimix::string(raw));
        if (command_word.has_value() && in_pattern_program_commands(*command_word)) {
            pattern_family = *command_word;
        } else {
            pattern_family = std::nullopt;
        }
        pattern_pending = std::nullopt;
        pattern_bare = 0;

        const auto reparse_count = literal.has_value() ? reparse_builtin_count(*literal) : std::nullopt;
        if (reparse_count.has_value()) {
            reparse_operands = *reparse_count;
            reparse_leading = true;
            command_expected = false;
            wrapper = std::nullopt;
            assignment_prefix = false;
            continue;
        }

        if (literal.has_value() && is_fallback_name(*literal)) {
            record_name(*literal);
            if (executable_wrapper) {
                record_edit(word_start, word_end, wrapper_runner(*literal));
            }
        } else if (record_cmd_jump(raw, literal, literal.has_value(), word_end, end)) {
            // refused jump: nothing more
        } else if (record_unsupported(raw, literal, true)) {
            // unsupported: nothing more
        } else {
            const auto replacement = path_replacement_for(raw);
            if (replacement.has_value()) {
                record_edit(word_start, word_end, *replacement);
                record_path(kimix::string(raw));
            }
        }
        command_expected = false;
        wrapper = std::nullopt;
    }
}

void Scanner::scan_all() {
    // `_scan_range(0, n)` on a dedicated large-stack thread (see the depth
    // model in the file header).  Nested scanners (an inner `-c`/eval/trap
    // script) are constructed ON that thread, so a thread-local flag makes
    // them scan inline instead of spawning a thread per nesting level; the
    // budget counter already bounds recursion depth.
    static thread_local bool t_on_scan_stack = false;
    if (t_on_scan_stack) {
        scan_range(0, n_);
        return;
    }
    t_on_scan_stack = true;
    run_on_big_stack([this]() { scan_range(0, n_); });
    t_on_scan_stack = false;
}

kimix::string Scanner::build_source() const {
    return render_splices(s_, reports_.edits);
}

kimix::string Scanner::build_fixed_source(const kimix::string &source, bool reuse_events) const {
    // `_build_fixed_source`: heredoc trailing-operator repair LAST.
    if (reuse_events) {
        return fix_heredoc_trailing_operators(source, reports_.heredoc_events);
    }
    return fix_heredoc_trailing_operators(source, std::nullopt);
}

BashFix Scanner::fix() {
    // Faithful two-stage port: `_BashFixScanner.fix()` produces `result`,
    // then `translator.translate` applies its own final heredoc repair on
    // top of `result.command` (events fast path when the command is
    // unchanged, else a rescan).  Both stages are reproduced so the output
    // is byte-identical to the reference.
    scan_all();

    const bool no_activity = reports_.names.empty() && reports_.edits.empty() &&
                             reports_.shell_notes.empty() && reports_.nul_fixes.empty() &&
                             reports_.unsupported.empty() && reports_.cmd_conditional_notes.empty();

    // ── stage 1: _BashFixScanner.fix() ──
    kimix::string result_command;
    if (no_activity) {
        result_command = kimix::string(s_); // BashFix(self.s), no repair
    } else {
        const kimix::string prefix = fallback_prefix(reports_.names);
        const kimix::string source = build_source();
        // _build_fixed_source: reuse events iff source is the original bytes.
        const bool reuse_events = reports_.edits.empty();
        kimix::string fixed = build_fixed_source(source, reuse_events);
        fixed.insert(fixed.begin(), prefix.begin(), prefix.end());
        result_command = std::move(fixed);
    }

    // ── stage 2: translator.translate's final heredoc repair ──
    kimix::string final_command;
    if (result_command == s_) {
        final_command = fix_heredoc_trailing_operators(s_, reports_.heredoc_events);
    } else {
        final_command = fix_heredoc_trailing_operators(result_command, std::nullopt);
    }

    if (no_activity) {
        return BashFix(final_command);
    }
    return BashFix(final_command, reports_.names, reports_.path_notes, reports_.shell_notes,
                   reports_.nul_fixes, reports_.unsupported, reports_.cmd_conditional_notes);
}

} // namespace kimix::llm::bash_parser
