// Test for src/runtime/tools/shell_safety.h (plan: commit 0582e09 "Study from
// hermes" -- file/bash/safety.py + file/bash/output_enhance.py).
// This test covers:
// - command_detection_variants: collapse/dedup/deobfuscate
// - detect_hardline_command: all 7 ordered checks with reference goldens
// - check_hardline_blocked: obfuscation defeated via variants
// - foreground_background_guidance: 12 patterns + quoted-span stripping
// - base_command_name / interpret_exit_code: well-known exit codes
// - annotate_failure: 4000-char sample, module-not-found capture

#include "ut/ut.hpp"
#include <runtime/tools/shell_safety.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

kimix::vector<kimix::string> variants_of(const char* cmd) {
    kimix::vector<kimix::string> out;
    command_detection_variants(sv(cmd), out);
    return out;
}

const char* kFgBg = "Long-running process detected. Consider mode='send' "
                    "(background) + TaskOutput to avoid blocking on timeout.";
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "variants_basic"_test = [] {
        auto v = variants_of("rm -rf /");
        expect((v.size() == 1u));
        expect((v[0] == "rm -rf /"));
        v = variants_of("  rm   -rf  /  ");
        expect((v.size() == 1u));
        expect((v[0] == "rm -rf /"));
        v = variants_of("");
        expect(v.empty());
        v = variants_of("   ");
        expect(v.empty());
    };

    "variants_deobfuscate"_test = [] {
        auto v = variants_of("r\\m -rf /");
        expect((v.size() == 2u));
        expect((v[0] == "r\\m -rf /"));
        expect((v[1] == "rm -rf /"));
        v = variants_of("\"rm\" -rf /");
        expect((v.size() == 2u));
        expect((v[0] == "\"rm\" -rf /"));
        expect((v[1] == "rm -rf /"));
        v = variants_of("RM -RF /");
        expect((v.size() == 2u));
        expect((v[0] == "RM -RF /"));
        expect((v[1] == "rm -rf /"));
        // quoted span: original, dequoted-lowered, lowered all distinct
        v = variants_of("echo 'hi'");
        expect((v.size() == 2u));
        expect((v[0] == "echo 'hi'"));
        expect((v[1] == "echo hi"));
    };

    "hardline_recursive_delete"_test = [] {
        hardline_result r = detect_hardline_command(sv("rm -rf /"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`/`)"));
        r = detect_hardline_command(sv("rm -rf ~"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`~`)"));
        r = detect_hardline_command(sv("rm -rf $HOME"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`$home`)"));
        r = detect_hardline_command(sv("rm -rf ${HOME}"));
        expect(r.blocked);
        // the message names the RAW lowercased token (${home}), not $home
        expect((*r.description == "Recursive delete of protected root/home (`${home}`)"));
        r = detect_hardline_command(sv("rm -rf /*"));
        expect(r.blocked);
        r = detect_hardline_command(sv("rm -r /"));
        expect(r.blocked);
        r = detect_hardline_command(sv("rm -f /"));
        expect(r.blocked);
        // no r/f flags -> not armed
        r = detect_hardline_command(sv("rm /"));
        expect(!r.blocked);
        // deeper paths are never protected
        r = detect_hardline_command(sv("rm -rf /tmp/build"));
        expect(!r.blocked);
        // rmdir needs r/s
        r = detect_hardline_command(sv("rmdir /"));
        expect(!r.blocked);
        r = detect_hardline_command(sv("rmdir -r /"));
        expect(r.blocked);
        r = detect_hardline_command(sv("rmdir -s /"));
        expect(r.blocked);
        r = detect_hardline_command(sv("rmdir -p /"));
        expect(!r.blocked);
        // Windows drive roots
        r = detect_hardline_command(sv("rm -rf C:\\"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`c:\\`)"));
        r = detect_hardline_command(sv("rm -rf c:/"));
        expect(r.blocked);
        r = detect_hardline_command(sv("del /f /s /q C:\\*"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`c:\\*`)"));
        r = detect_hardline_command(sv("del /q C:\\Windows"));
        expect(!r.blocked);
        // rmdir vs rm alternation: "rmdir" matches the longer alternative
        r = detect_hardline_command(sv("rmdir -rf /"));
        expect(r.blocked);
    };

    "hardline_other_rules"_test = [] {
        // mkfs
        hardline_result r = detect_hardline_command(sv("mkfs.ext4 /dev/sda1"));
        expect(r.blocked);
        expect((*r.description == "Disk formatting command (`mkfs`) is blocked"));
        r = detect_hardline_command(sv("mkfs.fat -n USB /dev/sdb1"));
        expect(r.blocked);
        // dd to raw device
        r = detect_hardline_command(sv("dd if=/dev/zero of=/dev/sda"));
        expect(r.blocked);
        expect((*r.description == "`dd` writing to a raw device is blocked"));
        r = detect_hardline_command(sv("dd of=/dev/rdisk1"));
        expect(r.blocked);
        r = detect_hardline_command(sv("dd if=/dev/zero of=/tmp/out"));
        expect(!r.blocked);
        // power commands (first token)
        r = detect_hardline_command(sv("shutdown -h now"));
        expect(r.blocked);
        expect((*r.description == "System `shutdown` command is blocked"));
        r = detect_hardline_command(sv("reboot"));
        expect(r.blocked);
        r = detect_hardline_command(sv("poweroff"));
        expect(r.blocked);
        r = detect_hardline_command(sv("halt"));
        expect(r.blocked);
        // fork bomb
        r = detect_hardline_command(sv(":(){ :|:& };:"));
        expect(r.blocked);
        expect((*r.description == "Fork bomb pattern detected"));
        r = detect_hardline_command(sv(":(){ :& };:"));
        expect(!r.blocked);
        // kill PID 1 / $PPID
        r = detect_hardline_command(sv("kill 1"));
        expect(r.blocked);
        expect((*r.description == "`kill` targeting PID 1 (or `$PPID`) is blocked"));
        r = detect_hardline_command(sv("kill -9 1"));
        expect(r.blocked);
        r = detect_hardline_command(sv("kill $PPID"));
        expect(r.blocked);
        r = detect_hardline_command(sv("kill 123"));
        expect(!r.blocked);
        r = detect_hardline_command(sv("killall 1"));
        expect(!r.blocked);
        // Windows format
        r = detect_hardline_command(sv("format C:"));
        expect(r.blocked);
        expect((*r.description == "Windows `format` on a drive is blocked"));
        r = detect_hardline_command(sv("format D:\\"));
        expect(r.blocked);
        r = detect_hardline_command(sv("format C:\\Windows"));
        expect(!r.blocked);
        // benign
        r = detect_hardline_command(sv("ls -la"));
        expect(!r.blocked);
        r = detect_hardline_command(sv(""));
        expect(!r.blocked);
        r = detect_hardline_command(sv("   "));
        expect(!r.blocked);
    };

    "hardline_detect_does_not_deobfuscate"_test = [] {
        // detect_hardline_command only collapses whitespace; quote/backslash
        // tricks need check_hardline_blocked's variants.
        expect(!detect_hardline_command(sv("r\\m -rf /")).blocked);
        expect(!detect_hardline_command(sv("r'm' -rf /")).blocked);
        expect(!detect_hardline_command(sv("rm '-rf' /")).blocked);
    };

    "check_hardline_blocked"_test = [] {
        hardline_result r = check_hardline_blocked(sv("r\\m -rf /"));
        expect(r.blocked);
        expect((*r.description == "Recursive delete of protected root/home (`/`)"));
        r = check_hardline_blocked(sv("r'm' -rf /"));
        expect(r.blocked);
        r = check_hardline_blocked(sv("rm '-rf' /"));
        expect(r.blocked);
        r = check_hardline_blocked(sv("echo hello"));
        expect(!r.blocked);
        r = check_hardline_blocked(sv(""));
        expect(!r.blocked);
        r = check_hardline_blocked(sv("sudo rm -rf /"));
        expect(r.blocked);
    };

    "guidance_patterns"_test = [] {
        expect((*foreground_background_guidance(sv("npm run dev")) == kFgBg));
        expect((*foreground_background_guidance(sv("npm run start")) == kFgBg));
        expect(!foreground_background_guidance(sv("npm run build")).has_value());
        expect((*foreground_background_guidance(sv("pnpm run watch")) == kFgBg));
        expect((*foreground_background_guidance(sv("yarn run serve")) == kFgBg));
        expect((*foreground_background_guidance(sv("bun run dev")) == kFgBg));
        expect((*foreground_background_guidance(sv("next dev")) == kFgBg));
        expect((*foreground_background_guidance(sv("vite")) == kFgBg));
        expect((*foreground_background_guidance(sv("nodemon app.js")) == kFgBg));
        expect((*foreground_background_guidance(sv("uvicorn app:app")) == kFgBg));
        expect((*foreground_background_guidance(sv("gunicorn app:app")) == kFgBg));
        expect((*foreground_background_guidance(sv("python -m http.server")) == kFgBg));
        expect((*foreground_background_guidance(sv("docker compose up")) == kFgBg));
        expect((*foreground_background_guidance(sv("docker-compose up")) == kFgBg));
        expect((*foreground_background_guidance(sv("echo done &")) == kFgBg));
        expect((*foreground_background_guidance(sv("nohup python app.py")) == kFgBg));
        expect((*foreground_background_guidance(sv("setsid bash")) == kFgBg));
        // keywords inside quoted spans are ignored
        expect(!foreground_background_guidance(sv("echo 'npm run dev'")).has_value());
        expect(!foreground_background_guidance(sv("echo \"docker compose up\"")).has_value());
        // benign / empty
        expect(!foreground_background_guidance(sv("ls -la")).has_value());
        expect(!foreground_background_guidance(sv("npmrun dev")).has_value());
        expect(!foreground_background_guidance(sv("")).has_value());
        expect(!foreground_background_guidance(sv("   ")).has_value());
    };

    "base_command_name"_test = [] {
        expect((base_command_name(sv("/usr/bin/grep -r foo")) == "grep"));
        expect((base_command_name(sv("FOO=1 git diff")) == "git"));
        expect((base_command_name(sv("python -m http.server")) == "python"));
        expect((base_command_name(sv("app.exe --help")) == "app"));
        expect((base_command_name(sv("cmd.exe")) == "cmd"));
        expect((base_command_name(sv("echo hi && grep foo")) == "grep"));
        expect((base_command_name(sv("ls")) == "ls"));
        expect((base_command_name(sv("/x/y/tool.EXE arg")) == "tool"));
        expect((base_command_name(sv("python3.11 -V")) == "python3.11"));
        expect((base_command_name(sv("a=b c=d")) == ""));
        expect((base_command_name(sv("")) == ""));
        expect((base_command_name(sv("   ")) == ""));
    };

    "interpret_exit_code"_test = [] {
        auto r = interpret_exit_code(sv("grep foo"), 1);
        expect(r.has_value());
        expect((*r == "No matches found (not an error)"));
        expect(!interpret_exit_code(sv("grep foo"), 2).has_value());
        expect(!interpret_exit_code(sv("grep foo"), 0).has_value());
        expect(!interpret_exit_code(sv("grep foo"), std::nullopt).has_value());
        r = interpret_exit_code(sv("egrep foo"), 1);
        expect(r.has_value());
        expect((*r == "No matches found (not an error)"));
        r = interpret_exit_code(sv("diff a b"), 1);
        expect((*r == "Files differ (expected, not an error)"));
        r = interpret_exit_code(sv("find . -name x"), 1);
        expect((*r == "Some directories were inaccessible (partial results may "
                      "still be valid)"));
        r = interpret_exit_code(sv("test -f x"), 1);
        expect((*r == "Condition evaluated to false (expected, not an error)"));
        r = interpret_exit_code(sv("[ -f x ]"), 1);
        expect((*r == "Condition evaluated to false (expected, not an error)"));
        r = interpret_exit_code(sv("curl https://x"), 6);
        expect((*r == "Could not resolve host (DNS failure)"));
        r = interpret_exit_code(sv("curl https://x"), 7);
        expect((*r == "Failed to connect to host"));
        r = interpret_exit_code(sv("curl https://x"), 22);
        expect((*r == "HTTP error (server returned an error status)"));
        r = interpret_exit_code(sv("curl https://x"), 28);
        expect((*r == "Connection timed out"));
        expect(!interpret_exit_code(sv("curl https://x"), 99).has_value());
        r = interpret_exit_code(sv("git diff"), 1);
        expect(r.has_value());
        expect((*r == "Non-zero exit (often normal \xE2\x80\x94 e.g. 'git diff' "
                      "returns 1 when files differ)"));
        expect(!interpret_exit_code(sv("git diff"), 2).has_value());
        expect(!interpret_exit_code(sv("ls"), 1).has_value());
        expect(!interpret_exit_code(sv(""), 1).has_value());
        expect(!interpret_exit_code(sv("   "), 1).has_value());
        expect(!interpret_exit_code(sv("python -m pytest"), 5).has_value());
    };

    "annotate_failure"_test = [] {
        expect(!annotate_failure(sv(""), sv("x"), 1).has_value());
        auto r = annotate_failure(sv("bash: foo: command not found"), sv("foo"), 127);
        expect(r.has_value());
        expect((*r == "The command was not found. Check it is installed and on "
                      "PATH (use `which <cmd>` / `Get-Command <cmd>`)."));
        r = annotate_failure(sv("'foo' is not recognized as an internal or "
                                "external command"), sv("foo"), 1);
        expect(r.has_value());
        expect((*r == "The command was not found. Check it is installed and on "
                      "PATH (use `which <cmd>` / `Get-Command <cmd>`)."));
        r = annotate_failure(sv("ls: cannot access 'x': No such file or "
                                "directory"), sv("ls"), 2);
        expect((*r == "A file or directory referenced by the command does not "
                      "exist. Verify the path with `Glob`/ReadFile."));
        r = annotate_failure(sv("ModuleNotFoundError: No module named "
                                "'requests'"), sv("python"), 1);
        expect((*r == "Python module requests is missing. Install it (e.g. "
                      "`pip install requests`) or check the environment."));
        r = annotate_failure(sv("MODULENOTFOUNDERROR: No Module Named 'FooBar'"),
                             sv("python"), 1);
        expect((*r == "Python module FooBar is missing. Install it (e.g. "
                      "`pip install FooBar`) or check the environment."));
        r = annotate_failure(sv("Permission denied"), sv("cat"), 1);
        expect((*r == "Permission denied. Check file permissions (ls -la) or "
                      "ownership."));
        expect(!annotate_failure(sv("everything fine"), sv("ls"), 0).has_value());
        // the sample is capped at 4000 chars: a hit beyond that is invisible
        const std::string sample5000(5000, 'x');
        expect(!annotate_failure(sv(sample5000 + "command not found"), sv("x"), 1).has_value());
    };
}
