// test_compaction_ledger.cpp - audit rows C10 / C12 / C13:
//
//   C10 - the durable compaction transaction ledger
//         (src/agent/compaction_ledger.{h,cpp}, the
//         kimi_cli/soul/compaction_ledger.py port): the reference byte shape,
//         record_start / record_end rewrite-in-place (one line per
//         transaction, error key popped on success and set on failure),
//         latest() / records() with malformed-line skip, the no-op ledger for
//         a disabled knob or an unusable directory, failure isolation, and
//         the emit sites wired into KimiSoul's compaction paths.
//   C13 - KimiSoul::estimated_token_count_for_model (compaction.py:165-176)
//         and the KV-cache-aligned compaction transport (the request replays
//         the live system prompt + the to_compact region verbatim, with the
//         compaction instruction as the final user message).
//   C12 - the agent-side mode -> guidance mapping
//         (kimix::agent::compaction_style_guidance, compaction.py
//         _MODE_GUIDANCE).
//
// Framework: Boost.UT (tests/ut/ut.hpp); the soul is driven with a scripted
// fake chat backend (no network).
#include "ut/ut.hpp"

#include <cstdio>
#include <system_error>

#include <core/kimix_core.h>
#include <agent/compaction_ledger.h>
#include <agent/soul.h>
#include <builtin_tools/compact_tool.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::string tmp_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base = kimix::filesystem::temp_directory_path(ec) /
                                   "kimix_compaction_ledger_test" / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

kimix::string file_text(const kimix::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return kimix::string("<missing: ") + path + ">";
    }
    kimix::string text;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    return text;
}

bool has_substr(const kimix::string &hay, kimix::string_view needle) {
    return hay.find(needle) != kimix::string::npos;
}

bool write_text(const kimix::string &path, kimix::string_view text) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

kimix::llm::Message user_msg(kimix::string_view content) {
    kimix::llm::Message m;
    m.role = "user";
    m.content = kimix::string(content);
    return m;
}

kimix::llm::Message assistant_msg(kimix::string_view content) {
    kimix::llm::Message m;
    m.role = "assistant";
    m.content = kimix::string(content);
    return m;
}

// Scripted chat backend (compaction summary on the first call).
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    size_t index = 0;
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)tools;
        (void)on_chunk;
        requests.push_back(messages);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::builtin_tools::compact::message
to_compact(const kimix::llm::Message &m) {
    kimix::builtin_tools::compact::message cm;
    cm.role = m.role;
    if (!m.thinking.empty()) {
        kimix::builtin_tools::compact::content_part p;
        p.type = "think";
        p.text = m.thinking;
        cm.content.push_back(std::move(p));
    }
    if (!m.content.empty()) {
        kimix::builtin_tools::compact::content_part p;
        p.type = "text";
        p.text = m.content;
        cm.content.push_back(std::move(p));
    }
    cm.tool_call_count = static_cast<int32_t>(m.tool_calls.size());
    return cm;
}

int64_t estimate_all(const kimix::vector<kimix::llm::Message> &msgs) {
    kimix::vector<kimix::builtin_tools::compact::message> cms;
    for (const kimix::llm::Message &m : msgs) {
        cms.push_back(to_compact(m));
    }
    return kimix::builtin_tools::compact::estimate_message_tokens(
        kimix::span<const kimix::builtin_tools::compact::message>(
            cms.data(), static_cast<int64_t>(cms.size())));
}

} // namespace

