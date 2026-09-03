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

    "pwsh_transform_operators"_test = [] {
        auto run = [](kimix::string_view code) {
            return pwsh_transform(code);
        };

        auto t1 = run(R"~~~($x ?? $y)~~~");
        expect(t1.status == tool_status::ok);
        expect(t1.command == R"~~~(if ($null -ne $x) { $x } else { $y })~~~");
        expect(eq(t1.warnings.size(), size_t(1)));
        expect(t1.warnings[0] ==
               R"~~~(Line 1: ?? operator `$x ?? $y` rewritten to `if ($null -ne $x) { $x } else { $y }`)~~~");

        auto t2 = run(R"~~~($a ?? $b ?? $c)~~~");
        expect(t2.status == tool_status::ok);
        expect(t2.command ==
               R"~~~(if ($null -ne $a) { $a } else {if ($null -ne $b) { $b } else { $c }})~~~");
        expect(eq(t2.warnings.size(), size_t(2)));
        expect(t2.warnings[0] ==
               R"~~~(Line 1: ?? operator `$a ?? $b ?? $c` rewritten to `if ($null -ne $a) { $a } else { $b ?? $c }`)~~~");
        expect(t2.warnings[1] ==
               R"~~~(Line 1: ?? operator `$b ?? $c` rewritten to `if ($null -ne $b) { $b } else { $c }`)~~~");

        auto t3 = run(R"~~~($x ??= 1)~~~");
        expect(t3.command == R"~~~(if ($null -eq $x) { $x = 1 })~~~");
        expect(eq(t3.warnings.size(), size_t(1)));
        expect(t3.warnings[0] ==
               R"~~~(Line 1: null-coalescing assignment `$x ??= 1` rewritten to `if ($null -eq $x) { $x = 1 }`)~~~");

        auto t4 = run(R"~~~($a = $b ??= $c)~~~");
        expect(t4.command == R"~~~($a = if ($null -eq $b) { $b = $c })~~~");
        expect(eq(t4.warnings.size(), size_t(1)));

        auto t5 = run(R"~~~($ok ? "yes" : "no")~~~");
        expect(t5.command == R"~~~(if ($ok) { "yes" } else { "no" })~~~");
        expect(eq(t5.warnings.size(), size_t(1)));
        expect(t5.warnings[0] ==
               R"~~~(Line 1: ternary operator `$ok ? "yes" : "no"` rewritten to `if ($ok) { "yes" } else { "no" }`)~~~");

        auto t6 = run(R"~~~(a && b)~~~");
        expect(t6.command == R"~~~(a; if ($?) { b })~~~");
        expect(eq(t6.warnings.size(), size_t(1)));
        expect(t6.warnings[0] ==
               R"~~~(Line 1: pipeline chain `a && b` rewritten to `a; if ($?) { b }`)~~~");

        auto t7 = run(R"~~~(a || b)~~~");
        expect(t7.command == R"~~~(a; if (-not $?) { b })~~~");
        expect(eq(t7.warnings.size(), size_t(1)));
        expect(t7.warnings[0] ==
               R"~~~(Line 1: pipeline chain `a || b` rewritten to `a; if (-not $?) { b }`)~~~");

        auto t8 = run(R"~~~(a && b || c)~~~");
        expect(t8.command ==
               R"~~~(a; if ($?) { b; if (-not $?) { c } })~~~");
        expect(eq(t8.warnings.size(), size_t(2)));

        auto t9 = run(R"~~~($obj?.Prop)~~~");
        expect(t9.command ==
               R"~~~($(if ($null -ne $obj) { $obj.Prop }))~~~");
        expect(eq(t9.warnings.size(), size_t(1)));
        expect(t9.warnings[0] ==
               R"~~~(Line 1: null-conditional member access `$obj?.Prop` rewritten to `$(if ($null -ne $obj) { $obj.Prop })`)~~~");

        auto t10 = run(R"~~~($arr?[0])~~~");
        expect(t10.command ==
               R"~~~($(if ($null -ne $arr) { $arr[0] }))~~~");
        expect(eq(t10.warnings.size(), size_t(1)));
        expect(t10.warnings[0] ==
               R"~~~(Line 1: null-conditional index `$arr?[0]` rewritten to `$(if ($null -ne $arr) { $arr[0] })`)~~~");

        auto t11 = run(R"~~~($obj?.A?.B)~~~");
        expect(t11.command ==
               R"~~~($(if ($null -ne $obj) { if ($null -ne $obj.A) { $obj.A.B } }))~~~");
        expect(eq(t11.warnings.size(), size_t(1)));
        expect(t11.warnings[0] ==
               R"~~~(Line 1: null-conditional member access `$obj?.A?.B` rewritten to `$(if ($null -ne $obj) { if ($null -ne $obj.A) { $obj.A.B } })`)~~~");

        auto t12 = run(R"~~~($x = 1 `
?? $y)~~~");
        expect(t12.command ==
               R"~~~($x = if ($null -ne 1) { 1 } else { $y })~~~");
        expect(eq(t12.warnings.size(), size_t(1)));
        expect(t12.warnings[0] ==
               R"~~~(Line 1: ?? operator `1 ?? $y` rewritten to `if ($null -ne 1) { 1 } else { $y }`)~~~");
    };

    "pwsh_transform_region_skipping"_test = [] {
        auto run = [](kimix::string_view code) { return pwsh_transform(code); };

        auto t1 = run(R"~~~("keep ?? inside")~~~");
        expect(t1.command == R"~~~("keep ?? inside")~~~");
        expect(t1.warnings.empty()) << "operators inside double-quoted string are skipped";

        auto t2 = run(R"~~~(# comment ??
$a ?? $b)~~~");
        expect(t2.command ==
               R"~~~(# comment ??
if ($null -ne $a) { $a } else { $b })~~~");
        expect(eq(t2.warnings.size(), size_t(1)));
        expect(t2.warnings[0] ==
               R"~~~(Line 2: ?? operator `$a ?? $b` rewritten to `if ($null -ne $a) { $a } else { $b }`)~~~");

        auto t3 = run(R"~~~(@'
here ??
'@
$a ?? $b)~~~");
        expect(t3.command ==
               R"~~~(@'
here ??
'@
if ($null -ne $a) { $a } else { $b })~~~");
        expect(eq(t3.warnings.size(), size_t(1)));
        expect(t3.warnings[0] ==
               R"~~~(Line 4: ?? operator `$a ?? $b` rewritten to `if ($null -ne $a) { $a } else { $b }`)~~~");

        auto t4 = run(R"~~~(<# block ?? #>
$a ?? $b)~~~");
        expect(t4.command ==
               R"~~~(<# block ?? #>
if ($null -ne $a) { $a } else { $b })~~~");
        expect(eq(t4.warnings.size(), size_t(1)));
        expect(t4.warnings[0] ==
               R"~~~(Line 2: ?? operator `$a ?? $b` rewritten to `if ($null -ne $a) { $a } else { $b }`)~~~");
    };

    "pwsh_transform_ascii_gate"_test = [] {
        auto t = pwsh_transform("$x ?? caf\xC3\xA9");
        expect(t.status == tool_status::unsupported);
        expect(t.command.empty());
        expect(t.warnings.empty());
    };

    "pwsh_fix_repair"_test = [] {
        auto run = [](kimix::string_view cmd) { return fix_pwsh_command(cmd); };

        auto f1 = run(R"~~~(echo hi)~~~");
        expect(f1.valid);
        expect(!f1.changed);
        expect(f1.command == R"~~~(echo hi)~~~");
        expect(f1.warning.empty());

        auto f2 = run(R"~~~(echo "hi)~~~");
        expect(f2.valid);
        expect(f2.changed);
        expect(f2.command == R"~~~(echo "hi")~~~");
        expect(!f2.warning.empty());

        auto f3 = run(R"~~~(echo 'hi)~~~");
        expect(f3.valid);
        expect(f3.changed);
        expect(f3.command == R"~~~(echo 'hi')~~~");

        auto f4 = run(R"~~~(@"
unclosed)~~~");
        expect(f4.valid);
        expect(f4.changed);
        expect(f4.command == R"~~~(@"
unclosed
"@)~~~");

        auto f5 = run(R"~~~(@'
unclosed)~~~");
        expect(f5.valid);
        expect(f5.changed);
        expect(f5.command == R"~~~(@'
unclosed
'@)~~~");

        auto f6 = run(R"~~~(x <# c)~~~");
        expect(f6.valid);
        expect(f6.changed);
        expect(f6.command == R"~~~(x <# c#>)~~~");

        auto f7 = run(R"~~~(x # comment)~~~");
        expect(f7.valid);
        expect(f7.changed);
        expect(f7.command == R"~~~(x # comment
)~~~");

        auto f8 = run(R"~~~(x --%)~~~");
        expect(f8.valid);
        expect(f8.changed);
        expect(f8.command == R"~~~(x --%
)~~~");

        auto f9 = run(R"~~~(# comment)~~~");
        expect(f9.valid);
        expect(f9.changed);
        expect(f9.command == R"~~~(# comment
$null)~~~");
    };

    "pwsh_fix_nul_redirect"_test = [] {
        auto run = [](kimix::string_view cmd) { return fix_pwsh_command(cmd); };

        auto n1 = run(R"~~~(Write-Output hi > nul)~~~");
        expect(n1.valid);
        expect(n1.changed);
        expect(n1.command == R"~~~(Write-Output hi > $null)~~~");
        expect(n1.warning.find("nul") != kimix::string::npos);
        expect(n1.warning.find("$null") != kimix::string::npos);

        auto n2 = run(R"~~~(Write-Output hi >NUL)~~~");
        expect(n2.valid);
        expect(n2.changed);
        expect(n2.command == R"~~~(Write-Output hi >$null)~~~");

        auto n3 = run(R"~~~(Write-Output hi >> nul)~~~");
        expect(n3.valid);
        expect(n3.changed);
        expect(n3.command == R"~~~(Write-Output hi > $null)~~~");

        auto n4 = run(R"~~~(Write-Output hi >nul)~~~");
        expect(n4.valid);
        expect(n4.changed);
        expect(n4.command == R"~~~(Write-Output hi >$null)~~~");

        auto n5 = run(R"~~~(Write-Output hi 2> nul)~~~");
        expect(n5.valid);
        expect(n5.changed);
        expect(n5.command == R"~~~(Write-Output hi 2> $null)~~~");

        auto n6 = run(R"~~~(Write-Output hi 2>> nul)~~~");
        expect(n6.valid);
        expect(n6.changed);
        expect(n6.command == R"~~~(Write-Output hi 2> $null)~~~");

        auto n7 = run(R"~~~(Write-Output hi *> nul)~~~");
        expect(n7.valid);
        expect(n7.changed);
        expect(n7.command == R"~~~(Write-Output hi *> $null)~~~");

        auto n8 = run(R"~~~(Write-Output 'nul' > nul)~~~");
        expect(n8.valid);
        expect(n8.changed);
        expect(n8.command == R"~~~(Write-Output 'nul' > $null)~~~");

        // preserved / tricky
        auto p1 = run(R"~~~(Write-Output hi > 'nul')~~~");
        expect(p1.valid);
        expect(!p1.changed);
        expect(p1.command == R"~~~(Write-Output hi > 'nul')~~~");
        expect(p1.warning.empty());

        auto p2 = run(R"~~~(cmd /c echo --% > nul)~~~");
        expect(p2.valid);
        expect(p2.changed);
        expect(p2.command.find("> nul") != kimix::string::npos);
        expect(p2.command.find("$null") == kimix::string::npos);

        auto p3 = run(R"~~~(Write-Output "hi > nul")~~~");
        expect(p3.valid);
        expect(!p3.changed);
        expect(p3.command == R"~~~(Write-Output "hi > nul")~~~");

        auto p4 = run(R"~~~(Write-Output 'text > nul')~~~");
        expect(p4.valid);
        expect(!p4.changed);

        auto p5 = run(R"~~~(# comment > nul
Write-Output hi)~~~");
        expect(p5.valid);
        expect(!p5.changed);

        auto p6 = run(R"~~~(Write-Output nul)~~~");
        expect(p6.valid);
        expect(!p6.changed);

        auto p7 = run(R"~~~(Write-Output hi > nul.txt)~~~");
        expect(p7.valid);
        expect(!p7.changed);
        expect(p7.command == R"~~~(Write-Output hi > nul.txt)~~~");
    };

    "pwsh_fix_unrepairable"_test = [] {
        auto f = fix_pwsh_command(R"~~~(x `)~~~");
        expect(!f.valid);
        expect(!f.changed);
    };

    "pwsh_fix_ascii_gate"_test = [] {
        auto f = fix_pwsh_command("$x ?? caf\xC3\xA9");
        expect(!f.valid);
        expect(!f.changed);
        expect(f.command.empty());
        expect(f.warning.empty());
    };

    "pwsh_hardline"_test = [] {
        auto run = [](kimix::string_view cmd) { return check_hardline_blocked(cmd); };

        auto h1 = run("rm -rf /");
        expect(h1.blocked);
        expect(h1.description ==
               "Recursive delete of protected root/home (`/`)");

        auto h2 = run("rmdir /s /q C:\\");
        expect(h2.blocked);
        expect(h2.description ==
               "Recursive delete of protected root/home (`c:\\`)");

        auto h3 = run("mkfs.ext4 /dev/sda1");
        expect(h3.blocked);
        expect(h3.description ==
               "Disk formatting command (`mkfs`) is blocked");

        auto h4 = run("dd if=/dev/zero of=/dev/sda");
        expect(h4.blocked);
        expect(h4.description ==
               "`dd` writing to a raw device is blocked");

        auto h5 = run("shutdown /r");
        expect(h5.blocked);
        expect(h5.description ==
               "System `shutdown` command is blocked");

        auto h6 = run(":(){ :|:& };:");
        expect(h6.blocked);
        expect(h6.description == "Fork bomb pattern detected");

        auto h7 = run("kill 1");
        expect(h7.blocked);
        expect(h7.description ==
               "`kill` targeting PID 1 (or `$PPID`) is blocked");

        auto h8 = run("format C:");
        expect(h8.blocked);
        expect(h8.description ==
               "Windows `format` on a drive is blocked");

        auto h9 = run("echo hello");
        expect(!h9.blocked);
        expect(h9.description.empty());
    };

    "pwsh_hardline_ascii_gate"_test = [] {
        auto h = check_hardline_blocked("rm -rf caf\xC3\xA9");
        expect(!h.blocked);
        expect(h.description.empty());
    };

    "pwsh_rtk_rewrite"_test = [] {
        auto rr = maybe_rewrite_with_rtk("git status", true, true,
                                        "C:\\rtk.exe");
        expect(rr.changed);
        expect(rr.segment == "& rtk git status");

        rr = maybe_rewrite_with_rtk("git status", true, true, "");
        expect(rr.changed);
        expect(rr.segment == "& rtk git status");

        rr = maybe_rewrite_with_rtk("cat file", true, true, "C:\\rtk.exe");
        expect(!rr.changed);
        expect(rr.segment == "cat file");

        rr = maybe_rewrite_with_rtk("git status", true, false, "");
        expect(!rr.changed);
        expect(rr.segment == "git status");

        rr = maybe_rewrite_with_rtk("git status; git log", true, true,
                                    "C:\\rtk.exe");
        expect(!rr.changed);
        expect(rr.segment == "git status; git log");
    };

    return 0;
}
