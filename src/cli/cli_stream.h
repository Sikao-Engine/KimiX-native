// cli/cli_stream.h - Terminal streaming renderer: the C++ port of kimi-agent's
// kimix/ui/stream.py (the parts reachable from the CLI) plus the display-block
// formatter it drives.
//
// Reference (read-only): C:/dev/kimi-agent/src/kimix/ui/stream.py @ 86b7bf6 and
// the derived spec .kimix_cache/cli_specs/03_ui.md §2.  Every literal, colour and
// newline rule implemented here is traceable to that source; the byte-exact table
// lives in src/cli/reports/cli_stream.md.
//
// Mapping from print_agent_json's wire-message dispatch (stream.py:1106-1178) to
// this class:
//
//   ToolCall         -> on_tool_call_begin       _handle_tool_call (ToolCall)
//   ToolCallPart     -> on_tool_call_args_delta  _handle_tool_call (ToolCallPart)
//   ToolResult       -> on_display_blocks        _format_display_blocks
//                       + on_tool_result         _handle_tool_result
//   ThinkPart        -> on_reasoning_delta       _handle_think_part
//   TextPart         -> on_text_delta            _handle_text_part
//   StepBegin        -> on_step_begin            _handle_noop
//   StepInterrupted  -> finish_turn              _handle_noop
//   CompactionBegin  -> on_compaction_begin      _handle_compaction_begin
//   CompactionEnd    -> on_compaction_end        _handle_noop
//   (anything else)  -> no entry point           _handle_other (state = Other)
//   ApprovalRequest  -> not ported (the native toolset has no approval gate)
//
// Every non-tool-call entry point first runs the reference's
// _finish_tool_call_stream() (it terminates a half-written argument line), and
// every entry point whose wire message maps to a MessageType runs the transition
// check (_print_transition_usage) before its own output.
//
// Line state: stream.py renders through a process-wide PrintStream singleton
// (_stream).  A renderer instance owns that state instead - it is initialised to
// "the last character was a newline", exactly like PrintStream.__init__, so a
// fresh renderer never emits a leading newline.  As in the reference the state is
// computed from the *pre-colour* text, which is why every emit helper here takes
// both the rendered and the raw form.
//
// Native additions (all documented in src/cli/reports/cli_stream.md):
//   * set_output()/output()  - the reference writes to the process-wide
//     _print_func (stdout).  The renderer can be pointed at any FILE* so tests
//     can render into a tmpfile() and compare exact bytes; the default is stdout.
//     Colour still goes through cli_print's colourful_text*/gray_text helpers, so
//     --no_color / the console detection keep working unchanged.
//   * on_display_blocks()    - the frozen on_tool_result() carries plain strings,
//     not wire display blocks, so the block formatter (codes 90/93/91/92/250) has
//     its own entry point; on_tool_result's output_summary is rendered as a Brief
//     block through the same path (a ToolOk's `brief`).
//   * on_error()             - the reference has no error wire message; modelled
//     on printing.py's print_error (bright red + bold).
//   * reset_capture()        - drops captured_text() at a turn boundary.
//
// Rules (see src/cli/PLAN.md §3 and .agents/skills/cpp): namespace kimix::cli,
// exception-free (bool/empty results, never throw), kimix:: containers in every
// public API, unity build (TU-local helpers live in an anonymous namespace with
// the `clist_` prefix).

#pragma once

#include <cstdint>
#include <cstdio>

#include <core/kimix_core.h>

#include "llm/llm.h"

