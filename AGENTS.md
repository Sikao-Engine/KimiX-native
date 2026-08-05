# Rules
- NEVER change third-party extensions, unless asked.
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
python bootstrap.py --test                   # build + run tests
python bootstrap.py --clean --debug          # clean rebuild in debug mode
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

---

## `publish.py` — Build + package release 7z archives

Builds the project in **release mode, x64**, then packages the result into a 7-Zip archive.

```bash
python publish.py                          # build + package all supported platforms
python publish.py --platform windows       # Windows MSVC only
python publish.py --platform linux         # Linux GCC only (native, or via WSL on Windows)
python publish.py --no-verify              # skip post-build verification
python publish.py --clean --jobs 8         # clean rebuild with 8 jobs
python publish.py --7z PATH                # explicit 7-Zip executable
```

**What it does:**
- **Build** — delegates to `bootstrap.py` (`--toolchain msvc` on Windows, `--toolchain gcc` on Linux). On a Windows host the Linux target is built through WSL (`wsl.exe bash <script>`); if WSL is unavailable, the Linux target is skipped with a clear error (use `--platform windows`).
- **Package** — copies `bin/release/runtime.dll` and `bin/release/runtime_py.pyd` into a clean staging dir, then archives them with 7-Zip as `kimix_base-<platform>-<arch>-<version>.7z` (e.g. `kimix_base-windows-x64-0.1.0.7z`) written next to the release artifacts in `bin/release`.
- **Version** — read from `version.txt` in the project root (must match `X.Y.Z`); `publish.py` refuses to run if it is missing or malformed.
- **Verify** — lists the archive to confirm both artifacts are present, and on Windows imports `runtime_py.pyd` (which loads `runtime.dll`) checking that the reported version contains the configured version. Disable with `--no-verify`.

**Exit codes:** `0` = all platforms built/packaged/verified, `1` = any platform failed or bad input, `2` (per-platform result) = verification failed.

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
