/* bp_lexer.cpp — implementation of the lossless shell lexer (port of
 * kimi-agent `shell_compat/lexer.py`).  See bp_lexer.h for the contract.
 *
 * Every region boundary is taken from a bp_core oracle (find_* / match_* /
 * read_* / redirection_after_fd / comment_starts); none is re-implemented here.
 * `_scan_double_quote` reproduces the legacy `find_dq_end` decision sequence
 * (so its token end equals `find_dq_end(source, start + 1)`) extended only by
 * recording the `$( )` / backtick sub-spans it passes through as sibling parts.
 *
 * No exceptions / RTTI.  Byte offsets throughout.
 */
#include "llm/bash_parser/bp_lexer.h"

#include <core/kimix_core.h>

#include <utility>

#include "llm/bash_parser/bp_core.h"

namespace kimix::llm::bash_parser {
namespace src_llm_bash_parser_bp_lexer {

// ── lexical character classes (mirrors of the reference regexes) ────────────

// _WORD_SPECIAL_RE = [;&|()<>\n \t\r#\\'"`$]
inline bool is_word_special(char ch) {
    switch (ch) {
        case ';': case '&': case '|': case '(': case ')': case '<': case '>':
        case '\n': case ' ': case '\t': case '\r': case '#': case '\\':
        case '\'': case '"': case '`': case '$':
            return true;
        default:
            return false;
    }
}

// _DQ_SPECIAL_RE = [\\"`$]
inline bool is_dq_special(char ch) {
    return ch == '\\' || ch == '"' || ch == '`' || ch == '$';
}

// _HSPACE_RE = [ \t\r]
inline bool is_hspace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

// Return the first index in [i, end) whose char satisfies pred, or SIZE_MAX.
// Mirrors `regex.search(source, i, end)` (bounded by `end`).
template <typename Pred>
size_t search_special(kimix::string_view source, size_t i, size_t end, Pred pred) {
    for (size_t j = i; j < end; ++j) {
        if (pred(source[j])) {
            return j;
        }
    }
    return SIZE_MAX;
}

size_t find_char(kimix::string_view source, char ch, size_t from, size_t end) {
    for (size_t j = from; j < end; ++j) {
        if (source[j] == ch) {
            return j;
        }
    }
    return SIZE_MAX;
}

// `int(source[fd_start:j])` over an ASCII digit run (non-throwing).
int32_t parse_fd(kimix::string_view digits) {
    int32_t value = 0;
    for (const char c : digits) {
        value = value * 10 + static_cast<int32_t>(c - '0');
    }
    return value;
}

// ── _scan_double_quote ──────────────────────────────────────────────────────

// Close the `"` region opened at `start`, collecting `$( )` / backtick
// sub-spans into `nested` and any diagnostics into `diagnostics`.  Returns the
// index after the closing `"` (== find_dq_end(source, start + 1) on every
// terminable input), or `end` when the region never closes within the slice.
size_t scan_double_quote(kimix::string_view source, size_t start, size_t end,
                         kimix::vector<Diagnostic> &diagnostics,
                         kimix::vector<WordPart> &nested) {
    size_t i = start + 1;
    while (i < end) {
        size_t pos = search_special(source, i, end, is_dq_special);
        if (pos == SIZE_MAX) {
            break;
        }
        i = pos;
        const char ch = source[i];
        if (ch == '\\' && i + 1 < end && in_dq_escaped(source[i + 1])) {
            i += 2;
            continue;
        }
        if (ch == '"') {
            return i + 1;
        }
        if (ch == '$' && i + 1 < end && source[i + 1] == '(') {
            const int64_t close = find_matching_paren(source, static_cast<int64_t>(i + 1));
            if (close == -1 || static_cast<size_t>(close) >= end) {
                // The oracle returns -1 here: the quote never closes within the
                // slice.  Record the open region and stop at `end`, emitting the
                // two diagnostics in the reference order (SUBST then QUOTE).
                nested.push_back(WordPart{WordPartKind::kCommandSubstitution, SourceSpan{i, end}, WordPartInner{}});
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated command substitution in double quotes"});
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{start, end}, "unterminated double quote"});
                return end;
            }
            nested.push_back(WordPart{WordPartKind::kCommandSubstitution, SourceSpan{i, static_cast<size_t>(close) + 1}, WordPartInner{std::make_pair(i + 2, static_cast<size_t>(close))}});
            i = static_cast<size_t>(close) + 1;
            continue;
        }
        if (ch == '$' && i + 1 < end && source[i + 1] == '\'') {
            i += 1; // literal '$' inside "..." (legacy documented behaviour)
            continue;
        }
        if (ch == '`') {
            const int64_t close = find_backtick_end(source, static_cast<int64_t>(i + 1));
            if (close == -1 || static_cast<size_t>(close) > end) {
                nested.push_back(WordPart{WordPartKind::kBackquoteSubstitution, SourceSpan{i, end}, WordPartInner{}});
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated backquote in double quotes"});
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{start, end}, "unterminated double quote"});
                return end;
            }
            nested.push_back(WordPart{WordPartKind::kBackquoteSubstitution, SourceSpan{i, static_cast<size_t>(close)}, WordPartInner{std::make_pair(i + 1, static_cast<size_t>(close) - 1)}});
            i = static_cast<size_t>(close);
            continue;
        }
        i += 1;
    }
    diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{start, end}, "unterminated double quote"});
    return end;
}

