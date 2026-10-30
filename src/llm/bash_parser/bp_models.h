/* bp_models.h — lossless shell IR node types (port of kimi-agent
 * `shell_compat/models.py`).
 *
 * Every node represents source *by reference*: a `SourceSpan` into the single
 * source string owned by the root ScriptNode.  Nodes never copy or normalize
 * source text, so `render_source(script)` reproduces the input byte-for-byte
 * (the IR invariant: recognition may only *add* structure).
 *
 * OWNERSHIP MODEL (documented deviation from the frozen-dataclass reference):
 * the Python model uses frozen dataclasses shared by reference and refined in
 * place via `object.__setattr__` while the parser is still building a node.
 * That identity/refinement discipline cannot be reproduced with C++ value
 * semantics (a child copied into a parent vector gets a new address, and the
 * parser must still be able to mutate a HeredocNode or fill an ExpansionNode
 * child *after* the parent command was flushed).  So this port makes every
 * recursive edge an owned `kimix::unique_ptr<Node>`: a Node's heap address is
 * stable for its whole life regardless of where the owning pointer moves, and
 * the parser holds a non-owning `Node*` alias to a node it is still refining.
 * Repeated children (assignments/arguments/redirections/...) are
 * `kimix::vector<kimix::unique_ptr<Node>>`; single optional children (target /
 * delimiter_word / command_word / owner_word / child / inner) are
 * `kimix::unique_ptr<Node>` (empty == the reference's `None`).  `Node` is
 * therefore move-only (never copyable), and the root returned by `parse` owns
 * the whole tree transitively.
 *
 * The reference models each kind as a subclass of `Node`; this project forbids
 * RTTI/`dynamic_cast`, so there is ONE tagged struct `Node` (`kind`
 * discriminant) carrying every kind's fields (unused-per-kind fields default
 * empty).  `WordPartKind` is REUSED from bp_lexer.h (never re-declared here).
 *
 * Coordinate policy: every offset is a UTF-8 byte offset into the owning
 * source string, identical to the rest of the bash_parser module.
 */
#pragma once

#include <core/kimix_core.h>

#include "llm/bash_parser/bp_lexer.h" // WordPartKind, Token, LexResult (reused)
#include "llm/bash_parser/bp_spans.h" // SourceSpan, Diagnostic

namespace kimix::llm::bash_parser {

// Discriminant for the IR node tagged union (models.py `NodeKind`).
enum class NodeKind {
    kScript,
    kPipeline,
    kCommandList,
    kCommand,
    kCompound,
    kWord,
    kRedirection,
    kHeredoc,
    kExpansion,
    kEmbedded,
    kRaw,
    kNewline,
    kOperator,
};

// A word's lexical anatomy element: the `(kind, span)` pair kept by the IR
// (the lexer's third field `inner` is dropped — models.py WordPart is exactly
// `(kind, span)`).
using WordPartRef = std::pair<WordPartKind, SourceSpan>;

// A compound's named part: `(name, node)` (CompoundNode.parts in the
// reference; the name is the confident role, e.g. "body").
using CompoundPart = std::pair<kimix::string, kimix::unique_ptr<struct Node>>;

// The single IR node: a tagged union over all thirteen kinds.  Fields are
// grouped by owning kind; any field not meaningful for `kind` keeps its
// default (empty optional / empty vector / false / 0).  This mirrors the
// per-kind dataclass field sets of models.py one-for-one.
struct Node {
    NodeKind kind = NodeKind::kRaw;
    SourceSpan span;

    // ── WordNode (models.py WordNode) ────────────────────────────────────────
    kimix::vector<WordPartRef> parts;
    kimix::optional<kimix::string> literal_value;

    // ── RedirectionNode / HeredocNode / ExpansionNode / OperatorNode /
    //    NewlineNode: operator text (+ operator_span for redirection/heredoc) ─
    kimix::string operator_text;
    SourceSpan operator_span;

    // ── RedirectionNode (models.py RedirectionNode) ──────────────────────────
    kimix::optional<int32_t> fd;
    kimix::unique_ptr<Node> target; // owned target word (None == empty)
    bool can_truncate = false;

    // ── HeredocNode (models.py HeredocNode) ──────────────────────────────────
    kimix::unique_ptr<Node> delimiter_word; // owned delimiter word (None == empty)
    kimix::optional<kimix::string> delimiter_value;
    bool strip_tabs = false;
    bool expands = true;
    size_t body_start = 0;
    kimix::optional<size_t> body_end;
    kimix::optional<SourceSpan> terminator_span;

    // ── ExpansionNode (models.py ExpansionNode) ──────────────────────────────
    WordPartKind expansion_kind = WordPartKind::kRaw;
    kimix::unique_ptr<Node> child; // owned child ScriptNode (None == empty)

