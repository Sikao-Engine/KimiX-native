// test_retrieve_sqlite_index.cpp - Retrieve-tool-level test proving the
// injected HistoryIndexView can be served by the durable SQLite FTS5 history
// index (src/runtime/index/sqlite_history_index.*, report.md section D rows
// D3/D4/D7): the view closures here mirror AgentSession's wiring in
// src/agent/soul.cpp (get_tool "retrieve"), backed by a real history.db on
// disk instead of the in-memory kernel or stub closures.
//
// Covers:
// - query mode: run_retrieve resolves through SQLite and returns the RAW
//   verbatim text (D7) in the formatted markdown
// - id mode: "prune_N" references resolve against SQLite-assigned turn ids
//   (D6 authority)
// - the Retrieve Tool wrapper with the SQLite-backed view serializes ok
//
// All test logic lives in main() scope; no file-scope static registrations.

#include "ut/ut.hpp"

#include "builtin_tools/retrieve_tool.h"

#include <core/stl/filesystem.h>
#include <runtime/index/sqlite_history_index.h>

#include "agent/soul.h"

#include <optional>
#include <system_error>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::retrieve;

namespace {

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_retrieve_sqlite_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

bool json_contains(const kimix::vector<char> &json, kimix::string_view needle) {
    return kimix::string_view(json.data(), json.size()).find(needle) !=
           kimix::string_view::npos;
}

ToolParams make_params(const std::optional<std::string> &query,
                       const std::optional<std::string> &id) {
    ToolParams params;
    if (query.has_value()) {
        params.values["query"] = ValueElement::make_string(kix(query.value()));
    }
    if (id.has_value()) {
        params.values["id"] = ValueElement::make_string(kix(id.value()));
    }
    return params;
}

// The same dispatch AgentSession::get_tool wires for "retrieve" (soul.cpp):
// plain search from the index, tool-side recency boost; get_by_id through
// parse_turn_reference.
HistoryIndexView make_sqlite_view(kimix::runtime::index::SqliteHistoryIndex *index) {
    HistoryIndexView view;
    view.search_with_recency =
        [index](kimix::string_view query,
                int32_t top_k) -> kimix::vector<history_turn> {
        kimix::vector<history_turn> out;
        const kimix::vector<kimix::runtime::index::turn_meta> found =
            index->search(query, static_cast<uint32_t>(top_k));
        out.reserve(found.size());
        for (const kimix::runtime::index::turn_meta &t : found) {
            history_turn ht;
            ht.turn_id = static_cast<int64_t>(t.turn_id);
            ht.role = t.role == 0   ? "user"
                      : t.role == 1 ? "assistant"
                      : t.role == 2 ? "tool"
                                    : "other";
            ht.text = t.text;
            ht.timestamp = t.timestamp;
            ht.score = t.score;
            ht.is_compacted = t.is_compacted;
            ht.boosted_score = t.score;
            out.push_back(std::move(ht));
        }
        return out;
    };
    view.get_by_id =
        [index](kimix::string_view ref) -> kimix::optional<history_turn> {
        const int64_t id = parse_turn_reference(ref);
        if (id < 0) {
            return kimix::optional<history_turn>();
        }
        const auto turn = index->get_by_id(static_cast<uint32_t>(id));
        if (!turn.has_value()) {
            return kimix::optional<history_turn>();
        }
        history_turn ht;
        ht.turn_id = static_cast<int64_t>(turn->turn_id);
        ht.role = turn->role == 0   ? "user"
                  : turn->role == 1 ? "assistant"
                  : turn->role == 2 ? "tool"
                                    : "other";
        ht.text = turn->text;
        ht.timestamp = turn->timestamp;
        ht.score = turn->score;
        ht.is_compacted = turn->is_compacted;
        ht.boosted_score = turn->score;
        return ht;
    };
    return view;
}

} // namespace

