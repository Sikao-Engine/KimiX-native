/*
 * session_store.cpp - per-session part ring + session table.
 * See session_store.h for the model and the rendered JSON contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers. The arena is append-only; compact_arena() rewrites it when the
 * dead-byte ratio passes 25 %, so a long-lived serve process does not keep the
 * text of every evicted session.
 */

#include <core/kimix_core.h>

#include <runtime/common/blob_util.h>
#include <runtime/common/json_out.h>
#include <runtime/common/le_bytes.h>
#include <runtime/soul/session_store.h>

#include <yyjson.h>

#include <llm/yyjson_alc.h>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_session_store {

using kimix::llm::kYYJsonAlcMi;
using kimix::runtime::common::json_add_fragment;
using kimix::runtime::common::json_add_str;
using kimix::runtime::common::json_make_str;
using kimix::runtime::common::json_write;

inline kimix::string_view ss_kind_name(uint8_t kind) noexcept {
    switch (kind) {
    case kPartReasoning:
        return "reasoning";
    case kPartTool:
        return "tool";
    case kPartStepStart:
        return "step-start";
    case kPartStepFinish:
        return "step-finish";
    default:
        return "text";
    }
}

inline kimix::string_view ss_role_name(uint8_t role) noexcept {
    switch (role) {
    case kRoleUser:
        return "user";
    case kRoleSystem:
        return "system";
    default:
        return "assistant";
    }
}

using kimix::runtime::common::blob_capacity_bytes;

} // namespace src_runtime_soul_session_store
using namespace src_runtime_soul_session_store;

SessionStore::SessionStore(uint64_t per_session_budget_bytes,
                           uint64_t total_budget_bytes)
    : _per_session_budget(per_session_budget_bytes),
      _total_budget(total_budget_bytes) {}

// ── lookups ──────────────────────────────────────────────────────────────────

SessionStore::session_slot *SessionStore::find(uint32_t session) noexcept {
    if (session >= _sessions.size() || !_sessions[session].live) {
        return nullptr;
    }
    return &_sessions[session];
}

const SessionStore::session_slot *SessionStore::find(uint32_t session) const noexcept {
    if (session >= _sessions.size() || !_sessions[session].live) {
        return nullptr;
    }
    return &_sessions[session];
}

uint32_t SessionStore::find_by_id(kimix::string_view id) const noexcept {
    for (uint32_t i = 0; i < _sessions.size(); ++i) {
        if (_sessions[i].live && _sessions[i].id == id) {
            return i;
        }
    }
    return UINT32_MAX;
}

void SessionStore::touch(session_slot &s) noexcept {
    s.lru = ++_lru_counter;
}

// ── arena ────────────────────────────────────────────────────────────────────

int32_t SessionStore::put(kimix::string_view v) noexcept {
    if (v.data() == nullptr || v.empty()) {
        return static_cast<int32_t>(_arena.size());
    }
    const int32_t start = static_cast<int32_t>(_arena.size());
    _arena.append(v.data(), v.size());
    return start;
}

kimix::string_view SessionStore::view(int32_t start, int32_t end) const noexcept {
    if (end <= start) {
        return kimix::string_view();
    }
    return kimix::string_view(_arena.data() + start, static_cast<size_t>(end - start));
}

uint64_t SessionStore::slot_bytes(const part_slot &p) const noexcept {
    return static_cast<uint64_t>(p.id_end - p.id_start) +
           static_cast<uint64_t>(p.message_end - p.message_start) +
           static_cast<uint64_t>(p.part_end - p.part_start) +
           static_cast<uint64_t>(p.call_end - p.call_start) +
           static_cast<uint64_t>(p.tool_end - p.tool_start) +
           static_cast<uint64_t>(p.text_end - p.text_start) +
           static_cast<uint64_t>(p.state_end - p.state_start) +
           static_cast<uint64_t>(p.prompt_end - p.prompt_start) +
           static_cast<uint64_t>(p.tokens_end - p.tokens_start) +
           static_cast<uint64_t>(p.time_end - p.time_start) +
           static_cast<uint64_t>(p.snapshot_end - p.snapshot_start) +
           static_cast<uint64_t>(p.reason_end - p.reason_start);
}

