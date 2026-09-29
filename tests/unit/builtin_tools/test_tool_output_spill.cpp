// Test for the oversized-result spill of the built-in tools
// (builtin_tools/tool.h: tool_output_spill_scope, installed by every
// Tool::operator() right after tool_display_scope).
//
// When a tool's serialized result payload grows past
// kToolOutputSpillMaxBytes (128000 bytes), the payload is written to a temp
// file under <work dir>/.kimix_cache/tmp_<millis>/ (the ag_default_save_prompt
// pattern of agent_tool.cpp) and the result is REPLACED by a pointer payload -
// "output too long (<N> bytes), saved to <path>" - so the model can read/grep
// the file to recover the content instead of receiving an unusable wall of
// text. This test pins that contract for both result styles:
// - tools that keep the serialized payload in a kimix::vector<char> buffer;
// - tools (Edit/Write/Todo style) that keep a ToolParams object and serialize
//   it on demand.
// Plus the boundary: exactly kToolOutputSpillMaxBytes bytes is NOT spilled
// (only larger), and everything below passes through untouched.
#include "ut/ut.hpp"

#include "builtin_tools/tool.h"
#include <core/kimix_core.h>
#include <cstdio>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;

namespace {
namespace bt = kimix::builtin_tools;
namespace fs = kimix::filesystem;

// A temp work dir; remove_all + create_directories so every run starts clean.
fs::path ts_work_dir(const char *tag) {
    std::error_code ec;
    const fs::path dir =
        fs::temp_directory_path(ec) / (kimix::string("kimix_spill_") + tag);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// The spill dump of one call: <work_dir>/.kimix_cache/tmp_<millis>/output_*.txt
// (only the "no other call wrote here" tests use this, so the first match is
// THE dump). "" when nothing was spilled.
kimix::string ts_find_dump(const fs::path &work_dir) {
    std::error_code ec;
    const fs::path cache = work_dir / ".kimix_cache";
    fs::directory_iterator it(cache, ec);
    if (ec) {
        return {};
    }
    for (const fs::directory_entry &e : it) {
        if (!e.is_directory(ec)) {
            continue;
        }
        fs::directory_iterator f(e.path(), ec);
        for (const fs::directory_entry &d : f) {
            if (d.is_regular_file(ec)) {
                // Forward-slashed: the spill message quotes the display form
                // (ag_default_save_prompt's convention).
                kimix::string out = kimix::to_string(d.path());
                for (char &c : out) {
                    if (c == '\\') {
                        c = '/';
                    }
                }
                return out;
            }
        }
    }
    return {};
}

kimix::string ts_read_file(const fs::path &path) {
    kimix::string out;
    std::FILE *f = std::fopen(kimix::to_string(path).c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// A {"status":"ok","output":"<padding>"} payload padded to EXACTLY `size`
// bytes (the closing of the output string is appended after the padding).
kimix::vector<char> ts_padded_payload(size_t size) {
    const kimix::string prefix = R"JSON({"status":"ok","output":")JSON";
    const kimix::string suffix = R"JSON("})JSON";
    kimix::vector<char> out;
    out.reserve(size);
    out.insert(out.end(), prefix.begin(), prefix.end());
    if (size > prefix.size() + suffix.size()) {
        out.insert(out.end(), size - prefix.size() - suffix.size(), 'x');
    }
    out.insert(out.end(), suffix.begin(), suffix.end());
    return out;
}

// Fixture tool, buffer style: keeps the serialized payload in _result like
// Bash/Read/Grep/... do.
class buffer_tool final : public Tool {
public:
    explicit buffer_tool(Session *session) : Tool(session) {}
    size_t payload_size = 0;
    kimix::vector<char> _result;
    void operator()(ToolParams const *, kimix::string &display_str) override {
        const bt::tool_display_scope k_display{*this, display_str};
        const bt::tool_output_spill_scope k_spill{*this, _result};
        _result = ts_padded_payload(payload_size);
    }
    bool valid() const override { return true; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }
};

// Fixture tool, ToolParams style: keeps the result object and serializes on
// demand, like Edit/Write/Todo do.
class params_tool final : public Tool {
public:
    explicit params_tool(Session *session) : Tool(session) {}
    size_t output_chars = 0;
    ToolParams _result;
    void operator()(ToolParams const *, kimix::string &display_str) override {
        const bt::tool_display_scope k_display{*this, display_str};
        const bt::tool_output_spill_scope k_spill{*this, _result};
        _result.values.clear();
        _result["status"] = ValueElement::make_string("ok");
        _result["output"] = ValueElement::make_string(kimix::string(output_chars, 'y'));
    }
    bool valid() const override { return true; }
    void result_json(kimix::vector<char> &out) const override {
        _result.serialize(out);
    }
};

// The spilled pointer payload, parsed back: status must be "ok" (the tool
// SUCCEEDED; only the delivery was redirected) and the message must point at
// the dump. `dump_content` receives the message text.
bool ts_parse_pointer(const kimix::vector<char> &payload, kimix::string &message) {
    ToolParams p;
    kimix::string error;
    if (!p.try_deserialize(
            kimix::span<char const>(payload.data(), payload.size()), error)) {
        return false;
    }
    const ValueElement *status = p.get("status");
    if (status == nullptr || !status->is_string() || status->as_string() != "ok") {
        return false;
    }
    const ValueElement *msg = p.get("message");
    if (msg == nullptr || !msg->is_string()) {
        return false;
    }
    message = msg->as_string();
    return true;
}
} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "spill_replaces_oversized_buffer_result"_test = [] {
        const fs::path work = ts_work_dir("buf_over");
        bt::Session session;
        session.work_dir = kimix::to_string(work);
        buffer_tool tool(&session);
        tool.payload_size = bt::kToolOutputSpillMaxBytes + 1;
        kimix::string display;
        tool(nullptr, display);
        // The result is the pointer payload, not the oversized text.
        expect(tool._result.size() < bt::kToolOutputSpillMaxBytes);
        kimix::string message;
        expect(ts_parse_pointer(tool._result, message)) << "pointer payload";
        expect(message.find("output too long") != kimix::string::npos) << message;
        expect(message.find(" bytes), saved to ") != kimix::string::npos)
            << message;
        // The full payload was dumped next to the session work dir.
        const kimix::string dump = ts_find_dump(work);
        expect(!dump.empty()) << "a dump file exists under .kimix_cache";
        expect(message.find(dump) != kimix::string::npos)
            << "the message names the dump: " << message;
        const kimix::string content = ts_read_file(fs::path(dump));
        expect(content.size() == bt::kToolOutputSpillMaxBytes + 1)
            << "dump holds the full oversized payload";
        expect(content.find(R"JSON("status":"ok")JSON") != kimix::string::npos)
            << "dump content is the original payload";
        // The CLI display line reports the spill (composed after the guard).
        expect(display.find("output too long") != kimix::string::npos) << display;
        std::error_code ec;
        fs::remove_all(work, ec);
    };

    "keep_result_at_exactly_the_threshold"_test = [] {
        const fs::path work = ts_work_dir("buf_exact");
        bt::Session session;
        session.work_dir = kimix::to_string(work);
        buffer_tool tool(&session);
        tool.payload_size = bt::kToolOutputSpillMaxBytes; // not LARGER
        kimix::string display;
        tool(nullptr, display);
        expect(tool._result.size() == bt::kToolOutputSpillMaxBytes);
        expect(ts_find_dump(work).empty()) << "nothing spilled at the threshold";
        kimix::string message;
        expect(!ts_parse_pointer(tool._result, message))
            << "the original payload is not a pointer";
        std::error_code ec;
        fs::remove_all(work, ec);
    };

    "keep_small_result_untouched"_test = [] {
        const fs::path work = ts_work_dir("buf_small");
        bt::Session session;
        session.work_dir = kimix::to_string(work);
        buffer_tool tool(&session);
        tool.payload_size = 64;
        kimix::string display;
        tool(nullptr, display);
        expect(tool._result.size() == 64ul);
        expect(ts_find_dump(work).empty()) << "nothing spilled below the limit";
        expect(kimix::string_view(tool._result.data(), tool._result.size())
                   .find("output too long") == kimix::string_view::npos);
        std::error_code ec;
        fs::remove_all(work, ec);
    };

    "spill_replaces_oversized_params_result"_test = [] {
        const fs::path work = ts_work_dir("params_over");
        bt::Session session;
        session.work_dir = kimix::to_string(work);
        params_tool tool(&session);
        // The serialized {"status":"ok","output":"yyy..."} just over the limit.
        tool.output_chars = bt::kToolOutputSpillMaxBytes;
        kimix::string display;
        tool(nullptr, display);
        kimix::vector<char> payload;
        tool.result_json(payload);
        expect(payload.size() < bt::kToolOutputSpillMaxBytes);
        kimix::string message;
        expect(ts_parse_pointer(payload, message)) << "pointer payload";
        expect(message.find("output too long") != kimix::string::npos) << message;
        const kimix::string dump = ts_find_dump(work);
        expect(!dump.empty()) << "a dump file exists under .kimix_cache";
        const kimix::string content = ts_read_file(fs::path(dump));
        expect(content.find(R"JSON("status":"ok")JSON") != kimix::string::npos);
        expect(content.find("yyyy") != kimix::string::npos)
            << "dump holds the original ToolParams payload";
        // The tool's own result object was exchanged as well: a second
        // result_json (what the soul actually reads) stays the pointer.
        kimix::vector<char> again;
        tool.result_json(again);
        kimix::string again_msg;
        expect(ts_parse_pointer(again, again_msg));
        std::error_code ec;
        fs::remove_all(work, ec);
    };

    return 0;
}
