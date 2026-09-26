// agent/dynamic_injections/target_churn.cpp - see target_churn.h.

#include "agent/dynamic_injections/target_churn.h"

#include <algorithm>

#include <core/stl/hash.h>

#include "agent/tool_taxonomy.h"

namespace kimix::agent {

namespace {

// ── Path normalization (target_churn.py:_normalize_path) ────────────────────
// os.path.normcase(os.path.normpath(raw.strip().strip("'\""))): the native
// port trims whitespace + surrounding quotes, swaps '/' for '\\' and
// lower-cases ASCII (the Windows normcase). normpath collapses inner "."/".."
// components; an unresolvable ".." is kept verbatim, like os.path.normpath.
kimix::string normalize_path(kimix::string_view raw) {
    kimix::string_view v = raw;
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' ||
                          v.front() == '\n' || v.front() == '\'' || v.front() == '"')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' ||
                          v.back() == '\n' || v.back() == '\'' || v.back() == '"')) {
        v.remove_suffix(1);
    }
    // normpath on the separators: collapse a/../b, a/./b, duplicate slashes.
    kimix::vector<kimix::string> parts;
    kimix::string cur;
    const bool rooted = !v.empty() && (v.front() == '/' || v.front() == '\\' ||
                                       (v.size() > 1 && v[1] == ':'));
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!rooted) {
                // A ".." above a rooted path is dropped, as normpath does
                // for absolute paths; relative paths keep it.
                parts.push_back(cur);
            }
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '/' || c == '\\') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    kimix::string out;
    if (rooted) {
        if (v.size() > 1 && v[1] == ':') {
            out += static_cast<char>(v[0] >= 'A' && v[0] <= 'Z' ? v[0] - 'A' + 'a' : v[0]);
            out += ':';
            out += '\\';
        } else {
            out += '\\';
        }
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += '\\';
        }
        out += parts[i];
    }
    // normcase: lowercase ASCII.
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// ── Shell write-target extraction (regex-free _REDIRECT_RE/_SED_I_RE/_TEE_RE) ─