void SessionStore::compact_arena() noexcept {
    kimix::string arena;
    arena.reserve(static_cast<size_t>(bytes_live()));
    for (session_slot &s : _sessions) {
        if (!s.live) {
            continue;
        }
        for (part_slot &p : s.parts) {
            const auto move_span = [&](int32_t &start, int32_t &end) {
                const size_t len = static_cast<size_t>(end - start);
                if (len == 0) {
                    start = static_cast<int32_t>(arena.size());
                    end = start;
                    return;
                }
                const kimix::string_view src = view(start, end);
                start = static_cast<int32_t>(arena.size());
                arena.append(src.data(), len);
                end = static_cast<int32_t>(arena.size());
            };
            move_span(p.id_start, p.id_end);
            move_span(p.message_start, p.message_end);
            move_span(p.part_start, p.part_end);
            move_span(p.call_start, p.call_end);
            move_span(p.tool_start, p.tool_end);
            move_span(p.text_start, p.text_end);
            move_span(p.state_start, p.state_end);
            move_span(p.prompt_start, p.prompt_end);
            move_span(p.tokens_start, p.tokens_end);
            move_span(p.time_start, p.time_end);
            move_span(p.snapshot_start, p.snapshot_end);
            move_span(p.reason_start, p.reason_end);
        }
    }
    _arena = std::move(arena);
    _dead_bytes = 0;
}

// ── session lifecycle ────────────────────────────────────────────────────────

uint32_t SessionStore::create(kimix::string_view session_id) noexcept {
    const uint32_t existing = find_by_id(session_id);
    if (existing != UINT32_MAX) {
        touch(_sessions[existing]);
        return existing;
    }
    uint32_t handle = UINT32_MAX;
    if (!_free_slots.empty()) {
        handle = _free_slots.back();
        _free_slots.pop_back();
    } else {
        _sessions.push_back(session_slot{});
        handle = static_cast<uint32_t>(_sessions.size() - 1);
    }
    session_slot &s = _sessions[handle];
    s.id.assign(session_id.data(), session_id.size());
    s.parts.clear();
    s.status = kSessionIdle;
    s.next_seq = 1;
    s.bytes = 0;
    s.dropped = 0;
    s.gen = 0;
    s.live = true;
    touch(s);
    ++_live_sessions;
    return handle;
}

bool SessionStore::close(uint32_t session) noexcept {
    session_slot *s = find(session);
    if (s == nullptr) {
        return false;
    }
    for (const part_slot &p : s->parts) {
        _dead_bytes += slot_bytes(p);
    }
    s->parts.clear();
    s->live = false;
    s->bytes = 0;
    s->id.clear();
    s->id.shrink_to_fit();
    _free_slots.push_back(session);
    if (_live_sessions > 0) {
        --_live_sessions;
    }
    if (_dead_bytes * 4 > _arena.capacity()) {
        compact_arena();
    }
    return true;
}

void SessionStore::clear() noexcept {
    for (uint32_t i = 0; i < _sessions.size(); ++i) {
        if (_sessions[i].live) {
            close(i);
        }
    }
    _arena.clear();
    _dead_bytes = 0;
    _free_slots.clear();
    _sessions.clear();
    _live_sessions = 0;
}

