# Builtin tools — C++ implementation brief (shared conventions)

Target dir: `C:\dev\kimix-base\src\builtin_tools`  ·  registered in target
`kimix-llm` (`src/xmake.lua` → `add_files("builtin_tools/*.cpp")`,
`add_headerfiles("builtin_tools/*.h")`).

One `.h` + one `.cpp` per tool, named `<tool>_tool.h` / `<tool>_tool.cpp`.

## What already exists (reuse, do not re-implement)

`src/builtin_tools/tool_types.h` / `.cpp` — namespace `kimix::builtin_tools`:

- `enum class tool_status : uint8_t { ok, invalid_input, not_found, no_change,
  ambiguous, blocked, too_large, unsupported, external_library }`
- `struct tool_error { tool_status status; kimix::string message; bool failed(); }`
- `struct byte_range { uint64_t begin, end; }`
- `struct line_range { uint32_t start_line; kimix::optional<uint32_t> end_line; }`
- `struct named_value { kimix::string name, value; }`
- `truncate_line(sv, max_len, out)` · `join_with_byte_limit(span, max_bytes, out,
  &truncated, &omitted)` · `fold_lines(span, max_lines, head, tail, out, &omitted)`
  · `dedup_lines(span, min_repeats, out, &saved)` — byte-exact ports of
  `kimi-cli/src/kimi_cli/tools/file/output_utils.py`.
- Constants: `k_max_output_bytes` (100 KiB), `k_max_lines_fold` (500),
  `k_max_head_limit` (500), `k_record_cap` (500), `k_invalid_node`.

`src/builtin_tools/utf8_util.h` / `.cpp` — `is_ascii`, `decode_code_point`,
`utf8_code_point_count`, `utf8_byte_offset_of_code_point`,
`utf8_floor_boundary`, `utf8_validate`, `utf8_strict_error` (CPython
`UnicodeDecodeError` wording + 0-based offset).

`kimix-core` (`src/core`, include as `#include <core/kimix_core.h>`) gives the
mimalloc STL aliases (`kimix::string`, `kimix::vector`, `kimix::optional`,
`kimix::span`, `kimix::unordered_map`, …), `kimix::StringScratch`, `kimix::format`,
XXH3 `kimix::hash64`, `kimix::Clock`, `kimix::filesystem`.

Vendored third-party libs available to `kimix-llm` (already `add_deps`):
`kimix-cpp-httplib` (header-only, `<httplib.h>`), `kimix-mbedtls`,
`kimix-yyjson` (`<yyjson.h>`), `kimix-xxhash`, and **`kimix-reproc`**
(`#include <reproc/reproc.h>`, `#include <reproc++/reproc.hpp>`) for
subprocess spawning. If a task needs a library that is **not** in
`src/ext` (e.g. a PDF/DOCX/XLSX extractor, an HTML tokenizer library beyond
what you write by hand, PCRE2, a JPEG/PNG codec), do **not** vendor it: write a
report under `C:\dev\kimix-base\issue\<tool>.md` explaining the blocker, and
still deliver everything that *is* implementable with what we have.

## Hard rules (from `AGENTS.md` + `.agents/skills/*`)

- Read `.agents/skills/cpp/SKILL.md`, `.agents/skills/test/SKILL.md`,
  `.agents/skills/xmake/SKILL.md` before writing anything.
- `namespace kimix::builtin_tools`. Classes `CamelCase`, functions/vars
  `snake_case`, privates `_snake_case`. Fixed-width ints (`int32_t`/`uint32_t`/
  `int64_t`/`size_t`). K&R braces, 4-space indent, `int *p`.
