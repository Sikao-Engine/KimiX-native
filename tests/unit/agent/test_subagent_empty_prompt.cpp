// test_subagent_empty_prompt.cpp - Sub-agent empty prompt (pass-7 finding
// F-new-24, src/agent/agent_host.cpp install_subagent_runner):
//
// * the soul's empty-input guard answers an empty prompt with
// TurnResult{ignored=true} and no LLM call - nothing failed;
// * the runner must map that to a SUCCESS outcome ("(no text output)"),
// not to ok=false with an empty error, which agent_tool used to render as
// a bare "<system>ERROR: </system>" prefix despite the valid session id.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: a scripted fake
// chat backend would answer any sub-agent step (none is expected here).
#include "ut/ut.hpp"

#include <core/kimix_core.h>
#include "agent/agent_host.h"
#include "agent/soul.h"
#include "builtin_tools/agent_tool.h"

namespace {
using namespace boost::ut;

// Scripted chat backend: any chat() call is a test failure - an empty
// prompt must never reach the model.
class NoChatBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &,
         const kimix::llm::AbortCheck *) override {
        ++calls;
        kimix::llm::ChatResult result;
        result.ok = false;
        result.error = "empty prompt must not reach the backend";
        return result;
    }
    int64_t max_context_size() const override { return 100000; }
    kimix::string model_name() const override { return "fake-model"; }
    int calls = 0;
};
} // namespace

int main() {
    // Empty prompt: the guard ignores the turn (no chat call), and the
    // runner reports a successful no-op with the "(no text output)" body -
    // exactly the reference's ToolOk shape for this call.
    "subagent_empty_prompt_is_successful_noop"_test = [] {
        NoChatBackend backend;
        kimix::builtin_tools::agents::agent_registry registry;
        kimix::agent::KimiSoul::options opts;
        kimix::agent::install_subagent_runner(registry, backend, opts);
        kimix::builtin_tools::agents::subagent_request req;
        req.session_id = "empty-prompt-session";
        req.prompt = "";
        req.work_dir = ".";
        req.close_session = true;
        req.anonymous = true;
        const kimix::builtin_tools::agents::subagent_run_result outcome =
            registry.runner(req);
        expect(backend.calls == 0) << "no LLM call for an empty prompt";
        expect(outcome.ok) << "an ignored (empty) turn is a success, not a "
                              "failure with an empty error message";
        expect(!outcome.cancelled);
        expect(outcome.error.empty());
        expect(outcome.output == "(no text output)");
    };
    // Whitespace-only prompt: same guard, same contract.
    "subagent_whitespace_prompt_is_successful_noop"_test = [] {
        NoChatBackend backend;
        kimix::builtin_tools::agents::agent_registry registry;
        kimix::agent::KimiSoul::options opts;
        kimix::agent::install_subagent_runner(registry, backend, opts);
        kimix::builtin_tools::agents::subagent_request req;
        req.session_id = "blank-prompt-session";
        req.prompt = "   \n\t  ";
        req.work_dir = ".";
        req.close_session = true;
        req.anonymous = true;
        const kimix::builtin_tools::agents::subagent_run_result outcome =
            registry.runner(req);
        expect(backend.calls == 0);
        expect(outcome.ok);
        expect(outcome.output == "(no text output)");
    };
}
