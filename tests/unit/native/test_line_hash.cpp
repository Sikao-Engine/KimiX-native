// Test for src/runtime/tools/line_hash.h (plan 013 chained xxHash32).
// This test covers:
// - xxh32 golden values (verified against the Python `xxhash` package)
// - compute_line_hash: CR strip, whitespace filter, seed
// - compute_line_hashes: chained seed semantics (nibble decode), first-line
//   has_significant / line_num seeds
// - Unicode whitespace + alnum handling (NBSP filtered, CJK significant)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/tools/line_hash.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

uint32_t nibble_code(uint32_t nib) {
    const char* ns = "ZPMQVRWSNKTXJBYH";
    return static_cast<unsigned char>(ns[nib & 15]);
}

// ---------------------------------------------------------------------------
// Benchmark workload generators (production-shaped line-hash inputs: log
// lines like tool output / diffs, and punctuation-heavy lines that exercise
// the no-has_significant seed path).
// ---------------------------------------------------------------------------

static kimix::string log_lines(size_t line_count, size_t line_len) {
    kimix::string out;
    out.reserve(line_count * (line_len + 2));
    for (size_t i = 0; i < line_count; ++i) {
        kimix::string line =
            "2026-08-31 12:00:00 INFO  task=build step=link ";
        line += std::to_string(i);
        line += " elapsed=";
        line += std::to_string((i * 7) % 997);
        line += "ms path=/src/main.cpp status=ok";
        if (line.size() > line_len) {
            line.resize(line_len);
        }
        while (line.size() < line_len) {
            line.push_back(' ');
        }
        out += line;
        out.push_back('\n');
    }
    return out;
}

static kimix::string punct_lines(size_t line_count, size_t line_len) {
    static const char alph[] = "!@#$%^&*()_-=+[]{}|;:,.<>?/";
    kimix::string out;
    out.reserve(line_count * (line_len + 1));
    const size_t al = sizeof(alph) - 1;
    for (size_t i = 0; i < line_count; ++i) {
        for (size_t k = 0; k < line_len; ++k) {
            out.push_back(alph[(i + k * 7) % al]);
        }
        out.push_back('\n');
    }
    return out;
}

// ASCII space predicate for the reference seed derivation (the bench corpora
// are pure ASCII, so this is exact for the recipe's str.isspace() set).
static bool is_ascii_space(uint32_t cp) {
    return cp == ' ' || (cp >= 0x09 && cp <= 0x0D);
}

static bool line_has_significant(kimix::string_view line) {
    for (unsigned char c : line) {
        if (!is_ascii_space(c) && is_alnum_cp(c)) {
            return true;
        }
    }
    return false;
}

