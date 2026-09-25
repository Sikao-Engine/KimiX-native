// cli/cli_stream.cpp - implementation of the terminal streaming renderer.
//
// Unity build note: every TU-local helper lives in the anonymous namespace below
// with the `clist_` prefix (batch 8 merges the src/cli translation units).

#include <cstdio>

#include "cli/cli_stream.h"

#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Literals (stream.py / the spec's §2.14 marker inventory), written as explicit
// UTF-8 bytes so the output never depends on the source file's encoding.
// ---------------------------------------------------------------------------
constexpr char kThunder[] = "\xe2\x9a\xa1";      // U+26A1 "⚡"
constexpr char kCheck[] = "\xe2\x9c\x93"; // U+2713 "✓"
constexpr char kCross[] = "\xe2\x9c\x97";        // U+2717 "✗"
constexpr char kArrow[] = "\xe2\x86\x90";        // U+2190 "←"
constexpr char kReplacement[] = "\xef\xbf\xbd";  // U+FFFD
constexpr char kThinkBanner[] = "[Think] ";      // stream.py:1077
constexpr char kCompacting[] = "Compacting...";  // stream.py:1062
constexpr char kUsageLabel[] = " Context usage: "; // stream.py:124
constexpr char kDimDetailPrefix[] = "  ";        // stream.py:1029 (two spaces)
constexpr char kDiffHeader[] = "Diff: ";         // stream.py:151
constexpr size_t kBannerWidth = 80;              // stream.py:125
constexpr size_t kBannerEqualCount = 20; // stream.py:122
constexpr size_t kCompactValueMaxLen = 60;       // stream.py:384
constexpr size_t kFlushIntervalBytes = 256;      // stream.py:457

// print_stream state (printing.py:373-376): the last content type printed, used
// by the text/reasoning newline rules.
constexpr int32_t kStreamStateText = 0;
constexpr int32_t kStreamStateThinking = 1;
constexpr int32_t kStreamStateOther = 2;

// Foreground codes (printing.py Color / the named greys).
constexpr int32_t kFgBrightBlack = static_cast<int32_t>(color::bright_black);   // 90
constexpr int32_t kFgBrightRed = static_cast<int32_t>(color::bright_red);       // 91
constexpr int32_t kFgBrightGreen = static_cast<int32_t>(color::bright_green);   // 92
constexpr int32_t kFgBrightYellow = static_cast<int32_t>(color::bright_yellow); // 93
constexpr int32_t kFgBrightBlue = static_cast<int32_t>(color::bright_blue);     // 94
constexpr int32_t kFgBrightMagenta = static_cast<int32_t>(color::bright_magenta); // 95
constexpr int32_t kFgBrightCyan = static_cast<int32_t>(color::bright_cyan);     // 96

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------
bool clist_is_json_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Python str.splitlines(): \n, \r, \r\n, \v, \f, \x1c-\x1e, U+0085, U+2028,
// U+2029, with no trailing empty entry.  Only DiffDisplayBlock uses it.
void clist_splitlines(kimix::string_view text, kimix::vector<kimix::string> &out) {
    kimix::string line;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t width = 0;
        if (c == '\r') {
            width = (i + 1 < n && text[i + 1] == '\n') ? 2u : 1u;
        } else if (c == '\n' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d ||
                   c == 0x1e) {
            width = 1;
        } else if (c == 0xc2 && i + 1 < n && static_cast<unsigned char>(text[i + 1]) == 0x85) {
            width = 2; // U+0085
        } else if (c == 0xe2 && i + 2 < n &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(text[i + 2]) == 0xa8 ||
                    static_cast<unsigned char>(text[i + 2]) == 0xa9)) {
            width = 3; // U+2028 / U+2029
        }
        if (width == 0) {
            line.push_back(text[i]);
            ++i;
            continue;
        }
        out.push_back(line);
        line.clear();
        i += width;
    }
    if (!line.empty()) {
        out.push_back(line);
    }
}

// len(text) in Python counts characters; truncate at 60 characters + "..." the
// way `text[:60] + "..."` does for the UTF-8 subset used by tool arguments.
kimix::string clist_truncate_compact(kimix::string_view text) {
    size_t chars = 0;
    size_t cut = 0;
    while (cut < text.size() && chars < kCompactValueMaxLen) {
        const unsigned char c = static_cast<unsigned char>(text[cut]);
        size_t width = 1;
        if ((c & 0xe0) == 0xc0) {
            width = 2;
        } else if ((c & 0xf0) == 0xe0) {
            width = 3;
        } else if ((c & 0xf8) == 0xf0) {
            width = 4;
        }
        if (cut + width > text.size()) {
            width = 1;
        }
        cut += width;
        ++chars;
    }
    kimix::string out(text.substr(0, cut));
    if (cut < text.size()) {
        out.append("...");
    }
    return out;
}