bool is_word_byte(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

bool is_delim(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '&' ||
           c == '|' || c == '<' || c == '>';
}

// (?<![<>])>>?\s*([^\s;&|<>]+): a '>' or '>>' not preceded by '<' or '>',
// then the target word. '&>' / '<&' forms are excluded by the lookbehind /
// the delimiter class. The regex scans left to right, non-overlapping.
void find_redirect_targets(kimix::string_view cmd, kimix::vector<kimix::string> &out) {
    size_t i = 0;
    while (i < cmd.size()) {
        if (cmd[i] == '>' && (i == 0 || (cmd[i - 1] != '<' && cmd[i - 1] != '>'))) {
            size_t j = i + 1;
            if (j < cmd.size() && cmd[j] == '>') {
                ++j;
            }
            while (j < cmd.size() && (cmd[j] == ' ' || cmd[j] == '\t')) {
                ++j;
            }
            size_t k = j;
            while (k < cmd.size() && !is_delim(cmd[k])) {
                ++k;
            }
            if (k > j) {
                out.push_back(kimix::string(cmd.substr(j, k - j)));
            }
            i = k > j ? k : j;
        } else {
            ++i;
        }
    }
}

// \bsed\s+(?:-\w+\s+)*-i(?:\.\S+)?(?:\s+['"][^'"]*['"])+\s+([^\s;&|]+)
// sed, flag tokens, an -i (optionally -i.bak), one or more quoted script
// arguments, then the target word. A quoted target (e.g. sed -i 's/a/b/' 'f')
// is what the regex captures (the [^\s;&|]+ runs to the closing quote).
void find_sed_targets(kimix::string_view cmd, kimix::vector<kimix::string> &out) {
    size_t i = 0;
    while (i + 3 <= cmd.size()) {
        const bool word_boundary = i == 0 || !is_word_byte(cmd[i - 1]);
        if (word_boundary && cmd.substr(i, 3) == "sed" &&
            (i + 3 == cmd.size() || !is_word_byte(cmd[i + 3]))) {
            size_t j = i + 3;
            bool saw_inplace = false;
            for (;;) {
                while (j < cmd.size() && (cmd[j] == ' ' || cmd[j] == '\t')) {
                    ++j;
                }
                if (j >= cmd.size() || cmd[j] != '-') {
                    break;
                }
                // one flag token: -\w+ (for sed -i: -i or -i.bak)
                size_t k = j + 1;
                while (k < cmd.size() &&
                       ((cmd[k] >= 'a' && cmd[k] <= 'z') || (cmd[k] >= 'A' && cmd[k] <= 'Z') ||
                        (cmd[k] >= '0' && cmd[k] <= '9') || cmd[k] == '_')) {
                    ++k;
                }
                // -i.bak: a dot suffix is part of the -i token only
                if (k < cmd.size() && cmd[k] == '.' && k > j + 1 && cmd[j + 1] == 'i') {
                    ++k;
                    while (k < cmd.size() && !is_delim(cmd[k])) {
                        ++k;
                    }
                }
                const kimix::string_view flag = cmd.substr(j + 1, k - j - 1);
                if (flag.substr(0, 1) == "i") {
                    saw_inplace = true;
                }
                j = k;
            }
            if (!saw_inplace) {
                i += 3;
                continue;
            }
            // (?:\s+['"][^'"]*['"])+ : one or more quoted script args,
            // scanning from just after the flag tokens
            size_t quoted_end = j;
            size_t last_quote_end = 0;
            for (;;) {
                size_t q = quoted_end;
                while (q < cmd.size() && (cmd[q] == ' ' || cmd[q] == '\t')) {
                    ++q;
                }
                if (q >= cmd.size() || (cmd[q] != '\'' && cmd[q] != '"')) {
                    break;
                }
                const char quote = cmd[q];
                size_t e = q + 1;
                while (e < cmd.size() && cmd[e] != quote) {
                    ++e;
                }
                if (e >= cmd.size()) {
                    break; // unterminated: the + quantifier fails
                }
                last_quote_end = e + 1;
                quoted_end = e + 1;
            }
            if (last_quote_end == 0) {
                i += 3;
                continue;
            }
            size_t t = last_quote_end;
            while (t < cmd.size() && (cmd[t] == ' ' || cmd[t] == '\t')) {
                ++t;
            }
            size_t k = t;
            while (k < cmd.size() && cmd[k] != ' ' && cmd[k] != '\t' && cmd[k] != ';' &&
                   cmd[k] != '&' && cmd[k] != '|') {
                ++k;
            }
            if (k > t) {
                out.push_back(kimix::string(cmd.substr(t, k - t)));
            }
            i = k > t ? k : last_quote_end;
        } else {
            ++i;
        }
    }
}

// \btee\s+(?:-\w+\s+)*([^\s;&|]+)
void find_tee_targets(kimix::string_view cmd, kimix::vector<kimix::string> &out) {
    size_t i = 0;
    while (i + 3 <= cmd.size()) {
        const bool word_boundary = i == 0 || !is_word_byte(cmd[i - 1]);
        if (word_boundary && cmd.substr(i, 3) == "tee" &&
            (i + 3 == cmd.size() || !is_word_byte(cmd[i + 3]))) {
            size_t j = i + 3;
            for (;;) {
                while (j < cmd.size() && (cmd[j] == ' ' || cmd[j] == '\t')) {
                    ++j;
                }
                if (j >= cmd.size() || cmd[j] != '-') {
                    break;
                }
                size_t k = j + 1;
                while (k < cmd.size() &&
                       ((cmd[k] >= 'a' && cmd[k] <= 'z') || (cmd[k] >= 'A' && cmd[k] <= 'Z') ||
                        (cmd[k] >= '0' && cmd[k] <= '9') || cmd[k] == '_')) {
                    ++k;
                }
                j = k;
            }
            size_t k = j;
            while (k < cmd.size() && cmd[k] != ' ' && cmd[k] != '\t' && cmd[k] != ';' &&
                   cmd[k] != '&' && cmd[k] != '|') {
                ++k;
            }
            if (k > j) {
                out.push_back(kimix::string(cmd.substr(j, k - j)));
            }
            i = k > j ? k : j;
        } else {
            ++i;
        }
    }
}

// First string value found under `key` in a JSON object argument string
// (the mirror of tool_taxonomy.h's path_params_of for the command keys).
kimix::optional<kimix::string>
string_param_of(kimix::string_view arguments_json, kimix::string_view key) {
    if (arguments_json.empty()) {
        return kimix::optional<kimix::string>();
    }
    kimix::string buf(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return kimix::optional<kimix::string>();
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    kimix::optional<kimix::string> out;
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val *v = yyjson_obj_getn(root, key.data(), key.size());
        if (v != nullptr && yyjson_is_str(v)) {
            out.emplace(yyjson_get_str(v), yyjson_get_len(v));
        }
    }
    yyjson_doc_free(doc);
    return out;
}

// The edit-targets of one assistant tool call (EDIT_TOOLS via PATH_PARAM_KEYS,
// SHELL_TOOLS via the command + the three extraction patterns). Returns {}
// for tools outside the taxonomy or unparseable arguments - exactly the
// reference's defensive shape.
kimix::vector<kimix::string>
extract_tool_call_targets(kimix::string_view tool_name, kimix::string_view arguments) {
    kimix::vector<kimix::string> targets;
    if (is_edit_tool(tool_name)) {
        const kimix::optional<kimix::string> path = path_params_of(arguments);
        if (path.has_value() && !path->empty()) {
            targets.push_back(normalize_path(*path));
        }
        return targets;
    }
    if (is_shell_tool(tool_name)) {
        // COMMAND_PARAM_KEYS: the first key present wins.
        kimix::string command;
        for (kimix::string_view key : kCommandParamKeys) {
            const kimix::optional<kimix::string> value =
                string_param_of(arguments, key);
            if (value.has_value() && !value->empty()) {
                command = *value;
                break;
            }
        }
        if (command.empty()) {
            return targets;
        }
        const kimix::string_view command_view(command);
        find_redirect_targets(command_view, targets);
        find_sed_targets(command_view, targets);
        find_tee_targets(command_view, targets);
        for (kimix::string &t : targets) {
            t = normalize_path(t);
        }
        return targets;
    }
    return targets;
}

} // namespace

