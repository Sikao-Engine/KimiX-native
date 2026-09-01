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
#include "bench_util.h"
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

    // ------------------------------------------------------------------
    // Benchmarks (kimix_bench harness; timings on stderr as [bench] ...).
    // Each case asserts scan invariants before timing so a broken path is
    // never measured.
    // ------------------------------------------------------------------

    "bench_bash_fix_cmdline_10k"_test = [] {
        // 10k-char command line with many quoted/escaped segments.
        std::string cmd;
        cmd.reserve(20000);
        const char* seg =
            "echo \"a b\" 'c d' C:\\repo\\src && cd /d D:\\work\\data\n";
        size_t blocks = 0;
        while (cmd.size() < 10000) {
            cmd += seg;
            ++blocks;
        }
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names, notes;
        scan_shell(shell_dialect::BASH_FIX, sv(cmd), edits, nullptr, &names, &notes);
        // per block: path edit for C:\repo\src, /d flag drop, path edit for
        // D:\work\data; one note per edit; no fallback names here.
        expect(eq(edits.size(), blocks * 3));
        expect(eq(notes.size(), blocks * 3));
        expect(names.empty());
        expect(apply_edits(cmd, edits).find('/') != std::string::npos);
        kimix_bench::run("shell/bash_fix_cmdline_10k",
                         [&] { scan_shell(shell_dialect::BASH_FIX, sv(cmd), edits,
                                          nullptr, &names, &notes); },
                         1, static_cast<double>(cmd.size()));
        kimix_bench::sink(edits.size());
    };

    "bench_bash_fix_script_1k"_test = [] {
        // 1k-line shell script with path rewrites + fallback detection.
        std::string script;
        script.reserve(1u << 16);
        for (int i = 0; i < 500; ++i) {
            script += "cp C:\\a\\b D:\\c\\d && echo ok # setup\n";
            script += "rev C:\\repo\\src\n";
        }
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> names, notes;
        scan_shell(shell_dialect::BASH_FIX, sv(script), edits, nullptr, &names, &notes);
        expect(eq(names.size(), 500u));        // "rev" is a fallback name
        expect(eq(edits.size(), 1500u));       // 2 path edits + 1 path per pair
        expect(eq(notes.size(), 1500u));
        kimix_bench::run("shell/bash_fix_script_1k",
                         [&] { scan_shell(shell_dialect::BASH_FIX, sv(script),
                                          edits, nullptr, &names, &notes); },
                         1, static_cast<double>(script.size()));
        kimix_bench::sink(edits.size() + names.size());
    };

    "bench_process_unquoted_10k"_test = [] {
        // 10k-char line with heavily quoted/escaped segments + subshells.
        std::string cmd;
        cmd.reserve(12000);
        const char* seg = "echo \"C:\\a b\\c\" C:\\d\\e && x=$(cd C:\\f && pwd)\n";
        while (cmd.size() < 10000) {
            cmd += seg;
        }
        kimix::vector<edit> edits;
        kimix::string transformed;
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(cmd), edits, &transformed);
        size_t slashes = 0;
        for (char c : transformed) {
            if (c == '/') {
                ++slashes;
            }
        }
        // Backslashes inside single quotes / ANSI-C quotes / non-escaped dq
        // content are preserved by design, so only assert the 1:1 contract:
        // every edit is a one-char backslash -> slash replacement.
        expect(edits.size() > 0u);
        expect(eq(transformed.size(), cmd.size()));
        expect(eq(slashes, edits.size()));
        kimix_bench::run("shell/process_unquoted_10k",
                         [&] { scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED,
                                          sv(cmd), edits, &transformed); },
                         1, static_cast<double>(cmd.size()));
        kimix_bench::sink(edits.size());
    };

    "bench_process_unquoted_soup"_test = [] {
        // Pathological quoting/escape soup: single/double quotes, ansi-c
        // $'...', backticks and $(...) mixed with Windows paths.
        const std::string cmd =
            "cd C:\\foo\\bar && echo hi grep 'C:\\x' f x=$(cd C:\\d && pwd) "
            "echo `cd C:\\k` && echo $'an\\'si' \"dq \\\" esc \\\\ # n\"";
        kimix::vector<edit> edits;
        kimix::string transformed;
        scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED, sv(cmd), edits, &transformed);
        expect(edits.size() > 0u);
        expect(transformed.find('/') != kimix::string::npos);
        expect(apply_edits(cmd, edits) == transformed);
        kimix_bench::run("shell/process_unquoted_soup",
                         [&] { scan_shell(shell_dialect::BASH_PROCESS_UNQUOTED,
                                          sv(cmd), edits, &transformed); },
                         1, static_cast<double>(cmd.size()));
        kimix_bench::sink(edits.size());
    };

    "bench_pwsh_fix_script_1mb"_test = [] {
        // PowerShell script-like 1 MiB input: quotes, comments, assignments.
        std::string ps;
        ps.reserve(1u << 20);
        const char* seg = "Write-Output \"hi there\" # trailing\n$x = 'sq'\nGet-Date\n# whole-line comment\n";
        while (ps.size() < (size_t{1} << 20)) {
            ps += seg;
        }
        kimix::vector<edit> edits;
        kimix::string out;
        int code = 0;
        scan_shell(shell_dialect::PWSH_FIX, sv(ps), edits, &out, nullptr, nullptr, &code);
        expect(eq(code, 0));
        expect(eq(kimix::string_view(out), kimix::string_view(ps)));
        expect(edits.empty());
        kimix_bench::run("shell/pwsh_fix_script_1mb",
                         [&] { scan_shell(shell_dialect::PWSH_FIX, sv(ps), edits,
                                          &out, nullptr, nullptr, &code); },
                         1, static_cast<double>(ps.size()));
        kimix_bench::sink(out.size());
    };

    "bench_pwsh_fix_soup"_test = [] {
        // Pathological quoting: escaped quotes, doubled quotes, here-string,
        // trailing comment — all in one repairable script.
        const std::string ps =
            "Write-Output \"a`\"b\" 'c''d' $x\n"
            "Set-Location 'C:\\x'\n"
            "# t\n"
            "@\"\n"
            "$inside = 1\n"
            "\"@\n";
        kimix::vector<edit> edits;
        kimix::string out;
        int code = 0;
        scan_shell(shell_dialect::PWSH_FIX, sv(ps), edits, &out, nullptr, nullptr, &code);
        expect(eq(code, 0));
        expect(eq(kimix::string_view(out), kimix::string_view(ps)));
        kimix_bench::run("shell/pwsh_fix_soup",
                         [&] { scan_shell(shell_dialect::PWSH_FIX, sv(ps), edits,
                                          &out, nullptr, nullptr, &code); },
                         1, static_cast<double>(ps.size()));
        kimix_bench::sink(out.size());
    };

    "bench_pwsh_transform_script"_test = [] {
        // PowerShell 7 -> 5.1 transform on operator-heavy script lines
        // (??, ?:, ?., && / || rebuild region masks per operator found).
        std::string ps;
        for (int i = 0; i < 128; ++i) {
            ps += "$a = $b ?? $c\n";
            ps += "$x = $cond ? 1 : 2\n";
            ps += "cmd1 && cmd2 || cmd3\n";
            ps += "$o?.Prop.Method()\n";
        }
        kimix::vector<edit> edits;
        kimix::vector<kimix::string> warnings;
        kimix::string out;
        scan_shell(shell_dialect::PWSH_TRANSFORM, sv(ps), edits, &out, nullptr,
                   nullptr, nullptr, &warnings);
        expect(warnings.size() > 0u);
        expect(out.find("if ($null") != kimix::string::npos);
        for (const edit& e : edits) {
            expect(e.start <= e.end);
        }
        kimix_bench::run("shell/pwsh_transform_script",
                         [&] { scan_shell(shell_dialect::PWSH_TRANSFORM, sv(ps),
                                          edits, &out, nullptr, nullptr,
                                          nullptr, &warnings); },
                         1, static_cast<double>(ps.size()));
        kimix_bench::sink(warnings.size() + out.size());
    };
}
