// agent/agent_host.cpp - see agent_host.h.

#include "agent/agent_host.h"

#include <mutex>

#include <core/clock.h>

namespace kimix::agent {

namespace {

// Serializes chat calls when the parent turn is mid-request on the same
// backend (background runs execute on a worker thread, the reference's
// asyncio tasks interleave only at awaits - the C++ analogue is a mutex).
class SerializedBackend : public IChatBackend {
public:
    explicit SerializedBackend(IChatBackend &inner) : _inner(inner) {}

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        std::lock_guard<std::mutex> g(_mutex);
        return _inner.chat(messages, tools, on_chunk);
    }
    int64_t max_context_size() const override { return _inner.max_context_size(); }
    kimix::string model_name() const override { return _inner.model_name(); }
    void set_output_token_budget(int64_t tokens) override {
        _inner.set_output_token_budget(tokens);
    }
    int64_t output_token_budget() const override {
        return _inner.output_token_budget();
    }
    void set_abort_check(const kimix::llm::AbortCheck *check) override {
        _inner.set_abort_check(check);
    }
    kimix::string provider_name() const override { return _inner.provider_name(); }
    kimix::string thinking_effort() const override { return _inner.thinking_effort(); }
    bool generation_temperature_top_p(double &temperature, double &top_p) const override {
        return _inner.generation_temperature_top_p(temperature, top_p);
    }

private:
    IChatBackend &_inner;
    std::mutex _mutex;
};

} // namespace

void install_subagent_runner(builtin_tools::agents::agent_registry &registry,
                             IChatBackend &backend,
                             const KimiSoul::options &opts_template,
                             Approval *approval) {
    struct host_state {
        explicit host_state(IChatBackend &inner)
            : serialized(new SerializedBackend(inner)) {}
        kimix::unique_ptr<SerializedBackend> serialized;
        KimiSoul::options opts;
    };
    kimix::shared_ptr<host_state> state =
        kimix::shared_ptr<host_state>(new host_state(backend));
    state->opts = opts_template;
    registry.runner =
        [state, &registry, approval](const builtin_tools::agents::subagent_request &req)
        -> builtin_tools::agents::subagent_run_result {
            using namespace builtin_tools::agents;
            subagent_run_result out;
            AgentSession child;
            child.tool_session().session_id = req.session_id;
            child.tool_session().is_sub_agent = true;
            KimiSoul soul(child, *state->serialized, state->opts);
            if (approval != nullptr) {
                // G1-G4: the child shares the parent's gate and decision
                // state (approval.py share()); a grant on either side feeds
                // the same persisted set.
                soul.set_approval(approval);
            }
            // G7: a send_message pushed while this child runs is drained here
            // between steps (and wakes mid-stream on request_steer).
            kimix::string child_id = req.session_id;
            soul.set_external_steers(
                [&registry, child_id]() { return registry.drain_steer(child_id); });
            // G8: interrupt_agent's cancel flag rides into the child turn.
            CancelToken cancel;
            cancel.chain(req.cancel);
            const TurnResult result = soul.turn(req.prompt, {}, cancel);
            const double now = kimix::Clock::now_ms() / 1000.0;
            for (const kimix::llm::Message &m : child.history()) {
                conversation_turn t;
                t.role = m.role;
                t.content = m.content;
                t.timestamp = now;
                out.turns.push_back(std::move(t));
            }
            out.cancelled = result.cancelled;
            out.ok = result.ok;
            if (result.cancelled) {
                out.error = "cancelled by interrupt_agent";
            } else if (!result.ok && !result.ignored) {
                out.error = result.error;
            } else {
                out.output = result.content.empty() ? "(no text output)"
                                                    : result.content;
            }
            return out;
        };
}

} // namespace kimix::agent
