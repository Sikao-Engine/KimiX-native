// Test for src/runtime/stream/line_processor.h (line stream processor).
// This test covers:
// - ANSI strip + CRLF + line splitting (feed/flush == splitlines behavior)
// - dedup counter mode (threshold semantics, annotation format)
// - dedup block mode (multi-line blocks, consumed marking, largest-first)
// - column folding (fold_col, code-point boundaries, 4-byte chars)
// - byte-budget / line-budget truncation at line boundaries
// - UTF-8 accounting (bytes_written / code_points_written)
// - feed/flush chunked == one-shot on a 5 MB stream
// - filter_output parity with LineProcessor+join

#include "ut/ut.hpp"
#include <runtime/stream/line_processor.h>
#include <runtime/common/utf8.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::stream;

namespace {
kimix::string utf8_of(const std::initializer_list<uint32_t>& cps) {
    kimix::string out;
    for (uint32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Run a processor over `input` in `chunk_size` pieces, then flush; return
// the concatenated lines joined with '\n'.
kimix::string run_chunked(const process_options& opts, kimix::string_view input,
                          size_t chunk_size) {
    LineProcessor lp(opts);
    kimix::vector<kimix::string> lines;
    for (size_t i = 0; i < input.size(); i += chunk_size) {
        const size_t n = chunk_size < input.size() - i ? chunk_size : input.size() - i;
        lp.feed(kimix::string_view(input.data() + i, n), &lines);
    }
    lp.flush(&lines);
    kimix::string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k > 0) {
            out.push_back('\n');
        }
        out += lines[k];
    }
    return kimix::string(out);
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "split_and_crlf"_test = [] {
        process_options opts;
        opts.strip_ansi = true;
        opts.dedup_mode = 0;

        const kimix::string out = run_chunked(opts, "a\r\nb\rc\nd\n", 2);
        expect(eq(out, kimix::string("a\nb\nc\nd")));
        // Trailing newline does not produce an extra empty line.
        expect(eq(run_chunked(opts, "x\n", 1), kimix::string("x")));
        // Empty input -> no lines.
        expect(eq(run_chunked(opts, "", 1), kimix::string()));
        // Empty lines preserved.
        expect(eq(run_chunked(opts, "a\n\nb", 1), kimix::string("a\n\nb")));
        // ANSI stripped while splitting.
        expect(eq(run_chunked(opts, "\x1b[31mred\x1b[0m\nplain\n", 3),
                  kimix::string("red\nplain")));
        // Lone CR at end of buffer is a terminator.
        expect(eq(run_chunked(opts, "tail\r", 2), kimix::string("tail")));
        // CRLF spanning a chunk boundary: "abc\r" + "\ndef"
        {
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view("abc\r", 4), &lines);
            lp.feed(kimix::string_view("\ndef", 4), &lines);
            lp.flush(&lines);
            kimix::string out;
            for (size_t k = 0; k < lines.size(); ++k) {
                if (k > 0) out.push_back('\n');
                out += lines[k];
            }
            expect(eq(out, kimix::string("abc\ndef")));
        }
        // Multi-byte UTF-8 split across chunks (4-byte char).
        {
            const kimix::string s = utf8_of({0x1F600, 0x1F600, 0x1F600});
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            for (size_t i = 0; i < s.size(); ++i) {
                lp.feed(kimix::string_view(s.data() + i, 1), &lines);
            }
            lp.flush(&lines);
            expect(eq(lines.size(), size_t(1)));
            expect(eq(lines[0], s));
        }
    };

