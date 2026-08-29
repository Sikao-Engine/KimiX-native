// retrieve_tool.h - C++ port of the retrieve memory/history tool kernels.
//
// Plan: D:/KimiX-native/plans/retrieve.md
//
// Python source of truth:
//   D:/kimi-agent/kimi-cli/src/kimi_cli/tools/memory/__init__.py
//     Params model                          19-33
//     retrieve.attach_history_index         50-52
//     retrieve.__call__                     55-101
//     retrieve._retrieve_by_id              103-122
//   D:/kimi-agent/kimi-cli/src/kimi_cli/soul/history_index.py
//     HistoryIndex.search_with_recency      585-615
//     HistoryIndex.get_by_id                545-583
//     HistoryIndex._row_to_turn             456-463
//
// Design notes:
//   * All public symbols live in kimix::builtin_tools::retrieve.
//   * The heavy full-text search / FTS5 execution stays in Python.  The C++ side
//     exposes a small, injectable HistoryIndexView; the Python shim performs
//     the actual search/lookup, passes the candidate turns to run_retrieve(),
//     and receives the formatted markdown string back.
//   * The Retrieve Tool subclass is a thin CallableTool2-style wrapper.  A real
//     caller must inject a HistoryIndexView via the public `view` member before
//     invoking operator(); without a view the operator returns
//     tool_status::unsupported so the Python shim can fall back to its mirror.
//
// Namespace is mandatory: kimix-llm builds every src/builtin_tools/*.cpp in a
// unity batch, so no file-scope `using namespace` and no generic names at
// file scope.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/tool.h"

namespace kimix::builtin_tools::retrieve {

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

// Validated parameter set.
struct retrieve_params {
    kimix::optional<kimix::string> id; // when set, query is ignored
    kimix::string query;               // natural-language query
    int32_t k = 3;                     // max results, validated to [1, 10]
};

// One candidate turn supplied by the Python-side HistoryIndex shim.
// Mirrors the dict returned by HistoryIndex._row_to_turn(..., with_score=true)
// plus the optional boosted_score used only for ranking.
struct history_turn {
    int64_t turn_id = -1;
    kimix::string role;
    kimix::string text;
    double timestamp = 0.0; // seconds since epoch
    double score = 0.0;     // BM25 / LIKE score (higher = more relevant)
    bool is_compacted = false;
    double boosted_score = 0.0; // internal ranking value; not returned to user
};

// Final result envelope.
struct retrieve_result {
    tool_status status = tool_status::ok;
    kimix::string message; // e.g. "Found 3 result(s)"
    kimix::string output;  // markdown formatted output
};

// Injected view of the Python-side HistoryIndex.
// The Python shim populates these closures and passes the struct to run_retrieve().
struct HistoryIndexView {
    kimix::function<kimix::vector<history_turn>(kimix::string_view query,
                                                  int32_t top_k)>
        search_with_recency;
    kimix::function<kimix::optional<history_turn>(kimix::string_view ref)>
        get_by_id;
};

// ---------------------------------------------------------------------------
// Parameter validation (memory/__init__.py lines 55-74)
// ---------------------------------------------------------------------------

// Parse the JSON tool parameters into retrieve_params.  On error returns
// tool_status::invalid_input and fills error.message with a byte-exact
// diagnostic matching the Python ValueError wording where applicable.
tool_status parse_params(const ToolParams *params, retrieve_params &out,
                         tool_error &error);

// ---------------------------------------------------------------------------
// Reference parsing (history_index.py lines 545-557)
// ---------------------------------------------------------------------------

// Parse a turn reference.  Accepts "42" and "prune_42".
// Returns the integer turn_id, or -1 if the reference is not a valid integer.
int64_t parse_turn_reference(kimix::string_view ref) noexcept;

// ---------------------------------------------------------------------------
// Ranking (history_index.py lines 585-615)
// ---------------------------------------------------------------------------

// Apply the recency boost in place.  `now` is the caller's current timestamp
// in seconds (e.g. Python time.time()).  Formula:
//   hours_ago = (now - turn.timestamp) / 3600.0
//   boost = 1.0 + recency_weight * exp(-hours_ago / 24.0)
//   turn.boosted_score = turn.score * boost
void apply_recency_boost(kimix::span<history_turn> turns,
                         double recency_weight, double now) noexcept;

// Stable-sort by descending boosted_score and truncate to top_k.
// Python's list.sort is stable, so ties retain their original order.
void sort_and_truncate(kimix::vector<history_turn> &turns, int32_t top_k);

// ---------------------------------------------------------------------------
// Output formatting (memory/__init__.py lines 87-121)
// ---------------------------------------------------------------------------

// Format a search-result list.  `ref_id` is empty for query mode.
// For id mode it is the original reference string (e.g. "prune_3"), used in the
// header exactly as Python does with f"id={ref_id!r}".
void format_output(kimix::span<const history_turn> turns,
                   kimix::string_view ref_id, retrieve_result &out);

// ---------------------------------------------------------------------------
// Orchestrator (used by the Python shim)
// ---------------------------------------------------------------------------

// High-level entry point.  Validates params, dispatches to id lookup or
// search, applies recency ranking, and formats the output.
// `now` is injected by the caller so the kernel stays deterministic and
// unit-testable.
tool_status run_retrieve(const retrieve_params &params,
                         const HistoryIndexView &index, double now,
                         retrieve_result &out);

// ---------------------------------------------------------------------------
// Tool class wrapper (CallableTool2-style binding entry point)
// ---------------------------------------------------------------------------

class Retrieve : public kimix::builtin_tools::Tool {
public:
    explicit Retrieve(kimix::builtin_tools::Session *session);

    // Tool interface: parse params, dispatch to run_retrieve() when a
    // HistoryIndexView has been injected, otherwise return unsupported.
    void operator()(ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &serialized_result() const { return _result; }

    // Injected view.  A real Python-side caller must set this before invoking
    // the tool through the standard Tool interface.
    HistoryIndexView view;

private:
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::retrieve