kimix::vector<kimix::string> shell_write_targets(kimix::string_view command) {
    kimix::vector<kimix::string> targets;
    find_redirect_targets(command, targets);
    find_sed_targets(command, targets);
    find_tee_targets(command, targets);
    for (kimix::string &t : targets) {
        t = normalize_path(t);
    }
    return targets;
}

kimix::string normalize_error_text(kimix::string_view text) {
    // target_churn.py:_normalize_error:
    //   quoted spans -> <str>, path-like spans -> <path>, digits -> <n>,
    //   whitespace collapsed. Order: quotes, paths, digits.
    kimix::string out;
    size_t i = 0;
    const size_t n = text.size();
    auto is_path_char = [&](char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\'' &&
               c != '"';
    };
    while (i < n) {
        const char c = text[i];
        if (c == '\'' || c == '"') {
            // '[^']*' | "[^"]*"
            size_t e = i + 1;
            while (e < n && text[e] != c) {
                ++e;
            }
            if (e < n) {
                out += "<str>";
                i = e + 1;
            } else {
                out += c; // unterminated: literal
                ++i;
            }
            continue;
        }
        // (?:[A-Za-z]:\\[^\s'"]+|/[^\s'"]+)
        if ((c == '/' && i + 1 < n && is_path_char(text[i + 1])) ||
            (i + 2 < n && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) &&
             text[i + 1] == ':' && (text[i + 2] == '\\' || text[i + 2] == '/'))) {
            size_t e = i + ((c == '/') ? 1 : 3);
            while (e < n && is_path_char(text[e])) {
                ++e;
            }
            out += "<path>";
            i = e;
            continue;
        }
        if (c >= '0' && c <= '9') {
            while (i < n && text[i] >= '0' && text[i] <= '9') {
                ++i;
            }
            out += "<n>";
            continue;
        }
        out += c;
        ++i;
    }
    // " ".join(text.split()): collapse every whitespace run to one space and
    // trim both ends.
    kimix::string collapsed;
    collapsed.reserve(out.size());
    bool pending_space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            pending_space = !collapsed.empty();
        } else {
            if (pending_space) {
                collapsed += ' ';
                pending_space = false;
            }
            collapsed += c;
        }
    }
    return collapsed;
}

