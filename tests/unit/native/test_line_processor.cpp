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
#include "bench_util.h"
#include <runtime/stream/line_processor.h>
#include <runtime/common/utf8.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

kimix::string join_lines(const kimix::vector<kimix::string>& lines) {
    kimix::string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k > 0) {
            out.push_back('\n');
        }
        out += lines[k];
    }
    return out;
}

// Independent reference implementation of `_ANSI_ESCAPE_RE` (see ansi.h) — a
// direct character-class scanner without any streaming state machine. Used to
// prove benchmark outputs are the known-good ones before anything is timed.
kimix::string strip_ansi_ref(kimix::string_view s) {
    kimix::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t c0 = static_cast<uint8_t>(s[i]);
        if (c0 != 0x1Bu) {
            out.push_back(s[i]);
            ++i;
            continue;
        }
        bool matched = false;
        size_t k = n; // exclusive end of the matched escape
        if (i + 1 < n) {
            const uint8_t c = static_cast<uint8_t>(s[i + 1]);
            // OSC: ESC ] [^\x07\x1B]* (\x07 | \x1B\\)
            if (c == 0x5Du) {
                size_t j = i + 2;
                while (j < n && static_cast<uint8_t>(s[j]) != 0x07u &&
                       static_cast<uint8_t>(s[j]) != 0x1Bu) {
                    ++j;
                }
                if (j < n && static_cast<uint8_t>(s[j]) == 0x07u) {
                    matched = true;
                    k = j + 1;
                } else if (j + 1 < n &&
                           static_cast<uint8_t>(s[j]) == 0x1Bu &&
                           s[j + 1] == '\\') {
                    matched = true;
                    k = j + 2;
                }
            }
            // DCS/PM/APC: ESC [P^_] [^\x07\x1B]* (\x07 | \x1B\\)
            if (!matched && (c == 0x50u || c == 0x5Eu || c == 0x5Fu)) {
                size_t j = i + 2;
                while (j < n && static_cast<uint8_t>(s[j]) != 0x07u &&
                       static_cast<uint8_t>(s[j]) != 0x1Bu) {
                    ++j;
                }
                if (j < n && static_cast<uint8_t>(s[j]) == 0x07u) {
                    matched = true;
                    k = j + 1;
                } else if (j + 1 < n &&
                           static_cast<uint8_t>(s[j]) == 0x1Bu &&
                           s[j + 1] == '\\') {
                    matched = true;
                    k = j + 2;
                }
            }
            // Fe: ESC [@-Z\\-_] — two bytes (0x40-0x5A, 0x5C-0x5F).
            if (!matched && ((c >= 0x40u && c <= 0x5Au) ||
                             (c >= 0x5Cu && c <= 0x5Fu))) {
                matched = true;
                k = i + 2;
            }
            // CSI: ESC [ [0-?]* [ -/]* [@-~]
            if (!matched && c == 0x5Bu) {
                size_t j = i + 2;
                while (j < n && static_cast<uint8_t>(s[j]) >= 0x30u &&
                       static_cast<uint8_t>(s[j]) <= 0x3Fu) {
                    ++j;
                }
                while (j < n && static_cast<uint8_t>(s[j]) >= 0x20u &&
                       static_cast<uint8_t>(s[j]) <= 0x2Fu) {
                    ++j;
                }
                if (j < n && static_cast<uint8_t>(s[j]) >= 0x40u &&
                    static_cast<uint8_t>(s[j]) <= 0x7Eu) {
                    matched = true;
                    k = j + 1;
                }
            }
        }
        if (matched) {
            i = k;
        } else {
            out.push_back('\x1B');
            ++i;
        }
    }
    return out;
}

// CRLF / lone-CR -> LF, exactly `replace("\r\n", "\n").replace("\r", "\n")`.
std::string ref_crlf_normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char b = s[i];
        if (b == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(b);
        }
    }
    return out;
}

