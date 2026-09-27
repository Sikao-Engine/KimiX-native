// agent/tool_name_resolver.cpp - Hallucinated tool-name recovery kernels
// (see tool_name_resolver.h).  The redirect table below is a verbatim,
// generated port of the reference's
// kosong/tooling/__init__.py TOOL_NAME_REDIRECTS (1616-1903) merged with the
// kimi_cli/soul/toolset.py additions + platform shell block
// (_build_platform_redirects, toolset.py:433-648): the toolset's
// `dict.update` semantics are applied (a toolset entry replaces the kosong
// entry for the same key), then the platform block overrides the shell names
// exactly as the reference does on win32 / POSIX.  Self-mapping entries (raw
// or normalized) are dropped, like _TOOL_NAME_REDIRECTS_NORMALIZED
// (tooling/__init__.py:1907-1911).
//
// Unity build: TU-local helpers are static / anonymous-namespace.

#include "agent/tool_name_resolver.h"

#include <algorithm>
#include <utility>

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::agent {

namespace {

// One (hallucinated name, real tool name) redirect. Keys are looked up after
// normalize_tool_name(), so the spelling in the table is irrelevant.
struct name_redirect {
    kimix::string_view from;
    kimix::string_view to;
};

// The merged reference table (kosong TOOL_NAME_REDIRECTS + toolset.py
// update), 302 entries. Generated from the reference sources - do not
// hand-edit.
constexpr kimix::string_view kRedirectPairs[][2] = {
    {"AppendFile", "write"},
    {"CreateFile", "write"},
    {"SaveFile", "write"},
    {"NewFile", "write"},
    {"PutFile", "write"},
    {"TouchFile", "write"},
    {"OverwriteFile", "write"},
    {"UpdateFile", "edit"},
    {"WriteText", "write"},
    {"WriteContent", "write"},
    {"FileWrite", "write"},
    {"OutputFile", "write"},
    {"DumpFile", "write"},
    {"StoreFile", "write"},
    {"PersistFile", "write"},
    {"Create", "write"},
    {"Save", "write"},
    {"ReplaceFile", "edit"},
    {"ModifyFile", "edit"},
    {"PatchFile", "edit"},
    {"ChangeFile", "edit"},
    {"SubFile", "edit"},
    {"SedFile", "edit"},
    {"FileEdit", "edit"},
    {"ReplaceInFile", "edit"},
    {"FindReplace", "edit"},
    {"SearchReplace", "edit"},
    {"TextReplace", "edit"},
    {"StringReplace", "edit"},
    {"ReplaceText", "edit"},
    {"UpdateFileContent", "edit"},
    {"AlterFile", "edit"},
    {"AdjustFile", "edit"},
    {"RewriteFile", "edit"},
    {"ReadFile", "read"},
    {"WriteFile", "write"},
    {"EditFile", "edit"},
    {"ReadMediaFile", "read_image"},
    {"SearchWeb", "web_search"},
    {"Agent", "subagent"},
    {"AskAgent", "send_message"},
    {"AgentList", "list_agents"},
    {"AgentClose", "interrupt_agent"},
    {"TaskOutput", "job_output"},
    {"TodoWrite", "todo_list"},
    {"TodoUpdate", "todo_list"},
    {"TodoTree", "todo_list"},
    {"SubTodo", "todo_list"},
    {"AgentSwarm", "workflow"},
    {"Cat", "read"},
    {"ViewFile", "read"},
    {"ShowFile", "read"},
    {"DisplayFile", "read"},
    {"OpenFile", "read"},
    {"GetFile", "read"},
    {"LoadFile", "read"},
    {"ReadText", "read"},
    {"FileRead", "read"},
    {"PrintFile", "read"},
    {"Dump", "read"},
    {"Head", "read"},
    {"Tail", "read"},
    {"Preview", "read"},
    {"Inspect", "read"},
    {"Peek", "read"},
    {"ReadLines", "read"},
    {"ListFiles", "glob"},
    {"FindFiles", "glob"},
    {"SearchFiles", "glob"},
    {"FileGlob", "glob"},
    {"Ls", "glob"},
    {"Dir", "glob"},
    {"Walk", "glob"},
    {"FileSearch", "glob"},
    {"FileList", "glob"},
    {"ListDir", "glob"},
    {"ListDirectory", "glob"},
    {"DirectoryList", "glob"},
    {"GlobFiles", "glob"},
    {"MatchFiles", "glob"},
    {"Wildcard", "glob"},
    {"Find", "grep"},
    {"RipGrep", "grep"},
    {"Rg", "grep"},
    {"TextSearch", "grep"},
    {"ContentSearch", "grep"},
    {"GrepText", "grep"},
    {"SearchInFiles", "grep"},
    {"FindInFiles", "grep"},
    {"FileGrep", "grep"},
    {"PatternSearch", "grep"},
    {"RegexSearch", "grep"},
    {"MatchPattern", "grep"},
    {"Lookup", "grep"},
    {"TaskList", "todo_list"},
    {"Todo", "todo_list"},
    {"Todos", "todo_list"},
    {"TaskManager", "todo_list"},
    {"TaskPlan", "todo_list"},
    {"Plan", "todo_list"},
    {"Checklist", "todo_list"},
    {"TaskTracker", "todo_list"},
    {"Progress", "todo_list"},
    {"Fetch", "fetch_url"},
    {"HttpGet", "fetch_url"},
    {"GetUrl", "fetch_url"},
    {"Download", "fetch_url"},
    {"WebFetch", "fetch_url"},
    {"FetchPage", "fetch_url"},
    {"OpenUrl", "fetch_url"},
    {"Browse", "fetch_url"},
    {"Visit", "fetch_url"},
    {"ReadUrl", "fetch_url"},
    {"UrlFetch", "fetch_url"},
    {"HttpRequest", "fetch_url"},
    {"Curl", "fetch_url"},
    {"Wget", "fetch_url"},
    {"GoogleSearch", "web_search"},
    {"SearchInternet", "web_search"},
    {"InternetSearch", "web_search"},
    {"SearchOnline", "web_search"},
    {"OnlineSearch", "web_search"},
    {"WebQuery", "web_search"},
    {"QueryWeb", "web_search"},
    {"SearchEngine", "web_search"},
    {"Search", "web_search"},
    {"LookupWeb", "web_search"},
    {"FindOnline", "web_search"},
    {"PruneContext", "context_prune"},
    {"CompactContext", "compact"},
    {"Summarize", "compact"},
    {"SummarizeContext", "compact"},
    {"ContextSummary", "compact"},
    {"ContextTrim", "context_prune"},
    {"TrimContext", "context_prune"},
    {"Recall", "retrieve"},
    {"RetrieveContext", "retrieve"},
    {"SearchHistory", "retrieve"},
    {"HistorySearch", "retrieve"},
    {"SpawnAgent", "subagent"},
    {"StartAgent", "subagent"},
    {"RunAgent", "subagent"},
    {"LaunchAgent", "subagent"},
    {"AgentCreate", "subagent"},
    {"AgentSpawn", "subagent"},
    {"AgentRun", "subagent"},
    {"CreateAgent", "subagent"},
    {"NewAgent", "subagent"},
    {"Agents", "list_agents"},
    {"ActiveAgents", "list_agents"},
    {"AgentStatus", "list_agents"},
    {"CloseAgent", "interrupt_agent"},
    {"StopAgent", "interrupt_agent"},
    {"KillAgent", "interrupt_agent"},
    {"EndAgent", "interrupt_agent"},
    {"TerminateAgent", "interrupt_agent"},
    {"AgentStop", "interrupt_agent"},
    {"AgentKill", "interrupt_agent"},
    {"Swarm", "workflow"},
    {"AgentTeam", "workflow"},
    {"MultiAgent", "workflow"},
    {"ListTasks", "TaskList"},
    {"Tasks", "TaskList"},
    {"TaskStatus", "TaskList"},
    {"GetTaskOutput", "job_output"},
    {"TaskResult", "job_output"},
    {"GetOutput", "job_output"},
    {"ReadOutput", "job_output"},
    {"StopTask", "TaskStop"},
    {"KillTask", "TaskStop"},
    {"CancelTask", "TaskStop"},
    {"AbortTask", "TaskStop"},
    {"TerminateTask", "TaskStop"},
    {"TaskKill", "TaskStop"},
    {"TaskCancel", "TaskStop"},
    {"RunPython", "python"},
    {"ExecutePython", "Python"},
    {"Py", "python"},
    {"PythonExec", "Python"},
    {"PythonRun", "Python"},
    {"PythonCode", "python"},
    {"Eval", "Python"},
    {"ExecPython", "python"},
    {"Run", "Python"},
    {"Execute", "Python"},
    {"Exec", "Python"},
    {"MediaRead", "read_image"},
    {"ReadMedia", "read_image"},
    {"ViewMedia", "read_image"},
    {"AskUser", "AskUserQuestion"},
    {"UserQuestion", "AskUserQuestion"},
    {"Ask", "AskUserQuestion"},
    {"QueryUser", "AskUserQuestion"},
    {"PromptUser", "AskUserQuestion"},
    {"Reason", "Think"},
    {"Reflect", "Think"},
    {"Consider", "Think"},
    {"Analyze", "Think"},
    {"MakeDir", "Mkdir"},
    {"CreateDir", "Mkdir"},
    {"CreateDirectory", "Mkdir"},
    {"Remove", "Rm"},
    {"Delete", "Rm"},
    {"RemoveFile", "Rm"},
    {"DeleteFile", "Rm"},
    {"RmDir", "Rm"},
    {"RemoveDir", "Rm"},
    {"DeleteDir", "Rm"},
    {"Unlink", "Rm"},
    {"Parse", "ParserTool"},
    {"Parser", "ParserTool"},
    {"FindString", "FindStr"},
    {"SearchString", "FindStr"},
    {"AskParentAgent", "AskParent"},
    {"QueryParent", "AskParent"},
    {"ParentQuery", "AskParent"},
    {"HashReadFile", "HashRead"},
    {"ReadHash", "HashRead"},
    {"FileHash", "HashRead"},
    {"HashEditFile", "HashEdit"},
    {"EditHash", "HashEdit"},
    {"Shell", "Bash"},
    {"Terminal", "Bash"},
    {"Cmd", "Bash"},
    {"Command", "Bash"},
    {"Sh", "Bash"},
    {"Zsh", "Bash"},
    {"ShellCommand", "Bash"},
    {"BashCommand", "Bash"},
    {"RunCommand", "Bash"},
    {"Powershell", "Bash"},
    {"PowerShell", "Bash"},
    {"Pwsh", "Bash"},
    {"PS", "Bash"},
    {"TodoChild", "todo_list"},
    {"TodoAdd", "todo_list"},
    {"AddSubTodo", "todo_list"},
    {"TodoDetail", "todo_list"},
    {"TodoEdit", "todo_list"},
    {"SubTask", "todo_list"},
    {"AddTask", "todo_list"},
    {"TaskDetail", "todo_list"},
    {"TaskSub", "todo_list"},
    {"TodoStack", "todo_list"},
    {"TodoHierarchy", "todo_list"},
    {"TodoPlan", "todo_list"},
    {"UpdateTodo", "todo_list"},
    {"SetTodo", "todo_list"},
    {"TodoListSub", "todo_list"},
    {"todo_write", "todo_list"},
    {"todo_update", "todo_list"},
    {"CatFile", "read"},
    {"ReadCode", "read"},
    {"ReadFiles", "read"},
    {"Open", "read"},
    {"View", "read"},
    {"Patch", "edit"},
    {"Modify", "edit"},
    {"FindFile", "glob"},
    {"SearchCode", "grep"},
    {"SearchText", "grep"},
    {"FindText", "grep"},
    {"GrepFiles", "grep"},
    {"CodeSearch", "grep"},
    {"Ripgrep", "grep"},
    {"ViewImage", "read_image"},
    {"ImageRead", "read_image"},
    {"ReadPicture", "read_image"},
    {"ShowImage", "read_image"},
    {"Google", "web_search"},
    {"Bing", "web_search"},
    {"HttpFetch", "fetch_url"},
    {"PageFetch", "fetch_url"},
    {"ExtractWeb", "web_extract"},
    {"UrlExtract", "web_extract"},
    {"ExtractUrl", "web_extract"},
    {"ExtractUrls", "web_extract"},
    {"PageExtract", "web_extract"},
    {"ExtractPage", "web_extract"},
    {"FetchContent", "web_extract"},
    {"ReadUrls", "web_extract"},
    {"ReadURLs", "web_extract"},
    {"WebFetchContent", "web_extract"},
    {"AgentTool", "subagent"},
    {"Delegate", "subagent"},
    {"MessageAgent", "send_message"},
    {"AgentMessage", "send_message"},
    {"AskQuestion", "AskUserQuestion"},
    {"QuestionUser", "AskUserQuestion"},
    {"GetUserInput", "AskUserQuestion"},
    {"ListAgent", "list_agents"},
    {"GetJobOutput", "job_output"},
    {"ReadJobOutput", "job_output"},
    {"BackgroundOutput", "job_output"},
    {"AgentGroup", "workflow"},
    {"RunWorkflow", "workflow"},
    {"MemoryRetrieve", "retrieve"},
    {"RetrieveMemory", "retrieve"},
    {"Remember", "retrieve"},
    {"MemorySearch", "retrieve"},
    {"RunPy", "python"},
    {"PyRun", "python"},
};

// The platform shell block (toolset.py:595-639): on win32 every shell name
// (bash included) redirects to pwsh; on POSIX the PowerShell/generic-shell
// names redirect to bash. Applied AFTER the table above, like the reference's
// `_redirects.update({...})` inside _build_platform_redirects.
constexpr kimix::string_view kPlatformPairs[][2] = {
#ifdef KIMIX_PLATFORM_WINDOWS
    {"bash", "pwsh"},
    {"Shell", "pwsh"},
    {"Terminal", "pwsh"},
    {"Cmd", "pwsh"},
    {"Command", "pwsh"},
    {"Run", "pwsh"},
    {"Execute", "pwsh"},
    {"Exec", "pwsh"},
    {"RunCommand", "pwsh"},
    {"RunShell", "pwsh"},
    {"ShellRun", "pwsh"},
    {"Sh", "pwsh"},
    {"Zsh", "pwsh"},
    {"ShellCommand", "pwsh"},
    {"BashCommand", "pwsh"},
    {"Powershell", "pwsh"},
    {"PowerShell", "pwsh"},
    {"Pwsh", "pwsh"},
    {"PS", "pwsh"},
#else
    {"Powershell", "bash"},
    {"PowerShell", "bash"},
    {"Pwsh", "bash"},
    {"PS", "bash"},
    {"Shell", "bash"},
    {"Terminal", "bash"},
    {"Cmd", "bash"},
    {"Command", "bash"},
    {"Run", "bash"},
    {"Execute", "bash"},
    {"Exec", "bash"},
    {"RunCommand", "bash"},
    {"RunShell", "bash"},
    {"ShellRun", "bash"},
    {"Sh", "bash"},
    {"Zsh", "bash"},
    {"ShellCommand", "bash"},
    {"BashCommand", "bash"},
#endif
};

// The process-wide pre-normalized redirect map (the reference's
// _PLATFORM_REDIRECTS_NORM / _TOOL_NAME_REDIRECTS_NORMALIZED, built once).
const kimix::unordered_map<kimix::string, kimix::string, kimix::string_hash> &
platform_redirects_norm() {
    static const kimix::unordered_map<kimix::string, kimix::string,
                                      kimix::string_hash>
        map = [] {
            kimix::unordered_map<kimix::string, kimix::string,
                                 kimix::string_hash>
                out;
            const auto put = [&out](kimix::string_view from,
                                    kimix::string_view to) {
                out.insert_or_assign(normalize_tool_name(from),
                                     kimix::string(to));
            };
            for (const auto &pair : kRedirectPairs) {
                if (normalize_tool_name(pair[0]) ==
                    normalize_tool_name(pair[1])) {
                    continue; // self-mapping: handled by exact match
                }
                put(pair[0], pair[1]);
            }
            for (const auto &pair : kPlatformPairs) {
                if (normalize_tool_name(pair[0]) ==
                    normalize_tool_name(pair[1])) {
                    continue;
                }
                put(pair[0], pair[1]); // platform override wins
            }
            return out;
        }();
    return map;
}

const kimix::string *redirect_of(kimix::string_view name) {
    const auto &redirects = platform_redirects_norm();
    const auto it = redirects.find(normalize_tool_name(name));
    return (it != redirects.end()) ? &it->second : nullptr;
}

bool name_in(const kimix::vector<kimix::string> &names,
             kimix::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Fuzzy-key-match thresholds (_fuzzy_match_keys, tooling/__init__.py:1313-):
// shorter keys demand a stronger match.
inline double key_cutoff_for(size_t length) noexcept {
    return length >= 8 ? 0.75 : 0.80;
}

} // namespace

kimix::string normalize_tool_name(kimix::string_view name) {
    kimix::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (c == '-' || c == '_') {
            continue;
        }
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                           : c);
    }
    return out;
}

