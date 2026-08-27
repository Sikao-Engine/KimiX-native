// common.h - Shared helpers for the LLM provider libraries (OpenAI Chat,
// OpenAI Responses, Anthropic). The config loader, URL splitter, retry-status
// check and path joining were duplicated verbatim in each provider's chat .cpp;
// they now live here in namespace kimix::llm.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::llm {

// Unified LLM backend config loaded from a JSON file (e.g. C:/dev/ds_flash.json).
struct Config {
    kimix::string model;
    kimix::string url; // e.g. http://host:port/llmproxy
    kimix::string api_key;
    kimix::string type;
    kimix::string thinking_effort = "high";
    int32_t max_tokens = 4096;
    int32_t max_context_size = 0;
    bool show_thinking_stream = true;
};

// Load and validate an LLM config from a JSON file (model + url non-empty).
bool load_config(const kimix::string &path, Config &cfg);

// A parsed URL: scheme / host[:port] / path prefix.
struct Endpoint {
    kimix::string scheme;
    kimix::string host;
    int32_t port = 0;
    kimix::string path_prefix;
};

// Split a config URL into scheme / host[:port] / path prefix.
// Defaults: http -> port 80, https -> port 443.
Endpoint parse_endpoint(const kimix::string &url);

// True for transient HTTP statuses worth retrying (403/408/429/5xx).
bool is_retriable_status(int32_t status);

// Append rel to prefix, ensuring exactly one '/' separator between them.
kimix::string join_path(const kimix::string &prefix, const kimix::string &rel);

} // namespace kimix::llm
