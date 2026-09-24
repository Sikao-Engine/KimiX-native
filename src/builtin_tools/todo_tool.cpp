// todo_tool.cpp - Built-in agent tools "TodoWrite" / "TodoUpdate" (see
// todo_tool.h for the contract and the Python source-of-truth map).
//
// Ports kimi-cli/src/kimi_cli/tools/todo/__init__.py (todo_write + todo_update)
// including the session-state persistence of kimi_cli/session_state.py
// (state.json, TodoItemState shape, atomic write, corrupt-file defaults).
//
// Deviations from Python (documented, deliberate):
//   * rapidfuzz token_sort_ratio is re-implemented as normalized indel
//     similarity (2*LCS/(len1+len2)*100) over lowercased, whitespace-split,
//     sorted tokens. Cut-offs (60 / 75) and semantics (best score wins, ties
//     keep the earliest candidate) are unchanged.
//   * str.lower()/strip() are ASCII-only (titles are matched after lowering;
//     non-ASCII case folding is not applied).
//   * `children: null` is accepted as "not provided" (the Python repair layer
//     coerces common LLM type mistakes before validation).
//   * Unknown parameter keys are ignored (the Python repair layer strips them
//     for extra="forbid" models instead of raising).
//
// Unity build: every symbol lives in kimix::builtin_tools::todo; TU-local
// helpers additionally use the td_ prefix / anonymous namespace.

#include "builtin_tools/todo_tool.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <core/json_repair.h>

#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::todo {

namespace {

// ---------------------------------------------------------------------------
// Small string helpers (TU-local, td_ prefix)
// ---------------------------------------------------------------------------

bool td_is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

char td_lower_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

kimix::string_view td_trim_view(kimix::string_view s) noexcept {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && td_is_space(s[b])) {
        ++b;
    }
    while (e > b && td_is_space(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

kimix::string td_trim(kimix::string_view s) {
    const kimix::string_view t = td_trim_view(s);
    return kimix::string(t.data(), t.size());
}

kimix::string td_lower(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(td_lower_char(c));
    }
    return out;
}

kimix::string td_join(const kimix::vector<kimix::string> &parts,
                      kimix::string_view sep) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(sep.data(), sep.size());
        }
        out += parts[i];
    }
    return out;
}

// Python list-of-str repr: ['a', 'b'] (single quotes; no escaping of embedded
// quotes - titles with apostrophes render slightly differently from Python).
kimix::string td_repr_list(const kimix::vector<kimix::string> &items) {
    kimix::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += '\'';
        out += items[i];
        out += '\'';
    }
    out += ']';
    return out;
}

kimix::vector<kimix::string> td_split_ws(kimix::string_view s) {
    kimix::vector<kimix::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && td_is_space(s[i])) {
            ++i;
        }
        const size_t b = i;
        while (i < s.size() && !td_is_space(s[i])) {
            ++i;
        }
        if (i > b) {
            out.push_back(kimix::string(s.substr(b, i - b)));
        }
    }
    return out;
}

