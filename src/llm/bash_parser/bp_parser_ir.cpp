/* bp_parser_ir.cpp — implementation of the tolerant shell parser (port of
 * kimi-agent `shell_compat/parser.py`).  See bp_parser_ir.h for the contract.
 *
 * Unity-build safe: every file-local helper is `ir_`-prefixed and the mutable
 * parser machinery (IrPart/IrSlot/IrTask/IrDoc/IrCtx) lives in an anonymous
 * namespace, so it cannot collide with sibling bash_parser translation units.
 * The shared inline helpers (bp_starts_with / bp_ends_with / bp_ascii_lower)
 * come from bp_core.h; no new starts_with/ends_with/ascii_lower is defined.
 *
 * No exceptions / RTTI.  Byte offsets throughout.
 */
#include "llm/bash_parser/bp_parser_ir.h"

#include <core/kimix_core.h>

#include <algorithm>
#include <utility>

#include "llm/bash_parser/bp_core.h"
#include "llm/bash_parser/bp_models.h"
#include "llm/bash_parser/bp_scanner.h"

namespace kimix::llm::bash_parser {
namespace src_llm_bash_parser_bp_parser_ir {

using Up = kimix::unique_ptr<Node>;
using sv = kimix::string_view;

// ── node factories (tagged-struct builders) ──────────────────────────────────

Up ir_raw(SourceSpan span, kimix::string reason) {
    Node nd;
    nd.kind = NodeKind::kRaw;
    nd.span = span;
    nd.reason = std::move(reason);
    return std::make_unique<Node>(std::move(nd));
}

Up ir_newline(SourceSpan span) {
    Node nd;
    nd.kind = NodeKind::kNewline;
    nd.span = span;
    return std::make_unique<Node>(std::move(nd));
}

// ── small string helpers (bounded to the region, never reusing global names) ──

bool ir_contains_nl(sv s) { return s.find('\n') != sv::npos; }

// Python `str.rstrip("\r")` over a span.
sv ir_rstrip_cr(sv s) {
    while (!s.empty() && s.back() == '\r') {
        s.remove_suffix(1);
    }
    return s;
}

// Python `str.lstrip("\t")` over a span.
sv ir_lstrip_tab(sv s) {
    while (!s.empty() && s.front() == '\t') {
        s.remove_prefix(1);
    }
    return s;
}

// `str.isdigit()` restricted to ASCII digits (the reference path is ASCII).
bool ir_all_digits(sv s) {
    if (s.empty()) {
        return false;
    }
    for (const char ch : s) {
        if (!is_ascii_digit(ch)) {
            return false;
        }
    }
    return true;
}

// Python `str.lstrip("0123456789")` over a span.
size_t ir_leading_digit_run(sv s) {
    size_t i = 0;
    while (i < s.size() && is_ascii_digit(s[i])) {
        ++i;
    }
    return i;
}

// ── module-level reserved-word sets (parser.py frozensets) ───────────────────

bool ir_in(const char *const arr[], size_t n, sv w) {
    for (size_t i = 0; i < n; ++i) {
        if (w == arr[i]) {
            return true;
        }
    }
    return false;
}

const char *const kReopeners[] = {"then", "do", "else", "elif", "in", "{"};
const char *const kClosers[] = {"fi", "done", "esac", "}"};
const char *const kCloserPop[] = {"fi", "done", "esac"}; // arg-position pop set
const char *const kCloserPopCmd[] = {"}", "fi", "done", "esac"}; // cmd-position pop
const char *const kBareKeywords[] = {
    "{", "}", "in", "then", "do", "else", "elif", "fi", "done", "esac", "if", "while", "until", "for", "case", "select", "time", "coproc", "function"};
const char *const kCompoundKwList[] = {"for", "select", "while", "until", "if"};

bool ir_in_reopeners(sv w) { return ir_in(kReopeners, 6, w); }
bool ir_in_closers(sv w) { return ir_in(kClosers, 4, w); }
bool ir_in_bare_keywords(sv w) { return ir_in(kBareKeywords, 19, w); }
bool ir_closer_pop_cmd(sv w) { return ir_in(kCloserPopCmd, 4, w); }
bool ir_closer_pop_arg(sv w) { return ir_in(kCloserPop, 3, w); }
bool ir_is_compound_keyword_open(sv w) { return ir_in(kCompoundKwList, 5, w); }

// The kMaxNestingDepth value rendered as text (diagnostic message only; no
// generic to_string for integers is available in kimix_core).
kimix::string ir_depth_str() { return kimix::string("1024"); }

// models.py `str(literal)` for the `reserved:{literal}` reason (None -> "None").
kimix::string ir_py_str_opt(const kimix::optional<kimix::string> &o) {
    return o.has_value() ? *o : kimix::string("None");
}

// ── _Part: mutable accumulation of one simple command ────────────────────────

struct IrPart {
    size_t start = 0;
    size_t end = 0;
    kimix::vector<Up> assignments;
    Up command_word;
    kimix::vector<Up> arguments;
    kimix::vector<Up> redirections;
    kimix::vector<Up> heredocs;
    kimix::vector<Up> embedded;
    kimix::vector<Up> expansions;
    kimix::optional<kimix::string> wrapper;
    kimix::optional<kimix::string> wrapper_hint;
    kimix::optional<kimix::string> command_value;
    int32_t reparse_remaining = 0;
    bool shell_c_pending = false;
    bool shell_exec_operand = false;
    bool array_data = false;

