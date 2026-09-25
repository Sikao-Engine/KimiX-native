// agent/soul.cpp - KimiSoul implementation (see soul.h).
//
// Mirrors the essential control flow of kimi_cli/soul/kimisoul.py:
//   * turn() = the step loop (chat -> tool calls -> tool results -> repeat),
//     with the empty-input guard and the auto-compaction check before every step
//     (should_auto_compact)
//   * tool-call arguments go through json repair (kimix::repair, the same
//     kernel LLM::chat already applies - re-applied defensively here) and
//     ToolParams::try_deserialize; failures surface as error tool messages
//     instead of aborting the turn
//   * compact_context() = SimpleCompaction.compact: resolve the preserve
//     boundary through builtin_tools::compact::resolve_preserve_split (ported
//     tool_pairing + Phase-6 primacy re-insertion, so a compaction never splits
//     an assistant tool_calls message from its tool results), assemble the
//     compaction prompt through the builtin_tools::compact kernels + the ported
//     compact.md body, summarize with one tool-less LLM call, and replace the
//     compacted head with the reference's summary message + preserved tail.

#include "agent/soul.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include <core/clock.h>
#include <core/json_repair.h>

#include "builtin_tools/compact_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::agent {

namespace {

// Hard cap for one tool result injected into the history (the builtin tools
// already bound themselves; this is the last-resort guard).
constexpr size_t k_max_tool_result_chars = 100000;

// The manifest's tool allow-list (KimiSoul::options::enabled_tools): empty ==
// every registered tool, otherwise exactly the listed registry names.
bool soul_tool_enabled(const kimix::vector<kimix::string> &enabled,
                       kimix::string_view name) {
    if (enabled.empty()) {
        return true;
    }
    for (const kimix::string &want : enabled) {
        if (want == name) {
            return true;
        }
    }
    return false;
}

// One LLM-facing tool definition out of a registry entry. An empty schema
// becomes the bare object schema every chat backend expects for a
// no-parameter tool.
kimix::llm::Tool soul_tool_definition(const builtin_tools::ToolMeta &meta) {
    kimix::llm::Tool t;
    t.name = meta.name;
    t.description = meta.description;
    t.parameters_json = meta.parameters_json.empty()
                            ? kimix::string(R"({"type":"object"})")
                            : meta.parameters_json;
    return t;
}

// args.KIMI_OS equivalent (kimi_cli/utils/environment.py os_kind values).
kimix::string soul_os_name() {
#if defined(KIMIX_PLATFORM_WINDOWS)
    return "Windows";
#elif defined(KIMIX_PLATFORM_APPLE)
    return "macOS";
#else
    return "Linux";
#endif
}

// Silent read of <work_dir>/AGENTS.md (the reference's agent_md.is_file() +
// read_text(errors='replace') boundary). Returns "" when the file is missing
// or unreadable; the caller decides whether the role embeds it at all.
kimix::string soul_read_agents_md(const kimix::string &work_dir) {
    std::error_code ec;
    const kimix::filesystem::path path =
        kimix::filesystem::path(work_dir) / "AGENTS.md";
    if (!kimix::filesystem::is_regular_file(path, ec) || ec) {
        return kimix::string();
    }
    std::FILE *f = std::fopen(kimix::to_string(path).c_str(), "rb");
    if (f == nullptr) {
        return kimix::string();
    }
    kimix::string out;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
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
    // The balanced-cut fold counts persisted tool calls (kosong Message.tool_calls)
    // exactly; the "[tool_call] name(args)" text below is only for the
    // summarizer's view of the flattened conversation.
    cm.tool_call_count = static_cast<int32_t>(m.tool_calls.size());
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
    // Port of SimpleCompaction.compact's summary message (compaction.py:626-653):
    // a ``user`` message whose content is the ``system(...)`` marker followed by
    // the summary text (thinking parts dropped). The C++ message carries a single
    // content string, so the two reference TextParts are concatenated in order.
    kimix::llm::Message m;
    m.role = "user";
    m.content =
        "<system>Previous context has been compacted. Here is the compaction "
        "output:</system>";
    m.content.append(summary.data(), summary.size());
    return m;
}

// ── Python str.isspace() (generated; see scripts/gen_line_hash_tables.py) ────
// Same table as edit_tool.cpp's ED-SPACE-TABLES / line_hash.cpp's kPySpace*:
// bit b of the bitmap is set when chr(b).isspace() for b < 0x80, plus the 8
// non-ASCII whitespace ranges. Python counts U+001C-U+001F and U+0085 as
// whitespace, unlike C isspace().
constexpr uint32_t k_soul_py_space_ascii_bits[4] = {
    0xF0003E00u, 0x00000001u, 0x00000000u, 0x00000000u,
};
constexpr uint32_t k_soul_py_space_ranges[][2] = {
    0x0085, 0x0085, 0x00A0, 0x00A0, 0x1680, 0x1680, 0x2000, 0x200A,
    0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000,
};

bool soul_is_py_space_cp(uint32_t cp) noexcept {
    if (cp < 0x80) {
        return ((k_soul_py_space_ascii_bits[cp >> 5] >> (cp & 31)) & 1u) != 0;
    }
    size_t lo = 0;
    size_t hi = sizeof(k_soul_py_space_ranges) / sizeof(k_soul_py_space_ranges[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < k_soul_py_space_ranges[mid][0]) {
            hi = mid;
        } else if (cp > k_soul_py_space_ranges[mid][1]) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// ── Compaction preserve boundary ─────────────────────────────────────────────
// The balanced tool-pairing cut search + Phase-6 primacy re-insertion live in the
// compact kernel library (builtin_tools::compact::resolve_preserve_split), which
// ports kimi_cli/soul/tool_pairing.py + SimpleCompaction.prepare. The C++ soul
// used to hand-roll "walk back over tool messages"; that walk silently accepted
// an unbalanced boundary (forcing preserve_start = 1 could leave the preserved
// tail starting with an orphan tool result) and never kept the first message.

// Turn the kernel's split into the new history: [summary] + optional primacy
// copy of the first message + the preserved tail.
kimix::vector<kimix::llm::Message>
soul_apply_preserve_split(const kimix::vector<kimix::llm::Message> &history,
                          const builtin_tools::compact::preserve_split &split,
                          kimix::string_view summary) {
    kimix::vector<kimix::llm::Message> out;
    const size_t tail = split.preserve_start_index;
    out.reserve(history.size() - tail + 2);
    out.push_back(soul_summary_message(summary));
    if (split.keep_first_message && !history.empty()) {
        out.push_back(history.front()); // Phase 6: primacy copy
    }
    for (size_t i = tail; i < history.size(); ++i) {
        out.push_back(history[i]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Turn input hygiene
// ---------------------------------------------------------------------------

bool agent_user_input_is_empty(kimix::string_view user_input) noexcept {
    const char *it = user_input.data();
    const char *end = it + user_input.size();
    while (it < end) {
        // Invalid UTF-8 decodes to U+FFFD (not whitespace) and consumes one byte,
        // so a malformed input is treated as content and cannot spin here. Python
        // str cannot hold invalid UTF-8, so there is no reference case to mirror.
        const uint32_t cp = builtin_tools::decode_code_point(it, end);
        if (!soul_is_py_space_cp(cp)) {
            return false;
        }
    }
    return true;
}

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
    // Explicit system prompt wins verbatim (get_system_prompt is bypassed).
    if (!_opts.system_prompt.empty()) {
        return _opts.system_prompt;
    }
    // Default prompt: byte-faithful port of kimix/utils/system_prompt.py's
    // get_system_prompt closure (agent/system_prompt.cpp).
    system_prompt_input in;
    in.role = _opts.prompt_role;
    in.os = soul_os_name();
    in.work_dir = _session.work_dir();
    in.yolo = _opts.yolo;
    in.shell_tool = effective_shell_tool();
    in.skills_text = _opts.skills_text;
    // compact_export_* stay default-off until the compaction flow wires them.
    if (system_prompt_role_uses_agent_md(in.role)) {
        in.agents_md = soul_read_agents_md(in.work_dir);
    }
    return build_system_prompt(in);
}

kimix::vector<kimix::llm::Tool> KimiSoul::tool_definitions() const {
    kimix::vector<kimix::llm::Tool> defs;
    for (const builtin_tools::ToolMeta &meta :
         builtin_tools::ToolRegistry::instance().all()) {
        // The validity gate is part of tool_offered(): an invalid tool (no
        // python interpreter, no Git Bash on Windows, a feature this session
        // switched off) is never listed, so the model cannot be shown - or
        // cannot call - something that is broken here. The same predicate also
        // applies the bash -> pwsh shell fallback, which is what keeps the
        // prompt's {shell_tool} substitution (effective_shell_tool()) and this
        // list telling the same story.
        if (!tool_offered(meta.name)) {
            continue;
        }
        defs.push_back(soul_tool_definition(meta));
    }
    return defs;
}

builtin_tools::Tool *KimiSoul::get_tool(kimix::string_view name) const {
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
    // The validity gate runs right after the constructor. A tool that answers
    // false is dropped WITHOUT caching it, so a dependency that appears later
    // (a sub-agent runner injected into the session, an interpreter installed
    // mid-session) is picked up by the next rebuild.
    if (!tool->valid()) {
        return nullptr;
    }
    builtin_tools::Tool *raw = tool.get();
    _tools.emplace(meta->name, std::move(tool));
    return raw;
}

kimix::vector<kimix::string> KimiSoul::unavailable_tools() const {
    // "asked for but not offered": every name the manifest enables (or, with
    // no allow-list, every registered name) that tool_definitions() skipped
    // because Tool::valid() answered false - or, for an explicit allow-list,
    // because the registry does not know it at all.
    kimix::vector<kimix::string> dropped;
    if (!_opts.enabled_tools.empty()) {
        for (const kimix::string &name : _opts.enabled_tools) {
            if (!tool_offered(name)) {
                dropped.push_back(name);
            }
        }
        return dropped;
    }
    for (const builtin_tools::ToolMeta &meta :
         builtin_tools::ToolRegistry::instance().all()) {
        if (!tool_offered(meta.name)) {
            dropped.push_back(meta.name);
        }
    }
    return dropped;
}

bool KimiSoul::tool_available(kimix::string_view name) const {
    const builtin_tools::ToolMeta *meta =
        builtin_tools::ToolRegistry::instance().find_ci(name);
    if (meta == nullptr) {
        return false;
    }
    if (!soul_tool_enabled(_opts.enabled_tools, meta->name)) {
        return false;
    }
    return get_tool(meta->name) != nullptr;
}

bool KimiSoul::tool_offered(kimix::string_view name) const {
    if (tool_available(name)) {
        return true; // registered, enabled by the manifest, valid here
    }
    // The shell fallback: the manifest asked for the bash tool but Git Bash is
    // not installed (bash answered valid() == false), so pwsh takes the shell
    // role even though the manifest never listed it - an agent without any
    // shell cannot run anything. The mirror direction (bash in place of a
    // missing pwsh) needs no rule: bash is a normal tool of the list.
    const builtin_tools::ToolMeta *meta =
        builtin_tools::ToolRegistry::instance().find_ci(name);
    if (meta == nullptr || meta->name != "pwsh") {
        return false;
    }
    if (get_tool(meta->name) == nullptr) {
        return false; // no PowerShell host either
    }
    return soul_tool_enabled(_opts.enabled_tools, "bash") &&
           !tool_available("bash");
}

kimix::string KimiSoul::effective_shell_tool() const {
    const kimix::string configured = _opts.shell_tool;
    // Only the two shell tools the fallback knows about are rewritten; any
    // other value (an empty string, a custom shell name) passes through.
    if (configured != "bash" && configured != "pwsh") {
        return configured;
    }
    if (tool_offered(configured)) {
        return configured;
    }
    const kimix::string other = (configured == "bash") ? kimix::string("pwsh")
                                                       : kimix::string("bash");
    return tool_offered(other) ? other : configured;
}

kimix::string KimiSoul::execute_tool_call(kimix::string_view name,
                                          kimix::string_view arguments_json,
                                          kimix::string &error) {
    error.clear();
    builtin_tools::Tool *tool = get_tool(name);
    if (tool == nullptr) {
        // A registered name that get_tool refused is a tool this environment
        // cannot run (Tool::valid() == false), not a typo: say so, because the
        // model may still carry it in a compacted/stale context.
        const builtin_tools::ToolMeta *meta =
            builtin_tools::ToolRegistry::instance().find_ci(name);
        kimix::string msg = (meta != nullptr)
                                ? kimix::string(
                                      "tool is not available in this "
                                      "environment: ")
                                : kimix::string("unknown tool: ");
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
    // No exceptions (kimix_enable_exception=false): a tool invocation cannot
    // throw, so the former `try { (*tool)(&params); } catch (std::exception&)`
    // -> "tool threw: ..." boundary is gone. Tools report failures as data
    // (tool_error in the result payload) and never across this call.
    (*tool)(&params);
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

    // Convert to compact::message (text parts + a faithful tool-call count, which
    // the balanced-cut fold needs) and let the kernel compute the preserve
    // boundary the same way SimpleCompaction.prepare does.
    kimix::vector<message> cms;
    cms.reserve(history.size());
    for (const kimix::llm::Message &m : history) {
        cms.push_back(soul_to_compact_message(m));
    }
    // Preserve depth: adaptive_preserve_depth over the tail, bounded by the same
    // min/max the reference passes from LoopControl (1 / 2 by default).
    const int32_t depth = adaptive_preserve_depth(cms,
                                                 _opts.min_preserved_turns,
                                                 _opts.max_preserved_turns);
    const preserve_split split =
        resolve_preserve_split(cms, depth, /*balanced_cuts=*/true);
    if (split.unbalanced) {
        error =
            "cannot compact: the history has a tool result with no matching "
            "tool call (unbalanced tool pairing)";
        return false;
    }
    if (!split.compact) {
        error = "nothing to compact (history is all preserved tail)";
        return false;
    }
    // The compacted region is the contiguous cut [0, preserve_start); the
    // preserved tail is history[preserve_start:] plus, when the kernel reports it,
    // a primacy copy of history[0] (the reference's non-contiguous
    // ``[messages[0]] + messages[k:]`` shape). The summarizer only ever sees the
    // compacted region, so the flattened request keeps the contiguous slice.
    const size_t preserve_start = split.preserve_start_index;

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

    // Replace history: summary message + [primacy copy of the first message] +
    // the preserved tail (the only two shapes the reference emits).
    _session.history() = soul_apply_preserve_split(history, split, res.content);
    ++_compactions;
    return true;
}

TurnResult KimiSoul::turn(kimix::string_view user_input,
                          const SoulEventCallback &on_event) {
    TurnResult out;
    // Empty-input guard (kimi_cli.soul.run_soul, soul/__init__.py:329-341 and
    // KimiSoul.steer): never start a turn for blank input. Without it the soul
    // appends an empty `user` message and the model answers a spurious "you sent
    // an empty message" turn. No history mutation, no LLM call.
    if (agent_user_input_is_empty(user_input)) {
        out.ignored = true;
        return out;
    }
    auto &history = _session.history();

    kimix::llm::Message user_msg;
    user_msg.role = "user";
    user_msg.content.assign(user_input.data(), user_input.size());
    history.push_back(std::move(user_msg));

    const kimix::vector<kimix::llm::Tool> tools = tool_definitions();

    for (int32_t step = 0; step < _opts.max_steps; ++step) {
        // Auto-compaction check before every step (kimisoul.py's
        // should_auto_compact gate, compaction.py:239-278). The reference feeds it
        // the live token count, the model window, the configured ratio and the
        // reserved-output budget (max_tokens + safety margin, tool-call buffer,
        // reserved context). The `history.size() >= 4` shortcut is port-only: a
        // shorter history can never be compacted anyway
        // (resolve_preserve_split reports "nothing to compact"), so it only skips
        // a no-op attempt.
        if (_opts.auto_compact && history.size() >= 4) {
            builtin_tools::compact::compaction_trigger_config cfg;
            cfg.trigger_ratio = _opts.auto_compact_ratio;
            cfg.max_context_size = _backend.max_context_size();
            cfg.reserved_context_size = _opts.reserved_context;
            cfg.max_tokens = _opts.max_tokens;
            cfg.tool_call_buffer_tokens = _opts.tool_call_buffer_tokens;
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