// Longest common subsequence length (two-row DP). Huge inputs fall back to an
// equality probe so the O(n*m) table stays bounded.
size_t td_lcs_len(kimix::string_view a, kimix::string_view b) {
    if (a.empty() || b.empty()) {
        return 0;
    }
    if (a.size() * b.size() > size_t(16) * 1024 * 1024) {
        return (a == b) ? std::min(a.size(), b.size()) : 0;
    }
    kimix::vector<uint32_t> prev(b.size() + 1, 0);
    kimix::vector<uint32_t> cur(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            cur[j] = (a[i - 1] == b[j - 1])
                         ? static_cast<uint32_t>(prev[j - 1] + 1)
                         : std::max(prev[j], cur[j - 1]);
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// processor=str.lower + token sort + single-space join (token_sort_ratio
// preprocessing).
kimix::string td_sorted_tokens_key(kimix::string_view s) {
    kimix::vector<kimix::string> toks = td_split_ws(td_lower(s));
    std::sort(toks.begin(), toks.end());
    return td_join(toks, " ");
}

// str(v) for non-string JSON scalars (error-message parity).
kimix::string td_value_repr(const ValueElement &el) {
    if (el.is_string()) {
        return el.as_string();
    }
    if (el.is_bool()) {
        return el.as_bool() ? kimix::string("True") : kimix::string("False");
    }
    if (el.is_int()) {
        return kimix::format("{}", el.as_int());
    }
    if (el.is_uint()) {
        return kimix::format("{}", el.as_uint());
    }
    if (el.is_real()) {
        return kimix::format("{}", el.as_real());
    }
    if (el.is_null()) {
        return kimix::string("None");
    }
    return el.is_array() ? kimix::string("<array>") : kimix::string("<object>");
}

bool td_stringify(const ValueElement &el, kimix::string &out) {
    if (el.is_string()) {
        out = el.as_string();
        return true;
    }
    if (el.is_bool() || el.is_int() || el.is_uint() || el.is_real()) {
        out = td_value_repr(el);
        return true;
    }
    return false;
}

// pydantic-style bool coercion (bool / 0 / 1 / "true" / "false" / ...).
bool td_coerce_bool(const ValueElement &el, bool &out) {
    if (el.is_bool()) {
        out = el.as_bool();
        return true;
    }
    if (el.is_int()) {
        const int64_t v = el.as_int();
        if (v == 0 || v == 1) {
            out = (v != 0);
            return true;
        }
        return false;
    }
    if (el.is_uint()) {
        const uint64_t v = el.as_uint();
        if (v <= 1) {
            out = (v != 0);
            return true;
        }
        return false;
    }
    if (el.is_string()) {
        const kimix::string s = td_lower(td_trim_view(el.as_string()));
        if (s == "true" || s == "1" || s == "yes" || s == "on") {
            out = true;
            return true;
        }
        if (s == "false" || s == "0" || s == "no" || s == "off") {
            out = false;
            return true;
        }
        return false;
    }
    return false;
}

// First present (canonical-first) key, mirroring AliasChoices + the
// alias-when-missing repair of _repair_dict_for_model.
const ValueElement *td_first_present(const ToolParams *params,
                                     std::initializer_list<const char *> names) {
    for (const char *n : names) {
        const ValueElement *el = params->get(kimix::string_view(n));
        if (el != nullptr) {
            return el;
        }
    }
    return nullptr;
}

// Error response builder: output = core + "\nHint: ..." (_hint_error parity).
tool_response td_error(tool_status st, kimix::string_view output_core,
                       kimix::string_view message,
                       kimix::string_view hint = k_default_error_hint) {
    tool_response r;
    r.status = st;
    r.is_error = true;
    r.output = kimix::string(output_core) + "\nHint: " + kimix::string(hint);
    r.message = kimix::string(message);
    return r;
}

// Validation-error flavour: message text becomes "Error: {msg}" + default hint.
tool_response td_validation_error(kimix::string_view msg) {
    return td_error(tool_status::invalid_input,
                    kimix::string("Error: ") + kimix::string(msg), msg);
}

commit_result td_fail(tool_response resp) {
    commit_result cr;
    cr.response = std::move(resp);
    cr.commit = false;
    return cr;
}

// All-done reminder + optional original-prompt context (_truncate_prompt).
kimix::string td_all_done_reminder(kimix::string_view current_prompt) {
    kimix::string r(k_all_done_reminder);
    if (!current_prompt.empty()) {
        r += "\nOriginal prompt:\n\n";
        r += truncate_prompt(current_prompt);
    }
    return r;
}

kimix::string td_stats(const status_count &counts, int32_t total) {
    return kimix::format("({} total: {} done, {} in progress, {} pending)",
                         total, counts.done, counts.in_progress,
                         counts.pending);
}

bool td_all_root_done(const kimix::vector<todo_item> &items) {
    for (const todo_item &t : items) {
        if (t.status != todo_status::done) {
            return false;
        }
    }
    return true;
}

kimix::string td_unfinished_lines(const kimix::vector<todo_item> &items) {
    kimix::vector<kimix::string> lines;
    for (const todo_item &t : items) {
        if (t.status != todo_status::done) {
            lines.push_back(t.content);
        }
    }
    return td_join(lines, "\n");
}

void td_collect_in_progress(kimix::vector<todo_item> &items,
                            kimix::vector<todo_item *> &slots) {
    for (todo_item &t : items) {
        if (t.status == todo_status::in_progress) {
            slots.push_back(&t);
        }
        td_collect_in_progress(t.children, slots); // DFS pre-order
    }
}

// _check_regressions detection half: titles whose old status was done and new
// status is not (DFS pre-order over the whole tree).
using td_status_map =
    kimix::unordered_map<kimix::string, todo_status, kimix::string_hash>;

void td_collect_old_status(const kimix::vector<todo_item> &items,
                           td_status_map &map) {
    for (const todo_item &t : items) {
        map[t.content] = t.status; // last write wins (Python dict)
        td_collect_old_status(t.children, map);
    }
}

// _check_regressions clamp half: a done todo that reappears as
// pending/in_progress is reverted to done; the regressed titles are appended
// in the same DFS pre-order as the Python clamp (used for the error display).
void td_clamp_regressions(const kimix::vector<todo_item> &items,
                          const td_status_map &old_map,
                          kimix::vector<todo_item> &out,
                          kimix::vector<kimix::string> &regressions) {
    for (const todo_item &src : items) {
        todo_item t = src;
        const auto it = old_map.find(t.content);
        if (it != old_map.end() && it->second == todo_status::done &&
            t.status != todo_status::done) {
            regressions.push_back(t.content);
            t.status = todo_status::done;
        }
        if (!t.children.empty()) {
            kimix::vector<todo_item> kids;
            td_clamp_regressions(t.children, old_map, kids, regressions);
            t.children = std::move(kids);
        }
        out.push_back(std::move(t));
    }
}

// _check_regressions(old_todos, final_todos) -> (clamped tree, regressions).
void td_check_regressions(const kimix::vector<todo_item> &old_todos,
                          const kimix::vector<todo_item> &final_todos,
                          kimix::vector<todo_item> &clamped,
                          kimix::vector<kimix::string> &regressions) {
    td_status_map old_map;
    td_collect_old_status(old_todos, old_map);
    clamped.clear();
    td_clamp_regressions(final_todos, old_map, clamped, regressions);
}

// _merge_one: same-title update preserving old notes/children when the new
// item omits them.
todo_item td_merge_one(const todo_item &old_item, const todo_item &new_item) {
    todo_item m;
    m.content = old_item.content;
    m.status = new_item.status;
    m.notes = new_item.notes.has_value() ? new_item.notes : old_item.notes;
    m.children = new_item.children_provided ? new_item.children : old_item.children;
    m.children_provided = true;
    return m;
}

// _merge_by_title_update: update existing root titles in place, append new.
kimix::vector<todo_item>
td_merge_by_title_update(const kimix::vector<todo_item> &old_todos,
                         const kimix::vector<todo_item> &new_todos) {
    kimix::unordered_map<kimix::string, const todo_item *, kimix::string_hash>
        new_by_title;
    for (const todo_item &n : new_todos) {
        new_by_title[n.content] = &n;
    }
    kimix::unordered_set<kimix::string, kimix::string_hash> old_titles;
    kimix::vector<todo_item> merged;
    merged.reserve(old_todos.size() + new_todos.size());
    for (const todo_item &o : old_todos) {
        const auto it = new_by_title.find(o.content);
        if (it != new_by_title.end()) {
            merged.push_back(td_merge_one(o, *it->second));
        } else {
            merged.push_back(o);
        }
        old_titles.insert(o.content);
    }
    for (const todo_item &n : new_todos) {
        if (old_titles.find(n.content) == old_titles.end()) {
            merged.push_back(n);
        }
    }
    return merged;
}

// _detect_fuzzy_warnings: non-blocking '"X" looks like existing "Y"' notes.
kimix::vector<kimix::string>
td_detect_fuzzy_warnings(const kimix::vector<todo_item> &new_todos,
                         const kimix::vector<todo_item> &old_todos) {
    kimix::vector<kimix::string> warnings;
    if (old_todos.empty()) {
        return warnings;
    }
    kimix::vector<kimix::string> old_title_list;
    kimix::unordered_set<kimix::string, kimix::string_hash> old_title_set;
    for (const todo_item &t : old_todos) {
        old_title_list.push_back(t.content);
        old_title_set.insert(t.content);
    }
    for (const todo_item &n : new_todos) {
        if (old_title_set.find(n.content) != old_title_set.end()) {
            continue;
        }
        const kimix::optional<fuzzy_hit> hit = find_nearest_title(
            n.content,
            kimix::span<const kimix::string>(old_title_list.data(),
                                             old_title_list.size()),
            k_fuzzy_warning_cutoff);
        if (hit.has_value()) {
            warnings.push_back(
                kimix::format("\"{}\" looks like existing \"{}\"", n.content,
                              hit->choice));
        }
    }
    return warnings;
}

// Em-dash (U+2014, UTF-8: E2 80 94) built from explicit byte values. A
// "\xe2\x80\x94" escape inside a narrow literal is translated into the ANSI
// execution charset by MSVC (the project builds without /utf-8), which can
// corrupt the byte sequence into invalid UTF-8; std::format validates its
// format string and throws std::format_error at runtime, and kimix::format is
// noexcept -> terminate. Building the bytes at runtime avoids the whole
// translation problem.
kimix::string td_em_dash() {
    kimix::string s;
    s.push_back(static_cast<char>(0xE2));
    s.push_back(static_cast<char>(0x80));
    s.push_back(static_cast<char>(0x94));
    return s;
}

// _detect_scope_duplicates: warn when a new root title exists deeper in the
// existing tree (append merges root-level titles only).
kimix::vector<kimix::string>
td_detect_scope_duplicates(const kimix::vector<todo_item> &new_todos,
                           const kimix::vector<todo_item> &old_todos) {
    kimix::vector<kimix::string> warnings;
    if (old_todos.empty() || new_todos.empty()) {
        return warnings;
    }
    kimix::unordered_set<kimix::string, kimix::string_hash> root_titles;
    for (const todo_item &t : old_todos) {
        root_titles.insert(t.content);
    }
    kimix::unordered_map<kimix::string, kimix::string, kimix::string_hash> nested;
    struct walker {
        static void walk(
            const kimix::vector<todo_item> &items,
            kimix::vector<kimix::string> &path,
            kimix::unordered_map<kimix::string, kimix::string,
                                 kimix::string_hash> &nested) {
            for (const todo_item &t : items) {
                if (!path.empty() &&
                    nested.find(t.content) == nested.end()) {
                    nested[t.content] = td_join(path, " > ");
                }
                path.push_back(t.content);
                walk(t.children, path, nested);
                path.pop_back();
            }
        }
    };
    kimix::vector<kimix::string> path;
    walker::walk(old_todos, path, nested);
    for (const todo_item &n : new_todos) {
        if (root_titles.find(n.content) != root_titles.end()) {
            continue;
        }
        const auto it = nested.find(n.content);
        if (it != nested.end()) {
            // Byte-parity note: Python's warning is built as
            //   f'"{t.content}" already exists in the tree (under "{parent}"); '
            //   'todo_write merges root-level titles only — use todo_update('
            //   'parent="{parent}", title="{t.content}") to update it.'
            // The second half is a PLAIN (non-f) string, so the reference
            // emits the literal "{parent}"/"{t.content}" placeholders. The port
            // reproduces that text verbatim.
            warnings.push_back(
                kimix::string("\"") + n.content +
                "\" already exists in the tree (under \"" + it->second +
                "\"); todo_write merges root-level titles only " +
                td_em_dash() +
                " use todo_update(parent=\"{parent}\", title=\"{t.content}\") "
                "to update it.");
        }
    }
    return warnings;
}

// ---------------------------------------------------------------------------
// Item / op parsing (pydantic model parity)
// ---------------------------------------------------------------------------

bool td_parse_item(const ValueElement &el, todo_item &out,
                   kimix::string &detail) {
    const ToolParams *obj = el.as_object();
    if (obj == nullptr) {
        detail = "Input should be a valid dictionary or instance of Todo";
        return false;
    }
    // content (AliasChoices: content, title, task, todo, item, name).
    const ValueElement *c =
        td_first_present(obj, {"content", "title", "task", "todo", "item", "name"});
    if (c == nullptr || c->is_null()) {
        detail = "Field required (content)";
        return false;
    }
    if (!c->is_string()) {
        detail = "Input should be a valid string (content)";
        return false;
    }
    kimix::string content = td_trim(c->as_string());
    if (content.empty()) {
        detail = "Title cannot be empty or contain only whitespace";
        return false;
    }
    if (utf8_code_point_count(content) > k_max_title_chars) {
        detail = kimix::format("String should have at most {} characters",
                               k_max_title_chars);
        return false;
    }
    // status (required, canonicalized).
    const ValueElement *s = obj->get("status");
    if (s == nullptr || s->is_null()) {
        detail = "Field required (status)";
        return false;
    }
    todo_status st = todo_status::pending;
    if (!s->is_string()) {
        const kimix::string vrepr = td_value_repr(*s);
        detail = kimix::format(
            "Invalid status '{}'. Must be one of: pending, in_progress, done "
            "(or completed).",
            vrepr);
        return false;
    }
    if (!parse_status(s->as_string(), st)) {
        detail = kimix::format(
            "Invalid status '{}'. Must be one of: pending, in_progress, done "
            "(or completed).",
            s->as_string());
        return false;
    }
    // notes (alias: description when notes is absent).
    const ValueElement *n = obj->get("notes");
    if (n == nullptr) {
        n = obj->get("description");
    }
    kimix::optional<kimix::string> notes;
    if (n != nullptr && !n->is_null()) {
        kimix::string raw;
        if (!td_stringify(*n, raw)) {
            detail = "Input should be a valid string (notes)";
            return false;
        }
        if (utf8_code_point_count(raw) > k_max_notes_chars) {
            detail = kimix::format("String should have at most {} characters",
                                   k_max_notes_chars);
            return false;
        }
        kimix::string stripped = td_trim(raw);
        if (!stripped.empty()) {
            notes = std::move(stripped);
        }
    }
    // children (recursive; `null` tolerated as "not provided").
    kimix::vector<todo_item> children;
    bool children_provided = false;
    const ValueElement *ch = obj->get("children");
    if (ch != nullptr && !ch->is_null()) {
        if (!ch->is_array()) {
            detail = "Input should be a valid list (children)";
            return false;
        }
        children_provided = true;
        const ValueElement::Array &arr = ch->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            todo_item child;
            kimix::string cdetail;
            if (!td_parse_item(arr[i], child, cdetail)) {
                detail = kimix::format("children[{}]: {}", i, cdetail);
                return false;
            }
            children.push_back(std::move(child));
        }
    }
    out.content = std::move(content);
    out.status = st;
    out.notes = std::move(notes);
    out.children = std::move(children);
    out.children_provided = children_provided;
    return true;
}

bool td_parse_op(const ToolParams &obj, update_op &out, kimix::string &detail) {
    // title (AliasChoices: title, content, task, todo, item, name).
    const ValueElement *t =
        td_first_present(&obj, {"title", "content", "task", "todo", "item", "name"});
    if (t == nullptr || t->is_null()) {
        detail = "Field required (title)";
        return false;
    }
    if (!t->is_string()) {
        detail = "Input should be a valid string (title)";
        return false;
    }
    out.title = td_trim(t->as_string());
    if (out.title.empty()) {
        detail = "Title cannot be empty or contain only whitespace";
        return false;
    }
    if (utf8_code_point_count(out.title) > k_max_title_chars) {
        detail = kimix::format("String should have at most {} characters",
                               k_max_title_chars);
        return false;
    }
    // status (optional, canonicalized).
    if (const ValueElement *s = obj.get("status");
        s != nullptr && !s->is_null()) {
        if (!s->is_string()) {
            const kimix::string vrepr = td_value_repr(*s);
            detail = kimix::format(
                "Invalid status '{}'. Must be one of: pending, in_progress, "
                "done (or completed).",
                vrepr);
            return false;
        }
        todo_status st = todo_status::pending;
        if (!parse_status(s->as_string(), st)) {
            detail = kimix::format(
                "Invalid status '{}'. Must be one of: pending, in_progress, "
                "done (or completed).",
                s->as_string());
            return false;
        }
        out.has_status = true;
        out.status = st;
    }
    // notes (optional; kept raw - the apply step strips; "" clears).
    if (const ValueElement *n = obj.get("notes");
        n != nullptr && !n->is_null()) {
        kimix::string raw;
        if (!td_stringify(*n, raw)) {
            detail = "Input should be a valid string (notes)";
            return false;
        }
        if (utf8_code_point_count(raw) > k_max_notes_chars) {
            detail = kimix::format("String should have at most {} characters",
                                   k_max_notes_chars);
            return false;
        }
        out.has_notes = true;
        out.notes = std::move(raw);
    }
    // rename_to (stripped; empty -> absent, _validate_rename_to parity).
    if (const ValueElement *r = obj.get("rename_to");
        r != nullptr && !r->is_null()) {
        if (!r->is_string()) {
            detail = "Input should be a valid string (rename_to)";
            return false;
        }
        kimix::string s = td_trim(r->as_string());
        if (!s.empty()) {
            out.has_rename = true;
            out.rename_to = std::move(s);
        }
    }
    // parent (stripped; "" == root scope).
    if (const ValueElement *p = obj.get("parent");
        p != nullptr && !p->is_null()) {
        if (!p->is_string()) {
            detail = "Input should be a valid string (parent)";
            return false;
        }
        out.has_parent = true;
        out.parent = td_trim(p->as_string());
    }
    // Booleans.
    struct bool_field {
        const char *name;
        bool default_value;
        bool *target;
    };
    const bool_field fields[] = {
        {"fuzzy", true, &out.fuzzy},
        {"force", false, &out.force},
        {"complete", false, &out.complete},
    };
    for (const bool_field &f : fields) {
        const ValueElement *el = obj.get(kimix::string_view(f.name));
        if (el == nullptr || el->is_null()) {
            *f.target = f.default_value;
            continue;
        }
        bool v = f.default_value;
        if (!td_coerce_bool(*el, v)) {
            detail = kimix::format("Input should be a valid boolean ({})",
                                   f.name);
            return false;
        }
        *f.target = v;
    }
    return true;
}

// Wrap embedded JSON text (string-valued todos/updates) into an object so
// ToolParams::try_deserialize (object-root only) can parse it.
//
// kimi_cli.tools.utils.repair_json_string only treats a string as JSON when it
// *starts* with '[' or '{' (`_looks_like_json`); anything else (a bare title,
// but also the scalars "123"/"true"/"null") is not JSON, so this must not try
// to parse it either.
bool td_parse_embedded_json(kimix::string_view text, kimix::string_view key,
                            ValueElement &out, kimix::string &detail) {
    kimix::string body = td_trim(text);
    if (body.empty()) {
        detail = "embedded JSON is empty";
        return false;
    }
    if (body[0] != '{' && body[0] != '[') {
        detail = "not JSON (must start with '{' or '[')";
        return false;
    }
    const kimix::string repaired = kimix::repair(body);
    if (!repaired.empty()) {
        body = repaired;
    }
    kimix::string wrapped = "{\"";
    wrapped.append(key.data(), key.size());
    wrapped += "\":";
    wrapped += body;
    wrapped += "}";
    ToolParams tp;
    kimix::string perr;
    if (!tp.try_deserialize(
            kimix::span<char const>(wrapped.data(), wrapped.size()), perr)) {
        detail = perr;
        return false;
    }
    const ValueElement *el = tp.get(key);
    if (el == nullptr) {
        detail = "missing key after parse";
        return false;
    }
    out = *el; // deep copy (tp dies here)
    return true;
}

// Mixed-fields check (TodoUpdateParams._check_no_mixed_fields).
kimix::vector<kimix::string> td_mixed_fields(const ToolParams *params) {
    kimix::vector<kimix::string> mixed;
    if (td_first_present(params,
                         {"title", "content", "task", "todo", "item", "name"}) !=
        nullptr) {
        mixed.push_back("title");
    }
    for (const char *name : {"status", "notes", "rename_to", "complete"}) {
        if (params->get(kimix::string_view(name)) != nullptr) {
            mixed.push_back(name);
        }
    }
    std::sort(mixed.begin(), mixed.end());
    return mixed;
}

// ---------------------------------------------------------------------------
// todo_update apply kernels
// ---------------------------------------------------------------------------

struct td_apply_out {
    bool ok = false;
    kimix::string summary;
    kimix::string message;
    tool_response error;
};

// _apply_update_to_tree (in-place variant: `todos` is already a fresh copy).
bool td_apply_update_to_tree(kimix::vector<todo_item> &todos,
                             const kimix::vector<int32_t> &path,
                             const kimix::string &matched_title,
                             const update_op &op, td_apply_out &out) {
    todo_item *node =
        node_at_path(todos, kimix::span<const int32_t>(path.data(), path.size()));
    if (node == nullptr) {
        out.error = td_error(tool_status::not_found,
                             "Error: No todo titled \"" + matched_title +
                                 "\" found.",
                             "Todo \"" + matched_title + "\" not found.");
        return false;
    }
    const todo_status new_status = op.has_status ? op.status : node->status;

    // complete=True cannot be combined with pending/in_progress.
    if (op.complete && op.has_status && op.status != todo_status::done) {
        const kimix::string sname(status_name(op.status));
        out.error = td_error(
            tool_status::invalid_input,
            kimix::format(
                "Error: complete=True cannot be combined with status=\"{}\" "
                "for \"{}\". complete=True always marks everything done.",
                sname, matched_title),
            kimix::format(
                "complete=True cannot be combined with status=\"{}\".", sname),
            kimix::format(
                "Use todo_update \"{}\" with status=\"done\" instead of "
                "complete=True, or omit status.",
                matched_title));
        return false;
    }
    // Regression guard: done -> pending/in_progress needs force.
    if (!op.force && node->status == todo_status::done &&
        new_status != todo_status::done) {
        const kimix::string nsname(status_name(new_status));
        out.error = td_error(
            tool_status::blocked,
            kimix::format(
                "Error: Cannot regress completed todo \"{}\" back to {}.",
                matched_title, nsname),
            kimix::format(
                "Cannot regress completed todo \"{}\" back to {}.",
                matched_title, nsname),
            kimix::format(
                "Use todo_update \"{}\" with force=True to reopen a done "
                "item, or todo_write with mode='replace' and force=True to "
                "restart the whole list.",
                matched_title));
        return false;
    }
    // Rename collision guard within the matched scope.
    kimix::string final_title = matched_title;
    if (op.has_rename && op.rename_to != matched_title) {
        kimix::vector<int32_t> parent_path(path.begin(), path.end() - 1);
        const todo_item *parent_node =
            parent_path.empty()
                ? nullptr
                : node_at_path(todos, kimix::span<const int32_t>(parent_path.data(),
                                                                 parent_path.size()));
        const kimix::vector<todo_item> &siblings =
            (parent_node != nullptr) ? parent_node->children : todos;
        const int32_t self_index = path.back();
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (static_cast<int32_t>(i) != self_index &&
                siblings[i].content == op.rename_to) {
                out.error = td_error(
                    tool_status::blocked,
                    kimix::format(
                        "Error: Cannot rename \"{}\" to \"{}\": title already "
                        "exists in this scope.",
                        matched_title, op.rename_to),
                    kimix::format(
                        "Cannot rename \"{}\" to \"{}\": title already exists.",
                        matched_title, op.rename_to),
                    kimix::format(
                        "Use todo_update \"{}\" to update the existing item "
                        "instead of renaming.",
                        op.rename_to));
                return false;
            }
        }
        final_title = op.rename_to;
    }
    // Notes: absent keeps; "" (or whitespace) clears.
    if (op.has_notes) {
        kimix::string stripped = td_trim(op.notes);
        if (stripped.empty()) {
            node->notes.reset();
        } else {
            node->notes = std::move(stripped);
        }
    }
    node->content = final_title;
    node->status = new_status;

    kimix::vector<kimix::string> change_parts;
    if (op.complete) {
        mark_subtree_done(*node);
        const int32_t n = count_all(kimix::span<const todo_item>(node, 1));
        change_parts.push_back(
            kimix::format("completed with {} sub-todo{} marked done", n,
                          (n != 1) ? "s" : ""));
    }
    if (op.has_status) {
        change_parts.push_back(
            kimix::format("status={}", status_name(new_status)));
    }
    if (op.has_notes) {
        change_parts.push_back("notes updated");
    }
    if (op.has_rename) {
        change_parts.push_back(
            kimix::format("renamed to \"{}\"", final_title));
    }
    const kimix::string change_summary =
        change_parts.empty() ? kimix::string("no changes")
                             : td_join(change_parts, ", ");
    out.summary = kimix::format("Updated \"{}\" ({}).", matched_title,
                                change_summary);
    out.message = kimix::format("Updated \"{}\".", matched_title);
    out.ok = true;
    return true;
}