    explicit IrPart(size_t at) : start(at), end(at) {}
};

// ── _Slot: install point for a parsed child ScriptNode ───────────────────────

enum class IrSlotKind { kExpansion, kEmbedded, kGroupBody };

struct IrSlot {
    IrSlotKind kind = IrSlotKind::kExpansion;
    Node *owner = nullptr;
};

void ir_slot_fill(const IrSlot &slot, Node &&script) {
    switch (slot.kind) {
        case IrSlotKind::kExpansion:
            slot.owner->child = std::make_unique<Node>(std::move(script));
            break;
        case IrSlotKind::kEmbedded:
            slot.owner->inner = std::make_unique<Node>(std::move(script));
            break;
        case IrSlotKind::kGroupBody:
            // the placeholder ScriptNode IS the compound body part: fill in place.
            slot.owner->children = std::move(script.children);
            slot.owner->diagnostics = std::move(script.diagnostics);
            break;
    }
}

// ── _Task: queued nested-context job ─────────────────────────────────────────

struct IrTask {
    IrSlot slot;
    SourceSpan region;
    int32_t depth = 0;
    bool standalone = false;
    kimix::string source_text;
};

// ── _Doc: pending heredoc document ───────────────────────────────────────────

struct IrDoc {
    SourceSpan op_span;
    kimix::string op_text;
    Node *delimiter_word = nullptr; // owned by the HeredocNode (non-owning alias)
    kimix::optional<kimix::string> delimiter;
    bool strip_tabs = false;
    bool expands = true;
    Node *node = nullptr; // owned in the command's heredocs (non-owning alias)
};

// ── module helpers ported from parser.py top-level functions ─────────────────

Up ir_word_from_token(const Token &token, sv source) {
    return std::make_unique<Node>(word_to_node(token, source));
}

bool ir_redirection_truncates(sv op_core, const Node *target) {
    if (is_truncating_redirect_operator(op_core)) {
        return true;
    }
    if (op_core == ">&") {
        if (target != nullptr) {
            const auto value = target->literal_value;
            if (value.has_value() && ir_all_digits(*value)) {
                return false; // fd dup never creates a file
            }
            return true;
        }
        return false;
    }
    return false;
}

bool ir_looks_whole_quoted(const Node &word) {
    if (!word.literal_value.has_value() || word.parts.empty()) {
        return false;
    }
    const WordPartKind kind = word.parts[0].first;
    if (kind != WordPartKind::kSingleQuote && kind != WordPartKind::kDoubleQuote && kind != WordPartKind::kAnsiCQuote && kind != WordPartKind::kLocaleQuote) {
        return false;
    }
    return word.parts.size() == 1 && word.parts[0].second == word.span;
}

bool ir_spans_tile(SourceSpan span, kimix::vector<SourceSpan> children) {
    kimix::sort(children.begin(), children.end(), [](const SourceSpan &a, const SourceSpan &b) { return a.start < b.start; });
    size_t cursor = span.start;
    for (const SourceSpan &child : children) {
        if (child.start < cursor) {
            return false;
        }
        if (child.end > span.end) {
            return false;
        }
        cursor = child.end;
    }
    return true;
}

bool ir_tiles(const Node &node) {
    switch (node.kind) {
        case NodeKind::kCommand: {
            kimix::vector<SourceSpan> spans;
            for (const Up &a : node.assignments) {
                spans.push_back(a->span);
            }
            if (node.command_word) {
                spans.push_back(node.command_word->span);
            }
            for (const Up &a : node.arguments) {
                spans.push_back(a->span);
            }
            for (const Up &r : node.redirections) {
                spans.push_back(r->span);
            }
            for (const Up &h : node.heredocs) {
                spans.push_back(h->span);
            }
            for (const Up &e : node.embedded) {
                spans.push_back(e->span);
            }
            return ir_spans_tile(node.span, std::move(spans));
        }
        case NodeKind::kPipeline: {
            kimix::vector<SourceSpan> spans;
            for (const Up &c : node.commands) {
                spans.push_back(c->span);
            }
            return ir_spans_tile(node.span, std::move(spans));
        }
        case NodeKind::kCommandList: {
            kimix::vector<SourceSpan> spans;
            for (const Up &c : node.children) {
                spans.push_back(c->span);
            }
            return ir_spans_tile(node.span, std::move(spans));
        }
        case NodeKind::kRedirection: {
            if (!node.target) {
                return true;
            }
            return node.target->span.start == node.operator_span.end && (node.span.end == node.target->span.end);
        }
        case NodeKind::kEmbedded:
            return !node.owner_word || node.owner_word->span == node.span;
        case NodeKind::kCompound:
            return true;
        default:
            return true;
    }
}

Up ir_make_command(SourceSpan span, IrPart &&p) {
    Node nd;
    nd.kind = NodeKind::kCommand;
    nd.span = span;
    nd.assignments = std::move(p.assignments);
    nd.command_word = std::move(p.command_word);
    nd.arguments = std::move(p.arguments);
    nd.redirections = std::move(p.redirections);
    nd.heredocs = std::move(p.heredocs);
    nd.embedded = std::move(p.embedded);
    nd.expansions = std::move(p.expansions);
    nd.wrapper = std::move(p.wrapper);
    nd.wrapper_hint = std::move(p.wrapper_hint);
    return std::make_unique<Node>(std::move(nd));
}

// ── _Ctx: segmentation state for ONE command context ─────────────────────────

class IrCtx {
public:
    IrCtx(sv source, SourceSpan region, const kimix::vector<Token> &tokens, int32_t depth, kimix::vector<Diagnostic> diagnostics)
        : source_(source), region_(region), tokens_(&tokens), depth_(depth), diagnostics_(std::move(diagnostics)) {}

