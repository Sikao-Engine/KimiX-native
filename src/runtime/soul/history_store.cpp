/*
 * history_store.cpp - the native conversation-history arena.
 * See history_store.h for the ownership model and the API contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions. Every span that
 * comes from a KMB2 blob is validated by kmb2_parse() before it is copied, and
 * the "KMSH1" loader validates every range before it commits, so a malformed
 * blob never corrupts a live store.
 */

#include <core/kimix_core.h>

#include <runtime/common/blob_util.h>
#include <runtime/common/le_bytes.h>
#include <runtime/soul/history_store.h>

#include <cstdint>
#include <cstring>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_history_store {

using kimix::runtime::common::le_span_ok;
using kimix::runtime::common::put_le16;
using kimix::runtime::common::put_le32;
using kimix::runtime::common::put_le8;
using kimix::runtime::common::put_le_i32;
using kimix::runtime::common::read_le32;
using kimix::runtime::common::read_le_i32;

constexpr char kSaveMagic[8] = {'K', 'M', 'S', 'H', '1', 0, 0, 0};
constexpr size_t kSaveHeaderBytes = 32;
constexpr size_t kSaveSlotBytes = 32;

using kimix::runtime::common::blob_capacity_bytes;

} // namespace src_runtime_soul_history_store
using namespace src_runtime_soul_history_store;

// ── construction ─────────────────────────────────────────────────────────────

HistoryStore::HistoryStore(uint64_t reserve_bytes) {
    if (reserve_bytes > 0 && reserve_bytes <= static_cast<uint64_t>(SIZE_MAX)) {
        _arena.reserve(static_cast<size_t>(reserve_bytes));
    }
}

// ── mutation ─────────────────────────────────────────────────────────────────

int32_t HistoryStore::append_bytes(kimix::string_view v, bool *ok) noexcept {
    if (v.data() == nullptr) {
        return -1; // absent (None), not "empty"
    }
    if (_arena.size() + v.size() > static_cast<size_t>(INT32_MAX)) {
        *ok = false;
        return -1;
    }
    const int32_t start = static_cast<int32_t>(_arena.size());
    _arena.append(v.data(), v.size());
    return start;
}

bool HistoryStore::write_message(uint32_t slot, uint8_t role,
                                 kimix::span<const part_view> parts,
                                 kimix::span<const tool_call_view> calls,
                                 kimix::string_view tool_call_id) noexcept {
    if (slot >= _slots.size()) {
        return false;
    }
    bool ok = true;
    kmb2_message_record rec{};
    rec.role = role;
    rec.part_first = static_cast<uint32_t>(_parts.size());
    rec.part_count = static_cast<uint32_t>(parts.size());
    rec.call_first = static_cast<uint32_t>(_calls.size());
    rec.call_count = static_cast<uint32_t>(calls.size());
    rec.tcid_start = -1;
    rec.tcid_end = -1;

    if (tool_call_id.data() != nullptr) {
        const int32_t s = append_bytes(tool_call_id, &ok);
        if (ok) {
            rec.tcid_start = s;
            rec.tcid_end = s + static_cast<int32_t>(tool_call_id.size());
            rec.flags |= kKmb2MsgHasToolCallId;
        }
    }
    for (const part_view &p : parts) {
        kmb2_part_record pr{};
        const int32_t s = append_bytes(p.text, &ok);
        if (!ok) {
            break;
        }
        pr.start = s;
        pr.end = s >= 0 ? s + static_cast<int32_t>(p.text.size()) : -1;
        if (s < 0) {
            // A part with an absent span is stored as an empty span, never as
            // a hole: KMB2 rejects a negative part span.
            pr.start = static_cast<int32_t>(_arena.size());
            pr.end = pr.start;
        }
        pr.kind = static_cast<uint8_t>(p.kind);
        if (p.kind == part_kind::THINK) {
            rec.flags |= kKmb2MsgHasThink;
        }
        _parts.push_back(pr);
    }
    if (!ok) {
        return false;
    }
    if (!calls.empty()) {
        rec.flags |= kKmb2MsgHasToolCalls;
    }
    for (const tool_call_view &c : calls) {
        kmb2_call_record cr{};
        cr.id_s = append_bytes(c.id, &ok);
        cr.id_e = cr.id_s >= 0 ? cr.id_s + static_cast<int32_t>(c.id.size()) : 0;
        if (!ok) {
            break;
        }
        if (cr.id_s < 0) {
            cr.id_s = static_cast<int32_t>(_arena.size());
            cr.id_e = cr.id_s;
        }
        cr.name_s = append_bytes(c.name, &ok);
        cr.name_e = cr.name_s >= 0 ? cr.name_s + static_cast<int32_t>(c.name.size()) : 0;
        if (!ok) {
            break;
        }
        if (cr.name_s < 0) {
            cr.name_s = static_cast<int32_t>(_arena.size());
            cr.name_e = cr.name_s;
        }
        if (c.arguments.data() == nullptr) {
            cr.args_s = -1;
            cr.args_e = -1;
        } else {
            cr.args_s = append_bytes(c.arguments, &ok);
            if (!ok) {
                break;
            }
            cr.args_e = cr.args_s + static_cast<int32_t>(c.arguments.size());
        }
        _calls.push_back(cr);
    }
    if (!ok) {
        return false;
    }
    _slots[slot].rec = rec;
    _slots[slot].live = 1;
    return true;
}

