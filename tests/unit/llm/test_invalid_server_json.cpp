// Test for invalid-JSON handling when the remote server returns garbage.
//
// Regression context: a glitching backend can answer a *successful* (200)
// request with a body that is not valid JSON — truncated mid-object, plain
// prose, an HTML error page, or SSE events that never parse. The SSE parsers
// already skip individual malformed events; the danger is a response where
// NOTHING parses: the providers used to report result.ok = true with empty
// content, and the soul then treated the turn as a successful empty reply.
//
// Contract under test:
//   - a malformed data line amid healthy SSE events is skipped, the response
//     still completes normally (parser tolerance);
//   - a 200 body from which no usable event ever parses yields
//     result.ok == false (never a silent empty success), and the request is
//     retried before giving up;
// - LLM::chat additionally guards against stub/3rd-party providers handing
//   back an ok-but-empty result.
//
// It also pins the *empty content block* corner case that the Python soul fixes
// with kimi_cli/soul/stream_filter.py: some OpenAI-compatible backends (e.g.
// scnet/Qwen in thinking mode) interleave empty ``reasoning_content`` / text /
// tool-call-argument deltas between real deltas. kosong needed a stream filter
// there because its single-``pending_part`` merge chain force-flushes on an
// empty part, truncating or dropping tool-call arguments (arguments arriving as
// "{" and then cut off). This port's accumulator is index-keyed and appends
// fragments, so an empty delta must stay a no-op --
// ``empty_deltas_do_not_truncate_tool_arguments`` proves the arguments survive.

#include "ut/ut.hpp"

#include <httplib.h>

#include "llm/llm.h"
#include "llm/openai/sse_parser.h"

#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::llm;

namespace {

// Run a one-shot httplib server on an ephemeral localhost port; handler
// responds to every request with the given status/body/content-type.
struct OneShotServer {
    httplib::Server svr;
    int port = 0;

    OneShotServer(int status, const std::string &body, const char *content_type) {
        svr.Get(".*", [status, body, content_type](const httplib::Request &, httplib::Response &res) {
            res.status = status;
            res.set_content(body, content_type);
        });
        svr.Post(".*", [status, body, content_type](const httplib::Request &, httplib::Response &res) {
            res.status = status;
            res.set_content(body, content_type);
        });
        port = svr.bind_to_any_port("127.0.0.1");
    }

    kimix::string url() const {
        return kimix::string("http://127.0.0.1:") + std::to_string(port).c_str();
    }

    // Run listen on a background thread; stop after the test body finishes.
    template <typename F>
    void run(F &&body) {
        std::thread t([this] { svr.listen_after_bind(); });
        body();
        svr.stop();
        t.join();
    }
};

Config make_cfg(const kimix::string &type, const kimix::string &url) {
    Config cfg;
    cfg.type = type;
    cfg.model = "m";
    cfg.url = url;
    cfg.api_key = "k";
    return cfg;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "sse_parser_skips_malformed_line_and_recovers"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n"
            "data: {invalid json\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo\"},\"finish_reason\":null}]}\n\n"
            "data: [DONE]\n\n";
        const auto chunks = parser.feed(sse.data(), sse.size());
        expect(eq(chunks.size(), 4u));
        expect(chunks[0].ok);
        expect(chunks[0].content == "Hel");
        expect(!chunks[1].ok); // malformed event dropped
        expect(chunks[2].ok);
        expect(chunks[2].content == "lo");
        expect(chunks[3].done);
    };

    "openai_garbage_200_body_is_not_ok"_test = [] {
        OneShotServer server(200, "this is not json at all", "application/json");
        server.run([&] {
            auto llm = create_llm(make_cfg("openai_legacy", server.url()));
            expect(llm != nullptr);
            if (llm) {
                const ChatResult r = llm->chat({}, {});
                expect(!r.ok) << "garbage 200 body must not be a silent success";
                expect(!r.error.empty());
            }
        });
    };

