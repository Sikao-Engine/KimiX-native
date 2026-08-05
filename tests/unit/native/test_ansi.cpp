// Test for src/runtime/stream/ansi.h (streaming ANSI escape stripper).
// This test covers:
// - every branch of the regex: OSC (BEL/ESC\ terminated), DCS/PM/APC,
//   Fe sequences (0x40-0x5A and 0x5C-0x5F), CSI (params/intermediates/final)
// - regex backtracking fallbacks (unterminated OSC/DCS -> Fe, `\x1b[1 2m` kept)
// - partial-escape-at-eof and chunk-boundary behavior: feeding at EVERY byte
//   offset of a 200-byte sample equals one-shot processing
// - filter_output: ANSI strip + CRLF/lone-CR normalization

#include "ut/ut.hpp"
#include <runtime/stream/ansi.h>
#include <runtime/stream/line_processor.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::stream;

namespace {
std::string esc() { return std::string(1, '\x1B'); }
}

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "one_shot_branches"_test = [] {
        // CSI SGR
        expect(eq(strip_ansi(kimix::string_view("\x1b[31mred\x1b[0m")), kimix::string("red")));
        // OSC terminated by BEL
        expect(eq(strip_ansi(kimix::string_view("\x1b]0;title\x07hello")), kimix::string("hello")));
        // OSC terminated by ESC\
        expect(eq(strip_ansi(kimix::string_view("\x1b]0;title\x1b\\hello")), kimix::string("hello")));
        // DCS terminated by BEL
        expect(eq(strip_ansi(kimix::string_view("\x1bPabc\x07x")), kimix::string("x")));
        // DCS fails at ESC+[, Fe fallback: `\x1bP` removed, abc kept
        expect(eq(strip_ansi(kimix::string_view("\x1bPabc\x1b[31mx")), kimix::string("abcx")));
        // DCS branch wins over Fe when terminated
        expect(eq(strip_ansi(kimix::string_view("\x1b_ab\x07")), kimix::string()));
        // Unterminated DCS falls back to Fe (2 bytes) at EOF
        expect(eq(strip_ansi(kimix::string_view("\x1b_")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1bP")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1b^")), kimix::string()));
        // Unterminated OSC at EOF: Fe fallback removes `\x1b]`, content kept
        expect(eq(strip_ansi(kimix::string_view("\x1b]abc")), kimix::string("abc")));
        // OSC fails at ESC+[, CSI after removed -> '\x1b]a'
        expect(eq(strip_ansi(kimix::string_view("\x1b]a\x1b[0m")), kimix::string("a")));
        // OSC fails; Fe at 2nd ESC; trailing ESC kept -> "a\x1b"
        expect(eq(strip_ansi(kimix::string_view("\x1b]a\x1b\x1b\\")), kimix::string("a\x1b")));
        // CSI ordering constraint: `\x1b[1 2m` (param space param) NOT a match
        expect(eq(strip_ansi(kimix::string_view("\x1b[1 2m")), kimix::string("\x1b[1 2m")));
        // Plain SGR, cursor hide, clear
        expect(eq(strip_ansi(kimix::string_view("\x1b[31m")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1b[?25l")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1b[2J\x1b[H")), kimix::string()));
        // Fe sequences: '(' 0x28 and '-' 0x2D are NOT in the class -> kept
        expect(eq(strip_ansi(kimix::string_view("\x1b(0")), kimix::string("\x1b(0")));
        expect(eq(strip_ansi(kimix::string_view("\x1b-")), kimix::string("\x1b-")));
        expect(eq(strip_ansi(kimix::string_view("\x1b" "8")), kimix::string("\x1b" "8")));
        expect(eq(strip_ansi(kimix::string_view("\x1b=")), kimix::string("\x1b=")));
        // Fe in class: M, \, ^(via DCS fallback), _ (via DCS fallback)
        expect(eq(strip_ansi(kimix::string_view("\x1bM")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1b\\")), kimix::string()));
        // Unterminated CSI / lone ESC at EOF -> kept
        expect(eq(strip_ansi(kimix::string_view("\x1b[31")), kimix::string("\x1b[31")));
        expect(eq(strip_ansi(kimix::string_view("\x1b")), kimix::string("\x1b")));
        // Plain text untouched
        expect(eq(strip_ansi(kimix::string_view("plain text")), kimix::string("plain text")));
        // Mixed: OSC + DCS + CSI
        expect(eq(strip_ansi(kimix::string_view("a\x1b]0;title\x07" "b\x1bPq\x1b\\c")),
                  kimix::string("abc")));
        // OSC hyperlinks
        expect(eq(strip_ansi(kimix::string_view("\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\")),
                  kimix::string("link")));
        // Empty OSC / DCS
        expect(eq(strip_ansi(kimix::string_view("\x1b]0;\x07")), kimix::string()));
        expect(eq(strip_ansi(kimix::string_view("\x1bP\x1b\\")), kimix::string()));
        // Partial CSI at EOF followed by text
        expect(eq(strip_ansi(kimix::string_view("\x1b[3")), kimix::string("\x1b[3")));
        // SGR with ';' params and erase-line
        expect(eq(strip_ansi(kimix::string_view("x\x1b[31;1m\x1b[Kbold\x1b[0m")),
                  kimix::string("xbold")));
    };

    "chunk_boundary_split_all_offsets"_test = [] {
        // 200-byte sample containing every escape family, split at EVERY byte
        // offset: feeding {prefix}{suffix} must equal one-shot processing.
        std::string sample;
        sample += "line1\x1b[31mred\x1b[0m\n";
        sample += "\x1b]0;title\x07osc-bel\n";
        sample += "\x1b]0;title\x1b\\osc-st\n";
        sample += "\x1bPdcs\x07" "dcs\n";
        sample += "\x1bPfall\x1b[32mdcs-fall\n";
        sample += "\x1b_apc\x07" "apc\n";
        sample += "\x1bMfe\n";
        sample += "\x1b[?25lhide\x1b[?25hshow\n";
        sample += "\x1b[1 2mkept\x1b[2J\n";
        sample += "partial \x1b[3 at eof";
        // pad to >= 200 bytes
        while (sample.size() < 200u) {
            sample += "padding text \x1b[0m ";
        }

        const kimix::string one_shot = strip_ansi(kimix::string_view(sample));
        expect(!one_shot.empty());

        for (size_t split = 0; split <= sample.size(); ++split) {
            kimix::string out;
            AnsiStripper s;
            s.feed(kimix::string_view(sample.data(), split), out);
            s.feed(kimix::string_view(sample.data() + split, sample.size() - split), out);
            s.flush(out);
            expect(eq(out, one_shot)) << "split at byte " << split;
        }
    };

    "chunk_boundary_every_byte"_test = [] {
        // Feed byte-by-byte: must equal one-shot.
        std::string sample = "abc\x1b[31mdef\x1b]0;t\x07ghi\x1bPq\x1b\\jkl\x1b";
        const kimix::string one_shot = strip_ansi(kimix::string_view(sample));
        kimix::string out;
        AnsiStripper s;
        for (size_t i = 0; i < sample.size(); ++i) {
            s.feed(kimix::string_view(sample.data() + i, 1), out);
        }
        s.flush(out);
        expect(eq(out, one_shot));
        // Also: flush with no feed does not crash and emits nothing.
        kimix::string out2;
        AnsiStripper s2;
        s2.flush(out2);
        expect(out2.empty());
    };

    "filter_output_crlf"_test = [] {
        // ANSI strip + CRLF/CR normalization (tools.common.py filter_output).
        expect(eq(filter_output(kimix::string_view("a\r\nb\rc\nd")),
                  kimix::string("a\nb\nc\nd")));
        expect(eq(filter_output(kimix::string_view("\x1b[31mred\x1b[0m\r\nnext")),
                  kimix::string("red\nnext")));
        // CR at end of buffer -> LF
        expect(eq(filter_output(kimix::string_view("tail\r")), kimix::string("tail\n")));
        // Multiple CRs
        expect(eq(filter_output(kimix::string_view("a\r\rb")), kimix::string("a\n\nb")));
        expect(eq(filter_output(kimix::string_view("a\r\n\r\nb")), kimix::string("a\n\nb")));
    };

    "reset_clears_state"_test = [] {
        AnsiStripper s;
        kimix::string out;
        s.feed(kimix::string_view("\x1b[31", 4), out); // partial CSI (ESC [ 3 1)
        s.reset();
        expect(out.empty());
        s.feed(kimix::string_view("x\x1b[0m", 5), out);
        expect(eq(out, kimix::string("x")));
    };
}
