// compact_tool.cpp - Implementation of the compact context-compaction kernels.
//
// Ports the pure decision and string-assembly kernels from
// D:/kimi-agent/kimi-cli/src/kimi_cli/soul/compaction.py and
// D:/kimi-agent/kimi-cli/src/kimi_cli/utils/tokens.py.
// See compact_tool.h for the full ownership map and references.

#include "builtin_tools/compact_tool.h"

#include "builtin_tools/utf8_util.h"

#include <cstring>
#include <limits>

#include <core/kimix_core.h>

namespace kimix::builtin_tools::compact {

namespace {

// ── Internal helpers (unity-build safe, compact_ prefix) ─────────────────────

// Saturating int64_t addition. Caps at INT64_MAX on overflow.
inline int64_t compact_sat_add(int64_t a, int64_t b) noexcept {
    if (a > 0 && b > std::numeric_limits<int64_t>::max() - a) {
        return std::numeric_limits<int64_t>::max();
    }
    if (a < 0 && b < std::numeric_limits<int64_t>::min() - a) {
        return std::numeric_limits<int64_t>::min();
    }
    return a + b;
}

// Saturating int64_t subtraction (a - b). Caps at INT64_MAX/min.
inline int64_t compact_sat_sub(int64_t a, int64_t b) noexcept {
    if (b > 0 && a < std::numeric_limits<int64_t>::min() + b) {
        return std::numeric_limits<int64_t>::min();
    }
    if (b < 0 && a > std::numeric_limits<int64_t>::max() + b) {
        return std::numeric_limits<int64_t>::max();
    }
    return a - b;
}

inline int64_t compact_max(int64_t a, int64_t b) noexcept { return (a > b) ? a : b; }
inline int64_t compact_min(int64_t a, int64_t b) noexcept { return (a < b) ? a : b; }

// ASCII-only lowercasing.
inline char compact_to_lower_ascii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return c;
}

// True if the code point is in one of the CJK ranges used by tokens.py.
inline bool compact_is_cjk_code_point(uint32_t cp) noexcept {
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return true;
    if (cp >= 0x3400u && cp <= 0x4DBFu) return true;
    if (cp >= 0x20000u && cp <= 0x2EBEFu) return true;
    if (cp >= 0xAC00u && cp <= 0xD7AFu) return true;
    if (cp >= 0x3040u && cp <= 0x309Fu) return true;
    if (cp >= 0x30A0u && cp <= 0x30FFu) return true;
    if (cp >= 0xFF00u && cp <= 0xFFEFu) return true;
    return false;
}

// Count byte-exact occurrences of needle in haystack.
size_t compact_count_substr(kimix::string_view haystack,
                            kimix::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return 0;
    }
    size_t count = 0;
    size_t pos = 0;
    while (true) {
        pos = haystack.find(needle, pos);
        if (pos == kimix::string_view::npos) {
            break;
        }
        ++count;
        pos += needle.size();
    }
    return count;
}

// True when haystack contains needle (byte exact).
bool compact_contains(kimix::string_view haystack, kimix::string_view needle) noexcept {
    return haystack.find(needle) != kimix::string_view::npos;
}

// Lowercase a string using ASCII-only lowercasing.
kimix::string compact_lower_ascii(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(compact_to_lower_ascii(c));
    }
    return out;
}

// Decision-section guidance block (compaction.py:365-375).
kimix::string_view compact_decision_section_guidance() noexcept {
    return "\n\n**Required Summary Sections:**\n"
           "Your summary MUST include these two sections with exact headings:\n"
           "## Decisions & Conclusions\n"
           "- Decisions already made and their rationale; approaches already "
           "evaluated and rejected (with the rejection reason); assumptions "
           "currently treated as valid.\n"
           "## Verification Status\n"
           "- What has been verified to work (and how it was verified); what "
           "remains unverified.";
}

} // namespace

// ── Message helpers ──────────────────────────────────────────────────────────

