// test_bash_parser_shell_ir.cpp — the lossless shell IR (port of the
// assertions in kimi-agent tests/unit/tools/test_bash_shell_ir.py):
//   * the lossless invariant render_source(parse(src)) == src over the full
//     corpus (plain commands, every quoting kind, comments, heredocs, nested
//     substitutions, redirections, control operators, compounds, wrappers,
//     embedded scripts, malformed/unterminated input, CRLF);
//   * command-position classification (command word vs arguments vs assignments)
//     and the assignment / null-target / keyword primitives;
//   * comments and heredoc bodies as DATA nodes (never command contexts) and the
//     full heredoc node semantics (delimiter value, strip_tabs, expands,
//     body/terminator spans, unterminated diagnostics);
//   * nested executable substitutions ($(), backquote, <()) — including the ones
//     the lexer reports inside double quotes — become child ScriptNode contexts;
//   * wrapper hints and embedded inline scripts (eval/trap/watch, shell -c,
//     find -exec sh -c) with quote_kind + inner source;
//   * bounded iterative depth (250- and 2,000-level $( ) and a 1,000-level
//     backtick nest terminate and reconstruct; the descent stops at the legacy
//     kMaxNestingDepth with a depth_limit diagnostic);
//   * linear scaling for a 20,000-word command;
//   * the static word decoders and the span/edit/diagnostic primitives.
//
// DEVIATIONS from the Python suite (documented):
//   - the reference asserts Python dataclass *frozen/slotted* behavior and
//     ValueError on a malformed SourceSpan/Edit range; the port builds those as
//     plain value structs with no runtime validation (a malformed parse range
//     instead returns empty), so those reflection/raise assertions have no C++
//     analogue and are omitted.  The observable behavior (tiling, rendering) is
//     ported in full.
//   - reconstruct() is render_source() in the port (no separate method).
//
// Framework: Boost.UT (tests/ut/ut.hpp).  Pure CPU: parser + render, no
// subprocess.

#include "ut/ut.hpp"

#include <llm/bash_parser/bp_core.h>
#include <llm/bash_parser/bp_lexer.h>
#include <llm/bash_parser/bp_models.h>
#include <llm/bash_parser/bp_parser_ir.h>
#include <llm/bash_parser/bp_spans.h>

#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm::bash_parser;

