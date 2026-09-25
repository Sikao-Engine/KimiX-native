// workflow_tool.h - C++ port of the kimi-agent `workflow` tool (Python class
// AgentSwarm) including the best-of-N sampling machinery.
//
// Python source of truth:
//   C:/dev/kimi-agent/src/kimix/tools/swarm/__init__.py
//     _MAX_SUB_AGENTS / _DEFAULT_BURST / _DEFAULT_INTERVAL_SECONDS /
//     _MAX_RETRIES / _RETRY_BASE_SECONDS / _SUBAGENT_TYPE_MAP   24-35
//     SwarmTask / SwarmSubagentResult                            37-63
//     AgentSwarmParams + _validate                               65-152
//     _RateLimiter                                               150-179
//     AgentSwarm.name / description / __call__ guards            182-215
//     AgentSwarm._execute (fanout)                               216-243
//     AgentSwarm._execute_parallel_sample                        244-300
//     _expand_template                                           320-329
//     _validate_uniqueness                                       331-339
//     _xml_escape                                                341-342
//     _render_results                                            344-372
//     _run_swarm (concurrency + rate limit)                      374-416
//     _is_rate_limit_error                                       418-431
//     _run_subagent_task (retry loop)                            433-489
//   C:/dev/kimi-agent/src/kimix/tools/swarm/best_of_n.py
//     SampleCandidate / SampleRunner / SelectorFn / VerifyFn     28-47
//     BestOfNResult / AllCandidatesFailedError /
//     VerificationRejectedError                                  49-63
//     _is_git_repo / _COPY_IGNORE / create_worker_workspace /
//     cleanup_worker_workspace                                   66-142
//     _snapshot_files / collect_diff / apply_diff_to_workspace   145-232
//     run_parallel_sample                                        238-296
//     format_candidates_for_review                               305-318
//     select_best_candidate                                      320-364
//     best_of_n                                                  366-423
//
// Design notes (project conventions):
// * namespace kimix::builtin_tools::workflow; TU-local helpers use the `wf_`
//   prefix (kimix-llm builds src/builtin_tools/*.cpp as one unity TU).
// * kimix:: containers only; no RTTI; kernels never throw across the tool
//   boundary.
// * Every LLM-facing piece is injectable exactly like the Python module
//   ("All LLM-facing pieces ... are injectable callables so the machinery is
//   fully testable offline"): `swarm_runner` for one sub-agent task,
//   `selector_fn` for the best-of-N review, `verify_fn` for post-application
//   verification. The defaults are wired to the session's agents registry
//   (builtin_tools/agent_tool.h) when Session::native_io is set.
// * The workspace isolation kernels (git worktree / directory copy) are
//   file-system effects and are therefore only exercised when native_io is
//   set; tests inject their own workspace functions.
// * Deviation: `_validate_uniqueness` renders the duplicate set sorted,
//   because Python's `set` repr order is unspecified; the message wording is
//   otherwise byte-identical.
#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::workflow {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// ---------------------------------------------------------------------------
// Constants (swarm/__init__.py 24-35)
// ---------------------------------------------------------------------------
inline constexpr int32_t k_max_sub_agents = 128; // _MAX_SUB_AGENTS
inline constexpr int32_t k_default_burst = 5; // _DEFAULT_BURST
inline constexpr double k_default_interval_seconds = 0.7; // _DEFAULT_INTERVAL_SECONDS
inline constexpr int32_t k_max_retries = 3; // _MAX_RETRIES
inline constexpr double k_retry_base_seconds = 1.0; // _RETRY_BASE_SECONDS
inline constexpr int32_t k_default_sample_n = 4; // _execute_parallel_sample
inline constexpr int32_t k_default_max_concurrency = 5; // run_parallel_sample
inline constexpr size_t k_prompt_offload_bytes = 100 * 1024;

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
// SwarmTask (37-45).
struct swarm_task {
    kimix::string prompt;
    kimix::optional<kimix::string> agent_id; // resume an existing sub-agent
    int32_t index = 0;
};

// SwarmSubagentResult (47-63).
struct swarm_result {
    int32_t index = 0;
    kimix::string agent_id;
    kimix::string output;
    bool success = false;
    kimix::optional<kimix::string> error;
    kimix::optional<double> elapsed; // seconds (monotonic)
};

// best_of_n.SampleCandidate (28-40).
struct sample_candidate {
    int32_t index = 0;
    kimix::string work_dir;
    kimix::string diff; // prefixed with "[workspace:<kind>]\n"
    kimix::string self_report;
    int32_t steps = 0;
    int64_t output_tokens = 0;
    bool success = false;
    kimix::optional<kimix::string> error;
};

// best_of_n.BestOfNResult (49-60).
struct best_of_n_result {
    int32_t winner_index = 0;
    kimix::string selection_reason;
    kimix::vector<sample_candidate> candidates;
    bool verified = false;
    kimix::string verify_detail;
};