// printing.py _ends_with_newline: strip SGR sequences, then check the last char.
// Only the raw (pre-colour) text is passed here, so the common path is a single
// byte compare; ESC-bearing input falls back to an exact strip.
bool clist_ends_with_newline(kimix::string_view raw) {
    if (raw.empty()) {
        return false;
    }
    if (raw.back() == '\n') {
        return true;
    }
    if (raw.find('\x1b') == kimix::string_view::npos) {
        return false;
    }
    kimix::string plain;
    plain.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '\x1b') {
            plain.push_back(raw[i]);
            ++i;
            continue;
        }
        size_t j = i + 2;
        while (j < raw.size() &&
               ((raw[j] >= '0' && raw[j] <= '9') || raw[j] == ';')) {
            ++j;
        }
        if (j < raw.size() && raw[j] == 'm') {
            i = j + 1; // well-formed SGR: drop it
        } else {
            ++i; // not an SGR: keep scanning (the ESC itself is dropped)
        }
    }
    return !plain.empty() && plain.back() == '\n';
}

// Python str.lower() restricted to the ASCII range (statuses are ASCII).
kimix::string clist_lower_ascii(kimix::string_view text) {
    return to_lower_ascii(text);
}

kimix::string clist_join(const kimix::vector<kimix::string> &parts,
                         kimix::string_view separator) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(separator);
        }
        out.append(parts[i]);
    }
    return out;
}

// --- colour wrapping -------------------------------------------------------
// printing.py's colorful_text(): the prefix + the text + "\x1b[0m", or the text
// unchanged when colour is off (the only global colour gate, so --no_color and
// the console detection keep working).
//
// NOTE (see src/cli/reports/cli_stream.md): cli_print's colorful_text() cannot
// be used here yet - it passes ansi_prefix()'s *complete* escape sequence into
// clip_wrap(), which re-adds "\x1b[" ... "m", producing "\x1b[\x1b[95mm" + text
// + "\x1b[0m" (a latent S1 bug that also affects print_info/print_success).
// These helpers build the identical bytes from the same building blocks
// (ansi_prefix/ansi_prefix_256 + colorful()); they can be deleted in favour of
// colorful_text/colorful_text_256 once clip_wrap is fixed.
kimix::string clist_wrap(kimix::string_view text, const kimix::string &prefix) {
    if (prefix.empty() || !colorful()) {
        return kimix::string(text);
    }
    kimix::string out(prefix);
    out.append(text);
    out.append("\x1b[0m");
    return out;
}

kimix::string clist_colorful(kimix::string_view text, int32_t fg) {
    return clist_wrap(text, ansi_prefix(fg, -1));
}

kimix::string clist_colorful_256(kimix::string_view text, int32_t fg256) {
    return clist_wrap(text, ansi_prefix_256(fg256, -1));
}

kimix::string clist_colorful_styled(kimix::string_view text, int32_t fg,
                                    kimix::string_view styles) {
    return clist_wrap(text, ansi_prefix(fg, -1, styles));
}

// ---------------------------------------------------------------------------
// Argument-key tables (stream.py:243-384, 462-475)
// ---------------------------------------------------------------------------
struct clist_key_alias {
    const char *from;
    const char *to;
};

const clist_key_alias kKeyAliases[] = {
    // pydantic Field(alias=...) declarations.
    {"old_string", "old"},
    {"new_string", "new"},
    {"text", "content"},
    {"source_code", "code"},
    {"task", "prompt"},
    {"file_path", "path"},
    {"cmd", "command"},
    {"session", "session_id"},
    {"edits", "edit"},
    {"items", "todos"},
    {"block", "wait"},
    {"token_kill", "deduplicate_output"},
    // kosong FIELD_ALIASES_FILE parity.
    {"old_str", "old"},
    {"new_str", "new"},
    {"old_content", "old"},
    {"new_content", "new"},
    {"original", "old"},
    {"replace_with", "new"},
    {"data", "content"},
    {"body", "content"},
    {"file", "path"},
    {"filepath", "path"},
    {"filename", "path"},
    {"file_name", "path"},
    {"changes", "edit"},
    {"modifications", "edit"},
    // Grep CLI-style flag aliases.
    {"-A", "after_context"},
    {"-B", "before_context"},
    {"-C", "context"},
    {"-n", "line_number"},
    {"-i", "ignore_case"},
};

const char *kStreamArgKeys[] = {
    "content", "code", "prompt", "old", "new", "question", "context", "instruction",
    "command", // stream.py:343-350
};

const char *kInlineArgKeys[] = {
    "command", // stream.py:362-364
};

struct clist_key_color {
    const char *key;
    bool indexed;
    int32_t code;
};