// ── _scan_word ──────────────────────────────────────────────────────────────

// Scan a WORD at `start`; returns the word end and appends its parts (in
// source order) to `parts`.  Parts tile [start, word_end) with no gaps.  A
// mid-word `#` is ordinary content; a `#` at the word start never reaches here
// (the caller emits a COMMENT token).
size_t scan_word(kimix::string_view source, size_t start, size_t end,
                 kimix::vector<Diagnostic> &diagnostics,
                 kimix::vector<WordPart> &parts) {
    size_t literal_start = start;
    size_t i = start;
    while (i < end) {
        size_t pos = search_special(source, i, end, is_word_special);
        const bool m_none = (pos == SIZE_MAX);
        if (i > literal_start && (m_none || pos > literal_start)) {
            const size_t flush_to = m_none ? end : pos;
            if (flush_to > literal_start) {
                parts.push_back(WordPart{WordPartKind::kLiteral, SourceSpan{literal_start, flush_to}, WordPartInner{}});
                literal_start = flush_to;
            }
        }
        if (m_none) {
            i = end;
            break;
        }
        i = pos;
        const char ch = source[i];
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
            if (i + 1 < end && source[i + 1] == '\n') {
                // line continuation: flush the run, own the pair, keep going
                if (i > literal_start) {
                    parts.push_back(WordPart{WordPartKind::kLiteral, SourceSpan{literal_start, i}, WordPartInner{}});
                }
                parts.push_back(WordPart{WordPartKind::kLineContinuation, SourceSpan{i, i + 2}, WordPartInner{}});
                i += 2;
                literal_start = i;
                continue;
            }
            const size_t stop = (i + 1 < end) ? i + 2 : i + 1;
            if (i > literal_start) {
                parts.push_back(WordPart{WordPartKind::kLiteral, SourceSpan{literal_start, i}, WordPartInner{}});
            }
            parts.push_back(WordPart{WordPartKind::kEscapedCharacter, SourceSpan{i, stop}, WordPartInner{}});
            i = stop;
            literal_start = i;
            continue;
        }
        if (ch == '\'') {
            const size_t close = find_char(source, '\'', i + 1, end);
            if (close == SIZE_MAX) {
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{i, end}, "unterminated single quote"});
                parts.push_back(WordPart{WordPartKind::kSingleQuote, SourceSpan{i, end}, WordPartInner{}});
                i = end;
                literal_start = i;
                continue;
            }
            parts.push_back(WordPart{WordPartKind::kSingleQuote, SourceSpan{i, close + 1}, WordPartInner{std::make_pair(i + 1, close)}});
            i = close + 1;
            literal_start = i;
            continue;
        }
        if (ch == '"') {
            kimix::vector<WordPart> nested;
            const size_t stop = scan_double_quote(source, i, end, diagnostics, nested);
            // Reproduce the FINAL value of the reference's (redundant) inner
            // computation: (i+1, stop-1) when stop <= end && stop > i &&
            // source[stop-1] == '"', else nullopt.
            WordPartInner inner;
            if (stop <= end && stop > i && source[stop - 1] == '"') {
                inner = WordPartInner{std::make_pair(i + 1, stop - 1)};
            }
            parts.push_back(WordPart{WordPartKind::kDoubleQuote, SourceSpan{i, stop}, inner});
            parts.insert(parts.end(), nested.begin(), nested.end());
            i = stop;
            literal_start = i;
            continue;
        }
        if (ch == '$' && i + 1 < end) {
            const char nxt = source[i + 1];
            if (nxt == '\'') {
                const int64_t stop = find_ansi_c_end(source, static_cast<int64_t>(i + 2));
                if (stop == -1 || static_cast<size_t>(stop) > end) {
                    diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{i, end}, "unterminated ANSI-C quote"});
                    parts.push_back(WordPart{WordPartKind::kAnsiCQuote, SourceSpan{i, end}, WordPartInner{}});
                    i = end;
                    literal_start = i;
                    continue;
                }
                parts.push_back(WordPart{WordPartKind::kAnsiCQuote, SourceSpan{i, static_cast<size_t>(stop)}, WordPartInner{std::make_pair(i + 2, static_cast<size_t>(stop) - 1)}});
                i = static_cast<size_t>(stop);
                literal_start = i;
                continue;
            }
            if (nxt == '"') {
                // locale quote $"...": body is data; boundary = closing " with
                // every \X pair skipped (the legacy _literal_word_value walk).
                size_t j = i + 2;
                while (j < end) {
                    if (source[j] == '\\' && j + 1 < end) {
                        j += 2;
                        continue;
                    }
                    if (source[j] == '"') {
                        break;
                    }
                    j += 1;
                }
                if (j >= end) {
                    diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedQuote, SourceSpan{i, end}, "unterminated locale quote"});
                    parts.push_back(WordPart{WordPartKind::kLocaleQuote, SourceSpan{i, end}, WordPartInner{}});
                    i = end;
                    literal_start = i;
                    continue;
                }
                parts.push_back(WordPart{WordPartKind::kLocaleQuote, SourceSpan{i, j + 1}, WordPartInner{std::make_pair(i + 2, j)}});
                i = j + 1;
                literal_start = i;
                continue;
            }
            if (nxt == '(') {
                if (i + 2 < source.size() && source[i + 2] == '(') {
                    const kimix::optional<size_t> stop = match_arithmetic_span(source, i + 3);
                    if (!stop.has_value()) {
                        diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated arithmetic expansion"});
                        parts.push_back(WordPart{WordPartKind::kArithmeticExpansion, SourceSpan{i, end}, WordPartInner{}});
                        i = end;
                        literal_start = i;
                        continue;
                    }
                    const size_t end_after = stop.value() + 2; // consume the '))'
                    parts.push_back(WordPart{WordPartKind::kArithmeticExpansion, SourceSpan{i, end_after}, WordPartInner{std::make_pair(i + 3, stop.value())}});
                    i = end_after;
                    literal_start = i;
                    continue;
                }
                const int64_t close = find_matching_paren(source, static_cast<int64_t>(i + 1));
                if (close == -1 || static_cast<size_t>(close) >= end) {
                    diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated command substitution"});
                    parts.push_back(WordPart{WordPartKind::kCommandSubstitution, SourceSpan{i, end}, WordPartInner{}});
                    i = end;
                    literal_start = i;
                    continue;
                }
                parts.push_back(WordPart{WordPartKind::kCommandSubstitution, SourceSpan{i, static_cast<size_t>(close) + 1}, WordPartInner{std::make_pair(i + 2, static_cast<size_t>(close))}});
                i = static_cast<size_t>(close) + 1;
                literal_start = i;
                continue;
            }
            if (nxt == '{') {
                const kimix::optional<size_t> stop = match_parameter_braces(source, i + 2);
                if (!stop.has_value()) {
                    diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated parameter expansion"});
                    parts.push_back(WordPart{WordPartKind::kParameterExpansion, SourceSpan{i, end}, WordPartInner{}});
                    i = end;
                    literal_start = i;
                    continue;
                }
                parts.push_back(WordPart{WordPartKind::kParameterExpansion, SourceSpan{i, stop.value() + 1}, WordPartInner{std::make_pair(i + 2, stop.value())}});
                i = stop.value() + 1;
                literal_start = i;
                continue;
            }
            // plain '$' is ordinary word content
            i += 1;
            continue;
        }
        if (ch == '`') {
            const int64_t close = find_backtick_end(source, static_cast<int64_t>(i + 1));
            if (close == -1 || static_cast<size_t>(close) > end) {
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated backquote substitution"});
                parts.push_back(WordPart{WordPartKind::kBackquoteSubstitution, SourceSpan{i, end}, WordPartInner{}});
                i = end;
                literal_start = i;
                continue;
            }
            parts.push_back(WordPart{WordPartKind::kBackquoteSubstitution, SourceSpan{i, static_cast<size_t>(close)}, WordPartInner{std::make_pair(i + 1, static_cast<size_t>(close) - 1)}});
            i = static_cast<size_t>(close);
            literal_start = i;
            continue;
        }
        i += 1;
    }
    if (i > literal_start) {
        parts.push_back(WordPart{WordPartKind::kLiteral, SourceSpan{literal_start, i}, WordPartInner{}});
    }
    return i;
}

