// test_tool_name_resolver.cpp - F8 (audit G03): hallucinated tool-name
// recovery kernels - normalize_tool_name, sequence_ratio (the documented
// rapidfuzz mapping), fuzzy_match_tool_name, the redirect table, name-only
// resolution (auto-correct at 0.75, suggestions at 0.5) and the
// argument-fit disambiguation - plus the soul-level dispatch behaviour
// (auto-corrected dispatch echoes the reference's <system-warning>; an
// unresolvable name answers the reference's not-found error).
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "agent/tool_name_resolver.h"
#include "builtin_tools/tool_registry.h"

#include <cstddef>

namespace {

using namespace boost::ut;

// Scripted chat backend: pops one canned result per chat() call (the soul is
// only driven through execute_tool_call here, so the backend stays idle).
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace() {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) /
        "kimix_test_tool_name_resolver_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// A schema provider over the live registry (mirrors soul.cpp's memoized
// lookup; test-local so it stays deterministic).
const kimix::agent::tool_param_schema *
registry_schema(kimix::string_view name) {
    static kimix::unordered_map<kimix::string, kimix::agent::tool_param_schema,
                                kimix::string_hash>
        cache;
    auto &reg = kimix::builtin_tools::ToolRegistry::instance();
    const auto *meta = reg.find(name);
    if (meta == nullptr) {
        return nullptr;
    }
    auto it = cache.find(kimix::string(name));
    if (it != cache.end()) {
        return &it->second;
    }
    kimix::agent::tool_param_schema schema;
    if (!kimix::agent::parse_tool_param_schema(meta->parameters_json, schema)) {
        return nullptr;
    }
    return &cache.emplace(kimix::string(name), std::move(schema)).first->second;
}

} // namespace

