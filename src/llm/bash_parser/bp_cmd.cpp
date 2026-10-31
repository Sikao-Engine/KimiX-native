/* bp_cmd.cpp — cmd.exe translations: leading-@ rules, `if`/`for` control-flow
 * translation, and the untranslatable jump refusals.
 *
 * Line-for-line port of kimi-agent `shell_compat/cmd_translator.py` (the LIVE
 * path functions, see bp_cmd.h).  The IR-only rule_/Node functions are excluded
 * per the module README.
 */
#include "llm/bash_parser/bp_cmd.h"

#include "llm/bash_parser/bp_core.h"
#include "llm/bash_parser/bp_scanner.h"

namespace kimix::llm::bash_parser {
namespace src_llm_bash_parser_bp_cmd {

// Python `str.startswith` on a byte slice.

// byte-for-byte equality of two string_views (ASCII compare only; no locale).
inline bool eq_sv(kimix::string_view a, kimix::string_view b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Case-fold ASCII [A-Z] to lower; bytes >= 0x80 pass through unchanged
// (the documented conservative difference from Python `str.lower`).

// True when `c` appears in the NUL-terminated ASCII `set`.
inline bool char_in_set(char c, const char *set) {
    for (const char *p = set; *p != '\0'; ++p) {
        if (*p == c) {
            return true;
        }
    }
    return false;
}

// The reference `_HSPACE_RE.match(s, p, end)`: skip " \t\r" from `p`, bounded
// by `end`.  Returns the index past the run (== `p` when no hspace), matching
// Python `m.end() if m is not None else p`.
inline size_t skip_hspace(kimix::string_view s, size_t p, size_t end) {
    while (p < end && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) {
        p += 1;
    }
    return p;
}

// `bash_core._CMD_IF_TAIL_SAFE_CHARS` = frozenset(" \t\r\n;&|()<>").
inline bool in_cmd_if_tail_safe(char c) {
    return char_in_set(c, " \t\r\n;&|()<>");
}

// `bash_core._CMD_REL_OPS` = {equ:"==", neq:"!=", lss:"<", leq:"<=", gtr:">", geq:">="}.
inline kimix::optional<kimix::string_view> cmd_rel_op(kimix::string_view lit) {
    if (eq_sv(lit, "equ")) {
        return kimix::string_view{"=="};
    }
    if (eq_sv(lit, "neq")) {
        return kimix::string_view{"!="};
    }
    if (eq_sv(lit, "lss")) {
        return kimix::string_view{"<"};
    }
    if (eq_sv(lit, "leq")) {
        return kimix::string_view{"<="};
    }
    if (eq_sv(lit, "gtr")) {
        return kimix::string_view{">"};
    }
    if (eq_sv(lit, "geq")) {
        return kimix::string_view{">="};
    }
    return kimix::nullopt;
}

// The `cmd_cmp_side` plain-text rejection set (reference:
// `"\\\"'!$`()*?[]{}~<>;%&|"`).  A decoded string-compare value must hold none.
inline bool in_cmd_cmp_forbidden(char c) {
    static constexpr char kSet[] = {'\\', '"', '\'', '!', '$', '`', '(', ')', '*', '?', '[', ']', '{', '}', '~', '<', '>', ';', '%', '&', '|'};
    for (const char x : kSet) {
        if (x == c) {
            return true;
        }
    }
    return false;
}

} // namespace src_llm_bash_parser_bp_cmd
using namespace src_llm_bash_parser_bp_cmd;

// ── leading-@ rule ──────────────────────────────────────────────────────────

kimix::optional<AtPrefixDecision> at_prefix_rule(Scanner &reader, kimix::string_view source, size_t word_start, size_t word_end, size_t end) {
    const kimix::string_view s = source;
    const kimix::string_view raw = s.substr(word_start, word_end - word_start);
    if (raw.empty() || raw[0] != '@') {
        return kimix::nullopt;
    }
    const auto literal = Scanner::literal_word_value(raw);
    if (!literal.has_value() || literal->size() < 2) {
        return kimix::nullopt; // bare `@` or a non-decodable word
    }
    const char nxt = (*literal)[1];
    if (char_in_set(nxt, "@ \t\r") || char_in_set(nxt, "&|<>()")) {
        return kimix::nullopt; // `@@x`, `@ x`, an operator after `@`
    }
    if (bp_starts_with(bp_ascii_lower(*literal), "@echo.")) {
        return kimix::nullopt; // cmd `@echo.` blank-line idiom: keep it loud
    }
    if (eq_sv(bp_ascii_lower(*literal), "@echo") && raw[0] == '@') {
        const size_t hspace_end = skip_hspace(s, word_end, end);
        if (hspace_end > word_end) { // _HSPACE_RE.match returned a non-None run
            const size_t off_start = hspace_end;
            const size_t off_end = reader.read_word(off_start, end, false);
            if (off_end > off_start) {
                const auto off_lit = Scanner::literal_word_value(s.substr(off_start, off_end - off_start));
                if (off_lit.has_value() && (eq_sv(*off_lit, "off") || eq_sv(*off_lit, "on")) && at_boundary_ok(s, off_end, end)) {
                    AtPrefixDecision decision;
                    decision.index = off_end;
                    decision.statement = true;
                    decision.edit_start = word_start;
                    decision.edit_end = off_end;
                    decision.edit_replacement = eq_sv(*off_lit, "off") ? "set +x" : "set -x";
                    decision.note = kimix::string(s.substr(word_start, off_end - word_start));
                    return decision;
                }
            }
        }
        // NOTE: the reference's nested `literal.lower().startswith("@echo.")`
        // check inside this branch is unreachable (literal.lower() == "@echo"
        // exactly, never a "@echo." prefix) — omitted as dead code.
    }
    // raw[0] != '@' was already rejected above, so this is the strip case.
    AtPrefixDecision decision;
    decision.index = word_start + 1;
    decision.statement = false;
    decision.edit_start = word_start;
    decision.edit_end = word_start + 1;
    decision.edit_replacement.clear();
    return decision;
}

bool at_boundary_ok(kimix::string_view source, size_t pos, size_t end) {
    const kimix::string_view s = source;
    while (pos < end && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r')) {
        pos += 1;
    }
    if (pos >= end) {
        return true;
    }
    const char ch = s[pos];
    if (ch == '&') {
        return pos + 1 < end && s[pos + 1] == '&'; // `&&` ok, `&` not
    }
    return char_in_set(ch, "\n;|<>");
}

// ── if-comparison primitives ────────────────────────────────────────────────

kimix::optional<std::pair<kimix::string, kimix::string>> split_top_eq(kimix::string_view raw) {
    bool in_quote = false;
    const size_t n = raw.size();
    for (size_t i = 0; i < n; ++i) {
        const char ch = raw[i];
        if (ch == '"') {
            in_quote = !in_quote;
        } else if (!in_quote && ch == '=' && i + 1 < n && raw[i + 1] == '=') {
            return std::pair<kimix::string, kimix::string>{kimix::string{raw.substr(0, i)}, kimix::string{raw.substr(i + 2)}};
        }
    }
    return kimix::nullopt;
}

kimix::optional<CmpSide> cmd_cmp_side(kimix::string_view raw, bool folded, bool numeric) {
    bool quoted = false;
    kimix::string_view core = raw;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"' && raw.substr(1, raw.size() - 2).find('"') == kimix::string_view::npos) {
        quoted = true;
        core = raw.substr(1, raw.size() - 2);
    }
    if (core.size() >= 2 && core.front() == '%' && core.back() == '%') {
        size_t percent_count = 0;
        for (const char c : core) {
            if (c == '%') {
                percent_count += 1;
            }
        }
        if (percent_count == 2) {
            const kimix::string_view name = core.substr(1, core.size() - 2);
            if (eq_sv(bp_ascii_lower(name), "errorlevel")) {
                return CmpSide{"$?", quoted};
            }
            if (is_name_word(name)) {
                kimix::string operand = "${";
                operand.append(name);
                if (numeric) {
                    operand.push_back('}');
                    return CmpSide{std::move(operand), quoted};
                }
                if (folded) {
                    operand.append(",,}");
                } else {
                    operand.push_back('}');
                }
                return CmpSide{std::move(operand), quoted};
            }
            return kimix::nullopt; // %partial%, %~ni%, %%..%
        }
    }
    if (numeric) {
        if (!is_int_operand(core)) {
            return kimix::nullopt;
        }
        return CmpSide{kimix::string{core}, quoted};
    }
    if (core.empty()) {
        return kimix::nullopt; // an empty cmd operand is not supported here
    }
    for (const char c : core) {
        if (in_cmd_cmp_forbidden(c)) {
            return kimix::nullopt;
        }
    }
    kimix::string operand;
    operand.push_back('"');
    operand.append(folded ? bp_ascii_lower(core) : kimix::string{core});
    operand.push_back('"');
    return CmpSide{std::move(operand), quoted};
}

kimix::optional<kimix::string> build_cmd_cmp(kimix::string_view lhs_raw, kimix::string_view rhs_raw, kimix::string_view op, bool folded, bool numeric) {
    const auto lhs = cmd_cmp_side(lhs_raw, folded, numeric);
    const auto rhs = cmd_cmp_side(rhs_raw, folded, numeric);
    if (!lhs.has_value() || !rhs.has_value()) {
        return kimix::nullopt;
    }
    if (!numeric && lhs->was_cmd_quoted != rhs->was_cmd_quoted) {
        return kimix::nullopt; // cmd compares the quote characters themselves
    }
    if (numeric) {
        kimix::string out = lhs->operand;
        out.push_back(' ');
        out.append(op);
        out.push_back(' ');
        out.append(rhs->operand);
        return out;
    }
    kimix::string rhs_str;
    if (!rhs->operand.empty() && rhs->operand[0] == '"') {
        rhs_str = rhs->operand;
    } else {
        rhs_str.push_back('"');
        rhs_str.append(rhs->operand);
        rhs_str.push_back('"');
    }
    kimix::string out = lhs->operand;
    out.append(" == ");
    out.append(rhs_str);
    return out;
}

bool cmd_if_branch_ok(kimix::string_view source, size_t start, size_t stop) {
    const kimix::string_view body = source.substr(start, stop - start);
    if (body.find('\n') != kimix::string_view::npos) {
        return false;
    }
    bool all_space = true;
    for (const char c : body) {
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')) {
            all_space = false;
            break;
        }
    }
    if (all_space) {
        return false; // an empty/whitespace body: Bash rejects `then ;`/`else ;`
    }
    const size_t n = body.size();
    size_t i = 0;
    while (i < n) {
        const char ch = body[i];
        if (ch == '\\') {
            i += 2;
            continue;
        }
        if (ch == '\'') {
            const size_t close = body.find('\'', i + 1);
            if (close == kimix::string_view::npos) {
                return false;
            }
            i = close + 1;
            continue;
        }
        if (ch == '"') {
            i += 1;
            while (i < n && body[i] != '"') {
                if (body[i] == '\\') {
                    i += 1;
                }
                i += 1;
            }
            i += 1;
            continue;
        }
        if (ch == '#' && (i == 0 || char_in_set(body[i - 1], " \t;&|()<>"))) {
            return false;
        }
        i += 1;
    }
    return true;
}

