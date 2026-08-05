// Shared test helpers for the soul-domain kernels (plans 014/015/016).
// Used by test_payload_builder.cpp / test_normalize_tool_call_ids.cpp /
// test_prune_scanner.cpp / test_reminder_stripper.cpp / test_prompt_builder.cpp
// / test_export_builder.cpp. All helpers are `inline` so the unity-batched
// test TUs never collide.

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace soul_test {

using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

// Builds message_view arrays over an owned UTF-8 buffer. Spans are valid
// until the builder is destroyed. Call finish() AFTER adding everything.
class message_builder {
public:
    message_builder() {
        // Reserve so data() is stable for the whole build (no realloc and no
        // SSO switching -- earlier string_views stay valid).
        buffer.reserve(1 << 20);
    }

    kimix::string_view str(const char* s) {
        const size_t start = buffer.size();
        buffer.append(s);
        return kimix::string_view(buffer.data() + start, buffer.size() - start);
    }
    kimix::string_view str(const kimix::string& s) {
        const size_t start = buffer.size();
        buffer.append(s.data(), s.size());
        return kimix::string_view(buffer.data() + start, buffer.size() - start);
    }
    kimix::string_view str(const kimix::string_view& s) {
        const size_t start = buffer.size();
        buffer.append(s.data(), s.size());
        return kimix::string_view(buffer.data() + start, buffer.size() - start);
    }

    void begin_message(uint8_t role, const char* tool_call_id = nullptr) {
        msg_spec spec;
        spec.role = role;
        spec.tool_call_id = tool_call_id != nullptr ? str(tool_call_id) : kimix::string_view();
        spec.part_begin = parts.size();
        spec.call_begin = calls.size();
        specs.push_back(spec);
    }

    void part(part_kind kind, const char* text) {
        part_view p;
        p.kind = kind;
        p.text = str(text);
        parts.push_back(p);
    }
    void part(part_kind kind, const kimix::string_view& text) {
        part_view p;
        p.kind = kind;
        p.text = str(text);
        parts.push_back(p);
    }

    void call(const char* id, const char* name, const char* args) {
        tool_call_view tc;
        tc.id = str(id);
        tc.name = str(name);
        tc.arguments = str(args);
        calls.push_back(tc);
    }
    // function.arguments == None (exclude_none drops the key).
    void call_args_none(const char* id, const char* name) {
        tool_call_view tc;
        tc.id = str(id);
        tc.name = str(name);
        calls.push_back(tc);
    }

    kimix::vector<message_view> finish() {
        kimix::vector<message_view> msgs;
        msgs.reserve(specs.size());
        for (size_t i = 0; i < specs.size(); ++i) {
            const size_t part_end =
                (i + 1 < specs.size()) ? specs[i + 1].part_begin : parts.size();
            const size_t call_end =
                (i + 1 < specs.size()) ? specs[i + 1].call_begin : calls.size();
            message_view m;
            m.role = specs[i].role;
            m.tool_call_id = specs[i].tool_call_id;
            m.parts = kimix::span<const part_view>(parts.data() + specs[i].part_begin,
                                                   part_end - specs[i].part_begin);
            m.tool_calls = kimix::span<const tool_call_view>(
                calls.data() + specs[i].call_begin, call_end - specs[i].call_begin);
            msgs.push_back(m);
        }
        return msgs;
    }

    // Re-append a string view produced earlier (stays valid; used to share
    // ids between assistant tool_calls and tool results).
    kimix::string_view reuse(const kimix::string_view& v) { return v; }

private:
    struct msg_spec {
        uint8_t role = 0;
        kimix::string_view tool_call_id;
        size_t part_begin = 0;
        size_t call_begin = 0;
    };
    kimix::string buffer;
    kimix::vector<part_view> parts;
    kimix::vector<tool_call_view> calls;
    kimix::vector<msg_spec> specs;
};

} // namespace soul_test
