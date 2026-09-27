// Test for E11 - the KIMI_* environment fallback chain ported from
// kimi_cli/llm.py:275-300 (augment_provider_with_env_vars):
//   KIMI_BASE_URL / KIMI_API_KEY / KIMI_MODEL_NAME fill the empty url /
//   api_key / model fields; KIMI_MODEL_MAX_CONTEXT_SIZE fills a zero
//   max_context_size; KIMI_MODEL_CAPABILITIES replaces the capabilities while
//   they were not configured (comma-separated, trimmed, lowercased, unknown
//   names dropped); KIMI_MODEL_TEMPERATURE / KIMI_MODEL_TOP_P fill the unset
//   (0) sampling controls.
// Every override is skipped when the config field is already set.
//
// The environment is manipulated with a portable setenv/unset helper and every
// touched variable is restored, so the suite leaves the process environment
// exactly as it found it.

#include "ut/ut.hpp"

#include "llm/common.h"
#include "llm/llm.h"

#include <cstdio>
#include <cstdlib>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

void unset_env(const char *name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// All variables this suite may touch; every test starts from a clean slate and
// restores them afterwards.
const char *const kVars[] = {
    "KIMI_BASE_URL",       "KIMI_API_KEY",
    "KIMI_MODEL_NAME",     "KIMI_MODEL_MAX_CONTEXT_SIZE",
    "KIMI_MODEL_CAPABILITIES", "KIMI_MODEL_TEMPERATURE",
    "KIMI_MODEL_TOP_P",
};

void clear_env() {
    for (const char *v : kVars) {
        unset_env(v);
    }
}

Config base_config() {
    Config cfg;
    cfg.type = "anthropic";
    cfg.model = "cfg-model";
    cfg.url = "http://cfg-url";
    cfg.api_key = "cfg-key";
    return cfg;
}

} // namespace

int main() {
    // -- url / api key / model fill only empty fields --------------------------
    "env_fills_empty_fields"_test = [] {
        clear_env();
        Config cfg;
        cfg.type = "anthropic";
        set_env("KIMI_BASE_URL", "http://env-url");
        set_env("KIMI_API_KEY", "env-key");
        set_env("KIMI_MODEL_NAME", "env-model");
        const kimix::vector<kimix::string> applied = apply_env_overrides(cfg);
        expect(cfg.url == "http://env-url");
        expect(cfg.api_key == "env-key");
        expect(cfg.model == "env-model");
        expect(applied.size() == 3u);
        clear_env();
    };

    "env_never_overrides_set_fields"_test = [] {
        clear_env();
        Config cfg = base_config();
        set_env("KIMI_BASE_URL", "http://env-url");
        set_env("KIMI_API_KEY", "env-key");
        set_env("KIMI_MODEL_NAME", "env-model");
        const kimix::vector<kimix::string> applied = apply_env_overrides(cfg);
        expect(cfg.url == "http://cfg-url");
        expect(cfg.api_key == "cfg-key");
        expect(cfg.model == "cfg-model");
        expect(applied.empty());
        clear_env();
    };

    // -- max context size ------------------------------------------------------
    "env_max_context_size_only_when_zero"_test = [] {
        clear_env();
        set_env("KIMI_MODEL_MAX_CONTEXT_SIZE", "8192");
        Config unset_cfg = base_config();
        unset_cfg.max_context_size = 0;
        expect(apply_env_overrides(unset_cfg).size() == 1u);
        expect(unset_cfg.max_context_size == 8192);

        Config set_cfg = base_config();
        set_cfg.max_context_size = 4096;
        expect(apply_env_overrides(set_cfg).empty());
        expect(set_cfg.max_context_size == 4096);
        clear_env();
    };

    // -- capabilities ----------------------------------------------------------
    "env_capabilities_parses_like_the_reference"_test = [] {
        clear_env();
        // kimi-cli tests/core/test_create_llm.py:
        // "Image_In,THINKING,unknown" -> {image_in, thinking}.
        set_env("KIMI_MODEL_CAPABILITIES", "Image_In, THINKING ,unknown");
        Config cfg = base_config();
        const kimix::vector<kimix::string> applied = apply_env_overrides(cfg);
        expect(applied.size() == 1u);
        expect(applied[0] == "KIMI_MODEL_CAPABILITIES");
        expect(cfg.capabilities.image_in);
        expect(cfg.capabilities.thinking);
        expect(!cfg.capabilities.video_in);
        expect(!cfg.capabilities.always_thinking);
        clear_env();
    };

    "env_capabilities_skipped_when_config_provided_them"_test = [] {
        clear_env();
        set_env("KIMI_MODEL_CAPABILITIES", "image_in");
        Config cfg = base_config();
        cfg.capabilities.video_in = true;
        cfg.capabilities_from_config = true;
        expect(apply_env_overrides(cfg).empty());
        expect(cfg.capabilities.image_in == false);
        expect(cfg.capabilities.video_in == true);
        clear_env();
    };

    // -- sampling controls -----------------------------------------------------
    "env_temperature_and_top_p_only_when_unset"_test = [] {
        clear_env();
        set_env("KIMI_MODEL_TEMPERATURE", "0.25");
        set_env("KIMI_MODEL_TOP_P", "0.75");

        Config unset_cfg = base_config();
        expect(apply_env_overrides(unset_cfg).size() == 2u);
        expect(unset_cfg.temperature > 0.24 && unset_cfg.temperature < 0.26);
        expect(unset_cfg.top_p > 0.74 && unset_cfg.top_p < 0.76);

        Config set_cfg = base_config();
        set_cfg.temperature = 0.5;
        set_cfg.top_p = 0.9;
        expect(apply_env_overrides(set_cfg).empty());
        expect(set_cfg.temperature == 0.5);
        expect(set_cfg.top_p == 0.9);
        clear_env();
    };

    // -- the chain runs during config loading ----------------------------------
    "load_config_applies_env_chain"_test = [] {
        clear_env();
        const char *path = "kimix_llm_env_test_config.json";
        FILE *fp = std::fopen(path, "wb");
        expect(fp != nullptr);
        if (fp != nullptr) {
            const char json[] = "{\"type\":\"openai_legacy\",\"max_context_size\":0}";
            std::fwrite(json, 1, sizeof(json) - 1, fp);
            std::fclose(fp);
        }
        set_env("KIMI_BASE_URL", "http://env-url");
        set_env("KIMI_MODEL_NAME", "env-model");
        set_env("KIMI_MODEL_MAX_CONTEXT_SIZE", "2048");
        set_env("KIMI_MODEL_TOP_P", "0.4");
        Config cfg;
        expect(load_config(path, cfg));
        expect(cfg.url == "http://env-url");
        expect(cfg.model == "env-model");
        expect(cfg.max_context_size == 2048);
        expect(cfg.top_p > 0.39 && cfg.top_p < 0.41);
        std::remove(path);
        clear_env();
    };

    // -- create_llm applies the chain too (programmatic configs) ---------------
    "create_llm_applies_env_chain"_test = [] {
        clear_env();
        Config cfg;
        cfg.type = "openai";
        cfg.model = "env-model";
        set_env("KIMI_BASE_URL", "http://env-url");
        kimix::unique_ptr<LLM> llm;
        const CreateLlmError err = create_llm(cfg, llm);
        expect(err == CreateLlmError::none);
        expect(llm != nullptr);
        clear_env();
    };

    return 0;
}
