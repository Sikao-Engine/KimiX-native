// test_cli_session_store.cpp - store-level tests for the audit rows that live
// on the session store (src/cli/cli_session.{h,cpp}):
//
//   B5  - meta-record write-through: the _system_prompt / _usage meta records
//         (reference byte shapes) and their DB rows, content-addressed system
//         prompt, adoption on resume (stored_system_prompt / last_usage).
//   B8  - checkpoints: create_checkpoint / revert_to / n_checkpoints with the
//         synthetic <system>CHECKPOINT n</system> user record, durable in the
//         DB backend, the reference refusal for unknown ids.
//   B9  - structured export: ExportedContext{system_prompt, messages,
//         checkpoints, usages} + the byte-exact export markdown golden.
//   B10 - lenient record parsing (loads_relaxed: strict -> json_repair) so one
//         malformed line cannot lose the transcript.
//   B11 - stale <system-reminder> strip on restore.
//   B12 - replace_history (only-lower token count, checkpoints/usage reset),
//         the restore()-after-mutation guard, and the backend auto-detect.
//
// Framework: Boost.UT (tests/ut/ut.hpp). Every test works inside its own
// directory under the system temp dir.
#include "ut/ut.hpp"

#include <cstdio>
#include <system_error>

#include <core/kimix_core.h>
#include <agent/context_db.h>
#include <cli/cli_common.h>
#include <cli/cli_session.h>
#include <runtime/soul/message_view.h>
#include <runtime/tools/export_builder.h>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace cli = kimix::cli;

