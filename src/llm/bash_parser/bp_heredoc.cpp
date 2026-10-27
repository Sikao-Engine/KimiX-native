/* bp_heredoc.cpp — heredoc trailing-operator repair.
 * Line-for-line port of kimi-agent `shell_compat/heredoc.py` LIVE path:
 * `_HEREDOC_TRAILING_OPERATORS`, `_apply_heredoc_operator_move`,
 * `_fix_heredoc_trailing_operators`, and
 * `_BashFixScanner._heredoc_line_continues` (bash_core.py).  The IR-side
 * event model (heredoc_event_for / collect_heredoc_events /
 * fix_heredoc_trailing_operators_ir) is part of the IR-layer port and is
 * deliberately absent here.
 *
 * Python-semantics simplifications (documented per task contract):
 * - `str.splitlines()` splits on \n, \r\n, lone \r (plus the Unicode breaks
 *   \v \f \x1c-\x1e \x85 \u2028 \u2029) and drops a trailing empty element
 *   for a final newline.  Over ASCII shell sources only \n / \r\n / lone \r
 *   can appear as breaks (\v and \f do not continue a bash logical line, and
 *   the Unicode breaks are multi-byte UTF-8 here), so `py_splitlines` below
 *   splits on those three and keeps the "no trailing empty element" rule.
 * - `str.strip()` with no argument strips the Python whitespace set
 * " \t\n\r\f\v"; the moved-line parts are already newline-free, so
 * `py_strip_ws` strips that set (and `trailing_backslashes` mirrors
 * `len(s) - len(s.rstrip("\\"))`).
 * - `str.find` / `str.rfind` / `str.count` are ported exactly with their
 *   Python slice bounds: `find(sub, start, end)` scans [start, end),
 *   `rfind(sub, start, end)` likewise, and the reference's
 *   `rfind("\n", move_start, move_end - 1)` therefore searches within
 *   [move_start, move_end-1) (the byte before the line's own newline).
 * - f-strings are plain concatenation; the final assembly is byte-exact.
 */
#include "llm/bash_parser/bp_heredoc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "llm/bash_parser/bp_core.h"
#include "llm/bash_parser/bp_scanner.h"

