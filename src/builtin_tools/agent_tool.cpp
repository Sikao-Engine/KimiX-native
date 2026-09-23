// agent_tool.cpp - C++ port of the kimi-agent sub-agent tools (see
// agent_tool.h for the reference line map).
//
// Unity-build rules: every TU-local helper lives in an anonymous namespace
// inside kimix::builtin_tools::agents and carries the `ag_` prefix.
#include "builtin_tools/agent_tool.h"

#include <chrono>
#include <cstdio>

#include <core/clock.h>

#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::agents {

namespace {

// ---------------------------------------------------------------------------
// Small utilities (ag_ prefix - unity build safety)
// ---------------------------------------------------------------------------

const char *ag_status_string(tool_status status) noexcept {
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

double ag_wall_clock() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// str(uuid.uuid4()) - 8-4-4-4-12 lowercase hex with the version/variant bits
// of a v4 uuid. Seeded from the monotonic clock, the address of a local and a
// process-wide counter: good enough for session ids, which only have to be
// unique inside one process tree.
kimix::string ag_new_session_id() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t seq = counter.fetch_add(1) + 1;
    const uint64_t tick = static_cast<uint64_t>(kimix::Clock::now_ms());
    const uint64_t addr =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&counter));
    uint8_t bytes[16];
    uint64_t a = tick ^ (seq * 0x9E3779B97F4A7C15ull);
    uint64_t b = addr ^ (seq * 0xC2B2AE3D27D4EB4Full) ^ (tick << 7);
    // A tiny xorshift-multiply mix so neighbouring ids differ in every group.
    auto mix = [](uint64_t &x) {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
    };
    mix(a);
    mix(b);
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((a >> (8 * i)) & 0xFF);
        bytes[8 + i] = static_cast<uint8_t>((b >> (8 * i)) & 0xFF);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80); // variant
    static const char *kHex = "0123456789abcdef";
    kimix::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

// Minimal JSON string escaping (orjson escapes exactly these).
void ag_json_escape(kimix::string_view text, kimix::string &out) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out.append(buf);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

// orjson float rendering: shortest round-trip decimal, which std::format's
// default float presentation also produces.
kimix::string ag_json_number(double value) {
    return kimix::format("{}", value);
}

void ag_indent(kimix::string &out, int32_t depth) {
    for (int32_t i = 0; i < depth; ++i) {
        out.append("  ");
    }
}

// orjson.dumps(value, option=OPT_INDENT_2) for one ValueElement.
void ag_pretty_value(const ValueElement &v, int32_t depth, kimix::string &out);

void ag_pretty_object(const ToolParams &obj, int32_t depth, kimix::string &out) {
    if (obj.values.empty()) {
        out.append("{}");
        return;
    }
    out.append("{\n");
    // Deterministic key order: sorted, so the rendering is reproducible.
    kimix::vector<kimix::string> keys;
    keys.reserve(obj.values.size());
    for (const auto &kv : obj.values) {
        keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < keys.size(); ++i) {
        ag_indent(out, depth + 1);
        ag_json_escape(keys[i], out);
        out.append(": ");
        const ValueElement *child = obj.get(keys[i]);
        if (child != nullptr) {
            ag_pretty_value(*child, depth + 1, out);
        } else {
            out.append("null");
        }
        if (i + 1 < keys.size()) {
            out.push_back(',');
        }
        out.push_back('\n');
    }
    ag_indent(out, depth);
    out.push_back('}');
}

void ag_pretty_value(const ValueElement &v, int32_t depth, kimix::string &out) {
    if (v.is_null()) {
        out.append("null");
        return;
    }
    if (v.is_bool()) {
        out.append(v.as_bool() ? "true" : "false");
        return;
    }
    if (v.is_int()) {
        out += kimix::format("{}", v.as_int());
        return;
    }
    if (v.is_uint()) {
        out += kimix::format("{}", v.as_uint());
        return;
    }
    if (v.is_real()) {
        out += ag_json_number(v.as_real());
        return;
    }
    if (v.is_string()) {
        ag_json_escape(v.as_string(), out);
        return;
    }
    if (v.is_array()) {
        const ValueElement::Array &arr = v.as_array();
        if (arr.empty()) {
            out.append("[]");
            return;
        }
        out.append("[\n");
        for (size_t i = 0; i < arr.size(); ++i) {
            ag_indent(out, depth + 1);
            ag_pretty_value(arr[i], depth + 1, out);
            if (i + 1 < arr.size()) {
                out.push_back(',');
            }
            out.push_back('\n');
        }
        ag_indent(out, depth);
        out.push_back(']');
        return;
    }
    const ToolParams *obj = v.as_object();
    if (obj == nullptr) {
        out.append("null");
        return;
    }
    ag_pretty_object(*obj, depth, out);
}

