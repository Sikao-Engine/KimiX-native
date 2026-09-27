// agent/hooks_engine.cpp - The tool lifecycle hooks engine (see hooks_engine.h).

#include "agent/hooks_engine.h"

#include <algorithm>

#include <builtin_tools/regex_lite.h>
#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::agent {
namespace hooks {

namespace {

// The mimalloc-backed writer contract (see tool.cpp D3): the buffer produced
// with write_opts + kYYJsonAlcMi is released with mi_free.
bool write_payload(yyjson_mut_doc *mdoc, kimix::string &out) {
    size_t len = 0;
    char *json = yyjson_mut_write_opts(mdoc, 0, &kimix::llm::kYYJsonAlcMi, &len,
                                       nullptr);
    if (json == nullptr) {
        return false;
    }
    out.assign(json, len);
    mi_free(json);
    return true;
}

// events.py _base: {"hook_event_name", "session_id", "cwd"} + the event's own
// fields. `tool_input_json` is embedded as an object ({} when unparseable).
// A deep copy of one JSON value into the mutable document (the payload must
// own its values).
yyjson_mut_val *copy_val(yyjson_val *v, yyjson_mut_doc *mdoc) {
    if (v == nullptr) {
        return yyjson_mut_null(mdoc);
    }
    if (yyjson_is_str(v)) {
        return yyjson_mut_strcpy(
            mdoc,
            kimix::string(yyjson_get_str(v), yyjson_get_len(v)).c_str());
    }
    if (yyjson_is_bool(v)) {
        return yyjson_mut_bool(mdoc, yyjson_get_bool(v));
    }
    if (yyjson_is_int(v)) {
        return yyjson_mut_sint(mdoc, yyjson_get_sint(v));
    }
    if (yyjson_is_uint(v)) {
        return yyjson_mut_uint(mdoc, yyjson_get_uint(v));
    }
    if (yyjson_is_real(v)) {
        return yyjson_mut_real(mdoc, yyjson_get_real(v));
    }
    if (yyjson_is_arr(v)) {
        yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
        yyjson_arr_iter aiter;
        yyjson_arr_iter_init(v, &aiter);
        yyjson_val *item = nullptr;
        while ((item = yyjson_arr_iter_next(&aiter)) != nullptr) {
            yyjson_mut_arr_append(arr, copy_val(item, mdoc));
        }
        return arr;
    }
    if (yyjson_is_obj(v)) {
        yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
        yyjson_obj_iter oiter;
        yyjson_obj_iter_init(v, &oiter);
        yyjson_val *k = nullptr;
        while ((k = yyjson_obj_iter_next(&oiter)) != nullptr) {
            yyjson_mut_obj_add(obj,
                               yyjson_mut_strcpy(
                                   mdoc, kimix::string(yyjson_get_str(k),
                                                       yyjson_get_len(k))
                                            .c_str()),
                               copy_val(yyjson_obj_iter_get_val(k), mdoc));
        }
        return obj;
    }
    return yyjson_mut_null(mdoc);
}

bool build_payload(kimix::string_view event, kimix::string_view session_id,
                   kimix::string_view cwd, kimix::string_view tool_name,
                   kimix::string_view tool_input_json,
                   kimix::string_view extra_key,
                   kimix::string_view extra_value,
                   kimix::string_view tool_call_id, kimix::string &out) {
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "hook_event_name"),
                       yyjson_mut_strcpy(mdoc, kimix::string(event).c_str()));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "session_id"),
                       yyjson_mut_strcpy(mdoc, kimix::string(session_id).c_str()));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "cwd"),
                       yyjson_mut_strcpy(mdoc, kimix::string(cwd).c_str()));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "tool_name"),
                       yyjson_mut_strcpy(mdoc, kimix::string(tool_name).c_str()));
    // tool_input: embedded verbatim as a JSON object.
    yyjson_mut_val *tool_input = yyjson_mut_obj(mdoc);
    if (!tool_input_json.empty()) {
        kimix::string buffer(tool_input_json);
        yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                           YYJSON_READ_STOP_WHEN_DONE,
                                           &kimix::llm::kYYJsonAlcMi, nullptr);
        if (doc != nullptr) {
            yyjson_val *root_in = yyjson_doc_get_root(doc);
            if (root_in != nullptr && yyjson_is_obj(root_in)) {
                tool_input = copy_val(root_in, mdoc);
            }
            yyjson_doc_free(doc);
        }
    }
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "tool_input"), tool_input);
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(mdoc, "tool_call_id"),
                       yyjson_mut_strcpy(mdoc, kimix::string(tool_call_id).c_str()));
    if (!extra_key.empty()) {
        yyjson_mut_obj_add(
            root, yyjson_mut_strcpy(mdoc, kimix::string(extra_key).c_str()),
            yyjson_mut_strcpy(mdoc, kimix::string(extra_value).c_str()));
    }
    const bool ok = write_payload(mdoc, out);
    yyjson_mut_doc_free(mdoc);
    return ok;
}

