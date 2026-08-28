// Test for the pwsh built-in tool kernels (builtin_tools/pwsh_tool.h).
// Coverage (plan pwsh.md §7.1):
// - detect_self_kill: all five ordered detectors (kill/tskill, taskkill,
//   stop-process+get-process, pkill/killall, wmic) with positive, negative,
//   and first-hit-wins cases; byte-exact hit descriptions
// - loop-variable PID resolution: bash `for pid in ...` and PowerShell
//   `foreach ($pid in ...)` headers; unresolvable sources never block
// - image-name matching: exact/stem/wildcard, short-name rejection
// - ASCII gate: non-ASCII input -> tool_status::unsupported
// - pkill regex-metacharacter gate: metachar patterns -> unsupported, plain
//   substring patterns handled natively
// - command_detection_variants: collapse/deobfuscate/lowercase + dedupe
// - self_kill_hint: message composition with the guidance block
//
// Golden vectors harvested from the Python reference
// (kimi-agent src/kimix/tools/file/bash/safety.py detect_self_kill) with
// protected_pids={4100,5000}, image_names={python.exe,python,kimi},
// cmdline="/usr/bin/python3 /home/u/kimi-cli/bin/kimi --agent worker".
#include "ut/ut.hpp"

#include "builtin_tools/pwsh_tool.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::pwsh;