const clist_key_color kStreamKeyColors[] = {
    {"old", false, kFgBrightRed},        // stream.py:463
    {"new", false, kFgBrightGreen},      // stream.py:464
    {"code", false, kFgBrightBlue},      // stream.py:465
    {"prompt", false, kFgBrightYellow},  // stream.py:466
    {"question", false, kFgBrightYellow},// stream.py:467
    {"instruction", false, kFgBrightYellow}, // stream.py:468
    {"content", false, kFgBrightBlack},  // stream.py:469
    {"context", true, kGray256},         // stream.py:470
    {"source_code", false, kFgBrightCyan}, // stream.py:471
    {"text", true, kGrayLight256},       // stream.py:472
    {"task", false, kFgBrightYellow},    // stream.py:473
    {"command", false, kFgBrightBlue},   // stream.py:474
};

// _canonical_key: exact hit first, then the lower-cased key (stream.py:283-291).
kimix::string clist_canonical_key(kimix::string_view key) {
    for (const clist_key_alias &row : kKeyAliases) {
        if (key == row.from) {
            return kimix::string(row.to);
        }
    }
    const kimix::string lower = clist_lower_ascii(key);
    for (const clist_key_alias &row : kKeyAliases) {
        if (lower == row.from) {
            return kimix::string(row.to);
        }
    }
    return kimix::string(key);
}

bool clist_in_table(kimix::string_view key, const char *const *table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (key == table[i]) {
            return true;
        }
    }
    return false;
}

bool clist_is_stream_arg_key(kimix::string_view key) {
    return clist_in_table(key, kStreamArgKeys,
                          sizeof(kStreamArgKeys) / sizeof(kStreamArgKeys[0]));
}

bool clist_is_inline_arg_key(kimix::string_view key) {
    return clist_in_table(key, kInlineArgKeys,
                          sizeof(kInlineArgKeys) / sizeof(kInlineArgKeys[0]));
}

// _stream_color_for_key: unlisted keys fall back to GRAY_LIGHT (stream.py:496-499).
void clist_stream_color(kimix::string_view key, bool &indexed, int32_t &code) {
    for (const clist_key_color &row : kStreamKeyColors) {
        if (key == row.key) {
            indexed = row.indexed;
            code = row.code;
            return;
        }
    }
    indexed = true;
    code = kGrayLight256;
}

// stream.py:449-452.
char clist_simple_escape(char c) {
    switch (c) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case '"':
        return '"';
    case '\\':
        return '\\';
    case '/':
        return '/';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    default:
        return '\0';
    }
}

// stream.py:458.
kimix::string_view clist_bare_literal(kimix::string_view text) {
    if (text == "true") {
        return "True";
    }
    if (text == "false") {
        return "False";
    }
    if (text == "null") {
        return "None";
    }
    return text;
}

int32_t clist_hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void clist_append_utf8(kimix::string &out, int32_t code_point) {
    if (code_point < 0) {
        return;
    }
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

// orjson refuses any document containing a \uD800-\uDFFF escape ("str is not
// valid UTF-8: surrogates not allowed") - it decodes them to UTF-16 surrogates.
// yyjson instead combines a surrogate pair into one code point, so the gate has
// to mirror orjson's refusal explicitly: whether the document validates decides
// if finish() terminates the streamed line, which is visible output.
bool clist_has_surrogate_escape(kimix::string_view text) {
    size_t i = 0;
    while (i + 1 < text.size()) {
        if (text[i] != '\\') {
            ++i;
            continue;
        }
        if (text[i + 1] == 'u' && i + 5 < text.size()) {
            int32_t code_point = 0;
            bool ok = true;
            for (size_t k = 2; k < 6; ++k) {
                const int32_t digit = clist_hex_value(text[i + k]);
                if (digit < 0) {
                    ok = false;
                    break;
                }
                code_point = code_point * 16 + digit;
            }
            if (ok) {
                if (code_point >= 0xD800 && code_point <= 0xDFFF) {
                    return true;
                }
                i += 6;
                continue;
            }
        }
        i += 2; // "\n" or "\\": neither introduces a surrogate escape
    }
    return false;
}

// orjson.loads equivalent for the completion gate (yyjson with the project's
// mimalloc-backed allocator, see src/llm/yyjson_alc.h).
bool clist_json_valid(kimix::string_view text) {
    if (text.empty() || clist_has_surrogate_escape(text)) {
        return false;
    }
    yyjson_read_err err{};
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(text.data()), text.size(), 0,
                                      &kimix::llm::kYYJsonAlcMi, &err);
    if (doc == nullptr) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

// stream.py:1027.
bool clist_is_trivial_message(kimix::string_view message) {
    return message == "success" || message == "failed" || message == "[rtk] success" ||
           message == "[rtk] failed";
}

} // namespace