kimix::string extract_text(const message &msg, kimix::string_view sep) noexcept {
    kimix::string out;
    bool first = true;
    for (const auto &part : msg.content) {
        if (part.type == "text") {
            if (!first) {
                out.append(sep);
            }
            out.append(part.text);
            first = false;
        }
    }
    return out;
}

bool has_think_part(const message &msg) noexcept {
    for (const auto &part : msg.content) {
        if (part.type == "think") {
            return true;
        }
    }
    return false;
}

bool is_user_or_assistant(const message &msg) noexcept {
    return msg.role == "user" || msg.role == "assistant";
}

// ── CompactMode and options ──────────────────────────────────────────────────

CompactMode parse_compact_mode(kimix::string_view mode) noexcept {
    if (mode == "balanced") return CompactMode::balanced;
    if (mode == "aggressive") return CompactMode::aggressive;
    if (mode == "retentive") return CompactMode::retentive;
    if (mode == "technical") return CompactMode::technical;
    return CompactMode::retentive;
}

kimix::string_view mode_guidance(CompactMode mode) noexcept {
    switch (mode) {
    case CompactMode::balanced:
        return "**Compaction Style Guidance:** Be balanced. Preserve essential context "
               "while condensing redundant information. Keep current task state, errors "
               "and solutions, code state, design decisions, and TODO items.";
    case CompactMode::aggressive:
        return "**Compaction Style Guidance:** Be aggressive. Prioritize brevity, drop "
               "intermediate attempts, exploratory dead-ends, and low-priority details. "
               "Keep only the essential facts, decisions, and current state.";
    case CompactMode::retentive:
        return "**Compaction Style Guidance:** Be retentive. Preserve more verbatim detail, "
               "especially recent reasoning steps, exact values, file paths, and user "
               "preferences. Do not over-compress.";
    case CompactMode::technical:
        return "**Compaction Style Guidance:** Focus on technical specifics. Prioritize "
               "code snippets, file paths, error messages, stack traces, architectural "
               "decisions, and current implementation state. Summarize conversational filler.";
    }
    return "";
}

// ── Auto-compaction trigger ──────────────────────────────────────────────────

bool should_auto_compact(int64_t token_count,
                         const compaction_trigger_config &cfg) noexcept {
    if (cfg.max_context_size <= 0) {
        return false;
    }

    const int64_t output_size = compact_sat_add(cfg.max_tokens, cfg.safety_margin_tokens);
    const int64_t reserved = compact_max(
        compact_max(cfg.tool_call_buffer_tokens, cfg.reserved_context_size),
        output_size);
    const int64_t min_input_room = compact_max(
        static_cast<int64_t>(0), compact_sat_sub(cfg.max_context_size, cfg.reserved_context_size));
    const int64_t effective_reserved = compact_min(reserved, min_input_room);

    const double ratio_threshold = static_cast<double>(cfg.max_context_size) * cfg.trigger_ratio;
    if (static_cast<double>(token_count) >= ratio_threshold) {
        return true;
    }
    const int64_t with_reserved = compact_sat_add(token_count, effective_reserved);
    if (with_reserved >= cfg.max_context_size) {
        return true;
    }
    return false;
}

// ── Adaptive preserve depth ──────────────────────────────────────────────────

int32_t adaptive_preserve_depth(kimix::span<const message> messages,
                                int32_t min_preserved,
                                int32_t max_preserved) noexcept {
    int32_t depth = min_preserved;
    if (messages.empty()) {
        return compact_min(compact_max(depth, min_preserved), max_preserved);
    }

    const message *target = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (is_user_or_assistant(*it)) {
            target = &(*it);
            break;
        }
    }
    if (target == nullptr) {
        return compact_min(compact_max(depth, min_preserved), max_preserved);
    }

    const kimix::string text = extract_text(*target);
    const kimix::string lower = compact_lower_ascii(text);
    const kimix::string_view lower_view(lower);

    if (compact_contains(lower_view, "error") ||
        compact_contains(lower_view, "exception") ||
        compact_contains(lower_view, "failed")) {
        ++depth;
    }
    if (has_think_part(*target)) {
        ++depth;
    }
    const size_t file_refs = compact_count_substr(lower_view, "file:") +
                             compact_count_substr(lower_view, ".py") +
                             compact_count_substr(lower_view, ".md");
    if (file_refs > 2) {
        ++depth;
    }

    if (depth < min_preserved) depth = min_preserved;
    if (depth > max_preserved) depth = max_preserved;
    return depth;
}