    // run(): the flat segmentation driver.  Returns the segment list.
    kimix::vector<Up> run() {
        const auto &tokens = *tokens_;
        size_t index = 0;
        while (index < tokens.size()) {
            const Token &token = tokens[index];
            ++index;
            index_ = index;
            TokenKind kind = token.kind;
            if (kind == TokenKind::kRaw) {
                continue;
            }
            if (!pending_docs_.empty() && doc_boundary(token, index)) {
                index = claim_docs(token, index);
                if (index >= tokens.size()) {
                    break;
                }
                const Token &t2 = tokens[index - 1];
                kind = t2.kind;
                dispatch(kind, t2, index);
                continue;
            }
            dispatch(kind, token, index);
        }
        flush(region_.end);
        if (!pending_docs_.empty()) {
            claim_docs_at(region_.end, tokens.size());
        }
        return std::move(segments_);
    }

    kimix::vector<Diagnostic> take_diagnostics() { return std::move(diagnostics_); }
    kimix::vector<IrTask> take_tasks() { return std::move(tasks_); }

private:
    // The post-claim dispatch (mirrors the run-loop if-chain for `token`).
    void dispatch(TokenKind kind, const Token &token, size_t &index) {
        if (kind == TokenKind::kNewline) {
            flush(token.span.start);
            emit(ir_newline(token.span));
            command_expected_ = true;
            return;
        }
        if (kind == TokenKind::kComment) {
            flush(token.span.start);
            emit(ir_raw(token.span, "comment"));
            return;
        }
        if (kind == TokenKind::kRedirection) {
            index = handle_redirection(token, index);
            return;
        }
        if (kind == TokenKind::kOperator) {
            index = handle_operator(token, index);
            return;
        }
        index = handle_word(token, index);
    }

    // ── segment emission ────────────────────────────────────────────────────
    void emit(Up node) { segments_.push_back(std::move(node)); }

    void flush(size_t until) {
        if (!parts_.has_value()) {
            return;
        }
        IrPart p = std::move(*parts_);
        parts_.reset();
        if (p.assignments.empty() && !p.command_word && p.arguments.empty() && p.redirections.empty() && p.heredocs.empty() && p.embedded.empty() && p.expansions.empty() && !p.array_data) {
            return;
        }
        const size_t end = until > p.end ? until : p.end;
        emit_command(std::move(p), end);
    }

    void emit_command(IrPart &&p, size_t end) {
        SourceSpan span{p.start, end};
        Up cmd = ir_make_command(span, std::move(p));
        if (ir_tiles(*cmd)) {
            emit(std::move(cmd));
        } else {
            emit(ir_raw(span, "non-tiling command shape"));
        }
    }

    // ── heredoc boundary ─────────────────────────────────────────────────────
    bool doc_boundary(const Token &token, size_t index) {
        if (token.kind == TokenKind::kNewline) {
            return true;
        }
        const auto &tokens = *tokens_;
        size_t prev_end = region_.start;
        for (int64_t j = static_cast<int64_t>(index) - 2; j >= 0; --j) {
            const Token &prev = tokens[static_cast<size_t>(j)];
            if (prev.kind == TokenKind::kRaw) {
                continue;
            }
            prev_end = prev.span.end;
            break;
        }
        return ir_contains_nl(source_.substr(prev_end, token.span.start - prev_end));
    }

    // ── operators ────────────────────────────────────────────────────────────
    size_t handle_operator(const Token &token, size_t index) {
        const sv op = token.text;
        if (op == "(") {
            flush(token.span.start);
            index = parse_group(token, index);
            command_expected_ = true;
            return index;
        }
        if (op == ")") {
            flush(token.span.start);
            if (case_state_.has_value() && *case_state_ == "patterns") {
                case_state_ = kimix::string("body");
                emit(ir_raw(token.span, "case-body-start"));
                command_expected_ = true;
                return index;
            }
            emit(ir_raw(token.span, "unexpected )"));
            diagnostics_.push_back(Diagnostic{DiagnosticKind::kUnexpectedToken, token.span, "unexpected )"});
            command_expected_ = false;
            return index;
        }
        if (op == ";;" || op == ";&" || op == ";;&") {
            flush(token.span.start);
            if (case_state_.has_value()) {
                emit(ir_raw(token.span, "case-arm-end:" + kimix::string(op)));
                case_state_ = kimix::string("patterns");
                command_expected_ = true;
            } else {
                emit(ir_raw(token.span, "unexpected " + kimix::string(op)));
                diagnostics_.push_back(Diagnostic{DiagnosticKind::kUnexpectedToken, token.span, "unexpected " + kimix::string(op) + " outside a case construct"});
                command_expected_ = true;
            }
            return index;
        }
        flush(token.span.start);
        emit(ir_raw(token.span, "op:" + kimix::string(op)));
        command_expected_ = true;
        return index;
    }

