#!/usr/bin/env python3
"""Native dual-configuration hard gate (kimix-native: standalone vs external).

WHAT IT PROVES, and why it is one command
=========================================
kimix-native can build the same core library two ways:

  sa  standalone  kimix-core over the vendored third-party libraries of src/ext
                  (mimalloc, kimix-xxhash, kimix-yyjson, kimix-marl, kimix-pdqsort)
                  -> xmake option kimix_external_thirdparty=false
  ex  external    kimix-core over lc-core of the external third-party tree named by
                  ../link.lua (LuisaCompute: EASTL + its bundled mimalloc, spdlog,
                  BTree, lc-yyjson, marl)
                  -> xmake option kimix_external_thirdparty=true

"Both legs build" is not a result. This gate builds each leg in its OWN build
directory and its OWN binary directory, then proves, for the same tree:

  1. each leg configures to exactly the values asked for, read back out of the
     evaluated configuration AND out of the tree's recorded xmake.conf, so a leg
     that silently ran in the other leg's tree is a failure, not a mystery;
  2. the evaluated dependency graph is the intended one: in the external leg
     kimix-core has EXACTLY one direct third-party dependency (lc-core), the
     KIMIX_EXTERNAL_THIRDPARTY define is PUBLIC (it reaches a dependent test target
     and is not declared on it), no vendored mimalloc/xxhash/yyjson/marl/pdqsort
     target exists or is compiled a second time, and no LuisaCompute backend, test,
     example or tutorial is included;
  3. EVERY registered test entry runs in both legs -- slow suites included -- and
     each suite reports a real Boost.UT summary with asserts > 0. A suite that
     prints "0 asserts in N tests", prints nothing at all, is registered but
     never runs is a false green, not a pass. The registered ENTRIES (not the
     number of targets) are compared with the entries that actually ran, and the
     per-entry (test name, status) lists are compared across the legs;
  4. the two legs' per-suite (asserts, tests) manifests agree, so the only thing
     that changed underneath is which third-party implementation is used. A count
     that legitimately cannot be compared is ATTRIBUTED with evidence, never
     exempted by name:
       - a suite whose own output carries `[bench] ... (N iters)` runs expect()
         inside a time-budgeted loop (unit/native/bench_util.h), so its assert
         count is a speed measurement. That is accepted only when the delta is
         ARITHMETICALLY the iteration delta -- asserts = fixed + weight*iters must
         hold for the measured pairs of BOTH legs AND on a re-run in both legs,
         and the fixed part and the tests count must agree. A bench line alone
         explains nothing;
       - a suite re-measured twice per leg that does not reproduce its OWN count is
         load-dependent and is reported FLAKY with all four numbers.
     Anything else that differs is a hard failure.

The gate fails closed: a missing option, a missing bridge or external tree, a stale
build tree, contamination, a dropped test, a zero-assert green or a diverging
manifest all stop the run with a named reason and a non-zero exit code.

USAGE  (run from the kimix-native root; Python 3.10+, stdlib only)
=================================================================
    python scripts/native_dual_config_gate.py
        The real gate: both legs, clean isolated trees, every target built, the
        complete registered sweep (slow suites included), the two manifests
        compared. ~16 minutes on a 32-core release build.

Iteration helpers -- never the default, because a gate that can be scoped silently
becomes a scoped gate. Every one of them makes the run SCOPED: the report says so,
the summary line says so, and a passing scoped run exits 7 instead of 0:

    --configure-only   configure both legs and stop (option/bridge/tree wired? plus
                       the whole evaluated-graph check). ~20 s.
    --skip-build       do not build (implies --reuse): graph + test phases only. The
                       leg's artifacts must already exist (missing ones are a
                       failure); stale ones are reported.
    --reuse            keep .gates/build/<leg> and .gates/bin/<leg> (incremental).
                       Refuses to start if they do not exist: a "reuse" from
                       nothing is not a reuse. An up-to-date tree that compiles
                       nothing IS a valid reuse; the outputs are then checked for
                       existence and freshness instead of demanding a compile step.
    --only-targets A,B run only those test targets. The report says SCOPED, and the
                       manifest comparison is limited to the same set.
    --legs sa          one leg only (also SCOPED).
    --test-jobs N      parallel test entries. Only 1 is accepted: the report blocks
                       are attributed to suites BY POSITION in the merged output, so
                       a parallel sweep would need a concurrency-safe parser first.
    --no-extra-targets skip the built-but-unregistered `xmake run` set (lua-tests).
    --no-tests         configure/build/graph only, do not run the sweep (SCOPED).
    --jobs N           build jobs (default: CPU count).

Exit codes: 0 the full gate passed - 2 preflight/environment - 3 configure/build -
4 evaluated graph/static - 5 test sweep/parity - 6 internal error -
7 a SCOPED run passed (--configure-only / --no-tests / --no-extra-targets /
one leg / --only-targets): never a full gate result, never 0.

The `.xmake/<plat>/<arch>/xmake.conf` file of every configuration is snapshotted
before the run and restored in a finally block (success, failure or interrupt), so
a gate never leaves the developer's tree configured for .gates/... Nothing else of
the .xmake state is touched.

Artifacts: .gates/<timestamp>/{report.md,report.json,logs/,graphs/,test-output/}.
.gates/ is gitignored: a gate run must never dirty the tree it proves.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THIRDPARTY = os.path.dirname(ROOT)
LINK_LUA = os.path.join(THIRDPARTY, "link.lua")
BRIDGE_LUA = os.path.join(ROOT, "scripts", "xmake_luisa.lua")
INSPECT_LUA = os.path.join(ROOT, "scripts", "xmake_inspect_graph.lua")
GATES = os.path.join(ROOT, ".gates")

# leg name -> (kimix_external_thirdparty value, human label)
LEGS = {
    "sa": (False, "standalone (vendored src/ext)"),
    "ex": (True, "external (lc-core of the ../link.lua tree)"),
}


def leg_builddir(leg):
    return os.path.join(GATES, "build", leg)


def leg_bindir(leg):
    return os.path.join(GATES, "bin", leg)


# kimix-core's direct dependencies, exactly. The external leg is the whole point:
# ONE third-party dependency, everything else reached through it.
EXPECTED_KIMIX_CORE_DEPS = {
    "sa": ["kimix-marl", "kimix-pdqsort", "kimix-xxhash", "kimix-yyjson", "mimalloc"],
    "ex": ["lc-core"],
}

# Targets that may exist in one leg and not the other. Anything outside this delta
# is a graph bug -- a test target present in only one leg is exactly the silent skip
# this gate exists for.
TARGET_DELTA_ADDED_EX = {"lc-core", "lc-yyjson", "lc-check-winsdk", "eastl", "spdlog", "btree"}
TARGET_DELTA_REMOVED_EX = {"kimix-xxhash", "kimix-yyjson", "kimix-pdqsort", "kimix-marl"}

# Every compiled file that lives in the external tree must be under one of these --
# the minimal subgraph lc-core needs. src/backends, src/tests, examples and every
# other vendored LC library (glslang, SPIRV-Tools, reproc, liblmdb, TVM, ...) are out
# of scope BY CONTRACT.
LC_ALLOWED_FILE_PREFIXES = (
    "src/core/",
    "src/ext/EASTL/",
    "src/ext/spdlog/",
    "src/ext/BTree/",
    "src/ext/yyjson/",
    "src/ext/marl/",
    "include/luisa/core/",
)

# Facilities that must never exist twice in one process: mimalloc (the one mi_* heap),
# xxHash, yyjson, marl and pdqsort. name -> (the standalone leg's target, the path
# fragments that identify a copy of it in THIS tree).
DUPLICATE_SENSITIVE_LIBS = {
    "mimalloc": ("mimalloc", ("src/ext/mimalloc/",)),
    "xxhash": ("kimix-xxhash", ("src/ext/xxhash/", "src/ext/xxHash/")),
    "yyjson": ("kimix-yyjson", ("src/ext/yyjson/",)),
    "marl": ("kimix-marl", ("src/ext/marl/",)),
    "pdqsort": ("kimix-pdqsort", ("src/ext/pdqsort/",)),
}

# Env-gated benchmarks: with their variable unset they legitimately have no body to
# run, so "asserts > 0" does not apply. They must still exist, pass, and agree.
# Each entry names the variable that gates it.
ZERO_ASSERT_EXEMPT = {
    "test_stl_pdqsort_bench",          # KIMIX_PDQSORT_BENCH
    "test_graph_db_bench",             # KIMIX_GRAPH_DB_BENCH
    "test_graph_db_bench_edges",       # KIMIX_GRAPH_DB_BENCH
    "test_builtin_grep_engine_bench",  # KIMIX_GREP_BENCH
    "test_script_bench",               # KIMIX_SCRIPT_BENCH
}

# Entries that legitimately print no Boost.UT summary at all, each with its reason:
#   kimix-test                the hand-written printf smoke binary (src/test/main.cpp)
#   test_pybind11             a COMPILE gate: main() returns 0, the #error directives
#                             are the assertion (see the file header)
#   lua-language-server-tests a phony target whose on_test runs the server's own Lua
#                             suite; its verdict is the process exit code
NO_SUMMARY_EXEMPT = {"kimix-test", "test_pybind11", "lua-language-server-tests"}

# The one leg-specific suite has an exact, reviewed manifest in each leg. Any
# other delta -- including an unexpected delta in this suite -- is a hard failure.
EXPECTED_MANIFEST_DELTAS = {
    "test_core_leg_contract": ((66, 5), (75, 6)),
}

# Built and run, but not part of `xmake test` (their rundir makes `xmake run` the
# only correct way to start them). Each entry is
#   (target, run arguments, why, expectation)
# with expectation one of:
#   "pass"             the target must exit 0;
#   "same-in-both-legs" a KNOWN pre-existing limitation -- the target's outcome is
#                      recorded and must be IDENTICAL in the two legs (same exit code,
#                      same first error line), so a repair or a leg-divergence is
#                      reported instead of hidden, and the limitation is never
#                      silently skipped.
EXTRA_RUN_TARGETS = [
    # `all.lua` is required: the binary IS the Lua interpreter, so a bare
    # `xmake run lua-tests` reads stdin (an interactive REPL) and hangs forever.
    # The target also drops a platform-appropriate `lua` alias next to itself
    # (src/ext/xmake.lua after_build) -- a copy on Windows, a symlink on POSIX --
    # because Lua's own testes/README runs the suite as `../lua all.lua`, and a
    # stand-alone Lua directory is what the corpus expects to find.
    #
    # WHAT STILL FAILS, and it is NOT the interpreter name (measured, both legs,
    # with the alias in place -- `lua -v` runs, progname resolves to the leg's own
    # binary): Lua's testes/main.lua is written for a POSIX shell. The first
    # construct that cannot work under cmd.exe is
    #     RUN('echo "print(10)\nprint(2)\n" | lua > %s', out)
    # (main.lua:100): cmd's echo does not interpret `\n`, so the interpreter is fed
    # the literal text `"print(10)\nprint(2)\n"` and answers
    # `stdin:1: unexpected symbol near '"print(10)`; the corpus then aborts reading
    # the redirect file it expected. Later constructs are POSIX-only too (`env
    # VAR=... lua`, single-quoted `-l 'str=string'`, `2> file`). Running the corpus
    # in its portable mode (`-e "_port=true"`) skips that block and gets ~10 files
    # further, then stops at files.lua:467 (`io.output('/dev/null')`) -- another
    # POSIX path assumption. So NO mode of the vendored corpus completes on
    # Windows; the outcome is recorded and must be IDENTICAL in the two third-party
    # legs, so a repair or a leg-divergence is reported instead of hidden.
    ("lua-tests", ["all.lua"],
     "the vendored Lua 5.5 test corpus (set_rundir, so `xmake run`, script arg required)",
     "same-in-both-legs"),
]

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def strip_ansi(text):
    """Drop colour escapes: xmake colours its step lines and the report blocks, which
    would otherwise defeat every line-anchored pattern below."""
    return ANSI_RE.sub("", text)


def _norm(path):
    # xmake may emit either absolute paths or paths relative to the project root.
    # Anchor the latter at ROOT, not at the caller's cwd (the gate is also supported
    # through an absolute script path from the workspace root).
    if path and not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    return os.path.normpath(os.path.abspath(path)).replace("\\", "/")


def as_list(value):
    """The Lua side emits a JSON array for a list, but an EMPTY list serialises as
    `{}`. Normalise both shapes to a Python list."""
    if isinstance(value, list):
        return value
    return []


class GateFailure(Exception):
    """A named, attributable gate failure (carries the process exit code)."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class _Context:
    """Per-run state: report directory, recorded commands, checks."""

    external_name = "LuisaCompute"

    def __init__(self):
        self.stamp = time.strftime("%Y%m%d-%H%M%S")
        self.outdir = os.path.join(GATES, self.stamp)
        self.last_commands = []
        self.checks = []

    def record(self, name, ok, detail=""):
        status = "PASS" if ok else "FAIL"
        self.checks.append({"check": name, "status": status, "detail": str(detail)})
        print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
        return ok

    def note(self, name, detail):
        self.checks.append({"check": name, "status": "INFO", "detail": str(detail)})
        print(f"[INFO] {name} -- {detail}")

    def log(self, name, text):
        path = os.path.join(self.outdir, "logs", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path


CTX = _Context()


def run(cmd, cwd=ROOT, timeout=None):
    """Run a command, capture merged output, return (returncode, text)."""
    started = time.time()
    proc = subprocess.run(
        cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", timeout=timeout)
    text = proc.stdout or ""
    CTX.last_commands.append({
        "cmd": cmd, "cwd": cwd, "rc": proc.returncode,
        "seconds": round(time.time() - started, 1),
    })
    return proc.returncode, text


def xmake_bin():
    found = shutil.which("xmake")
    if not found:
        raise GateFailure(2, "preflight: xmake is not on PATH (see .agents/skills/environment)")
    return found


def platform_args():
    """Explicit plat/arch for the host: the gate never relies on xmake's default."""
    if os.name == "nt":
        return ["-p", "windows", "-a", "x64"]
    if sys.platform == "darwin":
        return ["-p", "macosx", "-a", "arm64"]
    return ["-p", "linux", "-a", "x86_64"]


# ---------------------------------------------------------------------------
# The developer's recorded configuration: snapshot before, restore in `finally`
# ---------------------------------------------------------------------------
def snapshot_xmake_configs():
    """Read every .xmake/<plat>/<arch>/xmake.conf BEFORE the gate configures anything.

    `xmake f` rewrites that one file per plat/arch, and a gate leg must configure
    the SHARED tree to build at all -- so by the end of a run the tree is recorded
    as configured for `.gates/build/<leg>`. Restoring the bytes (in a finally, so
    failures and Ctrl-C are covered too) is what keeps a gate run from leaving the
    developer configured for a gate tree.

    Only these files are snapshotted: caches, tmp/, project.lock and the .gates/
    trees themselves are left exactly as they are.
    """
    snapshot = {}
    root = os.path.join(ROOT, ".xmake")
    if not os.path.isdir(root):
        return snapshot
    for plat in sorted(os.listdir(root)):
        platdir = os.path.join(root, plat)
        if not os.path.isdir(platdir):
            continue
        for arch in sorted(os.listdir(platdir)):
            conf = os.path.join(platdir, arch, "xmake.conf")
            if os.path.isfile(conf):
                with open(conf, "rb") as handle:
                    snapshot[conf] = handle.read()
    return snapshot


def restore_xmake_configs(snapshot):
    """Put the snapshotted configuration files back (and drop ones the run created)."""
    root = os.path.join(ROOT, ".xmake")
    current = []
    if os.path.isdir(root):
        for plat in sorted(os.listdir(root)):
            platdir = os.path.join(root, plat)
            if not os.path.isdir(platdir):
                continue
            for arch in sorted(os.listdir(platdir)):
                conf = os.path.join(platdir, arch, "xmake.conf")
                if os.path.isfile(conf):
                    current.append(conf)
    restored = 0
    for conf, data in snapshot.items():
        try:
            with open(conf, "rb") as handle:
                if handle.read() == data:
                    continue
        except OSError:
            pass
        try:
            with open(conf, "wb") as handle:
                handle.write(data)
            restored += 1
        except OSError as error:
            print(f"[WARN] could not restore {conf}: {error}")
    removed = 0
    for conf in current:
        if conf in snapshot:
            continue
        # Created by this run (a plat/arch the developer had no configuration for):
        # restoring the "no configuration" state means removing that one file.
        try:
            os.remove(conf)
            removed += 1
        except OSError as error:
            print(f"[WARN] could not remove {conf}: {error}")
    if restored or removed:
        print(f"[INFO] xmake configuration restored ({restored} file(s) rewritten, "
              f"{removed} created-by-this-run file(s) removed)")
    else:
        print("[INFO] xmake configuration unchanged (nothing to restore)")


# ---------------------------------------------------------------------------
# Phase 0 -- preflight (fail closed before anything is built)
# ---------------------------------------------------------------------------
def preflight(args):
    print("=" * 78)
    print("phase 0: preflight (fail closed)")
    print("=" * 78)
    os.makedirs(CTX.outdir, exist_ok=True)

    for required, what in ((os.path.join(ROOT, "xmake.lua"), "the kimix-native xmake.lua"),
                           (INSPECT_LUA, "scripts/xmake_inspect_graph.lua"),
                           (BRIDGE_LUA, "scripts/xmake_luisa.lua (the external bridge)")):
        if not os.path.isfile(required):
            raise GateFailure(2, f"preflight: {what} is missing ({required})")

    xmake = xmake_bin()
    rc, version_text = run([xmake, "--version"])
    if rc != 0:
        raise GateFailure(2, f"preflight: `xmake --version` failed: {version_text.strip()[:200]}")
    match = re.search(r"v(\d+)\.(\d+)\.(\d+)", version_text)
    if not match:
        raise GateFailure(2, f"preflight: cannot parse the xmake version: {version_text!r}")
    if tuple(int(part) for part in match.groups()) < (3, 0, 6):
        raise GateFailure(2, f"preflight: xmake {match.group(0)} is older than the project floor 3.0.6")
    CTX.note("xmake", version_text.splitlines()[0].strip())

    # The option must be DECLARED, or the "external" leg would be a byte-for-byte
    # clone of the standalone one -- a false green by construction. `xmake f --help`
    # lists the options this project actually declares.
    rc, help_text = run([xmake, "f", "--help"])
    if rc != 0:
        raise GateFailure(2, f"preflight: `xmake f --help` failed with rc={rc}")
    if "kimix_external_thirdparty" not in help_text:
        raise GateFailure(
            2,
            "preflight: the option kimix_external_thirdparty is NOT declared, so the "
            "external leg would be an exact clone of the standalone leg. Refusing to "
            "report a green gate over one configuration.")
    CTX.record("option kimix_external_thirdparty is declared", True)

    # ../link.lua names the external tree; the bridge includes it as real Lua.
    if not os.path.isfile(LINK_LUA):
        raise GateFailure(
            2,
            f"preflight: {LINK_LUA} is missing; the external leg is named by it "
            "(kimix_external_config = { external = 'LuisaCompute' })")
    with open(LINK_LUA, "r", encoding="utf-8", errors="replace") as handle:
        link_text = handle.read()
    match = re.search(r"external\s*=\s*['\"]([^'\"]+)['\"]", link_text)
    if not match:
        raise GateFailure(2, f"preflight: {LINK_LUA} does not define kimix_external_config.external")
    external_name = match.group(1)
    external_root = os.path.join(THIRDPARTY, external_name)
    if not os.path.isdir(external_root):
        raise GateFailure(
            2, f"preflight: the external tree {external_root} named by link.lua is missing")
    CTX.external_name = external_name
    CTX.note("external tree", f"{external_name} at {external_root}")

    # Disk: two legs of release objects + binaries (~3-6 GB + ~2 GB each).
    free_gb = shutil.disk_usage(GATES if os.path.isdir(GATES) else ROOT).free / (1 << 30)
    needed_gb = 6 if args.reuse else 20
    if free_gb < needed_gb:
        raise GateFailure(
            2, f"preflight: only {free_gb:.1f} GB free on this volume, the gate needs ~{needed_gb} GB")
    CTX.note("free disk", f"{free_gb:.1f} GB")

    # Record what is being certified. A dirty tree is NOT rejected (the phases under
    # test are uncommitted by design), but the report has to name it.
    for label, path in (("kimix-native", ROOT), (external_name, external_root),
                        ("semevia", THIRDPARTY)):
        rc, head = run(["git", "-C", path, "rev-parse", "--short", "HEAD"], cwd=ROOT)
        rc2, dirty = run(["git", "-C", path, "status", "--porcelain"], cwd=ROOT)
        modified = len([line for line in dirty.splitlines() if line.strip()])
        CTX.note(f"git {label}", f"{head.strip() if rc == 0 else '?'} ({modified} modified path(s))")

    rc, pyver = run([sys.executable, "--version"])
    CTX.note("python", pyver.strip())


# ---------------------------------------------------------------------------
# Phase 1 -- configure each leg explicitly, then read the config back
# ---------------------------------------------------------------------------
def configure_leg(leg, args):
    external, label = LEGS[leg]
    builddir = leg_builddir(leg)
    bindir = leg_bindir(leg)
    xmake = xmake_bin()

    if not args.reuse:
        for path in (builddir, bindir):
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            os.makedirs(path, exist_ok=True)
    elif not os.path.isdir(builddir):
        raise GateFailure(
            3,
            f"--reuse: {builddir} does not exist, so there is nothing to reuse. Run "
            "without --reuse once (or with --configure-only).")

    cmd = [xmake, "f", *platform_args(), "-m", "release", "-c", "-y",
           "-o", builddir,
           f"--kimix_bin_dir={bindir}",
           f"--kimix_external_thirdparty={'true' if external else 'false'}",
           "--kimix_enable_tests=true",
           "--kimix_slow_tests=true",
           "--kimix_enable_api=true"]
    cmd += args.configure_extra
    print(f"\n[configure {leg}] {' '.join(cmd)}")
    rc, text = run(cmd, timeout=1800)
    plain = strip_ansi(text)
    CTX.log(f"configure_{leg}.log", plain)
    if rc != 0:
        tail = "\n".join(plain.strip().splitlines()[-25:])
        raise GateFailure(3, f"configure {leg} failed (rc={rc}):\n{tail}")
    if leg == "ex" and "external third-party leg" not in plain:
        raise GateFailure(
            4,
            "configure ex: the external bridge never announced itself ('note: external "
            "third-party leg: ...' absent), so the leg did not actually switch trees")
    print(f"[PASS] configure {leg}: {label}")


def assert_tree_config(leg, when):
    """Read the configuration OUT of the tree and check it is this leg's.

    `.xmake/<plat>/<arch>/xmake.conf` is ONE file shared by every configuration of
    the tree, so a leg whose build or test sweep starts after another leg configured
    the tree would silently exercise the OTHER tree's binaries and still look green.
    Cheap (a file read) and called before the build and again before the sweep, so
    the failure names the contaminated phase.
    """
    external, _label = LEGS[leg]
    plat = platform_args()[1]
    arch = platform_args()[3]
    conf = os.path.join(ROOT, ".xmake", plat, arch, "xmake.conf")
    if not os.path.isfile(conf):
        raise GateFailure(3, f"{when} {leg}: {conf} does not exist (the leg is not configured)")
    with open(conf, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    match = re.search(r"kimix_external_thirdparty\s*=\s*(true|false)", text)
    if not match:
        raise GateFailure(3, f"{when} {leg}: {conf} records no kimix_external_thirdparty")
    recorded = match.group(1) == "true"
    if recorded != external:
        raise GateFailure(
            3,
            f"{when} {leg}: CONFIGURATION CONTAMINATION -- the tree is configured for "
            f"kimix_external_thirdparty={recorded} but this phase is the "
            f"{'external' if external else 'standalone'} leg")
    match = re.search(r"_kimix_bin_dir\s*=\s*\[\[(.*?)\]\]", text)
    if match and _norm(match.group(1)) != _norm(os.path.join(leg_bindir(leg), "release")):
        raise GateFailure(
            3,
            f"{when} {leg}: the tree's _kimix_bin_dir is {match.group(1)}, not this "
            f"leg's {os.path.join(leg_bindir(leg), 'release')}")
    return True


# ---------------------------------------------------------------------------
# Phase 2 -- build every target of the leg
# ---------------------------------------------------------------------------
# Target kinds whose targetfile is an artifact a "build ok" must have produced.
ARTIFACT_KINDS = ("binary", "shared", "static")


def check_leg_artifacts(leg, graph, fresh_noop):
    """The outputs a leg that did not compile anything still has to have.

    `xmake build` that reports no compile/link step is either (a) a fully up-to-date
    tree -- legitimate, and the whole point of --reuse -- or (b) a leg whose outputs
    are missing or older than the sources they claim to be built from, which would
    make every later phase measure the wrong binaries. Only (a) may pass.

    Staleness is fatal for both --reuse and --skip-build: a full gate may never
    certify binaries older than the sources they claim to represent.
    """
    targets = _targets(graph)
    missing, stale, checked = [], [], 0
    for name, entry in sorted(targets.items()):
        if not entry["enabled"] or entry["kind"] not in ARTIFACT_KINDS:
            continue
        targetfile = entry.get("targetfile") or ""
        if not targetfile:
            continue
        checked += 1
        if not os.path.isfile(targetfile):
            missing.append(f"{name} -> {targetfile}")
            continue
        binary_mtime = os.path.getmtime(targetfile)
        sources = []
        for path in as_list(entry["files"]):
            try:
                sources.append((os.path.getmtime(path), path))
            except OSError:
                continue
        if sources:
            newest, newest_path = max(sources)
            if newest > binary_mtime:
                stale.append(f"{name} ({_norm(newest_path)} is newer than {targetfile})")
    if checked == 0:
        raise GateFailure(
            3, f"{leg}: the evaluated graph lists no target with an artifact to check -- "
               "the graph dump is not usable for the reuse check")
    if missing:
        raise GateFailure(
            3,
            f"{leg}: no build step ran and {len(missing)} of {checked} artifact(s) are "
            f"MISSING, so there is nothing to reuse: {missing[:6]}"
            + (" ..." if len(missing) > 6 else ""))
    if stale and fresh_noop:
        raise GateFailure(
            3,
            f"{leg}: no build step ran, but {len(stale)} artifact(s) are STALE (a source "
            f"is newer than the binary): {stale[:6]}"
            + (" ..." if len(stale) > 6 else "")
            + " -- the tree did not really rebuild, so the sweep would measure old binaries")
    return checked, stale


def build_leg(leg, args, graph):
    assert_tree_config(leg, "build")
    xmake = xmake_bin()
    cmd = [xmake, "build", "-j", str(args.jobs)]
    print(f"\n[build {leg}] {' '.join(cmd)}")
    started = time.time()
    rc, text = run(cmd, timeout=None)
    plain = strip_ansi(text)
    CTX.log(f"build_{leg}.log", plain)
    seconds = round(time.time() - started, 1)
    if rc != 0:
        tail = "\n".join(plain.strip().splitlines()[-40:])
        raise GateFailure(3, f"build {leg} failed (rc={rc}):\n{tail}")
    # Anti-false-green: a FRESH build that reports no compile or link step built
    # nothing, and "build ok" alone cannot have produced the 200+ test binaries. On
    # --reuse the same output is legitimate (the tree is already up to date), so the
    # artifact check takes over: they must exist and be newer than their sources.
    compiles = len(re.findall(r"<[^>]+> compiling\.", plain))
    links = len(re.findall(r"<[^>]+> linking\.", plain))
    if compiles == 0 and links == 0:
        if not args.reuse:
            raise GateFailure(
                3, f"build {leg}: `xmake build` succeeded but reported no compile/link "
                   "step -- nothing was built, which is not a build")
        checked, _stale = check_leg_artifacts(leg, graph, fresh_noop=True)
        CTX.record(f"build {leg}", True,
                   f"{seconds}s, up-to-date tree (no rebuild needed); {checked} artifact(s) "
                   "present and newer than their sources")
        return
    CTX.record(f"build {leg}", True, f"{seconds}s, {compiles} compile(s), {links} link(s)")


# ---------------------------------------------------------------------------
# Phase 3 -- the evaluated graph (authoritative view of what was configured)
# ---------------------------------------------------------------------------
def load_graph(leg):
    xmake = xmake_bin()
    rc, text = run([xmake, "l", INSPECT_LUA], timeout=600)
    CTX.log(f"graph_{leg}.log", text)
    if rc != 0:
        raise GateFailure(4, f"graph {leg}: `xmake l xmake_inspect_graph.lua` failed (rc={rc})")
    try:
        payload = text.split("--8<--KIMIX-GRAPH-BEGIN", 1)[1].split("--8<--KIMIX-GRAPH-END", 1)[0]
        graph = json.loads(payload)
    except (IndexError, json.JSONDecodeError) as error:
        raise GateFailure(4, f"graph {leg}: cannot parse the inspected graph ({error})") from error
    os.makedirs(os.path.join(CTX.outdir, "graphs"), exist_ok=True)
    with open(os.path.join(CTX.outdir, "graphs", f"{leg}.json"), "w", encoding="utf-8") as handle:
        json.dump(graph, handle, indent=1, sort_keys=True)
    return graph


def _targets(graph):
    return {entry["name"]: entry for entry in graph["targets"]}


def _has_define(defines, name):
    return any(item == name or item.startswith(name + "=") for item in defines)


def assert_graph(leg, graph, args):
    external, _label = LEGS[leg]
    targets = _targets(graph)
    config = graph["config"]
    builddir = _norm(leg_builddir(leg))
    bindir = _norm(leg_bindir(leg))

    # --- 1. the configuration really is this leg's ---------------------------
    problems = []

    def expect(condition, message):
        if not condition:
            problems.append(message)

    expect(config.get("mode") == "release", f"mode is {config.get('mode')!r}, not 'release'")
    expect(_norm(config.get("kimix_bin_dir") or "") == bindir,
           f"kimix_bin_dir is {config.get('kimix_bin_dir')!r}, not {bindir}")
    expect(_norm(config.get("_kimix_bin_dir") or "") == _norm(os.path.join(bindir, "release")),
           f"_kimix_bin_dir is {config.get('_kimix_bin_dir')!r}, not <bin>/release")
    expect(_norm(config.get("builddir") or "") == builddir,
           f"builddir is {config.get('builddir')!r}, not {builddir}")
    expect(bool(config.get("kimix_external_thirdparty")) == external,
           f"kimix_external_thirdparty is {config.get('kimix_external_thirdparty')!r}, not "
           f"{external!r} (configuration contamination)")
    expect(config.get("kimix_enable_tests") is True,
           "kimix_enable_tests is off: the test suites are not built")
    expect(config.get("kimix_slow_tests") is True,
           "kimix_slow_tests is off: the slow suites would be skipped")
    expect(config.get("kimix_enable_api") is True,
           "kimix_enable_api is off: the C FFI tests are missing")
    expect(config.get("kimix_memory_strict") is False,
           "kimix_memory_strict is on: the legs would no longer be symmetric")
    if problems:
        raise GateFailure(
            4, f"graph {leg}: configuration readback failed:\n  - " + "\n  - ".join(problems))
    CTX.record(f"graph {leg}: configuration readback", True,
               f"builddir={config.get('builddir')} bin={config.get('_kimix_bin_dir')}")

    # --- 2. kimix-core's third-party dependency list -------------------------
    core = targets.get("kimix-core")
    if core is None:
        raise GateFailure(4, f"graph {leg}: there is no kimix-core target")
    deps = sorted(as_list(core["deps"]))
    expected = sorted(EXPECTED_KIMIX_CORE_DEPS[leg])
    if deps != expected:
        raise GateFailure(
            4,
            f"graph {leg}: kimix-core deps are {deps}, expected {expected}"
            + ("\n  the external leg's whole contract is ONE direct third-party dep (lc-core)"
               if external else ""))
    CTX.record(f"graph {leg}: kimix-core deps == {expected}", True, str(deps))

    # --- 3. the external define is PUBLIC -----------------------------------
    if external:
        if not _has_define(as_list(core["defines_self"]), "KIMIX_EXTERNAL_THIRDPARTY"):
            raise GateFailure(4, "graph ex: kimix-core does not define KIMIX_EXTERNAL_THIRDPARTY")
        dependents = [name for name, entry in targets.items()
                      if "kimix-core" in as_list(entry["deps"]) and entry["enabled"]]
        if not dependents:
            raise GateFailure(4, "graph ex: no enabled target depends on kimix-core")
        inherited = [name for name in dependents
                     if _has_define(as_list(targets[name]["defines_from"]), "KIMIX_EXTERNAL_THIRDPARTY")]
        if len(inherited) != len(dependents):
            missing = sorted(set(dependents) - set(inherited))
            raise GateFailure(
                4,
                "graph ex: KIMIX_EXTERNAL_THIRDPARTY is not PUBLIC -- these dependents of "
                f"kimix-core do not inherit it: {missing[:8]}{' ...' if len(missing) > 8 else ''}")
        CTX.record("graph ex: KIMIX_EXTERNAL_THIRDPARTY is public", True,
                   f"inherited by all {len(dependents)} dependents, declared only on kimix-core")
    else:
        leaked = sorted(name for name, entry in targets.items()
                        if _has_define(as_list(entry["defines_from"]), "KIMIX_EXTERNAL_THIRDPARTY"))
        if leaked:
            raise GateFailure(
                4, f"graph sa: KIMIX_EXTERNAL_THIRDPARTY is defined in the standalone leg on {leaked}")
        CTX.record("graph sa: no KIMIX_EXTERNAL_THIRDPARTY anywhere", True)

    # --- 4. no duplicate third-party target --------------------------------
    if external:
        mimalloc = targets.get("mimalloc")
        if mimalloc is None:
            raise GateFailure(4, "graph ex: no mimalloc target at all (lc-core's EASTL needs one)")
        vendored = [path for path in as_list(mimalloc["files"])
                    if "src/ext/mimalloc/" in path.replace("\\", "/")
                    and "/" + CTX.external_name + "/" not in path.replace("\\", "/")]
        if vendored:
            raise GateFailure(
                4,
                f"graph ex: the external leg still compiles the vendored mimalloc: {vendored[:3]} "
                "-- two mi_* heaps in one process")
        CTX.record("graph ex: mimalloc is the external tree's copy", True,
                   (as_list(mimalloc["files"]) or ["<no files>"])[0])

    for library, (owner_target, fragments) in DUPLICATE_SENSITIVE_LIBS.items():
        compiled_by = {}
        for name, entry in targets.items():
            if not entry["enabled"]:
                continue
            for path in as_list(entry["files"]):
                haystack = path.replace("\\", "/")
                # Only a copy inside THIS tree counts: the external tree has its own
                # src/ext/yyjson, src/ext/marl, ... and lc-yyjson compiles exactly that
                # one, which is the point of the external leg.
                if "/" + CTX.external_name + "/" in haystack:
                    continue
                if any(fragment in haystack for fragment in fragments):
                    compiled_by[name] = compiled_by.get(name, 0) + 1
        stray = sorted(name for name in compiled_by if name != owner_target)
        if leg == "sa":
            provider = targets.get(owner_target)
            if provider is None or not provider["enabled"]:
                raise GateFailure(
                    4, f"graph sa: the vendored {library} target {owner_target!r} is missing or disabled")
            if stray:
                raise GateFailure(
                    4,
                    f"graph sa: {library} is compiled by more than one target: {owner_target} + "
                    f"{stray} -- two copies in one process")
            CTX.record(
                f"graph sa: {library} has exactly one provider", True,
                f"{owner_target} ({compiled_by.get(owner_target, 0)} file(s))"
                + (" [header-only]" if not compiled_by else ""))
        else:
            # mimalloc keeps the same target NAME in both legs on purpose (EASTL's
            # bundled copy IS the mimalloc, archived into luisa-core.dll), so its
            # check is the source-tree one above. The other four must not exist.
            if library != "mimalloc":
                if owner_target in targets:
                    raise GateFailure(
                        4,
                        f"graph ex: the vendored {library} target {owner_target!r} still exists "
                        "-- the external leg must not declare a second copy of a facility "
                        "lc-core provides")
                if compiled_by:
                    raise GateFailure(
                        4, f"graph ex: {library} is still compiled from this tree by "
                           f"{sorted(compiled_by)}")
            CTX.record(
                f"graph ex: {library} is provided by the external tree only", True,
                "no vendored target, nothing compiled from this tree" if library != "mimalloc"
                else "target 'mimalloc' compiles only the external tree's copy")

    # --- 5. the external tree is included MINIMALLY ------------------------
    if external:
        allowed_targets = {"lc-core", "lc-yyjson", "lc-check-winsdk"} | TARGET_DELTA_ADDED_EX
        bad_targets = sorted(name for name in targets
                             if name.startswith(("lc-", "eastl", "spdlog", "btree"))
                             and name not in allowed_targets)
        if bad_targets:
            raise GateFailure(4, f"graph ex: unexpected external-tree target(s) {bad_targets}")
        offending = {}
        for name, entry in targets.items():
            if not entry["enabled"]:
                continue
            for path in as_list(entry["files"]):
                haystack = path.replace("\\", "/")
                marker = "/" + CTX.external_name + "/"
                if marker not in haystack:
                    continue
                relative = haystack.split(marker, 1)[1]
                if not relative.startswith(LC_ALLOWED_FILE_PREFIXES):
                    offending.setdefault(relative, []).append(name)
        if offending:
            sample = "; ".join(f"{path} <- {owners}" for path, owners in list(offending.items())[:6])
            raise GateFailure(
                4,
                f"graph ex: the external leg compiles files outside the minimal lc-core subgraph "
                f"({len(offending)} file(s)): {sample}")
        CTX.record("graph ex: only the minimal LC subgraph is included", True,
                   f"allowed prefixes: {', '.join(LC_ALLOWED_FILE_PREFIXES)}")

    # --- 6. registered tests -----------------------------------------------
    registered = {}
    for name, entry in targets.items():
        if entry["enabled"] and as_list(entry["tests"]):
            registered[name] = sorted(as_list(entry["tests"]))
    if not registered:
        raise GateFailure(4, f"graph {leg}: NO test entries are registered at all")
    CTX.record(f"graph {leg}: registered test entries", True, f"{len(registered)} target(s)")
    return registered


def compare_graphs(graphs, registrations):
    """Cross-leg graph comparison: same targets, same tests, documented delta only."""
    sa = _targets(graphs["sa"])
    ex = _targets(graphs["ex"])
    sa_enabled = {name for name, entry in sa.items() if entry["enabled"]}
    ex_enabled = {name for name, entry in ex.items() if entry["enabled"]}
    added = ex_enabled - sa_enabled
    removed = sa_enabled - ex_enabled
    if added != TARGET_DELTA_ADDED_EX:
        raise GateFailure(
            4,
            "target-set parity: the external leg adds targets outside the documented delta "
            f"(unexpected: {sorted(added - TARGET_DELTA_ADDED_EX)}, "
            f"missing: {sorted(TARGET_DELTA_ADDED_EX - added)})")
    if removed != TARGET_DELTA_REMOVED_EX:
        raise GateFailure(
            4,
            "target-set parity: the external leg removes targets outside the documented delta "
            f"(unexpected: {sorted(removed - TARGET_DELTA_REMOVED_EX)}, "
            f"missing: {sorted(TARGET_DELTA_REMOVED_EX - removed)})")
    CTX.record("target-set parity (both legs)", True,
               f"{len(sa_enabled)} targets in both, +{sorted(added)} / -{sorted(removed)}")

    sa_tests = registrations["sa"]
    ex_tests = registrations["ex"]
    if set(sa_tests) != set(ex_tests):
        raise GateFailure(
            4,
            "test-registration parity: the two legs register different test targets "
            f"(only standalone: {sorted(set(sa_tests) - set(ex_tests))[:8]}, "
            f"only external: {sorted(set(ex_tests) - set(sa_tests))[:8]})")
    for name in sa_tests:
        if sorted(sa_tests[name]) != sorted(ex_tests[name]):
            raise GateFailure(
                4, f"test-registration parity: {name} registers {sa_tests[name]} vs {ex_tests[name]}")
    total = sum(len(entries) for entries in sa_tests.values())
    CTX.record("test-registration parity (both legs)", True, f"{total} entries in each leg")
    return total


# ---------------------------------------------------------------------------
# Phase 4 -- the complete test sweep, parsed into a manifest
# ---------------------------------------------------------------------------
SUMMARY_PASS = re.compile(r"all tests passed \((\d+) asserts? in (\d+) tests?\)")
SUMMARY_TESTS_FAILED = re.compile(r"tests:\s*(\d+)\s*\|\s*(\d+) failed")
SUMMARY_ASSERTS_FAILED = re.compile(r"asserts:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed")
# "[ 50%]: test_core_leg_contract/default  passed 0.016s" -- and, when the suite
# printed something, "[100%]: test_graph_db_memory/default .. passed 0.140s": the dots
# are xmake's "there is a stdout/stderr block below" marker, not part of the name.
REPORT_LINE = re.compile(
    r"^\[\s*\d+%\]:\s+(\S+?)/(\S+?)(?:\s*\.+)?\s+(passed|failed|skipped|error)\b(.*)$")
RUNNER_TAIL = re.compile(r"(\d+)% tests passed,\s*(\d+) test\(s\) failed out of (\d+)")
# "[bench] hist/search_1k_queries 96497.5 ns/op 10363 ops/s  (3 iters)"
BENCH_ITERS = re.compile(r"\[bench\].*?\((\d+) iters?\)")


def count_summaries(text):
    """Sum the Boost.UT summaries in `text` -> (asserts, tests, suites, failed).

    A passing suite prints `Suite '<name>': all tests passed (N asserts in M tests)`;
    a failing one prints `tests: M | F failed` and `asserts: N | P passed | F failed`.
    Both are summed so a binary with several top-level suites compares as one number.
    """
    asserts = 0
    tests = 0
    suites = 0
    failed_suites = 0
    for match in SUMMARY_PASS.finditer(text):
        asserts += int(match.group(1))
        tests += int(match.group(2))
        suites += 1
    for match in SUMMARY_TESTS_FAILED.finditer(text):
        tests += int(match.group(1))
        failed_suites += 1
    for match in SUMMARY_ASSERTS_FAILED.finditer(text):
        asserts += int(match.group(1))
    return asserts, tests, suites, failed_suites


def parse_test_output(text):
    """Parse `xmake test -v` output into {target: entry}.

    One report block per test entry: a `[NN%]: <target>/<test>  passed|failed <time>`
    line followed by that entry's `stdout:` / `stderr:` bodies (where Boost.UT prints
    its summary), then the next block. Blocks are attributed by position, so the gate
    runs with --test-jobs 1 by default.
    """
    lines = text.splitlines()
    entries = {}
    current = None
    body = []

    def flush():
        if current is None:
            return
        target, test, status = current
        raw = "\n".join(body)
        asserts, tests, suites, failed_suites = count_summaries(raw)
        entry = entries.setdefault(target, {
            "target": target, "entries": [], "asserts": 0, "tests": 0, "suites": 0,
            "failed_suites": 0, "status": "passed", "has_summary": False,
            # Iteration counts of the suite's own time-budgeted benchmark loops. Those
            # cases put expect() INSIDE the loop, so the assert count is a function of
            # how many iterations fit in the budget -- a speed measurement.
            "bench_iters": [], "tail": [],
        })
        entry["entries"].append({"test": test, "status": status})
        entry["asserts"] += asserts
        entry["tests"] += tests
        entry["suites"] += suites
        entry["failed_suites"] += failed_suites
        if suites or failed_suites:
            entry["has_summary"] = True
        if status != "passed" or failed_suites:
            entry["status"] = "failed"
        for match in BENCH_ITERS.finditer(raw):
            entry["bench_iters"].append(int(match.group(1)))
        entry["tail"] = raw.strip().splitlines()[-6:]

    for line in lines:
        match = REPORT_LINE.match(line)
        if match:
            flush()
            current = (match.group(1), match.group(2), match.group(3))
            body = []
            continue
        if current is not None:
            if RUNNER_TAIL.search(line):
                flush()
                current = None
                body = []
                continue
            body.append(line)
    flush()
    return entries


def python_runtime_dir():
    """The directory holding pythonNNN.dll -- what tests/xmake.lua's before_run adds
    to PATH for the runtime_py suites. A directly-run test binary needs the same."""
    return os.path.dirname(os.path.abspath(sys.executable))


def run_single_test_binary(leg, target, runs):
    """Run one test binary `runs` times directly and return the parsed counts.

    Used only to prove that a manifest mismatch is nondeterminism: re-measuring
    through `xmake test` would need the tree reconfigured for that leg (the recorded
    configuration is shared), while the binary plus the one PATH entry before_run adds
    is enough to reproduce a suite.

    The benchmark iteration counts are parsed as well: an assert count that follows
    iterations can only be re-checked if the re-run carries its own `(N iters)`.
    """
    exe = os.path.join(leg_bindir(leg), "release", f"{target}.exe")
    if not os.path.isfile(exe):
        exe = os.path.join(leg_bindir(leg), "release", target)
    if not os.path.isfile(exe):
        return None
    env = os.environ.copy()
    env["PATH"] = python_runtime_dir() + os.pathsep + env.get("PATH", "")
    results = []
    for _ in range(runs):
        proc = subprocess.run(exe, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, encoding="utf-8",
                              errors="replace", timeout=1800)
        output = strip_ansi(proc.stdout or "")
        asserts, tests, _suites, failed = count_summaries(output)
        results.append({"asserts": asserts, "tests": tests, "rc": proc.returncode,
                        "failed": failed,
                        "bench_iters": [int(match.group(1))
                                        for match in BENCH_ITERS.finditer(output)]})
    return results


def run_tests(leg, args):
    # Re-read before the sweep: the test phase is the expensive one, and it must run
    # in ITS leg's tree, not in whichever leg configured the tree last.
    assert_tree_config(leg, "test")
    xmake = xmake_bin()
    cmd = [xmake, "test", "-v", "-j", str(args.test_jobs)]
    if args.only_targets:
        cmd += [f"{name}/*" for name in args.only_targets]
    print(f"\n[test {leg}] {' '.join(cmd)}")
    started = time.time()
    rc, text = run(cmd, timeout=None)
    seconds = round(time.time() - started, 1)
    plain = strip_ansi(text)
    os.makedirs(os.path.join(CTX.outdir, "test-output"), exist_ok=True)
    with open(os.path.join(CTX.outdir, "test-output", f"{leg}.log"), "w",
              encoding="utf-8") as handle:
        handle.write(plain)

    manifest = parse_test_output(plain)
    tail_match = RUNNER_TAIL.search(plain)
    out_of = int(tail_match.group(3)) if tail_match else -1
    if rc != 0 and not manifest:
        raise GateFailure(5, f"test {leg}: `xmake test` failed (rc={rc}) and produced no report")
    print(f"      {len(manifest)} suite(s) reported, runner says {out_of} test(s), {seconds}s")
    return manifest, plain, out_of, seconds


def assert_manifest(leg, manifest, out_of, registered):
    problems = []
    # 1. every executed entry passes and reports a real verdict
    for name, entry in sorted(manifest.items()):
        if entry["status"] != "passed":
            problems.append(f"{name}: xmake-level status {entry['status']} (tail: {entry['tail']})")
            continue
        if (not entry["has_summary"] and name not in NO_SUMMARY_EXEMPT
                and name not in ZERO_ASSERT_EXEMPT):
            problems.append(
                f"{name}: no Boost.UT summary line (a suite that prints nothing is not a pass)")
            continue
        if entry["has_summary"] and entry["tests"] == 0 and name not in ZERO_ASSERT_EXEMPT:
            problems.append(
                f"{name}: reported 0 tests ({entry['asserts']} asserts) -- a zero-test green "
                "is a false green, see the ut filter caveat in .agents/skills/test")
            continue
        if (entry["has_summary"] and entry["asserts"] == 0 and entry["tests"] > 0
                and name not in ZERO_ASSERT_EXEMPT):
            problems.append(f"{name}: {entry['tests']} test(s) but 0 asserts")
    if problems:
        raise GateFailure(5, f"test sweep {leg}:\n  - " + "\n  - ".join(problems))

    # 2. the executed set must be the REGISTERED set (an enabled test target that
    #    never ran is the silent-skip failure mode) -- compared per TARGET and per
    #    ENTRY, and against the runner's own count of ENTRIES. Counting report
    #    BLOCKS (i.e. targets) instead of entries is how a dropped `add_tests` entry
    #    could hide inside a target that still ran.
    executed = set(manifest)
    missing = sorted(set(registered) - executed)
    extra = sorted(executed - set(registered))
    if missing:
        raise GateFailure(
            5, f"test sweep {leg}: {len(missing)} registered test target(s) did not run: {missing[:10]}")
    if extra:
        raise GateFailure(5, f"test sweep {leg}: unexpected test target(s) ran: {extra[:10]}")
    parsed_entries = sum(len(entry["entries"]) for entry in manifest.values())
    registered_entries = sum(len(entries) for entries in registered.values())
    entry_mismatch = []
    for name, entries in sorted(registered.items()):
        entry = manifest.get(name)
        if entry is None:
            continue
        ran = sorted(item["test"] for item in entry["entries"])
        want = sorted(entries)
        if ran != want:
            entry_mismatch.append(f"{name}: ran {ran}, registered {want}")
    if entry_mismatch:
        raise GateFailure(
            5, f"test sweep {leg}: registered and executed test ENTRIES differ:\n  - "
            + "\n  - ".join(entry_mismatch[:10]))
    if out_of >= 0 and out_of != parsed_entries:
        raise GateFailure(
            5,
            f"test sweep {leg}: the runner reports {out_of} test ENTR(y/ies) but "
            f"{parsed_entries} entry block(s) were parsed -- the parse would otherwise "
            "hide a lost entry")
    if registered_entries != parsed_entries:
        raise GateFailure(
            5,
            f"test sweep {leg}: the evaluated graph registers {registered_entries} test "
            f"entr(y/ies) but {parsed_entries} ran -- a registered entry that never ran "
            "is a silent skip, not a pass")

    total_asserts = sum(entry["asserts"] for entry in manifest.values())
    total_tests = sum(entry["tests"] for entry in manifest.values())
    env_gated = sorted(
        name for name, entry in manifest.items()
        if name in ZERO_ASSERT_EXEMPT and (not entry["has_summary"] or entry["tests"] == 0))
    no_summary = sorted(name for name, entry in manifest.items()
                        if not entry["has_summary"] and name not in ZERO_ASSERT_EXEMPT)
    CTX.record(
        f"test sweep {leg}", True,
        f"{len(manifest)} suite(s) / {parsed_entries} entr(y/ies), {total_tests} tests, "
        f"{total_asserts} asserts"
        + (f"; env-gated (no body ran): {env_gated}" if env_gated else "")
        + (f"; no Boost.UT summary by design: {no_summary}" if no_summary else ""))
    return {"suites": len(manifest), "entries": parsed_entries, "tests": total_tests,
            "asserts": total_asserts}


def run_extra_targets(leg, args):
    """The built-but-unregistered targets that must still run (`xmake run ...`).

    A bare `xmake run <target>` can block forever when the target is an interpreter
    that reads stdin, so every extra run carries its own arguments and a timeout.
    """
    xmake = xmake_bin()
    results = {}
    for target, runargs, why, expectation in EXTRA_RUN_TARGETS:
        cmd = [xmake, "run", target, *runargs]
        print(f"\n[extra {leg}] {' '.join(cmd)}   ({why})")
        try:
            rc, text = run(cmd, timeout=args.extra_timeout)
        except subprocess.TimeoutExpired:
            raise GateFailure(
                5, f"extra target {target} ({leg}) did not finish within "
                   f"{args.extra_timeout}s -- a target that waits forever is a failure, "
                   "not a pass")
        plain = strip_ansi(text)
        CTX.log(f"extra_{leg}_{target}.log", plain)
        tail = "\n".join(line for line in plain.strip().splitlines()[-6:])
        error_line = ""
        # The informative line of a failing run is the script:line detail
        # (`main.lua:41: attempt to index a nil value (local 'f')`) or an explicit
        # `error:` line -- the bare "stack traceback:" header is only a fallback,
        # because two legs that fail at the SAME place must compare that place.
        for pattern in (r"[\w./\\-]+\.lua:\d+:", r"^\s*error:", r"^\s*stack traceback:"):
            for line in plain.splitlines():
                if re.search(pattern, line):
                    error_line = line.strip()
                    break
            if error_line:
                break
        # The captured line can name the leg's own binary (Lua's error prefix does:
        # "<exe>: main.lua:41: ..."), and the two legs' binaries live in different
        # directories. Replace the leg's bin path with a placeholder so the two legs
        # compare the FAILURE, not the path.
        for prefix in (os.path.join(leg_bindir(leg), "release"),
                       leg_bindir(leg), _norm(os.path.join(leg_bindir(leg), "release")),
                       _norm(leg_bindir(leg))):
            for variant in (prefix, prefix.replace("\\", "/"), prefix.replace("/", "\\")):
                error_line = error_line.replace(variant + os.sep, "<bin>/")
                error_line = error_line.replace(variant + "/", "<bin>/")
                error_line = error_line.replace(variant, "<bin>")
        results[target] = {
            "status": "passed" if rc == 0 else "failed",
            "rc": rc, "first_error": error_line, "tail": tail,
            "expectation": expectation, "why": why,
        }
        if rc == 0:
            CTX.record(f"extra target {leg}: {target}", True, tail.replace("\n", " | ")[-160:])
        elif expectation == "pass":
            raise GateFailure(5, f"extra target {target} ({leg}) failed (rc={rc}):\n{tail}")
        else:
            # Deferred: compare_extra_targets() requires the two legs to agree.
            CTX.note(f"extra target {leg}: {target} (known limitation, compared across legs)",
                     f"rc={rc} {error_line or tail.replace(chr(10), ' | ')[-140:]}")
    return results


def compare_extra_targets(extras):
    """For `same-in-both-legs` extras: the two legs must produce the SAME outcome.

    This is how a genuinely pre-existing limitation is reported without hiding it:
    the gate runs the target in both legs, records the exit code and the first error
    line, and fails if the legs disagree (a leg difference would be a real finding)
    or if the limitation disappeared in only one leg.
    """
    for target in EXTRA_RUN_TARGETS:
        name = target[0]
        if name not in extras.get("sa", {}) or name not in extras.get("ex", {}):
            continue
        sa = extras["sa"][name]
        ex = extras["ex"][name]
        if sa["expectation"] != "same-in-both-legs":
            continue
        if sa["rc"] != ex["rc"] or (sa["first_error"] or "") != (ex["first_error"] or ""):
            raise GateFailure(
                5,
                f"extra target {name}: the two legs disagree -- sa rc={sa['rc']} "
                f"({sa['first_error']!r}) vs ex rc={ex['rc']} ({ex['first_error']!r}). "
                "A leg-dependent outcome is a real finding, not a known limitation.")
        if sa["rc"] == 0:
            CTX.record(f"extra target {name}: both legs pass", True,
                       "the documented limitation is gone -- tighten this entry")
        else:
            CTX.record(
                f"extra target {name}: identical known limitation in both legs", True,
                f"rc={sa['rc']} in BOTH legs, first error: {sa['first_error'] or 'n/a'}")


def bench_assert_model(name, left, right, remeasured):
    """Explain an assert delta with the suite's OWN time-budgeted loop, or return None.

    A `[bench] ... (N iters)` line means the case runs expect() INSIDE a loop that
    is bounded by a time budget (tests/unit/native/bench_util.h), so for such a suite

        asserts = fixed + weight * sum(iters)

    with `weight` >= 1 asserts per iteration and a `fixed` part outside the loops.
    One pair of measurements cannot distinguish that model from a real divergence,
    so it is accepted only when ALL of the following hold:

      * both legs printed bench evidence (a bench line in one leg only proves
        nothing about the other);
      * the assert DELTA is exactly an integer multiple of the iteration DELTA, and
        the multiplier is >= 1 (a per-iteration assert count);
      * the implied `fixed` part is identical in both legs and not negative;
      * re-running each leg's own binary reproduces the SAME model: same tests
        count, same number of bench lines, and asserts - weight*iters == fixed for
        every re-run of both legs.

    Anything less is not evidence, and compare_manifests() then treats the delta as
    a real divergence (or as flakiness if a re-run is unstable).
    """
    sa_iters = left["bench_iters"]
    ex_iters = right["bench_iters"]
    if not sa_iters or not ex_iters:
        return None
    sa_total = sum(sa_iters)
    ex_total = sum(ex_iters)
    delta_asserts = left["asserts"] - right["asserts"]
    delta_iters = sa_total - ex_total
    if delta_iters == 0:
        return None
    weight, remainder = divmod(delta_asserts, delta_iters)
    if remainder != 0 or weight < 1:
        return None
    fixed = left["asserts"] - weight * sa_total
    if fixed != right["asserts"] - weight * ex_total or fixed < 0:
        return None
    evidence = []
    for leg, first in (("sa", left), ("ex", right)):
        runs = remeasured.get(leg)
        if not runs:
            return None
        for index, run in enumerate(runs, start=1):
            if run["rc"] != 0 or run["tests"] != first["tests"]:
                return None
            if len(run["bench_iters"]) != len(first["bench_iters"]):
                return None
            if run["asserts"] - weight * sum(run["bench_iters"]) != fixed:
                return None
            evidence.append(f"{leg}#{index} {run['asserts']}a/{sum(run['bench_iters'])}i")
    return (f"{name}: BENCH-COUNTED assert delta, proven arithmetically -- the suite's own "
            f"`(N iters)` lines give asserts = {fixed} + {weight}*iters: sa "
            f"{left['asserts']}a over {sa_total} iters, ex {right['asserts']}a over "
            f"{ex_total} iters (delta {delta_asserts}a = {weight} * {delta_iters}i, same "
            f"fixed part), tests {left['tests']} in both, and the same model reproduces on "
            f"re-runs in both legs ({', '.join(evidence)})")


def compare_manifests(manifests):
    sa = manifests["sa"]
    ex = manifests["ex"]
    problems = []
    diffs = []
    for name in sorted(set(sa) | set(ex)):
        left = sa.get(name)
        right = ex.get(name)
        if left is None or right is None:
            problems.append(f"{name}: present in only one leg")
            continue
        # The per-entry (test name, status) lists must match across the legs: the
        # suite counts alone would hide "entry A passed here and entry B failed
        # there" as long as the totals agreed.
        left_entries = sorted((item["test"], item["status"]) for item in left["entries"])
        right_entries = sorted((item["test"], item["status"]) for item in right["entries"])
        if left_entries != right_entries:
            problems.append(
                f"{name}: per-entry status differs -- sa {left_entries[:6]} vs ex "
                f"{right_entries[:6]}")
            continue
        if name in EXPECTED_MANIFEST_DELTAS:
            expected_sa, expected_ex = EXPECTED_MANIFEST_DELTAS[name]
            actual = ((left["asserts"], left["tests"]), (right["asserts"], right["tests"]))
            if actual != (expected_sa, expected_ex):
                problems.append(
                    f"{name}: leg-specific manifest {actual} differs from the reviewed "
                    f"expectation {(expected_sa, expected_ex)}")
            else:
                diffs.append(
                    f"{name}: {left['asserts']}a/{left['tests']}t (sa) vs {right['asserts']}a/"
                    f"{right['tests']}t (ex) -- exact reviewed leg-specific mapping contract")
            continue
        if (left["asserts"], left["tests"]) == (right["asserts"], right["tests"]):
            continue
        # A mismatch is either a real leg difference or a count that is not a
        # behaviour at all. Two attributions, both evidence-based, never a name
        # allowlist:
        #  a) the suite's own `[bench] ... (N iters)` lines say its expect() calls live
        #     INSIDE a time-budgeted loop, so the count follows iterations -- accepted
        #     ONLY when the delta is arithmetically the iteration delta and the model
        #     reproduces on a re-run in both legs (bench_assert_model);
        #  b) otherwise re-measure the suite twice per leg straight from its binary: a
        #     run of the SAME leg that does not reproduce its own count proves load
        #     dependence, and the suite is reported FLAKY with all four numbers.
        # A count that is stable within a leg but differs across legs is neither of
        # those and stays a hard failure.
        tests_equal = (left["tests"] == right["tests"]
                       and left["failed_suites"] == 0 and right["failed_suites"] == 0)
        remeasured = {leg: run_single_test_binary(leg, name, 2) for leg in ("sa", "ex")}
        if tests_equal and (left["bench_iters"] or right["bench_iters"]):
            note = bench_assert_model(name, left, right, remeasured)
            if note is not None:
                diffs.append(note)
                continue
        unstable = {}
        tests_agree = True
        for leg, counts in remeasured.items():
            if not counts:
                unstable[leg] = None
                tests_agree = False
                continue
            expected_tests = (left if leg == "sa" else right)["tests"]
            unstable[leg] = any(count["asserts"] != counts[0]["asserts"]
                                or count["tests"] != counts[0]["tests"]
                                or count["rc"] != 0 for count in counts)
            tests_agree = tests_agree and all(count["tests"] == expected_tests
                                              and count["rc"] == 0 for count in counts)
        if (any(flag is True for flag in unstable.values()) and tests_agree
                and left["bench_iters"] and right["bench_iters"]):
            diffs.append(
                f"{name}: FLAKY, proven by re-measurement (not exempted) -- sa reported "
                f"{left['asserts']}a/{left['tests']}t and re-ran "
                f"{[count['asserts'] for count in remeasured['sa']]}; ex reported "
                f"{right['asserts']}a/{right['tests']}t and re-ran "
                f"{[count['asserts'] for count in remeasured['ex']]}")
            continue
        problems.append(
            f"{name}: asserts/tests {left['asserts']}/{left['tests']} (sa) != "
            f"{right['asserts']}/{right['tests']} (ex); bench evidence "
            f"sa_iters={left['bench_iters']} ex_iters={right['bench_iters']} does not "
            f"explain the delta; re-measurement "
            f"sa={[count['asserts'] for count in (remeasured['sa'] or [])]} "
            f"ex={[count['asserts'] for count in (remeasured['ex'] or [])]} "
            "(stable within a leg => a real divergence)")
    if problems:
        raise GateFailure(
            5,
            "manifest parity: the two legs do not report the same per-suite results:\n  - "
            + "\n  - ".join(problems[:20]))
    for diff in diffs:
        CTX.note("manifest delta (allowed)", diff)
    CTX.record("manifest parity (both legs)", True,
               f"{len(sa)} suites compared; {len(diffs)} allowed delta(s): "
               + ("; ".join(diff.split(" -- ")[0] for diff in diffs) if diffs else "none"))


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def report_relative_text(text):
    """Replace workspace-absolute paths with workspace-relative report text.

    Commands still execute with absolute paths. Only persisted documentation is
    normalized, including JSON's doubled-backslash spelling.
    """
    workspace = os.path.dirname(THIRDPARTY)
    roots = (workspace, workspace.replace("\\", "/"), workspace.replace("\\", "\\\\"))
    for root in sorted(roots, key=len, reverse=True):
        for separator in ("\\\\", "\\", "/"):
            text = text.replace(root + separator, "")
        text = text.replace(root, ".")
    return text


def write_report(args, meta):
    os.makedirs(CTX.outdir, exist_ok=True)
    payload = {
        "stamp": CTX.stamp,
        "args": {key: value for key, value in vars(args).items()},
        "checks": CTX.checks,
        "commands": CTX.last_commands,
        "meta": meta,
    }
    with open(os.path.join(CTX.outdir, "report.json"), "w", encoding="utf-8") as handle:
        handle.write(report_relative_text(json.dumps(payload, indent=1, sort_keys=True)))

    lines = ["# native dual-configuration gate", "", f"stamp: {CTX.stamp}", ""]
    if meta.get("scoped"):
        lines += ["> **SCOPED RUN -- NOT a full dual-configuration gate result.**",
                  ">",
                  f"> A passing scoped run exits {SCOPED_EXIT} (never 0). What is missing:"]
        for reason in meta.get("scope_reasons", []):
            lines.append(f"> - {reason}")
        lines.append("")
    lines += ["## checks", "", "| check | status | detail |", "|---|---|---|"]
    for check in CTX.checks:
        detail = str(check["detail"]).replace("|", "\\|")
        lines.append(f"| {check['check']} | {check['status']} | {detail} |")
    lines += ["", "## inputs", ""]
    for key, value in meta.items():
        if key in ("scoped", "scope_reasons", "full_gate", "totals", "extra_targets"):
            continue
        lines.append(f"- **{key}**: {value}")
    if "totals" in meta:
        lines += ["", "## totals",
                  "", "| leg | suites | entries | tests | asserts | sweep seconds |",
                  "|---|---|---|---|---|---|"]
        for leg, totals in meta["totals"].items():
            lines.append(f"| {leg} | {totals['suites']} | {totals.get('entries', '?')} | "
                         f"{totals['tests']} | {totals['asserts']} | "
                         f"{totals.get('seconds', '?')} |")
    with open(os.path.join(CTX.outdir, "report.md"), "w", encoding="utf-8") as handle:
        handle.write(report_relative_text("\n".join(lines) + "\n"))
    print(f"\nreport: {os.path.relpath(os.path.join(CTX.outdir, 'report.md'), ROOT)}")
    print(f"      : {os.path.relpath(os.path.join(CTX.outdir, 'report.json'), ROOT)}")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Dual-configuration hard gate for kimix-native "
                    "(kimix_external_thirdparty=false vs true).")
    parser.add_argument("--legs", default="sa,ex",
                        help="comma separated legs: sa (standalone), ex (external). Default both.")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                        help="build jobs (default: CPU count)")
    parser.add_argument("--test-jobs", type=int, default=1,
                        help="parallel test entries: only 1 is accepted, because each "
                             "report block is attributed to a suite BY POSITION in the "
                             "merged output")
    parser.add_argument("--configure-only", action="store_true",
                        help="configure each leg and stop (the graph assertions still run)")
    parser.add_argument("--skip-build", action="store_true",
                        help="do not build (implies --reuse): graph + tests only")
    parser.add_argument("--reuse", action="store_true",
                        help="keep .gates/build/<leg> and .gates/bin/<leg> instead of wiping them")
    parser.add_argument("--no-tests", action="store_true",
                        help="configure/build/graph only, do not run the sweep")
    parser.add_argument("--no-extra-targets", action="store_true",
                        help="skip the `xmake run` targets that are not part of xmake test")
    parser.add_argument("--only-targets", default="",
                        help="comma separated test targets (scoped run, reported as SCOPED)")
    parser.add_argument("--extra-timeout", type=int, default=900,
                        help="seconds a built-but-unregistered `xmake run` target may take "
                             "(default 900)")
    parser.add_argument("--configure-extra", default="",
                        help="extra `xmake f` arguments, space separated (diagnostics only)")
    args = parser.parse_args(argv)
    args.legs = [leg.strip() for leg in args.legs.split(",") if leg.strip()]
    for leg in args.legs:
        if leg not in LEGS:
            parser.error(f"unknown leg {leg!r} (choose from {sorted(LEGS)})")
    args.reuse = args.reuse or args.skip_build
    if args.test_jobs != 1:
        # parse_test_output() attributes each `[NN%]: <target>/<test>` report block to
        # the suite by POSITION in the merged xmake output, and the runner interleaves
        # blocks when entries run in parallel. Refusing anything but 1 is what keeps
        # the manifest honest; making the parser concurrency-safe is the prerequisite
        # for a larger -j, not a flag.
        parser.error("--test-jobs must be 1: report blocks are attributed to suites by "
                     "position in the merged output, so a parallel sweep would "
                     "mis-attribute counts (the manifest comparison would be meaningless). "
                     "A concurrency-safe parser is required before this can be raised.")
    args.only_targets = [name.strip() for name in args.only_targets.split(",") if name.strip()]
    args.configure_extra = args.configure_extra.split() if args.configure_extra else []
    return args


SCOPED_EXIT = 7


def scope_reasons(args):
    """Why this run cannot be a full dual-configuration gate result.

    A scoped run is legitimate while iterating -- but it must never be reportable as
    `GATE: PASS` / exit 0, because "both legs, every target, every registered entry"
    is exactly what it did NOT check. Each reason names the flag responsible.
    """
    reasons = []
    if args.configure_only:
        reasons.append("--configure-only: no target was built and no test ran")
    if args.no_tests:
        reasons.append("--no-tests: the registered test sweep did not run")
    if args.no_extra_targets:
        reasons.append("--no-extra-targets: the built-but-unregistered `xmake run` targets "
                       "were skipped")
    if sorted(args.legs) != ["ex", "sa"]:
        reasons.append(f"one leg only ({','.join(args.legs)}): the cross-leg comparison "
                       "did not happen")
    if args.only_targets:
        reasons.append(f"--only-targets {','.join(args.only_targets)}: most suites were "
                       "not executed")
    return reasons


def main(argv):
    args = parse_args(argv)
    CTX.args = args
    started = time.time()
    reasons = scope_reasons(args)
    meta = {
        "scoped": bool(reasons),
        "scope_reasons": reasons,
        # A full run is the one that can report 0; everything else reports 7.
        "full_gate": not reasons,
    }
    if reasons:
        print("[SCOPED] this run is NOT a full dual-configuration gate result:")
        for reason in reasons:
            print(f"         - {reason}")
        print(f"         a passing scoped run exits {SCOPED_EXIT}, never 0")
    # Snapshot the recorded configuration(s) BEFORE anything configures the shared
    # tree: the finally block below puts them back, so a pass, a failure or Ctrl-C
    # never leaves the developer configured for .gates/...
    config_snapshot = snapshot_xmake_configs()
    try:
        preflight(args)

        graphs = {}
        registrations = {}
        manifests = {}
        totals = {}
        extras = {}
        # STRICTLY ONE LEG AT A TIME, and each leg's whole sequence -- configure,
        # evaluated graph, build, test sweep -- runs inside its own turn. The tree's
        # recorded configuration and its project.lock are shared, so a leg whose
        # sweep ran after another leg had configured the tree would exercise the
        # WRONG binaries and still look green; assert_tree_config() re-reads the
        # recorded configuration before the build and before the sweep for exactly
        # that reason.
        for leg in args.legs:
            print("\n" + "=" * 78)
            print(f"leg {leg}: {LEGS[leg][1]}")
            print("=" * 78)
            configure_leg(leg, args)
            # The evaluated graph is a READ-ONLY view of the configuration, so it can
            # be taken before the build -- and both the --reuse artifact check and the
            # --skip-build one need it to tell "nothing to compile" from "nothing to
            # reuse".
            graphs[leg] = load_graph(leg)
            registrations[leg] = assert_graph(leg, graphs[leg], args)
            if args.configure_only:
                continue
            if args.skip_build:
                checked, _stale = check_leg_artifacts(leg, graphs[leg], fresh_noop=True)
                CTX.record(f"build {leg}: skipped (--skip-build)", True,
                           f"{checked} artifact(s) present and newer than their sources")
            else:
                build_leg(leg, args, graphs[leg])
            if args.no_tests:
                continue
            manifests[leg], _plain, out_of, seconds = run_tests(leg, args)
            registered = registrations[leg]
            if args.only_targets:
                registered = {name: entries for name, entries in registered.items()
                              if any(token in name for token in args.only_targets)}
            totals[leg] = assert_manifest(leg, manifests[leg], out_of, registered)
            totals[leg]["seconds"] = seconds
            if not args.no_extra_targets and not args.only_targets:
                extras[leg] = run_extra_targets(leg, args)

        if len(graphs) == 2:
            compare_graphs(graphs, registrations)

        if args.configure_only or args.no_tests:
            print("\n[scoped] stopping before the test sweep as requested")
            write_report(args, meta)
            elapsed = round(time.time() - started, 1)
            print(f"\nGATE: SCOPED PASS ({elapsed}s, exit {SCOPED_EXIT}) -- configure + "
                  "evaluated graph only; NOT a full dual-configuration gate result")
            return SCOPED_EXIT

        if len(extras) == 2:
            compare_extra_targets(extras)

        if len(manifests) == 2:
            compare_manifests(manifests)
            sa, ex = totals["sa"], totals["ex"]
            print("\nresult totals")
            print(f"  sa: {sa['suites']} suites, {sa['entries']} entries, "
                  f"{sa['tests']} tests, {sa['asserts']} asserts, {sa['seconds']}s")
            print(f"  ex: {ex['suites']} suites, {ex['entries']} entries, "
                  f"{ex['tests']} tests, {ex['asserts']} asserts, {ex['seconds']}s")

        meta["totals"] = totals
        meta["extra_targets"] = {leg: sorted(value) for leg, value in extras.items()}
        write_report(args, meta)
        elapsed = round(time.time() - started, 1)
        if meta["scoped"]:
            print(f"\nGATE: SCOPED PASS ({elapsed}s, exit {SCOPED_EXIT}) -- NOT a full "
                  "dual-configuration gate result")
            for reason in meta["scope_reasons"]:
                print(f"      scope: {reason}")
            return SCOPED_EXIT
        print(f"\nGATE: PASS ({elapsed}s)")
        return 0
    except GateFailure as failure:
        print(f"\nGATE: FAIL [{failure.code}] {failure.message}")
        CTX.record("gate", False, failure.message.splitlines()[0][:200])
        meta["failure"] = failure.message
        try:
            write_report(args, meta)
        except OSError:
            pass
        return failure.code
    except KeyboardInterrupt:
        print("\nGATE: INTERRUPTED")
        return 6
    except Exception as error:  # noqa: BLE001 - the gate must never look like a pass
        import traceback
        traceback.print_exc()
        print(f"\nGATE: FAIL [6] internal error: {error}")
        return 6
    finally:
        # Unconditional: a gate run must never leave the tree configured for .gates/...
        restore_xmake_configs(config_snapshot)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))