/* bash_parser.cpp — the public native API of the Git Bash compatibility
 * kernel: BashFix accessors/equality/warning, fix_bash_command,
 * bash_compatibility_prelude, process_unquoted.
 *
 * Ports of kimi-agent shell_compat/bash_core.py::BashFix (+ its `warning`
 * property), translator.py::translate (via Scanner::fix), the
 * bash_compatibility_prelude entry, and unquoted.py::_process_unquoted.
 */
#include "llm/bash_parser/bash_parser.h"

#include "llm/bash_parser/bp_fallback.h"
#include "llm/bash_parser/bp_scanner.h"
#include "llm/bash_parser/bp_unquoted.h"

namespace kimix::llm::bash_parser {

// ── BashFix ──────────────────────────────────────────────────────────────────

BashFix::BashFix(kimix::string command) : command_(std::move(command)) {}

BashFix::BashFix(kimix::string command, kimix::vector<kimix::string> replacements, kimix::vector<kimix::string> path_changes, kimix::vector<kimix::string> shell_wrappers, kimix::vector<kimix::string> nul_fixes, kimix::vector<kimix::string> unsupported, kimix::vector<kimix::string> cmd_conditional_fixes)
    : command_(std::move(command)), replacements_(std::move(replacements)), path_changes_(std::move(path_changes)), shell_wrappers_(std::move(shell_wrappers)), nul_fixes_(std::move(nul_fixes)), unsupported_(std::move(unsupported)), cmd_conditional_fixes_(std::move(cmd_conditional_fixes)) {}

bool BashFix::changed() const {
    return !replacements_.empty() || !path_changes_.empty() || !shell_wrappers_.empty() || !nul_fixes_.empty() || !cmd_conditional_fixes_.empty();
}

namespace src_llm_bash_parser_bash_parser {

kimix::string join_backticked(const kimix::vector<kimix::string> &items) {
    kimix::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.append(", ");
        }
        out.push_back('`');
        out.append(items[i]);
        out.push_back('`');
    }
    return out;
}

} // namespace src_llm_bash_parser_bash_parser
using namespace src_llm_bash_parser_bash_parser;

kimix::string BashFix::warning() const {
    // Reference `BashFix.warning`: sections joined by " ", ordered
    // unsupported, fallbacks, paths, shell wrappers, nul, cmd control flow.
    kimix::vector<kimix::string> parts;

    if (!unsupported_.empty()) {
        kimix::string details;
        for (size_t i = 0; i < unsupported_.size(); ++i) {
            if (i > 0) {
                details.append("; ");
            }
            details.push_back('`');
            details.append(unsupported_[i]);
            const auto reason = unsupported_reason(unsupported_[i]);
            // The reference formats f"`{name}` — {reason}".  The em dash is
            // U+2014 (UTF-8: e2 80 94).
            details.append("` \xe2\x80\x94 ");
            if (reason.has_value()) {
                details.append(*reason);
            }
        }
        kimix::string part("Command(s) with no Windows Git Bash equivalent: ");
        part.append(details);
        part.push_back('.');
        parts.push_back(std::move(part));
    }
    if (!replacements_.empty()) {
        kimix::string part("Added Windows Git Bash fallback(s) for native command(s): ");
        part.append(join_backticked(replacements_));
        part.push_back('.');
        parts.push_back(std::move(part));
    }
    if (!path_changes_.empty()) {
        kimix::string part("Rewrote Windows path(s) for Git Bash (backslashes to forward slashes; Git Bash virtual paths to native spellings): ");
        part.append(join_backticked(path_changes_));
        part.push_back('.');
        parts.push_back(std::move(part));
    }
    if (!shell_wrappers_.empty()) {
        kimix::string part("Removed redundant shell wrapper(s): ");
        part.append(join_backticked(shell_wrappers_));
        part.push_back('.');
        parts.push_back(std::move(part));
    }
    if (!nul_fixes_.empty()) {
        kimix::string part("Rewrote null-device redirection target(s) for Git Bash: ");
        part.append(join_backticked(nul_fixes_));
        part.append(" -> `/dev/null` (an unquoted `nul` would otherwise create an empty file named `nul`).");
        parts.push_back(std::move(part));
    }
    if (!cmd_conditional_fixes_.empty()) {
        kimix::string part("Rewrote cmd.exe control-flow construct(s) to bash equivalents (`if exist`/`if defined`/`if errorlevel`/`if ...==...` tests, `for %v in (...) do` loops, `@echo off|on` -> `set +x|-x`): ");
        part.append(join_backticked(cmd_conditional_fixes_));
        part.push_back('.');
        parts.push_back(std::move(part));
    }

    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out.append(parts[i]);
    }
    return out;
}

bool BashFix::operator==(const BashFix &other) const {
    return command_ == other.command_ && replacements_ == other.replacements_ && path_changes_ == other.path_changes_ && shell_wrappers_ == other.shell_wrappers_ && nul_fixes_ == other.nul_fixes_ && unsupported_ == other.unsupported_ && cmd_conditional_fixes_ == other.cmd_conditional_fixes_;
}

// ── entry points ─────────────────────────────────────────────────────────────

BashFix fix_bash_command(kimix::string_view command) {
    return fix_bash_command(command, FixOptions{});
}

BashFix fix_bash_command(kimix::string_view command, const FixOptions &options) {
    // translator.translate: empty input returns BashFix(command) byte-for-byte.
    if (command.empty()) {
        return BashFix(kimix::string(command));
    }
    Scanner scanner(command, options.temp_dir);
    return scanner.fix();
}

kimix::string bash_compatibility_prelude() {
    return compatibility_prelude();
}

kimix::string process_unquoted(kimix::string_view command) {
    return process_unquoted_impl(command);
}

} // namespace kimix::llm::bash_parser
