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
//
// Reference-verified details (see python/tests/test_parity_retrieve.py, which
// diffs this file against the reference tool over the whole corpus):
//   * every turn carries a " [compacted]" / " [current]" marker (memory:92);
//   * scores use Python's f"{score:.2f}" (std::format, exact rounding);
//   * the id in "id={ref!r}" is Python-repr quoted (memory:109/117/121);
//   * a blank query, a missing turn and an empty index are all ToolOk
//     outcomes, never an error status.


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

// Format a relevance score exactly like Python's f"{score:.2f}" (memory:94).
// CPython formats the exact binary value with correct rounding;
// kimix::format("{:.2f}") (std::format) does the same, so the epsilon-based
// cents emulation this used to do (which rounded 0.005 -> "0.00" and
// 2.675 -> "2.68" where CPython says "0.01" / "2.67") is gone.
// Same conclusion as job_output_tool.cpp:67 and python_tool.cpp:381.
kimix::string retrieve_format_score(double score) noexcept {
    return kimix::format("{:.2f}", score);
}

// One decoded UTF-8 code point starting at `i`; advances `i` past it.  A byte
// that cannot start/continue a sequence is returned as its own (>= 0x80) value,
// which is never a Python space or ASCII digit.
uint32_t retrieve_next_cp(kimix::string_view s, size_t &i) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    size_t len = 0;
    uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
    } else {
        ++i;
        return b0;
    }
    if (i + len > s.size()) {
        ++i;
        return b0;
    }
    for (size_t k = 1; k < len; ++k) {
        const unsigned char bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) {
            ++i;
            return b0;
        }
        cp = (cp << 6) | (bk & 0x3Fu);
    }
    i += len;
    return cp;
}

// CPython's Py_UNICODE_ISSPACE -- the set str.strip() removes (and therefore
// the set `if not params.query.strip()` in memory:70 tests).
bool retrieve_is_py_space(uint32_t cp) noexcept {
    switch (cp) {
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20:
        case 0x85:
        case 0xA0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:
            return true;
        default:
            return cp >= 0x2000 && cp <= 0x200A;
    }
}

// True when `s` is empty or contains only whitespace.  `s` is UTF-8 (all of
// this tool's strings are); non-ASCII input is decoded so Python's Unicode
// strip semantics are matched ("\u3000" is blank, "\u3000x" is not).
bool retrieve_is_blank(kimix::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        if (!retrieve_is_py_space(retrieve_next_cp(s, i))) {
            return false;
        }
    }
    return true;
}

// Python repr() of a str for the f"id={ref_id!r}" interpolations
// (memory:109/117/121).  Exact for ASCII, and byte-preserving for UTF-8
// (the port's other repr helpers -- edit_tool.h:80 py_repr, run_tool.h:198
// py_repr_char -- have the same documented ASCII gate): Python additionally
// escapes *non-printable non-ASCII* code points (U+00A0, U+200B, lone
// surrogates, ...) as \xNN/\uNNNN.
kimix::string retrieve_py_repr(kimix::string_view text) {
    const bool has_single = text.find('\'') != kimix::string_view::npos;
    const bool has_double = text.find('"') != kimix::string_view::npos;
    const bool use_double = has_single && !has_double;
    kimix::string out;
    out.reserve(text.size() + 2);
    out.push_back(use_double ? '"' : '\'');
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c == '\\') {
            out.append("\\\\");
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c == '\r') {
            out.append("\\r");
        } else if (c == '\t') {
            out.append("\\t");
        } else if (c == '\'' && !use_double) {
            out.append("\\'");
        } else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out.append(buf);
        } else {
            out.push_back(raw);
        }
    }
    out.push_back(use_double ? '"' : '\'');
    return out;
}

