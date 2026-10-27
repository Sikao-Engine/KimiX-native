/* bp_wrappers.cpp — compatibility classification, wrapper state machine, and
 * redundant-shell unwrap rules.  Byte-for-byte ports of kimi-agent
 * shell_compat/wrappers.py (the LIVE-path functions: wrapper_step,
 * wrapper_path_inline_replacement, classify_pattern_option,
 * is_inline_script_word, shell_c_script_range, plausible_script_file,
 * unwrap_decision).  The IR-dispatcher-only wrappers of that module and
 * `fallback_prefix` (which lives in bp_fallback.cpp) are intentionally not
 * reproduced here — see the module README's exclusion list and bp_wrappers.h.
 *
 * The reader duck-type methods the reference reaches through `reader` are the
 * Scanner primitives (read_word / literal_word_value / redirection_after_fd_at)
 * plus path_replacement_for; every word boundary is therefore byte-identical to
 * the live scan.
 */
#include "llm/bash_parser/bp_wrappers.h"

#include "llm/bash_parser/bp_core.h"
#include "llm/bash_parser/bp_scanner.h"

namespace kimix::llm::bash_parser {

namespace src_llm_bash_parser_bp_wrappers {



// Python `str.startswith((" \t\r"))` skip cursor ("horizontal whitespace").
inline bool is_horizontal_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

// The _WRAPPER_PATH_OPTION_LONG frozenset, in the fixed order
// {"--chdir", "--output", "--arg-file"}: the options differ at char 3, so a
// word starts with at most one of them and the order never changes the result
// (see the task contract note).
constexpr kimix::string_view kWrapperPathLongOptions[] = {"--chdir", "--output", "--arg-file"};

// Keys of the reference `_WRAPPER_PATH_OPTIONS` dict: the wrapper kinds that
// take filesystem-path option values (env/sudo/time/xargs).  Gates the inline
// `--chdir=PATH`-style rewrite arm.
inline bool is_path_wrapper_kind(kimix::string_view kind) {
    return kind == "env" || kind == "sudo" || kind == "time" || kind == "xargs";
}

// Python `str.lower()` for the ASCII script-extension match (non-ASCII bytes
// pass through unchanged).

// `_plausible_script_file` extension table.
constexpr kimix::string_view kScriptFileExtensions[] = {".sh", ".bash", ".zsh", ".ksh", ".dash", ".ash", ".bats"};

} // namespace src_llm_bash_parser_bp_wrappers
using namespace src_llm_bash_parser_bp_wrappers;

// ── wrapper state machine ────────────────────────────────────────────────────

WrapperAction wrapper_step(WrapperState &state, kimix::string_view raw) {
    if (state.skip_next) {
        state.skip_next = false;
        if (state.opaque) {
            return WrapperAction::kInspect;
        }
        return WrapperAction::kSkip;
    }
    if (state.opaque) {
        return WrapperAction::kInspect;
    }
    // Option matching keys on the word VALUE (bash quote removal); fall back to
    // the raw spelling when the word has no statically decodable value.
    const kimix::optional<kimix::string> decoded = Scanner::literal_word_value(raw);
    const kimix::string_view word = decoded.has_value() ? kimix::string_view(*decoded) : raw;
    if (state.operands > 0 && !bp_starts_with(word, "-")) {
        // A plain operand before the command word (GNU `timeout` DURATION) is
        // wrapper data, not the wrapped command.
        state.operands -= 1;
        return WrapperAction::kSkip;
    }
    if (state.kind == "command" && (word == "-v" || word == "-V")) {
        return WrapperAction::kInspect;
    }
    if (state.kind == "command" &&
        (word == "-p" ||
         (bp_starts_with(word, "-") && !bp_starts_with(word, "--") && word.substr(1).find('p') != kimix::string_view::npos))) {
        state.opaque = true;
        return WrapperAction::kSkip;
    }
    if (state.kind == "env" && (word == "-S" || word == "--split-string")) {
        state.opaque = true;
        state.skip_next = true;
        return WrapperAction::kSkip;
    }
    if (state.kind == "env" &&
        (bp_starts_with(word, "--split-string=") || (bp_starts_with(word, "-S") && word != "-S"))) {
        return WrapperAction::kInspect;
    }
    if (word == "--") {
        return WrapperAction::kSkip;
    }
    if (wrapper_option_with_value(state.kind, word)) {
        state.skip_next = true;
        if (wrapper_path_option(state.kind, word)) {
            state.path_value = true;
        }
        return WrapperAction::kSkip;
    }
    if (bp_starts_with(word, "-")) {
        return WrapperAction::kSkip;
    }
    if (state.kind == "env" && is_assignment_word(raw)) {
        // NOTE: matches on the RAW spelling, not the decoded value.
        return WrapperAction::kSkip;
    }
    return WrapperAction::kCommand;
}

kimix::optional<kimix::string> wrapper_path_inline_replacement(WrapperState &state, kimix::string_view raw, const Scanner &scanner) {
    if (!is_path_wrapper_kind(state.kind)) {
        return std::nullopt;
    }
    for (const kimix::string_view option : kWrapperPathLongOptions) {
        if (bp_starts_with(raw, option) && raw.size() > option.size() && raw[option.size()] == '=') {
            const kimix::string_view value = raw.substr(option.size() + 1);
            const kimix::optional<kimix::string> replacement = scanner.path_replacement_for(value);
            state.skip_next = false;
            state.path_value = false;
            if (replacement.has_value()) {
                kimix::string out(option);
                out.push_back('=');
                out.append(*replacement);
                return out;
            }
            // The arm fired with no path change (empty means "consumed").
            return kimix::string{};
        }
    }
    return std::nullopt;
}

// ── pattern/program operand suppression (grep/sed/awk/rg) ────────────────────

kimix::optional<kimix::string_view> classify_pattern_option(kimix::string_view family, kimix::string_view word) {
    if (in_grep_pattern_family(family)) {
        if (word == "-e" || word == "--regexp") {
            return kimix::string_view("data-pending");
        }
        if (word == "-f" || word == "--file") {
            return kimix::string_view("path-pending");
        }
        if (bp_starts_with(word, "--regexp=")) {
            return kimix::string_view("data");
        }
        if (bp_starts_with(word, "--file=")) {
            return kimix::string_view("path");
        }
        if (bp_starts_with(word, "--include") || bp_starts_with(word, "--exclude")) {
            // Glob option (glued `--include=GLOB` or separate value).
            return kimix::string_view("data");
        }
        if (in_grep_value_options(word)) {
            return kimix::string_view("value-pending");
        }
        return std::nullopt;
    }
    if (family == "sed") {
        if (word == "-e" || word == "--expression") {
            return kimix::string_view("data-pending");
        }
        if (word == "-f" || word == "--file") {
            return kimix::string_view("path-pending");
        }
        if (bp_starts_with(word, "--expression=")) {
            return kimix::string_view("data");
        }
        if (bp_starts_with(word, "--file=")) {
            return kimix::string_view("path");
        }
        return std::nullopt;
    }
    // awk family.
    if (word == "-v" || word == "-F" || word == "--assign") {
        return kimix::string_view("data-pending");
    }
    if (word == "-f" || word == "--file") {
        return kimix::string_view("path-pending");
    }
    if (bp_starts_with(word, "--assign=")) {
        return kimix::string_view("data");
    }
    if (bp_starts_with(word, "--file=")) {
        return kimix::string_view("path");
    }
    return std::nullopt;
}

// ── runtime-reparsed scripts ─────────────────────────────────────────────────

bool is_inline_script_word(kimix::string_view raw) {
    return bp_starts_with(raw, "'") || bp_starts_with(raw, "\"") || bp_starts_with(raw, "$'") || bp_starts_with(raw, "$\"");
}

kimix::optional<std::pair<size_t, size_t>> shell_c_script_range(Scanner &reader, kimix::string_view source, size_t start, size_t end) {
    size_t k = start;
    size_t script_start = 0;
    size_t script_end = 0;
    bool have_script = false;
    bool expect_script = false;
    while (k < end) {
        while (k < end && is_horizontal_space(source[k])) {
            k += 1;
        }
        if (k >= end || in_operator_chars(source[k]) || source[k] == '#') {
            return std::nullopt;
        }
        const size_t word_end = reader.read_word(k, end, false);
        if (word_end <= k) {
            return std::nullopt;
        }
        if (expect_script) {
            // The word right after the `-c` option is the inline script.
            script_start = k;
            script_end = word_end;
            have_script = true;
            break;
        }
        const kimix::string_view raw = source.substr(k, word_end - k);
        // Key on the word VALUE so quoted option spellings (`sh '-c'`) reach
        // the script scan the same way raw ones do.
        const kimix::optional<kimix::string> opt = Scanner::literal_word_value(raw);
        if (!opt.has_value()) {
            return std::nullopt; // runtime expansion: not an option word we know
        }
        const kimix::string_view o = *opt;
        if (is_shell_c_script_option(o)) {
            expect_script = true;
        } else if (in_shell_value_options(o)) {
            // Value-taking option: skip its value word, keep looking for `-c`
            // (`bash -o pipefail -c '...'`).
            k = word_end;
            while (k < end && is_horizontal_space(source[k])) {
                k += 1;
            }
            if (k >= end || in_operator_chars(source[k]) || source[k] == '#') {
                return std::nullopt;
            }
            const size_t value_end = reader.read_word(k, end, false);
            if (value_end <= k) {
                return std::nullopt;
            }
            k = value_end;
            continue;
        } else if (o == "--") {
            // End of options: the next word is a script PATH, not an inline
            // `-c` script.
            return std::nullopt;
        } else if (bp_starts_with(o, "-") || bp_starts_with(o, "+")) {
            // another option (`-e`, `-l`): keep looking
        } else {
            // A plain operand before any `-c` (`sh script.sh`): not an
            // inline-script form.
            return std::nullopt;
        }
        k = word_end;
    }
    if (!have_script) {
        return std::nullopt;
    }
    return std::pair<size_t, size_t>{script_start, script_end};
}

// ── redundant-shell unwrap decision ──────────────────────────────────────────

bool plausible_script_file(kimix::string_view raw) {
    if (bp_starts_with(raw, "./") || bp_starts_with(raw, "../")) {
        return true;
    }
    if (bp_starts_with(raw, ".\\") || bp_starts_with(raw, "..\\")) {
        return true;
    }
    if (raw.find('/') != kimix::string_view::npos || raw.find('\\') != kimix::string_view::npos) {
        return true;
    }
    const kimix::string lower = bp_ascii_lower(raw);
    for (const kimix::string_view ext : kScriptFileExtensions) {
        if (bp_ends_with(lower, ext)) {
            return true;
        }
    }
    return false;
}

UnwrapDecision unwrap_decision(Scanner &reader, kimix::string_view source, size_t start, size_t end, bool assignment_prefix, bool wrapped) {
    if (assignment_prefix) {
        // `VAR=x bash -c 'echo $VAR'`: unwrapping would expand $VAR in the
        // outer shell before the scoped assignment takes effect.
        return UnwrapDecision{};
    }
    size_t j = start;
    while (j < end && is_horizontal_space(source[j])) {
        j += 1;
    }
    if (j >= end || in_operator_chars(source[j]) || source[j] == '#') {
        // Bare `bash` (or `bash && ...` / `bash` at EOF): not a compatibility
        // problem.
        return UnwrapDecision{};
    }
    const size_t next_end = reader.read_word(j, end, false);
    if (next_end <= j) {
        return UnwrapDecision{};
    }
    const kimix::string_view next_raw = source.substr(j, next_end - j);
    // Option/script-file decisions key on the word VALUE (bash quote removal):
    // a quoted option/script hides its role in the raw text, and a word with no
    // static value (`bash "$S"`) may open a cwd-relative script file, so the
    // unwrap is refused for it too.
    const kimix::optional<kimix::string> next_val_opt = Scanner::literal_word_value(next_raw);
    if (!next_val_opt.has_value()) {
        return UnwrapDecision{};
    }
    const kimix::string next_val = *next_val_opt;
    if (bp_starts_with(next_val, "-")) {
        // Optional leading login flag, then the `-c` family.
        size_t opt_end = next_end;
        kimix::string opt = next_val;
        if (opt == "-l" || opt == "-L" || opt == "--login") {
            size_t cursor = opt_end;
            while (cursor < end && is_horizontal_space(source[cursor])) {
                cursor += 1;
            }
            if (cursor >= end || in_operator_chars(source[cursor]) || source[cursor] == '#') {
                return UnwrapDecision{};
            }
            opt_end = reader.read_word(cursor, end, false);
            if (opt_end <= cursor) {
                return UnwrapDecision{};
            }
            const kimix::string_view opt_raw = source.substr(cursor, opt_end - cursor);
            const kimix::optional<kimix::string> opt_val_opt = Scanner::literal_word_value(opt_raw);
            if (!opt_val_opt.has_value()) {
                return UnwrapDecision{};
            }
            opt = *opt_val_opt;
        }
        if (!is_shell_c_option(opt)) {
            return UnwrapDecision{};
        }
        // The word after `-c` is the inline script.
        size_t k = opt_end;
        while (k < end && is_horizontal_space(source[k])) {
            k += 1;
        }
        if (k >= end || in_operator_chars(source[k]) || source[k] == '#') {
            return UnwrapDecision{}; // `bash -c` with no script
        }
        const size_t script_end = reader.read_word(k, end, false);
        if (script_end <= k) {
            return UnwrapDecision{};
        }
        size_t m = script_end;
        while (m < end && is_horizontal_space(source[m])) {
            m += 1;
        }
        if (m < end && !in_operator_chars(source[m]) && source[m] != '#') {
            // A digit run followed by a redirection operator (`2>/dev/null`,
            // `1>&2`) closes the wrapper segment like `;`/`&&`: it is NOT
            // script argv.
            if (!(is_ascii_digit(source[m]) && reader.redirection_after_fd_at(m, end))) {
                return UnwrapDecision{}; // trailing script argv
            }
        }
        const kimix::string_view script_raw = source.substr(k, script_end - k);
        const kimix::optional<kimix::string> script = Scanner::literal_word_value(script_raw);
        if (!script.has_value()) {
            // Expansions inside the script: the operand text cannot be
            // re-emitted, but its statically visible command words still need
            // their fallback definitions registered for the nested shell.
            UnwrapDecision decision;
            decision.action = UnwrapAction::kKeep;
            decision.script_span = std::pair<size_t, size_t>{k, script_end};
            decision.harvest = true;
            return decision;
        }
        // The wrapper stays; only the inline script is repaired in place.
        UnwrapDecision decision;
        decision.action = UnwrapAction::kFixScript;
        decision.keep_wrapper = wrapped;
        decision.script_span = std::pair<size_t, size_t>{k, script_end};
        decision.scan_index = script_end;
        return decision;
    }
    if (plausible_script_file(next_val)) {
        return UnwrapDecision{};
    }
    // `bash <command> ...`: drop the redundant shell word (plus the whitespace
    // that separated it from the real command).
    UnwrapDecision decision;
    decision.action = UnwrapAction::kUnwrap;
    decision.keep_wrapper = wrapped;
    decision.script_span = std::nullopt;
    decision.scan_index = j;
    return decision;
}

} // namespace kimix::llm::bash_parser
