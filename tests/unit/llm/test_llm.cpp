// Test for the unified LLM interface (llm/llm.h + llm/llm.cpp).
// Covers: create_llm provider dispatch (openai/openai_legacy/openai_responses/
// anthropic), unknown-type and missing model/url null returns,
// max_context_size passthrough, and create_llm_from_file config loading.

#include "ut/ut.hpp"

#include "llm/llm.h"

#include <cstdio>
#include <cstdlib>

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

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

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
        // The KIMI_* env chain can fill an empty model; make sure this test
        // exercises the missing-model path regardless of the host environment.
#ifdef _WIN32
        _putenv_s("KIMI_MODEL_NAME", "");
        _putenv_s("KIMI_BASE_URL", "");
#else
        ::unsetenv("KIMI_MODEL_NAME");
        ::unsetenv("KIMI_BASE_URL");
#endif
        auto llm = create_llm(make_config("openai", "", "http://localhost:9"));
        expect(llm == nullptr);
    };

    "llm_create_missing_url"_test = [] {
        auto llm = create_llm(make_config("openai", "m", ""));
        expect(llm == nullptr);
    };

    // A11: typed create_llm failures carry the reference wordings as data.
    "llm_create_typed_missing_model_or_url_wording"_test = [] {
        kimix::unique_ptr<LLM> out;
        kimix::string error;
        const CreateLlmError err =
            create_llm(make_config("openai_legacy", "", ""), out, &error);
        expect(err == CreateLlmError::llm_not_set);
        expect(out == nullptr);
        // kimi_cli/soul/__init__.py LLMNotSet: "LLM not set".
        expect(error == "LLM not set");
    };

    "llm_create_typed_unknown_provider_type_wording"_test = [] {
        kimix::unique_ptr<LLM> out;
        kimix::string error;
        const CreateLlmError err = create_llm(
            make_config("bogus", "m", "http://localhost:9"), out, &error);
        expect(err == CreateLlmError::unknown_provider_type);
        expect(out == nullptr);
        expect(error == "LLM not set (unknown provider type 'bogus')");
    };

    "llm_create_typed_success_returns_none_error"_test = [] {
        kimix::unique_ptr<LLM> out;
        kimix::string error = "keep";
        const CreateLlmError err =
            create_llm(make_config("anthropic", "m", "http://localhost:9"), out, &error);
        expect(err == CreateLlmError::none);
        expect(out != nullptr);
        expect(error == "keep") << "error untouched on success";
        expect(create_llm_error_text(CreateLlmError::none) == "LLM not set")
            << "unused for none, but stays the reference base wording";
    };

    "llm_create_typed_out_reset_on_failure"_test = [] {
        // A non-null `out` must not survive a failed call.
        kimix::unique_ptr<LLM> out = create_llm(make_config("openai", "m", "http://localhost:9"));
        expect(out != nullptr);
        const CreateLlmError err = create_llm(make_config("openai", "", ""), out);
        expect(err == CreateLlmError::llm_not_set);
        expect(out == nullptr);
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
                            const ChunkCallback &, const kimix::llm::AbortCheck * /*abort*/) const override {
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
            t.arguments = ""; // reset to "{}" (history must stay strict-valid)
            t.name = "prose";
            t.arguments = "hello world"; // not JSON-looking -> reset to "{}"
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
          // Empty / non-JSON arguments are reset to "{}" so the persisted
          // history is always strict-valid JSON when echoed back to strict
          // backends (e.g. scnet/Qwen 400s on invalid tool-call arguments).
          // The soul's execute_tool_call treats "{}" exactly like "".
          expect(r.tool_calls[4].arguments == "{}");
          expect(r.tool_calls[5].arguments == "{}");
      }
  };

    return 0;
}