    size_t parse_group(const Token &token, size_t index) {
        const int64_t close = find_matching_paren(source_, static_cast<int64_t>(token.span.start));
        if (close == -1 || static_cast<size_t>(close) >= region_.end) {
            SourceSpan span{token.span.start, region_.end};
            diagnostics_.push_back(Diagnostic{DiagnosticKind::kUnterminatedSubst, span, "unmatched ( in command position"});
            emit(ir_raw(span, "unmatched ("));
            return tokens_->size();
        }
        const size_t close_u = static_cast<size_t>(close);
        SourceSpan span{token.span.start, close_u + 1};
        SourceSpan body_region{token.span.end, close_u};
        if (depth_ + 1 >= kMaxNestingDepth) {
            diagnostics_.push_back(Diagnostic{DiagnosticKind::kDepthLimit, span, kimix::string("nesting depth limit ") + ir_depth_str() + kimix::string(" reached; group preserved")});
            emit(ir_raw(span, "depth limit: group"));
        } else {
            // placeholder ScriptNode owned by the compound body part.
            Node placeholder;
            placeholder.kind = NodeKind::kScript;
            placeholder.span = body_region;
            placeholder.region = body_region;
            placeholder.source = kimix::string(source_);
            Up holder = std::make_unique<Node>(std::move(placeholder));
            Node *ph_ptr = holder.get();
            Node compound;
            compound.kind = NodeKind::kCompound;
            compound.span = span;
            compound.compound_kind = "subshell";
            compound.named_parts.emplace_back(kimix::string("body"), std::move(holder));
            emit(std::make_unique<Node>(std::move(compound)));
            tasks_.push_back(IrTask{IrSlot{IrSlotKind::kGroupBody, ph_ptr}, body_region, depth_ + 1, false, kimix::string(source_)});
        }
        while (index < tokens_->size() && (*tokens_)[index].span.start <= close_u) {
            ++index;
        }
        return index;
    }

    // ── redirections / heredocs ──────────────────────────────────────────────
    size_t handle_redirection(const Token &token, size_t index) {
        IrPart &p = ensure(token.span.start);
        const sv text = token.span.text(source_);
        const size_t digit_run = ir_leading_digit_run(text);
        const sv op_core = text.substr(digit_run);
        const size_t op_start = token.span.start + digit_run;
        SourceSpan op_span{op_start, token.span.end};
        p.end = p.end > op_span.end ? p.end : op_span.end;
        const Token *target = peek_target(token, index);
        if (is_heredoc_operator(op_core)) {
            return record_heredoc(p, op_core, op_span, token, target, target_index(token, index));
        }
        if (op_core == "<<<") {
            if (target != nullptr) {
                Up word = ir_word_from_token(*target, source_);
                SourceSpan span{token.span.start, word->span.end};
                Node node;
                node.kind = NodeKind::kRedirection;
                node.span = span;
                node.operator_text = kimix::string(text);
                node.operator_span = op_span;
                node.fd = token.fd;
                node.target = std::move(word);
                node.can_truncate = false;
                p.redirections.push_back(std::make_unique<Node>(std::move(node)));
                p.end = p.end > span.end ? p.end : span.end;
                return target_index(token, index);
            }
            Node node;
            node.kind = NodeKind::kRedirection;
            node.span = token.span;
            node.operator_text = kimix::string(text);
            node.operator_span = op_span;
            node.fd = token.fd;
            node.can_truncate = false;
            p.redirections.push_back(std::make_unique<Node>(std::move(node)));
            return index;
        }
        Up word_node;
        if (target != nullptr) {
            word_node = ir_word_from_token(*target, source_);
            collect_regions(*target, p);
        }
        SourceSpan span = word_node ? SourceSpan{token.span.start, word_node->span.end} : token.span;
        const bool can_truncate = ir_redirection_truncates(op_core, word_node.get());
        Node node;
        node.kind = NodeKind::kRedirection;
        node.span = span;
        node.operator_text = kimix::string(text);
        node.operator_span = op_span;
        node.fd = token.fd;
        node.target = std::move(word_node);
        node.can_truncate = can_truncate;
        p.redirections.push_back(std::make_unique<Node>(std::move(node)));
        p.end = p.end > span.end ? p.end : span.end;
        if (target != nullptr) {
            index = target_index(token, index);
        }
        return index;
    }

    const Token *peek_target(const Token &token, size_t index) {
        const auto &tokens = *tokens_;
        if (index >= tokens.size()) {
            return nullptr;
        }
        const Token *candidate = &tokens[index];
        if (candidate->kind == TokenKind::kRaw) {
            if (index + 1 >= tokens.size()) {
                return nullptr;
            }
            candidate = &tokens[index + 1];
        }
        if (candidate->kind != TokenKind::kWord) {
            return nullptr;
        }
        if (ir_contains_nl(source_.substr(token.span.end, candidate->span.start - token.span.end))) {
            return nullptr;
        }
        return candidate;
    }

    size_t target_index(const Token &token, size_t index) {
        const auto &tokens = *tokens_;
        const Token *target = peek_target(token, index);
        if (target == nullptr) {
            return index;
        }
        while (index < tokens.size() && &tokens[index] != target) {
            ++index;
        }
        return index + 1;
    }

