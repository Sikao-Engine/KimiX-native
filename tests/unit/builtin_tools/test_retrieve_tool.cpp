// Test for builtin_tools/retrieve_tool.h (namespace kimix::builtin_tools::retrieve).
//
// Covers the plan's §7 golden-vector matrix:
//   - parse_params: query-only, id-only, id-wins, missing both, whitespace,
//     out-of-range k
//   - parse_turn_reference: plain ints, prune_N, invalid, empty
//   - format_output: search-mode header, compacted marker, multiline text,
//     trailing newline, id-mode header, no-results messages
//   - apply_recency_boost + sort_and_truncate: magnitude, reordering,
//     truncation, stable ties
//   - run_retrieve end-to-end with stub HistoryIndexView
//
// All test logic lives in main() scope; no file-scope static registrations.

#include "ut/ut.hpp"

#include "builtin_tools/retrieve_tool.h"

#include <core/kimix_core.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::retrieve;

namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

kimix::string kix(const char *s) { return kimix::string(s); }

ToolParams make_params(const std::optional<std::string> &query,
                       const std::optional<std::string> &id,
                       const std::optional<int64_t> &k) {
    ToolParams params;
    if (query.has_value()) {
        params.values["query"] = ValueElement::make_string(kix(query.value()));
    }
    if (id.has_value()) {
        params.values["id"] = ValueElement::make_string(kix(id.value()));
    }
    if (k.has_value()) {
        params.values["k"] = ValueElement::make_int(k.value());
    }
    return params;
}

history_turn make_turn(int64_t turn_id, std::string_view role,
                       std::string_view text, double timestamp, double score,
                       bool is_compacted) {
    history_turn turn;
    turn.turn_id = turn_id;
    turn.role = kix(role);
    turn.text = kix(text);
    turn.timestamp = timestamp;
    turn.score = score;
    turn.is_compacted = is_compacted;
    return turn;
}

