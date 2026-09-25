// common.cpp - Shared helpers for the LLM provider libraries.
//
// NOTE: <httplib.h> is included first (even though this file does not use it)
// to guarantee winsock2.h is set up before <core/kimix_core.h> pulls in
// <windows.h> on Windows. The kimix-llm unity build concatenates this file
// first, and windows.h-before-winsock2.h breaks ws2tcpip.h.

#include <httplib.h>

#include "llm/common.h"

#include <cstdio>
#include <cstdlib>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm {

bool load_config(const kimix::string &path, Config &cfg) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        return false;
    }
    kimix::vector<char> buf((size_t)size);
    size_t rd = std::fread(buf.data(), 1, (size_t)size, fp);
    std::fclose(fp);
    if (rd == 0) {
        return false;
    }

    yyjson_doc *doc = yyjson_read_opts(buf.data(), rd, 0, &kYYJsonAlcMi, nullptr);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        auto get_str = [&](const char *key) -> kimix::string {
            yyjson_val *v = yyjson_obj_get(root, key);
            if (yyjson_is_str(v)) {
                return kimix::string(yyjson_get_str(v), yyjson_get_len(v));
            }
            return {};
        };
        cfg.model = get_str("model");
        cfg.url = get_str("url");
        cfg.api_key = get_str("api_key");
        cfg.type = get_str("type");
        cfg.thinking_effort = get_str("thinking_effort");
        if (cfg.thinking_effort.empty()) {
            cfg.thinking_effort = "high";
        }
        yyjson_val *v = yyjson_obj_get(root, "show_thinking_stream");
        cfg.show_thinking_stream = yyjson_is_bool(v) && yyjson_get_bool(v);
        v = yyjson_obj_get(root, "max_context_size");
        if (yyjson_is_int(v)) {
            cfg.max_context_size = (int32_t)yyjson_get_int(v);
        }
        v = yyjson_obj_get(root, "max_tokens");
        if (yyjson_is_int(v)) {
            cfg.max_tokens = (int32_t)yyjson_get_int(v);
        }
        ok = !cfg.model.empty() && !cfg.url.empty();
    }
    yyjson_doc_free(doc);
    return ok;
}