bool SessionStore::append(uint32_t session, const part_record &rec) noexcept {
    session_slot *s = find(session);
    if (s == nullptr) {
        return false;
    }
    const uint64_t incoming =
        rec.session_id.size() + rec.message_id.size() + rec.part_id.size() +
        rec.call_id.size() + rec.tool.size() + rec.text.size() +
        rec.state_json.size() + rec.prompt_json.size() + rec.tokens_json.size() +
        rec.time_json.size() + rec.snapshot.size() + rec.reason.size();
    if (_per_session_budget != 0 && incoming > _per_session_budget) {
        return false; // a single part larger than the whole budget
    }

    part_slot p;
    p.seq = rec.seq != 0 ? rec.seq : s->next_seq;
    s->next_seq = p.seq + 1;
    p.kind = rec.kind;
    p.role = rec.role;
    p.cost = rec.cost;
    const auto place = [this](kimix::string_view v, int32_t &start, int32_t &end) {
        start = put(v);
        end = start + static_cast<int32_t>(v.size());
    };
    place(rec.session_id, p.id_start, p.id_end);
    place(rec.message_id, p.message_start, p.message_end);
    place(rec.part_id, p.part_start, p.part_end);
    place(rec.call_id, p.call_start, p.call_end);
    place(rec.tool, p.tool_start, p.tool_end);
    place(rec.text, p.text_start, p.text_end);
    place(rec.state_json, p.state_start, p.state_end);
    place(rec.prompt_json, p.prompt_start, p.prompt_end);
    place(rec.tokens_json, p.tokens_start, p.tokens_end);
    place(rec.time_json, p.time_start, p.time_end);
    place(rec.snapshot, p.snapshot_start, p.snapshot_end);
    place(rec.reason, p.reason_start, p.reason_end);

    s->bytes += slot_bytes(p);
    s->parts.push_back(p);
    ++s->gen;
    touch(*s);

    // Ring: drop the oldest parts until this session fits its budget.
    while (_per_session_budget != 0 && s->bytes > _per_session_budget &&
           !s->parts.empty()) {
        const part_slot &old = s->parts.front();
        _dead_bytes += slot_bytes(old);
        s->bytes -= slot_bytes(old);
        s->parts.erase(s->parts.begin());
        ++s->dropped;
    }

    evict_to_budget();
    if (_dead_bytes * 4 > _arena.capacity()) {
        compact_arena();
    }
    return true;
}

uint32_t SessionStore::evict_to_budget() noexcept {
    if (_total_budget == 0) {
        return 0;
    }
    uint32_t evicted = 0;
    while (bytes_live() > _total_budget) {
        uint32_t victim = UINT32_MAX;
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < _sessions.size(); ++i) {
            if (_sessions[i].live && _sessions[i].lru < oldest) {
                oldest = _sessions[i].lru;
                victim = i;
            }
        }
        if (victim == UINT32_MAX) {
            break;
        }
        close(victim);
        ++evicted;
    }
    return evicted;
}

void SessionStore::set_status(uint32_t session, uint8_t status) noexcept {
    if (session_slot *s = find(session)) {
        s->status = status;
        ++s->gen;
        touch(*s);
    }
}

uint8_t SessionStore::status(uint32_t session) const noexcept {
    const session_slot *s = find(session);
    return s == nullptr ? static_cast<uint8_t>(kSessionIdle) : s->status;
}

kimix::string_view SessionStore::session_id(uint32_t session) const noexcept {
    const session_slot *s = find(session);
    return s == nullptr ? kimix::string_view() : kimix::string_view(s->id);
}

uint64_t SessionStore::generation(uint32_t session) const noexcept {
    const session_slot *s = find(session);
    return s == nullptr ? 0 : s->gen;
}

uint32_t SessionStore::session_count() const noexcept { return _live_sessions; }

uint32_t SessionStore::part_count(uint32_t session) const noexcept {
    const session_slot *s = find(session);
    return s == nullptr ? 0 : static_cast<uint32_t>(s->parts.size());
}

uint64_t SessionStore::dropped_parts(uint32_t session) const noexcept {
    const session_slot *s = find(session);
    return s == nullptr ? 0 : s->dropped;
}

uint64_t SessionStore::bytes_live() const noexcept {
    uint64_t total = 0;
    for (const session_slot &s : _sessions) {
        if (s.live) {
            total += s.bytes;
        }
    }
    return total;
}

uint64_t SessionStore::bytes_owned() const noexcept {
    uint64_t total = blob_capacity_bytes(_arena) + blob_capacity_bytes(_sessions) +
                     blob_capacity_bytes(_free_slots);
    for (const session_slot &s : _sessions) {
        total += blob_capacity_bytes(s.id) + blob_capacity_bytes(s.parts);
    }
    return total;
}

// ── rendering ────────────────────────────────────────────────────────────────