namespace {

kimix::unordered_set<int64_t> test_pids() {
    kimix::unordered_set<int64_t> pids;
    pids.insert(4100);
    pids.insert(5000);
    return pids;
}

kimix::unordered_set<kimix::string, kimix::string_hash> test_names() {
    kimix::unordered_set<kimix::string, kimix::string_hash> names;
    names.insert("python.exe");
    names.insert("python");
    names.insert("kimi");
    return names;
}

const char *test_cmdline() {
    return "/usr/bin/python3 /home/u/kimi-cli/bin/kimi --agent worker";
}

// Run detect_self_kill with the shared golden identity; returns {status, desc}.
struct run_result {
    tool_status status;
    kimix::optional<kimix::string> desc;
};

run_result run(const char *cmd) {
    tool_status status = tool_status::ok;
    kimix::optional<kimix::string> desc =
        detect_self_kill(cmd, test_pids(), test_names(), test_cmdline(), status);
    return {status, desc};
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "empty_and_whitespace"_test = [] {
        expect(!run("").desc.has_value()) << "empty command is safe";
        expect(!run("   ").desc.has_value()) << "whitespace is safe";
        expect(!run("\t\n ").desc.has_value()) << "mixed whitespace is safe";
        expect(!run("echo hello").desc.has_value());
        expect(!run("Get-Date").desc.has_value());
        expect(!run("git status").desc.has_value());
    };

    "empty_protected_pids"_test = [] {
        tool_status status = tool_status::ok;
        kimix::unordered_set<int64_t> empty;
        auto desc = detect_self_kill("kill 4100", empty, test_names(),
                                     test_cmdline(), status);
        expect(status == tool_status::ok);
        expect(!desc.has_value()) << "no protected pids means no hit";
    };

    "detector1_kill_tskill_pid"_test = [] {
        expect(run("kill 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `kill`, which is the agent process "
                   "or one of its parent processes"));
        expect(run("kill -9 4100").desc.has_value());
        expect(run("kill 9999").desc.has_value() == false);
        expect(run("tskill 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `tskill`, which is the agent process "
                   "or one of its parent processes"));
        expect(run("tskill.exe 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `tskill.exe`, which is the agent "
                   "process or one of its parent processes"));
        expect(run("kill.exe 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `kill.exe`, which is the agent "
                   "process or one of its parent processes"));
        // case-insensitive collapse
        expect(run("KILL 4100").desc.has_value());
        // quoted / comma / multi-PID forms
        expect(run("kill '4100'").desc.has_value());
        expect(run("kill \"4100\"").desc.has_value());
        expect(run("kill 4100,5000").desc.has_value());
        expect(run("kill 1234 4100").desc.has_value());
        expect(run("kill -- 4100").desc.has_value());
        expect(run("kill -TERM 4100").desc.has_value());
        expect(run("kill 04100").desc.has_value());
        expect(run("kill 4100.").desc.has_value());
        // PowerShell expression style token: (Get-Process -Id 2100).Kill()
        // is handled by detector 3; the numeric target scanner alone:
        expect(run("kill 99999999999999999999").desc.has_value() == false)
            << "oversize digit run never equals an int64 protected PID";
    };

    "detector1_docker_podman_skip"_test = [] {
        expect(!run("docker kill 4100").desc.has_value());
        expect(!run("podman kill 4100").desc.has_value());
        expect(!run("kubectl kill 4100").desc.has_value());
        expect(!run("compose kill 4100").desc.has_value());
        expect(!run("docker compose kill 4100").desc.has_value());
        expect(!run("docker_kill 4100").desc.has_value())
            << "underscore is a word char, so no kill match at all";
        // a non-skip word before kill still hits
        expect(run("python-kill 4100").desc.has_value());
    };

    "segment_splitting"_test = [] {
        // kill in a later segment is still seen (segment splits only the
        // token collection, not the finditer scan)
        expect(run("echo a && kill 4100").desc.has_value());
        expect(run("kill 4100 && echo done").desc.has_value());
        // single & does NOT split: kill sees everything after
        expect(run("kill 4100 & echo").desc.has_value());
        expect(run("echo a & kill 4100").desc.has_value());
        expect(run("kill 4100|echo done").desc.has_value());
        expect(run("kill 4100||echo").desc.has_value());
        expect(run("kill 4100\necho done").desc.has_value());
    };

    "detector1_loop_variables"_test = [] {
        // Python _variable_pid_hit embeds the already-backticked `via`
        // ("`kill`") inside its own backticks -> ``kill`` (double).
        expect(run("for pid in 4100 5000; do kill $pid; done").desc ==
               std::optional<kimix::string>(
                   "kills PID 4100 via ``kill`` through loop variable `$pid` "
                   "(bound to PIDs 4100, 5000), which is the agent process "
                   "or one of its parent processes"));
        expect(run("for pid in 4100 5000; do kill ${pid}; done").desc.has_value());
        expect(run("for p in 4100; do kill \"$p\"; done").desc.has_value());
        expect(run("for PID in 4100; do kill $pid; done").desc.has_value())
            << "loop var lookup is case-insensitive";
        // unbound / non-protected / unresolvable never block
        expect(!run("for pid in 9000 9001; do kill $pid; done").desc.has_value());
        expect(!run("for pid in $(pgrep python); do kill $pid; done").desc.has_value());
        expect(!run("for pid in $list; do kill $pid; done").desc.has_value());
        expect(!run("for pid in *.pid; do kill $pid; done").desc.has_value());
        expect(!run("kill $pid").desc.has_value()) << "unbound variable";
    };

    "detector2_taskkill"_test = [] {
        expect(run("taskkill /PID 4100 /F").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `taskkill`, which is the agent "
                   "process or one of its parent processes"));
        expect(run("taskkill /pid 4100").desc.has_value());
        expect(!run("taskkill /PID 9999 /F").desc.has_value());
        expect(run("taskkill /FI \"PID eq 4100\"").desc.has_value());
        expect(!run("taskkill /FI \"PID eq 9999\"").desc.has_value());
        expect(run("taskkill.exe /PID 4100").desc.has_value());
        expect(run("for pid in 4100; do taskkill /PID $pid; done").desc.has_value());
        expect(!run("taskkill /PID $pid").desc.has_value()) << "unbound var";
        expect(!run("taskkill /im").desc.has_value()) << "/IM with no name";
    };

    "detector2_taskkill_image_name"_test = [] {
        expect(run("taskkill /IM python.exe /F").desc ==
               std::optional<kimix::string>(
                   "kills by image name `python` via `taskkill /IM`, which "
                   "also matches the agent process"));
        expect(run("taskkill /im python /F").desc.has_value());
        expect(run("taskkill /IM PYTHON.EXE /F").desc.has_value());
        expect(run("taskkill /IM python* /F").desc.has_value());
        expect(run("taskkill /IM pyth* /F").desc.has_value());
        expect(!run("taskkill /IM py* /F").desc.has_value())
            << "prefix shorter than 3 chars never matches";
        expect(!run("taskkill /IM ki*").desc.has_value());
        expect(run("taskkill /IM kimi").desc.has_value());
        expect(!run("taskkill /IM notepad.exe").desc.has_value());
        expect(run("taskkill /IM \"python.exe\"").desc.has_value());
        expect(!run("taskkill /IM python**").desc.has_value())
            << "double wildcard leaves a trailing '*' in the stem";
    };

