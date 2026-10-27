// test_bash_parser_lexer.cpp — the lossless shell lexer (port of the
// assertions in kimi-agent tests/unit/tools/test_bash_lexer.py):
//   * token-stream tiling of the [start, end) range and word-part coverage of
//     each WORD span (the IR's lossless invariant);
//   * every quoting kind (single / double / ANSI-C `$'` / locale `$"`), escapes
//     and backslash line continuations;
//   * control operators and redirections (incl. fd prefixes 2>, >&2, &>, 2>&1)
//     and word-boundary splitting;
//   * comments (`#` word-initial only) and the glued-after-substitution `#`
//     staying word content;
//   * `$( )` / `$(( ))` / `${ }` / backtick / `<(`/`>(` regions with nesting,
//     and command-substitutions reported *inside* double quotes as sibling parts;
//   * heredoc marker lines (`<<`/`<<-`/`<<<`) — the delimiter is the next WORD;
//   * boundary equivalence against the bp_core oracle helpers;
//   * malformed / unterminated constructs: diagnostics + EOF-extended tokens,
//     never a throw;
//   * deep `$( )` nesting and a 20000-word line: one flat pass, no recursion.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only: pure lexer, no subprocess,
// no parser/render (those are separate, not-yet-written modules).
//
// NOTE: the reference's `render_source(parse(source)) == source` half of the
// malformed-lex test is omitted — the parser/models module is out of scope for
// the lexer task; the tiling half (which the lexer alone guarantees) is ported.

#include "ut/ut.hpp"

#include <llm/bash_parser/bp_core.h>
#include <llm/bash_parser/bp_lexer.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm::bash_parser;

