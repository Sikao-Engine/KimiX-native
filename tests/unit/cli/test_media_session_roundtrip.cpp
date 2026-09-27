// test_media_session_roundtrip.cpp - E1/E2: the session-store round trip of
// image (media) content parts.
//   * the record writer emits the reference's part-array shape
//     (kosong Message._serialize_content -> part.model_dump():
//     {"type":"image_url","image_url":{"url":...}}) whenever the message
//     carries media parts, and stays byte-identical otherwise;
//   * load_history restores the media part as a real ContentPart (the
//     data: URL stays intact, no truncation) with the text backbone kept;
//   * the reference's own record bytes (nested image_url.url) parse;
//   * the transcript (wire.jsonl fallback) and the markdown export render
//     media parts as "[image: <url-or-data-len>]" placeholders.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only.

#include "ut/ut.hpp"

#include <cstdio>
#include <cstring>

#include <cli/cli_session.h>
#include <core/kimix_core.h>
#include <llm/llm.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace {

const char *const kDataUrl = "data:image/png;base64,AAAA";

kimix::llm::Message media_tool_message() {
    kimix::llm::Message msg;
    msg.role = "tool";
    msg.tool_call_id = "call-1";
    kimix::vector<kimix::llm::ContentPart> parts;
    kimix::llm::ContentPart text;
    text.kind = kimix::llm::ContentPart::Kind::text;
    text.text =
        "[Image: untouched, 8x8, 4 bytes]\n<image path=\"shot.png\"></image>";
    parts.push_back(std::move(text));
    kimix::llm::ContentPart image;
    image.kind = kimix::llm::ContentPart::Kind::image_url;
    image.url = kDataUrl;
    parts.push_back(std::move(image));
    kimix::llm::message_set_parts(msg, std::move(parts));
    return msg;
}

kimix::string slurp(const kimix::string &path) {
    kimix::string out;
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return out;
    }
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        out.append(buffer, n);
    }
    std::fclose(fp);
    return out;
}

bool contains_substr(const kimix::string &haystack, const char *needle) {
    return haystack.find(needle) != kimix::string::npos;
}

kimix::string tmp_work_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

} // namespace