// ── cmd `if` validate + commit ──────────────────────────────────────────────

kimix::optional<CmdIfMatch> validate_cmd_if(Scanner &reader, kimix::string_view source, size_t if_start, size_t pos, size_t end) {
    const kimix::string_view s = source;

    auto space = [&](size_t p) -> size_t { return skip_hspace(s, p, end); };

    struct Word {
        kimix::optional<kimix::string> lit;
        size_t start = 0;
        size_t end = 0;
    };
    auto word = [&](size_t p) -> Word {
        const size_t q = skip_hspace(s, p, end);
        const size_t r = reader.read_word(q, end, false);
        if (r <= q) {
            return Word{kimix::nullopt, q, q};
        }
        return Word{Scanner::literal_word_value(s.substr(q, r - q)), q, r};
    };

    CmdIfMatch match;
    size_t header_start = if_start;
    for (;;) {
        bool negated = false;
        bool folded = false;
        Word kw = word(pos);
        // Optional `not` and `/i` flags in either order; at most two iterations
        // (a duplicate flag bails), exactly like the reference `for _ in range(2)`.
        for (int _ = 0; _ < 2; ++_) {
            if (kw.lit.has_value() && eq_sv(*kw.lit, "not")) {
                if (negated) {
                    return kimix::nullopt;
                }
                negated = true;
                kw = word(kw.end);
            } else if (kw.lit.has_value() && eq_sv(*kw.lit, "/i")) {
                if (folded) {
                    return kimix::nullopt;
                }
                folded = true;
                kw = word(kw.end);
            } else {
                break;
            }
        }
        if (!kw.lit.has_value()) {
            return kimix::nullopt;
        }

        CmdIfSegment seg;
        seg.header_start = header_start;
        seg.negated = negated;
        seg.folded = folded;
        const kimix::string &kw_lit = *kw.lit;

        if (eq_sv(kw_lit, "exist")) {
            if (folded) {
                return kimix::nullopt; // `if /i exist` is invalid cmd
            }
            const size_t op_start = space(kw.end);
            const size_t op_end = reader.read_word(op_start, end, false);
            if (op_end <= op_start) {
                return kimix::nullopt;
            }
            const kimix::string_view operand_raw = s.substr(op_start, op_end - op_start);
            const auto operand_lit = Scanner::literal_word_value(operand_raw);
            if (!operand_lit.has_value() || operand_lit->empty()) {
                return kimix::nullopt;
            }
            // cmd `if exist` matches wildcards; `[[ -e ]]` tests the literal
            // name, so an operand holding `*`/`?`/`[` cannot be translated.
            for (const char c : operand_raw) {
                if (c == '*' || c == '?' || c == '[') {
                    return kimix::nullopt;
                }
            }
            // A quoted operand that still contains backslashes bails (quotes
            // are meaningless to cmd.exe but make the word literal data for
            // Bash, and the scanner never rewrites quoted words).
            if (!operand_raw.empty() && (operand_raw[0] == '\'' || operand_raw[0] == '"') && operand_raw.find('\\') != kimix::string_view::npos) {
                return kimix::nullopt;
            }
            seg.kind = CmdIfKind::kExist;
            seg.op_start = op_start;
            seg.op_end = op_end;
        } else if (eq_sv(kw_lit, "defined")) {
            if (folded) {
                return kimix::nullopt;
            }
            const size_t op_start = space(kw.end);
            const size_t op_end = reader.read_word(op_start, end, false);
            if (op_end <= op_start || !is_name_word(s.substr(op_start, op_end - op_start))) {
                return kimix::nullopt; // a defined operand must be a bare identifier
            }
            seg.kind = CmdIfKind::kDefined;
            seg.op_start = op_start;
            seg.op_end = op_end;
        } else if (eq_sv(kw_lit, "errorlevel")) {
            if (folded) {
                return kimix::nullopt;
            }
            const size_t op_start = space(kw.end);
            const size_t op_end = reader.read_word(op_start, end, false);
            if (op_end <= op_start || !is_errorlevel_operand(s.substr(op_start, op_end - op_start))) {
                return kimix::nullopt; // an errorlevel operand must be a plain integer
            }
            seg.kind = CmdIfKind::kErrorlevel;
            seg.op_start = op_start;
            seg.op_end = op_end;
        } else {
            // compare / arithmetic; the condition is the token(s) at kw_start.
            seg.op_start = kw.start;
            const kimix::string_view cond_raw = s.substr(kw.start, kw.end - kw.start);
            const auto eq_split = split_top_eq(cond_raw);
            if (eq_split.has_value()) {
                auto cmp_text = build_cmd_cmp(eq_split->first, eq_split->second, "==", folded, false);
                if (!cmp_text.has_value()) {
                    return kimix::nullopt;
                }
                seg.cmp_text = std::move(cmp_text);
                seg.op_end = kw.end;
                seg.kind = CmdIfKind::kCompare;
            } else {
                const Word rel = word(kw.end);
                kimix::string_view cmd_op;
                bool have_op = false;
                if (rel.lit.has_value()) {
                    const auto op = cmd_rel_op(*rel.lit);
                    if (op.has_value()) {
                        cmd_op = *op;
                        have_op = true;
                    }
                }
                if (!have_op) {
                    return kimix::nullopt; // neither glued == nor a relational word
                }
                const Word rhs = word(rel.end);
                if (!rhs.lit.has_value()) {
                    return kimix::nullopt;
                }
                auto cmp_text = build_cmd_cmp(cond_raw, s.substr(rhs.start, rhs.end - rhs.start), cmd_op, folded, true);
                if (!cmp_text.has_value()) {
                    return kimix::nullopt;
                }
                seg.cmp_text = std::move(cmp_text);
                seg.op_end = rhs.end;
                seg.kind = CmdIfKind::kArithmetic;
            }
        }

        const size_t open_pos = space(seg.op_end);
        if (open_pos >= end || s[open_pos] != '(') {
            return kimix::nullopt;
        }
        const size_t close_pos = reader.find_matching(open_pos + 1, end);
        if (close_pos >= end) {
            return kimix::nullopt;
        }
        if (!cmd_if_branch_ok(s, open_pos + 1, close_pos)) {
            return kimix::nullopt;
        }
        seg.open_pos = open_pos;
        seg.close_pos = close_pos;
        match.segments.push_back(std::move(seg));

        // Optional `else` tail.  The next word is probed across whitespace
        // including newlines; a keyword behind a newline is a multi-line cmd
        // conditional (out of scope, bail).
        size_t q = close_pos + 1;
        bool crossed_newline = false;
        while (q < end) {
            const char c = s[q];
            if (c == ' ' || c == '\t' || c == '\r') {
                q += 1;
            } else if (c == '\n') {
                crossed_newline = true;
                q += 1;
            } else {
                break;
            }
        }
        const Word els = word(q);
        if (!(els.lit.has_value() && eq_sv(*els.lit, "else"))) {
            if (close_pos + 1 < end && !in_cmd_if_tail_safe(s[close_pos + 1])) {
                return kimix::nullopt; // a word glued to `)` would corrupt the `; fi`
            }
            return match;
        }
        if (crossed_newline) {
            return kimix::nullopt;
        }
        const size_t nxt = space(els.end);
        if (nxt >= end) {
            return kimix::nullopt; // dangling `else`
        }
        if (s[nxt] == '(') {
            const size_t else_close = reader.find_matching(nxt + 1, end);
            if (else_close >= end) {
                return kimix::nullopt;
            }
            if (!cmd_if_branch_ok(s, nxt + 1, else_close)) {
                return kimix::nullopt;
            }
            if (else_close + 1 < end && !in_cmd_if_tail_safe(s[else_close + 1])) {
                return kimix::nullopt;
            }
            match.else_open = nxt;
            match.else_close = else_close;
            return match;
        }
        const Word ifw = word(nxt);
        if (!(ifw.lit.has_value() && eq_sv(*ifw.lit, "if"))) {
            return kimix::nullopt; // bare command branch: extent is ambiguous
        }
        header_start = els.start;
        pos = ifw.end;
    }
}