namespace kimix::cli {

// ---------------------------------------------------------------------------
// Display blocks (kimi_cli/wire/types.py, kosong/tooling DisplayBlock)
// ---------------------------------------------------------------------------
enum class display_block_kind : int32_t {
    brief = 0,      // BriefDisplayBlock: block.text
    diff = 1,       // DiffDisplayBlock: Diff: {path} / - {old line} / + {new line}
    todo = 2,       // TodoDisplayBlock: the three per-item forms
    shell = 3,      // ShellDisplayBlock: renders nothing
    background = 4, // BackgroundTaskDisplayBlock: [{status}] {task_id}: {description}
    unknown = 5,    // UnknownDisplayBlock: str(block.data)
    base = 6,       // DisplayBlock: str(block.model_dump()) when non-empty
};

// TodoDisplayBlock.items[i]: `status` is the raw wire status ("done",
// "in_progress", ...); the renderer lower-cases it after replacing "_" with " ".
struct todo_display_item {
    kimix::string title;
    kimix::string status;
};

struct display_block {
    display_block_kind kind = display_block_kind::brief;
    kimix::string text;      // brief: text; diff: path; background: description;
                             // unknown/base: str(data)
    kimix::string old_text;  // diff: old_text (splitlines)
    kimix::string new_text;  // diff: new_text (splitlines)
    kimix::string status;    // background: status
    kimix::string task_id;   // background: task_id
    kimix::vector<todo_display_item> items; // todo: items
};

// _format_display_blocks: the per-block rendered parts, in order, with Shell and
// empty briefs contributing nothing.  Each part is already colour-wrapped.
kimix::vector<kimix::string> display_block_parts(const display_block &block);
// _format_display_blocks: "" when no part rendered (the reference returns None),
// otherwise the parts joined with "\n" plus one trailing "\n".
kimix::string format_display_blocks(const kimix::vector<display_block> &blocks);

// ---------------------------------------------------------------------------
// Percentage / context-usage helpers (stream.py:1183-1189, 117-133)
// ---------------------------------------------------------------------------
// percentage_str: f"{num * 100:.1f}%".
kimix::string percentage_str(double ratio);
// percentage_and_token: f"{ratio * 100:.1f}% ({tokens} tokens)".
kimix::string percentage_and_token(double ratio, int64_t tokens);
// The context-usage divider WITHOUT its trailing newline: 20 '=', the literal
// " Context usage: ", percentage_and_token(), one space and enough '=' to make
// exactly 80 characters (a single '=' when that would need zero or fewer).
// The renderer appends the "\n" the reference prints (stream.py:128).
kimix::string context_usage_banner(double ratio, int64_t tokens);

// ---------------------------------------------------------------------------
// stream_renderer - the PrintStream-facing half of stream.py
// ---------------------------------------------------------------------------
class stream_renderer {
public:
    // `show_thinking` false suppresses the whole ThinkPart path (the CLI's
    // --no_think / show_thinking_stream), `show_usage` false suppresses the
    // context-usage transition banner.  The reference gates thinking on the
    // module-global _quiet only; the quiet() check below keeps that behaviour.
    explicit stream_renderer(bool show_thinking, bool show_usage = true);
    // The argument printer holds a back-pointer to its renderer, so copies
    // re-point it instead of addressing the source instance (a renderer is
    // normally held by reference or by kimix::unique_ptr).
    stream_renderer(const stream_renderer &other);
    stream_renderer &operator=(const stream_renderer &other);

    // Native addition: the output stream (default stdout).  Anything else is
    // written with fwrite()/fflush() instead of cli_print's stdout helpers.
    void set_output(std::FILE *out);
    std::FILE *output() const;