double sequence_ratio(kimix::string_view a, kimix::string_view b) {
    // Normalized InDel similarity (the documented mapping to the reference's
    // rapidfuzz score): 1 - (len(a) + len(b) - 2*LCS) / (len(a) + len(b)),
    // i.e. fuzz.ratio(a, b) / 100.
    if (a.empty() && b.empty()) {
        return 1.0;
    }
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    kimix::vector<size_t> prev(b.size() + 1, 0);
    kimix::vector<size_t> curr(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); ++i) {
        curr[0] = 0;
        for (size_t j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                curr[j] = prev[j - 1] + 1;
            } else {
                curr[j] = prev[j] > curr[j - 1] ? prev[j] : curr[j - 1];
            }
        }
        std::swap(prev, curr);
    }
    const double total = static_cast<double>(a.size() + b.size());
    return (2.0 * static_cast<double>(prev[b.size()])) / total;
}

kimix::vector<kimix::string> fuzzy_match_tool_name(
    kimix::string_view tool_name, const kimix::vector<kimix::string> &valid_names,
    int n, double cutoff, int min_length) {
    kimix::vector<kimix::string> out;
    if (static_cast<int>(tool_name.size()) < min_length || n <= 0) {
        return out;
    }
    const kimix::string norm = normalize_tool_name(tool_name);

    // A scored candidate: normalized-form ratio is the primary key, the raw
    // (lowercase) ratio the tie breaker, then the name itself (deterministic,
    // like the reference sort keys).
    struct scored_match {
        double ratio;
        kimix::string name;
    };
    kimix::vector<scored_match> exact;
    kimix::vector<scored_match> scored;
    for (const kimix::string &candidate : valid_names) {
        const kimix::string cn = normalize_tool_name(candidate);
        if (cn == norm) {
            exact.push_back({1.0, candidate});
            continue;
        }
        const double ratio = sequence_ratio(norm, cn);
        if (ratio >= cutoff) {
            scored.push_back({ratio, candidate});
        }
    }
    const kimix::vector<scored_match> &pool = !exact.empty() ? exact : scored;
    kimix::vector<scored_match> sorted(pool.begin(), pool.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const scored_match &a, const scored_match &b) {
                  if (a.ratio != b.ratio) {
                      return a.ratio > b.ratio;
                  }
                  return a.name < b.name;
              });
    const size_t take = static_cast<size_t>(n) < sorted.size()
                            ? static_cast<size_t>(n)
                            : sorted.size();
    for (size_t i = 0; i < take; ++i) {
        out.push_back(sorted[i].name);
    }
    return out;
}

