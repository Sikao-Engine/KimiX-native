/*
 * ansi.h — Streaming ANSI escape-sequence stripper (kimix::runtime::stream).
 *
 * Plan 003: native port of `_ANSI_ESCAPE_RE` from
 * `src/kimix/tools/common.py` (regex sub in filter_output), as a state
 * machine that reproduces the regex semantics EXACTLY — including the
 * alternation order and backtracking fallbacks. Runs per 4096-byte chunk,
 * so the machine must hold partial-escape state across chunk boundaries.
 *
 * Regex (verified against Python `re`/`regex` on a battery of cases):
 *   \x1B(?:
 *       \][^\x07\x1B]*(?:\x07|\x1B\\)          # OSC (BEL or ESC\ terminated)
 *     | [P^_][^\x07\x1B]*(?:\x07|\x1B\\)      # DCS / PM / APC
 *     | [@-Z\\-_]                              # single Fe: 0x40-0x5A, 0x5C-0x5F
 *     | \[[0-?]*[ -/]*[@-~]                    # CSI
 *   )
 *
 * Verified regex facts this machine encodes:
 * - The Fe class `[@-Z\\-_]` is parsed by Python as `@-Z` (0x40-0x5A) plus
 *   the range `\\-_` (0x5C-0x5F); `[` (0x5B) is NOT in it, `-` (0x2D) is not.
 * - DCS/PM/APC (`P`, `^`, `_` — all inside the Fe class) is tried BEFORE Fe:
 *   a terminated `\x1bP...` is consumed as DCS; an unterminated one falls
 *   back to the 2-byte Fe match (`\x1bP`, `\x1b^`, `\x1b_` are removed).
 * - OSC/DCS/PM/APC strings are only matched when terminated (BEL or ESC\);
 *   on failure the regex falls back to the Fe match at the original ESC
 *   (e.g. `\x1b]a\x1bX` -> `\x1b]` removed via Fe, `a` kept, `\x1bX` removed).
 * - CSI enforces the order params (0x30-0x3F) then intermediates (0x20-0x2F)
 *   then a final byte (0x40-0x7E); `\x1b[1 2m` is NOT a match.
 * - Unterminated escapes at end-of-stream stay in the text (the regex finds
 *   no match) except for the DCS/OSC Fe fallback above.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace stream {

// One-shot ANSI strip: `strip_ansi(filter_output first half)`. Equivalent to
// feeding the whole buffer through AnsiStripper and flushing.
KIMIX_RUNTIME_API kimix::string strip_ansi(kimix::string_view utf8);

// Streaming state machine. Feed byte chunks; cleaned bytes are appended to
// `out` (the output of a chunk may be empty when the chunk is fully consumed
// by an escape sequence). Bytes belonging to a partial escape are withheld
// until the escape completes or flush() is called. Call flush() at
// end-of-stream to emit bytes held by unterminated escapes.
class KIMIX_RUNTIME_API AnsiStripper {
public:
    AnsiStripper() = default;
    ~AnsiStripper() = default;

    AnsiStripper(const AnsiStripper&) = delete;
    AnsiStripper& operator=(const AnsiStripper&) = delete;
    AnsiStripper(AnsiStripper&&) noexcept = default;
    AnsiStripper& operator=(AnsiStripper&&) noexcept = default;

    // Process one chunk of bytes; append cleaned bytes to `out`.
    void feed(kimix::string_view chunk, kimix::string& out);

    // End-of-stream: resolve any partial escape (unterminated escapes are
    // emitted as-is, modulo the OSC/DCS Fe fallback) and reset the state.
    void flush(kimix::string& out);

    // Clear all state (held bytes, escape state) without emitting anything.
    void reset();

private:
    enum class state : uint8_t {
        normal,        // plain text
        after_esc,     // ESC seen; next byte decides the branch
        osclike,       // ESC ] / ESC P / ESC ^ / ESC _ ... (OSC/DCS/PM/APC)
        osclike_esc,   // ... ESC seen inside an osclike string (terminator?)
        csi_params,    // ESC [ ... params (0x30-0x3F) and intermediates
        csi_inter,     // ESC [ ... intermediates (0x20-0x2F) only
    };

    void fail_escape(kimix::string& out);
    void flush_state(kimix::string& out);

    state state_ = state::normal;
    kimix::string pending_; // bytes from the ESC onward, undecided
    uint8_t first_ = 0;     // byte right after ESC when in osclike
};

} // namespace stream
} // namespace runtime
} // namespace kimix