// ── _hash_glued_after_substitution ──────────────────────────────────────────

// True when the previous WORD ends exactly at `hash_pos` with a command/process
// substitution region as its last part (the legacy `subst_just_closed` flag):
// only a '#' at the character directly after a substitution close is word
// content, not a comment.
bool hash_glued_after_substitution(const kimix::vector<Token> &tokens, size_t hash_pos) {
    if (tokens.empty()) {
        return false;
    }
    const Token &prev = tokens.back();
    if (prev.kind != TokenKind::kWord || prev.span.end != hash_pos) {
        return false;
    }
    if (prev.parts.empty()) {
        return false;
    }
    const WordPart &last = prev.parts.back();
    if (last.span.end != hash_pos) {
        return false;
    }
    return last.kind == WordPartKind::kCommandSubstitution || last.kind == WordPartKind::kProcessSubstitution;
}

} // namespace src_llm_bash_parser_bp_lexer
using namespace src_llm_bash_parser_bp_lexer;

// ── constant-set predicates ─────────────────────────────────────────────────

bool is_truncating_redirect_operator(kimix::string_view op) {
    static const char *const kOps[] = {">", ">|", ">>", "<>", "&>", "&>>"};
    for (const char *o : kOps) {
        if (op == kimix::string_view(o)) {
            return true;
        }
    }
    return false;
}