int main() {
    using namespace boost::ut;
    using namespace kimix::agent;

    "normalize_tool_name_folds_case_and_separators"_test = [] {
        expect(eq(normalize_tool_name("write_file"), kimix::string("writefile")));
        expect(eq(normalize_tool_name("Write-File"), kimix::string("writefile")));
        expect(eq(normalize_tool_name("WRITE_FILE"), kimix::string("writefile")));
        expect(eq(normalize_tool_name("WriteFile"), kimix::string("writefile")));
        expect(eq(normalize_tool_name("list_agents"), kimix::string("listagents")));
        expect(eq(normalize_tool_name(""), kimix::string("")));
    };

    "sequence_ratio_matches_the_reference_definition"_test = [] {
        // rapidfuzz fuzz.ratio/100 == 2*LCS/(len(a)+len(b)) (documented
        // mapping in tool_name_resolver.h).
        expect(sequence_ratio("", "") == 1.0_d);
        expect(sequence_ratio("abc", "") == 0.0_d);
        expect(sequence_ratio("abc", "abc") == 1.0_d);
        // identical prefix, one extra char: 2*3/(3+4)
        const double one_extra = sequence_ratio("abc", "abcd");
        expect(one_extra > 6.0 / 7.0 - 1e-9 && one_extra < 6.0 / 7.0 + 1e-9);
        // completely disjoint
        expect(sequence_ratio("abc", "xyz") == 0.0_d);
        // "readfile" vs "readfiles": 2*8/(8+9)
        const double files = sequence_ratio("readfile", "readfiles");
        expect(files > 16.0 / 17.0 - 1e-9 && files < 16.0 / 17.0 + 1e-9);
        // the ratio is byte-wise; callers normalize case first
        expect(sequence_ratio("ABC", "abc") == 0.0_d);
    };

    "fuzzy_match_ranks_by_normalized_ratio"_test = [] {
        const kimix::vector<kimix::string> names{"read", "write", "writeplan",
                                                 "readplan"};
        // A normalized-exact candidate is returned (any spelling).
        const auto plan = fuzzy_match_tool_name("write_plan", names, 3, 0.5, 3);
        expect(plan.size() == 1u);
        if (plan.size() == 1u) {
            expect(eq(plan[0], kimix::string("writeplan")));
        }
        // Typo tolerance: "reads" vs "read" scores 8/9 and clears 0.5.
        const auto typo = fuzzy_match_tool_name("reads", names, 3, 0.5, 3);
        expect(!typo.empty());
        if (!typo.empty()) {
            expect(eq(typo[0], kimix::string("read")));
        }
        // Short names never match (min_length 3, like the reference).
        expect(fuzzy_match_tool_name("wr", names, 3, 0.5, 3).empty());
        // A name below the requested min_length never matches.
        expect(fuzzy_match_tool_name("read", names, 3, 0.5, 10).empty());
    };

    "redirect_table_carries_the_reference_entries"_test = [] {
        const kimix::vector<kimix::string> names{
            "read",   "write", "edit",   "glob",  "grep",
            "read_image", "web_search", "fetch_url", "subagent",
            "send_message", "list_agents", "interrupt_agent",
            "job_output", "todo_list", "compact",
            "context_prune", "retrieve", "python", "bash", "pwsh"};
        // Legacy class names (kosong TOOL_NAME_REDIRECTS).
        expect(eq(resolve_tool_name("ReadFile", names).name,
                  kimix::string("read")));
        expect(eq(resolve_tool_name("WriteFile", names).name,
                  kimix::string("write")));
        expect(eq(resolve_tool_name("EditFile", names).name,
                  kimix::string("edit")));
        // Common hallucinations (toolset.py additions).
        expect(eq(resolve_tool_name("ViewFile", names).name,
                  kimix::string("read")));
        expect(eq(resolve_tool_name("Ls", names).name,
                  kimix::string("glob")));
        expect(eq(resolve_tool_name("Rg", names).name,
                  kimix::string("grep")));
        expect(eq(resolve_tool_name("SpawnAgent", names).name,
                  kimix::string("subagent")));
        expect(eq(resolve_tool_name("TaskOutput", names).name,
                  kimix::string("job_output")));
        expect(eq(resolve_tool_name("WebSearch", names).name,
                  kimix::string("web_search")));
        // Separator/case variants of a redirected key still hit the map.
        expect(eq(resolve_tool_name("read_file", names).name,
                  kimix::string("read")));
        // Redirects to tools outside the offered set are dropped (the
        // reference checks `redirected in valid_names`).
        const kimix::vector<kimix::string> few{"read"};
        expect(resolve_tool_name("WriteFile", few).name.empty());
        // A canonical name passes through unchanged, never "corrected".
        const auto exact = resolve_tool_name("read", names);
        expect(eq(exact.name, kimix::string("read")));
        expect(!exact.corrected);
    };

    "platform_shell_block_redirects_to_the_build_target_shell"_test = [] {
        const kimix::vector<kimix::string> names{"bash", "pwsh"};
#ifdef KIMIX_PLATFORM_WINDOWS
        const kimix::string_view expected = "pwsh";
#else
        const kimix::string_view expected = "bash";
#endif
        expect(eq(resolve_tool_name("Shell", names).name,
                  kimix::string(expected)));
        expect(eq(resolve_tool_name("Terminal", names).name,
                  kimix::string(expected)));
        expect(eq(resolve_tool_name("RunCommand", names).name,
                  kimix::string(expected)));
    };

    "resolve_tool_name_auto_corrects_only_above_the_cutoff"_test = [] {
        const kimix::vector<kimix::string> names{"read", "write", "retrieve",
                                                 "readplan", "writeplan"};
        // ratio("reads","read") = 8/9 = 0.889 >= 0.75 -> auto-correct.
        const auto corrected = resolve_tool_name("reads", names);
        expect(eq(corrected.name, kimix::string("read")));
        expect(corrected.corrected);
        // A distant name yields no correction (and, at most, suggestions).
        const auto distant = resolve_tool_name("retrieve_context_planner", names);
        expect(!distant.corrected);
        // The exact-match fast path is never flagged as a correction.
        const auto exact = resolve_tool_name("writeplan", names);
        expect(!exact.corrected);
        expect(eq(exact.name, kimix::string("writeplan")));
    };

    "collect_candidates_orders_by_confidence"_test = [] {
        const kimix::vector<kimix::string> names{"read", "readplan"};
        const auto candidates = collect_candidates("ReadFile", names);
        expect(!candidates.empty());
        if (!candidates.empty()) {
            // The redirect hit comes first.
            expect(eq(candidates.front(), kimix::string("read")));
        }
        // Deduplicated: "read" appears exactly once.
        size_t count = 0;
        for (const kimix::string &name : candidates) {
            if (name == "read") {
                ++count;
            }
        }
        expect(count == 1u);
    };

    "argument_fit_scores_the_best_matching_tool"_test = [] {
        const auto read_schema = [] {
            kimix::agent::tool_param_schema s;
            expect(parse_tool_param_schema(
                R"JSON({"type":"object","properties":{"file_path":{"type":"string"},"offset":{"type":"integer"},"limit":{"type":"integer"}},"required":["file_path"]})JSON",
                s));
            return s;
        }();
        const auto bash_schema = [] {
            kimix::agent::tool_param_schema s;
            expect(parse_tool_param_schema(
                R"JSON({"type":"object","properties":{"cmd":{"type":"string"},"timeout":{"type":"integer"}},"required":["cmd"]})JSON",
                s));
            return s;
        }();

        // Required present + exact name.
        const double fit_read =
            score_argument_fit(R"({"file_path":"a.txt"})", read_schema);
        expect(fit_read > 0.3_d);
        expect(fit_read <= 1.0_d);
        // A bash-shaped argument fits the bash schema far better than the
        // read schema (which maps none of its keys).
        const double fit_bash =
            score_argument_fit(R"({"cmd":"ls -la"})", bash_schema);
        const double fit_bash_on_read =
            score_argument_fit(R"({"cmd":"ls -la"})", read_schema);
        expect(fit_bash > fit_bash_on_read);
        // Nothing maps -> 0.0 (the reference returns (0.0, None)).
        expect(score_argument_fit(R"({"zzz":"x"})", read_schema) == 0.0_d);
    };

    "resolve_tool_by_arguments_disambiguates_by_schema_fit"_test = [] {
        const kimix::vector<kimix::string> candidates{"read", "bash"};
        // The bash schema requires "cmd": the arguments point at bash.
        const auto resolution = resolve_tool_by_arguments(
            "Shel", R"({"cmd":"git status"})", candidates, registry_schema);
        expect(eq(resolution.name, kimix::string("bash")));
        expect(resolution.corrected);
        // Without schemas nothing can be scored: no resolution.
        const auto none = resolve_tool_by_arguments(
            "Shel", R"({"cmd":"git status"})", candidates,
            [](kimix::string_view) -> const tool_param_schema * {
                return nullptr;
            });
        expect(none.name.empty());
    };

    // ── End-to-end dispatch behaviour through the soul ───────────────────

    "soul_auto_corrects_a_hallucinated_name_and_echoes_the_warning"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // "ReadFile" is a reference redirect to the read tool.
        const kimix::string out = soul.execute_tool_call(
            "ReadFile", R"JSON({"file_path":"x.txt"})JSON", err);
        expect(out.find("<system-warning>") != kimix::string::npos) << out;
        expect(out.find("Tool `ReadFile` was not found. Auto-corrected to "
                        "`read`.") != kimix::string::npos)
            << out;
        // The read tool actually ran (its not-found result, not a dispatch
        // refusal).
        expect(out.find("Tool `ReadFile` not found") == kimix::string::npos)
            << out;
        // Case-only differences resolve through the registry passes without
        // the warning (they are not corrections).
        err.clear();
        const kimix::string plain = soul.execute_tool_call(
            "READ", R"JSON({"file_path":"x.txt"})JSON", err);
        expect(plain.find("<system-warning>") == kimix::string::npos) << plain;
    };

    "soul_unknown_name_answers_the_reference_not_found_error"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // A name with no candidate above the cutoffs: the bare typed error.
        const kimix::string out = soul.execute_tool_call("zzqqxx_planner", "{}", err);
        expect(out.find("<system>ERROR: ") == 0) << out;
        expect(out.find("Tool `zzqqxx_planner` not found") != kimix::string::npos)
            << out;
        expect(err.find("not found") != kimix::string::npos) << err;
        // A name close to "read" ("reads" scores 0.889) is AUTO-CORRECTED
        // rather than suggested; suggestions only appear below 0.75.
        err.clear();
        const kimix::string corrected =
            soul.execute_tool_call("reads", "{}", err);
        expect(corrected.find("Auto-corrected to `read`") !=
               kimix::string::npos)
            << corrected;
    };

    "soul_declared_alias_wins_over_the_f8_recovery"_test = [] {
        kimix::agent::AgentSession session(tmp_workspace());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // "Shell" is a DECLARED alias of the bash tool, so the registry's own
        // alias passes resolve it - no auto-correct, no <system-warning>.
        const kimix::string out = soul.execute_tool_call("Shell", "{}", err);
        expect(out.find("<system-warning>") == kimix::string::npos) << out;
        // The shell tool actually ran (its validation error payload).
        expect(out.find("missing required") != kimix::string::npos ||
               out.find("not available in this environment") !=
                   kimix::string::npos)
            << out;
    };

    return 0;
}
