// grep_engine.h - High-performance pure-C++ grep search engine (ripgrep-inspired).
//
// Drives the native_io branch of the Grep tool (grep_tool.cpp): a recursive
// filesystem walk plus a content scan built for speed -
//   * whole-buffer reads: one fopen/fread into a single buffer, zero-copy line
//     iteration via memchr('\n') (no per-line string allocations while
//     scanning; one trailing '\r' is stripped per line view);
//   * literal fast path: a pure-literal pattern (no unescaped metacharacters)
//     skips the regex engine entirely and uses direct substring search
//     (memchr on the first byte + memcmp; ASCII folding under ignore_case).
//     A literal containing '\n' never takes this path, so multiline can never
//     match - same observable behaviour as the per-line regex scan;
  // * parallel search: the file list is collected in a single-threaded walk,
  // then searched over static index ranges (see Threading model below);
//   * NUL binary sniff on the READ BUFFER: the first 64 KiB is sniffed
//     BEFORE the rest of the file is read, so a binary blob is skipped after
//     one 64 KiB read; a '\0' within the first 64 KiB skips the file silently
//     (rg convention); a NUL past 64 KiB does NOT;
//   * per-file cap: stat size > 4 MiB (or the read buffer growing past it) is
//     skipped silently;
//   * the walk pre-filters the file list: hidden entries, non-regular
//     entries and include-glob mismatches never reach the scan (the cached
//     directory-entry type answers the regular-file question without the
//     per-file re-decode + stat pair the old scan side paid).
//
// Threading model: the engine NEVER creates a scheduler. When the calling
// thread is bound to a kimix::fiber pool (the host binds one at process /
// long-lived-thread start - see cli_main() and the background sub-agent
// worker), the static chunks fan out over that AMBIENT pool via
// `fiber::parallel` (one job per chunk) with one regex_lite::Regex COMPILED
// PER CHUNK and per-chunk result vectors merged in chunk index order -
// output is deterministic and ordered by walk order, no mutexes. An unbound
  // caller scans inline through the same single-chunk logic (correct, serial).
  //
  // Semantics intentionally mirror the previous inline branch (pinned by
// tests/unit/builtin_tools/test_grep_tool.cpp "grep_tool_native_io_branch_contract"):
// hidden entries are skipped at every depth (hidden dirs are not descended),
// only regular files are searched, the include glob is fnmatch_ascii over the
// file NAME, matching LINES are counted (not occurrences), paths are reported
// as walk paths (never base-stripped), the message is
// "{N} match(es) in {M} file(s)", and rendered lines follow the rg-style
// formats: files_with_matches -> the path alone; count_matches -> "path:count";
// content -> "path:LN:text" for hits, "path-LN:text" for -B/-A context with
// "--" between disjoint runs. files_with_matches caps its rendered lines at
// head_limit during collection while files[] stays complete; content and
// count lines are returned whole (the caller joins and truncates).
//
// Namespace: kimix::builtin_tools::grep. Unity-build safe: this header only
// declares types/the entry point; all helpers live in grep_engine.cpp inside
// anonymous/named detail namespaces.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::grep {

enum class grep_output_mode : uint8_t { files_with_matches, count_matches, content };

struct grep_options {
    kimix::string pattern;          // regex_lite syntax (see regex_lite.h header comment)
    kimix::string include_glob;     // fnmatch_ascii over the file NAME; empty = all files
    grep_output_mode mode = grep_output_mode::files_with_matches;
    bool ignore_case = false;
    uint32_t ctx_before = 0;        // -B
    uint32_t ctx_after  = 0;        // -A (-C sets both, done by the caller)
    int64_t head_limit = 250;       // <= 0 means unlimited
};

struct grep_file_result {
    kimix::string path;             // walk path as produced today (NOT base-stripped)
    int64_t match_count = 0;        // matching LINES (current semantics count lines, not occurrences)
};

struct grep_result {
    tool_status status = tool_status::ok;
    kimix::string message;          // exactly "{N} match(es) in {M} file(s)"
    int64_t total_matches = 0;
    kimix::vector<grep_file_result> files; // one per matched file, in deterministic path order
    kimix::vector<kimix::string> lines; // rendered lines, per mode (see above)
    // Parallel to `lines` (content mode): 1 = the rendered line is a MATCH,
    // 0 = a context line or "--" separator. Lets the caller's head_limit fold
    // count omitted MATCH lines only (F-new-8 residual: context lines used to
    // inflate the "match lines omitted" tally). Empty for legacy producers.
    kimix::vector<uint8_t> line_match;
};

// Walks `roots` (each a file or directory; relative entries resolve against
// work_dir), searches contents, fills `out`. Never throws.
tool_status run_grep(const grep_options &opts, kimix::span<const kimix::string> roots,
                     kimix::string_view work_dir, grep_result &out);

} // namespace kimix::builtin_tools::grep