// ── Cascade depth detection ────────────────────────────────────────────────

int32_t detect_cascade_depth(kimix::span<const message> messages) noexcept {
    constexpr kimix::string_view k_marker = "Previous context has been compacted";
    int32_t count = 0;
    for (const auto &msg : messages) {
        bool counted = false;
        for (const auto &part : msg.content) {
            if (part.type == "text" && compact_contains(part.text, k_marker)) {
                counted = true;
                break;
            }
        }
        if (counted) {
            ++count;
        }
    }
    return count;
}

// ── Prompt assembly ──────────────────────────────────────────────────────────

tool_error build_compaction_prompt(const compaction_prompt_input &in,
                                   compaction_prompt_output &out) {
    out.cascade_depth = detect_cascade_depth(in.to_compact);

    const kimix::string_view *base = &in.prompt_compact;
    if (!in.options.avoid_cascade && out.cascade_depth >= 3) {
        base = &in.prompt_compact_cascade;
    }

    kimix::StringScratch scratch;
    scratch.reserve(base->size() + 64);
    scratch << '\n' << *base;

    const kimix::string_view guidance = mode_guidance(in.options.mode);
    if (!guidance.empty()) {
        scratch << "\n\n" << guidance;
    }

    if (in.options.decision_section_enabled) {
        scratch << compact_decision_section_guidance();
    }

    if (!in.custom_instruction.empty()) {
        scratch << "\n\n**User's Custom Compaction Instruction:**\n"
                   "Prioritize this user focus over the default priorities and style guidance:\n"
                << in.custom_instruction;
    }

    out.prompt_text = scratch.string();
    return tool_error{tool_status::ok, kimix::string()};
}

kimix::string build_compact_message_text(const compact_message_request &req) noexcept {
    kimix::StringScratch scratch;
    size_t reserve = req.prompt_text.size();
    for (const auto &msg : req.to_compact) {
        for (const auto &part : msg.content) {
            if (part.type == "text") {
                reserve += part.text.size();
            }
        }
        reserve += 64;
    }
    scratch.reserve(reserve);

    for (size_t i = 0; i < req.to_compact.size(); ++i) {
        const auto &msg = req.to_compact[i];
        scratch << "## Message " << static_cast<unsigned>(i + 1)
                << "\nRole: " << msg.role << "\nContent:\n";
        for (const auto &part : msg.content) {
            if (part.type == "text") {
                scratch << part.text;
            }
        }
    }
    scratch << req.prompt_text;
    return scratch.string();
}

// ── Prepare compaction input ───────────────────────────────────────────────────

