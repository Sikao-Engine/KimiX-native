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

} // namespace kimix::llm
