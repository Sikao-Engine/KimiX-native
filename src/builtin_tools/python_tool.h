// python_tool.h - C++ port of the kimi-agent python tool's pure CPU kernels.
//
// Plan: C:/dev/kimi-agent/plans/python.md ("Plan: Rewrite python Tool to C++").
// Source of truth (C:/dev/kimi-agent/src/kimix/tools/):
//   py/__init__.py
//     _resolve_python / _resolve_python_uncached (103-150)  -> resolve_python_exe
//     _build_env (152-201)                                  -> prepare_python_env
//     _module_not_found_hint (203-213)                      -> module_not_found_hint
//   common.py
//     _create_script_file (722-741)                         -> ScriptFileWriter::plan_path
//     _extract_export_path (932-945)                        -> extract_export_path
//     _build_session_output_block (948-991)                 -> build_session_output_block
//   security.py
//     scrub_child_env (47-77)                               -> scrub_child_env
//   background/utils.py
//     BackgroundStream.wait_for_output pattern step (307-359)
//                                                           -> classify_wait_pattern /
//                                                              match_wait_pattern
//
// What deliberately stays in Python (plans/python.md §3.9 "non-goals"):
// process spawn/stream/wait/input (ProcessTask + BackgroundStream — async I/O,
// callback threading, process-tree registry/atexit cleanup, kill_child_tree),
// _syntax_check_error (CPython compile), long-output summarization (LLM
// network call), asyncio.Semaphore(8), rtk marker parsing
// (parse_rtk_rg_output), temp-folder lifecycle/cleanup, _display_temp_path
// (path display normalization), and the _temp_set keyed-append variant of the
// script writer (_export_to_temp_file) which only the export pipeline uses.
//
// Ownership map (src/builtin_tools/README.md): truncate_lines and
// find_error_line_index are owned by the bash tool — this header does not
// declare them.
//
// Namespace: kimix::builtin_tools::python. kimix-llm builds with a unity
// (jumbo) batch, so every symbol of this tool lives inside this namespace.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::python {

// Shared vocabulary (builtin_tools/tool_types.h) — reuse, never re-declare.
using kimix::builtin_tools::named_value;
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// ---------------------------------------------------------------------------
// 1. Script path planning (mirrors common.py _create_script_file 722-741)
// ---------------------------------------------------------------------------

// Pure path arithmetic for the shared temp-script naming scheme: scripts land
// at <base_dir>/<index><ext> with a monotonically increasing index, exactly
// like the Python `_temp_folder / (str(id) + ext)` creator.  The kernel never
// touches the filesystem (no mkdir, no write, no existence probe); the caller
// owns the base-dir lifecycle (.kimix_cache/tmp_<pid>) and the file writing.
//
// - `base_dir` must be an absolute directory path (the Python side resolves
//   the temp folder against the host cwd before calling).
// - The returned path is `<base_dir>/<index><ext>` using the host separator
//   (backslashes on Windows), i.e. the same string `Path / (str(id) + ext)`
//   produces.
// - Collision avoidance is the monotonic counter itself; `next_index()` is
//   the index the next planned path will use.  Thread-safe: guarded by an
//   internal spin_mutex so concurrent planners hand out distinct indices.
class ScriptFileWriter {
public:
    explicit ScriptFileWriter(kimix::string_view base_dir, uint64_t start_index = 0);

    // Plan the path for the next script and advance the counter.
    // `ext` is appended verbatim (the reference always passes ".py").
    kimix::string plan_path(kimix::string_view ext = ".py");

    // Peek at the index the next plan_path() will use.
    uint64_t next_index() const;

private:
    kimix::string _base_dir;
    uint64_t _next_index = 0;
    mutable kimix::spin_mutex _mutex;
};

// Stateless variant of plan_path for callers that manage the index themselves
// (mirrors the same naming arithmetic with an explicit attempt/counter).
kimix::string plan_script_path(kimix::string_view base_dir, uint64_t index,
                               kimix::string_view ext = ".py");

// ---------------------------------------------------------------------------
// 2. Interpreter resolution (mirrors py/__init__.py _resolve_python 103-150)
// ---------------------------------------------------------------------------