uint32_t HistoryStore::push_slot(uint8_t role, kimix::span<const part_view> parts,
                                 kimix::span<const tool_call_view> calls,
                                 kimix::string_view tool_call_id) noexcept {
    const uint32_t slot = static_cast<uint32_t>(_slots.size());
    slot_state st{};
    st.rec.role = role;
    _slots.push_back(st);
    if (!write_message(slot, role, parts, calls, tool_call_id)) {
        return UINT32_MAX;
    }
    _order.push_back(slot);
    invalidate();
    return static_cast<uint32_t>(_order.size() - 1);
}

uint32_t HistoryStore::append(uint8_t role, kimix::span<const part_view> parts,
                              kimix::span<const tool_call_view> calls,
                              kimix::string_view tool_call_id) noexcept {
    return push_slot(role, parts, calls, tool_call_id);
}

uint32_t HistoryStore::append_packed(kimix::string_view structure,
                                     kimix::string_view history) noexcept {
    kmb2_view v;
    if (!kmb2_parse(structure, history, v)) {
        return 0;
    }
    // Parse every message first (validate + size the scratch), then commit, so
    // a malformed blob never leaves half a conversation behind.
    const uint32_t before = size();
    for (uint32_t i = 0; i < v.msg_count; ++i) {
        kmb2_message_record m;
        if (!v.message(i, m)) {
            return 0;
        }
        _scratch_parts.clear();
        _scratch_calls.clear();
        for (uint32_t j = 0; j < m.part_count; ++j) {
            kmb2_part_record pr;
            if (!v.part(m.part_first + j, pr)) {
                return 0;
            }
            part_view pv;
            pv.kind = static_cast<part_kind>(pr.kind);
            pv.text = v.span(pr.start, pr.end);
            _scratch_parts.push_back(pv);
        }
        for (uint32_t j = 0; j < m.call_count; ++j) {
            kmb2_call_record cr;
            if (!v.call(m.call_first + j, cr)) {
                return 0;
            }
            tool_call_view tc;
            tc.id = v.span(cr.id_s, cr.id_e);
            tc.name = v.span(cr.name_s, cr.name_e);
            if (cr.args_s >= 0) {
                tc.arguments = v.span(cr.args_s, cr.args_e);
            }
            _scratch_calls.push_back(tc);
        }
        kimix::string_view tcid;
        if ((m.flags & kKmb2MsgHasToolCallId) != 0) {
            tcid = v.span(m.tcid_start, m.tcid_end);
        }
        if (push_slot(m.role, _scratch_parts, _scratch_calls, tcid) == UINT32_MAX) {
            return size() - before;
        }
    }
    return size() - before;
}

bool HistoryStore::replace(uint32_t index, uint8_t role,
                           kimix::span<const part_view> parts,
                           kimix::span<const tool_call_view> calls,
                           kimix::string_view tool_call_id) noexcept {
    if (index >= _order.size()) {
        return false;
    }
    const uint32_t slot = _order[index];
    if (!write_message(slot, role, parts, calls, tool_call_id)) {
        return false;
    }
    invalidate();
    return true;
}