bool near_eq(double a, double b, double eps = 1e-12) {
    return std::abs(a - b) < eps;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // parse_params
    // -----------------------------------------------------------------------
    "parse_params_query_only"_test = [] {
        const auto params = make_params("hello", std::nullopt, std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::ok);
        expect(!out.id.has_value());
        expect(out.query == kix("hello"));
        expect(out.k == 3_i);
    };

    "parse_params_id_only"_test = [] {
        const auto params = make_params(std::nullopt, "prune_7", std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::ok);
        expect(out.id.has_value());
        expect(out.id.value() == kix("prune_7"));
        expect(out.query.empty());
        expect(out.k == 3_i);
    };

    "parse_params_id_wins_over_query"_test = [] {
        const auto params = make_params("hello", "prune_7", std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::ok);
        expect(out.id.has_value());
        expect(out.id.value() == kix("prune_7"));
    };

    "parse_params_missing_both"_test = [] {
        const auto params = make_params(std::nullopt, std::nullopt, std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::invalid_input);
        expect(err.message ==
               kix("No query provided. Pass a query string or an id."));
    };

    "parse_params_whitespace_query"_test = [] {
        const auto params = make_params("   ", std::nullopt, std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::invalid_input);
        expect(err.message ==
               kix("No query provided. Pass a query string or an id."));
    };

    "parse_params_out_of_range_k"_test = [] {
        const auto params_zero = make_params("hello", std::nullopt, 0);
        retrieve_params out;
        tool_error err;
        auto st = parse_params(&params_zero, out, err);
        expect(st == tool_status::invalid_input);

        const auto params_eleven = make_params("hello", std::nullopt, 11);
        st = parse_params(&params_eleven, out, err);
        expect(st == tool_status::invalid_input);
    };

    // -----------------------------------------------------------------------
    // parse_turn_reference
    // -----------------------------------------------------------------------
    "parse_turn_reference"_test = [] {
        expect(parse_turn_reference("42") == 42_i);
        expect(parse_turn_reference("prune_42") == 42_i);
        expect(parse_turn_reference("prune_0") == 0_i);
        expect(parse_turn_reference("abc") == -1_i);
        expect(parse_turn_reference("prune_abc") == -1_i);
        expect(parse_turn_reference("") == -1_i);
    };

    // -----------------------------------------------------------------------
    // format_output (search mode)
    // -----------------------------------------------------------------------
    "format_output_search_no_results"_test = [] {
        kimix::vector<history_turn> turns;
        retrieve_result result;
        format_output(turns, {}, result);
        expect(result.output ==
               kix("No matching results found in conversation history."));
    };

    "format_output_search_single"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(1, "user", "hello world", 0.0, 0.12345, false));
        retrieve_result result;
        format_output(turns, {}, result);
        const kimix::string expected =
            kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                "> **user** (relevance: 0.12)\n> hello world");
        expect(result.output == expected);
    };

    "format_output_search_compacted"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(2, "assistant", "summary", 0.0, 1.0, true));
        retrieve_result result;
        format_output(turns, {}, result);
        expect(result.output.find(kix(" [compacted]")) != kimix::string::npos)
            << "compacted marker present";
    };

    "format_output_search_multiline"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(3, "user", "line1\nline2", 0.0, 0.5, false));
        retrieve_result result;
        format_output(turns, {}, result);
        expect(result.output.find(kix("> line1\n> line2")) != kimix::string::npos)
            << "multiline quote prefix";
    };

    "format_output_search_trailing_newline"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(4, "user", "end\n", 0.0, 0.5, false));
        retrieve_result result;
        format_output(turns, {}, result);
        const kimix::string expected =
            kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                "> **user** (relevance: 0.50)\n> end\n> ");
        expect(result.output == expected);
    };

    "format_output_search_multiple"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(5, "user", "first", 0.0, 0.1, false));
        turns.push_back(make_turn(6, "assistant", "second", 0.0, 0.2, false));
        retrieve_result result;
        format_output(turns, {}, result);
        const kimix::string expected =
            kix("Retrieved 2 result(s):\n\n[Conversation history]\n"
                "> **user** (relevance: 0.10)\n> first\n"
                "> **assistant** (relevance: 0.20)\n> second");
        expect(result.output == expected);
    };

    // -----------------------------------------------------------------------
    // format_output (id mode)
    // -----------------------------------------------------------------------
    "format_output_id_hit_compacted"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(10, "user", "turn body", 0.0, 0.75, true));
        retrieve_result result;
        format_output(turns, "prune_3", result);
        const kimix::string expected =
            kix("Retrieved turn id='prune_3':\n"
                "> **user** [compacted]\n> turn body");
        expect(result.output == expected);
        expect(result.output.find(kix("(relevance:")) == kimix::string::npos)
            << "id mode has no relevance marker";
    };

    "format_output_id_miss"_test = [] {
        kimix::vector<history_turn> turns;
        retrieve_result result;
        format_output(turns, "prune_999", result);
        expect(result.output == kix("No turn found with id='prune_999'."));
    };

    // -----------------------------------------------------------------------
    // apply_recency_boost + sort_and_truncate
    // -----------------------------------------------------------------------
    "apply_recency_boost_magnitude"_test = [] {
        kimix::vector<history_turn> turns;
        const double now = 1000000.0;
        turns.push_back(make_turn(1, "user", "x", now - 24.0 * 3600.0, 1.0, false));
        apply_recency_boost(turns, 1.0, now);
        const double expected = 1.0 + 1.0 * std::exp(-1.0);
        expect(near_eq(turns[0].boosted_score, expected));
    };

    "sort_and_truncate_recency_reorder"_test = [] {
        const double now = 1000000.0;
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(1, "old", "old text", now - 48.0 * 3600.0, 1.5, false));
        turns.push_back(
            make_turn(2, "new", "new text", now, 1.0, false));
        apply_recency_boost(turns, 1.0, now);
        sort_and_truncate(turns, 10);
        expect(turns[0].role == kix("new"))
            << "newer turn outranks older high-score turn";
    };

    "sort_and_truncate_top_k"_test = [] {
        const double now = 1000000.0;
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "a", "a", now - 48.0 * 3600.0, 1.0, false));
        turns.push_back(make_turn(2, "b", "b", now - 24.0 * 3600.0, 1.0, false));
        turns.push_back(make_turn(3, "c", "c", now, 1.0, false));
        apply_recency_boost(turns, 1.0, now);
        sort_and_truncate(turns, 2);
        expect(turns.size() == 2_u);
    };

    "sort_and_truncate_stable_ties"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "first", "x", 0.0, 1.0, false));
        turns.push_back(make_turn(2, "second", "x", 0.0, 1.0, false));
        turns[0].boosted_score = 1.0;
        turns[1].boosted_score = 1.0;
        sort_and_truncate(turns, 10);
        expect(turns[0].role == kix("first"));
        expect(turns[1].role == kix("second"));
    };

    // -----------------------------------------------------------------------
    // run_retrieve end-to-end with stub HistoryIndexView
    // -----------------------------------------------------------------------
    "run_retrieve_id_hit"_test = [] {
        HistoryIndexView index;
        index.get_by_id = [](kimix::string_view ref) -> kimix::optional<history_turn> {
            if (ref == "prune_3") {
                history_turn t =
                    make_turn(3, "user", "found turn", 0.0, 0.0, true);
                return kimix::optional<history_turn>(std::move(t));
            }
            return {};
        };

        retrieve_params params;
        params.id = kix("prune_3");
        retrieve_result result;
        const auto st = run_retrieve(params, index, 0.0, result);
        expect(st == tool_status::ok);
        expect(result.output ==
               kix("Retrieved turn id='prune_3':\n"
                   "> **user** [compacted]\n> found turn"));
    };

    "run_retrieve_id_miss"_test = [] {
        HistoryIndexView index;
        index.get_by_id = [](kimix::string_view) -> kimix::optional<history_turn> {
            return {};
        };

        retrieve_params params;
        params.id = kix("prune_999");
        retrieve_result result;
        const auto st = run_retrieve(params, index, 0.0, result);
        expect(st == tool_status::not_found);
        expect(result.output == kix("No turn found with id='prune_999'."));
    };

    "run_retrieve_query_hit"_test = [] {
        const double now = 1000000.0;
        HistoryIndexView index;
        index.search_with_recency =
            [now](kimix::string_view, int32_t) -> kimix::vector<history_turn> {
                kimix::vector<history_turn> out;
                out.push_back(
                    make_turn(1, "user", "one", now - 24.0 * 3600.0, 1.0, false));
                out.push_back(
                    make_turn(2, "assistant", "two", now, 1.0, false));
                return out;
            };

        retrieve_params params;
        params.query = kix("hello");
        params.k = 1;
        retrieve_result result;
        const auto st = run_retrieve(params, index, now, result);
        expect(st == tool_status::ok);
        expect(result.output.find(kix("Retrieved 1 result(s):")) !=
               kimix::string::npos);
        expect(result.output.find(kix("> **assistant**")) != kimix::string::npos)
            << "newer turn wins with k=1";
    };

    "run_retrieve_query_no_results"_test = [] {
        HistoryIndexView index;
        index.search_with_recency = [](kimix::string_view,
                                       int32_t) -> kimix::vector<history_turn> {
            return {};
        };

        retrieve_params params;
        params.query = kix("hello");
        retrieve_result result;
        const auto st = run_retrieve(params, index, 0.0, result);
        expect(st == tool_status::ok);
        expect(result.output ==
               kix("No matching results found in conversation history."));
    };

    // -----------------------------------------------------------------------
    // Retrieve Tool class wrapper
    // -----------------------------------------------------------------------
    "retrieve_tool_invalid_params"_test = [] {
        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        tool(nullptr);
        const auto &json = tool.serialized_result();
        const kimix::string_view sv(json.data(), json.size());
        expect(sv.find(kix("invalid_input").data(), 0, 13) != kimix::string::npos)
            << "missing parameters returns invalid_input";
    };
}
