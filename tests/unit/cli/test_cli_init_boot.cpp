// test_cli_init_boot.cpp - H1/H2/H5/H7/H11/E7/I4: the /init wizard, the
// boot-time auto-init, the sub-provider config parse, the boot diagnostics
// and the --no_think wire plumbing.
//
// Covered:
//   * run_init_wizard over a scripted input function (H1): the kimi template
//     default, the validated question walk (model type / context size /
//     thinking effort / capabilities retry loops, the max-tokens range check),
//     the "keyboard interruped." abort, the initialize=false boot gate
//     ("default config not found, initialize? ..."), the sub-provider block
//     and the "Configuration saved successfully to {path}." line.
//   * The non-TTY boot auto-init (args.py:86-96) driven through cli_main with
//     fd 0 redirected to a file: the kimi template is written with the env
//     api_key and the run continues into --dry-run with exit 0.
//   * H2: sub_provider / sub_providers parse - inherit defaults, the
//     required-key validation, role normalisation, duplicate-role debug and
//     the pick-main-when-root-has-no-model promotion.
//   * H5: .kimix/mcp.json diagnostics ("Loaded MCP config from", the
//     non-object warning and the parse-failure warning).
//   * H7: print_error writes to stdout, not stderr.
//   * H11: "Native acceleration enabled." + the KIMIX_NATIVE=0 opt-out.
//   * E7: --no_think / default_thinking=false reach llm::Config.
//   * I4: --help lists the Python-only front ends; each refused subcommand
//     prints the front-end list and exits 3.
//
// Framework: Boost.UT (tests/ut/ut.hpp); every test body lives in a
// main()-scope "name"_test lambda.

#include "ut/ut.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <core/kimix_core.h>

#include "llm/llm.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include "cli/cli_app.h"
#include "cli/cli_args.h"
#include "cli/cli_common.h"
#include "cli/cli_config.h"
#include "cli/cli_init_wizard.h"
#include "cli/cli_print.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace cli = kimix::cli;
using namespace boost::ut;

bool has_substr(kimix::string_view haystack, kimix::string_view needle) {
    return cli::contains(haystack, needle);
}

kimix::string ws_dir(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

int test_dup(int fd) {
#if defined(_WIN32)
    return _dup(fd);
#else
    return dup(fd);
#endif
}
int test_dup2(int from, int to) {
#if defined(_WIN32)
    return _dup2(from, to);
#else
    return dup2(from, to);
#endif
}
int test_fileno(std::FILE *stream) {
#if defined(_WIN32)
    return _fileno(stream);
#else
    return fileno(stream);
#endif
}
void test_close_fd(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
}

// Redirect fd 1+2 into one file (the shared capture from test_cli.cpp).
class output_capture {
public:
    bool begin(const kimix::string &path) {
        path_ = path;
        std::fflush(stdout);
        std::fflush(stderr);
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr) {
            return false;
        }
        saved_out_ = test_dup(1);
        saved_err_ = test_dup(2);
        test_dup2(test_fileno(file_), 1);
        test_dup2(test_fileno(file_), 2);
        return true;
    }
    kimix::string end() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (saved_out_ >= 0) {
            test_dup2(saved_out_, 1);
            test_close_fd(saved_out_);
            saved_out_ = -1;
        }
        if (saved_err_ >= 0) {
            test_dup2(saved_err_, 2);
            test_close_fd(saved_err_);
            saved_err_ = -1;
        }
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        kimix::string text;
        kimix::string error;
        cli::read_file(path_, text, error);
        return text;
    }

private:
    kimix::string path_;
    std::FILE *file_ = nullptr;
    int saved_out_ = -1;
    int saved_err_ = -1;
};

