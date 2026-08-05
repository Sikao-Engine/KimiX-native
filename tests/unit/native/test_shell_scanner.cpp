// Test for src/runtime/parse/shell_scanner.h (plan 012 bash/pwsh scanners).
// This test covers:
// - BASH_FIX: Windows path rewrites, cd /d drops, fallback-name edits,
//   wrappers, nested $() depth abort
// - BASH_PROCESS_UNQUOTED: backslash -> slash conversions, quote/escape
//   preservation, $(...) / backtick descent
// - PWSH_FIX: quote repair + warning codes, --% handling, here-strings
// - PWSH_TRANSFORM: ??= ?. ?[ ?? ternary && || rewrites, region masks
// All golden outputs verified against the kimi-agent reference scanners.

#include "ut/ut.hpp"
#include <runtime/parse/shell_scanner.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::parse;

namespace {

kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

std::string apply_edits(const std::string& src, const kimix::vector<edit>& edits) {
    std::string out;
    size_t prev = 0;
    for (const edit& e : edits) {
        out.append(src, prev, e.start - prev);
        out.append(e.replacement.data(), e.replacement.size());
        prev = e.end;
    }
    out.append(src, prev, src.size() - prev);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "bash_fix_path_rewrites"_test = [] {
        const std::string cmd = "git log --oneline C:\\repo\\src";
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names, notes;
        kimix::string transformed;
        scan_shell(shell_dialect::BASH_FIX, sv(cmd), edits, &transformed, &names, &notes);
        expect(eq(edits.size(), 1u));
        expect(eq(edits[0].replacement, kimix::string("C:/repo/src")));
        expect(eq(notes.size(), 1u));
        expect(eq(apply_edits(cmd, edits), std::string("git log --oneline C:/repo/src")));
        expect(names.empty());
    };

    "bash_fix_fallback_command"_test = [] {
        const std::string cmd = "rev C:\\repo\\src";
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names, notes;
        scan_shell(shell_dialect::BASH_FIX, sv(cmd), edits, nullptr, &names, &notes);
        expect(eq(names.size(), 1u));
        expect(eq(names[0], kimix::string("rev")));
        // no wrapper -> no marker edit for the command word; only the path
        expect(eq(edits.size(), 1u));
        expect(eq(edits[0].replacement, kimix::string("C:/repo/src")));
        expect(eq(notes.size(), 1u));
        // with a wrapper the command word gets the "\x01<name>\x01" marker
        const std::string w = "sudo rev C:\\x";
        edits.clear();
        names.clear();
        notes.clear();
        scan_shell(shell_dialect::BASH_FIX, sv(w), edits, nullptr, &names, &notes);
        expect(eq(names.size(), 1u));
        expect(eq(names[0], kimix::string("rev")));
        bool has_marker = false;
        for (const edit& e : edits) {
            if (e.replacement == kimix::string("\x01rev\x01")) {
                has_marker = true;
            }
        }
        expect(has_marker);
    };

    "bash_fix_cd_dash_d"_test = [] {
        const std::string cmd = "cd /d D:\\x && dir";
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> notes;
        scan_shell(shell_dialect::BASH_FIX, sv(cmd), edits, nullptr, nullptr, &notes);
        // /d flag dropped + path rewrite
        expect(eq(edits.size(), 2u));
        expect(eq(edits[0].replacement, kimix::string()));
        expect(eq(apply_edits(cmd, edits), std::string("cd  D:/x && dir")));
        expect(eq(notes.size(), 2u));
        expect(eq(notes[0], kimix::string("cd /d")));
    };

    "bash_fix_wrapper_and_heredoc"_test = [] {
        // wrapper option value rewrite (env -C <path>) + fallback marker
        const std::string w = "env -C C:\\work python3 script.py";
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names, notes;
        scan_shell(shell_dialect::BASH_FIX, sv(w), edits, nullptr, &names, &notes);
        expect(eq(edits.size(), 2u));
        expect(eq(edits[0].replacement, kimix::string("C:/work")));
        expect(eq(edits[1].replacement, kimix::string("\x01python3\x01")));
        expect(eq(names.size(), 1u));
        expect(eq(names[0], kimix::string("python3")));
        // heredoc body is data: no path rewrite inside
        const std::string h = "cat <<EOF\nC:\\not\\a\\path\nEOF\n";
        edits.clear();
        names.clear();
        notes.clear();
        scan_shell(shell_dialect::BASH_FIX, sv(h), edits, nullptr, &names, &notes);
        expect(edits.empty());
        expect(names.empty());
    };

    "bash_fix_deep_nesting_abort"_test = [] {
        std::string deep;
        for (int i = 0; i < 2000; ++i) {
            deep += "$(";
        }
        deep += "rev";
        for (int i = 0; i < 2000; ++i) {
            deep += ")";
        }
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names;
        scan_shell(shell_dialect::BASH_FIX, sv(deep), edits, nullptr, &names);
        // reference: RecursionError -> command returned unchanged
        expect(edits.empty());
        expect(names.empty());
    };

    "process_unquoted_backslashes"_test = [] {
        const std::string cmd = "cd C:\\foo\\bar && echo hi";
        kimix::vector<edit> edits;
        kimix::string transformed;
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(cmd), edits, &transformed);
        expect(eq(edits.size(), 2u));
        expect(eq(transformed, kimix::string("cd C:/foo/bar && echo hi")));
        // metachar-escaped backslashes preserved; unquoted \n -> /n
        const std::string m = "echo \\\"a\\nb\\\"";
        edits.clear();
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(m), edits, &transformed);
        expect(eq(transformed, kimix::string("echo \\\"a/nb\\\"")));
        // inside single quotes: preserved
        const std::string q = "grep 'C:\\x' f";
        edits.clear();
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(q), edits, &transformed);
        expect(eq(transformed, kimix::string("grep 'C:\\x' f")));
        // inside $() : converted (subshell parses unquoted)
        const std::string s = "x=$(cd C:\\d && pwd)";
        edits.clear();
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(s), edits, &transformed);
        expect(eq(transformed, kimix::string("x=$(cd C:/d && pwd)")));
    };

    "pwsh_fix_repairs"_test = [] {
        auto run = [](const std::string& cmd, kimix::string& out, int& code) {
            kimix::vector<edit> edits;
            scan_shell(shell_dialect::PWSH_FIX, sv(cmd), edits, &out, nullptr, nullptr, &code);
        };
        kimix::string out;
        int code = 0;
        // valid
        run("Write-Output \"hi\"", out, code);
        expect(eq(code, 0));
        expect(eq(out, kimix::string("Write-Output \"hi\"")));
        // unclosed dq
        run("Write-Output \"unclosed", out, code);
        expect(eq(code, 1));
        expect(eq(out, kimix::string("Write-Output \"unclosed\"")));
        // unclosed sq
        run("x = 'sq", out, code);
        expect(eq(code, 2));
        expect(eq(out, kimix::string("x = 'sq'")));
        // unclosed here-string double
        run("@\"\nline\n", out, code);
        expect(eq(code, 3));
        expect(eq(out, kimix::string("@\"\nline\n\n\"@")));
        // unclosed block comment WITHOUT preceding code -> comment-only
        run("<# block", out, code);
        expect(eq(code, 8));
        expect(eq(out, kimix::string("<# block#>\n$null")));
        // unclosed block comment WITH code -> repair to "#>"
        run("Write-Output 1 <# block", out, code);
        expect(eq(code, 5));
        expect(eq(out, kimix::string("Write-Output 1 <# block#>")));
        // trailing comment (saw code)
        run("Write-Output 1 # c", out, code);
        expect(eq(code, 6));
        expect(eq(out, kimix::string("Write-Output 1 # c\n")));
        // comment-only
        run("# only", out, code);
        expect(eq(code, 8));
        expect(eq(out, kimix::string("# only\n$null")));
        // stop-parsing at EOF appends newline
        run("cmd /c echo --% \"x", out, code);
        expect(eq(code, 7));
        expect(eq(out, kimix::string("cmd /c echo --% \"x\n")));
        // unrepairable: dangling trailing backtick
        run("Write-Output `", out, code);
        expect(eq(code, -1));
        // empty / whitespace-only -> unrepairable
        run("   ", out, code);
        expect(eq(code, -1));
    };

    "pwsh_transform_operators"_test = [] {
        auto run = [](const std::string& cmd) {
            kimix::string out;
            kimix::vector<edit> edits;
            scan_shell(shell_dialect::PWSH_TRANSFORM, sv(cmd), edits, &out);
            return out;
        };
        expect(eq(run("$a = $b ?? $c"),
                  kimix::string("$a = if ($null -ne $b) { $b } else { $c }")));
        expect(eq(run("$x = $cond ? 1 : 2"),
                  kimix::string("$x = if ($cond) { 1 } else { 2 }")));
        expect(eq(run("$o?.Prop.Method()"),
                  kimix::string("$(if ($null -ne $o) { $o.Prop }).Method()")));
        expect(eq(run("$o?[0]"),
                  kimix::string("$(if ($null -ne $o) { $o[0] })")));
        expect(eq(run("$a ??= $b"),
                  kimix::string("if ($null -eq $a) { $a = $b }")));
        expect(eq(run("cmd1 && cmd2 || cmd3"),
                  kimix::string("cmd1; if ($?) { cmd2; if (-not $?) { cmd3 } }")));
        // strings/comments are untouched
        expect(eq(run("Write-Output \"a ?? b\""),
                  kimix::string("Write-Output \"a ?? b\"")));
        expect(eq(run("# comment ?? x"),
                  kimix::string("# comment ?? x")));
        // multiline region skipped (here-string content untouched)
        expect(eq(run("@\"\n$a ?? $b\n\"@\n"),
                  kimix::string("@\"\n$a ?? $b\n\"@\n")));
    };

    "region_mask_helpers"_test = [] {
        RegionMask mask(8);
        expect(mask.is_code(0));
        expect(mask.is_code(7));
        mask.mark(2, 5);
        expect(!mask.is_code(2));
        expect(!mask.is_code(4));
        expect(mask.is_code(1));
        expect(mask.is_code(5));
        expect(!mask.is_code(8)); // out of range -> not code
    };
}
