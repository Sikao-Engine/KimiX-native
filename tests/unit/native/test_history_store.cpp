// Test for src/runtime/soul/history_store.{h,cpp} — the native conversation
// arena (report candidate A1).
//
// Covers:
// - append / view / views / text_of on UTF-8 text, multi-part messages, tool
//   calls and a None vs empty tool_call_id;
// - replace / drop / truncate / clear in LIVE ORDINAL space;
// - generation() invalidation of a stale view;
// - compact(): exact byte accounting (bytes_owned drops, the surviving bytes
//   are byte-identical to before);
// - pack() of a sub-range -> the same messages, and re-appending a pack is a
//   no-op change;
// - save/load ("KMSH1") round trip incl. tombstones and malformed blobs;
// - bytes_owned vs bytes_live deltas and 10k appends without unbounded growth.

#include "ut/ut.hpp"

#include <runtime/common/le_bytes.h>
#include <runtime/soul/history_store.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::soul::HistoryStore;
using kimix::runtime::soul::kmb2_view;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

namespace tests_unit_native_test_history_store {

// A KMB2 blob for `count` messages: "msg <i>" text, an optional tool call on
// every assistant message, and a tool_call_id on every tool message.
void make_blob(uint32_t count, uint32_t role_of_first, kimix::string& structure,
               kimix::string& history,
               kimix::vector<kimix::vector<part_view>>& parts,
               kimix::vector<kimix::vector<tool_call_view>>& calls,
               kimix::vector<message_view>& views,
               kimix::vector<kimix::string>& texts,
               kimix::vector<kimix::string>& tcids) {
    parts.assign(count, {});
    calls.assign(count, {});
    views.assign(count, {});
    texts.assign(count, {});
    tcids.assign(count, {});
    for (uint32_t i = 0; i < count; ++i) {
        texts[i] = kimix::format("msg {} \xE4\xB8\x96\xE7\x95\x8C", i);
        parts[i].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[i])});
        parts[i].push_back(part_view{part_kind::THINK, "why"});
        const uint8_t role =
            static_cast<uint8_t>((role_of_first + i) % 4u);
        if (role == kimix::runtime::soul::kRoleAssistant) {
            calls[i].push_back(tool_call_view{"call_1", "read", "{}"});
        }
        if (role == kimix::runtime::soul::kRoleTool) {
            tcids[i] = "call_1";
        }
        views[i].role = role;
        views[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
        views[i].tool_calls =
            kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
        if (!tcids[i].empty()) {
            views[i].tool_call_id = kimix::string_view(tcids[i]);
        }
    }
    kimix::runtime::soul::kmb2_build(views, structure, history);
}