// Decision kernel for `_resolve_python_uncached`.  Precedence, exactly as in
// the reference:
//   1. `override` (KIMIX_PYTHON_EXECUTABLE) when non-empty and exists.
//   2. Walk up from each search base (session dir, then cwd): for base and
//      every ancestor, probe <base>/.venv/Scripts/python.exe then
//      <base>/.venv/bin/python; first hit wins.
//   3. `virtual_env` (VIRTUAL_ENV): probe <venv>/Scripts/python.exe then
//      <venv>/bin/python.
//   4. `fallback` (sys.executable, passed in — not derivable from C++).
//
// `exists` is an injected file-existence probe (Path.is_file() in the
// reference) so the kernel stays deterministic and fixture-free.  Returns the
// resolved path, or nullopt when every candidate is missing and `fallback` is
// empty (the reference always falls back to sys.executable, which is never
// empty in-process; nullopt lets the binding layer signal that).
kimix::optional<kimix::string>
resolve_python_exe(kimix::string_view override,
                   kimix::span<const kimix::string> search_bases,
                   kimix::string_view virtual_env,
                   kimix::string_view fallback,
                   const kimix::function<bool(kimix::string_view)> &exists);

// ---------------------------------------------------------------------------
// 3. Child environment assembly (mirrors py/__init__.py _build_env 152-201
//    and security.py scrub_child_env 47-77)
// ---------------------------------------------------------------------------

// security.py scrub_child_env (47-77), byte-exact: a variable is kept when
// its uppercased name starts with a safe prefix; otherwise it is dropped when
// the uppercased name contains a secret substring; everything else is kept.
// Insertion order is preserved; the input is never mutated.
// Non-ASCII names: the Python mirror gates native use on `name.isascii()`;
// kernels here are defined for ASCII names only (non-ASCII bytes are compared
// byte-wise, which agrees with str.upper() for ASCII names).
kimix::vector<named_value> scrub_child_env(kimix::span<const named_value> env);

// One entry of the prepare_python_env delta: the variable `name` is set to
// `value` in the child environment (added when absent from the parent,
// changed when it differs).
struct env_change {
    kimix::string name;
    kimix::string value;
};

// Decision kernel for `_build_env`.  Pure computation over injected strings —
// no os.environ access, no filesystem access except the injected
// `is_file` probe used for the pyvenv.cfg check.
//
// Inputs:
//   python_exe    resolved interpreter path (exe.parent drives venv detection)
//   share_bin_dir the shared bin directory prepended to PATH
//   current_path  the parent's PATH value ("" when PATH is unset)
//   path_sep      os.pathsep (";" on Windows, ":" on POSIX)
//   is_file       probe for the pyvenv.cfg existence check
//
// Returns nullopt for the reference's zero-copy fast path (interpreter not
// inside a virtualenv AND share_bin_dir already first in PATH), exactly like
// `_build_env` returning None.  Otherwise returns the env *delta*: the
// variables that must be added/changed on top of the (already scrubbed) base
// snapshot.  The delta carries at most PATH and VIRTUAL_ENV, in that order,
// matching the reference's assignment order.
kimix::optional<kimix::vector<env_change>>
prepare_python_env(kimix::string_view python_exe,
                   kimix::string_view share_bin_dir,
                   kimix::string_view current_path,
                   kimix::string_view path_sep,
                   const kimix::function<bool(kimix::string_view)> &is_file);

// ---------------------------------------------------------------------------
// 4. Module-not-found hint (mirrors py/__init__.py _module_not_found_hint
//    203-213)
// ---------------------------------------------------------------------------

// Scan `output` for the first `ModuleNotFoundError: No module named '<m>'`
// (single or double quotes) and return the byte-exact remediation hint.  The
// reference regex is ASCII-only; the marker itself is ASCII, so a byte-level
// scanner is exact.  Returns an empty string when there is no match (the
// reference returns "").
kimix::string module_not_found_hint(kimix::string_view output,
                                    kimix::string_view python_exe);

// ---------------------------------------------------------------------------
// 5. Session output block (mirrors common.py _build_session_output_block
//    948-991)
// ---------------------------------------------------------------------------

