/*
 * kmb2.cpp - the packed message-structure boundary ABI ("KMB2").
 * See kmb2.h for the layout and the parity contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions. Every count and
 * span is validated before use, so malformed Python input degrades to a false
 * return instead of an out-of-bounds read.
 */

#include <core/kimix_core.h>

#include <runtime/common/le_bytes.h>
#include <runtime/soul/kmb2.h>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_kmb2 {

using kimix::runtime::common::le_span_ok;
using kimix::runtime::common::put_le16;
using kimix::runtime::common::put_le32;
using kimix::runtime::common::put_le8;
using kimix::runtime::common::put_le_i32;
using kimix::runtime::common::read_le16;
using kimix::runtime::common::read_le32;
using kimix::runtime::common::read_le8;
using kimix::runtime::common::read_le_i32;

inline size_t table_bytes(uint32_t msgs, uint32_t parts, uint32_t calls,
                          bool &ok) noexcept {
    const uint64_t total = static_cast<uint64_t>(kKmb2HeaderBytes) +
                           static_cast<uint64_t>(msgs) * kKmb2MessageBytes +
                           static_cast<uint64_t>(parts) * kKmb2PartBytes +
                           static_cast<uint64_t>(calls) * kKmb2CallBytes;
    ok = total <= static_cast<uint64_t>(SIZE_MAX);
    return ok ? static_cast<size_t>(total) : 0;
}

} // namespace src_runtime_soul_kmb2
using namespace src_runtime_soul_kmb2;

// ── kmb2_view ────────────────────────────────────────────────────────────────

bool kmb2_view::message(uint32_t index, kmb2_message_record &out) const noexcept {
    if (message_table == nullptr || index >= msg_count) {
        return false;
    }
    const char *p = message_table + static_cast<size_t>(index) * kKmb2MessageBytes;
    out.role = read_le8(p);
    out.flags = read_le8(p + 1);
    out.reserved = read_le16(p + 2);
    out.part_first = read_le32(p + 4);
    out.part_count = read_le32(p + 8);
    out.call_first = read_le32(p + 12);
    out.call_count = read_le32(p + 16);
    out.tcid_start = read_le_i32(p + 20);
    out.tcid_end = read_le_i32(p + 24);
    out.reserved2 = read_le32(p + 28);
    return true;
}

bool kmb2_view::part(uint32_t index, kmb2_part_record &out) const noexcept {
    if (part_table == nullptr || index >= part_total) {
        return false;
    }
    const char *p = part_table + static_cast<size_t>(index) * kKmb2PartBytes;
    out.start = read_le_i32(p);
    out.end = read_le_i32(p + 4);
    out.kind = read_le8(p + 8);
    out.flags = read_le8(p + 9);
    out.reserved = read_le16(p + 10);
    return true;
}

bool kmb2_view::call(uint32_t index, kmb2_call_record &out) const noexcept {
    if (call_table == nullptr || index >= call_total) {
        return false;
    }
    const char *p = call_table + static_cast<size_t>(index) * kKmb2CallBytes;
    out.id_s = read_le_i32(p);
    out.id_e = read_le_i32(p + 4);
    out.name_s = read_le_i32(p + 8);
    out.name_e = read_le_i32(p + 12);
    out.args_s = read_le_i32(p + 16);
    out.args_e = read_le_i32(p + 20);
    return true;
}

kimix::string_view kmb2_view::span(int32_t start, int32_t end) const noexcept {
    if (start < 0 || end < start) {
        return kimix::string_view();
    }
    return kimix::string_view(history.data() + start,
                              static_cast<size_t>(end - start));
}

// ── kmb2_parse ───────────────────────────────────────────────────────────────

bool kmb2_is_packed(kimix::string_view structure) noexcept {
    if (structure.size() < kKmb2HeaderBytes ||
        structure.data() == nullptr) {
        return false;
    }
    return read_le32(structure.data()) == kKmb2Magic &&
           read_le32(structure.data() + 4) == kKmb2Version;
}

