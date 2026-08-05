/*
 * incremental_lexer.h -- Incremental JSON lexer (kimix::runtime::json).
 *
 * Plan 010: native port of the pure-Python streamingjson incremental lexer
 * used for streaming tool-call arguments. Char-level token-stack state
 * machine over an append-only buffer:
 *
 *   - feed(chunk): incremental; state (strings, escapes, \uXXXX, comments,
 *     numbers, literals) persists across chunk boundaries -- a \uD83D split
 *     across two feeds is handled exactly like one feed.
 *   - is_complete(): a complete top-level JSON value (depth 0 after a value
 *     token, no pending comma). Whitespace/comments after the value are
 *     fine; any other content after the value is an error (two top-level
 *     values).
 *   - has_error(): malformed input (unmatched/mismatched close, missing
 *     value `{"a": }`, bad escape, invalid number/literal, junk after a
 *     complete value). Once set, feed() ignores further input until reset().
 *   - value_span(key)/top_level_keys(): byte spans of top-level object
 *     key -> value pairs, recorded WITHOUT reparsing. Spans are byte
 *     offsets into the internal buffer (append-only; offsets stay valid,
 *     but callers should copy the bytes they need before the next feed).
 *
 * Relaxed parsing (matches the documented streamingjson loads_relaxed
 * scope): trailing commas (`[1,2,]`, `{"a":1,}`) and // line / block
 * comments are accepted wherever whitespace is allowed (outside
 * strings). Raw control characters INCLUDING newlines are allowed inside
 * strings (LLM streams emit them unescaped; strict JSON would reject).
 *
 * Pure C++ kernel compiled into runtime.dll: no Python includes, no RTTI.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace json {

struct top_level_span {
    kimix::string key;
    size_t start = 0; // byte offset of the value's first token
    size_t end = 0;   // byte offset one past the value's last token
};

class KIMIX_RUNTIME_API IncrementalJsonLexer {
public:
    IncrementalJsonLexer() = default;
    ~IncrementalJsonLexer() = default;

    IncrementalJsonLexer(const IncrementalJsonLexer&) = delete;
    IncrementalJsonLexer& operator=(const IncrementalJsonLexer&) = delete;

    // Feed a chunk of UTF-8/JSON text. State is incremental; spans recorded
    // before this call stay valid as offsets (the buffer is append-only).
    void feed(kimix::string_view chunk) noexcept;

    // True once a complete top-level JSON value has been seen (and no error).
    bool is_complete() const noexcept { return _complete && !_error; }

    // True once malformed input was detected. Sticky until reset().
    bool has_error() const noexcept { return _error; }

    // Byte span of a top-level key's value in the internal buffer. False if
    // the key is unknown or its value has not completed yet. Searches from
    // the most recent pair backwards (Python dict semantics for dup keys).
    bool value_span(kimix::string_view key, size_t& start, size_t& end) const noexcept;

    // Keys of the top-level object in document order (only completed pairs).
    kimix::vector<kimix::string> top_level_keys() const;

    // All recorded top-level (key, start, end) spans (document order).
    kimix::span<const top_level_span> spans() const noexcept {
        return kimix::span<const top_level_span>(_spans.data(), _spans.size());
    }

    // Drop all state (buffer, spans, flags).
    void reset() noexcept;

private:
    struct Frame {
        uint8_t kind; // 0 = object, 1 = array
        uint8_t sub;  // per-kind substate, see enum in .cpp
    };

    void scan(size_t from);
    void set_error() noexcept { _error = true; }
    void record_span(size_t end) noexcept;
    void append_utf8(uint32_t cp) noexcept;

    kimix::string _buf;      // append-only accumulation
    size_t _processed = 0;   // resume point inside _buf
    bool _complete = false;
    bool _error = false;
    bool _root_done = false; // root value finished; extra value -> error

    kimix::vector<Frame> _stack; // open containers (innermost last)

    // scanner state
    uint8_t _scan = 0;         // see SCAN_* enum in .cpp
    uint8_t _number_sub = 0;   // see NUM_* enum in .cpp
    uint8_t _literal_kind = 0; // LIT_TRUE / LIT_FALSE / LIT_NULL
    size_t _literal_pos = 0;   // chars of the literal matched so far
    uint8_t _unicode_need = 0; // hex digits still needed for \uXXXX
    uint32_t _unicode_val = 0; // hex digits collected so far
    bool _comment_star = false; // '*' seen inside a block comment

    // top-level key -> value span bookkeeping
    bool _in_key = false;      // currently scanning a key string
    kimix::string _str_buf;    // decoded bytes of the current key string
    bool _pending_high = false;
    uint32_t _pending_high_val = 0;
    bool _have_key = false;    // top-level key name captured, awaiting value
    kimix::string _cur_key;    // decoded top-level key name
    bool _want_span = false;   // ':' consumed for the key; value start unknown
    size_t _value_start = 0;   // byte offset where the key's value begins
    kimix::vector<top_level_span> _spans;
};

} // namespace json
} // namespace runtime
} // namespace kimix
