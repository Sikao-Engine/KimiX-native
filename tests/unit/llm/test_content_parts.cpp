// test_content_parts.cpp - E1/E2: the content-part media adjunct of
// kimix::llm::Message (kosong message.py ContentPart registry):
//   * message_parts / message_set_parts keep the text backbone in sync and a
//     plain-text message reads back as its single implicit TextPart;
//   * message_required_capabilities flags image_in / video_in (check_message)
//     and LLM::chat refuses BEFORE the request with the exact LLMNotSupported
//     wording;
//   * the three providers serialize parts with the reference wire shapes -
//     OpenAI Chat block arrays (model_dump of the part list), Responses
//     input_text/input_image/input_file items, Anthropic content blocks with
//     the base64/url image sources - and a history WITHOUT parts produces the
//     exact same bodies as before the media model existed.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only: no network, only the
// builders and a fake provider.

#include "ut/ut.hpp"

#include <llm/anthropic/anthropic_chat.h>
#include <llm/common.h>
#include <llm/llm.h>
#include <llm/openai/openai_chat.h>
#include <llm/openai_responses/responses_chat.h>

#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

bool contains(const kimix::string &haystack, const kimix::string &needle) {
    return haystack.find(needle) != kimix::string::npos;
}

ContentPart text_part(kimix::string_view text) {
    ContentPart p;
    p.kind = ContentPart::Kind::text;
    p.text.assign(text.data(), text.size());
    return p;
}

ContentPart image_part(kimix::string_view url) {
    ContentPart p;
    p.kind = ContentPart::Kind::image_url;
    p.url.assign(url.data(), url.size());
    return p;
}

struct FakeProvider : kimix::llm::ChatProvider {
    mutable int calls = 0;
    kimix::string model_name() const override { return "fake-model"; }
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &,
         const kimix::llm::AbortCheck * /*abort*/) const override {
        ++calls;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "ok";
        return r;
    }
};

kimix::unique_ptr<kimix::llm::LLM> media_capable_llm(bool image_in,
                                                     FakeProvider *&provider) {
    kimix::llm::Config cfg;
    cfg.type = "openai";
    cfg.model = "fake-model";
    cfg.url = "http://localhost:9";
    cfg.capabilities.image_in = image_in;
    cfg.capabilities.video_in = image_in;
    provider = new FakeProvider();
    return kimix::unique_ptr<kimix::llm::LLM>(new kimix::llm::LLM(
        kimix::unique_ptr<kimix::llm::ChatProvider>(provider),
        std::move(cfg)));
}

} // namespace