TargetChurnProvider::TargetChurnProvider(int32_t file_warn, int32_t file_strong,
                                         int32_t error_warn, int32_t cooldown_steps)
    : _file_warn(file_warn < 2 ? 2 : file_warn),
      _file_strong(file_strong < _file_warn + 1 ? _file_warn + 1 : file_strong),
      _error_warn(error_warn < 2 ? 2 : error_warn),
      _cooldown_steps(cooldown_steps < 0 ? 0 : cooldown_steps) {}

void TargetChurnProvider::process_message(const kimix::llm::Message &message) {
    // target_churn.py:_process_message.
    if (message.role == "assistant" && !message.tool_calls.empty()) {
        for (const kimix::llm::ToolCall &tc : message.tool_calls) {
            const kimix::vector<kimix::string> targets =
                extract_tool_call_targets(tc.name, tc.arguments);
            for (const kimix::string &target : targets) {
                _file_counts[target] += 1; // operator[] inserts at 0
            }
        }
        return;
    }
    if (message.role == "tool") {
        constexpr kimix::string_view k_error_prefix = "<system>ERROR:";
        kimix::string_view text(message.content);
        const size_t at = text.find(k_error_prefix);
        if (at != kimix::string_view::npos) {
            // Take the first error line (exception type + message): the text
            // between the prefix and the next "</system>".
            kimix::string_view error_text = text.substr(at + k_error_prefix.size());
            const size_t close = error_text.find("</system>");
            if (close != kimix::string_view::npos) {
                error_text = error_text.substr(0, close);
            }
            // .strip() (Python whitespace), then the first line.
            while (!error_text.empty() &&
                   (error_text.front() == ' ' || error_text.front() == '\t' ||
                    error_text.front() == '\r' || error_text.front() == '\n')) {
                error_text.remove_prefix(1);
            }
            while (!error_text.empty() &&
                   (error_text.back() == ' ' || error_text.back() == '\t' ||
                    error_text.back() == '\r' || error_text.back() == '\n')) {
                error_text.remove_suffix(1);
            }
            const size_t nl = error_text.find('\n');
            if (nl != kimix::string_view::npos) {
                error_text = error_text.substr(0, nl);
            }
            const kimix::string normalized = normalize_error_text(error_text);
            const uint64_t fingerprint =
                kimix::hash64(normalized.data(), normalized.size());
            if (_has_error_fingerprint && fingerprint == _error_last_fingerprint) {
                _error_streak += 1;
            } else {
                _error_last_fingerprint = fingerprint;
                _has_error_fingerprint = true;
                _error_streak = 1;
            }
        } else {
            // A successful tool result breaks the error streak.
            _has_error_fingerprint = false;
            _error_streak = 0;
        }
    }
}

void TargetChurnProvider::sync_turn(const InjectionStepContext &ctx) {
    // target_churn.py:_sync_turn with the turn-id analogue.
    if (ctx.turn_seq != 0 && ctx.turn_seq != _turn_seq) {
        _turn_seq = ctx.turn_seq;
        _warned_files.clear();
        _strong_warned_files.clear();
        _error_warned_this_turn = false;
    }
}