// The per-turn marker: " [compacted]" or " [current]" (memory:92/114).
kimix::string_view retrieve_marker(const history_turn &turn) noexcept {
    return turn.is_compacted ? kimix::string_view(" [compacted]")
                             : kimix::string_view(" [current]");
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

// memory:70-74 -- an empty (or whitespace-only) query with no id is *guidance*,
// not an error: the reference returns ToolOk(output, "No query").
constexpr kimix::string_view k_no_query_output =
    "No query provided. Pass a `query` string or an `id`.";
constexpr kimix::string_view k_no_query_message = "No query";
constexpr kimix::string_view k_no_results_output =
    "No matching results found in conversation history.";
constexpr kimix::string_view k_no_results_message = "No results";

// The header of an id-mode hit: "Retrieved turn id='<repr>':\n".
kimix::string retrieve_id_header(kimix::string_view ref_id) {
    const kimix::string repr = retrieve_py_repr(ref_id);
    return kimix::format("Retrieved turn id={}:\n", kimix::string_view(repr));
}

// Message of an id-mode hit: "Found turn id='<repr>'".
kimix::string retrieve_id_message(kimix::string_view ref_id) {
    const kimix::string repr = retrieve_py_repr(ref_id);
    return kimix::format("Found turn id={}", kimix::string_view(repr));
}

// Build the no-results output text.  Search mode is the "no matching results"
// sentence; id mode names the reference the caller asked for.
void retrieve_no_results_output(bool id_mode, kimix::string_view ref_id,
                                retrieve_result &out) {
    if (!id_mode) {
        out.output = k_no_results_output;
    } else {
        const kimix::string repr = retrieve_py_repr(ref_id);
        out.output =
            kimix::format("No turn found with id={}.", kimix::string_view(repr));
    }
}

// Render `turns` in search or id mode (memory:87-96 / 112-120).
void retrieve_format_turns(kimix::span<const history_turn> turns, bool id_mode,
                           kimix::string_view ref_id, retrieve_result &out) {
    if (turns.empty()) {
        retrieve_no_results_output(id_mode, ref_id, out);
        return;
    }

    if (!id_mode) {
        out.output = kimix::format(
            "Retrieved {} result(s):\n\n[Conversation history]", turns.size());
        for (const history_turn &turn : turns) {
            out.output.push_back('\n');
            out.output.append("> **");
            out.output.append(turn.role);
            out.output.append("**");
            out.output.append(retrieve_marker(turn));
            out.output.append(" (relevance: ");
            out.output.append(retrieve_format_score(turn.score));
            out.output.append(")\n");
            retrieve_append_quoted_text(out.output, turn.text);
        }
        return;
    }

    out.output = retrieve_id_header(ref_id);
    const history_turn &turn = turns[0];
    out.output.append("> **");
    out.output.append(turn.role);
    out.output.append("**");
    out.output.append(retrieve_marker(turn));
    out.output.push_back('\n');
    retrieve_append_quoted_text(out.output, turn.text);
}

} // namespace


// ---------------------------------------------------------------------------
// Parameter validation
// ---------------------------------------------------------------------------

static const kimix::builtin_tools::param_alias k_retrieve_aliases[] = {
    {"query", "q search search_query text"},
    {"id", "turn_id turn message_id conversation_id"},
    {"k", "limit top_k count max_results max_turns"},
};

