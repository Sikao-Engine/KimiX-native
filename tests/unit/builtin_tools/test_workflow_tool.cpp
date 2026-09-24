// Test for builtin_tools/workflow_tool.h
// (namespace kimix::builtin_tools::workflow).
//
// Covers:
// - parse_params: every AgentSwarmParams._validate branch and its verbatim
//   ValueError wording (fanout item/resume counts, the 128 cap, the
//   template-XOR-prefix rules, the '{{item}}' requirement, and all three
//   parallel_sample rules), plus the mode/selector Literal validation
// - expand_template: {{item}} substitution, prefix+suffix, bare items
// - validate_uniqueness: duplicate detection and the set-repr message
// - xml_escape: html.escape(quote=True) incl. the &#x27; apostrophe
// - render_results: the full <agent_swarm_result> document, the resume_hint
//   gate, the single-space inner indentation and the elapsed "-" fallback
// - render_best_of_n: <best_of_n_result> incl. the double-escaped failure
// - is_rate_limit_error / retry_delay_seconds
// - rate_limiter: the token-bucket burst + refill arithmetic
// - format_candidates_for_review / all_candidates_failed_message /
//   single_run_failed_message / verification_rejected_message / format_votes
// - select_best_candidate: no viable candidate, single viable, majority
//   pairwise voting with the (votes, -index) tie-break, self-eval and the
//   invalid-index fallback
// - run_parallel_sample: index ordering, the "[workspace:<kind>]" diff marker,
//   per-worker failure isolation
// - best_of_n: the n<=1 degenerate path, apply + verify, verification
//   rejection
// - run_swarm: ordering by index and the retry/backoff hook
// - Workflow Tool wrapper: the recursion guard, the swarm-session gate,
//   validation errors, a fanout run with a scripted runner and a
//   parallel_sample run with scripted runner + selector
//
// All test logic lives in main() scope; no file-scope static registrations.
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/tool_registry.h"
#include "builtin_tools/workflow_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::workflow;

namespace {

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

std::string sv_of(const kimix::string &s) {
    return std::string(s.data(), s.size());
}

std::string json_of(const kimix::vector<char> &buf) {
    return std::string(buf.data(), buf.size());
}

bool has(const kimix::vector<char> &buf, std::string_view needle) {
    return json_of(buf).find(std::string(needle)) != std::string::npos;
}

kimix::vector<kimix::string> kvec(std::vector<std::string> values) {
    kimix::vector<kimix::string> out;
    out.reserve(values.size());
    for (const std::string &v : values) {
        out.push_back(kix(v));
    }
    return out;
}

sample_candidate make_candidate(int32_t index, bool success,
                                std::string_view report,
                                std::string_view diff,
                                std::string_view error = "") {
    sample_candidate c;
    c.index = index;
    c.success = success;
    c.self_report = kix(report);
    c.diff = kix(diff);
    c.steps = 2;
    if (!error.empty()) {
        c.error = kix(error);
    }
    return c;
}

// Scripted swarm runner recording every task it received.
struct fake_swarm {
    kimix::shared_ptr<std::vector<swarm_task>> calls =
        kimix::shared_ptr<std::vector<swarm_task>>(
            new std::vector<swarm_task>());
    kimix::shared_ptr<std::vector<kimix::string>> types =
        kimix::shared_ptr<std::vector<kimix::string>>(
            new std::vector<kimix::string>());
    // prompt substring -> failure message ("" == success)
    kimix::shared_ptr<std::vector<std::pair<std::string, std::string>>> rules =
        kimix::shared_ptr<std::vector<std::pair<std::string, std::string>>>(
            new std::vector<std::pair<std::string, std::string>>());

    swarm_runner fn() {
        auto c = calls;
        auto t = types;
        auto r = rules;
        return [c, t, r](const swarm_task &task,
                         kimix::string_view type) {
            c->push_back(task);
            t->emplace_back(type);
            swarm_result out;
            out.index = task.index;
            out.agent_id = task.agent_id.value_or(kimix::string("agent-x"));
            out.success = true;
            out.output = "result for " + task.prompt;
            const std::string prompt(task.prompt.data(), task.prompt.size());
            for (const auto &rule : *r) {
                if (prompt.find(rule.first) != std::string::npos) {
                    if (!rule.second.empty()) {
                        out.success = false;
                        out.output = rule.second;
                        out.error = kix(rule.second);
                    }
                    break;
                }
            }
            out.elapsed = 1.5;
            return out;
        };
    }
};

} // namespace

/* -------------------------------------------------------------------------
 * Python-derived golden vectors
 * -------------------------------------------------------------------------
 * tests/unit/builtin_tools/workflow_goldens.inc is generated by
 * scripts/gen_workflow_goldens.py, which drives the *real* kimi-agent
 * implementation (kimix.tools.swarm.AgentSwarm + kimix.tools.swarm.best_of_n)
 * with stub sessions, sub-agent runners, selectors, worker workspaces and a
 * fixed-step clock, and records every byte the port must reproduce.
 *
 * The replay below feeds the C++ port the same arguments and compares every
 * column: the tool-level envelope (ok/status/message/brief/output), the prompts
 * that were dispatched, and every pure kernel the port exposes.
 * ------------------------------------------------------------------------- */
#include "workflow_goldens.inc"

namespace {

// Parse a golden JSON fragment. ToolParams only accepts object roots, so array
// and scalar fragments are wrapped in {"v": ...} and unwrapped again.
bool golden_parse(const char *json, ValueElement &out) {
    kimix::string wrapped("{\"v\":");
    wrapped += json;
    wrapped += "}";
    ToolParams tp;
    kimix::string err;
    if (!tp.try_deserialize(
            kimix::span<char const>(wrapped.data(), wrapped.size()), err)) {
        return false;
    }
    const ValueElement *el = tp.get_exact("v");
    if (el == nullptr) {
        return false;
    }
    out = *el;
    return true;
}

std::string g_str(const ValueElement *el) {
    if (el == nullptr || !el->is_string()) {
        return {};
    }
    const kimix::string &s = el->as_string();
    return std::string(s.data(), s.size());
}

bool g_int(const ValueElement *el, int64_t &out) {
    if (el == nullptr) {
        return false;
    }
    if (el->is_int()) {
        out = el->as_int();
        return true;
    }
    if (el->is_uint()) {
        out = static_cast<int64_t>(el->as_uint());
        return true;
    }
    return false;
}

bool g_num(const ValueElement *el, double &out) {
    if (el == nullptr) {
        return false;
    }
    if (el->is_real()) {
        out = el->as_real();
        return true;
    }
    int64_t i = 0;
    if (g_int(el, i)) {
        out = static_cast<double>(i);
        return true;
    }
    return false;
}

// Python's str()/f-string rendering of an optional sample error: a missing
// error is "None" there, "" in the C++ port's fallback - the goldens pin the
// Python wording.
std::string g_opt_error(const ValueElement *el) {
    if (el == nullptr || el->is_null()) {
        return "None";
    }
    if (el->is_string()) {
        return g_str(el);
    }
    return {};
}

kimix::vector<kimix::string> g_str_list(const char *json) {
    kimix::vector<kimix::string> out;
    ValueElement el;
    if (!golden_parse(json, el) || !el.is_array()) {
        return out;
    }
    for (const ValueElement &item : el.as_array()) {
        out.push_back(item.is_string() ? item.as_string() : kimix::string());
    }
    return out;
}

kimix::vector<kimix::string> sorted(kimix::vector<kimix::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

std::string strip_exc_prefix(const char *text) {
    const std::string s(text);
    const size_t pos = s.find(": ");
    if (pos == std::string::npos) {
        return s;
    }
    // "AllCandidatesFailedError: <msg>" / "VerificationRejectedError: <msg>"
    const std::string head = s.substr(0, pos);
    if (head.find("Error") == std::string::npos) {
        return s;
    }
    return s.substr(pos + 2);
}

// -------------------------------------------------------------------------
// The scripted runner shared by every golden case: a prompt is served by the
// first rule group whose `match` is a substring of it, and each group consumes
// its steps in order (the last one repeats).
// -------------------------------------------------------------------------
struct wf_script {
    const wf_golden_rule *rules = nullptr;
    int32_t count = 0;
    kimix::vector<int32_t> group_start;
    kimix::vector<int32_t> group_steps;
    kimix::vector<int32_t> used;
    kimix::vector<kimix::string> calls;
    std::mutex mu;

    wf_script() = default;
    wf_script(const wf_golden_rule *r, int32_t n) { init(r, n); }

    void init(const wf_golden_rule *r, int32_t n) {
        rules = r;
        count = n;
        group_start.clear();
        group_steps.clear();
        used.clear();
        calls.clear();
        for (int32_t i = 0; i < n; ++i) {
            // A new group starts where the match pattern changes. The i == 0
            // case must not evaluate rules[i - 1] at all.
            bool start_group = (i == 0);
            if (!start_group) {
                start_group = std::string(rules[i].match) !=
                              std::string(rules[i - 1].match);
            }
            if (start_group) {
                group_start.push_back(i);
                group_steps.push_back(0);
            }
            const int32_t need = rules[i].seq + 1;
            if (need > group_steps.back()) {
                group_steps.back() = need;
            }
        }
        used.assign(group_start.size(), 0);
    }

    static kimix::string substitute(kimix::string text,
                                    std::string_view prompt) {
        const std::string needle("{prompt}");
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), prompt.data(), prompt.size());
            pos += prompt.size();
        }
        return text;
    }

    // Resolve one call. Returns the rule; `out` is the sub-agent text and
    // `error` (when non-empty) the failure message.
    const wf_golden_rule *next(std::string_view prompt, kimix::string &out,
                               kimix::string &error, bool &failed) {
        std::lock_guard<std::mutex> lock(mu);
        calls.emplace_back(prompt.data(), prompt.size());
        const wf_golden_rule *rule = nullptr;
        if (count > 0) {
            for (size_t g = 0; g < group_start.size(); ++g) {
                const std::string match(rules[group_start[g]].match);
                if (match.empty() || prompt.find(match) != std::string_view::npos) {
                    const int32_t step = std::min(used[g], group_steps[g] - 1);
                    used[g] += 1;
                    rule = &rules[group_start[g] + step];
                    break;
                }
            }
        }
        failed = false;
        out.clear();
        error.clear();
        if (rule == nullptr) {
            return nullptr;
        }
        const std::string kind(rule->kind);
        const kimix::string text = substitute(rule->text, prompt);
        if (kind == "echo") {
            out = kimix::string("report for ") + kimix::string(prompt);
        } else if (kind == "text") {
            out = text;
        } else if (kind == "silent") {
            out.clear();
        } else if (kind == "pick_b") {
            const std::string head("Candidate B (#");
            const size_t at = std::string(prompt).find(head);
            std::string digits;
            if (at != std::string::npos) {
                size_t i = at + head.size();
                const std::string p(prompt);
                while (i < p.size() && p[i] >= '0' && p[i] <= '9') {
                    digits.push_back(p[i]);
                    ++i;
                }
            }
            out = digits.empty() ? "0" : digits;
        } else if (kind == "fail") {
            failed = true;
            out = text;
            error = text;
        }
        return rule;
    }
};