// Default file readers/writers used when the host does not inject any.
bool ag_default_read_file(kimix::string_view path, kimix::string &out) {
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

// Save `text` under .kimix_cache/tmp_<pid>/ (common.py _create_script_file).
kimix::string ag_default_save_prompt(kimix::string_view text,
                                     kimix::string_view ext) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(".kimix_cache") /
                         kimix::format("tmp_{}", static_cast<int64_t>(
                                                     ag_wall_clock() * 1000.0));
    fs::create_directories(dir, ec);
    static std::atomic<uint64_t> index{0};
    const uint64_t n = index.fetch_add(1);
    kimix::string name = kimix::format("{}.{}", n, ext);
    const fs::path full = dir / fs::path(name);
    std::FILE *f = std::fopen(kimix::to_string(full).c_str(), "wb");
    if (f == nullptr) {
        return {};
    }
    if (!text.empty()) {
        std::fwrite(text.data(), 1, text.size(), f);
    }
    std::fclose(f);
    kimix::string out = kimix::to_string(full);
    for (char &c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

// Portable absolute-path test. std::filesystem::path::is_absolute() answers
// false for "/abs/path" on Windows (no root name), which would send an
// absolute @path reference through the base_dir join - so the shape is tested
// directly instead (POSIX leading separator, or a Windows drive prefix).
bool ag_is_absolute(kimix::string_view path) {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return true;
    }
    if (path.size() >= 2 && path[1] == ':') {
        const char c = path[0];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    return false;
}

// Join a base directory and a relative path (Python `base / path`). Forward
// slashes are used on every platform: Win32 accepts them, and it keeps the
// result byte-stable for the injected-reader tests.
kimix::string ag_join(kimix::string_view base, kimix::string_view rel) {
    if (base.empty()) {
        return kimix::string(rel);
    }
    kimix::string out(base);
    const char last = out.back();
    if (last != '/' && last != '\\') {
        out += '/';
    }
    out.append(rel.data(), rel.size());
    return out;
}

void ag_error(ToolParams &result, tool_status status, kimix::string_view message,
              kimix::string_view output, kimix::string_view brief) {
    result.values["ok"] = ValueElement::make_bool(false);
    result.values["status"] =
        ValueElement::make_string(kimix::string(ag_status_string(status)));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

void ag_ok(ToolParams &result, kimix::string_view message,
           kimix::string_view output, kimix::string_view brief) {
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(kimix::string(message));
    result.values["output"] = ValueElement::make_string(kimix::string(output));
    result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

// Agent._build_extras (701-724).
void ag_build_extras(ToolParams &result, kimix::string_view session_id,
                     kimix::string_view status, size_t turn_count,
                     kimix::string_view question, bool has_question,
                     bool return_history, kimix::string_view history_format,
                     kimix::span<const conversation_turn> turns) {
    kimix::shared_ptr<ToolParams> extras(new ToolParams());
    extras->values["session_id"] =
        ValueElement::make_string(kimix::string(session_id));
    extras->values["status"] = ValueElement::make_string(kimix::string(status));
    extras->values["turn_count"] =
        ValueElement::make_int(static_cast<int64_t>(turn_count));
    if (has_question) {
        extras->values["question"] =
            ValueElement::make_string(kimix::string(question));
    }
    if (return_history) {
        if (history_format == "json") {
            ValueElement::Array arr;
            arr.reserve(turns.size());
            for (const conversation_turn &t : turns) {
                arr.push_back(turn_to_value(t));
            }
            extras->values["conversation_history"] =
                ValueElement::make_array(std::move(arr));
        } else if (history_format == "markdown") {
            extras->values["conversation_history"] =
                ValueElement::make_string(format_history_markdown(turns));
        } else if (history_format == "summary") {
            extras->values["conversation_history"] =
                ValueElement::make_string(format_history_summary(turns));
        } else {
            extras->values["conversation_history"] =
                ValueElement::make_array(ValueElement::Array{});
        }
    }
    result.values["extras"] = ValueElement::make_object(std::move(extras));
}

// Resolve the effective work dir for prompt/@path resolution.
kimix::string ag_work_dir(const kimix::builtin_tools::Session *session) {
    if (session != nullptr && !session->work_dir.empty()) {
        return session->work_dir;
    }
    return ".";
}

} // namespace

// ---------------------------------------------------------------------------
// agent_registry
// ---------------------------------------------------------------------------

agent_registry::~agent_registry() {
    kimix::vector<slot *> owned;
    {
        std::lock_guard<kimix::spin_mutex> g(_mutex);
        owned.reserve(_slots.size());
        for (auto &kv : _slots) {
            owned.push_back(&kv.second);
        }
    }
    // Join outside the lock: a worker may still be inside runner().
    for (slot *s : owned) {
        if (s->run != nullptr) {
            s->run->cancel.store(true);
            if (s->run->worker.joinable()) {
                s->run->worker.join();
            }
        }
    }
}

agent_registry::slot *
agent_registry::find_locked(kimix::string_view session_id) {
    const auto it = _slots.find(kimix::string(session_id));
    return (it == _slots.end()) ? nullptr : const_cast<slot *>(&it->second);
}

const agent_registry::slot *
agent_registry::find_locked(kimix::string_view session_id) const {
    const auto it = _slots.find(kimix::string(session_id));
    return (it == _slots.end()) ? nullptr : &it->second;
}

double agent_registry::clock_now() const {
    return now_seconds();
}

double agent_registry::now_seconds() const {
    if (now) {
        return now();
    }
    return ag_wall_clock();
}

const agent_entry *agent_registry::get(kimix::string_view session_id) const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const slot *s = find_locked(session_id);
    return (s == nullptr) ? nullptr : s->entry.get();
}

agent_entry *agent_registry::get(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    slot *s = find_locked(session_id);
    return (s == nullptr) ? nullptr : s->entry.get();
}

void agent_registry::put(agent_entry entry) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const kimix::string id = entry.session_id;
    slot *existing = find_locked(id);
    if (existing != nullptr) {
        existing->entry = kimix::unique_ptr<agent_entry>(
            new agent_entry(std::move(entry)));
        return;
    }
    slot fresh;
    fresh.entry =
        kimix::unique_ptr<agent_entry>(new agent_entry(std::move(entry)));
    _slots.emplace(id, std::move(fresh));
    _order.push_back(id);
}

bool agent_registry::close(kimix::string_view session_id) {
    const kimix::string id(session_id);
    std::thread worker;
    {
        std::lock_guard<kimix::spin_mutex> g(_mutex);
        slot *s = find_locked(id);
        if (s == nullptr) {
            return false;
        }
        if (s->run != nullptr) {
            s->run->cancel.store(true);
            if (s->run->worker.joinable()) {
                // Move the handle out ONLY: the agent_run object must stay in
                // the slot while the worker lives, because the worker
                // dereferences it (req.cancel, finished) until it exits.
                worker = std::move(s->run->worker);
            }
        }
    }
    // Join AFTER releasing the lock. A real runner polls the steer queue
    // (drain_steer) between steps, so joining while holding the lock
    // deadlocks the worker against this thread (found by new_tools_e2e:
    // interrupt_agent froze forever the moment close() joined under _mutex).
    if (worker.joinable()) {
        worker.join();
    }
    {
        std::lock_guard<kimix::spin_mutex> g(_mutex);
        slot *s = find_locked(id);
        if (s == nullptr) {
            return true; // evicted while we were joining
        }
        if (s->run != nullptr) {
            // Park the settled result so join_run()/run_finished() can still
            // report the outcome (e.g. the cancelled flag) after the session
            // bookkeeping is dropped. Cleared by join_run/clear_run.
            _finished[id] = std::move(s->run->result);
        }
        _slots.erase(id);
        for (size_t i = 0; i < _order.size(); ++i) {
            if (_order[i] == id) {
                _order.erase(_order.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        _live_sessions.erase(id);
    }
    return true;
}

kimix::vector<agent_list_item> agent_registry::list_active() const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    kimix::vector<agent_list_item> out;
    out.reserve(_order.size());
    for (const kimix::string &id : _order) {
        const slot *s = find_locked(id);
        if (s == nullptr || s->entry == nullptr || !s->entry->is_active) {
            continue;
        }
        const agent_entry &e = *s->entry;
        agent_list_item item;
        item.session_id = e.session_id;
        item.created_at = e.created_at;
        item.last_accessed = e.last_accessed;
        item.total_turns = e.total_turns;
        item.state = e.state;
        item.is_active = e.is_active;
        out.push_back(std::move(item));
    }
    return out;
}

size_t agent_registry::size() const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    return _slots.size();
}

kimix::vector<kimix::string> agent_registry::evict_lru_if_needed() {
    kimix::vector<kimix::string> evicted;
    while (true) {
        kimix::string lru_id;
        {
            std::lock_guard<kimix::spin_mutex> g(_mutex);
            if (static_cast<int32_t>(_slots.size()) < k_max_sessions) {
                break;
            }
            double oldest = 0.0;
            for (const auto &kv : _slots) {
                if (kv.second.entry == nullptr) {
                    continue;
                }
                const double accessed = kv.second.entry->last_accessed;
                if (lru_id.empty() || accessed < oldest) {
                    oldest = accessed;
                    lru_id = kv.first;
                }
            }
            if (lru_id.empty()) {
                break;
            }
            slot *s = find_locked(lru_id);
            if (s != nullptr && s->entry != nullptr) {
                s->entry->is_active = false;
            }
        }
        // close() re-locks; call it outside the scope above.
        close(lru_id);
        evicted.push_back(lru_id);
    }
    return evicted;
}

void agent_registry::register_session(kimix::string_view session_id) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    _live_sessions.emplace(kimix::string(session_id));
}

void agent_registry::unregister_session(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    _live_sessions.erase(kimix::string(session_id));
}

bool agent_registry::has_session(kimix::string_view session_id) const {
    if (session_id.empty()) {
        return false;
    }
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    return _live_sessions.find(kimix::string(session_id)) != _live_sessions.end();
}

void agent_registry::queue_pending_message(kimix::string_view target_id,
                                           kimix::string_view message) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    kimix::vector<kimix::string> &pending =
        _pending[kimix::string(target_id)];
    if (pending.size() >= k_max_pending_messages) {
        return;
    }
    pending.emplace_back(message);
}