namespace kimix::llm::bash_parser {
namespace src_llm_bash_parser_bp_heredoc {


// `str.find(ch, start, end)` over [start, end); -1 when absent.
int64_t py_find(kimix::string_view s, char ch, int64_t start, int64_t end) {
    const int64_t n = static_cast<int64_t>(s.size());
    if (start < 0) {
        start = 0;
    }
    if (end > n) {
        end = n;
    }
    for (int64_t i = start; i < end; ++i) {
        if (s[static_cast<size_t>(i)] == ch) {
            return i;
        }
    }
    return -1;
}

// `str.rfind(ch, start, end)` over [start, end); -1 when absent.
int64_t py_rfind(kimix::string_view s, char ch, int64_t start, int64_t end) {
    const int64_t n = static_cast<int64_t>(s.size());
    if (end > n) {
        end = n;
    }
    if (start < 0) {
        start = 0;
    }
    for (int64_t i = end - 1; i >= start; --i) {
        if (s[static_cast<size_t>(i)] == ch) {
            return i;
        }
    }
    return -1;
}

// `str.rfind(sub)` / `str.rfind(ch)` with no bounds.
int64_t py_rfind_all(kimix::string_view s, char ch) {
    return py_rfind(s, ch, 0, static_cast<int64_t>(s.size()));
}

// `str.count(ch)`.
int64_t py_count(kimix::string_view s, char ch) {
    int64_t total = 0;
    for (const char c : s) {
        if (c == ch) {
            ++total;
        }
    }
    return total;
}

// `s[begin, end)` with Python-style clamping (end may exceed the size).
kimix::string_view py_slice(kimix::string_view s, int64_t begin, int64_t end) {
    const int64_t n = static_cast<int64_t>(s.size());
    if (begin < 0) {
        begin = 0;
    }
    if (begin > n) {
        begin = n;
    }
    if (end > n) {
        end = n;
    }
    if (end < begin) {
        end = begin;
    }
    return s.substr(static_cast<size_t>(begin), static_cast<size_t>(end - begin));
}

// `s.substr(pos)` with a Python-safe start clamp.
kimix::string_view py_slice_to_end(kimix::string_view s, int64_t begin) {
    return py_slice(s, begin, static_cast<int64_t>(s.size()));
}

kimix::string_view py_lstrip(kimix::string_view s, const char *set) {
    int64_t i = 0;
    const int64_t n = static_cast<int64_t>(s.size());
    while (i < n && strchr(set, s[static_cast<size_t>(i)]) != nullptr) {
        ++i;
    }
    return py_slice_to_end(s, i);
}

kimix::string_view py_rstrip(kimix::string_view s, const char *set) {
    int64_t n = static_cast<int64_t>(s.size());
    while (n > 0 && strchr(set, s[static_cast<size_t>(n - 1)]) != nullptr) {
        --n;
    }
    return py_slice(s, 0, n);
}

kimix::string_view py_strip(kimix::string_view s, const char *set) {
    return py_rstrip(py_lstrip(s, set), set);
}

// `str.strip()` with no argument: Python whitespace " \t\n\r\f\v".
kimix::string_view py_strip_ws(kimix::string_view s) {
    return py_strip(s, " \t\n\r\f\v");
}

// `len(s) - len(s.rstrip("\\"))`: the trailing-backslash count.
int64_t trailing_backslashes(kimix::string_view s) {
    const int64_t n = static_cast<int64_t>(s.size());
    int64_t k = n;
    while (k > 0 && s[static_cast<size_t>(k - 1)] == '\\') {
        --k;
    }
    return n - k;
}

// `str.split("\n")`: never empty; a trailing "\n" yields a final "" element.
kimix::vector<kimix::string_view> py_split_newline(kimix::string_view s) {
    kimix::vector<kimix::string_view> out;
    int64_t start = 0;
    const int64_t n = static_cast<int64_t>(s.size());
    for (;;) {
        const int64_t nl = py_find(s, '\n', start, n);
        if (nl < 0) {
            out.push_back(py_slice(s, start, n));
            break;
        }
        out.push_back(py_slice(s, start, nl));
        start = nl + 1;
    }
    return out;
}

// `str.splitlines()` over \n / \r\n / lone \r, with the "no trailing empty
// element for a final newline" semantics (see file header).
kimix::vector<kimix::string_view> py_splitlines(kimix::string_view s) {
    kimix::vector<kimix::string_view> out;
    const int64_t n = static_cast<int64_t>(s.size());
    int64_t start = 0;
    int64_t i = 0;
    while (i < n) {
        const char ch = s[static_cast<size_t>(i)];
        if (ch == '\n' || ch == '\r') {
            out.push_back(py_slice(s, start, i));
            int64_t next = i + 1;
            if (ch == '\r' && next < n && s[static_cast<size_t>(next)] == '\n') {
                ++next; // consume \r\n as ONE break
            }
            i = next;
            start = i;
            continue;
        }
        ++i;
    }
    // A string ending exactly at a break produces no trailing element;
    // otherwise the tail (start < n) is the last line.
    if (start < n) {
        out.push_back(py_slice(s, start, n));
    }
    return out;
}

} // namespace src_llm_bash_parser_bp_heredoc
using namespace src_llm_bash_parser_bp_heredoc;

bool is_heredoc_trailing_operator(kimix::string_view op) {
    // `_HEREDOC_TRAILING_OPERATORS = frozenset({"&&", "||", "|", "|&", ";", "&"})`
    return op == "&&" || op == "||" || op == "|" || op == "|&" || op == ";" || op == "&";
}

bool heredoc_line_continues(kimix::string_view line) {
    // `_BashFixScanner._heredoc_line_continues`:
    // `trailing = len(line) - len(line.rstrip("\\")); return trailing % 2 == 1`
    return trailing_backslashes(line) % 2 == 1;
}

kimix::string apply_heredoc_operator_move(kimix::string_view source, int64_t redir_line_end, int64_t terminator_end) {
    const int64_t n = static_cast<int64_t>(source.size());
    if (redir_line_end < 0 || redir_line_end >= n || source[static_cast<size_t>(redir_line_end)] != '\n') {
        return kimix::string(source);
    }
    if (terminator_end < 0 || terminator_end > n) {
        return kimix::string(source);
    }

    // Skip whitespace, newlines and comment lines to find the operator start.
    int64_t i = terminator_end;
    while (i < n) {
        const char ch = source[static_cast<size_t>(i)];
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            ++i;
            continue;
        }
        if (ch == '\n') {
            ++i;
            continue;
        }
        if (ch == '#') {
            const int64_t nl = py_find(source, '\n', i, n);
            i = nl < 0 ? n : nl + 1;
            continue;
        }
        break;
    }
    if (i >= n) {
        return kimix::string(source);
    }

