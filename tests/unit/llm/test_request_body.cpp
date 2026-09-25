// Test for the request-body builders of the three LLM providers
// (llm/openai/openai_chat.cpp, llm/openai_responses/responses_chat.cpp,
// llm/anthropic/anthropic_chat.cpp) and for the UTF-8 policy that keeps a
// request body from being silently dropped.
//
// Regression context (the recorded CLI failure):
//   >>> xmake run kimix_cli --config=qwen_scnet.json
//   chat failed: failed to build request body
// Every build_*_body() ends with yyjson_mut_write_opts(doc, 0, ...), and yyjson
// validates UTF-8 while writing: a single invalid byte sequence makes the
// writer fail with YYJSON_WRITE_ERROR_INVALID_STRING ("invalid utf-8 encoding
// in string"), the builder returns an EMPTY string and the chat call reports
// the opaque "failed to build request body" - the whole turn is lost.
// The bad bytes reached the request from two places:
//  * the embedded prompt templates of agent/system_prompt.cpp, which contain
//    U+2014. MSVC reads a BOM-less UTF-8 source with the ANSI codepage when
//    /utf-8 is not set; on a GBK (ACP 936) host the sequence E2 80 94 decodes
//    as one GBK char plus a dangling lead byte and is re-encoded as E2 80 3F
//    ("?") - invalid UTF-8 in the compiled literal.
//  * runtime text that is not UTF-8 (a tool result cut on a byte boundary,
//    output of a legacy-codepage subprocess, the body of a GBK file).
// So the wire layer must (a) carry byte-exact UTF-8 templates and (b) never let
// one bad sequence destroy the entire request: invalid sequences are replaced
// with U+FFFD, exactly like CPython's decode(errors="replace").
//
// No network access: only the builders are exercised.

#include "ut/ut.hpp"

#include "agent/system_prompt.h"
#include "llm/anthropic/anthropic_chat.h"
#include "llm/common.h"
#include "llm/llm.h"
#include "llm/openai/openai_chat.h"
#include "llm/openai_responses/responses_chat.h"
#include "llm/yyjson_alc.h"

#include "yyjson.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

// U+2014 EM DASH in UTF-8, hex-escaped so this test file stays pure ASCII (a
// raw em dash here would be mangled by the very bug under test).
const char *const kEmDash = "\xE2\x80\x94";
// The GBK-miscompiled form of the same character (E2 80 followed by '?').
const char *const kBrokenEmDash = "\xE2\x80\x3F";
// U+FFFD REPLACEMENT CHARACTER in UTF-8.
const char *const kReplacement = "\xEF\xBF\xBD";

// Strict UTF-8 validator, written independently of the product code so it
// cannot be fooled by the implementation it checks (same policy as yyjson's
// writer: no bad lead bytes, no short sequences, no overlongs, no surrogates,
// max U+10FFFF).
bool utf8_ok(const char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        size_t need = 0;
        unsigned int cp = 0;
        if (c < 0x80u) {
            ++i;
            continue;
        } else if ((c & 0xE0u) == 0xC0u) {
            need = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3;
            cp = c & 0x07u;
        } else {
            return false; // a lone continuation byte or 0xF8-0xFF
        }
        if (i + need >= n) {
            return false; // truncated sequence
        }
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(p[i + k]);
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | static_cast<unsigned int>(cc & 0x3Fu);
        }
        if (need == 1u && cp < 0x80u) return false;    // overlong
        if (need == 2u && cp < 0x800u) return false;   // overlong
        if (need == 3u && cp < 0x10000u) return false; // overlong
        if (cp > 0x10FFFFu) return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false; // surrogate
        i += need + 1u;
    }
    return true;
}

bool utf8_ok(const kimix::string &s) { return utf8_ok(s.data(), s.size()); }

// True when `hay` contains the byte sequence `needle`.
bool contains(const kimix::string &hay, const char *needle) {
    return hay.find(needle) != kimix::string::npos;
}

bool contains(const kimix::string &hay, const kimix::string &needle) {
    return hay.find(needle) != kimix::string::npos;
}

