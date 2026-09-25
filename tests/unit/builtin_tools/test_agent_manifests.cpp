// Test for the kimi-agent agent_*.json manifests against ToolRegistry.
// This test covers:
// - Locating the kimi-agent checkout (env KIMI_AGENT_ROOT, else
//   C:/dev/kimi-agent, else D:/kimi-agent); skipping with a notice when none
//   exists (mirrors the python/tests parity-test convention).
// - Parsing each agent_<role>.json with yyjson_read_opts + kYYJsonAlcMi,
//   extracting agent.extend (must be a string) and the agent.tools array.
// - Resolving every "<module>:<attr>" tool entry through ToolRegistry::resolve
//   on the attr after the LAST ':' (the fuzzy resolution: exact canonical ->
//   case-insensitive canonical -> exact alias -> folded alias), asserting each
//   resolves to a meta with a non-empty factory.
// - Classifying the resolution mechanism per tool (exact canonical / CI
//   canonical / alias exact / alias folded) and recording tool counts.
// - Malformed-JSON robustness: broken entries (missing ':', empty attr,
//   non-string entries, invalid JSON) are reported as unresolved WITHOUT
//   crashing, via check_manifest_text() unit-tested with inline JSON so most
//   of the coverage runs even without the checkout.
//
// Distinction from test_tool.cpp's "registry_covers_every_agent_json_tool":
// that test checks a hardcoded static copy of the union table; this test
// parses the actual JSON files on disk and is therefore sensitive to manifest
// drift in the reference checkout.
#include "ut/ut.hpp"

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_registry.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;