namespace {

kimix::string ws_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base = kimix::filesystem::temp_directory_path(ec) /
                                   "kimix_cli_session_store" / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

kimix::string file_text(const kimix::string &path) {
    kimix::string text;
    kimix::string error;
    if (!cli::read_file(path, text, error)) {
        return kimix::string("<unreadable: ") + path + ">";
    }
    return text;
}

bool has_substr(const kimix::string &hay, kimix::string_view needle) {
    return hay.find(needle) != kimix::string::npos;
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

// llm::Message -> runtime message view (the conversion soul.cpp's C8 export
// path performs; text parts only, thinking carried as a think part).
kimix::vector<kimix::runtime::soul::message_view>
to_views(const kimix::vector<kimix::llm::Message> &msgs,
         kimix::vector<kimix::vector<kimix::runtime::soul::part_view>> &parts,
         kimix::vector<kimix::vector<kimix::runtime::soul::tool_call_view>> &tcs) {
    using namespace kimix::runtime::soul;
    const auto role_code = [](kimix::string_view role) -> uint8_t {
        if (role == "system") return kRoleSystem;
        if (role == "user") return kRoleUser;
        if (role == "assistant") return kRoleAssistant;
        return kRoleTool;
    };
    kimix::vector<message_view> views;
    views.reserve(msgs.size());
    parts.reserve(msgs.size());
    tcs.reserve(msgs.size());
    for (const kimix::llm::Message &m : msgs) {
        kimix::vector<part_view> p;
        if (!m.thinking.empty()) {
            p.push_back(part_view{part_kind::THINK, kimix::string_view(m.thinking)});
        }
        if (!m.content.empty()) {
            p.push_back(part_view{part_kind::TEXT, kimix::string_view(m.content)});
        }
        kimix::vector<tool_call_view> t;
        t.reserve(m.tool_calls.size());
        for (const kimix::llm::ToolCall &call : m.tool_calls) {
            t.push_back(tool_call_view{kimix::string_view(call.id),
                                       kimix::string_view(call.name),
                                       kimix::string_view(call.arguments)});
        }
        message_view v;
        v.role = role_code(m.role);
        v.tool_call_id = kimix::string_view(m.tool_call_id);
        parts.push_back(std::move(p));
        tcs.push_back(std::move(t));
        v.parts = kimix::span<const part_view>(parts.back());
        v.tool_calls = kimix::span<const tool_call_view>(tcs.back());
        views.push_back(v);
    }
    return views;
}

} // namespace

int main() {
    using namespace boost::ut::literals;

    // =======================================================================
    // B5: the meta records keep the reference's byte shapes (jsonl backend)
    // =======================================================================
    "meta_system_prompt_record_shape"_test = [] {
        const kimix::string ws = ws_dir("meta_prompt_shape");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "meta-shape", false, error)) << error;
        // The jsonl branch (no context.db yet): the record is PREPENDED.
        expect(store.set_system_prompt("You are Kimi.", error)) << error;
        const kimix::string first =
            file_text(cli::join_path(store.dir(), "context.jsonl"));
        expect(has_substr(first,
                          "{\"role\":\"_system_prompt\",\"content\":\"You are "
                          "Kimi.\"}"))
            << first;
        // Content-addressed: an unchanged prompt is a no-op (the file is not
        // rewritten, so the store stays byte-stable).
        const kimix::string before = first;
        expect(store.set_system_prompt("You are Kimi.", error)) << error;
        expect(file_text(cli::join_path(store.dir(), "context.jsonl")) == before);
        // A changed prompt replaces the prompt line at the top and keeps the
        // other records.  (The reference's jsonl writer prepends, leaving the
        // stale prompt line in the file - where its own two readers disagree
        // about which one wins: get_system_prompt takes the FIRST record,
        // restore_full the LAST.  The store writes exactly one prompt line so
        // both readers agree on the newest prompt; the DB backend likewise
        // keeps a single row.)
        expect(store.set_system_prompt("You are Kimi v2.", error)) << error;
        const kimix::string second =
            file_text(cli::join_path(store.dir(), "context.jsonl"));
        expect(second.rfind(
                   "{\"role\":\"_system_prompt\",\"content\":\"You are Kimi v2.\"}",
                   0) == 0)
            << second;
        expect(!has_substr(second, "\"content\":\"You are Kimi.\"}")) << second;
        expect(store.stored_system_prompt().has_value());
        expect(store.stored_system_prompt().value() == "You are Kimi v2.");
    };

    "meta_usage_record_shape"_test = [] {
        const kimix::string ws = ws_dir("meta_usage_shape");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "meta-usage", false, error)) << error;
        expect(!store.last_usage().has_value());
        expect(store.record_usage(1234, error)) << error;
        expect(store.record_usage(2345, error)) << error;
        const kimix::string context =
            file_text(cli::join_path(store.dir(), "context.jsonl"));
        expect(has_substr(context, "{\"role\":\"_usage\",\"token_count\":1234}"))
            << context;
        expect(has_substr(context, "{\"role\":\"_usage\",\"token_count\":2345}"))
            << context;
        // get_latest_usage semantics: the LAST snapshot wins.
        expect(store.last_usage().has_value());
        expect(store.last_usage().value() == 2345_i);
        expect(store.meta().usages.size() == 2u);
    };

    // =======================================================================
    // B5: adoption on resume through the DB backend
    // =======================================================================
    "meta_adopted_on_resume"_test = [] {
        const kimix::string ws = ws_dir("meta_resume");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "resume-me", false, error)) << error;
        // A first save migrates the session to context.db (B4).
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("hello there"));
        expect(store.save_history(history, error)) << error;
        // Reading migrates the legacy JSONL to context.db (B4), so the rest
        // of the test runs on the SQLite backend.
        expect(store.load_history(history, error)) << error;
        expect(cli::file_exists(cli::join_path(store.dir(), "context.db")));
        expect(store.set_system_prompt("prompt one", error)) << error;
        expect(store.record_usage(4321, error)) << error;
        expect(store.n_checkpoints() == 0_i);

        // A fresh store (a new process) resumes and adopts the meta.
        cli::session_store resumed;
        expect(resumed.open(ws, "resume-me", true, error)) << error;
        kimix::vector<kimix::llm::Message> loaded;
        expect(resumed.load_history(loaded, error)) << error;
        expect(loaded.size() == 1u);
        expect(resumed.stored_system_prompt().has_value());
        expect(resumed.stored_system_prompt().value() == "prompt one");
        expect(resumed.last_usage().has_value());
        expect(resumed.last_usage().value() == 4321_i);
        // The content-addressed write: the same prompt is a no-op, a changed
        // one is persisted and adopted on the next resume.
        expect(resumed.set_system_prompt("prompt one", error)) << error;
        expect(resumed.set_system_prompt("prompt two", error)) << error;
        cli::session_store again;
        expect(again.open(ws, "resume-me", true, error)) << error;
        kimix::vector<kimix::llm::Message> unused;
        expect(again.load_history(unused, error)) << error;
        expect(again.stored_system_prompt().value() == "prompt two");
    };

    // =======================================================================
    // B8: checkpoints (jsonl backend record shapes + revert)
    // =======================================================================
    "checkpoint_jsonl_roundtrip"_test = [] {
        const kimix::string ws = ws_dir("checkpoint_jsonl");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "cp-jsonl", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("before the checkpoint"));
        expect(store.save_history(history, error)) << error;

        int64_t id = -1;
        expect(store.create_checkpoint(history, /*add_user_message=*/true, id,
                                       error))
            << error;
        expect(id == 0_i);
        expect(store.n_checkpoints() == 1_i);
        // The reference's _checkpoint record + the synthetic marker message.
        const kimix::string context =
            file_text(cli::join_path(store.dir(), "context.jsonl"));
        expect(has_substr(context, "{\"role\":\"_checkpoint\",\"id\":0}"))
            << context;
        expect(has_substr(context, "{\"role\":\"user\",\"content\":\"<system>"
                                  "CHECKPOINT 0</system>\"}"))
            << context;
        expect(history.size() == 2u);
        expect(history.back().role == "user");
        expect(history.back().content == "<system>CHECKPOINT 0</system>");

        // Reverting drops everything from the checkpoint record onwards and
        // restores the in-memory history from the store.
        kimix::vector<kimix::llm::Message> restored;
        expect(store.revert_to(0, restored, error)) << error;
        expect(restored.size() == 1u);
        expect(restored[0].role == "user");
        expect(restored[0].content == "before the checkpoint");
        expect(store.n_checkpoints() == 0_i);
        const kimix::string after =
            file_text(cli::join_path(store.dir(), "context.jsonl"));
        expect(!has_substr(after, "_checkpoint")) << after;
        expect(!has_substr(after, "CHECKPOINT 0</system>")) << after;

        // Context.revert_to's ValueError wording for an unknown id.
        kimix::vector<kimix::llm::Message> none;
        expect(!store.revert_to(7, none, error));
        expect(has_substr(error, "Checkpoint 7 does not exist")) << error;
    };

    // =======================================================================
    // B8: checkpoints are durable in the DB backend
    // =======================================================================
    "checkpoint_db_roundtrip"_test = [] {
        const kimix::string ws = ws_dir("checkpoint_db");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "cp-db", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("root"));
        expect(store.save_history(history, error)) << error;
        expect(store.load_history(history, error)) << error; // migrates to db
        expect(cli::file_exists(cli::join_path(store.dir(), "context.db")));
        expect(store.set_system_prompt("keep me", error)) << error;

        int64_t id = -1;
        expect(store.create_checkpoint(history, /*add_user_message=*/true, id,
                                       error))
            << error;
        expect(id == 0_i);
        history.push_back(user_msg("after the checkpoint"));
        expect(store.save_history(history, error)) << error;

        // Resume: the checkpoint survives the restart and the marker message
        // is part of the persisted history.
        cli::session_store resumed;
        expect(resumed.open(ws, "cp-db", true, error)) << error;
        kimix::vector<kimix::llm::Message> loaded;
        expect(resumed.load_history(loaded, error)) << error;
        expect(loaded.size() == 3u);
        expect(loaded[1].content == "<system>CHECKPOINT 0</system>");
        expect(resumed.n_checkpoints() == 1_i);

        kimix::vector<kimix::llm::Message> restored;
        expect(resumed.revert_to(0, restored, error)) << error;
        expect(restored.size() == 1u);
        expect(restored[0].content == "root");
        expect(resumed.n_checkpoints() == 0_i);
        // A second revert of the same (now dropped) checkpoint refuses.
        kimix::vector<kimix::llm::Message> none;
        expect(!resumed.revert_to(0, none, error));
        expect(has_substr(error, "Checkpoint 0 does not exist")) << error;
    };

    // =======================================================================
    // B10 + B11: lenient parsing and the stale-reminder strip on restore
    // =======================================================================
    "lenient_parse_and_reminder_strip"_test = [] {
        const kimix::string ws = ws_dir("lenient_load");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "lenient", false, error)) << error;
        // One strict-invalid-but-repairable line (trailing comma), one
        // unparseable line, and a stale <system-reminder> user message.
        const kimix::string jsonl =
            "{\"role\":\"user\",\"content\":\"good one\",}\n"
            "totally not json\n"
            "{\"role\":\"assistant\",\"content\":\"good two\"}\n"
            "{\"role\":\"user\",\"content\":\"<system-reminder>stale "
            "nudge</system-reminder>\"}\n";
        expect(cli::write_file(cli::join_path(store.dir(), "context.jsonl"),
                               jsonl, error))
            << error;
        kimix::vector<kimix::llm::Message> loaded;
        expect(store.load_history(loaded, error)) << error;
        // The repairable record survived; the junk line did not take the
        // transcript down; the stale reminder was stripped.
        expect(loaded.size() == 2u) << loaded.size();
        expect(loaded[0].role == "user");
        expect(loaded[0].content == "good one");
        expect(loaded[1].role == "assistant");
        expect(loaded[1].content == "good two");
        for (const kimix::llm::Message &m : loaded) {
            expect(m.content.find("<system-reminder>") == kimix::string::npos);
        }
    };

    "reminder_strip_db_backend"_test = [] {
        const kimix::string ws = ws_dir("reminder_db");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "reminder-db", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("real question"));
        history.push_back(
            user_msg("<system-reminder>ephemeral</system-reminder>"));
        history.push_back(assistant_msg("real answer"));
        expect(store.save_history(history, error)) << error;
        kimix::vector<kimix::llm::Message> loaded;
        expect(store.load_history(loaded, error)) << error;
        expect(loaded.size() == 2u);
        expect(loaded[0].content == "real question");
        expect(loaded[1].content == "real answer");
    };

    // =======================================================================
    // B12: replace_history (only-lower token count) + the restore guard
    // =======================================================================
    "replace_history_resets_and_lowers"_test = [] {
        const kimix::string ws = ws_dir("replace_history");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "replace", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        for (int i = 0; i < 6; ++i) {
            history.push_back(user_msg(kimix::format("question {}", i)));
            history.push_back(assistant_msg(kimix::format("answer {}", i)));
        }
        expect(store.save_history(history, error)) << error;
        expect(store.load_history(history, error)) << error; // migrate to db
        int64_t id = -1;
        expect(store.create_checkpoint(history, false, id, error)) << error;
        expect(store.record_usage(9000, error)) << error;

        // The pruned history is much smaller; the recorded token count may
        // only be LOWERED to the estimate (never raised).
        kimix::vector<kimix::llm::Message> pruned;
        pruned.push_back(user_msg("question 5"));
        pruned.push_back(assistant_msg("answer 5"));
        int64_t token_count = 9000;
        expect(store.replace_history(pruned, /*new_token_estimate=*/300,
                                     token_count, error))
            << error;
        expect(token_count == 300_i);
        expect(store.n_checkpoints() == 0_i);
        expect(!store.last_usage().has_value());
        kimix::vector<kimix::llm::Message> loaded;
        expect(store.load_history(loaded, error)) << error;
        expect(loaded.size() == 2u);
        expect(loaded[0].content == "question 5");

        // A higher estimate is left for the next API usage update to correct.
        int64_t raised = 100;
        expect(store.replace_history(pruned, /*new_token_estimate=*/5000,
                                     raised, error))
            << error;
        expect(raised == 100_i);
    };

    "restore_history_mutation_guard"_test = [] {
        const kimix::string ws = ws_dir("restore_guard");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "guard", false, error)) << error;
        // The first restore of an untouched store loads the context.
        expect(cli::write_file(cli::join_path(store.dir(), "context.jsonl"),
                               "{\"role\":\"user\",\"content\":\"hi\"}\n", error))
            << error;
        kimix::vector<kimix::llm::Message> first;
        expect(store.restore_history(first, error)) << error;
        expect(first.size() == 1u);
        // A second restore is refused with the reference's wording.
        kimix::vector<kimix::llm::Message> second;
        expect(!store.restore_history(second, error));
        expect(error == "The context storage is already modified") << error;

        // A fresh store refuses after an in-process write, too.
        cli::session_store writer;
        expect(writer.open(ws, "guard-2", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("hello"));
        expect(writer.save_history(history, error)) << error;
        kimix::vector<kimix::llm::Message> blocked;
        expect(!writer.restore_history(blocked, error));
        expect(error == "The context storage is already modified") << error;
        // clear_context resets the store, so a restore works again.
        expect(writer.clear_context(error)) << error;
        expect(writer.restore_history(blocked, error));
        expect(blocked.empty());
    };

    // =======================================================================
    // B12: the backend auto-detect helper
    // =======================================================================
    "detect_context_file_backend"_test = [] {
        const kimix::string ws = ws_dir("detect_backend");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "detect", false, error)) << error;
        // Nothing persisted yet.
        expect(cli::session_store::detect_context_file(store.dir()).empty());
        expect(cli::write_file(cli::join_path(store.dir(), "context.jsonl"),
                               "{\"role\":\"user\",\"content\":\"x\"}\n", error))
            << error;
        expect(cli::session_store::detect_context_file(store.dir()) ==
               cli::join_path(store.dir(), "context.jsonl"));
        kimix::vector<kimix::llm::Message> migrated;
        migrated.push_back(user_msg("migrated"));
        expect(store.save_history(migrated, error)) << error;
        // Reading migrates the JSONL to context.db (B4); the store then
        // reports the SQLite file as its backend.
        expect(store.load_history(migrated, error)) << error;
        expect(cli::session_store::detect_context_file(store.dir()) ==
               cli::join_path(store.dir(), "context.db"));
        // A directory left with BOTH files (an interrupted migration): the
        // newer file wins.
        {
            std::FILE *f = std::fopen(
                cli::join_path(store.dir(), "context.jsonl").c_str(), "wb");
            expect(f != nullptr);
            std::fwrite("x\n", 1, 2, f);
            std::fclose(f);
            std::error_code ec;
            kimix::filesystem::last_write_time(
                kimix::filesystem::path(cli::join_path(store.dir(),
                                                       "context.jsonl")),
                kimix::filesystem::file_time_type::clock::now() +
                    std::chrono::hours(1),
                ec);
            expect(!ec);
            expect(cli::session_store::detect_context_file(store.dir()) ==
                   cli::join_path(store.dir(), "context.jsonl"));
        }
    };

    // =======================================================================
    // B9: the structured export (both backends) + the markdown golden
    // =======================================================================
    "export_context_structured"_test = [] {
        const kimix::string ws = ws_dir("export_structured");
        kimix::string error;
        cli::session_store store;
        expect(store.open(ws, "export-db", false, error)) << error;
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("question"));
        history.push_back(assistant_msg("answer"));
        expect(store.save_history(history, error)) << error;
        expect(store.load_history(history, error)) << error; // migrate to db
        expect(store.set_system_prompt("sys prompt", error)) << error;
        expect(store.record_usage(11, error)) << error;
        expect(store.record_usage(22, error)) << error;
        int64_t id = -1;
        expect(store.create_checkpoint(history, false, id, error)) << error;

        cli::exported_context out;
        expect(store.export_context(out, error)) << error;
        expect(out.has_system_prompt);
        expect(out.system_prompt == "sys prompt");
        expect(out.messages.size() == 2u);
        expect(out.messages[0].content == "question");
        expect(out.messages[1].content == "answer");
        expect(out.checkpoints.size() == 1u);
        expect(out.checkpoints[0] == 0_i);
        expect(out.usages.size() == 2u);
        expect(out.usages[0] == 11_i);
        expect(out.usages[1] == 22_i);

        // The jsonl backend exports the same projection from the records.
        cli::session_store jsonl_store;
        expect(jsonl_store.open(ws, "export-jsonl", false, error)) << error;
        expect(jsonl_store.set_system_prompt("sys prompt", error)) << error;
        expect(jsonl_store.record_usage(11, error)) << error;
        expect(jsonl_store.record_usage(22, error)) << error;
        expect(jsonl_store.save_history(history, error)) << error;
        int64_t jsonl_id = -1;
        expect(jsonl_store.create_checkpoint(history, false, jsonl_id, error))
            << error;
        cli::exported_context jout;
        expect(jsonl_store.export_context(jout, error)) << error;
        expect(jout.system_prompt == "sys prompt");
        expect(jout.messages.size() == 2u);
        expect(jout.checkpoints.size() == 1u);
        expect(jout.usages.size() == 2u);
    };

    "export_markdown_golden"_test = [] {
        using namespace kimix::runtime::soul;
        using namespace kimix::runtime::tools;
        // Small fixture session: a user turn, an assistant tool call, the tool
        // result and the final assistant answer.
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("Find the bug in parse"));
        kimix::llm::Message call;
        call.role = "assistant";
        call.thinking = "check file";
        kimix::llm::ToolCall tc;
        tc.id = "c1";
        tc.type = "function";
        tc.name = "read";
        tc.arguments = "{\"path\": \"a.py\"}";
        call.tool_calls.push_back(tc);
        history.push_back(call);
        kimix::llm::Message result;
        result.role = "tool";
        result.tool_call_id = "c1";
        result.content = "def main():\n    pass";
        history.push_back(result);
        history.push_back(assistant_msg("Fixed it."));

        kimix::vector<kimix::vector<part_view>> parts;
        kimix::vector<kimix::vector<tool_call_view>> tcs;
        const kimix::vector<message_view> views = to_views(history, parts, tcs);

        export_options opts;
        opts.session_id = "sess1";
        opts.work_dir = "D:/work";
        opts.exported_at = "2024-05-06T07:08:09";
        opts.token_count = 12345;

        export_context ctx;
        ctx.has_system_prompt = false;
        ctx.messages = kimix::span<const message_view>(views);
        const int64_t checkpoints[1] = {0};
        const int64_t usages[2] = {11, 22};
        ctx.checkpoints = kimix::span<const int64_t>(checkpoints, 1);
        ctx.usages = kimix::span<const int64_t>(usages, 2);

        kimix::string md;
        build_export_context_markdown(ctx, opts, md);
        // Kept next to the fixture for failure diagnosis.
        {
            const kimix::string dump = ws_dir("export_golden_dump");
            std::FILE *f =
                std::fopen((dump + "/actual_export.md").c_str(), "wb");
            if (f != nullptr) {
                std::fwrite(md.data(), 1, md.size(), f);
                std::fclose(f);
            }
        }

        // Byte-exact expectation (build_export_markdown: metadata header,
        // Overview with the comma-grouped token count, logical turns at real
        // user messages, tool-call / collapsible tool-result blocks).
        constexpr kimix::string_view expected =
            "---\n"
            "session_id: sess1\n"
            "exported_at: 2024-05-06T07:08:09\n"
            "work_dir: D:/work\n"
            "message_count: 4\n"
            "token_count: 12345\n"
            "---\n"
            "\n"
            "# Kimi Session Export\n"
            "\n"
            "## Overview\n"
            "\n"
            "- **Topic**: Find the bug in parse\n"
            "- **Conversation**: 1 turns | 1 tool calls | 12,345 tokens\n"
            "---\n"
            "\n"
            "## Turn 1\n"
            "\n"
            "### User\n"
            "\n"
            "Find the bug in parse\n"
            "\n"
            "### Assistant\n"
            "\n"
            "<details><summary>Thinking</summary>\n"
            "\n"
            "check file\n"
            "\n"
            "</details>\n"
            "\n"
            "#### Tool Call: read (`a.py`)\n"
            "<!-- call_id: c1 -->\n"
            "```json\n"
            "{\n"
            "  \"path\": \"a.py\"\n"
            "}\n"
            "```\n"
            "\n"
            "<details><summary>Tool Result: read (`a.py`)</summary>\n"
            "\n"
            "<!-- call_id: c1 -->\n"
            "def main():\n"
            "    pass\n"
            "\n"
            "</details>\n"
            "\n"
            "Fixed it.\n";
        expect(md == expected) << md;
    };

    return 0;
}
