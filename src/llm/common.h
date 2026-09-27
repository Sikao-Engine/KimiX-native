// common.h - Shared helpers for the LLM provider libraries (OpenAI Chat,
// OpenAI Responses, Anthropic). The config loader, URL splitter, retry-status
// check and path joining were duplicated verbatim in each provider's chat .cpp;
// they now live here in namespace kimix::llm.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "yyjson.h"

namespace kimix::llm {

// ---------------------------------------------------------------------------
// E1/E2: one typed content part of a message
// (kosong/message.py ContentPart registry + kimi_cli/wire/types.py parts)
// ---------------------------------------------------------------------------
// The reference message content is a LIST of typed parts (TextPart, ThinkPart,
// ImageURLPart, AudioURLPart, VideoURLPart). The native kimix::llm::Message
// keeps `content` as the concatenated TEXT backbone (every existing consumer -
// session store, pruning, export, prompts - keeps working unchanged) and adds
// a `parts` list ONLY when non-text parts exist.
struct ContentPart {
    enum class Kind : uint8_t {
        text = 0,
        think = 1,
        image_url = 2,
        audio_url = 3,
        video_url = 4,
    };

    Kind kind = Kind::text;
    // TextPart.text / ThinkPart.think. Unused for the media kinds.
    kimix::string text;
    // The URL of a media part (ImageURLPart.image_url.url and friends); may be
    // a "data:<mime>;base64,..." URI.
    kimix::string url;
    // OpenAI Responses "input_image" detail hint ("auto"/"low"/"high"). Empty
    // == the provider default (the Responses message path serializes "auto",
    // mirroring _content_parts_to_input_items).
    kimix::string detail;
};

// Model capability flags a provider may or may not support
// (kimi_cli/llm.py ModelCapability). Part of the capability pre-flight gate
// checked BEFORE a request is sent: message parts the model cannot consume
// (thinking blocks today; image/video parts arrive with the Phase-5 media
// message model) must fail with the reference's LLMNotSupported wording
// instead of a provider 400.
struct ModelCapabilities {
    bool image_in = false;       // ImageURLPart
    bool video_in = false;       // VideoURLPart
    bool thinking = true;        // ThinkPart (default mirrors the reference's
                                 // thinking-capable model defaults)
    bool always_thinking = false;
};

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
    ModelCapabilities capabilities; // from the config "capabilities" key
    // True when `capabilities` was read from the config (or filled
    // programmatically); gates the KIMI_MODEL_CAPABILITIES env fallback, which
    // only applies while the field is unset (kimi_cli/llm.py:287).
    bool capabilities_from_config = false;
    // E7 thinking control: first-class OFF switch mapped to the CLI's
    // --no-think/--no-thinking flag (kimi_cli/llm.py create_llm thinking=False
    // -> with_thinking("off")). Thinking stays on while this is true AND
    // thinking_effort != "off" (see thinking_enabled() below).
    bool enable_thinking = true;
    // E5 anthropic prompt caching: place cache_control = {"type":"ephemeral"}
    // on the system block, the last content block of the serialized
    // conversation and the last tool definition. The reference
    // (kosong/contrib/chat_provider/anthropic.py generate()) applies it
    // unconditionally, so the default mirrors that; the field exists so tests
    // (and a future config key) can turn it off.
    bool anthropic_cache_control = true;
    // Sampling controls (kimi_cli/config.py LLMModel.temperature/top_p, both
    // default None there). 0 means "unset" - the field is then not serialized
    // and stays eligible for the KIMI_MODEL_TEMPERATURE / KIMI_MODEL_TOP_P
    // env fallbacks.
    double temperature = 0.0;
    double top_p = 0.0;
    // Kimi provider (llm/kimi/kimi_chat.cpp, kosong/chat_provider/kimi.py):
    // the Moonshot-specific ``thinking.keep`` switch for preserved thinking
    // (empty == absent). When "all" (and thinking is not disabled), every
    // assistant message must carry a ``reasoning_content`` field on the wire
    // (Kimi._convert_message's preserved_thinking_enabled backfill). Set from
    // the KIMI_MODEL_THINKING_KEEP env var (kimi_cli/llm.py:868-874, applied
    // only while thinking is on) or programmatically.
    kimix::string thinking_keep;
    // Kimi provider: the session id mapped to the top-level
    // ``prompt_cache_key`` request field (kimi_cli/llm.py kimi branch:
    // `gen_kwargs["prompt_cache_key"] = session_id`). Empty == omitted.
    kimix::string prompt_cache_key;
    // A7: the credential re-arm seam behind LLMBackend::refresh_auth() (the
    // reference's oauth.ensure_fresh(force=True) branch of
    // _run_with_connection_recovery). Unset by default - plain API-key
    // providers have nothing to refresh, and no real OAuth flow is
    // implemented; a host installs the callback when its provider uses
    // OAuth. True == the credentials were refreshed and the failed step is
    // retried once more outside the retry budget.
    kimix::function<bool()> auth_refresh;
};