bool kmb2_parse(kimix::string_view structure, kimix::string_view history,
                kmb2_view &out) noexcept {
    if (!kmb2_is_packed(structure)) {
        return false;
    }
    const char *h = structure.data();
    const uint32_t msg_count = read_le32(h + 8);
    const uint32_t part_total = read_le32(h + 12);
    const uint32_t call_total = read_le32(h + 16);
    const uint32_t flags = read_le32(h + 20);

    bool size_ok = false;
    const size_t required = table_bytes(msg_count, part_total, call_total, size_ok);
    if (!size_ok || structure.size() < required) {
        return false;
    }
    if (history.data() == nullptr && history.size() != 0) {
        return false;
    }

    kmb2_view v;
    v.structure = structure;
    v.history = history;
    v.message_table = h + kKmb2HeaderBytes;
    v.part_table = v.message_table + static_cast<size_t>(msg_count) * kKmb2MessageBytes;
    v.call_table = v.part_table + static_cast<size_t>(part_total) * kKmb2PartBytes;
    v.msg_count = msg_count;
    v.part_total = part_total;
    v.call_total = call_total;
    v.flags = flags;

    // Part table: every span must lie inside `history`, every kind must be a
    // known part_kind.
    for (uint32_t i = 0; i < part_total; ++i) {
        kmb2_part_record p;
        if (!v.part(i, p)) {
            return false;
        }
        if (!le_span_ok(p.start, p.end, history.size())) {
            return false;
        }
        if (p.kind > static_cast<uint8_t>(part_kind::OTHER)) {
            return false;
        }
    }

    // Call table: id/name are mandatory spans; arguments are optional.
    for (uint32_t i = 0; i < call_total; ++i) {
        kmb2_call_record c;
        if (!v.call(i, c)) {
            return false;
        }
        if (!le_span_ok(c.id_s, c.id_e, history.size()) ||
            !le_span_ok(c.name_s, c.name_e, history.size())) {
            return false;
        }
        if (c.args_s >= 0 && !le_span_ok(c.args_s, c.args_e, history.size())) {
            return false;
        }
    }

    // Message table: part/call ranges must fit their tables, tool_call_id must
    // be either fully absent (-1/-1) or a valid span.
    for (uint32_t i = 0; i < msg_count; ++i) {
        kmb2_message_record m;
        if (!v.message(i, m)) {
            return false;
        }
        if (m.role > kRoleTool) {
            return false;
        }
        if (static_cast<uint64_t>(m.part_first) + m.part_count > part_total) {
            return false;
        }
        if (static_cast<uint64_t>(m.call_first) + m.call_count > call_total) {
            return false;
        }
        const bool has_tcid = (m.flags & kKmb2MsgHasToolCallId) != 0;
        if (has_tcid) {
            if (!le_span_ok(m.tcid_start, m.tcid_end, history.size())) {
                return false;
            }
        } else if (m.tcid_start != -1 || m.tcid_end != -1) {
            return false;
        }
    }

    out.structure = structure;
    out.history = history;
    out.message_table = v.message_table;
    out.part_table = v.part_table;
    out.call_table = v.call_table;
    out.msg_count = msg_count;
    out.part_total = part_total;
    out.call_total = call_total;
    out.flags = flags;
    return true;
}

// ── kmb2_writer ──────────────────────────────────────────────────────────────

kmb2_writer::kmb2_writer(kimix::string &structure, kimix::string &history) noexcept
    : _structure(structure), _history(history) {}

int32_t kmb2_writer::put(kimix::string_view v) noexcept {
    if (v.data() == nullptr || v.empty()) {
        return _overflow ? -1 : static_cast<int32_t>(_history.size());
    }
    if (_history.size() + v.size() > static_cast<size_t>(INT32_MAX)) {
        _overflow = true;
        return -1;
    }
    const int32_t start = static_cast<int32_t>(_history.size());
    _history.append(v.data(), v.size());
    return start;
}

void kmb2_writer::add(uint8_t role, kimix::span<const part_view> parts,
                      kimix::span<const tool_call_view> calls,
                      kimix::string_view tool_call_id,
                      bool has_tool_call_id) noexcept {
    kmb2_message_record m{};
    m.role = role;
    m.part_first = _part_total;
    m.part_count = static_cast<uint32_t>(parts.size());
    m.call_first = _call_total;
    m.call_count = static_cast<uint32_t>(calls.size());
    m.tcid_start = -1;
    m.tcid_end = -1;

    if (has_tool_call_id) {
        const int32_t s = put(tool_call_id);
        if (s >= 0) {
            m.tcid_start = s;
            m.tcid_end = s + static_cast<int32_t>(tool_call_id.size());
            m.flags |= kKmb2MsgHasToolCallId;
        }
    }

    for (const part_view &p : parts) {
        kmb2_part_record rec{};
        const int32_t s = put(p.text);
        rec.start = s;
        rec.end = s >= 0 ? s + static_cast<int32_t>(p.text.size()) : -1;
        rec.kind = static_cast<uint8_t>(p.kind);
        if (p.kind == part_kind::THINK) {
            m.flags |= kKmb2MsgHasThink;
        }
        _parts.push_back(rec);
        ++_part_total;
    }

    if (!calls.empty()) {
        m.flags |= kKmb2MsgHasToolCalls;
    }
    for (const tool_call_view &c : calls) {
        kmb2_call_record rec{};
        rec.id_s = put(c.id);
        rec.id_e = rec.id_s >= 0 ? rec.id_s + static_cast<int32_t>(c.id.size()) : -1;
        rec.name_s = put(c.name);
        rec.name_e = rec.name_s >= 0 ? rec.name_s + static_cast<int32_t>(c.name.size()) : -1;
        if (c.arguments.data() == nullptr) {
            rec.args_s = -1;
            rec.args_e = -1;
        } else {
            rec.args_s = put(c.arguments);
            rec.args_e = rec.args_s >= 0
                             ? rec.args_s + static_cast<int32_t>(c.arguments.size())
                             : -1;
        }
        _calls.push_back(rec);
        ++_call_total;
    }

    _msgs.push_back(m);
    ++_msg_count;
}