// index -> elapsed seconds ("-" means "no elapsed").
std::map<int64_t, double> g_elapsed_map(const char *json) {
    std::map<int64_t, double> out;
    ValueElement el;
    if (!golden_parse(json, el) || !el.is_object()) {
        return out;
    }
    const ToolParams *obj = el.as_object();
    for (const auto &kv : obj->values) {
        const std::string key(kv.first.data(), kv.first.size());
        const std::string value = g_str(&kv.second);
        if (value.empty() || value == "-") {
            continue;
        }
        if (value.back() == 's') {
            out[std::strtoll(key.c_str(), nullptr, 10)] =
                std::strtod(value.c_str(), nullptr);
        }
    }
    return out;
}

// swarm_runner replaying `script`; elapsed comes from the golden (the wall
// clock itself is not reproducible).
swarm_runner g_swarm_runner(wf_script &script,
                            const std::map<int64_t, double> &elapsed) {
    return [&script, &elapsed](const swarm_task &task,
                               kimix::string_view) {
        kimix::string text;
        kimix::string error;
        bool failed = false;
        script.next(std::string_view(task.prompt.data(), task.prompt.size()),
                    text, error, failed);
        swarm_result out;
        out.index = task.index;
        out.agent_id = task.agent_id.has_value()
                           ? *task.agent_id
                           : kimix::format("agent-{}", task.index);
        // _run_subagent_task: `if not output_text: output_text =
        // "(no text output)"`
        out.output = (!failed && text.empty())
                         ? kimix::string("(no text output)")
                         : text;
        out.success = !failed;
        if (failed) {
            out.error = error;
        }
        const auto hit = elapsed.find(task.index);
        if (hit != elapsed.end()) {
            out.elapsed = hit->second;
        }
        return out;
    };
}

sample_runner g_sample_runner(wf_script &script) {
    return [&script](kimix::string_view prompt,
                     kimix::string_view) -> sample_run_outcome {
        kimix::string text;
        kimix::string error;
        bool failed = false;
        script.next(prompt, text, error, failed);
        sample_run_outcome out;
        out.ok = !failed;
        out.self_report = text;
        out.error = error;
        out.steps = 1;
        out.output_tokens = 2;
        return out;
    };
}

// The generator's workspace stubs, mirrored: worker "/wf/w<i>", kind "copy",
// a deterministic diff, and an apply log.
struct g_workspaces {
    kimix::vector<kimix::string> applied;
    workspace_hooks hooks;
};


workspace_hooks g_workspace_hooks(g_workspaces &state) {
    workspace_hooks hooks;
    hooks.create = [](kimix::string_view, int32_t index) {
        return std::make_pair(kimix::format("/wf/w{}", index),
                              kimix::string("copy"));
    };
    hooks.cleanup = [](kimix::string_view, kimix::string_view,
                       kimix::string_view) {};
    hooks.collect_diff = [](kimix::string_view worker_path, kimix::string_view,
                            kimix::string_view) {
        const std::string path(worker_path.data(), worker_path.size());
        const size_t slash = path.find_last_of('/');
        const std::string name =
            (slash == std::string::npos) ? path : path.substr(slash + 1);
        return kimix::string("diff for worker " + name + "\nline2\n");
    };
    hooks.apply = [&state](const sample_candidate &winner,
                           kimix::string_view, kimix::string_view kind) {
        state.applied.push_back(kimix::format(
            "apply {} {}", winner.index,
            kimix::string(kind.data(), kind.size())));
    };
    return hooks;
}

// Build a sample_candidate from a golden JSON object.
sample_candidate g_candidate(const ValueElement &el, bool with_workspace = true) {
    sample_candidate c;
    int64_t value = 0;
    if (g_int(el.as_object()->get("index"), value)) {
        c.index = static_cast<int32_t>(value);
    }
    const ValueElement *success = el.as_object()->get("success");
    c.success = (success != nullptr && success->is_bool() && success->as_bool());
    if (!with_workspace) {
        return c;
    }
    const ValueElement *error = el.as_object()->get("error");
    if (error != nullptr && error->is_string()) {
        c.error = error->as_string();
    }
    const ValueElement *report = el.as_object()->get("report");
    if (report != nullptr && report->is_string()) {
        c.self_report = report->as_string();
    }
    const ValueElement *steps = el.as_object()->get("steps");
    g_int(steps, value);
    c.steps = static_cast<int32_t>(value);
    const ValueElement *tokens = el.as_object()->get("tokens");
    if (g_int(tokens, value)) {
        c.output_tokens = value;
    }
    const ValueElement *diff = el.as_object()->get("diff");
    if (diff != nullptr && diff->is_string()) {
        c.diff = diff->as_string();
    } else {
        c.diff = kimix::format("[workspace:copy]\nd{}", c.index);
    }
    c.work_dir = kimix::format("/wf/w{}", c.index);
    return c;
}

kimix::vector<sample_candidate> g_candidates(const char *json) {
    kimix::vector<sample_candidate> out;
    ValueElement el;
    if (!golden_parse(json, el) || !el.is_array()) {
        return out;
    }
    for (const ValueElement &item : el.as_array()) {
        out.push_back(g_candidate(item));
    }
    return out;
}

kimix::vector<kimix::string> g_kvec(const char *json) {
    return g_str_list(json);
}

// Field-by-field comparison of one replayed candidate with its golden.
void expect_candidate(const ValueElement &want, const sample_candidate &got,
                      std::string_view ctx) {
    int64_t index = -1;
    g_int(want.as_object()->get("index"), index);
    expect(got.index == static_cast<int32_t>(index)) << ctx;
    const ValueElement *success = want.as_object()->get("success");
    const bool want_success =
        success != nullptr && success->is_bool() && success->as_bool();
    expect(got.success == want_success) << ctx;
    const ValueElement *error = want.as_object()->get("error");
    const bool want_error_null = (error == nullptr || error->is_null());
    expect(got.error.has_value() == !want_error_null) << ctx;
    if (!want_error_null && got.error.has_value()) {
        expect(*got.error == kix(g_str(error))) << ctx;
    }
    const ValueElement *report = want.as_object()->get("report");
    if (report != nullptr) {
        expect(got.self_report == kix(g_str(report))) << ctx;
    }
    int64_t value = 0;
    if (g_int(want.as_object()->get("steps"), value)) {
        expect(got.steps == static_cast<int32_t>(value)) << ctx;
    }
    if (g_int(want.as_object()->get("tokens"), value)) {
        expect(got.output_tokens == value) << ctx;
    }
    const ValueElement *diff = want.as_object()->get("diff");
    if (diff != nullptr) {
        expect(got.diff == kix(g_str(diff))) << ctx;
    }
}

void expect_candidates(const char *json,
                       const kimix::vector<sample_candidate> &got,
                       std::string_view ctx) {
    ValueElement el;
    const bool parsed = golden_parse(json, el);
    expect(parsed) << ctx;
    if (!parsed || !el.is_array()) {
        return;
    }
    expect(got.size() == el.as_array().size()) << ctx;
    for (size_t i = 0; i < got.size() && i < el.as_array().size(); ++i) {
        expect_candidate(el.as_array()[i], got[i], ctx);
    }
}

bool g_has_parallel_sample_mode(const char *args) {
    ValueElement el;
    if (!golden_parse(args, el) || !el.is_object()) {
        return false;
    }
    const ValueElement *mode = el.as_object()->get("mode");
    return mode != nullptr && mode->is_string() &&
           mode->as_string() == "parallel_sample";
}

kimix::string g_kstr(const char *text) { return kimix::string(text); }