// Split on '\n' like filter_output's output (no trailing empty line after a
// trailing terminator; empty lines between terminators are preserved).
std::vector<std::string> ref_split_lines(std::string_view s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            const size_t len = i - start;
            if (i == s.size() && len == 0) {
                break;
            }
            lines.emplace_back(s.data() + start, len);
            start = i + 1;
        }
    }
    return lines;
}

// Independent mode-0 reference: strip (optional) + CRLF normalize + split +
// join with '\n'. Equal to `run_chunked(opts{strip_ansi, mode 0, no budgets,
// no fold}, ...)`.
kimix::string ref_line_join(std::string_view in, bool strip) {
    std::string norm;
    if (strip) {
        const kimix::string stripped = strip_ansi_ref(in);
        norm = ref_crlf_normalize(std::string_view(stripped.data(), stripped.size()));
    } else {
        norm = ref_crlf_normalize(in);
    }
    const std::vector<std::string> lines = ref_split_lines(norm);
    kimix::string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k > 0) {
            out.push_back('\n');
        }
        out += lines[k];
    }
    return out;
}

// Independent counter-mode dedup reference (tools/common.py `_dedup_output`
// counter path via std::map/std::set instead of the native unordered maps).
kimix::string ref_counter_dedup_join(std::string_view in, uint32_t threshold) {
    const std::vector<std::string> lines = ref_split_lines(ref_crlf_normalize(in));
    std::map<std::string, size_t> counts;
    for (const auto& line : lines) {
        ++counts[line];
    }
    std::vector<std::string> emitted;
    kimix::string out;
    for (const auto& line : lines) {
        const size_t cnt = counts[line];
        if (cnt > threshold) {
            if (std::find(emitted.begin(), emitted.end(), line) == emitted.end()) {
                emitted.push_back(line);
                if (!out.empty()) {
                    out.push_back('\n');
                }
                out += kimix::string(line);
                out += "  (";
                out += std::to_string(cnt);
                out += " repeats)";
            }
        } else {
            if (!out.empty()) {
                out.push_back('\n');
            }
            out += kimix::string(line);
        }
    }
    return out;
}

// Independent block-mode dedup reference: greedy largest-block-first with
// `consumed` marking (tools/common.py `_dedup_output` block path), written
// with std::vector<std::string> + std::vector<int>.
kimix::string ref_block_dedup_join(std::string_view in, uint32_t threshold,
                                   uint32_t block_window) {
    const std::vector<std::string> lines = ref_split_lines(ref_crlf_normalize(in));
    const size_t n = lines.size();
    std::vector<int> consumed(n, 0);
    std::vector<std::string> result;
    size_t i = 0;
    while (i < n) {
        if (consumed[i]) {
            ++i;
            continue;
        }
        bool collapsed = false;
        const size_t h_max = block_window < (n - i) ? block_window : (n - i);
        for (size_t h = h_max; h >= 1; --h) {
            size_t j = i;
            size_t repeats = 0;
            while (j + h <= n) {
                bool eq = true;
                for (size_t k = 0; k < h; ++k) {
                    if (lines[i + k] != lines[j + k]) {
                        eq = false;
                        break;
                    }
                }
                if (!eq) {
                    break;
                }
                bool any_consumed = false;
                for (size_t k = j; k < j + h; ++k) {
                    if (consumed[k]) {
                        any_consumed = true;
                        break;
                    }
                }
                if (any_consumed) {
                    break;
                }
                ++repeats;
                j += h;
            }
            if (repeats > threshold) {
                for (size_t k = 0; k + 1 < h; ++k) {
                    result.push_back(lines[i + k]);
                }
                std::string annotated = lines[i + h - 1] + "  (" +
                                        std::to_string(repeats) + " repeats)";
                result.push_back(std::move(annotated));
                for (size_t k = 0; k < h * repeats; ++k) {
                    consumed[i + k] = 1;
                }
                i = i + h * repeats;
                collapsed = true;
                break;
            }
        }
        if (!collapsed) {
            result.push_back(lines[i]);
            consumed[i] = 1;
            ++i;
        }
    }
    kimix::string out;
    for (size_t k = 0; k < result.size(); ++k) {
        if (k > 0) {
            out.push_back('\n');
        }
        out += result[k];
    }
    return out;
}