// True when thinking mode is ON for `cfg`: the enable flag AND an effort that
// is not "off" (kimi_cli/llm.py LEGAL_THINKING_EFFORT includes "off", which
// with_thinking maps to thinking-off). Providers use this to decide between
// the thinking-enabled wire shape and the disabled one.
bool thinking_enabled(const Config &cfg) noexcept;

// ---------------------------------------------------------------------------
// KIMI_* environment fallback chain (kimi_cli/llm.py:275-300
// augment_provider_with_env_vars, kimi branch)
// ---------------------------------------------------------------------------
// Each override applies ONLY while the config field is still empty/zero:
//   KIMI_BASE_URL               -> url            (when empty)
//   KIMI_API_KEY                -> api_key        (when empty)
//   KIMI_MODEL_NAME             -> model          (when empty)
//   KIMI_MODEL_MAX_CONTEXT_SIZE -> max_context_size (when 0)
//   KIMI_MODEL_CAPABILITIES     -> capabilities   (when not from config);
//                                  comma-separated, trimmed, lowercased,
//                                  unknown names dropped ("Image_In,THINKING,"
//                                  "unknown" -> {image_in, thinking})
//   KIMI_MODEL_TEMPERATURE      -> temperature    (when 0)
//   KIMI_MODEL_TOP_P            -> top_p          (when 0)
// Returns the names of the variables that were applied (the reference returns
// the same `applied` mapping).
kimix::vector<kimix::string> apply_env_overrides(Config &cfg);

// Load and validate an LLM config from a JSON file (model + url non-empty).
bool load_config(const kimix::string &path, Config &cfg);

// Transport-level error category carried on each provider's ChatResult and
// mapped onto the unified ChatErrorKind by the LLM adapters (llm.cpp).
enum class TransportErrorKind : uint8_t {
    none = 0,
    connection,    // no HTTP response at all
    timeout,       // connect/read timeout
    empty_response,// 200 body with no usable content
    http,          // non-200 status (error_status carries it)
};

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

// ---------------------------------------------------------------------------
// Retry backoff policy (kimisoul.py _RateLimitAwareWait, :247-287)
// ---------------------------------------------------------------------------
// Shared by the three provider transport loops and the agent-level step retry
// policy (agent/step_retry.*), so every layer sleeps with the same schedule:
// exponential backoff + jitter capped at 5s; HTTP 429 -> Retry-After honoured
// (capped at 60s) or exponential 1s..30s.

struct RateLimitWaitParams {
    double default_initial = 0.3;   // seconds
    double default_max = 5.0;       // seconds
    double default_jitter = 0.5;    // uniform(0, jitter) seconds
    double rate_limit_initial = 1.0;// seconds
    double rate_limit_max = 30.0;   // seconds
    double rate_limit_jitter = 1.0; // uniform(0, jitter) seconds
    double max_retry_after = 60.0;  // seconds
};

// The wait before retrying after `failed_attempt` (1-based, tenacity's
// attempt_number): min(initial * 2^(n-1) + jitter, max), with the 429 branch
// honouring retry_after when present. `jitter_rng` is a mutable xorshift state
// (seed it for deterministic waits in tests).
double rate_limit_aware_wait(int32_t failed_attempt, int32_t status,
                             double retry_after_seconds, uint64_t &jitter_rng,
                             const RateLimitWaitParams &p = {}) noexcept;

