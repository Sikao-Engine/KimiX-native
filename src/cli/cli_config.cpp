// cli/cli_config.cpp - Provider/agent config deserialization (see cli_config.h).
//
// Union loader for the two provider dialects the reference accepts:
//   * flat kimix   - C:/dev/ds_flash.json, C:/dev/kimi-agent/src/kimix/
//                    default_config.json (model/type/url/api_key/... at the top).
//   * nested kimi  - kimi_cli/config.py (model name + provider{...} + an optional
//                    models{...} table).
// Both may coexist; nested keys win over flat duplicates and base_url wins over
// url.  Unknown keys only warn.  Ground truth:
// .kimix_cache/cli_specs/02_config.md.
//
// The model-defaults resolver is a verbatim port of _MODEL_DEFAULTS /
// _resolve_model_defaults (spec §3.3): lowercase tokenisation on [^a-z0-9]+,
// exact-first matching over a token multiset, numeric keyword tokens only match
// exactly, alphabetic keyword tokens may fuzzy-match with an Indel-distance
// similarity >= 80 (_FUZZY_TOKEN_THRESHOLD), first matching row wins.
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `clicfg_` prefix.

#include "cli/cli_config.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_tools.h"

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Small JSON / string helpers
// ---------------------------------------------------------------------------

kimix::string clicfg_i64(int64_t value) {
    if (value == 0) {
        return kimix::string("0");
    }
    const bool neg = value < 0;
    uint64_t v = neg ? static_cast<uint64_t>(-(value + 1)) + 1u
                     : static_cast<uint64_t>(value);
    char buf[24];
    size_t i = sizeof(buf);
    while (v > 0) {
        buf[--i] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    if (neg) {
        buf[--i] = '-';
    }
    return kimix::string(buf + i, sizeof(buf) - i);
}

kimix::string clicfg_str_val(yyjson_val *v) {
    if (yyjson_is_str(v)) {
        return kimix::string(yyjson_get_str(v), yyjson_get_len(v));
    }
    return {};
}

yyjson_val *clicfg_obj(yyjson_val *obj, const char *key) {
    return yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : nullptr;
}

yyjson_val *clicfg_arr(yyjson_val *obj, const char *key) {
    yyjson_val *v = clicfg_obj(obj, key);
    return yyjson_is_arr(v) ? v : nullptr;
}

kimix::string clicfg_get_str(yyjson_val *obj, const char *key) {
    return clicfg_str_val(clicfg_obj(obj, key));
}

bool clicfg_get_int(yyjson_val *obj, const char *key, int64_t &out) {
    yyjson_val *v = clicfg_obj(obj, key);
    if (yyjson_is_uint(v)) {
        out = static_cast<int64_t>(yyjson_get_uint(v));
        return true;
    }
    if (yyjson_is_sint(v)) {
        out = yyjson_get_sint(v);
        return true;
    }
    return false;
}

bool clicfg_get_bool(yyjson_val *obj, const char *key, bool &out) {
    yyjson_val *v = clicfg_obj(obj, key);
    if (yyjson_is_bool(v)) {
        out = yyjson_get_bool(v);
        return true;
    }
    return false;
}

// `a[key_a]` when it is an object, else `b[key_b]` when it is an object, else
// null.  Used for "nested kimi-cli object wins over the flat duplicate".
yyjson_val *clicfg_pick_obj(yyjson_val *a, const char *key_a, yyjson_val *b,
                            const char *key_b) {
    yyjson_val *v = clicfg_obj(a, key_a);
    if (yyjson_is_obj(v)) {
        return v;
    }
    v = clicfg_obj(b, key_b);
    return yyjson_is_obj(v) ? v : nullptr;
}

void clicfg_kv(kimix::string &out, const char *key, const kimix::string &value) {
    out += "  ";
    out += key;
    out += ": ";
    out += value;
    out += "\n";
}

kimix::string clicfg_join(const kimix::vector<kimix::string> &parts,
                          const char *sep) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Recognised-key tables (only used to warn about unknown keys)
// ---------------------------------------------------------------------------

bool clicfg_in_list(const char *const *list, size_t count, kimix::string_view key) {
    for (size_t i = 0; i < count; ++i) {
        if (key == list[i]) {
            return true;
        }
    }
    return false;
}

const char *const k_clicfg_top_keys[] = {
    "model", "model_name", "name", "role", "version",
    "type", "url", "base_url", "api_key", "env", "custom_headers",
    "reasoning_key", "openai_settings", "oauth",
    "max_context_size", "max_tokens", "capabilities", "display_name",
    "supported_efforts", "temperature", "top_p", "top_k", "thinking_effort",
    "show_thinking_stream", "default_thinking", "default_yolo", "default_editor",
    "theme", "loop_control", "background", "notifications", "services", "web",
    "mcp", "hooks", "merge_all_available_skills", "extra_skill_dirs",
    "sub_provider", "sub_providers", "provider", "models",
};

const char *const k_clicfg_provider_keys[] = {
    "type", "url", "base_url", "api_key", "env", "custom_headers",
    "reasoning_key", "openai_settings", "oauth",
};

const char *const k_clicfg_model_keys[] = {
    "model", "max_context_size", "max_tokens", "capabilities", "display_name",
    "supported_efforts",
};

const char *const k_clicfg_services_keys[] = {"search", "fetch"};

const char *const k_clicfg_endpoint_keys[] = {
    "base_url", "api_key", "custom_headers", "oauth",
};

bool clicfg_known_top(kimix::string_view k) {
    return clicfg_in_list(k_clicfg_top_keys,
                          sizeof(k_clicfg_top_keys) / sizeof(k_clicfg_top_keys[0]), k);
}
bool clicfg_known_provider(kimix::string_view k) {
    return clicfg_in_list(k_clicfg_provider_keys,
                          sizeof(k_clicfg_provider_keys) / sizeof(k_clicfg_provider_keys[0]), k);
}
bool clicfg_known_model(kimix::string_view k) {
    return clicfg_in_list(k_clicfg_model_keys,
                          sizeof(k_clicfg_model_keys) / sizeof(k_clicfg_model_keys[0]), k);
}
bool clicfg_known_services(kimix::string_view k) {
    return clicfg_in_list(k_clicfg_services_keys,
                          sizeof(k_clicfg_services_keys) / sizeof(k_clicfg_services_keys[0]), k);
}
bool clicfg_known_endpoint(kimix::string_view k) {
    return clicfg_in_list(k_clicfg_endpoint_keys,
                          sizeof(k_clicfg_endpoint_keys) / sizeof(k_clicfg_endpoint_keys[0]), k);
}

void clicfg_warn_unknown(yyjson_val *obj, bool (*known)(kimix::string_view),
                         const char *where, kimix::vector<kimix::string> &warnings) {
    if (!yyjson_is_obj(obj)) {
        return;
    }
    size_t idx, max;
    yyjson_val *key;
    yyjson_val *val;
    yyjson_obj_foreach(obj, idx, max, key, val) {
        (void)val;
        if (!yyjson_is_str(key)) {
            continue;
        }
        kimix::string_view k(yyjson_get_str(key), yyjson_get_len(key));
        if (!known(k)) {
            warnings.push_back(kimix::string("unrecognized key '") + kimix::string(k) +
                               "' in " + where);
        }
    }
}

// ---------------------------------------------------------------------------
// provider_family normalisation (create_llm accepts openai / openai_responses
// / anthropic; see src/llm/llm.cpp).
// ---------------------------------------------------------------------------

kimix::string clicfg_normalize_family(kimix::string_view type) {
    const kimix::string t = to_lower_ascii(type);
    if (t == "openai" || t == "openai_legacy" || t == "kimi" ||
        starts_with(t, "moonshot") || starts_with(t, "openai_chat")) {
        return kimix::string("openai");
    }
    if (starts_with(t, "openai_responses") || starts_with(t, "responses")) {
        return kimix::string("openai_responses");
    }
    if (starts_with(t, "anthropic") || starts_with(t, "claude")) {
        return kimix::string("anthropic");
    }
    return {};
}

// ---------------------------------------------------------------------------
// _MODEL_DEFAULTS + _resolve_model_defaults (spec §3.3, verbatim rows/order)
// ---------------------------------------------------------------------------

constexpr int64_t k_clicfg_no_output = -1; // row max_output == None
constexpr double k_clicfg_fuzzy_threshold = 80.0; // _FUZZY_TOKEN_THRESHOLD

struct clicfg_model_row {
    const char *const *keywords; // keyword phrases (each is tokenised on match)
    size_t count;
    int64_t context;
    int64_t output;
};

const char *const k_clicfg_kw_gpt56_sol[] = {"gpt-5.6", "sol"};
const char *const k_clicfg_kw_gpt55[] = {"gpt-5.5"};
const char *const k_clicfg_kw_gpt54_mini[] = {"gpt-5.4", "mini"};
const char *const k_clicfg_kw_gpt54[] = {"gpt-5.4"};
const char *const k_clicfg_kw_opus5[] = {"claude", "opus", "5"};
const char *const k_clicfg_kw_opus48[] = {"claude", "opus", "4.8"};
const char *const k_clicfg_kw_sonnet5[] = {"claude", "sonnet", "5"};
const char *const k_clicfg_kw_sonnet46[] = {"claude", "sonnet", "4.6"};
const char *const k_clicfg_kw_haiku45[] = {"claude", "haiku", "4.5"};
const char *const k_clicfg_kw_gemini36[] = {"gemini", "3.6"};
const char *const k_clicfg_kw_gemini35_flash[] = {"gemini", "3.5", "flash"};
const char *const k_clicfg_kw_gemini31_pro[] = {"gemini", "3.1", "pro"};
const char *const k_clicfg_kw_nova2_lite[] = {"amazon", "nova", "2", "lite"};
const char *const k_clicfg_kw_ds_v4_pro[] = {"deepseek", "v4", "pro"};
const char *const k_clicfg_kw_ds_v4_flash[] = {"deepseek", "v4", "flash"};
const char *const k_clicfg_kw_supergrok[] = {"supergrok", "heavy"};
const char *const k_clicfg_kw_grok[] = {"grok"};

const clicfg_model_row k_clicfg_model_defaults[] = {
    // OpenAI GPT
    {k_clicfg_kw_gpt56_sol, 2, 1050000, 128000},
    {k_clicfg_kw_gpt55, 1, 1050000, 128000},
    {k_clicfg_kw_gpt54_mini, 2, 1000000, 65536},
    {k_clicfg_kw_gpt54, 1, 1000000, 128000},
    // Anthropic Claude
    {k_clicfg_kw_opus5, 3, 1000000, 128000},
    {k_clicfg_kw_opus48, 3, 1000000, 128000},
    {k_clicfg_kw_sonnet5, 3, 1000000, 128000},
    {k_clicfg_kw_sonnet46, 3, 1000000, 64000},
    {k_clicfg_kw_haiku45, 3, 200000, 64000},
    // Google Gemini
    {k_clicfg_kw_gemini36, 2, 1048576, 65536},
    {k_clicfg_kw_gemini35_flash, 3, 1048576, 65536},
    {k_clicfg_kw_gemini31_pro, 3, 1048576, 65536},
    // Amazon Nova
    {k_clicfg_kw_nova2_lite, 4, 1000000, 64000},
    // DeepSeek
    {k_clicfg_kw_ds_v4_pro, 3, 1000000, 384000},
    {k_clicfg_kw_ds_v4_flash, 3, 1000000, 384000},
    // xAI Grok
    {k_clicfg_kw_supergrok, 2, 2000000, k_clicfg_no_output},
    {k_clicfg_kw_grok, 1, 2000000, k_clicfg_no_output},
};

bool clicfg_is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// _tokenize_model_name: lowercase, then split on runs of [^a-z0-9]+.
void clicfg_tokenize(kimix::string_view name, kimix::vector<kimix::string> &out) {
    out.clear();
    const kimix::string lower = to_lower_ascii(name);
    size_t i = 0;
    const size_t n = lower.size();
    while (i < n) {
        while (i < n && !clicfg_is_token_char(lower[i])) {
            ++i;
        }
        const size_t start = i;
        while (i < n && clicfg_is_token_char(lower[i])) {
            ++i;
        }
        if (i > start) {
            out.emplace_back(lower.data() + start, i - start);
        }
    }
}

// Python str.isalpha() for the ASCII [a-z] tokens this tokenizer produces.
bool clicfg_is_alpha_token(kimix::string_view token) {
    if (token.empty()) {
        return false;
    }
    for (char c : token) {
        if (c < 'a' || c > 'z') {
            return false;
        }
    }
    return true;
}

// rapidfuzz.fuzz.ratio = normalized Indel similarity =
// 100 * (len(a)+len(b) - indel_distance) / (len(a)+len(b)), where
// indel_distance = len(a)+len(b) - 2*LCS, i.e. 100 * 2*LCS / (la+lb).
double clicfg_indel_ratio(kimix::string_view a, kimix::string_view b) {
    const size_t la = a.size();
    const size_t lb = b.size();
    if (la == 0 && lb == 0) {
        return 100.0;
    }
    kimix::vector<size_t> prev(lb + 1, 0);
    kimix::vector<size_t> cur(lb + 1, 0);
    for (size_t i = 1; i <= la; ++i) {
        for (size_t j = 1; j <= lb; ++j) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
            } else {
                cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
            }
        }
        kimix::vector<size_t> tmp = std::move(prev);
        prev = std::move(cur);
        cur = std::move(tmp);
        for (size_t j = 0; j <= lb; ++j) {
            cur[j] = 0;
        }
    }
    const size_t lcs = prev[lb];
    return 100.0 * (2.0 * static_cast<double>(lcs)) /
           static_cast<double>(la + lb);
}

