/*
 * normalize_tool_call_ids.cpp - see normalize_tool_call_ids.h (plan 014).
 *
 * Exact port of kimi.py::_normalize_tool_call_ids:
 *   raw_ids in first-seen order (tool_calls ids first, then tool_call_id),
 *   ids already satisfying the contract keep their value (first pass), only
 *   genuinely invalid ids are rewritten (second pass) via _make_unique
 *   ("tool_call" fallback for empty sanitized ids, _2/_3... suffixes).
 */

#include <runtime/soul/normalize_tool_call_ids.h>

#include <runtime/common/utf8.h>

namespace kimix {
namespace runtime {
namespace soul {
namespace {

constexpr size_t kMaxIdLength = 64;
constexpr const char* kEmptyToolCallId = "tool_call";

// _sanitize_tool_call_id: replace every code point outside [a-zA-Z0-9_-]
// with '_' (one underscore per code point, matching the str-level regex),
// then truncate to 64 characters (all ASCII after sanitization).
kimix::string sanitize_tool_call_id(kimix::string_view id) noexcept {
    kimix::string out;
    out.reserve(id.size());
    const char* it = id.data();
    const char* end = id.data() + id.size();
    while (it < end) {
        const uint32_t cp = common::decode_cp(it, end);
        const bool safe =
            (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
            (cp >= '0' && cp <= '9') || cp == '_' || cp == '-';
        if (safe) {
            out.push_back(static_cast<char>(cp));
        } else {
            out.push_back('_');
        }
    }
    if (out.size() > kMaxIdLength) {
        out.resize(kMaxIdLength);
    }
    return out;
}

// _make_unique_tool_call_id: base = normalized or "tool_call"; try base
// truncated to 64, then base[:64-len("_N")] + "_2", "_3", ...
kimix::string make_unique_tool_call_id(kimix::string_view normalized,
                                       const kimix::set<kimix::string>& used) noexcept {
    kimix::string base =
        normalized.empty() ? kimix::string(kEmptyToolCallId)
                           : kimix::string(normalized.data(),
                                           (std::min)(normalized.size(), kMaxIdLength));
    if (!base.empty() && used.find(base) == used.end()) {
        return base;
    }
    uint64_t index = 2;
    for (;;) {
        kimix::string suffix("_");
        suffix.append(std::to_string(index).c_str());
        const size_t keep =
            (suffix.size() >= kMaxIdLength) ? 0u : (kMaxIdLength - suffix.size());
        kimix::string candidate = base.substr(0, keep) + suffix;
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
        ++index;
    }
}

} // namespace

void normalize_tool_call_ids(kimix::span<const message_view> msgs,
                             kimix::vector<id_fix>& out) noexcept {
    out.clear();

    // First pass: collect raw ids in first-seen order.
    kimix::vector<kimix::string_view> raw_ids;
    raw_ids.reserve(msgs.size() * 2);
    kimix::set<kimix::string> seen;
    for (const message_view& msg : msgs) {
        for (const tool_call_view& tc : msg.tool_calls) {
            if (seen.find(kimix::string(tc.id)) == seen.end()) {
                seen.insert(kimix::string(tc.id));
                raw_ids.push_back(tc.id);
            }
        }
        if (!msg.tool_call_id.empty() &&
            seen.find(kimix::string(msg.tool_call_id)) == seen.end()) {
            seen.insert(kimix::string(msg.tool_call_id));
            raw_ids.push_back(msg.tool_call_id);
        }
    }
    if (raw_ids.empty()) {
        return;
    }

    // Ids that already satisfy the contract keep their value (first pass).
    kimix::map<kimix::string, kimix::string> mapped;
    kimix::set<kimix::string> used;
    for (kimix::string_view raw_id : raw_ids) {
        kimix::string normalized = sanitize_tool_call_id(raw_id);
        if (normalized == raw_id && !normalized.empty()) {
            mapped.emplace(kimix::string(raw_id), normalized);
            used.insert(std::move(normalized));
        }
    }
    // Only genuinely invalid ids are rewritten (second pass).
    for (kimix::string_view raw_id : raw_ids) {
        const kimix::string key(raw_id);
        if (mapped.find(key) != mapped.end()) {
            continue;
        }
        kimix::string unique =
            make_unique_tool_call_id(sanitize_tool_call_id(raw_id), used);
        used.insert(unique);
        mapped.emplace(key, unique);
    }

    // Nothing to fix -> empty plan.
    bool any_changed = false;
    for (kimix::string_view raw_id : raw_ids) {
        auto it = mapped.find(kimix::string(raw_id));
        if (it != mapped.end() && it->second != raw_id) {
            any_changed = true;
            break;
        }
    }
    if (!any_changed) {
        return;
    }

    for (size_t i = 0; i < msgs.size(); ++i) {
        const message_view& msg = msgs[i];
        size_t j = 0;
        for (const tool_call_view& tc : msg.tool_calls) {
            auto it = mapped.find(kimix::string(tc.id));
            if (it != mapped.end() && it->second != tc.id) {
                id_fix fix;
                fix.msg_index = static_cast<uint32_t>(i);
                fix.call_index = static_cast<uint32_t>(j);
                fix.new_id = it->second;
                out.push_back(std::move(fix));
            }
            ++j;
        }
        if (!msg.tool_call_id.empty()) {
            auto it = mapped.find(kimix::string(msg.tool_call_id));
            if (it != mapped.end() && it->second != msg.tool_call_id) {
                id_fix fix;
                fix.msg_index = static_cast<uint32_t>(i);
                fix.call_index = UINT32_MAX; // tool_call_id field
                fix.new_id = it->second;
                out.push_back(std::move(fix));
            }
        }
    }
}

} // namespace soul
} // namespace runtime
} // namespace kimix