    kimix::string_view op{};
    const int64_t op_end = static_cast<int64_t>(read_shell_control_operator(source, static_cast<size_t>(i), static_cast<size_t>(n), &op));
    if (!is_heredoc_trailing_operator(op)) {
        return kimix::string(source);
    }
    // ``&>`` / ``&>>`` are redirections, not list terminators.
    if (op == "&" && op_end < n && source[static_cast<size_t>(op_end)] == '>') {
        return kimix::string(source);
    }

    const int64_t move_start = i;
    int64_t line_end = py_find(source, '\n', i, n);
    int64_t move_end = 0;
    if (line_end < 0) {
        line_end = n;
        move_end = n;
    } else {
        move_end = line_end + 1;
    }

    const kimix::string_view rest = py_lstrip(py_slice(source, op_end, line_end), " \t\r");
    if (rest.empty() || (!rest.empty() && rest.front() == '#')) {
        // The operator line is empty or comment-only past the operator: look
        // for the next non-blank, non-comment line and extend the move region
        // over it.
        int64_t k = move_end;
        while (k < n) {
            const char ch = source[static_cast<size_t>(k)];
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                ++k;
                continue;
            }
            if (ch == '\n') {
                ++k;
                continue;
            }
            if (ch == '#') {
                const int64_t nl = py_find(source, '\n', k, n);
                k = nl < 0 ? n : nl + 1;
                continue;
            }
            break;
        }
        if (k >= n) {
            return kimix::string(source);
        }
        const int64_t next_line_end = py_find(source, '\n', k, n);
        move_end = next_line_end < 0 ? n : next_line_end + 1;
    }

    // A trailing backslash continues the logical command on the following
    // physical lines; the continuation must move with its line, or the
    // orphaned tail is left behind and read as heredoc body.
    while (move_end < n) {
        const int64_t prev_nl = py_rfind(source, '\n', move_start, move_end - 1);
        const kimix::string_view last_line = py_slice(source, prev_nl + 1, move_end - 1);
        if (!heredoc_line_continues(last_line)) {
            break;
        }
        const int64_t next_end = py_find(source, '\n', move_end, n);
        move_end = next_end < 0 ? n : next_end + 1;
    }

    // A list may continue over SEVERAL physical operator lines (the model can
    // write ``&& echo A`` on one line and ``&& echo B`` on the next, both
    // after the closing delimiter).  The backslash loop above only carries a
    // single logical line; absorb each following operator-led line as well,
    // or it is left orphaned after the terminator and bash reports
    // ``syntax error near unexpected token `&&'``.
    while (move_end < n) {
        int64_t j = move_end;
        while (j < n) {
            const char cj = source[static_cast<size_t>(j)];
            if (cj == ' ' || cj == '\t' || cj == '\r') {
                ++j;
                continue;
            }
            if (cj == '\n') {
                ++j;
                continue;
            }
            if (cj == '#') {
                const int64_t nl2 = py_find(source, '\n', j, n);
                j = nl2 < 0 ? n : nl2 + 1;
                continue;
            }
            break;
        }
        if (j >= n) {
            break;
        }
        kimix::string_view op2{};
        const int64_t op2_end = static_cast<int64_t>(read_shell_control_operator(source, static_cast<size_t>(j), static_cast<size_t>(n), &op2));
        if (!is_heredoc_trailing_operator(op2)) {
            break;
        }
        if (op2 == "&" && op2_end < n && source[static_cast<size_t>(op2_end)] == '>') {
            break;
        }
        const int64_t cand_nl = py_find(source, '\n', j, n);
        move_end = cand_nl < 0 ? n : cand_nl + 1;
        // carry this operator line's own backslash continuations too
        while (move_end < n) {
            const int64_t pl = py_rfind(source, '\n', j, move_end - 1);
            const kimix::string_view ll = py_slice(source, pl + 1, move_end - 1);
            if (!heredoc_line_continues(ll)) {
                break;
            }
            const int64_t ne = py_find(source, '\n', move_end, n);
            move_end = ne < 0 ? n : ne + 1;
        }
    }

    kimix::vector<kimix::string_view> lines = py_splitlines(py_slice(source, move_start, move_end));

    // Closing delimiters that belong to a construct opened BEFORE the
    // redirection line (a command substitution ``$(`` or backtick, a subshell
    // ``(``, a brace group ``{``) must NOT ride up onto the redirection line:
    // doing so closes the construct before the heredoc body, ejects the body
    // lines into the outer command list, and bash reports an unterminated
    // here-document.  The excess closers — and everything after them — stay
    // in place after the terminator (e.g. ``x=$(cat <<E | wc -l`` + body +
    // ``E`` + ``); echo q``).  The stay suffix may sit mid-line (``| wc -l);
    // echo q``) or after balanced groups inside the moved text (``| (a); b)``;
    // only the closers exceeding the moved text's own openers belong outside).
    // Guards are naive character counts; the prefix guard (an unclosed opener
    // before the redirection line) keeps ordinary operator lines such as
    // ``| (wc -l)`` moving whole.
    // `moved_text` outlives the stay block: when the stay guards fire, `lines`
    // is re-sliced from it (the Python rebinding of `lines`) and the later
    // parts/joined assembly reads those views.
    kimix::string moved_text{};
    kimix::string stay{};
    if (!lines.empty()) {
        for (size_t li = 0; li < lines.size(); ++li) {
            if (li != 0) {
                moved_text.push_back('\n');
            }
            moved_text.append(lines[li].data(), lines[li].size());
        }
        const kimix::string_view prefix = py_slice(source, 0, redir_line_end);
        const kimix::string_view moved_sv = moved_text;
        int64_t stay_start = -1;
        const int64_t paren_excess = py_count(moved_sv, ')') - py_count(moved_sv, '(');
        if (paren_excess > 0 && py_count(prefix, '(') > py_count(prefix, ')')) {
            // The suffix from the last excess ``)`` onward stays: it is the
            // construct closer (any ``)`` beyond the moved text's own openers
            // must close a prefix opener).
            const int64_t target = py_count(moved_sv, '(') + paren_excess;
            int64_t idx = -1;
            for (int64_t k = 0; k < target; ++k) {
                idx = py_find(moved_sv, ')', idx + 1, static_cast<int64_t>(moved_sv.size()));
                if (idx < 0) {
                    idx = -1; // Python find returns -1; keep scanning as the reference does
                }
            }
            if (idx >= 0 && (stay_start < 0 || idx < stay_start)) {
                stay_start = idx;
            }
        }
        if (py_count(moved_sv, '`') % 2 == 1 && py_count(prefix, '`') % 2 == 1) {
            const int64_t idx = py_rfind_all(moved_sv, '`');
            if (idx >= 0 && (stay_start < 0 || idx < stay_start)) {
                stay_start = idx;
            }
        }
        const int64_t brace_excess = py_count(moved_sv, '}') - py_count(moved_sv, '{');
        if (brace_excess > 0 && py_count(prefix, '{') > py_count(prefix, '}')) {
            const int64_t target = py_count(moved_sv, '{') + brace_excess;
            int64_t idx = -1;
            for (int64_t k = 0; k < target; ++k) {
                idx = py_find(moved_sv, '}', idx + 1, static_cast<int64_t>(moved_sv.size()));
                if (idx < 0) {
                    idx = -1;
                }
            }
            if (idx >= 0 && (stay_start < 0 || idx < stay_start)) {
                stay_start = idx;
            }
        }
        if (stay_start >= 0) {
            stay = kimix::string(moved_sv.substr(static_cast<size_t>(stay_start)));
            kimix::vector<kimix::string_view> split = py_splitlines(moved_sv.substr(0, static_cast<size_t>(stay_start)));
            // Python: `moved_text[:stay_start].splitlines() or [""]`
            if (split.empty()) {
                split.push_back(kimix::string_view{});
            }
            lines = std::move(split);
            // If the first stay line is only construct closers and the next
            // stay line starts with a control operator, joining them with a
            // space is required: an operator-led physical line after a
            // completed command is a bash syntax error (``cmd \n && next``).
            const kimix::vector<kimix::string_view> stay_lines = py_split_newline(stay);
            if (stay_lines.size() > 1 && py_strip(py_strip(stay_lines[0], " \t\r"), ")`}").empty() && !py_lstrip(stay_lines[1], " \t\r").empty()) {
                const kimix::string_view nxt = stay_lines[1];
                const int64_t lead = static_cast<int64_t>(nxt.size()) - static_cast<int64_t>(py_lstrip(nxt, " \t\r").size());
                kimix::string_view op2{};
                static_cast<void>(read_shell_control_operator(nxt, static_cast<size_t>(lead), static_cast<size_t>(nxt.size()), &op2));
                const bool amp_redir = op2 == "&" && bp_starts_with(py_slice_to_end(nxt, lead + static_cast<int64_t>(op2.size())), ">");
                if (is_heredoc_trailing_operator(op2) && !amp_redir) {
                    kimix::string joined_stay{};
                    joined_stay.append(py_rstrip(stay_lines[0], " \t\r").data(), py_rstrip(stay_lines[0], " \t\r").size());
                    for (size_t li = 1; li < stay_lines.size(); ++li) {
                        const kimix::string_view stripped = py_strip(stay_lines[li], " \t\r");
                        if (stripped.empty()) {
                            continue;
                        }
                        joined_stay.push_back(' ');
                        joined_stay.append(stripped.data(), stripped.size());
                    }
                    stay = std::move(joined_stay);
                }
            }
        }
    }

    kimix::vector<kimix::string_view> parts{};
    if (!lines.empty()) {
        // Python: `lines[0][len(op):].strip()` — the operator text rides along
        // on the first moved line; drop it, then strip whitespace.
        const kimix::string_view first = lines[0];
        const kimix::string_view after_op = first.size() >= op.size() ? first.substr(static_cast<size_t>(op.size())) : kimix::string_view{};
        parts.push_back(py_strip_ws(after_op));
        for (size_t li = 1; li < lines.size(); ++li) {
            parts.push_back(py_strip_ws(lines[li]));
        }
    }
    // Re-join stripped parts, but keep a backslash line-continuation as a real
    // newline: splicing the next line on with a space would turn ``\`` + NL
    // into ``\ `` (an escaped space), corrupting the continued command.
    kimix::string joined{};
    for (const kimix::string_view part : parts) {
        if (part.empty() || (!part.empty() && part.front() == '#')) {
            continue;
        }
        if (!joined.empty()) {
            // `trailing % 2 == 1` depends only on the bytes from the last real
            // newline onward, and every appended segment starts with ' ' or
            // '\n', so the count over the whole `segments[-1]` string equals
            // the count over the last physical line — identical result.
            const int64_t trailing = trailing_backslashes(joined);
            joined.push_back(trailing % 2 == 1 ? '\n' : ' ');
        }
        joined.append(part.data(), part.size());
    }

    kimix::string result{};
    result.reserve(static_cast<size_t>(n));
    // moved = f"{op} {joined}\n" if joined else f"{op}\n"
    const bool joined_nonempty = !joined.empty();
    result.append(py_slice(source, 0, redir_line_end));
    result.push_back(' ');
    result.append(op);
    if (joined_nonempty) {
        result.push_back(' ');
        result.append(joined);
    }
    result.push_back('\n');
    result.append(py_slice(source, redir_line_end + 1, move_start));
    if (!stay.empty()) {
        result.append(stay);
        result.push_back('\n');
    }
    result.append(py_slice_to_end(source, move_end));
    return result;
}

