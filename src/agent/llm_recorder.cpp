// agent/llm_recorder.cpp - LLMRequestRecorder implementation (see
// llm_recorder.h). Port of kimi_cli/soul/llm_request_recorder.py.

#include "agent/llm_recorder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <mbedtls/sha256.h>

#include "llm/common.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

namespace kimix::agent {

namespace {

// The sha256 hex digest of `bytes` (_hash_text / _hash_json's core).
kimix::string sha256_hex(kimix::string_view bytes) {
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
                   digest, 0);
    kimix::string out;
    out.reserve(64);
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

// Recursive object-key sort into a fresh mutable value (the canonical form
// _hash_json hashes; lists recurse, scalars copy). Mirrors the reference's
// orjson.dumps(obj, option=orjson.OPT_SORT_KEYS) byte output when
// re-serialized compactly.
yyjson_mut_val *sort_value(yyjson_mut_doc *doc, yyjson_val *value) {
    if (yyjson_is_obj(value)) {
        yyjson_mut_val *out = yyjson_mut_obj(doc);
        struct entry {
            yyjson_val *key; // immutable key (deep-copied on add)
            yyjson_mut_val *val;
        };
        kimix::vector<entry> entries;
        size_t idx = 0, max = 0;
        yyjson_val *key = nullptr, *item = nullptr;
        yyjson_obj_foreach(value, idx, max, key, item) {
            entry e;
            e.key = key;
            e.val = sort_value(doc, item);
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(), [](const entry &a, const entry &b) {
            const size_t alen = yyjson_get_len(a.key);
            const size_t blen = yyjson_get_len(b.key);
            const size_t n = alen < blen ? alen : blen;
            const int cmp = std::memcmp(yyjson_get_str(a.key), yyjson_get_str(b.key), n);
            return cmp != 0 ? cmp < 0 : alen < blen;
        });
        for (const entry &e : entries) {
            // Deep-copy the immutable key into the mutable doc (a plain
            // yyjson_mut_strn over the key bytes produced a corrupt val with
            // the mimalloc-backed allocator - the val_mut_copy path is the
            // one yyjson's own obj_copy uses).
            yyjson_mut_val *k = yyjson_val_mut_copy(doc, e.key);
            yyjson_mut_obj_add(out, k, e.val);
        }
        return out;
    }
    if (yyjson_is_arr(value)) {
        // Lists recurse (orjson OPT_SORT_KEYS sorts every level, including
        // objects nested inside arrays).
        yyjson_mut_val *out = yyjson_mut_arr(doc);
        size_t idx = 0, max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(value, idx, max, item) {
            yyjson_mut_arr_add_val(out, sort_value(doc, item));
        }
        return out;
    }
    return yyjson_val_mut_copy(doc, value);
}

} // namespace

kimix::string recorder_hash_text(kimix::string_view text) { return sha256_hex(text); }

kimix::string recorder_hash_json(kimix::string_view json) {
    kimix::string buf(json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return sha256_hex(json); // unparseable: hash the raw text (never raises)
    }
    yyjson_mut_doc *mut = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *sorted = sort_value(mut, yyjson_doc_get_root(doc));
    yyjson_mut_doc_set_root(mut, sorted);
    size_t len = 0;
    char *text = yyjson_mut_write_opts(mut, YYJSON_WRITE_NOFLAG,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    kimix::string out;
    if (text != nullptr) {
        out = sha256_hex(kimix::string_view(text, len));
        mi_free(text);
    }
    yyjson_mut_doc_free(mut);
    yyjson_doc_free(doc);
    return out;
}

namespace {

// The canonical JSON of the offered tool table: a list of
// {"name":..., "description":..., "parameters": <parsed schema>} objects -
// _tool_schemas + _canonical_tools. An unparseable parameters schema
// degrades to {} (the hash stays defined; the snapshot writer applies the
// same degradation so the hashes agree).
kimix::string canonical_tools_json(const kimix::vector<kimix::llm::Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    for (const kimix::llm::Tool &tool : tools) {
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strn(doc, entry, "name", tool.name.data(),
                                tool.name.size());
        yyjson_mut_obj_add_strn(doc, entry, "description", tool.description.data(),
                                tool.description.size());
        if (!kimix::llm::add_json_fragment(doc, entry, "parameters", tool.parameters_json)) {
            yyjson_mut_obj_add_val(doc, entry, "parameters", yyjson_mut_obj(doc));
        }
        yyjson_mut_arr_add_val(arr, entry);
    }
    size_t len = 0;
    char *text = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    kimix::string out;
    if (text != nullptr) {
        out.assign(text, len);
        mi_free(text);
    }
    yyjson_mut_doc_free(doc);
    return out;
}

} // namespace

void LLMRequestRecorder::restore_from(kimix::string_view wire_path) noexcept {
    // llm_request_recorder.py restore_from (:81-104): read the raw envelope
    // payloads by type string; never re-validate, never fail.
    if (wire_path.empty()) {
        return;
    }
    std::FILE *f = std::fopen(kimix::string(wire_path).c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    kimix::string line;
    char chunk[4096];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        line.append(chunk, n);
    }
    std::fclose(f);
    // Split into lines; each is one envelope record.
    size_t begin = 0;
    while (begin <= line.size()) {
        const size_t end = line.find('\n', begin);
        const size_t len = (end == kimix::string::npos) ? line.size() - begin
                                                        : end - begin;
        if (len > 0) {
            kimix::string buf = line.substr(begin, len);
            yyjson_doc *doc =
                yyjson_read_opts(buf.data(), buf.size(), 0,
                                 &kimix::llm::kYYJsonAlcMi, nullptr);
            yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
            yyjson_val *message =
                root != nullptr && yyjson_is_obj(root) ? yyjson_obj_get(root, "message")
                                                       : nullptr;
            yyjson_val *payload =
                message != nullptr && yyjson_is_obj(message)
                    ? yyjson_obj_get(message, "payload")
                    : nullptr;
            yyjson_val *type =
                message != nullptr && yyjson_is_obj(message)
                    ? yyjson_obj_get(message, "type")
                    : nullptr;
            if (payload != nullptr && yyjson_is_obj(payload) && type != nullptr &&
                yyjson_is_str(type)) {
                const kimix::string_view ttype(yyjson_get_str(type),
                                               yyjson_get_len(type));
                if (ttype == "LLMToolsSnapshot") {
                    yyjson_val *h = yyjson_obj_get(payload, "hash");
                    if (h != nullptr && yyjson_is_str(h)) {
                        _seen_tools_hashes.emplace(yyjson_get_str(h),
                                                   yyjson_get_len(h));
                    }
                } else if (ttype == "LLMRequest") {
                    yyjson_val *ph = yyjson_obj_get(payload, "system_prompt_hash");
                    if (ph != nullptr && yyjson_is_str(ph)) {
                        _seen_prompt_hashes.emplace(yyjson_get_str(ph),
                                                    yyjson_get_len(ph));
                    }
                    yyjson_val *th = yyjson_obj_get(payload, "tools_hash");
                    if (th != nullptr && yyjson_is_str(th)) {
                        _seen_tools_hashes.emplace(yyjson_get_str(th),
                                                   yyjson_get_len(th));
                    }
                } else if (ttype == "MCPToolsDiscovered") {
                    yyjson_val *server = yyjson_obj_get(payload, "server_name");
                    yyjson_val *h = yyjson_obj_get(payload, "hash");
                    if (server != nullptr && yyjson_is_str(server) &&
                        h != nullptr && yyjson_is_str(h)) {
                        kimix::string key(yyjson_get_str(server),
                                          yyjson_get_len(server));
                        key += '\x1F'; // unit separator: the (server, hash) tuple
                        key.append(yyjson_get_str(h), yyjson_get_len(h));
                        _seen_mcp_discoveries.insert(std::move(key));
                    }
                }
            }
            if (doc != nullptr) {
                yyjson_doc_free(doc);
            }
        }
        if (end == kimix::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

void LLMRequestRecorder::record(WireSink *sink,
                                const recorder_provider_fields &provider,
                                kimix::string_view system_prompt,
                                const kimix::vector<kimix::llm::Tool> &tools,
                                int32_t message_count, kimix::string_view kind,
                                int32_t turn_step, int32_t attempt,
                                int32_t dropped_count) noexcept {
    if (sink == nullptr) {
        return;
    }
    // The pre-flight rule (record.py :119-121) has no native equivalent - the
    // C++ turn is synchronous, so a cancelled request never reaches record().
    // 1. Tools snapshot, content-addressed, once per unique hash (:151-165).
    // NOTE: the reference's id(tool)-identity cache (skipping the re-hash of
    // an unchanged table) is not ported - the native tool table is rebuilt per
    // turn, so identity keys do not survive; the sha256 is cheap and the dedup
    // outcome is identical.
    const kimix::string tools_hash =
        recorder_hash_json(canonical_tools_json(tools));
    if (_seen_tools_hashes.find(tools_hash) == _seen_tools_hashes.end()) {
        _seen_tools_hashes.insert(tools_hash);
        sink->wire_llm_tools_snapshot(tools_hash, tools);
    }
    // 2. The request record itself (:167-194).
    const kimix::string prompt_hash = recorder_hash_text(system_prompt);
    llm_request_record rec;
    rec.kind = kimix::string(kind);
    rec.provider = provider.provider;
    rec.model = provider.model;
    rec.thinking_effort = provider.thinking_effort;
    rec.has_temperature = provider.has_temperature;
    rec.temperature = provider.temperature;
    rec.has_top_p = provider.has_top_p;
    rec.top_p = provider.top_p;
    rec.max_tokens = provider.max_tokens;
    rec.system_prompt_hash = prompt_hash;
    if (_seen_prompt_hashes.find(prompt_hash) == _seen_prompt_hashes.end()) {
        _seen_prompt_hashes.insert(prompt_hash);
        rec.system_prompt = kimix::string(system_prompt); // inline once
    }
    rec.tools_hash = tools_hash;
    rec.message_count = message_count;
    rec.turn_step = turn_step;
    rec.attempt = attempt;
    rec.dropped_count = dropped_count;
    sink->wire_llm_request(rec);
}

void LLMRequestRecorder::record_mcp_discovery(
    WireSink *sink, kimix::string_view server_name,
    const kimix::vector<kimix::llm::Tool> &tools,
    const kimix::vector<kimix::string> &enabled_names,
    const kimix::vector<kimix::string> &collisions) noexcept {
    if (sink == nullptr) {
        return;
    }
    // :196-229: hash over {tools, enabled_names, collisions}; dedup per
    // (server_name, hash).
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    {
        kimix::string tools_json = canonical_tools_json(tools);
        if (!kimix::llm::add_json_fragment(doc, root, "tools", tools_json)) {
            yyjson_mut_obj_add_val(doc, root, "tools", yyjson_mut_arr(doc));
        }
    }
    yyjson_mut_val *enabled = yyjson_mut_arr(doc);
    for (const kimix::string &name : enabled_names) {
        yyjson_mut_arr_add_strn(doc, enabled, name.data(), name.size());
    }
    yyjson_mut_obj_add_val(doc, root, "enabled_names", enabled);
    yyjson_mut_val *collide = yyjson_mut_arr(doc);
    for (const kimix::string &name : collisions) {
        yyjson_mut_arr_add_strn(doc, collide, name.data(), name.size());
    }
    yyjson_mut_obj_add_val(doc, root, "collisions", collide);
    size_t len = 0;
    char *text = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    const kimix::string payload = text != nullptr ? kimix::string(text, len) : kimix::string();
    if (text != nullptr) {
        mi_free(text);
    }
    yyjson_mut_doc_free(doc);
    const kimix::string discovery_hash = recorder_hash_json(payload);
    kimix::string key(server_name);
    key += '\x1F';
    key += discovery_hash;
    if (_seen_mcp_discoveries.find(key) != _seen_mcp_discoveries.end()) {
        return;
    }
    _seen_mcp_discoveries.insert(key);
    sink->wire_mcp_tools_discovered(server_name, discovery_hash, tools,
                                    enabled_names, collisions);
}

} // namespace kimix::agent
