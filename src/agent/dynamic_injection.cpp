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
// Reminder message helpers
// ---------------------------------------------------------------------------

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