bool HistoryStore::replace_packed(uint32_t index, kimix::string_view structure,
                                  kimix::string_view history) noexcept {
    kmb2_view v;
    if (!kmb2_parse(structure, history, v) || v.msg_count == 0) {
        return false;
    }
    kmb2_message_record m;
    if (!v.message(0, m)) {
        return false;
    }
    _scratch_parts.clear();
    _scratch_calls.clear();
    for (uint32_t j = 0; j < m.part_count; ++j) {
        kmb2_part_record pr;
        if (!v.part(m.part_first + j, pr)) {
            return false;
        }
        part_view pv;
        pv.kind = static_cast<part_kind>(pr.kind);
        pv.text = v.span(pr.start, pr.end);
        _scratch_parts.push_back(pv);
    }
    for (uint32_t j = 0; j < m.call_count; ++j) {
        kmb2_call_record cr;
        if (!v.call(m.call_first + j, cr)) {
            return false;
        }
        tool_call_view tc;
        tc.id = v.span(cr.id_s, cr.id_e);
        tc.name = v.span(cr.name_s, cr.name_e);
        if (cr.args_s >= 0) {
            tc.arguments = v.span(cr.args_s, cr.args_e);
        }
        _scratch_calls.push_back(tc);
    }
    kimix::string_view tcid;
    if ((m.flags & kKmb2MsgHasToolCallId) != 0) {
        tcid = v.span(m.tcid_start, m.tcid_end);
    }
    return replace(index, m.role, _scratch_parts, _scratch_calls, tcid);
}

bool HistoryStore::drop(uint32_t index) noexcept {
    if (index >= _order.size()) {
        return false;
    }
    _slots[_order[index]].live = 0;
    _order.erase(_order.begin() + static_cast<ptrdiff_t>(index));
    invalidate();
    return true;
}

void HistoryStore::truncate(uint32_t count) noexcept {
    if (count >= _order.size()) {
        return;
    }
    for (size_t i = count; i < _order.size(); ++i) {
        _slots[_order[i]].live = 0;
    }
    _order.resize(count);
    invalidate();
}

void HistoryStore::clear() noexcept {
    if (_slots.empty() && _arena.empty() && _parts.empty() && _calls.empty()) {
        invalidate();
        return;
    }
    _arena.clear();
    _slots.clear();
    _parts.clear();
    _calls.clear();
    _order.clear();
    invalidate();
}

uint64_t HistoryStore::compact() noexcept {
    const uint64_t before = bytes_owned();

    kimix::string arena;
    arena.reserve(static_cast<size_t>(bytes_live()));
    kimix::vector<slot_state> slots;
    slots.reserve(_order.size());
    kimix::vector<kmb2_part_record> parts;
    parts.reserve(_parts.size());
    kimix::vector<kmb2_call_record> calls;
    calls.reserve(_calls.size());

    for (size_t ordinal = 0; ordinal < _order.size(); ++ordinal) {
        const slot_state &src = _slots[_order[ordinal]];
        slot_state dst{};
        dst.rec = src.rec;
        dst.live = 1;
        dst.rec.part_first = static_cast<uint32_t>(parts.size());
        dst.rec.call_first = static_cast<uint32_t>(calls.size());

        if (dst.rec.tcid_start >= 0) {
            const size_t len = static_cast<size_t>(dst.rec.tcid_end - dst.rec.tcid_start);
            dst.rec.tcid_start = static_cast<int32_t>(arena.size());
            arena.append(_arena.data() + src.rec.tcid_start, len);
            dst.rec.tcid_end = static_cast<int32_t>(arena.size());
        }
        for (uint32_t j = 0; j < src.rec.part_count; ++j) {
            kmb2_part_record pr = _parts[src.rec.part_first + j];
            const size_t len = static_cast<size_t>(pr.end - pr.start);
            pr.start = static_cast<int32_t>(arena.size());
            arena.append(_arena.data() + _parts[src.rec.part_first + j].start, len);
            pr.end = static_cast<int32_t>(arena.size());
            parts.push_back(pr);
        }
        for (uint32_t j = 0; j < src.rec.call_count; ++j) {
            kmb2_call_record cr = _calls[src.rec.call_first + j];
            const size_t id_len = static_cast<size_t>(cr.id_e - cr.id_s);
            const size_t name_len = static_cast<size_t>(cr.name_e - cr.name_s);
            cr.id_s = static_cast<int32_t>(arena.size());
            arena.append(_arena.data() + _calls[src.rec.call_first + j].id_s, id_len);
            cr.id_e = static_cast<int32_t>(arena.size());
            cr.name_s = static_cast<int32_t>(arena.size());
            arena.append(_arena.data() + _calls[src.rec.call_first + j].name_s, name_len);
            cr.name_e = static_cast<int32_t>(arena.size());
            if (cr.args_s >= 0) {
                const size_t args_len = static_cast<size_t>(cr.args_e - cr.args_s);
                cr.args_s = static_cast<int32_t>(arena.size());
                arena.append(_arena.data() + _calls[src.rec.call_first + j].args_s,
                             args_len);
                cr.args_e = static_cast<int32_t>(arena.size());
            }
            calls.push_back(cr);
        }
        slots.push_back(dst);
    }

    _arena = std::move(arena);
    _slots = std::move(slots);
    _parts = std::move(parts);
    _calls = std::move(calls);
    _order.clear();
    _order.reserve(_slots.size());
    for (size_t i = 0; i < _slots.size(); ++i) {
        _order.push_back(static_cast<uint32_t>(i));
    }
    invalidate();

    const uint64_t after = bytes_owned();
    return before > after ? before - after : 0;
}

