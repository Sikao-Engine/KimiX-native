// test_media_capability_gate.cpp - The media capability gate in the soul's
// tool dispatch (src/agent/soul.cpp finish_tool_dispatch): when a tool result
// carries media (read_image's data_url) but the model does not advertise
// image_in, the dispatcher must refuse with the reference's ToolError wording
// (read_media.py:532-539, brief "Unsupported media type") instead of attaching
// an image_url part - one media part in the history would fail the capability
// pre-flight of EVERY later chat of the session. The tool result is a regular
// error the model can read and the turn continues.
//
// Framework: Boost.UT (tests/ut/ut.hpp). No network access: scripted fake
// chat backends.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"

#include <cstdio>

namespace {

using namespace boost::ut;

// Scripted chat backend with controllable model capabilities: the default is
// a TEXT-ONLY model (image_in == false), the production-Config shape that
// produced "chat failed: LLM model '<model>' does not support required
// capability: image_in" before the gate existed.
class CapBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "text-only-fake"; }
    kimix::llm::ModelCapabilities model_capabilities() const override {
        return caps;
    }

    kimix::llm::ModelCapabilities caps; // zeroed: no image_in, no video_in
};

kimix::string tmp_workspace() {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / "kimix_media_gate_test_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// A 1x1 PNG: 8-byte signature + IHDR (length 13, CRC not validated by the
// sniffer). The .png suffix alone decides the file type; the IHDR gives the
// dimension sniffer real 1x1 values.
bool write_png(const kimix::filesystem::path &p) {
    static const unsigned char k_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // signature
        0x00, 0x00, 0x00, 0x0D,                         // IHDR length
        0x49, 0x48, 0x44, 0x52,                         // "IHDR"
        0x00, 0x00, 0x00, 0x01,                         // width = 1
        0x00, 0x00, 0x00, 0x01,                         // height = 1
        0x08, 0x06, 0x00, 0x00, 0x00,                   // 8-bit RGBA, defaults
        0x00, 0x00, 0x00, 0x00,                         // CRC (unvalidated)
    };
    std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = std::fwrite(k_png, 1, sizeof(k_png), f);
    std::fclose(f);
    return n == sizeof(k_png);
}

bool history_has_image_part(const kimix::agent::AgentSession &session) {
    for (const kimix::llm::Message &m : session.history()) {
        for (const kimix::llm::ContentPart &part : m.parts) {
            if (part.kind == kimix::llm::ContentPart::Kind::image_url) {
                return true;
            }
        }
    }
    return false;
}

const kimix::string k_refusal =
    "<system>ERROR: The current model does not support image input. "
    "Tell the user to use a model with image input capability.</system>";

} // namespace

int main() {
    using namespace boost::ut;

    // A text-only model: the read_image result must be refused with the
    // reference's ToolError wording, no image_url part is attached, and the
    // turn CONTINUES (the model gets a second step and finishes normally)
    // instead of dying on the capability pre-flight of the next chat.
    "text_only_model_refuses_media_and_turn_continues"_test = [] {
        const kimix::string ws = tmp_workspace();
        const kimix::filesystem::path png =
            kimix::filesystem::path(ws) / "pixel.png";
        expect(write_png(png));

        kimix::agent::AgentSession session(ws);
        CapBackend backend; // caps zeroed: image_in == false

        kimix::llm::ChatResult step1;
        step1.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call_img";
        tc.name = "read_image";
        tc.arguments = R"JSON({"path":"pixel.png"})JSON";
        step1.tool_calls.push_back(tc);
        kimix::llm::ChatResult step2;
        step2.ok = true;
        step2.content = "Understood, no image support.";
        backend.scripted = {step1, step2};

        kimix::agent::KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false;
        kimix::agent::KimiSoul soul(session, backend, opts);
        const kimix::agent::TurnResult tr = soul.turn("look at the picture");
        expect(tr.ok) << tr.error;
        expect(eq(tr.steps, 2));
        expect(eq(tr.content, kimix::string("Understood, no image support.")));

        // The tool message is the regular error envelope (read_media.py
        // ToolError wording) - the model can read it and react.
        bool saw_refusal = false;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool" && m.tool_call_id == "call_img") {
                saw_refusal = m.content.find(k_refusal) != kimix::string::npos;
            }
        }
        expect(saw_refusal);
        // No image_url part anywhere: the session is not poisoned, so the
        // next chat cannot hit the capability pre-flight refusal.
        expect(!history_has_image_part(session));
    };

    // An image-capable model: the same call lifts the data_url into a real
    // image_url ContentPart on the tool message (the pre-gate E1/E2 path).
    "image_capable_model_receives_media_part"_test = [] {
        const kimix::string ws = tmp_workspace();
        const kimix::filesystem::path png =
            kimix::filesystem::path(ws) / "pixel.png";
        expect(write_png(png));

        kimix::agent::AgentSession session(ws);
        CapBackend backend;
        backend.caps.image_in = true;

        kimix::llm::ChatResult step1;
        step1.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call_img";
        tc.name = "read_image";
        tc.arguments = R"JSON({"path":"pixel.png"})JSON";
        step1.tool_calls.push_back(tc);
        kimix::llm::ChatResult step2;
        step2.ok = true;
        step2.content = "I see a pixel.";
        backend.scripted = {step1, step2};

        kimix::agent::KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false;
        kimix::agent::KimiSoul soul(session, backend, opts);
        const kimix::agent::TurnResult tr = soul.turn("look at the picture");
        expect(tr.ok) << tr.error;

        expect(history_has_image_part(session));
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool" && m.tool_call_id == "call_img") {
                expect(m.content.find("<system>ERROR") == kimix::string::npos);
            }
        }
    };

    // Direct dispatch view of the same gate: the out-param media collection
    // stays empty for a text-only model and the error string is reported.
    "direct_dispatch_gate"_test = [] {
        const kimix::string ws = tmp_workspace();
        const kimix::filesystem::path png =
            kimix::filesystem::path(ws) / "pixel.png";
        expect(write_png(png));

        kimix::agent::AgentSession session(ws);
        CapBackend backend; // text-only

        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        kimix::agent::KimiSoul::ToolDispatchInfo info;
        kimix::vector<kimix::llm::ContentPart> media;
        const kimix::string out = soul.execute_tool_call(
            "read_image", R"JSON({"path":"pixel.png"})JSON", err, "call_x",
            &info, &media);
        expect(eq(media.size(), size_t(0)));
        expect(!err.empty());
        expect(out.find(k_refusal) != kimix::string::npos);

        // And with image_in advertised the media part is lifted as before.
        backend.caps.image_in = true;
        kimix::vector<kimix::llm::ContentPart> media2;
        const kimix::string out2 = soul.execute_tool_call(
            "read_image", R"JSON({"path":"pixel.png"})JSON", err, "call_y",
            &info, &media2);
        expect(eq(media2.size(), size_t(1)));
        expect(media2[0].kind == kimix::llm::ContentPart::Kind::image_url);
        expect(media2[0].url.find("data:image/png;base64,") == 0);
        expect(out2.find("<system>ERROR") == kimix::string::npos);
    };

    return 0;
}