    size_t record_heredoc(IrPart &p, sv op_core, SourceSpan op_span, const Token &token, const Token *delimiter_token, size_t index) {
        Up delimiter_word;
        kimix::optional<kimix::string> delimiter;
        bool expands = true;
        size_t line_end = op_span.end;
        if (delimiter_token != nullptr) {
            auto scanned = Scanner::heredoc_delimiter(delimiter_token->span.text(source_));
            if (!scanned.has_value()) {
                diagnostics_.push_back(Diagnostic{DiagnosticKind::kAmbiguousRegion, delimiter_token->span, "malformed heredoc delimiter: no body claimed"});
                p.arguments.push_back(ir_word_from_token(*delimiter_token, source_));
                line_end = delimiter_token->span.end;
            } else {
                delimiter = scanned->delimiter;
                expands = scanned->expands;
                delimiter_word = ir_word_from_token(*delimiter_token, source_);
                line_end = delimiter_token->span.end;
            }
        }
        Node node;
        node.kind = NodeKind::kHeredoc;
        node.span = SourceSpan{token.span.start, line_end};
        node.operator_text = kimix::string(op_core);
        node.operator_span = op_span;
        node.delimiter_word = std::move(delimiter_word);
        node.delimiter_value = delimiter;
        node.strip_tabs = (op_core == "<<-");
        node.expands = expands;
        node.body_start = line_end;
        Up holder = std::make_unique<Node>(std::move(node));
        Node *node_ptr = holder.get();
        Node *delim_ptr = node_ptr->delimiter_word.get();
        p.heredocs.push_back(std::move(holder));
        pending_docs_.push_back(IrDoc{op_span, kimix::string(op_core), delim_ptr, delimiter, op_core == "<<-", expands, node_ptr});
        p.end = p.end > line_end ? p.end : line_end;
        return index;
    }

    size_t claim_docs(const Token &token, size_t index) {
        size_t body_start;
        size_t next_index;
        if (token.kind == TokenKind::kNewline) {
            flush(token.span.start);
            body_start = token.span.end;
            next_index = index;
        } else {
            flush(token.span.start);
            body_start = token.span.start;
            next_index = index - 1;
        }
        return claim_docs_at(body_start, next_index);
    }

    size_t claim_docs_at(size_t body_start, size_t next_index) {
        kimix::vector<IrDoc> docs = std::move(pending_docs_);
        pending_docs_.clear();
        const auto n = region_.end;
        size_t end_for_tokens = body_start;
        for (IrDoc &doc : docs) {
            const size_t claimed = claim_one_doc(doc, body_start, n);
            if (claimed > end_for_tokens) {
                end_for_tokens = claimed;
            }
        }
        const auto &tokens = *tokens_;
        while (next_index < tokens.size() && tokens[next_index].span.start < end_for_tokens) {
            ++next_index;
        }
        index_ = next_index;
        return next_index;
    }

    size_t claim_one_doc(IrDoc &doc, size_t body_start, size_t n) {
        kimix::optional<size_t> terminator_start;
        kimix::optional<size_t> terminator_end;
        if (doc.delimiter.has_value()) {
            size_t scan = body_start;
            while (scan < n) {
                const size_t nl_raw = source_.find('\n', scan);
                const bool found = (nl_raw != sv::npos && nl_raw < n);
                const size_t line_end = found ? nl_raw : n;
                sv line = source_.substr(scan, line_end - scan);
                line = ir_rstrip_cr(line);
                const sv compare = doc.strip_tabs ? ir_lstrip_tab(line) : line;
                if (compare == *doc.delimiter) {
                    terminator_start = scan;
                    terminator_end = line_end;
                    break;
                }
                if (!found) {
                    break;
                }
                scan = nl_raw + 1;
            }
        }
        if (!terminator_start.has_value()) {
            doc.node->body_start = body_start;
            doc.node->body_end = n;
            diagnostics_.push_back(Diagnostic{DiagnosticKind::kUnterminatedHeredoc, SourceSpan{body_start, n}, "unterminated heredoc body"});
            emit(ir_raw(SourceSpan{body_start, n}, "heredoc-body: unterminated"));
            return n;
        }
        const size_t t_end = terminator_end.value();
        const size_t nl_raw = source_.find('\n', t_end);
        const size_t after = (nl_raw != sv::npos && nl_raw < n) ? nl_raw + 1 : n;
        doc.node->body_start = body_start;
        doc.node->body_end = t_end;
        doc.node->terminator_span = SourceSpan{terminator_start.value(), t_end};
        if (after > body_start) {
            emit(ir_raw(SourceSpan{body_start, after}, "heredoc-body"));
        }
        return after;
    }

