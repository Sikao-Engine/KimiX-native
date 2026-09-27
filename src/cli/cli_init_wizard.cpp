// cli/cli_init_wizard.cpp - the /init interactive provider wizard (see
// cli_init_wizard.h).  Port of kimix/cli_impl/init.py; every prompt, default,
// validation message and template is byte-traceable to that file.

#include "cli/cli_init_wizard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include "cli/cli_common.h"
#include "cli/cli_print.h"

#if defined(KIMIX_PLATFORM_WINDOWS)
#include <windows.h>
// ShellExecuteW ("os.startfile" parity in open_with_default_app). The shell32
// link is declared per-platform in src/cli/xmake.lua (add_syslinks) - a
// `#pragma comment(lib, ...)` here would be extracted by xmake on EVERY
// platform and break the Linux link with "cannot find -lshell32".
#endif

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Templates (init.py:12-49, verbatim JSON)
// ---------------------------------------------------------------------------

constexpr const char *k_deepseek_template =
    "{\n"
    "    \"model\": \"deepseek-v4-pro\",\n"
    "    \"max_context_size\": 1048576,\n"
    "    \"capabilities\": [\"thinking\"],\n"
    "    \"url\": \"https://api.deepseek.com/\",\n"
    "    \"type\": \"openai_legacy\",\n"
    "    \"max_tokens\": 384000,\n"
    "    \"thinking_effort\": \"max\"\n"
    "}";

constexpr const char *k_minimax_template =
    "{\n"
    "    \"model\": \"minimax-m2.7\",\n"
    "    \"max_context_size\": 204800,\n"
    "    \"capabilities\": [\"thinking\"],\n"
    "    \"url\": \"https://api.minimaxi.com/anthropic\",\n"
    "    \"type\": \"anthropic\",\n"
    "    \"max_tokens\": 128000,\n"
    "    \"thinking_effort\": \"max\"\n"
    "}";

constexpr const char *k_kimi_template =
    "{\n"
    "    \"model\": \"kimi-for-coding\",\n"
    "    \"max_context_size\": 1048576,\n"
    "    \"capabilities\": [\"thinking\", \"image_in\"],\n"
    "    \"url\": \"https://api.kimi.com/coding/v1\",\n"
    "    \"type\": \"kimi\",\n"
    "    \"max_tokens\": 131072,\n"
    "    \"show_thinking_stream\": true,\n"
    "    \"thinking_effort\": \"max\"\n"
    "}";

// init.py:53-72: the validated option tables.
const char *const k_valid_types[] = {
    "kimi", "openai_legacy", "openai_responses",
    "anthropic", "google_genai", "gemini", "vertexai",
};
const char *const k_valid_efforts[] = {"off", "low", "medium", "high", "xhigh", "max"};
const char *const k_valid_capabilities[] = {"thinking", "always_thinking", "image_in",
                                            "video_in"};
struct initi_context_size_option {
    const char *label;
    int64_t value;
};
constexpr initi_context_size_option k_context_size_options[] = {
    {"128k", 128000}, {"256k", 256000}, {"512k", 512000}, {"1M", 1000000},
};

kimix::string initi_join_options(const char *const *items, size_t count) {
    kimix::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += items[i];
    }
    return out;
}