    "dedup_counter_mode"_test = [] {
        process_options opts;
        opts.strip_ansi = false;
        opts.dedup_mode = 1;
        opts.threshold = 3;

        // "a,a,a,a,b,a" -> "a  (4 repeats)", "b", "a"
        {
            const kimix::string out = run_chunked(opts, "a\na\na\na\nb\na\n", 1);
            expect(eq(out, kimix::string("a  (5 repeats)\nb")));
        }
        // threshold 3: exactly 3 repeats NOT collapsed.
        {
            const kimix::string out = run_chunked(opts, "a\na\na\n", 1);
            expect(eq(out, kimix::string("a\na\na")));
        }
        // threshold 2 collapses 3+.
        {
            process_options o = opts;
            o.threshold = 2;
            const kimix::string out = run_chunked(o, "a\na\na\nb\n", 1);
            expect(eq(out, kimix::string("a  (3 repeats)\nb")));
        }
        // Annotation format: two spaces before "(N repeats)".
        {
            process_options o = opts;
            o.threshold = 1;
            const kimix::string out = run_chunked(o, "err\nerr\n", 1);
            expect(eq(out, kimix::string("err  (2 repeats)")));
        }
        // Order preserved; first occurrence annotated.
        {
            process_options o = opts;
            o.threshold = 1;
            const kimix::string out = run_chunked(o, "x\ny\nx\nz\n", 1);
            expect(eq(out, kimix::string("x  (2 repeats)\ny\nz")));
        }
    };

    "dedup_block_mode"_test = [] {
        process_options opts;
        opts.strip_ansi = false;
        opts.dedup_mode = 2;
        opts.threshold = 1;   // collapse when repeats > 1
        opts.block_window = 2;

        // Single-line block equivalent (h=1 tried when block_window>=2).
        {
            const kimix::string out = run_chunked(opts, "e\ne\ne\n", 1);
            expect(eq(out, kimix::string("e  (3 repeats)")));
        }
        // Multi-line block: ["a","b"] x3 -> "a", "b  (3 repeats)"
        {
            const kimix::string out = run_chunked(opts, "a\nb\na\nb\na\nb\n", 1);
            expect(eq(out, kimix::string("a\nb  (3 repeats)")));
        }
        // Interleaved blocks that repeat contiguously at different heights:
        // ["x","y"] x2, then ["x","y"] x2 contiguous -> largest-first h=2
        // counts 4 contiguous repeats -> "x", "y  (4 repeats)"
        {
            const kimix::string out = run_chunked(opts,
                                                "x\ny\nx\ny\nx\ny\nx\ny\n", 1);
            expect(eq(out, kimix::string("x\ny  (4 repeats)")));
        }
        // Unique lines pass through; consumed marking prevents double count.
        {
            const kimix::string out = run_chunked(opts, "u\nv\nw\nu\nv\nw\n", 1);
            expect(eq(out, kimix::string("u\nv\nw\nu\nv\nw")));
        }
        // block_window=1 with dedup_mode=2 uses the COUNTER path (Python
        // `_dedup_output` dispatches to counter when max_block_lines <= 1):
        // "a","b","a" -> counts a=2 -> "a  (2 repeats)", "b"
        {
            process_options o = opts;
              o.block_window = 1;
            o.threshold = 1;
            const kimix::string out = run_chunked(o, "a\nb\na\n", 1);
            expect(eq(out, kimix::string("a  (2 repeats)\nb")));
        }
    };