size_t commit_cmd_if(Scanner &reader, kimix::string_view source, const CmdIfMatch &match) {
    const kimix::string_view s = source;
    const size_t last = match.segments.size() - 1;
    for (size_t index = 0; index <= last; ++index) {
        const CmdIfSegment &seg = match.segments[index];
        const kimix::string_view keyword = index == 0 ? kimix::string_view{"if"} : kimix::string_view{"elif"};
        const bool neg = seg.negated;
        switch (seg.kind) {
        case CmdIfKind::kDefined: {
            kimix::string head{keyword};
            head.append(" [[ ");
            if (neg) {
                head.append("! ");
            }
            head.append("-n ${");
            reader.record_edit(seg.header_start, seg.op_start, head);
            reader.record_edit(seg.op_end, seg.open_pos + 1, "+x} ]]; then ");
            break;
        }
        case CmdIfKind::kErrorlevel: {
            kimix::string head{keyword};
            head.append(" (( ");
            head.append(neg ? "$? < " : "$? >= ");
            reader.record_edit(seg.header_start, seg.op_start, head);
            reader.record_edit(seg.op_end, seg.open_pos + 1, " )); then ");
            break;
        }
        case CmdIfKind::kCompare: {
            kimix::string head{keyword};
            head.append(" [[ ");
            if (neg) {
                head.append("! ");
            }
            reader.record_edit(seg.header_start, seg.op_start, head);
            reader.record_edit(seg.op_end, seg.open_pos + 1, " ]]; then ");
            reader.record_edit(seg.op_start, seg.op_end, *seg.cmp_text);
            break;
        }
        case CmdIfKind::kArithmetic: {
            if (seg.negated) {
                kimix::string head{keyword};
                head.append(" (( ! (");
                reader.record_edit(seg.header_start, seg.op_start, head);
                reader.record_edit(seg.op_end, seg.open_pos + 1, " ) )); then ");
            } else {
                kimix::string head{keyword};
                head.append(" (( ");
                reader.record_edit(seg.header_start, seg.op_start, head);
                reader.record_edit(seg.op_end, seg.open_pos + 1, " )); then ");
            }
            reader.record_edit(seg.op_start, seg.op_end, *seg.cmp_text);
            break;
        }
        default: { // `exist` (the landed default)
            kimix::string test{keyword};
            test.append(seg.negated ? " [[ ! -e " : " [[ -e ");
            reader.record_edit(seg.header_start, seg.op_start, test);
            const kimix::string_view operand = s.substr(seg.op_start, seg.op_end - seg.op_start);
            const auto replacement = reader.path_replacement_for(operand);
            if (replacement.has_value()) {
                reader.record_edit(seg.op_start, seg.op_end, *replacement);
                reader.record_path(kimix::string{operand});
            }
            reader.record_edit(seg.op_end, seg.open_pos + 1, " ]]; then ");
            break;
        }
        }
        reader.scan_range(seg.open_pos + 1, seg.close_pos);
        if (!match.else_open.has_value() && index == last) {
            reader.record_edit(seg.close_pos, seg.close_pos + 1, "; fi");
        } else {
            reader.record_edit(seg.close_pos, seg.close_pos + 1, ";");
        }
    }
    if (!match.else_open.has_value()) {
        return match.segments[last].close_pos + 1;
    }
    const size_t else_open = *match.else_open;
    const size_t else_close = *match.else_close;
    // Delete the final `else (` paren, keeping the (valid bash) `else` keyword;
    // glue a space in when the source had `else(`.
    const char prev = s[else_open - 1];
    const bool had_space = prev == ' ' || prev == '\t' || prev == '\r';
    reader.record_edit(else_open, else_open + 1, had_space ? "" : " ");
    reader.scan_range(else_open + 1, else_close);
    reader.record_edit(else_close, else_close + 1, "; fi");
    return else_close + 1;
}