    // ── EmbeddedScriptNode (models.py EmbeddedScriptNode) ────────────────────
    kimix::unique_ptr<Node> owner_word; // owned quoted word (None == empty)
    kimix::optional<WordPartKind> quote_kind;
    kimix::unique_ptr<Node> inner; // owned inner ScriptNode (None == empty)

    // ── CommandNode (models.py CommandNode) ──────────────────────────────────
    kimix::vector<kimix::unique_ptr<Node>> assignments;
    kimix::unique_ptr<Node> command_word; // owned command word (None == empty)
    kimix::vector<kimix::unique_ptr<Node>> arguments;
    kimix::vector<kimix::unique_ptr<Node>> redirections;
    kimix::vector<kimix::unique_ptr<Node>> heredocs;
    kimix::vector<kimix::unique_ptr<Node>> embedded;
    kimix::vector<kimix::unique_ptr<Node>> expansions;
    kimix::optional<kimix::string> wrapper;
    kimix::optional<kimix::string> wrapper_hint;

    // ── PipelineNode (models.py PipelineNode) ────────────────────────────────
    kimix::vector<kimix::unique_ptr<Node>> commands;

    // ── CommandListNode (models.py CommandListNode) AND ScriptNode.children ──
    kimix::vector<kimix::unique_ptr<Node>> children;
    kimix::vector<kimix::string> operators; // PipelineNode / CommandListNode

    // ── CompoundNode (models.py CompoundNode) ────────────────────────────────
    kimix::string compound_kind;
    kimix::vector<CompoundPart> named_parts;

    // ── RawNode (models.py RawNode) ──────────────────────────────────────────
    kimix::string reason;

    // ── ScriptNode (models.py ScriptNode) ────────────────────────────────────
    kimix::string source;                       // the single string every span indexes into
    SourceSpan region;                          // the substring parsed as this context
    kimix::vector<kimix::unique_ptr<Node>> pending_heredocs;
    kimix::vector<Diagnostic> diagnostics;
    // Teardown worklist link -- bookkeeping, not IR.  ~Node detaches its owned
    // children into an intrusive thread-local list through this pointer
    // (bp_models.cpp) so a 1024-deep tree is freed iteratively instead of
    // recursively.  It is only meaningful while the node sits in that list.
    Node *teardown_next = nullptr;

    // Move-only (owning unique_ptr members); never copyable.
    Node() = default;
    Node(Node &&) = default;
    Node &operator=(Node &&) = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    // Iterative teardown (defined in bp_models.cpp): the nested command-substitution
    // chain can be up to kMaxNestingDepth deep, so the default recursive unique_ptr
    // destructor would overflow the native stack; this drains the owned graph through
    // a thread-local worklist instead.
    ~Node();

    // Non-owning accessors for the single optional children (null == `None`).
    const Node *command_word_p() const { return command_word.get(); }
    const Node *target_p() const { return target.get(); }
    const Node *delimiter_word_p() const { return delimiter_word.get(); }
    const Node *owner_word_p() const { return owner_word.get(); }
    const Node *child_p() const { return child.get(); }
    const Node *inner_p() const { return inner.get(); }
};

// Compound kinds the tolerant parser recognizes (models.py COMPOUND_KINDS).
bool is_compound_kind(kimix::string_view kind);

// render_source(node, source=None): for a ScriptNode, reproduce the region
// byte-for-byte from its tiling children (gap render, recursing through any
// ScriptNode child); for any other node, return the node's span slice of the
// `source` passed in (which must outlive the result).  `source` is ignored for
// a ScriptNode (its own `node.source` is the index space).
kimix::string render_source(const Node &node, kimix::string_view source = {});

// walk_nodes(node): yield `node` and every descendant in deterministic source
// order via an explicit worklist (no recursion at any depth, matching the
// reference's stack algorithm).  Embedded-script inner sources and child
// scripts are included.  The returned pointers borrow the tree rooted at
// `node` (valid while `node` lives).
kimix::vector<const Node *> walk_nodes(const Node &node);

// unglue(source, node): the exact source text a node stands for.
kimix::string_view unglue(kimix::string_view source, const Node &node);

// decode_word_literal(raw): the word value produced *solely by bash quote
// removal*, or nullopt when a parameter/command/arithmetic expansion, a
// backquote substitution, a glob/brace/tilde expansion, or malformed quoting
// contributes.  NOTE: this is models.py's decoder (its own `_LITERAL_SPECIAL_RE`
// and rules), which is intentionally NOT the same as bp_core's
// `literal_word_value` — it rejects ANSI-C `$'...'` and treats a `"` region
// containing `$`/backtick/glob as undecidable.  Port of models.py:634-712.
kimix::optional<kimix::string> decode_word_literal(kimix::string_view raw);

// decode_word_literal_partial(raw): best-effort decode (static quotes removed,
// expansions kept verbatim) — used only for hint recording; never fails.
// Port of models.py:715-752.
kimix::string decode_word_literal_partial(kimix::string_view raw);

} // namespace kimix::llm::bash_parser
