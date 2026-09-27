// agent/tool_name_resolver.h - Hallucinated tool-name recovery (F8 / audit G03).
//
// Port of the reference's three recovery layers:
//   * kosong/tooling/__init__.py: normalize_tool_name (1433-1441),
//     _sequence_ratio (1307-1310, rapidfuzz fuzz.ratio/100), fuzzy_match_tool_name
//     (1459-1516), resolve_tool_name (1535-1601), TOOL_NAME_REDIRECTS (1616-1903)
//     + _TOOL_NAME_REDIRECTS_NORMALIZED (1907-1911), _score_argument_fit
//     (1914-2019), resolve_tool_by_arguments (2031-2091);
//   * kimi_cli/soul/toolset.py: _collect_candidates (384-426),
//     _build_platform_redirects (433-648) incl. the win32/POSIX shell block.
//
// Similarity mapping (documented): the reference scores names with
// `rapidfuzz.fuzz.ratio(a, b) / 100.0`, i.e. the normalized InDel similarity
//   ratio = 1 - indel_distance(a, b) / (len(a) + len(b))
//         = 2 * LCS(a, b) / (len(a) + len(b)),
// where indel_distance = len(a) + len(b) - 2*LCS.  sequence_ratio() below
// implements exactly that definition through an LCS dynamic program (tool
// names are short, so the O(n*m) table is bounded), which keeps every cutoff
// (0.75 auto-correct, 0.5 suggestions, 0.80/0.75 key match) semantically
// identical to the reference.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI, MSVC + GCC portable (no most-vexing-parse).

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

namespace kimix::agent {

// The reference's cutoffs (toolset.py:110 _AUTO_CORRECT_CUTOFF and
// resolve_tool_name's keyword defaults, kosong/tooling/__init__.py:1539-1542).
inline constexpr double kAutoCorrectCutoff = 0.75; // _AUTO_CORRECT_CUTOFF
inline constexpr double kSuggestCutoff = 0.5;
inline constexpr int kNSuggestions = 3;
inline constexpr int kMinNameLength = 3;
// Argument-fit acceptance (resolve_tool_by_arguments keyword defaults).
inline constexpr double kMinArgumentScore = 0.3;
inline constexpr double kScoreDelta = 0.15;

// normalize_tool_name (kosong/tooling/__init__.py:1433-1441): drop '-' and '_'
// and ASCII-casefold, so write_file / write-file / WRITE_FILE / WriteFile all
// collapse to "writefile". (The reference casefolds with str.casefold(); the
// tool vocabulary is ASCII, so ASCII lowering is exact.)
kimix::string normalize_tool_name(kimix::string_view name);

// _sequence_ratio (tooling/__init__.py:1307-1310): normalized InDel similarity
// in [0, 1] (== rapidfuzz fuzz.ratio(a, b) / 100). Both empty -> 1.0, one
// empty -> 0.0.
double sequence_ratio(kimix::string_view a, kimix::string_view b);

// fuzzy_match_tool_name (tooling/__init__.py:1459-1516). Resolution order:
//   1. normalized exact match (case- AND separator-insensitive: the
//      reference's steps 1-2 collapse into one comparison here, and every
//      spelling of the same logical name is returned);
//   2. fuzzy ratio on the normalized forms, >= cutoff, best first.
// Ties break on the raw (case-insensitive) ratio, then by candidate name.
// Names shorter than `min_length` never match. Returns up to `n` names.
kimix::vector<kimix::string>
fuzzy_match_tool_name(kimix::string_view tool_name,
                      const kimix::vector<kimix::string> &valid_names, int n,
                      double cutoff, int min_length);

// The result of resolving a (possibly mis-formatted) tool name
// (tooling/__init__.py ToolNameResolution, 1519-1532).
struct ToolNameResolution {
    kimix::string name;   // resolved real tool name; "" when nothing matched
    kimix::string original;
    bool corrected = false;
    kimix::vector<kimix::string> suggestions; // only when name is empty
};

// resolve_tool_name (tooling/__init__.py:1535-1601): exact -> redirect map ->
// high-confidence fuzzy auto-correct (>= auto_correct_cutoff) -> suggestions
// (>= suggest_cutoff). Pure and side-effect free.
ToolNameResolution
resolve_tool_name(kimix::string_view name,
                  const kimix::vector<kimix::string> &valid_names,
                  double auto_correct_cutoff = kAutoCorrectCutoff,
                  double suggest_cutoff = kSuggestCutoff,
                  int n_suggestions = kNSuggestions,
                  int min_length = kMinNameLength, bool use_redirects = true);

// _collect_candidates (toolset.py:384-426): redirect hit, normalized exact
// matches, then fuzzy matches (n=5, cutoff=0.5) - deduplicated in priority
// order.
kimix::vector<kimix::string>
collect_candidates(kimix::string_view tool_name,
                   const kimix::vector<kimix::string> &valid_names,
                   bool use_redirects = true);

// ── Argument-fit disambiguation (_score_argument_fit /
//    resolve_tool_by_arguments) ─────────────────────────────────────────────

// One tool candidate's parameter schema, reduced to what the fit scorer uses:
// the declared field names, which of them are required, and each field's JSON
// type ("string" == the reference's plain-str annotation). Parsed from a tool
// registry entry's parameters_json.
struct tool_param_schema {
    // (field name, schema "type") for every entry of "properties"; the type is
    // "" when the schema does not declare one.
    kimix::vector<std::pair<kimix::string, kimix::string>> fields;
    // (field name, "string") for fields whose declared type is a union of
    // string|null - the schema analogue of the reference's Optional[str]
    // annotations (still "plain string" for the repair pass).
    kimix::vector<std::pair<kimix::string, kimix::string>> unions;
    kimix::vector<kimix::string> required;

    bool empty() const noexcept { return fields.empty(); }
    const kimix::string *type_of(kimix::string_view field) const;
    const kimix::string *union_of(kimix::string_view field) const;
    bool is_required(kimix::string_view field) const;
    // True when the field is a plain string (the reference's
    // _is_plain_string_annotation): "type": "string", a ["string","null"]
    // union, or an anyOf/oneOf of string|null.
    bool is_plain_string(kimix::string_view field) const;
};

// Parse a registry parameters_json into the reduced schema. False when the
// text is not a JSON object with a non-empty "properties" map (the caller then
// skips the tool, like the reference skips tools without typed params).
bool parse_tool_param_schema(kimix::string_view parameters_json,
                             tool_param_schema &out);

// _score_argument_fit (tooling/__init__.py:1914-2019) score components:
//   +0.3/n_required per present required field, +0.1/n_optional per optional
//   field, +0.1 exact-name bonus, +0.05 alias/fuzzy-match bonus, -0.15 per
//   unmapped key, +0.02 per type coercion; clamped to [0, 1]. Returns 0 when
//   no argument key could be mapped onto the schema.
double score_argument_fit(kimix::string_view arguments_json,
                          const tool_param_schema &schema);

// resolve_tool_by_arguments (tooling/__init__.py:2031-2091): score the
// arguments against every candidate's schema; return a corrected resolution
// when the best score reaches kMinArgumentScore and beats the runner-up by
// kScoreDelta. A resolution with an empty name means "fall through to
// name-only matching".
ToolNameResolution
resolve_tool_by_arguments(kimix::string_view tool_name,
                          kimix::string_view arguments_json,
                          const kimix::vector<kimix::string> &candidates,
                          const kimix::function<
                              const tool_param_schema *(kimix::string_view)>
                              &schema_of);

} // namespace kimix::agent
