// Test for src/runtime/soul/kmb2.{h,cpp} — the packed message-structure ABI
// every soul kernel parses (report §2, the "bytes + span tables" boundary).
//
// Covers:
// - encode -> decode round trip incl. UTF-8, think parts, tool calls, a None
//   tool_call_id vs an EMPTY one, and an absent vs empty arguments string;
// - byte layout invariants (header/tables sizes, counts, flags);
// - the malformed-blob rejection matrix (bad magic/version, truncated tables,
//   out-of-range spans, unknown part kind, half-present tool_call_id,
//   part/call range overflow) — every case must return false, never read out
//   of bounds;
// - packed -> assemble() -> message_view equality with a hand-built view set
//   (the guarantee the Python side relies on: both decoders converge).
//
// No Python: the suite links runtime_py (which re-exports the runtime kernels)
// and drives the kernel API directly.

#include "ut/ut.hpp"

#include <runtime/common/le_bytes.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::soul::kmb2_call_record;
using kimix::runtime::soul::kmb2_message_record;
using kimix::runtime::soul::kmb2_part_record;
using kimix::runtime::soul::kmb2_view;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

namespace tests_unit_native_test_soul_bridge {

// A small hand-built conversation:
//   0 user    "hello"                        (UTF-8 + a think part)
//   1 assistant text + think + tool call "c1"/read/{"path":"a"}
//   2 tool    "file body" tool_call_id "c1"
//   3 user    "" (an EMPTY text part)
struct fixture {
    kimix::vector<message_view> msgs;
    kimix::vector<kimix::vector<part_view>> parts;
    kimix::vector<kimix::vector<tool_call_view>> calls;
    kimix::vector<kimix::string> tcid;
    // Only messages 2 and 3 carry a tool_call_id (2 = "c1", 3 = "" present but
    // EMPTY). A default-constructed kimix::string still has a non-null data(),
    // so presence must be tracked explicitly.
    bool has_tcid[4] = {false, false, true, true};