kimix::string fix_heredoc_trailing_operators(kimix::string_view source, const kimix::optional<kimix::vector<std::pair<int64_t, int64_t>>> &events) {
    kimix::vector<std::pair<int64_t, int64_t>> local{};
    const kimix::vector<std::pair<int64_t, int64_t>> *use_events = nullptr;
    if (!events.has_value()) {
        // Legacy fallback: rescan the source with a fresh Scanner (the Python
        // reference wraps the scan in try/except RecursionError and returns
        // the source unchanged; the native Scanner runs the scan on a
        // dedicated large-stack thread and never overflows, so the scan
        // always succeeds).
        Scanner scanner(source);
        scanner.scan_all();
        local = scanner.reports().heredoc_events;
        use_events = &local;
    } else {
        use_events = &events.value();
    }
    if (use_events->empty()) {
        return kimix::string(source);
    }
    // Apply the moves in REVERSE order against the progressively modified
    // source, exactly like the reference (later positions stay valid because
    // earlier events are untouched by moves applied to the tail).
    kimix::string current(source);
    for (size_t ri = use_events->size(); ri != 0; --ri) {
        const std::pair<int64_t, int64_t> &event = (*use_events)[ri - 1];
        current = apply_heredoc_operator_move(current, event.first, event.second);
    }
    return current;
}

} // namespace kimix::llm::bash_parser