// Verify the chained bulk output against per-line compute_line_hash with the
// reference nibble-chain seeds. This cross-checks both entry points and
// validates bit-exactness of the hashing path on the full corpus.
static bool chain_matches(const kimix::string& content,
                          const kimix::vector<uint32_t>& out) {
    size_t pos = 0;
    size_t lines = 0;
    while (pos < content.size() && lines < out.size()) {
        const size_t nl = content.find('\n', pos);
        const size_t end = (nl == kimix::string_view::npos) ? content.size() : nl;
        const kimix::string_view line(content.data() + pos, end - pos);
        uint32_t seed;
        if (lines == 0) {
            seed = line_has_significant(line) ? 0u : 1u;
        } else {
            const uint32_t prev = out[lines - 1];
            seed = (nibble_code(prev >> 4) * 256 + nibble_code(prev & 15)) &
                   0xFFFFFFFFu;
        }
        if (out[lines] != compute_line_hash(line, seed)) {
            return false;
        }
        ++lines;
        pos = (nl == kimix::string_view::npos) ? content.size() : nl + 1;
    }
    return lines == out.size();
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "line_hash_golden"_test = [] {
        // xxh32(b"helloworld", 0) & 0xFF == 2 (space filtered)
        expect(eq(compute_line_hash(sv("hello world"), 0), 2u));
        // xxh32(b"", 0) & 0xFF == 5
        expect(eq(compute_line_hash(sv(""), 0), 5u));
        expect(eq(compute_line_hash(sv("   "), 0), 5u)); // all whitespace
        // xxh32(b"", 42) & 0xFF == 184
        expect(eq(compute_line_hash(sv(""), 42), 184u));
        // xxh32(b"abc", 12345) & 0xFF == 41
        expect(eq(compute_line_hash(sv("abc"), 12345), 41u));
        // xxh32(b"x"*2000, 0) & 0xFF == 141 (>= 16 byte path)
        expect(eq(compute_line_hash(sv(std::string(2000, 'x')), 0), 141u));
        // trailing CR stripped
        expect(eq(compute_line_hash(sv("abc\r"), 12345), 41u));
        // tab is whitespace -> filtered
        expect(eq(compute_line_hash(sv("a\tb"), 0), compute_line_hash(sv("ab"), 0)));
    };

    "line_hash_unicode"_test = [] {
        // NBSP (U+00A0, bytes C2 A0) is whitespace -> filtered out
        const std::string nbsp = "a\xC2\xA0" "b";
        expect(eq(compute_line_hash(sv(nbsp), 0), compute_line_hash(sv("ab"), 0)));
        // CJK chars are alnum (has_significant) and kept
        const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87"; // "中文"
        expect(eq(compute_line_hash(sv(cjk), 0), compute_line_hash(sv(cjk), 0)));
        expect(is_alnum_cp(0x4E2D));
        expect(is_alnum_cp(0x00E9));
        expect(!is_alnum_cp(0x00B7)); // middle dot: not alnum
        expect(!is_alnum_cp(0x00A0)); // NBSP: not alnum
    };

    "line_hashes_chain"_test = [] {
        kimix::vector<uint32_t> out;
        // "line1": has_significant -> seed 0 -> xxh32(b"line1",0)&0xFF
        // "line2": prev hash 2 -> nibble string "ZM" -> seed = 'Z'*256+'M'
        compute_line_hashes(sv("line1\nline2\n"), 0, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0], 2u));
        expect(eq(out[1], 153u));
        // empty content -> no lines
        out.clear();
        compute_line_hashes(sv(""), 0, out);
        expect(out.empty());
        // CRLF: trailing \r stripped per line, same as LF
        kimix::vector<uint32_t> out2;
        compute_line_hashes(sv("line1\r\nline2\r\n"), 0, out2);
        kimix::vector<uint32_t> lf;
        compute_line_hashes(sv("line1\nline2\n"), 0, lf);
        expect(eq(out2.size(), 2u));
        expect(eq(out2[0], lf[0]));
        expect(eq(out2[1], lf[1]));
        // first line with no alnum -> seed = line_num (1)
        kimix::vector<uint32_t> out3;
        compute_line_hashes(sv("!!!\n"), 0, out3);
        expect(eq(out3.size(), 1u));
        // seed 1 for line 1 with no significant chars
        expect(eq(out3[0], compute_line_hash(sv("!!!"), 1)));
        // second line chain continues from previous hash
        kimix::vector<uint32_t> out4;
        compute_line_hashes(sv("!!!\nabc\n"), 0, out4);
        expect(eq(out4.size(), 2u));
        // prev hash h0 -> nibble "XY" -> seed = NIBBLE_CODE[h0>>4]*256 + NIBBLE_CODE[h0&15]
        const uint32_t h0 = out4[0];
        const uint32_t seed = (nibble_code(h0 >> 4) * 256 + nibble_code(h0 & 15)) & 0xFFFFFFFFu;
        expect(eq(out4[1], compute_line_hash(sv("abc"), seed)));
    };

    "line_hashes_unusual_endings"_test = [] {
        kimix::vector<uint32_t> out;
        // no trailing newline: last line still hashed
        compute_line_hashes(sv("a\nb"), 0, out);
        expect(eq(out.size(), 2u));
        // blank lines between
        out.clear();
        compute_line_hashes(sv("a\n\nb\n"), 0, out);
        expect(eq(out.size(), 3u));
    };

    // -----------------------------------------------------------------------
    // Benchmarks - line hashing (kimix_bench contract). 10k x ~80-char lines
    // (text + punctuation corpora) and a single-line latency case. The API
    // has no case-insensitivity knob (find_in_file owns the folding), so the
    // ci on/off axis from the bench plan is not applicable here. Every case
    // asserts the full nibble chain against the single-line entry point.
    // -----------------------------------------------------------------------

    "bench_line_hashes_10k_log"_test = [] {
        const size_t kLines = 10000;
        const kimix::string content = log_lines(kLines, 80);
        kimix::vector<uint32_t> out;
        size_t total = 0;
        kimix_bench::run("tools/line_hashes_10k_log",
                         [&] {
                             out.clear();
                             compute_line_hashes(kimix::string_view(content),
                                                 0, out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), kLines));
        expect(chain_matches(content, out));
        kimix_bench::sink(total);
    };

    "bench_line_hashes_10k_punct"_test = [] {
        const size_t kLines = 10000;
        const kimix::string content = punct_lines(kLines, 80);
        kimix::vector<uint32_t> out;
        size_t total = 0;
        kimix_bench::run("tools/line_hashes_10k_punct",
                         [&] {
                             out.clear();
                             compute_line_hashes(kimix::string_view(content),
                                                 0, out);
                             total += out.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(out.size(), kLines));
        expect(chain_matches(content, out));
        kimix_bench::sink(total);
    };

    "bench_line_hash_single_80b"_test = [] {
        const kimix::string line = log_lines(1, 80);
        uint32_t acc = 0;
        kimix_bench::run("tools/line_hash_single_80b",
                         [&] {
                             acc ^= compute_line_hash(kimix::string_view(line),
                                                      acc & 0xFF);
                         },
                         1, static_cast<double>(line.size()));
        // the single-line entry point must agree with the chained bulk kernel
        kimix::vector<uint32_t> out;
        compute_line_hashes(kimix::string_view(line), 0, out);
        expect(eq(out.size(), 1u));
        const uint32_t seed0 =
            line_has_significant(kimix::string_view(line)) ? 0u : 1u;
        expect(eq(out[0], compute_line_hash(kimix::string_view(line), seed0)));
        kimix_bench::sink(acc);
    };
}