// Redirect fd 0 to a file (stdin stops being a console -> the non-TTY boot).
class stdin_redirect {
public:
    bool begin(const kimix::string &path) {
        file_ = std::fopen(path.c_str(), "rb");
        if (file_ == nullptr) {
            return false;
        }
        saved_ = test_dup(0);
        test_dup2(test_fileno(file_), 0);
        return true;
    }
    void end() {
        if (saved_ >= 0) {
            test_dup2(saved_, 0);
            test_close_fd(saved_);
            saved_ = -1;
        }
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    std::FILE *file_ = nullptr;
    int saved_ = -1;
};

// The scripted wizard input: pre-loaded lines; records every prompt so the
// test can assert the question order/wording.
struct scripted_input {
    kimix::vector<kimix::string> lines;
    kimix::vector<kimix::string> prompts;
    size_t pos = 0;

    cli::init_input_fn fn() {
        return [this](kimix::string_view prompt, kimix::string &line) {
            prompts.emplace_back(prompt);
            if (pos >= lines.size()) {
                return false; // EOF -> the "keyboard interruped." path
            }
            line = lines[pos++];
            return true;
        };
    }
};

// Parse a JSON file into a read-only doc and return the root object value.
yyjson_val *load_root(const kimix::string &path) {
    kimix::string text, error;
    if (!cli::read_file(path, text, error)) {
        return nullptr;
    }
    // NOTE: the doc leaks intentionally - the test process is short-lived and
    // yyjson handles are returned across asserts.
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(text.data()), text.size(),
                                       0, &kimix::llm::kYYJsonAlcMi, nullptr);
    return doc ? yyjson_doc_get_root(doc) : nullptr;
}

kimix::string file_text_must(const kimix::string &path) {
    kimix::string out;
    kimix::string error;
    const bool ok = cli::read_file(path, out, error);
    expect(ok) << "read_file(" << path << "): " << error;
    return out;
}

kimix::string str_member(yyjson_val *root, const char *key) {
    if (root == nullptr) {
        return {};
    }
    yyjson_val *v = yyjson_obj_get(root, key);
    if (v == nullptr || !yyjson_is_str(v)) {
        return {};
    }
    return kimix::string(yyjson_get_str(v), yyjson_get_len(v));
}

int64_t int_member(yyjson_val *root, const char *key) {
    if (root == nullptr) {
        return -1;
    }
    yyjson_val *v = yyjson_obj_get(root, key);
    // yyjson tags positive JSON integers as UINT: accept every numeric
    // subtype here.
    return (v != nullptr && yyjson_is_num(v)) ? static_cast<int64_t>(yyjson_get_num(v))
                                              : -1;
}

} // namespace

