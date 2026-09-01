// Test for src/runtime/text/sanitize.h (UTF-8 text sanitizer).
// This test covers:
// - sanitize_pre_nfc: surrogates, noncharacters (FDD0-FDEF, FFFE/FFFF),
//   PUA (BMP + planes 15/16), U+FFFD, zero-width set, C0/C1 controls
// - sanitize_post_nfc: strip (Python whitespace set), dedupe repeats
//   (boundaries max_repeat 1/2/100, disabled 0), truncation with/without
//   truncate_msg (shorter/equal/longer than max_chars), code-point counting
// - clean_text: keep_newlines vs not; strip set incl. \u3000 \u00a0 \u2028
// - golden vectors: "A"*10000 collapse, NFC-composing pairs e\u0301

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/text/sanitize.h>
#include <runtime/common/utf8.h>


using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::text;

static kimix::string utf8_of(const std::initializer_list<uint32_t>& cps) {
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

static kimix::string repeat(const kimix::string& s, size_t n) {
    kimix::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) {
        out += s;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Benchmark workload generators (production-shaped sanitizer inputs).
// All generators produce text the sanitizer pipeline passes through unchanged
// (identity asserts) unless stated otherwise, so the timed path is the real
// one without any dedupe collapse or strip side effects muddying the picture.
// ---------------------------------------------------------------------------

// Varied CJK: cycles 128 distinct ideographs (U+4E00..U+4E7F) so no two
// adjacent code points are ever identical (dedupe must pass through).
static kimix::string varied_cjk(size_t byte_goal) {
    kimix::string out;
    out.reserve(byte_goal + 3);
    size_t i = 0;
    while (out.size() < byte_goal) {
        out += utf8_of({0x4E00u + static_cast<uint32_t>(i % 128u)});
        ++i;
    }
    return out;
}

// ASCII + CJK + emoji + Latin-1 mix, no adjacent repeats, non-space edges.
static kimix::string mixed_text(size_t byte_goal) {
    const kimix::string seg = "chunk " + utf8_of({0x4E00, 0x1F600, 0xE9}) + " ;";
    kimix::string out;
    out.reserve(byte_goal + seg.size());
    while (out.size() < byte_goal) {
        out += seg;
    }
    return out;
}

// Cycling ASCII letters: no runs, non-space edges.
static kimix::string varied_ascii(size_t n) {
    kimix::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>('a' + (i % 26)));
    }
    return out;
}

// Many short lines joined by '\n' (no trailing newline, non-space edges).
static kimix::string many_short_lines(size_t byte_goal) {
    const kimix::string line = "The quick brown fox jumps over the lazy dog.";
    kimix::string out;
    out.reserve(byte_goal + line.size() + 2);
    bool first = true;
    while (out.size() + line.size() + 1 < byte_goal) {
        if (!first) {
            out.push_back('\n');
        }
        first = false;
        out += line;
    }
    return out;
}

// One long line of distinct text, non-space tail so strip() keeps it.
static kimix::string long_line(size_t byte_goal) {
    const kimix::string lorem = "The quick brown fox jumps over the lazy dog. ";
    kimix::string out;
    out.reserve(byte_goal + lorem.size() + 1);
    while (out.size() + lorem.size() < byte_goal) {
        out += lorem;
    }
    out.push_back('z');
    return out;
}