// _update_global_in_memory.
bool td_update_global(kimix::vector<todo_item> &todos, const update_op &op,
                      kimix::vector<kimix::string> &warnings,
                      td_apply_out &out) {
    kimix::vector<int32_t> path;
    kimix::string matched = op.title;
    if (!find_path(todos, op.title, path)) {
        if (!op.fuzzy) {
            out.error = td_error(
                tool_status::not_found,
                kimix::format("Error: No todo titled \"{}\" found.", op.title),
                kimix::format("Todo \"{}\" not found.", op.title),
                "Use todo_write to read the tree, or set fuzzy=True to search "
                "by similarity.");
            return false;
        }
        const kimix::vector<kimix::string> titles = collect_titles(todos);
        const kimix::optional<fuzzy_hit> hit = find_nearest_title(
            op.title,
            kimix::span<const kimix::string>(titles.data(), titles.size()),
            k_fuzzy_title_cutoff);
        if (!hit.has_value()) {
            out.error = td_error(
                tool_status::not_found,
                kimix::format("Error: No todo matching \"{}\" found.",
                              op.title),
                kimix::format("No todo matching \"{}\" found.", op.title),
                "Use todo_write to read the tree.");
            return false;
        }
        matched = hit->choice;
        path.clear();
        find_path(todos, matched, path);
        warnings.push_back(
            kimix::format("Fuzzy matched \"{}\" to \"{}\".", op.title, matched));
    }
    return td_apply_update_to_tree(todos, path, matched, op, out);
}