int main() {
    // -----------------------------------------------------------------------
    // Query mode: the SQLite-backed view resolves through run_retrieve and
    // returns the raw verbatim text (D7).
    // -----------------------------------------------------------------------
    "sqlite_view_query_mode_returns_raw_text"_test = [] {
        const auto dir = test_dir("query_mode");
        kimix::runtime::index::SqliteHistoryIndex index(dir / "history.db");
        kimix::string err;
        expect(index.open(err)) << err.c_str();
        // Mixed casing + CJK must survive retrieval byte-exact.
        const kimix::string raw =
            "Check Src/Main.cpp APIToken \xe6\x9f\xa5\xe8\xaf\xa2\xe7\xbb\x93"
            "\xe6\x9e\x9c";
        kimix::runtime::index::turn_meta t;
        t.timestamp = 1000000.0;
        t.role = 1;
        t.text = raw;
        const kimix::runtime::index::turn_meta turns[1] = {t};
        index.append_turns(kimix::span<const kimix::runtime::index::turn_meta>(turns, 1));

        const HistoryIndexView view = make_sqlite_view(&index);
        retrieve_params params;
        params.query = kix("APIToken");
        retrieve_result result;
        const auto st = run_retrieve(params, view, 1000000.0, result);
        expect(st == tool_status::ok);
        expect(result.message == kix("Found 1 result(s)"));
        expect(result.output.find(raw) != kimix::string::npos)
            << "the raw verbatim text must appear in the markdown";
        expect(result.output.find("**assistant** [current]") !=
               kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // Id mode: "prune_N" resolves against the index-owned turn ids (D6).
    // -----------------------------------------------------------------------
    "sqlite_view_id_mode_resolves_prune_ref"_test = [] {
        const auto dir = test_dir("id_mode");
        kimix::runtime::index::SqliteHistoryIndex index(dir / "history.db");
        kimix::string err;
        expect(index.open(err)) << err.c_str();
        kimix::runtime::index::turn_meta t;
        t.timestamp = 1000000.0;
        t.role = 0;
        t.text = kix("archived original content");
        const kimix::runtime::index::turn_meta turns[1] = {t};
        index.append_turns(kimix::span<const kimix::runtime::index::turn_meta>(turns, 1));

        const HistoryIndexView view = make_sqlite_view(&index);
        retrieve_params params;
        params.id = kix("prune_0"); // index-assigned first turn id
        retrieve_result result;
        const auto st = run_retrieve(params, view, 1000000.0, result);
        expect(st == tool_status::ok);
        expect(result.message == kix("Found turn id='prune_0'"));
        expect(result.output.find("archived original content") !=
               kimix::string::npos);

        // A miss is still ToolOk-shaped.
        retrieve_params miss;
        miss.id = kix("prune_42");
        retrieve_result miss_result;
        expect(run_retrieve(miss, view, 1000000.0, miss_result) ==
               tool_status::ok);
        expect(miss_result.output == kix("No turn found with id='prune_42'."));
    };

    // -----------------------------------------------------------------------
    // The Retrieve Tool wrapper with the SQLite-backed view.
    // -----------------------------------------------------------------------
    "retrieve_tool_with_sqlite_view_serializes_ok"_test = [] {
        const auto dir = test_dir("tool_wrapper");
        kimix::runtime::index::SqliteHistoryIndex index(dir / "history.db");
        kimix::string err;
        expect(index.open(err)) << err.c_str();
        kimix::runtime::index::turn_meta t;
        t.timestamp = 1000000.0;
        t.role = 0;
        t.text = kix("durable sqlite recall works");
        const kimix::runtime::index::turn_meta turns[1] = {t};
        index.append_turns(kimix::span<const kimix::runtime::index::turn_meta>(turns, 1));

        kimix::builtin_tools::Session session;
        Retrieve tool(&session);
        tool.view = make_sqlite_view(&index);
        expect(tool.valid()) << "a view is injected, so the tool is offered";
        const auto params = make_params("sqlite recall", std::nullopt);
        kimix::builtin_tools::tool_invoke(tool, &params);
        const auto &json = tool.serialized_result();
        expect(json_contains(json, "\"ok\":true"));
        expect(json_contains(json, "Found 1 result(s)"));
        expect(json_contains(json, "durable sqlite recall works"));
    };

    // -----------------------------------------------------------------------
    // D4(durable): the AgentSession hooks — open_history_index swaps the
    // session onto the durable store; append_history / on_history_compacted /
    // history_search / history_get_by_id serve from SQLite.
    // -----------------------------------------------------------------------
    "agent_session_durable_index_hooks"_test = [] {
        const auto dir = test_dir("session_hooks");
        kimix::agent::AgentSession session;
        kimix::string err;
        expect(!session.has_durable_history_index());
        const kimix::string db_path = kimix::to_string(dir / "history.db");
        expect(session.open_history_index(kimix::string_view(db_path), err))
            << err.c_str();
        expect(session.has_durable_history_index());

        kimix::llm::Message m;
        m.role = "user";
        m.content = kix("Remember The Token AB12cd");
        session.append_history(m);
        expect(session.history_index().turn_count() == 0_u)
            << "the in-memory fallback stays empty in durable mode";

        // The dispatch serves from SQLite with raw verbatim text (D7).
        const auto found = session.history_search("AB12cd", 3);
        expect(found.size() == 1_u);
        expect(found[0].text == m.content);
        expect(found[0].turn_id == 0_u) << "index-owned turn id (D6)";
        const auto by_id = session.history_get_by_id(0);
        expect(by_id.has_value());
        expect(by_id->text == m.content);

        // Post-compaction hook: durable mark_compacted + re-index.
        session.on_history_compacted();
        expect(session.history_get_by_id(0).value().is_compacted);
        expect(session.history_get_by_id(1).has_value())
            << "the re-indexed history turn gets a fresh id";
        expect(session.history_get_by_id(1).value().text == m.content);

        session.close_history_index();
        expect(!session.has_durable_history_index());
    };

    return 0;
}