bool initi_in_list(const char *const *items, size_t count, kimix::string_view value) {
    for (size_t i = 0; i < count; ++i) {
        if (value == items[i]) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The wizard state: a mutable JSON document (the reference's config dict)
// ---------------------------------------------------------------------------

// The merge step (init.py:275-284): the loaded config wins, template keys
// only fill holes (one level of dict merge).
void initi_merge_missing(yyjson_mut_doc *doc, yyjson_mut_val *into,
                         yyjson_val *from) {
    if (from == nullptr || !yyjson_is_obj(from)) {
        return;
    }
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(from, idx, max, key, val) {
        const kimix::string name(yyjson_get_str(key), yyjson_get_len(key));
        if (yyjson_mut_obj_get(into, name.c_str()) != nullptr) {
            continue;
        }
        yyjson_mut_val *cloned = yyjson_val_mut_copy(doc, val);
        if (cloned != nullptr) {
            yyjson_mut_obj_add(into, yyjson_mut_strncpy(doc, name.c_str(), name.size()),
                               cloned);
        }
    }
}

// Pretty writer: orjson OPT_INDENT_2 (two-space indent, no trailing newline).
bool initi_dump_pretty(yyjson_mut_doc *doc, kimix::string &out) {
    size_t len = 0;
    char *json = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY_TWO_SPACES,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    if (json == nullptr) {
        return false;
    }
    out.assign(json, len);
    mi_free(json);
    return true;
}

} // namespace

const char *const k_init_default_config_template = k_kimi_template;

void open_with_default_app(const kimix::string &path) {
    // init.py:328-333 (best effort, like the reference's bare subprocess.run).
    // KIMIX_CLI_SKIP_OPEN=1 is the test seam: /init and /plan run in-process
    // under the Boost.UT suites, where an OS file reveal must not fire.
    kimix::string skip;
    if (get_env("KIMIX_CLI_SKIP_OPEN", skip) && trim(skip) == "1") {
        return;
    }
#if defined(KIMIX_PLATFORM_WINDOWS)
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide <= 0) {
        return;
    }
    kimix::vector<wchar_t> buf(static_cast<size_t>(wide), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, buf.data(), wide) <= 0) {
        return;
    }
    // os.startfile: SHELLEXECUTEINFO / ShellExecuteW "open".
    ::ShellExecuteW(nullptr, L"open", buf.data(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const std::string cmd = "open \"" + std::string(path.c_str()) + "\"";
    (void)std::system(cmd.c_str());
#else
    const std::string cmd = "xdg-open \"" + std::string(path.c_str()) + "\"";
    (void)std::system(cmd.c_str());
#endif
}

bool run_init_wizard(app_context &app, const kimix::string &config_path,
                     bool initialize, bool open_after, const init_input_fn &input) {
    // `_ask(prompt, default)` (init.py:91-94): the prompt line ends with
    // " [default]: " and an empty answer picks the default.
    auto ask = [&input](kimix::string_view prompt, kimix::string_view fallback,
                        kimix::string &value) -> int {
        // 1 = answer, 0 = default, -1 = EOF/abort.
        kimix::string line;
        kimix::string full(prompt);
        full += " [";
        full += fallback;
        full += "]: ";
        if (!input(full, line)) {
            return -1;
        }
        const kimix::string trimmed = kimix::string(trim(line));
        if (trimmed.empty()) {
            value.assign(fallback.data(), fallback.size());
            return 0;
        }
        value = trimmed;
        return 1;
    };

    // ---- template selection (init.py:97-104, 268-274) -----------------------
    const char *template_json = k_kimi_template;
    if (!initialize) {
        // The boot gate: `input('default config not found, initialize? you
        // can use /init any time. (y/n)')` - empty answer == initialise.
        kimix::string line;
        if (!input("default config not found, initialize? you can use /init any "
                   "time. (y/n)",
                   line)) {
            return false;
        }
        const kimix::string lowered = to_lower_ascii(trim(line));
        if (!(lowered.empty() || lowered == "y")) {
            return false; // declined: the caller keeps going without a config
        }
    } else {
        kimix::string line;
        // init.py:98: the template prompt is printed raw (no " [default]: "
        // wrapper); the input function writes it without a trailing newline.
        if (!input("Select provider template ('kimi', 'deepseek' or 'minimax') [kimi]: ",
                   line)) {
            return false;
        }
        const kimix::string choice = to_lower_ascii(trim(line));
        if (choice == "deepseek") {
            template_json = k_deepseek_template;
        } else if (choice == "minimax") {
            template_json = k_minimax_template;
        }
    }

    // ---- load the existing config + merge the template over it --------------
    yyjson_doc *existing = nullptr;
    {
        kimix::string text, error;
        if (read_file(config_path, text, error)) {
            existing = yyjson_read_opts(const_cast<char *>(text.data()), text.size(), 0,
                                        &kimix::llm::kYYJsonAlcMi, nullptr);
        }
    }
    yyjson_doc *templ = yyjson_read_opts(const_cast<char *>(template_json),
                                         std::strlen(template_json), 0,
                                         &kimix::llm::kYYJsonAlcMi, nullptr);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (existing != nullptr) {
        // Copy the loaded config in (loaded values win).
        yyjson_val *eroot = yyjson_doc_get_root(existing);
        if (yyjson_is_obj(eroot)) {
            size_t idx, max;
            yyjson_val *key, *val;
            yyjson_obj_foreach(eroot, idx, max, key, val) {
                const kimix::string name(yyjson_get_str(key), yyjson_get_len(key));
                if (yyjson_mut_obj_get(root, name.c_str()) == nullptr) {
                    yyjson_mut_val *cloned = yyjson_val_mut_copy(doc, val);
                    if (cloned != nullptr) {
                        yyjson_mut_obj_add(root,
                                           yyjson_mut_strncpy(doc, name.c_str(), name.size()),
                                           cloned);
                    }
                }
            }
        }
    }
    if (templ != nullptr) {
        initi_merge_missing(doc, root, yyjson_doc_get_root(templ));
    }

    auto set_str = [&](const char *key, const kimix::string &value) {
        yyjson_mut_obj_put(root, yyjson_mut_str(doc, key),
                           yyjson_mut_strncpy(doc, value.c_str(), value.size()));
    };
    auto set_int = [&](const char *key, int64_t value) {
        yyjson_mut_obj_put(root, yyjson_mut_str(doc, key), yyjson_mut_sint(doc, value));
    };
    auto get_str = [&](const char *key, const char *fallback) -> kimix::string {
        yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
        if (v != nullptr && yyjson_mut_is_str(v)) {
            return kimix::string(yyjson_mut_get_str(v), yyjson_mut_get_len(v));
        }
        return kimix::string(fallback);
    };
    auto get_int = [&](const char *key, int64_t fallback) -> int64_t {
        yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
        // yyjson tags positive JSON integers as UINT: accept every numeric
        // subtype here.
        if (v != nullptr && yyjson_mut_is_num(v)) {
            return static_cast<int64_t>(yyjson_mut_get_num(v));
        }
        return fallback;
    };

    // ---- the validated question walk (init.py:285-321) ----------------------
    if (initialize) {
        kimix::string value;
        // Model name.
        int rc = ask("Enter model name", get_str("model", "kimi-for-coding"), value);
        if (rc < 0) {
            print_warning("keyboard interruped.");
            return false;
        }
        set_str("model", value);

        // Model type (validated, re-prompt loop).
        const kimix::string types =
            initi_join_options(k_valid_types, sizeof(k_valid_types) / sizeof(*k_valid_types));
        for (;;) {
            rc = ask("Enter model type (" + types + ")", get_str("type", "kimi"), value);
            if (rc < 0) {
                print_warning("keyboard interruped.");
                return false;
            }
            if (initi_in_list(k_valid_types, sizeof(k_valid_types) / sizeof(*k_valid_types),
                              value)) {
                set_str("type", value);
                break;
            }
            print_warning("Invalid type '" + value + "', please choose from: " + types);
        }

        // API key (init.py:124-138: the env notices + the two-step skip).
        {
            bool skip = false;
            kimix::string env_key;
            if (get_env("KIMI_API_KEY", env_key) && !env_key.empty()) {
                print_info("API key already set in environment variable KIMI_API_KEY.");
                skip = true;
            }
            if (get_env("KIMIX_API_KEY", env_key) && !env_key.empty()) {
                print_info("API key already set in environment variable KIMIX_API_KEY.");
                skip = true;
            }
            kimix::string line;
            bool have = input("Enter API key (usually required, or set "
                              "KIMI_API_KEY/KIMIX_API_KEY env var): ",
                              line);
            if (!have) {
                print_warning("keyboard interruped.");
                return false;
            }
            value = kimix::string(trim(line));
            if (!skip && value.empty()) {
                print_warning("API key is usually required. Press Enter again to skip.");
                if (!input("Enter API key: ", line)) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                value = kimix::string(trim(line));
            }
            set_str("api_key", value);
        }

        // Context size (128k/256k/512k/1M or a number > 1).
        constexpr size_t kCtxCount = sizeof(k_context_size_options) / sizeof(*k_context_size_options);
        {
            kimix::string ctx_default = "1M";
            const int64_t current = get_int("max_context_size", 0);
            for (const auto &opt : k_context_size_options) {
                if (opt.value == current) {
                    ctx_default = opt.label;
                    break;
                }
            }
            kimix::string options_str;
            for (size_t i = 0; i < kCtxCount; ++i) {
                if (i > 0) {
                    options_str += ", ";
                }
                options_str += k_context_size_options[i].label;
            }
            int64_t chosen = 0;
            for (;;) {
                const int rc2 =
                    ask("Enter model context size (" + options_str + " or a number)",
                        ctx_default, value);
                if (rc2 < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                bool matched = false;
                for (const auto &opt : k_context_size_options) {
                    if (value == opt.label) {
                        chosen = opt.value;
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    break;
                }
                char *end = nullptr;
                const long long num = std::strtoll(value.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || value.empty()) {
                    print_warning("Invalid size '" + value +
                                  "', please choose from: " + options_str +
                                  " or enter a specific number");
                    continue;
                }
                if (num <= 1) {
                    print_warning(kimix::format("Context size must be larger than 1, got {}",
                                                num));
                    continue;
                }
                chosen = static_cast<int64_t>(num);
                break;
            }
            set_int("max_context_size", chosen);

            // Max tokens (init.py:198-213) - bounded by context - reserved.
            int64_t reserved = 50000;
            yyjson_mut_val *lc = yyjson_mut_obj_get(root, "loop_control");
            if (lc != nullptr && yyjson_mut_is_obj(lc)) {
                yyjson_mut_val *r = yyjson_mut_obj_get(lc, "reserved_context_size");
                if (r != nullptr && yyjson_mut_is_sint(r)) {
                    reserved = yyjson_mut_get_sint(r);
                }
            }
            const int64_t max_allowed = chosen - reserved;
            const int64_t tokens_default = get_int("max_tokens", 128000);
            const kimix::string tokens_prompt =
                "Enter max tokens (max " + kimix::format("{}", max_allowed) + ")";
            for (;;) {
                const int rc2 = ask(tokens_prompt, kimix::format("{}", tokens_default), value);
                if (rc2 < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                char *end = nullptr;
                const long long num = std::strtoll(value.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || value.empty()) {
                    print_warning("Invalid number '" + value + "', using default " +
                                  kimix::format("{}", tokens_default));
                    set_int("max_tokens", tokens_default);
                    break;
                }
                if (num <= 0 || num > max_allowed) {
                    print_warning("Value " + kimix::format("{}", num) + " out of range, using default " +
                                  kimix::format("{}", tokens_default));
                    set_int("max_tokens", tokens_default);
                    break;
                }
                set_int("max_tokens", static_cast<int64_t>(num));
                break;
            }

            // Thinking effort.
            const kimix::string efforts = initi_join_options(
                k_valid_efforts, sizeof(k_valid_efforts) / sizeof(*k_valid_efforts));
            for (;;) {
                const int rc2 =
                    ask("Enter thinking effort (" + efforts + ")",
                        get_str("thinking_effort", "low"), value);
                if (rc2 < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                if (initi_in_list(k_valid_efforts,
                                  sizeof(k_valid_efforts) / sizeof(*k_valid_efforts), value)) {
                    set_str("thinking_effort", value);
                    break;
                }
                print_warning("Invalid effort '" + value + "', please choose from: " + efforts);
            }

            // Capabilities (multi-value, 'none' for empty).
            constexpr size_t kCapCount =
                sizeof(k_valid_capabilities) / sizeof(*k_valid_capabilities);
            const kimix::string caps_str = initi_join_options(k_valid_capabilities, kCapCount);
            kimix::string caps_default;
            {
                yyjson_mut_val *cur = yyjson_mut_obj_get(root, "capabilities");
                kimix::vector<kimix::string> items;
                if (cur != nullptr && yyjson_mut_is_arr(cur)) {
                    size_t i2, n2;
                    yyjson_mut_val *it;
                    yyjson_mut_arr_foreach(cur, i2, n2, it) {
                        if (yyjson_mut_is_str(it)) {
                            items.emplace_back(yyjson_mut_get_str(it), yyjson_mut_get_len(it));
                        }
                    }
                }
                if (items.empty()) {
                    items.push_back("thinking");
                    items.push_back("image_in");
                }
                caps_default = join(items, ", ");
            }
            kimix::vector<kimix::string> caps;
            for (;;) {
                const int rc2 = ask("Enter capabilities (" + caps_str +
                                        "), multiple allowed, 'none' for empty",
                                    caps_default, value);
                if (rc2 < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                if (to_lower_ascii(value) == "none") {
                    caps.clear();
                    break;
                }
                kimix::vector<kimix::string> parts;
                {
                    // replace(",", " ").split()
                    kimix::string spaced = replace_all(value, ",", " ");
                    kimix::string word;
                    for (const char ch : spaced) {
                        if (ch == ' ' || ch == '\t') {
                            if (!word.empty()) {
                                parts.push_back(word);
                                word.clear();
                            }
                        } else {
                            word.push_back(ch);
                        }
                    }
                    if (!word.empty()) {
                        parts.push_back(word);
                    }
                }
                kimix::vector<kimix::string> invalid;
                for (const kimix::string &p : parts) {
                    if (!initi_in_list(k_valid_capabilities, kCapCount, p)) {
                        invalid.push_back(p);
                    }
                }
                if (!invalid.empty()) {
                    print_warning("Invalid capabilities: " + join(invalid, ", ") +
                                  ", please choose from: " + caps_str);
                    continue;
                }
                caps = parts;
                break;
            }
            // always_thinking supersedes thinking (init.py:310-311); the key
            // is written even when the list is empty (config["capabilities"]
            // = capabilities in init.py:312).
            {
                bool has_always = false;
                for (const kimix::string &c : caps) {
                    if (c == "always_thinking") {
                        has_always = true;
                    }
                }
                if (has_always) {
                    kimix::vector<kimix::string> filtered;
                    for (const kimix::string &c : caps) {
                        if (c != "thinking") {
                            filtered.push_back(c);
                        }
                    }
                    caps = filtered;
                }
                yyjson_mut_val *arr = yyjson_mut_arr(doc);
                for (const kimix::string &c : caps) {
                    yyjson_mut_arr_append(arr,
                                          yyjson_mut_strncpy(doc, c.c_str(), c.size()));
                }
                yyjson_mut_obj_put(root, yyjson_mut_str(doc, "capabilities"), arr);
            }

            // URL.
            {
                const int rc2 =
                    ask("Enter model URL", get_str("url", "https://api.kimi.com/coding/v1"),
                        value);
                if (rc2 < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                set_str("url", value);
            }

            // ---- optional sub-provider block (init.py:216-265) -----------------
            print_info("Configure a sub-agent provider? (y/n)");
            kimix::string sub_answer;
            if (!input("", sub_answer)) {
                print_warning("keyboard interruped.");
                return false;
            }
            if (to_lower_ascii(trim(sub_answer)) == "y") {
                // The defaults come from the existing sub_provider dict.
                yyjson_mut_val *prev = yyjson_mut_obj_get(root, "sub_provider");
                auto sub_default_str = [&](const char *key, const char *fallback) {
                    if (prev != nullptr && yyjson_mut_is_obj(prev)) {
                        yyjson_mut_val *v = yyjson_mut_obj_get(prev, key);
                        if (v != nullptr && yyjson_mut_is_str(v)) {
                            return kimix::string(yyjson_mut_get_str(v), yyjson_mut_get_len(v));
                        }
                    }
                    return kimix::string(fallback);
                };
                auto sub_default_int = [&](const char *key, int64_t fallback) {
                    if (prev != nullptr && yyjson_mut_is_obj(prev)) {
                        yyjson_mut_val *v = yyjson_mut_obj_get(prev, key);
                        if (v != nullptr && yyjson_mut_is_sint(v)) {
                            return yyjson_mut_get_sint(v);
                        }
                    }
                    return fallback;
                };
                print_success("--- Sub-provider configuration ---");
                rc = ask("Enter model name", sub_default_str("model", "kimi-for-coding"),
                         value);
                if (rc < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                kimix::string sub_model = value;
                const kimix::string types2 = initi_join_options(
                    k_valid_types, sizeof(k_valid_types) / sizeof(*k_valid_types));
                kimix::string sub_type;
                for (;;) {
                    rc = ask("Enter model type (" + types2 + ")",
                             sub_default_str("type", "kimi"), value);
                    if (rc < 0) {
                        print_warning("keyboard interruped.");
                        return false;
                    }
                    if (initi_in_list(k_valid_types,
                                      sizeof(k_valid_types) / sizeof(*k_valid_types), value)) {
                        sub_type = value;
                        break;
                    }
                    print_warning("Invalid type '" + value + "', please choose from: " + types2);
                }
                rc = ask("Enter model URL", sub_default_str("url", "https://api.kimi.com/coding/v1"),
                         value);
                if (rc < 0) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                kimix::string sub_url = value;
                kimix::string sub_key_line;
                if (!input("Enter API key (usually required, or set "
                           "KIMI_API_KEY/KIMIX_API_KEY env var): ",
                           sub_key_line)) {
                    print_warning("keyboard interruped.");
                    return false;
                }
                kimix::string sub_key = kimix::string(trim(sub_key_line));
                constexpr size_t kCtxCount2 =
                    sizeof(k_context_size_options) / sizeof(*k_context_size_options);
                kimix::string sub_ctx_default = "1M";
                {
                    const int64_t cur = sub_default_int("max_context_size", 0);
                    for (const auto &opt : k_context_size_options) {
                        if (opt.value == cur) {
                            sub_ctx_default = opt.label;
                            break;
                        }
                    }
                }
                kimix::string options_str2;
                for (size_t i = 0; i < kCtxCount2; ++i) {
                    if (i > 0) {
                        options_str2 += ", ";
                    }
                    options_str2 += k_context_size_options[i].label;
                }
                int64_t sub_ctx = 0;
                for (;;) {
                    const int rc2 =
                        ask("Enter model context size (" + options_str2 + " or a number)",
                            sub_ctx_default, value);
                    if (rc2 < 0) {
                        print_warning("keyboard interruped.");
                        return false;
                    }
                    bool matched = false;
                    for (const auto &opt : k_context_size_options) {
                        if (value == opt.label) {
                            sub_ctx = opt.value;
                            matched = true;
                            break;
                        }
                    }
                    if (matched) {
                        break;
                    }
                    char *end = nullptr;
                    const long long num = std::strtoll(value.c_str(), &end, 10);
                    if (end == nullptr || *end != '\0' || value.empty()) {
                        print_warning("Invalid size '" + value +
                                      "', please choose from: " + options_str2 +
                                      " or enter a specific number");
                        continue;
                    }
                    if (num <= 1) {
                        print_warning(kimix::format(
                            "Context size must be larger than 1, got {}", num));
                        continue;
                    }
                    sub_ctx = static_cast<int64_t>(num);
                    break;
                }
                const kimix::string efforts2 = initi_join_options(
                    k_valid_efforts, sizeof(k_valid_efforts) / sizeof(*k_valid_efforts));
                kimix::string sub_effort;
                for (;;) {
                    rc = ask("Enter thinking effort (" + efforts2 + ")",
                             sub_default_str("thinking_effort", "low"), value);
                    if (rc < 0) {
                        print_warning("keyboard interruped.");
                        return false;
                    }
                    if (initi_in_list(k_valid_efforts,
                                      sizeof(k_valid_efforts) / sizeof(*k_valid_efforts),
                                      value)) {
                        sub_effort = value;
                        break;
                    }
                    print_warning("Invalid effort '" + value +
                                  "', please choose from: " + efforts2);
                }
                // Capabilities (default ("thinking",) for the sub-provider).
                constexpr size_t kCapCount2 =
                    sizeof(k_valid_capabilities) / sizeof(*k_valid_capabilities);
                const kimix::string caps_str2 =
                    initi_join_options(k_valid_capabilities, kCapCount2);
                kimix::vector<kimix::string> sub_caps;
                for (;;) {
                    kimix::string sub_caps_default = "thinking";
                    if (prev != nullptr && yyjson_mut_is_obj(prev)) {
                        yyjson_mut_val *cur = yyjson_mut_obj_get(prev, "capabilities");
                        if (cur != nullptr && yyjson_mut_is_arr(cur)) {
                            kimix::vector<kimix::string> items;
                            size_t i2, n2;
                            yyjson_mut_val *it;
                            yyjson_mut_arr_foreach(cur, i2, n2, it) {
                                if (yyjson_mut_is_str(it)) {
                                    items.emplace_back(yyjson_mut_get_str(it),
                                                       yyjson_mut_get_len(it));
                                }
                            }
                            if (!items.empty()) {
                                sub_caps_default = join(items, ", ");
                            }
                        }
                    }
                    const int rc2 = ask("Enter capabilities (" + caps_str2 +
                                            "), multiple allowed, 'none' for empty",
                                        sub_caps_default, value);
                    if (rc2 < 0) {
                        print_warning("keyboard interruped.");
                        return false;
                    }
                    if (to_lower_ascii(value) == "none") {
                        sub_caps.clear();
                        break;
                    }
                    kimix::vector<kimix::string> parts;
                    {
                        kimix::string spaced = replace_all(value, ",", " ");
                        kimix::string word;
                        for (const char ch : spaced) {
                            if (ch == ' ' || ch == '\t') {
                                if (!word.empty()) {
                                    parts.push_back(word);
                                    word.clear();
                                }
                            } else {
                                word.push_back(ch);
                            }
                        }
                        if (!word.empty()) {
                            parts.push_back(word);
                        }
                    }
                    kimix::vector<kimix::string> invalid;
                    for (const kimix::string &p : parts) {
                        if (!initi_in_list(k_valid_capabilities, kCapCount2, p)) {
                            invalid.push_back(p);
                        }
                    }
                    if (!invalid.empty()) {
                        print_warning("Invalid capabilities: " + join(invalid, ", ") +
                                      ", please choose from: " + caps_str2);
                        continue;
                    }
                    sub_caps = parts;
                    break;
                }
                {
                    bool has_always = false;
                    for (const kimix::string &c : sub_caps) {
                        if (c == "always_thinking") {
                            has_always = true;
                        }
                    }
                    if (has_always) {
                        kimix::vector<kimix::string> filtered;
                        for (const kimix::string &c : sub_caps) {
                            if (c != "thinking") {
                                filtered.push_back(c);
                            }
                        }
                        sub_caps = filtered;
                    }
                }
                // Max tokens (reserved fixed at 50000 for the sub-provider).
                const int64_t sub_tokens_default = sub_default_int("max_tokens", 128000);
                const int64_t sub_max_allowed = sub_ctx - 50000;
                int64_t sub_tokens = sub_tokens_default;
                for (;;) {
                    const int rc2 =
                        ask("Enter max tokens (max " + kimix::format("{}", sub_max_allowed) +
                            ")",
                            kimix::format("{}", sub_tokens_default), value);
                    if (rc2 < 0) {
                        print_warning("keyboard interruped.");
                        return false;
                    }
                    char *end = nullptr;
                    const long long num = std::strtoll(value.c_str(), &end, 10);
                    if (end == nullptr || *end != '\0' || value.empty()) {
                        print_warning("Invalid number '" + value + "', using default " +
                                      kimix::format("{}", sub_tokens_default));
                        break;
                    }
                    if (num <= 0 || num > sub_max_allowed) {
                        print_warning("Value " + kimix::format("{}", num) +
                                      " out of range, using default " +
                                      kimix::format("{}", sub_tokens_default));
                        break;
                    }
                    sub_tokens = static_cast<int64_t>(num);
                    break;
                }
                print_success("Sub-provider configuration complete.");
                // Assemble the sub_provider object (deleting any previous one).
                yyjson_mut_obj_put(root, yyjson_mut_str(doc, "sub_provider"), nullptr);
                yyjson_mut_val *sub = yyjson_mut_obj(doc);
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "model"),
                                   yyjson_mut_strncpy(doc, sub_model.c_str(), sub_model.size()));
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "type"),
                                   yyjson_mut_strncpy(doc, sub_type.c_str(), sub_type.size()));
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "url"),
                                   yyjson_mut_strncpy(doc, sub_url.c_str(), sub_url.size()));
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "api_key"),
                                   yyjson_mut_strncpy(doc, sub_key.c_str(), sub_key.size()));
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "max_context_size"),
                                   yyjson_mut_sint(doc, sub_ctx));
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "thinking_effort"),
                                   yyjson_mut_strncpy(doc, sub_effort.c_str(),
                                                      sub_effort.size()));
                yyjson_mut_val *cap_arr = yyjson_mut_arr(doc);
                for (const kimix::string &c : sub_caps) {
                    yyjson_mut_arr_append(cap_arr,
                                          yyjson_mut_strncpy(doc, c.c_str(), c.size()));
                }
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "capabilities"), cap_arr);
                yyjson_mut_obj_add(sub, yyjson_mut_str(doc, "max_tokens"),
                                   yyjson_mut_sint(doc, sub_tokens));
                yyjson_mut_obj_add(root, yyjson_mut_str(doc, "sub_provider"), sub);
            } else if (yyjson_mut_obj_get(root, "sub_provider") != nullptr) {
                // init.py:320-321: a declined question drops a stale entry.
                yyjson_mut_obj_put(root, yyjson_mut_str(doc, "sub_provider"), nullptr);
            }
        }
    }

    // ---- save (init.py:86-88, 325-327) --------------------------------------
    kimix::string json;
    if (!initi_dump_pretty(doc, json)) {
        print_error("failed to serialize the config");
        yyjson_mut_doc_free(doc);
        yyjson_doc_free(templ);
        yyjson_doc_free(existing);
        return false;
    }
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(templ);
    yyjson_doc_free(existing);
    kimix::string error;
    if (!write_file(config_path, json, error)) {
        print_error(error);
        return false;
    }
    if (initialize) {
        print_success("Configuration saved successfully to " + config_path + ".");
    }
    if (open_after) {
        open_with_default_app(config_path);
    }
    return true;
}

} // namespace kimix::cli