// ---------------------------------------------------------------------------
// Display blocks (stream.py:136-184)
// ---------------------------------------------------------------------------
kimix::vector<kimix::string> display_block_parts(const display_block &block) {
    kimix::vector<kimix::string> parts;
    switch (block.kind) {
    case display_block_kind::brief:
        if (!block.text.empty()) { // stream.py:146-148
            parts.push_back(clist_colorful(block.text, kFgBrightBlack));
        }
        break;
    case display_block_kind::diff: { // stream.py:149-155
        kimix::string header(kDiffHeader);
        header.append(block.text);
        parts.push_back(clist_colorful(header, kFgBrightYellow));
        kimix::vector<kimix::string> lines;
        clist_splitlines(block.old_text, lines);
        for (const kimix::string &line : lines) {
            kimix::string body("- ");
            body.append(line);
            parts.push_back(clist_colorful(body, kFgBrightRed));
        }
        lines.clear();
        clist_splitlines(block.new_text, lines);
        for (const kimix::string &line : lines) {
            kimix::string body("+ ");
            body.append(line);
            parts.push_back(clist_colorful(body, kFgBrightGreen));
        }
        break;
    }
    case display_block_kind::todo: // stream.py:156-167
        for (const todo_display_item &item : block.items) {
            kimix::string status(item.status);
            for (char &c : status) {
                if (c == '_') {
                    c = ' ';
                }
            }
            status = clist_lower_ascii(status);
            if (status == "done") {
                kimix::string body("- ~~");
                body.append(item.title);
                body.append("~~");
                parts.push_back(clist_colorful(body, kFgBrightBlack));
            } else if (status == "in progress") {
                kimix::string body("- ");
                body.append(item.title);
                body.push_back(' ');
                body.append(kArrow);
                parts.push_back(clist_colorful(body, kFgBrightYellow));
            } else {
                kimix::string body("- ");
                body.append(item.title);
                parts.push_back(clist_colorful_256(body, kGrayLight256));
            }
        }
        break;
    case display_block_kind::shell:
        break; // stream.py:168-170: nothing
    case display_block_kind::background: { // stream.py:171-175
        kimix::string body("[");
        body.append(block.status);
        body.append("] ");
        body.append(block.task_id);
        body.append(": ");
        body.append(block.text);
        parts.push_back(clist_colorful(body, kFgBrightBlack));
        break;
    }
    case display_block_kind::unknown: // stream.py:176-177
        parts.push_back(clist_colorful(block.text, kFgBrightBlack));
        break;
    case display_block_kind::base: // stream.py:178-181
        if (!block.text.empty()) {
            parts.push_back(clist_colorful_256(block.text, kGrayLight256));
        }
        break;
    }
    return parts;
}

kimix::string format_display_blocks(const kimix::vector<display_block> &blocks) {
    kimix::vector<kimix::string> parts;
    for (const display_block &block : blocks) {
        const kimix::vector<kimix::string> block_parts = display_block_parts(block);
        for (const kimix::string &part : block_parts) {
            parts.push_back(part);
        }
    }
    if (parts.empty()) {
        return {}; // the reference returns None
    }
    kimix::string out = clist_join(parts, "\n");
    out.push_back('\n'); // always ends with one newline (stream.py:184)
    return out;
}

// ---------------------------------------------------------------------------
// Percentage / banner helpers (stream.py:117-133, 1183-1189)
// ---------------------------------------------------------------------------
kimix::string percentage_str(double ratio) {
    return kimix::format("{:.1f}%", ratio * 100.0);
}

kimix::string percentage_and_token(double ratio, int64_t tokens) {
    return kimix::format("{:.1f}% ({} tokens)", ratio * 100.0, tokens);
}

kimix::string context_usage_banner(double ratio, int64_t tokens) {
    kimix::string left(kBannerEqualCount, '=');
    left.append(kUsageLabel); // ASCII only: byte length == character length
    left.append(percentage_and_token(ratio, tokens));
    left.push_back(' ');
    const size_t pad = (kBannerWidth > left.size()) ? (kBannerWidth - left.size()) : 1;
    kimix::string out(left);
    out.append(pad, '=');
    return out;
}

// ---------------------------------------------------------------------------
// stream_renderer
// ---------------------------------------------------------------------------
stream_renderer::stream_renderer(bool show_thinking, bool show_usage)
    : show_thinking_(show_thinking), show_usage_(show_usage), out_(stdout),
      last_char_was_newline_(true), stream_state_(kStreamStateOther),
      message_type_(message_type::none), ratio_(0.0), tokens_(0), has_printer_(false) {
    printer_.owner = this;
}

stream_renderer::stream_renderer(const stream_renderer &other)
    : show_thinking_(other.show_thinking_), show_usage_(other.show_usage_), out_(other.out_),
      last_char_was_newline_(other.last_char_was_newline_),
      stream_state_(other.stream_state_), message_type_(other.message_type_),
      ratio_(other.ratio_), tokens_(other.tokens_), captured_text_(other.captured_text_),
      printer_(other.printer_), has_printer_(other.has_printer_) {
    printer_.owner = this;
}