kimix::vector<kimix::string>
agent_registry::drain_pending_messages(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const auto it = _pending.find(kimix::string(session_id));
    if (it == _pending.end()) {
        return {};
    }
    kimix::vector<kimix::string> out = std::move(it->second);
    _pending.erase(it);
    return out;
}

size_t agent_registry::pending_message_count(
    kimix::string_view session_id) const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const auto it = _pending.find(kimix::string(session_id));
    return (it == _pending.end()) ? 0 : it->second.size();
}

bool agent_registry::start_background(kimix::string_view session_id,
                                      const subagent_request &request) {
    if (!runner) {
        return false;
    }
    const kimix::string id(session_id);
    agent_run *run = nullptr;
    {
        std::lock_guard<kimix::spin_mutex> g(_mutex);
        slot *s = find_locked(id);
        if (s == nullptr) {
            return false;
        }
        s->run = kimix::unique_ptr<agent_run>(new agent_run());
        s->run->prompt = request.prompt;
        s->run->started_at = now_seconds();
        run = s->run.get();
    }
    subagent_request req = request;
    req.session_id = id;
    req.background = true;
    req.cancel = &run->cancel;
    subagent_runner active = runner;
    run->worker = std::thread([this, id, req, active, run]() {
        // No exceptions (kimix_enable_exception=false): the sub-agent runner
        // must report failures through subagent_run_result::ok / ::error
        // (subagent_runner is a no-throw callable now); the former
        // try/catch -> outcome.ok = false boundary is gone.
        subagent_run_result outcome = active(req);
        {
            std::lock_guard<kimix::spin_mutex> g(_mutex);
            slot *s = find_locked(id);
            if (s != nullptr && s->run.get() == run) {
                s->run->result = std::move(outcome);
            }
        }
        run->finished.store(true);
    });
    return true;
}

bool agent_registry::is_running(kimix::string_view session_id) const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const slot *s = find_locked(session_id);
    if (s == nullptr || s->run == nullptr) {
        return false;
    }
    return !s->run->finished.load();
}

bool agent_registry::push_steer(kimix::string_view session_id,
                                kimix::string_view message) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    slot *s = find_locked(session_id);
    if (s == nullptr || s->run == nullptr || s->run->finished.load()) {
        return false;
    }
    s->steer.emplace_back(message);
    return true;
}

kimix::vector<kimix::string>
agent_registry::drain_steer(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    slot *s = find_locked(session_id);
    if (s == nullptr) {
        return {};
    }
    kimix::vector<kimix::string> out = std::move(s->steer);
    s->steer.clear();
    return out;
}

bool agent_registry::request_cancel(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    slot *s = find_locked(session_id);
    if (s == nullptr || s->run == nullptr) {
        return false;
    }
    s->run->cancel.store(true);
    return true;
}

bool agent_registry::join_run(kimix::string_view session_id,
                              subagent_run_result &out) {
    const kimix::string id(session_id);
    agent_run *run = nullptr;
    {
        std::lock_guard<kimix::spin_mutex> g(_mutex);
        slot *s = find_locked(id);
        if (s == nullptr || s->run == nullptr) {
            // No live run: close() may have parked the settled result here
            // (interrupt_agent closes the session before anyone joins).
            const auto it = _finished.find(id);
            if (it == _finished.end()) {
                return false;
            }
            out = std::move(it->second);
            _finished.erase(it);
            return true;
        }
        run = s->run.get();
    }
    // The worker calls back into the registry (find_locked, drain_steer),
    // which needs the mutex - so join WITHOUT holding it.
    if (run->worker.joinable()) {
        run->worker.join();
    }
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    out = std::move(run->result);
    return true;
}

bool agent_registry::run_finished(kimix::string_view session_id) const {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    const slot *s = find_locked(session_id);
    if (s != nullptr && s->run != nullptr && s->run->finished.load()) {
        return true;
    }
    return _finished.find(kimix::string(session_id)) != _finished.end();
}

void agent_registry::clear_run(kimix::string_view session_id) {
    std::lock_guard<kimix::spin_mutex> g(_mutex);
    _finished.erase(kimix::string(session_id));
    slot *s = find_locked(session_id);
    if (s != nullptr && s->run != nullptr) {
        if (s->run->worker.joinable()) {
            s->run->worker.join();
        }
        s->run.reset();
    }
}

agent_registry &session_registry(kimix::builtin_tools::Session *session) {
    // A session always owns its registry; the standalone fallback keeps the
    // tools usable when a caller constructs them with a null Session.
    static agent_registry process_default;
    if (session == nullptr) {
        return process_default;
    }
    if (session->agents == nullptr) {
        session->agents =
            kimix::shared_ptr<agent_registry>(new agent_registry());
        session->agents->owner_session_id = session->session_id;
    }
    return *session->agents;
}

// ---------------------------------------------------------------------------
// Pure kernels
// ---------------------------------------------------------------------------