bool TargetChurnProvider::get_injections(const InjectionStepContext &ctx,
                                         kimix::vector<DynamicInjection> &out,
                                         kimix::string &error) {
    (void)error;
    // target_churn.py:553-630.
    sync_turn(ctx);
    const kimix::vector<kimix::llm::Message> empty_history;
    const kimix::vector<kimix::llm::Message> &history =
        ctx.history != nullptr ? *ctx.history : empty_history;

    // Defensive: history may have been rebuilt without a compaction
    // notification (e.g. D-Mail revert); never scan backwards blindly.
    if (_cursor > history.size()) {
        _cursor = 0;
        _file_counts.clear();
        _has_error_fingerprint = false;
        _error_streak = 0;
    }
    for (size_t i = _cursor; i < history.size(); ++i) {
        process_message(history[i]);
    }
    _cursor = history.size();

    const int32_t step_no = ctx.step_no;
    if (_last_alert_step.has_value() &&
        (step_no - *_last_alert_step) < _cooldown_steps) {
        return true;
    }

    // Strong file-churn alert has the highest priority. The reference walks
    // Counter.most_common() - descending count, ties by first-insertion -
    // and alerts on the FIRST entry that qualifies; the snapshot below is
    // the same order with ties resolved alphabetically (vector_map is
    // key-sorted; stable sort keeps that order within equal counts), an
    // accepted port-only difference for equal counts.
    kimix::vector<const kimix::vector_map<kimix::string, int32_t>::value_type *> by_count;
    by_count.reserve(_file_counts.size());
    for (const auto &entry : _file_counts) {
        by_count.push_back(&entry);
    }
    std::stable_sort(by_count.begin(), by_count.end(),
                     [](const auto *a, const auto *b) {
                         return a->second > b->second;
                     });

    // Strong file-churn alert has the highest priority.
    for (const kimix::vector_map<kimix::string, int32_t>::value_type *entry : by_count) {
        const kimix::string &path = entry->first;
        const int32_t best_count = entry->second;
        if (best_count >= _file_strong &&
            _strong_warned_files.find(path) == _strong_warned_files.end()) {
            _strong_warned_files.insert(path);
            _warned_files.insert(path);
            _last_alert_step = step_no;
            DynamicInjection injection;
            injection.type = "target_churn";
            // target_churn.py:583-593 (verbatim).
            injection.content =
                kimix::string("You have modified the same file `") + path +
                "` " + kimix::string(std::to_string(best_count)) +
                " times. "
                "Repeated patching of one file is a strong signal of a wrong "
                "approach.\n"
                "Stop patching. Rewrite the file as a whole from your current "
                "understanding, or switch to a fundamentally different approach. "
                "If tests keep failing, re-read the error and fix the root cause "
                "instead of iterating on the same spot.";
            out.push_back(std::move(injection));
            return true;
        }
    }

    // Error-signature streak.
    if (_error_streak >= _error_warn && !_error_warned_this_turn) {
        _error_warned_this_turn = true;
        _last_alert_step = step_no;
        DynamicInjection injection;
        injection.type = "target_churn";
        // target_churn.py:601-611 (verbatim).
        injection.content =
              kimix::string("The same error has occurred ") +
              kimix::string(std::to_string(_error_streak)) +
              " times in a row "
            "(identical modulo line numbers/paths). Retrying the same fix is not "
            "working.\n"
            "Analyze the root cause: read the full error output, inspect the "
            "exact code involved, and form a new hypothesis before your next "
            "action.";
        out.push_back(std::move(injection));
        return true;
    }

    // Normal file-churn alert (same most_common priority as above): the
    // first entry meeting the threshold that was not already warned.
    for (const kimix::vector_map<kimix::string, int32_t>::value_type *entry : by_count) {
        const kimix::string &path = entry->first;
        const int32_t best_count = entry->second;
        if (best_count >= _file_warn &&
            _warned_files.find(path) == _warned_files.end()) {
            _warned_files.insert(path);
            _last_alert_step = step_no;
            DynamicInjection injection;
            injection.type = "target_churn";
            // target_churn.py:618-627 (verbatim).
            injection.content =
                kimix::string("You have edited `") + path + "` " +
                kimix::string(std::to_string(best_count)) +
                " times. If you are iterating "
                "without progress, pause and reconsider: verify your understanding "
                "of the failure, and consider rewriting the file or choosing a "
                "different approach instead of another small patch.";
            out.push_back(std::move(injection));
            return true;
        }
    }
    return true;
}

void TargetChurnProvider::on_context_compacted() {
    // target_churn.py:632-641.
    _cursor = 0;
    _file_counts.clear();
    _has_error_fingerprint = false;
    _error_streak = 0;
    _warned_files.clear();
    _strong_warned_files.clear();
    _error_warned_this_turn = false;
    _last_alert_step.reset();
}

} // namespace kimix::agent