stream_renderer &stream_renderer::operator=(const stream_renderer &other) {
    if (this == &other) {
        return *this;
    }
    show_thinking_ = other.show_thinking_;
    show_usage_ = other.show_usage_;
    out_ = other.out_;
    last_char_was_newline_ = other.last_char_was_newline_;
    stream_state_ = other.stream_state_;
    message_type_ = other.message_type_;
    ratio_ = other.ratio_;
    tokens_ = other.tokens_;
    captured_text_ = other.captured_text_;
    printer_ = other.printer_;
    has_printer_ = other.has_printer_;
    printer_.owner = this;
    return *this;
}

void stream_renderer::set_output(std::FILE *out) {
    out_ = (out != nullptr) ? out : stdout;
}

std::FILE *stream_renderer::output() const {
    return out_;
}

void stream_renderer::write_bytes(kimix::string_view text, bool flush) {
    if (text.empty()) {
        return;
    }
    if (out_ == stdout) {
        print_raw(text); // cli_print's plain sink: no colour, no newline
    } else {
        std::fwrite(text.data(), 1, text.size(), out_);
    }
    if (flush) {
        std::fflush(out_);
    }
}

// PrintStream.print_word: print `rendered`, tracking the newline state from the
// pre-colour `raw` text (stream.py printing.py:391-405).
void stream_renderer::emit_word(kimix::string_view rendered, bool require_new_line,
                                kimix::string_view raw, bool flush) {
    if (rendered.empty()) {
        if (require_new_line && !last_char_was_newline_) {
            write_bytes("\n", flush);
            last_char_was_newline_ = true;
            reset_print_state();
        }
        return;
    }
    if (require_new_line && !last_char_was_newline_) {
        write_bytes("\n", flush);
    }
    write_bytes(rendered, flush);
    last_char_was_newline_ = clist_ends_with_newline(raw);
    if (last_char_was_newline_) {
        // Keep cli_print's process-wide tracker truthful: its print_word derives
        // the flag from the rendered text, where a trailing "\x1b[0m" hides the
        // newline.  Without this a later print_word/print_info would insert a
        // newline that was already emitted.
        reset_print_state();
    }
}

void stream_renderer::emit_colored(kimix::string_view raw, int fg, bool require_new_line,
                                   bool flush) {
    const kimix::string rendered = clist_colorful(raw, fg);
    emit_word(rendered, require_new_line, raw, flush);
}

void stream_renderer::emit_colored_256(kimix::string_view raw, int fg256,
                                       bool require_new_line, bool flush) {
    const kimix::string rendered = clist_colorful_256(raw, fg256);
    emit_word(rendered, require_new_line, raw, flush);
}

// stream.py:117-133.
void stream_renderer::transition(message_type type) {
    if (type == message_type::none) {
        return;
    }
    if (show_usage_ && message_type_ != message_type::none && message_type_ != type) {
        const kimix::string banner = context_usage_banner(ratio_, tokens_);
        kimix::string line(banner);
        line.push_back('\n'); // stream.py:128 prints f"{left}{right_split}\n"
        emit_colored_256(line, kGray256, true, true);
    }
    message_type_ = type;
}

void stream_renderer::finish_tool_call_stream() {
    if (has_printer_) {
        printer_.finish(); // clears has_printer_ as well
        has_printer_ = false;
    }
}

void stream_renderer::on_step_begin(int32_t step, int32_t max_steps) {
    (void)step;
    (void)max_steps;
    finish_tool_call_stream(); // print_agent_json: non-ToolCall message
    // StepBegin is _handle_noop: nothing is printed (stream.py:1111).
}

void stream_renderer::on_text_delta(kimix::string_view delta) {
    transition(message_type::text);
    finish_tool_call_stream();
    captured_text_.append(delta);
    // stream.py:1096-1098.
    emit_word(delta, stream_state_ != kStreamStateText, delta, false);
    stream_state_ = kStreamStateText;
}

void stream_renderer::on_reasoning_delta(kimix::string_view delta) {
    transition(message_type::thinking);
    finish_tool_call_stream();
    // stream.py:1072-1083: the `if not _quiet` gate also skips the state update.
    if (!show_thinking_ || quiet()) {
        return;
    }
    if (stream_state_ != kStreamStateThinking) {
        kimix::string banner(kThinkBanner);
        banner.append(delta);
        emit_colored(banner, kFgBrightCyan, true, true);
    } else {
        emit_colored(delta, kFgBrightCyan, false, true);
    }
    stream_state_ = kStreamStateThinking;
}

void stream_renderer::on_tool_call_begin(const kimix::llm::ToolCall &call) {
    transition(message_type::tool_calling);
    finish_tool_call_stream(); // a new call supersedes any previous printer
    kimix::string header(kThunder);
    header.push_back(' ');
    header.append(call.name);
    // stream.py:918-920: require_new_line=True guarantees a fresh line.
    emit_colored(header, kFgBrightMagenta, true, true);
    stream_state_ = kStreamStateOther;
    printer_.reset();
    printer_.owner = this;
    has_printer_ = true;
    if (!call.arguments.empty()) {
        printer_.feed(call.arguments);
    }
}