// Parse a Retry-After header value (delta-seconds form) into seconds;
// returns 0 for empty/unparseable input. HTTP-date form is not implemented
// (the reference treats a missing value as "no hint" as well).
double parse_retry_after_seconds(kimix::string_view header) noexcept;

// ---------------------------------------------------------------------------
// Model capability pre-flight (kimi_cli/llm.py ModelCapability,
// soul/message.py check_message)
// ---------------------------------------------------------------------------

// The reference's LLMNotSupported wording (soul/__init__.py:40-48):
// "LLM model '<model>' does not support required capability: thinking."
kimix::string capability_error_text(kimix::string_view model_name,
                                    const ModelCapabilities &missing);

// Append rel to prefix, ensuring exactly one '/' separator between them.
kimix::string join_path(const kimix::string &prefix, const kimix::string &rel);

// Repair tool-call arguments so they can be safely echoed back to the backend
// in message history. Some gateways stream duplicated argument chunks which
// merge into strings like "{}{}"; such arguments execute leniently on the
// client but poison the persisted history — strict backends (e.g. scnet/Qwen)
// reject them with HTTP 400 (code 10013) when they reappear in a request.
// Repair strategy, in order:
//   1. Already valid JSON -> returned unchanged (fast path).
//   2. Trailing garbage / duplicated chunks -> truncated to the first complete
//      JSON value (via yyjson YYJSON_READ_STOP_WHEN_DONE).
//   3. Nothing parseable -> "{}".
// Empty input also yields "{}" so the wire always carries a parseable object.
kimix::string sanitize_tool_arguments(const kimix::string &arguments);

// ---------------------------------------------------------------------------
// UTF-8 policy for request content
// ---------------------------------------------------------------------------
// The three providers all serialize agent-authored text (system prompt, message
// content, tool names and JSON schemas) with yyjson, and yyjson validates UTF-8
// while WRITING: one ill-formed sequence fails the whole document with
// YYJSON_WRITE_ERROR_INVALID_STRING and produces no bytes at all. A single bad
// byte anywhere in the request - a tool result cut on a byte boundary, text read
// from a legacy-codepage file, a prompt template compiled through an ANSI
// codepage - therefore dropped the entire body and killed the agent turn with
// the opaque "failed to build request body". Normalizing the text instead of
// losing it costs one character, not the turn.

// True when `bytes` is valid UTF-8: no bad lead byte, no truncated sequence, no
// overlong form, no surrogate code point (U+D800-DFFF), nothing above U+10FFFF.
bool utf8_valid(kimix::string_view bytes) noexcept;

// `bytes` decoded with the "replace" error handler: every ill-formed sequence
// becomes a single U+FFFD and the *maximal subpart* of that sequence is
// consumed - the Unicode recommended practice, which is also what CPython's
// bytes.decode("utf-8", errors="replace") does (b"\xe0\x80\x80" is three
// replacement characters because E0 must not be followed by 80, while
// b"\xe2\x80" is one maximal subpart). Valid input is copied unchanged.
kimix::string utf8_sanitize(kimix::string_view bytes);

// Add `value` to the mutable object `obj` under `key` as a JSON string that is
// guaranteed valid UTF-8 and keeps the caller's explicit length: an embedded
// '\0' is escaped as \u0000 instead of ending the string (yyjson's own add_str
// helpers are strlen-based). Valid text is referenced, not copied; invalid text
// is sanitized into a copy owned by the document.
void add_json_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                  kimix::string_view value);

// Parse `raw` as JSON and add the parsed value to `obj` under `key`. Invalid
// UTF-8 is sanitized before parsing, so a schema or an argument payload that
// carries a bad byte is still sent rather than silently replaced by the
// caller's fallback (an empty object costs the model its parameter list).
// Returns false when `raw` is not parseable JSON at all.
bool add_json_fragment(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                       const char *key, kimix::string_view raw);

// Serialize the document with the default (strict) writer options and release
// it - the caller must not touch `doc` again. On failure an empty string is
// returned and `error` carries yyjson's reason ("invalid utf-8 encoding in
// string", "memory allocation failed", ...), so a request that really cannot be
// built says why instead of only "failed to build request body".
kimix::string write_json_doc(yyjson_mut_doc *doc, kimix::string &error);

} // namespace kimix::llm