// ── cmd `for` validate + commit ─────────────────────────────────────────────

bool cmd_for_item_ok(kimix::string_view item) {
    if (!item.empty() && (item.front() == '\'' || item.front() == '"')) {
        return false;
    }
    if (!item.empty() && (item.back() == '\'' || item.back() == '"')) {
        return false;
    }
    for (const char c : item) {
        if (char_in_set(c, "$`!%^")) {
            return false;
        }
    }
    return true;
}

bool cmd_body_has_bare_ampersand(kimix::string_view body) {
    const size_t n = body.size();
    size_t i = 0;
    bool in_single = false;
    while (i < n) {
        const char ch = body[i];
        if (ch == '\\' && !in_single) {
            i += 2;
            continue;
        }
        if (ch == '\'') {
            in_single = !in_single;
            i += 1;
            continue;
        }
        if (in_single || ch != '&') {
            i += 1;
            continue;
        }
        size_t run = 1;
        while (i + run < n && body[i + run] == '&') {
            run += 1;
        }
        if (run == 1) {
            return true;
        }
        i += run;
    }
    return false;
}

kimix::optional<CmdForListEdits> validate_cmd_for_list(Scanner &reader, kimix::string_view source, size_t i, size_t close) {
    const kimix::string_view s = source;
    CmdForListEdits out;
    while (i < close) {
        const char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            i += 1;
            continue;
        }
        const size_t word_end = reader.read_word(i, close, false);
        if (word_end <= i) {
            i += 1;
            continue;
        }
        size_t seg_start = i;
        size_t k = i;
        while (k < word_end) {
            if (s[k] == ',' || k == word_end - 1) {
                const size_t item_end = (s[k] == ',') ? k : k + 1;
                const kimix::string_view item = s.substr(seg_start, item_end - seg_start);
                if (!item.empty() && !cmd_for_item_ok(item)) {
                    return kimix::nullopt;
                }
                if (!item.empty()) {
                    // The reference calls module-level path_replacement(item) with
                    // the DEFAULT temp dir; here we use the scanner's own temp dir.
                    const auto repl = reader.path_replacement_for(item);
                    if (repl.has_value()) {
                        out.list_path_edits.push_back(CmdForListEdits::PathEdit{seg_start, item_end, std::move(*repl)});
                    }
                }
                if (s[k] == ',') {
                    out.comma_spans.push_back(std::pair<size_t, size_t>{k, k + 1});
                    seg_start = k + 1;
                }
                k += 1;
                continue;
            }
            k += 1;
        }
        i = word_end;
    }
    return out;
}

