// retrieve_tool.cpp - C++ port of the retrieve memory/history tool kernels.
//
// Implements D:/KimiX-native/plans/retrieve.md §3 kernels:
//   - parse_params
//   - parse_turn_reference
//   - apply_recency_boost / sort_and_truncate
//   - format_output / run_retrieve
//   - Retrieve Tool subclass wrapper
//
// The heavy full-text search (FTS5 / BM25) stays in Python.  This file only
// contains the deterministic ranking/formatting kernels and the thin
// CallableTool2 wrapper.  All symbols live inside
// kimix::builtin_tools::retrieve; internal helpers are in an anonymous
// namespace with the retrieve_ prefix so the unity build cannot collide.

#include "builtin_tools/retrieve_tool.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <mimalloc.h>
#include <yyjson.h>

namespace kimix::builtin_tools::retrieve {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers (unity-build safe, retrieve_ prefix)
// ---------------------------------------------------------------------------

// Convert a tool_status to the short string used in the JSON result envelope.
const char *retrieve_status_string(tool_status status) noexcept {
    switch (status) {
        case tool_status::ok:
            return "ok";
        case tool_status::invalid_input:
            return "invalid_input";
        case tool_status::not_found:
            return "not_found";
        case tool_status::no_change:
            return "no_change";
        case tool_status::ambiguous:
            return "ambiguous";
        case tool_status::blocked:
            return "blocked";
        case tool_status::too_large:
            return "too_large";
        case tool_status::unsupported:
            return "unsupported";
        case tool_status::external_library:
            return "external_library";
    }
    return "unknown";
}

// Round a positive dollar amount to the nearest cent using Python's
// round-half-to-even rule.  Scores are non-negative in this domain.
int64_t retrieve_round_cents(double score) noexcept {
    double scaled = score * 100.0;
    double intpart = 0.0;
    double frac = std::modf(scaled, &intpart);
    int64_t i = static_cast<int64_t>(intpart);

    // Compensate for tiny floating-point overshoots/undershoots around .5.
    if (frac < 0.0) {
        frac += 1.0;
        i -= 1;
    }

    if (frac > 0.5 + 1e-12) {
        i += 1;
    } else if (std::abs(frac - 0.5) <= 1e-12 && (i & 1) != 0) {
        i += 1;
    }
    return i;
}

// Format a relevance score as "{:.2f}" with Python-compatible rounding.
kimix::string retrieve_format_score(double score) noexcept {
    int64_t cents = retrieve_round_cents(score);
    if (cents < 0) {
        return kimix::format("-{}.{:02}", std::llabs(cents / 100),
                             std::llabs(cents % 100));
    }
    return kimix::format("{}.{:02}", cents / 100, cents % 100);
}

// True when `s` is empty or contains only whitespace.
bool retrieve_is_blank(kimix::string_view s) noexcept {
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

// Append `text` with every '\n' replaced by "\n> ", prefixed with "> ".
// Mirrors Python's text.replace(chr(10), chr(10) + '> ').
void retrieve_append_quoted_text(kimix::string &out,
                                 kimix::string_view text) noexcept {
    out.push_back('>');
    out.push_back(' ');
    for (const char c : text) {
        if (c == '\n') {
            out.push_back('\n');
            out.push_back('>');
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
}

// Build the no-results output text for search or id mode.
void retrieve_no_results_output(kimix::string_view ref_id,
                                retrieve_result &out) noexcept {
    if (ref_id.empty()) {
        out.output = "No matching results found in conversation history.";
    } else {
        out.output = kimix::format("No turn found with id='{}'.", ref_id);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Parameter validation
// ---------------------------------------------------------------------------

tool_status parse_params(const ToolParams *params, retrieve_params &out,
                         tool_error &error) {
    error = {};
    out = {};
    out.k = 3;

    if (params == nullptr) {
        error.status = tool_status::invalid_input;
        error.message = "No query provided. Pass a query string or an id.";
        return error.status;
    }

    const ValueElement *id_el = params->get("id");
    if (id_el != nullptr && id_el->is_string()) {
        const kimix::string_view id = id_el->as_string();
        if (!id.empty()) {
            out.id = kimix::string(id.data(), id.size());
        }
    }

    if (!out.id.has_value()) {
        const ValueElement *query_el = params->get("query");
        if (query_el == nullptr || !query_el->is_string()) {
            error.status = tool_status::invalid_input;
            error.message = "No query provided. Pass a query string or an id.";
            return error.status;
        }
        out.query = query_el->as_string();
        if (retrieve_is_blank(out.query)) {
            error.status = tool_status::invalid_input;
            error.message = "No query provided. Pass a query string or an id.";
            return error.status;
        }
    }

    const ValueElement *k_el = params->get("k");
    if (k_el != nullptr) {
        int64_t k_value = 0;
        if (k_el->is_int()) {
            k_value = k_el->as_int();
        } else if (k_el->is_uint()) {
            k_value = static_cast<int64_t>(k_el->as_uint());
        } else {
            error.status = tool_status::invalid_input;
            error.message = "k must be an integer between 1 and 10";
            return error.status;
        }

        if (k_value < 1 || k_value > 10) {
            error.status = tool_status::invalid_input;
            error.message =
                kimix::format("k must be between 1 and 10, got {}", k_value);
            return error.status;
        }
        out.k = static_cast<int32_t>(k_value);
    }

    return tool_status::ok;
}

// ---------------------------------------------------------------------------
// Reference parsing
// ---------------------------------------------------------------------------

int64_t parse_turn_reference(kimix::string_view ref) noexcept {
    if (ref.empty()) {
        return -1;
    }

    constexpr kimix::string_view k_prune_prefix = "prune_";
    kimix::string_view num = ref;
    if (ref.size() > k_prune_prefix.size() &&
        ref.compare(0, k_prune_prefix.size(), k_prune_prefix) == 0) {
        num = ref.substr(k_prune_prefix.size());
    }

    if (num.empty()) {
        return -1;
    }

    int64_t value = 0;
    const auto [ptr, ec] =
        std::from_chars(num.data(), num.data() + num.size(), value, 10);
    if (ec != std::errc() || ptr != num.data() + num.size()) {
        return -1;
    }
    return value;
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

void apply_recency_boost(kimix::span<history_turn> turns,
                         double recency_weight, double now) noexcept {
    for (history_turn &turn : turns) {
        const double hours_ago = (now - turn.timestamp) / 3600.0;
        const double boost = 1.0 + recency_weight * std::exp(-hours_ago / 24.0);
        turn.boosted_score = turn.score * boost;
    }
}

void sort_and_truncate(kimix::vector<history_turn> &turns, int32_t top_k) {
    std::stable_sort(turns.begin(), turns.end(),
                     [](const history_turn &a, const history_turn &b) {
                         return a.boosted_score > b.boosted_score;
                     });

    if (top_k >= 0 && static_cast<size_t>(top_k) < turns.size()) {
        turns.resize(static_cast<size_t>(top_k));
    }
}

// ---------------------------------------------------------------------------
// Output formatting
// ---------------------------------------------------------------------------

void format_output(kimix::span<const history_turn> turns,
                   kimix::string_view ref_id, retrieve_result &out) {
    if (turns.empty()) {
        retrieve_no_results_output(ref_id, out);
        return;
    }

    if (ref_id.empty()) {
        // Search mode.
        out.output = kimix::format(
            "Retrieved {} result(s):\n\n[Conversation history]", turns.size());
        for (const history_turn &turn : turns) {
            out.output.push_back('\n');
            out.output.append("> **");
            out.output.append(turn.role);
            out.output.append("**");
            if (turn.is_compacted) {
                out.output.append(" [compacted]");
            }
            out.output.append(" (relevance: ");
            out.output.append(retrieve_format_score(turn.score));
            out.output.append(")\n");
            retrieve_append_quoted_text(out.output, turn.text);
        }
    } else {
        // Id mode.
        out.output = kimix::format("Retrieved turn id='{}':\n", ref_id);
        const history_turn &turn = turns[0];
        out.output.append("> **");
        out.output.append(turn.role);
        out.output.append("**");
        if (turn.is_compacted) {
            out.output.append(" [compacted]");
        }
        out.output.push_back('\n');
        retrieve_append_quoted_text(out.output, turn.text);
    }
}

// ---------------------------------------------------------------------------
// Orchestrator
// ---------------------------------------------------------------------------

tool_status run_retrieve(const retrieve_params &params,
                         const HistoryIndexView &index, double now,
                         retrieve_result &out) {
    out = {};

    if (params.id.has_value()) {
        const kimix::optional<history_turn> turn =
            index.get_by_id(params.id.value());
        if (!turn.has_value()) {
            out.status = tool_status::not_found;
            out.message = "No results";
            retrieve_no_results_output(params.id.value(), out);
            return out.status;
        }

        kimix::vector<history_turn> turns;
        turns.push_back(turn.value());
        out.status = tool_status::ok;
        out.message = kimix::format("Found turn id='{}'", params.id.value());
        format_output(turns, params.id.value(), out);
        return out.status;
    }

    // Query mode: ask the shim for k*3 candidates, then boost/rank/truncate.
    kimix::vector<history_turn> candidates =
        index.search_with_recency(params.query, static_cast<int32_t>(params.k * 3));
    apply_recency_boost(candidates, 1.0, now);
    sort_and_truncate(candidates, params.k);

    if (candidates.empty()) {
        out.status = tool_status::ok;
        out.message = "No results";
        retrieve_no_results_output({}, out);
        return out.status;
    }

    out.status = tool_status::ok;
    out.message = kimix::format("Found {} result(s)", candidates.size());
    format_output(candidates, {}, out);
    return out.status;
}

// ---------------------------------------------------------------------------
// Tool class wrapper
// ---------------------------------------------------------------------------

Retrieve::Retrieve(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Retrieve::operator()(ToolParams const *parameters) {
    _result.clear();
    kimix::builtin_tools::ToolParams result;

    retrieve_params params;
    tool_error err;
    const tool_status st = parse_params(parameters, params, err);
    if (st != tool_status::ok) {
        result.values["ok"] = ValueElement::make_bool(false);
        result.values["status"] =
            ValueElement::make_string(retrieve_status_string(err.status));
        result.values["message"] = ValueElement::make_string(err.message);
        result.values["output"] = ValueElement::make_string(kimix::string{});
        result.serialize(_result);
        return;
    }

    if (!view.search_with_recency || !view.get_by_id) {
        result.values["ok"] = ValueElement::make_bool(false);
        result.values["status"] = ValueElement::make_string("unsupported");
        result.values["message"] = ValueElement::make_string(
            "Native retrieve requires an injected history index view");
        result.values["output"] = ValueElement::make_string(kimix::string{});
        result.serialize(_result);
        return;
    }

    retrieve_result r;
    const double now =
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    run_retrieve(params, view, now, r);

    result.values["ok"] = ValueElement::make_bool(r.status == tool_status::ok);
    result.values["status"] = ValueElement::make_string(retrieve_status_string(r.status));
    result.values["message"] = ValueElement::make_string(r.message);
    result.values["output"] = ValueElement::make_string(r.output);
    result.serialize(_result);
}

} // namespace kimix::builtin_tools::retrieve