ToolNameResolution
resolve_tool_name(kimix::string_view name,
                  const kimix::vector<kimix::string> &valid_names,
                  double auto_correct_cutoff, double suggest_cutoff,
                  int n_suggestions, int min_length, bool use_redirects) {
    ToolNameResolution out;
    out.original = kimix::string(name);
    // 1. Exact match -> unchanged.
    if (name_in(valid_names, name)) {
        out.name = kimix::string(name);
        return out;
    }
    // 2. Redirect map (pre-normalized keys) before fuzzy matching.
    if (use_redirects) {
        const kimix::string *redirected = redirect_of(name);
        if (redirected != nullptr && name_in(valid_names, *redirected)) {
            out.name = *redirected;
            out.corrected = true;
            return out;
        }
    }
    // 3./4. One ranked scan at the suggestion cutoff; the auto-correct
    // decision derives from the top candidate (the reference's fast path,
    // tooling/__init__.py:1570-1589; behaviour-identical to the two-call form
    // because the fuzzy ranking is monotonic in the cutoff).
    const kimix::vector<kimix::string> matches =
        fuzzy_match_tool_name(name, valid_names, n_suggestions, suggest_cutoff,
                              min_length);
    if (matches.empty()) {
        return out; // name empty, no suggestions
    }
    const kimix::string &top = matches.front();
    if (normalize_tool_name(top) == normalize_tool_name(name) ||
        sequence_ratio(normalize_tool_name(name), normalize_tool_name(top)) >=
            auto_correct_cutoff) {
        out.name = top;
        out.corrected = true;
        return out;
    }
    out.suggestions = matches;
    return out;
}