// ---------------------------------------------------------------------------
// Injectable execution hooks
// ---------------------------------------------------------------------------
// One sub-agent task: (task, subagent_type) -> result. The default wires this
// to the session's agents::agent_registry runner.
using swarm_runner = kimix::function<swarm_result(const swarm_task &,
                                                  kimix::string_view)>;
// Best-of-N sample runner: (task_prompt, worker_work_dir) ->
// (self_report, steps, output_tokens). Throws/returns ok=false on failure.
struct sample_run_outcome {
    bool ok = false;
    kimix::string self_report;
    int32_t steps = 0;
    int64_t output_tokens = 0;
    kimix::string error;
};
using sample_runner =
    kimix::function<sample_run_outcome(kimix::string_view task_prompt,
                                       kimix::string_view worker_dir)>;
// Best-of-N selector: (task_prompt, formatted_candidates) -> chosen index.
using selector_fn = kimix::function<int32_t(kimix::string_view task_prompt,
                                            kimix::string_view review_text)>;
// Best-of-N verification: work_dir -> (ok, detail).
using verify_fn =
    kimix::function<std::pair<bool, kimix::string>(kimix::string_view work_dir)>;

// Workspace isolation hooks (best_of_n.py 66-232).
struct workspace_hooks {
    // create_worker_workspace(work_dir, index) -> (worker_path, kind)
    kimix::function<std::pair<kimix::string, kimix::string>(
        kimix::string_view work_dir, int32_t index)>
        create;
    // cleanup_worker_workspace(worker_path, kind, main_work_dir)
    kimix::function<void(kimix::string_view worker_path,
                         kimix::string_view kind,
                         kimix::string_view main_work_dir)>
        cleanup;
    // collect_diff(worker_path, kind, main_work_dir) -> unified diff text.
    // The copy mode compares the worker tree against `main_work_dir` (the
    // Python reference compares against a pre-run snapshot of the worker,
    // which is the same tree the copy was made from).
    kimix::function<kimix::string(kimix::string_view worker_path,
                                  kimix::string_view kind,
                                  kimix::string_view main_work_dir)>
        collect_diff;
    // apply_diff_to_workspace(winner, main_work_dir, kind)
    kimix::function<void(const sample_candidate &winner,
                         kimix::string_view main_work_dir,
                         kimix::string_view kind)>
        apply;
};

// ---------------------------------------------------------------------------
// Parameters (AgentSwarmParams 65-152)
// ---------------------------------------------------------------------------
struct workflow_params {
    kimix::string description;
    kimix::string mode = "fanout"; // fanout | parallel_sample
    kimix::optional<int32_t> sample_n;
    kimix::optional<kimix::string> selector; // self_eval | majority
    kimix::string subagent_type = "coder";
    kimix::optional<kimix::string> prompt_template;
    kimix::optional<kimix::string> prompt_prefix;
    kimix::optional<kimix::string> prompt_suffix;
    kimix::vector<kimix::string> items;
    // resume_agent_ids preserves the caller's key order (sorted for
    // determinism - ToolParams is an unordered map).
    kimix::vector<std::pair<kimix::string, kimix::string>> resume_agent_ids;
};

// AgentSwarmParams + _validate. Every ValueError message is reproduced
// byte-exactly (see the .cpp for the list).
tool_error parse_params(const ToolParams *params, workflow_params &out);

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------
// _expand_template (320-329).
kimix::vector<kimix::string>
expand_template(kimix::optional<kimix::string> prompt_template,
                kimix::span<const kimix::string> items,
                kimix::optional<kimix::string> prefix,
                kimix::optional<kimix::string> suffix);

// _validate_uniqueness (331-339). Returns the error message ("" when unique).
kimix::string validate_uniqueness(kimix::span<const kimix::string> prompts);

// _xml_escape == html.escape(text, quote=True): & < > " '
kimix::string xml_escape(kimix::string_view text);

// _render_results (344-372): the full <agent_swarm_result> document.
kimix::string render_results(kimix::span<const swarm_result> results,
                             kimix::string_view description);

// AgentSwarm._execute_parallel_sample (288-300): <best_of_n_result>.
kimix::string render_best_of_n(const best_of_n_result &result,
                               kimix::string_view description);

// _is_rate_limit_error (418-431): ASCII-lowered scan for the 8 markers.
bool is_rate_limit_error(kimix::string_view text);

// _run_subagent_task retry backoff (455-460): _RETRY_BASE_SECONDS * 2^attempt.
double retry_delay_seconds(int32_t attempt);

// _RateLimiter (150-179): token bucket. `start` seeds it at `now`;
// `acquire` returns the seconds the caller must sleep before proceeding and
// consumes one token. Pure - the caller owns the clock and the sleep.
class rate_limiter {
public:
    rate_limiter() = default;
    rate_limiter(int32_t burst, double interval, double now);
    void start(int32_t burst, double interval, double now);
    double acquire(double now);
    double tokens() const { return _tokens; }

private:
    int32_t _burst = 1;
    double _interval = 1.0;
    double _tokens = 0.0;
    double _last = 0.0;
};

