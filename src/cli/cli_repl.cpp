// cli/cli_repl.cpp - The REPL loop (see cli_repl.h).
//
// Port of kimix/cli_impl/core.py::_client_cli (revision 86b7bf6) plus
// kimix/cli_impl/utils.py::_input: the reference's `text_arr` queue is the
// `pending` vector below (seeded from --script), consulted by app_read_input()
// before stdin.  Every print goes through cli_print / the stream renderer.
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `clirpl_` prefix.

#include "cli/cli_repl.h"

#include <cstdint>
#include <cstdio>
#include <system_error>

#include "cli/cli_commands.h"
#include "cli/cli_common.h"
#include "cli/cli_print.h"

namespace kimix::cli {

namespace {

// Python's Path.is_absolute() (a Windows drive-qualified or rooted path).
bool clirpl_is_absolute(kimix::string_view path) {
    const kimix::filesystem::path p{kimix::string(path)};
    return p.is_absolute();
}

// The `print_info(text, end="\n\n")` form the reference uses for "Executing x".
void clirpl_print_info_blank(kimix::string_view text) {
    print_string(colorful_text(text, 95) + "\n");
}

} // namespace

int repl_run(app_context &app, std::FILE *in, std::FILE *out,
             const kimix::vector<kimix::string> &scripted) {
    kimix::vector<kimix::string> pending = scripted;
    app.pending = &pending;
    app.input = in;
    app.output = out;

    const kimix::string prompt = app_prompt_line();
    for (;;) {
        // `_input(prompt, text_arr)`: the queue first (printing nothing), then
        // the prompt + one line from stdin.  EOF / Ctrl-C -> "\nbye." + exit 0.
        kimix::string input;
        if (!app_read_input(app, prompt, input)) {
            print_success("\nbye.");
            break;
        }
        if (input.empty()) {
            continue; // (A) blank input: silently re-prompt
        }

        if (input[0] == '/') {
            // (B) slash command: `task = input_str[1:]`; the colon is searched in
            // the TRIMMED string but the slicing uses the untrimmed one.
            const kimix::string task = input.substr(1);
            const kimix::string_view stripped = trim(task);
            const size_t split_idx = find(stripped, ":");
            kimix::vector<kimix::string> args;
            if (split_idx != kimix::string_view::npos) {
                args.push_back(task.substr(0, split_idx));
                args.push_back(task.substr(split_idx + 1));
            } else {
                args.push_back(task);
            }
            const command_entry *entry = find_command(args[0]);
            if (entry == nullptr) {
                entry = find_command("unknown");
            }
            kimix::vector<kimix::string> text_arr;
            command_result result;
            if (entry != nullptr) {
                result = entry->handler(args, app, text_arr);
            }
            if (result.should_break) {
                break;
            }
            // A handler's next_input is the next REPL input (the native form of
            // the interface's `has_input`); text_arr entries queue behind it,
            // exactly like the reference's _cmd_txt pushes.
            if (result.has_input && !result.next_input.empty()) {
                pending.insert(pending.begin(), result.next_input);
            }
            for (const kimix::string &block : text_arr) {
                pending.push_back(block);
            }
            continue;
        }

        // (C) non-slash text: an existing file is read (a .py file is reported
        // as unsupported - no embedded Python interpreter), everything else is
        // sent to the agent as a prompt.
        kimix::string path = input;
        if (!clirpl_is_absolute(path)) {
            path = join_path(app.work_dir, path);
        }
        kimix::string prompt_text = input;
        if (file_exists(path)) {
            kimix::string content;
            kimix::string error;
            if (!read_file(path, content, error)) {
                print_error(error);
                continue;
            }
            if (extension(path) == ".py") {
                clirpl_print_info_blank("Executing " + file_name(path));
                print_error("the native CLI has no embedded Python interpreter: " +
                            file_name(path) +
                            " was not executed (run it yourself, or use /code:<path>)");
                continue;
            }
            print_debug("File not executable, consider as prompt.");
            prompt_text = std::move(content);
        }
        if (prompt_text.empty()) {
            continue;
        }
        if (app.opts.manually_cot) {
            // Reduced --manually-cot (documented): the reference runs cot_prompt()
            // (a separate multi-session CoT driver); the native CLI sends the
            // prompt through the normal turn.
            print_info("Manually CoT mode enabled: may use multiple sessions and "
                       "extra tokens.");
        }
        app_run_prompt(app, prompt_text);
    }

    app.pending = nullptr;
    app.input = nullptr;
    app.output = nullptr;
    return app.exit_code;
}

} // namespace kimix::cli