// _update_or_create_under_parent_in_memory.
bool td_update_under_parent(kimix::vector<todo_item> &todos,
                            const update_op &op,
                            kimix::vector<kimix::string> &warnings,
                            int32_t max_layers, td_apply_out &out) {
    kimix::vector<int32_t> parent_path;
    kimix::string resolved_parent_title;
    todo_item *parent_node = nullptr;
    if (op.parent.empty()) {
        resolved_parent_title = "root";
    } else {
        if (!find_path(todos, op.parent, parent_path)) {
            if (!op.fuzzy) {
                out.error = td_error(
                    tool_status::not_found,
                    kimix::format("Error: No parent todo titled \"{}\" found.",
                                  op.parent),
                    kimix::format("Parent todo \"{}\" not found.", op.parent),
                    "Use todo_write to read the tree, or set fuzzy=True to "
                    "search by similarity.");
                return false;
            }
            const kimix::vector<kimix::string> titles = collect_titles(todos);
            const kimix::optional<fuzzy_hit> hit = find_nearest_title(
                op.parent,
                kimix::span<const kimix::string>(titles.data(), titles.size()),
                k_fuzzy_title_cutoff);
            if (!hit.has_value()) {
                out.error = td_error(
                    tool_status::not_found,
                    kimix::format(
                        "Error: No parent todo matching \"{}\" found.",
                        op.parent),
                    kimix::format("No parent todo matching \"{}\" found.",
                                  op.parent),
                    "Use todo_write to read the tree.");
                return false;
            }
            resolved_parent_title = hit->choice;
            parent_path.clear();
            find_path(todos, resolved_parent_title, parent_path);
            warnings.push_back(kimix::format(
                "Fuzzy matched parent \"{}\" to \"{}\".", op.parent,
                resolved_parent_title));
            parent_node = node_at_path(
                todos,
                kimix::span<const int32_t>(parent_path.data(), parent_path.size()));
        } else {
            parent_node = node_at_path(
                todos,
                kimix::span<const int32_t>(parent_path.data(), parent_path.size()));
            resolved_parent_title =
                (parent_node != nullptr) ? parent_node->content : op.parent;
        }
    }

    // Exact-title lookup in the resolved scope (direct children only).
    const kimix::vector<todo_item> &scope =
        (parent_node != nullptr) ? parent_node->children : todos;
    int32_t child_index = -1;
    for (size_t i = 0; i < scope.size(); ++i) {
        if (scope[i].content == op.title) {
            child_index = static_cast<int32_t>(i);
            break;
        }
    }
    if (child_index >= 0) {
        kimix::vector<int32_t> path = parent_path;
        path.push_back(child_index);
        return td_apply_update_to_tree(todos, path, op.title, op, out);
    }

    // New child creation under the resolved parent.
    if (op.has_rename) {
        out.error = td_error(
            tool_status::invalid_input,
            kimix::format("Error: \"{}\" does not exist under \"{}\".",
                          op.title, resolved_parent_title),
            kimix::format("Cannot rename non-existent todo \"{}\".", op.title),
            kimix::format(
                "Cannot rename a new child; use title=\"{}\" to create it.",
                op.rename_to));
        return false;
    }
    if (op.complete) {
        out.error = td_error(
            tool_status::invalid_input,
            kimix::format("Error: \"{}\" does not exist under \"{}\".",
                          op.title, resolved_parent_title),
            kimix::format("Cannot complete non-existent todo \"{}\".",
                          op.title),
            kimix::format(
                "complete=True requires an existing todo; \"{}\" does not "
                "exist under \"{}\". Create it first or use status=\"done\".",
                op.title, resolved_parent_title));
        return false;
    }
    const int32_t parent_depth = static_cast<int32_t>(parent_path.size());
    if (parent_depth > max_layers) {
        out.error = td_error(
            tool_status::blocked,
            kimix::format(
                "Error: \"{}\" is at depth {}; children would exceed the "
                "maximum depth ({}).",
                resolved_parent_title, parent_depth, max_layers + 1),
            kimix::format("Cannot add a child under \"{}\": too deep.",
                          resolved_parent_title),
            kimix::format("Cannot add children deeper than {} layers.",
                          max_layers + 1));
        return false;
    }
    todo_item child;
    child.content = op.title;
    child.status = op.has_status ? op.status : todo_status::pending;
    if (op.has_notes) {
        kimix::string stripped = td_trim(op.notes);
        if (!stripped.empty()) {
            child.notes = std::move(stripped);
        }
    }
    if (parent_node != nullptr) {
        parent_node->children.push_back(std::move(child));
    } else {
        todos.push_back(std::move(child));
    }
    out.summary = kimix::format("Created \"{}\" under \"{}\".", op.title,
                                resolved_parent_title);
    out.message = out.summary;
    out.ok = true;
    return true;
}

