// Test for the unified LLM interface (llm/llm.h + llm/llm.cpp).
// Covers: create_llm provider dispatch (openai/openai_legacy/openai_responses/
// anthropic), unknown-type and missing model/url null returns,
// max_context_size passthrough, and create_llm_from_file config loading.
//
// Also contains one real e2e tool-calling test against DeepSeek. The backend
// config path is supplied at run time with --config=<path> (e.g.
// --config=C:/dev/backup_ds_flash.json); it drives all three providers
// (openai_legacy + openai_responses on https://api.deepseek.com, anthropic on
// https://api.deepseek.com/anthropic) with dummy tools and asserts that the
// single streamed result contains a reasoning block, a text block, and a tool
// calling block.

#include "ut/ut.hpp"

#include "llm/llm.h"

#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::llm;

namespace {

// Build a Config with the given type/model/url (other fields keep defaults).
Config make_config(const char *type, const char *model, const char *url) {
    Config cfg;
    cfg.type = type;
    cfg.model = model;
    cfg.url = url;
    return cfg;
}

// Write a small JSON config file for create_llm_from_file.
kimix::string write_temp_config() {
    const kimix::string path = "kimix_llm_test_config.json";
    FILE *fp = std::fopen(path.c_str(), "wb");
    if (fp) {
        const char json[] =
            "{\"model\":\"m\",\"url\":\"http://localhost:9\",\"type\":\"anthropic\",\"api_key\":\"k\"}";
        std::fwrite(json, 1, sizeof(json) - 1, fp);
        std::fclose(fp);
    }
    return path;
}

void remove_temp_config(const kimix::string &path) {
    std::remove(path.c_str());
}

// Build the dummy tool set used by the real e2e tool-calling test. Each tool
// carries a tiny JSON schema so every provider accepts it unchanged.
kimix::vector<Tool> make_dummy_tools() {
    kimix::vector<Tool> tools;
    tools.push_back({"get_weather",
                     "Get the current weather for a city.",
                     "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}"});
    tools.push_back({"get_current_time",
                     "Get the current time for an IANA timezone.",
                     "{\"type\":\"object\",\"properties\":{\"timezone\":{\"type\":\"string\"}},\"required\":[\"timezone\"]}"});
    tools.push_back({"search_knowledge",
                     "Search a knowledge base for a query.",
                     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}"});
    return tools;
}

// Build the prompt that forces the backend to emit reasoning + text + tool
// calls in one streamed response.
kimix::vector<Message> make_tool_calling_messages() {
    kimix::vector<Message> messages;
    messages.push_back({"system",
                        "You are a helpful assistant with access to tools. "
                        "Always reason step by step before acting, then call "
                        "exactly the tools the user asks for, then write a "
                        "short text summary of what you did.",
                        "", {}, "", ""});
    messages.push_back({"user",
                        "Call the get_weather tool for Beijing, the "
                        "get_current_time tool for UTC, and the "
                        "search_knowledge tool for the query 'kimix'. First "
                        "show your reasoning. Then, before the tool calls, "
                        "write one short sentence of visible text. Then call "
                        "all three tools at once. Do not wait for tool "
                        "results.",
                        "", {}, "", ""});
    return messages;
}

// Print one e2e ChatResult (bounded to keep test output readable).
void print_e2e_result(const char *label, const ChatResult &r) {
    std::printf("--- e2e %s ---\n", label);
    if (!r.ok) {
        std::printf("error: %s\n", r.error.c_str());
        return;
    }
    const auto block = [](const char *tag, const kimix::string &text) {
        const size_t n = text.size() < 300 ? text.size() : 300;
        std::printf("[%s] %.*s%s\n", tag, (int)n, text.data(),
                    text.size() > 300 ? "..." : "");
    };
    block("reasoning", r.reasoning);
    block("text", r.content);
    for (const auto &tc : r.tool_calls) {
        std::printf("[tool_call] id=%s name=%s args=%s\n", tc.id.c_str(),
                    tc.name.c_str(), tc.arguments.c_str());
    }
    std::printf("usage: prompt=%lld completion=%lld cached=%lld total=%lld\n",
                (long long)r.prompt_tokens, (long long)r.completion_tokens,
                (long long)r.cached_tokens, (long long)r.total_tokens);
}

} // namespace

int main(int argc, char *argv[]) {
    // Read the real-backend config path from the command line before Boost.UT
    // parses argv (Boost.UT rejects unknown --options once it has seen a known
    // one, so --config must be removed from the argument list it sees).
    kimix::string config_path;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const char *arg = argv[i];
        if (std::strncmp(arg, "--config=", 9) == 0) {
            config_path = arg + 9;
            argv[i] = nullptr;
        } else if (std::strcmp(arg, "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            argv[i] = nullptr;
            argv[i + 1] = nullptr;
            ++i; // skip the value in the next iteration
        }
    }
    int filtered_argc = 1;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr) {
            argv[filtered_argc++] = argv[i];
        }
    }

    boost::ut::detail::cfg::parse_arg_with_fallback(
        filtered_argc, const_cast<const char **>(argv));

    "llm_create_openai"_test = [] {
        auto llm = create_llm(make_config("openai", "m", "http://localhost:9"));
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "m");
        }
    };

    "llm_create_openai_legacy"_test = [] {
        auto llm = create_llm(make_config("openai_legacy", "m", "http://localhost:9"));
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "m");
        }
    };

    "llm_create_openai_responses"_test = [] {
        auto llm = create_llm(make_config("openai_responses", "m", "http://localhost:9"));
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "m");
        }
    };

    "llm_create_anthropic"_test = [] {
        auto llm = create_llm(make_config("anthropic", "m", "http://localhost:9"));
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "m");
        }
    };

    "llm_create_unknown_type"_test = [] {
        auto llm = create_llm(make_config("bogus", "m", "http://localhost:9"));
        expect(llm == nullptr);
    };

    "llm_create_missing_model"_test = [] {
        auto llm = create_llm(make_config("openai", "", "http://localhost:9"));
        expect(llm == nullptr);
    };

    "llm_create_missing_url"_test = [] {
        auto llm = create_llm(make_config("openai", "m", ""));
        expect(llm == nullptr);
    };

    "llm_max_context_size_passthrough"_test = [] {
        Config cfg = make_config("openai", "m", "http://localhost:9");
        cfg.max_context_size = 123;
        auto llm = create_llm(cfg);
        expect(llm != nullptr);
        if (llm) {
            expect(eq(llm->max_context_size(), 123));
        }
    };

    "llm_create_from_file"_test = [] {
        const kimix::string path = write_temp_config();
        auto llm = create_llm_from_file(path);
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "m");
        }
        remove_temp_config(path);
    };

    "llm_repairs_backend_tool_call_json"_test = [] {
        // A fake provider that returns ChatResult tool calls whose arguments
        // are the hallucinated JSON a real backend may emit.
        struct FakeProvider : ChatProvider {
            kimix::string model_name() const override { return "fake"; }
            ChatResult chat(const kimix::vector<Message> &,
                            const kimix::vector<Tool> &,
                            const ChunkCallback &) const override {
                ChatResult r;
                r.ok = true;
                r.content = "ok";
                ToolCall t;
                t.name = "broken_trailing_comma";
                t.arguments = "{\"city\": \"Beijing\",}";
                r.tool_calls.push_back(std::move(t));
                t = ToolCall{};
                t.name = "broken_truncated";
                t.arguments = "{\"city\": \"Beijing\"";
                r.tool_calls.push_back(std::move(t));
                t = ToolCall{};
                t.name = "broken_quotes";
                t.arguments = "{city: 'Beijing'}";
                r.tool_calls.push_back(std::move(t));
                t = ToolCall{};
                t.name = "valid";
                t.arguments = "{\"tz\": \"UTC\"}"; // must stay byte-identical
                r.tool_calls.push_back(std::move(t));
                t = ToolCall{};
                t.name = "empty";
                t.arguments = ""; // must stay empty, not become "null"
                r.tool_calls.push_back(std::move(t));
                t = ToolCall{};
                t.name = "prose";
                t.arguments = "hello world"; // not JSON-looking -> left alone
                r.tool_calls.push_back(std::move(t));
                return r;
            }
        };

        auto llm = kimix::unique_ptr<LLM>(new LLM(
            kimix::unique_ptr<ChatProvider>(new FakeProvider),
            make_config("openai", "m", "http://localhost:9")));
        const ChatResult r = llm->chat({}, {});
        expect(r.ok);
        expect(r.tool_calls.size() == 6u);
        if (r.tool_calls.size() == 6u) {
            expect(r.tool_calls[0].arguments == "{\"city\":\"Beijing\"}");
            expect(r.tool_calls[1].arguments == "{\"city\":\"Beijing\"}");
            expect(r.tool_calls[2].arguments == "{\"city\":\"Beijing\"}");
            expect(r.tool_calls[3].arguments == "{\"tz\": \"UTC\"}");
            expect(r.tool_calls[4].arguments.empty());
            expect(r.tool_calls[5].arguments == "hello world");
        }
    };

    // Real e2e tool-calling test against DeepSeek. The config path comes from
    // --config=<path>; without it (or when the file is missing) the test is
    // skipped with a notice, so the rest of the suite still runs in
    // environments without the backend credentials. Each provider is driven
    // with the same dummy tools + prompt; a passing run must produce reasoning
    // + text + tool calls in the single streamed result.
    "llm_e2e_tool_calling_real_backend"_test = [config_path] {
        if (config_path.empty()) {
            std::printf("SKIPPED llm_e2e_tool_calling_real_backend: pass "
                        "--config=<path> (e.g. "
                        "--config=C:/dev/backup_ds_flash.json)\n");
            return;
        }

        Config base_cfg;
        if (!load_config(config_path, base_cfg)) {
            std::printf("SKIPPED llm_e2e_tool_calling_real_backend: cannot "
                        "load %s\n",
                        config_path.c_str());
            return;
        }

        const kimix::vector<Tool> tools = make_dummy_tools();
        const kimix::vector<Message> messages = make_tool_calling_messages();

        struct ProviderCase {
            const char *type;
            const char *url;
            const char *label;
        };
        const ProviderCase cases[] = {
            {"openai_legacy", "https://api.deepseek.com", "openai legacy"},
            {"openai_responses", "https://api.deepseek.com", "openai responses"},
            {"anthropic", "https://api.deepseek.com/anthropic", "anthropic"},
        };

        for (const auto &c : cases) {
            Config cfg = base_cfg;
            cfg.type = c.type;
            cfg.url = c.url;

            auto llm = create_llm(std::move(cfg));
            if (!llm) {
                expect(false) << "create_llm(" << c.type << ") returned null";
                continue;
            }

            const ChatResult r = llm->chat(messages, tools);
            print_e2e_result(c.label, r);

            expect(r.ok) << c.label << " request ok, error=" << r.error;
            if (!r.ok) {
                continue;
            }
            expect(!r.reasoning.empty()) << c.label
                                         << " contains reasoning block";
            expect(!r.content.empty()) << c.label << " contains text block";
            expect(!r.tool_calls.empty()) << c.label
                                          << " contains tool calling block";
            for (const auto &tc : r.tool_calls) {
                expect(!tc.name.empty()) << c.label << " tool call has a name";
            }
        }
    };

    return 0;
}
