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
- **No RTTI**: no `dynamic_cast`, no `typeid`. Project builds `kimix_rtti=false`.
  Exceptions are ON (`kimix_enable_exception=true`) but kernels must not throw
  across the tool boundary — return `tool_error` / `tool_status` instead.
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
  (they will conflict). The only build-file edit allowed is the single
  `test_proj(...)` line in `tests/xmake.lua`, inside the block appended at the
  **end of the file** between the markers
  `-- >>> BEGIN builtin_tools test registrations (per-tool lines go here) >>>`
  and `-- <<< END builtin_tools test registrations <<<`.
- File-system access: prefer keeping kernels *pure* (take bytes / metadata as
  arguments, e.g. `struct file_stat { bool is_dir; uint64_t size_bytes;
  uint64_t mtime_ns; };`, and inject existence probes as
  `kimix::function<bool(kimix::string_view)>`) so unit tests are deterministic
  and need no fixtures. `kimix::filesystem` (alias of `std::filesystem`, via
  `<core/stl/filesystem.h>`) IS available and already used by kimix-core, so a
  thin walking/stat helper is allowed when the plan demands it — just isolate
  it in one function so tests can run against a temp dir they create.

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