kimix::optional<kimix::vector<std::pair<size_t, size_t>>> validate_cmd_for_body(Scanner &reader, kimix::string_view source, size_t start, size_t stop, kimix::string_view name) {
    const kimix::string_view s = source;
    const kimix::string_view body = s.substr(start, stop - start);
    if (body.find('^') != kimix::string_view::npos) {
        return kimix::nullopt;
    }
    if (cmd_body_has_bare_ampersand(body)) {
        return kimix::nullopt;
    }
    kimix::string single;
    single.push_back('%');
    single.append(name);
    kimix::string doubled;
    doubled.push_back('%');
    doubled.push_back('%');
    doubled.append(name);
    const kimix::string_view single_sv{single};
    const kimix::string_view doubled_sv{doubled};

    kimix::vector<std::pair<size_t, size_t>> var_edits;
    size_t i = start;
    while (i < stop) {
        const size_t word_end = reader.read_word(i, stop, false);
        if (word_end <= i) {
            i += 1;
            continue;
        }
        const kimix::string_view w = s.substr(i, word_end - i);
        if (w.find('%') != kimix::string_view::npos) {
            if (!(w == single_sv || w == doubled_sv)) {
                return kimix::nullopt;
            }
            var_edits.push_back(std::pair<size_t, size_t>{i, word_end});
        }
        i = word_end;
    }
    return var_edits;
}