    "openai_html_error_page_200_is_not_ok"_test = [] {
        OneShotServer server(200, "<html>Bad Gateway</html>", "text/html");
        server.run([&] {
            auto llm = create_llm(make_cfg("openai_legacy", server.url()));
            expect(llm != nullptr);
            if (llm) {
                const ChatResult r = llm->chat({}, {});
                expect(!r.ok) << "HTML 200 body must not be a silent success";
            }
        });
    };

    "anthropic_garbage_200_body_is_not_ok"_test = [] {
        OneShotServer server(200, "this is not json at all", "application/json");
        server.run([&] {
            auto llm = create_llm(make_cfg("anthropic", server.url()));
            expect(llm != nullptr);
            if (llm) {
                const ChatResult r = llm->chat({}, {});
                expect(!r.ok) << "garbage 200 body must not be a silent success";
            }
        });
    };

    "openai_mixed_garbage_sse_still_recovers"_test = [] {
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"Hi\"},\"finish_reason\":null}]}\n\n"
            "data: {broken\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1,\"total_tokens\":4}}\n\n"
            "data: [DONE]\n\n";
        OneShotServer server(200, sse, "text/event-stream");
        server.run([&] {
            auto llm = create_llm(make_cfg("openai_legacy", server.url()));
            expect(llm != nullptr);
            if (llm) {
                const ChatResult r = llm->chat({}, {});
                expect(r.ok) << "one malformed event must not kill a healthy stream";
                expect(r.content == "Hi");
            }
        });
    };

        "empty_deltas_do_not_truncate_tool_arguments"_test = [] {
            // Corner case fixed in the Python soul
            // (kimi_cli/soul/stream_filter.py, see this file's header): backends
            // such as scnet/Qwen in thinking mode interleave present-but-empty
            // reasoning_content / content deltas between the real deltas. The
            // reference needed a stream filter because kosong's merge chain
            // force-flushes on an empty part, truncating tool-call arguments
            // (arguments arriving as "{" and then cut off) or dropping them.
            // This port's accumulator is index-keyed and concatenates fragments,
            // so an empty delta must stay a no-op.
            const std::string sse =
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\",\"reasoning_content\":\"Plan\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"\",\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"city\\\"\"}}]},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\",\"reasoning_content\":\"\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\":\\\"Paris\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\",\"reasoning_content\":\"\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                "data: [DONE]\n\n";
            OneShotServer server(200, sse, "text/event-stream");
            server.run([&] {
                auto llm = create_llm(make_cfg("openai_legacy", server.url()));
                expect(llm != nullptr);
                if (llm) {
                    const ChatResult r = llm->chat({}, {});
                    expect(r.ok) << r.error;
                    // The reasoning stream is reassembled without the empty deltas.
                    expect(r.reasoning == "Plan") << r.reasoning;
                    expect(r.content.empty()) << r.content;
                    expect(eq(r.tool_calls.size(), 1u));
                    if (!r.tool_calls.empty()) {
                        expect(r.tool_calls[0].id == "call_1");
                        expect(r.tool_calls[0].name == "get_weather");
                        expect(r.tool_calls[0].arguments == "{\"city\":\"Paris\"}")
                            << r.tool_calls[0].arguments;
                    }
                }
            });
        };

        "llm_chat_guards_ok_but_empty_result"_test = [] {
            // A stub provider (or a future provider without the guard) handing
            // back ok=true with nothing in it must not reach the caller as a
            // successful empty message.
        struct EmptyProvider : ChatProvider {
            kimix::string model_name() const override { return "stub"; }
            ChatResult chat(const kimix::vector<Message> &,
                            const kimix::vector<Tool> &,
                            const ChunkCallback &) const override {
                ChatResult r;
                r.ok = true; // everything else empty
                return r;
            }
        };
        LLM llm(kimix::unique_ptr<ChatProvider>(new EmptyProvider()), Config{});
        const ChatResult r = llm.chat({}, {});
        expect(!r.ok) << "ok-but-empty result must be flagged as an error";
        expect(!r.error.empty());
    };
}