kimix::string format_pending_messages(
    kimix::span<const kimix::string> messages) {
    if (messages.empty()) {
        return {};
    }
    kimix::vector<kimix::string> lines;
    lines.reserve(messages.size() + 4);
    lines.push_back("<pending-messages>");
    lines.push_back("You have the following queued message(s) from the parent "
                    "agent (sent while you were idle or not running):");
    size_t index = 1;
    for (const kimix::string &m : messages) {
        lines.push_back(kimix::format("{}. {}", index, kimix::string_view(m)));
        ++index;
    }
    lines.push_back("</pending-messages>");
    kimix::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

kimix::string queued_message_output(kimix::string_view target_id,
                                    kimix::string_view reason) {
    kimix::string out = "Agent '";
    out.append(target_id.data(), target_id.size());
    out += "' is not running (";
    out.append(reason.data(), reason.size());
    out += "). Message queued; it will be listed in the target's next prompt "
           "only if you resume the session with subagent(session_id='";
    out.append(target_id.data(), target_id.size());
    out += "', ...). Otherwise it stays queued (no delivery).";
    return out;
}

bool resolve_prompt(kimix::string_view prompt, kimix::string_view base_dir,
                    const read_file_fn &read_file, kimix::string &out,
                    kimix::string &error) {
    out.clear();
    error.clear();
    if (prompt.empty() || prompt.front() != '@') {
        out = kimix::string(prompt);
        return true;
    }
    const kimix::string rel(prompt.substr(1));
    read_file_fn reader = read_file ? read_file : read_file_fn(ag_default_read_file);
    // Python (_resolve_prompt 168-179): a relative reference resolves against
    // base_dir, "falling back to CWD-relative resolution" when the base-dir
    // candidate does not exist. Existence is probed THROUGH the reader so an
    // injected reader (tests, or a host with a virtual file table) is
    // authoritative instead of the process file system.
    if (!ag_is_absolute(rel)) {
        const kimix::string base = base_dir.empty() ? kimix::string(".")
                                                    : kimix::string(base_dir);
        const kimix::string candidate = ag_join(base, rel);
        if (reader(candidate, out)) {
            return true;
        }
        out.clear();
    }
    if (reader(rel, out)) {
        return true;
    }
    out.clear();
    error = "prompt file not found: " + rel;
    return false;
}

kimix::string prompt_saved_message(kimix::string_view prompt,
                                   kimix::string_view saved_display) {
    if (prompt.empty() || saved_display.empty()) {
        return {};
    }
    kimix::string out = "[prompt saved to ";
    out.append(saved_display.data(), saved_display.size());
    out += "] Retry with subagent(prompt=@";
    out.append(saved_display.data(), saved_display.size());
    out += ") to reuse this prompt.";
    return out;
}

kimix::string build_context_block(
    kimix::span<const kimix::string> context_files,
    kimix::string_view context_data_json, const read_file_fn &read_file,
    kimix::string_view base_dir) {
    if (context_files.empty() && context_data_json.empty()) {
        return {};
    }
    read_file_fn reader = read_file ? read_file : read_file_fn(ag_default_read_file);
    kimix::vector<kimix::string> parts;
    parts.push_back("<context>");
    for (const kimix::string &fp : context_files) {
        // Agent.__call__ 601-608: `base_dir / fp`, read as UTF-8 with
        // errors="replace"; a failure renders the self-closing error element.
        // An absolute reference is used verbatim; a relative one is tried
        // against base_dir first and then bare (same resolution order as
        // resolve_prompt).
        kimix::string content;
        kimix::string full = fp;
        bool loaded = false;
        if (ag_is_absolute(fp)) {
            loaded = reader(full, content);
        } else {
            const kimix::string joined =
                ag_join(base_dir.empty() ? kimix::string_view(".") : base_dir,
                        fp);
            loaded = reader(joined, content);
            if (loaded) {
                full = joined;
            } else {
                content.clear();
                loaded = reader(fp, content);
            }
        }
        if (loaded) {
            parts.push_back(kimix::format("<file path='{}'>", kimix::string_view(fp)));
            parts.push_back(content);
            parts.push_back("</file>");
        } else {
            parts.push_back(kimix::format(
                "<file path='{}' error='[Errno 2] No such file or directory: {}'/>",
                kimix::string_view(fp), kimix::string_view(full)));
        }
    }
    if (!context_data_json.empty()) {
        parts.push_back("<data>");
        parts.push_back(kimix::string(context_data_json));
        parts.push_back("</data>");
    }
    parts.push_back("</context>");
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += parts[i];
    }
    return out;
}

kimix::string inject_response(kimix::string_view prompt,
                              kimix::string_view question,
                              kimix::string_view response) {
    kimix::string out = "The parent agent responded to your question (";
    out.append(question.data(), question.size());
    out += "):\n\n";
    out.append(response.data(), response.size());
    out += "\n\nNow, regarding your original task: ";
    out.append(prompt.data(), prompt.size());
    return out;
}

kimix::string offload_long_prompt(kimix::string_view prompt,
                                  kimix::string_view saved_display) {
    if (prompt.size() <= k_prompt_offload_bytes) {
        return kimix::string(prompt);
    }
    return "Please read the task from `" + kimix::string(saved_display) +
           "` and execute it.";
}

ValueElement turn_to_value(const conversation_turn &turn) {
    kimix::shared_ptr<ToolParams> obj(new ToolParams());
    obj->values["role"] = ValueElement::make_string(turn.role);
    obj->values["content"] = ValueElement::make_string(turn.content);
    obj->values["timestamp"] = ValueElement::make_real(turn.timestamp);
    if (!turn.type.empty()) {
        kimix::shared_ptr<ToolParams> meta(new ToolParams());
        meta->values["type"] = ValueElement::make_string(turn.type);
        obj->values["metadata"] = ValueElement::make_object(std::move(meta));
    } else {
        obj->values["metadata"] = ValueElement::make_null();
    }
    return ValueElement::make_object(std::move(obj));
}