// ── read ─────────────────────────────────────────────────────────────────────

void HistoryStore::build_views() const noexcept {
    _views.clear();
    _view_parts.clear();
    _view_calls.clear();
    // Reserve the FULL table sizes: every message_view below holds spans into
    // these two vectors, so they must never reallocate afterwards.
    _views.reserve(_order.size());
    _view_parts.reserve(_parts.size());
    _view_calls.reserve(_calls.size());

    for (uint32_t slot_index : _order) {
        const kmb2_message_record &rec = _slots[slot_index].rec;
        message_view mv;
        mv.role = rec.role;
        const uint32_t part_first = static_cast<uint32_t>(_view_parts.size());
        for (uint32_t j = 0; j < rec.part_count; ++j) {
            const kmb2_part_record &pr = _parts[rec.part_first + j];
            part_view pv;
            pv.kind = static_cast<part_kind>(pr.kind);
            pv.text = kimix::string_view(_arena.data() + pr.start,
                                         static_cast<size_t>(pr.end - pr.start));
            _view_parts.push_back(pv);
        }
        const uint32_t call_first = static_cast<uint32_t>(_view_calls.size());
        for (uint32_t j = 0; j < rec.call_count; ++j) {
            const kmb2_call_record &cr = _calls[rec.call_first + j];
            tool_call_view tc;
            tc.id = kimix::string_view(_arena.data() + cr.id_s,
                                       static_cast<size_t>(cr.id_e - cr.id_s));
            tc.name = kimix::string_view(_arena.data() + cr.name_s,
                                         static_cast<size_t>(cr.name_e - cr.name_s));
            if (cr.args_s >= 0) {
                tc.arguments = kimix::string_view(
                    _arena.data() + cr.args_s,
                    static_cast<size_t>(cr.args_e - cr.args_s));
            }
            _view_calls.push_back(tc);
        }
        mv.parts = kimix::span<const part_view>(_view_parts.data() + part_first,
                                               rec.part_count);
        mv.tool_calls = kimix::span<const tool_call_view>(_view_calls.data() + call_first,
                                                         rec.call_count);
        if (rec.tcid_start >= 0) {
            mv.tool_call_id = kimix::string_view(
                _arena.data() + rec.tcid_start,
                static_cast<size_t>(rec.tcid_end - rec.tcid_start));
        }
        _views.push_back(mv);
    }
    _views_gen = _gen;
}

kimix::span<const message_view> HistoryStore::views() const noexcept {
    if (_views_gen != _gen) {
        build_views();
    }
    return kimix::span<const message_view>(_views.data(), _views.size());
}

bool HistoryStore::view(uint32_t index, message_view &out) const noexcept {
    if (_views_gen != _gen) {
        build_views();
    }
    if (index >= _views.size()) {
        return false;
    }
    out = _views[index];
    return true;
}

// ── export / persistence ─────────────────────────────────────────────────────

void HistoryStore::pack(uint32_t first, uint32_t count, kimix::string &structure,
                        kimix::string &history) const noexcept {
    const kimix::span<const message_view> all = views();
    if (first >= all.size()) {
        kmb2_build(kimix::span<const message_view>(), structure, history);
        return;
    }
    uint64_t avail = all.size() - first;
    uint64_t take = count;
    if (take > avail) {
        take = avail;
    }
    kmb2_build(all.subspan(first, static_cast<size_t>(take)), structure, history);
}