    "detector3_stop_process"_test = [] {
        expect(run("Stop-Process -Id 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `Stop-Process`, which is the agent "
                   "process or one of its parent processes"));
        expect(run("stop-process -id 4100 -Force").desc.has_value());
        expect(!run("Stop-Process -Id 9999").desc.has_value());
        expect(run("Stop-Process -Name python").desc ==
               std::optional<kimix::string>(
                   "kills by process name `python` via `Stop-Process`, which "
                   "also matches the agent process"));
        expect(run("Stop-Process -Name python.exe").desc.has_value());
        expect(!run("Stop-Process -Name python3.12").desc.has_value())
            << "python3.12 keeps its full stem, no match";
        expect(!run("Stop-Process -Name notepad").desc.has_value());
        expect(!run("Stop-Process -Name py*").desc.has_value());
        expect(!run("Stop-Process -Name ***").desc.has_value());
        expect(run("Stop-Process python").desc.has_value())
            << "positional name token";
        expect(run("stop-process python -force").desc.has_value());
        expect(!run("stop-process -whatif").desc.has_value());
        expect(run("foreach ($pid in 4100,5000) { Stop-Process -Id $pid }").desc ==
               std::optional<kimix::string>(
                   "kills PID 4100 via ``Stop-Process`` through loop variable "
                   "`$pid` (bound to PIDs 4100, 5000), which is the agent "
                   "process or one of its parent processes"));
        expect(!run("foreach ($pid in 9000,9001) { Stop-Process -Id $pid }").desc.has_value());
        expect(!run("foreach ($pid in $list) { Stop-Process -Id $pid }").desc.has_value());
        expect(!run("Stop-Process -Id $pid").desc.has_value());
        // variable whose name starts with a digit never binds
        expect(!run("foreach ($9pid in 4100) { stop-process -id $9pid }").desc.has_value());
    };

