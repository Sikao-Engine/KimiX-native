// Test for src/runtime/soul/session_store.{h,cpp} — the serve-mode part ring
// and session table (report candidates A3 + C3).
//
// Covers the per-session byte budget (drop-oldest), the rendered
// MessagePart.to_dict() JSON (pinned byte-for-byte; the same golden is asserted
// by python/tests/test_session_store.py against the shim's _compat mirror),
// snapshot(since_seq), part_json, LRU eviction over whole sessions, status /
// generation bookkeeping, close idempotency and byte accounting.

#include "ut/ut.hpp"

#include <runtime/soul/session_store.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::soul::SessionStore;
using kimix::runtime::soul::part_record;

namespace tests_unit_native_test_session_store {

// The five-part fixture whose JSON is pinned below.
const char* kGolden =
    "{\"sessionID\":\"ses_1\",\"status\":0,\"seq\":5,\"parts\":["
    "{\"id\":\"p1\",\"sessionID\":\"ses_1\",\"messageID\":\"msg_1\",\"type\":\"text\","
    "\"text\":\"hello\",\"time\":{\"start\":1,\"end\":2}},"
    "{\"id\":\"p2\",\"sessionID\":\"ses_1\",\"messageID\":\"msg_1\",\"type\":\"tool\","
    "\"callID\":\"call_1\",\"tool\":\"read\",\"state\":{\"status\":\"completed\"}},"
    "{\"id\":\"p3\",\"sessionID\":\"ses_1\",\"messageID\":\"msg_1\","
    "\"type\":\"step-finish\",\"reason\":\"stop\",\"snapshot\":\"snap\",\"cost\":0.5,"
    "\"prompt\":{\"a\":1},\"tokens\":{\"in\":2}},"
    "{\"id\":\"p4\",\"sessionID\":\"ses_1\",\"messageID\":\"msg_1\","
    "\"type\":\"reasoning\",\"text\":\"thinking\"},"
    "{\"id\":\"p5\",\"sessionID\":\"ses_1\",\"messageID\":\"msg_1\","
    "\"type\":\"step-start\",\"snapshot\":\"s0\"}]}";

void fill(SessionStore& store, uint32_t session) {
    part_record text;
    text.kind = kimix::runtime::soul::kPartText;
    text.session_id = "ses_1";
    text.message_id = "msg_1";
    text.part_id = "p1";
    text.text = "hello";
    text.time_json = "{\"start\":1,\"end\":2}";
    store.append(session, text);

    part_record tool;
    tool.kind = kimix::runtime::soul::kPartTool;
    tool.session_id = "ses_1";
    tool.message_id = "msg_1";
    tool.part_id = "p2";
    tool.call_id = "call_1";
    tool.tool = "read";
    tool.state_json = "{\"status\":\"completed\"}";
    store.append(session, tool);

    part_record finish;
    finish.kind = kimix::runtime::soul::kPartStepFinish;
    finish.session_id = "ses_1";
    finish.message_id = "msg_1";
    finish.part_id = "p3";
    finish.reason = "stop";
    finish.snapshot = "snap";
    finish.cost = 0.5;
    finish.prompt_json = "{\"a\":1}";
    finish.tokens_json = "{\"in\":2}";
    store.append(session, finish);

    part_record reasoning;
    reasoning.kind = kimix::runtime::soul::kPartReasoning;
    reasoning.session_id = "ses_1";
    reasoning.message_id = "msg_1";
    reasoning.part_id = "p4";
    reasoning.text = "thinking";
    store.append(session, reasoning);

    part_record start;
    start.kind = kimix::runtime::soul::kPartStepStart;
    start.session_id = "ses_1";
    start.message_id = "msg_1";
    start.part_id = "p5";
    start.snapshot = "s0";
    store.append(session, start);
}

} // namespace tests_unit_native_test_session_store
using namespace tests_unit_native_test_session_store;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "session_snapshot_golden"_test = [] {
        SessionStore store(4096, 65536);
        const uint32_t session = store.create("ses_1");
        expect(store.session_count() == 1u);
        fill(store, session);
        expect(store.part_count(session) == 5u);
        expect(store.session_id(session) == "ses_1");

        kimix::string json;
        expect(store.snapshot(session, 0, json));
        expect(json == kimix::string_view(kGolden))
            << "session snapshot drifted from the pinned to_dict() shape";
        expect(store.generation(session) == 5u);
    };

    "session_snapshot_since_seq"_test = [] {
        SessionStore store(4096, 65536);
        const uint32_t session = store.create("ses_1");
        fill(store, session);
        kimix::string json;
        expect(store.snapshot(session, 4, json));
        expect(json.find("\"p1\"") == kimix::string::npos);
        expect(json.find("\"p4\"") != kimix::string::npos);
        expect(json.find("\"p5\"") != kimix::string::npos);
        expect(json.find("\"seq\":5") != kimix::string::npos);
        // Unknown session.
        expect(!store.snapshot(99, 0, json));
    };

    "session_part_json"_test = [] {
        SessionStore store(4096, 65536);
        const uint32_t session = store.create("ses_1");
        fill(store, session);
        kimix::string json;
        expect(store.part_json(session, "p2", json));
        expect(json ==
               kimix::string_view("{\"id\":\"p2\",\"sessionID\":\"ses_1\","
                                  "\"messageID\":\"msg_1\",\"type\":\"tool\","
                                  "\"callID\":\"call_1\",\"tool\":\"read\","
                                  "\"state\":{\"status\":\"completed\"}}"));
        expect(!store.part_json(session, "nope", json));
        expect(!store.part_json(99, "p1", json));
    };

    "session_ring_budget"_test = [] {
        // A budget that holds ~3 of the 5 parts (they measure 149 bytes total).
        SessionStore store(120, 65536);
        const uint32_t session = store.create("ses_1");
        fill(store, session);
        expect(store.part_count(session) < 5u);
        expect(store.dropped_parts(session) >= 1u);
        expect(store.bytes_live() <= 120u);
        kimix::string json;
        expect(store.snapshot(session, 0, json));
        expect(json.find("\"p1\"") == kimix::string::npos);
        expect(json.find("\"p5\"") != kimix::string::npos);

        // A single part larger than the whole per-session budget is refused.
        part_record huge;
        huge.kind = kimix::runtime::soul::kPartText;
        huge.session_id = "ses_1";
        huge.message_id = "msg_1";
        huge.part_id = "big";
        kimix::string big_text(4096, 'x');
        huge.text = big_text;
        expect(!store.append(session, huge));
    };

    "session_lru_eviction"_test = [] {
        // Total budget that fits one session's parts (149 B) but not two.
        SessionStore store(4096, 200);
        const uint32_t a = store.create("ses_a");
        fill(store, a);
        const uint32_t b = store.create("ses_b");
        fill(store, b);
        // The second session pushed the total over budget: the least recently
        // used (ses_a) is evicted whole.
        expect(store.session_count() == 1u);
        kimix::string json;
        expect(!store.snapshot(a, 0, json));
        expect(store.snapshot(b, 0, json));
        expect(json.find("ses_b") != kimix::string::npos);
        expect(store.bytes_live() <= 200u);
    };

    "session_lifecycle"_test = [] {
        SessionStore store(4096, 65536);
        const uint32_t session = store.create("ses_1");
        // Idempotent create.
        expect(store.create("ses_1") == session);
        expect(store.session_count() == 1u);

        store.set_status(session, kimix::runtime::soul::kSessionBusy);
        expect(store.status(session) == kimix::runtime::soul::kSessionBusy);
        kimix::string json;
        expect(store.snapshot(session, 0, json));
        expect(json.find("\"status\":1") != kimix::string::npos);

        expect(store.close(session));
        expect(!store.close(session)); // idempotent
        expect(store.session_count() == 0u);
        expect(!store.snapshot(session, 0, json));
        expect(store.status(session) == kimix::runtime::soul::kSessionIdle);

        // A closed handle can be recreated, and the aggregate clear() closes all.
        const uint32_t again = store.create("ses_1");
        fill(store, again);
        expect(store.bytes_owned() > 0u);
        store.clear();
        expect(store.session_count() == 0u);
        expect(store.bytes_live() == 0u);
    };

    "session_accepts_utf8_and_empty"_test = [] {
        SessionStore store(4096, 65536);
        const uint32_t session = store.create("ses_\xE4\xB8\x96");
        part_record rec;
        rec.kind = kimix::runtime::soul::kPartText;
        rec.session_id = "ses_\xE4\xB8\x96";
        rec.message_id = "m";
        rec.part_id = "p";
        rec.text = "text \xE4\xB8\x96\xE7\x95\x8C";
        expect(store.append(session, rec));
        kimix::string json;
        expect(store.snapshot(session, 0, json));
        expect(json.find("\xE4\xB8\x96\xE7\x95\x8C") != kimix::string::npos);
    };

    return 0;
}