int main() {
    using namespace boost::ut::literals;

    // =======================================================================
    // C10: the ledger record byte shape (compaction_ledger.py _record_to_dict)
    // =======================================================================
    "ledger_record_byte_shape"_test = [] {
        const kimix::string dir = tmp_dir("byte_shape");
        const kimix::string path = dir + "/compaction_ledger.jsonl";
        kimix::agent::CompactionLedger ledger(path);
        expect(ledger.enabled());

        kimix::agent::CompactionRecord r;
        r.compaction_id = "abc123";
        r.trigger = "auto";
        r.started_at = 1712345678.5;
        r.shadowed_start = 0;
        r.shadowed_end = 12;
        r.shadowed_tokens = 100;
        r.summary_tokens = 0;
        r.preserved_tokens = 40;
        r.shrank = false;
        kimix::string error;
        expect(ledger.record_start(r, error)) << error;
        // orjson compact + OPT_APPEND_NEWLINE, keys in _record_to_dict order,
        // error: null on the start record.
        expect(file_text(path) ==
               "{\"compaction_id\":\"abc123\",\"trigger\":\"auto\","
               "\"started_at\":1712345678.5,\"shadowed_range\":[0,12],"
               "\"shadowed_tokens\":100,\"summary_tokens\":0,"
               "\"preserved_tokens\":40,\"shrank\":false,\"error\":null}\n")
            << file_text(path);

        // record_end rewrites the single line IN PLACE: the error key is
        // popped on success and the real summary/shrank land.
        expect(ledger.record_end_success("abc123", 77, true, error)) << error;
        expect(file_text(path) ==
               "{\"compaction_id\":\"abc123\",\"trigger\":\"auto\","
               "\"started_at\":1712345678.5,\"shadowed_range\":[0,12],"
               "\"shadowed_tokens\":100,\"summary_tokens\":77,"
               "\"preserved_tokens\":40,\"shrank\":true}\n")
            << file_text(path);
        const kimix::optional<kimix::agent::CompactionRecord> last =
            ledger.latest();
        expect(last.has_value());
        expect(last->summary_tokens == 77_i);
        expect(last->shrank);
        expect(!last->has_error);

        // record_end with an unknown id leaves the file untouched.
        const kimix::string before = file_text(path);
        kimix::string end_error;
        expect(!ledger.record_end_failure("missing", "boom", end_error));
        expect(has_substr(end_error, "no start record")) << end_error;
        expect(file_text(path) == before);

        // A later failure re-sets the error key on the matching line.
        expect(ledger.record_end_failure("abc123", "kaboom", error)) << error;
        expect(has_substr(file_text(path), "\"error\":\"kaboom\"}"))
            << file_text(path);
        const kimix::optional<kimix::agent::CompactionRecord> failed =
            ledger.latest();
        expect(failed.has_value());
        expect(failed->has_error);
        expect(failed->error == "kaboom");
    };

    "ledger_skips_malformed_lines_and_keeps_one_line_per_transaction"_test = [] {
        const kimix::string dir = tmp_dir("malformed");
        const kimix::string path = dir + "/compaction_ledger.jsonl";
        kimix::agent::CompactionLedger ledger(path);
        kimix::string error;
        for (int i = 0; i < 3; ++i) {
            kimix::agent::CompactionRecord r;
            r.compaction_id = kimix::format("c{}", i);
            r.trigger = i == 0 ? "auto" : (i == 1 ? "manual" : "overflow");
            r.started_at = 1000.0 + i;
            r.shadowed_end = i;
            expect(ledger.record_start(r, error)) << error;
        }
        // A hand-corrupted line (the reference skips it with a warning).
        {
            std::FILE *f = std::fopen(path.c_str(), "ab");
            expect(f != nullptr);
            std::fwrite("{{{\n", 1, 4, f);
            std::fclose(f);
        }
        // "records" must survive: only the malformed line is dropped.
        expect(ledger.records().size() == 3u);
        expect(ledger.latest().has_value());
        expect(ledger.latest()->compaction_id == "c2");
        // record_end on the FIRST record still rewrites exactly that line.
        expect(ledger.record_end_success("c0", 5, true, error)) << error;
        const kimix::string text = file_text(path);
        expect(has_substr(text, "\"compaction_id\":\"c0\""));
        expect(has_substr(text, "\"compaction_id\":\"c1\""));
        expect(has_substr(text, "\"compaction_id\":\"c2\""));
        expect(ledger.records().size() == 3u);
    };

    "ledger_noop_when_disabled_or_unusable"_test = [] {
        // A disabled knob -> the no-op ledger (the reference's
        // CompactionLedger(None)): nothing is written, nothing raises.
        kimix::agent::CompactionLedger disabled =
            kimix::agent::CompactionLedger::for_session(tmp_dir("disabled"),
                                                        /*enabled=*/false);
        expect(!disabled.enabled());
        kimix::string error;
        kimix::agent::CompactionRecord r;
        r.compaction_id = "x";
          expect(disabled.record_start(r, error));
          // A no-op method "succeeded" by doing nothing (never an error).
          expect(disabled.record_end_success("x", 1, true, error));
          expect(!disabled.latest().has_value());
          expect(disabled.records().empty());

        // A session dir whose .kimix_cache cannot be created (a FILE sits
        // there) degrades to the no-op ledger instead of failing.
        const kimix::string dir = tmp_dir("blocked");
        expect(write_text(dir + "/.kimix_cache", "not a dir"));
        kimix::agent::CompactionLedger blocked =
            kimix::agent::CompactionLedger::for_session(dir, /*enabled=*/true);
        expect(!blocked.enabled());
    };

    // =======================================================================
    // C10: the emit sites in KimiSoul's compaction path
    // =======================================================================
    "soul_writes_ledger_on_manual_compaction"_test = [] {
        const kimix::string ws = tmp_dir("soul_manual");
        kimix::agent::AgentSession session(ws);
        session.set_state_dir(ws);
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};
        kimix::agent::KimiSoul::options opts;
        opts.loop_control.compaction_ledger_enabled = true;
        kimix::agent::KimiSoul soul(session, backend, opts);
        expect(soul.compaction_ledger().enabled());
        expect(soul.compaction_records().empty());

        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            h.push_back(user_msg(kimix::format(
                "question {} with enough padding text to be worth compacting",
                i)));
            h.push_back(assistant_msg(kimix::format(
                "answer {} with enough padding text to be worth compacting",
                i)));
        }
        kimix::string error;
        expect(soul.compact_context("", error, /*manual=*/true)) << error;

        // The transaction is durable at
        // <session_dir>/.kimix_cache/compaction_ledger.jsonl ...
        const kimix::string path =
            ws + "/.kimix_cache/compaction_ledger.jsonl";
        const kimix::string text = file_text(path);
        expect(has_substr(text, "\"trigger\":\"manual\"")) << text;
        // ... exactly ONE line: record_start + record_end rewrote in place.
        size_t lines = 0;
        for (const char c : text) {
            lines += c == '\n';
        }
        expect(lines == 1u) << text;
        // The finalized record carries the real outcome.
        expect(soul.compaction_records().size() == 1u);
        const kimix::agent::CompactionRecord &rec =
            soul.compaction_records().front();
        expect(rec.trigger == "manual");
        expect(rec.shrank);
        expect(!rec.has_error);
        expect(rec.shadowed_tokens > 0);
        expect(rec.summary_tokens > 0);
        expect(rec.preserved_tokens >= 0);
        expect(rec.shadowed_end > rec.shadowed_start);
        expect(rec.compaction_id.size() == 32u); // uuid4().hex shape
    };

    "soul_ledger_disabled_writes_nothing"_test = [] {
        const kimix::string ws = tmp_dir("soul_disabled");
        kimix::agent::AgentSession session(ws);
        session.set_state_dir(ws);
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};
        kimix::agent::KimiSoul::options opts;
        opts.loop_control.compaction_ledger_enabled = false;
        kimix::agent::KimiSoul soul(session, backend, opts);
        expect(!soul.compaction_ledger().enabled());
        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            h.push_back(user_msg(kimix::format("question {}", i)));
            h.push_back(assistant_msg(kimix::format("answer {}", i)));
        }
        kimix::string error;
        expect(soul.compact_context("", error, /*manual=*/true)) << error;
        expect(!kimix::filesystem::exists(
            kimix::filesystem::path(ws) / ".kimix_cache" /
            "compaction_ledger.jsonl"));
        expect(soul.compaction_records().empty());
    };

    "soul_ledger_records_failure_and_overflow_trigger"_test = [] {
        const kimix::string ws = tmp_dir("soul_failure");
        kimix::agent::AgentSession session(ws);
        session.set_state_dir(ws);
        FakeBackend backend;
        // The summarizer call fails: record_end must carry the error and the
        // compaction reports failure (the ledger itself never fails the flow).
        kimix::llm::ChatResult bad;
        bad.ok = false;
        bad.error = "provider exploded";
        backend.scripted = {bad};
        kimix::agent::KimiSoul::options opts;
        opts.loop_control.compaction_ledger_enabled = true;
        kimix::agent::KimiSoul soul(session, backend, opts);
        auto &h = session.history();
        for (int i = 0; i < 6; ++i) {
            h.push_back(user_msg(kimix::format("question {}", i)));
            h.push_back(assistant_msg(kimix::format("answer {}", i)));
        }
        kimix::string error;
        expect(!soul.compact_context("", error, /*manual=*/false,
                                     kimix::builtin_tools::compact::CompactMode::
                                         aggressive,
                                     1, "overflow"));
        const kimix::optional<kimix::agent::CompactionRecord> rec =
            soul.compaction_ledger().latest();
        expect(rec.has_value());
        expect(rec->trigger == "overflow");
        expect(rec->has_error);
        expect(rec->error.find("provider exploded") != kimix::string::npos)
            << rec->error;
        expect(!rec->shrank);
    };

    // =======================================================================
    // C13: estimated_token_count_for_model
    // =======================================================================
    "estimated_token_count_for_model"_test = [] {
        kimix::vector<kimix::llm::Message> msgs;
        msgs.push_back(user_msg("first message with some content"));
        msgs.push_back(assistant_msg("second message, a bit longer content"));
        msgs.push_back(user_msg("third"));

        // No usage: every message is estimated from its text.
        expect(kimix::agent::KimiSoul::estimated_token_count_for_model(
                   msgs, kimix::optional<int64_t>()) == estimate_all(msgs));

        // With a usage: the exact summary token count replaces the estimate
        // of messages[0]; the preserved tail (messages[1:]) is estimated.
        const int64_t expected = 555 + estimate_all(
            kimix::vector<kimix::llm::Message>(msgs.begin() + 1, msgs.end()));
        expect(kimix::agent::KimiSoul::estimated_token_count_for_model(
                   msgs, kimix::optional<int64_t>(555)) == expected);

        // usage with an empty history falls back to the all-messages estimate
        // (the reference's `len(self.messages) > 0` guard).
        expect(kimix::agent::KimiSoul::estimated_token_count_for_model(
                   {}, kimix::optional<int64_t>(555)) == 0_i);
    };

    // =======================================================================
    // C13: the KV-cache-aligned summarization transport
    // =======================================================================
    "soul_compaction_uses_aligned_transport"_test = [] {
        const kimix::string ws = tmp_dir("aligned");
        kimix::agent::AgentSession session(ws);
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};
        kimix::agent::KimiSoul::options opts;
        opts.system_prompt = "the live serialized system prompt";
        kimix::agent::KimiSoul soul(session, backend, opts);

        auto &h = session.history();
        // A compactable region plus a preserved tail.
        for (int i = 0; i < 5; ++i) {
            h.push_back(user_msg(kimix::format("region question {}", i)));
            h.push_back(assistant_msg(kimix::format("region answer {}", i)));
        }
        h.push_back(user_msg("tail question"));
        h.push_back(assistant_msg("tail answer"));

        kimix::string error;
        expect(soul.compact_context("keep the tail", error, /*manual=*/true))
            << error;
        expect(backend.requests.size() == 1u);
        const kimix::vector<kimix::llm::Message> &req = backend.requests[0];
        // The request replays: [system] + the region verbatim + [instruction].
        expect(req.size() >= 3u);
        expect(req.front().role == "system");
        expect(req.front().content == "the live serialized system prompt")
            << "the compaction request must replay the SAME serialized system "
               "prompt as the live turns (KV-cache alignment)";
        // The region messages appear VERBATIM (in order, unmodified).
        bool saw_region_first = req[1].role == "user" &&
                                req[1].content == "region question 0";
        expect(saw_region_first) << req[1].content;
        // The instruction is the LAST message and carries the compaction
        // prompt (the same text the legacy transport appended).
        const kimix::llm::Message &instruction = req.back();
        expect(instruction.role == "user");
        expect(instruction.content.find(
                   "Compact the above agent conversation context") !=
               kimix::string::npos)
            << instruction.content;
        expect(instruction.content.find("keep the tail") !=
               kimix::string::npos)
            << "the custom instruction rides on the compaction prompt";
        // The instruction appears exactly once (no double append).
        const size_t first =
            instruction.content.find("Compact the above agent conversation");
        expect(instruction.content.find("Compact the above agent conversation",
                                        first + 1) == kimix::string::npos);
    };

    "soul_compaction_legacy_transport_when_disabled"_test = [] {
        const kimix::string ws = tmp_dir("legacy");
        kimix::agent::AgentSession session(ws);
        FakeBackend backend;
        kimix::llm::ChatResult summary;
        summary.ok = true;
        summary.content = "<current_focus>compacted</current_focus>";
        backend.scripted = {summary};
        kimix::agent::KimiSoul::options opts;
        opts.compact_aligned_transport = false;
        kimix::agent::KimiSoul soul(session, backend, opts);
        auto &h = session.history();
        for (int i = 0; i < 5; ++i) {
            h.push_back(user_msg(kimix::format("region question {}", i)));
            h.push_back(assistant_msg(kimix::format("region answer {}", i)));
        }
        h.push_back(user_msg("tail question"));
        h.push_back(assistant_msg("tail answer"));
        kimix::string error;
        expect(soul.compact_context("", error, /*manual=*/true)) << error;
        expect(backend.requests.size() == 1u);
        // Legacy flattened shape: [system, one flattened user message].
        expect(backend.requests[0].size() == 2u);
        expect(backend.requests[0][0].role == "system");
        expect(backend.requests[0][0].content.find("conversation compactor") !=
               kimix::string::npos);
        expect(backend.requests[0][1].role == "user");
    };

    // =======================================================================
    // C12: the agent-side mode -> guidance mapping
    // =======================================================================
    "compaction_style_guidance_mapping"_test = [] {
        using kimix::agent::compaction_style_guidance;
        // Every style mode carries the reference guidance text.
        expect(compaction_style_guidance("auto").find(
                   "**Compaction Style Guidance:** Be balanced.") ==
               0); // "auto" is the balanced default (CompactMode.BALANCED)
        expect(compaction_style_guidance("").find(
                   "**Compaction Style Guidance:** Be balanced.") == 0);
        expect(compaction_style_guidance("balanced").find(
                   "**Compaction Style Guidance:** Be balanced.") == 0);
        expect(compaction_style_guidance("aggressive").find(
                   "**Compaction Style Guidance:** Be aggressive.") == 0);
        expect(compaction_style_guidance("retentive").find(
                   "**Compaction Style Guidance:** Be retentive.") == 0);
        expect(compaction_style_guidance("technical").find(
                   "**Compaction Style Guidance:** Focus on technical "
                   "specifics.") == 0);
        // The retentive guidance keeps the "Do not over-compress." tail and
        // the aggressive one the "Keep only the essential facts" tail (the
        // reference's _MODE_GUIDANCE bodies, verbatim).
        expect(has_substr(kimix::string(compaction_style_guidance("retentive")),
                          "Do not over-compress."));
        expect(has_substr(kimix::string(compaction_style_guidance("aggressive")),
                          "Keep only the essential facts, decisions, and current state."));
        // The four guidance bodies are pairwise distinct.
        expect(compaction_style_guidance("balanced") !=
               compaction_style_guidance("technical"));
    };

    return 0;
}