// ~target_bytes of terminal output dense with CSI/OSC sequences (progress
// bars, colors, cursor hide, window titles) interleaved with short text runs.
std::string make_dense_terminal(size_t target_bytes) {
    std::string s;
    s.reserve(target_bytes + 64);
    size_t n = 0;
    while (s.size() < target_bytes) {
        s += "\x1b]0;kimix build\x07";
        s += "\x1b[2K\r\x1b[36m[";
        const int filled = static_cast<int>(n % 25u);
        for (int i = 0; i < 24; ++i) {
            s.push_back(i < filled ? '#' : '-');
        }
        s += "]\x1b[0m ";
        s += std::to_string(n % 1000u);
        s += "/";
        s += std::to_string(1000u);
        s += " downloading artifact-000042 ";
        s += std::string(24, '.');
        s += "\x1b[32mOK\x1b[0m\x1b[?25l";
        ++n;
    }
    return s;
}

// ~target_bytes of heavy control-char garbage: partial escapes, CR/LF/NUL/BEL
// bytes, lone ESCs, aborted OSC/DCS/CSI fragments, plus ASCII text.
std::string make_control_garbage(size_t target_bytes) {
    static const char* const kTokens[] = {
        "\x1b[", "31m", "\x1b]0;", "t\x07", "\x1bP", "abc", "\x00", "\x07",
        "\r",   "\n",  "\x1b",     "  ",   "\x1b[?25l", "\x1b]8;;@", "\x1b\\", "x",
    };
    std::string s;
    s.reserve(target_bytes + 16);
    size_t n = 0;
    while (s.size() < target_bytes) {
        s += kTokens[n % (sizeof(kTokens) / sizeof(kTokens[0]))];
        ++n;
    }
    return s;
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

    // --- benchmarks (see bench_util.h contract) ---

    "bench_line_processor_plain_100k_lines"_test = [] {
        // 100k short lines: tiny-line churn (many small line allocations).
        const size_t kLines = 100000u;
        std::string input;
        input.reserve(kLines * 16u);
        for (size_t i = 0; i < kLines; ++i) {
            input += "task-";
            input += std::to_string(i);
            input += ": ok\n";
        }
        const kimix::string expected = ref_line_join(kimix::string_view(input), true);
        {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/plain_100k_lines", [&] {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_line_processor_huge_line_10mb"_test = [] {
        // One giant 10 MB minified line: line_buf_ growth + one big emit copy.
        const size_t kBytes = 10u * 1024u * 1024u;
        static const char kPiece[] =
            "var a=1,b=2;function f(x){return x+1}data[42].foo('bar');";
        const size_t kPieceLen = sizeof(kPiece) - 1;
        std::string input;
        input.reserve(kBytes);
        while (input.size() + kPieceLen <= kBytes) {
            input.append(kPiece, kPieceLen);
        }
        input.append(kPiece, kBytes - input.size());
        {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(lines.size(), size_t(1)));
            expect(eq(lines[0], kimix::string(kimix::string_view(input))));
        }
        size_t total_bytes = 0;
        kimix_bench::run("line_processor/huge_line_10mb", [&] {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_bytes += lines.empty() ? 0 : lines[0].size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_bytes);
    };

    "bench_line_processor_dense_ansi_1mb"_test = [] {
        // ~1 MB terminal output dense with CSI/OSC, split into lines.
        const std::string input = make_dense_terminal(size_t{1} << 20);
        const kimix::string expected = ref_line_join(kimix::string_view(input), true);
        {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/dense_ansi_1mb", [&] {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_line_processor_garbage_1mb"_test = [] {
        // Heavy control-char garbage: partial escapes, lone ESCs, CR/LF/NUL,
        // aborted OSC/DCS/CSI fragments.
        const std::string input = make_control_garbage(size_t{1} << 20);
        const kimix::string expected = ref_line_join(kimix::string_view(input), true);
        {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/garbage_1mb", [&] {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_line_processor_chunked_4096_1mb"_test = [] {
        // Streaming use: dense input fed in 4096-byte chunks (the documented
        // per-chunk processing unit).
        const size_t kChunk = 4096u;
        const std::string input = make_dense_terminal(size_t{1} << 20);
        const kimix::string expected = ref_line_join(kimix::string_view(input), true);
        {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            for (size_t off = 0; off < input.size(); off += kChunk) {
                const size_t n = input.size() - off < kChunk ? input.size() - off : kChunk;
                lp.feed(kimix::string_view(input.data() + off, n), &lines);
            }
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/stream_4096_dense_1mb", [&] {
            LineProcessor lp{process_options()};
            kimix::vector<kimix::string> lines;
            for (size_t off = 0; off < input.size(); off += kChunk) {
                const size_t n = input.size() - off < kChunk ? input.size() - off : kChunk;
                lp.feed(kimix::string_view(input.data() + off, n), &lines);
            }
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_line_processor_dedup_counter_20k"_test = [] {
        // 20k short lines, only 50 distinct -> counter-mode dedup churn.
        const size_t kLines = 20000u;
        std::string input;
        input.reserve(kLines * 12u);
        for (size_t i = 0; i < kLines; ++i) {
            input += "warn-";
            input += std::to_string(i % 50u);
            input += "\n";
        }
        process_options opts;
        opts.strip_ansi = false;
        opts.dedup_mode = 1;
        opts.threshold = 3;
        const kimix::string expected =
            ref_counter_dedup_join(kimix::string_view(input), opts.threshold);
        {
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/dedup_counter_20k", [&] {
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_line_processor_dedup_block_6k"_test = [] {
        // 1000 repeated 3-line blocks + unique tail: block-mode dedup.
        std::string input;
        input.reserve(1000u * 20u + 64u * 12u);
        for (int r = 0; r < 1000; ++r) {
            input += "alpha\nbeta\ngamma\n";
        }
        for (int t = 0; t < 60; ++t) {
            input += "unique-";
            input += std::to_string(t);
            input += "\n";
        }
        process_options opts;
        opts.strip_ansi = false;
        opts.dedup_mode = 2;
        opts.threshold = 2;
        opts.block_window = 3;
        const kimix::string expected =
            ref_block_dedup_join(kimix::string_view(input), opts.threshold,
                                 opts.block_window);
        {
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            expect(eq(join_lines(lines), expected));
        }
        size_t total_lines = 0;
        kimix_bench::run("line_processor/dedup_block_6k", [&] {
            LineProcessor lp(opts);
            kimix::vector<kimix::string> lines;
            lp.feed(kimix::string_view(input), &lines);
            lp.flush(&lines);
            total_lines += lines.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total_lines);
    };

    "bench_filter_output_1mb"_test = [] {
        // One-shot filter_output: ANSI strip + CRLF/CR normalization on
        // ~1 MB of terminal-style output with CRLF line endings.
        const size_t kBytes = size_t{1} << 20;
        std::string input;
        input.reserve(kBytes + kBytes / 40u);
        while (input.size() < kBytes) {
            input += "build step 12: \x1b[32mOK\x1b[0m\x1b]0;title\x07 now\r\n";
            input += "more log text here and more log text here\r\n";
        }
        if (input.size() > kBytes) {
            input.resize(kBytes);
        }
        const kimix::string stripped_ref =
            strip_ansi_ref(kimix::string_view(input));
        const std::string expected = ref_crlf_normalize(
            std::string_view(stripped_ref.data(), stripped_ref.size()));
        expect(eq(filter_output(kimix::string_view(input)),
                  kimix::string(expected)));
        size_t total = 0;
        kimix_bench::run("filter_output/1mb_crlf", [&] {
            kimix::string r = filter_output(kimix::string_view(input));
            total += r.size();
        }, 1, static_cast<double>(input.size()));
        kimix_bench::sink(total);
    };
}