void HistoryStore::text_of(uint32_t index, kimix::string &out) const noexcept {
    out.clear();
    message_view mv;
    if (!view(index, mv)) {
        return;
    }
    bool first = true;
    for (const part_view &p : mv.parts) {
        if (p.kind != part_kind::TEXT) {
            continue;
        }
        if (!first) {
            out.push_back('\n');
        }
        out.append(p.text.data(), p.text.size());
        first = false;
    }
}

void HistoryStore::save(kimix::string &blob) const noexcept {
    const size_t slot_count = _slots.size();
    const size_t total = kSaveHeaderBytes + _arena.size() +
                         slot_count * kSaveSlotBytes + _parts.size() * kKmb2PartBytes +
                         _calls.size() * kKmb2CallBytes;
    blob.resize(total);
    char *p = blob.data();
    // header
    for (size_t i = 0; i < 8; ++i) {
        put_le8(p + i, static_cast<uint8_t>(kSaveMagic[i]));
    }
    put_le32(p + 8, size());
    put_le32(p + 12, static_cast<uint32_t>(slot_count));
    put_le32(p + 16, static_cast<uint32_t>(_arena.size()));
    put_le32(p + 20, static_cast<uint32_t>(_parts.size()));
    put_le32(p + 24, static_cast<uint32_t>(_calls.size()));
    put_le32(p + 28, 0);
    p += kSaveHeaderBytes;

    if (!_arena.empty()) {
        memcpy(p, _arena.data(), _arena.size());
    }
    p += _arena.size();

    for (const slot_state &st : _slots) {
        put_le8(p, st.rec.role);
        put_le8(p + 1, st.live);
        put_le8(p + 2, st.rec.flags);
        put_le8(p + 3, 0);
        put_le32(p + 4, st.rec.part_first);
        put_le32(p + 8, st.rec.part_count);
        put_le32(p + 12, st.rec.call_first);
        put_le32(p + 16, st.rec.call_count);
        put_le_i32(p + 20, st.rec.tcid_start);
        put_le_i32(p + 24, st.rec.tcid_end);
        put_le32(p + 28, 0);
        p += kSaveSlotBytes;
    }
    for (const kmb2_part_record &pr : _parts) {
        put_le_i32(p, pr.start);
        put_le_i32(p + 4, pr.end);
        put_le8(p + 8, pr.kind);
        put_le8(p + 9, pr.flags);
        put_le16(p + 10, 0);
        p += kKmb2PartBytes;
    }
    for (const kmb2_call_record &cr : _calls) {
        put_le_i32(p, cr.id_s);
        put_le_i32(p + 4, cr.id_e);
        put_le_i32(p + 8, cr.name_s);
        put_le_i32(p + 12, cr.name_e);
        put_le_i32(p + 16, cr.args_s);
        put_le_i32(p + 20, cr.args_e);
        p += kKmb2CallBytes;
    }
}