tool_error prepare_compaction_input(const prepare_request &req,
                                    prepare_result &out) {
    if (req.preserve_start_index > req.messages.size()) {
        return tool_error{tool_status::invalid_input,
                          kimix::string("preserve_start_index out of range")};
    }

    out.to_compact.clear();
    out.to_preserve.clear();
    out.compact_message_text.clear();
    out.prompt_text.clear();
    out.cascade_depth = 0;

    out.to_compact.reserve(req.preserve_start_index);
    for (size_t i = 0; i < req.preserve_start_index; ++i) {
        out.to_compact.push_back(req.messages[i]);
    }

    out.to_preserve.reserve(req.messages.size() - req.preserve_start_index);
    for (size_t i = req.preserve_start_index; i < req.messages.size(); ++i) {
        out.to_preserve.push_back(req.messages[i]);
    }

    if (out.to_compact.empty()) {
        return tool_error{tool_status::no_change,
                          kimix::string("no messages to compact")};
    }

    compaction_prompt_input prompt_in;
    prompt_in.to_compact = out.to_compact;
    prompt_in.options = req.options;
    prompt_in.custom_instruction = req.custom_instruction;
    prompt_in.prompt_compact = req.prompt_compact;
    prompt_in.prompt_compact_cascade = req.prompt_compact_cascade;

    compaction_prompt_output prompt_out;
    tool_error err = build_compaction_prompt(prompt_in, prompt_out);
    if (err.failed()) {
        return err;
    }

    compact_message_request cm_req;
    cm_req.to_compact = out.to_compact;
    cm_req.prompt_text = prompt_out.prompt_text;

    out.compact_message_text = build_compact_message_text(cm_req);
    out.prompt_text = std::move(prompt_out.prompt_text);
    out.cascade_depth = prompt_out.cascade_depth;

    return tool_error{tool_status::ok, kimix::string()};
}

// ── Surface fingerprint ────────────────────────────────────────────────────────

surface_fingerprint compute_surface_fingerprint(
    kimix::span<const message> messages,
    kimix::function<int64_t(kimix::span<const message>)> token_counter) {
    surface_fingerprint fp;
    fp.history_len = static_cast<uint32_t>(messages.size());
    if (!messages.empty()) {
        const kimix::string text = extract_text(messages.back(), " ");
        if (!text.empty()) {
            fp.last_message_text = text;
        }
    }
    if (token_counter) {
        fp.token_count = token_counter(messages);
    } else {
        fp.token_count = estimate_message_tokens(messages);
    }
    return fp;
}

// ── Token estimation ───────────────────────────────────────────────────────────

int64_t estimate_text_tokens(kimix::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }

    size_t total_cp = 0;
    size_t ascii_cp = 0;
    size_t cjk_cp = 0;

    const char *it = text.data();
    const char *end = text.data() + text.size();
    while (it < end) {
        const uint32_t cp = kimix::builtin_tools::decode_code_point(it, end);
        ++total_cp;
        if (cp < 128u) {
            ++ascii_cp;
        }
        if (compact_is_cjk_code_point(cp)) {
            ++cjk_cp;
        }
    }

    if (total_cp == 0) {
        return 0;
    }

    const double ascii_ratio = static_cast<double>(ascii_cp) /
                               static_cast<double>(total_cp);
    if (ascii_ratio > 0.95) {
        return compact_max(static_cast<int64_t>(1), static_cast<int64_t>(total_cp / 4));
    }
    const double cjk_ratio = static_cast<double>(cjk_cp) /
                             static_cast<double>(total_cp);
    if (cjk_ratio > 0.15) {
        return compact_max(static_cast<int64_t>(1), static_cast<int64_t>(total_cp / 3));
    }
    return compact_max(static_cast<int64_t>(1),
                        static_cast<int64_t>(static_cast<double>(total_cp) / 3.5));
}

int64_t estimate_message_tokens(kimix::span<const message> messages) noexcept {
    int64_t total = 0;
    for (const auto &msg : messages) {
        for (const auto &part : msg.content) {
            if (part.type == "text") {
                total = compact_sat_add(total, estimate_text_tokens(part.text));
            }
        }
    }
    return total;
}

// ── Tool class and standard integration ────────────────────────────────────────

