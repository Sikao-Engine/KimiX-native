// test_capabilities.cpp - Unit tests for the model-capability pre-flight
// (A10): check_message() must refuse, BEFORE the request is sent, a history
// whose parts the provider does not support - today the thinking blocks on
// assistant messages vs the provider's `thinking` capability - with the
// reference's LLMNotSupported wording. Also covers the "capabilities" config
// key parsing and message_required_capabilities.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only: the provider is a fake.

#include "ut/ut.hpp"

#include <llm/llm.h>

#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

struct FakeProvider : kimix::llm::ChatProvider {
    mutable int calls = 0;
    kimix::string model_name() const override { return "fake-model"; }
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &, const kimix::llm::AbortCheck * /*abort*/) const override {
        ++calls;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "ok";
        return r;
    }
};

kimix::unique_ptr<kimix::llm::LLM> make_llm(kimix::llm::Config cfg,
                                            FakeProvider *&provider) {
    provider = new FakeProvider();
    return kimix::unique_ptr<kimix::llm::LLM>(new kimix::llm::LLM(
        kimix::unique_ptr<kimix::llm::ChatProvider>(provider), std::move(cfg)));
}

kimix::llm::Config base_config() {
    kimix::llm::Config cfg;
    cfg.type = "openai";
    cfg.model = "fake-model";
    cfg.url = "http://localhost:9";
    return cfg;
}

} // namespace

int main() {
    using kimix::llm::ChatErrorKind;
    using kimix::llm::ModelCapabilities;

    "thinking_message_requires_thinking_capability"_test = [] {
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.thinking = "let me reason about this";
        messages.push_back(std::move(assistant));
        const ModelCapabilities needed =
            kimix::llm::message_required_capabilities(messages);
        expect(needed.thinking);
        kimix::vector<kimix::llm::Message> plain;
        kimix::llm::Message user;
        user.role = "user";
        user.content = "hello";
        plain.push_back(std::move(user));
        expect(!kimix::llm::message_required_capabilities(plain).thinking);
    };

    "unsupported_thinking_is_refused_before_the_request"_test = [] {
        kimix::llm::Config cfg = base_config();
        cfg.capabilities.thinking = false;
        FakeProvider *provider = nullptr;
        auto llm = make_llm(std::move(cfg), provider);
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.thinking = "reasoning from a previous step";
        messages.push_back(std::move(assistant));
        const kimix::llm::ChatResult r = llm->chat(messages, {});
        expect(!r.ok);
        expect(r.error_kind == ChatErrorKind::not_supported);
        expect(provider->calls == 0); // refused BEFORE sending
        // The reference's LLMNotSupported wording.
        expect(r.error == "LLM model 'fake-model' does not support required "
                          "capability: thinking.");
    };

    "thinking_signature_also_requires_thinking"_test = [] {
        kimix::llm::Config cfg = base_config();
        cfg.capabilities.thinking = false;
        FakeProvider *provider = nullptr;
        auto llm = make_llm(std::move(cfg), provider);
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.thinking_signature = "sig";
        messages.push_back(std::move(assistant));
        const kimix::llm::ChatResult r = llm->chat(messages, {});
        expect(!r.ok);
        expect(r.error_kind == ChatErrorKind::not_supported);
        expect(provider->calls == 0);
    };

    "thinking_capable_provider_passes_the_gate"_test = [] {
        kimix::llm::Config cfg = base_config(); // thinking defaults to true
        FakeProvider *provider = nullptr;
        auto llm = make_llm(std::move(cfg), provider);
        kimix::vector<kimix::llm::Message> messages;
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.content = "answer";
        assistant.thinking = "reasoning";
        messages.push_back(std::move(assistant));
        const kimix::llm::ChatResult r = llm->chat(messages, {});
        expect(r.ok);
        expect(provider->calls == 1);
    };

    "output_token_budget_reaches_the_shared_config"_test = [] {
        // The think-only escalation (LLM::set_output_token_budget) must
        // mutate the very Config the provider builds requests from - the
        // provider and the LLM wrapper share one instance.
        struct ConfigPeekProvider : kimix::llm::ChatProvider {
            kimix::shared_ptr<kimix::llm::Config> cfg;
            explicit ConfigPeekProvider(kimix::shared_ptr<kimix::llm::Config> c)
                : cfg(std::move(c)) {}
            kimix::string model_name() const override { return "peek"; }
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &,
                 const kimix::vector<kimix::llm::Tool> &,
                 const kimix::llm::ChunkCallback &, const kimix::llm::AbortCheck * /*abort*/) const override {
                kimix::llm::ChatResult r;
                r.ok = true;
                r.content = "ok";
                return r;
            }
        };
        auto cfg = kimix::shared_ptr<kimix::llm::Config>(
            new kimix::llm::Config(base_config()));
        ConfigPeekProvider *provider = new ConfigPeekProvider(cfg);
        kimix::unique_ptr<kimix::llm::LLM> llm(new kimix::llm::LLM(
            kimix::unique_ptr<kimix::llm::ChatProvider>(provider), cfg));
        expect(llm->output_token_budget() == 4096_i); // Config default
        llm->set_output_token_budget(12288);
        expect(provider->cfg->max_tokens == 12288);
        expect(llm->output_token_budget() == 12288_i);
    };

    "capabilities_config_key_parsing"_test = [] {
        const kimix::string path = "kimix_capabilities_test_config.json";
        {
            std::FILE *fp = std::fopen(path.c_str(), "wb");
            const char json[] =
                "{\"model\":\"m\",\"url\":\"http://localhost:9\","
                "\"capabilities\":[\"image_in\",\"thinking\"]}";
            std::fwrite(json, 1, sizeof(json) - 1, fp);
            std::fclose(fp);
        }
        kimix::llm::Config cfg;
        expect(kimix::llm::load_config(path, cfg));
        expect(cfg.capabilities.image_in);
        expect(cfg.capabilities.thinking);
        expect(!cfg.capabilities.video_in);
        expect(!cfg.capabilities.always_thinking);
        std::remove(path.c_str());

        // An explicit empty list replaces the thinking-capable default.
        {
            std::FILE *fp = std::fopen(path.c_str(), "wb");
            const char json[] =
                "{\"model\":\"m\",\"url\":\"http://localhost:9\","
                "\"capabilities\":[]}";
            std::fwrite(json, 1, sizeof(json) - 1, fp);
            std::fclose(fp);
        }
        kimix::llm::Config bare;
        expect(kimix::llm::load_config(path, bare));
        expect(!bare.capabilities.thinking);
        std::remove(path.c_str());

        // Absent key keeps the defaults (thinking-capable).
        {
            std::FILE *fp = std::fopen(path.c_str(), "wb");
            const char json[] =
                "{\"model\":\"m\",\"url\":\"http://localhost:9\"}";
            std::fwrite(json, 1, sizeof(json) - 1, fp);
            std::fclose(fp);
        }
        kimix::llm::Config dflt;
        expect(kimix::llm::load_config(path, dflt));
        expect(dflt.capabilities.thinking);
        std::remove(path.c_str());
    };

    return 0;
}