tool_status parse_params(const ToolParams *params, retrieve_params &out,
                         tool_error &error) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("command" for "cmd") are accepted; the canonical name always wins.
    const kimix::builtin_tools::ToolParams k_resolved =
        kimix::builtin_tools::ToolParams::with_aliases(params, k_retrieve_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    error = {};
    out = {};
    out.k = 3;

    // Params.query defaults to "" and Params.id to None (memory:19-33): an
    // absent query is *valid* and the tool answers with guidance instead
    // (memory:70-74, verified against the reference tool).  Nothing here is an
    // error until a field is present with the wrong type.
    if (params == nullptr) {
        return tool_status::ok;
    }

    const ValueElement *id_el = params->get("id");
    if (id_el != nullptr && !id_el->is_null()) {
        if (!id_el->is_string()) {
            // pydantic: "Input should be a valid string" for id: int (the
            // reference raises, it does not fall back to the query path).
            error.status = tool_status::invalid_input;
            error.message = "id must be a string";
            return error.status;
        }
        // An empty string is still an id: `params.id is not None` sends the
        // reference into _retrieve_by_id(""), which reports
        // "No turn found with id=''." -- it never searches by query.
        const kimix::string_view id = id_el->as_string();
        out.id = kimix::string(id.data(), id.size());
    }

    if (!out.id.has_value()) {
        const ValueElement *query_el = params->get("query");
        if (query_el != nullptr) {
            // Present but null is a validation error (pydantic `str` accepts no
            // None); only an *absent* query falls back to the "" default.
            if (query_el->is_null() || !query_el->is_string()) {
                error.status = tool_status::invalid_input;
                error.message = "query must be a string";
                return error.status;
            }
            out.query = query_el->as_string();
        }
    }

    const ValueElement *k_el = params->get("k");
    if (k_el != nullptr) {
        int64_t k_value = 0;
        bool have_k = false;
        if (k_el->is_int()) {
            k_value = k_el->as_int();
            have_k = true;
        } else if (k_el->is_uint()) {
            k_value = static_cast<int64_t>(k_el->as_uint());
            have_k = true;
        } else if (k_el->is_bool()) {
            // pydantic lax mode: Parameters(k=True) -> 1.
            k_value = k_el->as_bool() ? 1 : 0;
            have_k = true;
        } else if (k_el->is_real()) {
            // pydantic lax mode: 5.0 -> 5, 5.7 -> validation error.
            const double d = k_el->as_real();
            if (d == static_cast<double>(static_cast<int64_t>(d)) &&
                d >= -9.2e18 && d <= 9.2e18) {
                k_value = static_cast<int64_t>(d);
                have_k = true;
            }
        } else if (k_el->is_string()) {
            // pydantic lax mode: "5" -> 5 (numeric strings only).
            const kimix::string_view text = k_el->as_string();
            int64_t parsed = 0;
            const auto [ptr, ec] = std::from_chars(
                text.data(), text.data() + text.size(), parsed, 10);
            if (ec == std::errc() && ptr == text.data() + text.size()) {
                k_value = parsed;
                have_k = true;
            }
        }
        if (!have_k) {
            error.status = tool_status::invalid_input;
            error.message = "k must be an integer";
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
    constexpr kimix::string_view k_prune_prefix = "prune_";
    kimix::string_view num = ref;
    if (ref.size() >= k_prune_prefix.size() &&
        ref.compare(0, k_prune_prefix.size(), k_prune_prefix) == 0) {
        num = ref.substr(k_prune_prefix.size());
    }

    // history_index.py:551-557 -- `int(ref_str)`: optional surrounding
    // whitespace (Py_UNICODE_ISSPACE), an optional '+'/'-', and ASCII digits
    // with single '_' separators between them.  Everything else is a
    // ValueError -> None.  -1 stays the "unparsable" sentinel (turn ids are
    // never negative), and values that do not fit int64 parse as unparsable
    // because no such turn can exist.
    size_t begin = 0;
    size_t end = num.size();
    // `cursor` (not `begin`) absorbs retrieve_next_cp's advance, so a
    // non-space code point is never skipped.
    while (begin < end) {
        size_t cursor = begin;
        if (!retrieve_is_py_space(retrieve_next_cp(num, cursor))) {
            break;
        }
        begin = cursor;
    }
    while (end > begin) {
        size_t probe = end;
        // step back one code point (over any UTF-8 continuation bytes)
        do {
            --probe;
        } while (probe > begin &&
                 (static_cast<unsigned char>(num[probe]) & 0xC0) == 0x80);
        size_t cursor = probe;
        if (!retrieve_is_py_space(retrieve_next_cp(num, cursor))) {
            break;
        }
        end = probe;
    }
    if (begin >= end) {
        return -1;
    }

    bool negative = false;
    if (num[begin] == '+' || num[begin] == '-') {
        negative = num[begin] == '-';
        ++begin;
    }
    if (begin >= end) {
        return -1;
    }

    unsigned long long value = 0;
    bool any_digit = false;
    bool previous_digit = false;
    for (size_t i = begin; i < end; ++i) {
        const char c = num[i];
        if (c >= '0' && c <= '9') {
            const unsigned long long digit = static_cast<unsigned long long>(c - '0');
            if (value > (0x7FFFFFFFFFFFFFFFull - digit) / 10ull) {
                return -1; // out of int64 range: cannot be a turn id
            }
            value = value * 10ull + digit;
            any_digit = true;
            previous_digit = true;
        } else if (c == '_') {
            // Single underscores only, and only between digits.
            if (!previous_digit || i + 1 >= end ||
                num[i + 1] < '0' || num[i + 1] > '9') {
                return -1;
            }
            previous_digit = false;
        } else {
            return -1;
        }
    }
    if (!any_digit) {
        return -1;
    }
    const int64_t signed_value =
        negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    if (signed_value == -1) {
        return -1; // "-1" and "unparsable" share the sentinel (see header)
    }
    return signed_value;
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
    // Binding convention (py_builtin_web.cpp format_retrieve_result): an empty
    // ref_id means search mode.  `run_retrieve` does not go through this
    // wrapper -- it knows the mode from params.id, so `id=""` stays an id
    // lookup (the reference's `params.id is not None`).
    retrieve_format_turns(turns, !ref_id.empty(), ref_id, out);
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
            // memory:107-111 -- a miss is ToolOk, not an error.
            out.status = tool_status::ok;
            out.message = k_no_results_message;
            retrieve_no_results_output(true, params.id.value(), out);
            return out.status;
        }

        kimix::vector<history_turn> turns;
        turns.push_back(turn.value());
        out.status = tool_status::ok;
        out.message = retrieve_id_message(params.id.value());
        retrieve_format_turns(turns, true, params.id.value(), out);
        return out.status;
    }

    // memory:70-74: an empty / whitespace-only query is guidance, not an error.
    if (retrieve_is_blank(params.query)) {
        out.status = tool_status::ok;
        out.message = k_no_query_message;
        out.output = k_no_query_output;
        return out.status;
    }

    // Query mode: ask the shim for k*3 candidates, then boost/rank/truncate.
    kimix::vector<history_turn> candidates =
        index.search_with_recency(params.query, static_cast<int32_t>(params.k * 3));
    apply_recency_boost(candidates, 1.0, now);
    sort_and_truncate(candidates, params.k);

    out.status = tool_status::ok;
    if (candidates.empty()) {
        out.message = k_no_results_message;
        retrieve_no_results_output(false, {}, out);
        return out.status;
    }

    out.message = kimix::format("Found {} result(s)", candidates.size());
    retrieve_format_turns(candidates, false, {}, out);
    return out.status;
}

// ---------------------------------------------------------------------------
// Tool class wrapper
// ---------------------------------------------------------------------------

Retrieve::Retrieve(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

bool Retrieve::valid() const {
    const bool view_wired =
        static_cast<bool>(view.search_with_recency) &&
        static_cast<bool>(view.get_by_id);
    return tool_valid("retrieve", view_wired);
}

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