namespace {

// Which leg of ToolRegistry::resolve() accepted a manifest tool attr.
enum class resolve_mechanism {
    exact_canonical, // (a) attr == meta.name, byte for byte
    ci_canonical,    // (b) attr equals meta.name ASCII case-insensitively
    alias_exact,     // (c) attr equals one of meta.aliases exactly
    alias_folded,    // (d) attr equals one of meta.aliases folded
    unresolved,      // resolve() found nothing
};

const char *mechanism_name(resolve_mechanism m) {
    switch (m) {
    case resolve_mechanism::exact_canonical: return "exact canonical";
    case resolve_mechanism::ci_canonical: return "canonical (case-insensitive)";
    case resolve_mechanism::alias_exact: return "alias (exact)";
    case resolve_mechanism::alias_folded: return "alias (folded)";
    default: return "UNRESOLVED";
    }
}

// Replicates the resolve() classification so the test can report HOW each
// manifest tool was accepted (never crashes on nullptr meta).
resolve_mechanism classify_resolution(const ToolMeta *meta, kimix::string_view attr) {
    if (meta == nullptr) {
        return resolve_mechanism::unresolved;
    }
    if (meta->name == attr) {
        return resolve_mechanism::exact_canonical;
    }
    if (meta->name.size() == attr.size()) {
        bool ci = true;
        for (size_t i = 0; i < attr.size(); ++i) {
            const char a = attr[i];
            const char b = meta->name[i];
            const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
            const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
            if (la != lb) {
                ci = false;
                break;
            }
        }
        if (ci) {
            return resolve_mechanism::ci_canonical;
        }
    }
    bool alias_exact = false;
    bool alias_folded = false;
    alias_detail::for_each_alias_name(meta->aliases, [&](kimix::string_view alias) {
        if (alias == attr) {
            alias_exact = true;
        }
        if (alias_detail::alias_name_equals(alias, attr)) {
            alias_folded = true;
        }
    });
    if (alias_exact) {
        return resolve_mechanism::alias_exact;
    }
    if (alias_folded) {
        return resolve_mechanism::alias_folded;
    }
    return resolve_mechanism::unresolved;
}

// Aggregate result of checking one manifest text.
struct manifest_stats {
    kimix::string extend;          // agent.extend value ("" when not a string)
    bool extend_is_string = false;
    size_t tool_count = 0;         // entries in agent.tools
    size_t resolved = 0;           // entries that resolved with a live factory
    size_t exact = 0;              // resolved via exact canonical name
    size_t ci = 0;                 // resolved via case-insensitive canonical name
    size_t alias = 0;              // resolved via an alias (exact or folded)
    size_t unresolved = 0;         // entries that failed to resolve
};

// Core checker: parse one manifest text and resolve every tool entry through
// the registry. Never crashes on malformed input; failures surface as failed
// Boost.UT expects plus nonzero `unresolved`. Returns false when the text is
// not even valid JSON with an object root.
bool check_manifest_text(const char *label, kimix::string text,
                         manifest_stats &st) {
    // yyjson_read_opts takes a mutable buffer (the vendored fork signature);
    // the text is a private copy, so handing over data() is safe.
      yyjson_doc *doc = yyjson_read_opts(text.data(), text.size(), 0,
                                         &kimix::llm::kYYJsonAlcMi, nullptr);
      // A non-JSON document is reported through the return value (false), not
      // an expect(): the robustness test feeds garbage on purpose and asserts
      // the graceful-false path. Real manifests still fail the suite via the
      // caller's expect(ok).
      if (doc == nullptr) {
          printf("  %s: manifest does not parse as JSON\n", label);
          return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const bool root_is_obj = yyjson_is_obj(root);
    expect(root_is_obj) << label << ": manifest root is a JSON object";
    if (!root_is_obj) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *agent = yyjson_obj_get(root, "agent");
    const bool agent_is_obj = yyjson_is_obj(agent);
    expect(agent_is_obj) << label << ": manifest has an 'agent' object";
    if (!agent_is_obj) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *extend = yyjson_obj_get(agent, "extend");
    st.extend_is_string = yyjson_is_str(extend);
    expect(st.extend_is_string) << label << ": agent.extend is a string";
    if (st.extend_is_string) {
        st.extend = kimix::string(yyjson_get_str(extend), yyjson_get_len(extend));
    }
    yyjson_val *tools = yyjson_obj_get(agent, "tools");
    const bool tools_is_arr = yyjson_is_arr(tools);
    expect(tools_is_arr) << label << ": agent.tools is an array";
    if (!tools_is_arr) {
        yyjson_doc_free(doc);
        return false;
    }
    auto &reg = ToolRegistry::instance();
    size_t idx = 0, max = 0;
    yyjson_val *entry = nullptr;
    yyjson_arr_foreach(tools, idx, max, entry) {
        ++st.tool_count;
          const bool entry_is_str = yyjson_is_str(entry);
          // Reported, not expected: malformed entries are an accepted input
          // of this checker (the robustness test feeds one on purpose); the
          // aggregate `unresolved` count drives the caller's assertion.
          if (!entry_is_str) {
              printf("  %s: tool entry #%zu is not a string\n", label, idx);
              ++st.unresolved;
              continue;
          }
        const kimix::string_view tool_entry(yyjson_get_str(entry), yyjson_get_len(entry));
        // Split on the LAST ':' (Python rsplit(":", 1)); the attr part is
        // what the registry keys on. Missing ':' or empty attr -> malformed
        // entry: report as unresolved, keep going.
          const size_t colon = tool_entry.rfind(':');
          if (colon == kimix::string_view::npos || colon + 1 >= tool_entry.size()) {
              // Reported, not expected: malformed entries are an accepted
              // input of this checker (see the robustness test).
              printf("  %s: tool entry '%.*s' is malformed (missing ':' or empty "
                     "attr)\n",
                     label, static_cast<int>(tool_entry.size()), tool_entry.data());
              ++st.unresolved;
              continue;
          }
        const kimix::string_view attr = tool_entry.substr(colon + 1);
        const ToolMeta *meta = reg.resolve(attr);
        expect(meta != nullptr)
            << label << ": tool '" << tool_entry << "' (attr '" << attr
            << "') resolves in the registry";
        if (meta == nullptr) {
            ++st.unresolved;
            continue;
        }
        expect(static_cast<bool>(meta->factory))
            << label << ": '" << attr << "' has a non-empty factory";
        const resolve_mechanism mech = classify_resolution(meta, attr);
        printf("  %.*s -> %-14s [%s]\n", static_cast<int>(tool_entry.size()),
               tool_entry.data(), meta->name.c_str(), mechanism_name(mech));
        switch (mech) {
        case resolve_mechanism::exact_canonical:
            ++st.exact;
            break;
        case resolve_mechanism::ci_canonical:
            ++st.ci;
            break;
        case resolve_mechanism::alias_exact:
        case resolve_mechanism::alias_folded:
            ++st.alias;
            break;
        default:
            break;
        }
        if (mech != resolve_mechanism::unresolved && meta->factory) {
            ++st.resolved;
        } else {
            ++st.unresolved;
        }
    }
    yyjson_doc_free(doc);
    return st.unresolved == 0;
}

// Read a whole file (binary) into a kimix::string. False when unreadable.
bool read_file_text(const kimix::string &path, kimix::string &out) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string tmp = ss.str();
    out.assign(tmp.data(), tmp.size());
    return true;
}

// Locate the kimi-agent checkout: env override, then default candidates.
// Returns "" when none exists (callers print the skip notice).
kimix::string find_kimi_agent_root() {
    static const char *k_candidates[] = {"C:/dev/kimi-agent", "D:/kimi-agent"};
    if (const char *env = std::getenv("KIMI_AGENT_ROOT")) {
        if (env[0] != '\0') {
            return kimix::string(env);
        }
    }
    for (const char *cand : k_candidates) {
        kimix::string probe = kimix::string(cand) + "/src/kimix/agent_worker.json";
        kimix::string text;
        if (read_file_text(probe, text)) {
            return kimix::string(cand);
        }
    }
    return kimix::string();
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // Robustness of the manifest checker itself: these run without the
    // kimi-agent checkout. A broken tool entry must be reported as
    // unresolved and must NOT crash the check.
    "manifest_checker_robustness"_test = [] {
        manifest_stats st;

        // Valid minimal manifest: both entries resolve.
        expect(check_manifest_text("inline-valid",
                                   kimix::string(R"json(
            {"agent": {"extend": "default", "tools": [
                "kimix.tools.file.bash:bash",
                "kimix.tools.file.run:Run"
            ]}})json"),
                                   st));
        expect(st.tool_count == size_t(2));
        expect(st.resolved == size_t(2));
        expect(st.unresolved == size_t(0));
        expect(st.extend_is_string);
        expect(st.extend == "default");

        // Broken entry: trailing ':' (empty attr) -> unresolved, no crash.
        st = manifest_stats{};
        expect(!check_manifest_text("inline-empty-attr",
                                    kimix::string(R"json(
            {"agent": {"extend": "default", "tools": [
                "kimix.tools.file.bash:"
            ]}})json"),
                                    st));
        expect(st.tool_count == size_t(1));
        expect(st.resolved == size_t(0));
        expect(st.unresolved == size_t(1));

        // Broken entry: no ':' at all -> unresolved, no crash.
        st = manifest_stats{};
        expect(!check_manifest_text("inline-no-colon",
                                    kimix::string(R"json(
            {"agent": {"extend": "default", "tools": [
                "not_a_tool_path"
            ]}})json"),
                                    st));
        expect(st.unresolved == size_t(1));

        // Broken entry: non-string tool entry -> unresolved, no crash.
        st = manifest_stats{};
        expect(!check_manifest_text("inline-non-string",
                                    kimix::string(R"json(
            {"agent": {"extend": "default", "tools": [42]}})json"),
                                    st));
        expect(st.tool_count == size_t(1));
        expect(st.unresolved == size_t(1));

        // Not JSON at all: the checker reports false and does not crash.
        st = manifest_stats{};
        expect(!check_manifest_text("inline-garbage",
                                    kimix::string("this is not json"),
                                    st));
        expect(st.tool_count == size_t(0));
    };

    // The real acceptance check: parse the 5 kimi-agent manifests on disk and
    // resolve every tool entry through the actual registry.
    "agent_manifest_files"_test = [] {
        const kimix::string root = find_kimi_agent_root();
        static const char *k_manifests[] = {"agent_worker", "agent_planner",
                                            "agent_boss", "agent_subagent",
                                            "agent_readonly"};
        size_t present = 0;
        size_t all_resolved = 0;
        for (const char *name : k_manifests) {
            const kimix::string path = root.empty()
                                           ? kimix::string()
                                           : kimix::string(root) + "/src/kimix/" +
                                                 name + ".json";
            kimix::string text;
            if (!read_file_text(path, text)) {
                printf("  %s: manifest not found at %s (skipped)\n", name,
                       path.c_str());
                continue;
            }
            ++present;
            printf("== %s (%s)\n", name, path.c_str());
            manifest_stats st;
            const bool ok = check_manifest_text(name, text, st);
            expect(ok) << name << ": every tool entry resolves";
            expect(st.extend_is_string) << name << ": extend is a string";
            printf("  %s: extend=%s tools=%zu resolved=%zu (exact=%zu ci=%zu "
                   "alias=%zu unresolved=%zu)\n",
                   name, st.extend_is_string ? st.extend.c_str() : "<none>",
                   st.tool_count, st.resolved, st.exact, st.ci, st.alias,
                   st.unresolved);
            all_resolved += ok ? 1 : 0;
        }
        if (present == 0) {
            // Mirrors the python/tests parity skip convention: no kimi-agent
            // checkout available -> notice + pass.
            printf("SKIP: KIMI_AGENT_ROOT not set and no default candidate "
                   "(C:/dev/kimi-agent, D:/kimi-agent); agent manifest checks "
                   "skipped. The inline robustness test above still ran.\n");
            return;
        }
        expect(present == size_t(5)) << "all 5 agent manifests found under " << root;
        expect(all_resolved == present)
            << "every tool in every present manifest resolves via the registry";
    };
}