// Lone surrogates cannot be encoded as valid UTF-8; feed them as the
// surrogatepass bytes Python produces (CESU-8-like: ED A0 80 = U+D800).
static kimix::string surrogate_bytes(uint32_t cp) {
    return kimix::string({static_cast<char>(0xED),
                        static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                        static_cast<char>(0x80 | (cp & 0x3F))});
}

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "strip_surrogates"_test = [] {
        // "a\uD800b" -> "ab" (kernel sees CESU-8-like surrogate bytes)
        const kimix::string s = "a" + surrogate_bytes(0xD800) + "b" + surrogate_bytes(0xDFFF);
        expect(eq(sanitize_pre_nfc(kimix::string_view(s)), kimix::string("ab")));
    };

    "strip_noncharacters"_test = [] {
        // U+FDD0, U+FDEF removed; U+FFFE/U+FFFF (any plane) removed.
        const kimix::string s = utf8_of({'a', 0xFDD0, 'b', 0xFDEF, 'c', 0xFFFE, 'd', 0xFFFF,
                                       0x1FFFE, 0x1FFFF, 0x10FFFE, 0x10FFFF, 'e'});
        expect(eq(sanitize_pre_nfc(kimix::string_view(s)), kimix::string("abcde")));
        // U+FDCF and U+FE00 are NOT noncharacters -> kept.
        const kimix::string t = utf8_of({0xFDCF, 0xFE00});
        expect(eq(sanitize_pre_nfc(kimix::string_view(t)), t));
    };

    "strip_pua"_test = [] {
        // BMP PUA, plane 15 PUA, plane 16 PUA removed; U+F8FF/U+FFFFD/U+10FFFD
        // removed; U+F900 (compat ideograph, NOT PUA) kept.
        const kimix::string s = utf8_of({'x', 0xE000, 0xF8FF, 0xF0000, 0xFFFFD,
                                       0x100000, 0x10FFFD, 'y', 0xF900});
        const kimix::string expected = utf8_of({'x', 'y', 0xF900});
        expect(eq(sanitize_pre_nfc(kimix::string_view(s)), expected));
    };

    "strip_replacement_chars"_test = [] {
        // All U+FFFD removed (not collapsed — reference uses .replace).
        const kimix::string s = utf8_of({'a', 0xFFFD, 0xFFFD, 'b', 0xFFFD});
        expect(eq(sanitize_pre_nfc(kimix::string_view(s)), kimix::string("ab")));
    };

    "strip_zero_width_and_controls"_test = [] {
        // Each zero-width char from the clean_text regex set.
        const kimix::string zw = utf8_of({0x200B, 0x200C, 0x200D, 0x2060, 0x00AD,
                                        0xFEFF, 0x200E, 0x200F, 0x202A, 0x202E,
                                        0x2066, 0x2069});
        expect(eq(sanitize_pre_nfc(kimix::string_view("a" + zw + "b")),
                  kimix::string("ab")));
        // C0/C1 controls removed; \n \r \t kept by default.
        const kimix::string ctl("\x00\x01\x08\x0B\x0C\x0E\x1F\x7F\x80\x9F", 10);
        expect(eq(sanitize_pre_nfc(kimix::string_view("a" + ctl + "\nb\rc\td")),
                  kimix::string("a\nb\rc\td")));
        // clean_text with keep_newlines=False removes \n \r \t too.
        expect(eq(clean_text(kimix::string_view("a\nb\rc\td"), false),
                  kimix::string("abcd")));
    };

    "clean_text_strip_set"_test = [] {
        // Python str.strip() whitespace set: \u3000 \u00a0 \u2028 \u2029 \u2007
        // \u205f \u1680 \u0085 and ASCII set (space, \t \n \v \f \r, \x1c-\x1f).
        const kimix::string ws = utf8_of({0x3000, 0xA0, 0x2028, 0x2029, 0x2007,
                                        0x205F, 0x1680, 0x85});
        expect(eq(clean_text(kimix::string_view(ws + "hi" + ws), true),
                  kimix::string("hi")));
        // ASCII whitespace incl. 0x1C-0x1F.
        expect(eq(clean_text(kimix::string_view(" \t\n\v\f\r\x1c\x1d\x1e\x1f" "x" " \n"), true),
                  kimix::string("x")));
        // All-whitespace -> empty.
        expect(eq(clean_text(kimix::string_view(ws + " \t\n"), true), kimix::string()));
        // Non-space controls at the edges are NOT stripped by strip() (they
        // are removed by the control pass first, so "x" remains).
        expect(eq(clean_text(kimix::string_view("\x01x\x7f"), true), kimix::string("x")));
    };

    "dedupe_repeats"_test = [] {
        sanitize_options opts;
        opts.max_repeat = 100;
        // "A"*10000 collapses to "A"*100.
        {
            const kimix::string s(10000, 'A');
            const kimix::string out = sanitize_post_nfc(kimix::string_view(s), opts);
            expect(eq(out, kimix::string(100, 'A')));
        }
        // Exactly 100 A's stays 100.
        {
            const kimix::string s(100, 'A');
            expect(eq(sanitize_post_nfc(kimix::string_view(s), opts), s));
        }
        // 101 -> 100; max_repeat=1 -> any run of 2+ collapses to 1.
        {
            const kimix::string s(101, 'A');
            expect(eq(sanitize_post_nfc(kimix::string_view(s), opts), kimix::string(100, 'A')));
            sanitize_options o1;
            o1.max_repeat = 1;
            expect(eq(sanitize_post_nfc(kimix::string_view("aaabbbc"), o1),
                      kimix::string("abc")));
            o1.max_repeat = 2;
            expect(eq(sanitize_post_nfc(kimix::string_view("aaabbbc"), o1),
                      kimix::string("aabbc")));
        }
        // max_repeat=0 disables dedupe.
        {
            sanitize_options o0;
            o0.max_repeat = 0;
            const kimix::string s(500, 'B');
            expect(eq(sanitize_post_nfc(kimix::string_view(s), o0), s));
        }
        // CJK run collapse (multi-byte chars).
        {
            const kimix::string s = repeat(utf8_of({0x4F60}), 250);
            const kimix::string out = sanitize_post_nfc(kimix::string_view(s), opts);
            expect(eq(out, repeat(utf8_of({0x4F60}), 100)));
        }
        // Runs separated by other chars: "A"*150 + "B" + "A"*150.
        {
            const kimix::string s = kimix::string(150, 'A') + "B" + kimix::string(150, 'A');
            const kimix::string out = sanitize_post_nfc(kimix::string_view(s), opts);
            expect(eq(out, kimix::string(100, 'A') + "B" + kimix::string(100, 'A')));
        }
    };

    "truncate_cases"_test = [] {
        // No truncation when length <= max_chars.
        {
            sanitize_options opts;
            opts.max_chars = 5;
            expect(eq(sanitize_post_nfc(kimix::string_view("hello"), opts),
                      kimix::string("hello")));
        }
        // Truncation without msg.
        {
            sanitize_options opts;
            opts.max_chars = 3;
            expect(eq(sanitize_post_nfc(kimix::string_view("hello"), opts),
                      kimix::string("hel")));
        }
        // Truncation with msg shorter than max_chars: reserve room.
        {
            sanitize_options opts;
            opts.max_chars = 5;
            opts.truncate_msg = kimix::string_view("...");
            expect(eq(sanitize_post_nfc(kimix::string_view("hello world"), opts),
                      kimix::string("he...")));
        }
        // msg length EQUAL to max_chars -> msg dropped, plain truncation.
        {
            sanitize_options opts;
            opts.max_chars = 3;
            opts.truncate_msg = kimix::string_view("abc");
            expect(eq(sanitize_post_nfc(kimix::string_view("hello"), opts),
                      kimix::string("hel")));
        }
        // msg longer than max_chars -> dropped.
        {
            sanitize_options opts;
            opts.max_chars = 2;
            opts.truncate_msg = kimix::string_view("long");
            expect(eq(sanitize_post_nfc(kimix::string_view("hello"), opts),
                      kimix::string("he")));
        }
        // Truncation counts CODE POINTS: 4-byte CJK.
        {
            sanitize_options opts;
            opts.max_chars = 3;
            const kimix::string s = utf8_of({0x20000, 0x20001, 0x20002, 0x20003});
            const kimix::string out = sanitize_post_nfc(kimix::string_view(s), opts);
            expect(eq(out, utf8_of({0x20000, 0x20001, 0x20002})));
            expect(eq(kimix::runtime::common::utf8_code_point_count(kimix::string_view(out)),
                      size_t(3)));
        }
        // truncated flag.
        {
            sanitize_options opts;
            opts.max_chars = 2;
            bool truncated = false;
            sanitize_post_nfc(kimix::string_view("hello"), opts, &truncated);
            expect(truncated);
            sanitize_post_nfc(kimix::string_view("hi"), opts, &truncated);
            expect(!truncated);
        }
    };

    "sanitize_pipeline_order"_test = [] {
        // PUA/FFFD chars removed before dedupe: a run of PUA chars is gone,
        // so dedupe sees only what survived.
        {
            sanitize_options opts;
            const kimix::string s = utf8_of({0xE000, 0xE000, 0xE000}) + "abc";
            expect(eq(sanitize_for_tokenizer(kimix::string_view(s), opts),
                      kimix::string("abc")));
        }
        // Zero-width + controls stripped; NFC-composing pair is NOT composed
        // by the C++ kernel (that is the shim hook) — "e\u0301" stays as-is.
        {
            sanitize_options opts;
            const kimix::string s = utf8_of({0x200B, 'e', 0x0301, 0x0001});
            expect(eq(sanitize_for_tokenizer(kimix::string_view(s), opts),
                      utf8_of({'e', 0x0301})));
        }
        // clean_text identical behavior on the same input.
        {
            expect(eq(clean_text(kimix::string_view(utf8_of({0x200B, 'e', 0x0301, 0x0001})), true),
                      utf8_of({'e', 0x0301})));
        }
        // strip happens before truncation (leading/trailing spaces removed).
        {
            sanitize_options opts;
            opts.max_chars = 3;
            expect(eq(sanitize_for_tokenizer(kimix::string_view("  hi there  "), opts),
                      kimix::string("hi ")));
        }
        // Truncate_msg is appended verbatim (no final strip, matching the
        // reference implementation which has no strip after truncation).
        {
            sanitize_options opts;
            opts.max_chars = 6;
            opts.truncate_msg = kimix::string_view("...");
            expect(eq(sanitize_for_tokenizer(kimix::string_view("hello world"), opts),
                      kimix::string("hel...")));
        }
    };

    "strip_controls_api"_test = [] {
        // Note: "\x01" "b" split literals — "\x01b" would be one hex escape.
        expect(eq(strip_controls(kimix::string_view("a\x01" "b\x7f" "c\n"), true),
                  kimix::string("abc\n")));
        expect(eq(strip_controls(kimix::string_view("a\x01" "b\x7f" "c\n"), false),
                  kimix::string("abc")));
        // Multi-byte chars pass through unchanged.
        expect(eq(strip_controls(kimix::string_view(utf8_of({0x4F60, 0x1F600})), true),
                  utf8_of({0x4F60, 0x1F600})));
    };

    // ---------------------------------------------------------------------------
    // Benchmarks — sanitizer pipeline (kimix_bench contract, bench_util.h).
    // Production shape: sanitize_for_tokenizer runs on every tool output and
    // every Text/Think part; clean_text on assistant output. Workloads cover
    // pre/post/full pipeline, ASCII / CJK / mixed, with and without max_len,
    // worst-case dedupe runs, and long-line vs many-short-lines shapes.
    // Every case asserts a known-good result and sinks it.
    // ---------------------------------------------------------------------------

    "bench_pre_nfc_ascii_1mb"_test = [] {
        const kimix::string data = varied_ascii(1 << 20);
        kimix::string out;
        kimix_bench::run("sanitize/pre_nfc_ascii_1mb",
                         [&] { out = sanitize_pre_nfc(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_pre_nfc_cjk_500kb"_test = [] {
        const kimix::string data = varied_cjk(500000);
        kimix::string out;
        kimix_bench::run("sanitize/pre_nfc_cjk_500kb",
                         [&] { out = sanitize_pre_nfc(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_pre_nfc_mixed_256kb"_test = [] {
        const kimix::string data = mixed_text(256000);
        kimix::string out;
        kimix_bench::run("sanitize/pre_nfc_mixed_256kb",
                         [&] { out = sanitize_pre_nfc(kimix::string_view(data)); },
                         1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_for_tokenizer_ascii_1mb"_test = [] {
        const kimix::string data = varied_ascii(1 << 20);
        sanitize_options opts;
        kimix::string out;
        kimix_bench::run("sanitize/for_tokenizer_ascii_1mb", [&] {
            out = sanitize_for_tokenizer(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_for_tokenizer_cjk_500kb"_test = [] {
        const kimix::string data = varied_cjk(500000);
        sanitize_options opts;
        kimix::string out;
        kimix_bench::run("sanitize/for_tokenizer_cjk_500kb", [&] {
            out = sanitize_for_tokenizer(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_for_tokenizer_mixed_256kb"_test = [] {
        const kimix::string data = mixed_text(256000);
        sanitize_options opts;
        kimix::string out;
        kimix_bench::run("sanitize/for_tokenizer_mixed_256kb", [&] {
            out = sanitize_for_tokenizer(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_post_nfc_dedupe_worst_1mb"_test = [] {
        // Worst case for dedupe: one 1 MB run collapses to max_repeat copies.
        const kimix::string data(1 << 20, 'A');
        sanitize_options opts;
        opts.max_repeat = 100;
        kimix::string out;
        kimix_bench::run("sanitize/post_nfc_dedupe_1mb_run", [&] {
            out = sanitize_post_nfc(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, kimix::string(100, 'A')));
        kimix_bench::sink(out.size());
    };

    "bench_post_nfc_truncate_ascii_500kb"_test = [] {
        // max_len path: truncate to 2000 cps, reserve room for the 3-cp msg.
        const kimix::string data = varied_ascii(500000);
        sanitize_options opts;
        opts.max_chars = 2000;
        opts.truncate_msg = kimix::string_view("...");
        kimix::string out;
        bool truncated = false;
        kimix_bench::run("sanitize/post_nfc_truncate_ascii_500kb", [&] {
            truncated = false;
            out = sanitize_post_nfc(kimix::string_view(data), opts, &truncated);
        }, 1, static_cast<double>(data.size()));
        expect(truncated);
        expect(eq(out, kimix::string(data.data(), 1997) + "..."));
        kimix_bench::sink(out.size());
    };

    "bench_post_nfc_no_truncate_ascii_500kb"_test = [] {
        // max_chars = 0 (disabled): dedupe passthrough on varied ASCII.
        const kimix::string data = varied_ascii(500000);
        sanitize_options opts;
        kimix::string out;
        bool truncated = true;
        kimix_bench::run("sanitize/post_nfc_ascii_500kb_nomax", [&] {
            out = sanitize_post_nfc(kimix::string_view(data), opts, &truncated);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        expect(!truncated);
        kimix_bench::sink(out.size());
    };

    "bench_for_tokenizer_many_short_lines"_test = [] {
        const kimix::string data = many_short_lines(1 << 20);
        sanitize_options opts;
        kimix::string out;
        kimix_bench::run("sanitize/for_tokenizer_many_short_lines", [&] {
            out = sanitize_for_tokenizer(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_for_tokenizer_single_long_line"_test = [] {
        const kimix::string data = long_line(1 << 20);
        sanitize_options opts;
        kimix::string out;
        kimix_bench::run("sanitize/for_tokenizer_single_long_line", [&] {
            out = sanitize_for_tokenizer(kimix::string_view(data), opts);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_clean_text_ascii_1mb"_test = [] {
        const kimix::string data = varied_ascii(1 << 20);
        kimix::string out;
        kimix_bench::run("sanitize/clean_text_ascii_1mb", [&] {
            out = clean_text(kimix::string_view(data), true);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };

    "bench_clean_text_sparse_controls"_test = [] {
        // Every 97th byte is a C0 control: exercises the strip/filter path.
        kimix::string data;
        data.reserve(1 << 20);
        kimix::string expected;
        expected.reserve(1 << 20);
        for (size_t i = 0; i < (1 << 20); ++i) {
            const char c = (i % 97 == 0) ? '\x01' : static_cast<char>('a' + (i % 26));
            data.push_back(c);
            if (c != '\x01') {
                expected.push_back(c);
            }
        }
        kimix::string out;
        kimix_bench::run("sanitize/clean_text_sparse_controls_1mb", [&] {
            out = clean_text(kimix::string_view(data), true);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, expected));
        kimix_bench::sink(out.size());
    };

    "bench_strip_controls_ascii_1mb"_test = [] {
        const kimix::string data = varied_ascii(1 << 20);
        kimix::string out;
        kimix_bench::run("sanitize/strip_controls_ascii_1mb", [&] {
            out = strip_controls(kimix::string_view(data), true);
        }, 1, static_cast<double>(data.size()));
        expect(eq(out, data));
        kimix_bench::sink(out.size());
    };
}