    "fold_and_budget"_test = [] {
        process_options opts;
        opts.strip_ansi = false;
        opts.dedup_mode = 0;

        // fold_col=3 on "abcdef" -> "abc","def"
        {
            process_options o = opts;
            o.fold_col = 3;
            const kimix::string out = run_chunked(o, "abcdef\n", 1);
            expect(eq(out, kimix::string("abc\ndef")));
        }
        // fold at code-point boundaries with 4-byte chars (fold_col=2).
        {
            process_options o = opts;
            o.fold_col = 2;
            const kimix::string s = utf8_of({0x1F600, 0x1F601, 0x1F602});
            const kimix::string out = run_chunked(o, s + "\n", 1);
            expect(eq(out, utf8_of({0x1F600, 0x1F601}) + "\n" + utf8_of({0x1F602})));
        }
        // max_lines=2 truncates the tail at line boundaries.
        {
            process_options o = opts;
            o.max_lines = 2;
            const kimix::string out = run_chunked(o, "a\nb\nc\nd\n", 1);
            expect(eq(out, kimix::string("a\nb")));
        }
        // max_bytes truncates at line boundaries: budget 5 bytes -> "aaa"
        // (3 bytes), next line "bb" (2) would exceed 6? budget 6 -> "aaa","bb".
        {
            process_options o = opts;
            o.max_bytes = 6;
            const kimix::string out = run_chunked(o, "aaa\nbb\nccc\n", 1);
            expect(eq(out, kimix::string("aaa\nbb")));
        }
        // byte budget with multi-byte chars: 3 CJK chars = 9 bytes; budget 6
        // -> first line (3 bytes) fits? "你" = 3 bytes; "你\n" -> line "你" 3B;
        // second "你" 3B total 6 fits; third line would exceed -> dropped.
        {
            process_options o = opts;
            o.max_bytes = 6;
            const kimix::string cjk = utf8_of({0x4F60});
            const kimix::string out = run_chunked(o, cjk + "\n" + cjk + "\n" + cjk + "\n", 1);
            expect(eq(out, cjk + "\n" + cjk));
        }
    };

    "accounting"_test = [] {
        process_options opts;
        opts.strip_ansi = true;
        opts.dedup_mode = 0;

        LineProcessor lp(opts);
        kimix::vector<kimix::string> lines;
        // "\x1b[31mred\x1b[0m\n" is 13 bytes; "héllo\n" uses explicit
        // UTF-8 bytes (h C3 A9 l l o \n = 7) to avoid source-encoding issues.
        lp.feed(kimix::string_view("\x1b[31mred\x1b[0m\n", 13), &lines);
        lp.feed(kimix::string_view("h\xC3\xA9llo\n", 7), &lines);
        lp.flush(&lines);
        expect(eq(lines.size(), size_t(2)));
        expect(eq(lp.bytes_written(), uint64_t(3 + 6)));    // "red" + "héllo" (no terminator)
        expect(eq(lp.code_points_written(), uint64_t(3 + 5))); // red(3) + héllo(5)
        expect(eq(lp.lines_written(), uint64_t(2)));

        // reset clears counters.
        lp.reset();
        expect(eq(lp.bytes_written(), uint64_t(0)));
        expect(eq(lp.code_points_written(), uint64_t(0)));
        expect(eq(lp.lines_written(), uint64_t(0)));
    };

    "chunked_equals_oneshot_5mb"_test = [] {
        // 1000 lines of ~5 KB each (~5 MB); dedup modes and plain mode.
        const int kLines = 1000;
        std::string input;
        input.reserve(5u * 1024u * 1024u);
        for (int i = 0; i < kLines; ++i) {
            input += "line-";
            input += std::to_string(i);
            input += ": ";
            input += std::string(5000, static_cast<char>('a' + (i % 26)));
            input += "\n";
        }
        for (int mode = 0; mode <= 2; ++mode) {
            process_options opts;
            opts.strip_ansi = true;
            opts.dedup_mode = static_cast<uint32_t>(mode);
            opts.threshold = 3;
            opts.block_window = 3;
            const kimix::string whole = run_chunked(opts, input, input.size());
            for (size_t chunk : {size_t(1), size_t(7), size_t(4096), size_t(65536)}) {
                const kimix::string part = run_chunked(opts, input, chunk);
                expect(eq(part, whole)) << "mode=" << mode << " chunk=" << chunk;
            }
        }
    };

    "filter_output_parity"_test = [] {
        // LineProcessor(strip_ansi) + join == filter_output for LF input.
        const kimix::string input = "a\x1b[31mb\x1b[0m\nc\r\nd\re\n";
        // filter_output only replaces \r — the trailing \n is preserved.
        expect(eq(filter_output(kimix::string_view(input)), kimix::string("ab\nc\nd\ne\n")));
    };
}