int main() {
    "plain_text_message_reads_back_as_one_text_part"_test = [] {
        kimix::llm::Message m;
        m.role = "user";
        m.content = "hello";
        const kimix::vector<kimix::llm::ContentPart> parts = message_parts(m);
        expect(parts.size() == 1_u);
        expect(parts[0].kind == kimix::llm::ContentPart::Kind::text);
        expect(parts[0].text == kimix::string("hello"));
        // No parts on the wire model: the adjunct stays empty.
        expect(m.parts.empty());
    };

    "message_set_parts_syncs_the_text_backbone"_test = [] {
        kimix::llm::Message m;
        m.role = "tool";
        m.tool_call_id = "call-1";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("[Image: untouched, 8x8, 42 bytes]\n"));
        parts.push_back(text_part("<image path=\"shot.png\">"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        parts.push_back(text_part("</image>"));
        message_set_parts(m, std::move(parts));
        // The backbone is the concatenation of the text parts only.
        expect(m.content ==
               kimix::string("[Image: untouched, 8x8, 42 bytes]\n<image "
                             "path=\"shot.png\"></image>"));
        expect(m.parts.size() == 4_u);
        expect(m.parts[2].kind == kimix::llm::ContentPart::Kind::image_url);
        expect(m.parts[2].url == kimix::string("data:image/png;base64,AAAA"));
        // Round trip: parts read back unchanged.
        expect(message_parts(m).size() == 4_u);
    };

    "image_part_requires_the_image_in_capability"_test = [] {
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "user";
        m.content = "what is in this picture?";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("what is in this picture?"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));

        const ModelCapabilities needed =
            message_required_capabilities(messages);
        expect(needed.image_in);
        expect(!needed.video_in);
        expect(!needed.thinking);
    };

    "video_part_requires_the_video_in_capability"_test = [] {
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "user";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("watch"));
        kimix::llm::ContentPart video;
        video.kind = kimix::llm::ContentPart::Kind::video_url;
        video.url = "data:video/mp4;base64,AAAA";
        parts.push_back(std::move(video));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));
        expect(message_required_capabilities(messages).video_in);
        expect(!message_required_capabilities(messages).image_in);
    };

    "unsupported_media_refused_before_the_request"_test = [] {
        FakeProvider *provider = nullptr;
        auto llm = media_capable_llm(/*image_in=*/false, provider);
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "user";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("look"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));
        const kimix::llm::ChatResult r = llm->chat(messages, {});
        expect(!r.ok);
        expect(r.error_kind == ChatErrorKind::not_supported);
        expect(provider->calls == 0); // refused BEFORE sending
        // The reference's LLMNotSupported wording (soul/__init__.py:40-48).
        expect(r.error == "LLM model 'fake-model' does not support required "
                          "capability: image_in.");
    };

    "media_capable_model_passes_the_gate"_test = [] {
        FakeProvider *provider = nullptr;
        auto llm = media_capable_llm(/*image_in=*/true, provider);
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "user";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("look"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));
        const kimix::llm::ChatResult r = llm->chat(messages, {});
        expect(r.ok);
        expect(provider->calls == 1);
    };

    "openai_chat_serializes_a_part_array"_test = [] {
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "user";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("look"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));
        const kimix::string body =
            openai::build_chat_body(cfg, openai_wire_messages(messages), {});
        expect(contains(body, "\"content\":["));
        expect(contains(body, "{\"type\":\"text\",\"text\":\"look\"}"));
        // model_dump of ImageURLPart -> {"type":"image_url",
        // "image_url":{"url":"..."}} (id: None is excluded).
        expect(contains(body, "{\"type\":\"image_url\",\"image_url\":"
                              "{\"url\":\"data:image/png;base64,AAAA\"}}"));
        // No stray string form of the backbone beside the array.
        expect(!contains(body, "\"content\":\"look"));
    };

    "openai_chat_tool_results_stay_text_only"_test = [] {
        // openai_legacy flattens tool content to text (extract_text), so a
        // tool result's media parts never reach the Chat Completions wire.
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message m;
        m.role = "tool";
        m.tool_call_id = "call-1";
        kimix::vector<kimix::llm::ContentPart> parts;
        parts.push_back(text_part("note"));
        parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(m, std::move(parts));
        messages.push_back(std::move(m));
        const kimix::string body =
            openai::build_chat_body(cfg, openai_wire_messages(messages), {});
        expect(contains(body, "\"content\":\"note"));
        expect(!contains(body, "image_url"));
    };

    "responses_serializes_input_blocks_and_tool_output_items"_test = [] {
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        kimix::vector<kimix::llm::Message> messages;

        kimix::llm::Message user;
        user.role = "user";
        kimix::vector<kimix::llm::ContentPart> user_parts;
        user_parts.push_back(text_part("look"));
        user_parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(user, std::move(user_parts));
        messages.push_back(std::move(user));

        kimix::llm::Message tool;
        tool.role = "tool";
        tool.tool_call_id = "call-9";
        kimix::vector<kimix::llm::ContentPart> tool_parts;
        tool_parts.push_back(text_part("preview"));
        tool_parts.push_back(image_part("data:image/jpeg;base64,BBBB"));
        message_set_parts(tool, std::move(tool_parts));
        messages.push_back(std::move(tool));

        const kimix::string body =
            openai_responses::build_responses_body(
                cfg, responses_wire_input(messages), {});
        // _content_parts_to_input_items: text then image with the default
        // "auto" detail.
        expect(contains(body, "{\"type\":\"input_text\",\"text\":\"look\"}"));
        expect(contains(body, "{\"type\":\"input_image\",\"detail\":\"auto\","
                              "\"image_url\":\"data:image/png;base64,AAAA\"}"));
        // _message_content_to_function_output_items: the tool output is a
        // list and the image item carries NO detail key.
        expect(contains(body, "\"output\":["));
        expect(contains(body, "{\"type\":\"input_image\",\"image_url\":"
                              "\"data:image/jpeg;base64,BBBB\"}"));
    };

    "responses_maps_audio_urls_to_input_file_items"_test = [] {
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message user;
        user.role = "user";
        kimix::vector<kimix::llm::ContentPart> parts;
        kimix::llm::ContentPart audio;
        audio.kind = kimix::llm::ContentPart::Kind::audio_url;
        audio.url = "data:audio/mpeg;base64,AAAA";
        parts.push_back(std::move(audio));
        message_set_parts(user, std::move(parts));
        messages.push_back(std::move(user));
        const kimix::string body =
            openai_responses::build_responses_body(
                cfg, responses_wire_input(messages), {});
        std::printf("AUDIO-BODY: %s\n", body.c_str());
        // _map_audio_url_to_input_item: mpeg -> inline.mp3 input_file.
        expect(contains(body, "{\"type\":\"input_file\",\"file_data\":"
                              "\"AAAA\",\"filename\":\"inline.mp3\"}"));
    };

    "anthropic_serializes_base64_and_url_image_blocks"_test = [] {
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        cfg.anthropic_cache_control = false;

        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message png;
        png.role = "user";
        kimix::vector<kimix::llm::ContentPart> png_parts;
        png_parts.push_back(text_part("look"));
        png_parts.push_back(image_part("data:image/png;base64,AAAA"));
        message_set_parts(png, std::move(png_parts));
        messages.push_back(std::move(png));

        kimix::llm::Message remote;
        remote.role = "user";
        kimix::vector<kimix::llm::ContentPart> remote_parts;
        remote_parts.push_back(image_part("https://example.com/cat.png"));
        message_set_parts(remote, std::move(remote_parts));
        messages.push_back(std::move(remote));

        const kimix::string body =
            anthropic::build_messages_body(
                cfg, "", anthropic_wire_request(messages).messages, {});
        // Base64 source for the data URL.
        expect(contains(body, "{\"type\":\"text\",\"text\":\"look\"}"));
        expect(contains(body, "{\"type\":\"image\",\"source\":{\"type\":"
                              "\"base64\",\"data\":\"AAAA\",\"media_type\":"
                              "\"image/png\"}}"));
        // URL source for the remote image.
        expect(contains(body, "{\"type\":\"image\",\"source\":{\"type\":"
                              "\"url\",\"url\":\"https://example.com/"
                              "cat.png\"}}"));
    };

    "anthropic_degrades_bad_media_to_error_text_blocks"_test = [] {
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        cfg.anthropic_cache_control = false;

        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message bad_scheme;
        bad_scheme.role = "user";
        kimix::vector<kimix::llm::ContentPart> bad_parts;
        bad_parts.push_back(image_part("data:image/png,not-base64"));
        message_set_parts(bad_scheme, std::move(bad_parts));
        messages.push_back(std::move(bad_scheme));

        kimix::llm::Message bad_type;
        bad_type.role = "user";
        kimix::vector<kimix::llm::ContentPart> type_parts;
        type_parts.push_back(image_part("data:image/tiff;base64,AAAA"));
        message_set_parts(bad_type, std::move(type_parts));
        messages.push_back(std::move(bad_type));

          const kimix::string body =
              anthropic::build_messages_body(
                  cfg, "", anthropic_wire_request(messages).messages, {});
          std::printf("ANTH-BODY: %s\n", body.c_str());
          expect(contains(body, "Error: Invalid data URL for image: "
                                "data:image/png,not-base64"));
          expect(contains(body, "Error: Unsupported media type for base64 "
                                "image: image/tiff, url: "
                                "data:image/tiff;base64,AAAA"));
    };

    "plain_text_histories_produce_the_legacy_wire"_test = [] {
        // Gate everything behind message content actually carrying parts: a
        // history without parts must not contain any block-array content.
        kimix::llm::Config cfg;
        cfg.model = "m";
        cfg.url = "http://localhost:9";
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message user;
        user.role = "user";
        user.content = "hello";
        messages.push_back(std::move(user));
        kimix::llm::Message tool;
        tool.role = "tool";
        tool.tool_call_id = "call-1";
        tool.content = "result";
        messages.push_back(std::move(tool));

        const kimix::string openai_body =
            openai::build_chat_body(cfg, openai_wire_messages(messages), {});
        expect(!contains(openai_body, "\"content\":["));
        const kimix::string responses_body =
            openai_responses::build_responses_body(
                cfg, responses_wire_input(messages), {});
        expect(!contains(responses_body, "\"input_image\""));
        const kimix::string anthropic_body = anthropic::build_messages_body(
            cfg, "", anthropic_wire_request(messages).messages, {});
        expect(!contains(anthropic_body, "\"source\""));
    };

    return 0;
}