// Compare two texts and, on mismatch, print the first differing byte so a
// golden failure is diagnosable without re-running the generator.
void expect_text(const kimix::string &got, const char *want,
                 std::string_view ctx, const char *label) {
    const std::string g(got.data(), got.size());
    const std::string w(want);
    if (g == w) {
        expect(true) << ctx;
        return;
    }
    size_t at = 0;
    while (at < g.size() && at < w.size() && g[at] == w[at]) {
        ++at;
    }
    const size_t from = (at > 60) ? at - 60 : 0;
    std::fprintf(stderr, "[diff] %.*s %s at byte %zu (got %zu bytes, want %zu)\n",
                 static_cast<int>(ctx.size()), ctx.data(), label, at, g.size(),
                 w.size());
    std::fprintf(stderr, "  want: %s\n", w.substr(from, 120).c_str());
    std::fprintf(stderr, "  got : %s\n", g.substr(from, 120).c_str());
    size_t reported = 0;
    const size_t max = (g.size() > w.size()) ? g.size() : w.size();
    for (size_t i = 0; i < max && reported < 6; ++i) {
        const char gc = (i < g.size()) ? g[i] : '\0';
        const char wc = (i < w.size()) ? w[i] : '\0';
        if (gc != wc) {
            std::fprintf(stderr, "  byte %zu: got %02x want %02x\n", i,
                         static_cast<unsigned char>(gc),
                         static_cast<unsigned char>(wc));
            ++reported;
        }
    }
    std::fflush(stderr);
    expect(false) << ctx << " " << label;
}

