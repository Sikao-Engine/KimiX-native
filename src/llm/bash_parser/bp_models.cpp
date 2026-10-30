/* bp_models.cpp — implementation of the lossless shell IR models (port of
 * kimi-agent `shell_compat/models.py`: render_source / walk_nodes / unglue /
 * decode_word_literal / decode_word_literal_partial / COMPOUND_KINDS).
 *
 * The unity build (all src/llm/bash_parser/*.cpp concatenated into one TU)
 * means anonymous-namespace helper names must not collide across files, so the
 * file-local helpers here are `ir_`-prefixed.  No exceptions / RTTI.  Byte
 * offsets throughout.
 */
#include "llm/bash_parser/bp_models.h"

#include <core/kimix_core.h>

#include <utility>

namespace kimix::llm::bash_parser {
namespace src_llm_bash_parser_bp_models {

// models.py COMPOUND_KINDS membership.
constexpr const char *const kIrCompoundKinds[] = {
    "subshell", "brace_group", "function", "if", "for", "while", "until", "select", "case"};

// models.py `_LITERAL_SPECIAL_RE = [\\'\"$`*?[{~]`.  True for every character
// the static decoder must inspect (the backslash/quote/expansion/glob set).
bool ir_is_literal_special(char ch) {
    switch (ch) {
        case '\\':
        case '\'':
        case '"':
        case '$':
        case '`':
        case '*':
        case '?':
        case '[':
        case '{':
        case '~':
            return true;
        default:
            return false;
    }
}

// The index of the first literal-special character at or after `from`, or the
// sentinel `npos` when there is none (models.py `_LITERAL_SPECIAL_RE.search`).
size_t ir_literal_special_search(kimix::string_view raw, size_t from) {
    for (size_t i = from; i < raw.size(); ++i) {
        if (ir_is_literal_special(raw[i])) {
            return i;
        }
    }
    return kimix::string_view::npos;
}

// models.py `_find_quote_run_end(raw, i, quote)`: the index of the closing
// `quote` after `i` (skipping `\X` pairs), or -1 when unterminated.
int64_t ir_find_quote_run_end(kimix::string_view raw, size_t i, char quote) {
    const size_t n = raw.size();
    while (i < n) {
        const char ch = raw[i];
        if (ch == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (ch == quote) {
            return static_cast<int64_t>(i);
        }
        ++i;
    }
    return -1;
}

} // namespace src_llm_bash_parser_bp_models
using namespace src_llm_bash_parser_bp_models;

// ── iterative tree teardown ────────────────────────────────────────────────
// The nested command-substitution / embedded chain can be up to
// kMaxNestingDepth (1024) deep; the implicit recursive ~Node would then overflow
// the native stack.  ~Node detaches every owned child into a thread-local
// worklist (so the auto member destructors see empty containers and never
// recurse) and the single topmost destructor drains that worklist iteratively.
//
// The worklist is an intrusive singly-linked list threaded through
// Node::teardown_next, NOT a thread_local kimix::vector<unique_ptr<Node>>.
// A thread_local container is constructed by the CRT's dynamic TLS initializer
// on every thread that touches it and destroyed by that thread's TLS
// destructor -- and a thread that is still running when the process exits never
// runs its TLS destructors (the process-lifetime marl worker pool is exactly
// that case).  Each such thread then leaked the container's debug
// _Container_proxy (16 B) plus its buffer, which the KIMIX_MEMORY_STRICT
// tracker reported as a leak.  A raw pointer head has trivial thread-local
// storage: no allocation, no destructor, nothing to lose at exit.
namespace src_llm_bash_parser_bp_models {
thread_local Node *g_ir_node_teardown_head = nullptr;
thread_local bool g_ir_node_teardown_active = false;

/* Take ownership of a detached node and push it onto this thread's worklist. */
void teardown_push(kimix::unique_ptr<Node> &&owned) noexcept {
    Node *const node = owned.release();
    node->teardown_next = g_ir_node_teardown_head;
    g_ir_node_teardown_head = node;
}
} // namespace src_llm_bash_parser_bp_models
using namespace src_llm_bash_parser_bp_models;

Node::~Node() {
    kimix::vector<kimix::vector<kimix::unique_ptr<Node>> *> lists = {
        &assignments, &arguments, &redirections, &heredocs, &embedded, &expansions,
        &commands, &children, &pending_heredocs};
    for (auto *list : lists) {
        for (auto &up : *list) {
            if (up) {
                teardown_push(std::move(up));
            }
        }
        list->clear();
    }
    kimix::vector<kimix::unique_ptr<Node> *> singles = {
        &target, &delimiter_word, &command_word, &owner_word, &child, &inner};
    for (auto *single : singles) {
        if (*single) {
            teardown_push(std::move(*single));
        }
    }
    for (auto &named : named_parts) {
        if (named.second) {
            teardown_push(std::move(named.second));
        }
    }
    if (!g_ir_node_teardown_active) {
        g_ir_node_teardown_active = true;
        while (Node *head = g_ir_node_teardown_head) {
            g_ir_node_teardown_head = head->teardown_next;
            kimix::unique_ptr<Node> owned{head}; // adopt the detached node
            owned.reset(); // its ~Node pushes its own children (active => no nested drain)
        }
        g_ir_node_teardown_active = false;
    }
}

bool is_compound_kind(kimix::string_view kind) {
    for (const char *const candidate : kIrCompoundKinds) {
        if (kind == candidate) {
            return true;
        }
    }
    return false;
}

kimix::string render_source(const Node &node, kimix::string_view source) {
    if (node.kind != NodeKind::kScript) {
        // Non-script node: return the node's span slice of `source`.  Python
        // slices clamp silently when a span runs past the buffer (the lexer can
        // emit an over-long arithmetic/parameter region span); `substr` clamps
        // the same way, so this reproduces `source[start:end]` byte-for-byte.
        if (node.span.start >= source.size()) {
            return kimix::string();
        }
        return kimix::string(source.substr(node.span.start, node.span.length()));
    }
    const kimix::string_view src = node.source;
    const size_t n = src.size();
    kimix::string out;
    size_t cursor = node.region.start;
    for (const kimix::unique_ptr<Node> &child_ptr : node.children) {
        const Node &child = *child_ptr;
        const size_t start = child.span.start;
        if (start > cursor) {
            if (cursor < n) {
                out.append(src.substr(cursor, (start > n ? n : start) - cursor));
            }
        } else if (start < cursor) {
            continue; // defensive: never duplicate overlapping text
        }
        if (child.kind == NodeKind::kScript) {
            out.append(render_source(child));
        } else if (child.span.start < n) {
            out.append(src.substr(child.span.start, child.span.length()));
        }
        cursor = child.span.end;
        if (cursor > n) {
            cursor = n; // clamped cursor matches Python's slice semantics
        }
    }
    if (cursor < node.region.end && cursor < n) {
        out.append(src.substr(cursor, node.region.end - cursor));
    }
    return out;
}

kimix::vector<const Node *> walk_nodes(const Node &node) {
    // Explicit worklist (stack), matching the reference's pop-last / extend
    // order so the traversal sequence is byte-for-byte deterministic.
    kimix::vector<const Node *> stack;
    kimix::vector<const Node *> out;
    stack.push_back(&node);
    kimix::vector<const Node *> children; // reused scratch (order-sensitive)
    while (!stack.empty()) {
        const Node *current = stack.back();
        stack.pop_back();
        out.push_back(current);
        children.clear();
        const Node &cur = *current;
        switch (cur.kind) {
            case NodeKind::kScript:
                for (auto it = cur.children.rbegin(); it != cur.children.rend(); ++it) {
                    children.push_back(it->get());
                }
                for (auto it = cur.pending_heredocs.rbegin(); it != cur.pending_heredocs.rend(); ++it) {
                    children.push_back(it->get());
                }
                break;
            case NodeKind::kCommandList:
                for (auto it = cur.children.rbegin(); it != cur.children.rend(); ++it) {
                    children.push_back(it->get());
                }
                break;
            case NodeKind::kPipeline:
                for (auto it = cur.commands.rbegin(); it != cur.commands.rend(); ++it) {
                    children.push_back(it->get());
                }
                break;
            case NodeKind::kCommand:
                for (auto it = cur.assignments.rbegin(); it != cur.assignments.rend(); ++it) {
                    children.push_back(it->get());
                }
                if (cur.command_word) {
                    children.push_back(cur.command_word.get());
                }
                for (auto it = cur.arguments.rbegin(); it != cur.arguments.rend(); ++it) {
                    children.push_back(it->get());
                }
                for (auto it = cur.redirections.rbegin(); it != cur.redirections.rend(); ++it) {
                    children.push_back(it->get());
                }
                for (auto it = cur.heredocs.rbegin(); it != cur.heredocs.rend(); ++it) {
                    children.push_back(it->get());
                }
                for (auto it = cur.embedded.rbegin(); it != cur.embedded.rend(); ++it) {
                    children.push_back(it->get());
                }
                for (auto it = cur.expansions.rbegin(); it != cur.expansions.rend(); ++it) {
                    children.push_back(it->get());
                }
                break;
            case NodeKind::kRedirection:
                if (cur.target) {
                    children.push_back(cur.target.get());
                }
                break;
            case NodeKind::kHeredoc:
                if (cur.delimiter_word) {
                    children.push_back(cur.delimiter_word.get());
                }
                break;
            case NodeKind::kExpansion:
                if (cur.child) {
                    children.push_back(cur.child.get());
                }
                break;
            case NodeKind::kEmbedded:
                if (cur.owner_word) {
                    children.push_back(cur.owner_word.get());
                }
                if (cur.inner) {
                    children.push_back(cur.inner.get());
                }
                break;
            case NodeKind::kCompound:
                for (auto it = cur.named_parts.rbegin(); it != cur.named_parts.rend(); ++it) {
                    children.push_back(it->second.get());
                }
                break;
            default:
                break; // word/raw/newline/operator/self-tiling leaves
        }
        stack.insert(stack.end(), children.begin(), children.end());
    }
    return out;
}

kimix::string_view unglue(kimix::string_view source, const Node &node) {
    return node.span.text(source);
}

kimix::optional<kimix::string> decode_word_literal(kimix::string_view raw) {
    // Port of models.py decode_word_literal (lines 634-712).
    if (ir_literal_special_search(raw, 0) == kimix::string_view::npos) {
        return kimix::string(raw);
    }
    kimix::string value;
    const size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        const size_t m = ir_literal_special_search(raw, i);
        if (m == kimix::string_view::npos) {
            value.append(raw.data() + i, n - i);
            break;
        }
        if (m > i) {
            value.append(raw.data() + i, m - i);
            i = m;
        }
        const char ch = raw[i];
        if (ch == '\\') {
            if (i + 1 >= n) {
                return std::nullopt; // trailing lone backslash
            }
            if (raw[i + 1] == '\n') {
                i += 2; // line continuation joins the value
                continue;
            }
            value.push_back(raw[i + 1]);
            i += 2;
            continue;
        }
        if (ch == '\'') {
            const size_t close = raw.find('\'', i + 1);
            if (close == kimix::string_view::npos) {
                return std::nullopt; // unterminated single quote
            }
            value.append(raw.data() + i + 1, close - (i + 1));
            i = close + 1;
            continue;
        }
        if (ch == '"') {
            ++i;
            while (i < n && raw[i] != '"') {
                const char inner = raw[i];
                if (inner == '$' || inner == '`' || inner == '*' || inner == '?' || inner == '[') {
                    return std::nullopt; // runtime expansion inside the region
                }
                if (inner == '\\') {
                    if (i + 1 >= n) {
                        return std::nullopt;
                    }
                    const char nxt = raw[i + 1];
                    if (nxt == '"' || nxt == '\\' || nxt == '$' || nxt == '`') {
                        value.push_back(nxt);
                        i += 2;
                        continue;
                    }
                    if (nxt == '\n') {
                        i += 2; // escaped newline: line continuation
                        continue;
                    }
                    value.push_back('\\');
                    ++i;
                    continue;
                }
                value.push_back(inner);
                ++i;
            }
            if (i >= n) {
                return std::nullopt; // unterminated double quote
            }
            ++i; // past the closing "
            continue;
        }
        if (ch == '$') {
            // nxt = raw[i+1 : i+2] — the single char after `$` ("" at EOF).
            if (i + 1 < n) {
                const char nxt = raw[i + 1];
                if (nxt == '(' || nxt == '{' || nxt == '`' || nxt == '\'') {
                    return std::nullopt; // $( ${ $(( $' expansions are runtime values
                }
            }
            value.push_back('$');
            ++i;
            continue;
        }
        if (ch == '`') {
            return std::nullopt; // backquote substitution: runtime value
        }
        if (ch == '*' || ch == '?' || ch == '[' || ch == '{' || ch == '~') {
            return std::nullopt; // glob / brace / tilde expansion: runtime value
        }
        value.push_back(ch);
        ++i;
    }
    return value;
}

kimix::string decode_word_literal_partial(kimix::string_view raw) {
    // Port of models.py decode_word_literal_partial (lines 715-752).
    kimix::string value;
    const size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        const char ch = raw[i];
        if (ch == '\\') {
            if (i + 1 < n && raw[i + 1] != '\n') {
                value.push_back(raw[i + 1]);
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        if (ch == '\'') {
            const size_t close = raw.find('\'', i + 1);
            if (close == kimix::string_view::npos) {
                value.append(raw.data() + i, n - i);
                break;
            }
            value.append(raw.data() + i + 1, close - (i + 1));
            i = close + 1;
            continue;
        }
        if (ch == '"') {
            const int64_t close = ir_find_quote_run_end(raw, i + 1, '"');
            if (close < 0) {
                value.append(raw.data() + i, n - i);
                break;
            }
            const size_t cu = static_cast<size_t>(close);
            value.append(raw.data() + i + 1, cu - (i + 1));
            i = cu + 1;
            continue;
        }
        value.push_back(ch);
        ++i;
    }
    return value;
}

} // namespace kimix::llm::bash_parser