namespace tests_unit_llm_test_bash_parser_lexer {

using sv = kimix::string_view;

// ── small structural helpers ────────────────────────────────────────────────

bool tiles(const LexResult &r, size_t size) {
    size_t cursor = 0;
    for (const Token &t : r.tokens) {
        if (t.span.start != cursor) {
            return false;
        }
        if (!(t.span.end > cursor || t.kind == TokenKind::kRaw)) {
            return false;
        }
        cursor = t.span.end;
    }
    return cursor == size;
}

// Every byte of each WORD span is covered by at least one part, and each part
// lies inside the word span.
bool word_parts_cover(const LexResult &r) {
    for (const Token &t : r.tokens) {
        if (t.kind != TokenKind::kWord || t.parts.empty()) {
            continue;
        }
        kimix::vector<char> covered(t.span.length(), 0);
        for (const WordPart &p : t.parts) {
            if (!t.span.contains(p.span)) {
                return false;
            }
            for (size_t pos = p.span.start; pos < p.span.end; ++pos) {
                covered[pos - t.span.start] = 1;
            }
        }
        for (const char c : covered) {
            if (!c) {
                return false;
            }
        }
    }
    return true;
}

kimix::string joined_spans(const LexResult &r, sv src) {
    kimix::string out;
    for (const Token &t : r.tokens) {
        const sv piece = t.span.text(src);
        out.append(piece.data(), piece.size());
    }
    return out;
}

kimix::vector<sv> texts_of(const LexResult &r, TokenKind kind) {
    kimix::vector<sv> out;
    for (const Token &t : r.tokens) {
        if (t.kind == kind) {
            out.push_back(t.text);
        }
    }
    return out;
}

size_t count_kind(const LexResult &r, TokenKind kind) {
    size_t n = 0;
    for (const Token &t : r.tokens) {
        if (t.kind == kind) {
            ++n;
        }
    }
    return n;
}

bool has_diagnostic(const LexResult &r, DiagnosticKind kind) {
    for (const Diagnostic &d : r.diagnostics) {
        if (d.kind == kind) {
            return true;
        }
    }
    return false;
}

bool any_part_kind(const LexResult &r, WordPartKind kind) {
    for (const Token &t : r.tokens) {
        for (const WordPart &p : t.parts) {
            if (p.kind == kind) {
                return true;
            }
        }
    }
    return false;
}

// Collect every part of `kind` across all tokens (the outer region only per lex).
kimix::vector<WordPart> parts_of(const LexResult &r, WordPartKind kind) {
    kimix::vector<WordPart> out;
    for (const Token &t : r.tokens) {
        for (const WordPart &p : t.parts) {
            if (p.kind == kind) {
                out.push_back(p);
            }
        }
    }
    return out;
}

sv span_text(sv src, const SourceSpan &s) { return src.substr(s.start, s.length()); }

// The second WORD token in source order (the one after a leading `echo`/`cmd`).
const Token *second_word(const LexResult &r) {
    size_t seen = 0;
    for (const Token &t : r.tokens) {
        if (t.kind == TokenKind::kWord && ++seen == 2) {
            return &t;
        }
    }
    return nullptr;
}

bool kind_mapped(WordPartKind k) {
    switch (k) {
        case WordPartKind::kDoubleQuote:
        case WordPartKind::kCommandSubstitution:
        case WordPartKind::kBackquoteSubstitution:
        case WordPartKind::kAnsiCQuote:
        case WordPartKind::kParameterExpansion:
        case WordPartKind::kArithmeticExpansion:
            return true;
        default:
            return false;
    }
}

// Legacy-oracle prediction of a region part's end (== the value the Python
// test_lexer_region_ends_match_legacy_oracles compares against).  `eof` is the
// full-range end.
size_t oracle_end(sv src, WordPartKind kind, size_t a, size_t eof) {
    switch (kind) {
        case WordPartKind::kDoubleQuote: {
            const int64_t v = find_dq_end(src, static_cast<int64_t>(a + 1));
            return v == -1 ? eof : static_cast<size_t>(v);
        }
        case WordPartKind::kCommandSubstitution: {
            const int64_t v = find_matching_paren(src, static_cast<int64_t>(a + 1));
            return v == -1 ? eof : static_cast<size_t>(v) + 1;
        }
        case WordPartKind::kBackquoteSubstitution: {
            const int64_t v = find_backtick_end(src, static_cast<int64_t>(a + 1));
            return v == -1 ? eof : static_cast<size_t>(v);
        }
        case WordPartKind::kAnsiCQuote: {
            const int64_t v = find_ansi_c_end(src, static_cast<int64_t>(a + 2));
            return v == -1 ? eof : static_cast<size_t>(v);
        }
        case WordPartKind::kParameterExpansion: {
            const auto v = match_parameter_braces(src, a + 2);
            return v.has_value() ? v.value() + 1 : eof;
        }
        case WordPartKind::kArithmeticExpansion: {
            const auto v = match_arithmetic_span(src, a + 3);
            return v.has_value() ? v.value() + 2 : eof;
        }
        default:
            return eof;
    }
}

// The reference's TILE_CASES / MALFORMED / ORACLE_CASES inputs (raw byte strings).
const char *const kTileCases[] = {
    "",
    " ",
    "\n",
    "echo hi",
    "echo  'a b'   \"c $(d) e\" ; x",
    "cat <<EOF\nbody\nEOF\nmore",
    "echo 2>&1 > out",
    "diff <(cat a) <(cat b)",
    "echo $(a) $(b) #notcomment ; x",
    "(a; b) #comment",
    "echo $'a\\n' $\"loc\" x",
    "echo a\\\nb",
    "echo 'unterminated",
    "echo \"unterminated",
    "echo $(unterminated",
    "echo `unterminated",
    "a=(1 2 3)",
    "case $x in\n a) tree;; esac",
    "f() { rev; }",
    "for ((i=0;i<2;i++)) { tree; }",
    "echo <(x)#glued",
    "cmd arg1\n\n",
    "a && b || c",
    "a | b | c",
    "<<< word",
    ">&2",
    "\\",
    "echo \\",
};

const char *const kMalformed[] = {
    "echo 'unterminated",
    "echo \"unterminated",
    "echo $'unterminated",
    "echo $\"unterminated",
    "echo $(unterminated",
    "echo `unterminated",
    "echo ${unterminated",
    "echo $((unterminated",
    "echo <(unterminated",
    "echo $( \"mixed up quotes",
    "echo a\\",
    "cat <<",
    "cat <<EOF",
    "\"",
    "'",
    "$",
    "$(",
    "${",
    "$((",
    "`",
};

const char *const kOracleCases[] = {
    "echo \"a b\" x",
    "echo \"a $(b c) d\"",
    "echo \"a\\\"b\" x",
    "echo 'plain' x",
    "echo $'a\\nb' tail",
    "echo $'unterminated",
    "echo \"unterminated",
    "echo $(unterminated",
    "echo `unterminated",
    "echo $(a) $(b) tail",
    "echo $(a $(b)) tail",
    "echo ${x} tail",
    "echo ${x:-$(y)} tail",
    "echo $((1+2)) tail",
    "echo $((1+$(x))) tail",
    "diff <(cat a) <(cat b)",
    "echo `date +'%y'`",
    "echo \"a `b` $(c) d\" e",
    "echo $(echo \"nested $(deep) here\") out",
};

} // namespace tests_unit_llm_test_bash_parser_lexer
using namespace tests_unit_llm_test_bash_parser_lexer;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "token_stream_tiles_its_range"_test = [] {
        for (const char *const s : kTileCases) {
            const sv src(s);
            const LexResult r = lex(src);
            expect(tiles(r, src.size())) << "tiles: " << src;
        }
    };

    "word_parts_tile_their_word"_test = [] {
        for (const char *const s : kTileCases) {
            const sv src(s);
            const LexResult r = lex(src);
            expect(word_parts_cover(r)) << "cover: " << src;
        }
    };

    "lex_range_subset_and_bad_range_declines"_test = [] {
        const sv src = "echo 'q' ; tail -f x";
        const LexResult r = lex(src, 8); // "; tail -f x"
        expect(!r.tokens.empty());
        expect(joined_spans(r, src) == kimix::string(src.substr(8)));
        // DEVIATION: a bad range returns empty + a diagnostic instead of throwing.
        const LexResult bad = lex(src, 5, 2);
        expect(bad.tokens.empty());
        expect(bad.diagnostics.size() == 1u);
        if (!bad.diagnostics.empty()) {
            expect(bad.diagnostics[0].kind == DiagnosticKind::kAmbiguousRegion);
        }
        // end beyond the source is also a bad range.
        const LexResult too_big = lex(src, 0, src.size() + 1);
        expect(too_big.tokens.empty());
        // an explicit full range covers exactly the same total as the default.
        expect(lex(src, 0, src.size()).tokens.size() == r.tokens.size() + lex(src, 0, 8).tokens.size());
    };

    "quote_region_part_kind"_test = [] {
        struct Case {
            sv word;
            WordPartKind kind;
        };
        const Case cases[] = {
            {"'a'", WordPartKind::kSingleQuote},
            {"\"a\"", WordPartKind::kDoubleQuote},
            {"$'a'", WordPartKind::kAnsiCQuote},
            {"$\"a\"", WordPartKind::kLocaleQuote},
        };
        for (const Case &c : cases) {
            kimix::string src("echo ");
            src.append(c.word.data(), c.word.size());
            const sv ssv(src);
            const LexResult r = lex(ssv);
            const Token *w2 = second_word(r);
            expect(w2 != nullptr);
            if (w2 == nullptr) {
                continue;
            }
            expect(w2->parts.size() >= 1u);
            if (w2->parts.empty()) {
                continue;
            }
            expect(w2->parts[0].kind == c.kind);
            expect(w2->parts[0].span == w2->span);
            expect(span_text(ssv, w2->parts[0].span) == c.word);
        }
    };

    "single_quote_content_is_literal"_test = [] {
        const sv src = "echo 'a $b `c' d";
        const LexResult r = lex(src);
        const Token *w2 = second_word(r);
        expect(w2 != nullptr);
        if (w2 == nullptr) {
            return;
        }
        expect(span_text(src, w2->span) == "'a $b `c'");
        expect(w2->parts.size() == 1u);
        if (!w2->parts.empty()) {
            expect(w2->parts[0].kind == WordPartKind::kSingleQuote);
        }
    };

    "ansi_c_escapes_skip_pairs"_test = [] {
        const sv src = "echo $'a\\'b' tail"; // $'a\'b'
        const LexResult r = lex(src);
        const Token *w2 = second_word(r);
        expect(w2 != nullptr);
        if (w2 == nullptr) {
            return;
        }
        expect(span_text(src, w2->span) == "$'a\\'b'");
        expect(w2->parts.size() == 1u);
        if (!w2->parts.empty()) {
            expect(w2->parts[0].kind == WordPartKind::kAnsiCQuote);
        }
    };

    "line_continuation_is_never_dropped"_test = [] {
        const sv src = "cmd a\\\nb"; // backslash newline
        const LexResult r = lex(src);
        bool has_lc = false;
        bool has_raw = false;
        for (const Token &t : r.tokens) {
            if (t.kind == TokenKind::kRaw) {
                has_raw = true;
            }
            for (const WordPart &p : t.parts) {
                if (p.kind == WordPartKind::kLineContinuation) {
                    has_lc = true;
                    expect(span_text(src, p.span) == "\\\n");
                }
            }
        }
        expect(has_lc || has_raw);
        expect(!r.tokens.empty());
        expect(tiles(r, src.size()));
    };

    "escaped_metacharacters_stay_in_words"_test = [] {
        const sv src = "echo a\\;b\\|c\\&d"; // a\;b\|c\&d
        const LexResult r = lex(src);
        const Token *w2 = second_word(r);
        expect(w2 != nullptr);
        if (w2 == nullptr) {
            return;
        }
        expect(span_text(src, w2->span) == "a\\;b\\|c\\&d");
    };

    "control_operators_tokenized"_test = [] {
        struct Case {
            sv src;
            kimix::vector<sv> ops;
        };
        const Case cases[] = {
            {"a && b || c | d ; e & f", {"&&", "||", "|", ";", "&"}},
            {"a |& b", {"|&"}},
            {"case x in a) b;; esac", {")", ";;"}},
            {"(a)", {"(", ")"}},
            {"a ;& b", {";&"}},
            {"a ;;& b", {";;&"}},
        };
        for (const Case &c : cases) {
            const LexResult r = lex(c.src);
            expect(texts_of(r, TokenKind::kOperator) == c.ops) << "ops for: " << c.src;
        }
    };

    "redirection_operators_with_fd_prefixes"_test = [] {
        struct Case {
            sv src;
            kimix::vector<sv> redirs;
        };
        const Case cases[] = {
            {"cat < a > b >> c", {"<", ">", ">>"}},
            {"cmd 2>&1", {"2>&"}},
            {"cmd > out 2>&1", {">", "2>&"}},
            {"cmd 2> err", {"2>"}},
            {"cmd &> both", {"&>"}},
            {"cmd &>> both", {"&>>"}},
            {"cmd <<< here", {"<<<"}},
            {"cmd << EOF", {"<<"}},
            {"cmd <<- EOF", {"<<-"}},
            {"cmd >&2", {">&"}},
            {"cmd <> rw", {"<>"}},
        };
        for (const Case &c : cases) {
            const LexResult r = lex(c.src);
            expect(texts_of(r, TokenKind::kRedirection) == c.redirs) << "redirs for: " << c.src;
        }
    };

    "fd_prefix_value_recorded"_test = [] {
        const sv src = "cmd 2>&1";
        for (const Token &t : lex(src).tokens) {
            if (t.kind == TokenKind::kRedirection) {
                expect(t.fd.has_value());
                expect(t.fd.has_value() && t.fd.value() == 2);
                expect(t.text == "2>&");
            }
        }
    };

    "digit_word_is_not_a_redirection"_test = [] {
        const LexResult r = lex("echo 2x");
        expect(count_kind(r, TokenKind::kRedirection) == 0u);
    };

    "operator_adjacency_boundaries"_test = [] {
        const sv src1 = "echo hi>out";
        const LexResult r1 = lex(src1);
        expect(tiles(r1, src1.size()));
        expect(!r1.tokens.empty());
        if (!r1.tokens.empty()) {
            expect(span_text(src1, r1.tokens[0].span) == "echo");
        }
        const auto reds = texts_of(r1, TokenKind::kRedirection);
        expect(reds.size() == 1u);
        if (!reds.empty()) {
            expect(reds[0] == ">");
        }
        const sv src2 = "foo;bar";
        const LexResult r2 = lex(src2);
        expect(joined_spans(r2, src2) == kimix::string(src2));
    };

    "read_control_operator_table"_test = [] {
        struct Case {
            sv src;
            sv op;
            size_t after;
        };
        const Case cases[] = {
            {";;&x", ";;&", 3}, {";;x", ";;", 2}, {";&x", ";&", 2}, {"&&x", "&&", 2}, {"||x", "||", 2}, {"|&x", "|&", 2},
            {"|x", "|", 1},     {";x", ";", 1},    {"&x", "&", 1}, {"(x", "(", 1}, {")x", ")", 1}, {"x", "", 0},         {"", "", 0},
        };
        for (const Case &c : cases) {
            sv op;
            const size_t after = read_control_operator(c.src, 0, c.src.size(), &op);
            expect(op == c.op) << "op for " << c.src;
            expect(after == c.after) << "after for " << c.src;
        }
    };

    "read_redirection_operator_table"_test = [] {
        const sv ops[] = {"&>>", "&>", "<<<", "<<-", "<<", ">>", "<>", ">|", "<&", ">&", "<", ">"};
        for (sv op : ops) {
            kimix::string s(op.data(), op.size());
            s += " rest";
            sv got;
            const size_t after = read_redirection_operator(s, 0, s.size(), &got);
            expect(got == op) << "op " << op;
            expect(after == op.size()) << "after " << op;
        }
        sv got;
        const size_t after = read_redirection_operator("&&", 0, 2, &got);
        expect(got.empty() && after == 0u);
    };

    "redirection_after_fd_predicate"_test = [] {
        expect(redirection_after_fd("2>&1", 0, 4) == true);
        expect(redirection_after_fd("23>", 0, 3) == true);
        expect(redirection_after_fd("2x", 0, 2) == false);
        expect(redirection_after_fd("2", 0, 1) == false);
    };

    "truncating_operator_tables"_test = [] {
        expect(is_truncating_redirect_operator(">>"));
        expect(is_truncating_redirect_operator("&>"));
        expect(is_truncating_redirect_operator(">"));
        expect(is_truncating_redirect_operator(">|"));
        expect(is_truncating_redirect_operator("<>"));
        expect(is_truncating_redirect_operator("&>>"));
        expect(!is_truncating_redirect_operator("<"));
        expect(is_input_only_redirect_operator("<"));
        expect(is_input_only_redirect_operator("<<"));
        expect(is_input_only_redirect_operator("<<<"));
        expect(is_input_only_redirect_operator("<&"));
        expect(!is_input_only_redirect_operator(">"));
        expect(is_heredoc_operator("<<"));
        expect(is_heredoc_operator("<<-"));
        expect(!is_heredoc_operator("<<<"));
        expect(is_executable_region(WordPartKind::kCommandSubstitution));
        expect(is_executable_region(WordPartKind::kBackquoteSubstitution));
        expect(is_executable_region(WordPartKind::kProcessSubstitution));
        expect(!is_executable_region(WordPartKind::kParameterExpansion));
        expect(!is_executable_region(WordPartKind::kLiteral));
    };

    "comment_tokens"_test = [] {
        struct Case {
            sv src;
            sv text;
        };
        const Case cases[] = {
            {"# whole line", "# whole line"},
            {"echo hi # tail", "# tail"},
            {"a; #tail", "#tail"},
            {"a && #tail", "#tail"},
            {"(a) #tail", "#tail"},
        };
        for (const Case &c : cases) {
            const LexResult r = lex(c.src);
            size_t n = 0;
            for (const Token &t : r.tokens) {
                if (t.kind == TokenKind::kComment) {
                    ++n;
                    expect(t.text == c.text) << "comment text for " << c.src;
                    expect(t.span.end == c.src.size());
                }
            }
            expect(n == 1u) << "one comment for " << c.src;
        }
    };

    "hash_not_a_comment_in_glued_positions"_test = [] {
        const sv srcs[] = {"echo a#b", "echo $(x)#glued", "echo <(x)#glued", "echo \"x\"#tail"};
        for (sv src : srcs) {
            expect(count_kind(lex(src), TokenKind::kComment) == 0u) << "glued # in " << src;
        }
    };

    "comment_stops_at_newline_only"_test = [] {
        const sv src = "x # c\ny";
        for (const Token &t : lex(src).tokens) {
            if (t.kind == TokenKind::kComment) {
                expect(t.text == "# c");
            }
        }
    };

    "region_part_kinds"_test = [] {
        struct Case {
            sv src;
            WordPartKind kind;
        };
        const Case cases[] = {
            {"echo $(rev)", WordPartKind::kCommandSubstitution},
            {"echo `rev`", WordPartKind::kBackquoteSubstitution},
            {"echo ${x}", WordPartKind::kParameterExpansion},
            {"echo $((1+2))", WordPartKind::kArithmeticExpansion},
            {"<(cat a) x", WordPartKind::kProcessSubstitution},
        };
        for (const Case &c : cases) {
            expect(any_part_kind(lex(c.src), c.kind)) << "kind for " << c.src;
        }
    };

    "inner_content_ranges"_test = [] {
        const sv src = "echo $(rev x)";
        // The LexResult must outlive the `inner` pointer taken below (a
        // range-for over lex(src).tokens would leave it dangling).
        const LexResult r = lex(src);
        const WordPartInner *inner = nullptr;
        for (const Token &t : r.tokens) {
            for (const WordPart &p : t.parts) {
                if (p.kind == WordPartKind::kCommandSubstitution) {
                    expect(p.inner.has_value());
                    inner = &p.inner;
                }
            }
        }
        expect(inner != nullptr);
        if (inner != nullptr && inner->has_value()) {
            const auto ip = inner->value();
            expect(src.substr(ip.first, ip.second - ip.first) == "rev x");
        }
    };

    "nested_command_substitutions_descend"_test = [] {
        const sv src = "echo $(a $(b $(c)) d)";
        const auto subs = parts_of(lex(src), WordPartKind::kCommandSubstitution);
        expect(subs.size() == 1u);
        if (subs.size() != 1u || !subs[0].inner.has_value()) {
            return;
        }
        expect(span_text(src, subs[0].span) == "$(a $(b $(c)) d)");
        const auto inner0 = subs[0].inner.value();
        const auto subs1 = parts_of(lex(src, inner0.first, inner0.second), WordPartKind::kCommandSubstitution);
        expect(!subs1.empty());
        if (subs1.empty() || !subs1[0].inner.has_value()) {
            return;
        }
        const auto inner1 = subs1[0].inner.value();
        expect(src.substr(inner1.first, inner1.second - inner1.first) == "b $(c)");
        const auto subs2 = parts_of(lex(src, inner1.first, inner1.second), WordPartKind::kCommandSubstitution);
        expect(!subs2.empty());
        if (!subs2.empty() && subs2[0].inner.has_value()) {
            const auto inner2 = subs2[0].inner.value();
            expect(src.substr(inner2.first, inner2.second - inner2.first) == "c");
        }
    };

    "substitution_inside_double_quotes_reported"_test = [] {
        const sv src = "echo \"a $(b c) d\"";
        // The LexResult must outlive the `cs` pointer taken below (a
        // range-for over lex(src).tokens would leave it dangling).
        const LexResult r = lex(src);
        const WordPart *cs = nullptr;
        bool has_dq = false;
        for (const Token &t : r.tokens) {
            if (t.kind != TokenKind::kWord) {
                continue;
            }
            bool word_has_dq = false;
            for (const WordPart &p : t.parts) {
                if (p.kind == WordPartKind::kDoubleQuote) {
                    word_has_dq = true;
                }
            }
            if (word_has_dq) {
                has_dq = true;
                for (const WordPart &p : t.parts) {
                    if (p.kind == WordPartKind::kCommandSubstitution) {
                        cs = &p;
                    }
                }
            }
        }
        expect(has_dq);
        expect(cs != nullptr);
        if (cs != nullptr) {
            expect(span_text(src, cs->span) == "$(b c)");
        }
    };

    "backquote_inside_double_quotes_reported"_test = [] {
        const sv src = "echo \"a `b c` d\"";
        const LexResult r = lex(src);
        expect(any_part_kind(r, WordPartKind::kDoubleQuote));
        expect(any_part_kind(r, WordPartKind::kBackquoteSubstitution));
    };

    "substitutions_do_not_cross_word_boundaries"_test = [] {
        const sv src = "x$(y)z";
        const LexResult r = lex(src);
        expect(count_kind(r, TokenKind::kWord) == 1u);
        for (const Token &t : r.tokens) {
            if (t.kind == TokenKind::kWord) {
                expect(span_text(src, t.span) == "x$(y)z");
            }
        }
    };

    "heredoc_marker_tokens"_test = [] {
        const sv src = "cat <<EOF\nbody\nEOF\n";
        const LexResult r = lex(src);
        expect(texts_of(r, TokenKind::kRedirection) == kimix::vector<sv>({"<<"}));
        bool eof_word = false;
        for (const Token &t : r.tokens) {
            if (t.kind == TokenKind::kWord && span_text(src, t.span) == "EOF") {
                eof_word = true;
            }
        }
        expect(eof_word);
    };

    "herestring_is_its_own_operator"_test = [] {
        const LexResult r = lex("rev <<< abc");
        expect(texts_of(r, TokenKind::kRedirection) == kimix::vector<sv>({"<<<"}));
    };

    "region_ends_match_legacy_oracles"_test = [] {
        for (const char *const s : kOracleCases) {
            const sv src(s);
            const LexResult r = lex(src);
            const size_t eof = src.size();
            for (const Token &t : r.tokens) {
                for (const WordPart &p : t.parts) {
                    if (!kind_mapped(p.kind)) {
                        continue;
                    }
                    const size_t expected = oracle_end(src, p.kind, p.span.start, eof);
                    expect(p.span.end == expected) << "oracle end mismatch: " << src << " kind@" << p.span.start << " exp " << expected << " got " << p.span.end;
                }
            }
        }
    };

    "dq_end_equals_find_dq_end_simple"_test = [] {
        struct Case {
            sv src;
            size_t quote_pos;
        };
        const Case cases[] = {
            {"echo \"a\" x", 5},
            {"echo \"a\\\"b\" x", 5},
            {"echo \"plain\" ", 5},
        };
        for (const Case &c : cases) {
            const int64_t expected = find_dq_end(c.src, static_cast<int64_t>(c.quote_pos + 1));
            bool matched = false;
            for (const Token &t : lex(c.src).tokens) {
                for (const WordPart &p : t.parts) {
                    if (p.kind == WordPartKind::kDoubleQuote && p.span.start == c.quote_pos) {
                        const size_t exp = expected == -1 ? c.src.size() : static_cast<size_t>(expected);
                        expect(p.span.end == exp) << "dq end for " << c.src;
                        matched = true;
                    }
                }
            }
            expect(matched) << "no dq part at " << c.src;
        }
    };

    "malformed_lexes_without_raising_and_tiles"_test = [] {
        for (const char *const s : kMalformed) {
            const sv src(s);
            const LexResult r = lex(src);
            expect(tiles(r, src.size())) << "malformed tiles: " << src;
        }
    };

    "unterminated_quotes_diagnostic"_test = [] {
        const sv srcs[] = {"echo 'x", "echo \"x", "echo $'x", "echo $\"x"};
        for (sv src : srcs) {
            expect(has_diagnostic(lex(src), DiagnosticKind::kUnterminatedQuote)) << "quote diag for " << src;
        }
    };

    "unterminated_substitutions_diagnostic"_test = [] {
        const sv srcs[] = {"echo $(x", "echo `x", "echo ${x", "echo $((x", "echo <(x", "echo \"a $(b"};
        for (sv src : srcs) {
            const LexResult r = lex(src);
            expect(!r.diagnostics.empty()) << "has diag for " << src;
            expect(has_diagnostic(r, DiagnosticKind::kUnterminatedSubst)) << "subst diag for " << src;
        }
    };

    "deep_nesting_lexes_in_bounded_time_without_recursion"_test = [] {
        kimix::string src;
        for (int k = 0; k < 2000; ++k) {
            src += "$(";
        }
        src += "x";
        for (int k = 0; k < 2000; ++k) {
            src += ")";
        }
        const sv ssv(src);
        const LexResult r = lex(ssv);
        expect(tiles(r, ssv.size()));
        if (!r.tokens.empty()) {
            expect(r.tokens[0].span.end <= ssv.size());
        }
    };

    "wide_input_is_linear_and_counts_exactly"_test = [] {
        kimix::string words;
        for (int k = 0; k < 20000; ++k) {
            if (k > 0) {
                words += " ";
            }
            words += "w";
        }
        const sv src(words);
        const LexResult r = lex(src);
        expect(count_kind(r, TokenKind::kWord) == 20000u);
        expect(count_kind(r, TokenKind::kRaw) == 19999u);
        expect(tiles(r, src.size()));
    };

    "diagnostic_spans_and_kinds_are_internal_data"_test = [] {
        const LexResult r = lex("echo 'x");
        expect(!r.diagnostics.empty());
        if (!r.diagnostics.empty()) {
            const Diagnostic &d = r.diagnostics[0];
            expect(d.span.start <= d.span.end && d.span.end <= 7u);
        }
    };

    return 0;
}
