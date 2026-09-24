// Test for builtin_tools/retrieve_tool.h (namespace kimix::builtin_tools::retrieve).
//
// Every expected string in this file was derived from the kimi-agent reference
// (kimi-cli/src/kimi_cli/tools/memory/__init__.py + soul/history_index.py) by
// running it -- python/tests/test_parity_retrieve.py::test_unexposed_kernels_
// match_reference_goldens re-derives them, so a wrong golden cannot survive.
//
// Covers:
// - parse_params: query-only, id-only, id-wins, no query at all (guidance, NOT
//   an error), whitespace / Unicode-blank query, empty-string id, wrong types,
//   out-of-range k, pydantic's lax k coercions, nullptr params
// - parse_turn_reference: Python `int()` semantics (whitespace, sign,
//   underscores) + prune_ prefix handling
// - format_output: search-mode header/marker/relevance, the "[current]" and
//   "[compacted]" markers, multiline quoting, python-repr'd ids, no-results
// - apply_recency_boost + sort_and_truncate: 24h decay, reordering,
//   truncation, stable ties, weight 0
// - run_retrieve end-to-end with stub HistoryIndexView: the k*3 candidate pool,
//   recency_weight 1.0, ToolOk-shaped envelopes, the Retrieve wrapper JSON
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

bool holds(kimix::string_view haystack, kimix::string_view needle) {
    return haystack.find(needle) != kimix::string_view::npos;
}

bool json_contains(const kimix::vector<char> &json, kimix::string_view needle) {
    return kimix::string_view(json.data(), json.size()).find(needle) !=
           kimix::string_view::npos;
}

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

// Search-mode output for a single turn, so the marker/relevance goldens stay
// readable.
kimix::string fmt_one(const history_turn &turn) {
    kimix::vector<history_turn> turns;
    turns.push_back(turn);
    retrieve_result result;
    format_output(turns, {}, result);
    return result.output;
}

kimix::string relevance_field(kimix::string_view output) {
    const kimix::string_view marker = "(relevance: ";
    const size_t start = output.find(marker);
    if (start == kimix::string_view::npos) {
        return {};
    }
    const size_t from = start + marker.size();
    const size_t end = output.find(')', from);
    return kimix::string(output.substr(from, end - from));
}