void stream_renderer::on_tool_call_args_delta(kimix::string_view delta) {
    transition(message_type::tool_calling);
    if (has_printer_) {
        printer_.feed(delta);
    } else {
        emit_word("", true, kimix::string_view(), false); // stream.py:986
    }
    stream_state_ = kStreamStateOther; // stream.py:987
}

void stream_renderer::on_tool_result(kimix::string_view name, bool ok,
                                     kimix::string_view message,
                                     kimix::string_view output_summary) {
    transition(message_type::tool_calling);
    finish_tool_call_stream();
    // The display half of _handle_tool_result.  The frozen signature carries a
    // plain summary instead of wire blocks, so it is rendered as a Brief block.
    kimix::string display;
    if (!output_summary.empty()) {
        kimix::vector<display_block> blocks;
        display_block brief;
        brief.kind = display_block_kind::brief;
        brief.text = output_summary;
        blocks.push_back(brief);
        display = format_display_blocks(blocks);
    }
    emit_word(display, true, display, false); // stream.py:993 (None -> newline only)

    const kimix::string prefix = ok ? kCheck : kCross; // "✓ " / "✗ "
    const int32_t result_fg = ok ? kFgBrightGreen : kFgBrightRed;
    if (!name.empty()) {
        kimix::string line(prefix);
        line.push_back(' ');
        line.append(name);
        emit_colored(line, result_fg, true, true); // stream.py:1020-1025
        if (!message.empty() && !clist_is_trivial_message(message)) {
            kimix::string detail(kDimDetailPrefix);
            detail.append(message);
            emit_colored(detail, kFgBrightBlack, true, true); // stream.py:1027-1033
        }
    } else if (!message.empty()) {
        kimix::string line(prefix);
        line.push_back(' ');
        line.append(message);
        emit_colored(line, result_fg, true, true); // stream.py:1034-1041
    } else {
        emit_word("", true, kimix::string_view(), true); // stream.py:1043
    }
    stream_state_ = kStreamStateOther; // stream.py:1045
}

void stream_renderer::on_display_blocks(const kimix::vector<display_block> &blocks) {
    finish_tool_call_stream();
    const kimix::string text = format_display_blocks(blocks);
    // stream.py:993: no colour of its own, each part is pre-coloured.
    emit_word(text, true, text, false);
}

void stream_renderer::on_compaction_begin() {
    finish_tool_call_stream();
    emit_colored(kCompacting, kFgBrightMagenta, true, true); // stream.py:1060-1062
}

void stream_renderer::on_compaction_end(bool ok) {
    (void)ok;
    finish_tool_call_stream();
    // CompactionEnd is _handle_noop: no output (stream.py:1113).
}

void stream_renderer::on_context_usage(double ratio, int64_t tokens) {
    ratio_ = ratio;
    tokens_ = tokens;
    // The reference reads session.status when it prints the transition banner,
    // so this only records the snapshot.
}

void stream_renderer::on_error(kimix::string_view message) {
    finish_tool_call_stream();
    // Native addition: printing.py's print_error colours (BRIGHT_RED + BOLD),
    // written to the renderer's stream so it stays capturable and in order.
    const kimix::string rendered = clist_colorful_styled(message, kFgBrightRed, "1");
    emit_word(rendered, true, message, true);
}

void stream_renderer::finish_turn() {
    finish_tool_call_stream();
    // print_agent_json_flush_text() is a no-op here: text is printed live (the
    // CLI does not use format_output=True / the markdown buffer).
}

const kimix::string &stream_renderer::captured_text() const {
    return captured_text_;
}

void stream_renderer::reset_capture() {
    captured_text_.clear();
}

// ---------------------------------------------------------------------------
// _ToolCallStreamPrinter (stream.py:418-856)
// ---------------------------------------------------------------------------
void stream_renderer::arg_printer::reset() {
    state = expect_value;
    stack.clear();
    current_key.clear();
    key_chars.clear();
    value_chars.clear();
    emit_chars.clear();
    escape_buf.clear();
    in_escape = false;
    pending_high_surrogate = -1;
    string_streamed = false;
    color_indexed = true;
    color_code = kGrayLight256;
    json_parts.clear();
    finished = false;
    broken = false;
    bytes_since_flush = 0;
}

void stream_renderer::arg_printer::feed(kimix::string_view fragment) {
    if (finished) {
        return;
    }
    if (!fragment.empty()) {
        json_parts.append(fragment);
        if (broken) {
            // Defensive fallback of stream.py:509-520.  The C++ lexer cannot
            // fail (no exceptions), so this stays unreachable.
            owner->emit_colored_256(fragment, kGrayLight256, false, true);
        } else {
            lex(fragment);
            flush_emit(false);
        }
    }
    check_complete();
}

