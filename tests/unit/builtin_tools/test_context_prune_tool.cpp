// test_context_prune_tool.cpp - Unit tests for the context_prune agent tool
// (src/builtin_tools/context_prune_tool.*, report.md row D2).
//
// Covers the validity gate (a soul must be bound, the KimiToolset analogue),
// the subagent refusal, the structural validation refusals (verbatim texts),
// the three modes (prune / compact / strip_reasoning), the markdown summary,
// the dry-run idempotency and the history application + prune_N archiving
// through KimiSoul::apply_pruned_history (D5).
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/soul.h>
#include <builtin_tools/context_prune_tool.h>
#include <builtin_tools/tool_registry.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Scripted chat backend (no network): canned answers + request recording.
class FakeBackend : public kimix::agent::IChatBackend {
public:
    // A small window: prune_with_policy's 0.5 target-ratio fallback
    // (max_context * 0.5) must stay below the ~800-token test history or the
    // budget is <= 0 exactly like the reference (same reason the reference
    // tool asks for target_token_count on small contexts).
    int64_t max_context = 1200;
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return max_context; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::llm::Message make_msg(kimix::string_view role, kimix::string_view content) {
    kimix::llm::Message m;
    m.role.assign(role.data(), role.size());
    m.content.assign(content.data(), content.size());
    return m;
}

kimix::string filler(size_t n) { return kimix::string(n, 'x'); }

// 4 head fillers + an oversized tool result at index 4 + 6 tail fillers: the
// only prunable slot under the reference default protected set.
void build_prunable_history(kimix::agent::AgentSession &session) {
    for (int i = 0; i < 4; ++i) {
        session.append_history(make_msg("user", filler(64)));
    }
    kimix::llm::Message tool = make_msg("tool", filler(2600));
    tool.tool_call_id = "c1";
    session.append_history(tool);
    for (int i = 0; i < 6; ++i) {
        session.append_history(make_msg(i % 2 ? "assistant" : "user", filler(64)));
    }
}

kimix::builtin_tools::ToolParams parse_payload(kimix::builtin_tools::Tool &tool) {
    kimix::vector<char> raw;
    tool.result_json(raw);
    kimix::builtin_tools::ToolParams payload;
    kimix::string err;
    expect(payload.try_deserialize(
        kimix::span<char const>(raw.data(), static_cast<int64_t>(raw.size())), err))
        << err;
    return payload;
}

kimix::string payload_string(const kimix::builtin_tools::ToolParams &p,
                             kimix::string_view key) {
    const kimix::builtin_tools::ValueElement *v = p.get(key);
    return (v != nullptr && v->is_string()) ? v->as_string() : kimix::string();
}

bool payload_ok(const kimix::builtin_tools::ToolParams &p) {
    const kimix::builtin_tools::ValueElement *v = p.get("ok");
    return v != nullptr && v->is_bool() && v->as_bool();
}

void set_arg(kimix::builtin_tools::ToolParams &p, kimix::string_view key,
             kimix::string_view value) {
    p.values[kimix::string(key)] =
        kimix::builtin_tools::ValueElement::make_string(kimix::string(value));
}

// const char* overload: without it a string literal would prefer the bool
// overload (const char* -> bool is a standard conversion).
void set_arg(kimix::builtin_tools::ToolParams &p, kimix::string_view key,
             const char *value) {
    p.values[kimix::string(key)] =
        kimix::builtin_tools::ValueElement::make_string(kimix::string(value));
}

void set_arg(kimix::builtin_tools::ToolParams &p, kimix::string_view key, bool value) {
    p.values[kimix::string(key)] = kimix::builtin_tools::ValueElement::make_bool(value);
}

void set_arg(kimix::builtin_tools::ToolParams &p, kimix::string_view key, int64_t value) {
    p.values[kimix::string(key)] = kimix::builtin_tools::ValueElement::make_int(value);
}

} // namespace