constexpr double k_now = 1000000.0;
constexpr double k_hour = 3600.0;

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

    // memory:19-33 -- query defaults to "" and id to None, so "nothing at all"
    // is *valid* params: the tool answers with guidance (memory:70-74).
    "parse_params_missing_both_is_not_an_error"_test = [] {
        const auto params = make_params(std::nullopt, std::nullopt, std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::ok);
        expect(out.query.empty());
        expect(!out.id.has_value());
        expect(err.message.empty());
    };

    "parse_params_null_params_is_not_an_error"_test = [] {
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(nullptr, out, err);
        expect(st == tool_status::ok);
        expect(out.query.empty());
        expect(out.k == 3_i);
    };

    "parse_params_blank_query_is_not_an_error"_test = [] {
        // Python: `if not params.query.strip()` (memory:70) -- ASCII and
        // Unicode whitespace behave the same.
        for (const char *query : {" ", "\t\n ", "\xc2\xa0", "\xe3\x80\x80"}) {
            const auto params = make_params(query, std::nullopt, std::nullopt);
            retrieve_params out;
            tool_error err;
            expect(parse_params(&params, out, err) == tool_status::ok) << query;
            expect(out.query == kix(query));
        }
    };

    "parse_params_empty_id_is_still_an_id"_test = [] {
        // Python: `params.id is not None` -> _retrieve_by_id("") -> "No turn
        // found with id=''." (it never falls back to the query path).
        const auto params = make_params("hello", "", std::nullopt);
        retrieve_params out;
        tool_error err;
        const auto st = parse_params(&params, out, err);
        expect(st == tool_status::ok);
        expect(out.id.has_value());
        expect(out.id.value().empty());
    };

    "parse_params_id_null_is_absent"_test = [] {
        ToolParams params;
        params.values["id"] = ValueElement::make_null();
        params.values["query"] = ValueElement::make_string(kix("hello"));
        retrieve_params out;
        tool_error err;
        expect(parse_params(&params, out, err) == tool_status::ok);
        expect(!out.id.has_value());
        expect(out.query == kix("hello"));
    };

    "parse_params_non_string_query_and_id_are_invalid"_test = [] {
        retrieve_params out;
        tool_error err;

        ToolParams id_int;
        id_int.values["id"] = ValueElement::make_int(42);
        expect(parse_params(&id_int, out, err) == tool_status::invalid_input);
        expect(err.message == kix("id must be a string"));

        ToolParams query_int;
        query_int.values["query"] = ValueElement::make_int(5);
        expect(parse_params(&query_int, out, err) == tool_status::invalid_input);
        expect(err.message == kix("query must be a string"));

        ToolParams query_null;
        query_null.values["query"] = ValueElement::make_null();
        expect(parse_params(&query_null, out, err) == tool_status::invalid_input);
        expect(err.message == kix("query must be a string"));
    };

    "parse_params_k_bounds"_test = [] {
        retrieve_params out;
        tool_error err;
        for (int64_t k : {1, 5, 10}) {
            const auto params = make_params("hello", std::nullopt, k);
            expect(parse_params(&params, out, err) == tool_status::ok);
            expect(out.k == static_cast<int32_t>(k));
        }
        for (int64_t k : {0, -1, 11, 100}) {
            const auto params = make_params("hello", std::nullopt, k);
            expect(parse_params(&params, out, err) == tool_status::invalid_input);
            expect(err.message ==
                   kimix::format("k must be between 1 and 10, got {}", k));
        }
    };

    // pydantic lax coercions (Params: int, ge=1, le=10).
    "parse_params_k_coercions"_test = [] {
        retrieve_params out;
        tool_error err;

        ToolParams k_string;
        k_string.values["k"] = ValueElement::make_string(kix("7"));
        expect(parse_params(&k_string, out, err) == tool_status::ok);
        expect(out.k == 7_i);

        ToolParams k_real;
        k_real.values["k"] = ValueElement::make_real(5.0);
        expect(parse_params(&k_real, out, err) == tool_status::ok);
        expect(out.k == 5_i);

        ToolParams k_frac;
        k_frac.values["k"] = ValueElement::make_real(5.7);
        expect(parse_params(&k_frac, out, err) == tool_status::invalid_input);

        ToolParams k_bool;
        k_bool.values["k"] = ValueElement::make_bool(true);
        expect(parse_params(&k_bool, out, err) == tool_status::ok);
        expect(out.k == 1_i);

        ToolParams k_uint;
        k_uint.values["k"] = ValueElement::make_uint(9);
        expect(parse_params(&k_uint, out, err) == tool_status::ok);
        expect(out.k == 9_i);

        ToolParams k_none;
        k_none.values["k"] = ValueElement::make_null();
        expect(parse_params(&k_none, out, err) == tool_status::invalid_input);

        ToolParams k_bad_string;
        k_bad_string.values["k"] = ValueElement::make_string(kix("many"));
        expect(parse_params(&k_bad_string, out, err) ==
               tool_status::invalid_input);
    };

    "parse_params_alias_resolution"_test = [] {
        // ToolParams::with_aliases: the documented synonyms resolve.
        ToolParams params;
        params.values["q"] = ValueElement::make_string(kix("aliased"));
        params.values["limit"] = ValueElement::make_int(2);
        retrieve_params out;
        tool_error err;
        expect(parse_params(&params, out, err) == tool_status::ok);
        expect(out.query == kix("aliased"));
        expect(out.k == 2_i);
    };

    // -----------------------------------------------------------------------
    // parse_turn_reference (history_index.py:551-557 -- `int()` semantics)
    // -----------------------------------------------------------------------
    "parse_turn_reference"_test = [] {
        expect(parse_turn_reference("42") == 42_i);
        expect(parse_turn_reference("prune_42") == 42_i);
        expect(parse_turn_reference("prune_0") == 0_i);
        expect(parse_turn_reference("0") == 0_i);
        expect(parse_turn_reference("0007") == 7_i);
        expect(parse_turn_reference("abc") == -1_i);
        expect(parse_turn_reference("prune_abc") == -1_i);
        expect(parse_turn_reference("") == -1_i);
        expect(parse_turn_reference("prune_") == -1_i);
        expect(parse_turn_reference("prune_prune_1") == -1_i);
        expect(parse_turn_reference("0x10") == -1_i);
        expect(parse_turn_reference("4.2") == -1_i);
        expect(parse_turn_reference("4 2") == -1_i);
    };

    "parse_turn_reference_python_int_extras"_test = [] {
        // Python: int("+42") == 42, int(" 42 ") == 42, int("1_0") == 10,
        // int("-5") == -5 (the -1 sentinel is reserved for unparsable refs;
        // turn ids are never negative).
        expect(parse_turn_reference("+42") == 42_i);
        expect(parse_turn_reference(" 42") == 42_i);
        expect(parse_turn_reference("42 ") == 42_i);
        expect(parse_turn_reference("\t42\n") == 42_i);
        expect(parse_turn_reference("1_0") == 10_i);
        expect(parse_turn_reference("prune_+42") == 42_i);
        expect(parse_turn_reference("prune_ 7") == 7_i);
        expect(parse_turn_reference("-5") == -5_i);
        expect(parse_turn_reference("+ 42") == -1_i);
        expect(parse_turn_reference("42_") == -1_i);
        expect(parse_turn_reference("_42") == -1_i);
        expect(parse_turn_reference("1__0") == -1_i);
        // Non-ASCII decimal digits are the documented ASCII gate: Python's
        // int() accepts them, this kernel reports "unparsable".
        expect(parse_turn_reference("\xef\xbc\x91\xef\xbc\x92") == -1_i);
        // Out of int64 range cannot be a turn id.
        expect(parse_turn_reference("99999999999999999999999") == -1_i);
        const int64_t int64_max = parse_turn_reference("9223372036854775807");
        expect(int64_max > 0_i) << "int64 max still parses";
        expect(parse_turn_reference("9223372036854775808") == -1_i);
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
        // Golden re-derived from the reference (memory:87-96).
        const kimix::string expected =
            kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                "> **user** [current] (relevance: 0.12)\n> hello world");
        expect(fmt_one(make_turn(1, "user", "hello world", 0.0, 0.12345,
                                 false)) == expected);
    };

    "format_output_search_markers"_test = [] {
        const kimix::string current =
            fmt_one(make_turn(1, "user", "body", 0.0, 1.0, false));
        expect(holds(current, "> **user** [current] (relevance: 1.00)"));
        expect(!holds(current, "[compacted]"));

        const kimix::string compacted =
            fmt_one(make_turn(2, "assistant", "summary", 0.0, 1.0, true));
        expect(holds(compacted, "> **assistant** [compacted] (relevance: 1.00)"));
        expect(!holds(compacted, "[current]"));
    };

    "format_output_search_multiline"_test = [] {
        const kimix::string out =
            fmt_one(make_turn(3, "user", "line1\nline2", 0.0, 0.5, false));
        expect(holds(out, "> line1\n> line2"));
        expect(out ==
               kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                   "> **user** [current] (relevance: 0.50)\n> line1\n> line2"));
    };

    "format_output_search_trailing_newline"_test = [] {
        expect(fmt_one(make_turn(4, "user", "end\n", 0.0, 0.5, false)) ==
               kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                   "> **user** [current] (relevance: 0.50)\n> end\n> "));
    };

    "format_output_search_empty_text"_test = [] {
        expect(fmt_one(make_turn(5, "user", "", 0.0, 0.0, false)) ==
               kix("Retrieved 1 result(s):\n\n[Conversation history]\n"
                   "> **user** [current] (relevance: 0.00)\n> "));
    };

    "format_output_search_multiple"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(5, "user", "first", 0.0, 0.1, false));
        turns.push_back(make_turn(6, "assistant", "second", 0.0, 0.2, false));
        retrieve_result result;
        format_output(turns, {}, result);
        expect(result.output ==
               kix("Retrieved 2 result(s):\n\n[Conversation history]\n"
                   "> **user** [current] (relevance: 0.10)\n> first\n"
                   "> **assistant** [current] (relevance: 0.20)\n> second"));
    };

    "format_output_search_empty_role"_test = [] {
        expect(holds(fmt_one(make_turn(1, "", "body", 0.0, 0.0, false)),
                     "> **** [current]"));
    };

    // memory:94 -- f"{score:.2f}": CPython rounds the exact binary value.
    "format_output_relevance_matches_python_f2f"_test = [] {
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 0.005,
                                                 false))) == kix("0.01"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 0.015,
                                                 false))) == kix("0.01"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 0.025,
                                                 false))) == kix("0.03"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 2.675,
                                                 false))) == kix("2.67"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 0.125,
                                                 false))) == kix("0.12"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 99.995,
                                                 false))) == kix("100.00"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 123456.789,
                                                 false))) == kix("123456.79"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, -0.001,
                                                 false))) == kix("-0.00"));
        expect(relevance_field(fmt_one(make_turn(1, "u", "b", 0.0, 1e9,
                                                 false))) == kix("1000000000.00"));
    };

    // -----------------------------------------------------------------------
    // format_output (id mode)
    // -----------------------------------------------------------------------
    "format_output_id_hit_compacted"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(10, "user", "turn body", 0.0, 0.75, true));
        retrieve_result result;
        format_output(turns, "prune_3", result);
        expect(result.output ==
               kix("Retrieved turn id='prune_3':\n"
                   "> **user** [compacted]\n> turn body"));
        expect(!holds(result.output, "(relevance:"))
            << "id mode has no relevance marker";
    };

    "format_output_id_hit_current"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(10, "user", "turn body", 0.0, 0.75, false));
        retrieve_result result;
        format_output(turns, "0", result);
        expect(result.output ==
               kix("Retrieved turn id='0':\n> **user** [current]\n> turn body"));
    };

    "format_output_id_miss"_test = [] {
        kimix::vector<history_turn> turns;
        retrieve_result result;
        format_output(turns, "prune_999", result);
        expect(result.output == kix("No turn found with id='prune_999'."));
    };

    // memory:109/117/121 -- f"id={ref_id!r}".
    "format_output_id_repr"_test = [] {
        kimix::vector<history_turn> turns;
        retrieve_result result;

        format_output(turns, "a\\b", result);
        expect(result.output == kix("No turn found with id='a\\\\b'."));
        format_output(turns, "a\nb", result);
        expect(result.output == kix("No turn found with id='a\\nb'."));
        format_output(turns, "a\tb", result);
        expect(result.output == kix("No turn found with id='a\\tb'."));
        format_output(turns, "\x01", result);
        expect(result.output == kix("No turn found with id='\\x01'."));
        format_output(turns, "\x7f", result);
        expect(result.output == kix("No turn found with id='\\x7f'."));
        // A single quote switches the repr to double quotes.
        format_output(turns, "it's", result);
        expect(result.output == kix("No turn found with id=\"it's\"."));
        // A double quote keeps the single quotes.
        format_output(turns, "a\"b", result);
        expect(result.output == kix("No turn found with id='a\"b'."));
        // Printable UTF-8 passes through.
        format_output(turns, "caf\xc3\xa9", result);
        expect(result.output == kix("No turn found with id='caf\xc3\xa9'."));
    };

    // The binding convention: an empty ref_id means search mode
    // (py_builtin_web.cpp format_retrieve_result).  run_retrieve does not use
    // this wrapper, so `id=""` stays an id lookup there (see below).
    "format_output_empty_ref_is_search_mode"_test = [] {
        kimix::vector<history_turn> turns;
        retrieve_result result;
        format_output(turns, "", result);
        expect(result.output ==
               kix("No matching results found in conversation history."));
    };

    // -----------------------------------------------------------------------
    // apply_recency_boost + sort_and_truncate (history_index.py:585-615)
    // -----------------------------------------------------------------------
    "apply_recency_boost_magnitude"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(1, "user", "x", k_now - 24.0 * k_hour, 1.0, false));
        apply_recency_boost(turns, 1.0, k_now);
        expect(near_eq(turns[0].boosted_score, 1.0 + std::exp(-1.0)));
    };

    "apply_recency_boost_decay_is_exponential_24h"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "u", "now", k_now, 2.0, false));
        turns.push_back(make_turn(2, "u", "24h", k_now - 24.0 * k_hour, 2.0, false));
        turns.push_back(make_turn(3, "u", "48h", k_now - 48.0 * k_hour, 2.0, false));
        apply_recency_boost(turns, 1.0, k_now);
        expect(near_eq(turns[0].boosted_score, 2.0 * (1.0 + 1.0)));
        expect(near_eq(turns[1].boosted_score, 2.0 * (1.0 + std::exp(-1.0))));
        expect(near_eq(turns[2].boosted_score, 2.0 * (1.0 + std::exp(-2.0))));
        expect(turns[0].boosted_score > turns[1].boosted_score);
        expect(turns[1].boosted_score > turns[2].boosted_score);
    };

    "apply_recency_boost_weight_scales_only_the_boost"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "u", "x", k_now, 3.0, false));
        apply_recency_boost(turns, 0.0, k_now);
        expect(near_eq(turns[0].boosted_score, 3.0))
            << "weight 0 falls back to plain BM25 (history_index.py:610)";
        apply_recency_boost(turns, 2.0, k_now);
        expect(near_eq(turns[0].boosted_score, 3.0 * (1.0 + 2.0)));
        expect(near_eq(turns[0].score, 3.0)) << "the BM25 score is preserved";
    };

    "apply_recency_boost_future_timestamp"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "u", "future", k_now + 5.0 * k_hour, 1.0, false));
        apply_recency_boost(turns, 1.0, k_now);
        expect(near_eq(turns[0].boosted_score, 1.0 + std::exp(5.0 / 24.0)));
    };

    "sort_and_truncate_recency_reorder"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(
            make_turn(1, "old", "old text", k_now - 48.0 * k_hour, 1.5, false));
        turns.push_back(make_turn(2, "new", "new text", k_now, 1.0, false));
        apply_recency_boost(turns, 1.0, k_now);
        sort_and_truncate(turns, 10);
        expect(turns[0].role == kix("new"))
            << "newer turn outranks older high-score turn";
        expect(turns[1].role == kix("old"));
    };

    "sort_and_truncate_top_k"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "a", "a", k_now - 48.0 * k_hour, 1.0, false));
        turns.push_back(make_turn(2, "b", "b", k_now - 24.0 * k_hour, 1.0, false));
        turns.push_back(make_turn(3, "c", "c", k_now, 1.0, false));
        apply_recency_boost(turns, 1.0, k_now);
        sort_and_truncate(turns, 2);
        expect(turns.size() == 2_u);
        expect(turns[0].role == kix("c"));
        expect(turns[1].role == kix("b"));
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

    "sort_and_truncate_edges"_test = [] {
        kimix::vector<history_turn> turns;
        turns.push_back(make_turn(1, "a", "a", 0.0, 1.0, false));
        turns.push_back(make_turn(2, "b", "b", 0.0, 2.0, false));
        sort_and_truncate(turns, 0);
        expect(turns.empty()) << "k=0 keeps nothing";
        sort_and_truncate(turns, 5);
        expect(turns.empty());
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
        expect(result.message == kix("Found turn id='prune_3'"));
        expect(result.output ==
               kix("Retrieved turn id='prune_3':\n"
                   "> **user** [compacted]\n> found turn"));
    };

    // memory:107-111 -- a miss is ToolOk ("No results"), not an error.
    "run_retrieve_id_miss"_test = [] {
        HistoryIndexView index;
        index.get_by_id = [](kimix::string_view) -> kimix::optional<history_turn> {
            return {};
        };

        retrieve_params params;
        params.id = kix("prune_999");
        retrieve_result result;
        const auto st = run_retrieve(params, index, 0.0, result);
        expect(st == tool_status::ok);
        expect(result.message == kix("No results"));
        expect(result.output == kix("No turn found with id='prune_999'."));
    };

    "run_retrieve_empty_id_is_an_id_lookup"_test = [] {
        HistoryIndexView index;
        int calls = 0;
        index.get_by_id = [&calls](kimix::string_view) -> kimix::optional<history_turn> {
            ++calls;
            return {};
        };
        index.search_with_recency = [](kimix::string_view,
                                       int32_t) -> kimix::vector<history_turn> {
            return {};
        };

        retrieve_params params;
        params.id = kix("");
        params.query = kix("ignored");
        retrieve_result result;
        expect(run_retrieve(params, index, 0.0, result) == tool_status::ok);
        expect(calls == 1_i) << "id is not None -> the query path is skipped";
        expect(result.message == kix("No results"));
        expect(result.output == kix("No turn found with id=''."));
    };

    "run_retrieve_id_message_uses_repr"_test = [] {
        HistoryIndexView index;
        index.get_by_id = [](kimix::string_view) -> kimix::optional<history_turn> {
            return {};
        };
        retrieve_params params;
        params.id = kix("it's");
        retrieve_result result;
        expect(run_retrieve(params, index, 0.0, result) == tool_status::ok);
        expect(result.output == kix("No turn found with id=\"it's\"."));
    };

    "run_retrieve_guidance_when_no_query"_test = [] {
        HistoryIndexView index;
        int searches = 0;
        index.search_with_recency = [&searches](kimix::string_view,
                                                int32_t) -> kimix::vector<history_turn> {
            ++searches;
            return {};
        };

        for (const char *query : {"", "   ", "\xe3\x80\x80"}) {
            retrieve_params params;
            params.query = kix(query);
            retrieve_result result;
            const auto st = run_retrieve(params, index, 0.0, result);
            expect(st == tool_status::ok);
            expect(result.message == kix("No query"));
            expect(result.output ==
                   kix("No query provided. Pass a `query` string or an `id`."));
        }
        expect(searches == 0_i) << "a blank query never reaches the index";
    };

    "run_retrieve_query_hit"_test = [] {
        HistoryIndexView index;
        int32_t seen_top_k = -1;
        index.search_with_recency =
            [&seen_top_k](kimix::string_view,
                          int32_t top_k) -> kimix::vector<history_turn> {
            seen_top_k = top_k;
            kimix::vector<history_turn> out;
            out.push_back(
                make_turn(1, "user", "one", k_now - 24.0 * k_hour, 1.0, false));
            out.push_back(make_turn(2, "assistant", "two", k_now, 1.0, false));
            return out;
        };

        retrieve_params params;
        params.query = kix("hello");
        params.k = 1;
        retrieve_result result;
        const auto st = run_retrieve(params, index, k_now, result);
        expect(st == tool_status::ok);
        expect(seen_top_k == 3_i) << "k*3 candidates (history_index.py:601)";
        expect(result.message == kix("Found 1 result(s)"));
        expect(holds(result.output, "Retrieved 1 result(s):"))
            << "the header counts the truncated list";
        expect(holds(result.output, "> **assistant** [current]"))
            << "newer turn wins with k=1";
        expect(holds(result.output, "> **user** [current]") == false);
    };

    "run_retrieve_query_pool_and_selection"_test = [] {
        HistoryIndexView index;
        kimix::vector<history_turn> pool;
        for (int i = 0; i < 8; ++i) {
            pool.push_back(make_turn(i, "user", kimix::format("turn {}", i),
                                     k_now - static_cast<double>(i) * k_hour,
                                     1.0, false));
        }
        index.search_with_recency =
            [&pool](kimix::string_view, int32_t) -> kimix::vector<history_turn> {
            return pool;
        };

        retrieve_params params;
        params.query = kix("turn");
        params.k = 3;
        retrieve_result result;
        expect(run_retrieve(params, index, k_now, result) == tool_status::ok);
        expect(result.message == kix("Found 3 result(s)"));
        // Newest first (history_index.py:614 `sort(key=..., reverse=True)`).
        expect(holds(result.output, "> **user** [current] (relevance: 1.00)\n> turn 0\n"));
        expect(holds(result.output, "> turn 1\n"));
        expect(holds(result.output, "> turn 2"));
        expect(!holds(result.output, "turn 3"));
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
        expect(result.message == kix("No results"));
        expect(result.output ==
               kix("No matching results found in conversation history."));
    };

    // -----------------------------------------------------------------------
    // Retrieve Tool class wrapper
    // -----------------------------------------------------------------------
    "retrieve_tool_missing_params_reports_ok"_test = [] {
        // params == nullptr -> Params() semantics -> the guidance ToolOk.
        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        HistoryIndexView view;
        view.search_with_recency = [](kimix::string_view,
                                      int32_t) -> kimix::vector<history_turn> {
            return {};
        };
        view.get_by_id = [](kimix::string_view) -> kimix::optional<history_turn> {
            return {};
        };
        tool.view = view;
        tool(nullptr);
        expect(json_contains(tool.serialized_result(), "\"status\":\"ok\""))
            << "no query is not an error";
        expect(json_contains(tool.serialized_result(), "No query provided"));
        expect(json_contains(tool.serialized_result(), "\"ok\":true"));
    };

    "retrieve_tool_without_view_is_unsupported"_test = [] {
        const auto params = make_params("hello", std::nullopt, std::nullopt);
        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        tool(&params);
        expect(json_contains(tool.serialized_result(), "unsupported"))
            << "no injected view -> the shim must fall back to Python";
        expect(json_contains(tool.serialized_result(), "\"ok\":false"));
    };

    "retrieve_tool_invalid_k_reports_invalid_input"_test = [] {
        ToolParams params;
        params.values["query"] = ValueElement::make_string(kix("hello"));
        params.values["k"] = ValueElement::make_int(0);
        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        tool(&params);
        expect(json_contains(tool.serialized_result(), "invalid_input"));
        expect(
            json_contains(tool.serialized_result(), "k must be between 1 and 10"));
    };

    "retrieve_tool_with_view_serializes_result"_test = [] {
        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        HistoryIndexView view;
        view.search_with_recency = [](kimix::string_view,
                                      int32_t) -> kimix::vector<history_turn> {
            kimix::vector<history_turn> out;
            out.push_back(make_turn(0, "user", "hello world", 0.0, 0.12345, false));
            return out;
        };
        view.get_by_id = [](kimix::string_view) -> kimix::optional<history_turn> {
            return {};
        };
        tool.view = view;

        const auto params = make_params("hello", std::nullopt, std::nullopt);
        tool(&params);
        const auto &json = tool.serialized_result();
        expect(json_contains(json, "\"ok\":true"));
        expect(json_contains(json, "Found 1 result(s)"));
        expect(json_contains(json, "[current] (relevance: 0.12)"));
    };
}
