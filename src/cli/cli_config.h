// cli/cli_config.h - Provider/agent config deserialization for the native CLI.
//
// Port of the config half of kimi-agent's kimix/utils/config.py (the flat kimix
// provider JSON: ds_flash.json, default_config.json) together with the
// structured kimi-cli config (kimi_cli/config.py: model + provider + models
// tables) and the agent manifest loader (kimi_cli/agentspec.py +
// kimi_cli/soul/toolset.py).  One tolerant loader accepts BOTH dialects in one
// document; see src/cli/PLAN.md §3.4 and .kimix_cache/cli_specs/02_config.md.
//
// Rules (see src/cli/PLAN.md and .agents/skills/cpp): namespace kimix::cli,
// kimix:: containers in every public API, no RTTI, no exceptions (failures
// travel through bool + `error` out-parameters), JSON through the vendored
// yyjson with the mimalloc allocator (kimix::llm::kYYJsonAlcMi), unity build
// (every TU-local helper is static / anonymous with the `clicfg_` prefix).

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

#include "llm/common.h" // kimix::llm::Config (to_llm_config result)

namespace kimix::cli {

// One Services.search / Services.fetch endpoint (kimi_cli.config FetchConfig).
struct service_endpoint {
    kimix::string base_url, api_key;
};

// LLMProvider.openai_settings (all three default to true, OpenAI only).
struct openai_settings {
    bool thinking = true, reasoning = true, chat_template_kwargs = true;
};

// Resolved provider config, normalised across the flat kimix and nested
// kimi-cli dialects.  Nothing here exits the process: a failed load returns
// false with `error` set, an unknown-but-tolerable key lands in `warnings`.
struct provider_config {
    kimix::string model;           // "model"
    kimix::string type;            // as written, e.g. "openai_legacy" | "kimi"
    kimix::string provider_family; // "openai" | "openai_responses" | "anthropic"
    kimix::string base_url;        // "base_url" else "url" (base_url wins)
    kimix::string api_key;
    kimix::string reasoning_key;    // default "reasoning_content"
    kimix::string thinking_effort;  // default "high"
    int64_t max_context_size = 0;   // resolved (explicit > model defaults > 0)
    int64_t max_tokens = 0;         // resolved (explicit > model default > ctx/4)
    bool show_thinking_stream = true;
    bool max_context_size_explicit = false;
    bool max_tokens_explicit = false;
    kimix::vector<kimix::string> capabilities; // image_in/video_in/thinking/...
    kimix::vector<std::pair<kimix::string, kimix::string>> custom_headers;
    kimix::vector<std::pair<kimix::string, kimix::string>> env;
    openai_settings openai;
    service_endpoint search, fetch;
    bool has_oauth = false;
    kimix::string oauth_storage, oauth_key;
    kimix::string source_path;
    kimix::vector<kimix::string> warnings; // unknown keys / derived values / dropped
};

// Resolved agent manifest (agent_worker.json + friends).  `tools` is the raw
// requested list; `enabled_tools` holds the resolved registry names in order.
struct agent_config {
    kimix::string name, extend, manifest_path, manifest_dir;
    kimix::string system_prompt_path; // resolved against manifest_dir when relative
    kimix::vector<std::pair<kimix::string, kimix::string>> system_prompt_args;
    kimix::string model, when_to_use; // "" == inherit
    bool has_tools = false;
    kimix::vector<kimix::string> tools, allowed_tools, exclude_tools, subagents;
    kimix::vector<kimix::string> enabled_tools; // resolved registry names, in order
    kimix::vector<kimix::string> warnings;
};

// Load + validate a provider config (both dialects).  Returns false with
// `error` set on: unreadable/invalid JSON, a missing required field
// (model / base_url|url / type), an unsupported provider type, or an unknown
// model with no explicit max_context_size.  On success the manifest's `env`
// entries are applied to the process environment.  Never calls exit().
bool load_provider_config(const kimix::string &path, provider_config &out, kimix::string &error);

// Load an agent manifest ({"agent": {...}} or a bare top-level agent object).
// Returns false with `error` set on unreadable/invalid JSON, a non-object root,
// a non-empty unsupported `version`, or a missing "agent" wrapper with none of
// the recognised keys.  Unknown tool paths are warned about and dropped, never
// fatal.  `system_prompt_args` stays a raw pair list (S5 substitutes ${name}).
bool load_agent_config(const kimix::string &path, agent_config &out, kimix::string &error);

// Port of _resolve_model_defaults: tokenise `model_name` and return the first
// matching _MODEL_DEFAULTS row.  `max_output` is 0 when the row's default output
// is None (e.g. grok).  Returns false when no row matches.
bool resolve_model_defaults(kimix::string_view model_name, int64_t &max_context_size,
                            int64_t &max_output);

// Build the unified LLM config: type = the normalised provider family + the
// resolved token limits.
kimix::llm::Config to_llm_config(const provider_config &p);

// Deterministic --dry-run text.  Never prints an api_key value (present/absent
// only).  provider_report covers the provider section; agent_report the agent
// section; the caller prints both and a final "OK" line.
kimix::string provider_report(const provider_config &p);
kimix::string agent_report(const agent_config &a);

} // namespace kimix::cli