bool HistoryStore::load(kimix::string_view blob) noexcept {
    if (blob.size() < kSaveHeaderBytes) {
        return false;
    }
    const char *h = blob.data();
    for (size_t i = 0; i < 7; ++i) {
        if (static_cast<uint8_t>(h[i]) != static_cast<uint8_t>(kSaveMagic[i])) {
            return false;
        }
    }
    const uint32_t live_count = read_le32(h + 8);
    const uint32_t slot_count = read_le32(h + 12);
    const uint32_t arena_len = read_le32(h + 16);
    const uint32_t part_total = read_le32(h + 20);
    const uint32_t call_total = read_le32(h + 24);

    const uint64_t required = static_cast<uint64_t>(kSaveHeaderBytes) + arena_len +
                              static_cast<uint64_t>(slot_count) * kSaveSlotBytes +
                              static_cast<uint64_t>(part_total) * kKmb2PartBytes +
                              static_cast<uint64_t>(call_total) * kKmb2CallBytes;
    if (required > blob.size()) {
        return false;
    }

    kimix::string arena;
    kimix::vector<slot_state> slots;
    kimix::vector<kmb2_part_record> parts;
    kimix::vector<kmb2_call_record> calls;
    kimix::vector<uint32_t> order;

    const char *p = h + kSaveHeaderBytes;
    if (arena_len > 0) {
        arena.assign(p, arena_len);
    }
    p += arena_len;

    slots.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        slot_state st{};
        st.rec.role = static_cast<uint8_t>(p[0]);
        st.live = static_cast<uint8_t>(p[1]);
        st.rec.flags = static_cast<uint8_t>(p[2]);
        st.rec.part_first = read_le32(p + 4);
        st.rec.part_count = read_le32(p + 8);
        st.rec.call_first = read_le32(p + 12);
        st.rec.call_count = read_le32(p + 16);
        st.rec.tcid_start = read_le_i32(p + 20);
        st.rec.tcid_end = read_le_i32(p + 24);
        if (st.rec.role > kRoleTool) {
            return false;
        }
        if (static_cast<uint64_t>(st.rec.part_first) + st.rec.part_count > part_total) {
            return false;
        }
        if (static_cast<uint64_t>(st.rec.call_first) + st.rec.call_count > call_total) {
            return false;
        }
        if (st.rec.tcid_start >= 0) {
            if (!le_span_ok(st.rec.tcid_start, st.rec.tcid_end, arena.size())) {
                return false;
            }
        } else if (st.rec.tcid_start != -1 || st.rec.tcid_end != -1) {
            return false;
        }
        p += kSaveSlotBytes;
        if (st.live != 0) {
            order.push_back(i);
        }
        slots.push_back(st);
    }

    parts.reserve(part_total);
    for (uint32_t i = 0; i < part_total; ++i) {
        kmb2_part_record pr{};
        pr.start = read_le_i32(p);
        pr.end = read_le_i32(p + 4);
        pr.kind = static_cast<uint8_t>(p[8]);
        pr.flags = static_cast<uint8_t>(p[9]);
        if (pr.kind > static_cast<uint8_t>(part_kind::OTHER) ||
            !le_span_ok(pr.start, pr.end, arena.size())) {
            return false;
        }
        parts.push_back(pr);
        p += kKmb2PartBytes;
    }

    calls.reserve(call_total);
    for (uint32_t i = 0; i < call_total; ++i) {
        kmb2_call_record cr{};
        cr.id_s = read_le_i32(p);
        cr.id_e = read_le_i32(p + 4);
        cr.name_s = read_le_i32(p + 8);
        cr.name_e = read_le_i32(p + 12);
        cr.args_s = read_le_i32(p + 16);
        cr.args_e = read_le_i32(p + 20);
        if (!le_span_ok(cr.id_s, cr.id_e, arena.size()) ||
            !le_span_ok(cr.name_s, cr.name_e, arena.size())) {
            return false;
        }
        if (cr.args_s >= 0 && !le_span_ok(cr.args_s, cr.args_e, arena.size())) {
            return false;
        }
        calls.push_back(cr);
        p += kKmb2CallBytes;
    }

    if (order.size() != live_count) {
        return false;
    }

    _arena = std::move(arena);
    _slots = std::move(slots);
    _parts = std::move(parts);
    _calls = std::move(calls);
    _order = std::move(order);
    invalidate();
    return true;
}

uint64_t HistoryStore::bytes_owned() const noexcept {
    return blob_capacity_bytes(_arena) + blob_capacity_bytes(_slots) + blob_capacity_bytes(_parts) +
           blob_capacity_bytes(_calls) + blob_capacity_bytes(_order) +
           blob_capacity_bytes(_view_parts) + blob_capacity_bytes(_view_calls) +
           blob_capacity_bytes(_views);
}

uint64_t HistoryStore::bytes_live() const noexcept {
    uint64_t total = 0;
    for (uint32_t slot_index : _order) {
        const kmb2_message_record &rec = _slots[slot_index].rec;
        if (rec.tcid_start >= 0) {
            total += static_cast<uint64_t>(rec.tcid_end - rec.tcid_start);
        }
        for (uint32_t j = 0; j < rec.part_count; ++j) {
            const kmb2_part_record &pr = _parts[rec.part_first + j];
            total += static_cast<uint64_t>(pr.end - pr.start);
        }
        for (uint32_t j = 0; j < rec.call_count; ++j) {
            const kmb2_call_record &cr = _calls[rec.call_first + j];
            total += static_cast<uint64_t>(cr.id_e - cr.id_s);
            total += static_cast<uint64_t>(cr.name_e - cr.name_s);
            if (cr.args_s >= 0) {
                total += static_cast<uint64_t>(cr.args_e - cr.args_s);
            }
        }
    }
    return total;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