// _apply_one_update.
bool td_apply_one_update(kimix::vector<todo_item> &todos, const update_op &op,
                         kimix::vector<kimix::string> &warnings,
                         int32_t max_layers, td_apply_out &out) {
    if (!op.has_parent) {
        if (todos.empty()) {
            out.error = td_error(
                tool_status::not_found, "Error: No todos exist.",
                "No todos to update.",
                "Use todo_update(parent=\"\", title=\"...\") to create a root "
                "todo, or todo_write to set the whole list.");
            return false;
        }
        return td_update_global(todos, op, warnings, out);
    }
    return td_update_under_parent(todos, op, warnings, max_layers, out);
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool td_read_file(kimix::string_view path, kimix::string &out) {
    out.clear();
    std::FILE *f = std::fopen(kimix::string(path).c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

bool td_write_file(kimix::string_view path, const kimix::vector<char> &data,
                   kimix::string &error) {
    std::FILE *f = std::fopen(kimix::string(path).c_str(), "wb");
    if (f == nullptr) {
        error = "cannot open file for writing: " + kimix::string(path);
        return false;
    }
    const size_t written =
        data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (written != data.size()) {
        error = "short write: " + kimix::string(path);
        return false;
    }
    return true;
}

ValueElement td_item_to_value(const todo_item &item) {
    kimix::shared_ptr<ToolParams> obj(new ToolParams());
    obj->values["title"] = ValueElement::make_string(item.content);
    obj->values["status"] =
        ValueElement::make_string(kimix::string(status_name(item.status)));
    obj->values["notes"] = item.notes.has_value()
                               ? ValueElement::make_string(*item.notes)
                               : ValueElement::make_null();
    ValueElement::Array children;
    for (const todo_item &c : item.children) {
        children.push_back(td_item_to_value(c));
    }
    obj->values["children"] = ValueElement::make_array(std::move(children));
    return ValueElement::make_object(std::move(obj));
}

ValueElement td_items_to_array(const kimix::vector<todo_item> &items) {
    ValueElement::Array arr;
    for (const todo_item &it : items) {
        arr.push_back(td_item_to_value(it));
    }
    return ValueElement::make_array(std::move(arr));
}

// Lenient state-file item parse (TodoItemState + Todo.model_validate parity;
// malformed items are skipped by the caller). A malformed CHILD fails the
// whole item (pydantic raises for the parent too).
bool td_item_from_value(const ValueElement &el, todo_item &out) {
    const ToolParams *obj = el.as_object();
    if (obj == nullptr) {
        return false;
    }
    const ValueElement *t = obj->get("title");
    if (t == nullptr) {
        t = obj->get("content"); // tolerant fallback
    }
    if (t == nullptr || !t->is_string()) {
        return false;
    }
    kimix::string title = td_trim(t->as_string());
    if (title.empty()) {
        return false;
    }
    const ValueElement *s = obj->get("status");
    if (s == nullptr || !s->is_string()) {
        return false;
    }
    todo_status st = todo_status::pending;
    if (!parse_status(s->as_string(), st)) {
        return false;
    }
    out.content = std::move(title);
    out.status = st;
    if (const ValueElement *n = obj->get("notes");
        n != nullptr && !n->is_null()) {
        kimix::string raw;
        if (td_stringify(*n, raw)) {
            kimix::string stripped = td_trim(raw);
            if (!stripped.empty()) {
                out.notes = std::move(stripped);
            }
        }
    }
    out.children_provided = true;
    if (const ValueElement *c = obj->get("children");
        c != nullptr && !c->is_null()) {
        if (!c->is_array()) {
            return false;
        }
        for (const ValueElement &cel : c->as_array()) {
            todo_item child;
            if (!td_item_from_value(cel, child)) {
                return false;
            }
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Status vocabulary
// ---------------------------------------------------------------------------

kimix::string_view status_name(todo_status s) noexcept {
    switch (s) {
        case todo_status::pending:
            return "pending";
        case todo_status::in_progress:
            return "in_progress";
        case todo_status::done:
            return "done";
    }
    return "pending";
}

bool parse_status(kimix::string_view v, todo_status &out) noexcept {
    kimix::string n = td_lower(td_trim_view(v));
    for (char &c : n) {
        if (c == '-') {
            c = '_';
        }
    }
    if (n == "pending") {
        out = todo_status::pending;
        return true;
    }
    if (n == "in_progress") {
        out = todo_status::in_progress;
        return true;
    }
    if (n == "done" || n == "completed") {
        out = todo_status::done;
        return true;
    }
    return false;
}

kimix::string_view td_status_string(tool_status s) noexcept {
    switch (s) {
        case tool_status::ok:
            return "ok";
        case tool_status::invalid_input:
            return "invalid_input";
        case tool_status::not_found:
            return "not_found";
        case tool_status::no_change:
            return "no_change";
        case tool_status::ambiguous:
            return "ambiguous";
        case tool_status::blocked:
            return "blocked";
        case tool_status::too_large:
            return "too_large";
        case tool_status::unsupported:
            return "unsupported";
        case tool_status::external_library:
            return "external_library";
    }
    return "ok";
}

// ---------------------------------------------------------------------------
// Tree kernels
// ---------------------------------------------------------------------------

int32_t count_all(kimix::span<const todo_item> items) noexcept {
    int32_t total = 0;
    for (const todo_item &t : items) {
        total += 1;
        total += count_all(kimix::span<const todo_item>(t.children.data(),
                                                        t.children.size()));
    }
    return total;
}

status_count status_counts(kimix::span<const todo_item> items) noexcept {
    status_count c;
    for (const todo_item &t : items) {
        switch (t.status) {
            case todo_status::pending:
                ++c.pending;
                break;
            case todo_status::in_progress:
                ++c.in_progress;
                break;
            case todo_status::done:
                ++c.done;
                break;
        }
        const status_count sub = status_counts(
            kimix::span<const todo_item>(t.children.data(), t.children.size()));
        c.pending += sub.pending;
        c.in_progress += sub.in_progress;
        c.done += sub.done;
    }
    return c;
}

int32_t max_tree_depth(kimix::span<const todo_item> items) noexcept {
    int32_t best = 0;
    for (const todo_item &t : items) {
        const int32_t sub = max_tree_depth(kimix::span<const todo_item>(
            t.children.data(), t.children.size()));
        best = std::max(best, sub + 1);
    }
    return best;
}

kimix::vector<kimix::string>
find_duplicate_titles(kimix::span<const todo_item> items) {
    kimix::unordered_set<kimix::string, kimix::string_hash> seen;
    kimix::unordered_set<kimix::string, kimix::string_hash> dups;
    for (const todo_item &t : items) {
        if (seen.find(t.content) != seen.end()) {
            dups.insert(t.content);
        } else {
            seen.insert(t.content);
        }
    }
    kimix::vector<kimix::string> out(dups.begin(), dups.end());
    std::sort(out.begin(), out.end());
    return out;
}

kimix::vector<kimix::string> collect_titles(kimix::span<const todo_item> items) {
    kimix::vector<kimix::string> titles;
    for (const todo_item &t : items) {
        titles.push_back(t.content);
        const kimix::vector<kimix::string> sub = collect_titles(
            kimix::span<const todo_item>(t.children.data(), t.children.size()));
        titles.insert(titles.end(), sub.begin(), sub.end());
    }
    return titles;
}

bool find_path(kimix::span<const todo_item> items, kimix::string_view title,
               kimix::vector<int32_t> &path) {
    const size_t base = path.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].content == title) {
            path.push_back(static_cast<int32_t>(i));
            return true;
        }
        path.push_back(static_cast<int32_t>(i));
        const bool found = find_path(
            kimix::span<const todo_item>(items[i].children.data(),
                                         items[i].children.size()),
            title, path);
        if (found) {
            return true;
        }
        path.resize(base);
    }
    path.resize(base);
    return false;
}

todo_item *node_at_path(kimix::vector<todo_item> &items,
                        kimix::span<const int32_t> path) noexcept {
    if (path.empty()) {
        return nullptr;
    }
    const int32_t idx = path[0];
    if (idx < 0 || static_cast<size_t>(idx) >= items.size()) {
        return nullptr;
    }
    if (path.size() == 1) {
        return &items[static_cast<size_t>(idx)];
    }
    return node_at_path(items[static_cast<size_t>(idx)].children,
                        path.subspan(1));
}

const todo_item *node_at_path(const kimix::vector<todo_item> &items,
                              kimix::span<const int32_t> path) noexcept {
    if (path.empty()) {
        return nullptr;
    }
    const int32_t idx = path[0];
    if (idx < 0 || static_cast<size_t>(idx) >= items.size()) {
        return nullptr;
    }
    if (path.size() == 1) {
        return &items[static_cast<size_t>(idx)];
    }
    return node_at_path(items[static_cast<size_t>(idx)].children,
                        path.subspan(1));
}

void mark_subtree_done(todo_item &node) noexcept {
    node.status = todo_status::done;
    for (todo_item &c : node.children) {
        mark_subtree_done(c);
    }
}

int32_t count_unfinished_descendants(const todo_item &node) noexcept {
    int32_t total = 0;
    for (const todo_item &c : node.children) {
        if (c.status != todo_status::done) {
            ++total;
        }
        total += count_unfinished_descendants(c);
    }
    return total;
}

kimix::vector<kimix::string>
find_in_progress_conflicts(kimix::span<const todo_item> items) {
    kimix::vector<kimix::string> in_progress;
    struct walker {
        static void walk(kimix::span<const todo_item> nodes,
                         kimix::vector<kimix::string> &out) {
            for (const todo_item &t : nodes) {
                if (t.status == todo_status::in_progress) {
                    out.push_back(t.content);
                }
                walk(kimix::span<const todo_item>(t.children.data(),
                                                  t.children.size()),
                     out);
            }
        }
    };
    walker::walk(items, in_progress);
    if (in_progress.size() > 1) {
        return in_progress;
    }
    return {};
}

void auto_fix_in_progress(kimix::vector<todo_item> &items,
                          kimix::vector<kimix::string> &warnings) {
    kimix::vector<todo_item *> slots;
    td_collect_in_progress(items, slots);
    if (slots.size() <= 1) {
        return;
    }
    const kimix::string kept_title = slots.back()->content;
    for (size_t i = 0; i + 1 < slots.size(); ++i) {
        todo_item *n = slots[i];
        warnings.push_back(kimix::format(
            "Auto-fixed \"{}\": set to done (only one item may be "
            "in_progress; keeping \"{}\" in_progress)",
            n->content, kept_title));
        n->status = todo_status::done;
    }
}

// ---------------------------------------------------------------------------
// Fuzzy matching
// ---------------------------------------------------------------------------

double token_sort_ratio(kimix::string_view a, kimix::string_view b) noexcept {
    const kimix::string sa = td_sorted_tokens_key(a);
    const kimix::string sb = td_sorted_tokens_key(b);
    if (sa.empty() && sb.empty()) {
        return 100.0;
    }
    if (sa.empty() || sb.empty()) {
        return 0.0;
    }
    const size_t lcs = td_lcs_len(sa, sb);
    return 200.0 * static_cast<double>(lcs) /
           static_cast<double>(sa.size() + sb.size());
}

kimix::optional<fuzzy_hit>
find_nearest_title(kimix::string_view query,
                   kimix::span<const kimix::string> candidates,
                   double cutoff) {
    if (candidates.empty()) {
        return std::nullopt;
    }
    double best_score = -1.0;
    size_t best_index = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const double score = token_sort_ratio(query, candidates[i]);
        if (score >= cutoff && score > best_score) {
            best_score = score;
            best_index = i;
        }
    }
    if (best_score < 0.0) {
        return std::nullopt;
    }
    fuzzy_hit hit;
    hit.choice = candidates[best_index];
    hit.score = best_score;
    hit.index = static_cast<int32_t>(best_index);
    return hit;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

kimix::string format_todos(kimix::span<const todo_item> items) {
    kimix::vector<kimix::string> lines;
    for (const todo_item &t : items) {
        if (t.status == todo_status::done) {
            continue; // status_filter == (pending, in_progress)
        }
        kimix::string line = "- [";
        line += (t.status == todo_status::in_progress) ? "in progress"
                                                       : "pending";
        line += "] ";
        line += t.content;
        if (t.status == todo_status::in_progress && t.notes.has_value()) {
            line += "  Notes: ";
            line += *t.notes;
        }
        lines.push_back(std::move(line));
    }
    return td_join(lines, "\n");
}

namespace {

void td_walk_render(kimix::span<const todo_item> items, int32_t depth,
                    size_t max_lines, kimix::vector<kimix::string> &lines) {
    for (const todo_item &t : items) {
        if (lines.size() >= max_lines) {
            return;
        }
        kimix::string line(static_cast<size_t>(depth) * 2, ' ');
        line += "- [";
        line += status_name(t.status);
        line += "] ";
        line += t.content;
        if (t.status == todo_status::in_progress && t.notes.has_value()) {
            line += "  Notes: ";
            line += *t.notes;
        }
        lines.push_back(std::move(line));
        td_walk_render(
            kimix::span<const todo_item>(t.children.data(), t.children.size()),
            depth + 1, max_lines, lines);
    }
}

} // namespace

kimix::string render_read_tree(kimix::span<const todo_item> items,
                               int32_t max_lines) {
    kimix::vector<kimix::string> lines;
    td_walk_render(items, 0, static_cast<size_t>(std::max(max_lines, 0)), lines);
    return td_join(lines, "\n");
}

kimix::vector<display_item>
build_display_items(kimix::span<const todo_item> items) {
    kimix::vector<display_item> out;
    struct walker {
        static void walk(kimix::span<const todo_item> nodes, int32_t depth,
                         kimix::vector<display_item> &items) {
            for (const todo_item &t : nodes) {
                display_item d;
                d.title = t.content;
                d.status = t.status;
                d.notes = t.notes;
                d.depth = depth;
                items.push_back(std::move(d));
                walk(kimix::span<const todo_item>(t.children.data(),
                                                  t.children.size()),
                     depth + 1, items);
            }
        }
    };
    walker::walk(items, 0, out);
    return out;
}

kimix::string truncate_prompt(kimix::string_view text, size_t max_len) {
    const size_t cps = utf8_code_point_count(text);
    if (cps <= max_len) {
        return kimix::string(text.data(), text.size());
    }
    const size_t half = max_len / 2;
    const size_t head_end = utf8_byte_offset_of_code_point(text, half);
    const size_t tail_start =
        utf8_byte_offset_of_code_point(text, cps - half);
    kimix::string out(text.substr(0, head_end));
    out += "...";
    out += kimix::string(text.substr(tail_start));
    return out;
}

// ---------------------------------------------------------------------------
// State (de)serialization + persistence
// ---------------------------------------------------------------------------

kimix::string serialize_state(const todo_state &state) {
    ToolParams root;
    root.values["todos"] = td_items_to_array(state.todos);
    root.values["archived_todos"] = td_items_to_array(state.archived_todos);
    kimix::vector<char> buf;
    root.serialize(buf);
    return kimix::string(buf.data(), buf.size());
}

bool deserialize_state(kimix::string_view json, todo_state &out,
                       kimix::string &error) {
    out = todo_state{};
    ToolParams root;
    if (!root.try_deserialize(kimix::span<char const>(json.data(), json.size()),
                              error)) {
        return false;
    }
    auto load_array = [&](const char *key,
                          kimix::vector<todo_item> &dst) -> bool {
        const ValueElement *el = root.get(kimix::string_view(key));
        if (el == nullptr || el->is_null()) {
            return true;
        }
        if (!el->is_array()) {
            error = kimix::string(key) + " must be an array";
            return false;
        }
        for (const ValueElement &iel : el->as_array()) {
            todo_item item;
            if (td_item_from_value(iel, item)) {
                dst.push_back(std::move(item));
            }
            // Malformed items are skipped ("Skipping malformed todo item").
        }
        return true;
    };
    if (!load_array("todos", out.todos)) {
        return false;
    }
    if (!load_array("archived_todos", out.archived_todos)) {
        return false;
    }
    return true;
}

kimix::string state_file_path(kimix::string_view state_dir) {
    const kimix::filesystem::path p =
        kimix::filesystem::path(kimix::string(state_dir)) / "state.json";
    return kimix::to_string(p);
}

bool save_state_file(kimix::string_view path, const todo_state &state,
                     kimix::string &error) {
    // Merge into an existing JSON object so unknown sibling keys survive
    // (subagent-state merge behaviour); a corrupt file starts fresh.
    ToolParams root;
    kimix::string existing;
    if (td_read_file(path, existing) && !existing.empty()) {
        kimix::string perr;
        if (!root.try_deserialize(
                kimix::span<char const>(existing.data(), existing.size()),
                perr)) {
            root.values.clear();
        }
    }
    root.values["todos"] = td_items_to_array(state.todos);
    root.values["archived_todos"] = td_items_to_array(state.archived_todos);
    kimix::vector<char> buf;
    root.serialize(buf);

    std::error_code ec;
    const kimix::filesystem::path target{kimix::string(path)};
    const kimix::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        kimix::filesystem::create_directories(parent, ec); // best effort
    }
    const kimix::string tmp = kimix::string(path) + ".tmp";
    if (!td_write_file(tmp, buf, error)) {
        return false;
    }
    kimix::filesystem::rename(kimix::filesystem::path(tmp), target, ec);
    if (ec) {
        // Windows: retry with an explicit remove (rename should replace, but
        // locked/stale targets occasionally refuse).
        std::error_code rc;
        kimix::filesystem::remove(target, rc);
        kimix::filesystem::rename(kimix::filesystem::path(tmp), target, ec);
        if (ec) {
            std::error_code kc;
            kimix::filesystem::remove(kimix::filesystem::path(tmp), kc);
            const std::string m = ec.message();
            error = "failed to rename state file: " +
                    kimix::string(m.data(), m.size());
            return false;
        }
    }
    return true;
}

bool load_state_file(kimix::string_view path, todo_state &out,
                     kimix::string &error) {
    out = todo_state{};
    std::error_code ec;
    if (!kimix::filesystem::exists(kimix::filesystem::path(kimix::string(path)),
                                   ec)) {
        return true; // missing file -> empty state
    }
    kimix::string text;
    if (!td_read_file(path, text)) {
        error = "cannot read state file: " + kimix::string(path);
        return false;
    }
    return deserialize_state(text, out, error);
}

todo_state &session_todos(builtin_tools::Session &session, bool force_reload,
                          kimix::string *warning) {
    bool fresh = false;
    if (!session.todo_state) {
        session.todo_state = kimix::shared_ptr<todo_state>(new todo_state());
        fresh = true;
    }
    if ((fresh || force_reload) && !session.state_dir.empty()) {
        todo_state loaded;
        kimix::string err;
        if (load_state_file(state_file_path(session.state_dir), loaded, err)) {
            *session.todo_state = std::move(loaded);
        } else if (warning != nullptr) {
            *warning = "Corrupted todo state, using defaults: " + err;
        }
        // Corrupt file -> keep the empty defaults (Python parity).
    }
    return *session.todo_state;
}

tool_error persist_session_todos(builtin_tools::Session &session) {
    tool_error ok;
    if (session.state_dir.empty() || !session.todo_state) {
        return ok; // in-memory only
    }
    kimix::string err;
    if (!save_state_file(state_file_path(session.state_dir),
                         *session.todo_state, err)) {
        return tool_error{tool_status::blocked,
                          "Error: Failed to save root todos: " + err};
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Parameter parsing
// ---------------------------------------------------------------------------

bool parse_write_mode(kimix::string_view v, write_mode &mode,
                      bool &force_from_mode) noexcept {
    force_from_mode = false;
    // Legacy force spellings: strip -> lower -> '-'/' ' -> '_'.
    kimix::string n = td_lower(td_trim_view(v));
    for (char &c : n) {
        if (c == '-' || c == ' ') {
            c = '_';
        }
    }
    if (n == "force_overwrite" || n == "force_override" || n == "force" ||
        n == "forcewrite" || n == "forceoverride") {
        mode = write_mode::replace;
        force_from_mode = true;
        return true;
    }
    // Canonical map (_MODE_MAP): strip -> lower -> '-' -> '_'.
    kimix::string m = td_lower(td_trim_view(v));
    for (char &c : m) {
        if (c == '-') {
            c = '_';
        }
    }
    if (m == "append") {
        mode = write_mode::append;
        return true;
    }
    if (m == "replace" || m == "overwrite") {
        mode = write_mode::replace;
        return true;
    }
    if (m == "clear") {
        mode = write_mode::clear;
        return true;
    }
    return false;
}

// Fuzzy alias matching (tool.h): the alternate argument names the model may
// send instead of the documented one ("items" for "todos", ...). The canonical
// name always wins; the explicit AliasChoices loops below (which also cover the
// legacy {"replace": true} mode keys) stay as a second chance.
static const kimix::builtin_tools::param_alias k_todo_write_aliases[] = {
    {"todos", "items tasks todo_list todos_list task_list list entries"},
    {"mode", "write_mode action operation"},
    {"force", "force_overwrite force_flag"},
    {"auto_fix", "autofix fix_conflicts auto_fix_conflicts"},
};

bool parse_write_params(const ToolParams *params, write_params &out,
                        tool_response &err) {
    // Fuzzy alias matching (tool.h): "items" is accepted for "todos".
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_todo_write_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = write_params{};
    if (params == nullptr) {
        return true; // read mode
    }
    // ── todos / items (+ FIELD_ALIASES_TODO) ──
    const ValueElement *tel = td_first_present(
        params,
        {"todos", "items", "list", "tasks", "entries", "todo_list", "task_list"});
    if (tel != nullptr && !tel->is_null()) {
        ValueElement wrapped; // storage for the string-embedded case
        const ValueElement *src = tel;
        if (src->is_string()) {
            kimix::string detail;
            if (!td_parse_embedded_json(src->as_string(), "todos", wrapped,
                                        detail)) {
                err = td_validation_error(
                    "todos must be a list of todos, a single todo "
                    "dict/object, or None");
                return false;
            }
            src = &wrapped;
        }
        out.has_todos = true;
        if (src->is_array()) {
            const ValueElement::Array &arr = src->as_array();
            for (size_t i = 0; i < arr.size(); ++i) {
                todo_item item;
                kimix::string detail;
                if (!td_parse_item(arr[i], item, detail)) {
                    const kimix::string msg = kimix::format(
                        "Invalid todo at index {}: {}", i, detail);
                    err = td_validation_error(msg);
                    return false;
                }
                out.todos.push_back(std::move(item));
            }
        } else if (src->is_object()) {
            todo_item item;
            kimix::string detail;
            if (!td_parse_item(*src, item, detail)) {
                err = td_validation_error(
                    kimix::string("Invalid todo: ") + detail);
                return false;
            }
            out.todos.push_back(std::move(item));
        } else {
            err = td_validation_error(
                "todos must be a list of todos, a single todo dict/object, or "
                "None");
            return false;
        }
    }
    // ── mode (+ legacy force translation + FIELD_ALIASES_TODO) ──
    bool force_from_mode = false;
    const ValueElement *mel = params->get("mode");
    if (mel == nullptr) {
        for (const char *alias :
             {"replace", "override", "overwrite", "append", "merge", "update"}) {
            // FIELD_ALIASES_TODO maps these keys onto `mode`. The alias only
            // renames the *key*: the value still has to be a mode string, so a
            // non-string value is the same error as a non-string `mode`
            // (Python's Literal["append","replace","clear"] rejects
            // {"replace": true} - the port must not accept it either).
            const ValueElement *ael = params->get(kimix::string_view(alias));
            if (ael == nullptr) {
                continue;
            }
            mel = ael;
            break; // first present alias wins (repair parity)
        }
    }
    if (mel != nullptr) {
        if (!mel->is_string()) {
            err = td_validation_error(
                "Invalid mode. Must be 'append', 'replace', or 'clear'.");
            return false;
        }
        write_mode m = write_mode::append;
        bool ffm = false;
        if (!parse_write_mode(mel->as_string(), m, ffm)) {
            err = td_validation_error(kimix::format(
                "Invalid mode '{}'. Must be 'append', 'replace', or 'clear'.",
                mel->as_string()));
            return false;
        }
        out.mode = m;
        force_from_mode = force_from_mode || ffm;
    }
    // ── force / auto_fix ──
    if (const ValueElement *fel = params->get("force");
        fel != nullptr && !fel->is_null()) {
        bool v = false;
        if (!td_coerce_bool(*fel, v)) {
            err = td_validation_error(
                "Input should be a valid boolean (force)");
            return false;
        }
        out.force = v;
    }
    if (force_from_mode) {
        out.force = true; // legacy force_* modes force unconditionally
    }
    if (const ValueElement *ael = params->get("auto_fix");
        ael != nullptr && !ael->is_null()) {
        bool v = true;
        if (!td_coerce_bool(*ael, v)) {
            err = td_validation_error(
                "Input should be a valid boolean (auto_fix)");
            return false;
        }
        out.auto_fix = v;
    }
    return true;
}

// Fuzzy alias matching (tool.h): aliases of the TodoUpdate parameters
// (FIELD_ALIASES_TODO: `content`/`task`/`todo`/`item`/`name` for `title`, ...).
static const kimix::builtin_tools::param_alias k_todo_update_aliases[] = {
    {"title", "content task todo item name"},
    {"status", "state"},
    {"notes", "note description details"},
    {"rename_to", "new_title rename title_new"},
    {"parent", "parent_title parent_name"},
    {"fuzzy", "fuzzy_match approximate near_match"},
    {"force", "force_overwrite force_flag reopen"},
    {"complete", "done mark_done complete_subtree"},
    {"updates", "ops operations edits changes"},
};

bool parse_update_params(const ToolParams *params, update_params &out,
                         tool_response &err) {
    // Fuzzy alias matching (tool.h): "content" is accepted for "title" and
    // "state" for "status"; the canonical name always wins.
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_todo_update_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = update_params{};
    auto fail_no_title = [&]() {
        err.status = tool_status::invalid_input;
        err.is_error = true;
        err.output =
            "Error: title is required when updates is not provided.\nHint: "
            "Provide a title for a single update, or pass updates=[...] for "
            "multiple updates.";
        err.message = "No todo title provided.";
    };
    if (params == nullptr) {
        fail_no_title();
        return false;
    }
    // ── updates / todos (+ aliases) ──
    const ValueElement *uel = td_first_present(
        params, {"updates", "todos", "items",     "list",   "tasks",
                 "entries", "todo_list", "task_list", "edits", "changes",
                 "operations", "actions", "modifications", "batch"});
    ValueElement wrapped; // storage for the string-embedded case
    bool have_updates = false;
    if (uel != nullptr && !uel->is_null()) {
        const ValueElement *src = uel;
        bool resolved = true;
        if (src->is_string()) {
            kimix::string detail;
            if (td_parse_embedded_json(src->as_string(), "updates", wrapped,
                                       detail)) {
                src = &wrapped;
            } else {
                // Not JSON: a bare title for a single update
                // (TodoUpdateParams._validate_updates ->
                //  TodoUpdateItem(title=stripped)).
                const kimix::string stripped = td_trim(src->as_string());
                if (stripped.empty()) {
                    err = td_validation_error(
                        "updates title string cannot be empty");
                    return false;
                }
                update_op op;
                op.title = stripped;
                out.ops.push_back(std::move(op));
                resolved = false;
            }
        }
        have_updates = true;
        if (!resolved) {
            // Bare-title string: fall through to the mixed-field / common
            // parent handling below.
        } else if (src->is_array()) {
            const ValueElement::Array &arr = src->as_array();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (arr[i].is_string()) {
                    // Bare title in a batch (updates=["A", "B"]): a title-only
                    // item, matching TodoUpdateItem(title=item) semantics
                    // (min_length is checked before the strip validator, so a
                    // whitespace-only title is NOT rejected here).
                    update_op op;
                    op.title = td_trim(arr[i].as_string());
                    out.ops.push_back(std::move(op));
                    continue;
                }
                const ToolParams *obj = arr[i].as_object();
                if (obj == nullptr) {
                    err = td_validation_error(kimix::format(
                        "Invalid update at index {}: expected a dict or "
                        "TodoUpdateItem",
                        i));
                    return false;
                }
                update_op op;
                kimix::string detail;
                if (!td_parse_op(*obj, op, detail)) {
                    err = td_validation_error(kimix::format(
                        "Invalid update at index {}: {}", i, detail));
                    return false;
                }
                out.ops.push_back(std::move(op));
            }
        } else if (src->is_object()) {
            update_op op;
            kimix::string detail;
            if (!td_parse_op(*src->as_object(), op, detail)) {
                err = td_validation_error(
                    kimix::string("Invalid update: ") + detail);
                return false;
            }
            out.ops.push_back(std::move(op));
        } else {
            err = td_validation_error(
                "updates must be a list of updates, a single update "
                "dict/object, or None");
            return false;
        }
    }
    if (have_updates) {
        // Cannot mix top-level single-edit fields with `updates`.
        const kimix::vector<kimix::string> mixed = td_mixed_fields(params);
        if (!mixed.empty()) {
            const kimix::string mixed_repr = td_repr_list(mixed);
            err = td_validation_error(kimix::format(
                "Cannot mix top-level {} with `updates`; pass all edits "
                "inside `updates`.",
                mixed_repr));
            return false;
        }
        // Common parent applies to items without their own parent.
        if (const ValueElement *pel = params->get("parent");
            pel != nullptr && !pel->is_null()) {
            if (!pel->is_string()) {
                err = td_validation_error(
                    "Input should be a valid string (parent)");
                return false;
            }
            const kimix::string common_parent = td_trim(pel->as_string());
            for (update_op &op : out.ops) {
                if (!op.has_parent) {
                    op.has_parent = true;
                    op.parent = common_parent;
                }
            }
        }
        return true;
    }
    // ── single top-level update ──
    const ValueElement *tel = td_first_present(
        params, {"title", "content", "task", "todo", "item", "name"});
    if (tel == nullptr || tel->is_null()) {
        fail_no_title();
        return false;
    }
    update_op op;
    kimix::string detail;
    if (!td_parse_op(*params, op, detail)) {
        err = td_validation_error(detail);
        return false;
    }
    out.ops.push_back(std::move(op));
    return true;
}

// ---------------------------------------------------------------------------
// write flow (_write_todos)
// ---------------------------------------------------------------------------

commit_result write_todos(const todo_state &old, const write_params &params,
                          kimix::string_view current_prompt) {
    const kimix::vector<todo_item> &new_todos = params.todos;
    const bool had_old = !old.todos.empty();

    // 0. mode='clear' cannot be combined with todos.
    if (params.mode == write_mode::clear && !new_todos.empty()) {
        return td_fail(td_error(
            tool_status::invalid_input,
            "Error: mode='clear' cannot be combined with todos. Use "
            "mode='append' or 'replace' to write todos, or call with no "
            "todos to read.",
            "mode='clear' cannot be combined with todos."));
    }
    // 1. Validate the new input.
    if (params.mode != write_mode::clear) {
        const kimix::vector<kimix::string> duplicates =
            find_duplicate_titles(new_todos);
        if (!duplicates.empty()) {
            const kimix::string msg =
                "Duplicate todo titles found: " + td_repr_list(duplicates);
            return td_fail(td_error(
                tool_status::invalid_input, "Error: " + msg, msg,
                "todo_update(parent=...) to target a specific duplicate, or "
                "todo_write to read the tree."));
        }
        if (count_all(new_todos) > k_max_todos) {
            const kimix::string msg = kimix::format(
                "Todo list exceeds maximum limit of {} items.", k_max_todos);
            return td_fail(td_error(tool_status::too_large, "Error: " + msg, msg));
        }
    }

    // 2/3. Branch on write mode.
    kimix::vector<todo_item> final_todos;
    kimix::vector<kimix::string> warnings;
    bool replaces_list = false;
    if (params.mode == write_mode::clear) {
        if (had_old && !td_all_root_done(old.todos) && !params.force) {
            tool_response resp = td_error(
                tool_status::blocked,
                "Error: Cannot clear todos while old todos are not all done. "
                "Next step: mark them done first, or call with mode='clear' "
                "and force=True to discard them intentionally.\nUnfinished:\n" +
                    td_unfinished_lines(old.todos),
                "Cannot clear todos while old todos are not all done.");
            resp.display = build_display_items(old.todos);
            return td_fail(std::move(resp));
        }
        replaces_list = true;
    } else if (params.mode == write_mode::replace) {
        if (had_old && !td_all_root_done(old.todos) && !params.force) {
            return td_fail(td_error(
                tool_status::blocked,
                "Error: Cannot replace todos while old todos are not all "
                "done. Use force=True if you really want to discard "
                "unfinished work.\nUnfinished:\n" +
                    td_unfinished_lines(old.todos),
                "Cannot replace todos while old todos are not all done."));
        }
        final_todos = new_todos;
        replaces_list = true;
    } else { // append
        if (new_todos.empty()) {
            // Explicitly empty list: no-op response (never persisted).
            const status_count counts = status_counts(old.todos);
            const int32_t total = count_all(old.todos);
            tool_response resp;
            resp.output = "Todo list unchanged; no todos provided " +
                          td_stats(counts, total);
            const kimix::string active = format_todos(old.todos);
            if (!active.empty()) {
                resp.output += "\n" + active;
            }
            if (total > 0) {
                resp.output += "\nNext: ";
                resp.output += k_success_hint;
            }
            resp.message = "No todos provided; todo list unchanged.";
            if (had_old) {
                resp.display = build_display_items(old.todos);
            }
            commit_result cr;
            cr.response = std::move(resp);
            cr.commit = false;
            return cr;
        }
        if (had_old) {
            warnings = td_detect_fuzzy_warnings(new_todos, old.todos);
            const kimix::vector<kimix::string> scope_warnings =
                td_detect_scope_duplicates(new_todos, old.todos);
            warnings.insert(warnings.end(), scope_warnings.begin(),
                            scope_warnings.end());
        }
        final_todos = had_old ? td_merge_by_title_update(old.todos, new_todos)
                              : new_todos;
    }

    // 3b. Maximum tree nesting depth.
    const int32_t max_depth = params.max_layers + 1;
    if (max_tree_depth(final_todos) > max_depth) {
        tool_response resp = td_error(
            tool_status::blocked,
            kimix::format(
                "Error: Todo tree exceeds maximum nesting depth of {} levels "
                "(todo_max_layers={}). Flatten the tree, or build it with "
                "todo_write/todo_update(parent=...).",
                max_depth, params.max_layers),
            kimix::format(
                "Todo tree exceeds maximum nesting depth of {} levels.",
                max_depth));
        resp.display = build_display_items(final_todos);
        return td_fail(std::move(resp));
    }

    // 4. Regression detection (done -> pending/in_progress). Python assigns the
    //    clamped tree back to `final_todos`, so the error display shows the
    //    regressed items still marked done.
    if (!params.force && params.mode != write_mode::clear && had_old) {
        kimix::vector<todo_item> clamped;
        kimix::vector<kimix::string> regressions;
        td_check_regressions(old.todos, final_todos, clamped, regressions);
        final_todos = std::move(clamped);
        if (!regressions.empty()) {
            tool_response resp = td_error(
                tool_status::blocked,
                "Error: Cannot regress completed todos back to "
                "pending/in_progress: " +
                    td_join(regressions, ", ") +
                    "\nNext step: resend with these items kept as 'done', or "
                    "use force=True to restart them intentionally.",
                "Cannot regress completed todos.");
            resp.display = build_display_items(final_todos);
            return td_fail(std::move(resp));
        }
    }

    // 5. Archive completed todos dropped by replace/clear.
    kimix::vector<todo_item> archived = old.archived_todos;
    if (replaces_list && had_old) {
        kimix::unordered_set<kimix::string, kimix::string_hash> kept_titles;
        for (const todo_item &t : final_todos) {
            kept_titles.insert(t.content);
        }
        for (const todo_item &t : old.todos) {
            if (t.status == todo_status::done &&
                kept_titles.find(t.content) == kept_titles.end()) {
                archived.push_back(t);
            }
        }
        if (static_cast<int32_t>(archived.size()) > k_max_archived_todos) {
            archived.erase(
                archived.begin(),
                archived.begin() +
                    (static_cast<size_t>(archived.size()) - k_max_archived_todos));
        }
    }

    // 5b. Single in_progress invariant.
    if (!params.force && params.mode != write_mode::clear) {
        const kimix::vector<kimix::string> conflicts =
            find_in_progress_conflicts(final_todos);
        if (!conflicts.empty()) {
            if (params.auto_fix) {
                auto_fix_in_progress(final_todos, warnings);
            } else {
                tool_response resp = td_error(
                    tool_status::blocked,
                    "Error: Multiple items are in_progress: " +
                        td_repr_list(conflicts) +
                        ". Keep exactly one item in_progress at a time. Mark "
                        "the current item as 'done' before starting another, "
                        "use force=True to override, or set auto_fix=True to "
                        "automatically resolve conflicts.",
                    "Multiple items in_progress");
                resp.display = build_display_items(final_todos);
                return td_fail(std::move(resp));
            }
        }
    }

    // 6/7. Success response.
    const status_count counts = status_counts(final_todos);
    const int32_t total = count_all(final_todos);
    const char *mode_msg = (params.mode == write_mode::append)
                               ? "appended"
                               : (params.mode == write_mode::replace)
                                     ? "replaced"
                                     : "cleared";
    const kimix::string reminder = td_all_done_reminder(current_prompt);
    const bool all_done_cond =
        counts.pending == 0 && counts.in_progress == 0 && !final_todos.empty();

    kimix::vector<kimix::string> output_lines;
    const kimix::string stats = td_stats(counts, total);
    output_lines.push_back(
        kimix::format("Todo list {} {}", mode_msg, stats));
    const kimix::string active = format_todos(final_todos);
    if (!active.empty()) {
        output_lines.push_back(active);
    }
    if (all_done_cond) {
        output_lines.push_back(reminder);
    }
    tool_response resp;
    resp.output = td_join(output_lines, "\n");
    if (total > 0) {
        resp.output += "\nNext: ";
        resp.output += k_success_hint;
    }

    kimix::vector<kimix::string> message_lines;
    message_lines.push_back(kimix::format("Todo list {}.", mode_msg));
    if (all_done_cond) {
        message_lines.push_back(reminder);
    }
    if (params.force && had_old) {
        message_lines.push_back(
            "Warning: force=True bypassed the all-done guard and replaced the "
            "existing todo list.");
    }
    if (counts.in_progress > 1) {
        message_lines.push_back(
            kimix::format("Note: {} items are in_progress; prefer exactly one "
                          "at a time.",
                          counts.in_progress));
    }
    if (!warnings.empty()) {
        message_lines.push_back("");
        message_lines.insert(message_lines.end(), warnings.begin(),
                             warnings.end());
    }
    resp.message = td_join(message_lines, "\n");
    resp.display = build_display_items(final_todos);

    commit_result cr;
    cr.response = std::move(resp);
    cr.commit = true;
    cr.todos = std::move(final_todos);
    cr.archived = std::move(archived);
    return cr;
}

// ---------------------------------------------------------------------------
// read flow (_read_todos)
// ---------------------------------------------------------------------------

tool_response read_todos(const todo_state &state,
                         kimix::string_view current_prompt) {
    tool_response r;
    if (state.todos.empty()) {
        kimix::vector<kimix::string> lines;
        lines.push_back("Todo list is empty.");
        if (!state.archived_todos.empty()) {
            lines.push_back(kimix::format("Archived: {} completed todo(s).",
                                          state.archived_todos.size()));
        }
        r.output = td_join(lines, "\n");
        r.output += "\nNext: ";
        r.output += k_success_hint;
        r.message = "Todo list is empty.";
        return r;
    }
    const status_count counts = status_counts(state.todos);
    const int32_t total = count_all(state.todos);
    const bool all_done =
        total > 0 && counts.pending == 0 && counts.in_progress == 0;

    kimix::vector<kimix::string> lines;
    lines.push_back("Current todo list:");
    const kimix::string tree = render_read_tree(state.todos, k_max_read_items);
    if (!tree.empty()) {
        lines.push_back(tree);
    }
    if (total > k_max_read_items) {
        lines.push_back(kimix::format(
            "... and {} more ({} pending, {} in_progress, {} done total)",
            total - k_max_read_items, counts.pending, counts.in_progress,
            counts.done));
    }
    if (!state.archived_todos.empty()) {
        lines.push_back(kimix::format("Archived: {} completed todo(s).",
                                      state.archived_todos.size()));
    }
    const kimix::string reminder = td_all_done_reminder(current_prompt);
    if (all_done) {
        lines.push_back(reminder);
    }
    kimix::string next_line = "Next: ";
    next_line += k_success_hint;
    lines.push_back(std::move(next_line));
    r.output = td_join(lines, "\n");
    r.message = all_done ? reminder : kimix::string("Current todo list displayed.");
    return r;
}

// ---------------------------------------------------------------------------
// update flow (todo_update.__call__)
// ---------------------------------------------------------------------------

commit_result update_todos(const todo_state &old, const update_params &params) {
    kimix::vector<todo_item> todos = old.todos;
    kimix::vector<kimix::string> warnings;
    kimix::vector<kimix::string> summaries;
    kimix::vector<kimix::string> messages;
    for (const update_op &op : params.ops) {
        td_apply_out applied;
        if (!td_apply_one_update(todos, op, warnings, params.max_layers,
                                 applied)) {
            return td_fail(std::move(applied.error));
        }
        summaries.push_back(std::move(applied.summary));
        messages.push_back(std::move(applied.message));
        // The single-in_progress invariant is always auto-fixed here.
        const kimix::vector<kimix::string> conflicts =
            find_in_progress_conflicts(todos);
        if (!conflicts.empty()) {
            auto_fix_in_progress(todos, warnings);
        }
    }

    kimix::vector<kimix::string> output_lines;
    output_lines.push_back("Current todo list:");
    const kimix::string tree = render_read_tree(todos, k_max_read_items);
    if (!tree.empty()) {
        output_lines.push_back(tree);
    }
    tool_response resp;
    resp.output = td_join(output_lines, "\n") + "\n" + td_join(summaries, "\n");
    resp.output += "\nNext: ";
    resp.output += k_update_next_hint;
    if (!warnings.empty()) {
        resp.output += "\n" + td_join(warnings, "\n");
    }
    resp.message = td_join(messages, "; ");
    resp.display = build_display_items(todos);

    commit_result cr;
    cr.response = std::move(resp);
    cr.commit = true;
    cr.todos = std::move(todos);
    cr.archived = old.archived_todos;
    return cr;
}

// ---------------------------------------------------------------------------
// Tool classes
// ---------------------------------------------------------------------------

void TodoToolBase::result_json(kimix::vector<char> &out) const {
    _result.serialize(out);
}

builtin_tools::Session *TodoToolBase::require_session() {
    if (_session != nullptr) {
        return _session;
    }
    set_error(tool_status::unsupported,
              "Error: the todo tools require a session.",
              "todo tools require a session");
    return nullptr;
}

void TodoToolBase::set_response(const tool_response &resp) {
    _result.values.clear();
    auto &r = _result.values;
    r["status"] =
        ValueElement::make_string(kimix::string(td_status_string(resp.status)));
    r["is_error"] = ValueElement::make_bool(resp.is_error);
    r["output"] = ValueElement::make_string(resp.output);
    r["message"] = ValueElement::make_string(resp.message);
    ValueElement::Array arr;
    for (const display_item &d : resp.display) {
        kimix::shared_ptr<ToolParams> obj(new ToolParams());
        obj->values["title"] = ValueElement::make_string(d.title);
        obj->values["status"] =
            ValueElement::make_string(kimix::string(status_name(d.status)));
        obj->values["notes"] = d.notes.has_value()
                                   ? ValueElement::make_string(*d.notes)
                                   : ValueElement::make_null();
        obj->values["depth"] = ValueElement::make_int(d.depth);
        arr.push_back(ValueElement::make_object(std::move(obj)));
    }
    r["todos"] = ValueElement::make_array(std::move(arr));
}

void TodoToolBase::set_error(tool_status st, kimix::string_view output,
                             kimix::string_view message) {
    tool_response resp;
    resp.status = st;
    resp.is_error = true;
    resp.output.assign(output.data(), output.size());
    resp.message.assign(message.data(), message.size());
    set_response(resp);
}

void TodoToolBase::run_read(const tool_response &resp) { set_response(resp); }

void TodoToolBase::run_flow(commit_result &cr, kimix::string_view save_hint) {
    if (!cr.response.is_error && cr.commit && _session != nullptr) {
        todo_state &st = session_todos(*_session);
        st.todos = std::move(cr.todos);
        st.archived_todos = std::move(cr.archived);
        const tool_error serr = persist_session_todos(*_session);
        if (serr.failed()) {
            tool_response r;
            r.status = serr.status;
            r.is_error = true;
            r.output = serr.message + "\nHint: " + kimix::string(save_hint);
            r.message = "Failed to save todos.";
            set_response(r);
            return;
        }
    }
    set_response(cr.response);
}

TodoWrite::TodoWrite(kimix::builtin_tools::Session *session)
    : TodoToolBase(session) {}

void TodoWrite::operator()(ToolParams const *parameters) {
    _result.values.clear();
    builtin_tools::Session *sess = require_session();
    if (sess == nullptr) {
        return;
    }
    write_params p;
    tool_response perr;
    if (!parse_write_params(parameters, p, perr)) {
        set_response(perr);
        return;
    }
    p.max_layers = max_layers;
    todo_state &st = session_todos(*sess);
    if (!p.has_todos) {
        run_read(read_todos(st, current_prompt));
        return;
    }
    commit_result cr = write_todos(st, p, current_prompt);
    run_flow(cr, k_default_error_hint);
}

TodoUpdate::TodoUpdate(kimix::builtin_tools::Session *session)
    : TodoToolBase(session) {}

void TodoUpdate::operator()(ToolParams const *parameters) {
    _result.values.clear();
    builtin_tools::Session *sess = require_session();
    if (sess == nullptr) {
        return;
    }
    update_params p;
    tool_response perr;
    if (!parse_update_params(parameters, p, perr)) {
        set_response(perr);
        return;
    }
    p.max_layers = max_layers;
    todo_state &st = session_todos(*sess);
    commit_result cr = update_todos(st, p);
    run_flow(cr, "Use todo_write to read the tree and retry todo_update.");
}

} // namespace kimix::builtin_tools::todo
