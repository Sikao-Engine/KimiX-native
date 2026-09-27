// cli/cli_repl.cpp - The REPL loop (see cli_repl.h).
//
// Port of kimix/cli_impl/core.py::_client_cli (revision 86b7bf6) plus
// kimix/cli_impl/utils.py::_input: the reference's `text_arr` queue is the
// `pending` vector below (seeded from --script), consulted before stdin.
// Every print goes through cli_print / the stream renderer.
//
// Phase 3 (G7/G8) additions over the synchronous original:
// * A reader thread owns the blocking stdin reads and queues completed lines,
//   so the user can type WHILE the model is streaming: while a turn runs
//   (app.steering), a finished line is routed to the running soul through
//   request_steer() instead of the input queue (the reference's mid-stream
//   steering, CLI edition).
// * Ctrl-C goes through cli_signal: the handler stores into the app cancel
//   token's flag. At the prompt the loop observes the flag, prints the
//   reference's "\nbye." and exits cleanly; mid-turn the turn polls the same
//   flag, aborts at the next step boundary, interrupts the in-flight request
//   and the caller prints "Keyboard Interrupt." with the session kept
//   (core.py's two KeyboardInterrupt handlers).
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `clirpl_` prefix.

#include "cli/cli_repl.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

#include "cli/cli_commands.h"
#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_signal.h"

namespace kimix::cli {

namespace {

// One line from `in` (Python's input(): the trailing newline / CRLF is dropped,
// a final unterminated line is returned, EOF with nothing read reports false).
bool clirpl_read_line(std::FILE *in, kimix::string &line) {
    line.clear();
    if (in == nullptr) {
        return false;
    }
    bool any = false;
    for (;;) {
        const int ch = std::fgetc(in);
        if (ch == EOF) {
            break;
        }
        any = true;
        if (ch == '\n') {
            break;
        }
        line.push_back(static_cast<char>(ch));
    }
    if (!any) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

void clirpl_reader_loop(std::FILE *in, cli_input_queue *queue) {
    for (;;) {
        kimix::string line;
        if (!clirpl_read_line(in, line)) {
            std::lock_guard<std::mutex> g(queue->mutex);
            queue->eof = true;
            queue->cv.notify_all();
            return;
        }
        app_context *app = queue->app;
        // G1: a line typed while the turn blocks inside the approval gate
        // answers the pending prompt (never steers, never queues).
        approval_answer_slot *slot =
            app != nullptr
                ? app->approval_slot.load(std::memory_order_acquire)
                : nullptr;
        if (slot != nullptr) {
            {
                std::lock_guard<std::mutex> g(slot->mutex);
                slot->line = line;
                slot->answered = true;
            }
            slot->cv.notify_all();
            continue;
        }
        if (app != nullptr && app->steering.load(std::memory_order_acquire) &&
            app->soul != nullptr) {
            // G7: mid-turn input steers the running turn (never queued).
            app->soul->request_steer(line);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(queue->mutex);
            queue->lines.push_back(std::move(line));
        }
        queue->cv.notify_all();
    }
}

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

    // G7: the reader thread keeps stdin flowing while turns run.  The queue
    // is published on the app so command handlers blocked in app_read_input
    // (multi-line /end, /cancel, ...) consume the same lines instead of
    // racing the reader with a second fgetc on `in`.
    cli_input_queue queue;
    queue.app = &app;
    app.input_queue = &queue;
    std::thread reader(clirpl_reader_loop, in, &queue);

    const kimix::string prompt = app_prompt_line();
    for (;;) {
        // `_input(prompt, text_arr)`: the scripted queue first (printing
        // nothing), then the reader queue. Ctrl-C at the prompt -> "\nbye." +
        // exit 0 (core.py's first KeyboardInterrupt handler); EOF likewise.
        kimix::string input;
        bool have_input = false;
        if (!pending.empty()) {
            input = pending.front();
            pending.erase(pending.begin());
            have_input = true;
        } else {
            if (!prompt.empty() && out != nullptr) {
                std::fwrite(prompt.data(), 1, prompt.size(), out);
                std::fflush(out);
            }
            if (ctrlc_pending()) {
                print_success("\nbye.");
                break;
            }
            have_input = cli_input_next_line(queue, input, queue.stop);
            if (!have_input) {
                // Distinguish Ctrl-C (bye.) from EOF (bye.) only in wording:
                // the reference prints the same "\nbye." for both.
                print_success("\nbye.");
                break;
            }
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

    queue.stop.store(true, std::memory_order_release);
    queue.cv.notify_all();
    // The reader may stay blocked in fgets (Ctrl-C is handled, not delivered to
    // stdin); detaching lets the process exit cleanly without killing it.
    reader.detach();

    app.pending = nullptr;
    app.input_queue = nullptr;
    app.input = nullptr;
    app.output = nullptr;
    return app.exit_code;
}

} // namespace kimix::cli