    // ── words ────────────────────────────────────────────────────────────────
    size_t handle_word(const Token &token, size_t index) {
        const sv raw = token.span.text(source_);
        Up word = ir_word_from_token(token, source_);
        const kimix::optional<kimix::string> literal = word->literal_value;
        const kimix::optional<kimix::string> keyword = keyword_of(raw, literal);
        const bool kw_present = keyword.has_value();

        if (case_state_.has_value() && *case_state_ == "word") {
            case_state_ = kimix::string("await-in");
            flush(token.span.end);
            emit(std::move(word));
            return index;
        }
        if (case_state_.has_value() && *case_state_ == "await-in" && kw_present && *keyword == "in") {
            case_state_ = kimix::string("patterns");
            flush(token.span.end);
            emit(std::move(word));
            return index;
        }
        if (case_state_.has_value() && *case_state_ == "patterns") {
            emit(std::move(word));
            return index;
        }

        IrPart &p = ensure(token.span.start);
        collect_regions(token, p);

        if (command_expected_ && !p.command_word) {
            if (kw_present && (ir_in_reopeners(*keyword) || ir_in_closers(*keyword))) {
                flush(token.span.end);
                emit(ir_raw(token.span, "reserved:" + ir_py_str_opt(literal)));
                command_expected_ = ir_in_reopeners(*keyword);
                if (ir_closer_pop_cmd(*keyword)) {
                    if (!compound_state_.empty()) {
                        compound_state_.pop_back();
                    }
                    command_expected_ = false;
                }
                return index;
            }
            if (classify_assignment(raw).has_value()) {
                parts_->assignments.push_back(std::move(word));
                parts_->end = parts_->end > token.span.end ? parts_->end : token.span.end;
                return index;
            }
            parts_->command_word = std::move(word);
            parts_->command_value = literal;
            record_wrapper_hints(*parts_, literal);
            parts_->end = parts_->end > token.span.end ? parts_->end : token.span.end;
            command_expected_ = false;
            if (kw_present && *keyword == "case") {
                case_state_ = kimix::string("word");
            } else if (kw_present && ir_is_compound_keyword_open(*keyword)) {
                compound_state_.push_back(*keyword);
            }
            return index;
        }

        // argument/data position
        if (kw_present && ir_in_reopeners(*keyword)) {
            flush(token.span.end);
            emit(ir_raw(token.span, "reserved:" + *keyword));
            command_expected_ = true;
            return index;
        }
        if (kw_present && ir_in_closers(*keyword)) {
            flush(token.span.start);
            emit(ir_raw(token.span, "reserved:" + *keyword));
            if (!compound_state_.empty() && ir_closer_pop_arg(*keyword)) {
                compound_state_.pop_back();
            }
            command_expected_ = false;
            return index;
        }
        IrPart &q = ensure(token.span.start);
        if (q.command_value.has_value() && is_declaration_builtin(*q.command_value) && classify_assignment(raw).has_value()) {
            q.assignments.push_back(std::move(word));
            q.end = q.end > token.span.end ? q.end : token.span.end;
            return index;
        }
        if (literal.has_value() && is_shell_c_option(*literal) && (q.command_value.has_value() && (in_shell_wrappers(*q.command_value) || q.shell_exec_operand))) {
            q.shell_c_pending = true;
            q.arguments.push_back(std::move(word));
            q.end = q.end > token.span.end ? q.end : token.span.end;
            return index;
        }
        if ((q.command_value.has_value() && in_exec_operand_commands(*q.command_value)) || q.shell_exec_operand) {
            if (literal.has_value() && in_shell_wrappers(*literal)) {
                q.shell_exec_operand = true;
                q.arguments.push_back(std::move(word));
                q.end = q.end > token.span.end ? q.end : token.span.end;
                return index;
            }
        }
        if (q.shell_c_pending && ir_looks_whole_quoted(*word)) {
            q.shell_c_pending = false;
            q.embedded.push_back(make_embedded(std::move(word)));
            q.end = q.end > token.span.end ? q.end : token.span.end;
            return index;
        }
        if (q.reparse_remaining != 0 && ir_looks_whole_quoted(*word)) {
            q.embedded.push_back(make_embedded(std::move(word)));
            if (q.reparse_remaining > 0) {
                --q.reparse_remaining;
            }
            q.end = q.end > token.span.end ? q.end : token.span.end;
            return index;
        }
        q.arguments.push_back(std::move(word));
        q.end = q.end > token.span.end ? q.end : token.span.end;
        return index;
    }

    IrPart &ensure(size_t start) {
        if (!parts_.has_value()) {
            parts_.emplace(start);
        }
        return *parts_;
    }

    void record_wrapper_hints(IrPart &p, const kimix::optional<kimix::string> &literal) {
        if (!literal.has_value()) {
            return;
        }
        const sv lit = *literal;
        if (in_shell_wrappers(lit)) {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("shell");
        } else if (in_command_wrappers(lit)) {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("command");
        }
        if (in_same_shell_wrappers(lit) && !p.wrapper.has_value()) {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("same-shell");
        }
        const auto reparse_count = reparse_builtin_count(lit);
        if (reparse_count.has_value()) {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("reparse");
            p.reparse_remaining = reparse_count.value();
        }
        if (lit == "watch") {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("same-shell");
            p.reparse_remaining = 1;
        }
        if (in_exec_operand_commands(lit) && !p.wrapper.has_value()) {
            p.wrapper = lit;
            p.wrapper_hint = kimix::string("exec-operand");
        }
    }

    void collect_regions(const Token &token, IrPart &p) {
        for (const WordPart &wp : token.parts) {
            if (!is_executable_region(wp.kind) || !wp.inner.has_value()) {
                continue;
            }
            SourceSpan region{wp.inner->first, wp.inner->second};
            if (depth_ + 1 >= kMaxNestingDepth) {
                diagnostics_.push_back(Diagnostic{DiagnosticKind::kDepthLimit, wp.span, kimix::string("nesting depth limit ") + ir_depth_str() + kimix::string(" reached; region preserved as data")});
                continue;
            }
            Node expansion;
            expansion.kind = NodeKind::kExpansion;
            expansion.span = wp.span;
            expansion.expansion_kind = wp.kind;
            Up holder = std::make_unique<Node>(std::move(expansion));
            Node *exp_ptr = holder.get();
            p.expansions.push_back(std::move(holder));
            tasks_.push_back(IrTask{IrSlot{IrSlotKind::kExpansion, exp_ptr}, region, depth_ + 1, false, kimix::string(source_)});
        }
    }

