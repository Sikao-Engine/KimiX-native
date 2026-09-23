// workflow_tool.cpp - C++ port of the kimi-agent `workflow` tool (AgentSwarm)
// and the best-of-N sampling machinery. See workflow_tool.h for the reference
// line map.
//
// Unity-build rules: TU-local helpers live in an anonymous namespace inside
// kimix::builtin_tools::workflow and carry the `wf_` prefix.
//
// Reuse (never re-implemented here):
// * write::build_unified_diff - the difflib-compatible unified diff used by
//   best_of_n.collect_diff for copy-mode workspaces and untracked files.
// * proc::run_process - the git invocations behind the worktree workspace.
// * agents::agent_registry - the default swarm_runner binding.
#include "builtin_tools/workflow_tool.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

#include <core/clock.h>

#include "builtin_tools/agent_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/write_tool.h"

namespace kimix::builtin_tools::workflow {

namespace {

// ---------------------------------------------------------------------------
// Small utilities (wf_ prefix - unity build safety)
// ---------------------------------------------------------------------------

const char *wf_status_string(tool_status status) noexcept {
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

double wf_monotonic_seconds() {
    return static_cast<double>(kimix::Clock::now_ms()) / 1000.0;
}

// "{:.1f}" - Python f-string formatting (std::format rounds the exact binary
// value half-to-even, same as CPython).
kimix::string wf_format_1f(double value) {
    return kimix::format("{:.1f}", value);
}

bool wf_is_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

kimix::string wf_strip(kimix::string_view text) {
    size_t b = 0;
    size_t e = text.size();
    while (b < e && wf_is_whitespace(text[b])) {
        ++b;
    }
    while (e > b && wf_is_whitespace(text[e - 1])) {
        --e;
    }
    return kimix::string(text.substr(b, e - b));
}

// Python repr() of a string (for the duplicates set rendering).
kimix::string wf_py_repr(kimix::string_view text) {
    const bool has_single = text.find('\'') != kimix::string_view::npos;
    const bool has_double = text.find('"') != kimix::string_view::npos;
    const bool use_double = has_single && !has_double;
    kimix::string out;
    out.push_back(use_double ? '"' : '\'');
    for (const char c : text) {
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
        } else if (static_cast<unsigned char>(c) < 0x20 ||
                   static_cast<unsigned char>(c) == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out.append(buf);
        } else {
            out.push_back(c);
        }
    }
    out.push_back(use_double ? '"' : '\'');
    return out;
}

kimix::string wf_join(kimix::span<const kimix::string> parts,
                      kimix::string_view separator) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(separator.data(), separator.size());
        }
        out += parts[i];
    }
    return out;
}

// String parameter helper honouring one alias.
tool_error wf_string(const ToolParams *params, kimix::string_view name,
                     bool required, kimix::optional<kimix::string> &out) {
    out = std::nullopt;
    if (params == nullptr) {
        return required ? tool_error{tool_status::invalid_input,
                                     kimix::format("missing required field: {}",
                                                   name)}
                        : tool_error{tool_status::ok, {}};
    }
    const ValueElement *el = params->get(name);
    if (el == nullptr || el->is_null()) {
        return required ? tool_error{tool_status::invalid_input,
                                     kimix::format("missing required field: {}",
                                                   name)}
                        : tool_error{tool_status::ok, {}};
    }
    if (!el->is_string()) {
        return {tool_status::invalid_input,
                kimix::format("{} must be a string", name)};
    }
    out = el->as_string();
    return {tool_status::ok, {}};
}

void wf_error(ToolParams &result, tool_status status, kimix::string_view message,
              kimix::string_view output, kimix::string_view brief) {
    result.values["ok"] = ValueElement::make_bool(false);
    result.values["status"] =
        ValueElement::make_string(kimix::string(wf_status_string(status)));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

// best_of_n._COPY_IGNORE (105-108).
bool wf_ignored_dir(kimix::string_view name) {
    return name == ".git" || name == ".venv" || name == "venv" ||
           name == "node_modules" || name == "__pycache__" ||
           name == ".kimix_cache" || name == "bench_runs" ||
           name == ".mypy_cache" || name == ".pytest_cache" ||
           name == ".ruff_cache";
}

// best_of_n._snapshot_files ignore list (145-155) - a smaller set.
bool wf_snapshot_ignored_dir(kimix::string_view name) {
    return name == ".git" || name == ".venv" || name == "venv" ||
           name == "node_modules" || name == "__pycache__";
}

bool wf_read_binary(kimix::string_view path, kimix::string &out) {
    std::FILE *f = std::fopen(kimix::string(path).c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

// Relative path -> file content, walking `root` and skipping the snapshot
// ignore list (best_of_n._snapshot_files).
kimix::vector<std::pair<kimix::string, kimix::string>>
wf_snapshot_files(kimix::string_view root) {
    namespace fs = kimix::filesystem;
    kimix::vector<std::pair<kimix::string, kimix::string>> out;
    std::error_code ec;
    const fs::path base = fs::path(kimix::string(root));
    if (!fs::is_directory(base, ec)) {
        return out;
    }
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec),
                                          end;
         it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const fs::path &p = it->path();
        if (it->is_directory(ec)) {
            const kimix::string name = kimix::to_string(p.filename());
            if (wf_snapshot_ignored_dir(name)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const fs::path rel = fs::relative(p, base, ec);
        if (ec) {
            continue;
        }
        kimix::string rel_text = kimix::to_string(rel);
        for (char &c : rel_text) {
            if (c == '\\') {
                c = '/';
            }
        }
        kimix::string content;
        if (wf_read_binary(kimix::to_string(p), content)) {
            out.emplace_back(std::move(rel_text), std::move(content));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return out;
}

// Recursive copy that skips _COPY_IGNORE (shutil.copytree(ignore=...)).
void wf_copy_tree(kimix::string_view from, kimix::string_view to) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    const fs::path src = fs::path(kimix::string(from));
    const fs::path dst = fs::path(kimix::string(to));
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec),
                                          end;
         it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const fs::path &p = it->path();
        const fs::path rel = fs::relative(p, src, ec);
        if (ec) {
            continue;
        }
        if (it->is_directory(ec)) {
            const kimix::string name = kimix::to_string(p.filename());
            if (wf_ignored_dir(name)) {
                it.disable_recursion_pending();
                continue;
            }
            fs::create_directories(dst / rel, ec);
            continue;
        }
        // Skip any file whose ancestor directory is ignored.
        bool skip = false;
        for (const fs::path &part : rel) {
            if (wf_ignored_dir(kimix::to_string(part))) {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }
        const fs::path target = dst / rel;
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(p, target, fs::copy_options::overwrite_existing, ec);
    }
}

// Run one git command and capture stdout (best_of_n's subprocess.run calls).
bool wf_git(kimix::string_view work_dir,
            const kimix::vector<kimix::string> &args,
            kimix::string &stdout_text) {
    proc::run_options opts;
    opts.argv.push_back("git");
    for (const kimix::string &a : args) {
        opts.argv.push_back(a);
    }
    opts.working_directory = kimix::string(work_dir);
    opts.timeout_ms = 60000;
    const proc::run_result rr = proc::run_process(opts);
    stdout_text = rr.output;
    return rr.exit_code.has_value() && *rr.exit_code == 0;
}

bool wf_is_git_repo(kimix::string_view path) {
    kimix::string out;
    if (!wf_git(path, {"rev-parse", "--is-inside-work-tree"}, out)) {
        return false;
    }
    return wf_strip(out) == "true";
}

// Unique temp directory name (tempfile.mkdtemp(prefix=...)).
kimix::string wf_temp_dir(kimix::string_view prefix, int32_t index) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1) + 1;
    namespace fs = kimix::filesystem;
    const fs::path tmp = fs::temp_directory_path();
    return kimix::to_string(
        tmp / fs::path(kimix::format("{}{}_{}", prefix, index, n)));
}

kimix::vector<kimix::string> wf_split_lines(kimix::string_view text) {
    kimix::vector<kimix::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == kimix::string_view::npos) {
            if (start < text.size()) {
                out.emplace_back(text.substr(start));
            }
            break;
        }
        out.emplace_back(text.substr(start, nl - start + 1));
        start = nl + 1;
    }
    return out;
}