// _keywords_match: every keyword token must be consumed from the model-token
// multiset; numeric tokens only match exactly, alphabetic tokens may fuzzy-match
// the best remaining alphabetic token (strict >, first occurrence wins on ties).
bool clicfg_keywords_match(const clicfg_model_row &row,
                           const kimix::vector<kimix::string> &model_tokens) {
    kimix::vector<kimix::string> distinct; // first-occurrence order
    kimix::vector<int> counts;
    for (const kimix::string &tok : model_tokens) {
        bool found = false;
        for (size_t i = 0; i < distinct.size(); ++i) {
            if (distinct[i] == tok) {
                counts[i] += 1;
                found = true;
                break;
            }
        }
        if (!found) {
            distinct.push_back(tok);
            counts.push_back(1);
        }
    }

    kimix::vector<kimix::string> keyword_tokens;
    for (size_t k = 0; k < row.count; ++k) {
        clicfg_tokenize(row.keywords[k], keyword_tokens);
        for (const kimix::string &kt : keyword_tokens) {
            int *exact = nullptr;
            for (size_t i = 0; i < distinct.size(); ++i) {
                if (distinct[i] == kt) {
                    exact = &counts[i];
                    break;
                }
            }
            if (exact != nullptr && *exact > 0) {
                *exact -= 1;
                continue;
            }
            if (!clicfg_is_alpha_token(kt)) {
                return false; // numeric keyword tokens never fuzzy-match
            }
            double best_score = -1.0;
            size_t best_index = distinct.size();
            for (size_t i = 0; i < distinct.size(); ++i) {
                if (counts[i] <= 0 || !clicfg_is_alpha_token(distinct[i])) {
                    continue;
                }
                const double score = clicfg_indel_ratio(kt, distinct[i]);
                if (score > best_score) {
                    best_score = score;
                    best_index = i;
                }
            }
            if (best_index != distinct.size() && best_score >= k_clicfg_fuzzy_threshold) {
                counts[best_index] -= 1;
            } else {
                return false;
            }
        }
    }
    return true;
}

