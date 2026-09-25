// common.h - Shared helpers for the LLM provider libraries (OpenAI Chat,
// OpenAI Responses, Anthropic). The config loader, URL splitter, retry-status
// check and path joining were duplicated verbatim in each provider's chat .cpp;
// they now live here in namespace kimix::llm.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "yyjson.h"

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