void stream_renderer::arg_printer::finish() {
    if (finished) {
        return;
    }
    finished = true;
    if (in_escape && !escape_buf.empty()) {
        // Incomplete escape at end of input: emit verbatim (stream.py:529-533).
        append_value_char(escape_buf);
        in_escape = false;
        escape_buf.clear();
    }
    if (pending_high_surrogate >= 0) {
        append_value_char(kReplacement);
        pending_high_surrogate = -1;
    }
    if (state == in_string) {
        if (string_streamed) {
            flush_emit(true);
        } else if (!value_chars.empty()) {
            kimix::string text(value_chars);
            text.append("...");
            emit_compact(text);
        }
    } else if (state == in_bare && !value_chars.empty()) {
        end_bare_value();
    } else {
        flush_emit(true);
    }
    owner->emit_word("", true, kimix::string_view(), true); // stream.py:548
    owner->stream_state_ = kStreamStateOther;
    json_parts.clear();
    owner->has_printer_ = false; // stream.py:552-553 (leaves _tmp_data)
}

void stream_renderer::arg_printer::lex(kimix::string_view fragment) {
    // Reduction: stream.py:585-632 bulk-consumes boring spans with C-level
    // find()/regex fast paths.  Those are pure performance - per-character
    // dispatch appends exactly the same bytes to the same buffers - so only the
    // per-char state machine is ported here.
    for (char ch : fragment) {
        if (in_escape) {
            feed_escape_char(ch);
            continue;
        }
        if (state == done) {
            return; // characters after a complete document are ignored
        }
        feed_char(ch);
    }
}

void stream_renderer::arg_printer::feed_char(char ch) {
    if (in_escape) {
        feed_escape_char(ch);
        return;
    }
    if (state == done) {
        return;
    }
    if (state == expect_key) {
        if (ch == '"') {
            key_chars.clear();
            state = in_key;
        } else if (ch == '}') {
            close_container();
        }
    } else if (state == in_key) {
        if (ch == '\\') {
            in_escape = true;
            escape_buf = "\\";
        } else if (ch == '"') {
            current_key = key_chars;
            state = expect_colon;
        } else {
            key_chars.push_back(ch);
        }
    } else if (state == expect_colon) {
        if (ch == ':') {
            state = expect_value;
        }
    } else if (state == expect_value) {
        if (ch == '"') {
            begin_string_value();
        } else if (ch == '{') {
            stack.push_back('{');
            state = expect_key;
        } else if (ch == '[') {
            stack.push_back('[');
        } else if (ch == ']' || ch == '}') {
            close_container();
        } else if (!clist_is_json_space(ch)) {
            value_chars.assign(1, ch);
            state = in_bare;
        }
    } else if (state == in_string) {
        if (ch == '\\') {
            in_escape = true;
            escape_buf = "\\";
        } else if (ch == '"') {
            end_string_value();
        } else {
            append_value_char(kimix::string_view(&ch, 1));
        }
    } else if (state == in_bare) {
        if (ch == ',') {
            end_bare_value();
            after_comma();
        } else if (ch == '}' || ch == ']') {
            end_bare_value();
            close_container();
        } else if (clist_is_json_space(ch)) {
            end_bare_value();
        } else {
            value_chars.push_back(ch);
        }
    } else if (state == after_value) {
        if (ch == ',') {
            after_comma();
        } else if (ch == '}' || ch == ']') {
            close_container();
        }
    }
}

void stream_renderer::arg_printer::feed_escape_char(char ch) {
    escape_buf.push_back(ch);
    const size_t n = escape_buf.size();
    const bool unicode_escape = n >= 2 && escape_buf[0] == '\\' && escape_buf[1] == 'u';
    if (n == 2 && escape_buf[1] != 'u') {
        const char decoded = clist_simple_escape(escape_buf[1]);
        reset_escape();
        if (decoded == '\0') {
            // Unknown two-char escape (e.g. a single-backslash Windows path or a
            // regex "\d"): keep the backslash verbatim (stream.py:702-710).
            kimix::string verbatim("\\");
            verbatim.push_back(ch);
            append_value_char(verbatim);
        } else {
            append_value_char(kimix::string_view(&decoded, 1));
        }
        return;
    }
    if (unicode_escape && n == 6) {
        const kimix::string buffered(escape_buf); // copy: reset_escape() clears it
        reset_escape();
        int32_t code_point = 0;
        bool ok = true;
        for (size_t i = 2; i < 6; ++i) {
            const int32_t digit = clist_hex_value(buffered[i]);
            if (digit < 0) {
                ok = false;
                break;
            }
            code_point = code_point * 16 + digit;
        }
        if (!ok) {
            // stream.py:714-717: a broken "\u" escape is emitted verbatim.
            append_value_char(buffered);
            return;
        }
        handle_code_point(code_point);
        return;
    }
    if (n > 6 || (n > 2 && !unicode_escape)) {
        const kimix::string buffered(escape_buf); // copy before reset
        reset_escape();
        append_value_char(buffered);
        return;
    }
    // Incomplete "\u" escape whose next character is not a hex digit: this was
    // never a unicode escape, so emit the buffered prefix verbatim and reprocess
    // the character (stream.py:723-741).
    if (n > 2 && clist_hex_value(ch) < 0) {
        const kimix::string head = escape_buf.substr(0, n - 1);
        reset_escape();
        append_value_char(head);
        feed_char(ch);
    }
}

