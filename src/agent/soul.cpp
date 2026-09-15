// agent/soul.cpp - KimiSoul implementation (see soul.h).
//
// Mirrors the essential control flow of kimi_cli/soul/kimisoul.py:
//   * turn() = the step loop (chat -> tool calls -> tool results -> repeat),
//     with the auto-compaction check before every step (should_auto_compact)
//   * tool-call arguments go through json repair (kimix::repair, the same
//     kernel LLM::chat already applies - re-applied defensively here) and
//     ToolParams::try_deserialize; failures surface as error tool messages
//     instead of aborting the turn
//   * compact_context() = SimpleCompaction.compact: slice the history on a
//     balanced boundary (never split an assistant tool_calls message from its
//     tool results), assemble the compaction prompt through the
//     builtin_tools::compact kernels + the ported compact.md body, summarize
//     with one tool-less LLM call, and replace the compacted head with the
//     summary message.

#include "agent/soul.h"

#include <atomic>
#include <utility>

#include <core/clock.h>
#include <core/json_repair.h>

#include "builtin_tools/compact_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool_registry.h"

namespace kimix::agent {

namespace {

// Hard cap for one tool result injected into the history (the builtin tools
// already bound themselves; this is the last-resort guard).
constexpr size_t k_max_tool_result_chars = 100000;

kimix::string soul_default_system_prompt() {
    return kimix::string(
        "You are Kimi, an AI assistant running natively inside a C++ agent "
        "runtime on the user's machine.\n"
        "# Environment\n"
        "- You can read/write files, run bash commands (native POSIX syntax), "
        "search the workspace with grep/glob, and execute Python.\n"
        "- The working directory is the session work_dir; relative paths "
        "resolve against it.\n"
        "- Track multi-step work with the TodoWrite/TodoUpdate tools; the "
        "todo list persists with the session.\n"
        "# Rules\n"
        "- Persist until the requirement is met; prefer acting over asking.\n"
        "- Verify your work: run builds/tests before declaring done.\n"
        "- Call tools with exact parameter names from their schemas.\n"
        "- When the context grows large, the runtime compacts it "
        "automatically; keep working from the summary.\n");
}

// Port of kimi_cli/prompts/compact.md (condensed): the instruction appended
// after the flattened conversation that the summarization call must follow.
kimix::string soul_prompt_compact() {
    return kimix::string(
        "Compact the above agent conversation context. Very detailed, "
        "comprehensive.\n"
        "**What to keep (ordered by priority):**\n"
        "1. **Current Task State** - what is being worked on right now, plus "
        "any user-supplied custom instructions, preferences, or constraints "
        "for future turns.\n"
        "2. **Errors & Solutions** - preserve the full error message and the "
        "final working solution; summarize intermediate steps as a brief "
        "narrative.\n"
        "3. **Code State** - final working versions only (drop intermediate "
        "attempts).\n"
        "4. **Design Decisions** - architectural choices and rationale.\n"
        "5. **Environment** - OS, work directory, key dependencies.\n"
        "6. **TODO Items** - unfinished tasks and known issues.\n"
        "7. **Key Decisions / Risks / Important Files / Architecture / "
        "Dependencies / Technical Notes** - as applicable.\n"
        "**What to remove or condense:**\n"
        "- Drop redundant explanations, failed intermediate attempts (retain "
        "lessons learned), verbose comments, conversational filler.\n"
        "- Merge similar discussions into single summary points.\n"
        "- Condense code: keep full version if <= 20 lines; otherwise keep "
        "signature + key logic only.\n"
        "**Length:** Aim to reduce the context to approximately 20-30% of the "
        "original length while preserving all essential information.\n"
        "**User Instructions:** Preserve any explicit user preferences, "
        "constraints, or custom compaction instructions for future turns.\n"
        "**Output Structure:**\n"
        "```xml\n"
        "<current_focus>\n[What we're working on now]\n</current_focus>\n"
        "<environment>\n- OS / work dir / key deps\n</environment>\n"
        "<completed_tasks>\n- [Task]: [Brief outcome]\n</completed_tasks>\n"
        "<active_issues>\n- [Issue]: [Status/Next steps]\n</active_issues>\n"
        "<todo>\n- [ ] [Unfinished task]\n</todo>\n"
        "<code_state>\n<file name=\"path\">... key logic ...</file>\n"
        "</code_state>\n"
        "<decisions>\n- [Decision]: [Rationale]\n</decisions>\n"
        "<important_files>\n- [path]: [role]\n</important_files>\n"
        "<important_context>\n- [Crucial information not covered above]\n"
        "</important_context>\n"
        "```\n");
}

kimix::string soul_prompt_compact_cascade() {
    return kimix::string(
        "This is a CASCADING compaction: the conversation below already "
        "contains one or more previous compaction summaries. Merge everything "
        "into ONE updated summary. Keep the same XML structure as the previous "
        "summaries, preserve all still-relevant facts, drop superseded state, "
        "and never lose unfinished tasks or user instructions.\n");
}

// Convert one llm::Message into the compact::message shape (text parts only;
// assistant thinking is carried as a "think" part so the estimator sees it).
builtin_tools::compact::message
soul_to_compact_message(const kimix::llm::Message &m) {
    builtin_tools::compact::message cm;
    cm.role = m.role;
    if (!m.thinking.empty()) {
        builtin_tools::compact::content_part tp;
        tp.type = "think";
        tp.text = m.thinking;
        cm.content.push_back(std::move(tp));
    }
    builtin_tools::compact::content_part tp;
    tp.type = "text";
    tp.text = m.content;
    // Serialize tool calls into the text part so the summarizer sees them.
    for (const kimix::llm::ToolCall &tc : m.tool_calls) {
        tp.text += kimix::string("\n[tool_call] ") + tc.name + "(" +
                   tc.arguments + ")";
    }
    if (!tp.text.empty()) {
        cm.content.push_back(std::move(tp));
    }
    return cm;
}

kimix::llm::Message soul_summary_message(kimix::string_view summary) {
    kimix::llm::Message m;
    m.role = "user";
    m.content =
        "[system-reminder] This session is being continued from a previous "
        "conversation that was compacted. The summary below replaces the "
        "earlier history:\n\n";
    m.content.append(summary.data(), summary.size());
    m.content +=
        "\n\nContinue working from this summary. Do not mention the "
        "compaction to the user unless asked.";
    return m;
}

// Balance the preserve boundary: the preserved tail must never start with a
// `tool` message whose assistant tool_calls message was compacted away (the
// OpenAI wire format rejects orphan tool results). Walk the boundary back
// over any leading tool messages plus their calling assistant message.
size_t soul_balance_preserve_start(
    const kimix::vector<kimix::llm::Message> &history, size_t start) {
    while (start > 0 && history[start].role == "tool") {
        --start;
    }
    // `start` now points at the assistant message that produced the tool
    // results (or earlier); keep it preserved as well.
    if (start > 0 && start < history.size() &&
        !history[start].tool_calls.empty()) {
        // history[start] is the calling assistant message: preserve it too.
        // (start already includes it because the tail begins AT start.)
    }
    return start;
}

} // namespace

// ---------------------------------------------------------------------------
// LLMBackend
// ---------------------------------------------------------------------------

LLMBackend::LLMBackend(kimix::unique_ptr<kimix::llm::LLM> llm)
    : _llm(std::move(llm)) {}

kimix::llm::ChatResult
LLMBackend::chat(const kimix::vector<kimix::llm::Message> &messages,
                 const kimix::vector<kimix::llm::Tool> &tools,
                 const kimix::llm::ChunkCallback &on_chunk) {
    return _llm->chat(messages, tools, on_chunk);
}

int64_t LLMBackend::max_context_size() const {
    const int32_t v = _llm->max_context_size();
    return v > 0 ? v : 128000;
}

kimix::string LLMBackend::model_name() const { return _llm->model_name(); }

// ---------------------------------------------------------------------------
// AgentSession
// ---------------------------------------------------------------------------

AgentSession::AgentSession() : AgentSession(kimix::string()) {}

AgentSession::AgentSession(kimix::string work_dir) {
    // Random 16-hex-char session id (uuid4().hex flavour).
    static std::atomic<uint64_t> seed{
        static_cast<uint64_t>(kimix::Clock::now_ms()) ^ 0x9E3779B97F4A7C15ull};
    uint64_t s = seed.fetch_add(0x9E3779B97F4A7C15ull);
    auto mix = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        if ((i & 7) == 0) {
            mix();
        }
        _id.push_back(kHex[(s >> ((i & 7) * 8)) & 0xF]);
    }
    set_work_dir(std::move(work_dir));
}