namespace {

// Deserialize a Kosong-style message object from a ValueElement object.
// Returns true on success; on failure fills err.
bool compact_deserialize_message(const kimix::builtin_tools::ToolParams *obj,
                                 message &out,
                                 kimix::string &err) {
    if (obj == nullptr) {
        err = "message is not an object";
        return false;
    }

    const auto *role_val = obj->get("role");
    if (role_val == nullptr || !role_val->is_string()) {
        err = "message missing 'role' string";
        return false;
    }
    out.role = role_val->as_string();

    out.content.clear();
    const auto *content_val = obj->get("content");
    if (content_val == nullptr) {
        return true; // no content is valid
    }
    if (!content_val->is_array()) {
        err = "message 'content' must be an array";
        return false;
    }

    const auto &arr = content_val->as_array();
    out.content.reserve(arr.size());
    for (const auto &elem : arr) {
        const auto *part_obj = elem.as_object();
        if (part_obj == nullptr) {
            err = "content part is not an object";
            return false;
        }
        content_part part;
        const auto *type_val = part_obj->get("type");
        if (type_val != nullptr && type_val->is_string()) {
            part.type = type_val->as_string();
        } else {
            part.type = "other";
        }
        const auto *text_val = part_obj->get("text");
        if (text_val != nullptr && text_val->is_string()) {
            part.text = text_val->as_string();
        }
        if (part.type != "text" && part.type != "think") {
            part.type = "other";
            part.text.clear();
        }
        out.content.push_back(std::move(part));
    }
    return true;
}

// Deserialize a Kosong-style message array from a ValueElement array.
bool compact_deserialize_messages(const kimix::builtin_tools::ValueElement::Array *arr,
                                  kimix::vector<message> &out,
                                  kimix::string &err) {
    if (arr == nullptr) {
        err = "messages is not an array";
        return false;
    }
    out.clear();
    out.reserve(arr->size());
    for (const auto &elem : *arr) {
        const auto *obj = elem.as_object();
        message msg;
        if (!compact_deserialize_message(obj, msg, err)) {
            return false;
        }
        out.push_back(std::move(msg));
    }
    return true;
}

// Serialize a message to a ToolParams object value.
kimix::builtin_tools::ValueElement compact_serialize_message(
    const message &msg) {
    using namespace kimix::builtin_tools;
    kimix::shared_ptr<ToolParams> obj(new ToolParams());
    obj->values["role"] = ValueElement::make_string(msg.role);

    ValueElement::Array content;
    content.reserve(msg.content.size());
    for (const auto &part : msg.content) {
        kimix::shared_ptr<ToolParams> part_obj(new ToolParams());
        part_obj->values["type"] = ValueElement::make_string(part.type);
        part_obj->values["text"] = ValueElement::make_string(part.text);
        content.push_back(ValueElement::make_object(std::move(part_obj)));
    }
    obj->values["content"] = ValueElement::make_array(std::move(content));
    return ValueElement::make_object(std::move(obj));
}

// Serialize a message vector to a ValueElement array.
kimix::builtin_tools::ValueElement compact_serialize_messages(
    const kimix::vector<message> &messages) {
    using namespace kimix::builtin_tools;
    ValueElement::Array arr;
    arr.reserve(messages.size());
    for (const auto &msg : messages) {
        arr.push_back(compact_serialize_message(msg));
    }
    return ValueElement::make_array(std::move(arr));
}

// Build a default compaction_options from a ToolParams object (optional).
compaction_options compact_build_options(
    const kimix::builtin_tools::ToolParams *obj) {
    compaction_options opts;
    if (obj == nullptr) {
        return opts;
    }
    const auto *avoid = obj->get("avoid_cascade");
    if (avoid != nullptr && avoid->is_bool()) {
        opts.avoid_cascade = avoid->as_bool();
    }
    const auto *mode = obj->get("mode");
    if (mode != nullptr && mode->is_string()) {
        opts.mode = parse_compact_mode(mode->as_string());
    }
    const auto *todos = obj->get("todos_max_items");
    if (todos != nullptr && todos->is_int()) {
        opts.todos_max_items = static_cast<int32_t>(todos->as_int());
    }
    const auto *preserve_override = obj->get("preserve_depth_override");
    if (preserve_override != nullptr && preserve_override->is_int()) {
        opts.preserve_depth_override = static_cast<int32_t>(preserve_override->as_int());
    }
    const auto *decision = obj->get("decision_section_enabled");
    if (decision != nullptr && decision->is_bool()) {
        opts.decision_section_enabled = decision->as_bool();
    }
    return opts;
}

} // namespace