    Up make_embedded(Up word) {
        const WordPartKind kind = word->parts[0].first;
        const kimix::string script = word->literal_value.has_value() ? *word->literal_value : kimix::string();
        Node node;
        node.kind = NodeKind::kEmbedded;
        node.span = word->span;
        node.owner_word = std::move(word);
        node.quote_kind = kind;
        Up holder = std::make_unique<Node>(std::move(node));
        Node *emb_ptr = holder.get();
        if (!script.empty()) {
            SourceSpan region{0, script.size()};
            if (depth_ + 1 < kMaxNestingDepth) {
                Node placeholder;
                placeholder.kind = NodeKind::kScript;
                placeholder.span = region;
                placeholder.region = region;
                placeholder.source = script;
                emb_ptr->inner = std::make_unique<Node>(std::move(placeholder));
                tasks_.push_back(IrTask{IrSlot{IrSlotKind::kEmbedded, emb_ptr}, region, depth_ + 1, true, script});
            } else {
                diagnostics_.push_back(Diagnostic{DiagnosticKind::kDepthLimit, emb_ptr->span, "embedded script not descended: depth limit"});
            }
        }
        return holder;
    }

    sv source_;
    SourceSpan region_;
    const kimix::vector<Token> *tokens_ = nullptr;
    int32_t depth_ = 0;
    kimix::vector<Diagnostic> diagnostics_;
    kimix::vector<Up> segments_;
    kimix::optional<IrPart> parts_;
    kimix::vector<IrDoc> pending_docs_;
    bool command_expected_ = true;
    kimix::optional<kimix::string> case_state_;
    kimix::vector<kimix::string> compound_state_;
    kimix::vector<IrTask> tasks_;
    size_t index_ = 0;
};

// ── _fold_segments: fold term/operator runs into pipeline / list nodes ────────

kimix::vector<Up> ir_fold_segments(kimix::vector<Up> &segments) {
    kimix::vector<Up> folded;
    kimix::vector<Up> run;
    kimix::vector<kimix::string> ops;

    auto flush_run = [&]() {
        if (run.empty()) {
            return;
        }
        if (run.size() == 1) {
            folded.push_back(std::move(run[0]));
        } else {
            const size_t start = run.front()->span.start;
            const size_t end = run.back()->span.end;
            bool all_pipeline = true;
            for (const kimix::string &op : ops) {
                if (!is_pipeline_operator(op)) {
                    all_pipeline = false;
                    break;
                }
            }
            Node group;
            group.span = SourceSpan{start, end};
            group.operators = ops;
            if (all_pipeline) {
                group.kind = NodeKind::kPipeline;
                group.commands = std::move(run);
            } else {
                group.kind = NodeKind::kCommandList;
                group.children = std::move(run);
            }
            folded.push_back(std::make_unique<Node>(std::move(group)));
        }
        run.clear();
        ops.clear();
    };

    const size_t n = segments.size();
    size_t i = 0;
    while (i < n) {
        const Node &seg = *segments[i];
        if (seg.kind == NodeKind::kRaw && bp_starts_with(seg.reason, "op:")) {
            const kimix::string op = seg.reason.substr(3);
            if (!run.empty() && i + 1 < n && segments[i + 1]->kind != NodeKind::kRaw) {
                ops.push_back(op);
                ++i;
                continue;
            }
            flush_run();
            folded.push_back(std::move(segments[i]));
            ++i;
            continue;
        }
        if (seg.kind == NodeKind::kRaw && bp_starts_with(seg.reason, "case-")) {
            flush_run();
            folded.push_back(std::move(segments[i]));
            ++i;
            continue;
        }
        if (seg.kind == NodeKind::kNewline) {
            flush_run();
            folded.push_back(std::move(segments[i]));
            ++i;
            continue;
        }
        if (seg.kind == NodeKind::kRaw &&
            (bp_starts_with(seg.reason, "comment") || bp_starts_with(seg.reason, "heredoc-body") || bp_starts_with(seg.reason, "reserved:") || bp_starts_with(seg.reason, "unexpected") || bp_starts_with(seg.reason, "unmatched") || bp_starts_with(seg.reason, "depth limit") || bp_starts_with(seg.reason, "non-tiling"))) {
            flush_run();
            folded.push_back(std::move(segments[i]));
            ++i;
            continue;
        }
        // a term segment
        run.push_back(std::move(segments[i]));
        ++i;
    }
    flush_run();
    return folded;
}

// ── _build_context / _drain_tasks ────────────────────────────────────────────

void ir_build_context(sv text, SourceSpan region, const kimix::vector<Token> &tokens, int32_t depth, kimix::vector<Diagnostic> diag_in, Node &out_script, kimix::vector<IrTask> &out_tasks) {
    IrCtx ctx(text, region, tokens, depth, std::move(diag_in));
    kimix::vector<Up> segments = ctx.run();
    kimix::vector<Up> children = ir_fold_segments(segments);

    Node script;
    script.kind = NodeKind::kScript;
    script.span = region;
    script.source = kimix::string(text);
    script.region = region;
    script.children = std::move(children);
    script.diagnostics = ctx.take_diagnostics();
    out_script = std::move(script);
    out_tasks = ctx.take_tasks();
}

Node ir_empty_script(SourceSpan region, kimix::string source_text) {
    Node nd;
    nd.kind = NodeKind::kScript;
    nd.span = region;
    nd.region = region;
    nd.source = std::move(source_text);
    return nd;
}

void ir_drain_tasks(const kimix::string &root_source, kimix::vector<IrTask> &tasks) {
    kimix::deque<IrTask> queue;
    for (IrTask &t : tasks) {
        queue.push_back(std::move(t));
    }
    while (!queue.empty()) {
        IrTask task = std::move(queue.front());
        queue.pop_front();
        const SourceSpan region = task.region;
        if (region.length() == 0) {
            Node empty = ir_empty_script(region, task.source_text);
            ir_slot_fill(task.slot, std::move(empty));
            continue;
        }
        const sv text = task.standalone ? sv(task.source_text) : sv(root_source);
        LexResult result = lex(text, region.start, region.end);
        kimix::vector<IrTask> child_tasks;
        Node script;
        ir_build_context(text, region, result.tokens, task.depth, std::move(result.diagnostics), script, child_tasks);
        ir_slot_fill(task.slot, std::move(script));
        for (IrTask &ct : child_tasks) {
            queue.push_back(std::move(ct));
        }
    }
}

} // namespace src_llm_bash_parser_bp_parser_ir
using namespace src_llm_bash_parser_bp_parser_ir;

// ── public surface (parser.py module functions) ──────────────────────────────

bool is_command_position_operator(kimix::string_view op) {
    static const char *const arr[] = {"|", "|&", "&&", "||", ";", ";&", ";;&", "("};
    return ir_in(arr, 8, op);
}

bool is_declaration_builtin(kimix::string_view word) {
    static const char *const arr[] = {"declare", "typeset", "local", "export", "readonly"};
    return ir_in(arr, 5, word);
}

bool is_pipeline_operator(kimix::string_view op) {
    return op == "|" || op == "|&";
}

kimix::optional<kimix::string> classify_assignment(kimix::string_view raw_word) {
    // _ASSIGNMENT_RE = ^[A-Za-z_][A-Za-z0-9_]*(\+)?=  -> captured name (the
    // `+` kept; only the `=` is dropped): raw[:match.end()-1].
    const size_t n = raw_word.size();
    size_t i = 0;
    if (i >= n) {
        return std::nullopt;
    }
    const char c0 = raw_word[i];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) {
        return std::nullopt;
    }
    ++i;
    while (i < n) {
        const char c = raw_word[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            ++i;
        } else {
            break;
        }
    }
    if (i < n && raw_word[i] == '+') {
        ++i;
    }
    if (i >= n || raw_word[i] != '=') {
        return std::nullopt;
    }
    // match.end() == i+1; return raw[: (i+1) - 1] == raw[:i].
    return kimix::string(raw_word.substr(0, i));
}