kimix::optional<CmdForMatch> validate_cmd_for(Scanner &reader, kimix::string_view source, size_t for_start, size_t pos, size_t end) {
    const kimix::string_view s = source;

    auto space = [&](size_t p) -> size_t { return skip_hspace(s, p, end); };

    struct Word {
        kimix::optional<kimix::string> lit;
        size_t start = 0;
        size_t end = 0;
    };
    auto word = [&](size_t p) -> Word {
        const size_t q = skip_hspace(s, p, end);
        const size_t r = reader.read_word(q, end, false);
        if (r <= q) {
            return Word{kimix::nullopt, q, q};
        }
        return Word{Scanner::literal_word_value(s.substr(q, r - q)), q, r};
    };

    // 1. loop token
    const Word tok = word(pos);
    if (tok.end <= tok.start) {
        return kimix::nullopt;
    }
    const auto name_opt = for_loop_token_name(s.substr(tok.start, tok.end - tok.start));
    if (!name_opt.has_value()) {
        return kimix::nullopt;
    }
    const kimix::string name = std::move(*name_opt);

    // 2. `in`
    const Word inw = word(tok.end);
    if (!(inw.lit.has_value() && eq_sv(*inw.lit, "in"))) {
        return kimix::nullopt;
    }

    // 3. `(` .. matching `)` closing before the first newline
    const size_t open_pos = space(inw.end);
    if (open_pos >= end || s[open_pos] != '(') {
        return kimix::nullopt;
    }
    const size_t close_pos = reader.find_matching(open_pos + 1, end);
    if (close_pos >= end) {
        return kimix::nullopt;
    }
    for (size_t k = open_pos + 1; k <= close_pos; ++k) { // s.find("\n", open+1, close+1)
        if (s[k] == '\n') {
            return kimix::nullopt; // a list paren must not span lines
        }
    }
    const auto list_edits = validate_cmd_for_list(reader, s, open_pos + 1, close_pos);
    if (!list_edits.has_value()) {
        return kimix::nullopt;
    }

    // 4. whitespace then `do` (a glued `)do` is cmd-legal but bails v1)
    const Word dow = word(close_pos + 1);
    if (!(dow.lit.has_value() && eq_sv(*dow.lit, "do")) || dow.start <= close_pos + 1) {
        return kimix::nullopt;
    }

    // 5. body = rest of the physical line
    size_t line_end = end;
    for (size_t k = dow.end; k < end; ++k) { // s.find("\n", do_end, end)
        if (s[k] == '\n') {
            line_end = k;
            break;
        }
    }
    if (line_end <= dow.end) {
        return kimix::nullopt; // empty body
    }
    const size_t body_head = skip_hspace(s, dow.end, end);
    if (body_head < end && s[body_head] == '(') {
        return kimix::nullopt; // a parenthesised do-body drops shared context
    }
    if (!cmd_if_branch_ok(s, dow.end, line_end)) {
        return kimix::nullopt;
    }
    const auto var_edits = validate_cmd_for_body(reader, s, dow.end, line_end, name);
    if (!var_edits.has_value()) {
        return kimix::nullopt;
    }

    CmdForMatch match;
    match.for_start = for_start;
    match.name = name;
    match.tok_start = tok.start;
    match.tok_end = tok.end;
    match.open_pos = open_pos;
    match.close_pos = close_pos;
    match.do_end = dow.end;
    match.body_start = dow.end;
    match.body_end = line_end;
    match.comma_spans = std::move(list_edits->comma_spans);
    match.list_path_edits = std::move(list_edits->list_path_edits);
    match.var_edits = std::move(*var_edits);
    return match;
}

