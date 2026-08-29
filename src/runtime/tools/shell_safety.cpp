/*
 * shell_safety.cpp - Shell-safety kernels implementation.
 *
 * Exact port of kimi-agent commit 0582e09 "Study from hermes":
 *   src/kimix/tools/file/bash/safety.py
 *   src/kimix/tools/file/bash/output_enhance.py
 *
 * ASCII-only (see shell_safety.h). The scanner helpers below replicate the
 * `regex` module semantics for the reference patterns: leftmost match,
 * ordered alternation (rm|rmdir|del falls through to the longer alternative
 * when \b after the short one fails), greedy + and {n,} runs, finditer
 * resuming after each match end, and re.split(..., maxsplit=1) taking the
 * earliest separator position.
 */

#include <runtime/tools/shell_safety.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace tools {

namespace {

// ---- ASCII character classes ---------------------------------------------

inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ASCII \w = [A-Za-z0-9_].
inline bool is_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

inline bool is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

inline bool eq_ci(char a, char b) noexcept { return lower_ascii(a) == lower_ascii(b); }

bool match_lit(kimix::string_view t, size_t pos, kimix::string_view lit,
               bool ci = false) noexcept {
    if (pos + lit.size() > t.size()) {
        return false;
    }
    for (size_t i = 0; i < lit.size(); ++i) {
        if (ci ? !eq_ci(t[pos + i], lit[i]) : t[pos + i] != lit[i]) {
            return false;
        }
    }
    return true;
}

// Whitespace-collapse + ASCII-lowercase of a command (" ".join(split()).lower()).
kimix::string collapse_lower(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    bool first = true;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        if (!first) {
            out.push_back(' ');
        }
        first = false;
        while (i < s.size() && !is_space(s[i])) {
            out.push_back(lower_ascii(s[i]));
            ++i;
        }
    }
    return out;
}

bool contains(const kimix::vector<kimix::string>& v, kimix::string_view s) {
    for (const auto& e : v) {
        if (e == s) {
            return true;
        }
    }
    return false;
}

// ---- _segment_tokens (safety.py 49-57) ------------------------------------
//
// re.split(r";|\|\||&&|\||\n", tail, maxsplit=1)[0] then .split(): the split
// index is the earliest position of ';', "||", "&&", '|' or '\n' (the "||"
// alternative implies a '|' at the same position, and match length is
// irrelevant because only the pre-split text is used).
size_t segment_split_pos(kimix::string_view text, size_t start) noexcept {
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ';' || c == '|' || c == '\n') {
            return i;
        }
        if (c == '&' && i + 1 < text.size() && text[i + 1] == '&') {
            return i;
        }
    }
    return text.size();
}

// Whitespace-separated tokens of text[start : split_pos) (views into text).
kimix::vector<kimix::string_view> segment_tokens(kimix::string_view text,
                                                 size_t start) {
    kimix::vector<kimix::string_view> toks;
    const size_t e = segment_split_pos(text, start);
    size_t i = start;
    while (i < e) {
        while (i < e && is_space(text[i])) {
            ++i;
        }
        if (i >= e) {
            break;
        }
        size_t j = i;
        while (j < e && !is_space(text[j])) {
            ++j;
        }
        toks.push_back(text.substr(i, j - i));
        i = j;
    }
    return toks;
}