    fixture() {
        parts.resize(4);
        calls.resize(4);
        tcid.resize(4);

        parts[0].push_back(part_view{part_kind::TEXT, "hello \xE4\xB8\x96\xE7\x95\x8C"});
        parts[0].push_back(part_view{part_kind::THINK, "pondering"});

        parts[1].push_back(part_view{part_kind::TEXT, "answer"});
        parts[1].push_back(part_view{part_kind::THINK, "reasoning"});
        calls[1].push_back(tool_call_view{"c1", "read", "{\"path\":\"a\"}"});
        calls[1].push_back(tool_call_view{"c2", "glob", kimix::string_view()});

        parts[2].push_back(part_view{part_kind::TEXT, "file body"});
        tcid[2] = "c1";

        parts[3].push_back(part_view{part_kind::TEXT, ""});
        tcid[3] = ""; // present but empty

        msgs.resize(4);
        msgs[0].role = kimix::runtime::soul::kRoleUser;
        msgs[1].role = kimix::runtime::soul::kRoleAssistant;
        msgs[2].role = kimix::runtime::soul::kRoleTool;
        msgs[3].role = kimix::runtime::soul::kRoleUser;
        for (size_t i = 0; i < 4; ++i) {
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
            msgs[i].tool_calls =
                kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
            if (has_tcid[i]) {
                msgs[i].tool_call_id = kimix::string_view(tcid[i]);
            }
        }
    }
};

bool same_view(const message_view& a, const message_view& b) {
    if (a.role != b.role || a.parts.size() != b.parts.size() ||
        a.tool_calls.size() != b.tool_calls.size() ||
        a.tool_call_id != b.tool_call_id) {
        return false;
    }
    for (size_t i = 0; i < a.parts.size(); ++i) {
        if (a.parts[i].kind != b.parts[i].kind || a.parts[i].text != b.parts[i].text) {
            return false;
        }
    }
    for (size_t i = 0; i < a.tool_calls.size(); ++i) {
        if (a.tool_calls[i].id != b.tool_calls[i].id ||
            a.tool_calls[i].name != b.tool_calls[i].name ||
            a.tool_calls[i].arguments != b.tool_calls[i].arguments) {
            return false;
        }
    }
    return true;
}

// Rebuild message_view spans from a decoded blob (what py_soul_bridge does for
// Python; the kernel itself only hands out records).
kimix::vector<message_view> assemble(const kmb2_view& v,
                                     kimix::vector<part_view>& parts_out,
                                     kimix::vector<tool_call_view>& calls_out) {
    kimix::vector<message_view> msgs;
    msgs.resize(v.msg_count);
    for (uint32_t i = 0; i < v.msg_count; ++i) {
        kmb2_message_record m;
        v.message(i, m);
        const uint32_t part_first = static_cast<uint32_t>(parts_out.size());
        for (uint32_t j = 0; j < m.part_count; ++j) {
            kmb2_part_record pr;
            v.part(m.part_first + j, pr);
            part_view pv;
            pv.kind = static_cast<part_kind>(pr.kind);
            pv.text = v.span(pr.start, pr.end);
            parts_out.push_back(pv);
        }
        const uint32_t call_first = static_cast<uint32_t>(calls_out.size());
        for (uint32_t j = 0; j < m.call_count; ++j) {
            kmb2_call_record cr;
            v.call(m.call_first + j, cr);
            tool_call_view tc;
            tc.id = v.span(cr.id_s, cr.id_e);
            tc.name = v.span(cr.name_s, cr.name_e);
            if (cr.args_s >= 0) {
                tc.arguments = v.span(cr.args_s, cr.args_e);
            }
            calls_out.push_back(tc);
        }
        message_view mv;
        mv.role = m.role;
        mv.parts = kimix::span<const part_view>(parts_out.data() + part_first,
                                               m.part_count);
        mv.tool_calls = kimix::span<const tool_call_view>(calls_out.data() + call_first,
                                                         m.call_count);
        if ((m.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) != 0) {
            mv.tool_call_id = v.span(m.tcid_start, m.tcid_end);
        }
        msgs[i] = mv;
    }
    return msgs;
}

} // namespace tests_unit_native_test_soul_bridge
using namespace tests_unit_native_test_soul_bridge;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "kmb2_layout_constants"_test = [] {
        expect(sizeof(kmb2_message_record) == 32u);
        expect(sizeof(kmb2_part_record) == 12u);
        expect(sizeof(kmb2_call_record) == 24u);
        expect(kimix::runtime::soul::kKmb2HeaderBytes == 32u);
    };

    "kmb2_roundtrip"_test = [] {
        fixture f;
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(f.msgs, structure, history);

        // header
        expect(structure.size() == 32u + 4 * 32u + 6 * 12u + 2 * 24u);
        expect(kimix::runtime::soul::kmb2_is_packed(structure));
        expect(std::memcmp(structure.data(), "KMB2", 4) == 0);
        expect(kimix::runtime::common::read_le32(structure.data() + 8) == 4u);
        expect(kimix::runtime::common::read_le32(structure.data() + 12) == 6u);
        expect(kimix::runtime::common::read_le32(structure.data() + 16) == 2u);

        kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 4u);

        kimix::vector<part_view> parts;
        kimix::vector<tool_call_view> calls;
        // Reserve the full table sizes: the assembled spans point into these
        // vectors, so they must never reallocate.
        parts.reserve(view.part_total);
        calls.reserve(view.call_total);
        kimix::vector<message_view> decoded = assemble(view, parts, calls);
        expect(decoded.size() == f.msgs.size());
        for (size_t i = 0; i < decoded.size(); ++i) {
            expect(same_view(decoded[i], f.msgs[i])) << "message " << i;
        }
                // Absent vs empty tool_call_id survive as different states.
        expect(decoded[0].tool_call_id.data() == nullptr);
        expect(decoded[1].tool_call_id.data() == nullptr);
        expect(decoded[3].tool_call_id.data() != nullptr);
        expect(decoded[3].tool_call_id.empty());
        // Absent arguments stay absent.
        expect(decoded[1].tool_calls[1].arguments.data() == nullptr);
        expect(decoded[1].tool_calls[0].arguments == "{\"path\":\"a\"}");
    };

    "kmb2_rebuild_is_stable"_test = [] {
        fixture f;
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(f.msgs, structure, history);
        kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        kimix::vector<part_view> parts;
        kimix::vector<tool_call_view> calls;
        // The assembled views point INTO these two vectors: reserve the full
        // table sizes first so they never reallocate (the binding gets the same
        // guarantee from the bridge struct that owns them).
        parts.reserve(view.part_total);
        calls.reserve(view.call_total);
        kimix::vector<message_view> decoded = assemble(view, parts, calls);
        kimix::string structure2, history2;
        kimix::runtime::soul::kmb2_build(decoded, structure2, history2);
        expect(structure2 == structure);
        expect(history2 == history);
    };

    "kmb2_empty"_test = [] {
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(kimix::span<const message_view>(), structure,
                                        history);
        expect(structure.size() == 32u);
        expect(history.empty());
        kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 0u);
    };

    "kmb2_rejects_malformed"_test = [] {
        fixture f;
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(f.msgs, structure, history);
        kmb2_view view;

        // Not packed at all.
        expect(!kimix::runtime::soul::kmb2_parse("", "", view));
        expect(!kimix::runtime::soul::kmb2_parse("not a blob", "", view));
        expect(!kimix::runtime::soul::kmb2_is_packed(""));

        // Magic / version.
        {
            kimix::string bad = structure;
            bad[0] = 'X';
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        {
            kimix::string bad = structure;
            kimix::runtime::common::put_le32(bad.data() + 4, 99);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // Truncated tables.
        {
            kimix::string bad = structure.substr(0, structure.size() - 1);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        {
            kimix::string bad = structure.substr(0, 20);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // A count that cannot fit the blob.
        {
            kimix::string bad = structure;
            kimix::runtime::common::put_le32(bad.data() + 8, 1000);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // A part span past the end of `history`.
        {
            kimix::string bad = structure;
            char* part_table = bad.data() + 32 + 4 * 32;
            kimix::runtime::common::put_le32(part_table + 4, 9999);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // A negative part start.
        {
            kimix::string bad = structure;
            char* part_table = bad.data() + 32 + 4 * 32;
            kimix::runtime::common::put_le_i32(part_table, -2);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // An unknown part kind.
        {
            kimix::string bad = structure;
            char* part_table = bad.data() + 32 + 4 * 32;
            part_table[8] = static_cast<char>(99);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // A message part range past the part table.
        {
            kimix::string bad = structure;
            char* msg_table = bad.data() + 32;
            kimix::runtime::common::put_le32(msg_table + 8, 99);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // An unknown role.
        {
            kimix::string bad = structure;
            bad[32] = static_cast<char>(9);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // Half-present tool_call_id on message 3 (the flag says present, the
        // offsets are -1): rejected. Clearing the flag makes the same record
        // valid, which is the positive control for the check.
        {
            kimix::string bad = structure;
            char* msg3 = bad.data() + 32 + 3 * 32;
            kimix::runtime::common::put_le_i32(msg3 + 20, -1);
            kimix::runtime::common::put_le_i32(msg3 + 24, -1);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
            msg3[1] = static_cast<char>(msg3[1] &
                                        ~kimix::runtime::soul::kKmb2MsgHasToolCallId);
            expect(kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // A tool_call_id that is valid but flag-less.
        {
            kimix::string bad = structure;
            char* msg_table = bad.data() + 32 + 3 * 32;
            msg_table[1] = static_cast<char>(msg_table[1] &
                                             ~kimix::runtime::soul::kKmb2MsgHasToolCallId);
            expect(!kimix::runtime::soul::kmb2_parse(bad, history, view));
        }
        // The pristine blob still parses (the matrix above never mutated it).
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
    };

    "kmb2_flags"_test = [] {
        fixture f;
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(f.msgs, structure, history);
        kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        kmb2_message_record m0, m1, m2, m3;
        const auto& k = kimix::runtime::soul::kKmb2MsgHasThink;
        view.message(0, m0);
        expect((m0.flags & k) != 0);                              // think part
        expect((m0.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) == 0);
        view.message(1, m1);
        expect((m1.flags & kimix::runtime::soul::kKmb2MsgHasToolCalls) != 0);
        view.message(2, m2);
        expect((m2.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) != 0);
        view.message(3, m3);
        expect((m3.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) != 0);
        expect(m3.tcid_start == m3.tcid_end); // empty but present
    };

    "bench_kmb2_roundtrip"_test = [] {
        // 750 messages / 1500 parts / 750 calls: the report's fixture size.
        kimix::vector<kimix::vector<part_view>> parts(750);
        kimix::vector<kimix::vector<tool_call_view>> calls(750);
        kimix::vector<message_view> msgs(750);
        kimix::string tool_call_id;
        for (size_t i = 0; i < 750; ++i) {
            parts[i].push_back(part_view{part_kind::TEXT, "some message text body"});
            parts[i].push_back(part_view{part_kind::TEXT, "and a second part"});
            calls[i].push_back(tool_call_view{"call_1", "read", "{\"path\":\"a\"}"});
            msgs[i].role = static_cast<uint8_t>(i % 4);
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
            msgs[i].tool_calls =
                kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
        }
        kimix::string structure, history;
        kimix::runtime::soul::kmb2_build(msgs, structure, history);
        // 32 + 750*32 + 1500*12 + 750*24 = 60 032 B (report §2.1).
        expect(structure.size() == 60032u);
        kmb2_view view;
        for (int i = 0; i < 100; ++i) {
            expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        }
        expect(view.msg_count == 750u);
    };

    return 0;
}