// A body is only useful if the backend can parse it: strict yyjson parse.
bool is_strict_json(const kimix::string &s) {
    yyjson_doc *doc = yyjson_read_opts((char *)s.data(), s.size(), 0,
                                       &kYYJsonAlcMi, nullptr);
    if (doc) {
        yyjson_doc_free(doc);
        return true;
    }
    return false;
}

Config make_config() {
    Config cfg;
    cfg.type = "openai_legacy";
    cfg.model = "test-model";
    cfg.url = "http://127.0.0.1:9/v1";
    cfg.api_key = "k";
    return cfg;
}

// The system prompt exactly as the native CLI builds it for a worker turn.
kimix::string worker_system_prompt() {
    agent::system_prompt_input in;
    in.role = agent::system_prompt_role::worker;
    in.os = "Windows";
    in.work_dir = "D:\\proj";
    in.yolo = true;
    in.skills_text = "- demo\n";
    in.agents_md = "Project rules.\n";
    return agent::build_system_prompt(in);
}

} // namespace

int main() {
    // -- (a) the embedded prompt templates reach the wire as valid UTF-8 ------
    "prompt_templates_are_valid_utf8"_test = [] {
        const kimix::string prompt = worker_system_prompt();
        expect(utf8_ok(prompt))
            << "system prompt holds invalid UTF-8: the compiled literals are "
               "not read as UTF-8 (MSVC needs /utf-8 on a legacy-ACP host)";
        // The "# Tool Conventions" template line carries a real em dash; a
        // codepage round-trip turns it into E2 80 3F.
        expect(contains(prompt, kEmDash)) << "em dash lost in compilation";
        expect(!contains(prompt, kBrokenEmDash))
            << "em dash mangled into E2 80 3F by the ANSI source charset";
    };

    // The recorded failure, end to end: CLI system prompt -> OpenAI body.
    "openai_body_with_system_prompt_builds"_test = [] {
        const Config cfg = make_config();
        kimix::vector<openai::ChatMessage> messages;
        messages.push_back({"system", worker_system_prompt(), {}, {}});
        messages.push_back({"user", "list the tools you have", {}, {}});
        const kimix::string body = openai::build_chat_body(cfg, messages, {});
        expect(!body.empty()) << "build_chat_body returned an empty body";
        expect(is_strict_json(body)) << "the body is not parseable JSON";
        expect(utf8_ok(body)) << "the body is not valid UTF-8";
        expect(contains(body, "\"model\":\"test-model\"")) << "model missing";
    };

    // -- (b) one invalid sequence must not destroy the whole request ---------
    "openai_body_survives_invalid_utf8"_test = [] {
        const Config cfg = make_config();
        // truncated 2-byte sequence, stray 0xFF, overlong encoding, lone
        // continuation byte - one of each, spread over the fields that go on
        // the wire (content, tool_call_id, tool name, tool arguments).
        kimix::vector<openai::ChatMessage> messages;
        messages.push_back({"system", "rules \xE2\x80 ok", {}, {}});
        messages.push_back({"user", "bad tail \xE2\x80", {}, {}});
        messages.push_back({"tool", "stray \xFF byte", "call_1\x41", {}});
        openai::ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = "overlong \xC0\xAF\x41";
        openai::ToolCall tc;
        tc.id = "id_1\x41";
        tc.type = "function";
        tc.name = "na\x80 me";
        tc.arguments = "{\"a\":\"\xC2\"}";
        assistant.tool_calls.push_back(tc);
        messages.push_back(assistant);
        const kimix::string body = openai::build_chat_body(cfg, messages, {});
        expect(!body.empty())
            << "invalid UTF-8 dropped the whole request body";
        expect(is_strict_json(body)) << "sanitized body is not valid JSON";
        expect(utf8_ok(body)) << "sanitized body still holds invalid UTF-8";
        expect(contains(body, kReplacement)) << "no U+FFFD replacement marker";
        expect(contains(body, "\"tool_calls\"")) << "tool_calls lost";
        expect(contains(body, "bad tail")) << "message lost";
    };

    "anthropic_body_survives_invalid_utf8"_test = [] {
        const Config cfg = make_config();
        kimix::vector<anthropic::ChatMessage> messages;
        anthropic::ChatMessage user;
        user.role = "user";
        user.text = "truncated \xE2\x80";
        messages.push_back(user);
        anthropic::ChatMessage tr;
        tr.role = "user";
        tr.tool_result_id = "tu_1\x41";
        tr.tool_result_content = "stray \xFF byte";
        messages.push_back(tr);
        anthropic::ChatMessage as;
        as.role = "assistant";
        as.text = "overlong \xC0\xAF\x41";
        as.thinking = "thinking \x41";
        as.thinking_signature = "sig \x80";
        anthropic::ToolUse tu;
        tu.id = "id_1\x41";
        tu.name = "bash";
        tu.input_json = "{\"a\":1}\x80";
        as.tool_uses.push_back(tu);
        messages.push_back(as);
        const kimix::string body =
            anthropic::build_messages_body(cfg, "system \xE2\x80", messages, {});
        expect(!body.empty()) << "invalid UTF-8 dropped the Anthropic body";
        expect(is_strict_json(body)) << "sanitized body is not valid JSON";
        expect(utf8_ok(body)) << "sanitized body still holds invalid UTF-8";
        expect(contains(body, "\"model\":\"test-model\"")) << "model missing";
        expect(contains(body, "tool_use")) << "tool_use block lost";
    };

    "responses_body_survives_invalid_utf8"_test = [] {
        const Config cfg = make_config();
        kimix::vector<openai_responses::InputItem> input;
        openai_responses::InputItem msg;
        msg.type = "message";
        msg.role = "user";
        msg.content = "bad \xE2\x80 tail";
        input.push_back(msg);
        openai_responses::InputItem rs;
        rs.type = "reasoning";
        rs.content = "bad \xFF byte";
        rs.item_id = "rs_1\x41";
        input.push_back(rs);
        openai_responses::InputItem fc;
        fc.type = "function_call";
        fc.call_id = "call_1\x41";
        fc.name = "ba\x80 sh";
        fc.arguments = "{\"command\":\"ls\"}\x80";
        input.push_back(fc);
        openai_responses::InputItem fo;
        fo.type = "function_call_output";
        fo.call_id = "call_1\x41";
        fo.content = "out \xC0\x41";
        input.push_back(fo);
        const kimix::string body =
            openai_responses::build_responses_body(cfg, input, {});
        expect(!body.empty()) << "invalid UTF-8 dropped the Responses body";
        expect(is_strict_json(body)) << "sanitized body is not valid JSON";
        expect(utf8_ok(body)) << "sanitized body still holds invalid UTF-8";
        expect(contains(body, "function_call_output")) << "output item lost";
    };

    // -- valid multi-byte text must survive byte-exact ----------------------
    "valid_multibyte_text_preserved"_test = [] {
        const Config cfg = make_config();
        // U+4E2D U+6587 ("Chinese"), U+1F600 (grinning face), U+2014 (em dash).
        const kimix::string text =
            "\xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80 \xE2\x80\x94";
        expect(utf8_ok(text)) << "test fixture is not valid UTF-8";

        kimix::vector<openai::ChatMessage> omessages;
        omessages.push_back({"user", text, {}, {}});
        const kimix::string obody = openai::build_chat_body(cfg, omessages, {});
        expect(contains(obody, text)) << "OpenAI body altered valid UTF-8";

        kimix::vector<anthropic::ChatMessage> amessages;
        anthropic::ChatMessage m;
        m.role = "user";
        m.text = text;
        amessages.push_back(m);
        const kimix::string abody =
            anthropic::build_messages_body(cfg, text, amessages, {});
        expect(contains(abody, text)) << "Anthropic body altered valid UTF-8";
        expect(contains(abody, kEmDash)) << "em dash not preserved";

        kimix::vector<openai_responses::InputItem> input;
        openai_responses::InputItem item;
        item.type = "message";
        item.role = "user";
        item.content = text;
        input.push_back(item);
        const kimix::string rbody =
            openai_responses::build_responses_body(cfg, input, {});
        expect(contains(rbody, text)) << "Responses body altered valid UTF-8";
    };

    // A tool schema that carries an invalid byte must still be sent: a strict
    // parse of the raw schema fails and silently replaces it with null, which
    // costs the model the whole parameter list.
    "tool_schema_survives_invalid_utf8"_test = [] {
        const Config cfg = make_config();

        kimix::vector<openai::Tool> otools;
        openai::Tool ot;
        ot.name = "bash";
        ot.description = "Run a command \xE2\x80 in a shell";
        ot.parameters_json =
            "{\"type\":\"object\",\"properties\":{\"cmd\":"
            "{\"type\":\"string\",\"description\":\"what \xC0\x41 to run\"}},"
            "\"required\":[\"cmd\"]}";
        otools.push_back(ot);
        kimix::vector<openai::ChatMessage> omessages;
        omessages.push_back({"user", "hi", {}, {}});
        const kimix::string obody =
            openai::build_chat_body(cfg, omessages, otools);
        expect(!obody.empty()) << "OpenAI body dropped for a bad schema byte";
        expect(is_strict_json(obody)) << "OpenAI body invalid JSON";
        expect(utf8_ok(obody)) << "OpenAI tool body still invalid UTF-8";
        expect(!contains(obody, "\"parameters\":null"))
            << "OpenAI tool schema nulled out by invalid UTF-8";
        expect(contains(obody, "\"required\":[\"cmd\"]")) << "schema lost";

        kimix::vector<anthropic::Tool> atools;
        anthropic::Tool at;
        at.name = "bash";
        at.description = "Run a command \xE2\x80 in a shell";
        at.input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"cmd\":"
            "{\"type\":\"string\",\"description\":\"what \xC0\x41 to run\"}}}";
        atools.push_back(at);
        kimix::vector<anthropic::ChatMessage> amessages;
        anthropic::ChatMessage um;
        um.role = "user";
        um.text = "hi";
        amessages.push_back(um);
        const kimix::string abody =
            anthropic::build_messages_body(cfg, "", amessages, atools);
        expect(!abody.empty()) << "Anthropic body dropped for a bad schema byte";
        expect(is_strict_json(abody)) << "Anthropic body invalid JSON";
        expect(!contains(abody, "\"input_schema\":{}"))
            << "Anthropic input_schema dropped on invalid UTF-8";
        expect(contains(abody, "\"required\"") ||
               contains(abody, "\"properties\""))
            << "Anthropic schema lost";
    };

    // Kimix strings carry an explicit length, so an embedded NUL must not end
    // the JSON string (yyjson_mut_obj_add_str is strlen-based).
    "embedded_nul_does_not_truncate"_test = [] {
        const Config cfg = make_config();
        kimix::string with_nul;
        with_nul.push_back('a');
        with_nul.push_back('\0');
        with_nul.push_back('b');
        kimix::vector<openai::ChatMessage> messages;
        messages.push_back({"user", with_nul, {}, {}});
        const kimix::string body = openai::build_chat_body(cfg, messages, {});
        expect(!body.empty()) << "NUL content dropped the body";
        expect(is_strict_json(body)) << "NUL body is not valid JSON";
        expect(utf8_ok(body)) << "NUL body is not valid UTF-8";
        expect(contains(body, "a\\u0000b")) << "embedded NUL not preserved";
    };

    // -- the UTF-8 helpers themselves ---------------------------------------
    // Expectations are CPython's bytes.decode("utf-8", errors="replace"): one
    // U+FFFD per maximal subpart, verified case by case against Python 3.
    "utf8_valid_accepts_and_rejects"_test = [] {
        expect(utf8_valid(kimix::string_view("")));
        expect(utf8_valid(kimix::string_view("plain ascii")));
        // U+2014, U+4E2D U+6587, U+1F600
        expect(utf8_valid(kimix::string_view("\xE2\x80\x94")));
        expect(utf8_valid(kimix::string_view("\xE4\xB8\xAD\xE6\x96\x87")));
        expect(utf8_valid(kimix::string_view("\xF0\x9F\x98\x80")));
        expect(!utf8_valid(kimix::string_view("\xE2\x80")));      // truncated
        expect(!utf8_valid(kimix::string_view("\xFF")));          // bad lead
        expect(!utf8_valid(kimix::string_view("\x80")));          // lone cont
        expect(!utf8_valid(kimix::string_view("\xC0\xAF")));      // overlong
        expect(!utf8_valid(kimix::string_view("\xED\xA0\x80")));  // surrogate
        expect(!utf8_valid(kimix::string_view("\xF4\x90\x80\x80"))); // > U+10FFFF
    };

    "utf8_sanitize_matches_python_replace"_test = [] {
        struct Case {
            const char *in;
            const char *out;
        };
        const Case cases[] = {
            {"", ""},
            {"plain", "plain"},
            {"\xE2\x80\x94", "\xE2\x80\x94"},                    // kept
            {"\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80"},             // kept
            {"bad \xE2\x80", "bad \xEF\xBF\xBD"},                  // b'bad \xe2\x80'
            {"\xFF", "\xEF\xBF\xBD"},                              // b'\xff'
            {"a\x80" "b", "a\xEF\xBF\xBD" "b"}, // b'a\x80b'
            {"\xC0\xAF\x41", "\xEF\xBF\xBD\xEF\xBF\xBD\x41"},       // b'\xc0\xafA'
            {"\xE0\x80\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"}, // overlong
            {"\xED\xA0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"}, // surrogate
            {"\xF5\x80\x80\x80",
             "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},   // F5..FF
            {"\xE2\x28\xA1", "\xEF\xBF\xBD\x28\xEF\xBF\xBD"},       // b'\xe2(\xa1'
            {"\xF0\x9F\x98", "\xEF\xBF\xBD"},                       // truncated 4-byte
            {"\xE2\x80\x94x\xE2\x80", "\xE2\x80\x94x\xEF\xBF\xBD"},  // mixed
        };
        for (const Case &c : cases) {
            const kimix::string got = utf8_sanitize(kimix::string_view(c.in));
            expect(got == c.out) << "utf8_sanitize mismatch";
            expect(utf8_valid(got)) << "sanitize output is still invalid";
            // Idempotent: a sanitized string has nothing left to replace.
            expect(utf8_sanitize(got) == got) << "utf8_sanitize not idempotent";
        }
    };

    // One bad byte in a JSON fragment must not lose the schema: the strict
    // reader rejects it and the caller's fallback would be an empty object.
    "json_fragment_is_repaired_not_dropped"_test = [] {
        const Config cfg = make_config();
        kimix::vector<openai::ChatMessage> messages;
        messages.push_back({"user", "hi", {}, {}});
        kimix::vector<openai::Tool> tools;
        openai::Tool t;
        t.name = "bash";
        t.description = "Run a command.";
        // A valid schema whose description string carries a truncated sequence.
        t.parameters_json =
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":"
            "\"string\",\"description\":\"run \xE2\x80 now\"}}}";
        tools.push_back(t);
        const kimix::string body = openai::build_chat_body(cfg, messages, tools);
        expect(!body.empty()) << "body dropped for one bad byte in a schema";
        expect(is_strict_json(body)) << "schema body is not valid JSON";
        expect(utf8_ok(body)) << "schema body still holds invalid UTF-8";
        expect(!contains(body, "\"parameters\":null"))
            << "schema was nulled out instead of repaired";
        expect(contains(body, "\"command\"")) << "parameter name lost";

        // Garbage that is not JSON at all keeps the provider's fallback.
        openai::Tool bad;
        bad.name = "bash";
        bad.parameters_json = "not json at all";
        kimix::vector<openai::Tool> bad_tools;
        bad_tools.push_back(bad);
        const kimix::string bad_body = openai::build_chat_body(cfg, messages, bad_tools);
        expect(!bad_body.empty()) << "bad schema dropped the whole body";
        expect(contains(bad_body, "\"parameters\":null"))
            << "unparseable schema did not fall back to null";
    };

    // A failed build must name its reason instead of dying silently.
    "build_body_reports_the_reason"_test = [] {
        const Config cfg = make_config();
        kimix::vector<openai::ChatMessage> messages;
        messages.push_back({"user", "truncated \xE2\x80", {}, {}});
        kimix::string why("untouched");
        const kimix::string body =
            openai::build_chat_body(cfg, messages, {}, &why);
        // The sanitizer keeps the request alive, so a successful build reports
        // nothing. A failure has to name its reason (see write_json_doc).
        expect(!body.empty()) << "one bad byte still drops the body";
        expect(why == "untouched") << "a successful build set an error";
    };
}