Compact::Compact(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

void Compact::operator()(kimix::builtin_tools::ToolParams const *parameters) {
    using namespace kimix::builtin_tools;

    _last_result.clear();
    ToolParams result;
    if (parameters == nullptr) {
        result.values["status"] = ValueElement::make_string("no_change");
        result.values["message"] = ValueElement::make_string(
            kimix::string("no parameters provided"));
        result.serialize(_last_result);
        return;
    }

    const auto *messages_val = parameters->get("messages");
    if (messages_val == nullptr || !messages_val->is_array()) {
        result.values["status"] = ValueElement::make_string("invalid_input");
        result.values["message"] = ValueElement::make_string(
            kimix::string("missing or invalid 'messages' array"));
        result.serialize(_last_result);
        return;
    }

    kimix::vector<message> messages;
    kimix::string err;
    if (!compact_deserialize_messages(&messages_val->as_array(), messages, err)) {
        result.values["status"] = ValueElement::make_string("invalid_input");
        result.values["message"] = ValueElement::make_string(std::move(err));
        result.serialize(_last_result);
        return;
    }

    prepare_request req;
    req.messages = messages;

    const auto *index_val = parameters->get("preserve_start_index");
    if (index_val != nullptr && index_val->is_int()) {
        const int64_t idx = index_val->as_int();
        req.preserve_start_index = (idx < 0) ? 0
            : (static_cast<size_t>(idx) > messages.size() ? messages.size()
                                                          : static_cast<size_t>(idx));
    } else if (index_val != nullptr && index_val->is_uint()) {
        const uint64_t idx = index_val->as_uint();
        req.preserve_start_index = (static_cast<size_t>(idx) > messages.size())
                                       ? messages.size()
                                       : static_cast<size_t>(idx);
    } else {
        req.preserve_start_index = 0;
    }

    const auto *options_val = parameters->get("options");
    if (options_val != nullptr && options_val->is_object()) {
        req.options = compact_build_options(options_val->as_object());
    }

    const auto *custom_val = parameters->get("custom_instruction");
    if (custom_val != nullptr && custom_val->is_string()) {
        req.custom_instruction = custom_val->as_string();
    }

    const auto *prompt_compact_val = parameters->get("prompt_compact");
    if (prompt_compact_val != nullptr && prompt_compact_val->is_string()) {
        req.prompt_compact = prompt_compact_val->as_string();
    }

    const auto *prompt_cascade_val = parameters->get("prompt_compact_cascade");
    if (prompt_cascade_val != nullptr && prompt_cascade_val->is_string()) {
        req.prompt_compact_cascade = prompt_cascade_val->as_string();
    }

    prepare_result prep;
    tool_error err_obj = prepare_compaction_input(req, prep);

    if (err_obj.status == tool_status::no_change) {
        result.values["status"] = ValueElement::make_string("no_change");
        result.values["message"] = ValueElement::make_string(
            kimix::string("no messages to compact"));
    } else if (err_obj.failed()) {
        result.values["status"] = ValueElement::make_string("error");
        result.values["message"] = ValueElement::make_string(std::move(err_obj.message));
    } else {
        result.values["status"] = ValueElement::make_string("ok");
        result.values["compact_message_text"] =
            ValueElement::make_string(prep.compact_message_text);
        result.values["prompt_text"] = ValueElement::make_string(prep.prompt_text);
        result.values["cascade_depth"] =
            ValueElement::make_int(static_cast<int64_t>(prep.cascade_depth));
        result.values["to_compact"] = compact_serialize_messages(prep.to_compact);
        result.values["to_preserve"] = compact_serialize_messages(prep.to_preserve);
    }

    result.serialize(_last_result);
}

} // namespace kimix::builtin_tools::compact
