// Test for src/runtime/tools/shell_safety.h (plan: commit 0582e09 "Study from
// hermes" -- file/bash/safety.py + file/bash/output_enhance.py).
// This test covers:
// - command_detection_variants: collapse/dedup/deobfuscate
// - detect_hardline_command: all 7 ordered checks with reference goldens
// - check_hardline_blocked: obfuscation defeated via variants
// - foreground_background_guidance: 12 patterns + quoted-span stripping
// - base_command_name: first non-assignment command word
// - annotate_failure: 4000-char sample, module-not-found capture

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/tools/shell_safety.h>

#include <cstdio>
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

// 10k diverse commands (safe + blocked + obfuscated mixes) with per-template
// expectations: <count> commands, <variants> total detection variants,
// <detect> blocked by detect_hardline_command, <check> blocked by
// check_hardline_blocked.  The templates mirror the unit-test goldens above,
// so a semantics break shows up as a count mismatch.
struct cmd_fixtures {
    kimix::vector<kimix::string> cmds;
    size_t e_variants = 0;
    size_t e_detect = 0;
    size_t e_check = 0;
    double bytes = 0.0;
};

cmd_fixtures build_cmd_fixtures() {
    cmd_fixtures f;
    f.cmds.reserve(10000);
    char buf[128];
    const size_t kRm = 2000, kSafe = 2000, kObf = 1000, kMkfs = 1000,
                 kDd = 1000, kShut = 1000, kKill = 500, kKillSafe = 500,
                 kFmt = 500, kGit = 500;
    for (size_t i = 0; i < kRm; ++i) {
        std::snprintf(buf, sizeof(buf), "rm -rf / ; echo %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kSafe; ++i) {
        std::snprintf(buf, sizeof(buf), "echo hello world %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kObf; ++i) {
        std::snprintf(buf, sizeof(buf), "r\\m -rf / ; echo x %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kMkfs; ++i) {
        std::snprintf(buf, sizeof(buf), "mkfs.ext4 /dev/sda%zu", i % 4);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kDd; ++i) {
        std::snprintf(buf, sizeof(buf), "dd if=/dev/zero of=/dev/sda%zu",
                      i % 4);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kShut; ++i) {
        std::snprintf(buf, sizeof(buf), "'shutdown' -h now ; echo %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kKill; ++i) {
        std::snprintf(buf, sizeof(buf), "kill -9 1 ; echo %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kKillSafe; ++i) {
        std::snprintf(buf, sizeof(buf), "kill 123");
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kFmt; ++i) {
        std::snprintf(buf, sizeof(buf), "format C: ; echo %zu", i);
        f.cmds.emplace_back(buf);
    }
    for (size_t i = 0; i < kGit; ++i) {
        std::snprintf(buf, sizeof(buf), "git status && grep foo %zu", i);
        f.cmds.emplace_back(buf);
    }
    f.e_variants = kRm * 1 + kSafe * 1 + kObf * 2 + kMkfs * 1 + kDd * 1 +
                   kShut * 2 + kKill * 1 + kKillSafe * 1 + kFmt * 2 + kGit * 1;
    f.e_detect = kRm + kMkfs + kDd + kKill + kFmt;
    f.e_check = kRm + kObf + kMkfs + kDd + kShut + kKill + kFmt;
    for (const auto& c : f.cmds) {
        f.bytes += static_cast<double>(c.size());
    }
    return f;
}
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

    // ---------------------------------------------------------------------------
    // Benchmarks -- shell-safety kernels (kimix_bench contract, bench_util.h).
    // Production shape: every tool command is checked with
    // check_hardline_blocked (variants + detect), detect_hardline_command and
    // command_detection_variants run on every command in a session, and
    // base_command_name on every launched process name.  Workloads: 10k
    // diverse commands (safe + blocked + obfuscated mixes, quotes/escapes)
    // and 10k process paths.
    // ---------------------------------------------------------------------------

    "bench_detect_hardline_10k"_test = [] {
        const cmd_fixtures f = build_cmd_fixtures();
        size_t blocked = 0;
        kimix_bench::run("shell/detect_hardline_10k", [&] {
            for (const auto& c : f.cmds) {
                hardline_result r = detect_hardline_command(kimix::string_view(c));
                if (r.blocked) {
                    ++blocked;
                }
            }
        }, 10000, f.bytes);
        expect((blocked % f.e_detect == 0u));
        expect((blocked > 0u));
        kimix_bench::sink(blocked);
    };

    "bench_check_blocked_10k"_test = [] {
        const cmd_fixtures f = build_cmd_fixtures();
        size_t blocked = 0;
        kimix_bench::run("shell/check_blocked_10k", [&] {
            for (const auto& c : f.cmds) {
                hardline_result r = check_hardline_blocked(kimix::string_view(c));
                if (r.blocked) {
                    ++blocked;
                }
            }
        }, 10000, f.bytes);
        expect((blocked % f.e_check == 0u));
        expect((blocked > 0u));
        kimix_bench::sink(blocked);
    };

    "bench_variants_10k"_test = [] {
        const cmd_fixtures f = build_cmd_fixtures();
        kimix::vector<kimix::string> vout;
        size_t vtotal = 0;
        kimix_bench::run("shell/variants_10k", [&] {
            for (const auto& c : f.cmds) {
                command_detection_variants(kimix::string_view(c), vout);
                vtotal += vout.size();
            }
        }, 10000, f.bytes);
        expect((vtotal % f.e_variants == 0u));
        expect((vtotal > 0u));
        kimix_bench::sink(vtotal);
    };

    "bench_base_command_name_10k"_test = [] {
        kimix::vector<kimix::string> cmds;
        cmds.reserve(10000);
        double bytes = 0.0;
        for (size_t i = 0; i < 10000; ++i) {
            char b[80];
            std::snprintf(b, sizeof(b), "/usr/bin/kit_%04zu.exe --work %zu",
                          i, i);
            cmds.emplace_back(b);
            bytes += static_cast<double>(cmds.back().size());
        }
        size_t size_total = 0;
        kimix_bench::run("shell/base_cmd_name_10k", [&] {
            for (const auto& c : cmds) {
                size_total += base_command_name(kimix::string_view(c)).size();
            }
        }, 10000, bytes);
        // Correctness pass (unmeasured): every path maps to its expected stem.
        for (size_t i = 0; i < 10000; ++i) {
            char exp[32];
            std::snprintf(exp, sizeof(exp), "kit_%04zu", i);
            expect((base_command_name(kimix::string_view(cmds[i])) == exp));
        }
        expect((size_total >= 5u * 10000u));
        kimix_bench::sink(size_total);
    };
}