// ---- _looks_like_flag (safety.py 60-67) -----------------------------------
bool looks_like_flag(kimix::string_view token) noexcept {
    if (token.size() <= 1) {
        return false;
    }
    if (token[0] == '-') {
        return true;
    }
    if (token[0] == '/') {
        for (size_t i = 1; i < token.size(); ++i) {
            if (!is_alpha(token[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// ---- _collect_flags (safety.py 70-86) -------------------------------------
void collect_flags(const kimix::vector<kimix::string_view>& tokens,
                   bool flags[256]) noexcept {
    for (const auto& token : tokens) {
        if (!looks_like_flag(token)) {
            continue;
        }
        size_t i = 0;
        while (i < token.size() && (token[i] == '-' || token[i] == '/')) {
            ++i;
        }
        if (i >= token.size()) {
            continue;
        }
        const kimix::string_view core = token.substr(i);
        if (core.find("recursive") != kimix::string_view::npos) {
            flags[static_cast<unsigned char>('r')] = true;
        }
        if (core.find("force") != kimix::string_view::npos) {
            flags[static_cast<unsigned char>('f')] = true;
        }
        for (const char c : core) {
            if (c == 'r' || c == 'f' || c == 's' || c == 'q') {
                flags[static_cast<unsigned char>(c)] = true;
            }
        }
    }
}

// ---- _rm_target_is_protected (safety.py 89-107) ----------------------------
void replace_all(kimix::string& s, kimix::string_view from, kimix::string_view to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != kimix::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

kimix::string_view strip_view(kimix::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) {
        ++i;
    }
    size_t j = s.size();
    while (j > i && is_space(s[j - 1])) {
        --j;
    }
    return s.substr(i, j - i);
}

bool rm_target_is_protected(kimix::string_view target) {
    // t = target.strip().strip("\"'").lower()
    kimix::string t;
    t.reserve(target.size());
    kimix::string_view s = strip_view(target);
    size_t i = 0;
    while (i < s.size() && (s[i] == '\'' || s[i] == '"')) {
        ++i;
    }
    size_t j = s.size();
    while (j > i && (s[j - 1] == '\'' || s[j - 1] == '"')) {
        --j;
    }
    for (size_t k = i; k < j; ++k) {
        t.push_back(lower_ascii(s[k]));
    }
    replace_all(t, "${home}", "$home");

    // t.rstrip("/\\") in ("~", "$home")
    size_t e = t.size();
    while (e > 0 && (t[e - 1] == '/' || t[e - 1] == '\\')) {
        --e;
    }
    const kimix::string_view rstrip(t.data(), e);
    if (rstrip == "~" || rstrip == "$home") {
        return true;
    }

    // re.match(r"^[a-z]:[\\/]?(?:[\\/]?\*)?$", t) -- all valid suffix shapes:
    // "", "*", S, S*, S S* (S = one backslash or slash).  Two separators
    // without a trailing '*' do NOT match (the star is required inside the
    // optional group).
    if (t.size() >= 2 && t[0] >= 'a' && t[0] <= 'z' && t[1] == ':') {
        const auto is_sep = [](char c) noexcept { return c == '\\' || c == '/'; };
        const size_t rem = t.size() - 2;
        const char* suf = t.data() + 2;
        bool ok = false;
        if (rem == 0) {
            ok = true;
        } else if (rem == 1 && (suf[0] == '*' || is_sep(suf[0]))) {
            ok = true;
        } else if (rem == 2 && is_sep(suf[0]) && suf[1] == '*') {
            ok = true;
        } else if (rem == 3 && is_sep(suf[0]) && is_sep(suf[1]) && suf[2] == '*') {
            ok = true;
        }
        if (ok) {
            return true;
        }
    }

    // t.startswith("/"): parts = split("/") minus "", "." and "..".
    if (!t.empty() && t[0] == '/') {
        kimix::vector<kimix::string_view> parts;
        size_t i = 1;
        for (;;) {
            const size_t slash = t.find('/', i);
            const size_t e2 = (slash == kimix::string::npos) ? t.size() : slash;
            const kimix::string_view part(t.data() + i, e2 - i);
            if (!(part == "" || part == "." || part == "..")) {
                parts.push_back(part);
            }
            if (slash == kimix::string::npos) {
                break;
            }
            i = slash + 1;
        }
        if (parts.empty()) {
            return true;
        }
        if (parts.size() == 1 && parts[0] == "*") {
            return true;
        }
    }
    return false;
}

// ---- _detect_recursive_delete (safety.py 110-126) --------------------------
// \b(rm|rmdir|del)(?:\.exe)?\b -- ordered alternation; the trailing \b after
// the short alternative makes "rmdir" fall through to the longer one.
bool match_delete_word(kimix::string_view text, size_t pos, kimix::string_view& word,
                       size_t& end) noexcept {
    const auto try_word = [&](kimix::string_view w) -> bool {
        if (!match_lit(text, pos, w)) {
            return false;
        }
        size_t after = pos + w.size();
        if (after + 4 <= text.size() && match_lit(text, after, ".exe")) {
            after += 4;
        }
        if (after < text.size() && is_word(text[after])) {
            return false;
        }
        word = w;
        end = after;
        return true;
    };
    if (try_word("rm")) {
        return true;
    }
    if (try_word("rmdir")) {
        return true;
    }
    if (try_word("del")) {
        return true;
    }
    return false;
}

kimix::optional<kimix::string> detect_recursive_delete(kimix::string_view text) {
    for (size_t pos = 0; pos < text.size();) {
        if (pos > 0 && is_word(text[pos - 1])) {
            ++pos;
            continue;
        }
        kimix::string_view cmd;
        size_t end = 0;
        if (!match_delete_word(text, pos, cmd, end)) {
            ++pos;
            continue;
        }
        const auto tokens = segment_tokens(text, end);
        bool flags[256] = {};
        collect_flags(tokens, flags);
        bool armed = false;
        if (cmd == "rm") {
            armed = flags['r'] || flags['f'];
        } else if (cmd == "rmdir") {
            armed = flags['r'] || flags['s'];
        } else { // del
            armed = flags['r'] || flags['f'] || flags['s'];
        }
        if (armed) {
            for (const auto& tok : tokens) {
                if (looks_like_flag(tok)) {
                    continue;
                }
                if (rm_target_is_protected(tok)) {
                    kimix::string msg = "Recursive delete of protected root/home (`";
                    msg.append(tok.data(), tok.size());
                    msg.append("`)");
                    return std::make_optional(std::move(msg));
                }
            }
        }
        pos = end; // finditer resumes after the match end
    }
    return std::nullopt;
}

// ---- small pattern matchers (detect_hardline_command) ----------------------

// \bmkfs(?:\.\w+)?\b
bool search_mkfs(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, "mkfs")) {
            continue;
        }
        size_t after = pos + 4;
        if (after < t.size() && t[after] == '.') {
            size_t k = after + 1;
            while (k < t.size() && is_word(t[k])) {
                ++k;
            }
            if (k > after + 1) {
                after = k; // (?:\.\w+)? matched
            }
            // else the group is empty and \b applies right after "mkfs"
        }
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bdd\b
bool has_word(kimix::string_view t, kimix::string_view w) {
    for (size_t pos = 0; pos + w.size() <= t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, w)) {
            continue;
        }
        const size_t after = pos + w.size();
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bof=/dev/(?:sd|nvme|disk|rdisk)[a-z0-9]*  ("of=/dev/" is 8 chars)
bool has_dd_output(kimix::string_view t) {
    static const char* kDev[] = {"sd", "nvme", "disk", "rdisk"};
    for (size_t pos = 0; pos + 8 <= t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue; // \b before "of="
        }
        if (!match_lit(t, pos, "of=/dev/")) {
            continue;
        }
        for (const char* d : kDev) {
            const size_t n = std::strlen(d);
            if (!match_lit(t, pos + 8, kimix::string_view(d, n))) {
                continue;
            }
            size_t k = pos + 8 + n;
            while (k < t.size() && ((t[k] >= 'a' && t[k] <= 'z') ||
                                    (t[k] >= '0' && t[k] <= '9'))) {
                ++k;
            }
            return true; // [a-z0-9]* may be empty
        }
    }
    return false;
}

// :()\{ and :\|:&
bool has_fork_bomb(kimix::string_view t) {
    return t.find(":(){") != kimix::string_view::npos &&
           t.find(":|:&") != kimix::string_view::npos;
}

// \bkill(?:\.exe)?\b and \bformat(?:\.exe)?\b -- shared finditer body.
template <typename OnWord>
void each_word_with_exe(kimix::string_view t, kimix::string_view w,
                        const OnWord& on) {
    for (size_t pos = 0; pos < t.size();) {
        if (pos > 0 && is_word(t[pos - 1])) {
            ++pos;
            continue;
        }
        if (!match_lit(t, pos, w)) {
            ++pos;
            continue;
        }
        size_t after = pos + w.size();
        if (after + 4 <= t.size() && match_lit(t, after, ".exe")) {
            after += 4;
        }
        if (after < t.size() && is_word(t[after])) {
            ++pos;
            continue;
        }
        if (!on(after)) {
            return; // stop requested
        }
        pos = after; // finditer resumes after the match end
    }
}

} // namespace

// ---------------------------------------------------------------------------
// public kernels
// ---------------------------------------------------------------------------

void command_detection_variants(kimix::string_view command,
                                kimix::vector<kimix::string>& out) {
    out.clear();
    if (command.empty()) {
        return;
    }
    bool has_non_space = false;
    for (const char c : command) {
        if (!is_space(c)) {
            has_non_space = true;
            break;
        }
    }
    if (!has_non_space) {
        return;
    }
    // collapsed = " ".join(command.split())
    kimix::string collapsed;
    collapsed.reserve(command.size());
    bool first = true;
    size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && is_space(command[i])) {
            ++i;
        }
        if (i >= command.size()) {
            break;
        }
        if (!first) {
            collapsed.push_back(' ');
        }
        first = false;
        const size_t j = i;
        while (i < command.size() && !is_space(command[i])) {
            ++i;
        }
        collapsed.append(command.data() + j, i - j);
    }
    // deobfuscated = re.sub(r"[\\'\"]", "", collapsed).lower()
    kimix::string deobfuscated;
    deobfuscated.reserve(collapsed.size());
    for (const char c : collapsed) {
        if (c != '\\' && c != '\'' && c != '"') {
            deobfuscated.push_back(lower_ascii(c));
        }
    }
    // lowered = collapsed.lower()
    kimix::string lowered = collapsed;
    for (char& c : lowered) {
        c = lower_ascii(c);
    }
    for (const kimix::string* variant : {&collapsed, &deobfuscated, &lowered}) {
        if (!variant->empty() && !contains(out, *variant)) {
            out.push_back(*variant);
        }
    }
    if (out.empty()) {
        out.push_back(collapsed);
    }
}

hardline_result detect_hardline_command(kimix::string_view command) {
    hardline_result res;
    if (command.empty()) {
        return res;
    }
    bool has_non_space = false;
    for (const char c : command) {
        if (!is_space(c)) {
            has_non_space = true;
            break;
        }
    }
    if (!has_non_space) {
        return res;
    }
    // text = " ".join(command.split()).lower()
    const kimix::string text = collapse_lower(command);

    // 1. Recursive delete of root / home / Windows drive root.
    if (auto desc = detect_recursive_delete(text)) {
        res.blocked = true;
        res.description = std::move(desc);
        return res;
    }

    // 2. Disk formatting (mkfs.*).
    if (search_mkfs(text)) {
        res.blocked = true;
        res.description =
            kimix::string("Disk formatting command (`mkfs`) is blocked");
        return res;
    }

    // 3. dd writing to a raw device.
    if (has_word(text, "dd") && has_dd_output(text)) {
        res.blocked = true;
        res.description =
            kimix::string("`dd` writing to a raw device is blocked");
        return res;
    }

    // 4. System power commands (first token).
    {
        size_t i = 0;
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        size_t j = i;
        while (j < text.size() && !is_space(text[j])) {
            ++j;
        }
        const kimix::string first = text.substr(i, j - i);
        if (first == "shutdown" || first == "reboot" || first == "poweroff" ||
            first == "halt") {
            res.blocked = true;
            kimix::string msg = "System `";
            msg.append(first.data(), first.size());
            msg.append("` command is blocked");
            res.description = std::move(msg);
            return res;
        }
    }

    // 5. Fork bomb.
    if (has_fork_bomb(text)) {
        res.blocked = true;
        res.description = kimix::string("Fork bomb pattern detected");
        return res;
    }

    // 6. kill targeting PID 1 (or $PPID).
    {
        bool found = false;
        each_word_with_exe(
            text, "kill",
            [&](size_t after) -> bool {
                const auto tokens = segment_tokens(text, after);
                for (const auto& tok : tokens) {
                    if (looks_like_flag(tok)) {
                        continue;
                    }
                    if (tok == "1" || tok == "$ppid") {
                        res.blocked = true;
                        res.description = kimix::string(
                            "`kill` targeting PID 1 (or `$PPID`) is blocked");
                        found = true;
                        return false; // stop
                    }
                }
                return true;
            });
        if (found) {
            return res;
        }
    }

    // 7. Windows format <drive>:.
    {
        bool found = false;
        each_word_with_exe(
            text, "format",
            [&](size_t after) -> bool {
                const auto tokens = segment_tokens(text, after);
                for (const auto& tok : tokens) {
                    // re.match(r"^[a-z]:[\\/]?$", target)
                    if (tok.size() >= 2 && tok[0] >= 'a' && tok[0] <= 'z' &&
                        tok[1] == ':') {
                        size_t k = 2;
                        if (k < tok.size() &&
                            (tok[k] == '\\' || tok[k] == '/')) {
                            ++k;
                        }
                        if (k == tok.size()) {
                            res.blocked = true;
                            res.description = kimix::string(
                                "Windows `format` on a drive is blocked");
                            found = true;
                            return false;
                        }
                    }
                }
                return true;
            });
        if (found) {
            return res;
        }
    }

    return res;
}

hardline_result check_hardline_blocked(kimix::string_view command) {
    kimix::vector<kimix::string> variants;
    command_detection_variants(command, variants);
    for (const auto& v : variants) {
        hardline_result r = detect_hardline_command(v);
        if (r.blocked) {
            return r;
        }
    }
    return hardline_result();
}

// ---------------------------------------------------------------------------
// foreground_background_guidance
// ---------------------------------------------------------------------------

// _strip_quoted: re.sub(r"'[^']*'|\"[^\"]*\"", " ", command)
kimix::string strip_quoted(kimix::string_view command) {
    kimix::string out;
    out.reserve(command.size());
    size_t i = 0;
    while (i < command.size()) {
        const char c = command[i];
        if (c == '\'' || c == '"') {
            size_t j = i + 1;
            while (j < command.size() && command[j] != c) {
                ++j;
            }
            if (j < command.size()) { // matched quoted span -> one space
                out.push_back(' ');
                i = j + 1;
                continue;
            }
            out.push_back(c); // unterminated quote: literal char
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

kimix::string collapse(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    bool first = true;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        if (!first) {
            out.push_back(' ');
        }
        first = false;
        while (i < s.size() && !is_space(s[i])) {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

// \bword\b
bool search_boundary_word(kimix::string_view t, kimix::string_view w) {
    for (size_t pos = 0; pos + w.size() <= t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, w)) {
            continue;
        }
        const size_t after = pos + w.size();
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// Skip a whitespace run; returns the index after it.
size_t skip_ws(kimix::string_view t, size_t pos) noexcept {
    while (pos < t.size() && is_space(t[pos])) {
        ++pos;
    }
    return pos;
}

// \b(?:npm|pnpm|yarn|bun)\s+run\s+(?:dev|start|serve|watch)\b
bool pat_npm_run(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        size_t k = 0;
        if (match_lit(t, pos, "npm")) {
            k = pos + 3;
        } else if (match_lit(t, pos, "pnpm")) {
            k = pos + 4;
        } else if (match_lit(t, pos, "yarn")) {
            k = pos + 4;
        } else if (match_lit(t, pos, "bun")) {
            k = pos + 3;
        } else {
            continue;
        }
        size_t ws = skip_ws(t, k);
        if (ws == k || !match_lit(t, ws, "run")) {
            continue;
        }
        size_t ws2 = skip_ws(t, ws + 3);
        if (ws2 == ws + 3) {
            continue;
        }
        size_t w = ws2;
        size_t wlen = 0;
        if (match_lit(t, w, "dev")) {
            wlen = 3;
        } else if (match_lit(t, w, "start")) {
            wlen = 5;
        } else if (match_lit(t, w, "serve")) {
            wlen = 5;
        } else if (match_lit(t, w, "watch")) {
            wlen = 5;
        }
        if (wlen == 0) {
            continue;
        }
        w += wlen;
        if (w < t.size() && is_word(t[w])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bnext\s+dev\b
bool pat_next_dev(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, "next")) {
            continue;
        }
        size_t ws = skip_ws(t, pos + 4);
        if (ws == pos + 4 || !match_lit(t, ws, "dev")) {
            continue;
        }
        const size_t after = ws + 3;
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bpython\s+-m\s+http\.server\b
bool pat_python_http(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, "python")) {
            continue;
        }
        size_t ws = skip_ws(t, pos + 6);
        if (ws == pos + 6 || !match_lit(t, ws, "-m")) {
            continue;
        }
        size_t ws2 = skip_ws(t, ws + 2);
        if (ws2 == ws + 2 || !match_lit(t, ws2, "http.server")) {
            continue;
        }
        const size_t after = ws2 + 11;
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bdocker\s+compose\s+up\b
bool pat_docker_compose(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, "docker")) {
            continue;
        }
        size_t ws = skip_ws(t, pos + 6);
        if (ws == pos + 6 || !match_lit(t, ws, "compose")) {
            continue;
        }
        size_t ws2 = skip_ws(t, ws + 7);
        if (ws2 == ws + 7 || !match_lit(t, ws2, "up")) {
            continue;
        }
        const size_t after = ws2 + 2;
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// \bdocker-compose\s+up\b
bool pat_docker_dash_compose(kimix::string_view t) {
    for (size_t pos = 0; pos + 14 <= t.size(); ++pos) {
        if (pos > 0 && is_word(t[pos - 1])) {
            continue;
        }
        if (!match_lit(t, pos, "docker-compose")) {
            continue;
        }
        size_t ws = skip_ws(t, pos + 14);
        if (ws == pos + 14 || !match_lit(t, ws, "up")) {
            continue;
        }
        const size_t after = ws + 2;
        if (after < t.size() && is_word(t[after])) {
            continue;
        }
        return true;
    }
    return false;
}

// &\s*$
bool pat_trailing_amp(kimix::string_view t) {
    for (size_t pos = 0; pos < t.size(); ++pos) {
        if (t[pos] != '&') {
            continue;
        }
        if (skip_ws(t, pos + 1) == t.size()) {
            return true;
        }
    }
    return false;
}

const char* kFgBgHint =
    "Long-running process detected. Consider mode='send' (background) + "
    "TaskOutput to avoid blocking on timeout.";

kimix::optional<kimix::string> foreground_background_guidance(
    kimix::string_view command) {
    if (command.empty()) {
        return std::nullopt;
    }
    bool has_non_space = false;
    for (const char c : command) {
        if (!is_space(c)) {
            has_non_space = true;
            break;
        }
    }
    if (!has_non_space) {
        return std::nullopt;
    }
    const kimix::string stripped = strip_quoted(command);
    const kimix::string text = collapse(stripped);

    // Patterns in reference order; any() short-circuits.
    if (pat_npm_run(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (pat_next_dev(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "vite")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "nodemon")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "uvicorn")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "gunicorn")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (pat_python_http(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (pat_docker_compose(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (pat_docker_dash_compose(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (pat_trailing_amp(text)) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "nohup")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    if (search_boundary_word(text, "setsid")) {
        return std::make_optional(kimix::string(kFgBgHint));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// exit-code interpretation / failure annotation
// ---------------------------------------------------------------------------

kimix::string base_command_name(kimix::string_view command) {
    const auto split_last = [](kimix::string_view s,
                               kimix::string_view sep) -> kimix::string_view {
        const size_t p = s.rfind(sep);
        if (p == kimix::string_view::npos) {
            return s;
        }
        return s.substr(p + sep.size());
    };
    kimix::string_view seg = strip_view(command);
    seg = split_last(seg, "&&");
    seg = split_last(seg, "||");
    seg = split_last(seg, "|");
    seg = split_last(seg, ";");
    seg = strip_view(seg);
    size_t i = 0;
    while (i < seg.size()) {
        while (i < seg.size() && is_space(seg[i])) {
            ++i;
        }
        if (i >= seg.size()) {
            break;
        }
        const size_t j = i;
        while (i < seg.size() && !is_space(seg[i])) {
            ++i;
        }
        const kimix::string_view word = seg.substr(j, i - j);
        if (word.find('=') != kimix::string_view::npos && !word.starts_with("-")) {
            continue; // FOO=1 assignment
        }
        kimix::string_view stem = word;
        const size_t slash = word.rfind('/');
        if (slash != kimix::string_view::npos) {
            stem = word.substr(slash + 1);
        }
        const bool exe =
            stem.size() >= 4 &&
            lower_ascii(stem[stem.size() - 4]) == '.' &&
            lower_ascii(stem[stem.size() - 3]) == 'e' &&
            lower_ascii(stem[stem.size() - 2]) == 'x' &&
            lower_ascii(stem[stem.size() - 1]) == 'e';
        if (exe) {
            return kimix::string(stem.substr(0, stem.size() - 4));
        }
        return kimix::string(stem);
    }
    return kimix::string();
}

kimix::optional<kimix::string> annotate_failure(kimix::string_view output,
                                                kimix::string_view /*command*/,
                                                kimix::optional<int64_t> /*exit_code*/) {
    if (output.empty()) {
        return std::nullopt;
    }
    const size_t sample_len = output.size() < 4000 ? output.size() : 4000;
    const kimix::string_view sample(output.data(), sample_len);

    kimix::string lowered;
    lowered.reserve(sample.size());
    for (const char c : sample) {
        lowered.push_back(lower_ascii(c));
    }

    if (lowered.find("command not found") != kimix::string::npos ||
        lowered.find("not recognized as an internal or external command") !=
            kimix::string::npos) {
        return std::make_optional(kimix::string(
            "The command was not found. Check it is installed and on PATH "
            "(use `which <cmd>` / `Get-Command <cmd>`)."));
    }
    if (lowered.find("no such file or directory") != kimix::string::npos) {
        return std::make_optional(kimix::string(
            "A file or directory referenced by the command does not exist. "
            "Verify the path with `Glob`/ReadFile."));
    }

    // (?i)modulenotfounderror:\s*no module named '([^']+)'
    static const char kModule[] = "modulenotfounderror:";
    static const char kNoModule[] = "no module named '";
    constexpr size_t kModuleLen = 20;   // strlen("modulenotfounderror:")
    constexpr size_t kNoModuleLen = 17; // strlen("no module named '")
    for (size_t pos = 0; pos + kModuleLen <= sample.size(); ++pos) {
        size_t k = 0;
        while (k < kModuleLen && eq_ci(sample[pos + k], kModule[k])) {
            ++k;
        }
        if (k != kModuleLen) {
            continue;
        }
        size_t i = pos + kModuleLen;
        i = skip_ws(sample, i); // \s*
        // The whole reference pattern is re.IGNORECASE, so "no module named '"
        // matches case-insensitively too (the capture itself stays verbatim).
        if (i + kNoModuleLen > sample.size() ||
            !match_lit(sample, i, kimix::string_view(kNoModule, kNoModuleLen), true)) {
            continue;
        }
        size_t v = i + kNoModuleLen;
        size_t ve = v;
        while (ve < sample.size() && sample[ve] != '\'') {
            ++ve;
        }
        if (ve == v || ve == sample.size()) {
            continue; // [^']+ needs at least one char and a closing quote
        }
        kimix::string msg = "Python module ";
        msg.append(sample.data() + v, ve - v);
        msg.append(" is missing. Install it (e.g. `pip install ");
        msg.append(sample.data() + v, ve - v);
        msg.append("`) or check the environment.");
        return std::make_optional(std::move(msg));
    }

    if (lowered.find("permission denied") != kimix::string::npos) {
        return std::make_optional(
            kimix::string("Permission denied. Check file permissions (ls -la) or ownership."));
    }
    return std::nullopt;
}

} // namespace tools
} // namespace runtime
} // namespace kimix