int main() {
    "wizard_happy_path_writes_config"_test = [] {
        const kimix::string work = ws_dir("cli_boot_wizard_ok");
        const kimix::string path = cli::join_path(work, "default_config.json");
        scripted_input in;
        in.lines = {"",              // template select -> kimi
                    "my-model",      // model name
                    "openai_legacy", // type
                    "key123",        // api key
                    "256k",          // context size
                    "64000",         // max tokens (<= 256000-50000)
                    "low",           // thinking effort
                    "thinking, image_in", // capabilities
                    "https://x.io/v1",    // url
                    "n"};                 // no sub-provider
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "wiz.txt")));
            cli::set_colorful(false);
            cli::app_context app;
            const bool ok = cli::run_init_wizard(app, path, /*initialize=*/true,
                                                 /*open_after=*/false, in.fn());
            out = capture.end();
            cli::set_colorful(true);
            expect(ok);
        }
        expect(has_substr(in.prompts[1], "Enter model name"));
        expect(has_substr(in.prompts[1], "[kimi-for-coding]"));
        expect(has_substr(in.prompts[2], "Enter model type ("));
        expect(has_substr(in.prompts[4], "Enter model context size ("));
        expect(has_substr(in.prompts[6], "Enter thinking effort ("));
        expect(has_substr(in.prompts[7], "Enter capabilities ("));
        expect(has_substr(in.prompts[7], "'none' for empty"));
        expect(has_substr(in.prompts[8], "Enter model URL"));
        expect(has_substr(out, "Configuration saved successfully to " + path + "."));
        yyjson_val *root = load_root(path);
        expect(root != nullptr);
        expect(str_member(root, "model") == kimix::string("my-model"));
        expect(str_member(root, "type") == kimix::string("openai_legacy"));
        expect(str_member(root, "api_key") == kimix::string("key123"));
        expect(int_member(root, "max_context_size") == 256000);
        expect(int_member(root, "max_tokens") == 64000);
        expect(str_member(root, "thinking_effort") == kimix::string("low"));
        expect(str_member(root, "url") == kimix::string("https://x.io/v1"));
        expect(yyjson_obj_get(root, "sub_provider") == nullptr);
    };

    "wizard_validates_and_reprompts"_test = [] {

        cli::set_env("KIMI_API_KEY", "");
        cli::set_env("KIMIX_API_KEY", "");
        const kimix::string work = ws_dir("cli_boot_wizard_validate");
        const kimix::string path = cli::join_path(work, "default_config.json");
        scripted_input in;
        in.lines = {"deepseek",     // template select
                    "",             // model name -> default
                    "bogus",        // INVALID type
                    "kimi",         // type retried
                    "",             // api key (twice: skip guard)
                    "",             // key second press
                    "banana",       // INVALID context size
                    "1",            // INVALID context size (<= 1)
                    "1M",           // context size ok
                    "999999999",    // max tokens out of range -> default
                    "mega",         // INVALID effort
                    "max",          // effort ok
                    "nope-cap",     // INVALID capability among valid ones
                    "none",         // capabilities cleared
                    "",             // url -> default
                    "n"};           // no sub-provider
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "wiz.txt")));
            cli::set_colorful(false);
            cli::app_context app;
            expect(cli::run_init_wizard(app, path, true, false, in.fn()));
            out = capture.end();
            cli::set_colorful(true);
        }
        expect(has_substr(out, "Invalid type 'bogus', please choose from:"));
        expect(has_substr(out, "Context size must be larger than 1, got 1"));
        expect(has_substr(out, "Invalid size 'banana', please choose from:"));
        expect(has_substr(out, "Invalid effort 'mega', please choose from:"));
        expect(has_substr(out, "Invalid capabilities: nope-cap,"));
        expect(has_substr(out, "Value 999999999 out of range, using default 384000"));
        yyjson_val *root = load_root(path);
        expect(root != nullptr);
        // deepseek template defaults merged in (model filled by the wizard).
        expect(int_member(root, "max_context_size") == 1000000);
        // The out-of-range tokens kept the template's default 128000? No: the
        // deepseek template's max_tokens is 384000 (the merge fills it first).
        expect(int_member(root, "max_tokens") == 384000);
        expect(str_member(root, "type") == kimix::string("kimi"));
        // capabilities written even when cleared (init.py:312).
        yyjson_val *caps = yyjson_obj_get(root, "capabilities");
        expect(caps != nullptr && yyjson_is_arr(caps) && yyjson_get_len(caps) == 0);
    };

    "wizard_boot_gate_and_abort"_test = [] {
        const kimix::string work = ws_dir("cli_boot_wizard_gate");
        const kimix::string path = cli::join_path(work, "default_config.json");
        // Declining the gate writes nothing and reports false.
        {
            scripted_input in;
            in.lines = {"n"};
            cli::app_context app;
            expect(!cli::run_init_wizard(app, path, /*initialize=*/false,
                                         /*open_after=*/false, in.fn()));
            expect(!cli::file_exists(path)) << "declined: no config written";
        }
        // An empty answer counts as "yes": the template is written without
        // any question (the reference's run_init(initialize=False) save).
        {
            scripted_input in;
            in.lines = {""};
            kimix::string out;
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "gate.txt")));
            cli::set_colorful(false);
            cli::app_context app;
            expect(cli::run_init_wizard(app, path, false, false, in.fn()));
            out = capture.end();
            cli::set_colorful(true);
            expect(!has_substr(out, "Configuration saved successfully"))
                << "the boot path stays silent about the save";
            expect(has_substr(in.prompts[0],
                              "default config not found, initialize? you can use "
                              "/init any time. (y/n)"));
            expect(cli::file_exists(path));
        }
        // EOF mid-questions -> "keyboard interruped." and no crash.
        {
            const kimix::string abort_path = cli::join_path(work, "abort.json");
            scripted_input in;
            in.lines = {"", "partial-model"}; // EOF right after the model name
            kimix::string out;
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "abort.txt")));
            cli::set_colorful(false);
            cli::app_context app;
            expect(!cli::run_init_wizard(app, abort_path, true, false, in.fn()));
            out = capture.end();
            cli::set_colorful(true);
            expect(has_substr(out, "keyboard interruped."));
            expect(!cli::file_exists(abort_path));
        }
    };

    "wizard_sub_provider_block"_test = [] {
        const kimix::string work = ws_dir("cli_boot_wizard_sub");
        const kimix::string path = cli::join_path(work, "default_config.json");
        scripted_input in;
        in.lines = {"",        // template -> kimi
                    "main-m",  // model
                    "kimi",    // type
                    "k1",      // api key
                    "1M",      // context
                    "131072",  // max tokens
                    "high",    // effort
                    "thinking",// capabilities
                    "",        // url -> default
                    "y",       // configure a sub-provider
                    "sub-m",   // sub model
                    "anthropic",// sub type
                    "",        // sub url -> default
                    "sk2",     // sub key
                    "256k",    // sub context
                    "medium",  // sub effort
                    "always_thinking, thinking", // sub caps (dedupe)
                    "100000",  // sub max tokens
                    ""};       // (no further input)
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "sub.txt")));
            cli::set_colorful(false);
            cli::app_context app;
            expect(cli::run_init_wizard(app, path, true, false, in.fn()));
            capture.end();
            cli::set_colorful(true);
        }
        yyjson_val *root = load_root(path);
        expect(root != nullptr);
        yyjson_val *sub = yyjson_obj_get(root, "sub_provider");
        expect(sub != nullptr && yyjson_is_obj(sub));
        expect(str_member(sub, "model") == kimix::string("sub-m"));
        expect(str_member(sub, "type") == kimix::string("anthropic"));
        expect(int_member(sub, "max_context_size") == 256000);
        expect(str_member(sub, "thinking_effort") == kimix::string("medium"));
        expect(int_member(sub, "max_tokens") == 100000);
        // always_thinking supersedes thinking in the sub caps.
        yyjson_val *caps = yyjson_obj_get(sub, "capabilities");
        expect(caps != nullptr && yyjson_is_arr(caps) && yyjson_get_len(caps) == 1);
    };

    "boot_auto_init_non_tty_writes_template"_test = [] {
        const kimix::string work = ws_dir("cli_boot_auto");
        // fd 0 -> a plain file: stdin stops being a console.
        const kimix::string stdin_path = cli::join_path(work, "stdin.txt");
        kimix::string error;
        expect(cli::write_file(stdin_path, "\n", error));
        stdin_redirect redirect;
        expect(redirect.begin(stdin_path));
        kimix::string out;
        int code = -1;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "boot.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--dry-run",
                                                  "--work-dir", work};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            code = cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        redirect.end();
        expect(eq(code, cli::kExitOk))
            << "the non-TTY boot wrote a template and continued";
        expect(has_substr(out, "Created default config at "));
        expect(has_substr(out, "Please set KIMI_API_KEY/KIMIX_API_KEY or edit the file."));
        // The written template is the kimi one and loads as the provider.
        yyjson_val *root = load_root(cli::join_path(work, "default_config.json"));
        expect(root != nullptr);
        expect(str_member(root, "model") == kimix::string("kimi-for-coding"));
        expect(str_member(root, "type") == kimix::string("kimi"));
        expect(int_member(root, "max_context_size") == 1048576);
        // The --dry-run report ran against the fresh config.
        expect(has_substr(out, "LLMConfig: model=kimi-for-coding"));
        expect(has_substr(out, "OK"));
    };

    "sub_provider_parsing_h2"_test = [] {
        const kimix::string work = ws_dir("cli_boot_subcfg");
        // (a) A model-less root with a roleless sub entry: it is promoted.
        const kimix::string pick = cli::join_path(work, "pick.json");
        kimix::string error;
        expect(cli::write_file(
            pick,
            "{\"type\":\"openai\",\"url\":\"http://root/\",\"api_key\":\"rk\","
            "\"max_context_size\":1000,"
            "\"sub_provider\":{\"model\":\"main-from-sub\",\"type\":\"anthropic\","
            "\"url\":\"http://sub/\",\"max_context_size\":2000,"
            "\"max_tokens\":500}}",
            error));
        cli::provider_config cfg;
        kimix::string cfg_error;
        expect(cli::load_provider_config(pick, cfg, cfg_error)) << cfg_error;
        expect(cfg.model == kimix::string("main-from-sub"))
            << "the no-role entry became the main provider";
        expect(cfg.base_url == kimix::string("http://sub/"));
        expect(cfg.max_context_size == 2000);
        expect(eq(cfg.sub_providers.size(), size_t(1)));
        expect(cfg.sub_providers[0].role == kimix::string("sub_agent"));

        // (b) A full sub_providers list: roles normalised, invalid entries
        // dropped, root model kept.
        const kimix::string list = cli::join_path(work, "list.json");
        expect(cli::write_file(
            list,
            "{\"model\":\"root-m\",\"type\":\"openai\",\"url\":\"http://r/\","
            "\"api_key\":\"k\",\"max_context_size\":1000,"
            "\"sub_providers\":["
            "{\"model\":\"s1\",\"type\":\"kimi\",\"url\":\"http://s1/\","
            "\"max_context_size\":10,\"role\":\"planner\"},"
            "{\"model\":\"s2\",\"type\":\"kimi\",\"url\":\"http://s2/\","
            "\"max_context_size\":20},"
            "{\"model\":\"s3\",\"type\":\"kimi\",\"max_context_size\":30}"
            "]}",
            error));
        cfg = cli::provider_config{};
        expect(cli::load_provider_config(list, cfg, cfg_error)) << cfg_error;
        expect(cfg.model == kimix::string("root-m"));
        expect(eq(cfg.sub_providers.size(), size_t(2)))
            << "s3 lacks 'url' and was dropped with a warning";
        expect(cfg.sub_providers[0].role == kimix::string("planner"));
        expect(cfg.sub_providers[1].role == kimix::string("sub_agent"));
        // The inherited api_key reached the sub entries.
        expect(cfg.sub_providers[0].api_key == kimix::string("k"));

        // (c) The old behaviour (silently failing on a model-less root with a
        // sub_provider) is gone: the config above loads without an error.
    };

    "mcp_json_boot_diagnostics_h5"_test = [] {
        const kimix::string work = ws_dir("cli_boot_mcp");
        const kimix::string ok_provider = cli::join_path(work, "ok.json");
        kimix::string error;
        expect(cli::write_file(ok_provider,
                               "{\"model\":\"m\",\"type\":\"openai\","
                               "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"k\","
                               "\"max_context_size\":1000,\"max_tokens\":100}",
                               error));
        const kimix::string kimix_dir = cli::join_path(work, ".kimix");
        expect(cli::make_dirs(kimix_dir, error));

        // Remember the cwd (the reference reads ./.kimix/mcp.json) and restore.
        const kimix::string saved_cwd = cli::current_dir();
        std::error_code chdir_ec;
        kimix::filesystem::current_path(kimix::filesystem::path(work), chdir_ec);
        expect(!chdir_ec);

        // A valid object -> the debug line.
        expect(cli::write_file(cli::join_path(kimix_dir, "mcp.json"),
                               "{\"mcpServers\":{}}", error));
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "mcp_ok.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--dry-run",
                                                  "--provider", ok_provider,
                                                  "--work-dir", work};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        expect(has_substr(out, "Loaded MCP config from " +
                                  cli::join_path(kimix_dir, "mcp.json")));

        // A non-object -> the must-contain warning.
        expect(cli::write_file(cli::join_path(kimix_dir, "mcp.json"), "[1,2]", error));
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "mcp_arr.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--dry-run",
                                                  "--provider", ok_provider,
                                                  "--work-dir", work};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        expect(has_substr(out, "MCP config file " + cli::join_path(kimix_dir, "mcp.json") +
                                  " must contain a JSON object."));

        // Broken JSON -> the parse-failure warning.
        expect(cli::write_file(cli::join_path(kimix_dir, "mcp.json"), "{oops", error));
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "mcp_bad.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--dry-run",
                                                  "--provider", ok_provider,
                                                  "--work-dir", work};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        expect(has_substr(out, "Failed to parse MCP config file " +
                                  cli::join_path(kimix_dir, "mcp.json") + ":"));

        std::error_code restore_ec;
        kimix::filesystem::current_path(kimix::filesystem::path(saved_cwd), restore_ec);
        expect(!restore_ec);
    };

    "print_error_goes_to_stdout_h7"_test = [] {
        const kimix::string work = ws_dir("cli_boot_print_error");
        // Capture fd 1 and fd 2 into SEPARATE files.
        const kimix::string out_path = cli::join_path(work, "out.txt");
        const kimix::string err_path = cli::join_path(work, "err.txt");
        std::fflush(stdout);
        std::fflush(stderr);
        std::FILE *out_file = std::fopen(out_path.c_str(), "wb");
        std::FILE *err_file = std::fopen(err_path.c_str(), "wb");
        expect(out_file != nullptr && err_file != nullptr);
        const int saved_out = test_dup(1);
        const int saved_err = test_dup(2);
        test_dup2(test_fileno(out_file), 1);
        test_dup2(test_fileno(err_file), 2);
        cli::set_colorful(false);
        cli::print_error("stream routing probe");
        std::fflush(stdout);
        std::fflush(stderr);
        test_dup2(saved_out, 1);
        test_close_fd(saved_out);
        test_dup2(saved_err, 2);
        test_close_fd(saved_err);
        std::fclose(out_file);
        std::fclose(err_file);
        expect(has_substr(file_text_must(out_path), "stream routing probe"));
        expect(!has_substr(file_text_must(err_path), "stream routing probe"))
            << "H7: print_error no longer writes to stderr";
    };

    "native_acceleration_log_h11"_test = [] {
        const kimix::string work = ws_dir("cli_boot_native");
        const kimix::string provider = cli::join_path(work, "p.json");
        kimix::string error;
        expect(cli::write_file(provider,
                               "{\"model\":\"m\",\"type\":\"openai\","
                               "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"k\","
                               "\"max_context_size\":1000,\"max_tokens\":100}",
                               error));
        // The info line prints by default.
        kimix::string out;
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "native_on.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--version"};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        expect(has_substr(out, "Native acceleration enabled."));
        // KIMIX_NATIVE=0 is the explicit opt-out: nothing prints.
        cli::set_env("KIMIX_NATIVE", "0");
        {
            output_capture capture;
            expect(capture.begin(cli::join_path(work, "native_off.txt")));
            cli::set_colorful(false);
            kimix::vector<kimix::string> owned = {"kimix_cli", "--version"};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            cli::cli_main(static_cast<int>(argv.size()), argv.data());
            out = capture.end();
            cli::set_colorful(true);
        }
        cli::set_env("KIMIX_NATIVE", "");
        expect(!has_substr(out, "Native acceleration enabled."));
    };

    "no_think_sets_enable_thinking_e7"_test = [] {
        const kimix::string work = ws_dir("cli_boot_nothink");
        const kimix::string provider = cli::join_path(work, "p.json");
        kimix::string error;
        expect(cli::write_file(provider,
                               "{\"model\":\"m\",\"type\":\"openai\","
                               "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"k\","
                               "\"max_context_size\":1000,\"max_tokens\":100}",
                               error));
        // Without the flag: thinking stays on.
        {
            cli::cli_options opts;
            opts.config_path = provider;
            opts.config_is_provider_only = true;
            opts.work_dir = work;
            opts.dry_run = true;
            cli::app_context app;
            kimix::string init_error;
            expect(cli::app_init(opts, app, init_error));
            expect(cli::to_llm_config(app.provider).enable_thinking);
        }
        // With --no_think: the request-level switch turns off (E7).
        {
            cli::cli_options opts;
            opts.config_path = provider;
            opts.config_is_provider_only = true;
            opts.work_dir = work;
            opts.dry_run = true;
            opts.no_think = true;
            cli::app_context app;
            kimix::string init_error;
            expect(cli::app_init(opts, app, init_error));
            expect(!cli::to_llm_config(app.provider).enable_thinking);
            expect(!kimix::llm::thinking_enabled(cli::to_llm_config(app.provider)))
                << "the providers send the thinking-disabled wire shape";
        }
        // default_thinking=false in the config alone flips it too.
        {
            const kimix::string off = cli::join_path(work, "off.json");
            expect(cli::write_file(off,
                                   "{\"model\":\"m\",\"type\":\"openai\","
                                   "\"url\":\"http://127.0.0.1:1/v1\",\"api_key\":\"k\","
                                   "\"max_context_size\":1000,\"max_tokens\":100,"
                                   "\"default_thinking\":false}",
                                   error));
            cli::provider_config cfg;
            expect(cli::load_provider_config(off, cfg, error));
            expect(!cli::to_llm_config(cfg).enable_thinking);
        }
    };

    "help_lists_python_only_front_ends_i4"_test = [] {
        // The extended help documents the four refused front ends.
        const kimix::string help = cli::cli_help_text_extended(false);
        expect(has_substr(help, "Python-only front ends"));
        expect(has_substr(help, "serve"));
        expect(has_substr(help, "gui"));
        expect(has_substr(help, "ssecli"));
        expect(has_substr(help, "mcp"));
        // The frozen HELP_STR itself is untouched (I10 byte-exactness).
        const kimix::string base = cli::cli_help_text(false);
        expect(!has_substr(base, "Python-only front ends"));

        // The refusal prints the front-end list and exits 3 for each.
        const char *const subcommands[] = {"serve", "gui", "ssecli", "mcp"};
        const kimix::string work = ws_dir("cli_boot_i4");
        for (const char *const name : subcommands) {
            kimix::vector<kimix::string> owned = {"kimix_cli", name, "--x", "1"};
            kimix::vector<char *> argv;
            for (kimix::string &arg : owned) {
                argv.push_back(arg.data());
            }
            output_capture capture;
            expect(capture.begin(cli::join_path(work, kimix::string(name) + ".txt")));
            cli::set_colorful(false);
            const int code = cli::cli_main(static_cast<int>(argv.size()), argv.data());
            const kimix::string out = capture.end();
            cli::set_colorful(true);
            expect(eq(code, cli::kExitUnsupported)) << name;
            expect(has_substr(out, "not supported by the native CLI")) << name;
            expect(has_substr(out, "Front ends implemented only in the Python CLI"))
                << name;
        }
    };
}