bool clicfg_resolve_model(kimix::string_view model_name, int64_t &context,
                          int64_t &output) {
    kimix::vector<kimix::string> tokens;
    clicfg_tokenize(model_name, tokens);
    const size_t rows = sizeof(k_clicfg_model_defaults) / sizeof(k_clicfg_model_defaults[0]);
    for (size_t i = 0; i < rows; ++i) {
        if (clicfg_keywords_match(k_clicfg_model_defaults[i], tokens)) {
            context = k_clicfg_model_defaults[i].context;
            output = k_clicfg_model_defaults[i].output;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Provider parsing
// ---------------------------------------------------------------------------

void clicfg_read_str_array(yyjson_val *arr, kimix::vector<kimix::string> &out) {
    if (!yyjson_is_arr(arr)) {
        return;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        if (yyjson_is_str(item)) {
            out.push_back(clicfg_str_val(item));
        }
    }
}

void clicfg_read_pair_object(yyjson_val *obj,
                             kimix::vector<std::pair<kimix::string, kimix::string>> &out) {
    if (!yyjson_is_obj(obj)) {
        return;
    }
    size_t idx, max;
    yyjson_val *key;
    yyjson_val *val;
    yyjson_obj_foreach(obj, idx, max, key, val) {
        out.emplace_back(clicfg_str_val(key), clicfg_str_val(val));
    }
}

bool clicfg_parse_provider(const kimix::string &path, yyjson_val *root,
                           provider_config &out, kimix::string &error) {
    const kimix::string where = kimix::string("provider config '") + path + "': ";

    yyjson_val *provider = clicfg_obj(root, "provider");
    if (!yyjson_is_obj(provider)) {
        provider = nullptr;
    }
    yyjson_val *services = clicfg_obj(root, "services");
    if (!yyjson_is_obj(services)) {
        services = nullptr;
    }

    // Model name + optional models table entry (nested kimi-cli dialect).
    yyjson_val *model_entry = nullptr;
    yyjson_val *model_field = clicfg_obj(root, "model");
    if (yyjson_is_obj(model_field)) {
        // [model] table form.
        model_entry = model_field;
        out.model = clicfg_get_str(model_field, "model");
    } else if (yyjson_is_str(model_field)) {
        out.model = clicfg_str_val(model_field);
        yyjson_val *models = clicfg_obj(root, "models");
        if (yyjson_is_obj(models) && !out.model.empty()) {
            yyjson_val *entry =
                yyjson_obj_getn(models, out.model.data(), out.model.size());
            if (yyjson_is_obj(entry)) {
                model_entry = entry;
            }
        }
    }

    // Provider / connection fields (nested provider wins over flat duplicates).
    out.type = clicfg_get_str(provider, "type");
    if (out.type.empty()) {
        out.type = clicfg_get_str(root, "type");
    }
    out.base_url = clicfg_get_str(provider, "base_url");
    if (out.base_url.empty()) {
        out.base_url = clicfg_get_str(provider, "url");
    }
    if (out.base_url.empty()) {
        out.base_url = clicfg_get_str(root, "base_url");
    }
    if (out.base_url.empty()) {
        out.base_url = clicfg_get_str(root, "url");
    }
    out.api_key = clicfg_get_str(provider, "api_key");
    if (out.api_key.empty()) {
        out.api_key = clicfg_get_str(root, "api_key");
    }
    out.reasoning_key = clicfg_get_str(provider, "reasoning_key");
    if (out.reasoning_key.empty()) {
        out.reasoning_key = clicfg_get_str(root, "reasoning_key");
    }
    if (out.reasoning_key.empty()) {
        out.reasoning_key = "reasoning_content";
    }

    // env / custom_headers (nested provider object wins).
    clicfg_read_pair_object(clicfg_pick_obj(provider, "env", root, "env"), out.env);
    clicfg_read_pair_object(
        clicfg_pick_obj(provider, "custom_headers", root, "custom_headers"),
        out.custom_headers);

    // openai_settings.
    yyjson_val *openai =
        clicfg_pick_obj(provider, "openai_settings", root, "openai_settings");
    if (openai != nullptr) {
        bool b = true;
        if (clicfg_get_bool(openai, "thinking", b)) {
            out.openai.thinking = b;
        }
        if (clicfg_get_bool(openai, "reasoning", b)) {
            out.openai.reasoning = b;
        }
        if (clicfg_get_bool(openai, "chat_template_kwargs", b)) {
            out.openai.chat_template_kwargs = b;
        }
    }

    // oauth.
    yyjson_val *oauth = clicfg_pick_obj(provider, "oauth", root, "oauth");
    if (oauth != nullptr) {
        out.has_oauth = true;
        out.oauth_storage = clicfg_get_str(oauth, "storage");
        if (out.oauth_storage.empty()) {
            out.oauth_storage = "file";
        }
        out.oauth_key = clicfg_get_str(oauth, "key");
    }

    // Model fields (models-table / [model] entry wins over flat duplicates).
    clicfg_read_str_array(clicfg_arr(model_entry, "capabilities"), out.capabilities);
    if (out.capabilities.empty()) {
        clicfg_read_str_array(clicfg_arr(root, "capabilities"), out.capabilities);
    }

    int64_t value = 0;
    bool ctx_explicit = clicfg_get_int(model_entry, "max_context_size", value);
    if (!ctx_explicit) {
        ctx_explicit = clicfg_get_int(root, "max_context_size", value);
    }
    out.max_context_size = value;
    out.max_context_size_explicit = ctx_explicit && value > 0;

    value = 0;
    bool tokens_explicit = clicfg_get_int(model_entry, "max_tokens", value);
    if (!tokens_explicit) {
        tokens_explicit = clicfg_get_int(root, "max_tokens", value);
    }
    out.max_tokens = value;
    out.max_tokens_explicit = tokens_explicit && value > 0;

    // Config-level flags.
    bool flag = true;
    if (clicfg_get_bool(root, "show_thinking_stream", flag)) {
        out.show_thinking_stream = flag;
    }
    out.thinking_effort = clicfg_get_str(root, "thinking_effort");
    if (out.thinking_effort.empty()) {
        out.thinking_effort = "high";
    }

    // services{search,fetch}.
    if (services != nullptr) {
        yyjson_val *search = clicfg_obj(services, "search");
        if (yyjson_is_obj(search)) {
            out.search.base_url = clicfg_get_str(search, "base_url");
            out.search.api_key = clicfg_get_str(search, "api_key");
            clicfg_warn_unknown(search, clicfg_known_endpoint, "services.search",
                                out.warnings);
        }
        yyjson_val *fetch = clicfg_obj(services, "fetch");
        if (yyjson_is_obj(fetch)) {
            out.fetch.base_url = clicfg_get_str(fetch, "base_url");
            out.fetch.api_key = clicfg_get_str(fetch, "api_key");
            clicfg_warn_unknown(fetch, clicfg_known_endpoint, "services.fetch",
                                out.warnings);
        }
        clicfg_warn_unknown(services, clicfg_known_services, "services", out.warnings);
    }

    // Unknown-key warnings (never fatal, never a silent drop).
    clicfg_warn_unknown(root, clicfg_known_top, "provider config", out.warnings);
    clicfg_warn_unknown(provider, clicfg_known_provider, "provider", out.warnings);
    clicfg_warn_unknown(model_entry, clicfg_known_model, "model", out.warnings);

    // Required fields (the reference's _create_config assertions).
    if (out.type.empty()) {
        error = where + "missing required field 'type'";
        return false;
    }
    if (out.model.empty()) {
        error = where + "missing required field 'model'";
        return false;
    }
    if (out.base_url.empty()) {
        error = where + "missing required field 'url' or 'base_url'";
        return false;
    }
    out.provider_family = clicfg_normalize_family(out.type);
    if (out.provider_family.empty()) {
        error = where + "unsupported provider type '" + out.type +
                "' (expected openai/openai_legacy/kimi/moonshot*/openai_chat*, "
                "openai_responses*/responses*, anthropic*/claude*)";
        return false;
    }

    // Token limits: explicit > model defaults > hard error (context) and
    // explicit > model-default output > max_context_size / 4 (max_tokens).
    if (!out.max_context_size_explicit) {
        int64_t default_context = 0;
        int64_t default_output = k_clicfg_no_output;
        if (!clicfg_resolve_model(out.model, default_context, default_output)) {
            error = where + "Unknown model '" + out.model +
                    "'. Cannot determine max_context_size and max_tokens from the "
                    "model name.";
            return false;
        }
        out.max_context_size = default_context;
        out.warnings.push_back(kimix::string("max_context_size derived from the "
                                             "model defaults for '") +
                               out.model + "'");
        if (!out.max_tokens_explicit) {
            out.max_tokens = (default_output > 0) ? default_output
                                                  : out.max_context_size / 4;
            out.warnings.push_back(kimix::string("max_tokens derived from the "
                                                 "model defaults for '") +
                                   out.model + "'");
        }
    } else if (!out.max_tokens_explicit) {
        out.max_tokens = out.max_context_size / 4;
        out.warnings.push_back("max_tokens derived from max_context_size / 4");
    }

    // api_key fallback: config -> $KIMI_API_KEY -> $KIMIX_API_KEY (+ warning).
    if (out.api_key.empty()) {
        kimix::string env_key;
        if (get_env("KIMI_API_KEY", env_key) && !env_key.empty()) {
            out.api_key = env_key;
            out.warnings.push_back("api_key missing in config; using $KIMI_API_KEY");
        } else if (get_env("KIMIX_API_KEY", env_key) && !env_key.empty()) {
            out.api_key = env_key;
            out.warnings.push_back("api_key missing in config; using $KIMIX_API_KEY");
        } else {
            out.warnings.push_back(
                "api_key missing in config and in $KIMI_API_KEY / $KIMIX_API_KEY");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Agent manifest parsing
// ---------------------------------------------------------------------------

bool clicfg_is_absolute_path(kimix::string_view path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return path.size() >= 2 && path[1] == ':' &&
           ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'));
}

bool clicfg_parse_agent(const kimix::string &path, yyjson_val *root,
                        agent_config &out, kimix::string &error) {
    const kimix::string where = kimix::string("agent manifest '") + path + "': ";

    // version (str or int) must be "1" when present.
    kimix::string version;
    yyjson_val *version_val = clicfg_obj(root, "version");
    if (yyjson_is_str(version_val)) {
        version = clicfg_str_val(version_val);
    } else if (yyjson_is_int(version_val)) {
        version = "1";
    }
    if (!version.empty() && version != "1") {
        error = where + "unsupported agent spec version: " + version;
        return false;
    }

    // {"agent": {...}} or a bare top-level agent object.
    yyjson_val *agent = clicfg_obj(root, "agent");
    if (!yyjson_is_obj(agent)) {
        agent = root;
    }

    out.extend = clicfg_get_str(agent, "extend");
    out.name = clicfg_get_str(agent, "name");
    out.model = clicfg_get_str(agent, "model");
    out.when_to_use = clicfg_get_str(agent, "when_to_use");
    kimix::string raw_prompt = clicfg_get_str(agent, "system_prompt_path");

    clicfg_read_pair_object(clicfg_obj(agent, "system_prompt_args"),
                            out.system_prompt_args);

    yyjson_val *tools = clicfg_arr(agent, "tools");
    const bool has_tools = tools != nullptr;
    clicfg_read_str_array(tools, out.tools);

    yyjson_val *allowed = clicfg_arr(agent, "allowed_tools");
    const bool has_allowed = allowed != nullptr;
    clicfg_read_str_array(allowed, out.allowed_tools);

    clicfg_read_str_array(clicfg_arr(agent, "exclude_tools"), out.exclude_tools);

    // subagents: keep the names (keys) in order.
    yyjson_val *subagents = clicfg_obj(agent, "subagents");
    if (yyjson_is_obj(subagents)) {
        size_t idx, max;
        yyjson_val *key;
        yyjson_val *val;
        yyjson_obj_foreach(subagents, idx, max, key, val) {
            (void)val;
            if (yyjson_is_str(key)) {
                out.subagents.push_back(clicfg_str_val(key));
            }
        }
    }

    out.has_tools = has_tools || has_allowed;

    // Effective requested list: allowed_tools wins over tools; tools replaces
    // the inherited default list; otherwise the built-in default agent applies.
    //
    // Fix (S6): the default list is requested through its "<module>:<attr>"
    // paths, not through the registry names default_agent_tools() returns -
    // resolve_tool_path() only understands paths and would drop every registry
    // name as an unknown path, leaving an `extend:"default"` manifest with no
    // tools at all.  Going through the paths keeps the resolution (and the
    // exclude_tools filter below) working and yields exactly
    // default_agent_tools() in default_agent_tools() order.
    kimix::vector<kimix::string> requested;
    if (has_allowed) {
        requested = out.allowed_tools;
    } else if (has_tools) {
        requested = out.tools;
    } else {
        for (const std::pair<kimix::string, kimix::string> &entry : agent_tool_table()) {
            requested.push_back(entry.first);
        }
    }

    for (const kimix::string &tool_path : requested) {
        bool excluded = false;
        for (const kimix::string &drop : out.exclude_tools) {
            if (drop == tool_path) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            continue;
        }
        const kimix::string registry = resolve_tool_path(tool_path);
        if (registry.empty()) {
            out.warnings.push_back(kimix::string("dropped unknown tool path '") +
                                   tool_path + "'");
        } else {
            out.enabled_tools.push_back(registry);
        }
    }

    // system_prompt_path: relative -> against the manifest dir; absolute kept.
    if (!raw_prompt.empty()) {
        if (starts_with(raw_prompt, "./") || starts_with(raw_prompt, ".\\")) {
            raw_prompt = raw_prompt.substr(2);
        }
        out.system_prompt_path = clicfg_is_absolute_path(raw_prompt)
                                     ? raw_prompt
                                     : join_path(out.manifest_dir, raw_prompt);
    }
    return true;
}

size_t clicfg_dropped_tool_count(const agent_config &a) {
    size_t dropped = 0;
    for (const kimix::string &w : a.warnings) {
        if (starts_with(w, "dropped unknown tool path")) {
            ++dropped;
        }
    }
    return dropped;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool resolve_model_defaults(kimix::string_view model_name, int64_t &max_context_size,
                            int64_t &max_output) {
    int64_t context = 0;
    int64_t output = k_clicfg_no_output;
    if (!clicfg_resolve_model(model_name, context, output)) {
        return false;
    }
    max_context_size = context;
    max_output = (output > 0) ? output : 0;
    return true;
}

bool load_provider_config(const kimix::string &path, provider_config &out,
                          kimix::string &error) {
    out = provider_config{};
    out.source_path = path;

    kimix::string text;
    kimix::string read_error;
    if (!read_file(path, text, read_error)) {
        error = kimix::string("cannot read provider config '") + path +
                "': " + read_error;
        return false;
    }
    yyjson_doc *doc =
        yyjson_read_opts(text.data(), text.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        error = kimix::string("invalid JSON in provider config '") + path + "'";
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (!yyjson_is_obj(root)) {
        error = kimix::string("provider config '") + path + "' must be a JSON object";
    } else {
        ok = clicfg_parse_provider(path, root, out, error);
    }
    yyjson_doc_free(doc);
    if (!ok) {
        return false;
    }

    // kimix/utils/config.py::_load_and_set_provider: "Provider model: ...".
    print_debug(kimix::string("Provider model: ") +
                (out.model.empty() ? kimix::string("None") : out.model));

    // env entries are applied to the process environment after a good load.
    for (const std::pair<kimix::string, kimix::string> &kv : out.env) {
        if (!kv.first.empty()) {
            set_env(kv.first.c_str(), kv.second);
        }
    }
    return true;
}

bool load_agent_config(const kimix::string &path, agent_config &out,
                       kimix::string &error) {
    out = agent_config{};
    out.manifest_path = path;
    out.manifest_dir = parent_path(absolute_path(path));

    kimix::string text;
    kimix::string read_error;
    if (!read_file(path, text, read_error)) {
        error = kimix::string("cannot read agent manifest '") + path +
                "': " + read_error;
        return false;
    }
    yyjson_doc *doc =
        yyjson_read_opts(text.data(), text.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        error = kimix::string("invalid JSON in agent manifest '") + path + "'";
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (!yyjson_is_obj(root)) {
        error = kimix::string("agent manifest '") + path + "' must be a JSON object";
    } else {
        ok = clicfg_parse_agent(path, root, out, error);
    }
    yyjson_doc_free(doc);
    return ok;
}

kimix::llm::Config to_llm_config(const provider_config &p) {
    kimix::llm::Config cfg;
    cfg.model = p.model;
    cfg.url = p.base_url;
    cfg.api_key = p.api_key;
    cfg.type = p.provider_family;
    cfg.thinking_effort = p.thinking_effort;
    cfg.max_tokens = static_cast<int32_t>(p.max_tokens);
    cfg.max_context_size = static_cast<int32_t>(p.max_context_size);
    cfg.show_thinking_stream = p.show_thinking_stream;
    return cfg;
}

kimix::string provider_report(const provider_config &p) {
    kimix::string out;
    out += "ProviderConfig: ";
    out += p.source_path.empty() ? kimix::string("(none)") : p.source_path;
    out += "\n";
    clicfg_kv(out, "model", p.model);
    clicfg_kv(out, "type", p.type);
    clicfg_kv(out, "family", p.provider_family);
    clicfg_kv(out, "base_url", p.base_url);
    clicfg_kv(out, "api_key", p.api_key.empty() ? kimix::string("absent")
                                                : kimix::string("present"));
    clicfg_kv(out, "reasoning_key", p.reasoning_key);
    clicfg_kv(out, "thinking_effort", p.thinking_effort);
    clicfg_kv(out, "show_thinking_stream",
              p.show_thinking_stream ? kimix::string("true") : kimix::string("false"));

    kimix::string ctx = clicfg_i64(p.max_context_size);
    ctx += p.max_context_size_explicit ? " (explicit)" : " (from model defaults)";
    clicfg_kv(out, "max_context_size", ctx);

    kimix::string tokens = clicfg_i64(p.max_tokens);
    if (p.max_tokens_explicit) {
        tokens += " (explicit)";
    } else if (p.max_context_size_explicit) {
        tokens += " (derived: max_context_size / 4)";
    } else {
        tokens += " (from model defaults)";
    }
    clicfg_kv(out, "max_tokens", tokens);

    clicfg_kv(out, "capabilities",
              p.capabilities.empty() ? kimix::string("(none)")
                                     : clicfg_join(p.capabilities, ", "));
    clicfg_kv(out, "custom_headers", clicfg_i64((int64_t)p.custom_headers.size()));
    clicfg_kv(out, "env", clicfg_i64((int64_t)p.env.size()));
    clicfg_kv(out, "has_oauth", p.has_oauth ? kimix::string("true")
                                            : kimix::string("false"));

    kimix::string services = "search=";
    services += p.search.base_url.empty() ? "absent" : "present";
    services += " fetch=";
    services += p.fetch.base_url.empty() ? "absent" : "present";
    clicfg_kv(out, "services", services);

    clicfg_kv(out, "warnings", clicfg_i64((int64_t)p.warnings.size()));
    for (const kimix::string &w : p.warnings) {
        out += "    - ";
        out += w;
        out += "\n";
    }
    return out;
}

kimix::string agent_report(const agent_config &a) {
    kimix::string out;
    out += "AgentConfig: ";
    out += a.manifest_path.empty() ? kimix::string("(built-in default agent)")
                                   : a.manifest_path;
    out += "\n";

    clicfg_kv(out, "name", a.name.empty() ? kimix::string("(inherit)") : a.name);
    clicfg_kv(out, "extend", a.extend.empty() ? kimix::string("default (built-in)")
                                              : a.extend);
    clicfg_kv(out, "model", a.model.empty() ? kimix::string("(inherit)") : a.model);
    clicfg_kv(out, "system_prompt_path",
              a.system_prompt_path.empty() ? kimix::string("(inherited from default)")
                                           : a.system_prompt_path);

    const size_t dropped = clicfg_dropped_tool_count(a);
    const size_t requested = a.enabled_tools.size() + dropped;
    clicfg_kv(out, "tools requested", clicfg_i64((int64_t)requested));
    clicfg_kv(out, "tools enabled", clicfg_i64((int64_t)a.enabled_tools.size()));
    clicfg_kv(out, "tools dropped", clicfg_i64((int64_t)dropped));
    clicfg_kv(out, "enabled", a.enabled_tools.empty() ? kimix::string("(none)")
                                                      : clicfg_join(a.enabled_tools, ", "));
    if (!a.subagents.empty()) {
        clicfg_kv(out, "subagents", clicfg_join(a.subagents, ", "));
    }

    clicfg_kv(out, "warnings", clicfg_i64((int64_t)a.warnings.size()));
    for (const kimix::string &w : a.warnings) {
        out += "    - ";
        out += w;
        out += "\n";
    }
    return out;
}

} // namespace kimix::cli