// engine.py _match_regex: a regex search over the matcher value; an empty
// pattern matches everything; an invalid pattern fails closed (skipped) with
// the reference's warning behaviour.
bool matcher_matches(kimix::string_view pattern, kimix::string_view value) {
    if (pattern.empty()) {
        return true;
    }
    builtin_tools::regex_lite::Regex regex;
    kimix::string error;
    if (!regex.compile(pattern, /*ignore_case=*/false, error)) {
        return false; // invalid regex in hook matcher: skip the hook
    }
    size_t begin = 0;
    size_t end = 0;
    return regex.search(value, begin, end, 0);
}

} // namespace

bool HookEngine::add_hook(kimix::string_view event, kimix::string_view matcher,
                          HookCallback callback) {
    if (event.empty() || !callback) {
        return false;
    }
    HookDef def;
    def.event.assign(event);
    def.matcher.assign(matcher);
    def.callback = std::move(callback);
    _hooks.push_back(std::move(def));
    return true;
}

void HookEngine::add_hooks(kimix::span<const HookDef> defs) {
    for (const HookDef &def : defs) {
        if (def.event.empty() || !def.callback) {
            continue;
        }
        _hooks.push_back(def);
    }
}

kimix::vector<HookResult>
HookEngine::trigger(kimix::string_view event, kimix::string_view matcher_value,
                    kimix::string_view payload_json) const {
    // engine.py trigger (205-244): match by event + regex, run, return the
    // per-hook results. The reference wraps the whole pass in a fail-open
    // try/except; a scripted callback reports its own failure through
    // `error` and the hook is skipped (fail-open) here.
    kimix::vector<HookResult> results;
    for (const HookDef &def : _hooks) {
        if (def.event != event) {
            continue;
        }
        if (!matcher_matches(def.matcher, matcher_value)) {
            continue;
        }
        kimix::string error;
        HookResult result;
        if (def.callback) {
            const kimix::string payload(payload_json); // callbacks take a string
            result = def.callback(payload, error);
        }
        if (!error.empty()) {
            // runner.py run_hook's fail-open contract: a failed hook allows.
            result = HookResult{};
            result.stderr_text = error;
        }
        results.push_back(std::move(result));
    }
    return results;
}

bool HookEngine::has_hooks() const noexcept { return !_hooks.empty(); }

bool HookEngine::has_hooks_for(kimix::string_view event) const noexcept {
    for (const HookDef &def : _hooks) {
        if (def.event == event) {
            return true;
        }
    }
    return false;
}

kimix::vector<std::pair<kimix::string, size_t>> HookEngine::summary() const {
    kimix::vector<std::pair<kimix::string, size_t>> counts;
    for (const HookDef &def : _hooks) {
        bool found = false;
        for (auto &entry : counts) {
            if (entry.first == def.event) {
                ++entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            counts.emplace_back(def.event, static_cast<size_t>(1));
        }
    }
    return counts;
}

kimix::string pre_tool_use_payload(kimix::string_view session_id,
                                   kimix::string_view cwd,
                                   kimix::string_view tool_name,
                                   kimix::string_view tool_input_json,
                                   kimix::string_view tool_call_id) {
    // events.py pre_tool_use (12-25).
    kimix::string out;
    build_payload(kEventPreToolUse, session_id, cwd, tool_name, tool_input_json,
                  {}, {}, tool_call_id, out);
    return out;
}

kimix::string post_tool_use_payload(kimix::string_view session_id,
                                    kimix::string_view cwd,
                                    kimix::string_view tool_name,
                                    kimix::string_view tool_input_json,
                                    kimix::string_view tool_output,
                                    kimix::string_view tool_call_id) {
    // events.py post_tool_use (28-43): tool_output is truncated to 2000
    // characters by the caller (toolset.py str(ret)[:2000]).
    kimix::string out;
    build_payload(kEventPostToolUse, session_id, cwd, tool_name,
                  tool_input_json, "tool_output", tool_output, tool_call_id,
                  out);
    return out;
}

kimix::string
post_tool_use_failure_payload(kimix::string_view session_id,
                              kimix::string_view cwd,
                              kimix::string_view tool_name,
                              kimix::string_view tool_input_json,
                              kimix::string_view error,
                              kimix::string_view tool_call_id) {
    // events.py post_tool_use_failure (46-61).
    kimix::string out;
    build_payload(kEventPostToolUseFailure, session_id, cwd, tool_name,
                  tool_input_json, "error", error, tool_call_id, out);
    return out;
}

} // namespace hooks
} // namespace kimix::agent
