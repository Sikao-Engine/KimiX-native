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

#include "agent/soul.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>
#include <core/clock.h>
#include <core/json_repair.h>
#include <runtime/soul/message_view.h>
#include <runtime/text/sanitize.h>
#include <runtime/tools/export_builder.h>
#include "agent/context_overflow.h"
#include "agent/dynamic_injections/budget_reminder.h"
#include "agent/dynamic_injections/compact_reminder.h"
#include "agent/dynamic_injections/context_meter.h"
#include "agent/dynamic_injections/target_churn.h"
#include "agent/dynamic_injections/todo_reminder.h"
#include "agent/step_retry.h"
#include "builtin_tools/compact_tool.h"
#include "builtin_tools/retrieve_tool.h"
#include "builtin_tools/todo_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

namespace kimix::agent {

namespace {

// ── Tool-result post-processing (E3/F3/F4) ───────────────────────────────────
// Ports the reference's toolset.py handle() tail + soul/message.py
// tool_result_to_message: the tool payload is parsed once, sanitized
// (sanitize_for_tokenizer), checked against the dynamic per-tool output budget
// and finally wrapped in the <system> envelope the model is conditioned on.

// One parse of a tool result payload: {"status", "message", "brief", "output"}.
struct soul_tool_result_fields {
    bool parsed = false; // the payload was a JSON object with a string status
    bool ok = true;
    // True for status "error" - the generic runtime-failure status the builtin
    // tools use for exceptions during execution (the reference's
    // ToolRuntimeError, which earns the "unexpected error" sentence).
    bool runtime_error = false;
    kimix::string message;
    kimix::string output;
};

soul_tool_result_fields soul_parse_tool_result(kimix::string_view json) {
    soul_tool_result_fields f;
    if (json.empty()) {
        return f;
    }
    yyjson_doc *doc = yyjson_read_opts(
        const_cast<char *>(json.data()), json.size(), YYJSON_READ_STOP_WHEN_DONE,
        &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return f;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return f;
    }
    f.parsed = true;
    yyjson_val *status = yyjson_obj_get(root, "status");
    if (yyjson_is_str(status)) {
        const kimix::string_view value(yyjson_get_str(status),
                                       static_cast<size_t>(yyjson_get_len(status)));
        f.ok = (value == "ok");
        f.runtime_error = (value == "error");
    }
    yyjson_val *msg = yyjson_obj_get(root, "message");
    if (yyjson_is_str(msg)) {
        f.message.assign(yyjson_get_str(msg),
                         static_cast<size_t>(yyjson_get_len(msg)));
    }
    yyjson_val *out = yyjson_obj_get(root, "output");
    if (yyjson_is_str(out)) {
        f.output.assign(yyjson_get_str(out),
                        static_cast<size_t>(yyjson_get_len(out)));
    }
    yyjson_doc_free(doc);
    return f;
}

// E3: port of message.py tool_result_to_message (soul/message.py:52-87). The
// C++ message model flattens content parts into one string, so the parts are
// joined with "\n" instead of being separate TextParts.
kimix::string soul_tool_result_envelope(bool ok, bool runtime_error,
                                        kimix::string_view message,
                                        kimix::string_view output) {
    kimix::string content;
    if (!ok) {
        content = "<system>ERROR: ";
        content.append(message.data(), message.size());
        if (runtime_error) {
            content += "\nThis is an unexpected error and the tool is probably "
                       "not working.";
        }
        content += "</system>";
    } else if (!message.empty()) {
        content = "<system>";
        content.append(message.data(), message.size());
        content += "</system>";
    }
    if (!output.empty()) {
        if (!content.empty()) {
            content += "\n";
        }
        content.append(output.data(), output.size());
    }
    if (content.empty()) {
        content = "<system>Tool output is empty.</system>";
    }
    return content;
}

// The error envelope for dispatch-level failures (unknown tool, refused,
// invalid arguments JSON) - the reference's ToolParseError/ToolValidateError
// path of tool_result_to_message.
kimix::string soul_dispatch_error_envelope(kimix::string_view message) {
    kimix::string content = "<system>ERROR: ";
    content.append(message.data(), message.size());
    content += "</system>";
    return content;
}

// F3: port of toolset.py _estimate_tool_output_byte_budget (toolset.py:951-973):
// the more restrictive of 50% of the model window, 90% of the remaining tokens
// (both through the 4 bytes/token estimate) and the 128 KiB ceiling.
int64_t soul_tool_output_byte_budget(int64_t max_context_size,
                                     int64_t current_tokens) noexcept {
    constexpr double k_bytes_per_token = 4.0;      // _TOOL_OUTPUT_BYTES_PER_TOKEN
    constexpr double k_context_fraction = 0.5;     // _TOOL_OUTPUT_CONTEXT_FRACTION
    constexpr double k_remaining_fraction = 0.9;   // _TOOL_OUTPUT_REMAINING_FRACTION
    constexpr int64_t k_default_max_bytes = 128 << 10; // _DEFAULT_TOOL_OUTPUT_MAX_BYTES
    if (max_context_size <= 0) {
        return k_default_max_bytes; // _get_max_output_bytes fallback
    }
    int64_t bytes = static_cast<int64_t>(
        static_cast<double>(max_context_size) * k_bytes_per_token *
        k_context_fraction);
    int64_t remaining = max_context_size - current_tokens;
    if (remaining < 0) {
        remaining = 0;
    }
    const int64_t remaining_bytes = static_cast<int64_t>(
        static_cast<double>(remaining) * k_bytes_per_token * k_remaining_fraction);
    if (remaining_bytes < bytes) {
        bytes = remaining_bytes;
    }
    if (k_default_max_bytes < bytes) {
        bytes = k_default_max_bytes;
    }
    if (bytes < 0) {
        bytes = 0;
    }
    return bytes;
}

// ISO-8601 UTC with seconds precision (the reference's
// pendulum isoformat(timespec="seconds")).
kimix::string soul_iso_now_utc() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return kimix::string(buf);
}

// C8 (kimisoul.py:2160-2198): durable pre-compaction export of the whole
// history to <work_dir>/.kimix_cache/context_compacted.md through the
// export-builder kernel (the same build_export_markdown /export uses).
// Returns the export path on success, "" on failure (the reference logs a
// warning and continues with compact_export_path=None; a failed export must
// never abort the compaction).
kimix::string soul_export_precompaction(const AgentSession &session,
                                        uint64_t token_count) {
    using namespace kimix::runtime::soul;
    const kimix::vector<kimix::llm::Message> &history = session.history();
    kimix::vector<kimix::vector<part_view>> part_storage;
    kimix::vector<kimix::vector<tool_call_view>> tc_storage;
    kimix::vector<message_view> views;
    part_storage.reserve(history.size());
    tc_storage.reserve(history.size());
    views.reserve(history.size());
    const auto role_code = [](kimix::string_view role) -> uint8_t {
        if (role == "system") {
            return kRoleSystem;
        }
        if (role == "user") {
            return kRoleUser;
        }
        if (role == "assistant") {
            return kRoleAssistant;
        }
        return kRoleTool;
    };
    for (const kimix::llm::Message &m : history) {
        kimix::vector<part_view> parts;
        if (!m.thinking.empty()) {
            parts.push_back(
                part_view{part_kind::THINK, kimix::string_view(m.thinking)});
        }
        if (!m.content.empty()) {
            parts.push_back(
                part_view{part_kind::TEXT, kimix::string_view(m.content)});
        }
        kimix::vector<tool_call_view> tcs;
        tcs.reserve(m.tool_calls.size());
        for (const kimix::llm::ToolCall &tc : m.tool_calls) {
            tcs.push_back(tool_call_view{kimix::string_view(tc.id),
                                         kimix::string_view(tc.name),
                                         kimix::string_view(tc.arguments)});
        }
        message_view v;
        v.role = role_code(m.role);
        v.tool_call_id = kimix::string_view(m.tool_call_id);
        part_storage.push_back(std::move(parts));
        tc_storage.push_back(std::move(tcs));
        v.parts = kimix::span<const part_view>(part_storage.back());
        v.tool_calls = kimix::span<const tool_call_view>(tc_storage.back());
        views.push_back(v);
    }
    kimix::runtime::tools::export_options opts;
    opts.session_id = session.id();
    opts.work_dir = session.work_dir();
    opts.exported_at = soul_iso_now_utc();
    opts.token_count = token_count;
    kimix::string markdown;
    kimix::runtime::tools::build_export_markdown(
        kimix::span<const message_view>(views.data(),
                                        static_cast<int64_t>(views.size())),
        opts, markdown);
    std::error_code ec;
    const kimix::filesystem::path dir =
        kimix::filesystem::path(session.work_dir()) / ".kimix_cache";
    kimix::filesystem::create_directories(dir, ec);
    if (ec) {
        return kimix::string();
    }
    const kimix::filesystem::path file = dir / "context_compacted.md";
    std::FILE *f = std::fopen(kimix::to_string(file).c_str(), "wb");
    if (f == nullptr) {
        return kimix::string();
    }
    const size_t n = std::fwrite(markdown.data(), 1, markdown.size(), f);
    std::fclose(f);
    if (n != markdown.size()) {
        return kimix::string();
    }
    return kimix::to_string(file);
}

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

// Tools forbidden when KimiSoul::options::read_only is set: the reference's
// _READ_ONLY_BLOCKED_TOOLS frozenset (toolset.py:93-105).  The refusal text is
// the reference's verbatim (toolset.py:1347-1362).  Compared
// case-insensitively so the canonical registry name blocks every spelling
// ("Run" blocks a "run" tool, like the reference's dict lookup).
bool soul_read_only_blocked(kimix::string_view name) {
    static constexpr kimix::string_view k_blocked[] = {
        "bash", "pwsh", "Run", "python", "edit", "write",
        "subagent", "interrupt_agent", "workflow", "job_output",
    };
    for (kimix::string_view blocked : k_blocked) {
        if (blocked.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (size_t i = 0; i < name.size(); ++i) {
            const char a = name[i] >= 'A' && name[i] <= 'Z' ? char(name[i] - 'A' + 'a') : name[i];
            const char b = blocked[i] >= 'A' && blocked[i] <= 'Z' ? char(blocked[i] - 'A' + 'a') : blocked[i];
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
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

// ── Compaction prompt bodies (C5/C6) ─────────────────────────────────────────
// Byte-exact ports of kimi_cli/prompts/compact.md and compact_cascade.md (the
// reference's prompts.COMPACT / prompts.COMPACT_CASCADE), generated by
// scripts/gen_compact_prompt_inc.py. The full template text is kept verbatim:
// 14 keep-priorities, the "Key logic means" definition, "Special Handling",
// the 20-30% length guidance and the 16-block XML output structure (cascade:
// the flat deduplicated <facts> list, 8 rules and the 14-block XML). The
// per-mode style guidance stays in the compact kernel (mode_guidance,
// compact_tool.cpp) and is appended by build_compaction_prompt exactly like
// the reference's _build_prompt_text (compaction.py:397-432).
#include "agent/compact_prompts.inc"

kimix::string soul_prompt_compact() { return kimix::string(k_compact_prompt); }

kimix::string soul_prompt_compact_cascade() {
    return kimix::string(k_compact_cascade_prompt);
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
    return _llm->chat(messages, tools, on_chunk, _abort);
}
void LLMBackend::set_abort_check(const kimix::llm::AbortCheck *check) {
    _abort = check;
}

int64_t LLMBackend::max_context_size() const {
    const int32_t v = _llm->max_context_size();
    return v > 0 ? v : 128000;
}

kimix::string LLMBackend::model_name() const { return _llm->model_name(); }

void LLMBackend::set_output_token_budget(int64_t tokens) {
    _llm->set_output_token_budget(static_cast<int32_t>(tokens));
}

int64_t LLMBackend::output_token_budget() const {
    return _llm->output_token_budget();
}

void KimiSoul::set_wire_sink(WireSink *sink) noexcept {
    _wire = sink;
    if (_approval != nullptr) {
        // The gate's ApprovalRequest/ApprovalResponse records join the soul's
        // stream (and disappear with it).
        _approval->set_wire_sink(sink);
    }
}

void KimiSoul::set_approval(Approval *approval) noexcept {
    _approval = approval;
    // Adopt the stream only when this soul actually owns one: sub-agent souls
    // share the parent's gate (approval.py share()) and must not detach its
    // wire sink. The owner clears the sink before destroying the writer.
    if (_approval != nullptr && _wire != nullptr) {
        _approval->set_wire_sink(_wire);
    }
}

kimix::string LLMBackend::provider_name() const {
    // The recorder's provider identity (str(getattr(chat_provider, "name",
    // ...))): the unified Config.type, e.g. "openai_legacy" | "anthropic".
    return _llm->config().type;
}

kimix::string LLMBackend::thinking_effort() const {
    return _llm->config().thinking_effort;
}

bool LLMBackend::generation_temperature_top_p(double &, double &) const {
    // The native LLM Config carries no temperature/top_p knobs (the reference
    // reads _generation_kwargs); both serialize as null.
    return false;
}

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

void AgentSession::index_history_message(const kimix::llm::Message &message) {
    using kimix::runtime::index::turn_meta;
    // history_index.py index_messages / _index_messages_legacy: only
    // user/assistant/tool roles with non-blank text become turns.
    uint8_t role_code = 3; // other
    if (message.role == "user") {
        role_code = 0;
    } else if (message.role == "assistant") {
        role_code = 1;
    } else if (message.role == "tool") {
        role_code = 2;
    } else {
        return;
    }
    bool has_text = false;
    for (char c : message.content) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' &&
            c != '\f') {
            has_text = true;
            break;
        }
    }
    if (!has_text) {
        return;
    }
    turn_meta turn;
    turn.timestamp =
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    turn.role = role_code;
    turn.is_compacted = false;
    // history_index.py _message_to_text: the TextPart texts joined by "\n" -
    // the C++ port's flat-string message model stores exactly that, so the
    // content string is the turn text. The durable index stores it RAW (D7:
    // verbatim on recall; FTS tokenizers fold case at index time only).
    turn.text = message.content;
    if (_sqlite_history_index != nullptr) {
        // D6: the durable index owns the turn-id counter and assigns ids
        // itself (the turn_id field below is ignored).
        const turn_meta turns[1] = {std::move(turn)};
        _sqlite_history_index->append_turns(
            kimix::span<const turn_meta>(turns, 1));
        return;
    }
    turn.turn_id = _next_turn_id++; // _doc_id_counter (in-memory fallback)
    const turn_meta turns[1] = {std::move(turn)};
    _history_index.append_turns(kimix::span<const turn_meta>(turns, 1));
}

void AgentSession::append_history(const kimix::llm::Message &message) {
    _history.push_back(message);
    index_history_message(message);
}

void AgentSession::on_history_compacted() {
    // kimisoul.py post-compaction sequence: mark_compacted() on the old
    // turns, then append_message(compaction_result.messages) re-indexes the
    // summary + preserved tail as fresh turns.
    if (_sqlite_history_index != nullptr) {
        _sqlite_history_index->mark_compacted();
    } else {
        _history_index.mark_compacted();
    }
    for (const kimix::llm::Message &m : _history) {
        index_history_message(m);
    }
}

AgentSession::~AgentSession() {
    close_history_index();
}

bool AgentSession::open_history_index(kimix::string_view db_path,
                                      kimix::string &error) {
    close_history_index();
    auto index = std::make_unique<kimix::runtime::index::SqliteHistoryIndex>(
        kimix::filesystem::path(kimix::string(db_path)));
    if (!index->open(error)) {
        // Keep the in-memory fallback (retrieve stays live, session durable
        // history is simply lost for this run).
        return false;
    }
    _sqlite_history_index = std::move(index);
    return true;
}

void AgentSession::close_history_index() noexcept {
    if (_sqlite_history_index != nullptr) {
        _sqlite_history_index->save();
        _sqlite_history_index->close();
        _sqlite_history_index.reset();
    }
}

kimix::vector<kimix::runtime::index::turn_meta>
AgentSession::history_search(kimix::string_view query, uint32_t top_k) {
    if (_sqlite_history_index != nullptr) {
        return _sqlite_history_index->search(query, top_k);
    }
    return _history_index.search(query, top_k);
}

kimix::optional<kimix::runtime::index::turn_meta>
AgentSession::history_get_by_id(uint32_t turn_id) const {
    if (_sqlite_history_index != nullptr) {
        return _sqlite_history_index->get_by_id(turn_id);
    }
    const kimix::runtime::index::turn_meta *t = _history_index.get_by_id(turn_id);
    if (t == nullptr) {
        return kimix::optional<kimix::runtime::index::turn_meta>();
    }
    return kimix::optional<kimix::runtime::index::turn_meta>(*t);
}

void AgentSession::reindex_history() {
    for (const kimix::llm::Message &m : _history) {
        index_history_message(m);
    }
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
    : _session(session), _backend(backend),
      _opts(reconcile_options(std::move(opts))) {
    // G9: register the five reference providers, each behind its
    // loop_control gate (all default off, config.py:318-414 /
    // kimisoul.py:504-552). The todo loader reads the session's todo state
    // through the same session_todos() cache the todo tools use, so the
    // reminder always agrees with the tool; it cannot fail (the cache falls
    // back to an empty state on a corrupt state file).
    const LoopControl &lc = _opts.loop_control;
    if (lc.compact_reminder_enabled) {
        _injections.add_provider(kimix::unique_ptr<DynamicInjectionProvider>(
            new CompactReminderProvider(lc.compact_reminder_threshold)));
    }
    if (lc.todo_reminder_enabled) {
        kimix::function<kimix::vector<builtin_tools::todo::todo_item>()> loader =
            [this] {
                return builtin_tools::todo::session_todos(_session.tool_session())
                    .todos;
            };
        _injections.add_provider(kimix::unique_ptr<DynamicInjectionProvider>(
            new TodoReminderProvider(std::move(loader),
                                     lc.todo_reminder_interval_steps)));
    }
    if (lc.target_churn_enabled) {
        _injections.add_provider(kimix::unique_ptr<DynamicInjectionProvider>(
            new TargetChurnProvider(lc.target_churn_file_warn,
                                    lc.target_churn_file_strong,
                                    lc.target_churn_error_warn,
                                    lc.target_churn_cooldown_steps)));
    }
    if (lc.budget_reminder_enabled) {
        _injections.add_provider(kimix::unique_ptr<DynamicInjectionProvider>(
            new BudgetReminderProvider(lc.budget_warn_ratios,
                                       lc.budget_wall_clock_seconds)));
    }
      if (lc.context_meter_enabled) {
          // The meter stays silent in the compact reminder's region when that
          // reminder is enabled (kimisoul.py:544-552).
          kimix::optional<double> suppress_above;
          if (lc.compact_reminder_enabled) {
              suppress_above = lc.compact_reminder_threshold;
          }
          _injections.add_provider(kimix::unique_ptr<DynamicInjectionProvider>(
              new ContextMeterProvider(lc.context_meter_min_delta,
                                       lc.context_meter_cooldown_steps,
                                       suppress_above)));
      }
      // G7: register in the soul registry so Steer::from_session can resolve
      // this soul from its session (steer.py:39-60's attribute chain).
      steer_register_soul(_session, *this);
  }

  KimiSoul::~KimiSoul() { steer_unregister_soul(_session, *this); }

void KimiSoul::add_injection_provider(
    kimix::unique_ptr<DynamicInjectionProvider> provider) {
    _injections.add_provider(std::move(provider));
}

void KimiSoul::notify_afk_changed(bool enabled) { _injections.notify_afk_changed(enabled); }

KimiSoul::options KimiSoul::reconcile_options(options opts) {
    // The [loop_control] section is authoritative; the legacy numeric fields
    // predate it and are kept so existing callers (tests, demos, the CLI's
    // pre-loop_control plumbing) keep working. A legacy field left at its
    // LoopControl default defers to loop_control; an explicitly changed legacy
    // value wins and is written back into loop_control.
    LoopControl &lc = opts.loop_control;
    if (opts.max_steps != 15000) {
        lc.max_steps_per_turn = opts.max_steps;
    }
    if (opts.auto_compact_ratio != 0.8) {
        lc.compaction_trigger_ratio = opts.auto_compact_ratio;
    }
    if (opts.reserved_context != 75000) {
        lc.reserved_context_size = opts.reserved_context;
    }
    return opts;
}

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
    // C8: after a committed compaction the pre-compaction export path is
    // advertised in the prompt (kimisoul.py:2189-2199 promotes the compacting
    // render to the persistent prompt slot; the native soul re-renders per
    // request, so carrying the path in the input is the equivalent).
    in.compact_export_pending = _compact_export_pending;
    in.compact_export_path = _compact_export_path;
    if (system_prompt_role_uses_agent_md(in.role)) {
        in.agents_md = soul_read_agents_md(in.work_dir);
    }
    return build_system_prompt(in);
}

kimix::vector<kimix::llm::Tool> KimiSoul::tool_definitions() const {
    kimix::vector<kimix::llm::Tool> defs;
    // Iterate in a deterministic, dependency-friendly order. A tool's
    // validity may consult a sibling instance through the session
    // tool-pointer map (Pwsh::valid() yields to a valid Bash), so the tool
    // CREATION order must not depend on static-registration order - that
    // order follows the unity-batch file concatenation and could create
    // "pwsh" before "bash", letting both shells pass their probes.
    // Alphabetical by registry key: "bash" is always created (and its
    // pointer registered) before "pwsh" is constructed.
    kimix::vector<builtin_tools::ToolMeta> metas =
        builtin_tools::ToolRegistry::instance().all();
    std::sort(metas.begin(), metas.end(),
              [](const builtin_tools::ToolMeta &a,
                 const builtin_tools::ToolMeta &b) { return a.name < b.name; });
    for (const builtin_tools::ToolMeta &meta : metas) {
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
    // D4 - wire the retrieve tool's HistoryIndexView to this session's
    // history index BEFORE the validity gate: Retrieve::valid() requires an
    // injected view, and without one the tool silently vanishes from
    // tool_definitions() even though the system prompt advertises it.
    // The registry name is the type tag (no RTTI). The view closures go
    // through AgentSession::history_search/history_get_by_id so they serve
    // from the durable SQLite index (D4-durable) when one is open and from
    // the in-memory fallback otherwise.
    if (meta->name == "retrieve") {
        auto *retrieve_tool =
            static_cast<builtin_tools::retrieve::Retrieve *>(tool.get());
        AgentSession *session = &_session;
        retrieve_tool->view.search_with_recency =
            [session](kimix::string_view query,
                      int32_t top_k) -> kimix::vector<builtin_tools::retrieve::history_turn> {
                kimix::vector<builtin_tools::retrieve::history_turn> out;
                const kimix::vector<kimix::runtime::index::turn_meta> found =
                    session->history_search(query, static_cast<uint32_t>(top_k));
                out.reserve(found.size());
                for (const kimix::runtime::index::turn_meta &t : found) {
                    builtin_tools::retrieve::history_turn ht;
                    ht.turn_id = static_cast<int64_t>(t.turn_id);
                    ht.role = t.role == 0   ? "user"
                              : t.role == 1 ? "assistant"
                              : t.role == 2 ? "tool"
                                            : "other";
                    ht.text = t.text; // raw verbatim (D7)
                    ht.timestamp = t.timestamp;
                    ht.score = t.score;
                    ht.is_compacted = t.is_compacted;
                    ht.boosted_score = t.score;
                    out.push_back(std::move(ht));
                }
                return out;
            };
        retrieve_tool->view.get_by_id =
            [session](kimix::string_view ref)
                -> kimix::optional<builtin_tools::retrieve::history_turn> {
                const int64_t id =
                    builtin_tools::retrieve::parse_turn_reference(ref);
                if (id < 0) {
                    return kimix::optional<builtin_tools::retrieve::history_turn>();
                }
                const kimix::optional<kimix::runtime::index::turn_meta> turn =
                    session->history_get_by_id(static_cast<uint32_t>(id));
                if (!turn.has_value()) {
                    return kimix::optional<builtin_tools::retrieve::history_turn>();
                }
                const kimix::runtime::index::turn_meta &t = turn.value();
                builtin_tools::retrieve::history_turn ht;
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
                return ht;
            };
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
                                          kimix::string &error,
                                          kimix::string_view tool_call_id) {
    error.clear();
    // F1 - dispatch gating: the manifest's enabled_tools allow-list filters
    // tool_definitions(), so it must filter dispatch too (toolset.py only ever
    // builds _tool_dict from the enabled tools).  With no allow-list every
    // registered tool passes here and an invalid one keeps the "not available"
    // path below.  The pwsh shell fallback counts as enabled when adopted
    // (tool_offered), so an offered tool is never refused here.
    const builtin_tools::ToolMeta *called_meta =
        builtin_tools::ToolRegistry::instance().find_ci(name);
    if (called_meta != nullptr &&
        !soul_tool_enabled(_opts.enabled_tools, called_meta->name) &&
        !(called_meta->name == "pwsh" && tool_offered(name))) {
        kimix::string msg =
            "tool is not enabled in this session (enabled_tools manifest): ";
        msg.append(name.data(), name.size());
        error = msg;
        return soul_dispatch_error_envelope(msg);
    }
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
        return soul_dispatch_error_envelope(msg);
    }
    // F2 - read-only mode: the reference's _READ_ONLY_BLOCKED_TOOLS guard
    // (toolset.py:1347-1362), enforced at dispatch with the exact refusal.
    if (_opts.read_only && soul_read_only_blocked(name)) {
        kimix::string msg = "Tool '";
        msg.append(name.data(), name.size());
        msg += "' is forbidden in read-only mode. The agent should quit the "
               "conversation immediately.";
        error = msg;
        return soul_dispatch_error_envelope(msg);
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
        return soul_dispatch_error_envelope(msg);
    }
    // G1/G3: the approval gate (approval.py request(), called by the reference
    // inside the file tools before they touch the disk: edit/base.py:148,
    // write.py:355). Only the gated set asks (approval_gated_tool); yolo/afk
    // and approve-for-session grants short-circuit inside the gate. A
    // rejection feeds the typed reason back to the model as an error tool
    // result (the ToolRejectedError wording, approval.py:33-52) - the tool
    // never runs and the turn continues.
    if (_approval != nullptr) {
        const builtin_tools::ToolMeta *meta =
            builtin_tools::ToolRegistry::instance().resolve(name);
        const kimix::string_view canonical =
            meta != nullptr ? kimix::string_view(meta->name)
                            : kimix::string_view(name);
        if (approval_gated_tool(canonical)) {
            const kimix::string action = approval_action_for(_session.work_dir(), args);
            const kimix::string description = approval_description_for(canonical, args);
            const ApprovalResult verdict =
                _approval->request(canonical, action, description,
                                   /*is_subagent=*/_session.tool_session().is_sub_agent,
                                   tool_call_id);
            if (!verdict) {
                const kimix::string msg =
                    verdict.rejection_message(_session.tool_session().is_sub_agent);
                error = verdict.rejection_brief();
                // The model-facing envelope: a rejected ToolError becomes the
                // <system>ERROR: ...</system> tool message (message.py
                // tool_result_to_message), exactly like every other failure.
                return soul_tool_result_envelope(/*ok=*/false, /*runtime_error=*/false,
                                                 msg, "");
            }
        }
    }
    // No exceptions (kimix_enable_exception=false): a tool invocation cannot
    // throw, so the former `try { (*tool)(&params); } catch (std::exception&)`
    // -> "tool threw: ..." boundary is gone. Tools report failures as data
    // (tool_error in the result payload) and never across this call.
    (*tool)(&params);
    kimix::vector<char> out;
    tool->result_json(out);
    kimix::string result(out.data(), out.size());
    if (result.empty()) {
        result = R"JSON({"status":"ok","message":"(no result payload)"})JSON";
    }
    // E3: parse the payload once (status/message/output); a payload that is
    // not the expected JSON shape degrades to "ok with the raw text as output"
    // (the CLI display parser's contract, cli_app.cpp).
    soul_tool_result_fields fields = soul_parse_tool_result(result);
    if (!fields.parsed) {
        fields.ok = true;
        fields.runtime_error = false;
        fields.output = std::move(result);
    }
    // F4 (toolset.py:1439-1447): sanitize_for_tokenizer on the tool output
    // BEFORE the budget check, so tokenization of the history can never fail.
    // The flat-string message model sanitizes the single output part.
    {
        kimix::runtime::text::sanitize_options sanitize_opts;
        fields.output =
            kimix::runtime::text::sanitize_for_tokenizer(fields.output, sanitize_opts);
    }
    // F3 (toolset.py:1469-1495): the dynamic per-tool output budget -
    // min(ctx*4*0.5, remaining*4*0.9, 128 KiB) - re-measured per call from the
    // live context. Overflow turns the result into a ToolError whose output is
    // the budget-truncated text (the envelope below is built first, so the
    // history never holds a structurally corrupted payload).
    const int64_t max_output_bytes =
        soul_tool_output_byte_budget(_backend.max_context_size(), estimated_tokens());
    if (static_cast<int64_t>(fields.output.size()) > max_output_bytes) {
        const size_t cut = builtin_tools::utf8_floor_boundary(fields.output,
            static_cast<size_t>(max_output_bytes < 0 ? 0 : max_output_bytes));
        kimix::string msg = "Tool output exceeded the maximum allowed size (";
        msg += std::to_string(fields.output.size());
        msg += " bytes; limit ";
        msg += std::to_string(max_output_bytes);
        msg += " bytes). The result has been truncated.";
        kimix::string truncated = fields.output.substr(0, cut);
        fields.ok = false;
        fields.runtime_error = false;
        fields.message = std::move(msg);
        fields.output = std::move(truncated);
    }
    // E3: the model-facing envelope (message.py tool_result_to_message).
    return soul_tool_result_envelope(fields.ok, fields.runtime_error,
                                     fields.message, fields.output);
}

void KimiSoul::append_history_with_ledger(const kimix::llm::Message &message) {
    // context.py append_message: history.extend + pending += estimate(msgs).
    _session.append_history(message);
    const builtin_tools::compact::message cm = soul_to_compact_message(message);
    const builtin_tools::compact::message cms[1] = {cm};
    _ledger.add_pending_estimate(
        builtin_tools::compact::estimate_message_tokens(
            kimix::span<const builtin_tools::compact::message>(cms, 1)));
}

void KimiSoul::reanchor_ledger() {
    // kimisoul.py:2223-2232 (post-compaction): store estimate(history) +
    // count_tokens(system_prompt) as the recorded count, no pending on top.
    using namespace builtin_tools::compact;
    kimix::vector<message> cms;
    cms.reserve(_session.history().size());
    for (const kimix::llm::Message &m : _session.history()) {
        cms.push_back(soul_to_compact_message(m));
    }
    const int64_t recorded =
        estimate_message_tokens(cms) + estimate_text_tokens(effective_system_prompt());
    _ledger.reanchor(recorded);
}

int64_t KimiSoul::tool_call_buffer_tokens() const {
    // kimisoul.py _tool_call_buffer_tokens -> toolset.py
    // estimate_tool_output_token_budget (toolset.py:951-988). The old static
    // max_tokens/4 is gone: the budget is the more restrictive of 50% of the
    // model window, 90% of the remaining tokens and a 128KiB ceiling.
    if (_opts.tool_call_buffer_tokens > 0) {
        return _opts.tool_call_buffer_tokens; // explicit pin (tests, hosts)
    }
    const int64_t max_context = _backend.max_context_size();
    if (max_context <= 0) {
        return 0;
    }
    constexpr double k_bytes_per_token = 4.0;      // _TOOL_OUTPUT_BYTES_PER_TOKEN
    constexpr double k_context_fraction = 0.5;     // _TOOL_OUTPUT_CONTEXT_FRACTION
    constexpr double k_remaining_fraction = 0.9;   // _TOOL_OUTPUT_REMAINING_FRACTION
    constexpr int64_t k_default_max_bytes = 128 << 10; // _DEFAULT_TOOL_OUTPUT_MAX_BYTES
    const int64_t total_budget_bytes = static_cast<int64_t>(
        static_cast<double>(max_context) * k_bytes_per_token * k_context_fraction);
    int64_t remaining = max_context - estimated_tokens();
    if (remaining < 0) {
        remaining = 0;
    }
    const int64_t remaining_budget_bytes = static_cast<int64_t>(
        static_cast<double>(remaining) * k_bytes_per_token * k_remaining_fraction);
    int64_t bytes = total_budget_bytes;
    if (remaining_budget_bytes < bytes) {
        bytes = remaining_budget_bytes;
    }
    if (k_default_max_bytes < bytes) {
        bytes = k_default_max_bytes;
    }
    if (bytes < 0) {
        bytes = 0;
    }
    return bytes / static_cast<int64_t>(k_bytes_per_token);
}

kimix::llm::ChatResult KimiSoul::chat_with_step_retry(
    const kimix::vector<kimix::llm::Message> &messages,
    const kimix::vector<kimix::llm::Tool> &tools,
    const SoulEventCallback &on_event, int32_t step_no,
    const CancelToken *cancel) {
    // tenacity stop_after_attempt(max_retries_per_step) +
    // retry=_is_retryable_error + wait=_RETRY_WAIT (kimisoul.py:1774-1789).
    // The whole loop is ONE step: retried attempts never count against
    // max_steps (the caller increments TurnResult.steps once, after success).
    StepRetryPolicy::params p;
    p.max_attempts = _opts.loop_control.max_retries_per_step;
    p.sleep = _opts.step_retry_sleep;
    p.jitter_seed = _opts.step_retry_jitter_seed;
    const StepRetryPolicy policy(std::move(p));
    for (int32_t attempt = 1;; ++attempt) {
        // G10: LLMRequest observability record (llm_request_recorder.py) - one
        // per outbound request with the provider identity + content-addressed
        // hashes; the recorder owns the tools-snapshot dedup.
        if (_wire != nullptr) {
            recorder_provider_fields identity;
            identity.provider = _backend.provider_name();
            identity.model = _backend.model_name();
            identity.thinking_effort = _backend.thinking_effort();
            identity.has_temperature = _backend.generation_temperature_top_p(
                identity.temperature, identity.top_p);
            identity.max_tokens = _backend.output_token_budget();
            _recorder.record(_wire, identity, effective_system_prompt(), tools,
                             static_cast<int32_t>(messages.size()), "loop",
                             step_no, attempt);
        }
        kimix::llm::ChatResult res = _backend.chat(messages, tools, on_event);
        if (res.ok) {
            return res;
        }
        // G8: a cancelled request is never slept on or retried.
        if (cancel != nullptr && cancel->cancelled()) {
            return res;
        }
        const StepError error = classify_step_error(res);
        if (!is_retryable_step_error(error) || !policy.can_retry(attempt)) {
            return res;
        }
        if (error.category == StepErrorCategory::empty_response) {
            // kimisoul.py _before_step_retry_sleep: a think-only retry gets a
            // x1.5 output-budget escalation starting from 8192 so the model
            // has room to emit text after finishing its reasoning chain.
            const int64_t current = _backend.output_token_budget() > 0
                                        ? _backend.output_token_budget()
                                        : 8192;
            _backend.set_output_token_budget(
                static_cast<int64_t>(static_cast<double>(current) * 1.5));
        }
        // B7: StepRetry (kimisoul.py:2547-2558) carries the wait the policy
        // is about to sleep, the next 1-based attempt and the error type.
        if (_wire != nullptr) {
            _wire->wire_step_retry(step_no, attempt + 1, policy.max_attempts(),
                                   policy.wait_seconds(attempt, error),
                                   error.type_name, error.status);
        }
        policy.before_retry(attempt, error); // _RETRY_WAIT sleep
    }
}

void KimiSoul::apply_dynamic_injections(int32_t step_no) {
    // kimisoul.py:1629-1650 (2e.2 DYNAMIC INJECTION).
    // 2e.2a. Strip stale system reminders from previous steps/turns:
    // providers re-inject fresh reminders below, so removing old ones is safe
    // (reminders are ephemeral: one fresh copy per step, never accumulated).
    // NOTE (reference, verbatim): this must NOT notify providers of
    // "compaction" - doing so resets their throttling state every step.
    strip_system_reminders(_session.history());

    // 2e.2b. Collect from every registered provider (each in error
    // isolation) and append ONE combined reminder user message.
    InjectionStepContext ctx;
    ctx.history = &_session.history();
    ctx.is_subagent = false; // the native soul is always the root session
    ctx.max_context_tokens = _backend.max_context_size();
    ctx.token_count_with_pending = estimated_tokens();
    ctx.context_tokens = _ledger.token_count();
    ctx.context_usage =
        ctx.max_context_tokens > 0
            ? static_cast<double>(ctx.token_count_with_pending) /
                  static_cast<double>(ctx.max_context_tokens)
            : 0.0;
    ctx.step_no = step_no;
    ctx.max_steps_per_turn = _opts.loop_control.max_steps_per_turn;
    ctx.turn_seq = _turn_seq;

    const kimix::vector<DynamicInjection> injections = _injections.collect(ctx);
    const kimix::string combined = build_combined_reminder(injections);
    if (combined.empty()) {
        return;
    }
    // The reference appends through context.append_message: the native
    // analogue also feeds the ledger's pending estimate.
    kimix::llm::Message reminder;
    reminder.role = "user";
    reminder.content = combined;
    append_history_with_ledger(reminder);
}

int64_t KimiSoul::estimated_tokens() const {
    // B1: with provider-measured usage on record, the recorded count +
    // pending estimate IS the next-request input (it already covers the
    // system prompt and tool schemas, like the reference's
    // token_count_with_pending). Only fall back to the char heuristic while
    // nothing has been recorded yet.
    if (_ledger.has_recorded_usage()) {
        return _ledger.token_count_with_pending();
    }
    using namespace builtin_tools::compact;
    kimix::vector<message> cms;
    cms.reserve(_session.history().size());
    for (const kimix::llm::Message &m : _session.history()) {
        cms.push_back(soul_to_compact_message(m));
    }
    int64_t total = estimate_message_tokens(cms);
    // Non-history overhead the reference's count carries but estimate_message_
    // tokens does not. The Python trigger/display basis is
    // Context.token_count_with_pending, whose recorded usage counts the system
    // prompt and the tool schemas on every request (kimisoul.py:1230: "also
    // carries the system prompt and tool schemas"), and whose post-compaction
    // estimate re-adds the system prompt explicitly (kimisoul.py:2223-2229).
    // Mirror both so context usage and should_auto_compact see the full
    // next-request input (system + history + tool descriptions), not just the
    // history text.
    total += estimate_text_tokens(effective_system_prompt());
    for (const kimix::llm::Tool &t : tool_definitions()) {
        total += estimate_text_tokens(t.name);
        total += estimate_text_tokens(t.description);
        total += estimate_text_tokens(t.parameters_json);
    }
    return total;
}

// ---------------------------------------------------------------------------
// G7 steering + G8 cancellation API (see steer.h / cancel.h)
// ---------------------------------------------------------------------------

void KimiSoul::steer(kimix::string_view content) {
    _steer_queue.push(content); // step-boundary only (kimisoul.py:1007-1020)
}

void KimiSoul::request_steer(kimix::string_view content) {
    _steer_queue.request(content); // + wake: interrupt the in-flight step
}

bool KimiSoul::push_steer_sync(kimix::string_view content, double timeout_s) {
    return _steer_queue.push_sync(content, timeout_s);
}

size_t KimiSoul::pending_steers() const { return _steer_queue.pending(); }

void KimiSoul::clear_steers() { _steer_queue.clear(); }

size_t KimiSoul::consume_steers() {
    kimix::vector<kimix::string> steers = _steer_queue.drain();
    if (_external_steers) {
        const kimix::vector<kimix::string> external = _external_steers();
        steers.insert(steers.end(), external.begin(), external.end());
    }
    size_t injected = 0;
    for (kimix::string &steer : steers) {
        if (agent_user_input_is_empty(steer)) {
            continue; // G14: never inject an empty user message
        }
        kimix::llm::Message message;
        message.role = "user";
        message.content = std::move(steer);
        append_history_with_ledger(message);
        if (_wire != nullptr) {
            _wire->wire_steer_input(message.content);
        }
        ++injected;
    }
    return injected;
}

bool KimiSoul::compact_context(kimix::string_view custom_instruction,
                               kimix::string &error, bool manual) {
    return compact_context(custom_instruction, error, manual,
                           builtin_tools::compact::CompactMode::balanced, -1,
                           manual ? "manual" : "auto");
}

bool KimiSoul::compact_context(kimix::string_view custom_instruction,
                                 kimix::string &error, bool manual,
                                 builtin_tools::compact::CompactMode mode,
                                 int32_t preserve_depth_override,
                                 kimix::string_view trigger) {
    // B7: the Phase 3 transaction envelope (kimisoul.py:2160-2300) - a
    // provisional id pairs Begin with End; End is ALWAYS emitted, on success
    // with the post-compaction estimate, on failure with the error, so the
    // wire sees a balanced Begin/End pair.
    const kimix::string compaction_id = new_compaction_id();
    if (_wire != nullptr) {
        _wire->wire_compaction_begin(compaction_id, trigger);
    }
    int64_t shadowed = -1;
    kimix::string attempt_error;
    const bool ok = compact_context_attempt(custom_instruction, attempt_error,
                                            manual, mode, preserve_depth_override,
                                            trigger, shadowed);
    if (_wire != nullptr) {
        _wire->wire_compaction_end(
            compaction_id, trigger, shadowed, ok ? estimated_tokens() : -1,
            ok ? kimix::string_view() : kimix::string_view(attempt_error));
    }
    if (!ok) {
        error = std::move(attempt_error);
    }
    return ok;
}

bool KimiSoul::compact_context_attempt(
    kimix::string_view custom_instruction, kimix::string &error,
    bool manual, builtin_tools::compact::CompactMode mode,
    int32_t preserve_depth_override, kimix::string_view trigger,
    int64_t &shadowed_out) {
    shadowed_out = -1;
    using namespace builtin_tools::compact;
    error.clear();
    // `trigger` labels the attempt for diagnostics and the future compaction
    // ledger (the reference's trigger_override, e.g. "overflow"); the native
    // loop logs it on the auto-failure path below.
    (void)trigger;

    // One full compaction attempt: prepare -> snapshot the surface -> LLM call
    // -> stability check (C2) -> shrink check (C1) -> apply.  `changed` is the
    // C++ analogue of the reference's SurfaceChangedError: the conversation
    // surface moved while the summary was generated.
    enum class attempt_result { applied, noop, failed, changed };
    auto run_once = [&](attempt_result &outcome) {
        outcome = attempt_result::failed;
        const kimix::vector<kimix::llm::Message> &history = _session.history();

        // Convert to compact::message (text parts + a faithful tool-call count,
        // which the balanced-cut fold needs) and let the kernel compute the
        // preserve boundary the same way SimpleCompaction.prepare does.
        kimix::vector<message> cms;
        cms.reserve(history.size());
        for (const kimix::llm::Message &m : history) {
            cms.push_back(soul_to_compact_message(m));
        }
        // Preserve depth: adaptive_preserve_depth over the tail, bounded by the
        // same min/max the reference passes from LoopControl (1 / 2 by
        // default). An explicit override (the overflow recovery passes
        // context_overflow_preserve_depth) bypasses the adaptive resolution
        // exactly like CompactionOptions.preserve_depth_override.
        const int32_t depth =
            preserve_depth_override >= 0
                ? preserve_depth_override
                : adaptive_preserve_depth(cms, _opts.min_preserved_turns,
                                          _opts.max_preserved_turns);
        const preserve_split split =
            resolve_preserve_split(cms, depth, /*balanced_cuts=*/true);
        if (split.unbalanced) {
            error =
                "cannot compact: the history has a tool result with no matching "
                "tool call (unbalanced tool pairing)";
            return;
        }
        if (!split.compact) {
            error = "nothing to compact (history is all preserved tail)";
            return;
        }
        // The compacted region is the contiguous cut [0, preserve_start); the
        // preserved tail is history[preserve_start:] plus, when the kernel
        // reports it, a primacy copy of history[0] (the reference's
        // non-contiguous ``[messages[0]] + messages[k:]`` shape). The summarizer
        // only ever sees the compacted region, so the flattened request keeps
        // the contiguous slice.
        const size_t preserve_start = split.preserve_start_index;

        // Assemble the compaction prompt through the kernels (slice + legacy
        // flattened text + cascade detection).
        prepare_request req;
        req.messages = cms;
        req.preserve_start_index = preserve_start;
        req.options.mode = mode;
        req.custom_instruction = custom_instruction;
        // Keep the bodies alive for the view fields of `req` (assigning a
        // view straight off the returned temporary would dangle).
        const kimix::string prompt_compact = soul_prompt_compact();
        const kimix::string prompt_cascade = soul_prompt_compact_cascade();
        req.prompt_compact = prompt_compact;
        req.prompt_compact_cascade = prompt_cascade;
        prepare_result prep;
        const tool_error terr = prepare_compaction_input(req, prep);
        if (terr.failed()) {
            error = terr.message;
            return;
        }

        // Phase 3 stability check (compaction.py _surface_fingerprint): snapshot
        // the conversation surface BEFORE the LLM call so a mid-call mutation
        // can be detected afterwards.
        const surface_fingerprint before = compute_surface_fingerprint(cms);

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
        // compact::build_compact_message_text already ends the flattened
        // message with prompt_text (compaction.py's compact_message TextPart
        // sequence), so the instruction is sent exactly once - do NOT append
        // prep.prompt_text again here.
        user.content = prep.compact_message_text;
        summary_messages.push_back(std::move(user));

        const kimix::llm::ChatResult res = [&]() {
            // G10: the compaction request trace (kind "compaction", no
            // turn_step, dropped_count = the compacted region's message
            // count). The compactor's own system prompt (not the agent's) is
            // hashed; the tool table is empty.
            if (_wire != nullptr) {
                recorder_provider_fields identity;
                identity.provider = _backend.provider_name();
                identity.model = _backend.model_name();
                identity.thinking_effort = _backend.thinking_effort();
                identity.has_temperature = _backend.generation_temperature_top_p(
                    identity.temperature, identity.top_p);
                identity.max_tokens = _backend.output_token_budget();
                _recorder.record(_wire, identity, summary_messages.front().content,
                                 /*tools=*/{}, static_cast<int32_t>(summary_messages.size()),
                                 "compaction", /*turn_step=*/-1, /*attempt=*/1,
                                 static_cast<int32_t>(preserve_start));
            }
            return _backend.chat(summary_messages, {}, {});
        }();
        if (!res.ok) {
            error = "compaction LLM call failed: " + res.error;
            return;
        }
        if (res.content.empty()) {
            error = "compaction returned an empty summary";
            return;
        }

        // Stability check: history_len and last_message_text are the
        // authoritative signals (compaction.py:604-612); a token_count drift
        // with an unchanged surface is diagnostics-only and never fails.
        kimix::vector<message> after_cms;
        after_cms.reserve(_session.history().size());
        for (const kimix::llm::Message &m : _session.history()) {
            after_cms.push_back(soul_to_compact_message(m));
        }
        const surface_fingerprint after = compute_surface_fingerprint(after_cms);
        if (after.history_len != before.history_len ||
            after.last_message_text != before.last_message_text) {
            outcome = attempt_result::changed;
            return;
        }

        // Phase 3 shrink check (compaction.py:621-659, C1): a summary that is
        // not smaller than the region it replaces is silently discarded - the
        // history is left verbatim (a no-op compaction), never replaced by a
        // longer "summary".
        shadowed_out =
              estimate_message_tokens(kimix::span<const message>(cms.data(), preserve_start));
          const int64_t shadowed_tokens = shadowed_out;
        const int64_t summary_tokens = estimate_text_tokens(res.content);
        if (summary_tokens >= shadowed_tokens) {
            outcome = attempt_result::noop;
            return;
        }

        // C7 (compaction.py:671-687): Hermes-style todo re-injection - the
        // active (unfinished) todo list is deterministically appended to the
        // compaction output so the plan survives context compression. Gated by
        // [loop_control] todo_compact_injection_enabled (default ON) with
        // todo_compact_injection_max_items (default 20); failure-isolated, the
        // loader never raises (session_todos falls back to an empty state).
        const LoopControl &lc = _opts.loop_control;
        kimix::string summary_text(res.content);
        if (lc.todo_compact_injection_enabled) {
            const builtin_tools::todo::todo_state &todos =
                builtin_tools::todo::session_todos(_session.tool_session());
            const kimix::optional<kimix::string> injection = format_todo_injection(
                todos.todos, lc.todo_compact_injection_max_items);
            if (injection.has_value()) {
                summary_text += "\n\n";
                summary_text += *injection;
            }
        }

        // C8 (kimisoul.py:2160-2198): durable pre-compaction export of the
        // whole history to <work_dir>/.kimix_cache/context_compacted.md BEFORE
        // the history is replaced (the dropped region stays recoverable), then
        // the path is advertised in the system prompt. Export failure is
        // warning-only: the reference continues with compact_export_path=None
        // and a failed export must never abort the compaction.
        _compact_export_path = soul_export_precompaction(
            _session, static_cast<uint64_t>(estimated_tokens()));
        _compact_export_pending = true;
        if (_compact_export_path.empty()) {
            std::fprintf(stderr,
                         "failed to export pre-compaction context to "
                         ".kimix_cache/context_compacted.md\n");
        }

        // Replace history: summary message + [primacy copy of the first
        // message] + the preserved tail (the only two shapes the reference
        // emits).
        _session.history() =
            soul_apply_preserve_split(history, split, summary_text);
        // Re-index the post-compaction history (mark old turns compacted,
        // index the summary + tail as fresh turns) - kimisoul.py's
        // mark_compacted() + append_message(compaction_result.messages).
        _session.on_history_compacted();
        // Re-anchor the token ledger on the compacted context
        // (kimisoul.py:2223-2232): recorded = estimate(history) + system
        // prompt, pending = 0.
        reanchor_ledger();
        ++_compactions;
        // G9: notify the injection providers that the context was compacted
        // (kimisoul.py:2238, _notify_injection_providers_compacted) - a real
        // compaction lets throttled reminders re-arm (budget re-alert,
        // compact reminder, context-meter fresh report, churn counters).
        // Per the reference, failures are isolated inside the registry so a
        // buggy provider cannot abort compaction.
        _injections.notify_context_compacted();
        outcome = attempt_result::applied;
    };

    attempt_result outcome = attempt_result::failed;
    run_once(outcome);
    if (outcome == attempt_result::changed) {
        // SurfaceChangedError -> re-prepare and try once more
        // (kimisoul.py _compact_with_stability_retry).
        run_once(outcome);
    }
    switch (outcome) {
    case attempt_result::applied:
    case attempt_result::noop:
        // A discarded (non-shrinking) summary leaves the context unchanged and
        // is not an error - the reference degrades to a no-op compaction.
        return true;
    case attempt_result::changed:
        // A second SurfaceChangedError is a classified manual failure
        // (ManualCompactionError("changed"), mapped by slash.py); an auto
        // compaction reports and lets the caller continue.
        error = manual
                    ? kimix::string(
                          "History changed during compaction; try again.")
                    : kimix::string(
                          "conversation changed during compaction");
        return false;
    case attempt_result::failed:
        return false;
    }
    return false;
}

TurnResult KimiSoul::turn(kimix::string_view user_input,
                          const SoulEventCallback &on_event) {
    // No outside token: the turn runs against a never-cancelled dummy.
    static const CancelToken k_never;
    return turn(user_input, on_event, k_never);
}

TurnResult KimiSoul::turn(kimix::string_view user_input,
                          const SoulEventCallback &on_event,
                          const CancelToken &cancel) {
    TurnResult out;
    // Empty-input guard (kimi_cli.soul.run_soul, soul/__init__.py:329-341 and
    // KimiSoul.steer): never start a turn for blank input. Without it the soul
    // appends an empty `user` message and the model answers a spurious "you sent
    // an empty message" turn. No history mutation, no LLM call.
    if (agent_user_input_is_empty(user_input)) {
        out.ignored = true;
        return out;
    }
    // G8/G7: arm the turn's outside handles and guarantee teardown on EVERY
    // exit path (kimisoul.py:1090-1118's run()/finally): the streaming abort
    // check detaches from the backend, the steer queue closes (push_sync
    // waiters unblock), the running flag clears, and the wire sees TurnEnd
    // exactly once.
    _turn_cancel = &cancel;
    _turn_abort.cancel = &cancel;
    _turn_abort.queue = &_steer_queue;
    _backend.set_abort_check(&_turn_abort);
    _steer_queue.reopen();
    // G7 stale-steer semantics (kimisoul.py:1148-1158): discard any steers
    // queued while no turn was running and clear the wake event so a steer
    // arriving later in THIS turn can interrupt a streaming step.
    _steer_queue.clear();
    _steer_queue.reset_wake();
    struct turn_teardown {
        KimiSoul &self;
        ~turn_teardown() {
            self._backend.set_abort_check(nullptr);
            self._steer_queue.close();
            self._turn_cancel = nullptr;
            self._turn_abort.cancel = nullptr;
            if (self._wire != nullptr) {
                self._wire->wire_turn_end();
            }
        }
    } teardown{*this};
    // B7: the turn frame on the wire (kimisoul.py:1108-1118).
    if (_wire != nullptr) {
        _wire->wire_turn_begin(user_input);
    }
    auto &history = _session.history();
    // G9: fresh turn identity for the per-turn provider state (budget levels,
    // churn alert dedup) - the reference's _current_turn_id change.
    ++_turn_seq;

    kimix::llm::Message user_msg;
    user_msg.role = "user";
    user_msg.content.assign(user_input.data(), user_input.size());
    // append_history feeds the in-memory history index (D4) as well as the
    // history vector (the reference's context._on_append hook), and the
    // ledger picks up the incremental estimate of the appended message.
    append_history_with_ledger(user_msg);

    const kimix::vector<kimix::llm::Tool> tools = tool_definitions();
    const LoopControl &lc = _opts.loop_control;

    // A1/A2/A4: fresh per-turn loop-detector and verification-gate state
    // (KimiToolset's turn-id change at toolset.py:1070-1076 clears the same
    // counters; the verification gate's _sync_turn resets the nudge budget).
    _loop_guard.begin_turn();
    _verification_gate.begin_turn();
    _verification_gate.set_max_nudges(lc.verification_gate_max_nudges);
    // The previous step's calls, handed to the guard's begin_step each step
    // (kimisoul.py:1715-1717 _last_tool_calls).
    kimix::vector<std::pair<kimix::string, kimix::string>> last_tool_calls;
    // A2: bounded loop-recovery budget (kimisoul.py:1378-1439).
    int32_t loop_recovery_rounds = 0;

    // Emit a user-visible text notice through the streaming callback (the
    // reference's wire_send(TextPart(...))).
    const auto emit_text = [&on_event](kimix::string_view text) {
        if (on_event) {
            kimix::llm::Chunk chunk;
            chunk.ok = true;
            chunk.content.assign(text.data(), text.size());
            on_event(chunk);
        }
    };

    // A8: the user-visible stop explanation once the model kept producing
    // empty / think-only responses through every retry and escalation
    // (kimisoul.py:1794-1806, the wire TextPart wording verbatim).
    const auto stop_think_only = [&]() -> TurnResult {
        constexpr kimix::string_view k_notice =
            "\n(The model produced only thinking content without a response. "
            "Stopping this turn.)\n";
        emit_text(k_notice);
        out.ok = false;
        out.error_kind = TurnErrorKind::empty_response_exhausted;
        out.error.assign(k_notice.data(), k_notice.size());
        // Trim the surrounding newlines for the structured error field.
        while (!out.error.empty() &&
               (out.error.back() == '\n' || out.error.back() == ' ')) {
            out.error.pop_back();
        }
        while (!out.error.empty() && out.error.front() == '\n') {
            out.error.erase(out.error.begin());
        }
        return out;
    };

    // One full step attempt: build the request, chat with the step-retry
    // policy, and - on a provider-confirmed context overflow - force-compact
    // and re-enter, bounded by the shared overflow budget (context_overflow.py
    // / kimisoul.py:1815-1882). The budget is per top-level step and shared
    // across the re-entries, so the recovery loop cannot recurse unboundedly.
    // `overflow_recovery_failed` is set when the forced compaction itself
    // fails: the reference raises SessionRestartRequired for that case (the
    // session restarts even though a bare 400 is not restartable).
    const auto run_step = [&](bool &overflow_recovery_failed,
                               int32_t step_no) -> kimix::llm::ChatResult {
        OverflowRecoveryState overflow(lc.context_overflow_retries);
        for (;;) {
            kimix::vector<kimix::llm::Message> messages;
            messages.reserve(history.size() + 1);
            kimix::llm::Message sys;
            sys.role = "system";
            sys.content = effective_system_prompt();
            messages.push_back(std::move(sys));
            // G9 (kimisoul.py:1704, 2e.4 HISTORY NORMALIZATION): adjacent
            // user messages are merged for the request, except ephemeral
            // <system-reminder> messages. Applied to the request copy only -
            // live history keeps the standalone reminder message so the
            // strip-and-reinject cycle of the next step still finds it.
            const kimix::vector<kimix::llm::Message> effective_history =
                normalize_history(history);
            for (const kimix::llm::Message &m : effective_history) {
                messages.push_back(m);
            }
            kimix::llm::ChatResult res = chat_with_step_retry(
                messages, tools, on_event, step_no,
                _turn_cancel);
            if (res.ok) {
                return res;
            }
            if (!is_context_overflow_error(res.error, res.error_status) ||
                !overflow.can_retry()) {
                return res;
            }
            // Context window exceeded: force an AGGRESSIVE compaction
            // (preserve_depth_override = context_overflow_preserve_depth,
            // trigger "overflow"), bypassing should_auto_compact entirely.
            kimix::string cerr;
            if (!compact_context("", cerr, /*manual=*/false,
                                 builtin_tools::compact::CompactMode::aggressive,
                                 lc.context_overflow_preserve_depth, "overflow")) {
                // kimisoul.py:1855-1868: the recovery compaction itself
                // failed - "context overflow recovery compaction failed,
                // restarting session": preserve the original error and fall
                // through to the (restartable) session-restart machinery.
                std::fprintf(stderr,
                             "overflow recovery compaction failed: %s\n",
                             cerr.c_str());
                overflow_recovery_failed = true;
                return res;
            }
            out.compacted = true;
            overflow.consumed();
            // Re-enter the same step on the compacted context.
        }
    };

    for (int32_t step = 0; step < lc.max_steps_per_turn; ++step) {
        // G8: the outside asked to stop - abort at this step boundary
        // (run_soul's RunCancelled). The history keeps everything up to the
        // last completed step: the cancelled step's partial output is never
        // appended (the assistant message is recorded only after a successful
        // chat), the reference's "partial output is not grown into the
        // context" cleanup.
        if (cancel_requested()) {
            out.cancelled = true;
            out.error_kind = TurnErrorKind::cancelled;
            return out;
        }
        // A1: per-step dedup/loop window setup with the previous step's calls
        // (toolset.py begin_step, kimisoul.py:1715-1717). This also clears any
        // previous force-stop trip: the trip is consulted between end_step and
        // the next begin_step (outcome resolution below).
        _loop_guard.begin_step(last_tool_calls);
        // B7: one StepBegin per step (kimisoul.py:2b).
        if (_wire != nullptr) {
            _wire->wire_step_begin(step + 1);
        }

        // Auto-compaction check before every step (kimisoul.py's
        // should_auto_compact gate, compaction.py:239-278). All inputs come
        // from [loop_control]: the trigger ratio, the reserved context, the
        // safety margin, the model output budget, and the LIVE tool-call
        // buffer estimate (not the old max_tokens/4). The token count is the
        // ledger-anchored token_count_with_pending when provider usage has
        // been recorded, else the char heuristic. The `history.size() >= 4`
        // shortcut is port-only: a shorter history can never be compacted
        // anyway (resolve_preserve_split reports "nothing to compact"), so it
        // only skips a no-op attempt.
        if (_opts.auto_compact && history.size() >= 4) {
            builtin_tools::compact::compaction_trigger_config cfg;
            cfg.trigger_ratio = lc.compaction_trigger_ratio;
            cfg.max_context_size = _backend.max_context_size();
            cfg.reserved_context_size = lc.reserved_context_size;
            cfg.max_tokens = _opts.max_tokens;
            cfg.tool_call_buffer_tokens = tool_call_buffer_tokens();
            cfg.safety_margin_tokens = _opts.safety_margin_tokens;
            const int64_t tokens = estimated_tokens();
            if (builtin_tools::compact::should_auto_compact(tokens, cfg)) {
                kimix::string cerr;
                if (compact_context("", cerr, /*manual=*/false)) {
                    out.compacted = true;
                } else {
                    // Auto-compaction is best-effort (kimisoul.py:1260-1261
                    // logs and continues): report the failure instead of
                    // discarding it silently, then keep the turn going.
                    std::fprintf(stderr, "auto-compaction failed: %s\n",
                                 cerr.c_str());
                }
            }
        }

        // G9 (kimisoul.py:1629-1650, 2e.2 DYNAMIC INJECTION): strip the
        // previous step's stale reminders, then collect fresh ones from all
        // providers and append them as one <system-reminder> user message
        // before the request is built.
        apply_dynamic_injections(step + 1);

        // G7: clear the wake event before the step races it (kimisoul.py:1281).
        _steer_queue.reset_wake();

        bool overflow_recovery_failed = false;
        kimix::llm::ChatResult res = run_step(overflow_recovery_failed, step + 1);
        // G8: cancellation wins over every other outcome, success included
        // (the request may have been interrupted mid-stream and still
        // reported ok - RunCancelled is raised regardless).
        if (cancel_requested()) {
            out.cancelled = true;
            out.error_kind = TurnErrorKind::cancelled;
            return out;
        }
        // G7: the wake event fired before the step resolved - the steer
        // interrupted the in-flight step (the abort check stops the HTTP
        // read, so res is failed/short). The partial output is NOT grown
        // into the context; the steers are injected as follow-up user
        // messages and the step ends interrupted on the wire
        // (kimisoul.py:1298-1310).
        const bool steer_wake = _steer_queue.wake_requested();
        if (!res.ok && steer_wake) {
            consume_steers();
            if (_wire != nullptr) {
                _wire->wire_step_interrupted();
            }
            continue;
        }
        if (!res.ok) {
            StepError error = classify_step_error(res);
            if (error.category == StepErrorCategory::empty_response) {
                // A8: retries and x1.5 escalation exhausted on an empty /
                // think-only response - stop the turn with the reference's
                // user-visible explanation (never the restart machinery).
                return stop_think_only();
            }
            // A6: the step failed for good - automatic session restart,
            // bounded by LoopControl.max_session_restarts
            // (kimisoul.py:1873-1882 -> kimi_agent_sdk._session.py:940-999).
            // Restartable errors mirror _is_restartable_error: transient
            // statuses (408/429/5xx), non-HTTP failures, and the failed
            // overflow-recovery compaction; deterministic 4xx fails
            // immediately instead of burning the budget.
            bool restartable =
                overflow_recovery_failed ||
                error.category == StepErrorCategory::network ||
                error.category == StepErrorCategory::timeout ||
                (error.status == 408) || (error.status >= 500) ||
                (error.status == 429);
            while (!cancel_requested() && restartable &&
                   out.session_restarts < lc.max_session_restarts) {
                ++out.session_restarts;
                // The reference's user-visible wire text:
                // "\n⚠️ Connection lost (<type>). Restarting session (k/n)...\n"
                kimix::string notice = "\n⚠️ Connection lost (";
                notice += error.type_name;
                notice += "). Restarting session (";
                notice += std::to_string(out.session_restarts);
                notice += "/";
                notice += std::to_string(lc.max_session_restarts);
                notice += ")...\n";
                emit_text(notice);
                // _restart() preserves the conversation context and re-invokes
                // the same user input; the native analogue retries the step on
                // the same history (the provider state the reference recreates
                // lives behind IChatBackend).
                overflow_recovery_failed = false;
                res = run_step(overflow_recovery_failed, step + 1);
                if (res.ok) {
                    break;
                }
                error = classify_step_error(res);
                if (error.category == StepErrorCategory::empty_response) {
                    return stop_think_only();
                }
                restartable =
                    overflow_recovery_failed ||
                    error.category == StepErrorCategory::network ||
                    error.category == StepErrorCategory::timeout ||
                    (error.status == 408) || (error.status >= 500) ||
                    (error.status == 429);
            }
                        if (cancel_requested()) {
                // G8: cancelled during the restart attempts.
                out.cancelled = true;
                out.error_kind = TurnErrorKind::cancelled;
                return out;
            }
if (!res.ok) {
                out.ok = false;
                // The reference surfaces the original provider error once the
                // restart budget is exhausted ("Session restart limit reached
                // (k/n)" is log-only; the original error is re-raised).
                out.error = "chat failed: " + res.error;
                out.error_kind =
                    restartable ? TurnErrorKind::session_restart_exhausted
                                : TurnErrorKind::chat_failed;
                return out;
            }
        }

        // A successful step: one LLM round-trip (retries inside run_step do
        // not count against max_steps).
        ++out.steps;

        // B1: anchor the token ledger on the provider-measured usage BEFORE
        // growing the context (kimisoul.py:1900 update_token_count(usage.input)
        // marks the count for the context before the step; the recorded input
        // mirrors kosong TokenUsage.input = prompt + cache_read +
        // cache_creation).
        if (res.prompt_tokens > 0 || res.cached_tokens > 0 ||
            res.cache_creation_tokens > 0) {
            _ledger.update_token_count(res.prompt_tokens + res.cached_tokens +
                                       res.cache_creation_tokens);
        }

 // B7: StatusUpdate (kimisoul.py:1937-1947) - the step's provider-measured
 // token usage plus the post-anchor context snapshot, BEFORE the assistant
 // message grows the context (the reference updates the token count first
 // and reads the usage snapshot afterwards).
 if (_wire != nullptr) {
 const int64_t mctx = _backend.max_context_size();
 const int64_t ctx_tokens = estimated_tokens();
 _wire->wire_status_update(
 mctx > 0 ? static_cast<double>(ctx_tokens) / static_cast<double>(mctx)
 : 0.0,
 ctx_tokens, mctx, res.prompt_tokens, res.completion_tokens,
 res.cached_tokens, res.cache_creation_tokens);
 }

        // Record the assistant message (text + tool calls).
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.content = res.content;
        assistant.thinking = res.reasoning;
        assistant.thinking_signature = res.signature;
        assistant.tool_calls = res.tool_calls;
        append_history_with_ledger(assistant);

        // Execute every tool call and append the tool results. A1: each call
        // is fed to the loop guard; the graded <system-reminder> reminder is
        // appended to the tool result the model sees (toolset.py handle() ->
        // _append_reminder_to_return_value).
        for (const kimix::llm::ToolCall &tc : res.tool_calls) {
            kimix::string terr;
            kimix::string result =
                execute_tool_call(tc.name, tc.arguments, terr, tc.id);
            const ToolLoopGuard::verdict verdict =
                _loop_guard.record_call(tc.name, tc.arguments);
            result += verdict.reminder;
            kimix::llm::Message tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.content = std::move(result);
            append_history_with_ledger(tool_msg);
        }
        // Update the dedup tracking for the next step (toolset.py end_step,
        // kimisoul.py:1913-1915).
        last_tool_calls = _loop_guard.end_step();

        // A3 (kimisoul.py:1923-1935): a step that emitted a non-empty thinking
        // block is progress - reset the loop detectors (counters AND any trip
        // set mid-step) BEFORE the outcome checks consult force_stop_turn().
        if (!res.reasoning.empty()) {
            _loop_guard.reset_loop_detectors();
        }

        // ── Outcome resolution (kimisoul.py:1937-1956) ────────────────────
        kimix::string_view stop_reason;
        if (_loop_guard.force_stop_turn()) {
            stop_reason = "tool_call_repeat";
        } else if (res.tool_calls.empty()) {
            stop_reason = "no_tool_calls";
        }
        // G7 (kimisoul.py:1352-1357): a step that is about to stop first
        // drains the steer queues - any injected steer forces ANOTHER step
        // instead of ending the turn, so a mid-turn follow-up is never dropped.
        if (!stop_reason.empty() && consume_steers() > 0) {
            continue;
        }
        if (!stop_reason.empty()) {
            // ── A4: Verification Gate (kimisoul.py:1354-1369) ────────────
            // Before ending on no_tool_calls, check whether the turn is
            // actually finished (todos done, verifications run).
            if (stop_reason == "no_tool_calls" && lc.verification_gate_enabled) {
                const builtin_tools::todo::todo_state &todos =
                    builtin_tools::todo::session_todos(_session.tool_session());
                kimix::optional<kimix::string> gate_msg =
                    _verification_gate.check(history, todos.todos);
                if (gate_msg.has_value()) {
                    // The reference wraps the gate text in system_reminder()
                    // (soul/message.py:24-25) and appends a user message.
                    kimix::llm::Message nudge;
                    nudge.role = "user";
                    nudge.content = "<system-reminder>\n";
                    nudge.content += *gate_msg;
                    nudge.content += "\n</system-reminder>";
                    append_history_with_ledger(nudge);
                    continue; // do not end the turn; force another step
                }
            }

            // ── A2: Loop-recovery gate (kimisoul.py:1371-1439) ───────────
            // A tool_call_repeat stop must NOT end the turn silently: feed up
            // to _MAX_LOOP_RECOVERY_ROUNDS plain-user recovery prompts that
            // restate the top-level requirement (a REAL user message, never a
            // <system-reminder>, so it survives reminder stripping). Only after
            // the budget is exhausted do we fall through to a synthesized text
            // answer.
            if (stop_reason == "tool_call_repeat") {
                constexpr int32_t k_max_loop_recovery_rounds =
                    3; // _MAX_LOOP_RECOVERY_ROUNDS (kimisoul.py:149)
                if (loop_recovery_rounds < k_max_loop_recovery_rounds) {
                    ++loop_recovery_rounds;
                    // _make_loop_recovery_prompt (kimisoul.py:153-176): the
                    // detector reason and repeated tool are inserted after the
                    // marker line.
                    kimix::string recovery_text =
                        "[loop-recovery] Stop repeating tool calls.";
                    const kimix::string_view loop_reason =
                        _loop_guard.force_stop_reason();
                    if (!loop_reason.empty()) {
                        recovery_text += "\nLoop detector: ";
                        recovery_text.append(loop_reason.data(),
                                             loop_reason.size());
                        recovery_text += ".";
                    }
                    const kimix::string_view loop_tool =
                        _loop_guard.force_stop_tool();
                    if (!loop_tool.empty()) {
                        recovery_text += "\nRepeated tool: ";
                        recovery_text.append(loop_tool.data(), loop_tool.size());
                        recovery_text += ".";
                    }
                    recovery_text += "\nOriginal task: ";
                    recovery_text += agent_user_input_is_empty(user_input)
                                         ? kimix::string("<original user request "
                                                         "is not available in "
                                                         "this context>")
                                         : kimix::string(user_input);
                    recovery_text +=
                        "\nNext: do something different, or stop tools and "
                        "summarize progress/blockers.";
                    recovery_text +=
                        "\n(recovery " + std::to_string(loop_recovery_rounds) +
                        "/" + std::to_string(k_max_loop_recovery_rounds) + ")";
                    // The reference's user-visible wire text
                    // (kimisoul.py:1401-1405).
                    emit_text("\n[Recovering from repeated tool calls...]\n");
                    kimix::llm::Message recovery_msg;
                    recovery_msg.role = "user";
                    recovery_msg.content = std::move(recovery_text);
                    append_history_with_ledger(recovery_msg);
                    // The next begin_step re-arms the guard (fresh per-step
                    // window, force-stop cleared).
                    continue;
                }

                // Recovery budget exhausted: never return with no final text.
                // Synthesize a fallback that references the user request so the
                // session hands control back cleanly (kimisoul.py:1418-1439,
                // _synthesize_loop_recovery_text at 179-190).
                kimix::string final_text =
                    "I could not complete the task without repeating tool "
                    "calls.\n\nOriginal request: ";
                final_text += agent_user_input_is_empty(user_input)
                                  ? kimix::string("<original user request is "
                                                  "not available in this "
                                                  "context>")
                                  : kimix::string(user_input);
                final_text +=
                    "\n\nProgress is preserved above. Rephrase, narrow the "
                    "scope, or add context so I can continue.";
                kimix::llm::Message fallback;
                fallback.role = "assistant";
                fallback.content = final_text;
                append_history_with_ledger(fallback);
                emit_text(final_text);
                out.ok = true;
                out.content = std::move(final_text);
                return out;
            }

            // no_tool_calls: the model's plain-text answer ends the turn.
            out.ok = true;
            out.content = res.content;
            return out;
        }
    // G7 (kimisoul.py:1531-1532): consume any pending steers between
    // steps before the next iteration.
    consume_steers();
    }

    // A11: typed MaxStepsReached carrying the count, with the reference's
    // wording (kimi_cli/soul/__init__.py:51-57).
    out.ok = false;
    out.error_kind = TurnErrorKind::max_steps_reached;
    out.max_steps = lc.max_steps_per_turn;
    out.error = kimix::format("Max number of steps reached: {}",
                              lc.max_steps_per_turn);
    return out;
}

} // namespace kimix::agent