size_t commit_cmd_for(Scanner &reader, kimix::string_view source, const CmdForMatch &match) {
    const kimix::string_view s = source;
    reader.record_edit(match.tok_start, match.tok_end, match.name);
    reader.record_edit(match.open_pos, match.open_pos + 1, "");
    reader.record_edit(match.close_pos, match.close_pos + 1, ";");
    for (const auto &span : match.comma_spans) {
        reader.record_edit(span.first, span.second, " ");
    }
    for (const auto &pe : match.list_path_edits) {
        reader.record_edit(pe.start, pe.end, pe.replacement);
        reader.record_path(kimix::string{s.substr(pe.start, pe.end - pe.start)});
    }
    kimix::string var_repl;
    var_repl.push_back('$');
    var_repl.append(match.name);
    for (const auto &span : match.var_edits) {
        reader.record_edit(span.first, span.second, var_repl);
    }
    reader.record_edit(match.body_end, match.body_end, "; done");
    reader.scan_range(match.body_start, match.body_end);
    return match.body_end;
}

// ── jump refusals ───────────────────────────────────────────────────────────

kimix::optional<kimix::string> peek_literal_word(Scanner &reader, kimix::string_view source, size_t pos, size_t end) {
    const kimix::string_view s = source;
    const size_t q = skip_hspace(s, pos, end);
    const size_t r = reader.read_word(q, end, false);
    if (r <= q) {
        return kimix::nullopt;
    }
    return Scanner::literal_word_value(s.substr(q, r - q));
}