Node word_to_node(const Token &token, kimix::string_view source) {
    const sv raw = token.span.text(source);
    Node nd;
    nd.kind = NodeKind::kWord;
    nd.span = token.span;
    nd.parts.reserve(token.parts.size());
    for (const WordPart &wp : token.parts) {
        nd.parts.emplace_back(wp.kind, wp.span);
    }
    nd.literal_value = decode_word_literal(raw);
    return nd;
}

kimix::optional<bool> is_effective_nul_target(const Node &word, kimix::string_view source) {
    const sv raw = word.span.text(source);
    if (raw.find('$') != sv::npos && !(bp_starts_with(raw, "$'") || bp_starts_with(raw, "$\""))) {
        return std::nullopt;
    }
    if (raw.find('`') != sv::npos) {
        return std::nullopt;
    }
    if (!word.literal_value.has_value()) {
        return std::nullopt;
    }
    return bp_ascii_lower(*word.literal_value) == "nul";
}

kimix::optional<kimix::string> keyword_of(kimix::string_view raw, const kimix::optional<kimix::string> &literal) {
    if (literal.has_value()) {
        if (*literal == raw) {
            return literal;
        }
        return std::nullopt;
    }
    if (ir_in_bare_keywords(raw)) {
        return kimix::string(raw);
    }
    return std::nullopt;
}

Node parse(kimix::string_view source) {
    if (source.empty()) {
        return ir_empty_script(SourceSpan{0, 0}, kimix::string());
    }
    const kimix::string owned(source);
    const sv root = owned;
    const SourceSpan region{0, owned.size()};
    LexResult result = lex(root);
    kimix::vector<IrTask> tasks;
    Node script;
    ir_build_context(root, region, result.tokens, 0, std::move(result.diagnostics), script, tasks);
    ir_drain_tasks(owned, tasks);
    return script;
}

Node parse_inner(kimix::string_view source) {
    // Identical to `parse`: the returned ScriptNode owns `source` with local
    // coordinates (the runtime-reparsed operand's standalone string).
    return parse(source);
}

Node parse_command_context(kimix::string_view source) { return parse(source); }

} // namespace kimix::llm::bash_parser