Endpoint parse_endpoint(const kimix::string &url) {
    Endpoint ep;
    ep.scheme = "http";
    ep.port = 80;
    kimix::string rest = url;
    if (rest.rfind("https://", 0) == 0) {
        ep.scheme = "https";
        rest = rest.substr(8);
        ep.port = 443;
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    ep.host = rest;
    ep.path_prefix = "/";
    size_t slash = rest.find('/');
    if (slash != kimix::string::npos) {
        ep.host = rest.substr(0, slash);
        ep.path_prefix = rest.substr(slash);
    }
    size_t colon = ep.host.find(':');
    if (colon != kimix::string::npos) {
        ep.port = (int32_t)std::atoi(ep.host.substr(colon + 1).c_str());
        ep.host = ep.host.substr(0, colon);
    }
    return ep;
}

bool is_retriable_status(int32_t status) {
    return status == 403 || status == 408 || status == 429 || status >= 500;
}

kimix::string join_path(const kimix::string &prefix, const kimix::string &rel) {
    kimix::string path = prefix;
    if (path.empty() || path.back() != '/') {
        path += '/';
    }
    if (!rel.empty() && rel.front() == '/') {
        path += rel.substr(1);
    } else {
        path += rel;
    }
    return path;
}

kimix::string sanitize_tool_arguments(const kimix::string &arguments) {
    if (arguments.empty()) {
        return "{}";
    }
    // Fast path: already strict-valid JSON.
    if (yyjson_doc *doc = yyjson_read_opts((char *)arguments.data(),
                                           arguments.size(), 0,
                                           &kYYJsonAlcMi, nullptr)) {
        yyjson_doc_free(doc);
        return arguments;
    }
    // Duplicated gateway chunks / trailing garbage: keep the first complete
    // JSON value. STOP_WHEN_DONE stops after the root value; the read size is
    // the length of the consumed prefix, which is strict-parseable on its own.
    if (yyjson_doc *doc = yyjson_read_opts((char *)arguments.data(),
                                           arguments.size(),
                                           YYJSON_READ_STOP_WHEN_DONE,
                                           &kYYJsonAlcMi, nullptr)) {
        const size_t n = yyjson_doc_get_read_size(doc);
        yyjson_doc_free(doc);
        if (n > 0) {
            return arguments.substr(0, n);
        }
    }
        return "{}";
    }

    // -----------------------------------------------------------------------
    // UTF-8 policy for request content (see the header comment in common.h)
    // -----------------------------------------------------------------------

    namespace {

    // Sequence shape of one UTF-8 lead byte: total length (0 when the byte can
    // not start a sequence) and the inclusive range the SECOND byte must stay
    // in. The ranges are what rejects overlong forms (E0 80..9F, F0 80..8F),
    // surrogates (ED A0..BF) and anything above U+10FFFF (F4 90..BF) without
    // decoding the code point.
    struct LlmUtf8Seq {
        size_t len;
        unsigned char lo;
        unsigned char hi;
    };

    constexpr LlmUtf8Seq llm_utf8_seq_of(unsigned char lead) {
        if (lead < 0x80u) return {1u, 0u, 0u};
        if (lead < 0xC2u) return {0u, 0u, 0u}; // continuation, or C0/C1
        if (lead < 0xE0u) return {2u, 0x80u, 0xBFu};
        if (lead == 0xE0u) return {3u, 0xA0u, 0xBFu};
        if (lead == 0xEDu) return {3u, 0x80u, 0x9Fu};
        if (lead < 0xF0u) return {3u, 0x80u, 0xBFu};
        if (lead == 0xF0u) return {4u, 0x90u, 0xBFu};
        if (lead < 0xF4u) return {4u, 0x80u, 0xBFu};
        if (lead == 0xF4u) return {4u, 0x80u, 0x8Fu};
        return {0u, 0u, 0u}; // F5..FF
    }

    const char kLlmUtf8Replacement[] = {"\xEF\xBF\xBD"};

    } // namespace

    bool utf8_valid(kimix::string_view bytes) noexcept {
        const size_t n = bytes.size();
        size_t i = 0;
        while (i < n) {
            const LlmUtf8Seq seq = llm_utf8_seq_of(static_cast<unsigned char>(bytes[i]));
            if (seq.len == 0u) {
                return false;
            }
            if (seq.len == 1u) {
                ++i;
                continue;
            }
            if (i + seq.len > n) {
                return false; // truncated
            }
            const unsigned char second =
                static_cast<unsigned char>(bytes[i + 1]);
            if (second < seq.lo || second > seq.hi) {
                return false;
            }
            for (size_t k = 2; k < seq.len; ++k) {
                if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0u) != 0x80u) {
                    return false;
                }
            }
            i += seq.len;
        }
        return true;
    }

    kimix::string utf8_sanitize(kimix::string_view bytes) {
        kimix::string out;
        out.reserve(bytes.size());
        const size_t n = bytes.size();
        size_t i = 0;
        while (i < n) {
            const char c = bytes[i];
            const LlmUtf8Seq seq = llm_utf8_seq_of(static_cast<unsigned char>(c));
            if (seq.len == 1u) {
                out.push_back(c);
                ++i;
                continue;
            }
            if (seq.len == 0u) {
                // Bad lead byte: its own maximal subpart (CPython decodes
                // b"\xc0\xaf" as two replacement characters, not one).
                out += kLlmUtf8Replacement;
                ++i;
                continue;
            }
            // Length of the maximal valid prefix of this sequence.
            size_t k = 1u;
            if (i + 1u < n) {
                const unsigned char second =
                    static_cast<unsigned char>(bytes[i + 1]);
                if (second >= seq.lo && second <= seq.hi) {
                    k = 2u;
                    while (k < seq.len && i + k < n &&
                           (static_cast<unsigned char>(bytes[i + k]) & 0xC0u) ==
                               0x80u) {
                        ++k;
                    }
                }
            }
            if (k == seq.len) {
                out.append(bytes.data() + i, seq.len);
                i += seq.len;
            } else {
                out += kLlmUtf8Replacement;
                i += k;
            }
        }
        return out;
    }

    void add_json_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                      kimix::string_view value) {
        if (!doc || !obj || !key) {
            return;
        }
        static const char kEmpty[] = "";
        const char *data = value.empty() ? kEmpty : value.data();
        if (utf8_valid(value)) {
            // Reference the caller's bytes with their explicit length: no copy,
            // and an embedded '\0' does not end the string the way the
            // strlen-based yyjson_mut_obj_add_str() would.
            yyjson_mut_val *val = yyjson_mut_strn(doc, data, value.size());
            if (val) {
                yyjson_mut_obj_add_val(doc, obj, key, val);
            }
            return;
        }
        const kimix::string fixed = utf8_sanitize(value);
        // Copies into the document, so the temporary may die right here.
        yyjson_mut_obj_add_strncpy(doc, obj, key, fixed.data(), fixed.size());
    }

    bool add_json_fragment(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                           const char *key, kimix::string_view raw) {
        if (!doc || !obj || !key || raw.empty()) {
            return false;
        }
        kimix::string fixed;
        const char *data = raw.data();
        size_t len = raw.size();
        if (!utf8_valid(raw)) {
            // The strict reader rejects invalid UTF-8, and the caller's fallback
            // is an empty object: sanitizing keeps the payload (a tool schema,
            // a tool argument object) instead of silently dropping it.
            fixed = utf8_sanitize(raw);
            data = fixed.data();
            len = fixed.size();
        }
        yyjson_doc *parsed =
            yyjson_read_opts(const_cast<char *>(data), len, 0, &kYYJsonAlcMi, nullptr);
        if (!parsed) {
            return false;
        }
        // Deep copy into `doc` (strings included), so `parsed`, `raw` and
        // `fixed` may all go away before the document is written.
        yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
        yyjson_doc_free(parsed);
        if (!copy) {
            return false;
        }
        return yyjson_mut_obj_add_val(doc, obj, key, copy);
    }

    kimix::string write_json_doc(yyjson_mut_doc *doc, kimix::string &error) {
        kimix::string body;
        if (!doc) {
            error = "no JSON document";
            return body;
        }
        yyjson_write_err werr;
        char *json =
            yyjson_mut_write_opts(doc, 0, &kYYJsonAlcMi, nullptr, &werr);
        if (json) {
            body = kimix::string(json);
            mi_free(json);
        } else if (werr.msg) {
            error = werr.msg;
        } else {
            error = "yyjson write failed";
        }
        yyjson_mut_doc_free(doc);
        return body;
    }

} // namespace kimix::llm
