// test_subagent_session_dir.cpp - Sub-agent session scratch dirs
// (src/agent/agent_host.cpp install_subagent_runner):
//
//   * a sub-agent session persists its state under
//     <work_dir>/.kimix_cache/<session_id> - a temp dir next to the other
//     .kimix_cache data (todo state, the compaction ledger cache, ...);
//   * when the request is marked anonymous (a fresh session without a
//     caller-chosen id, agent_tool's Agent._resolve_session parity) and the
//     session closes after the run, the temp dir is deleted;
//   * a named session (caller-chosen id) keeps its dir on close so a later
//     resume finds the persisted state, and an anonymous session that stays
//     open (close_session=false) keeps its dir as well.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: a scripted fake
// chat backend answers every sub-agent step.
#include "ut/ut.hpp"

#include <system_error>

#include <core/kimix_core.h>

#include "agent/agent_host.h"
#include "agent/soul.h"
#include "builtin_tools/agent_tool.h"

#include <cstdio>

namespace {
using namespace boost::ut;

// A fresh, empty workspace: <temp>/<name>, removed and recreated.
kimix::string ws_dir(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

bool path_exists(const kimix::string &path) {
    std::error_code ec;
    return kimix::filesystem::exists(kimix::filesystem::path(path), ec) && !ec;
}

// Scripted chat backend: one canned text result per chat() call.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck *) override {
        kimix::llm::ChatResult result;
        result.ok = true;
        result.content = "sub-agent done";
        if (on_chunk) {
            kimix::llm::Chunk chunk;
            chunk.ok = true;
            chunk.content = result.content;
            on_chunk(chunk);
        }
        return result;
    }
    int64_t max_context_size() const override { return 100000; }
    kimix::string model_name() const override { return "fake-model"; }
};

// Run one sub-agent turn through the production runner against `work`.
kimix::builtin_tools::agents::subagent_run_result
run_subagent(kimix::agent::IChatBackend &backend, const kimix::string &work,
             const kimix::string &session_id, bool anonymous,
             bool close_session) {
    kimix::builtin_tools::agents::agent_registry registry;
    kimix::agent::KimiSoul::options opts;
    kimix::agent::install_subagent_runner(registry, backend, opts);
    kimix::builtin_tools::agents::subagent_request req;
    req.session_id = session_id;
    req.prompt = "do the task";
    req.work_dir = work;
    req.close_session = close_session;
    req.anonymous = anonymous;
    return registry.runner(req);
}

} // namespace

int main() {
    // Anonymous session that closes: the scratch temp dir under
    // .kimix_cache is deleted with the session.
    "anonymous_closed_session_dir_deleted"_test = [] {
        FakeBackend backend;
        const kimix::string work = ws_dir("subagent_dir_anon");
        const kimix::string id = "anon-session-id";
        const kimix::string dir = work + "/.kimix_cache/" + id;
        const auto outcome = run_subagent(backend, work, id,
                                          /*anonymous=*/true,
                                          /*close_session=*/true);
        expect(outcome.ok);
        expect(!path_exists(dir)) << "temp dir must be deleted: " << dir;
        expect(!path_exists(work + "/.kimix_cache/" + id)) << dir;
    };

    // Named session (caller-chosen id): the temp dir survives the close so a
    // later resume finds the persisted state.
    "named_session_dir_kept_on_close"_test = [] {
        FakeBackend backend;
        const kimix::string work = ws_dir("subagent_dir_named");
        const kimix::string id = "named-session-id";
        const kimix::string dir = work + "/.kimix_cache/" + id;
        const auto outcome = run_subagent(backend, work, id,
                                          /*anonymous=*/false,
                                          /*close_session=*/true);
        expect(outcome.ok);
        expect(path_exists(dir)) << "temp dir must survive: " << dir;
        // The session state is actually persisted there: the compaction
        // ledger cache lives at <state_dir>/.kimix_cache.
        expect(path_exists(dir + "/.kimix_cache"))
            << "state must be saved under the temp dir: " << dir;
    };

    // Anonymous session that stays open (close_session=false): the session
    // is still alive, so its temp dir must survive the run.
    "anonymous_open_session_dir_kept"_test = [] {
        FakeBackend backend;
        const kimix::string work = ws_dir("subagent_dir_open");
        const kimix::string id = "open-anon-session-id";
        const kimix::string dir = work + "/.kimix_cache/" + id;
        const auto outcome = run_subagent(backend, work, id,
                                          /*anonymous=*/true,
                                          /*close_session=*/false);
        expect(outcome.ok);
        expect(path_exists(dir)) << "open session keeps its dir: " << dir;
        // Clean up what the runner intentionally left behind.
        std::error_code ec;
        kimix::filesystem::remove_all(kimix::filesystem::path(work), ec);
    };

    return 0;
}