// Plain input struct mirroring the Python keyword arguments.  Field order in
// the rendered block is fixed by the reference and independent of this struct.
struct session_output_block {
    kimix::string task_id;
    kimix::string status;
    kimix::string output;
    kimix::optional<int32_t> exit_code;
    kimix::optional<kimix::string> exit_code_meaning; // empty string == falsy
    kimix::optional<kimix::string> failure_hint;      // empty string == falsy
    kimix::optional<bool> wait_matched;
    kimix::optional<double> elapsed_seconds;
    kimix::optional<kimix::string> output_path; // empty string == falsy
    bool output_truncated = false;
    kimix::optional<kimix::string> original_path; // empty string == falsy
};

// Render the YAML-like metadata block, byte-exact against the Python builder:
//   task_id / status / exit_code / exit_code_meaning / failure_hint /
//   "output: |" / the output lines indented two spaces (rstrip("\n") first,
//   Python splitlines() on the ASCII separators) or "  (no output)" when
//   empty / output_truncated (true|false) / output_path / wait_matched /
//   elapsed_seconds ({:.2f} formatting) / original_path, joined with '\n'.
// Falsy optional strings render as "null", matching Python truthiness.
kimix::string build_session_output_block(const session_output_block &block);

// ---------------------------------------------------------------------------
// 6. Export-path extraction (mirrors common.py _extract_export_path 932-945)
// ---------------------------------------------------------------------------

// Scan `output` for the four export markers in reference order and return the
// trailing path trimmed of trailing ']' and backtick characters.  Returns
// nullopt when no marker is present (the reference returns None).
kimix::optional<kimix::string> extract_export_path(kimix::string_view output);

// ---------------------------------------------------------------------------
// 7. wait_for_pattern matching (mirrors the pattern step of
//    BackgroundStream.wait_for_output, background/utils.py 307-359)
// ---------------------------------------------------------------------------

// Classification of a wait_for_pattern string against the native subset.
//
// The reference compiles the pattern with Python's `regex` engine and calls
// pattern.search(buffer) over the accumulated stream (background/utils.py
// 338).  The native kernel covers the two hot cases without a regex engine:
//   * literal  — no metacharacters at all: regex.search over a
//                metacharacter-free pattern is an exact substring search, so
//                the native result is byte-exact.
//   * glob     — only the fnmatch metacharacters (*, ?, [seq], [!seq]) plus
//                literals, matched with fnmatchcase semantics wrapped in
//                leading/trailing '*' (search-over-buffer).  NOTE: this is a
//                documented deviation from the reference, where '*', '?' and
//                '[' carry *regex* meaning (e.g. regex 'ready*' matches
//                'read' but glob 'ready*' does not); the deviation is
//                recorded in reports/python.md.  Patterns that would be
//                invalid regexes in the reference (e.g. '*done*' -> 'nothing
//                to repeat') also land here and match as globs instead of
//                surfacing the reference's 'Invalid wait_for_pattern' error.
//   * unsupported — anything else (regex-only metacharacters . ^ $ + { } \
//                | ( ) ], or non-ASCII input): the caller must route to the
//                Python mirror (tool_status::unsupported contract).
enum class wait_pattern_kind : uint8_t {
    literal,    // no metacharacters: exact substring search
    glob,       // fnmatch-style metacharacters only (see note above)
    unsupported // full Python regex engine needed
};

// Classify `pattern`.  ASCII patterns only: non-ASCII input is classified as
// unsupported (the reference regex engine is Unicode-aware).
wait_pattern_kind classify_wait_pattern(kimix::string_view pattern);

// Match `pattern` against the accumulated `buffer` with the semantics of
// `pattern.search(buffer)` for the literal subset and fnmatch-style search
// for the glob subset (see wait_pattern_kind for the deviation note).
// Returns:
//   status ok          -> `matched` holds the result
//   status unsupported -> the pattern needs the full Python regex engine;
//                         the caller must fall back to the Python mirror.
//   status invalid_input -> empty pattern (the reference compiles "" and
//                         matches everything; kernels refuse the degenerate
//                         input instead of guessing).
tool_error match_wait_pattern(kimix::string_view pattern,
                              kimix::string_view buffer, bool &matched);

} // namespace kimix::builtin_tools::python