void AgentSession::set_work_dir(kimix::string dir) {
    if (dir.empty()) {
        std::error_code ec;
        dir = kimix::to_string(kimix::filesystem::current_path(ec));
    }
    _tool_session.work_dir = dir;
    _tool_session.native_io = true; // real IO for agent-driven sessions
}

void AgentSession::set_state_dir(kimix::string dir) {
    _tool_session.state_dir = std::move(dir);
}

bool AgentSession::save_state(kimix::string &error) const {
    if (_tool_session.state_dir.empty()) {
        error = "no state_dir set on the session";
        return false;
    }
    const kimix::string path =
        builtin_tools::todo::state_file_path(_tool_session.state_dir);
    if (_tool_session.todo_state) {
        return builtin_tools::todo::save_state_file(path,
                                                    *_tool_session.todo_state,
                                                    error);
    }
    const builtin_tools::todo::todo_state empty;
    return builtin_tools::todo::save_state_file(path, empty, error);
}

bool AgentSession::load_state(kimix::string &error) {
    if (_tool_session.state_dir.empty()) {
        error = "no state_dir set on the session";
        return false;
    }
    builtin_tools::todo::todo_state loaded;
    const kimix::string path =
        builtin_tools::todo::state_file_path(_tool_session.state_dir);
    if (!builtin_tools::todo::load_state_file(path, loaded, error)) {
        return false;
    }
    if (!_tool_session.todo_state) {
        _tool_session.todo_state =
            kimix::shared_ptr<builtin_tools::todo::todo_state>(
                new builtin_tools::todo::todo_state());
    }
    *_tool_session.todo_state = std::move(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// KimiSoul
// ---------------------------------------------------------------------------

KimiSoul::KimiSoul(AgentSession &session, IChatBackend &backend)
    : KimiSoul(session, backend, options{}) {}

KimiSoul::KimiSoul(AgentSession &session, IChatBackend &backend, options opts)
    : _session(session), _backend(backend), _opts(std::move(opts)) {}

kimix::string KimiSoul::effective_system_prompt() const {
    return _opts.system_prompt.empty() ? soul_default_system_prompt()
                                       : _opts.system_prompt;
}

kimix::vector<kimix::llm::Tool> KimiSoul::tool_definitions() const {
    kimix::vector<kimix::llm::Tool> defs;
    for (const builtin_tools::ToolMeta &meta :
         builtin_tools::ToolRegistry::instance().all()) {
        if (!_opts.enabled_tools.empty()) {
            bool enabled = false;
            for (const kimix::string &want : _opts.enabled_tools) {
                if (want == meta.name) {
                    enabled = true;
                    break;
                }
            }
            if (!enabled) {
                continue;
            }
        }
        kimix::llm::Tool t;
        t.name = meta.name;
        t.description = meta.description;
        t.parameters_json = meta.parameters_json.empty()
                                ? kimix::string(R"({"type":"object"})")
                                : meta.parameters_json;
        defs.push_back(std::move(t));
    }
    return defs;
}

builtin_tools::Tool *KimiSoul::get_tool(kimix::string_view name) {
    const builtin_tools::ToolMeta *meta =
        builtin_tools::ToolRegistry::instance().find_ci(name);
    if (meta == nullptr) {
        return nullptr;
    }
    auto it = _tools.find(meta->name);
    if (it != _tools.end()) {
        return it->second.get();
    }
    kimix::unique_ptr<builtin_tools::Tool> tool =
        meta->factory(&_session.tool_session());
    if (tool == nullptr) {
        return nullptr;
    }
    builtin_tools::Tool *raw = tool.get();
    _tools.emplace(meta->name, std::move(tool));
    return raw;
}

kimix::string KimiSoul::execute_tool_call(kimix::string_view name,
                                          kimix::string_view arguments_json,
                                          kimix::string &error) {
    error.clear();
    builtin_tools::Tool *tool = get_tool(name);
    if (tool == nullptr) {
        kimix::string msg = "unknown tool: ";
        msg.append(name.data(), name.size());
        error = msg;
        return msg;
    }
    // JSON repair + parse (mirrors kimi_cli.tools.utils.repair_json_string +
    // the pydantic model parse; kimix::repair returns "" for valid input).
    kimix::string args(arguments_json);
    // Trim surrounding whitespace.
    size_t b = 0;
    while (b < args.size() &&
           (args[b] == ' ' || args[b] == '\t' || args[b] == '\n' ||
            args[b] == '\r')) {
        ++b;
    }
    size_t e = args.size();
    while (e > b &&
           (args[e - 1] == ' ' || args[e - 1] == '\t' || args[e - 1] == '\n' ||
            args[e - 1] == '\r')) {
        --e;
    }
    args = args.substr(b, e - b);
    if (args.empty()) {
        args = "{}";
    }
    if (args[0] == '{' || args[0] == '[') {
        const kimix::string repaired = kimix::repair(args);
        if (!repaired.empty()) {
            args = repaired;
        }
    }
    builtin_tools::ToolParams params;
    kimix::string parse_error;
    if (!params.try_deserialize(
            kimix::span<char const>(args.data(), args.size()), parse_error)) {
        error = parse_error;
        kimix::string msg = "invalid tool arguments JSON: " + parse_error;
        return msg;
    }
    try {
        (*tool)(&params);
    } catch (const std::exception &ex) {
        error = ex.what();
        kimix::string msg = "tool threw: ";
        msg += ex.what();
        return msg;
    }
    kimix::vector<char> out;
    tool->result_json(out);
    kimix::string result(out.data(), out.size());
    if (result.size() > k_max_tool_result_chars) {
        result.resize(k_max_tool_result_chars);
        result += "\n[... tool result truncated ...]";
    }
    if (result.empty()) {
        result = R"JSON({"status":"ok","message":"(no result payload)"})JSON";
    }
    return result;
}

int64_t KimiSoul::estimated_tokens() const {
    using namespace builtin_tools::compact;
    kimix::vector<message> cms;
    cms.reserve(_session.history().size());
    for (const kimix::llm::Message &m : _session.history()) {
        cms.push_back(soul_to_compact_message(m));
    }
    return estimate_message_tokens(cms);
}

bool KimiSoul::compact_context(kimix::string_view custom_instruction,
                               kimix::string &error) {
    using namespace builtin_tools::compact;
    error.clear();
    const kimix::vector<kimix::llm::Message> &history = _session.history();
    if (history.size() < 4) {
        error = "history too short to compact";
        return false;
    }

    // Convert to compact::message and compute the balanced preserve boundary:
    // adaptive depth over the tail, then walk back so the tail never starts
    // with an orphan tool result.
    kimix::vector<message> cms;
    cms.reserve(history.size());
    for (const kimix::llm::Message &m : history) {
        cms.push_back(soul_to_compact_message(m));
    }
    // Preserve boundary (SimpleCompaction.adaptive_preserve_depth): at least
    // the last `depth` messages, and always the most recent user turn so the
    // model keeps its live task. Walk back from the end to the last user
    // message, then extend by the adaptive depth, then balance on tool pairs.
    const int32_t depth = adaptive_preserve_depth(cms, 1, 10);
    size_t preserve_start =
        (static_cast<size_t>(depth) >= history.size())
            ? 1
            : history.size() - static_cast<size_t>(depth);
    // Only when the most recent user message sits in the recent half of the
    // history (otherwise a short conversation would preserve everything and
    // the compaction could not shrink).
    const size_t half = history.size() / 2;
    for (size_t i = history.size(); i-- > half;) {
        if (history[i].role == "user" && i < preserve_start) {
            preserve_start = i;
            break;
        }
    }
    preserve_start = soul_balance_preserve_start(history, preserve_start);
    if (preserve_start == 0) {
        preserve_start = 1; // always leave something to compact
    }
    if (preserve_start >= history.size()) {
        error = "nothing to compact (history is all preserved tail)";
        return false;
    }

    // Assemble the compaction prompt through the kernels (slice + legacy
    // flattened text + cascade detection).
    prepare_request req;
    req.messages = cms;
    req.preserve_start_index = preserve_start;
    req.options.mode = CompactMode::balanced;
    req.custom_instruction = custom_instruction;
    req.prompt_compact = soul_prompt_compact();
    req.prompt_compact_cascade = soul_prompt_compact_cascade();
    prepare_result prep;
    const tool_error terr = prepare_compaction_input(req, prep);
    if (terr.failed()) {
        error = terr.message;
        return false;
    }

    // One tool-less LLM call: the flattened conversation + the instruction.
    kimix::vector<kimix::llm::Message> summary_messages;
    kimix::llm::Message sys;
    sys.role = "system";
    sys.content =
        "You are a conversation compactor. Produce the compaction summary "
        "exactly as instructed. Output only the summary.";
    summary_messages.push_back(std::move(sys));
    kimix::llm::Message user;
    user.role = "user";
    user.content = prep.compact_message_text;
    user.content += "\n\n";
    user.content += prep.prompt_text;
    summary_messages.push_back(std::move(user));

    const kimix::llm::ChatResult res = _backend.chat(summary_messages, {}, {});
    if (!res.ok) {
        error = "compaction LLM call failed: " + res.error;
        return false;
    }
    if (res.content.empty()) {
        error = "compaction returned an empty summary";
        return false;
    }

    // Replace history: summary message + preserved tail.
    kimix::vector<kimix::llm::Message> new_history;
    new_history.reserve(history.size() - preserve_start + 1);
    new_history.push_back(soul_summary_message(res.content));
    for (size_t i = preserve_start; i < history.size(); ++i) {
        new_history.push_back(history[i]);
    }
    _session.history() = std::move(new_history);
    ++_compactions;
    return true;
}

TurnResult KimiSoul::turn(kimix::string_view user_input,
                          const SoulEventCallback &on_event) {
    TurnResult out;
    auto &history = _session.history();

    kimix::llm::Message user_msg;
    user_msg.role = "user";
    user_msg.content.assign(user_input.data(), user_input.size());
    history.push_back(std::move(user_msg));

    const kimix::vector<kimix::llm::Tool> tools = tool_definitions();

    for (int32_t step = 0; step < _opts.max_steps; ++step) {
        // Auto-compaction check before every step (kimisoul.py
        // should_auto_compact gate).
        if (_opts.auto_compact && history.size() >= 4) {
            builtin_tools::compact::compaction_trigger_config cfg;
            cfg.trigger_ratio = _opts.auto_compact_ratio;
            cfg.max_context_size = _backend.max_context_size();
            cfg.reserved_context_size = _opts.reserved_context;
            cfg.safety_margin_tokens = 1024;
            const int64_t tokens = estimated_tokens();
            if (builtin_tools::compact::should_auto_compact(tokens, cfg)) {
                kimix::string cerr;
                if (compact_context("", cerr)) {
                    out.compacted = true;
                }
            }
        }

        // Build the request: system prompt + history.
        kimix::vector<kimix::llm::Message> messages;
        messages.reserve(history.size() + 1);
        kimix::llm::Message sys;
        sys.role = "system";
        sys.content = effective_system_prompt();
        messages.push_back(std::move(sys));
        for (const kimix::llm::Message &m : history) {
            messages.push_back(m);
        }

        const kimix::llm::ChatResult res =
            _backend.chat(messages, tools, on_event);
        ++out.steps;
        if (!res.ok) {
            out.error = "chat failed: " + res.error;
            return out;
        }

        // Record the assistant message (text + tool calls).
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.content = res.content;
        assistant.thinking = res.reasoning;
        assistant.thinking_signature = res.signature;
        assistant.tool_calls = res.tool_calls;
        history.push_back(assistant);

        if (res.tool_calls.empty()) {
            out.ok = true;
            out.content = res.content;
            return out;
        }

        // Execute every tool call and append the tool results.
        for (const kimix::llm::ToolCall &tc : res.tool_calls) {
            kimix::string terr;
            kimix::string result =
                execute_tool_call(tc.name, tc.arguments, terr);
            kimix::llm::Message tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.content = std::move(result);
            history.push_back(std::move(tool_msg));
        }
    }

    out.ok = false;
    out.error = "max steps reached";
    return out;
}

} // namespace kimix::agent