    // StepBegin / StepInterrupted: no output in the reference (_handle_noop).
    void on_step_begin(int32_t step, int32_t max_steps);
    // TextPart: plain text, no colour; a newline is inserted when the previous
    // printed state was not Text.  Also accumulates captured_text().
    void on_text_delta(kimix::string_view delta);
    // ThinkPart: "[Think] " + the first chunk of a reasoning run, bright cyan,
    // suppressed entirely when quiet() (or show_thinking was false).
    void on_reasoning_delta(kimix::string_view delta);
    // ToolCall: the bright-magenta "⚡ <name>" header, then call.arguments is
    // fed to the incremental argument printer (providers may deliver them later
    // through on_tool_call_args_delta).
    void on_tool_call_begin(const kimix::llm::ToolCall &call);
    // ToolCallPart: feed one raw JSON fragment to the argument printer.
    void on_tool_call_args_delta(kimix::string_view delta);
    // ToolResult: output_summary is rendered first as a Brief display block
    // (dim, code 90) when non-empty, then "✓ <name>" bright green / "✗ <name>"
    // bright red, then the dim "  <message>" detail line unless the message is
    // one of the four trivial ones ("success", "failed", "[rtk] success",
    // "[rtk] failed").  An empty `name` selects the reference's no-tool-call
    // fallback.
    void on_tool_result(kimix::string_view name, bool ok, kimix::string_view message,
                        kimix::string_view output_summary = {});
    // Native addition: the full _format_display_blocks path (see §2.11).
    void on_display_blocks(const kimix::vector<display_block> &blocks);
    // CompactionBegin: "Compacting..." bright magenta.
    void on_compaction_begin();
    // CompactionEnd is _handle_noop in the reference: nothing is printed.
    void on_compaction_end(bool ok);
    // Native addition: store the latest status snapshot the banner renders.
    // It prints nothing itself (the reference prints on content transitions).
    void on_context_usage(double ratio, int64_t tokens);
    // Native addition: bright red + bold line (no reference counterpart).
    void on_error(kimix::string_view message);
    // _finish_tool_call_stream + print_agent_json_flush_text: terminate a
    // half-written argument line.  captured_text() survives until
    // reset_capture() (the CLI reads it after the turn).
    void finish_turn();
    // The assistant text seen through on_text_delta (raw, uncoloured).
    const kimix::string &captured_text() const;
    // Native addition: clear captured_text() for the next turn.
    void reset_capture();

private:
    // _ToolCallStreamPrinter (stream.py:418-856).  The incremental JSON lexer:
    // long values of the streamed keys are printed decoded, fragment by
    // fragment; every other value is printed inline by _emit_compact.
    struct arg_printer {
        enum : int32_t {
            expect_key = 0,
            in_key = 1,
            expect_colon = 2,
            expect_value = 3,
            in_string = 4,
            in_bare = 5,
            after_value = 6,
            done = 7,
        };

        stream_renderer *owner = nullptr;
        int32_t state = expect_value;
        kimix::string stack; // '{' / '[' container balance
        kimix::string current_key;
        kimix::string key_chars;
        kimix::string value_chars;
        kimix::string emit_chars;
        kimix::string escape_buf;
        bool in_escape = false;
        int32_t pending_high_surrogate = -1;
        bool string_streamed = false;
        bool color_indexed = true;   // _stream_color is a Color256 ...
        int32_t color_code = 250;    // ... GRAY_LIGHT until a key resolves it
        kimix::string json_parts;
        bool finished = false;
        bool broken = false; // no lexer exception path exists; kept for parity
        size_t bytes_since_flush = 0;

        void feed(kimix::string_view fragment);
        void finish();
        void reset();
        void lex(kimix::string_view fragment);
        void feed_char(char ch);
        void feed_escape_char(char ch);
        void reset_escape();
        void handle_code_point(int32_t code_point);
        void append_value_char(kimix::string_view s);
        void begin_string_value();
        void end_string_value();
        void end_bare_value();
        void emit_compact(kimix::string_view text);
        void flush_emit(bool flush);
        void after_comma();
        void close_container();
        void check_complete();
    };

    enum class message_type : int32_t { none = 0, text = 1, thinking = 2, tool_calling = 3 };

    void finish_tool_call_stream();
    // _print_transition_usage: the banner only appears when a previous content
    // type was recorded and differs from `type`.
    void transition(message_type type);
    // print_word: `rendered` goes to the stream, `raw` drives the newline
    // tracker (ANSIs are stripped from it), like PrintStream.print_word.
    void emit_word(kimix::string_view rendered, bool require_new_line,
                   kimix::string_view raw, bool flush);
    void emit_colored(kimix::string_view raw, int fg, bool require_new_line, bool flush);
    void emit_colored_256(kimix::string_view raw, int fg256, bool require_new_line,
                          bool flush);
    void write_bytes(kimix::string_view text, bool flush);

    bool show_thinking_;
    bool show_usage_;
    std::FILE *out_;
    bool last_char_was_newline_;
    int32_t stream_state_;   // Text / Thinking / Other (StreamPrintState)
    message_type message_type_;
    double ratio_;
    int64_t tokens_;
    kimix::string captured_text_;
    arg_printer printer_;
    bool has_printer_;
};

} // namespace kimix::cli
