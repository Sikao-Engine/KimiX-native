/*
 * compaction_view.cpp - compaction / prune decision kernels over the arena.
 * See compaction_view.h for the design and the boundary contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions. The heavy
 * lifting stays in kimix::builtin_tools::compact; these functions only project
 * the arena views onto its message shape and forward the result.
 */

#include <core/kimix_core.h>

#include <runtime/soul/compaction_view.h>

namespace kimix {
namespace runtime {
namespace soul {

namespace src_runtime_soul_compaction_view {

using kimix::builtin_tools::compact::content_part;
using kimix::builtin_tools::compact::message;

inline kimix::string_view cv_role_name(uint8_t role) noexcept {
    switch (role) {
    case kRoleSystem:
        return "system";
    case kRoleAssistant:
        return "assistant";
    case kRoleTool:
        return "tool";
    default:
        return "user";
    }
}

inline void project_message(const message_view &mv, message &out) {
    out.role.assign(cv_role_name(mv.role).data(), cv_role_name(mv.role).size());
    out.content.clear();
    out.content.reserve(mv.parts.size());
    for (const part_view &p : mv.parts) {
        content_part cp;
        switch (p.kind) {
        case part_kind::TEXT:
            cp.type.assign("text", 4);
            cp.text.assign(p.text.data(), p.text.size());
            break;
        case part_kind::THINK:
            cp.type.assign("think", 5);
            cp.text.assign(p.text.data(), p.text.size());
            break;
        case part_kind::TOOL_CALL:
            // The streamed ToolCallPart marker: message_tool_call_delta counts
            // it, and no kernel reads its text.
            cp.type.assign("tool_call", 9);
            break;
        default:
            cp.type.assign("other", 5);
            cp.text.assign(p.text.data(), p.text.size());
            break;
        }
        out.content.push_back(std::move(cp));
    }
    out.tool_call_count = static_cast<int32_t>(mv.tool_calls.size());
}

} // namespace src_runtime_soul_compaction_view
using namespace src_runtime_soul_compaction_view;

void project(kimix::span<const message_view> in, compaction_scratch &s) noexcept {
    s.msgs.clear();
    s.msgs.reserve(in.size());
    for (const message_view &mv : in) {
        message m;
        project_message(mv, m);
        s.msgs.push_back(std::move(m));
    }
}

void balanced_cut_indices(kimix::span<const message_view> in,
                          kimix::vector<uint32_t> &cuts, bool &unbalanced,
                          int64_t &unbalanced_index) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    const auto res = kimix::builtin_tools::compact::balanced_cut_indices(scratch.msgs);
    cuts.clear();
    cuts.reserve(res.cuts.size());
    for (size_t c : res.cuts) {
        cuts.push_back(static_cast<uint32_t>(c));
    }
    unbalanced = res.unbalanced;
    unbalanced_index = res.unbalanced ? static_cast<int64_t>(res.unbalanced_index) : -1;
}

uint32_t nearest_balanced_cut_before(kimix::span<const message_view> in,
                                     uint32_t index, bool &unbalanced) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    bool flag = false;
    const size_t cut = kimix::builtin_tools::compact::nearest_balanced_cut_before(
        scratch.msgs, static_cast<int64_t>(index), &flag);
    unbalanced = flag;
    return static_cast<uint32_t>(cut);
}

void resolve_preserve_split(
    kimix::span<const message_view> in, int32_t preserve_depth, bool balanced_cuts,
    builtin_tools::compact::preserve_split &out) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    out = kimix::builtin_tools::compact::resolve_preserve_split(scratch.msgs,
                                                                preserve_depth,
                                                                balanced_cuts);
}

bool should_auto_compact(
    int64_t token_count,
    const builtin_tools::compact::compaction_trigger_config &cfg) noexcept {
    return kimix::builtin_tools::compact::should_auto_compact(token_count, cfg);
}

bool build_compaction_prompt(
    kimix::span<const message_view> in,
    const builtin_tools::compact::compaction_options &opts,
    kimix::string_view custom_instruction, kimix::string_view prompt_compact,
    kimix::string_view prompt_compact_cascade,
    builtin_tools::compact::compaction_prompt_output &out) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    kimix::builtin_tools::compact::compaction_prompt_input pin;
    pin.to_compact = scratch.msgs;
    pin.options = opts;
    pin.custom_instruction = custom_instruction;
    pin.prompt_compact = prompt_compact;
    pin.prompt_compact_cascade = prompt_compact_cascade;
    const auto err = kimix::builtin_tools::compact::build_compaction_prompt(pin, out);
    return !err.failed();
}

bool prepare_compaction_input(
    kimix::span<const message_view> in, size_t preserve_start_index,
    const builtin_tools::compact::compaction_options &opts,
    kimix::string_view custom_instruction, kimix::string_view prompt_compact,
    kimix::string_view prompt_compact_cascade,
    builtin_tools::compact::prepare_result &out) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    kimix::builtin_tools::compact::prepare_request req;
    req.messages = scratch.msgs;
    req.preserve_start_index =
        preserve_start_index > scratch.msgs.size() ? scratch.msgs.size()
                                                   : preserve_start_index;
    req.options = opts;
    req.custom_instruction = custom_instruction;
    req.prompt_compact = prompt_compact;
    req.prompt_compact_cascade = prompt_compact_cascade;
    const auto err =
        kimix::builtin_tools::compact::prepare_compaction_input(req, out);
    return !err.failed();
}

void extract_text(kimix::span<const message_view> in, kimix::string_view sep,
                  kimix::string &out) noexcept {
    out.clear();
    bool first = true;
    for (const message_view &mv : in) {
        for (const part_view &p : mv.parts) {
            if (p.kind != part_kind::TEXT) {
                continue;
            }
            if (!first) {
                out.append(sep.data(), sep.size());
            }
            out.append(p.text.data(), p.text.size());
            first = false;
        }
    }
}

int64_t count_tokens(kimix::span<const message_view> in) noexcept {
    int64_t total = 0;
    for (const message_view &mv : in) {
        for (const part_view &p : mv.parts) {
            if (p.kind == part_kind::TEXT) {
                total += kimix::builtin_tools::compact::estimate_text_tokens(p.text);
            }
        }
    }
    return total;
}

void build_compact_message_text(kimix::span<const message_view> in,
                                kimix::string_view prompt_text,
                                kimix::string &out) noexcept {
    compaction_scratch scratch;
    project(in, scratch);
    kimix::builtin_tools::compact::compact_message_request req;
    req.to_compact = scratch.msgs;
    req.prompt_text = prompt_text;
    out = kimix::builtin_tools::compact::build_compact_message_text(req);
}

} // namespace soul
} // namespace runtime
} // namespace kimix
