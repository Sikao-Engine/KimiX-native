// grep_engine.h - High-performance pure-C++ grep search engine (ripgrep-inspired).
//
// Drives the native_io branch of the Grep tool (grep_tool.cpp): a recursive
// filesystem walk plus a content scan built for speed -
//   * whole-buffer reads: one fopen/fread into a single buffer, zero-copy line
//     iteration via memchr('\n') (no per-line string allocations while
//     scanning; one trailing '\r' is stripped per line view);
//   * literal fast path: a pure-literal pattern (no unescaped metacharacters)
//     skips the regex engine entirely and uses direct substring search
//     (memchr on a probe byte + memcmp; ASCII folding under ignore_case).
//     A literal containing '\n' never takes this path, so multiline can never
//     match - same observable behaviour as the per-line regex scan;
//   * multi-literal fast path: a top-level alternation of pure literals
//     ("foo|bar|baz", 2..16 branches, no '\n', ASCII) is likewise answered by
//     an any-of substring scan with the regex engine skipped;
//   * required-literal prefilter (the ripgrep trick): for every other pattern
//     the engine extracts the longest run of bytes EVERY match must contain
//     (flat patterns only - see extract_required_literal in grep_engine.cpp),
//     skips a file whose whole buffer lacks it, and runs the regex only on the
//     lines that contain it. Both byte-level tests are ASCII-only and are
//     backed by an overlong-UTF-8 escape hatch (may_hide_ascii_cp), so the
//     prefilter can only ever skip lines the engine could not match: the
//     observable answers are identical to the un-prefiltered scan;
  // * parallel search: the file list is collected in a single-threaded walk
  //   (each entry carrying a size hint), assigned to chunks by Longest-
  //   Processing-Time-first (largest file into the emptiest chunk, so no chunk
  //   is the critical path because it happens to hold two huge files) and
  //   searched over those chunks; the per-chunk output is merged back in WALK
  //   order, which is what makes the result byte-identical to a serial scan
  //   (see Threading model below);
//   * NUL binary sniff on the READ BUFFER: the first 64 KiB is sniffed
//     BEFORE the rest of the file is read, so a binary blob is skipped after
//     one 64 KiB read; a '\0' within the first 64 KiB skips the file silently
//     (rg convention); a NUL past 64 KiB does NOT;
//   * per-file cap: stat size > 4 MiB (or the read buffer growing past it) is
//     skipped silently;
//   * the walk pre-filters the file list: hidden entries, non-regular
//     entries and include-glob mismatches never reach the scan (the cached
//     directory-entry type answers the regular-file question without the
//     per-file re-decode + stat pair the old scan side paid) and records each
//     entry's size as a BALANCING HINT for the LPT split (re-checked by the
//     scan: the hint is never a correctness input);
//
// Threading model: the engine NEVER creates a scheduler, and it only touches
// fiber when the file list is big enough to fan out (>= 8 files) - small scans
// run inline on the calling thread, bound or not, with no bind cost. When
// fan-out IS required it always runs over kimix::fiber: a calling thread
// already bound to a pool (the host binds one at process / long-lived-thread
// start - see cli_main() and the background sub-agent worker) spreads the
// chunks over that AMBIENT pool; an unbound calling thread transiently binds
// the process-wide shared pool (kimix::fiber::shared_scheduler()) for the
// duration of the call and unbinds it on the way out - the same pattern as
// fiber::schedule_background() and the Python host's binding guard, and still
// no scheduler creation. Only where fiber cannot be made available at all
// does the search fail, with tool_status::unsupported (run_grep never
// throws). Either way the split is done via `fiber::parallel` (one job per
// chunk) with one regex_lite::Regex COMPILED PER CHUNK; each chunk keeps its
// matched files as indexed output blocks and the merge emits them in original
// walk order (one cursor per chunk), so the assignment may scatter the list
// over the pool while the output stays deterministic and walk-ordered, with no
// mutexes.
// The match plan (literal / alternation / required-literal prefilter) is
// derived ONCE by run_grep on the calling thread, next to the upfront compile
// validation, and every chunk worker only reads it - so the prefilter costs no
// per-chunk re-derivation and needs no synchronisation.
  //
  // Semantics intentionally mirror the previous inline branch (pinned by
// tests/unit/builtin_tools/test_grep_tool.cpp "grep_tool_native_io_branch_contract"):
// hidden entries are skipped at every depth (hidden dirs are not descended),
// only regular files are searched, the include glob is fnmatch_ascii over the
// file NAME, matching LINES are counted (not occurrences), paths are reported
// as walk paths (never base-stripped), the message is
// "{N} match(es) in {M} file(s)", and rendered lines follow the rg-style
// formats: files_with_matches -> the path alone; count_matches -> "path:count";
// content -> "path:LN:text" for hits, "path-LN-text" for -B/-A context (the
// SAME delimiter on both sides of the line number, like rg, so the caller's
// parse_content_line grammar "^(.*?)([:\-])(\d+)\2(.*)$" parses every
// rendered line) with "--" between disjoint runs. files_with_matches caps its rendered lines at
// head_limit while the walk-ordered merge runs (counted on the rendered lines
// it has produced so far); files[] stays complete; content and
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
// work_dir), searches contents, fills `out`. Never throws. When the file list
// is big enough to fan out (>= 8 files) the scan spreads over kimix::fiber:
// the calling thread's own pool, or - when unbound - the process-wide shared
// pool bound transiently for the call; returns tool_status::unsupported when
// no pool can be made available. Smaller lists scan inline on the caller.
tool_status run_grep(const grep_options &opts, kimix::span<const kimix::string> roots,
                     kimix::string_view work_dir, grep_result &out);

} // namespace kimix::builtin_tools::grep
