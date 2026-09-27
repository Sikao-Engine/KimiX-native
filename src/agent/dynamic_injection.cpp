// agent/dynamic_injection.cpp - implementation of the dynamic-injection
// framework (see dynamic_injection.h).

#include "agent/dynamic_injection.h"

#include <cmath>
#include <cstdio>

#include <runtime/common/text_util.h>

namespace kimix::agent {

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void InjectionRegistry::add_provider(
    kimix::unique_ptr<DynamicInjectionProvider> provider) {
    _providers.push_back(std::move(provider));
}

kimix::vector<DynamicInjection>
InjectionRegistry::collect(const InjectionStepContext &ctx) const {
    // kimisoul.py:692-705: per provider, try/except around get_injections;
    // a raising provider is logged and the loop continues. The exception-free
    // port reports failure via the bool + error out-parameter instead.
    kimix::vector<DynamicInjection> injections;
    for (const kimix::unique_ptr<DynamicInjectionProvider> &provider : _providers) {
        kimix::string error;
        const size_t before = injections.size();
        if (!provider->get_injections(ctx, injections, error)) {
            std::fprintf(stderr, "injection provider failed: %s\n", error.c_str());
            injections.resize(before); // discard anything a failed provider added
        }
    }
    return injections;
}

void InjectionRegistry::notify_context_compacted() const {
    // kimisoul.py:883-897: failures isolated so a buggy provider cannot abort
    // compaction. The hooks are void in the reference too - nothing to
    // propagate; log and continue.
    for (const kimix::unique_ptr<DynamicInjectionProvider> &provider : _providers) {
        provider->on_context_compacted();
    }
}

void InjectionRegistry::notify_afk_changed(bool enabled) const {
    // kimisoul.py:899-910 (notify_afk_changed): same isolation contract.
    for (const kimix::unique_ptr<DynamicInjectionProvider> &provider : _providers) {
        provider->on_afk_changed(enabled);
    }
}

// ---------------------------------------------------------------------------
// Model-visible wrapper helpers (soul/message.py:20-25)
// ---------------------------------------------------------------------------

kimix::string system_block_text(kimix::string_view content) {
    // message.py:20-21 verbatim: TextPart(text=f"<system>{message}</system>").
    kimix::string out;
    out.reserve(content.size() + 17);
    out += "<system>";
    out.append(content.data(), content.size());
    out += "</system>";
    return out;
}

kimix::string system_reminder_text(kimix::string_view content) {
    // message.py:24-25 verbatim:
    //   TextPart(text=f"<system-reminder>\n{message}\n</system-reminder>")
    kimix::string out;
    out.reserve(content.size() + 40);
    out += "<system-reminder>\n";
    out.append(content.data(), content.size());
    out += "\n</system-reminder>";
    return out;
}

bool is_system_reminder_message(const kimix::llm::Message &message) noexcept {
    // message.py:28-33: role user, single TextPart whose stripped text starts
    // with "<system-reminder>". The native Message has one content string;
    // empty content can never match, so the parts==1 condition reduces to the
    // startswith check on the trimmed content.
    if (message.role != "user") {
        return false;
    }
    return runtime::common::trimmed_starts_with(message.content, "<system-reminder>");
}

size_t strip_system_reminders(
    kimix::vector<kimix::llm::Message> &history) noexcept {
    // message.py:36-44, in place.
    size_t removed = 0;
    size_t i = 0;
    while (i < history.size()) {
        if (is_system_reminder_message(history[i])) {
            history.erase(history.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        } else {
            ++i;
        }
    }
    return removed;
}

// ── E10 - Layer 1 coalescing (soul/message.py:82-191) ──────────────────────

namespace {

// Python str.strip() on the ASCII whitespace set (the blocks the passes
// recognize are ASCII-delimited; the inner text may be any UTF-8).
kimix::string_view trimmed(kimix::string_view text) noexcept {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

constexpr kimix::string_view k_system_open = "<system>";
constexpr kimix::string_view k_system_close = "</system>";

// The `(.*)` inner text of one "<system>…</system>" block that starts at
// `begin`, or npos when the region does not open and close exactly like the
// reference's ^<system>(.*)</system>$ (DOTALL) match. `end` receives the
// offset one past the closing tag.
size_t system_block_span(kimix::string_view text, size_t begin, size_t &end) {
    if (text.compare(begin, k_system_open.size(), k_system_open) != 0) {
        return kimix::string_view::npos;
    }
    const size_t close =
        text.find(k_system_close, begin + k_system_open.size());
    if (close == kimix::string_view::npos) {
        return kimix::string_view::npos;
    }
    end = close + k_system_close.size();
    return begin + k_system_open.size();
}

} // namespace

// The LEADING "<system>…</system>" block of a message content (the reference
// matches content[0]; the flat string keeps the following parts in the tail
// after the block). The inner text is stripped (the reference strips
// part.text before matching); `block_end` is the raw-content offset one past
// the closing tag, so callers can keep the tail verbatim. False when the
// content does not start with a full block.
bool leading_system_block(kimix::string_view content, kimix::string &inner,
                          size_t &block_end) noexcept {
    inner.clear();
    if (content.size() < k_system_open.size() + k_system_close.size() ||
        content.compare(0, k_system_open.size(), k_system_open) != 0) {
        return false;
    }
    const size_t start = system_block_span(content, 0, block_end);
    if (start == kimix::string_view::npos) {
        return false;
    }
    inner = trimmed(content.substr(start, block_end - start -
                                            k_system_close.size()));
    return true;
}

bool extract_system_block(kimix::string_view content,
                          kimix::string &inner) noexcept {
    inner.clear();
    const kimix::string_view text = trimmed(content);
    if (text.size() < k_system_open.size() + k_system_close.size()) {
        return false;
    }
    size_t end = 0;
    const size_t start = system_block_span(text, 0, end);
    if (start == kimix::string_view::npos || end != text.size()) {
        return false; // not exactly one block
    }
    inner = trimmed(text.substr(start, end - start - k_system_close.size()));
    return true;
}

size_t coalesce_adjacent_tool_metadata(
    kimix::vector<kimix::llm::Message> &history) {
    // message.py:100-161. The native message content is the flat string the
    // parts list collapses to, so "the first content part" is the LEADING
    // block of the content (any output parts follow in the tail) and "the
    // message keeps at least one part" becomes "the content never becomes
    // empty".
    size_t removed = 0;
    size_t i = 0;
    while (i + 1 < history.size()) {
        const kimix::llm::Message &msg = history[i];
        const kimix::llm::Message &next_msg = history[i + 1];
        if (msg.role != "tool" || next_msg.role != "tool" ||
            msg.content.empty() || next_msg.content.empty()) {
            ++i;
            continue;
        }
        kimix::string sys_text;
        size_t head_end = 0;
        if (!leading_system_block(msg.content, sys_text, head_end)) {
            ++i;
            continue;
        }
        // Look ahead: how many consecutive tool messages share this exact
        // system metadata as their leading block.
        size_t run = 0;
        size_t j = i + 1;
        while (j < history.size() && history[j].role == "tool" &&
               !history[j].content.empty()) {
            kimix::string follower_text;
            size_t follower_end = 0;
            if (!leading_system_block(history[j].content, follower_text,
                                      follower_end) ||
                follower_text != sys_text) {
                break;
            }
            ++run;
            ++j;
        }
        if (run == 0) {
            ++i;
            continue;
        }
        // Annotate the first occurrence when more than one message shared it:
        // the leading block becomes "<system>[×N] inner</system>" and the
        // tail (the following parts) is kept verbatim.
        const size_t total = run + 1;
        if (total > 1) {
            // message.py:147-150: f"[×{total}] {sys_text}" - U+00D7 (×).
            kimix::string annotated(k_system_open);
            annotated += "[\xC3\x97" + std::to_string(total) + "] ";
            annotated += sys_text;
            annotated += k_system_close;
            annotated += msg.content.substr(head_end);
            history[i].content = std::move(annotated);
        }
        // Remove the block from each follower, never emptying it (provider
        // invariants require non-empty tool results).
        for (size_t k = i + 1; k < j; ++k) {
            kimix::string follower_text;
            size_t follower_end = 0;
            if (!leading_system_block(history[k].content, follower_text,
                                      follower_end)) {
                continue;
            }
            kimix::string rest = history[k].content.substr(follower_end);
            if (rest.empty()) {
                continue; // the block is the whole content: keep it
            }
            history[k].content = std::move(rest);
            ++removed;
        }
        i = j; // skip past the coalesced run
    }
    return removed;
}

kimix::string coalesce_adjacent_system_blocks(kimix::string_view content) {
    // message.py:164-191 coalesce_content_parts: adjacent <system> blocks
    // (separated only by whitespace in the flat-string model) merge into one
    // block whose inner text is the ". "-joined sequence. Whitespace is
    // dropped between two merged blocks and preserved everywhere else.
    const kimix::string_view text = content;
    kimix::string out;
    kimix::string pending_ws;
    kimix::vector<kimix::string> pending;
    size_t i = 0;
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v';
    };
    const auto flush = [&]() {
        if (!pending.empty()) {
            out += k_system_open;
            for (size_t k = 0; k < pending.size(); ++k) {
                if (k != 0) {
                    out += ". ";
                }
                out += pending[k];
            }
            out += k_system_close;
            pending.clear();
        }
        // Whitespace held while a merge was pending trails the merged run
        // (it is the gap between the run and whatever follows).
        out += pending_ws;
        pending_ws.clear();
    };
    while (i < text.size()) {
        if (is_space(text[i])) {
            if (!pending.empty()) {
                pending_ws += text[i]; // between blocks: held for the flush
            } else {
                out += text[i];
            }
            ++i;
            continue;
        }
        size_t end = 0;
        const size_t inner_start = system_block_span(text, i, end);
        if (inner_start == kimix::string_view::npos) {
            flush();
            out += text[i];
            ++i;
            continue;
        }
        pending.emplace_back(trimmed(text.substr(
            inner_start, end - inner_start - k_system_close.size())));
        pending_ws.clear(); // the gap before a merged block is dropped
        i = end;
    }
    flush();
    return out;
}

kimix::vector<kimix::llm::Message>
normalize_history(const kimix::vector<kimix::llm::Message> &history) {
    // dynamic_injection.py:59-93. Merged content concatenates the two
    // messages' text (the reference concatenates TextPart lists; the native
    // single-string content is the concatenation of those parts). The
    // notification exemption from the reference is vacuous here -
    // notification messages were never ported to the native soul.
    kimix::vector<kimix::llm::Message> result;
    result.reserve(history.size());
    for (const kimix::llm::Message &msg : history) {
        if (!result.empty() && result.back().role == msg.role && msg.role == "user" &&
            !is_system_reminder_message(result.back()) &&
            !is_system_reminder_message(msg)) {
            kimix::string merged = std::move(result.back().content);
            merged += msg.content;
            result.back().content = std::move(merged);
        } else {
            result.push_back(msg);
        }
    }
    // E10 (Layer 1, context_pruning.py:1061-1080 coalesce_tool_metadata):
    // duplicate adjacent <system> metadata is cosmetic - it costs tokens on
    // every request. Coalesce inside each message first, then across runs of
    // consecutive tool messages. Both passes are no-ops on a history without
    // adjacent <system> blocks, so plain-text requests are byte-identical.
    if (!result.empty()) {
        for (kimix::llm::Message &m : result) {
            if (m.content.empty()) {
                continue;
            }
            kimix::string coalesced =
                coalesce_adjacent_system_blocks(m.content);
            if (coalesced != m.content) {
                m.content = std::move(coalesced);
            }
        }
        coalesce_adjacent_tool_metadata(result);
    }
    return result;
}

kimix::string
build_combined_reminder(kimix::span<const DynamicInjection> injections) {
    // kimisoul.py:1642-1648:
    //   combined_reminders = "\n".join(system_reminder(inj.content).text ...)
    kimix::string combined;
    for (size_t i = 0; i < injections.size(); ++i) {
        if (i != 0) {
            combined += '\n';
        }
        combined += system_reminder_text(injections[i].content);
    }
    return combined;
}

kimix::string py_percent0(double ratio) {
    // Python format spec "{x:.0%}": x * 100, rounded to 0 decimals with
    // round-half-to-even, then "%". Usage ratios are non-negative.
    if (ratio < 0.0) {
        ratio = 0.0;
    }
    const double scaled = ratio * 100.0;
    const double fl = std::floor(scaled);
    const double frac = scaled - fl;
    int64_t base = static_cast<int64_t>(fl);
    if (frac > 0.5) {
        ++base;
    } else if (frac == 0.5 && (base % 2) != 0) {
        ++base; // round half to even
    }
    return kimix::format("{}%", base);
}

} // namespace kimix::agent