void kmb2_writer::add_raw(uint8_t role, kimix::span<const kmb2_part_record> parts,
                          kimix::span<const kmb2_call_record> calls,
                          int32_t tcid_start, int32_t tcid_end) noexcept {
    kmb2_message_record m{};
    m.role = role;
    m.part_first = _part_total;
    m.part_count = static_cast<uint32_t>(parts.size());
    m.call_first = _call_total;
    m.call_count = static_cast<uint32_t>(calls.size());
    m.tcid_start = tcid_start;
    m.tcid_end = tcid_end;
    if (tcid_start >= 0) {
        m.flags |= kKmb2MsgHasToolCallId;
    }
    if (!calls.empty()) {
        m.flags |= kKmb2MsgHasToolCalls;
    }
    for (const kmb2_part_record &p : parts) {
        if (p.kind == static_cast<uint8_t>(part_kind::THINK)) {
            m.flags |= kKmb2MsgHasThink;
        }
        _parts.push_back(p);
        ++_part_total;
    }
    for (const kmb2_call_record &c : calls) {
        _calls.push_back(c);
        ++_call_total;
    }
    _msgs.push_back(m);
    ++_msg_count;
}

void kmb2_writer::finish() noexcept {
    bool size_ok = false;
    const size_t total = table_bytes(static_cast<uint32_t>(_msgs.size()),
                                     static_cast<uint32_t>(_parts.size()),
                                     static_cast<uint32_t>(_calls.size()),
                                     size_ok);
    if (!size_ok) {
        _structure.clear();
        return;
    }
    _structure.resize(total);
    char *h = _structure.data();
    put_le32(h, kKmb2Magic);
    put_le32(h + 4, kKmb2Version);
    put_le32(h + 8, static_cast<uint32_t>(_msgs.size()));
    put_le32(h + 12, static_cast<uint32_t>(_parts.size()));
    put_le32(h + 16, static_cast<uint32_t>(_calls.size()));
    put_le32(h + 20, kKmb2FlagUtf8);
    put_le32(h + 24, 0);
    put_le32(h + 28, 0);

    char *mp = h + kKmb2HeaderBytes;
    for (const kmb2_message_record &m : _msgs) {
        put_le8(mp, m.role);
        put_le8(mp + 1, m.flags);
        put_le16(mp + 2, 0);
        put_le32(mp + 4, m.part_first);
        put_le32(mp + 8, m.part_count);
        put_le32(mp + 12, m.call_first);
        put_le32(mp + 16, m.call_count);
        put_le_i32(mp + 20, m.tcid_start);
        put_le_i32(mp + 24, m.tcid_end);
        put_le32(mp + 28, 0);
        mp += kKmb2MessageBytes;
    }

    char *pp = mp;
    for (const kmb2_part_record &p : _parts) {
        put_le_i32(pp, p.start);
        put_le_i32(pp + 4, p.end);
        put_le8(pp + 8, p.kind);
        put_le8(pp + 9, p.flags);
        put_le16(pp + 10, 0);
        pp += kKmb2PartBytes;
    }

    char *cp = pp;
    for (const kmb2_call_record &c : _calls) {
        put_le_i32(cp, c.id_s);
        put_le_i32(cp + 4, c.id_e);
        put_le_i32(cp + 8, c.name_s);
        put_le_i32(cp + 12, c.name_e);
        put_le_i32(cp + 16, c.args_s);
        put_le_i32(cp + 20, c.args_e);
        cp += kKmb2CallBytes;
    }
}

// ── builders ─────────────────────────────────────────────────────────────────

void kmb2_encode_message(kmb2_writer &w, const message_view &mv) noexcept {
    w.add(mv.role, mv.parts, mv.tool_calls, mv.tool_call_id,
          mv.tool_call_id.data() != nullptr);
}

void kmb2_build(kimix::span<const message_view> msgs, kimix::string &structure,
                kimix::string &history) noexcept {
    structure.clear();
    history.clear();
    kmb2_writer w(structure, history);
    for (const message_view &mv : msgs) {
        kmb2_encode_message(w, mv);
    }
    w.finish();
}

} // namespace soul
} // namespace runtime
} // namespace kimix
