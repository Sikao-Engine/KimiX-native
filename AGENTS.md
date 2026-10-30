# Rules
- NEVER change third-party extensions, unless asked.
- NEVER write an anonymous namespace (`namespace { ... }`) in `src/` (except `src/ext`) or `tests/`. The build merges translation units into unity/jumbo batches (e.g. `kimix-graph-db`, `kimix_api` with `batch_size = 8`), where anonymous-namespace helpers from different files collide. For TU-local helpers use a named namespace unique to the file, derived from its path — e.g. `src/graph_db/graph.cpp` uses `namespace src_graph_db_graph { ... }` — followed by `using namespace src_graph_db_graph;` right after each closing brace (this reproduces the implicit using-directive an anonymous namespace gets, so unqualified lookup is unchanged), and keep the `} // namespace src_graph_db_graph` closing comment. `static` functions at namespace scope are also fine. `scripts/gen_bash_fix_data.py` generates `.inc` files and must emit named namespaces the same way.
- Allocation goes through `kimix::allocate`/`deallocate` (`src/core/memory.h`); never call `mi_*` outside `src/ext` and `src/api/ffi_mem.cpp`.
- use `git diff <file>` command to check local change after done, to verify.
- read xmake skill before config/build/run c++ code target
- read test skill before write test case
- read cpp skill before start writing c++ code

# Scripts & Bootstrap Guide

This document describes every Python script in `scripts/` and the top-level `bootstrap.py`. All scripts expect to be run from the project root.

---

## `bootstrap.py` — Cross-platform C++ bootstrap (xmake)

Detects toolchains, downloads/installs xmake if missing, configures, builds, and optionally runs tests.

```bash
python bootstrap.py                          # auto-detect, release build
python bootstrap.py --debug                  # debug build
python bootstrap.py --toolchain clang-cl     # use a specific toolchain
python bootstrap.py --test # build + run tests
python bootstrap.py --target runtime_py # build one target only
python bootstrap.py --clean --debug # clean rebuild in debug mode
python bootstrap.py --list-toolchains        # list detected toolchains
python bootstrap.py --list-env               # show platform/environment info
python bootstrap.py --xmake PATH             # use a specific xmake binary
python bootstrap.py --no-download            # fail if xmake missing
python bootstrap.py --jobs 8                 # parallel build jobs (default: CPU count)
python bootstrap.py --verbose                # verbose build output
```

**Key mechanics:**
- Auto-detects MSVC, Clang-CL, LLVM, GCC depending on platform (Windows/Linux/macOS).
- Downloads xmake into `.deps/` if not on PATH (can be disabled with `--no-download`).
- On Windows with `clang-cl`/`llvm`, activates the latest MSVC vcvars environment so the linker and SDK are available.
  - `--clean` removes `.xmake/`, `build/`, and `bin/`.
  - `--target NAME` (repeatable) builds just that xmake target instead of the whole project — `publish.py` uses it to compile only the artifact it ships.

---

## `publish.py` — Build + package release ZIP archives

Builds the project in **release mode, x64**, then packages the result into a ZIP archive.

```bash
python publish.py # build + package + upload all supported platforms
python publish.py --platform windows # Windows MSVC only
python publish.py --platform linux # Linux GCC only (native, or via WSL on Windows)
python publish.py --dry-run # build + package + verify, never upload
python publish.py --skip-build # reuse bin/release (re-upload in seconds)
python publish.py --no-verify # skip post-build verification
python publish.py --no-upload # skip the gh release upload step
python publish.py --clean --jobs 8 # clean rebuild with 8 jobs
python publish.py --7z PATH # explicit 7-Zip executable
```