kimix::optional<kimix::string_view> cmd_jump_record(Scanner &reader, kimix::string_view source, kimix::string_view raw, const kimix::optional<kimix::string> &literal, bool literal_valid, size_t word_end, size_t end) {
    const kimix::string_view s = source;
    // `name = literal if isinstance(literal, str) else raw` — `literal_valid`
    // mirrors the isinstance test (a decoded value, possibly the empty string).
    const kimix::string_view name = (literal_valid && literal.has_value()) ? kimix::string_view{literal->data(), literal->size()} : raw;
    if (eq_sv(name, "goto") || is_glued_label(name)) {
        return kimix::string_view{"goto"};
    }
    if (eq_sv(name, "call")) {
        const auto nxt = peek_literal_word(reader, s, word_end, end);
        if (nxt.has_value() && ((!nxt->empty() && (*nxt)[0] == ':') || is_call_batch_target(*nxt))) {
            return kimix::string_view{"call :label"};
        }
        return kimix::nullopt;
    }
    if (eq_sv(name, "exit")) {
        const auto nxt = peek_literal_word(reader, s, word_end, end);
        if (nxt.has_value() && (eq_sv(*nxt, "/b") || eq_sv(*nxt, "/B"))) {
            return kimix::string_view{"exit /b"};
        }
        return kimix::nullopt;
    }
    return kimix::nullopt;
}

} // namespace kimix::llm::bash_parser
