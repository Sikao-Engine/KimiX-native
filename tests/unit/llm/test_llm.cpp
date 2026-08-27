// Test for the unified LLM interface (llm/llm.h + llm/llm.cpp).
// Covers: create_llm provider dispatch (openai/openai_legacy/openai_responses/
// anthropic), unknown-type and missing model/url null returns,
// max_context_size passthrough, and create_llm_from_file config loading.

#include "ut/ut.hpp"

#include "llm/llm.h"

#include <cstdio>

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

    return 0;
}