**What it does:**
- **Build** — delegates to `bootstrap.py` (`--toolchain msvc` on Windows, `--toolchain gcc` on Linux) with `--target runtime_py`, so a publish run compiles **only the artifact it ships** — never the CLI or the ~60 test binaries (a bare `bootstrap.py` run builds every target). On a Windows host the Linux target is built through WSL (`wsl.exe bash <script>`) after a preflight that checks the inner `python3`/`xmake` in seconds; if WSL is unavailable, the Linux target is skipped with a clear error (use `--platform windows`). `--skip-build` reuses `bin/release` for a re-upload after an auth/network failure.
- **Package** — copies `bin/release/runtime_py.pyd` into a clean staging dir, then archives it with 7-Zip as `kimix_base-<platform>-<arch>-<version>.zip` (e.g. `kimix_base-windows-x64-<version>.zip`, where `<version>` comes from `version.txt`) written next to the release artifacts in `bin/release`. The archive is a plain ZIP (Deflate), not a 7z — the old `.7z` used the BCJ2 filter, which `py7zr` cannot decompress. Before each build every output whose magic byte does not match the target platform (`runtime_py.pyd`, `runtime_py.so`, `libruntime_py.so` — bin/release is shared) is deleted, so xmake really relinks instead of no-op'ing onto the other platform's binary; packaging then refuses a foreign-magic artifact outright.
- **Version** — read from `version.txt` in the project root, the **single config file** for the version (must match `X.Y.Z`); `publish.py` refuses to run if it is missing or malformed. The version literal never appears anywhere else: xmake generates the C++ `version_string` headers (`kimix_core.h` / `runtime.h`) from it at build time, and the Python shim (`kimix_native`) plus its tests read it directly. Bumping the version = editing `version.txt` only.
- **Verify** — checks the archive listing, imports the freshly built `runtime_py.pyd` (Windows), then **extracts the packaged binary out of the archive and imports that** (Windows on the host, Linux through WSL / on a native Linux host) and requires the version from `version.txt`, so a broken or stale shipped file fails the run. Prints the archive size + sha256. Disable with `--no-verify`.
- **Upload** — after a platform builds, packages and verifies clean, its archive is uploaded with `gh release upload <tag> <zip> --repo Sikao-Engine/KimiX-native --clobber` to the standing release tag `Release` (https://github.com/Sikao-Engine/KimiX-native/releases/tag/Release), with retries on transient failures. `--clobber` makes re-running the same version replace the same-name asset. Credentials resolve automatically, first hit wins: `GH_TOKEN`/`GITHUB_TOKEN`, then an existing `gh auth login`, then the git credential helper for github.com (GitHub Desktop / GCM store a token there) exported to gh as `GH_TOKEN` for the run — so a machine that can push but never ran `gh auth login` still publishes. After the upload the release asset is read back with `gh api` and its size + sha256 digest must match the local archive, so a truncated upload fails the run. Skip the step with `--no-upload` / `--dry-run`.
  **Exit codes:** `0` = all platforms built/packaged/verified/uploaded, `1` = any platform failed or bad input (incl. missing gh), `2` (per-platform result) = verification failed, `3` (per-platform result) = gh upload failed.

---

## `scripts/`

### `check_cpp_syntax.py` — Single-file C++ syntax check via clangd

Launches a real clangd LSP server, opens the file, and prints diagnostics (errors, warnings).

```bash
python scripts\check_cpp_syntax.py myfile.cpp
python scripts\check_cpp_syntax.py --project-root .. src/main.cpp
python scripts\check_cpp_syntax.py --clangd /usr/bin/clangd file.cpp
python scripts\check_cpp_syntax.py --verbose file.cpp
```

**Flags:**
- `file` — C++ file to check.
- `--project-root` — project root (default: current dir). Used to locate `.vscode/compile_commands.json`.
- `--clangd` — path to clangd executable (default: `clangd`, also reads `clangd.path` from `.vscode/settings.json`).
- `--verbose` / `-v` — show LSP protocol messages and debug output.

**Exit codes:** `0` = no errors, `1` = syntax errors found, `2` = other failure.

---

### `check_all_cpp_syntax.py` — Parallel C++ syntax check for all project files

Reads every file listed in `compile_commands.json` and runs `check_cpp_syntax.py` on each in parallel.

```bash
python scripts\check_all_cpp_syntax.py
python scripts\check_all_cpp_syntax.py --compile-commands .vscode\compile_commands.json
python scripts\check_all_cpp_syntax.py --project-root .
python scripts\check_all_cpp_syntax.py --clangd /usr/bin/clangd
python scripts\check_all_cpp_syntax.py --jobs 8
```

**Flags:**
- `--compile-commands` — path to `compile_commands.json` (default: `.vscode/compile_commands.json`).
- `--project-root` — forwarded to each `check_cpp_syntax.py` invocation.
- `--clangd` — forwarded to each invocation.
- `--jobs` — max parallel workers (default: CPU count).

Filters source files by C++ extensions (`.cpp`, `.cc`, `.h`, `.hpp`, etc.). Treats lone `"unknown argument"` errors as harmless.

It calls `check_cpp_syntax.py` as a subprocess, so the exit code is `0` (all clean), `1` (some files have errors), or `2` (some checks failed).

### `detect_unity_build.py` — Lint sources for unity-build (jumbo) hazards
```bash
python scripts\detect_unity_build.py
python scripts\detect_unity_build.py --json build\unity_findings.json
python scripts\detect_unity_build.py --quiet
python scripts\detect_unity_build.py --clangd        # accurate, slower
python scripts\detect_unity_build.py --clangd --symbols-out build\symbols.json
```
Pure-source scan (no build needed): a small C++ lexer tracks scopes per file, then cross-file aggregation reports ERRORs for anonymous namespaces, duplicate external-linkage function/variable definitions, duplicate same-named type definitions, cross-file macro redefinitions, and same-signature functions that differ only by namespace (ERROR when one is defined directly in the namespace the `using namespace` lives in — the build-breaking case; WARNING for two sibling helper namespaces); WARNINGs for `#define` without `#undef` in `.cpp`/`.inc` files and file-scope `using namespace`; INFO (only with `--verbose`) for test-file idioms (`using namespace boost::ut;`) and header macros. Scans `src/` (except `src/ext`) and `tests/`; skips vendored single-file libraries outside `src/ext` (`src/core/lmdb/`, `src/core/detail/concurrent_queue.h`, `tests/ut/ut.hpp`). Internal-linkage (`static`) helpers in a file-unique namespace are exempt, but two definitions with the same qualified name still collide in a merged TU; `main` is exempt; findings wholly inside `tests/` are downgraded (every test file is its own single-TU target). Complements the AGENTS.md rule "never write an anonymous namespace" — run it after adding file-local helpers.

Flags:
- `--root` — project root to scan (default: repo root).
- `--json` — also write findings as a JSON array.
- `--quiet` — print only the summary line.
- `--verbose` — also print INFO findings.
- `--clangd [PATH]` — run the ambiguous-lookup check against clangd's resolved symbol tree instead of the lexer heuristic (resolves overload signatures, qualified/aliased namespace targets and transitive `using namespace`); much slower, opt-in.
- `--clangd-all` — export every non-test file through clangd, not just the candidate directories.
- `--clangd-jobs N` — parallel clangd processes (default 4).
- `--symbols-out FILE` — write the clangd symbol index as JSON (implies `--clangd`).
- `--symbols-in FILE` — compare a previously exported clangd symbol index instead of running clangd (fast, no clangd dependency).
`scripts/clangd_symbols.py` exposes the same exporter standalone (`--out`, `--all`, `--jobs`).

Exit codes: `0` = no ERROR findings, `1` = errors found, `2` = internal failure.

---

### `clangd_symbols.py` — Export a C++ symbol / using-directive index
```bash
python scripts\clangd_symbols.py --out build\symbols.json
python scripts\clangd_symbols.py --all --jobs 8 --out build\symbols.json
python scripts\clangd_symbols.py src\graph_db\graph.cpp src\graph_db\utils.cpp
```
Starts clangd (with `--compile-commands-dir=.vscode` when available), opens each file, and requests the hierarchical `textDocument/documentSymbol` tree. The flat index contains every namespace/class-qualified function with its parameter list, plus every `using namespace` with its exact lexical scope and resolved target namespace. It is the data source for `detect_unity_build.py --clangd`; flags: `--root`, `--clangd`, `--compile-commands-dir`, `--out`, `--all`, `--jobs`, `--quiet`. Exit codes: `0` = index written, `1` = clangd/parse failure, `2` = bad input.
---
### `debugger.py` — Lightweight Windows crash debugger (x64 only)

Launches a native x64 executable under the Windows Debug API, waits for a crash, and prints a symbolic stack trace by reading PDB files via DbgHelp.dll.

```bash
python scripts\debugger.py myapp.exe
python scripts\debugger.py myapp.exe C:\pdb\search\path
python scripts\debugger.py myapp.exe -- arg1 arg2
```

**Arguments:**
1. `path_to_exe` — native x64 executable to debug.
2. `pdb_search_path` — optional directory for PDB search (defaults to EXE's directory).
3. `--` — separator; everything after is forwarded to the target.

**How it works:**
- Launches the target as a debugged process (`DEBUG_PROCESS`).
- Initialises the DbgHelp symbol engine with the Microsoft symbol server (`SRV*C:\Symbols*https://msdl.microsoft.com/download/symbols`).
- On every `LOAD_DLL_DEBUG_EVENT` and `CREATE_PROCESS_DEBUG_EVENT`, it calls `SymLoadModule64` so symbols are available.
- On an exception, it calls `OpenThread` + `GetThreadContext` + `StackWalk64` to walk the stack, resolving function names and file:line via `SymFromAddr` / `SymGetLineFromAddr64`.
- First-chance exceptions are passed to the target (`DBG_EXCEPTION_NOT_HANDLED`); second-chance (unhandled) exceptions cause the debugger to print the trace and exit.

---

### `fetch_sqlite_amalgamation.py` — Install the official SQLite amalgamation

Downloads the official SQLite amalgamation from sqlite.org and installs
`sqlite3.c` + `sqlite3.h` into `src/ext/sqlite_amalgamation/`, rewriting the
`VERSION.txt` pin next to them. This is the whole SQLite bump procedure: the
repository no longer vendors the SQLite Fossil source tree, and
`sqlite_xmake.lua` compiles just the amalgamation into `kimix-sqlite3` (FTS5
enabled). It replaces the removed `scripts/gen_sqlite_amalgamation.sh` (which
needed bash + tclsh + a C compiler); this script needs only Python 3 and network
access.

```bash
python scripts\fetch_sqlite_amalgamation.py            # the pinned VERSION.txt
python scripts\fetch_sqlite_amalgamation.py --version 3.54.0
python scripts\fetch_sqlite_amalgamation.py --dry-run  # resolve only, no writes
```

Flags:
- `--version X.Y.Z` — version to install (default: `src/ext/sqlite_amalgamation/VERSION.txt`).
- `--url <zip-url-or-path>` — explicit amalgamation ZIP (http(s) URL or local file); use for offline bumps.
- `--year YYYY` — calendar year of the sqlite.org archive (only used when the download page cannot be parsed).
- `--sha3 <hex>` — expected SHA3-256 of the resulting `sqlite3.c`.
- `--dry-run` — resolve and report the download without downloading or writing.
- `--keep-temp` — keep the temporary download directory.

The install fails closed: it rejects an archive whose `sqlite3.h` does not define
`SQLITE_VERSION "<version>"`, whose `sqlite3.c` lacks the amalgamation banner or
the FTS5 sources (`sqlite3Fts5Init`), or whose `--sha3` does not match. Then
reconfigure (`xmake f -c`), rebuild and run the SQLite suites.

---

### `gen_bash_fix_data.py` — Regenerate the runtime BASH_FIX tables and the bash RTK goldens

Re-derives the data still compiled in from kimi-agent's pure-Python reference
(`bin/kimix_native/_shell_compat.py` / `src/kimix/tools/common.py`), so nothing
is transcribed by hand. The C++ Windows Git Bash compatibility fix that used to
live in `src/builtin_tools/bash_tool.cpp` has been removed — the bash tool hands
the command to Git Bash as written (only the `export MSYSTEM=; ` neutralization
survives) — so the `--tables` / `--goldens` modes and the
`GENERATED:BASH-FIX-DATA` region are gone with it.

```bash
python scripts\gen_bash_fix_data.py --all
```

Flags:
- `--tables-runtime` (alias `--parse-tables`) — rewrite the `GENERATED:BASH-FIX-PARSE-DATA` region of `src/runtime/parse/shell_scanner.cpp` (fallback names, fallback command wrappers, unsupported names) plus `tests/unit/native/shell_scanner_names_goldens.inc`.
- `--rtk` — rewrite `tests/unit/builtin_tools/bash_rtk_goldens.inc` (the RTK rewrite scanner vectors).
- `--all` — both of the above.
- `--reference` / `--reference-common` — path overrides (default: the kimi-agent checkout under `C:/dev/kimi-agent`).

Goldens use a fixed Windows temp directory (`C:/Temp`) so they do not depend on the machine that generated them.

---

### `py_lint.py` — Python syntax check & optional execution

Runs `py_compile` on a target file and optionally executes it if the syntax check passes.

```bash
python scripts\py_lint.py myscript.py          # syntax check only
python scripts\py_lint.py myscript.py --exec   # syntax check + execute
```

**Flags:**
- `target_file` — Python file to check. Can be absolute or relative to the project root.
- `--exec` / `-e` — after successful syntax check, run the file via `python <file>`.

Resolution: always resolved against the project root (parent of `scripts/`). Absolute paths outside the project are rejected.

---

### `pull_latest.py` — Pull main repo + all submodules to latest

```bash
python scripts\pull_latest.py
```

**What it does:**
1. `git pull --rebase --autostash origin <current-branch>` on the main repo (retries 3x on network error).
2. Reads `.gitmodules` and updates every submodule to the latest commit on its branch.

**Per-submodule behaviour:**
- **Not cloned** → `git clone <url> <path>` (retries 3x).
- **Directory exists but not a git repo** → removed (via `shutil.rmtree`) and re-cloned.
- **Valid git repo, on a branch** → `git fetch origin <branch>`, then `git pull --rebase origin <branch>`; if pull fails → `git reset --hard origin/<branch>`.
- **Detached HEAD** → resolves target branch from `.gitmodules` or remote HEAD, checks it out, then fetches and pulls.
- **All git operations** retry up to 3 times with exponential backoff (3s, 6s).
- **Nested submodules** → after the clone/pull, `git submodule update --init --recursive` in the submodule (src/ext/lua-language-server carries its own `3rd/*`, which its xmake target needs).

**Exit codes:** `0` = all ok, `1` = any failure.

---

### `update_submodule.py` — Clone or pull all git submodules

Reads `.gitmodules` and clones missing submodules or pulls the latest commits on their current branch.

```bash
python scripts\update_submodule.py
```

**Per-submodule behaviour:**
- **Not cloned** → `git clone` the configured URL + branch.
- **Cloned** → detect current branch, `git fetch origin <branch>`, then `git pull --rebase origin <branch>`.
- **Detached HEAD** → checkout the branch from `.gitmodules` (or the remote default via `git remote show origin`), then pull.
- **Pull / rebase fails** → fallback to `git reset --hard origin/<branch>`.
- **Nested submodules** → after the clone/pull, `git submodule update --init --recursive` in the submodule (src/ext/lua-language-server carries its own `3rd/*`, which its xmake target needs).

---

# Project Structure

```
bootstrap.py        # cross-platform xmake bootstrap (toolchain detect, configure, build, --test)
publish.py          # build + package release ZIPs (version read from version.txt)
xmake.lua           # build config: options, kimix-core / runtime_py / kimix-test targets
version.txt         # single version source, X.Y.Z
scripts/            # dev tools (check_*_syntax, debugger, pull_latest, ... see above)
src/ # C++ sources
  core/ # kimix-core static lib (namespace kimix)
  api/ # kimix_api shared lib: the plain-C FFI surface -> bin/<mode>/kimix_api.dll
  script/ # kimix-script static lib: the Lua script sandbox (vetted env, budgets, DENIED)
  runtime/ # runtime kernels + pybind11 bindings -> runtime_py.pyd
  ext/ # vendored deps (mimalloc, xxhash, yyjson, pybind11, reproc, marl, mbedtls, sqlite_amalgamation, lua, lua-language-server)
  test/ # kimix-test binary (add_tests "basic")
tests/ # C++ tests, Boost.UT only (vendored at tests/ut/ut.hpp)
  unit/ # core/ api/ ext/ native/ test executables, registered in tests/xmake.lua
  verify_workspace_parity.py
python/             # Python layer
  kimix_native/     #   pure-Python shim package over runtime_py
  tests/            #   pytest suite; conftest.py puts bin/<mode> + python/ on sys.path
```

# Testing

- **C++ tests** — Boost.UT executables under `tests/unit/<area>/` (14 areas: core, api, ext, llm, openai, openai_responses, anthropic, kimi, native, tools, builtin_tools, cli, agent, script), each registered via `test_proj(name, source, callable)` (or `builtin_tools_test(name, source)`) in `tests/xmake.lua`. Keep test logic in `main()` scope, never file-scope static lambdas (see test skill). The Boost.UT name filter accepts an **exact name / `?` wildcard only** — `"add*"` skips everything and still exits 0.
  ```bash
  python bootstrap.py --test # build + run the kimix-test smoke binary (NOT the suites)
  xmake f -m debug -c -y && xmake build
  xmake run test_kimix_core # run one test binary
  xmake test test_kimix_core/* # run it through xmake's test runner
  ./bin/debug/test_kimix_core.exe add_basic # filter by EXACT test name (see test skill)
  ```
- **Python tests** — pytest under `python/tests/`; needs a built `runtime_py.pyd` in `bin/<mode>`.
  ```bash
  python -m pytest python/tests -q
  ```
- **Lua suite** — the vendored `lua-language-server` (src/ext/lua-language-server) ships its own Lua test corpus (27 suites under `test/`). `lua-language-server-tests` builds the server binary and runs it; ~46 s on Windows / ~55 s in WSL, so it is registered in `xmake test` only with `--kimix_slow_tests=y`.
  ```bash
  xmake build lua-language-server # the server binary -> src/ext/lua-language-server/bin/
  xmake run lua-language-server-tests # whole suite
  xmake run lua-language-server-tests "-n=^basic$" # one suite (test.lua's -n= filter)
  ```
- Always `git diff <file>` to verify changes after done.

# When & How to Use Skills

Read the matching project skill before the task (all in `.agents/skills/`):

| Skill | Use before |
|---|---|
| `xmake` | configuring/building/running any C++ target (bootstrap.py delegates to xmake) |
| `cpp` | writing/editing C++ code (namespace kimix, STL wrappers, allocators, core API) |
| `test` | writing or adding a test case (Boost.UT layout, templates, xmake registration) |
| `debug` | debugging crashes/failures (stack traces, stderr logging, buffer inspection) |
| `pybind` | writing/editing the Python bindings in `src/runtime/py/` (pybind11 3.x, GIL, casts) |
| `reproc` | anything that spawns a process (`src/builtin_tools/process_runner.*` wraps it; nothing else may) |
| `fiber` | anything that runs work on several threads (`kimix::fiber` scheduler, `schedule`/`async`, `parallel`/`async_parallel`, events/counters/futures) or touches `src/ext/marl` / the `kimix-marl` target |
| `yyjson` | parsing/building JSON (`kYYJsonAlcMi` mimalloc allocator, read/write opts) |

Agent-side built-ins (`kimix_api`, `skill-creator`) apply only when their topic comes up.

# Version, Build & Publish

- **Version** — edit `version.txt` in the project root only (must match `X.Y.Z`). Nothing else hard-codes it: xmake regenerates `build/gen/kimix_version.h` at build time; the Python shim and tests read it directly. Bumping the version = editing `version.txt` only.
- **Build** — `python bootstrap.py` (add `--debug`, `--toolchain <name>`, `--test`, `--clean`, `--jobs N`).
- **Feature switches** — the `kimix_enable_*` xmake options (all default on) decide which targets a configuration contains: `kimix_enable_tests` (every `tests/unit/**` Boost.UT target, `kimix-test`), `kimix_enable_llm` (`kimix-llm`: `src/llm` + `src/agent` + `src/builtin_tools` + `src/mcp`), `kimix_enable_cli` (`kimix-cli` + `kimix_cli`) and `kimix_enable_runtime` (`runtime_py`) — the last two also need `kimix_enable_llm`. Turn one off with `xmake f --kimix_enable_llm=false`; a target that links a disabled target is disabled with it (the `kimix_feature_gate` rule in `scripts/xmake_func.lua` reads each target's own `add_deps()` list, so no target needs a per-option `if`). `kimix-core` and the vendored `src/ext` libraries are inputs, not dependents, so they stay built, and `xmake build <skipped-target>` is a silent no-op. Every combination still has to `xmake f` + `xmake` cleanly with no warnings.
- **File-level skip** — `kimix_enable_api` (`kimix_api`, `src/api`) uses the other mechanism: `src/xmake.lua` only `includes("api")` when the option is on, so the target and its headers are not in the configuration at all (`xmake build kimix_api` then reports "not a valid target name" rather than being a no-op), and `tests/xmake.lua` repeats the same guard for `test_kimix_api` because a dep on a target that was never declared cannot be gated.
- **ASan leg switch** — `kimix_mi_track_asan` (root `xmake.lua`, default **off**) compiles the vendored mimalloc with `MI_TRACK_ASAN=1` (private define on the `mimalloc` target in `src/ext/xmake.lua`) so an `--policies=build.sanitizer.address` build can see the project's real heap: mimalloc is `MI_WIN_NOREDIRECT` and only our own `operator new` is redirected, so without it ASan sees stack/globals/CRT memory only. Off by default because it only makes sense with ASan and costs per-allocation tracking; it exists for the memory-safety legs, together with `--lua_arena` the only memory-diagnostic build switches. Its LEAK counterpart is `kimix_memory_strict` (root `xmake.lua`, default **off**): a debug-only per-allocation tracker inside `src/core/memory.cpp` that reports the blocks still live at process exit, which needs no sanitizer runtime and therefore also works on the MSVC lane where `detect_leaks=1` is rejected (`F-ASAN-1`). The TSan leg (`build.sanitizer.thread`) is Linux/clang only and is **not** run on the Windows/MSVC lane (follow-up `F-TSAN-1`), leak detection is unavailable on Windows ASan (`detect_leaks=1` is rejected — `F-ASAN-1`) and `build.sanitizer.undefined` is a silent no-op under `cl` (`F-UBSAN-1`); see the debug skill §9 for the exact messages.
- **Publish** — `python publish.py` builds release x64, packages ZIP archives and uploads them via `gh` to the `Release` tag (https://github.com/Sikao-Engine/KimiX-native/releases/tag/Release):
  ```bash
  python publish.py # all supported platforms
  python publish.py --platform windows # Windows MSVC only
  python publish.py --no-verify # skip post-build verification
  python publish.py --no-upload # skip the gh release upload step
  ```
  Output: `bin/release/kimix_base-<platform>-<arch>-<version>.zip`; Linux target builds via WSL on a Windows host. Exit codes: 0 ok, 1 build/package failed (or gh missing/unauthenticated), 2 verification failed, 3 gh upload failed.

---

# Code Index — Where to Change What

Use this index to find the files for a feature change. Layout: **feature → primary files** (+ tests + notes). All C++ paths under `src/`; headers listed without the `.cpp` twin unless it matters. Every module has a file-header comment explaining its scope — read it before editing. Deeper module docs live in `src/*/README.md`, `src/*/reports/*.md`, `src/cli/PLAN.md`.

## Fast lookup (feature → code)

| If the change is about... | Go here |
|---|---|
| A built-in tool's behavior (bash, read, edit, ...) | `src/builtin_tools/<tool>_tool.*` + shared: `tool.*`, `tool_types.*`, `utf8_util.*` |
| Which tools exist / new tool registration | `src/builtin_tools/tool_registry.*` + `tool_registry_all.cpp` |
| Agent turn loop, compaction, tool dispatch | `src/agent/soul.cpp` |
| Sub-agents (spawn, message, interrupt) | `src/builtin_tools/agent_tool.*`, `src/builtin_tools/workflow_tool.*`, runner install: `src/agent/agent_host.*` |
| LLM providers / request & streaming | `src/llm/llm.*` + `src/llm/{openai,openai_responses,anthropic}/` |
| CLI app, REPL, slash commands, rendering | `src/cli/` (entry `main.cpp`, plan in `PLAN.md`) |
| Core utilities (strings, memory, STL, json) | `src/core/` (umbrella `kimix_core.h`, STL aliases in `core/stl/`) |
| Runtime kernels exposed to Python | `src/runtime/<area>/` + pybind layer `src/runtime/py/` |
| The C FFI surface (foreign-language callers) | `src/api/` (one `ffi_<area>.h/.cpp` pair per area) + `docs/ffi.md` |
| Lua interpreter memory (arena, trim, table presize/shrink) | `src/ext/lua/larena.c` (the arena) + `docs/lua_memory_arena.md`; build switch `--lua_arena`, exported diff `src/ext/lua.kimix.patch`, gates `test_lua_arena` / `test_lua_table_mem` (both also under `KIMIX_LUA_ARENA_DRYRUN=1`) / `bench_lua_mem` / `lua-tests` + `tests/unit/ext/lua_e2e/run_testes.py` |
| Leak hunting (per-allocation tracker, works on MSVC) | build switch `--kimix_memory_strict` (root `xmake.lua`, default off) → public `KIMIX_MEMORY_STRICT` on `kimix-core` (`src/xmake.lua`) → the tracker in `src/core/memory.cpp`; `xmake f -m debug --kimix_memory_strict=true -y`, then read the exit-time stderr report (`[kimix][error] memory: leak check: ...`). Debug only, off = compiled out; see the test skill's "Allocation Tracking" |
| Sanitizer / ASan runs (mimalloc heap visible to ASan) | build switch `--kimix_mi_track_asan` (root `xmake.lua`, default off) → `MI_TRACK_ASAN=1` on the `mimalloc` target (`src/ext/xmake.lua`, private define); `xmake f -m debug --policies=build.sanitizer.address --kimix_mi_track_asan=true -y`. TSan is Linux/clang only, not on the Windows/MSVC lane (`F-TSAN-1`) |
| What a script may do (sandbox capabilities, tripwires, budgets) | `src/script/script_env.cpp` (allow/replace/deny), `script_alloc.cpp` + `script_hook.cpp` (budgets), `script_sticky.cpp` (abort semantics) + `src/script/README.md` |
| What a script may CALL OUT to (host functions, globals, a named call) | `src/script/script_host.cpp` (`bind_host_function`/`set_global`/`clear_globals`/the trampoline), `script_run.cpp` (`run(RunSpec)`, the named call), `script_value.cpp` (the strict table reader); C ABI `kimix_lua_host_bind` & co., README §7 |
| Python-side shim / parity fallback | `python/kimix_native/` |
| Build wiring of any of the above | `src/xmake.lua` (targets), `tests/xmake.lua` (test targets) |

## `src/core/` — kimix-core static lib (namespace `kimix`)
Base library everything links; deps on `mimalloc`, `xxhash`, `yyjson`, `pybind11`, `marl` only.
- `kimix_core.h` — umbrella header; start here. `pch.h` — precompiled header.
- `core/stl/` — STL wrappers (`kimix::string/vector/unordered_map/...`, allocators over mimalloc, `format.h`, `lru_cache.h`, `unordered_dense.h`, `filesystem.h`). Never use `std::string`/`std::vector` in kimix APIs.
- `basic_types.*`, `basic_traits.h`, `concepts.h` — fundamental types/traits.
- `memory.*` — the `kimix::allocate`/`deallocate` gateway (the one mimalloc entry point) plus the debug-only (`KIMIX_MEMORY_STRICT`) leak tracker; `pool.*`, `first_fit.*`, `string_scratch.*` — pools, region allocator & scratch buffers.
- `binary_io.*`, `binary_file_stream.h` — file I/O: read-only `kimix::BinaryFileStream` (header-inline) plus the shader-source/cache `BinaryIO`. There is no core writer — a consumer writes through the CRT (`FILE*`, `std::ofstream`) or `DefaultBinaryIO`, using 64-bit positioned I/O (`_fseeki64`/`fseeko`, `_commit`/`fsync`). `platform.*`, `clock.h`, `constants.h`, `mathematics.h`.
- `json_repair.*` — repairs malformed LLM tool-call JSON (used by soul dispatch).
- `dynamic_module.*`, `dll_export.h` — symbol export/module loading.
- `spin_mutex.h`, `thread_safety.h`, `rbc_concurrent_queue.h`, `detail/concurrent_queue.h` — threading primitives.
- `fiber.h` + `fiber_future.h` + `shared_function.h` — `kimix::fiber`: fibers over the vendored `marl` (`kimix-marl` target): `scheduler`/`shared_scheduler` (the root main binds the shared pool once), `schedule`/`schedule_background`/`async`, `event`/`counter`/`mutex`/`condition_variable`/`Future<T>`/`sleep_for`/`blocking_call`, `parallel`/`async_parallel` (job ids or iterator ranges, with an `internal_jobs` claim size and a `task_limit` concurrency cap), `kimix_fiber_defer`. The `parallel()` forms run inline on a thread with no scheduler bound. Not in the umbrella — include `<core/fiber.h>` and see the `fiber` skill.
- Header-only (no `.cpp`): traits/concepts/clock/constants/mathematics, most of `core/stl/`.

## `src/api/` — kimix_api: the plain-C FFI shared library

`kimix_api` (`set_kind("shared")`) puts a C ABI on top of `kimix-core` for foreign callers (C, C#, Zig, Rust, Python ctypes/cffi, Julia, Node-ffi). Depends on `kimix-core` only; `xmake build kimix_api` -> `bin/<mode>/kimix_api.dll` / `libkimix_api.so`. The full documented index is `docs/ffi.md`.
- One `ffi_<area>.{h,cpp}` pair per area plus the umbrella `kimix_api.h`: `ffi_common` (linkage macro from `core/dll_export.h`, `kimix_status`, ABI/version/layout queries), `ffi_mem` (the mimalloc allocation functions, `kimix_mem_*`), `ffi_vec` (`kimix::vector<std::byte>` as an inline caller-owned placeholder, `kimix_vec_*`), `ffi_yyjson` (the `yyjson_*` JSON surface with the mimalloc allocator baked in -- no `yyjson_alc` in the surface, `kimix_yyjson_*`), `ffi_repair` (`kimix::repair()` from `core/json_repair.h`, results delivered into a `kimix_vec`), `ffi_map` (an opaque `uint64` -> `uint64` map handle, `kimix_map_*`), `ffi_parallel` (`kimix::fiber` job fan-out, `kimix_fiber_*`), `ffi_canonical` (deterministic canonical JSON, `kimix_canonical_json*`), `ffi_sqlite` (the SQLite3 C API over the vendored amalgamation, `kimix_sqlite_*`), `ffi_graphdb` (the kimix-graph-db C ABI re-export, `kimix_graphdb_*`), `ffi_lua` (the Lua script sandbox: run/result accessors plus the HOST surface -- `kimix_lua_host_bind`/`_global_set`/`_globals_clear`/`_run_call`/`_last_host_calls`, with `kimix_lua_value` 48 B and `kimix_lua_limits` 96 B since ABI 3, `kimix_lua_*`). `kimix_api` has NO file read/write area: the file I/O capability lives in `src/core` (read side) and in the consuming module (write side); a managed caller (the C# side) implements it without the FFI.
- The public headers are pure 7-bit ASCII and valid C99/C11 *and* C++: opaque typedefs, fixed-size POD structs, `size_t`/`<stdint.h>`/1-byte `bool`, no exceptions/RTTI across the boundary, every fallible call returns `kimix_status`.
- `ffi_vec.h`'s `kimix_vec` is the placement-new / `std::launder` pattern: fixed `KIMIX_VEC_BYTES` x `KIMIX_VEC_ALIGN` inline storage (sized for every STL and iterator-debug level, frozen by `static_assert`s in `ffi_vec.cpp`) plus a guard word in the tail, so "init twice", "use before init" and "use after destroy" return `KIMIX_ERR_INVALID_STATE` instead of corrupting memory.
- One heap: everything the FFI allocates is mimalloc memory *inside this DLL* and must be released by the matching `kimix_mem_free` / `kimix_vec_destroy` / `kimix_vec_free` / `kimix_yyjson_str_free` / `*_doc_free`.
- Tests: `tests/unit/api/test_kimix_api.cpp` (includes only the C headers), `test_kimix_canonical.cpp`, `test_ffi_lua.cpp` (the Lua script sandbox C ABI: layouts + the mimalloc-owned handle + the whole host surface). Gated by `kimix_enable_api`, see *File-level skip* under Version, Build & Publish.


## `src/script/` — the Lua script sandbox (target `kimix-script`, namespace `kimix::script`)
One throw-away, budgeted `lua_State` per run; the environment holds only base (minus file/process/bytecode), `string`, `table`, `math`, `utf8`. **Read `src/script/README.md` first** — five layers, capability matrix, message format, the longjmp rule for writing natives. Depends on `kimix-core` and the vendored `lua` target (the one third-party dep a module declares directly, like `src/api`'s `kimix-sqlite3`).
- `script_sandbox.h` — public surface: `Sandbox` (`run`/`run(RunSpec)`/`bind_host_function`/`set_global`/`clear_globals`/`add_native`/`allow_coroutines`/`cancel`/`busy`), `Limits` (incl. `max_host_calls`/`max_host_args`/`max_host_ret_values`), `Result`, `HostCall`/`HostFn`. `script_status.h` — `Status`, the message prefixes, `Violation`. `script_value.h` — the only value that crosses the boundary (no pointers; a table becomes canonical JSON in both directions, through the strict reader). `script_host.*` — the host bridge: the trampoline, `Value` → stack, the reserved-name rule (`sb_env_name_reserved`).
- `script_state.*` — `SandboxState` + state lifetime (fixed seed, `lua_atpanic`, `lua_setwarnf`, extraspace) and the panic/warning handlers. `script_alloc.*` — the budgeted `lua_Alloc` (memory cap, single-allocation cap, sticky OOM). `script_hook.cpp` — the count hook (deadline, instruction budget, cancel).
- `script_env.*` — `_ENV`: allow list, replacements (`load`, `pcall`/`xpcall`, `print`/`warn`, `tostring`, `collectgarbage`, `string.format`, `coroutine.resume`/`close`), the loud tripwires (`io`, `os`, `package`, `debug`, `require`, `dofile`, `loadfile`, `loadlib`, `string.dump`), the redaction converter and the env integrity post-check. `script_error.*` — the one message format + the raise helpers. `script_sticky.cpp` — first-violation-wins and the re-raise every catcher performs. `script_run.cpp` — THE single `lua_pcallk` boundary, the message handler, the run flow. `script_lua.h` — the module's only Lua include (`extern "C"`).
- Rules: every `lua_CFunction` is POD-only and raises at the end; scratch buffers live in `SandboxState`; no `lua_State*` escapes the module; `scripts/check_lua_sandbox.py` fails the tree on an `luaL_openlibs`, an `luaL_newstate`, a raw `lua_newstate` or a `luaopen_io|os|debug|package|loadlib` outside `src/script/`, `src/ext/` and `tests/unit/ext/test_lua.cpp`.
- C ABI: `<api/ffi_lua.h>` (`kimix_lua_*`, ABI 3) in `kimix_api`, documented in `docs/ffi.md` — including the HOST surface (`kimix_lua_host_bind`/`_global_set`/`_globals_clear`/`_run_call`/`_last_host_calls`), the C twin of `bind_host_function`/`set_global`/`run(RunSpec)` for callers that never see a `lua_State`.
- Tests: `tests/unit/script/test_script_sandbox.cpp`, `test_script_hostile.cpp` (17 forbidden APIs x 7 wrappers), `test_script_surface.cpp` (golden capability allowlist), `test_script_host.cpp` (the host surface: budgets, tables both ways, reserved names, `run(RunSpec)`), `test_script_bench.cpp` (`KIMIX_SCRIPT_BENCH=1`).

## `src/llm/` — LLM providers (namespace `kimix::llm`)
- `llm.*` — unified facade; `config.type` picks provider (`openai`|`openai_legacy` → `openai/`, `openai_responses` → `openai_responses/`, `anthropic` → `anthropic/`). Unified `ToolCall`/`Tool`/`Message` types live here.
- `common.*` — shared provider types. `stream_filter.h` — streaming content filtering. `http_client.h/.cpp` — the hand-written kimix::net HTTP(S) client (raw sockets + vendored mbedTLS). `yyjson_alc.h` — JSON allocator glue.
- `openai/openai_chat.*` + `sse_parser.h` — OpenAI Chat Completions.
- `openai_responses/responses_chat.*` + `stream_parser.h` — OpenAI Responses API.
- `anthropic/anthropic_chat.*` + `stream_parser.h` — Anthropic Messages (thinking blocks round-trip).
- Tests: `tests/unit/llm/*`, `tests/unit/openai/`, `tests/unit/openai_responses/`, `tests/unit/anthropic/`.

## `src/builtin_tools/` — agent built-in tools (namespace `kimix::builtin_tools`)
One `<name>_tool.h/.cpp` pair per tool, registered by key in the static registry. **Read `src/builtin_tools/README.md` first** — unity-build rules, alias tables, `Tool::valid()` contract, subprocess rules.
- Registry: `tool_registry.*` (static-constructor registry + `ToolMeta`), `tool_registry_all.cpp` (all registrations; edit to add/remove a tool), `tool.*` (Tool base + `ToolParams` + alias matching), `tool_types.*` (status/error enums + shared output utils), `tool_schema_validate.*` (schema validation), `utf8_util.*` (UTF-8 helpers), `regex_lite.*` (regex engine).
- Process spawning: `process_runner.*` — THE only layer allowed to spawn processes (wraps reproc); used by bash, pwsh, python, job_output, workflow, CLI `/cmd`.
- Tools: `bash_tool.*` (Git Bash; the command reaches the shell as written — the old Windows compat-fix port is gone, only the `MSYSTEM` neutralization remains), `pwsh_tool.*` (PowerShell; fallback when bash missing), `python_tool.*` + `python_tool_class.*` + `python_code_session.*` (REPL sessions), `read_tool.*`, `write_tool.*`, `edit_tool.*` (hashline edits), `glob_tool.*`, `grep_tool.*`, `read_image_tool.*`, `fetch_url_tool.*` + `http_fetch.*` (curl-style fetch), `web_search_tool.*`, `retrieve_tool.*` (history search), `compact_tool.*`, `todo_tool.*`, `plan_tool.*` (writeplan/readplan/editplan), `job_output_tool.*` (background job reading), `agent_tool.*` (subagent spawn), `workflow_tool.*` (multi-agent workflow), `context_prune_tool.*` (prune_N).
- Cross-cutting gates live in `src/agent/` (approval, verification_gate, tool_loop_guard) — a tool behavior gated at turn level is there, not here.
- Tests: `tests/unit/builtin_tools/test_<tool>_tool.cpp` (+ `test_tool*.cpp`, `test_param_aliases.cpp`, `test_tool_valid.cpp`, `test_process_runner.cpp`). Goldens: `tests/unit/builtin_tools/bash_rtk_goldens.inc` (regen with `scripts/gen_bash_fix_data.py --rtk`).

## `src/agent/` — agent soul & turn loop (namespace `kimix::agent`)
- `soul.*` — KimiSoul: session + turn loop + tool dispatch + compaction. The hub; most behavior hangs off it.
- Turn resilience: `errors.h` (typed failure taxonomy), `step_retry.*` (rate-limit retry), `context_overflow.*` (window overflow → force compact), `token_ledger.*` (provider token accounting), `loop_control.h` (LoopControl config knobs), `tool_loop_guard.*` (repeated-call detectors), `tool_name_resolver.*` (hallucinated tool-name recovery), `tool_argument_repair.*` (arg anti-hallucination), `tool_errors.*` (typed tool errors), `verification_gate.*` (finish-gate nudges), `cancel.h` (cancellation token), `steer.*` (mid-stream steering), `btw.*` (/btw side questions).
- Compaction/context: `compaction_ledger.*` (JSONL transaction ledger), `context_db.*` (SQLite context store + migration), `context_pruning.*` (ContextPruner engine), `auto_retrieve.*` (step-1 memory injection), `dynamic_injection.*` + `dynamic_injections/` (per-step <system-reminder> providers: budget/compact/todo reminders, context meter, target churn).
- Integration: `system_prompt.*` (system prompt builder, byte-faithful), `approval.*` (approval gate), `hooks_engine.*` (lifecycle hooks), `wire.*` (wire.jsonl event stream), `llm_recorder.*` (request traces), `agent_host.*` (production sub-agent runner), `tool_taxonomy.h` — file-editing / shell-tool classification used by gates.

## `src/mcp/` — MCP stdio client
- `mcp_client.*` — bridges external MCP servers' tools into the ToolRegistry as runtime external tools (F7).

## `src/cli/` — native CLI (targets `kimix-cli` static lib + `kimix_cli` exe; entry `main.cpp`)
**Plan & spec: `src/cli/PLAN.md`.** Port of kimi-agent's Python CLI.
- `cli_args.*` — arg parsing. `cli_config.*` — provider/agent config load. `cli_app.*` — application wiring (config → agent, one-turn, --dry-run).
- `cli_repl.*` — interactive REPL. `cli_commands.*` — slash-command table. `cli_session.*` — session store. `cli_skills.*` — skill discovery for the prompt.
- `cli_print.*` — terminal printing. `cli_markdown.*` — ANSI markdown renderer. `cli_stream.*` — streaming renderer. `cli_signal.*` — Ctrl-C handling. `cli_tools.*` — agent-manifest tool paths. `cli_common.*` — shared helpers.
- Tests: `tests/unit/cli/test_cli*.cpp`, `test_media_session_roundtrip.cpp`.

## `src/runtime/` — runtime kernels (namespace `kimix::runtime`) → `runtime_py.pyd`
Pure kernels + pybind11 bindings. **Ownership split (see `src/xmake.lua`):** `shell_scanner`, `shell_safety`, `compress`, `ansi`, history-index kernels (`history_index`, `inverted_index`, `ngram_tokenizer`, `sqlite_history_index`), `bm25`, `fuzzy`, `distance`, `utf8`, `sanitize`, `export_builder` are compiled into **kimix-llm** (used by builtin tools/agent) and *removed* from runtime_py to avoid duplicate symbols. Everything else lives only in runtime_py.
- `py/` — pybind11 binding layer (module `runtime_py`; `module.cpp` is the entry, one `py_<area>.cpp` per kernel area, `py_soul_bridge.h` for GIL/span bridging). Python-visible API changes go here. Exceptions are allowed ONLY in this layer.
- `codec/` — framing: `frame_writer` (JSONL frames), `recv_buffer`, `merge_buffer`, `args_buffer`, `sse` (SSE parsing), `wire_envelope`.
- `common/` — `gil.h`, `json_pretty.h`, `text_util.h`, `utf8.*`.
- `diff/` — `diff_engine` (line-level diff opcodes). `glob/gitignore.*` — gitignore-style matching (fnmatch semantics, case rules per platform).
- `index/` — retrieval indexes: `inverted_index`, `ngram_tokenizer` (CJK-aware), `history_index` (in-memory KNHIX1), `sqlite_history_index` (FTS5), `fts5_query.h` (input capping).
- `parse/` — scanners: `comment_scanner` (per-language comments/strings), `shell_scanner` (bash code/quote bitmap).
- `print/print_stream.*` — queued print stream. `stream/` — `ansi.*` (ANSI strip), `line_processor.*` (CRLF normalize + filter_output).
- `search/` — `bm25`, `distance` (Damerau-Levenshtein), `fuzzy` (expansion), `rerank` (MMR), `hash_kernels` (SimHash/minhash).
- `soul/` — `message_view.h`, `soul_util.h` — message/checkpoint helpers shared with the agent layer.
- `text/` — `sanitize.*` (tokenizer sanitize pipeline), `token_count.*` (code-point counts).
- `tools/` — tool kernels: `compress` (micro-compress), `export_builder` (session export markdown), `find_str`, `grep_pattern` + `grep_scan`, `line_hash`, `security` (child env / bounded output), `shell_safety` (hardline command detectors).
- Tests: `tests/unit/native/` (kernels via pyd), `tests/unit/tools/` (compress), plus parity tests in `python/tests/test_parity_*.py`.

## `src/ext/` — vendored third-party (DO NOT EDIT)
`mbedtls`, `mimalloc`, `pybind11`, `reproc`, `marl` (fibers/scheduler, submodule `LuisaGroup/marl`, built as `kimix-marl`), `sqlite_amalgamation/` — the official SQLite amalgamation (`sqlite_xmake.lua` builds `kimix-sqlite3` with FTS5); SQLite is **not** vendored as a source submodule — `xxHash`, `yyjson`. HTTP is not vendored: the hand-written `llm/http_client.h` (kimix::net, raw sockets + mbedTLS) serves the LLM providers and the fetch/web_search tools. Rule: never modify; if a needed lib is missing, write `issue/<topic>.md` instead of vendoring ad hoc. Per-target dep wiring is in `src/xmake.lua` (all third-party deps declared on `kimix-core` / `kimix-llm`; never depend on a third-party target directly).

## `python/kimix_native/` — Python shim (pure Python, fallback parity)
Loads `runtime_py.pyd` lazily; env toggles `KIMIX_NATIVE` / `KIMIX_NATIVE_<KERNEL>` (`__init__.py::use_native`). One module per kernel area mirroring the C++ kernels: `text.py`, `codec.py`, `diff.py`, `glob.py`, `index.py`, `parse.py`, `search.py`, `stream.py`, `tools.py`. Compat shims: `_parse_compat.py`, `_shell_compat.py` (the pure-Python reference behind the runtime BASH_FIX scanner in `src/runtime/parse/shell_scanner.cpp`). Changing a kernel = change C++ kernel **and** this fallback, keeping bit-identical behavior (parity tests: `python/tests/test_parity_*.py`, `python/tests/_parity_ref.py`).

## Tests map
- C++ (Boost.UT, vendored `tests/ut/ut.hpp`): `tests/unit/{core,api,ext,llm,openai,openai_responses,anthropic,native,tools,builtin_tools,cli,agent,script}/test_*.cpp`; registered via `test_proj(...)` in `tests/xmake.lua` (`test_kimix_api` and `test_ffi_lua` additionally behind `has_config("kimix_enable_api")`). `tests/unit/native/{bench_util,soul_test_util}.h` shared helpers.
- Python (pytest): `python/tests/` — kernels (`test_<kernel>.py`), parity (`test_parity_*.py`), tools (`test_tools.py`, `test_builtin_tools.py`); `conftest.py` puts `bin/<mode>` + `python/` on sys.path.
- Workspace parity: `tests/verify_workspace_parity.py`.

## Cross-cutting "how do I..." table
| Task | Files |
|---|---|
| Add a new built-in tool | `src/builtin_tools/<name>_tool.{h,cpp}` + register in `tool_registry_all.cpp` + `test_proj` in `tests/xmake.lua` + report in `src/builtin_tools/reports/<name>.md` |
| Add a param alias / fix arg parsing | alias table at the tool's `parse_*_params` in `<tool>_tool.cpp`; machinery in `tool.h` |
| Add a slash command | `src/cli/cli_commands.*` (+ tests `test_cli*.cpp`) |
| Add a dynamic injection / reminder | `src/agent/dynamic_injections/` (one pair per provider) |
| Add a compaction/pruning rule | `src/agent/context_pruning.*`, `compaction_ledger.*`, arbitration in `src/agent/soul.cpp` |
| Add an LLM provider or option | `src/llm/llm.*` (config type), provider dir, `stream_filter.h`; tests `tests/unit/llm/` |
| Change what the system prompt contains | `src/agent/system_prompt.*`, skills list via `src/cli/cli_skills.*` |
| Change subprocess/timeout/background-job behavior | `src/builtin_tools/process_runner.*` (all tools route through it) |
| Change retrieve/history index | `src/runtime/index/*` (+ `src/agent/auto_retrieve.*`, `context_db.*` for the SQLite store) |
| Change edit/hashline semantics | `src/builtin_tools/edit_tool.*` + `src/runtime/diff/diff_engine.*` + `src/runtime/tools/line_hash.*` |
| Change bash command handling on Windows | `src/builtin_tools/bash_tool.cpp` (`bash_is_git_bash_install` / `bash_spawn_script` — the `MSYSTEM` neutralization) |
| Change tool availability gating | `Tool::valid()` in each tool + `tool_availability` override + `src/agent/soul.cpp` drop logic |
| Change version string | `version.txt` only (see Version section above) |
| Add a Python-visible kernel | `src/runtime/<area>/`, bindings in `src/runtime/py/py_<area>.cpp` (+ `module.cpp`), fallback in `python/kimix_native/<area>.py`, tests both sides |
| Add / change a C-FFI entry point | the matching `src/api/ffi_<area>.{h,cpp}` pair (+ `api/detail.h` for C++-only helpers), then `docs/ffi.md`, then `tests/unit/api/test_kimix_api.cpp` (the `kimix_lua_*` family: `test_ffi_lua.cpp`); bump `KIMIX_API_ABI_VERSION` on a breaking change |
| Build config / new target / deps | `src/xmake.lua` (main targets), `src/ext/xmake.lua` (third-party), `xmake.lua` (root options) |