// difflib.unified_diff([], content.splitlines(keepends=True),
//                      fromfile="/dev/null", tofile=rel) for an untracked file.
kimix::string wf_new_file_diff(kimix::string_view rel,
                               kimix::string_view content) {
    return write::build_unified_diff(kimix::string_view(), content, rel, true);
}

} // namespace

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

static const kimix::builtin_tools::param_alias k_workflow_aliases[] = {
    {"description", "desc name task_description summary"},
    {"mode", "workflow_mode strategy"},
    {"sample_n", "n samples sample_count sample_size num_samples"},
    {"selector", "selection select choice"},
    {"subagent_type", "agent_type type subagent_type_name"},
    {"prompt_template", "template prompt_pattern template_text"},
    {"prompt_prefix", "prefix"},
    {"prompt_suffix", "suffix"},
    {"items", "inputs tasks rows"},
    {"resume_agent_ids", "agent_ids resume_ids sessions resume"},
};

tool_error parse_params(const ToolParams *params, workflow_params &out) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("command" for "cmd") are accepted; the canonical name always wins.
    const kimix::builtin_tools::ToolParams k_resolved =
        kimix::builtin_tools::ToolParams::with_aliases(params, k_workflow_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = workflow_params{};
    kimix::optional<kimix::string> description;
    tool_error err = wf_string(params, "description", true, description);
    if (err.failed()) {
        return err;
    }
    out.description = description.value_or(kimix::string());

    kimix::optional<kimix::string> mode;
    err = wf_string(params, "mode", false, mode);
    if (err.failed()) {
        return err;
    }
    if (mode.has_value()) {
        if (*mode != "fanout" && *mode != "parallel_sample") {
            return {tool_status::invalid_input,
                    kimix::format("Input should be 'fanout' or "
                                  "'parallel_sample' (mode={})",
                                  kimix::string_view(*mode))};
        }
        out.mode = *mode;
    }
    if (params != nullptr) {
        if (const ValueElement *n = params->get("sample_n");
            n != nullptr && !n->is_null()) {
            if (!n->is_int() && !n->is_uint()) {
                return {tool_status::invalid_input,
                        "sample_n must be an integer"};
            }
            out.sample_n = static_cast<int32_t>(
                n->is_int() ? n->as_int()
                            : static_cast<int64_t>(n->as_uint()));
        }
        if (const ValueElement *s = params->get("selector");
            s != nullptr && !s->is_null()) {
            if (!s->is_string()) {
                return {tool_status::invalid_input, "selector must be a string"};
            }
            const kimix::string &sel = s->as_string();
            if (sel != "self_eval" && sel != "majority") {
                return {tool_status::invalid_input,
                        kimix::format("Input should be 'self_eval' or "
                                      "'majority' (selector={})",
                                      kimix::string_view(sel))};
            }
            out.selector = sel;
        }
        if (const ValueElement *t = params->get("subagent_type");
            t != nullptr && t->is_string()) {
            out.subagent_type = t->as_string();
        }
    }
    err = wf_string(params, "prompt_template", false, out.prompt_template);
    if (err.failed()) {
        return err;
    }
    err = wf_string(params, "prompt_prefix", false, out.prompt_prefix);
    if (err.failed()) {
        return err;
    }
    err = wf_string(params, "prompt_suffix", false, out.prompt_suffix);
    if (err.failed()) {
        return err;
    }
    if (params != nullptr) {
        if (const ValueElement *items = params->get("items");
            items != nullptr && !items->is_null()) {
            if (!items->is_array()) {
                return {tool_status::invalid_input,
                        "items must be a list of strings"};
            }
            for (const ValueElement &item : items->as_array()) {
                if (!item.is_string()) {
                    return {tool_status::invalid_input,
                            "items must be a list of strings"};
                }
                out.items.push_back(item.as_string());
            }
        }
        if (const ValueElement *resume = params->get("resume_agent_ids");
            resume != nullptr && !resume->is_null()) {
            const ToolParams *obj = resume->as_object();
            if (obj == nullptr) {
                return {tool_status::invalid_input,
                        "resume_agent_ids must be a mapping of agent id to "
                        "prompt"};
            }
            kimix::vector<kimix::string> keys;
            keys.reserve(obj->values.size());
            for (const auto &kv : obj->values) {
                keys.push_back(kv.first);
            }
            std::sort(keys.begin(), keys.end());
            for (const kimix::string &key : keys) {
                const ValueElement *value = obj->get(key);
                if (value == nullptr || !value->is_string()) {
                    return {tool_status::invalid_input,
                            "resume_agent_ids values must be strings"};
                }
                out.resume_agent_ids.emplace_back(key, value->as_string());
            }
        }
    }

    // ---- AgentSwarmParams._validate (114-152) ---------------------------
    if (out.mode == "parallel_sample") {
        if (out.sample_n.has_value() && *out.sample_n < 1) {
            return {tool_status::invalid_input, "sample_n must be >= 1."};
        }
        const bool uses_template = out.prompt_template.has_value();
        const bool uses_prefix = out.prompt_prefix.has_value();
        if (uses_template && uses_prefix) {
            return {tool_status::invalid_input,
                    "Use either prompt_template or prompt_prefix+suffix, not "
                    "both."};
        }
        if (!uses_template && !uses_prefix) {
            return {tool_status::invalid_input,
                    "parallel_sample mode requires the task prompt via "
                    "prompt_template or prompt_prefix (+prompt_suffix)."};
        }
        return {tool_status::ok, {}};
    }
    const size_t resume_count = out.resume_agent_ids.size();
    if (out.items.size() < 2 && resume_count == 0) {
        return {tool_status::invalid_input,
                "Provide at least 2 items or resume_agent_ids."};
    }
    const size_t total = out.items.size() + resume_count;
    if (total > static_cast<size_t>(k_max_sub_agents)) {
        return {tool_status::invalid_input,
                kimix::format("Max {} sub-agents per swarm.", k_max_sub_agents)};
    }
    const bool uses_template =
        out.prompt_template.has_value() &&
        out.prompt_template->find("{{item}}") != kimix::string::npos;
    const bool uses_prefix = out.prompt_prefix.has_value();
    if (uses_template && uses_prefix) {
        return {tool_status::invalid_input,
                "Use either prompt_template or prompt_prefix+suffix, not "
                "both."};
    }
    if (!uses_template && !uses_prefix) {
        return {tool_status::invalid_input,
                "prompt_template must contain '{{item}}', or set "
                "prompt_prefix. For example: prompt_template='Fix errors in "
                "{{item}}.'"};
    }
    if (uses_template &&
        out.prompt_template->find("{{item}}") == kimix::string::npos) {
        return {tool_status::invalid_input,
                "prompt_template must contain '{{item}}'. For example: 'Fix "
                "all lint errors in {{item}}.'"};
    }
    return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------

kimix::vector<kimix::string>
expand_template(kimix::optional<kimix::string> prompt_template,
                kimix::span<const kimix::string> items,
                kimix::optional<kimix::string> prefix,
                kimix::optional<kimix::string> suffix) {
    kimix::vector<kimix::string> out;
    out.reserve(items.size());
    if (prompt_template.has_value() &&
        prompt_template->find("{{item}}") != kimix::string::npos) {
        for (const kimix::string &item : items) {
            kimix::string expanded;
            const kimix::string &tpl = *prompt_template;
            size_t pos = 0;
            while (pos < tpl.size()) {
                const size_t hit = tpl.find("{{item}}", pos);
                if (hit == kimix::string::npos) {
                    expanded.append(tpl.data() + pos, tpl.size() - pos);
                    break;
                }
                expanded.append(tpl.data() + pos, hit - pos);
                expanded += item;
                pos = hit + 8; // strlen("{{item}}") == 8
            }
            out.push_back(std::move(expanded));
        }
        return out;
    }
    if (prefix.has_value()) {
        const kimix::string suffix_text = suffix.value_or(kimix::string());
        for (const kimix::string &item : items) {
            out.push_back(*prefix + item + suffix_text);
        }
        return out;
    }
    for (const kimix::string &item : items) {
        out.push_back(item);
    }
    return out;
}

kimix::string validate_uniqueness(kimix::span<const kimix::string> prompts) {
    kimix::vector<kimix::string> seen;
    kimix::vector<kimix::string> duplicates;
    for (const kimix::string &p : prompts) {
        bool already_seen = false;
        for (const kimix::string &s : seen) {
            if (s == p) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            bool dup_listed = false;
            for (const kimix::string &d : duplicates) {
                if (d == p) {
                    dup_listed = true;
                    break;
                }
            }
            if (!dup_listed) {
                duplicates.push_back(p);
            }
        } else {
            seen.push_back(p);
        }
    }
    if (duplicates.empty()) {
        return {};
    }
    // Python renders a set literal; the iteration order of a str set is
    // unspecified, so the duplicates are sorted for a deterministic message.
    std::sort(duplicates.begin(), duplicates.end());
    kimix::vector<kimix::string> rendered;
    rendered.reserve(duplicates.size());
    for (const kimix::string &d : duplicates) {
        rendered.push_back(wf_py_repr(d));
    }
    return "Expanded prompts must be unique; duplicates: {" +
           wf_join(kimix::span<const kimix::string>(rendered), ", ") + "}";
}

kimix::string xml_escape(kimix::string_view text) {
    // html.escape(text, quote=True)
    kimix::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out.append("&amp;");
            break;
        case '<':
            out.append("&lt;");
            break;
        case '>':
            out.append("&gt;");
            break;
        case '"':
            out.append("&quot;");
            break;
        case '\'':
            out.append("&#x27;");
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

kimix::string render_results(kimix::span<const swarm_result> results,
                             kimix::string_view description) {
    size_t success_count = 0;
    for (const swarm_result &r : results) {
        if (r.success) {
            ++success_count;
        }
    }
    const size_t failed_count = results.size() - success_count;
    kimix::vector<kimix::string> lines;
    lines.push_back("<agent_swarm_result>");
    lines.push_back("  <description>" + xml_escape(description) +
                    "</description>");
    lines.push_back(kimix::format("  <total>{}</total>", results.size()));
    lines.push_back(kimix::format("  <succeeded>{}</succeeded>", success_count));
    lines.push_back(kimix::format("  <failed>{}</failed>", failed_count));
    if (failed_count > 0) {
        lines.push_back(
            "  <resume_hint>Some sub-agents failed. Re-run with "
            "resume_agent_ids mapping the failed agent IDs to adjusted "
            "prompts.</resume_hint>");
    }
    lines.push_back("  <subagents>");
    for (const swarm_result &r : results) {
        const kimix::string success_str = r.success ? "true" : "false";
        const kimix::string elapsed_str =
            (r.elapsed.has_value() && *r.elapsed != 0.0)
                ? wf_format_1f(*r.elapsed) + "s"
                : kimix::string("-");
        lines.push_back(kimix::format(
            " <subagent id=\"{}\" index=\"{}\" success=\"{}\" elapsed=\"{}\">",
            kimix::string_view(xml_escape(r.agent_id)), r.index,
            kimix::string_view(success_str),
            kimix::string_view(elapsed_str)));
        lines.push_back(" <output>" + xml_escape(r.output) + "</output>");
        if (r.error.has_value() && !r.error->empty()) {
            lines.push_back(" <error>" + xml_escape(*r.error) + "</error>");
        }
        lines.push_back(" </subagent>");
    }
    lines.push_back("  </subagents>");
    lines.push_back("</agent_swarm_result>");
    return wf_join(kimix::span<const kimix::string>(lines), "\n");
}

kimix::string render_best_of_n(const best_of_n_result &result,
                               kimix::string_view description) {
    kimix::vector<kimix::string> lines;
    lines.push_back("<best_of_n_result>");
    lines.push_back("  <description>" + xml_escape(description) +
                    "</description>");
    lines.push_back(
        kimix::format("  <samples>{}</samples>", result.candidates.size()));
    lines.push_back(kimix::format("  <winner>{}</winner>", result.winner_index));
    lines.push_back("  <selection>" + xml_escape(result.selection_reason) +
                    "</selection>");
    for (const sample_candidate &c : result.candidates) {
        const kimix::string status =
            c.success ? kimix::string("ok")
                      : "failed: " + xml_escape(c.error.value_or(kimix::string()));
        lines.push_back(kimix::format("  <candidate index=\"{}\" status=\"{}\"/>",
                                      c.index,
                                      kimix::string_view(xml_escape(status))));
    }
    lines.push_back("</best_of_n_result>");
    return wf_join(kimix::span<const kimix::string>(lines), "\n");
}

bool is_rate_limit_error(kimix::string_view text) {
    static constexpr kimix::string_view markers[] = {
        "rate limit",   "rate-limit",     "too many requests", "429",
        "capacity",     "throttled",      "quota exceeded"};
    kimix::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    for (const kimix::string_view marker : markers) {
        if (lowered.find(marker) != kimix::string::npos) {
            return true;
        }
    }
    return false;
}

double retry_delay_seconds(int32_t attempt) {
    double factor = 1.0;
    for (int32_t i = 0; i < attempt; ++i) {
        factor *= 2.0;
    }
    return k_retry_base_seconds * factor;
}

// ---------------------------------------------------------------------------
// rate_limiter (swarm/__init__.py 150-179)
// ---------------------------------------------------------------------------

rate_limiter::rate_limiter(int32_t burst, double interval, double now) {
    start(burst, interval, now);
}

void rate_limiter::start(int32_t burst, double interval, double now) {
    _burst = (burst < 1) ? 1 : burst;
    _interval = (interval <= 0.0) ? 1.0 : interval;
    _tokens = static_cast<double>(_burst);
    _last = now;
}

double rate_limiter::acquire(double now) {
    double wait_seconds = 0.0;
    _tokens = std::min(static_cast<double>(_burst),
                       _tokens + (now - _last) / _interval);
    if (_tokens < 1.0) {
        wait_seconds = (1.0 - _tokens) * _interval;
        // The reference sleeps and then re-reads the clock; the caller does
        // the sleeping, so the refill is computed for the post-sleep instant.
        const double after = now + wait_seconds;
        _tokens = std::min(static_cast<double>(_burst),
                           _tokens + (after - _last) / _interval);
    }
    _tokens -= 1.0;
    _last = now + wait_seconds;
    return wait_seconds;
}

// ---------------------------------------------------------------------------
// best-of-N kernels
// ---------------------------------------------------------------------------

kimix::string
format_candidates_for_review(kimix::span<const sample_candidate> candidates) {
    kimix::vector<kimix::string> parts;
    parts.reserve(candidates.size());
    for (const sample_candidate &c : candidates) {
        const kimix::string status =
            c.success ? kimix::string("ok")
                      : "failed: " + c.error.value_or(kimix::string());
        parts.push_back(kimix::format(
            "=== Candidate {} ({}, {} steps) ===\nSelf-report:\n{}\n\nDiff:\n{}",
            c.index, kimix::string_view(status), c.steps,
            kimix::string_view(c.self_report), kimix::string_view(c.diff)));
    }
    return wf_join(kimix::span<const kimix::string>(parts), "\n\n");
}

kimix::string all_candidates_failed_message(
    kimix::span<const sample_candidate> candidates) {
    kimix::vector<kimix::string> pieces;
    pieces.reserve(candidates.size());
    for (const sample_candidate &c : candidates) {
        pieces.push_back(kimix::format("#{}: {}", c.index,
                                       kimix::string_view(
                                           c.error.value_or(kimix::string()))));
    }
    return kimix::format("all {} sampled candidates failed: ",
                         candidates.size()) +
           wf_join(kimix::span<const kimix::string>(pieces), "; ");
}

kimix::string single_run_failed_message(kimix::string_view error) {
    kimix::string out = "single run failed: ";
    out.append(error.data(), error.size());
    return out;
}

kimix::string verification_rejected_message(int32_t winner_index,
                                            kimix::string_view detail) {
    kimix::string out = kimix::format("selected candidate #{} failed "
                                      "verification: ",
                                      winner_index);
    out.append(detail.data(), detail.size());
    return out;
}

kimix::string
format_votes(kimix::span<const std::pair<int32_t, int32_t>> votes) {
    kimix::vector<kimix::string> pieces;
    pieces.reserve(votes.size());
    for (const auto &kv : votes) {
        pieces.push_back(kimix::format("{}: {}", kv.first, kv.second));
    }
    return "{" + wf_join(kimix::span<const kimix::string>(pieces), ", ") + "}";
}

selection_outcome
select_best_candidate(kimix::string_view task_prompt,
                      kimix::span<const sample_candidate> candidates,
                      const selector_fn &selector, kimix::string_view strategy) {
    selection_outcome out;
    kimix::vector<const sample_candidate *> viable;
    for (const sample_candidate &c : candidates) {
        if (c.success) {
            viable.push_back(&c);
        }
    }
    if (viable.empty()) {
        out.error = all_candidates_failed_message(candidates);
        return out;
    }
    if (viable.size() == 1) {
        out.ok = true;
        out.winner_index = viable[0]->index;
        out.reason = "only one viable candidate";
        return out;
    }
    if (strategy == "majority" && viable.size() >= 3) {
        kimix::vector<std::pair<int32_t, int32_t>> votes;
        votes.reserve(viable.size());
        for (const sample_candidate *c : viable) {
            votes.emplace_back(c->index, 0);
        }
        auto vote_for = [&votes](int32_t index) -> int32_t & {
            for (auto &kv : votes) {
                if (kv.first == index) {
                    return kv.second;
                }
            }
            votes.emplace_back(index, 0);
            return votes.back().second;
        };
        for (size_t i = 0; i < viable.size(); ++i) {
            for (size_t j = i + 1; j < viable.size(); ++j) {
                const sample_candidate &a = *viable[i];
                const sample_candidate &b = *viable[j];
                const kimix::string pair_text = kimix::format(
                    "=== Candidate A (#{}) ===\n{}\n=== Candidate B (#{}) "
                    "===\n{}",
                    a.index, kimix::string_view(a.diff), b.index,
                    kimix::string_view(b.diff));
                const int32_t chosen = selector(task_prompt, pair_text);
                if (chosen == b.index) {
                    vote_for(b.index) += 1;
                } else {
                    vote_for(a.index) += 1;
                }
            }
        }
        // max(votes, key=lambda idx: (votes[idx], -idx))
        int32_t winner = votes[0].first;
        for (const auto &kv : votes) {
            if (kv.second > vote_for(winner) ||
                (kv.second == vote_for(winner) && kv.first < winner)) {
                winner = kv.first;
            }
        }
        out.ok = true;
        out.winner_index = winner;
        out.reason = "majority vote " + format_votes(
                                            kimix::span<const std::pair<
                                                int32_t, int32_t>>(votes));
        return out;
    }
    kimix::vector<sample_candidate> viable_copy;
    viable_copy.reserve(viable.size());
    for (const sample_candidate *c : viable) {
        viable_copy.push_back(*c);
    }
    const kimix::string review_text = format_candidates_for_review(
        kimix::span<const sample_candidate>(viable_copy));
    const int32_t chosen = selector(task_prompt, review_text);
    bool is_viable = false;
    for (const sample_candidate *c : viable) {
        if (c->index == chosen) {
            is_viable = true;
            break;
        }
    }
    out.ok = true;
    if (!is_viable) {
        out.winner_index = viable[0]->index;
        out.reason = kimix::format(
            "selector returned invalid index {}; fell back", chosen);
        return out;
    }
    out.winner_index = chosen;
    out.reason = "self-eval selection";
    return out;
}

kimix::vector<sample_candidate>
run_parallel_sample(kimix::string_view task_prompt, int32_t n,
                    kimix::string_view work_dir, const sample_runner &runner,
                    const workspace_hooks &hooks, int32_t max_concurrency) {
    kimix::vector<sample_candidate> out;
    if (n < 1) {
        return out; // ValueError("n must be >= 1") - callers validate first
    }
    out.resize(static_cast<size_t>(n));
    const int32_t concurrency = std::max<int32_t>(1, max_concurrency);
    std::atomic<int32_t> next_index{0};
    auto worker = [&]() {
        while (true) {
            const int32_t index = next_index.fetch_add(1);
            if (index >= n) {
                return;
            }
            sample_candidate candidate;
            candidate.index = index;
            kimix::string kind = "copy";
            kimix::string worker_path;
            if (hooks.create) {
                const auto created = hooks.create(work_dir, index);
                worker_path = created.first;
                kind = created.second;
            }
            candidate.work_dir = worker_path;
            const sample_run_outcome report =
                runner(task_prompt, worker_path);
            if (report.ok) {
                candidate.self_report = report.self_report;
                candidate.steps = report.steps;
                candidate.output_tokens = report.output_tokens;
                candidate.diff = hooks.collect_diff
                                     ? hooks.collect_diff(worker_path, kind,
                                                          work_dir)
                                     : kimix::string();
                candidate.success = true;
            } else {
                candidate.error = report.error;
                candidate.success = false;
            }
            // "Stash kind on the candidate via diff marker for apply step."
            candidate.diff = "[workspace:" + kind + "]\n" + candidate.diff;
            out[static_cast<size_t>(index)] = std::move(candidate);
        }
    };
    if (concurrency <= 1) {
        worker();
        return out;
    }
    kimix::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(concurrency));
    for (int32_t i = 0; i < concurrency; ++i) {
        pool.emplace_back(worker);
    }
    for (std::thread &t : pool) {
        if (t.joinable()) {
            t.join();
        }
    }
    return out;
}

best_of_n_outcome best_of_n(kimix::string_view task_prompt,
                            kimix::string_view work_dir,
                            const sample_runner &runner,
                            const selector_fn &selector,
                            const workspace_hooks &hooks, int32_t n,
                            kimix::string_view strategy, const verify_fn &verify,
                            int32_t max_concurrency) {
    best_of_n_outcome out;
    if (n <= 1) {
        out.result.candidates =
            run_parallel_sample(task_prompt, 1, work_dir, runner, hooks, 1);
        if (out.result.candidates.empty()) {
            out.error = "n must be >= 1";
            return out;
        }
        const sample_candidate &only = out.result.candidates[0];
        if (!only.success) {
            out.error =
                single_run_failed_message(only.error.value_or(kimix::string()));
            return out;
        }
        out.ok = true;
        out.result.winner_index = only.index;
        out.result.selection_reason = "n=1: no selection";
        return out;
    }
    out.result.candidates = run_parallel_sample(
        task_prompt, n, work_dir, runner, hooks, max_concurrency);
    const selection_outcome selection = select_best_candidate(
        task_prompt,
        kimix::span<const sample_candidate>(out.result.candidates), selector,
        strategy);
    if (!selection.ok) {
        out.error = selection.error;
        return out;
    }
    out.result.winner_index = selection.winner_index;
    out.result.selection_reason = selection.reason;
    const sample_candidate *winner = nullptr;
    for (const sample_candidate &c : out.result.candidates) {
        if (c.index == selection.winner_index) {
            winner = &c;
            break;
        }
    }
    if (winner != nullptr) {
        const bool worktree =
            winner->diff.rfind("[workspace:worktree]", 0) == 0;
        const kimix::string kind = worktree ? "worktree" : "copy";
        if (hooks.apply) {
            hooks.apply(*winner, work_dir, kind);
        }
    }
    if (verify) {
        const std::pair<bool, kimix::string> verdict = verify(work_dir);
        out.result.verified = verdict.first;
        out.result.verify_detail = verdict.second;
        if (!verdict.first) {
            out.error = verification_rejected_message(selection.winner_index,
                                                      verdict.second);
            return out;
        }
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// run_swarm
// ---------------------------------------------------------------------------

kimix::vector<swarm_result> run_swarm(kimix::span<const swarm_task> tasks,
                                      kimix::string_view subagent_type,
                                      const swarm_runner &runner,
                                      int32_t max_concurrency,
                                      double rate_interval) {
    kimix::vector<swarm_result> results;
    results.resize(tasks.size());
    if (tasks.empty()) {
        return results;
    }
    const int32_t concurrency = std::max<int32_t>(1, max_concurrency);
    rate_limiter limiter(concurrency < k_default_burst ? concurrency
                                                       : k_default_burst,
                         rate_interval, wf_monotonic_seconds());
    kimix::spin_mutex limiter_mutex;
    std::atomic<size_t> next_task{0};

    auto worker = [&]() {
        while (true) {
            const size_t index = next_task.fetch_add(1);
            if (index >= tasks.size()) {
                return;
            }
            {
                std::lock_guard<kimix::spin_mutex> g(limiter_mutex);
                const double wait_seconds =
                    limiter.acquire(wf_monotonic_seconds());
                if (wait_seconds > 0.0) {
                    const auto duration = std::chrono::duration<double>(wait_seconds);
                    std::this_thread::sleep_for(duration);
                }
            }
            const swarm_task &task = tasks[index];
            const double started = wf_monotonic_seconds();
            swarm_result result = runner(task, subagent_type);
            if (result.elapsed.has_value() == false) {
                result.elapsed = wf_monotonic_seconds() - started;
            }
            if (result.index == 0) {
                result.index = task.index;
            }
            results[index] = std::move(result);
        }
    };

    if (concurrency <= 1 || tasks.size() == 1) {
        worker();
    } else {
        kimix::vector<std::thread> pool;
        const size_t pool_size =
            std::min<size_t>(static_cast<size_t>(concurrency), tasks.size());
        pool.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            pool.emplace_back(worker);
        }
        for (std::thread &t : pool) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    // results.sort(key=lambda r: r.index)
    std::stable_sort(results.begin(), results.end(),
                     [](const swarm_result &a, const swarm_result &b) {
                         return a.index < b.index;
                     });
    return results;
}

// ---------------------------------------------------------------------------
// Native workspace hooks (best_of_n.py 66-232)
// ---------------------------------------------------------------------------

workspace_hooks native_workspace_hooks() {
    workspace_hooks hooks;
    hooks.create = [](kimix::string_view work_dir,
                      int32_t index) -> std::pair<kimix::string, kimix::string> {
        namespace fs = kimix::filesystem;
        if (wf_is_git_repo(work_dir)) {
            kimix::string worker_path = wf_temp_dir("best_of_n_wt_", index);
            // git worktree add requires the target to not exist yet.
            std::error_code ec;
            fs::remove_all(fs::path(worker_path), ec);
            kimix::string out;
            if (wf_git(work_dir,
                       {"worktree", "add", "--detach", worker_path, "HEAD"},
                       out)) {
                return {worker_path, "worktree"};
            }
        }
        const kimix::string worker_path = wf_temp_dir("best_of_n_", index);
        wf_copy_tree(work_dir, worker_path);
        return {worker_path, "copy"};
    };
    hooks.cleanup = [](kimix::string_view worker_path, kimix::string_view kind,
                       kimix::string_view main_work_dir) {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        if (kind == "worktree") {
            kimix::string out;
            wf_git(main_work_dir,
                   {"worktree", "remove", "--force",
                    kimix::string(worker_path)},
                   out);
        }
        fs::remove_all(fs::path(kimix::string(worker_path)), ec);
    };
    hooks.collect_diff = [](kimix::string_view worker_path,
                            kimix::string_view kind,
                            kimix::string_view main_work_dir) -> kimix::string {
        if (kind == "worktree") {
            kimix::string diff;
            kimix::string out;
            if (wf_git(worker_path, {"diff", "HEAD"}, out)) {
                diff = out;
            }
            // Include untracked files.
            kimix::string untracked;
            if (wf_git(worker_path,
                       {"ls-files", "--others", "--exclude-standard"},
                       untracked)) {
                for (const kimix::string &rel : wf_split_lines(untracked)) {
                    const kimix::string trimmed = wf_strip(rel);
                    if (trimmed.empty()) {
                        continue;
                    }
                    kimix::string content;
                    const kimix::string full =
                        kimix::to_string(kimix::filesystem::path(
                                             kimix::string(worker_path)) /
                                         kimix::filesystem::path(trimmed));
                    if (!wf_read_binary(full, content)) {
                        continue;
                    }
                    diff += wf_new_file_diff(trimmed, content);
                }
            }
            return diff;
        }
        // Copy mode: diff the worker tree against the main workspace.
        const auto before = wf_snapshot_files(main_work_dir);
        const auto after = wf_snapshot_files(worker_path);
        kimix::vector<kimix::string> rels;
        rels.reserve(before.size() + after.size());
        for (const auto &kv : before) {
            rels.push_back(kv.first);
        }
        for (const auto &kv : after) {
            bool present = false;
            for (const kimix::string &existing : rels) {
                if (existing == kv.first) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                rels.push_back(kv.first);
            }
        }
        std::sort(rels.begin(), rels.end());
        auto lookup = [](const kimix::vector<std::pair<kimix::string, kimix::string>> &set,
                         kimix::string_view key) -> const kimix::string * {
            for (const auto &kv : set) {
                if (kv.first == key) {
                    return &kv.second;
                }
            }
            return nullptr;
        };
        kimix::string chunks;
        for (const kimix::string &rel : rels) {
            const kimix::string empty;
            const kimix::string &old_text =
                lookup(before, rel) ? *lookup(before, rel) : empty;
            const kimix::string &new_text =
                lookup(after, rel) ? *lookup(after, rel) : empty;
            if (old_text == new_text) {
                continue;
            }
            chunks += write::build_unified_diff(old_text, new_text, rel, true);
        }
        return chunks;
    };
    hooks.apply = [](const sample_candidate &winner,
                     kimix::string_view main_work_dir,
                     kimix::string_view kind) {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        if (kind == "worktree") {
            kimix::string changed;
            wf_git(winner.work_dir, {"diff", "--name-only", "HEAD"}, changed);
            kimix::vector<kimix::string> names;
            for (const kimix::string &line : wf_split_lines(changed)) {
                const kimix::string trimmed = wf_strip(line);
                if (!trimmed.empty()) {
                    names.push_back(trimmed);
                }
            }
            kimix::string untracked;
            if (wf_git(winner.work_dir,
                       {"ls-files", "--others", "--exclude-standard"},
                       untracked)) {
                for (const kimix::string &line : wf_split_lines(untracked)) {
                    const kimix::string trimmed = wf_strip(line);
                    if (!trimmed.empty()) {
                        names.push_back(trimmed);
                    }
                }
            }
            for (const kimix::string &rel : names) {
                const fs::path src =
                    fs::path(kimix::string(winner.work_dir)) / fs::path(rel);
                const fs::path dst =
                    fs::path(kimix::string(main_work_dir)) / fs::path(rel);
                if (fs::exists(src, ec)) {
                    fs::create_directories(dst.parent_path(), ec);
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing,
                                  ec);
                } else if (fs::exists(dst, ec)) {
                    fs::remove(dst, ec);
                }
            }
            return;
        }
        wf_copy_tree(winner.work_dir, main_work_dir);
    };
    return hooks;
}

// ---------------------------------------------------------------------------
// Tool class
// ---------------------------------------------------------------------------

Workflow::Workflow(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Workflow::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;

    // Recursive guard: sub-agents must not spawn further swarms.
    if (_session != nullptr && _session->is_sub_agent) {
        wf_error(result, tool_status::blocked,
                 "Recursive sub-agent swarm call detected.", "",
                 "sub-agent recursively called workflow");
        result.serialize(_result);
        return;
    }
    // SkipThisTool: only offered inside a swarm session.
    if (_session != nullptr && !_session->swarm_enabled) {
        wf_error(result, tool_status::unsupported,
                 "workflow is only available in a swarm session", "",
                 "invalid tool.");
        result.serialize(_result);
        return;
    }

    workflow_params params;
    const tool_error perr = parse_params(parameters, params);
    if (perr.failed()) {
        wf_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }

    // Bind the default runner through the session's agent registry.
    swarm_runner active_runner = runner;
    if (!active_runner) {
        agents::agent_registry &registry = agents::session_registry(_session);
        if (registry.runner) {
            active_runner = [&registry](const swarm_task &task,
                                        kimix::string_view type) {
                agents::subagent_request request;
                request.session_id = task.agent_id.value_or(kimix::string());
                request.prompt = task.prompt;
                request.subagent_type = kimix::string(type);
                request.close_session = true;
                request.background = false;
                const agents::subagent_run_result run = registry.runner(request);
                swarm_result out;
                out.index = task.index;
                out.agent_id = task.agent_id.value_or(kimix::string("unknown"));
                out.output = run.ok ? (run.output.empty() ? "(no text output)"
                                                          : run.output)
                                    : run.error;
                out.success = run.ok;
                if (!run.ok) {
                    out.error = run.error;
                }
                return out;
            };
        }
    }
    if (!active_runner) {
        wf_error(result, tool_status::unsupported,
                 "native workflow requires an injected runner", "",
                 "Unsupported");
        result.serialize(_result);
        return;
    }

    workspace_hooks hooks = workspaces;
    if (!hooks.create && _session != nullptr && _session->native_io) {
        hooks = native_workspace_hooks();
    }

    const int32_t concurrency =
        max_concurrency.value_or(k_default_burst);

    // ---- parallel_sample (best-of-N) ------------------------------------
    if (params.mode == "parallel_sample") {
        kimix::string task_prompt;
        if (params.prompt_template.has_value()) {
            kimix::string stripped = *params.prompt_template;
            size_t pos = 0;
            while ((pos = stripped.find("{{item}}", pos)) !=
                   kimix::string::npos) {
                stripped.erase(pos, 8);
            }
            task_prompt = wf_strip(stripped);
        } else {
            task_prompt = wf_strip(params.prompt_prefix.value_or(kimix::string()) +
                                   params.prompt_suffix.value_or(kimix::string()));
        }
        const int32_t n = params.sample_n.value_or(k_default_sample_n);
        const kimix::string strategy =
            params.selector.value_or(kimix::string("self_eval"));

        sample_runner sampler = [&active_runner, &params](
                                    kimix::string_view prompt,
                                    kimix::string_view worker_dir) {
            sample_run_outcome outcome;
            swarm_task task;
            task.prompt = kimix::string(prompt);
            task.index = 0;
            (void)worker_dir;
            const swarm_result run = active_runner(task, params.subagent_type);
            outcome.ok = run.success;
            outcome.self_report = run.output;
            if (!run.success) {
                outcome.error = run.error.value_or(kimix::string("sample failed"));
            }
            return outcome;
        };
        selector_fn active_selector = selector;
        if (!active_selector) {
            // Default self-eval: one review sub-agent picks a candidate index.
            active_selector = [&active_runner, &params](
                                  kimix::string_view prompt,
                                  kimix::string_view review_text) -> int32_t {
                const kimix::string review_prompt =
                    kimix::string(
                        "You are reviewing multiple candidate solutions for "
                        "the same task.\n\nTask:\n") +
                    kimix::string(prompt) + "\n\n" +
                    kimix::string(review_text) +
                    "\n\nReply with ONLY the integer index of the best "
                    "candidate (most correct, complete, and verified). No "
                    "explanation.";
                swarm_task task;
                task.prompt = review_prompt;
                task.index = 0;
                const swarm_result run =
                    active_runner(task, params.subagent_type);
                // regex.search(r"-?\d+", report)
                const kimix::string &report = run.output;
                size_t i = 0;
                while (i < report.size()) {
                    const char c = report[i];
                    const bool digit = (c >= '0' && c <= '9');
                    if (digit || (c == '-' && i + 1 < report.size() &&
                                  report[i + 1] >= '0' && report[i + 1] <= '9')) {
                        size_t j = i + (c == '-' ? 1 : 0);
                        while (j < report.size() && report[j] >= '0' &&
                               report[j] <= '9') {
                            ++j;
                        }
                        if (j > i + (c == '-' ? 1 : 0)) {
                            return static_cast<int32_t>(
                                std::strtol(kimix::string(report.substr(i, j - i))
                                                .c_str(),
                                            nullptr, 10));
                        }
                    }
                    ++i;
                }
                return 0;
            };
        }

        const kimix::string work_dir =
            (_session != nullptr && !_session->work_dir.empty())
                ? _session->work_dir
                : kimix::string(".");
        const best_of_n_outcome outcome =
            best_of_n(task_prompt, work_dir, sampler, active_selector, hooks, n,
                      strategy, verify, concurrency);
        if (!outcome.ok) {
            wf_error(result, tool_status::external_library, outcome.error, "",
                     outcome.error.find("verification") != kimix::string::npos
                         ? "selected sample failed verification"
                         : "all samples failed");
            result.serialize(_result);
            return;
        }
        result.values["ok"] = ValueElement::make_bool(true);
        result.values["status"] = ValueElement::make_string(kimix::string("ok"));
        result.values["message"] = ValueElement::make_string(kimix::string());
        result.values["output"] = ValueElement::make_string(
            render_best_of_n(outcome.result, params.description));
        result.values["brief"] =
            ValueElement::make_string(kimix::string("best-of-N completed"));
        result.serialize(_result);
        return;
    }

    // ---- fanout ----------------------------------------------------------
    const kimix::vector<kimix::string> expanded = expand_template(
        params.prompt_template,
        kimix::span<const kimix::string>(params.items), params.prompt_prefix,
        params.prompt_suffix);
    const kimix::string uniqueness_error = validate_uniqueness(
        kimix::span<const kimix::string>(expanded));
    if (!uniqueness_error.empty()) {
        wf_error(result, tool_status::invalid_input, uniqueness_error, "",
                 "duplicate prompts");
        result.serialize(_result);
        return;
    }
    kimix::vector<swarm_task> tasks;
    tasks.reserve(expanded.size() + params.resume_agent_ids.size());
    for (size_t i = 0; i < expanded.size(); ++i) {
        swarm_task task;
        task.prompt = expanded[i];
        task.index = static_cast<int32_t>(i);
        tasks.push_back(std::move(task));
    }
    const size_t offset = tasks.size();
    for (size_t i = 0; i < params.resume_agent_ids.size(); ++i) {
        swarm_task task;
        task.prompt = params.resume_agent_ids[i].second;
        task.agent_id = params.resume_agent_ids[i].first;
        task.index = static_cast<int32_t>(offset + i);
        tasks.push_back(std::move(task));
    }

    const kimix::vector<swarm_result> results = run_swarm(
        kimix::span<const swarm_task>(tasks), params.subagent_type,
        active_runner, concurrency, k_default_interval_seconds);

    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(kimix::string());
    result.values["output"] = ValueElement::make_string(
        render_results(kimix::span<const swarm_result>(results),
                       params.description));
    result.values["brief"] =
        ValueElement::make_string(kimix::string("Swarm completed"));
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// Static registration
// ---------------------------------------------------------------------------

KIMIX_REGISTER_TOOL(
    Workflow,
    "Run a JavaScript workflow script that orchestrates subagents at scale. "
    "Use this for work that fans out across many independent pieces - an audit "
    "over many files, a migration, multi-angle research, adversarial "
    "verification of findings - where you write the orchestration as a script "
    "instead of delegating turn by turn. (This implementation dispatches a "
    "homogeneous swarm of sub-agents: split a large request into small, "
    "independent items, provide a prompt template containing {{item}}, and "
    "receive an aggregated XML result.)",
    R"JSON({"type":"object","properties":{"description":{"type":"string","description":"Short description of the whole swarm."},"mode":{"type":"string","enum":["fanout","parallel_sample"],"description":"'fanout': decompose into independent items (default). 'parallel_sample': run the SAME task N times in isolated workspaces, then select and apply the best result (best-of-N)."},"sample_n":{"type":"integer","description":"Number of parallel samples for mode='parallel_sample' (default 4).","minimum":1},"selector":{"type":"string","enum":["self_eval","majority"],"description":"Selection strategy for mode='parallel_sample' (default 'self_eval')."},"subagent_type":{"type":"string","description":"Type of sub-agent. Built-in: 'coder', 'explore', 'plan'. Custom types can be registered in agent configuration."},"prompt_template":{"type":"string","description":"Prompt template that contains the placeholder {{item}}. Mutually exclusive with prompt_prefix."},"prompt_prefix":{"type":"string","description":"Text to prepend to each item. Alternative to prompt_template. Mutually exclusive with prompt_template."},"prompt_suffix":{"type":"string","description":"Text to append after each item. Used with prompt_prefix."},"items":{"type":"array","items":{"type":"string"},"description":"List of items to expand the template with."},"resume_agent_ids":{"type":"object","description":"Optional mapping of existing agent ID to prompt for re-running failed sub-agents."}},"required":["description"]})JSON");

} // namespace kimix::builtin_tools::workflow