bool is_input_only_redirect_operator(kimix::string_view op) {
    static const char *const kOps[] = {"<", "<<", "<<-", "<<<", "<&"};
    for (const char *o : kOps) {
        if (op == kimix::string_view(o)) {
            return true;
        }
    }
    return false;
}

bool is_heredoc_operator(kimix::string_view op) {
    return op == kimix::string_view("<<") || op == kimix::string_view("<<-");
}

bool is_executable_region(WordPartKind kind) {
    return kind == WordPartKind::kCommandSubstitution || kind == WordPartKind::kBackquoteSubstitution ||
           kind == WordPartKind::kProcessSubstitution;
}

// ── the public tokenizer ────────────────────────────────────────────────────

LexResult lex(kimix::string_view source, size_t start, size_t end) {
    LexResult result;
    if (end == kLexUnbounded) {
        end = source.size();
    }
    // DEVIATION: the reference raises ValueError for a bad range; the port
    // declines with an empty tiling (no tokens) plus a diagnostic instead.
    if (!(start <= end && end <= source.size())) {
        result.diagnostics.push_back(Diagnostic{DiagnosticKind::kAmbiguousRegion, SourceSpan{start, end}, "bad lex range"});
        return result;
    }

    kimix::vector<Token> &tokens = result.tokens;
    kimix::vector<Diagnostic> &diagnostics = result.diagnostics;
    size_t i = start;
    while (i < end) {
        const char ch = source[i];
        if (is_hspace(ch)) {
            size_t stop = i;
            while (stop < end && is_hspace(source[stop])) {
                ++stop;
            }
            Token t;
            t.kind = TokenKind::kRaw;
            t.span = SourceSpan{i, stop};
            t.text = source.substr(i, stop - i);
            tokens.push_back(t);
            i = stop;
            continue;
        }
        if (ch == '\\' && i + 1 < end && source[i + 1] == '\n') {
            Token t;
            t.kind = TokenKind::kRaw;
            t.span = SourceSpan{i, i + 2};
            t.text = source.substr(i, 2);
            tokens.push_back(t);
            i += 2;
            continue;
        }
        if (ch == '\n') {
            Token t;
            t.kind = TokenKind::kNewline;
            t.span = SourceSpan{i, i + 1};
            tokens.push_back(t);
            i += 1;
            continue;
        }
        if (ch == '#' && comment_starts(source, i, start)) {
            // A '#' glued directly after a $(…) / <( / >( close continues the
            // word (substitutions expand inside words) and does NOT start a
            // comment — unlike '#' after a control operator ')'.
            if (!hash_glued_after_substitution(tokens, i)) {
                const size_t newline = find_char(source, '\n', i + 1, end);
                const size_t stop = (newline == SIZE_MAX) ? end : newline;
                Token t;
                t.kind = TokenKind::kComment;
                t.span = SourceSpan{i, stop};
                t.text = source.substr(i, stop - i);
                tokens.push_back(t);
                i = stop;
                continue;
            }
        }
        // process substitution: <( or >( — word-like region, not a redirection
        if (in_redirection_start(ch) && i + 1 < source.size() && source[i + 1] == '(') {
            const int64_t close = find_matching_paren(source, static_cast<int64_t>(i + 1));
            Token t;
            t.kind = TokenKind::kWord;
            if (close == -1 || static_cast<size_t>(close) >= end) {
                diagnostics.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, SourceSpan{i, end}, "unterminated process substitution"});
                t.span = SourceSpan{i, end};
                t.parts.push_back(WordPart{WordPartKind::kProcessSubstitution, SourceSpan{i, end}, WordPartInner{}});
                tokens.push_back(t);
                i = end;
                continue;
            }
            const size_t word_end = static_cast<size_t>(close) + 1;
            t.span = SourceSpan{i, word_end};
            t.parts.push_back(WordPart{WordPartKind::kProcessSubstitution, SourceSpan{i, word_end}, WordPartInner{std::make_pair(i + 2, static_cast<size_t>(close))}});
            tokens.push_back(t);
            i = word_end;
            continue;
        }
        // redirection operators, including fd prefixes (2>, >&2, &>, 2>&1)
        size_t j = i;
        bool has_fd = false;
        size_t fd_start = 0;
        char op_ch = ch; // the char at the (potential) operator position
        if (is_ascii_digit(ch) && redirection_after_fd(source, i, end)) {
            has_fd = true;
            fd_start = i;
            while (j < end && is_ascii_digit(source[j])) {
                ++j;
            }
            op_ch = (j < end) ? source[j] : '\0'; // '\0' stands for Python's ""
        }
        const bool op_is_redir_start = (j < end) && in_redirection_start(op_ch);
        const bool amp_gt = (op_ch == '&') && (j + 1 < source.size()) && (source[j + 1] == '>');
        if (op_is_redir_start || amp_gt) {
            kimix::string_view op;
            const size_t after = read_redirection_operator(source, j, end, &op);
            if (!op.empty()) {
                const size_t span_start = has_fd ? fd_start : i;
                Token t;
                t.kind = TokenKind::kRedirection;
                t.span = SourceSpan{span_start, after};
                t.text = source.substr(span_start, after - span_start);
                if (has_fd) {
                    t.fd = parse_fd(source.substr(fd_start, j - fd_start));
                }
                tokens.push_back(t);
                i = after;
                continue;
            }
        }
        // control operators / subshell parentheses
        if (in_control_op_starts(ch) && !in_redirection_start(ch)) {
            kimix::string_view op;
            const size_t after = read_control_operator(source, i, end, &op);
            if (!op.empty()) {
                Token t;
                t.kind = TokenKind::kOperator;
                t.span = SourceSpan{i, after};
                t.text = op;
                tokens.push_back(t);
                i = after;
                continue;
            }
        }
        if (ch == '#') {
            // glued '#' after a substitution close: word content, NOT a comment
            // (_scan_word would break on a leading '#', so we prepend the '#').
            Token t;
            t.kind = TokenKind::kWord;
            t.parts.push_back(WordPart{WordPartKind::kLiteral, SourceSpan{i, i + 1}, WordPartInner{}});
            const size_t word_end = scan_word(source, i + 1, end, diagnostics, t.parts);
            const size_t span_end = (word_end > i + 1) ? word_end : (i + 1);
            t.span = SourceSpan{i, span_end};
            tokens.push_back(t);
            i = span_end;
            continue;
        }
        // word
        {
            Token t;
            t.kind = TokenKind::kWord;
            const size_t word_end = scan_word(source, i, end, diagnostics, t.parts);
            if (word_end <= i) {
                // nothing consumed (defensive): one raw char keeps progress
                t.kind = TokenKind::kRaw;
                t.span = SourceSpan{i, i + 1};
                t.text = source.substr(i, 1);
                tokens.push_back(t);
                i += 1;
                continue;
            }
            t.span = SourceSpan{i, word_end};
            tokens.push_back(t);
            i = word_end;
        }
    }
    return result;
}

} // namespace kimix::llm::bash_parser