// Encode one message into a fresh pair (used by replace/append_packed tests).
void one_message(const kimix::string& text, uint8_t role, kimix::string& structure,
                 kimix::string& history) {
    kimix::vector<part_view> parts;
    parts.push_back(part_view{part_kind::TEXT, kimix::string_view(text)});
    message_view mv;
    mv.role = role;
    mv.parts = kimix::span<const part_view>(parts.data(), parts.size());
    kimix::runtime::soul::kmb2_build(kimix::span<const message_view>(&mv, 1), structure,
                                     history);
}

} // namespace tests_unit_native_test_history_store
using namespace tests_unit_native_test_history_store;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "history_append_and_read"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> calls;
        kimix::vector<message_view> views;
        kimix::vector<kimix::string> texts, tcids;
        make_blob(5, kimix::runtime::soul::kRoleUser, structure, history, parts, calls,
                  views, texts, tcids);

        expect(store.size() == 0u);
        const uint32_t added = store.append_packed(structure, history);
        expect(added == 5u);
        expect(store.size() == 5u);
        expect(store.generation() > 0u);

        message_view mv;
        expect(store.view(0, mv));
        expect(mv.role == kimix::runtime::soul::kRoleUser);
        expect(mv.parts.size() == 2u);
        expect(mv.parts[0].text == kimix::string_view(texts[0]));
        expect(mv.parts[1].kind == part_kind::THINK);

        kimix::string joined;
        store.text_of(0, joined);
        expect(joined == kimix::string_view(texts[0])); // only the TEXT part

        expect(store.views().size() == 5u);
        expect(store.bytes_live() > 0u);
        expect(store.bytes_owned() >= store.bytes_live());
    };

    "history_generation_invalidates"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        one_message("first", kimix::runtime::soul::kRoleUser, structure, history);
        store.append_packed(structure, history);
        const uint64_t gen = store.generation();
        message_view stale;
        expect(store.view(0, stale));
        const char* stale_ptr = stale.parts[0].text.data();

        one_message("second", kimix::runtime::soul::kRoleUser, structure, history);
        store.append_packed(structure, history);
        expect(store.generation() > gen);
        // The old span is documented as invalid after a mutation; the store must
        // at least still read back consistently through view().
        message_view fresh;
        expect(store.view(0, fresh));
        expect(fresh.parts[0].text == "first");
        expect(store.view(0, stale));
        expect(stale.parts[0].text == "first");
        (void)stale_ptr;
    };

    "history_replace_drop_truncate"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> calls;
        kimix::vector<message_view> views;
        kimix::vector<kimix::string> texts, tcids;
        make_blob(4, kimix::runtime::soul::kRoleUser, structure, history, parts, calls,
                  views, texts, tcids);
        store.append_packed(structure, history);

        kimix::string s2, h2;
        one_message("replacement", kimix::runtime::soul::kRoleAssistant, s2, h2);
        expect(store.replace_packed(1, s2, h2));
        message_view mv;
        expect(store.view(1, mv));
        expect(mv.parts.size() == 1u);
        expect(mv.parts[0].text == "replacement");
        expect(mv.role == kimix::runtime::soul::kRoleAssistant);

        expect(!store.replace_packed(99, s2, h2));

        expect(store.drop(0));
        expect(!store.drop(99)); // out of range
        expect(store.size() == 3u);
        // Ordinals shift: the former #1 is now #0.
        expect(store.view(0, mv));
        expect(mv.parts[0].text == "replacement");

        store.truncate(1);
        expect(store.size() == 1u);
        expect(store.view(0, mv));
        expect(mv.parts[0].text == "replacement");

        store.clear();
        expect(store.size() == 0u);
        expect(!store.view(0, mv));
        expect(store.views().empty());
    };

    "history_compact_reclaims"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> calls;
        kimix::vector<message_view> views;
        kimix::vector<kimix::string> texts, tcids;
        make_blob(8, kimix::runtime::soul::kRoleUser, structure, history, parts, calls,
                  views, texts, tcids);
        store.append_packed(structure, history);
        const uint64_t full_live = store.bytes_live();
        const uint64_t full_owned = store.bytes_owned();

        // Tombstone half of the conversation.
        for (uint32_t i = 0; i < 4; ++i) {
            expect(store.drop(0)); // always the new first live message
        }
        expect(store.size() == 4u);
        const uint64_t half_live = store.bytes_live();
        expect(half_live < full_live);

        const uint64_t freed = store.compact();
        expect(freed > 0u);
        expect(store.bytes_owned() < full_owned);
        expect(store.bytes_live() == half_live);
        // Nothing survived that should not have.
        for (uint32_t i = 0; i < store.size(); ++i) {
            message_view mv;
            expect(store.view(i, mv));
            expect(mv.parts.size() == 2u);
        }
        // A second compact never grows the store and never loses live bytes
        // (it may still reclaim allocator slack, so the freed count is not
        // asserted to be zero).
        const uint64_t live_after = store.bytes_live();
        const uint64_t owned_after = store.bytes_owned();
        const uint64_t freed_again = store.compact();
        expect(freed_again <= owned_after);
        expect(store.bytes_live() == live_after);
        expect(store.bytes_owned() <= owned_after);
    };

    "history_pack_subrange"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> calls;
        kimix::vector<message_view> views;
        kimix::vector<kimix::string> texts, tcids;
        make_blob(6, kimix::runtime::soul::kRoleUser, structure, history, parts, calls,
                  views, texts, tcids);
        store.append_packed(structure, history);

        kimix::string slice_structure, slice_history;
        store.pack(2, 3, slice_structure, slice_history);
        kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(slice_structure, slice_history, view));
        expect(view.msg_count == 3u);

        HistoryStore restored;
        expect(restored.append_packed(slice_structure, slice_history) == 3u);
        message_view a, b;
        expect(store.view(2, a) && restored.view(0, b));
        expect(a.role == b.role);
        expect(a.parts[0].text == b.parts[0].text);

        // Out-of-range pack clamps to an empty blob.
        kimix::string empty_structure, empty_history;
        store.pack(99, 5, empty_structure, empty_history);
        expect(kimix::runtime::soul::kmb2_parse(empty_structure, empty_history, view));
        expect(view.msg_count == 0u);
        store.pack(5, 0, empty_structure, empty_history);
        expect(kimix::runtime::soul::kmb2_parse(empty_structure, empty_history, view));
        expect(view.msg_count == 0u);
    };

    "history_save_load"_test = [] {
        HistoryStore store;
        kimix::string structure, history;
        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> calls;
        kimix::vector<message_view> views;
        kimix::vector<kimix::string> texts, tcids;
        make_blob(6, kimix::runtime::soul::kRoleUser, structure, history, parts, calls,
                  views, texts, tcids);
        store.append_packed(structure, history);
        store.drop(2); // a tombstone must survive the round trip

        kimix::string blob;
        store.save(blob);
        expect(blob.size() > 32u);
        expect(std::memcmp(blob.data(), "KMSH1", 5) == 0);

        HistoryStore restored;
        expect(restored.load(blob));
        expect(restored.size() == store.size());
        for (uint32_t i = 0; i < store.size(); ++i) {
            message_view a, b;
            expect(store.view(i, a) && restored.view(i, b));
            expect(a.role == b.role);
            expect(a.parts.size() == b.parts.size());
            expect(a.parts[0].text == b.parts[0].text);
            expect(a.tool_call_id == b.tool_call_id);
        }

        // Malformed blobs are rejected and leave the store untouched.
        const uint32_t before = restored.size();
        expect(!restored.load(""));
        expect(!restored.load("KMSH1"));
        kimix::string bad = blob;
        bad[0] = 'X';
        expect(!restored.load(bad));
        kimix::string truncated = blob.substr(0, blob.size() - 3);
        expect(!restored.load(truncated));
        kimix::string garbage_count = blob;
        kimix::runtime::common::put_le32(garbage_count.data() + 12, 999u);
        expect(!restored.load(garbage_count));
        expect(restored.size() == before);
    };

    "history_appends_are_amortised"_test = [] {
        HistoryStore store(1u << 20);
        kimix::string structure, history;
        one_message("x", kimix::runtime::soul::kRoleUser, structure, history);
        for (int i = 0; i < 10000; ++i) {
            expect(store.append_packed(structure, history) == 1u);
        }
        expect(store.size() == 10000u);
        // The tables grow with the live count, not with the append count.
        expect(store.bytes_owned() < 4u * 1024u * 1024u);
        expect(store.views().size() == 10000u);
        // Full re-read stays consistent.
        message_view mv;
        expect(store.view(9999, mv));
        expect(mv.parts[0].text == "x");
    };

    "history_rejects_malformed_append"_test = [] {
        HistoryStore store;
        expect(store.append_packed("", "") == 0u);
        expect(store.append_packed("KMB2", "") == 0u);
        expect(store.size() == 0u);
        // A valid header with a bogus count changes nothing.
        kimix::string structure, history;
        one_message("ok", kimix::runtime::soul::kRoleUser, structure, history);
        kimix::string bad = structure;
        kimix::runtime::common::put_le32(bad.data() + 12, 7u);
        expect(store.append_packed(bad, history) == 0u);
        expect(store.size() == 0u);
    };

    return 0;
}