kimix::vector<kimix::string>
collect_candidates(kimix::string_view tool_name,
                   const kimix::vector<kimix::string> &valid_names,
                   bool use_redirects) {
    kimix::vector<kimix::string> result;
    const auto push = [&](const kimix::string &name) {
        if (!name_in(result, name)) {
            result.push_back(name);
        }
    };
    const kimix::string norm_name = normalize_tool_name(tool_name);
    // 1. Redirect map (highest confidence - human-curated).
    if (use_redirects) {
        const kimix::string *redirected = redirect_of(tool_name);
        if (redirected != nullptr && name_in(valid_names, *redirected)) {
            push(*redirected);
        }
    }
    // 2. Normalized exact match against the valid names.
    for (const kimix::string &candidate : valid_names) {
        if (normalize_tool_name(candidate) == norm_name) {
            push(candidate);
        }
    }
    // 3. Fuzzy matches (n=5, cutoff=0.5, like _collect_candidates).
    for (const kimix::string &candidate : fuzzy_match_tool_name(
             tool_name, valid_names, 5, kSuggestCutoff, kMinNameLength)) {
        push(candidate);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Argument-fit disambiguation (_score_argument_fit / resolve_tool_by_arguments)
// ---------------------------------------------------------------------------

const kimix::string *tool_param_schema::type_of(kimix::string_view field) const {
    for (const auto &entry : fields) {
        if (entry.first == field) {
            return &entry.second;
        }
    }
    return nullptr;
}

const kimix::string *tool_param_schema::union_of(kimix::string_view field) const {
    for (const auto &entry : unions) {
        if (entry.first == field) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool tool_param_schema::is_required(kimix::string_view field) const {
    return std::find(required.begin(), required.end(), field) != required.end();
}

bool tool_param_schema::is_plain_string(kimix::string_view field) const {
    // The reference's _is_plain_string_annotation: `str` or Optional[str].
    const kimix::string *type = type_of(field);
    if (type != nullptr && *type == "string") {
        return true;
    }
    const kimix::string *union_kind = union_of(field);
    return union_kind != nullptr && *union_kind == "string";
}

namespace {

bool type_is_string_or_null(kimix::string_view type) {
    return type == "string" || type == "null";
}

// The schema type declared for one property; a union of string|null is
// reported through `union_kind` (the Optional[str] analogue).
void property_type(yyjson_val *value, kimix::string &type,
                   kimix::string &union_kind) {
    yyjson_val *type_val = yyjson_obj_get(value, "type");
    if (yyjson_is_str(type_val)) {
        type.assign(yyjson_get_str(type_val),
                    static_cast<size_t>(yyjson_get_len(type_val)));
        return;
    }
    if (yyjson_is_arr(type_val)) {
        union_kind = "string"; // "type": ["string", "null"]
        yyjson_arr_iter titer;
        yyjson_arr_iter_init(type_val, &titer);
        yyjson_val *t = nullptr;
        while ((t = yyjson_arr_iter_next(&titer)) != nullptr) {
            if (!yyjson_is_str(t) ||
                !type_is_string_or_null(kimix::string_view(
                    yyjson_get_str(t), static_cast<size_t>(yyjson_get_len(t))))) {
                union_kind.clear();
                return;
            }
        }
        return;
    }
    for (const char *comb : {"anyOf", "oneOf"}) {
        yyjson_val *arr = yyjson_obj_get(value, comb);
        if (!yyjson_is_arr(arr)) {
            continue;
        }
        bool all_string = true;
        bool any = false;
        yyjson_arr_iter uiter;
        yyjson_arr_iter_init(arr, &uiter);
        yyjson_val *u = nullptr;
        while ((u = yyjson_arr_iter_next(&uiter)) != nullptr) {
            yyjson_val *ut = yyjson_obj_get(u, "type");
            if (!yyjson_is_str(ut)) {
                all_string = false;
                break;
            }
            any = true;
            if (!type_is_string_or_null(kimix::string_view(
                    yyjson_get_str(ut), static_cast<size_t>(yyjson_get_len(ut))))) {
                all_string = false;
                break;
            }
        }
        if (any && all_string) {
            union_kind = "string";
            return;
        }
    }
}

// One argument key mapped onto a schema field (the reference's
// _repair_dict_for_model key-mapping passes): exact, or a folded/fuzzy match
// (cutoff 0.80 below 8 characters / 0.75 from 8; min length 4 on both sides;
// first-character prefilter; strongest match wins). An empty field means
// "unmapped".
struct key_match {
    kimix::string field;
    bool exact = false;
};

key_match match_argument_key(const kimix::string &key,
                             const tool_param_schema &schema) {
    key_match out;
    if (schema.type_of(key) != nullptr) {
        out.field = key;
        out.exact = true;
        return out;
    }
    if (key.size() < 4) {
        return out;
    }
    const kimix::string folded_key = normalize_tool_name(key);
    if (folded_key.empty()) {
        return out;
    }
    const double cutoff = key_cutoff_for(key.size());
    double best = cutoff - 1e-9;
    const kimix::string *best_field = nullptr;
    for (const auto &field : schema.fields) {
        if (field.first.size() < 4) {
            continue;
        }
        const kimix::string folded_field = normalize_tool_name(field.first);
        if (folded_field.empty() || folded_field[0] != folded_key[0]) {
            continue; // first-character prefilter (_fuzzy_match_keys)
        }
        const double ratio = sequence_ratio(folded_key, folded_field);
        if (ratio > best) {
            best = ratio;
            best_field = &field.first;
        }
    }
    if (best_field != nullptr) {
        out.field = *best_field;
    }
    return out;
}

} // namespace

bool parse_tool_param_schema(kimix::string_view parameters_json,
                             tool_param_schema &out) {
    out = tool_param_schema{};
    if (parameters_json.empty()) {
        return false;
    }
    kimix::string buffer(parameters_json);
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                       YYJSON_READ_STOP_WHEN_DONE,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *properties = yyjson_obj_get(root, "properties");
    if (yyjson_is_obj(properties)) {
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(properties, &iter);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            yyjson_val *value = yyjson_obj_iter_get_val(key);
            const kimix::string field(yyjson_get_str(key),
                                      static_cast<size_t>(yyjson_get_len(key)));
            kimix::string type;
            kimix::string union_kind;
            property_type(value, type, union_kind);
            out.fields.emplace_back(field, type);
            if (!union_kind.empty()) {
                out.unions.emplace_back(field, union_kind);
            }
        }
    }
    yyjson_val *required = yyjson_obj_get(root, "required");
    if (yyjson_is_arr(required)) {
        yyjson_arr_iter riter;
        yyjson_arr_iter_init(required, &riter);
        yyjson_val *r = nullptr;
        while ((r = yyjson_arr_iter_next(&riter)) != nullptr) {
            if (yyjson_is_str(r)) {
                out.required.emplace_back(yyjson_get_str(r),
                                          static_cast<size_t>(yyjson_get_len(r)));
            }
        }
    }
    yyjson_doc_free(doc);
    return !out.fields.empty();
}

double score_argument_fit(kimix::string_view arguments_json,
                          const tool_param_schema &schema) {
    // Port of kosong/tooling/__init__.py _score_argument_fit (1914-2019) over
    // the reduced JSON-schema table (the pydantic model_fields analogue):
    //   +0.3/n_required or +0.1/n_optional per mapped field, +0.1 exact-name
    //   bonus / +0.05 alias bonus, -0.15 per unmapped key, +0.02 per type
    //   coercion, clamped to [0, 1].
    if (schema.empty() || arguments_json.empty()) {
        return 0.0;
    }
    kimix::string buffer(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                       YYJSON_READ_STOP_WHEN_DONE,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return 0.0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0.0;
    }

    size_t n_required = 0;
    for (const auto &field : schema.fields) {
        if (schema.is_required(field.first)) {
            ++n_required;
        }
    }
    const size_t n_optional = schema.fields.size() - n_required;
    if (n_required == 0) {
        n_required = 1; // avoid division by zero; optional-only model
    }

    double score = 0.0;
    size_t mapped_keys = 0;
    size_t unmapped_keys = 0;
    size_t coercions = 0;

    yyjson_val *arg = nullptr;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    while ((arg = yyjson_obj_iter_next(&iter)) != nullptr) {
        yyjson_val *value = yyjson_obj_iter_get_val(arg);
        if (!yyjson_is_str(arg)) {
            continue;
        }
        const kimix::string key(yyjson_get_str(arg),
                                static_cast<size_t>(yyjson_get_len(arg)));
        const key_match match = match_argument_key(key, schema);
        if (match.field.empty()) {
            ++unmapped_keys; // a field this tool does not have
            continue;
        }
        ++mapped_keys;
        if (schema.is_required(match.field)) {
            score += 0.3 / static_cast<double>(n_required);
        } else {
            score +=
                0.1 / static_cast<double>(n_optional > 0 ? n_optional : 1);
        }
        score += match.exact ? 0.1 : 0.05; // exact / alias match bonus
        // Type coercion signal (weak, +0.02): a string value sitting on a
        // non-string field is what the repair pass rewrites.
        if (!match.exact && !schema.is_plain_string(match.field) &&
            yyjson_is_str(value)) {
            ++coercions;
        }
    }
    yyjson_doc_free(doc);

    if (mapped_keys == 0) {
        return 0.0; // nothing matched: the reference returns (0.0, None)
    }
    score -= 0.15 * static_cast<double>(unmapped_keys);
    score += 0.02 * static_cast<double>(coercions);
    if (score < 0.0) {
        score = 0.0;
    }
    if (score > 1.0) {
        score = 1.0;
    }
    return score;
}

ToolNameResolution resolve_tool_by_arguments(
    kimix::string_view tool_name, kimix::string_view arguments_json,
    const kimix::vector<kimix::string> &candidates,
    const kimix::function<const tool_param_schema *(kimix::string_view)> &schema_of) {
    ToolNameResolution out;
    out.original = kimix::string(tool_name);
    struct fit {
        kimix::string name;
        double score = 0.0;
    };
    kimix::vector<fit> scored;
    for (const kimix::string &candidate : candidates) {
        if (!schema_of) {
            continue;
        }
        const tool_param_schema *schema = schema_of(candidate);
        if (schema == nullptr || schema->empty()) {
            continue; // no typed params: skipped, like the reference
        }
        const double score = score_argument_fit(arguments_json, *schema);
        scored.push_back({candidate, score});
    }
    if (scored.empty()) {
        return out;
    }
    std::sort(scored.begin(), scored.end(),
              [](const fit &a, const fit &b) { return a.score > b.score; });
    const fit &best = scored.front();
    const bool beats_second =
        scored.size() < 2 || (best.score - scored[1].score) >= kScoreDelta;
    if (best.score >= kMinArgumentScore && beats_second) {
        out.name = best.name;
        out.corrected = true;
    }
    return out;
}

} // namespace kimix::agent