- Use `kimix::` containers/strings — never `std::vector`/`std::string` in APIs.
  - **No RTTI**: no `dynamic_cast`, no `typeid`. Project builds
    `kimix_rtti=false`; runtime type dispatch uses an explicit tag (a virtual
    tag getter or a tag member) plus `static_cast`.
  - **No C++ exceptions**: the project builds `kimix_enable_exception=false`
    (`/EHs-c-` + `_HAS_EXCEPTIONS=0` on MSVC, `-fno-exceptions` elsewhere), so
    `throw` / `try` / `catch` are not allowed anywhere in kimix code. Kernels
    and tools report failures as data — return `tool_error` / `tool_status`,
    or a `bool` plus a message out-parameter (`ToolParams::try_deserialize`,
    `serialize(out, &error)`) instead of throwing. Two build-configured
    exceptions keep exceptions enabled: the pybind11 binding layer
    (`src/runtime/py/*.cpp`, target `runtime_py`), because pybind11 itself is
    built on exceptions and is vendored third-party code, and every Boost.UT
    test binary (`test_proj` in `tests/xmake.lua`), because `ut.hpp` invokes a
    test body only inside `#if defined(__cpp_exceptions)` — with exceptions
    disabled the suite registers its tests but never runs one ("0 asserts in
    N tests").
- **Never edit anything under `src/ext/`** (vendored third-party).
- Unity build is on for `kimix-llm` (batch 8): every `.cpp` in the dir is
  concatenated. Therefore: no file-scope `using namespace`, no anonymous
  namespace with generic names like `to_vec`/`detail`, no non-`static`,
  non-inline free functions at file scope that could collide. Put helpers in
  the tool's own namespace or mark them `static`. Header guards use
  `#pragma once` (fine under unity).
- Include paths are rooted at `src/`, so write
  `#include "builtin_tools/tool_types.h"`, `#include "llm/llm.h"`,
  `#include <core/kimix_core.h>`.
- No new xmake target, no dependency edits, no changes to `kimix-core`.
  `src/xmake.lua` and `src/ext/xmake.lua` are already wired — do not touch them
- The only build-file edit allowed is the single
  `test_proj(...)` line in `tests/xmake.lua`, inside the block appended at the
  **end of the file** between the markers
  `-- >>> BEGIN builtin_tools test registrations (per-tool lines go here) >>>`
  and `-- <<< END builtin_tools test registrations <<<`.
  **Do not commit that edit** — the integrator collects all registrations in
  one pass to avoid 11-way conflicts. Edit it only in your own worktree so you
  can build/run your test, and report the exact line in your final answer.
- File-system access: prefer keeping kernels *pure* (take bytes / metadata as
  arguments, e.g. `struct file_stat { bool is_dir; uint64_t size_bytes;
  uint64_t mtime_ns; };`, and inject existence probes as
  `kimix::function<bool(kimix::string_view)>`) so unit tests are deterministic
  and need no fixtures. `kimix::filesystem` (alias of `std::filesystem`, via
  `<core/stl/filesystem.h>`) IS available and already used by kimix-core, so a
  thin walking/stat helper is allowed when the plan demands it — just isolate
  it in one function so tests can run against a temp dir they create.

## Fuzzy alias matching of tool arguments

A tool call comes from an LLM, so an argument name may be wrong but
reasonable: the Bash tool documents `cmd` and the model sends `command`.
Every tool therefore declares, for each of its arguments, the alternate
spellings it accepts, and installs that table at its parse entry point.

`tool.h` provides the machinery:

- `struct param_alias { kimix::string_view canonical; kimix::string_view
  alternates; };` — one declaration per argument; `alternates` is a
  space/comma/`|` separated list, e.g. `{"cmd", "command cmdline shell_command"}`.
- `ToolParams::alias_map` — the recorded table (canonical name -> accepted
  alternates). It is a side-table: never serialized, never part of the JSON
  body, empty by default (an empty table makes every lookup exact).
- `ToolParams::with_aliases(params, table)` — a copy of `*params` with the
  table installed. Parse entry points hold a `const ToolParams *`, so this is
  how they get alias-aware lookups without changing any call site:

```cpp
static const kimix::builtin_tools::param_alias k_<tool>_aliases[] = {
    {"cmd", "command cmdline command_line shell_command"},
    {"timeout", "timeout_seconds timeout_sec"},
};

tool_error parse_<tool>_params(const kimix::builtin_tools::ToolParams *params,
                               <tool>_params &out) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names are
    // accepted; the canonical name always wins.
    const kimix::builtin_tools::ToolParams k_resolved =
        kimix::builtin_tools::ToolParams::with_aliases(params, k_<tool>_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    ...
```

Resolution order of `get()` / `contains()` (and therefore of every parse that
reads through them): (1) the canonical key exactly as sent, (2) the declared
alternates by exact name, (3) the declared alternates with folded comparison
(ASCII case-insensitive, `_`, `-` and ` ` ignored, so `command_line` also
matches `commandLine`/`Command-Line`). A JSON null counts as absent for (2)/(3),
the first match wins, and `get_exact()` / `contains_exact()` stay strictly
exact for callers that must not accept a stray name. Alternate names must not
overlap between two canonicals of the same table.

Tests: `tests/unit/builtin_tools/test_param_aliases.cpp` drives every tool with
alias-only parameter objects (plus `test_tool.cpp` for the ToolParams
machinery). Tables live next to the parse entry point in the tool's `.cpp`;
add fresh aliases there rather than new ad-hoc fallbacks in a parser.

## Tool validity: `Tool::valid()`

`tool.h` declares `virtual bool valid() const = 0` on the `Tool` base, so every
concrete tool answers whether it can do its job **here** — in this environment
and this session. It is a statement about preconditions, never about one call's
arguments (bad arguments stay data in the result payload):

| precondition | example |
|---|---|
| an external program is installed | `python` (no interpreter found), `bash` (no Git Bash on Windows), `pwsh` (no PowerShell host) |
| the session switched a feature off | `writeplan`/`readplan`/`editplan` need `Session::plan_enabled`; `workflow` needs `Session::swarm_enabled` (both are the C++ counterpart of the reference's `SkipThisTool`) |
| a host dependency was injected | `retrieve` needs its `HistoryIndexView`, `subagent` needs the `agent_registry::runner`, `job_output` needs `native_io` or a `TaskSource` |
| the work directory exists | the file-system tools (`read`, `write`, `edit`, `glob`, `grep`, `read_image`) share `session_work_dir_usable()` |
| nothing at all | `compact`, `fetch_url`, `web_search`, `run`, the todo/agent tools: their dependencies are linked in |

The agent (`src/agent/soul.cpp`) calls it **right after the constructor**:

- `KimiSoul::get_tool()` builds the instance, asks `valid()`, and drops a tool
  that answers false *without caching it* (so a runner installed later, or an
  interpreter installed mid-session, re-enables the tool on the next rebuild);
- `KimiSoul::tool_definitions()` therefore never sends the definition of an
  invalid tool to the LLM backend, and `execute_tool_call()` refuses one with
  `tool is not available in this environment: <name>`;
- `KimiSoul::unavailable_tools()` reports the dropped names so a host can
  explain a missing tool instead of silently hiding it.

**Shell fallback.** `bash` and `pwsh` are the agent's two shells. When Git Bash
is missing on Windows, `bash` is invalid, so `tool_definitions()` adopts `pwsh`
in its place (even if the manifest never listed it) and
`KimiSoul::effective_shell_tool()` names the same tool in the default system
prompt's `{shell_tool}` slot — the prompt and the tool list never disagree.

**Test/embedder hook.** Every implementation is written as
`tool_valid("<registry key>", <probe>)`, where `tool_valid()` first consults the
process-wide `tool_availability` override table. That is what makes the "python
is not installed" and "no Git Bash" branches testable on a machine where they
*are* installed (`tests/unit/builtin_tools/test_tool_valid.cpp`), and it lets an
embedder pin a tool off without patching the probe. Probes must not spawn
processes (existence checks only) and must not mutate the tool.

## Subprocess management (`process_runner.h`)

`bash`, `pwsh`, `python`, `run`, `job_output`, `workflow` and the CLI `/cmd`
families all spawn through ONE layer: `src/builtin_tools/process_runner.cpp`
(namespace `kimix::builtin_tools::proc`), which wraps the vendored reproc
library. Nothing else in `src/builtin_tools` may call `CreateProcess`, `popen`
or a shell directly. Rules the layer guarantees (reproc skill + README §Multithreading):

- **Lifecycle pairing.** Every successful `reproc_start` is paired with
  `reproc_wait`/`reproc_stop` AND `p = reproc_destroy(p)` (the safe idiom), on
  every return path including spawn failure.
- **One owner per handle.** A task's drain thread owns its `reproc_t` for the
  whole life of the task, including the terminate on a stop request. A thread
  asking for a stop only sets the flag, joins the owner, and only then stops
  and destroys — driving one process from two threads at once is what reproc
  documents as broken (it aborts in debug inside `pipe_destroy`).
- **Windows stream redirects.** reproc pipes are loopback sockets and MSYS
  (Git Bash and its coreutils) cannot use socket fds: `>&2` dies on dup2 and
  even a plain read fails (`cat: -: Invalid argument`). So stdout/stderr go to
  temp FILES, a foreground stdin is a temp FILE holding the payload (an empty
  file gives immediate EOF), and only an interactive task gets a real Win32
  anonymous pipe.
- **Short I/O is normal.** `reproc_write` delivers "up to size" bytes and
  answers `REPROC_EWOULDBLOCK` (note: the `REPROC_E*` constants are ALREADY
  negative on Windows), so input is fed incrementally from inside the poll loop
  and stdin is closed only once the payload is out.
- **A quiet child is adopted, not killed.** When the inactivity bound fires, the
  live child moves into the interactive task registry and `run_result::task_id`
  names it, so a "running in background" report is true and `job_output` can
  read or stop it. A timeout kill leaves `exit_code` absent.
- Tests: `tests/unit/builtin_tools/test_process_runner.cpp` (26 cases, real
  children; they skip when no bash/python is installed).

## Deliverables per tool

1. `src/builtin_tools/<tool>_tool.h` — public API, file-header comment naming
   the plan file + the Python source of truth (path + line ranges) and the
   exact contract of every function.
2. `src/builtin_tools/<tool>_tool.cpp` — implementation.
3. `tests/unit/builtin_tools/test_<tool>_tool.cpp` — Boost.UT, `main`-scope
   `"<snake_case>"_test = [] { ... };` lambdas only (never file-scope static
   registration), `#include "ut/ut.hpp"` first, header comment listing what it
   covers. Aim for real behavioural coverage of the plan's §7 test list, not
   smoke tests.
4. One `test_proj(...)` registration line between the markers in
   `tests/xmake.lua`:
   ```lua
   builtin_tools_test("test_builtin_<tool>", "unit/builtin_tools/test_<tool>_tool.cpp")
   ```
   (helper `builtin_tools_test(name, source)` is already defined there and
   links `kimix-llm`.)
5. `build/<tool>_IMPL_REPORT.md` — what you implemented, what you deliberately
   left in Python (with the plan's justification), and any deviation.
6. If blocked by a missing third-party lib: `issue/<tool>.md` with the exact
   reason, what the library buys, alternatives evaluated, and the partial
   deliverables that shipped anyway.

## Namespace per tool (MANDATORY — unity build!)

`kimix-llm` uses a **unity (jumbo) build**: all `.cpp` files in this directory
are concatenated into one translation unit. Two tools declaring the same
extern-linkage symbol (e.g. `detect_self_kill`, `parse_line_ranges`, `to_vec`)
produce a duplicate-symbol link error. Therefore every tool puts its
declarations in its own nested namespace, declared inside its own header:

| tool | namespace | files |
|---|---|---|
| glob | `kimix::builtin_tools::glob` | `glob_tool.h` / `glob_tool.cpp` |
| grep | `kimix::builtin_tools::grep` | `grep_tool.h` / `grep_tool.cpp` |
| read | `kimix::builtin_tools::read` | `read_tool.h` / `read_tool.cpp` |
| read_image | `kimix::builtin_tools::read_image` | `read_image_tool.h` / `.cpp` |
| write | `kimix::builtin_tools::write` | `write_tool.h` / `write_tool.cpp` |
| edit | `kimix::builtin_tools::edit` | `edit_tool.h` / `edit_tool.cpp` |
| bash | `kimix::builtin_tools::bash` | `bash_tool.h` / `bash_tool.cpp` |
| pwsh | `kimix::builtin_tools::pwsh` | `pwsh_tool.h` / `pwsh_tool.cpp` |
| python | `kimix::builtin_tools::python` | `python_tool.h` / `python_tool.cpp` |
| fetch_url | `kimix::builtin_tools::fetch_url` | `fetch_url_tool.h` / `.cpp` |
| web_search | `kimix::builtin_tools::web_search` | `web_search_tool.h` / `.cpp` |

Shared helpers live **only** in `tool_types.h` / `utf8_util.h` (namespace
`kimix::builtin_tools`) — already implemented and tested. Do NOT edit
`tool_types.*` / `utf8_util.*` (another agent may touch the same file); if you
need a new shared helper, keep it inside your own tool namespace. Keep your
file set to your tool's pair (plus at most one `<tool>_detail.h` of
inline
amespace-scoped helpers). Unity-safe style: no file-scope
`using namespace`, and give any anonymous-namespace helper a tool-specific
name (they are TU-local, so they are safe, but names like `to_vec` confuse
readers of the merged file).

### Cross-tool ownership (prevents two agents porting the same function)

- `detect_self_kill` (the `safety.py` self-kill guard, claimed by **both**
  `pwsh.md` §3.2 and `bash.md` §3.2) is owned by **pwsh**. The bash agent does
  not implement it.
- `truncate_lines` + `find_error_line_index` (claimed by `bash.md` §3.3 and
  `python.md` §3.5) are owned by **bash**. The python agent must not declare
  symbols with those names.
- `url_safety` kernels (claimed by `fetch_url.md` and `web_search.md`) are
  owned by **fetch_url**. `web_search` ports only `convert_base64_images_to_links`,
  `truncate_with_footer`, `make_cache_slug`, and `build_search_output`.
- `conflict_scan` (claimed by `write.md` §3.3 and `read.md`) is owned by
  **write**. `read` must not declare conflict-marker symbols.

## Build & verify (isolated worktree — never build in `C:/dev/kimix-base`)

```
cd C:/dev/kimix-base
python scripts/agent_worktree.py create <tool>      # prints C:/dev/kimix_wt/<tool>
cd C:/dev/kimix_wt/<tool>
xmake f -m debug -y -c
xmake build kimix-llm
xmake build test_builtin_<tool>
xmake run test_builtin_<tool>
```

Expect `Suite 'global': all tests passed (N asserts in M tests)`. Iterate until
green — never declare done from reading alone, never delete/skip a failing test
to force a pass. (On Windows `xmake build a b` is unsupported: build one target
per command. If PCH/unity complains about stale headers, `xmake build -r <target>`.)

When reporting back, state the worktree path, the assert/test counts, and the
list of files you created (paths relative to the worktree root).
