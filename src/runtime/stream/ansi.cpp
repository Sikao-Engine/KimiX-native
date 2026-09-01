/*
 * ansi.cpp — implementation of the streaming ANSI stripper.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua.
 */

#include <runtime/stream/ansi.h>

namespace kimix {
namespace runtime {
namespace stream {

namespace {

// Fe class of `[@-Z\\-_]` as Python parses it: 0x40-0x5A plus 0x5C-0x5F.
// (`[` 0x5B and `-` 0x2D are NOT in it — verified against Python.)
inline bool is_fe_byte(uint8_t b) noexcept {
    return (b >= 0x40u && b <= 0x5Au) || (b >= 0x5Cu && b <= 0x5Fu);
}

// DCS / PM / APC introducers: ESC P (0x50), ESC ^ (0x5E), ESC _ (0x5F).
inline bool is_dcs_like(uint8_t b) noexcept {
    return b == 0x50u || b == 0x5Eu || b == 0x5Fu;
}

} // namespace

void AnsiStripper::reset() {
    state_ = state::normal;
    pending_.clear();
    first_ = 0;
}

// Handle a failed escape: emit what the regex would leave in the text.
// Called when the current byte cannot continue the escape; the caller then
// reprocesses that byte from the (possibly new) state.
void AnsiStripper::fail_escape(kimix::string& out) {
    if (state_ == state::osclike || state_ == state::osclike_esc) {
        // OSC/DCS/PM/APC failed to terminate. The regex then tries the Fe
        // branch at the original ESC: `]`, `P`, `^`, `_` are all in the Fe
        // class, so ESC+first is consumed by Fe and the string content
        // between them stays in the text.
        //   osclike:      pending_ = [ESC, first, content...]
        //   osclike_esc:  pending_ = [ESC, first, content..., ESC]
        const bool has_trailing_esc = (state_ == state::osclike_esc);
        const size_t content_len = pending_.size() - 2u - (has_trailing_esc ? 1u : 0u);
        out.append(pending_.data() + 2, content_len);
        if (has_trailing_esc) {
            // The trailing ESC starts a NEW escape candidate.
            pending_.assign(1, '\x1B');
            state_ = state::after_esc;
        } else {
            pending_.clear();
            state_ = state::normal;
        }
        return;
    }
    // CSI / lone-ESC failure: emit everything held.
    out.append(pending_);
    pending_.clear();
    state_ = state::normal;
}

void AnsiStripper::flush(kimix::string& out) {
    // End-of-stream: resolve any partial escape. fail_escape can transition
    // osclike_esc -> after_esc (trailing ESC becomes a lone ESC), so loop
    // until the machine returns to the normal state.
    while (state_ != state::normal) {
        switch (state_) {
        case state::normal:
            break;
        case state::after_esc:
            // Lone ESC at EOF: no branch matches -> emitted as-is.
            out.append(pending_);
            pending_.clear();
            state_ = state::normal;
            break;
        case state::osclike:
        case state::osclike_esc:
            // Unterminated OSC/DCS at EOF -> Fe fallback.
            fail_escape(out);
            break;
        case state::csi_params:
        case state::csi_inter:
            // Unterminated CSI at EOF: no final byte -> kept in the text.
            out.append(pending_);
            pending_.clear();
            state_ = state::normal;
            break;
        }
    }
}

void AnsiStripper::feed(kimix::string_view chunk, kimix::string& out) {
    const char* data = chunk.data();
    const size_t size = chunk.size();
    for (size_t i = 0; i < size;) {
        const uint8_t b = static_cast<uint8_t>(data[i]);
        switch (state_) {
        case state::normal: {
            if (b != 0x1Bu) {
                // Bulk-copy the run of plain (non-ESC) bytes in one append
                // instead of one push_back per byte.
                size_t run_end = i;
                while (run_end < size &&
                       static_cast<uint8_t>(data[run_end]) != 0x1Bu) {
                    ++run_end;
                }
                out.append(data + i, run_end - i);
                i = run_end;
                if (i >= size) {
                    break;
                }
                // data[i] is ESC: fall through to the escape handling.
            }
            pending_.assign(1, '\x1B');
            state_ = state::after_esc;
            ++i;
            break;
        }
        case state::after_esc: {
            if (b == 0x5Du) { // `]` -> OSC
                pending_.push_back(static_cast<char>(b));
                first_ = b;
                state_ = state::osclike;
            } else if (is_dcs_like(b)) { // `P`/`^`/`_` -> DCS/PM/APC
                pending_.push_back(static_cast<char>(b));
                first_ = b;
                state_ = state::osclike;
            } else if (b == 0x5Bu) { // `[` -> CSI
                pending_.push_back(static_cast<char>(b));
                state_ = state::csi_params;
            } else if (is_fe_byte(b)) {
                // 2-byte Fe sequence -> removed entirely.
                pending_.clear();
                state_ = state::normal;
            } else {
                // No branch matches: emit ESC, reprocess the current byte.
                out.append(pending_);
                pending_.clear();
                state_ = state::normal;
                continue; // do not consume `b`
            }
            ++i;
            break;
        }
        case state::osclike: {
            if (b == 0x07u) { // BEL terminates OSC/DCS/PM/APC
                pending_.clear();
                state_ = state::normal;
                ++i;
            } else if (b == 0x1Bu) { // ESC -> maybe the ESC\ terminator
                pending_.push_back(static_cast<char>(b));
                state_ = state::osclike_esc;
                ++i;
            } else {
                // Bulk-copy the run of content bytes (neither BEL nor ESC) in
                // one append instead of one push_back per byte — long OSC
                // payloads (hyperlinks, titles, image/data sequences) spend
                // most of their bytes here.
                size_t run_end = i;
                while (run_end < size &&
                       static_cast<uint8_t>(data[run_end]) != 0x07u &&
                       static_cast<uint8_t>(data[run_end]) != 0x1Bu) {
                    ++run_end;
                }
                pending_.append(data + i, run_end - i);
                i = run_end;
            }
            break;
        }
        case state::osclike_esc: {
            if (b == 0x5Cu) { // `\` completes the ESC\ terminator
                pending_.clear();
                state_ = state::normal;
                ++i;
            } else {
                // Terminator absent: OSC/DCS fails; Fe fallback emits the
                // string content, the trailing ESC starts a new escape, and
                // the current byte is reprocessed.
                fail_escape(out);
                continue; // reprocess `b` from the new state
            }
            break;
        }
        case state::csi_params: {
            if (b >= 0x30u && b <= 0x3Fu) { // params
                pending_.push_back(static_cast<char>(b));
                ++i;
            } else if (b >= 0x20u && b <= 0x2Fu) { // intermediates
                pending_.push_back(static_cast<char>(b));
                state_ = state::csi_inter;
                ++i;
            } else if (b >= 0x40u && b <= 0x7Eu) { // final byte -> complete
                pending_.clear();
                state_ = state::normal;
                ++i;
            } else { // illegal byte -> CSI fails, text kept, reprocess `b`
                out.append(pending_);
                pending_.clear();
                state_ = state::normal;
                continue;
            }
            break;
        }
        case state::csi_inter: {
            if (b >= 0x20u && b <= 0x2Fu) { // more intermediates
                pending_.push_back(static_cast<char>(b));
                ++i;
            } else if (b >= 0x40u && b <= 0x7Eu) { // final byte -> complete
                pending_.clear();
                state_ = state::normal;
                ++i;
            } else { // illegal byte after intermediates -> CSI fails
                out.append(pending_);
                pending_.clear();
                state_ = state::normal;
                continue;
            }
            break;
        }
        }
    }
}

kimix::string strip_ansi(kimix::string_view utf8) {
    kimix::string out;
    out.reserve(utf8.size()); // stripped output never exceeds input
    AnsiStripper stripper;
    stripper.feed(utf8, out);
    stripper.flush(out);
    return out;
}

} // namespace stream
} // namespace runtime
} // namespace kimix