// best_of_n.format_candidates_for_review (305-318).
kimix::string
format_candidates_for_review(kimix::span<const sample_candidate> candidates);

// best_of_n.select_best_candidate (320-364). `selector` is called for the
// review (self_eval) or for each pair (majority, viable >= 3). Returns false
// and fills `error` with the AllCandidatesFailedError message when nothing is
// viable.
struct selection_outcome {
    bool ok = false;
    int32_t winner_index = 0;
    kimix::string reason;
    kimix::string error; // all-candidates-failed message
};
selection_outcome
select_best_candidate(kimix::string_view task_prompt,
                      kimix::span<const sample_candidate> candidates,
                      const selector_fn &selector,
                      kimix::string_view strategy = "self_eval");

// "all {n} sampled candidates failed: #0: {err}; #1: {err}"
kimix::string all_candidates_failed_message(
    kimix::span<const sample_candidate> candidates);
// "single run failed: {error}"
kimix::string single_run_failed_message(kimix::string_view error);
// "selected candidate #{i} failed verification: {detail}"
kimix::string verification_rejected_message(int32_t winner_index,
                                            kimix::string_view detail);

// Python dict repr of the majority votes, e.g. "{0: 2, 1: 1}".
kimix::string format_votes(kimix::span<const std::pair<int32_t, int32_t>> votes);

// best_of_n.run_parallel_sample (238-296): run the same prompt in N isolated
// workspaces. `max_concurrency` bounds the fan-out; the workspace hooks and
// the runner are injected. Candidates keep their index order.
kimix::vector<sample_candidate>
run_parallel_sample(kimix::string_view task_prompt, int32_t n,
                    kimix::string_view work_dir, const sample_runner &runner,
                    const workspace_hooks &hooks,
                    int32_t max_concurrency = k_default_max_concurrency);

// Which reference exception produced `error` (best_of_n.py raises
// AllCandidatesFailedError / VerificationRejectedError; the tool maps the two
// onto different briefs).
enum class best_of_n_failure {
    none,
    all_candidates_failed,
    verification_rejected,
};

// best_of_n.best_of_n (366-423): sample -> select -> apply -> verify.
// Returns false with `error` set for AllCandidatesFailedError /
// VerificationRejectedError (never silently accepts a failure).
struct best_of_n_outcome {
    bool ok = false;
    best_of_n_result result;
    kimix::string error;
    best_of_n_failure failure = best_of_n_failure::none;
};
best_of_n_outcome best_of_n(kimix::string_view task_prompt,
                            kimix::string_view work_dir,
                            const sample_runner &runner,
                            const selector_fn &selector,
                            const workspace_hooks &hooks, int32_t n = 4,
                            kimix::string_view strategy = "self_eval",
                            const verify_fn &verify = {},
                            int32_t max_concurrency = k_default_max_concurrency);

// KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY (swarm/__init__.py 395-402): the
// fan-out concurrency override. Anything unparsable (or absent) falls back to
// _DEFAULT_BURST; the value is clamped to >= 1.
int32_t env_max_concurrency();

// _run_swarm (374-416) + _run_subagent_task (433-489): fan out `tasks`
// through `runner`, bounded by max_concurrency and the token-bucket rate
// limiter, retrying rate-limit failures up to _MAX_RETRIES times. Results are
// returned in task order (sorted by index).
kimix::vector<swarm_result> run_swarm(kimix::span<const swarm_task> tasks,
                                      kimix::string_view subagent_type,
                                      const swarm_runner &runner,
                                      int32_t max_concurrency = k_default_burst,
                                      double rate_interval =
                                          k_default_interval_seconds);

// Real (native) workspace hooks: git worktree when `work_dir` is inside a git
// repository, otherwise a recursive copy that skips _COPY_IGNORE.
workspace_hooks native_workspace_hooks();

// ---------------------------------------------------------------------------
// Tool class
// ---------------------------------------------------------------------------
class Workflow : public kimix::builtin_tools::Tool {
public:
    explicit Workflow(kimix::builtin_tools::Session *session);
    // Gated by Session::swarm_enabled: the reference raises SkipThisTool in
    // the constructor outside a swarm session, so the tool must not even be
    // listed there.
    bool valid() const override;
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    // Injected execution. Empty `runner` == bind the session's agents
    // registry (native_io only); empty `selector` == self-eval through the
    // same registry.
    swarm_runner runner;
    selector_fn selector;
    verify_fn verify;
    workspace_hooks workspaces;
    // Concurrency overrides (KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY).
    kimix::optional<int32_t> max_concurrency;

private:
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::workflow