kimix::string format_history_markdown(
    kimix::span<const conversation_turn> turns) {
    kimix::vector<kimix::string> lines;
    size_t index = 1;
    for (const conversation_turn &t : turns) {
        const char *icon = "?";
        if (t.role == "user") {
            icon = "\xF0\x9F\x91\xA4"; // U+1F464
        } else if (t.role == "assistant") {
            icon = "\xF0\x9F\xA4\x96"; // U+1F916
        } else if (t.role == "tool") {
            icon = "\xF0\x9F\x94\xA7"; // U+1F527
        } else if (t.role == "error") {
            icon = "\xE2\x9D\x8C"; // U+274C
        } else if (t.role == "system") {
            icon = "\xE2\x9A\x99\xEF\xB8\x8F"; // U+2699 U+FE0F
        }
        const kimix::string label = t.type.empty() ? t.role : t.type;
        lines.push_back(kimix::format("### Turn {}: {} {}", index,
                                      kimix::string_view(icon),
                                      kimix::string_view(label)));
        lines.push_back(t.content);
        lines.push_back("");
        ++index;
    }
    kimix::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

kimix::string format_history_summary(
    kimix::span<const conversation_turn> turns) {
    size_t tool_calls = 0;
    size_t tool_results = 0;
    size_t text_turns = 0;
    size_t total_chars = 0;
    for (const conversation_turn &t : turns) {
        if (t.type == "tool_call") {
            ++tool_calls;
        } else if (t.type == "tool_result") {
            ++tool_results;
        }
        if (t.role == "assistant" && t.type == "text") {
            ++text_turns;
            total_chars += utf8_code_point_count(t.content);
        }
    }
    return kimix::format(
        "Sub-agent made {} tool call(s) with {} result(s), and produced {} "
        "text response(s) ({} total characters).",
        tool_calls, tool_results, text_turns, total_chars);
}

kimix::string list_active_json(kimix::span<const agent_list_item> items) {
    ValueElement::Array arr;
    arr.reserve(items.size());
    for (const agent_list_item &it : items) {
        kimix::shared_ptr<ToolParams> obj(new ToolParams());
        // orjson preserves dict insertion order: session_id, created_at,
        // last_accessed, total_turns, state, is_active.
        obj->values["session_id"] = ValueElement::make_string(it.session_id);
        obj->values["created_at"] = ValueElement::make_real(it.created_at);
        obj->values["last_accessed"] = ValueElement::make_real(it.last_accessed);
        obj->values["total_turns"] =
            ValueElement::make_int(static_cast<int64_t>(it.total_turns));
        obj->values["state"] = ValueElement::make_string(it.state);
        obj->values["is_active"] = ValueElement::make_bool(it.is_active);
        arr.push_back(ValueElement::make_object(std::move(obj)));
    }
    // Hand-rolled to keep orjson's key order (ToolParams is an unordered map).
    if (arr.empty()) {
        return "[]";
    }
    kimix::string out = "[\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const agent_list_item &it = items[i];
        out.append("  {\n");
        out.append("    \"session_id\": ");
        ag_json_escape(it.session_id, out);
        out.append(",\n");
        out.append("    \"created_at\": " + ag_json_number(it.created_at) +
                   ",\n");
        out.append("    \"last_accessed\": " +
                   ag_json_number(it.last_accessed) + ",\n");
        out.append("    \"total_turns\": " +
                   kimix::format("{}", static_cast<int64_t>(it.total_turns)) +
                   ",\n");
        out.append("    \"state\": ");
        ag_json_escape(it.state, out);
        out.append(",\n");
        out.append("    \"is_active\": ");
        out.append(it.is_active ? "true" : "false");
        out.append("\n  }");
        if (i + 1 < items.size()) {
            out.push_back(',');
        }
        out.push_back('\n');
    }
    out.push_back(']');
    return out;
}

send_target resolve_send_target(const agent_registry &registry,
                                bool caller_is_sub_agent,
                                kimix::string_view parent_session_id,
                                kimix::string_view requested_id) {
    send_target t;
    if (caller_is_sub_agent) {
        // Sub-agents always message their parent; `id` is ignored.
        if (parent_session_id.empty()) {
            t.reason = "sub-agent has no recorded parent_session_id";
            return t;
        }
        t.target_id = kimix::string(parent_session_id);
        if (!registry.has_session(parent_session_id)) {
            t.reason = kimix::format("parent agent '{}' is not registered",
                                     parent_session_id);
            return t;
        }
        t.has_live_session = true;
        return t;
    }
    if (!requested_id.empty()) {
        t.target_id = kimix::string(requested_id);
        if (!registry.has_session(requested_id)) {
            t.reason =
                kimix::format("agent '{}' is not registered", requested_id);
            return t;
        }
        t.has_live_session = true;
        return t;
    }
    // Default: the most recently active sub-agent in this session's store.
    const kimix::vector<agent_list_item> active = registry.list_active();
    if (active.empty()) {
        t.reason = "no active sub-agents to message";
        return t;
    }
    const agent_list_item *best = &active[0];
    for (const agent_list_item &item : active) {
        if (item.last_accessed > best->last_accessed) {
            best = &item;
        }
    }
    t.target_id = best->session_id;
    t.has_live_session = registry.has_session(best->session_id);
    return t;
}

kimix::string prefix_sender(kimix::string_view caller_id,
                            kimix::string_view message) {
    if (caller_id.empty()) {
        return kimix::string(message);
    }
    kimix::string out = "Message from agent '";
    out.append(caller_id.data(), caller_id.size());
    out += "':\n";
    out.append(message.data(), message.size());
    return out;
}

bool is_resumable(const agent_registry &registry, kimix::string_view session_id) {
    if (session_id.empty()) {
        return false;
    }
    const agent_entry *entry = registry.get(session_id);
    return (entry != nullptr && entry->is_active);
}

// ---------------------------------------------------------------------------
// Parameter parsing
// ---------------------------------------------------------------------------