// Read one field of a serialized tool result envelope.
kimix::string res_str(const ToolParams &res, const char *key) {
    const ValueElement *el = res.get_exact(key);
    return (el != nullptr && el->is_string()) ? el->as_string()
                                             : kimix::string();
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // parse_params - fanout validation
    // -----------------------------------------------------------------------
    "params_fanout_happy_path"_test = [] {
        ToolParams params;
        params.values["description"] =
            ValueElement::make_string(kix("audit everything"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix {{item}}"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a.cpp")));
        items.push_back(ValueElement::make_string(kix("b.cpp")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.description == kix("audit everything"));
        expect(out.mode == kix("fanout"));
        expect(out.subagent_type == kix("coder"));
        expect(out.items.size() == 2u);
    };

    "params_requires_description"_test = [] {
        ToolParams params;
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("missing required field: description"));
    };

    "params_needs_two_items_or_resumes"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix {{item}}"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("only-one")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message ==
               kix("Provide at least 2 items or resume_agent_ids."));
    };

    "params_single_item_with_resume_is_allowed"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix {{item}}"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("one")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        kimix::shared_ptr<ToolParams> resumes(new ToolParams());
        resumes->values["agent-1"] = ValueElement::make_string(kix("retry"));
        params.values["resume_agent_ids"] =
            ValueElement::make_object(std::move(resumes));
        workflow_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.resume_agent_ids.size() == 1u);
        expect(out.resume_agent_ids[0].first == kix("agent-1"));
        expect(out.resume_agent_ids[0].second == kix("retry"));
    };

    "params_sub_agent_cap"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Fix "));
        ValueElement::Array items;
        for (int i = 0; i < 129; ++i) {
            items.push_back(
                ValueElement::make_string(kimix::format("item{}", i)));
        }
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("Max 128 sub-agents per swarm."));
    };

    "params_template_and_prefix_are_exclusive"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix {{item}}"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Fix "));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a")));
        items.push_back(ValueElement::make_string(kix("b")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("Use either prompt_template or "
                                  "prompt_prefix+suffix, not both."));
    };

    "params_needs_template_or_prefix"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a")));
        items.push_back(ValueElement::make_string(kix("b")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message ==
               kix("prompt_template must contain '{{item}}', or set "
                   "prompt_prefix. For example: prompt_template='Fix errors "
                   "in {{item}}.'"));
    };

    "params_template_without_placeholder"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        // A template WITHOUT {{item}} and without a prefix hits the
        // "neither" branch (uses_template requires the placeholder).
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix everything"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a")));
        items.push_back(ValueElement::make_string(kix("b")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find(
                   "prompt_template must contain '{{item}}', or set "
                   "prompt_prefix.") == 0);
    };

    "params_bad_mode_and_selector"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["mode"] = ValueElement::make_string(kix("sideways"));
        workflow_params out;
        const tool_error mode_err = parse_params(&params, out);
        expect(mode_err.failed());
        expect(sv_of(mode_err.message).find("'fanout' or 'parallel_sample'") !=
               std::string::npos);
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("do the thing"));
        params.values["selector"] = ValueElement::make_string(kix("dice"));
        const tool_error sel_err = parse_params(&params, out);
        expect(sel_err.failed());
        expect(sv_of(sel_err.message).find("'self_eval' or 'majority'") !=
               std::string::npos);
    };

    // -----------------------------------------------------------------------
    // parse_params - parallel_sample validation
    // -----------------------------------------------------------------------
    "params_parallel_sample_happy_path"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Solve it"));
        params.values["sample_n"] = ValueElement::make_int(3);
        params.values["selector"] =
            ValueElement::make_string(kix("majority"));
        workflow_params out;
        expect(!parse_params(&params, out).failed());
        expect(out.mode == kix("parallel_sample"));
        expect(*out.sample_n == 3_i);
        expect(*out.selector == kix("majority"));
    };

    "params_parallel_sample_rules"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        workflow_params out;

        params.values["sample_n"] = ValueElement::make_int(0);
        params.values["prompt_template"] =
            ValueElement::make_string(kix("task"));
        const tool_error n_err = parse_params(&params, out);
        expect(n_err.failed());
        expect(n_err.message == kix("sample_n must be >= 1."));

        params.values["sample_n"] = ValueElement::make_int(2);
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("task"));
        const tool_error both_err = parse_params(&params, out);
        expect(both_err.failed());
        expect(both_err.message == kix("Use either prompt_template or "
                                       "prompt_prefix+suffix, not both."));

        params.values.erase("prompt_template");
        params.values.erase("prompt_prefix");
        const tool_error none_err = parse_params(&params, out);
        expect(none_err.failed());
        expect(none_err.message ==
               kix("parallel_sample mode requires the task prompt via "
                   "prompt_template or prompt_prefix (+prompt_suffix)."));
    };

    "params_items_must_be_strings"_test = [] {
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_int(1));
        params.values["items"] = ValueElement::make_array(std::move(items));
        workflow_params out;
        const tool_error err = parse_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("items must be a list of strings"));
    };

    // -----------------------------------------------------------------------
    // expand_template
    // -----------------------------------------------------------------------
    "expand_template_placeholder"_test = [] {
        const kimix::vector<kimix::string> items = kvec({"a.cpp", "b.cpp"});
        const kimix::vector<kimix::string> out = expand_template(
            kimix::optional<kimix::string>(kix("Fix errors in {{item}}.")),
            kimix::span<const kimix::string>(items), std::nullopt,
            std::nullopt);
        expect(out.size() == 2u);
        expect(out[0] == kix("Fix errors in a.cpp."));
        expect(out[1] == kix("Fix errors in b.cpp."));
    };

    "expand_template_replaces_every_occurrence"_test = [] {
        const kimix::vector<kimix::string> items = kvec({"X"});
        const kimix::vector<kimix::string> out = expand_template(
            kimix::optional<kimix::string>(kix("{{item}} and {{item}}")),
            kimix::span<const kimix::string>(items), std::nullopt,
            std::nullopt);
        expect(out[0] == kix("X and X"));
    };

    "expand_template_prefix_suffix"_test = [] {
        const kimix::vector<kimix::string> items = kvec({"one", "two"});
        const kimix::vector<kimix::string> out =
            expand_template(std::nullopt,
                            kimix::span<const kimix::string>(items),
                            kimix::optional<kimix::string>(kix("PRE-")),
                            kimix::optional<kimix::string>(kix("-POST")));
        expect(out.size() == 2u);
        expect(out[0] == kix("PRE-one-POST"));
        expect(out[1] == kix("PRE-two-POST"));
    };

    "expand_template_prefix_without_suffix"_test = [] {
        const kimix::vector<kimix::string> items = kvec({"one"});
        const kimix::vector<kimix::string> out =
            expand_template(std::nullopt,
                            kimix::span<const kimix::string>(items),
                            kimix::optional<kimix::string>(kix("PRE-")),
                            std::nullopt);
        expect(out[0] == kix("PRE-one"));
    };

    "expand_template_passthrough"_test = [] {
        const kimix::vector<kimix::string> items = kvec({"full prompt"});
        // A template WITHOUT the placeholder falls through to the bare items.
        const kimix::vector<kimix::string> out = expand_template(
            kimix::optional<kimix::string>(kix("no placeholder here")),
            kimix::span<const kimix::string>(items), std::nullopt,
            std::nullopt);
        expect(out.size() == 1u);
        expect(out[0] == kix("full prompt"));
    };

    // -----------------------------------------------------------------------
    // validate_uniqueness / xml_escape
    // -----------------------------------------------------------------------
    "validate_uniqueness_passes"_test = [] {
        const kimix::vector<kimix::string> prompts = kvec({"a", "b", "c"});
        expect(validate_uniqueness(
                   kimix::span<const kimix::string>(prompts))
                   .empty());
    };

    "validate_uniqueness_reports_duplicates"_test = [] {
        const kimix::vector<kimix::string> prompts =
            kvec({"a", "b", "a", "b", "c"});
        const std::string message = sv_of(validate_uniqueness(
            kimix::span<const kimix::string>(prompts)));
        expect(message.find(
                   "Expanded prompts must be unique; duplicates: {") == 0)
            << message;
        expect(message.find("'a'") != std::string::npos);
        expect(message.find("'b'") != std::string::npos);
        // Sorted for determinism (Python renders an unordered set).
        expect(message.find("'a', 'b'") != std::string::npos) << message;
    };

    "xml_escape_matches_html_escape"_test = [] {
        expect(xml_escape("plain") == kix("plain"));
        expect(xml_escape("") == kix(""));
        expect(xml_escape("a & b") == kix("a &amp; b"));
        expect(xml_escape("<tag>") == kix("&lt;tag&gt;"));
        expect(xml_escape("say \"hi\"") == kix("say &quot;hi&quot;"));
        expect(xml_escape("it's") == kix("it&#x27;s"));
        expect(xml_escape("<a href=\"x\">&'</a>") ==
               kix("&lt;a href=&quot;x&quot;&gt;&amp;&#x27;&lt;/a&gt;"));
    };

    // -----------------------------------------------------------------------
    // render_results
    // -----------------------------------------------------------------------
    "render_results_all_succeeded"_test = [] {
        swarm_result first;
        first.index = 0;
        first.agent_id = "agent-a";
        first.output = "did A";
        first.success = true;
        first.elapsed = 2.25;
        swarm_result second;
        second.index = 1;
        second.agent_id = "agent-b";
        second.output = "did B";
        second.success = true;
        second.elapsed = std::nullopt; // falsy -> "-"
        const kimix::vector<swarm_result> results = {first, second};
        const std::string out = sv_of(render_results(
            kimix::span<const swarm_result>(results), "the swarm"));
        const std::string expected =
            "<agent_swarm_result>\n"
            "  <description>the swarm</description>\n"
            "  <total>2</total>\n"
            "  <succeeded>2</succeeded>\n"
            "  <failed>0</failed>\n"
            "  <subagents>\n"
            "    <subagent id=\"agent-a\" index=\"0\" success=\"true\" "
            "elapsed=\"2.2s\">\n"
            "      <output>did A</output>\n"
            "    </subagent>\n"
            "    <subagent id=\"agent-b\" index=\"1\" success=\"true\" "
            "elapsed=\"-\">\n"
            "      <output>did B</output>\n"
            "    </subagent>\n"
            "  </subagents>\n"
            "</agent_swarm_result>";
        expect(out == expected) << "got:\n" << out;
    };

    "render_results_with_failures"_test = [] {
        swarm_result ok;
        ok.index = 0;
        ok.agent_id = "a";
        ok.output = "fine";
        ok.success = true;
        ok.elapsed = 1.0;
        swarm_result bad;
        bad.index = 1;
        bad.agent_id = "b";
        bad.output = "boom";
        bad.success = false;
        bad.error = kimix::string("rate limit & <retry>");
        bad.elapsed = 0.5;
        const kimix::vector<swarm_result> results = {ok, bad};
        const std::string out = sv_of(render_results(
            kimix::span<const swarm_result>(results), "d <x>"));
        expect(out.find("  <description>d &lt;x&gt;</description>") !=
               std::string::npos);
        expect(out.find("  <succeeded>1</succeeded>") != std::string::npos);
        expect(out.find("  <failed>1</failed>") != std::string::npos);
        expect(out.find("  <resume_hint>Some sub-agents failed. Re-run with "
                        "resume_agent_ids mapping the failed agent IDs to "
                        "adjusted prompts.</resume_hint>") !=
               std::string::npos);
        expect(out.find("      <error>rate limit &amp; &lt;retry&gt;</error>") !=
               std::string::npos);
        expect(out.find("success=\"false\"") != std::string::npos);
    };

    "render_results_empty"_test = [] {
        const std::string out = sv_of(render_results(
            kimix::span<const swarm_result>(), "nothing"));
        expect(out.find("  <total>0</total>") != std::string::npos);
        expect(out.find("resume_hint") == std::string::npos);
        expect(out.find("  <subagents>\n  </subagents>") !=
               std::string::npos);
    };

    // -----------------------------------------------------------------------
    // render_best_of_n
    // -----------------------------------------------------------------------
    "render_best_of_n_document"_test = [] {
        best_of_n_result result;
        result.winner_index = 1;
        result.selection_reason = "self-eval selection";
        result.candidates = {make_candidate(0, true, "r0", "d0"),
                             make_candidate(1, true, "r1", "d1"),
                             make_candidate(2, false, "", "", "kaboom")};
        const std::string out =
            sv_of(render_best_of_n(result, "pick & win"));
        const std::string expected =
            "<best_of_n_result>\n"
            "  <description>pick &amp; win</description>\n"
            "  <samples>3</samples>\n"
            "  <winner>1</winner>\n"
            "  <selection>self-eval selection</selection>\n"
            "  <candidate index=\"0\" status=\"ok\"/>\n"
            "  <candidate index=\"1\" status=\"ok\"/>\n"
            "  <candidate index=\"2\" status=\"failed: kaboom\"/>\n"
            "</best_of_n_result>";
        expect(out == expected) << "got:\n" << out;
    };

    "render_best_of_n_double_escapes_the_failure"_test = [] {
        best_of_n_result result;
        result.candidates = {
            make_candidate(0, false, "", "", "a & b <c>")};
        const std::string out = sv_of(render_best_of_n(result, "d"));
        // status = "failed: " + xml_escape(error), then the whole status is
        // escaped again for the attribute.
        expect(out.find("status=\"failed: a &amp;amp; b &amp;lt;c&amp;gt;\"") !=
               std::string::npos)
            << out;
    };

    // -----------------------------------------------------------------------
    // Rate limiting / retry classification
    // -----------------------------------------------------------------------
    "is_rate_limit_error_markers"_test = [] {
        expect(is_rate_limit_error("Rate Limit exceeded"));
        expect(is_rate_limit_error("rate-limit hit"));
        expect(is_rate_limit_error("429 Too Many Requests"));
        expect(is_rate_limit_error("no capacity right now"));
        expect(is_rate_limit_error("request throttled"));
        expect(is_rate_limit_error("quota exceeded"));
        expect(!is_rate_limit_error("segmentation fault"));
        expect(!is_rate_limit_error(""));
    };

    "retry_delay_doubles"_test = [] {
        expect(retry_delay_seconds(0) == 1.0);
        expect(retry_delay_seconds(1) == 2.0);
        expect(retry_delay_seconds(2) == 4.0);
        expect(retry_delay_seconds(3) == 8.0);
    };

    "rate_limiter_burst_is_immediate"_test = [] {
        rate_limiter limiter(3, 0.5, 100.0);
        // The first `burst` acquisitions never wait.
        expect(limiter.acquire(100.0) == 0.0);
        expect(limiter.acquire(100.0) == 0.0);
        expect(limiter.acquire(100.0) == 0.0);
        // The fourth needs a full interval to refill one token.
        expect(limiter.acquire(100.0) == 0.5_d);
    };

    "rate_limiter_refills_over_time"_test = [] {
        rate_limiter limiter(2, 1.0, 0.0);
        expect(limiter.acquire(0.0) == 0.0);
        expect(limiter.acquire(0.0) == 0.0);
        // Exhausted at t=0; one second later a full token has accrued.
        expect(limiter.acquire(1.0) == 0.0);
        expect(limiter.acquire(1.0) == 1.0_d);
    };

    "rate_limiter_caps_at_burst"_test = [] {
        rate_limiter limiter(2, 1.0, 0.0);
        // A long idle period cannot bank more than `burst` tokens.
        expect(limiter.acquire(100.0) == 0.0);
        expect(limiter.acquire(100.0) == 0.0);
        expect(limiter.acquire(100.0) == 1.0_d);
    };

    // -----------------------------------------------------------------------
    // best-of-N kernels
    // -----------------------------------------------------------------------
    "format_candidates_for_review"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "report0", "diff0"),
            make_candidate(1, false, "", "", "oops")};
        const std::string out = sv_of(format_candidates_for_review(
            kimix::span<const sample_candidate>(candidates)));
        expect(out.find("=== Candidate 0 (ok, 2 steps) ===") == 0) << out;
        expect(out.find("Self-report:\nreport0\n\nDiff:\ndiff0") !=
               std::string::npos);
        expect(out.find("=== Candidate 1 (failed: oops, 2 steps) ===") !=
               std::string::npos);
        // Parts are joined with a blank line.
        expect(out.find("\n\n=== Candidate 1") != std::string::npos);
    };

    "all_candidates_failed_message"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, false, "", "", "err0"),
            make_candidate(1, false, "", "", "err1")};
        expect(all_candidates_failed_message(
                   kimix::span<const sample_candidate>(candidates)) ==
               kix("all 2 sampled candidates failed: #0: err0; #1: err1"));
    };

    "single_run_and_verification_messages"_test = [] {
        expect(single_run_failed_message("boom") ==
               kix("single run failed: boom"));
        expect(verification_rejected_message(2, "tests red") ==
               kix("selected candidate #2 failed verification: tests red"));
    };

    "format_votes_dict_repr"_test = [] {
        const kimix::vector<std::pair<int32_t, int32_t>> votes = {{0, 2},
                                                                  {1, 1}};
        expect(format_votes(
                   kimix::span<const std::pair<int32_t, int32_t>>(votes)) ==
               kix("{0: 2, 1: 1}"));
    };

    "select_no_viable_candidate"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, false, "", "", "e0"),
            make_candidate(1, false, "", "", "e1")};
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [](kimix::string_view, kimix::string_view) { return 0; });
        expect(!out.ok);
        expect(out.error ==
               kix("all 2 sampled candidates failed: #0: e0; #1: e1"));
    };

    "select_single_viable_short_circuits"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, false, "", "", "e0"),
            make_candidate(1, true, "only one", "d1")};
        int selector_calls = 0;
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [&selector_calls](kimix::string_view, kimix::string_view) {
                ++selector_calls;
                return 0;
            });
        expect(out.ok);
        expect(out.winner_index == 1_i);
        expect(out.reason == kix("only one viable candidate"));
        expect(selector_calls == 0);
    };

    "select_self_eval_honours_the_selector"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "r0", "d0"),
            make_candidate(1, true, "r1", "d1")};
        kimix::string seen_review;
        const selection_outcome out = select_best_candidate(
            "the task", kimix::span<const sample_candidate>(candidates),
            [&seen_review](kimix::string_view task,
                           kimix::string_view review) {
                expect(task == kix("the task"));
                seen_review = kimix::string(review);
                return 1;
            });
        expect(out.ok);
        expect(out.winner_index == 1_i);
        expect(out.reason == kix("self-eval selection"));
        expect(sv_of(seen_review).find("=== Candidate 0") != std::string::npos);
    };

    "select_self_eval_invalid_index_falls_back"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "r0", "d0"),
            make_candidate(1, true, "r1", "d1")};
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [](kimix::string_view, kimix::string_view) { return 99; });
        expect(out.ok);
        expect(out.winner_index == 0_i);
        expect(out.reason ==
               kix("selector returned invalid index 99; fell back"));
    };

    "select_majority_votes_pairwise"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "r0", "d0"),
            make_candidate(1, true, "r1", "d1"),
            make_candidate(2, true, "r2", "d2")};
        int pairs = 0;
        // The selector always prefers the SECOND candidate of a pair, so the
        // votes land on 1, 2, 2 -> winner 2.
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [&pairs](kimix::string_view, kimix::string_view review) {
                ++pairs;
                const std::string text(review.data(), review.size());
                return text.find("Candidate B (#2)") != std::string::npos ? 2
                                                                          : 1;
            },
            "majority");
        expect(pairs == 3); // C(3, 2)
        expect(out.ok);
        expect(out.winner_index == 2_i);
        expect(sv_of(out.reason).find("majority vote {") == 0) << out.reason;
    };

    "select_majority_tie_breaks_to_the_lower_index"_test = [] {
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "r0", "d0"),
            make_candidate(1, true, "r1", "d1"),
            make_candidate(2, true, "r2", "d2")};
        // Always prefer candidate A (the first of each pair) -> every vote
        // goes to 0 except the (1,2) pair which also goes to 1... a plain
        // "always A" selector gives 0 two votes and 1 one vote.
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [](kimix::string_view, kimix::string_view review) {
                const std::string text(review.data(), review.size());
                const size_t at = text.find("Candidate A (#");
                if (at == std::string::npos) {
                    return -1;
                }
                return static_cast<int32_t>(text[at + 14] - '0');
            },
            "majority");
        expect(out.ok);
        expect(out.winner_index == 0_i);
    };

    "select_majority_needs_three_viable"_test = [] {
        // With only two viable candidates the majority strategy is skipped and
        // the self-eval review path is used instead.
        const kimix::vector<sample_candidate> candidates = {
            make_candidate(0, true, "r0", "d0"),
            make_candidate(1, true, "r1", "d1"),
            make_candidate(2, false, "", "", "e2")};
        bool saw_pair_text = false;
        const selection_outcome out = select_best_candidate(
            "task", kimix::span<const sample_candidate>(candidates),
            [&saw_pair_text](kimix::string_view, kimix::string_view review) {
                saw_pair_text =
                    std::string(review.data(), review.size())
                        .find("Self-report:") != std::string::npos;
                return 1;
            },
            "majority");
        expect(saw_pair_text);
        expect(out.winner_index == 1_i);
        expect(out.reason == kix("self-eval selection"));
    };

    // -----------------------------------------------------------------------
    // run_parallel_sample / best_of_n
    // -----------------------------------------------------------------------
    "run_parallel_sample_orders_and_marks_workspace"_test = [] {
        workspace_hooks hooks;
        hooks.create = [](kimix::string_view work_dir,
                          int32_t index) {
            return std::make_pair(
                kimix::format("{}/worker{}", work_dir, index),
                kimix::string("copy"));
        };
        hooks.collect_diff = [](kimix::string_view worker,
                                kimix::string_view kind,
                                kimix::string_view) {
            return kimix::string("diff-of-") + kimix::string(worker) +
                   " (" + kimix::string(kind) + ")";
        };
        const kimix::vector<sample_candidate> out = run_parallel_sample(
            "the task", 3, "/main",
            [](kimix::string_view prompt, kimix::string_view worker) {
                sample_run_outcome r;
                r.ok = true;
                r.self_report = kimix::string("report from ") +
                                kimix::string(worker);
                r.steps = 4;
                r.output_tokens = 10;
                expect(prompt == kix("the task"));
                return r;
            },
            hooks, 1);
        expect(out.size() == 3u);
        for (int i = 0; i < 3; ++i) {
            expect(out[static_cast<size_t>(i)].index == i);
            expect(out[static_cast<size_t>(i)].success);
            expect(out[static_cast<size_t>(i)].steps == 4_i);
            expect(sv_of(out[static_cast<size_t>(i)].diff)
                       .find("[workspace:copy]") == 0);
            expect(sv_of(out[static_cast<size_t>(i)].diff)
                       .find("diff-of-/main/worker") != std::string::npos);
        }
    };

    "run_parallel_sample_isolates_failures"_test = [] {
        workspace_hooks hooks;
        hooks.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("worktree"));
        };
        hooks.collect_diff = [](kimix::string_view, kimix::string_view,
                                kimix::string_view) {
            return kimix::string("d");
        };
        const kimix::vector<sample_candidate> out = run_parallel_sample(
            "t", 3, "/main",
            [](kimix::string_view, kimix::string_view worker) {
                sample_run_outcome r;
                if (worker == kix("/w1")) {
                    r.ok = false;
                    r.error = "RuntimeError: nope";
                    return r;
                }
                r.ok = true;
                r.self_report = "fine";
                return r;
            },
            hooks, 1);
        expect(out[0].success);
        expect(!out[1].success);
        expect(*out[1].error == kix("RuntimeError: nope"));
        expect(out[2].success);
        // The workspace marker is prepended even for a failed candidate.
        expect(sv_of(out[1].diff).find("[workspace:worktree]") == 0);
    };

    "run_parallel_sample_rejects_n_below_one"_test = [] {
        workspace_hooks hooks;
        const kimix::vector<sample_candidate> out = run_parallel_sample(
            "t", 0, "/main",
            [](kimix::string_view, kimix::string_view) {
                return sample_run_outcome{};
            },
            hooks, 1);
        expect(out.empty());
    };

    "best_of_n_single_run_path"_test = [] {
        workspace_hooks hooks;
        hooks.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("copy"));
        };
        hooks.collect_diff = [](kimix::string_view, kimix::string_view,
                                kimix::string_view) {
            return kimix::string("the diff");
        };
        bool applied = false;
        hooks.apply = [&applied](const sample_candidate &,
                                 kimix::string_view, kimix::string_view) {
            applied = true;
        };
        const best_of_n_outcome ok = best_of_n(
            "task", "/main",
            [](kimix::string_view, kimix::string_view) {
                sample_run_outcome r;
                r.ok = true;
                r.self_report = "done";
                return r;
            },
            [](kimix::string_view, kimix::string_view) { return 0; }, hooks,
            1);
        expect(ok.ok);
        expect(ok.result.winner_index == 0_i);
        expect(ok.result.selection_reason == kix("n=1: no selection"));
        expect(ok.result.candidates.size() == 1u);
        // best_of_n.py 366-380: the n<=1 degenerate path returns right after
        // the single run - it does NOT call apply_diff_to_workspace.
        expect(!applied);

        // A failed single run surfaces AllCandidatesFailedError.
        const best_of_n_outcome bad = best_of_n(
            "task", "/main",
            [](kimix::string_view, kimix::string_view) {
                sample_run_outcome r;
                r.ok = false;
                r.error = "kaboom";
                return r;
            },
            [](kimix::string_view, kimix::string_view) { return 0; }, hooks,
            1);
        expect(!bad.ok);
        expect(bad.error == kix("single run failed: kaboom"));
    };

    "best_of_n_verification_rejection"_test = [] {
        workspace_hooks hooks;
        hooks.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("copy"));
        };
        hooks.collect_diff = [](kimix::string_view, kimix::string_view,
                                kimix::string_view) {
            return kimix::string("d");
        };
        kimix::string applied_kind;
        hooks.apply = [&applied_kind](const sample_candidate &,
                                      kimix::string_view,
                                      kimix::string_view kind) {
            applied_kind = kimix::string(kind);
        };
        const best_of_n_outcome out = best_of_n(
            "task", "/main",
            [](kimix::string_view, kimix::string_view) {
                sample_run_outcome r;
                r.ok = true;
                r.self_report = "done";
                return r;
            },
            [](kimix::string_view, kimix::string_view) { return 1; }, hooks,
            2, "self_eval",
            [](kimix::string_view work_dir) {
                expect(work_dir == kix("/main"));
                return std::make_pair(false, kimix::string("tests still red"));
            });
        expect(!out.ok);
        expect(out.error == kix("selected candidate #1 failed verification: "
                                "tests still red"));
        expect(applied_kind == kix("copy"));
    };

    "best_of_n_verify_passes"_test = [] {
        workspace_hooks hooks;
        hooks.create = [](kimix::string_view, int32_t i) {
            return std::make_pair(kimix::format("/w{}", i),
                                  kimix::string("worktree"));
        };
        hooks.collect_diff = [](kimix::string_view, kimix::string_view,
                                kimix::string_view) {
            return kimix::string("d");
        };
        kimix::string applied_kind;
        hooks.apply = [&applied_kind](const sample_candidate &,
                                      kimix::string_view,
                                      kimix::string_view kind) {
            applied_kind = kimix::string(kind);
        };
        const best_of_n_outcome out = best_of_n(
            "task", "/main",
            [](kimix::string_view, kimix::string_view) {
                sample_run_outcome r;
                r.ok = true;
                return r;
            },
            [](kimix::string_view, kimix::string_view) { return 0; }, hooks,
            2, "self_eval",
            [](kimix::string_view) {
                return std::make_pair(true, kimix::string("green"));
            });
        expect(out.ok);
        expect(out.result.verified);
        expect(out.result.verify_detail == kix("green"));
        // The winner's diff carried the worktree marker.
        expect(applied_kind == kix("worktree"));
    };

    // -----------------------------------------------------------------------
    // run_swarm
    // -----------------------------------------------------------------------
    "run_swarm_preserves_index_order"_test = [] {
        const kimix::vector<swarm_task> tasks = {
            {kix("third"), std::nullopt, 2},
            {kix("first"), std::nullopt, 0},
            {kix("second"), std::nullopt, 1}};
        fake_swarm runner;
        const kimix::vector<swarm_result> results =
            run_swarm(kimix::span<const swarm_task>(tasks), "coder",
                      runner.fn(), 1, 0.0);
        expect(results.size() == 3u);
        expect(results[0].index == 0_i);
        expect(results[1].index == 1_i);
        expect(results[2].index == 2_i);
        expect(results[0].output == kix("result for first"));
        expect(results[2].output == kix("result for third"));
        expect(runner.types->size() == 3u);
        expect((*runner.types)[0] == kix("coder"));
    };

    "run_swarm_reports_failures"_test = [] {
        const kimix::vector<swarm_task> tasks = {
            {kix("ok item"), std::nullopt, 0},
            {kix("bad item"), std::nullopt, 1}};
        fake_swarm runner;
        runner.rules->emplace_back("bad", "exploded");
        const kimix::vector<swarm_result> results =
            run_swarm(kimix::span<const swarm_task>(tasks), "explore",
                      runner.fn(), 1, 0.0);
        expect(results[0].success);
        expect(!results[1].success);
        expect(*results[1].error == kix("exploded"));
    };

    "run_swarm_uses_resume_agent_ids"_test = [] {
        const kimix::vector<swarm_task> tasks = {
            {kix("retry prompt"), kimix::optional<kimix::string>(
                                      kimix::string("agent-7")),
             0}};
        fake_swarm runner;
        const kimix::vector<swarm_result> results =
            run_swarm(kimix::span<const swarm_task>(tasks), "coder",
                      runner.fn(), 1, 0.0);
        expect(results[0].agent_id == kix("agent-7"));
    };

    "env_max_concurrency_matches_the_reference"_test = [] {
        // swarm/__init__.py 395-402: max(1, int(raw)) on a parseable value,
        // _DEFAULT_BURST when the variable is absent or unparsable.
        const char *name = "KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY";
        const auto set_env = [name](const char *value) {
#ifdef _WIN32
            _putenv_s(name, value == nullptr ? "" : value);
#else
            if (value == nullptr) {
                unsetenv(name);
            } else {
                setenv(name, value, 1);
            }
#endif
        };
        set_env(nullptr);
        expect(env_max_concurrency() == k_default_burst);
        set_env("3");
        expect(env_max_concurrency() == 3_i);
        set_env(" 7 ");
        expect(env_max_concurrency() == 7_i);
        set_env("1_0");
        expect(env_max_concurrency() == 10_i);
        set_env("0");
        expect(env_max_concurrency() == 1_i); // max(1, 0)
        set_env("-4");
        expect(env_max_concurrency() == 1_i); // max(1, -4)
        set_env("abc");
        expect(env_max_concurrency() == k_default_burst); // ValueError
        set_env(nullptr);
        expect(env_max_concurrency() == k_default_burst);
    };

    "run_swarm_empty_task_list"_test = [] {
        fake_swarm runner;
        const kimix::vector<swarm_result> results =
            run_swarm(kimix::span<const swarm_task>(), "coder", runner.fn(),
                      1, 0.0);
        expect(results.empty());
        expect(runner.calls->empty());
    };

    // -----------------------------------------------------------------------
    // Workflow Tool wrapper
    // -----------------------------------------------------------------------
    "workflow_recursion_guard"_test = [] {
        Session session;
        session.is_sub_agent = true;
        session.swarm_enabled = true;
        Workflow tool(&session);
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Recursive sub-agent swarm call detected."));
        expect(has(tool.serialized_result(),
                   "sub-agent recursively called workflow"));
    };

    "workflow_requires_a_swarm_session"_test = [] {
        Session session; // swarm_enabled defaults to false
        Workflow tool(&session);
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        tool(&params);
        expect(has(tool.serialized_result(), "unsupported"));
        expect(has(tool.serialized_result(),
                   "workflow is only available in a swarm session"));
    };

    "workflow_requires_a_runner"_test = [] {
        Session session;
        session.swarm_enabled = true;
        Workflow tool(&session);
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Fix "));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a")));
        items.push_back(ValueElement::make_string(kix("b")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "native workflow requires an injected runner"));
    };

    "workflow_validation_error_surfaces"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        Workflow tool(&session);
        tool.runner = runner.fn();
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Fix "));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("only")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Provide at least 2 items or resume_agent_ids."));
        expect(runner.calls->empty());
    };

    "workflow_duplicate_prompts_rejected"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        Workflow tool(&session);
        tool.runner = runner.fn();
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("Fix "));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("same")));
        items.push_back(ValueElement::make_string(kix("same")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Expanded prompts must be unique; duplicates:"));
        expect(runner.calls->empty());
    };

    "workflow_fanout_end_to_end"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        runner.rules->emplace_back("b.cpp", "compile error");
        Workflow tool(&session);
        tool.runner = runner.fn();
        tool.max_concurrency = 1; // deterministic, no rate-limit sleeping
        ToolParams params;
        params.values["description"] =
            ValueElement::make_string(kix("fix the builds"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix errors in {{item}}."));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a.cpp")));
        items.push_back(ValueElement::make_string(kix("b.cpp")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        params.values["subagent_type"] =
            ValueElement::make_string(kix("explore"));
        tool(&params);

        expect(runner.calls->size() == 2u);
        expect(runner.calls->at(0).prompt == kix("Fix errors in a.cpp."));
        expect(runner.calls->at(1).prompt == kix("Fix errors in b.cpp."));
        expect((*runner.types)[0] == kix("explore"));

        const std::string json = json_of(tool.serialized_result());
        expect(json.find("<agent_swarm_result>") != std::string::npos);
        expect(json.find("<description>fix the builds</description>") !=
               std::string::npos);
        expect(json.find("<total>2</total>") != std::string::npos);
        expect(json.find("<succeeded>1</succeeded>") != std::string::npos);
        expect(json.find("<failed>1</failed>") != std::string::npos);
        expect(json.find("resume_hint") != std::string::npos);
        expect(json.find("compile error") != std::string::npos);
        expect(json.find("Swarm completed") != std::string::npos);
    };

    "workflow_fanout_with_resume_ids"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        Workflow tool(&session);
        tool.runner = runner.fn();
        tool.max_concurrency = 1;
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Fix {{item}}"));
        ValueElement::Array items;
        items.push_back(ValueElement::make_string(kix("a")));
        items.push_back(ValueElement::make_string(kix("b")));
        params.values["items"] = ValueElement::make_array(std::move(items));
        kimix::shared_ptr<ToolParams> resumes(new ToolParams());
        resumes->values["agent-9"] =
            ValueElement::make_string(kix("retry this"));
        params.values["resume_agent_ids"] =
            ValueElement::make_object(std::move(resumes));
        tool(&params);
        expect(runner.calls->size() == 3u);
        // The resume task keeps its agent id and follows the expanded items.
        expect(runner.calls->at(2).prompt == kix("retry this"));
        expect(runner.calls->at(2).agent_id.has_value());
        expect(*runner.calls->at(2).agent_id == kix("agent-9"));
        expect(runner.calls->at(2).index == 2_i);
    };

    "workflow_parallel_sample_end_to_end"_test = [] {
        Session session;
        session.swarm_enabled = true;
        session.work_dir = "/main";
        fake_swarm runner;
        Workflow tool(&session);
        tool.runner = runner.fn();
        tool.max_concurrency = 1;
        // Scripted workspace hooks so no real git/filesystem work happens.
        tool.workspaces.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("copy"));
        };
        tool.workspaces.collect_diff = [](kimix::string_view worker,
                                          kimix::string_view,
                                          kimix::string_view) {
            return kimix::string("diff ") + kimix::string(worker);
        };
        int applied = 0;
        tool.workspaces.apply = [&applied](const sample_candidate &,
                                           kimix::string_view,
                                           kimix::string_view) { ++applied; };
        tool.selector = [](kimix::string_view, kimix::string_view review) {
            // Pick candidate 1 when its diff is present in the review text.
            const std::string text(review.data(), review.size());
            return text.find("/w1") != std::string::npos ? 1 : 0;
        };

        ToolParams params;
        params.values["description"] =
            ValueElement::make_string(kix("best solution"));
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("Solve {{item}} now"));
        params.values["sample_n"] = ValueElement::make_int(2);
        tool(&params);

        const std::string json = json_of(tool.serialized_result());
        expect(json.find("<best_of_n_result>") != std::string::npos);
        expect(json.find("<description>best solution</description>") !=
               std::string::npos);
        expect(json.find("<samples>2</samples>") != std::string::npos);
        expect(json.find("<winner>1</winner>") != std::string::npos);
        expect(json.find("<selection>self-eval selection</selection>") !=
               std::string::npos);
        expect(json.find("best-of-N completed") != std::string::npos);
        expect(applied == 1);
        // The {{item}} placeholder is stripped for the shared task prompt.
        expect(runner.calls->size() == 2u);
        expect(runner.calls->at(0).prompt == kix("Solve  now"));
    };

    "workflow_parallel_sample_prefix_form"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        Workflow tool(&session);
        tool.runner = runner.fn();
        tool.max_concurrency = 1;
        tool.workspaces.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("copy"));
        };
        tool.workspaces.collect_diff = [](kimix::string_view,
                                          kimix::string_view,
                                          kimix::string_view) {
            return kimix::string("d");
        };
        tool.selector = [](kimix::string_view, kimix::string_view) { return 0; };
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        params.values["prompt_prefix"] =
            ValueElement::make_string(kix("PRE "));
        params.values["prompt_suffix"] =
            ValueElement::make_string(kix(" POST"));
        tool(&params);
        expect(runner.calls->size() == 4u); // sample_n defaults to 4
        expect(runner.calls->at(0).prompt == kix("PRE  POST"));
    };

    "workflow_parallel_sample_all_failed"_test = [] {
        Session session;
        session.swarm_enabled = true;
        fake_swarm runner;
        runner.rules->emplace_back("", "everything broke");
        Workflow tool(&session);
        tool.runner = runner.fn();
        tool.max_concurrency = 1;
        tool.workspaces.create = [](kimix::string_view, int32_t index) {
            return std::make_pair(kimix::format("/w{}", index),
                                  kimix::string("copy"));
        };
        tool.workspaces.collect_diff = [](kimix::string_view,
                                          kimix::string_view,
                                          kimix::string_view) {
            return kimix::string("d");
        };
        tool.selector = [](kimix::string_view, kimix::string_view) { return 0; };
        ToolParams params;
        params.values["description"] = ValueElement::make_string(kix("d"));
        params.values["mode"] =
            ValueElement::make_string(kix("parallel_sample"));
        params.values["prompt_template"] =
            ValueElement::make_string(kix("task"));
        params.values["sample_n"] = ValueElement::make_int(2);
        tool(&params);
        expect(has(tool.serialized_result(), "all samples failed"));
        expect(has(tool.serialized_result(),
                   "sampled candidates failed:"));
    };

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------
    "all_ten_new_tools_are_registered"_test = [] {
        const kimix::vector<ToolMeta> all =
            ToolRegistry::instance().all();
        const char *expected[] = {"WritePlan",  "ReadPlan",   "EditPlan",
                                  "Run",        "JobOutput",  "Subagent",
                                  "SendMessage", "ListAgents",
                                  "InterruptAgent", "Workflow"};
        for (const char *name : expected) {
            bool found = false;
            for (const ToolMeta &meta : all) {
                if (meta.name == kix(name)) {
                    found = true;
                    expect(!meta.description.empty())
                        << name << " has no description";
                    expect(!meta.parameters_json.empty())
                        << name << " has no schema";
                    break;
                }
            }
            expect(found) << "tool not registered: " << name;
        }
        // Case-insensitive lookup maps the agent-facing snake_case names.
        expect(ToolRegistry::instance().find_ci("workflow") != nullptr);
        expect(ToolRegistry::instance().find_ci("sendmessage") != nullptr);
        expect(ToolRegistry::instance().find_ci("joboutput") != nullptr);
    };

    // =======================================================================
    // Python-derived golden replay (scripts/gen_workflow_goldens.py)
    // =======================================================================

    "golden_tool_calls"_test = [] {
        for (const wf_golden_tool &g : kWfToolGoldens) {
            const std::string ctx(g.name);
            Session session;
            session.swarm_enabled = g.swarm_session;
            session.is_sub_agent = g.sub_agent;
            Workflow tool(&session);

            wf_script script(g.rules, g.rule_count);
            const std::map<int64_t, double> elapsed = g_elapsed_map(g.elapsed);
            // kimi-agent always has a working sub-agent runner; the C++
            // port's "no runner" path is a native-only guard, so the replay
            // injects one unconditionally (the duplicate-prompt validation
            // must not be shadowed by it).
            tool.runner = g_swarm_runner(script, elapsed);
            if (g_has_parallel_sample_mode(g.args)) {
                // Deterministic sample/selector ordering; the best-of-N path
                // has no rate limiter, so this only removes thread races.
                tool.max_concurrency = 1;
            }
            g_workspaces ws;
            tool.workspaces = g_workspace_hooks(ws);

            ValueElement args_el;
            expect(golden_parse(g.args, args_el)) << ctx;
            ToolParams args = *args_el.as_object();
            tool(&args);

            ToolParams res;
            const kimix::vector<char> &buf = tool.serialized_result();
            const kimix::string body(buf.data(), buf.size());
            kimix::string parse_error;
            expect(res.try_deserialize(
                       kimix::span<char const>(body.data(), body.size()),
                       parse_error))
                << ctx << " envelope: " << sv_of(parse_error);

            const ValueElement *ok = res.get_exact("ok");
            const bool got_ok = ok != nullptr && ok->is_bool() && ok->as_bool();
            expect(got_ok == !g.is_error) << ctx;
            expect_text(res_str(res, "status"), g.expect_status, ctx,
                        "status");
            expect_text(res_str(res, "message"), g.expect_message, ctx,
                        "message");
            expect_text(res_str(res, "brief"), g.expect_brief, ctx, "brief");
            expect_text(res_str(res, "output"), g.expect_output, ctx, "output");

            // Every dispatched prompt, in any thread order.
            expect(sorted(script.calls) == sorted(g_str_list(g.calls))) << ctx;
        }
    };

    "golden_expand_template"_test = [] {
        for (const wf_golden_expand &g : kWfExpandGoldens) {
            const std::string ctx(g.name);
            const kimix::vector<kimix::string> items = g_kvec(g.items);
            kimix::optional<kimix::string> tpl;
            kimix::optional<kimix::string> prefix;
            kimix::optional<kimix::string> suffix;
            if (!g.tpl_null) {
                tpl = kix(g.tpl);
            }
            if (!g.prefix_null) {
                prefix = kix(g.prefix);
            }
            if (!g.suffix_null) {
                suffix = kix(g.suffix);
            }
            const kimix::vector<kimix::string> got =
                expand_template(tpl, kimix::span<const kimix::string>(items),
                                prefix, suffix);
            const kimix::vector<kimix::string> want = g_str_list(g.expect);
            expect(got.size() == want.size()) << ctx;
            for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
                expect(got[i] == kix(want[i])) << ctx << " [" << i << "]";
            }
        }
    };

    "golden_validate_uniqueness"_test = [] {
        for (const wf_golden_uniqueness &g : kWfUniquenessGoldens) {
            const std::string ctx(g.name);
            const kimix::vector<kimix::string> prompts = g_kvec(g.prompts);
            const kimix::string got = validate_uniqueness(
                kimix::span<const kimix::string>(prompts));
            expect_text(got, g.expect, ctx, "value");
        }
    };

    "golden_xml_escape"_test = [] {
        for (const wf_golden_text &g : kWfEscapeGoldens) {
            const kimix::string got = xml_escape(kimix::string_view(g.input));
            expect_text(got, g.expect, g.name, "value");
        }
    };

    "golden_render_results"_test = [] {
        for (const wf_golden_render &g : kWfRenderGoldens) {
            kimix::vector<swarm_result> results;
            ValueElement el;
            expect(golden_parse(g.results, el)) << g.name;
            for (const ValueElement &item : el.as_array()) {
                const ToolParams *obj = item.as_object();
                swarm_result r;
                int64_t value = 0;
                if (g_int(obj->get("index"), value)) {
                    r.index = static_cast<int32_t>(value);
                }
                r.agent_id = kix(g_str(obj->get("agent_id")));
                r.output = kix(g_str(obj->get("output")));
                const ValueElement *success = obj->get("success");
                r.success = success != nullptr && success->is_bool() &&
                            success->as_bool();
                const ValueElement *error = obj->get("error");
                if (error != nullptr && error->is_string()) {
                    r.error = error->as_string();
                }
                const ValueElement *time = obj->get("elapsed");
                if (time != nullptr && !time->is_null()) {
                    double seconds = 0.0;
                    if (g_num(time, seconds)) {
                        r.elapsed = seconds;
                    }
                }
                results.push_back(std::move(r));
            }
            const kimix::string got = render_results(
                kimix::span<const swarm_result>(results), kix(g.description));
            expect_text(got, g.expect, g.name, "value");
        }
    };

    "golden_render_best_of_n"_test = [] {
        for (const wf_golden_bon_render &g : kWfBonRenderGoldens) {
            best_of_n_result result;
            result.winner_index = g.winner;
            result.selection_reason = kix(g.reason);
            ValueElement el;
            expect(golden_parse(g.candidates, el)) << g.name;
            for (const ValueElement &item : el.as_array()) {
                result.candidates.push_back(g_candidate(item));
            }
            const kimix::string got =
                render_best_of_n(result, kix(g.description));
            expect_text(got, g.expect, g.name, "value");
        }
    };

    "golden_rate_limit_markers"_test = [] {
        for (const wf_golden_marker &g : kWfMarkerGoldens) {
            expect(is_rate_limit_error(kimix::string_view(g.text)) == g.expect)
                << g.name;
        }
    };

    "golden_retry_backoff"_test = [] {
        for (const wf_golden_retry &g : kWfRetryGoldens) {
            expect(retry_delay_seconds(g.attempt) == g.expect)
                << "attempt " << g.attempt;
        }
    };

    "golden_rate_limiter"_test = [] {
        for (const wf_golden_limiter &g : kWfLimiterGoldens) {
            const std::string ctx(g.name);
            ValueElement times;
            ValueElement expects;
            expect(golden_parse(g.times, times)) << ctx;
            expect(golden_parse(g.expects, expects)) << ctx;
            rate_limiter limiter(g.burst, g.interval, 1000.0);
            double now = 1000.0;
            for (size_t i = 0; i < times.as_array().size(); ++i) {
                double delta = 0.0;
                g_num(&times.as_array()[i], delta);
                now += delta;
                const double wait = limiter.acquire(now);
                if (wait > 0.0) {
                    now += wait; // the caller sleeps, as `run_swarm` does
                }
                double want = 0.0;
                g_num(&expects.as_array()[i], want);
                expect(std::fabs(now - want) < 1e-9)
                    << ctx << " step " << i << " got " << now
                    << " want " << want;
            }
        }
    };

    "golden_format_candidates_for_review"_test = [] {
        for (const wf_golden_text &g : kWfReviewGoldens) {
            const kimix::vector<sample_candidate> candidates =
                g_candidates(g.input);
            const kimix::string got = format_candidates_for_review(
                kimix::span<const sample_candidate>(candidates));
            expect_text(got, g.expect, g.name, "value");
        }
    };

    "golden_message_helpers"_test = [] {
        for (const wf_golden_message &g : kWfMessageGoldens) {
            const std::string ctx(g.name);
            const std::string kind(g.kind);
            kimix::string got;
            if (kind == "all_failed") {
                const kimix::vector<sample_candidate> candidates =
                    g_candidates(g.candidates);
                got = all_candidates_failed_message(
                    kimix::span<const sample_candidate>(candidates));
            } else if (kind == "single_run_failed") {
                got = single_run_failed_message(kimix::string_view(g.input));
            } else if (kind == "verification_rejected") {
                got = verification_rejected_message(g.index,
                                                    kimix::string_view(g.input));
            } else {
                ValueElement el;
                expect(golden_parse(g.votes, el)) << ctx;
                kimix::vector<std::pair<int32_t, int32_t>> votes;
                for (const ValueElement &pair : el.as_array()) {
                    int64_t index = 0;
                    int64_t count = 0;
                    g_int(&pair.as_array()[0], index);
                    g_int(&pair.as_array()[1], count);
                    votes.emplace_back(static_cast<int32_t>(index),
                                       static_cast<int32_t>(count));
                }
                got = format_votes(
                    kimix::span<const std::pair<int32_t, int32_t>>(votes));
            }
            expect_text(got, g.expect, ctx, "value");
        }
    };

    "golden_select_best_candidate"_test = [] {
        for (const wf_golden_select &g : kWfSelectGoldens) {
            const std::string ctx(g.name);
            const kimix::vector<sample_candidate> candidates =
                g_candidates(g.candidates);
            ValueElement replies_el;
            expect(golden_parse(g.replies, replies_el)) << ctx;
            std::vector<int32_t> replies;
            for (const ValueElement &item : replies_el.as_array()) {
                int64_t value = 0;
                g_int(&item, value);
                replies.push_back(static_cast<int32_t>(value));
            }
            kimix::vector<kimix::string> reviews;
            size_t used = 0;
            const selector_fn selector =
                [&replies, &reviews, &used](kimix::string_view,
                                            kimix::string_view review_text) {
                    reviews.emplace_back(review_text.data(),
                                         review_text.size());
                    if (replies.empty()) {
                        return int32_t{0};
                    }
                    const size_t pos = std::min(used, replies.size() - 1);
                    used += 1;
                    return replies[pos];
                };
            const selection_outcome got = select_best_candidate(
                "the task", kimix::span<const sample_candidate>(candidates),
                selector, kimix::string_view(g.strategy));

            expect(got.ok == (g.error[0] == '\0')) << ctx;
            expect(got.error == kix(strip_exc_prefix(g.error)))
                << ctx << " error=" << sv_of(got.error);
            if (g.winner >= 0) {
                expect(got.winner_index == g.winner)
                    << ctx << " winner=" << got.winner_index;
            }
            expect(got.reason == kix(g.reason))
                << ctx << " reason=" << sv_of(got.reason);
            const kimix::vector<kimix::string> want_reviews =
                g_str_list(g.reviews);
            expect(reviews.size() == want_reviews.size()) << ctx;
            for (size_t i = 0; i < reviews.size() && i < want_reviews.size();
                 ++i) {
                expect_text(reviews[i], want_reviews[i].c_str(), ctx,
                            "review");
            }
        }
    };

    "golden_run_parallel_sample"_test = [] {
        for (const wf_golden_sample &g : kWfSampleGoldens) {
            const std::string ctx(g.name);
            g_workspaces ws;
            wf_script script(g.rules, g.rule_count);
            const kimix::vector<sample_candidate> got = run_parallel_sample(
                kimix::string_view(g.task), g.n, kimix::string_view("/wf/main"),
                g_sample_runner(script), g_workspace_hooks(ws), 1);
            expect(script.calls == g_str_list(g.calls)) << ctx;
            expect(ws.applied == g_str_list(g.applied)) << ctx;
            expect_candidates(g.candidates, got, ctx);
        }
    };

    "golden_best_of_n"_test = [] {
        for (const wf_golden_bon &g : kWfBonGoldens) {
            const std::string ctx(g.name);
            g_workspaces ws;
            wf_script script(g.rules, g.rule_count);
            ValueElement replies_el;
            expect(golden_parse(g.replies, replies_el)) << ctx;
            std::vector<int32_t> replies;
            for (const ValueElement &item : replies_el.as_array()) {
                int64_t value = 0;
                g_int(&item, value);
                replies.push_back(static_cast<int32_t>(value));
            }
            kimix::vector<kimix::string> reviews;
            size_t used = 0;
            const selector_fn selector =
                [&replies, &reviews, &used](kimix::string_view,
                                            kimix::string_view review_text) {
                    reviews.emplace_back(review_text.data(),
                                         review_text.size());
                    if (replies.empty()) {
                        return int32_t{0};
                    }
                    const size_t pos = std::min(used, replies.size() - 1);
                    used += 1;
                    return replies[pos];
                };
            verify_fn verify;
            if (g.has_verify) {
                const bool ok = g.verify_ok;
                const std::string detail(g.verify_detail);
                verify = [ok, detail](kimix::string_view) {
                    return std::make_pair(ok, kimix::string(detail));
                };
            }
            const best_of_n_outcome got = best_of_n(
                "the task", "/wf/main", g_sample_runner(script), selector,
                g_workspace_hooks(ws), g.n, kimix::string_view(g.strategy),
                verify, 1);

            expect(got.ok == (g.error[0] == '\0')) << ctx;
            expect(got.error == kix(strip_exc_prefix(g.error)))
                << ctx << " error=" << sv_of(got.error);
            if (g.error[0] != '\0') {
                continue;
            }
            expect(got.result.winner_index == g.winner)
                << ctx << " winner=" << got.result.winner_index;
            expect(got.result.selection_reason == kix(g.reason))
                << ctx << " reason=" << sv_of(got.result.selection_reason);
            expect(got.result.verified == g.verified) << ctx;
            if (g.verified) {
                expect(got.result.verify_detail == kix(g.verify_detail)) << ctx;
            }
            expect(script.calls == g_str_list(g.calls)) << ctx;
            expect(ws.applied == g_str_list(g.applied)) << ctx;
            const kimix::vector<kimix::string> want_reviews =
                g_str_list(g.reviews);
            expect(reviews.size() == want_reviews.size()) << ctx;
            for (size_t i = 0; i < reviews.size() && i < want_reviews.size();
                 ++i) {
                expect_text(reviews[i], want_reviews[i].c_str(), ctx,
                            "review");
            }
            expect_candidates(g.candidates, got.result.candidates, ctx);
        }
    };
}