int main() {
    "valid_requires_a_bound_soul"_test = [] {
        kimix::agent::AgentSession session;
        kimix::builtin_tools::Session *ts = &session.tool_session();
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        expect(meta != nullptr);
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool = meta->factory(ts);
        expect(tool != nullptr);
        expect(!tool->valid());
        {
            FakeBackend backend;
            kimix::agent::KimiSoul soul(session, backend);
            kimix::unique_ptr<kimix::builtin_tools::Tool> tool2 = meta->factory(ts);
            expect(tool2->valid());
        }
        // The soul cleared the binding on destruction.
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool3 = meta->factory(ts);
        expect(!tool3->valid());
    };

    "prune_mode_dry_run_is_idempotent"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        const size_t before = session.history().size();
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "mode", "prune");
        set_arg(args, "dry_run", true);
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams payload = parse_payload(*tool);
        expect(payload_ok(payload));
        const kimix::string output = payload_string(payload, "output");
        expect(output.find("**Dry run** \xE2\x80\x94 session unchanged.\n\n- **Mode:** prune") ==
               0u)
            << output;
        expect(output.find("- **Estimated tokens freed:**") != kimix::string::npos);
        expect(output.find("- **Elided references:** `prune_") != kimix::string::npos);
        expect(payload_string(payload, "brief") == "Dry run complete");
        // Session unchanged.
        expect(session.history().size() == before);
        expect(!kimix::agent::is_pruned_stub(session.history()[4]));
    };

    "prune_mode_applies_and_archives"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "mode", "prune");
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams payload = parse_payload(*tool);
        expect(payload_ok(payload));
        expect(payload_string(payload, "brief") == "Context pruned");
        const kimix::string output = payload_string(payload, "output");
        expect(output.find("context_prune (prune) applied.\n\n") == 0u) << output;
        // The history now carries the stub in place of the tool output...
        expect(kimix::agent::is_pruned_stub(session.history()[4]));
        expect(session.history()[4].tool_call_id == "c1");
        // ...and the archived original resolves through the history index
        // under the prune_N id the summary names.
        expect(output.find("`prune_11`") != kimix::string::npos) << output;
        const kimix::optional<kimix::runtime::index::turn_meta> turn =
            session.history_get_by_id(11);
        expect(turn.has_value());
        expect(turn->text == filler(2600));
    };

    "no_removable_content_answer"_test = [] {
        kimix::agent::AgentSession session;
        // A long all-small history: validation passes, nothing is prunable.
        for (int i = 0; i < 12; ++i) {
            session.append_history(make_msg(i % 2 ? "assistant" : "user", filler(64)));
        }
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        (*tool)(&args); // default mode=prune
        const kimix::builtin_tools::ToolParams payload = parse_payload(*tool);
        expect(payload_ok(payload));
        expect(payload_string(payload, "output") ==
               "context_prune (prune): no removable content found.");
        expect(payload_string(payload, "brief") == "Nothing to prune");
    };

    "validation_refusals"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        // keep_recent_turns too large.
        {
            kimix::builtin_tools::ToolParams args;
            set_arg(args, "keep_recent_turns", int64_t{20});
            (*tool)(&args);
            const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
            expect(!payload_ok(p));
            expect(payload_string(p, "brief") == "Invalid keep_recent_turns");
            expect(payload_string(p, "message").find("are protected as a stable prefix.") !=
                   kimix::string::npos);
        }
        // target_token_count below the ge=1000 bound.
        {
            kimix::builtin_tools::ToolParams args;
            set_arg(args, "target_token_count", int64_t{500});
            (*tool)(&args);
            const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
            expect(!payload_ok(p));
            expect(payload_string(p, "message").find("target_token_count must be >= 1000") ==
                   0u);
        }
        // keep_recent_turns out of the 1..20 range.
        {
            kimix::builtin_tools::ToolParams args;
            set_arg(args, "keep_recent_turns", int64_t{0});
            (*tool)(&args);
            const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
            expect(!payload_ok(p));
            expect(payload_string(p, "message").find("between 1 and 20") !=
                   kimix::string::npos);
        }
    };

    "refuses_single_user_assistant_pair"_test = [] {
        kimix::agent::AgentSession session;
        // 11 messages but only ONE user/assistant pair (system + tool padding)
        // so the keep_recent_turns validation passes and the pair check fires.
        for (int i = 0; i < 4; ++i) {
            session.append_history(make_msg("system", filler(64)));
        }
        session.append_history(make_msg("user", "hello"));
        session.append_history(make_msg("assistant", "hi"));
        for (int i = 0; i < 5; ++i) {
            kimix::llm::Message t = make_msg("tool", filler(64));
            t.tool_call_id = "c";
            session.append_history(t);
        }
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
        expect(!payload_ok(p));
        expect(payload_string(p, "brief") == "History too short");
        expect(payload_string(p, "message").find("only one user/assistant pair") !=
               kimix::string::npos);
    };

    "target_below_protected_prefix_refused"_test = [] {
        kimix::agent::AgentSession session;
        // 10 protected messages of 512 chars (~128 tokens each): the
        // protected set (~1280 tokens) exceeds any allowed target.
        for (int i = 0; i < 4; ++i) {
            session.append_history(make_msg("user", filler(512)));
        }
        {
            kimix::llm::Message t = make_msg("tool", filler(512));
            t.tool_call_id = "c1";
            session.append_history(t);
        }
        for (int i = 0; i < 6; ++i) {
            session.append_history(make_msg(i % 2 ? "assistant" : "user", filler(512)));
        }
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "target_token_count", int64_t{1000});
        // The protected prefix/recent turns exceed the target.
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
        expect(!payload_ok(p));
        expect(payload_string(p, "brief") == "Target too low");
        expect(payload_string(p, "message").find("Increase the target or reduce "
                                                 "keep_recent_turns.") !=
               kimix::string::npos);
    };

    "strip_reasoning_mode"_test = [] {
        kimix::agent::AgentSession session;
        for (int i = 0; i < 4; ++i) {
            session.append_history(make_msg("user", filler(64)));
        }
        kimix::llm::Message old_a = make_msg("assistant", filler(64));
        old_a.thinking = filler(400); // outside the protected tail
        session.append_history(old_a);
        for (int i = 0; i < 6; ++i) {
            kimix::llm::Message m = make_msg(i % 2 ? "assistant" : "user", filler(64));
            if (i == 1) {
                m.thinking = filler(300); // protected tail thinking
            }
            session.append_history(m);
        }
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "mode", "strip_reasoning");
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams payload = parse_payload(*tool);
        expect(payload_ok(payload));
        expect(payload_string(payload, "brief") == "Context pruned");
        // Index 4 is outside the protected set: thinking stripped; the tail
        // thinking (index 6, inside the protected window) survives.
        expect(session.history()[4].thinking.empty());
        expect(session.history()[6].thinking == filler(300));
    };

    "compact_mode_dry_run_and_delegate"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "mode", "compact");
        set_arg(args, "dry_run", true);
        (*tool)(&args);
        kimix::builtin_tools::ToolParams payload = parse_payload(*tool);
        expect(payload_ok(payload));
        expect(payload_string(payload, "output").find(
                   "Mode 'compact' would invoke the compaction subsystem.") !=
               kimix::string::npos);
        expect(payload_string(payload, "brief") == "Compact dry run");
    };

    "subagent_gate_refusal"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        session.tool_session().is_sub_agent = true;
        kimix::agent::KimiSoul::options opts;
        opts.loop_control.prune_subagents = false;
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend, opts);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
        expect(!payload_ok(p));
        expect(payload_string(p, "brief") == "Subagent pruning disabled");
        expect(payload_string(p, "message").find(
                   "Enable loop_control.prune_subagents or run from the root "
                   "session.") != kimix::string::npos);
    };

    "invalid_mode_rejected"_test = [] {
        kimix::agent::AgentSession session;
        build_prunable_history(session);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::builtin_tools::ToolMeta *meta =
            kimix::builtin_tools::ToolRegistry::instance().find_ci("context_prune");
        kimix::unique_ptr<kimix::builtin_tools::Tool> tool =
            meta->factory(&session.tool_session());
        kimix::builtin_tools::ToolParams args;
        set_arg(args, "mode", "destroy");
        (*tool)(&args);
        const kimix::builtin_tools::ToolParams p = parse_payload(*tool);
        expect(!payload_ok(p));
        expect(payload_string(p, "status") == "invalid_input");
    };

    return 0;
}