int main() {
    using namespace kimix::cli;

    "media_message_round_trips_through_the_store"_test = [] {
        const kimix::string work = tmp_work_dir("kimix_media_roundtrip_ws");
        session_store store;
        kimix::string error;
        expect(store.open(work, "media-session", /*resume=*/false, error));

        kimix::vector<kimix::llm::Message> history;
        kimix::llm::Message user;
        user.role = "user";
        user.content = "look at the image";
        history.push_back(std::move(user));
        history.push_back(media_tool_message());
        expect(store.save_history(history, error));

        kimix::vector<kimix::llm::Message> loaded;
        expect(store.load_history(loaded, error));
        expect(loaded.size() == 2_u);
        // Guard the indexing so a failed restore reports instead of crashing.
        if (loaded.size() < 2 || loaded[1].parts.size() < 2) {
            std::printf("MEDIA PARTS MISSING: role=%s parts=%zu\n",
                        loaded.size() > 1 ? loaded[1].role.c_str() : "?",
                        loaded.size() > 1 ? loaded[1].parts.size() : 0);
            std::fflush(stdout);
            expect(false);
            return;
        }
        expect(loaded[0].parts.empty()); // plain text stays plain
        expect(loaded[1].role == kimix::string("tool"));
        expect(loaded[1].tool_call_id == kimix::string("call-1"));
        // The text backbone is preserved verbatim...
        expect(loaded[1].content ==
               kimix::string("[Image: untouched, 8x8, 4 bytes]\n<image "
                             "path=\"shot.png\"></image>"));
        // ...and the media part is restored intact (no truncation).
        expect(loaded[1].parts.size() == 2_u);
        expect(loaded[1].parts[0].kind ==
               kimix::llm::ContentPart::Kind::text);
        expect(loaded[1].parts[1].kind ==
               kimix::llm::ContentPart::Kind::image_url);
        expect(loaded[1].parts[1].url == kimix::string(kDataUrl));
        expect(loaded[1].parts[1].detail.empty());

        // A second save/load cycle is stable (idempotent records).
        expect(store.save_history(loaded, error));
        kimix::vector<kimix::llm::Message> reloaded;
        expect(store.load_history(reloaded, error));
        expect(reloaded[1].parts.size() == 2_u);
        expect(reloaded[1].parts[1].url == kimix::string(kDataUrl));

        expect(store.close(/*delete_if_anonymous=*/false, error));
    };

    "record_writer_emits_the_reference_part_shape"_test = [] {
        const kimix::string work = tmp_work_dir("kimix_media_record_ws");
        session_store store;
        kimix::string error;
        expect(store.open(work, "media-record", /*resume=*/false, error));

        kimix::vector<kimix::llm::Message> history;
        history.push_back(media_tool_message());
        expect(store.save_history(history, error));

        // The record line: role, tool_call_id, then the part array with the
        // reference's nested image_url payload (kosong part.model_dump(),
        // id: None excluded).
        const kimix::string context =
            slurp(session_store::session_dir(work, "media-record") +
                  "/context.jsonl");
        expect(contains_substr(context,
                               "\"type\":\"image_url\",\"image_url\":"
                               "{\"url\":\"data:image/png;base64,AAAA\"}"));
        expect(contains_substr(context, "\"tool_call_id\":\"call-1\""));
        // No legacy flat text form of the media part.
        expect(!contains_substr(context, "\"text\":\"data:image"));

        // Plain-text neighbours keep the bare-string record form.
        kimix::vector<kimix::llm::Message> plain;
        kimix::llm::Message user;
        user.role = "user";
        user.content = "hello";
        plain.push_back(std::move(user));
        expect(store.save_history(plain, error));
        const kimix::string context2 =
            slurp(session_store::session_dir(work, "media-record") +
                  "/context.jsonl");
        expect(contains_substr(context2, "{\"role\":\"user\",\"content\":"
                                         "\"hello\"}"));
        expect(store.close(false, error));
    };

    "reference_records_with_media_parts_parse"_test = [] {
        const kimix::string work = tmp_work_dir("kimix_media_ref_ws");
        session_store store;
        kimix::string error;
        expect(store.open(work, "media-ref", /*resume=*/false, error));

        // A record exactly as kosong's Message.model_dump_json(
        // exclude_none=True) writes it (content as a part array).
        const kimix::string context_path =
            session_store::session_dir(work, "media-ref") + "/context.jsonl";
        {
            const char *record =
                "{\"role\":\"user\",\"content\":[{\"type\":\"text\","
                "\"text\":\"what is this?\"},{\"type\":\"image_url\","
                "\"image_url\":{\"url\":\"data:image/jpeg;base64,BBBB\"}}]}";
            std::FILE *fp = std::fopen(context_path.c_str(), "wb");
            std::fwrite(record, 1, std::strlen(record), fp);
            std::fclose(fp);
        }
        kimix::vector<kimix::llm::Message> loaded;
        expect(store.load_history(loaded, error));
        expect(loaded.size() == 1_u);
        expect(loaded[0].content == kimix::string("what is this?"));
        expect(loaded[0].parts.size() == 2_u);
        expect(loaded[0].parts[1].kind ==
               kimix::llm::ContentPart::Kind::image_url);
        expect(loaded[0].parts[1].url ==
               kimix::string("data:image/jpeg;base64,BBBB"));
        expect(store.close(false, error));
    };

    "transcript_and_export_render_the_media_placeholder"_test = [] {
        const kimix::string work = tmp_work_dir("kimix_media_print_ws");
        session_store store;
        kimix::string error;
        expect(store.open(work, "media-print", /*resume=*/false, error));

        kimix::vector<kimix::llm::Message> history;
        kimix::llm::Message user;
        user.role = "user";
        user.content = "look";
        history.push_back(std::move(user));
        history.push_back(media_tool_message());
        expect(store.save_history(history, error));

        // wire.jsonl fallback transcript: the tool result's output text ends
        // with the "[image: <data-len>]" placeholder.
        const kimix::string wire =
            slurp(session_store::session_dir(work, "media-print") +
                  "/wire.jsonl");
        // "data:image/png;base64,AAAA" is 26 characters.
        expect(contains_substr(wire, "[image: 26 chars]"));

        // The markdown export renders the same placeholder.
        const kimix::string export_path =
            session_store::session_dir(work, "media-print") + "/export.md";
        expect(store.export_markdown(history, export_path, error));
        const kimix::string markdown = slurp(export_path);
        expect(contains_substr(markdown, "[image: 26 chars]"));
        // The base64 payload itself never lands in the transcript/export.
        expect(!contains_substr(markdown, "AAAA"));
        expect(store.close(false, error));
    };

    return 0;
}