    "detector3_get_process_piped"_test = [] {
        expect(run("Get-Process -Id 4100 | Stop-Process").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `Get-Process` piped to a kill, which "
                   "is the agent process or one of its parent processes"));
        expect(run("(Get-Process -Id 4100).Kill()").desc.has_value());
        expect(!run("(Get-Process -Id 2100).Kill()").desc.has_value());
        expect(run("Get-Process -Name python | kill").desc ==
               std::optional<kimix::string>(
                   "kills by process name `python` via `Get-Process` piped to "
                   "a kill, which also matches the agent process"));
        expect(!run("Get-Process -Id 9999").desc.has_value())
            << "no kill context";
        expect(!run("Get-Process -Name notepad | Stop-Process").desc.has_value());
        expect(run("Get-Process python | Stop-Process -Force").desc.has_value());
        expect(run("get-process python | stop-process -force").desc.has_value());
        expect(run("get-process -id 4100 | foreach { $_.kill() }").desc.has_value())
            << ".kill() provides the kill context";
        expect(!run("foreach ($pid in 4100) { (Get-Process -Id $pid).Kill() }").desc.has_value())
            << "no kill context triggers the get-process scan";
    };

    "detector4_pkill"_test = [] {
        expect(run("pkill python").desc ==
               std::optional<kimix::string>(
                   "kills processes matching `python` via `pkill`, which also "
                   "matches the agent process"));
        expect(run("pkill -f kimi").desc ==
               std::optional<kimix::string>(
                   "kills processes matching `kimi` via `pkill -f`, which "
                   "also matches the agent process"));
        expect(!run("pkill -f notepad").desc.has_value());
        expect(!run("pkill notepad").desc.has_value());
        expect(run("pkill kimi").desc.has_value());
        expect(run("pkill -f python").desc.has_value());
        expect(run("pkill -f \"python\"").desc.has_value());
        expect(run("pkill --full kimi").desc.has_value());
        expect(run("pkill -9 kimi").desc.has_value());
        expect(run("pkill.exe python").desc.has_value());
        // substring semantics of the native plain-pattern subset
        expect(run("pkill pyth").desc.has_value());
        expect(run("pkill thon").desc.has_value());
        expect(run("pkill ytho").desc.has_value());
        // -f matches the full command line haystack too
        expect(run("pkill -f python3").desc.has_value());
        expect(run("pkill -f 'kim'").desc.has_value());
        expect(!run("pkill agent").desc.has_value())
            << "substring only matches without -f against image names";
        // tokens without alphanumerics are skipped
        expect(!run("pkill \"()\"").desc.has_value());
        expect(!run("pkill $@").desc.has_value());
        expect(!run("pkill \"\"").desc.has_value());
        expect(!run("pkill ''").desc.has_value());
        expect(!run("pkill -f -- -kimi").desc.has_value());
    };

    "detector4_killall"_test = [] {
        expect(run("killall python").desc ==
               std::optional<kimix::string>(
                   "kills by process name `python` via `killall`, which also "
                   "matches the agent process"));
        expect(!run("killall notepad").desc.has_value());
        expect(run("killall python.exe").desc.has_value());
        expect(run("killall python*").desc.has_value());
        expect(!run("killall py*").desc.has_value());
        expect(run("killall -9 python").desc.has_value());
    };

    "detector5_wmic"_test = [] {
        expect(run("wmic process where ProcessId=4100 delete").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `wmic`, which is the agent process "
                   "or one of its parent processes"));
        expect(!run("wmic process where ProcessId=9999 delete").desc.has_value());
        expect(run("wmic process where ProcessId=4100 call terminate").desc.has_value());
        expect(!run("wmic process call terminate 4100").desc.has_value())
            << "terminate without processid= is not a PID kill";
        expect(run("wmic process where \"ProcessId=4100\" delete").desc.has_value());
        expect(!run("wmic process list brief").desc.has_value());
        expect(!run("wmic process where ProcessId=$pid delete").desc.has_value())
            << "unbound variable";
        expect(run("for pid in 4100; do wmic process where ProcessId=$pid delete; done").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `wmic` through loop variable `$pid` "
                   "(bound to PIDs 4100), which is the agent process or one "
                   "of its parent processes"));
        expect(run("wmic.exe process where processid=4100 delete").desc.has_value());
        expect(run("WMIC PROCESS WHERE PROCESSID=4100 DELETE").desc.has_value());
        expect(run("wmic process where processid = 4100 delete").desc.has_value());
    };

    "first_hit_wins_ordering"_test = [] {
        // detector 1 (kill) beats detector 2 (taskkill)
        auto desc = run("kill 4100 ; taskkill /IM python.exe").desc;
        expect(desc.has_value());
        expect(desc->find("`kill`") != kimix::string::npos);
        // detector 2 beats detector 3; the /IM description contains the
        // literal "`taskkill /IM`" (Python f-string), not "`taskkill`"
        desc = run("taskkill /IM python.exe /F ; stop-process -id 4100").desc;
        expect(desc.has_value());
        expect(desc->find("`taskkill /IM`") != kimix::string::npos);
        // detector 3 beats detector 4
        desc = run("stop-process -id 4100; pkill python").desc;
        expect(desc.has_value());
        expect(desc->find("`Stop-Process`") != kimix::string::npos);
        // detector 4 beats detector 5
        desc = run("pkill python; wmic process where processid=4100 delete").desc;
        expect(desc.has_value());
        expect(desc->find("`pkill`") != kimix::string::npos);
    };

    "ascii_gate"_test = [] {
        tool_status status = tool_status::ok;
        auto desc = detect_self_kill("kill 4100 # caf\xC3\xA9", test_pids(),
                                     test_names(), test_cmdline(), status);
        expect(status == tool_status::unsupported)
            << "non-ASCII command routes to Python";
        expect(!desc.has_value());

        status = tool_status::ok;
        desc = detect_self_kill("kill 4100", test_pids(), test_names(),
                                "cmdline caf\xC3\xA9", status);
        expect(status == tool_status::unsupported)
            << "non-ASCII cmdline routes to Python";

        // pure ASCII passes
        status = tool_status::ok;
        desc = detect_self_kill("kill 4100", test_pids(), test_names(),
                                test_cmdline(), status);
        expect(status == tool_status::ok);
        expect(desc.has_value());
    };

    "pkill_metachar_gate"_test = [] {
        // patterns with regex metacharacters -> unsupported (route to Python)
        const char *meta_cases[] = {
            "pkill ^kimi",
            "pkill -f \"^kimi\"",
            "pkill kimi[0-9]",
            "pkill k.m",
            "pkill k+m",
            "pkill k?m",
            "pkill (kimi)",
            "pkill {kimi}",
            "pkill \\kimi",
            "pkill $kimi",
            "pkill \"()\"",
            "pkill $@",
        };
        for (const char *cmd : meta_cases) {
            tool_status status = tool_status::ok;
            auto desc = detect_self_kill(cmd, test_pids(), test_names(),
                                         test_cmdline(), status);
            expect(status == tool_status::unsupported)
                << "metachar pattern routes to Python: " << cmd;
            expect(!desc.has_value());
        }
        // plain patterns stay native
        expect(run("pkill python").status == tool_status::ok);
        expect(run("pkill -9 kimi").status == tool_status::ok);
        // non-pkill metachar tokens do NOT gate
        expect(run("taskkill /FI \"PID eq 4100\"").status == tool_status::ok);
        expect(run("wmic process where \"ProcessId=4100\" delete").status == tool_status::ok);
        expect(run("(get-process -id 4100).kill()").status == tool_status::ok);
        expect(run("echo a && kill 4100").status == tool_status::ok);
        // a later pkill with metachars gates even if an earlier pkill is plain
        {
            tool_status status = tool_status::ok;
            auto desc = detect_self_kill("pkill -f kimi ; pkill ^x", test_pids(),
                                         test_names(), test_cmdline(), status);
            expect(status == tool_status::unsupported);
        }
    };

    "variants"_test = [] {
        kimix::vector<kimix::string> out;
        command_detection_variants("kill 4100", out);
        expect(eq(out.size(), size_t(1)));
        expect(out[0] == "kill 4100");

        command_detection_variants("k\\i\\l\\l 4100", out);
        expect(eq(out.size(), size_t(2)));
        expect(out[0] == "k\\i\\l\\l 4100");
        expect(out[1] == "kill 4100");

        command_detection_variants("'kill' 4100", out);
        expect(eq(out.size(), size_t(2)));
        expect(out[0] == "'kill' 4100");
        expect(out[1] == "kill 4100");

        command_detection_variants("KiLl  4100", out);
        expect(eq(out.size(), size_t(2)));
        expect(out[0] == "KiLl 4100");
        expect(out[1] == "kill 4100");

        command_detection_variants("", out);
        expect(eq(out.size(), size_t(0)));
        command_detection_variants("   ", out);
        expect(eq(out.size(), size_t(0)));
    };

    "self_kill_hint_composition"_test = [] {
        tool_status status = tool_status::ok;
        auto hint = self_kill_hint("taskkill /PID 4100", test_pids(),
                                   test_names(), test_cmdline(), 7100, status);
        expect(status == tool_status::ok);
        expect(hint.has_value());
        const std::string expected =
            "The command targets PID 4100 via `taskkill`, which is the agent "
            "process or one of its parent processes. Executing it would "
            "terminate this agent session (current agent PID: 7100). "
            "If you meant to stop a different process, re-check its PID first "
            "(`tasklist` / `Get-Process` / `ps aux`) and retry with a PID that "
            "does not belong to the agent. If the target merely shares the "
            "agent's image name, terminate that specific PID instead of a "
            "name/pattern match. If you really intend to stop or restart the "
            "agent itself, ask the user to do it from outside this session.";
        expect(hint.value() == expected) << "byte-exact hint composition";

        // safe command -> nullopt
        status = tool_status::ok;
        hint = self_kill_hint("echo hello", test_pids(), test_names(),
                              test_cmdline(), 7100, status);
        expect(status == tool_status::ok);
        expect(!hint.has_value());

        // obfuscated variant still caught via deobfuscation
        status = tool_status::ok;
        hint = self_kill_hint("k\\i\\l\\l 4100", test_pids(), test_names(),
                              test_cmdline(), 7100, status);
        expect(status == tool_status::ok);
        expect(hint.has_value());

        // metachar pkill pattern routes the whole hint to Python
        status = tool_status::ok;
        hint = self_kill_hint("pkill ^kimi", test_pids(), test_names(),
                              test_cmdline(), 7100, status);
        expect(status == tool_status::unsupported);
        expect(!hint.has_value());

        // non-ASCII routes to Python
        status = tool_status::ok;
        hint = self_kill_hint("kill 4100 # caf\xC3\xA9", test_pids(),
                              test_names(), test_cmdline(), 7100, status);
        expect(status == tool_status::unsupported);
        expect(!hint.has_value());

        // empty -> nullopt
        status = tool_status::ok;
        hint = self_kill_hint("", test_pids(), test_names(), test_cmdline(),
                              7100, status);
        expect(status == tool_status::ok);
        expect(!hint.has_value());
    };

    "differential_edge_cases"_test = [] {
        // Python finditer consumes only one optional ".exe" suffix
        expect(run("kill.exe.exe 4100").desc ==
               std::optional<kimix::string>(
                   "targets PID 4100 via `kill.exe`, which is the agent "
                   "process or one of its parent processes"));
        expect(run("tskill.exe.exe 4100").desc.has_value());
        // repeated command words: first match wins
        expect(run("kill kill 4100").desc.has_value());
        expect(run("taskkill taskkill /pid 4100").desc.has_value());
        expect(run("pkill pkill python").desc.has_value());
        // word-boundary negatives (ASCII \b = [A-Za-z0-9_])
        expect(!run("kill4100").desc.has_value());
        expect(!run("xkill 4100").desc.has_value());
        expect(!run("porkill 4100").desc.has_value());
        expect(!run("skill 4100").desc.has_value());
        expect(!run("taskkillx /PID 4100").desc.has_value());
        expect(!run("stop-processx -id 4100").desc.has_value());
        expect(!run("get-processx -id 4100 | stop-process").desc.has_value());
        expect(!run("pkillx python").desc.has_value());
        expect(!run("killallx python").desc.has_value());
        expect(!run("wmicx process where processid=4100 delete").desc.has_value());
        // comma list with a safe trailing pid still hits the protected one
        expect(run("kill 4100,5000,9999").desc.has_value());
        // -f and --full are equivalent for the description
        expect(run("pkill -f --full kimi").desc ==
               std::optional<kimix::string>(
                   "kills processes matching `kimi` via `pkill -f`, which "
                   "also matches the agent process"));
        // foreach without spaces around parens still binds the loop variable
        expect(run("foreach($p in 4100){ stop-process -id $p }").desc.has_value());
        // empty image-name set -> name-based kill is safe
        {
            tool_status status = tool_status::ok;
            kimix::unordered_set<kimix::string, kimix::string_hash> empty_names;
            auto desc = detect_self_kill("taskkill /IM python.exe", test_pids(),
                                         empty_names, test_cmdline(), status);
            expect(status == tool_status::ok);
            expect(!desc.has_value());
        }
        // empty cmdline: pkill -f still matches image names
        {
            tool_status status = tool_status::ok;
            auto desc = detect_self_kill("pkill -f kimi", test_pids(),
                                         test_names(), "", status);
            expect(status == tool_status::ok);
            expect(desc.has_value());
        }
    };

    "struct_api_rule_ids"_test = [] {
        auto r = detect_self_kill_ex("kill 4100", test_pids(), test_names(),
                                     test_cmdline());
        expect(r.status == tool_status::ok);
        expect(r.hit);
        expect(r.rule_id == "kill");

        r = detect_self_kill_ex("taskkill /IM python.exe", test_pids(),
                                test_names(), test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "taskkill");

        r = detect_self_kill_ex("Stop-Process -Id 4100", test_pids(),
                                test_names(), test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "stop-process");

        r = detect_self_kill_ex("Get-Process -Id 4100 | Stop-Process",
                                test_pids(), test_names(), test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "get-process");

        r = detect_self_kill_ex("pkill python", test_pids(), test_names(),
                                test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "pkill");

        r = detect_self_kill_ex("killall python", test_pids(), test_names(),
                                test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "killall");

        r = detect_self_kill_ex("wmic process where processid=4100 delete",
                                test_pids(), test_names(), test_cmdline());
        expect(r.hit);
        expect(r.rule_id == "wmic");

        r = detect_self_kill_ex("echo hello", test_pids(), test_names(),
                                test_cmdline());
        expect(r.status == tool_status::ok);
        expect(!r.hit);
        expect(r.rule_id == "");

        r = detect_self_kill_ex("pkill ^x", test_pids(), test_names(),
                                test_cmdline());
        expect(r.status == tool_status::unsupported);
        expect(!r.hit);
    };

    return 0;
}