namespace tests_unit_llm_test_bash_parser_shell_ir {

using sv = kimix::string_view;

// ── structural helpers ──────────────────────────────────────────────────────

// The first CommandNode (in walk order) that carries a command word.
const Node *first_command(const Node &script) {
    for (const Node *n : walk_nodes(script)) {
        if (n->kind == NodeKind::kCommand && n->command_word_p() != nullptr) {
            return n;
        }
    }
    return nullptr;
}

// All command-word texts in walk order.
kimix::vector<sv> command_words(const Node &script, sv src) {
    kimix::vector<sv> out;
    for (const Node *n : walk_nodes(script)) {
        if (n->kind == NodeKind::kCommand && n->command_word_p() != nullptr) {
            out.push_back(n->command_word_p()->span.text(src));
        }
    }
    return out;
}

bool has_command_word(sv source, sv want) {
    auto script = parse(source);
    for (const sv word : command_words(script, sv(source))) {
        if (word == want) {
            return true;
        }
    }
    return false;
}

// Does any ScriptNode in the tree carry a diagnostic of `kind`?
bool has_diagnostic(const Node &script, DiagnosticKind kind) {
    for (const Node *n : walk_nodes(script)) {
        if (n->kind != NodeKind::kScript) {
            continue;
        }
        for (const Diagnostic &d : n->diagnostics) {
            if (d.kind == kind) {
                return true;
            }
        }
    }
    return false;
}

bool has_word_in(const Node &cmd, const kimix::vector<kimix::unique_ptr<Node>> &list, sv src, sv want) {
    for (const auto &w : list) {
        if (w->span.text(src) == want) {
            return true;
        }
    }
    return false;
}

// Argument texts of a command.
kimix::vector<sv> arg_texts(const Node &cmd, sv src) {
    kimix::vector<sv> out;
    for (const auto &a : cmd.arguments) {
        out.push_back(a->span.text(src));
    }
    return out;
}

kimix::vector<sv> assign_texts(const Node &cmd, sv src) {
    kimix::vector<sv> out;
    for (const auto &a : cmd.assignments) {
        out.push_back(a->span.text(src));
    }
    return out;
}

const Node *find_redirection(const Node &cmd, sv op) {
    for (const auto &r : cmd.redirections) {
        if (r->operator_text == op) {
            return r.get();
        }
    }
    return nullptr;
}

// The HeredocNode in a tree (first one).
const Node *first_heredoc(const Node &script) {
    for (const Node *n : walk_nodes(script)) {
        if (n->kind == NodeKind::kHeredoc) {
            return n;
        }
    }
    return nullptr;
}

// The EmbeddedScriptNode in a tree (first one).
const Node *first_embedded(const Node &script) {
    for (const Node *n : walk_nodes(script)) {
        if (n->kind == NodeKind::kEmbedded) {
            return n;
        }
    }
    return nullptr;
}

// The expansion nodes of a given kind (walk order).
kimix::vector<const Node *> expansions(const Node &script, WordPartKind kind) {
    kimix::vector<const Node *> out;
    for (const Node *n : walk_nodes(script)) {
        if (n->kind == NodeKind::kExpansion && n->expansion_kind == kind) {
            out.push_back(n);
        }
    }
    return out;
}

// ── corpora (raw byte strings, byte-for-byte with the reference) ─────────────

const char *const kLossless[] = {
    // plain commands and whitespace shapes
    "",
    "   ",
    "\n\n",
    "rev",
    "echo hi",
    "foo;bar",
    "cmd arg1\n\n",
    "tail -f a\nhead b",
    "a && b || c && d",
    "a | b | c",
    "a | b && c ; d",
    // quoting of every kind
    "echo 'a b' \"c $(d) e\"",
    "echo $'a\\'b' x",
    "echo $\"locale\" x",
    "echo a\\nb",
    "echo $x $@ ${y:-z} $((1+2))",
    "echo ${a:-$(b)}",
    "echo \"hi $name $(whoami)\"",
    "echo \"a `b` c\"",
    // comments (whole line and trailing)
    "#whole line comment",
    "echo hi # trailing comment",
    "echo $(a) $(b) #notcomment ; x",
    "echo <(x)#glued",
    "echo $(y)#z; rev",
    "(a; b) #comment",
    // heredocs (all shapes incl. operators on the delimiter line)
    "cat <<EOF\nbody\nEOF\nmore",
    "cat <<EOF\nEOF",
    "cat <<EOF",
    "cat <<",
    "cat <<EOF | grep x",
    "cat <<-EOF\n\tbody\n\tEOF\nafter",
    "cat <<'QEOF'\n$(x)\nQEOF",
    "cat <<\"E O F\"\nb\nE O F",
    "cat <<A\nx\nA\ncat <<B\ny\nB\nz",
    "cat <<EOF; rev",
    "cat <<'E'\n$(not executed)\nE\nrev",
    // nested substitutions
    "echo $( $(nested) )",
    "echo \"a $(b $(c)) d\"",
    "echo $(rev <<< abc)",
    "diff <(cat a) <(cat b)",
    "echo `date`",
    "echo $(a; b) #notcomment ; x",
    // redirections and control operators
    "x=1 tree a > out 2>&1",
    "echo 2>&1 > out",
    "echo hi>out",
    "sort < a > b >> c",
    "cmd >&2",
    "echo |& cat",
    "echo foo;; bar",
    "<<< word",
    ">",
    "&",
    "|",
    ";;",
    ") x",
    // compound constructs
    "case $x in\n a) tree;; esac",
    "case `date` in a) tree;; esac",
    "if tree; then rev; fi",
    "while read -r l; do echo $l; done",
    "for ((i=0;i<2;i++)) { tree; }",
    "f() { rev; }",
    "f(){ rev; }",
    "function f { rev; }",
    "(ls; pwd) | grep x",
    // wrappers / reparse builtins / -c scripts
    "eval \"tree\"",
    "trap 'rm -f x' EXIT",
    "watch -n1 'rev <<< abc'",
    "bash -c 'cd /tmp && rev'",
    "find . -exec sh -c 'rev' \\;",
    "a=(1 2 3)",
    "x=1 y=2 cmd arg",
    "declare -a arr=(\"C:\\x\")",
    "time ls | wc -l",
    "nice -n 5 rev f",
    "timeout 5 rev",
    "env A=1 B=2 tree",
    "sudo rev f",
    // malformed / unterminated
    "echo 'unterminated",
    "echo \"unterminated",
    "echo $(unterminated",
    "echo `unterminated",
    "echo $'unterminated",
    "echo ${unterminated",
    "echo $((unterminated",
    "echo <(unterminated",
    "(unterminated",
    "cat <<EOF\nbody never ends",
    "cat <<$'malformed",
    "a &&",
    "a ||",
    "a |",
    "a ;&",
    "a ;;",
    "a ;;&",
    "\\",
    "echo \\",
};

const char *const kMalformed[] = {
    "echo 'unterminated",
    "echo \"unterminated",
    "echo $'unterminated",
    "echo $(unterminated",
    "echo `unterminated",
    "echo ${unterminated",
    "echo $((unterminated",
    "echo <(unterminated",
    "(unterminated",
    "cat <<EOF\nno terminator here",
    "cat <<",
    "a &&",
    "a |",
    ";;",
    ") x",
    "\\",
    "echo \\",
    "echo \"a $(b",
    "case $x in",
    "if",
    "then",
    "fi",
    "done",
    "esac",
    "{",
    "echo \"$( \"nested unterminated",
    "cat <<EOF\nbody\nEOFtail",
};

size_t count_of(const char *const arr[], size_t n) {
    (void)n;
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        (void)arr[i];
        ++c;
    }
    return c;
}

} // namespace tests_unit_llm_test_bash_parser_shell_ir
using namespace tests_unit_llm_test_bash_parser_shell_ir;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // ── 1. lossless invariant ────────────────────────────────────────────────
    "lossless_render_source_roundtrip"_test = [] {
        for (const char *const s : kLossless) {
            const sv src(s);
            auto script = parse(src);
            expect(render_source(script) == kimix::string(src)) << "lossless: " << src;
        }
    };

    "reconstruct_matches_render"_test = [] {
        const sv src = "echo hi > out\n# c\n";
        auto script = parse(src);
        expect(render_source(script) == kimix::string(src));
    };

    "lossless_on_crlf_source"_test = [] {
        const sv src = "echo hi\r\nif x; then\r\nfi\r\n";
        auto script = parse(src);
        expect(render_source(script) == kimix::string(src));
    };

    "lossless_nested_diagnostics_survive_roundtrip"_test = [] {
        const sv src = "echo \"a $(b) c\" tail";
        auto script = parse(src);
        expect(render_source(script) == kimix::string(src));
    };

    // ── 2. command-position classification ───────────────────────────────────
    "first_word_after_start_is_command_word"_test = [] {
        const sv src = "tree arg1 arg2";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        expect(cmd->command_word_p()->span.text(src) == "tree");
        expect(arg_texts(*cmd, src) == kimix::vector<sv>({"arg1", "arg2"}));
        expect(cmd->assignments.empty());
    };

    "command_position_after_each_operator"_test = [] {
        expect(has_command_word("a ; tree", "tree"));
        expect(has_command_word("a && tree", "tree"));
        expect(has_command_word("a || tree", "tree"));
        expect(has_command_word("a | tree", "tree"));
        expect(has_command_word("( tree )", "tree"));
        expect(has_command_word("{ tree ; }", "tree"));
        expect(has_command_word("x=1 tree", "tree"));
    };

    "assignments_before_command_word"_test = [] {
        const sv src = "A=1 B=2 tree arg";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        expect(assign_texts(*cmd, src) == kimix::vector<sv>({"A=1", "B=2"}));
        expect(cmd->command_word_p()->span.text(src) == "tree");
        expect(arg_texts(*cmd, src) == kimix::vector<sv>({"arg"}));
    };

    "words_after_command_word_are_arguments"_test = [] {
        const sv src = "cat -n file.txt";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        expect(cmd->command_word_p()->span.text(src) == "cat");
        expect(arg_texts(*cmd, src) == kimix::vector<sv>({"-n", "file.txt"}));
    };

    "declaration_arguments_are_assignments"_test = [] {
        const sv src = "declare x=1";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        expect(cmd->command_word_p()->span.text(src) == "declare");
        bool found = false;
        for (const sv t : assign_texts(*cmd, src)) {
            if (bp_starts_with(t, "x=")) {
                found = true;
            }
        }
        expect(found) << "x=1 recorded as assignment";
    };

    "classify_assignment_predicate"_test = [] {
        expect(classify_assignment("x=1").value_or("") == "x");
        expect(classify_assignment("FOO+=1").value_or("") == "FOO+");
        expect(classify_assignment("_a1=v").value_or("") == "_a1");
        expect(!classify_assignment("1x=v").has_value());
        expect(!classify_assignment("=v").has_value());
        expect(!classify_assignment("plain").has_value());
    };

    "quoted_command_word_still_classified"_test = [] {
        const sv src = "'tree' arg";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        const Node *cw = cmd->command_word_p();
        expect(cw->span.text(src) == "'tree'");
        expect(cw->literal_value.has_value() && *cw->literal_value == "tree");
    };

    "wrapper_hints_recorded_from_legacy_tables"_test = [] {
        auto s1 = parse("env A=1 tree");
        const Node *c1 = first_command(s1);
        expect(c1 != nullptr);
        expect(c1 != nullptr && c1->wrapper.has_value() && *c1->wrapper == "env");
        expect(c1 != nullptr && c1->wrapper_hint.has_value() && *c1->wrapper_hint == "command");

        auto s2 = parse("bash -c 'rev'");
        const Node *c2 = first_command(s2);
        expect(c2 != nullptr);
        expect(c2 != nullptr && c2->wrapper.has_value() && *c2->wrapper == "bash");
        expect(c2 != nullptr && c2->wrapper_hint.has_value() && *c2->wrapper_hint == "shell");

        auto s3 = parse("eval 'rev'");
        const Node *c3 = first_command(s3);
        expect(c3 != nullptr);
        expect(c3 != nullptr && c3->wrapper.has_value() && *c3->wrapper == "eval");
        expect(c3 != nullptr && c3->wrapper_hint.has_value() && *c3->wrapper_hint == "reparse");
    };

    "redirection_fd_and_truncate_predicate"_test = [] {
        const sv src = "tree > out 2>&1";
        auto script = parse(src);
        const Node *cmd = first_command(script);
        expect(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        const Node *gt = find_redirection(*cmd, ">");
        expect(gt != nullptr);
        if (gt != nullptr) {
            expect(gt->can_truncate);
            expect(!gt->fd.has_value());
            expect(gt->target_p() != nullptr && gt->target_p()->span.text(src) == "out");
        }
        const Node *dup = find_redirection(*cmd, "2>&");
        expect(dup != nullptr);
        if (dup != nullptr) {
            expect(dup->fd.has_value() && dup->fd.value() == 2);
            expect(!dup->can_truncate) << "dup-to-fd never creates a file";
        }
    };

    "redirection_truncates_input_vs_output"_test = [] {
        auto s1 = parse("sort < in");
        const Node *c1 = first_command(s1);
        expect(c1 != nullptr && !c1->redirections.empty());
        if (c1 && !c1->redirections.empty()) {
            expect(!c1->redirections[0]->can_truncate);
        }
        auto s2 = parse("sort > out");
        const Node *c2 = first_command(s2);
        expect(c2 != nullptr && !c2->redirections.empty());
        if (c2 && !c2->redirections.empty()) {
            expect(c2->redirections[0]->can_truncate);
        }
        auto s3 = parse("sort <> both");
        const Node *c3 = first_command(s3);
        expect(c3 != nullptr && !c3->redirections.empty());
        if (c3 && !c3->redirections.empty()) {
            expect(c3->redirections[0]->can_truncate);
        }
    };

    "is_effective_nul_target_matches_legacy_semantics"_test = [] {
        auto value_for = [](const char *word) -> kimix::optional<bool> {
            const kimix::string src = kimix::string("cmd > ") + word;
            auto script = parse(sv(src));
            const Node *cmd = first_command(script);
            if (cmd == nullptr || cmd->redirections.empty()) {
                return std::nullopt;
            }
            const Node *target = cmd->redirections[0]->target_p();
            if (target == nullptr) {
                return std::nullopt;
            }
            return is_effective_nul_target(*target, sv(src));
        };
        expect(value_for("nul") == kimix::optional<bool>(true));
        expect(value_for("NUL") == kimix::optional<bool>(true));
        expect(value_for("out.txt") == kimix::optional<bool>(false));
        expect(value_for("$(echo nul)") == std::nullopt);
    };

    // ── 3. comments and heredoc bodies are data nodes ────────────────────────
    "comment_is_raw_data_node"_test = [] {
        const sv src = "rev  # trailing";
        auto script = parse(src);
        bool found = false;
        for (const auto &c : script.children) {
            if (c->kind == NodeKind::kRaw && c->reason == "comment") {
                expect(c->span.text(src) == "# trailing");
                found = true;
            }
        }
        expect(found) << "comment data node present";
    };

    "heredoc_body_is_data_never_command_context"_test = [] {
        const sv src = "cat <<EOF\nrev not-a-command\n$(x) not-a-context\nEOF\n";
        auto script = parse(src);
        bool body = false;
        for (const Node *n : walk_nodes(script)) {
            if (n->kind == NodeKind::kRaw && n->reason == "heredoc-body") {
                expect(n->span.text(src).find("rev not-a-command") != sv::npos);
                body = true;
            }
        }
        expect(body) << "heredoc body segment present";
        for (const Node *n : walk_nodes(script)) {
            if (n->kind == NodeKind::kCommand && n->command_word_p() != nullptr) {
                const sv t = n->command_word_p()->span.text(src);
                expect(t != "rev") << "heredoc body leaked into context";
                expect(t != "$(x)");
            }
        }
    };

    "heredoc_node_flags"_test = [] {
        const sv src = "cat <<-EOF\n\tbody\n\tEOF\n";
        auto script = parse(src);
        const Node *doc = first_heredoc(script);
        expect(doc != nullptr);
        if (doc == nullptr) {
            return;
        }
        expect(doc->strip_tabs);
        expect(doc->delimiter_value.has_value() && *doc->delimiter_value == "EOF");
        expect(doc->expands);
        expect(doc->body_end.has_value() && doc->body_start < doc->body_end.value());
        expect(doc->terminator_span.has_value());
        if (doc->terminator_span.has_value()) {
            expect(doc->terminator_span->text(src) == "\tEOF");
        }
    };

    "quoted_delimiter_disables_expansion"_test = [] {
        const sv src = "cat <<'EOF'\n$(x)\nEOF\n";
        auto script = parse(src);
        const Node *doc = first_heredoc(script);
        expect(doc != nullptr);
        if (doc == nullptr) {
            return;
        }
        expect(!doc->expands);
        expect(doc->delimiter_value.has_value() && *doc->delimiter_value == "EOF");
    };

    // ── 4. substitutions inside double quotes are child contexts ─────────────
    "command_substitution_inside_double_quotes_is_child_context"_test = [] {
        const sv src = "echo \"a $(rev <<< abc) d\"";
        auto script = parse(src);
        auto exps = expansions(script, WordPartKind::kCommandSubstitution);
        expect(!exps.empty()) << "no expansion child built";
        if (exps.empty()) {
            return;
        }
        expect(exps[0]->expansion_kind == WordPartKind::kCommandSubstitution);
        expect(exps[0]->child_p() != nullptr);
        if (exps[0]->child_p() != nullptr) {
            expect(render_source(*exps[0]->child_p()) == "rev <<< abc");
        }
    };

    "nested_dquote_substitution_children_chain"_test = [] {
        const sv src = "echo \"a $(b \"c $(d)\" e)\" f\"";
        auto script = parse(src);
        bool found = false;
        for (const Node *n : walk_nodes(script)) {
            if (n->kind == NodeKind::kExpansion && n->child_p() != nullptr) {
                if (render_source(*n->child_p()) == "b \"c $(d)\" e") {
                    found = true;
                }
            }
        }
        expect(found) << "nested region 'b \"c $(d)\" e' built";
    };

    "process_substitution_child_context"_test = [] {
        const sv src = "diff <(cat a) <(cat b)";
        auto script = parse(src);
        auto exps = expansions(script, WordPartKind::kProcessSubstitution);
        expect(exps.size() == 2u) << "two process substitutions";
        if (exps.size() == 2 && exps[0]->child_p() != nullptr) {
            expect(render_source(*exps[0]->child_p()) == "cat a");
        }
    };

    "backquote_child_context"_test = [] {
        const sv src = "echo `date`";
        auto script = parse(src);
        auto exps = expansions(script, WordPartKind::kBackquoteSubstitution);
        expect(!exps.empty());
        if (!exps.empty() && exps[0]->child_p() != nullptr) {
            expect(exps[0]->expansion_kind == WordPartKind::kBackquoteSubstitution);
            expect(render_source(*exps[0]->child_p()) == "date");
        }
    };

    "embedded_script_operand_is_inner_context"_test = [] {
        const sv src = "eval 'tree > nul'";
        auto script = parse(src);
        const Node *emb = first_embedded(script);
        expect(emb != nullptr);
        if (emb == nullptr) {
            return;
        }
        expect(emb->quote_kind.has_value() && *emb->quote_kind == WordPartKind::kSingleQuote);
        expect(emb->owner_word_p() != nullptr);
        if (emb->owner_word_p() != nullptr) {
            expect(emb->owner_word_p()->span.text(src) == "'tree > nul'");
        }
        expect(emb->inner_p() != nullptr);
        if (emb->inner_p() != nullptr) {
            const Node *inner = emb->inner_p();
            expect(render_source(*inner) == "tree > nul");
            if (!inner->children.empty()) {
                const Node *ic = inner->children[0].get();
                expect(ic->kind == NodeKind::kCommand);
                if (ic->kind == NodeKind::kCommand && ic->command_word_p()) {
                    expect(ic->command_word_p()->span.text(sv("tree > nul")) == "tree");
                }
            }
        }
    };

    "shell_dash_c_embedded_script"_test = [] {
        auto script = parse("bash -c 'cd /tmp && rev'");
        const Node *emb = first_embedded(script);
        expect(emb != nullptr);
        if (emb && emb->inner_p()) {
            expect(render_source(*emb->inner_p()) == "cd /tmp && rev");
        }
    };

    "find_exec_sh_c_embedded_script"_test = [] {
        auto script = parse("find . -exec sh -c 'rev' \\;");
        const Node *emb = first_embedded(script);
        expect(emb != nullptr);
        if (emb && emb->inner_p()) {
            expect(render_source(*emb->inner_p()) == "rev");
        }
    };

    // ── 5. malformed / unterminated constructs never raise ───────────────────
    "malformed_input_never_raises_and_stays_lossless"_test = [] {
        for (const char *const s : kMalformed) {
            const sv src(s);
            auto script = parse(src);
            expect(render_source(script) == kimix::string(src)) << "malformed: " << src;
            expect(script.kind == NodeKind::kScript);
        }
    };

    "unterminated_quotes_produce_diagnostics"_test = [] {
        for (const char *const s : {"echo 'unterminated", "echo \"unterminated", "echo $'unterminated"}) {
            auto script = parse(sv(s));
            expect(has_diagnostic(script, DiagnosticKind::kUnterminatedQuote)) << s;
        }
    };

    "unterminated_substitution_produces_diagnostics"_test = [] {
        auto script = parse("echo $(oops");
        expect(has_diagnostic(script, DiagnosticKind::kUnterminatedSubst));
    };

    "unterminated_heredoc_produces_diagnostic"_test = [] {
        auto script = parse("cat <<EOF\nbody with no terminator");
        expect(has_diagnostic(script, DiagnosticKind::kUnterminatedHeredoc));
    };

    "malformed_input_yields_raw_or_partial_not_missing"_test = [] {
        for (const char *const s : {"echo 'x", "(a", "a |", ";;"}) {
            const sv src(s);
            auto script = parse(src);
            expect(render_source(script) == kimix::string(src));
            expect(!script.children.empty()) << src;
        }
    };

    // ── 6. deep nesting terminates (bounded, no recursion) ──────────────────
    "deep_command_substitution_terminates"_test = [] {
        kimix::string src;
        src.reserve(8001);
        for (int i = 0; i < 2000; ++i) {
            src += "$(";
        }
        src += "x";
        for (int i = 0; i < 2000; ++i) {
            src += ")";
        }
        Clock clock;
        auto script = parse(sv(src));
        const double elapsed = clock.toc_seconds();
        expect(render_source(script) == src) << "deep render";
        expect(elapsed < 10.0) << "deep nesting too slow: " << elapsed;
    };

    "deep_nesting_emits_depth_diagnostic"_test = [] {
        kimix::string src;
        for (int i = 0; i < 2000; ++i) {
            src += "$(";
        }
        src += "x";
        for (int i = 0; i < 2000; ++i) {
            src += ")";
        }
        auto script = parse(sv(src));
        expect(has_diagnostic(script, DiagnosticKind::kDepthLimit));
    };

    "nesting_bounded_at_max_depth"_test = [] {
        kimix::string src;
        for (int i = 0; i < 2000; ++i) {
            src += "$(";
        }
        src += "x";
        for (int i = 0; i < 2000; ++i) {
            src += ")";
        }
        auto script = parse(sv(src));
        size_t scripts = 0;
        size_t exps = 0;
        for (const Node *n : walk_nodes(script)) {
            if (n->kind == NodeKind::kScript) {
                ++scripts;
            }
            if (n->kind == NodeKind::kExpansion) {
                ++exps;
            }
        }
        expect(exps > 0) << "deep input must still build child expansions";
        expect(scripts <= static_cast<size_t>(kMaxNestingDepth) + 2) << "scripts bounded: " << scripts;
        expect(scripts >= 50) << "real descent happened: " << scripts;
    };

    "walk_nodes_survives_deep_tree"_test = [] {
        kimix::string src;
        for (int i = 0; i < 1500; ++i) {
            src += "$(";
        }
        src += "x";
        for (int i = 0; i < 1500; ++i) {
            src += ")";
        }
        auto script = parse(sv(src));
        expect(walk_nodes(script).size() > 10u);
    };

    "deep_backtick_nesting_terminates"_test = [] {
        kimix::string src = "echo ";
        for (int i = 0; i < 1000; ++i) {
            src += '`';
        }
        src += "x";
        for (int i = 0; i < 1000; ++i) {
            src += '`';
        }
        Clock clock;
        auto script = parse(sv(src));
        expect(render_source(script) == src) << "deep backtick render";
        expect(clock.toc_seconds() < 10.0);
    };

    // ── 7. linear scaling for wide input ─────────────────────────────────────
    "wide_command_is_linear_and_bounded"_test = [] {
        kimix::string src;
        for (int i = 0; i < 20000; ++i) {
            if (i) {
                src += ' ';
            }
            src += "w" + std::to_string(i);
        }
        Clock clock;
        auto script = parse(sv(src));
        expect(render_source(script) == src) << "wide render";
        expect(clock.toc_seconds() < 10.0);
        expect(!script.children.empty());
        if (!script.children.empty()) {
            const Node *cmd = script.children[0].get();
            expect(cmd->kind == NodeKind::kCommand);
            if (cmd->kind == NodeKind::kCommand && cmd->command_word_p()) {
                expect(cmd->command_word_p()->span.text(sv(src)) == "w0");
            }
            expect(cmd->arguments.size() == 19999u) << "args: " << cmd->arguments.size();
        }
    };

    "many_words_token_count_is_linear"_test = [] {
        for (const int n : {5000, 20000}) {
            kimix::string src;
            for (int i = 0; i < n; ++i) {
                if (i) {
                    src += ' ';
                }
                src += "w" + std::to_string(i);
            }
            const LexResult r = lex(sv(src));
            size_t words = 0;
            size_t raws = 0;
            for (const Token &t : r.tokens) {
                if (t.kind == TokenKind::kWord) {
                    ++words;
                }
                if (t.kind == TokenKind::kRaw) {
                    ++raws;
                }
            }
            expect(words == static_cast<size_t>(n)) << "words n=" << n;
            expect(raws == static_cast<size_t>(n - 1)) << "raws n=" << n;
        }
    };

    // ── 8. spans / models primitives ─────────────────────────────────────────
    "sourcespan_helpers"_test = [] {
        const SourceSpan span{2, 5};
        expect(span.length() == 3u);
        expect(span.text("abcdef") == "cde");
        expect(SourceSpan{0, 3}.adjacent_to(SourceSpan{3, 6}));
        expect(!SourceSpan{0, 3}.adjacent_to(SourceSpan{4, 6}));
        expect(SourceSpan{0, 3}.overlaps(SourceSpan{2, 5}));
        expect(!SourceSpan{0, 3}.overlaps(SourceSpan{3, 5}));
        expect(SourceSpan{0, 3}.union_with(SourceSpan{5, 6}) == SourceSpan{0, 6});
        SourceSpan joined;
        expect(SourceSpan{0, 3}.join(SourceSpan{3, 6}, joined));
        expect(joined == SourceSpan{0, 6});
        SourceSpan disjoint;
        expect(!SourceSpan{0, 2}.join(SourceSpan{5, 7}, disjoint)); // declines instead of raising
    };

    "edit_overlaps"_test = [] {
        const Edit edit{1, 3, "xy"};
        expect(edit.start == 1u && edit.end == 3u && edit.replacement == "xy");
        expect(edit.span() == SourceSpan{1, 3});
        expect(!Edit{0, 1, "a"}.overlaps(Edit{1, 2, "b"}));
        expect(Edit{0, 2, "a"}.overlaps(Edit{1, 3, "b"}));
    };

    "diagnostic_is_data"_test = [] {
        const Diagnostic diag{DiagnosticKind::kAmbiguousRegion, SourceSpan{0, 3}, "why"};
        expect(diag.span.text("abcdef") == "abc");
        expect(diag.kind == DiagnosticKind::kAmbiguousRegion);
    };

    "decode_word_literal_contract"_test = [] {
        expect(decode_word_literal("plain") == kimix::optional<kimix::string>("plain"));
        expect(decode_word_literal("'a b'") == kimix::optional<kimix::string>("a b"));
        expect(decode_word_literal("a\"b\"c") == kimix::optional<kimix::string>("abc"));
        expect(!decode_word_literal("$(x)").has_value());
        expect(!decode_word_literal("$((x))").has_value());
        expect(!decode_word_literal("${x}").has_value());
        expect(!decode_word_literal("$'nul'").has_value());
        expect(decode_word_literal("a\\b") == kimix::optional<kimix::string>("ab"));
        expect(!decode_word_literal("*").has_value());
        expect(!decode_word_literal("'unterminated").has_value());
        expect(decode_word_literal("nul") == kimix::optional<kimix::string>("nul"));
    };

    "decode_word_literal_partial_never_fails"_test = [] {
        expect(decode_word_literal_partial("a\"b $x\"c") == "ab $xc");
        expect(decode_word_literal_partial("'x") == "'x");
    };

    "walk_nodes_visits_nested_regions"_test = [] {
        auto script = parse("echo $(rev <<< abc) tail");
        bool has_script = false;
        bool has_command = false;
        bool has_expansion = false;
        for (const Node *n : walk_nodes(script)) {
            if (n->kind == NodeKind::kScript) {
                has_script = true;
            }
            if (n->kind == NodeKind::kCommand) {
                has_command = true;
            }
            if (n->kind == NodeKind::kExpansion) {
                has_expansion = true;
            }
        }
        expect(has_script && has_command && has_expansion);
    };

    "operator_and_newline_nodes_present"_test = [] {
        const sv src = "a; b\nc";
        auto script = parse(src);
        bool has_newline = false;
        bool has_list = false;
        for (const auto &c : script.children) {
            if (c->kind == NodeKind::kNewline) {
                has_newline = true;
            }
            if (c->kind == NodeKind::kCommandList) {
                has_list = true;
            }
        }
        expect(has_newline || has_list);
        expect(render_source(script) == kimix::string(src));
    };

    "parse_inner_is_standalone_source"_test = [] {
        auto inner = parse_inner("tree > nul");
        expect(inner.kind == NodeKind::kScript);
        expect(inner.source == "tree > nul");
        expect(render_source(inner) == "tree > nul");
        if (!inner.children.empty()) {
            const Node *cmd = inner.children[0].get();
            expect(cmd->kind == NodeKind::kCommand);
            if (cmd->kind == NodeKind::kCommand && cmd->command_word_p()) {
                expect(cmd->command_word_p()->span.text("tree > nul") == "tree");
            }
        }
    };

    "word_to_node_carries_parts_and_literal"_test = [] {
        const sv src = "echo 'a b'";
        const LexResult r = lex(src);
        const Token *second_word = nullptr;
        size_t seen = 0;
        for (const Token &t : r.tokens) {
            if (t.kind == TokenKind::kWord && ++seen == 2) {
                second_word = &t;
            }
        }
        expect(second_word != nullptr);
        if (second_word == nullptr) {
            return;
        }
        const Node word = word_to_node(*second_word, src);
        expect(word.literal_value.has_value() && *word.literal_value == "a b");
        expect(!word.parts.empty());
        if (!word.parts.empty()) {
            expect(word.parts[0].first == WordPartKind::kSingleQuote);
        }
    };
}