void stream_renderer::arg_printer::reset_escape() {
    in_escape = false;
    escape_buf.clear();
}

void stream_renderer::arg_printer::handle_code_point(int32_t code_point) {
    if (pending_high_surrogate >= 0) {
        const int32_t high = pending_high_surrogate;
        pending_high_surrogate = -1;
        if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            kimix::string out;
            clist_append_utf8(out, 0x10000 + ((high - 0xD800) << 10) + (code_point - 0xDC00));
            append_value_char(out);
            return;
        }
        append_value_char(kReplacement);
    }
    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
        pending_high_surrogate = code_point;
    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
        append_value_char(kReplacement);
    } else {
        kimix::string out;
        clist_append_utf8(out, code_point);
        append_value_char(out);
    }
}

void stream_renderer::arg_printer::append_value_char(kimix::string_view s) {
    if (state == in_key) {
        key_chars.append(s);
    } else if (string_streamed) {
        emit_chars.append(s);
    } else {
        value_chars.append(s);
    }
}

void stream_renderer::arg_printer::begin_string_value() {
    current_key = clist_canonical_key(current_key); // stream.py:772
    string_streamed = clist_is_stream_arg_key(current_key);
    value_chars.clear();
    state = in_string;
    if (!string_streamed) {
        return;
    }
    clist_stream_color(current_key, color_indexed, color_code);
    if (clist_is_inline_arg_key(current_key)) {
        // Inline: a space, no label - the value follows on the header line.
        owner->emit_colored_256(" ", kGray256, false, true); // stream.py:780-781
    } else {
        kimix::string label("\n");
        label.append(current_key);
        label.append(":\n"); // _separator() == "\n" (stream.py:783-785)
        owner->emit_colored_256(label, kGray256, false, true);
    }
}

void stream_renderer::arg_printer::end_string_value() {
    if (string_streamed) {
        if (pending_high_surrogate >= 0) {
            emit_chars.append(kReplacement);
            pending_high_surrogate = -1;
        }
        flush_emit(true);
    } else {
        emit_compact(value_chars);
    }
    value_chars.clear();
    state = after_value;
}

void stream_renderer::arg_printer::end_bare_value() {
    const kimix::string text(value_chars);
    value_chars.clear();
    emit_compact(clist_bare_literal(text));
    state = after_value;
}

void stream_renderer::arg_printer::emit_compact(kimix::string_view text) {
    const kimix::string clipped = clist_truncate_compact(text);
    const kimix::string key =
        current_key.empty() ? kimix::string() : clist_canonical_key(current_key);
    kimix::string segment(" ");
    if (!key.empty()) {
        segment.append(key);
        segment.push_back(':');
    }
    segment.append(clipped);
    // stream.py:821-822: bright magenta, stays inline on the header line.
    owner->emit_colored(segment, kFgBrightMagenta, false, true);
}

void stream_renderer::arg_printer::flush_emit(bool flush) {
    if (emit_chars.empty()) {
        return;
    }
    const kimix::string chunk(emit_chars);
    emit_chars.clear();
    bytes_since_flush += chunk.size();
    if (flush || bytes_since_flush >= kFlushIntervalBytes) {
        bytes_since_flush = 0;
        flush = true;
    }
    if (color_indexed) {
        owner->emit_colored_256(chunk, color_code, false, flush);
    } else {
        owner->emit_colored(chunk, color_code, false, flush);
    }
}

void stream_renderer::arg_printer::after_comma() {
    if (!stack.empty() && stack.back() == '{') {
        state = expect_key;
    } else if (!stack.empty()) {
        state = expect_value;
    }
}

void stream_renderer::arg_printer::close_container() {
    if (!stack.empty()) {
        stack.pop_back();
    }
    state = stack.empty() ? done : after_value;
}

void stream_renderer::arg_printer::check_complete() {
    if (json_parts.empty()) {
        return;
    }
    if (broken) {
        const size_t last = json_parts.find_last_not_of(" \t\r\n");
        if (last == kimix::string::npos) {
            return;
        }
        const char tail = json_parts[last];
        if (tail != '}' && tail != ']' && tail != '"') {
            return;
        }
    } else if (state != done) {
        if (!stack.empty() || (state != after_value && state != in_bare)) {
            return;
        }
    }
    if (!clist_json_valid(json_parts)) {
        return;
    }
    finish();
}

} // namespace kimix::cli