namespace src_runtime_soul_session_store {

// {"id","sessionID","messageID","type",<kind fields>} - MessagePart.to_dict()
yyjson_mut_val *make_part(yyjson_mut_doc *doc,
                kimix::string_view part_id,
                uint8_t kind, kimix::string_view session_id,
                kimix::string_view message_id, kimix::string_view text,
                kimix::string_view call_id, kimix::string_view tool,
                kimix::string_view state_json, kimix::string_view prompt_json,
                kimix::string_view tokens_json, kimix::string_view time_json,
                kimix::string_view snapshot, kimix::string_view reason,
                double cost) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    json_add_str(doc, obj, "id", part_id);
    json_add_str(doc, obj, "sessionID", session_id);
    json_add_str(doc, obj, "messageID", message_id);
    json_add_str(doc, obj, "type", ss_kind_name(kind));
    switch (kind) {
    case kPartText:
    case kPartReasoning:
        json_add_str(doc, obj, "text", text);
        if (!time_json.empty()) {
            if (!json_add_fragment(doc, obj, "time", time_json)) {
                json_add_str(doc, obj, "time", time_json);
            }
        }
        break;
    case kPartTool:
        json_add_str(doc, obj, "callID", call_id);
        json_add_str(doc, obj, "tool", tool);
        if (!json_add_fragment(doc, obj, "state", state_json)) {
            yyjson_mut_val *empty = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, obj, "state", empty);
        }
        break;
    case kPartStepStart:
        json_add_str(doc, obj, "snapshot", snapshot);
        break;
    case kPartStepFinish:
        json_add_str(doc, obj, "reason", reason);
        json_add_str(doc, obj, "snapshot", snapshot);
        if (cost != 0.0) {
            yyjson_mut_obj_add_real(doc, obj, "cost", cost);
        }
        if (!prompt_json.empty()) {
            if (!json_add_fragment(doc, obj, "prompt", prompt_json)) {
                json_add_str(doc, obj, "prompt", prompt_json);
            }
        }
        if (!tokens_json.empty()) {
            if (!json_add_fragment(doc, obj, "tokens", tokens_json)) {
                json_add_str(doc, obj, "tokens", tokens_json);
            }
        }
        break;
    default:
        break;
    }
    return obj;
}

} // namespace src_runtime_soul_session_store
using namespace src_runtime_soul_session_store;

// ── snapshot / part_json ─────────────────────────────────────────────────────

bool SessionStore::snapshot(uint32_t session, uint32_t since_seq,
                            kimix::string &out_json) noexcept {
    session_slot *s = find(session);
    if (s == nullptr) {
        return false;
    }
    touch(*s);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (doc == nullptr) {
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    json_add_str(doc, root, "sessionID", kimix::string_view(s->id));
    yyjson_mut_obj_add_int(doc, root, "status", s->status);
    yyjson_mut_obj_add_int(doc, root, "seq",
                           s->parts.empty() ? 0 : s->parts.back().seq);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "parts", arr);
    for (const part_slot &p : s->parts) {
        if (p.seq < since_seq) {
            continue;
        }
        yyjson_mut_val *obj = make_part(doc, view(p.part_start, p.part_end), p.kind,
                   view(p.id_start, p.id_end), view(p.message_start, p.message_end),
                   view(p.text_start, p.text_end), view(p.call_start, p.call_end),
                   view(p.tool_start, p.tool_end), view(p.state_start, p.state_end),
                   view(p.prompt_start, p.prompt_end),
                   view(p.tokens_start, p.tokens_end), view(p.time_start, p.time_end),
                   view(p.snapshot_start, p.snapshot_end),
                   view(p.reason_start, p.reason_end), p.cost);
        if (obj != nullptr) {
            yyjson_mut_arr_append(arr, obj);
        }
    }
    return json_write(doc, out_json);
}

bool SessionStore::part_json(uint32_t session, kimix::string_view part_id,
                             kimix::string &out_json) const noexcept {
    const session_slot *s = find(session);
    if (s == nullptr) {
        return false;
    }
    for (const part_slot &p : s->parts) {
        if (view(p.part_start, p.part_end) != part_id) {
            continue;
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
        if (doc == nullptr) {
            return false;
        }
        yyjson_mut_val *obj = make_part(doc, view(p.part_start, p.part_end), p.kind,
                                        view(p.id_start, p.id_end),
                                        view(p.message_start, p.message_end),
                                        view(p.text_start, p.text_end),
                                        view(p.call_start, p.call_end),
                                        view(p.tool_start, p.tool_end),
                                        view(p.state_start, p.state_end),
                                        view(p.prompt_start, p.prompt_end),
                                        view(p.tokens_start, p.tokens_end),
                                        view(p.time_start, p.time_end),
                                        view(p.snapshot_start, p.snapshot_end),
                                        view(p.reason_start, p.reason_end), p.cost);
        yyjson_mut_doc_set_root(doc, obj);
        return json_write(doc, out_json);
    }
    return false;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