namespace {

tool_error ag_string(const ToolParams *params, kimix::string_view name,
                     kimix::string_view alias, bool required,
                     kimix::optional<kimix::string> &out) {
    out = std::nullopt;
    if (params == nullptr) {
        return required ? tool_error{tool_status::invalid_input,
                                     kimix::format("missing required field: {}",
                                                   name)}
                        : tool_error{tool_status::ok, {}};
    }
    const ValueElement *el = params->get(name);
    if ((el == nullptr || el->is_null()) && !alias.empty()) {
        el = params->get(alias);
    }
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

bool ag_bool(const ToolParams *params, kimix::string_view name, bool fallback) {
    if (params == nullptr) {
        return fallback;
    }
    const ValueElement *el = params->get(name);
    return (el != nullptr && el->is_bool()) ? el->as_bool() : fallback;
}

} // namespace

// Fuzzy alias matching (tool.h): the alternate argument names the model may
// send instead of the documented one (`task` for `prompt`, ...). The canonical
// name always wins; the explicit fallbacks inside parse_subagent_params stay as
// a second chance.
static const kimix::builtin_tools::param_alias k_subagent_aliases[] = {
    {"description", "desc name summary task_description"},
    {"prompt", "task instruction message question"},
    {"run_in_background", "background async run_async in_background"},
    {"session_id", "session resume_session_id resume"},
    {"close_session", "close close_after"},
    {"return_history", "history with_history return_messages"},
    {"history_format", "format history_mode"},
    {"response", "answer reply"},
    {"context_files", "files context_file file_paths"},
    {"context_data", "data context payload"},
    {"inherit_context", "inherit inherit_session context_inherit"},
};

tool_error parse_subagent_params(const ToolParams *params,
                                 subagent_params &out) {
    // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
    // ("task" for "prompt") are accepted; the canonical name always wins.
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_subagent_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = subagent_params{};
    tool_error err = ag_string(params, "description", {}, false, out.description);
    if (err.failed()) {
        return err;
    }
    kimix::optional<kimix::string> prompt;
    err = ag_string(params, "prompt", "task", true, prompt);
    if (err.failed()) {
        return err;
    }
    out.prompt = prompt.value_or(kimix::string());
    out.run_in_background = ag_bool(params, "run_in_background", true);
    err = ag_string(params, "session_id", "session", false, out.session_id);
    if (err.failed()) {
        return err;
    }
    out.close_session = ag_bool(params, "close_session", true);
    out.return_history = ag_bool(params, "return_history", false);
    kimix::optional<kimix::string> history_format;
    err = ag_string(params, "history_format", {}, false, history_format);
    if (err.failed()) {
        return err;
    }
    if (history_format.has_value()) {
        const kimix::string &fmt = *history_format;
        if (fmt != "json" && fmt != "markdown" && fmt != "summary") {
            return {tool_status::invalid_input,
                    kimix::format("Input should be 'json', 'markdown' or "
                                  "'summary' (history_format={})",
                                  kimix::string_view(fmt))};
        }
        out.history_format = fmt;
    }
    err = ag_string(params, "response", {}, false, out.response);
    if (err.failed()) {
        return err;
    }
    if (params != nullptr) {
        if (const ValueElement *files = params->get("context_files");
            files != nullptr && !files->is_null()) {
            if (!files->is_array()) {
                return {tool_status::invalid_input,
                        "context_files must be a list of strings"};
            }
            for (const ValueElement &f : files->as_array()) {
                if (!f.is_string()) {
                    return {tool_status::invalid_input,
                            "context_files must be a list of strings"};
                }
                out.context_files.push_back(f.as_string());
            }
        }
        if (const ValueElement *data = params->get("context_data");
            data != nullptr && !data->is_null()) {
            // Serialize the structured payload with orjson OPT_INDENT_2 shape.
            kimix::string pretty;
            ag_pretty_value(*data, 0, pretty);
            out.context_data_json = std::move(pretty);
        }
    }
    out.inherit_context = ag_bool(params, "inherit_context", false);
    return {tool_status::ok, {}};
}

// Fuzzy alias matching (tool.h): aliases of the send_message parameters.
static const kimix::builtin_tools::param_alias k_send_message_aliases[] = {
    {"message", "question msg text content"},
    {"subagent_id", "id agent_id session_id subagent target"},
};

tool_error parse_send_message_params(const ToolParams *params,
                                     send_message_params &out) {
    // Fuzzy alias matching (tool.h): "question" is accepted for "message".
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_send_message_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = send_message_params{};
    kimix::optional<kimix::string> message;
    tool_error err = ag_string(params, "message", "question", true, message);
    if (err.failed()) {
        return err;
    }
    out.message = message.value_or(kimix::string());
    err = ag_string(params, "subagent_id", "id", false, out.subagent_id);
    if (err.failed()) {
        return err;
    }
    return {tool_status::ok, {}};
}

// Fuzzy alias matching (tool.h): aliases of the list_agents parameters.
static const kimix::builtin_tools::param_alias k_list_agents_aliases[] = {
    {"scope", "mode filter range"},
};

tool_error parse_list_agents_params(const ToolParams *params,
                                    list_agents_params &out) {
    // Fuzzy alias matching (tool.h): the canonical name always wins.
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_list_agents_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = list_agents_params{};
    kimix::optional<kimix::string> scope;
    const tool_error err = ag_string(params, "scope", {}, false, scope);
    if (err.failed()) {
        return err;
    }
    if (scope.has_value()) {
        out.scope = *scope;
    }
    return {tool_status::ok, {}};
}

// Fuzzy alias matching (tool.h): aliases of the interrupt_agent parameters.
static const kimix::builtin_tools::param_alias k_interrupt_aliases[] = {
    {"agent_id", "id session session_id subagent_id target"},
};

tool_error parse_interrupt_params(const ToolParams *params,
                                  interrupt_agent_params &out) {
    // Fuzzy alias matching (tool.h): "session"/"session_id" are accepted for
    // "agent_id" (the explicit fallbacks below stay as a second chance).
    const ToolParams k_resolved =
        ToolParams::with_aliases(params, k_interrupt_aliases);
    if (params != nullptr) {
        params = &k_resolved;
    }
    out = interrupt_agent_params{};
    kimix::optional<kimix::string> id;
    tool_error err = ag_string(params, "agent_id", "session", true, id);
    if (err.failed()) {
        // Third alias: session_id.
        err = ag_string(params, "session_id", {}, true, id);
        if (err.failed()) {
            return err;
        }
    }
    out.agent_id = id.value_or(kimix::string());
    return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// Subagent
// ---------------------------------------------------------------------------

Subagent::Subagent(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Subagent::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;
    agent_registry &registry = session_registry(_session);

    // Agent.__call__ 565-571: recursion guard.
    if (_session != nullptr && _session->is_sub_agent) {
        ag_error(result, tool_status::blocked,
                 "Recursive sub-agent call detected", "",
                 "sub-agent recursively");
        result.serialize(_result);
        return;
    }

    subagent_params params;
    const tool_error perr = parse_subagent_params(parameters, params);
    if (perr.failed()) {
        ag_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }

    if (!registry.runner) {
        ag_error(result, tool_status::unsupported,
                 "native subagent requires an injected runner", "",
                 "Unsupported");
        result.serialize(_result);
        return;
    }

    // ---- resolve the session id (Agent._resolve_session) ----------------
    const bool resume_requested =
        params.session_id.has_value() && !params.session_id->empty();
    const bool reused = resume_requested &&
                        is_resumable(registry, *params.session_id);
    kimix::string session_id =
        reused ? *params.session_id
               : (resume_requested ? *params.session_id : ag_new_session_id());
    const agent_entry *existing = registry.get(session_id);
    const kimix::optional<kimix::string> pending_question =
        (existing != nullptr) ? existing->pending_question : std::nullopt;

    // ---- prompt resolution ---------------------------------------------
    const kimix::string work_dir = ag_work_dir(_session);
    read_file_fn reader =
        registry.read_file ? registry.read_file : read_file_fn(ag_default_read_file);
    save_prompt_fn saver = registry.save_prompt
                               ? registry.save_prompt
                               : save_prompt_fn(ag_default_save_prompt);

    kimix::string task_text;
    kimix::string prompt_error;
    if (!resolve_prompt(params.prompt, work_dir, reader, task_text,
                        prompt_error)) {
        const kimix::string saved = saver(params.prompt, ".md");
        kimix::string message = prompt_error;
        const kimix::string suffix = prompt_saved_message(params.prompt, saved);
        if (!suffix.empty()) {
            message += " " + suffix;
        }
        ag_error(result, tool_status::not_found, message, "",
                 "Failed to create sub-agent session");
        if (!saved.empty()) {
            kimix::shared_ptr<ToolParams> extras(new ToolParams());
            extras->values["prompt_file"] = ValueElement::make_string(saved);
            result.values["extras"] = ValueElement::make_object(std::move(extras));
        }
        result.serialize(_result);
        return;
    }

    // Long-prompt offload (Agent.__call__ 583-590).
    kimix::string task_prompt = task_text;
    if (task_text.size() > k_prompt_offload_bytes) {
        const kimix::string saved = saver(task_text, ".md");
        task_prompt = offload_long_prompt(task_text, saved);
    }

    // Context block.
    kimix::string prompt = task_prompt;
    if (!params.context_files.empty() || params.context_data_json.has_value()) {
        const kimix::string block = build_context_block(
            kimix::span<const kimix::string>(params.context_files),
            params.context_data_json.value_or(kimix::string()), reader,
            work_dir);
        prompt = block + "\n\n" + prompt;
    }

    // Deprecated `response` injection.
    if (reused && pending_question.has_value() && params.response.has_value()) {
        prompt = inject_response(prompt, *pending_question, *params.response);
    }

    // Queued send_message payloads.
    const kimix::vector<kimix::string> queued =
        registry.drain_pending_messages(session_id);
    if (!queued.empty()) {
        prompt = prompt + "\n\n" + format_pending_messages(
                                       kimix::span<const kimix::string>(queued));
    }

    // ---- run ------------------------------------------------------------
    subagent_request request;
    request.session_id = session_id;
    request.prompt = prompt;
    request.work_dir = work_dir;
    request.description = params.description.value_or(kimix::string());
    request.resume = reused;
    request.inherit_context = params.inherit_context && !reused;
    request.close_session = params.close_session;
    request.background = params.run_in_background;

    // Make room in the store before registering (store.evict_lru_if_needed).
    registry.evict_lru_if_needed();

    agent_entry entry;
    entry.session_id = session_id;
    entry.created_at =
        (existing != nullptr) ? existing->created_at : registry.clock_now();
    entry.last_accessed = registry.clock_now();
    entry.is_active = true;
    entry.state = "running";
    registry.put(entry);
    registry.register_session(session_id);

    if (params.run_in_background) {
        if (!registry.start_background(session_id, request)) {
            ag_error(result, tool_status::external_library,
                     "failed to start the sub-agent worker", "",
                     "Failed to create sub-agent session");
            result.serialize(_result);
            return;
        }
        ag_ok(result, "Sub-agent task started in the background",
              "Session ID: " + session_id +
                  "\n\n(sub-agent running in the background; the runtime "
                  "reports its outcome when it settles)",
              "Sub-agent task started");
        ag_build_extras(result, session_id, "running", 0, {}, false,
                        params.return_history, params.history_format, {});
        result.serialize(_result);
        return;
    }

    subagent_run_result outcome = registry.runner(request);
    kimix::string output_text =
        outcome.output.empty() ? "(no text output)" : outcome.output;
    const kimix::string output_prefix = "Session ID: " + session_id + "\n\n";

    if (!outcome.ok) {
        const kimix::string saved = saver(prompt, ".md");
        kimix::string message = outcome.error;
        const kimix::string suffix = prompt_saved_message(prompt, saved);
        if (!suffix.empty()) {
            message = message.empty() ? suffix : (message + " " + suffix);
        }
        ag_error(result, tool_status::external_library, message,
                 output_prefix + output_text, "sub-agent task failed");
        ag_build_extras(result, session_id, "closed", outcome.turns.size(), {},
                        false, params.return_history, params.history_format,
                        kimix::span<const conversation_turn>(outcome.turns));
        if (!saved.empty()) {
            ToolParams *extras = result.values["extras"].as_object();
            if (extras != nullptr) {
                extras->values["prompt_file"] = ValueElement::make_string(saved);
            }
        }
        registry.close(session_id);
        registry.unregister_session(session_id);
        result.serialize(_result);
        return;
    }

    // Awaiting a response from the parent.
    if (outcome.pending_question.has_value()) {
        agent_entry updated;
        updated.session_id = session_id;
        updated.created_at = entry.created_at;
        updated.last_accessed = registry.clock_now();
        updated.conversation_history = outcome.turns;
        updated.total_turns = static_cast<int32_t>(outcome.turns.size());
        updated.is_active = true;
        updated.pending_question = outcome.pending_question;
        updated.state = "awaiting_response";
        registry.put(updated);
        ag_ok(result, "", output_prefix + output_text,
              "Sub-agent is awaiting a response");
        ag_build_extras(result, session_id, "awaiting_response",
                        outcome.turns.size(), *outcome.pending_question, true,
                        params.return_history, params.history_format,
                        kimix::span<const conversation_turn>(outcome.turns));
        result.serialize(_result);
        return;
    }

    // Agent._update_store (906-943).
    if (params.close_session) {
        registry.close(session_id);
        registry.unregister_session(session_id);
    } else {
        agent_entry updated;
        updated.session_id = session_id;
        updated.created_at = entry.created_at;
        updated.last_accessed = registry.clock_now();
        updated.conversation_history = outcome.turns;
        updated.total_turns = static_cast<int32_t>(outcome.turns.size());
        updated.is_active = true;
        updated.pending_question = std::nullopt;
        updated.state = "completed";
        registry.put(updated);
    }
    ag_ok(result, "", output_prefix + output_text, "Sub-agent task completed");
    ag_build_extras(result, session_id,
                    params.close_session ? "closed" : "continued",
                    outcome.turns.size(), {}, false, params.return_history,
                    params.history_format,
                    kimix::span<const conversation_turn>(outcome.turns));
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// SendMessageTool (registered as "SendMessage")
// ---------------------------------------------------------------------------

SendMessageTool::SendMessageTool(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void SendMessageTool::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;
    agent_registry &registry = session_registry(_session);

    send_message_params params;
    const tool_error perr = parse_send_message_params(parameters, params);
    if (perr.failed()) {
        ag_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }

    const kimix::string caller_id =
        (_session != nullptr) ? _session->session_id : kimix::string();
    const bool caller_is_sub =
        (_session != nullptr) && _session->is_sub_agent;
    const kimix::string parent_id =
        (_session != nullptr) ? _session->parent_session_id : kimix::string();
    const kimix::string requested =
        params.subagent_id.value_or(kimix::string());

    const send_target target =
        resolve_send_target(registry, caller_is_sub, parent_id, requested);

    if (target.target_id.has_value() && *target.target_id == caller_id) {
        ag_error(result, tool_status::invalid_input,
                 "Cannot message yourself.", "", "Self message rejected");
        result.serialize(_result);
        return;
    }

    const kimix::string message = prefix_sender(caller_id, params.message);

    if (!target.has_live_session) {
        if (target.target_id.has_value()) {
            registry.queue_pending_message(*target.target_id, message);
            ag_ok(result, "",
                  queued_message_output(*target.target_id,
                                        "session closed or idle"),
                  "Message queued");
            result.serialize(_result);
            return;
        }
        const kimix::string reason =
            target.reason.empty() ? "target not found" : target.reason;
        ag_error(result, tool_status::not_found,
                 "Cannot resolve target agent: " + reason, "",
                 "Target agent not found");
        result.serialize(_result);
        return;
    }

    const kimix::string target_id = target.target_id.value_or(kimix::string());
    if (registry.push_steer(target_id, message)) {
        ag_ok(result, "", "Message delivered to agent '" + target_id + "'.",
              "Message sent");
        result.serialize(_result);
        return;
    }
    // The session is live but idle: queue it for the next prompt.
    registry.queue_pending_message(target_id, message);
    ag_ok(result, "", queued_message_output(target_id, "not running"),
          "Message queued");
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// ListAgents
// ---------------------------------------------------------------------------

ListAgents::ListAgents(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void ListAgents::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;
    list_agents_params params;
    const tool_error perr = parse_list_agents_params(parameters, params);
    if (perr.failed()) {
        ag_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }
    agent_registry &registry = session_registry(_session);
    const kimix::vector<agent_list_item> items = registry.list_active();
    ag_ok(result, "",
          list_active_json(kimix::span<const agent_list_item>(items)),
          "Listed active subagents");
    result.values["scope"] = ValueElement::make_string(params.scope);
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// InterruptAgent
// ---------------------------------------------------------------------------

InterruptAgent::InterruptAgent(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void InterruptAgent::operator()(const ToolParams *parameters) {
    _result.clear();
    ToolParams result;
    interrupt_agent_params params;
    const tool_error perr = parse_interrupt_params(parameters, params);
    if (perr.failed()) {
        ag_error(result, perr.status, perr.message, "", "Invalid params");
        result.serialize(_result);
        return;
    }
    agent_registry &registry = session_registry(_session);
    if (registry.get(params.agent_id) == nullptr) {
        ag_error(result, tool_status::not_found, "Session not found", "",
                 "Session not found");
        result.serialize(_result);
        return;
    }
    registry.request_cancel(params.agent_id);
    registry.close(params.agent_id);
    registry.unregister_session(params.agent_id);
    ag_ok(result, "",
          kimix::format("Session {} closed.",
                        kimix::string_view(params.agent_id)),
          "Session closed");
    result.serialize(_result);
}

// ---------------------------------------------------------------------------
// Static registration
// ---------------------------------------------------------------------------

KIMIX_REGISTER_TOOL(
    Subagent,
    "Delegate a self-contained task to a subagent (a separate agent that works "
    "in its own context) to offload focused, independent work - research, a "
    "scoped implementation, an analysis - so it does not consume this "
    "conversation's context. The subagent returns its result, not its "
    "intermediate steps. Give it a complete, standalone prompt: it does not see "
    "this conversation. This tool runs in the background by default, "
    "immediately returns a durable subagent id, and keeps the child "
    "conversation available for later turns. When that run settles, the "
    "runtime sends the parent a notice containing its outcome and any final "
    "assistant message; send_message starts a later turn in the same child "
    "conversation. Set run_in_background: false only when your next action "
    "depends on receiving the result. Use send_message to answer a sub-agent's "
    "pending question.",
    R"JSON({"type":"object","properties":{"description":{"type":"string","description":"A short (3-5 word) description of the delegated task, for display."},"prompt":{"type":"string","description":"The complete, self-contained task for the subagent. Inline prompt text, or @path to read the task from a file (saved prompt paths are returned on failure). Accepts `prompt` or `task`."},"run_in_background":{"type":"boolean","description":"Whether to run in the background and return a durable subagent id immediately. Defaults to true. Set false to wait for the result when your next action depends on it."},"session_id":{"type":"string","description":"Optional session ID to resume an existing sub-agent session. Accepts `session_id` or `session`."},"close_session":{"type":"boolean","description":"Close the subagent session after this prompt. Set to False to keep it open for future follow-up."},"return_history":{"type":"boolean","description":"Return the full conversation history in extras."},"history_format":{"type":"string","enum":["json","markdown","summary"],"description":"'json': Raw conversation turns in JSON. 'markdown': Formatted as Markdown with headings. 'summary': Concise summary of what the sub-agent did."},"response":{"type":"string","description":"[Deprecated] Response to the sub-agent's pending question. Use the send_message tool instead."},"context_files":{"type":"array","items":{"type":"string"},"description":"File paths to pre-read into the sub-agent's context before the prompt."},"context_data":{"type":"object","description":"Structured JSON data to pass as context to the sub-agent."},"inherit_context":{"type":"boolean","description":"When True, a NEW sub-agent session is initialized by copying the parent agent's current session context. Ignored when `session_id` resolves to an active sub-agent session."}},"required":["prompt"]})JSON");

// Registered as "SendMessage": the class itself is SendMessageTool because
// <winuser.h> `#define SendMessage SendMessageW` would rewrite the class
// name in any unity batch that also contains process_runner.cpp.
KIMIX_REGISTER_TOOL_NAMED(
    SendMessageTool, "SendMessage",
    "Send a message to a background subagent by its subagent id, continuing the "
    "same conversation. If the target is running, the message becomes its next "
    "turn (waiting until the current turn finishes, so it cannot redirect work "
    "already underway). If the target is idle or its session is closed, the "
    "message is queued: it is delivered only when the session is resumed with "
    "subagent(session_id='<id>', ...), so it is NOT delivered automatically to "
    "a closed session. This call returns no answer from the subagent - only "
    "confirmation that the message was delivered or queued - so use it to give "
    "it more work. A failure means the message was NOT delivered.",
    R"JSON({"type":"object","properties":{"message":{"type":"string","description":"The message to deliver to the subagent. Delivered immediately if the target is running; otherwise queued and listed at its next prompt. Accepts `message` or `question`."},"subagent_id":{"type":"string","description":"The subagent id returned when the background subagent was started. Optional: omit to message the most recently active sub-agent. Ignored for sub-agents, which always message their parent. Accepts `subagent_id` or `id`."}},"required":["message"]})JSON");

KIMIX_REGISTER_TOOL(
    ListAgents,
    "List your continuable background subagents by durable id and label. Use it "
    "to recall which ones you started, not to poll for completion - you are "
    "told when one finishes. Each entry reports session_id, created_at, "
    "last_accessed, total_turns, state and is_active; sessions closed via "
    "interrupt_agent are removed from the list. A listed child remains a "
    "send_message candidate: send_message starts a new turn on the same "
    "conversation when it is running, or queues a message that is delivered "
    "when the session is resumed with subagent(session_id=..., ...). Scope "
    "`descendants` is accepted for compatibility but currently returns the "
    "same direct-children list.",
    R"JSON({"type":"object","properties":{"scope":{"type":"string","description":"`children` (default) lists direct children only. `descendants` is accepted for compatibility but currently returns the same direct-children list."}}})JSON");

KIMIX_REGISTER_TOOL(
    InterruptAgent,
    "Request cancellation of a background agent's current turn by its agent id. "
    "The target may be your direct child or a deeper agent created under you. "
    "The current turn stops (agents it started keep running) and the subagent "
    "session is closed and removed from the active list - messages queued for "
    "it are preserved and will be listed if the same session id is resumed "
    "with subagent(session_id=..., ...). This call returns as soon as the stop "
    "request is accepted, so the target may keep running briefly; interrupting "
    "an agent that already finished still closes its session (no error).",
    R"JSON({"type":"object","properties":{"agent_id":{"type":"string","description":"The agent id of the running agent to interrupt. Accepts `agent_id`, `session` or `session_id`."}},"required":["agent_id"]})JSON");

} // namespace kimix::builtin_tools::agents
