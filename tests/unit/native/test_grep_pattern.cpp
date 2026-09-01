// Test for src/runtime/tools/grep_pattern.h (plan: commit 0582e09 "Study from
// hermes" -- kimi-cli grep_local.py newline kernels).
// This test covers:
// - pattern_has_regex_newline: literal newline + odd/even backslash runs
// - multiline_pattern: CRLF normalization, regex \n escape rewrite, real
//   newline rewrite (order-safe)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/tools/grep_pattern.h>

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

// ---------------------------------------------------------------------------
// Reference models (exact ports of the Python recipe from grep_local.py);
// used as correctness oracles for the benchmark cases. The bench corpora are
// ASCII-only, matching the kernel's documented ASCII contract.
// ---------------------------------------------------------------------------

// True iff the pattern has a literal '\n' OR an odd-backslash `\n` escape.
static bool has_newline_ref(const std::string& p) {
    if (p.find('\n') != std::string::npos) {
        return true;
    }
    size_t i = 0;
    while (i < p.size()) {
        if (p[i] != '\\') {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < p.size() && p[j] == '\\') {
            ++j;
        }
        if (j < p.size() && p[j] == 'n' && ((j - i) % 2) == 1) {
            return true;
        }
        i = j;
    }
    return false;
}

// Exact 3-step port of _multiline_pattern:
//   1. "\r\n" -> "\n";  2. odd-backslash \n escape -> "\\r?\\n";
//   3. real '\n' -> "\\r?\\n".
static std::string multiline_ref(const std::string& p) {
    const std::string rep = "\\r?\\n";
    std::string s1;
    s1.reserve(p.size());
    for (size_t i = 0; i < p.size();) {
        if (i + 1 < p.size() && p[i] == '\r' && p[i + 1] == '\n') {
            s1.push_back('\n');
            i += 2;
        } else {
            s1.push_back(p[i]);
            ++i;
        }
    }
    std::string s2;
    s2.reserve(s1.size() + 16);
    size_t pos = 0;
    size_t i = 0;
    while (i < s1.size()) {
        if (s1[i] != '\\') {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s1.size() && s1[j] == '\\') {
            ++j;
        }
        if (j < s1.size() && s1[j] == 'n' && ((j - i) % 2) == 1) {
            s2.append(s1, pos, i - pos);
            s2 += rep;
            pos = j + 1;
            i = j + 1;
        } else {
            i = j;
        }
    }
    s2.append(s1, pos, s1.size() - pos);
    std::string s3;
    s3.reserve(s2.size() + 16);
    pos = 0;
    for (i = 0; i < s2.size(); ++i) {
        if (s2[i] == '\n') {
            s3.append(s2, pos, i - pos);
            s3 += rep;
            pos = i + 1;
        }
    }
    s3.append(s2, pos, s2.size() - pos);
    return s3;
}

// Pattern battery: literals, backslash-run escapes, real newlines, CRLF and
// order-sensitive run+CRLF combinations, plus larger multiline blocks.
static std::vector<std::string> pattern_battery() {
    std::vector<std::string> v;
    for (int i = 0; i < 40; ++i) {
        v.push_back("literal pattern " + std::to_string(i) + " alpha beta");
    }
    for (size_t run = 1; run <= 8; ++run) {
        v.push_back(std::string(run, '\\') + "n");
        v.push_back("prefix " + std::string(run, '\\') + "n suffix");
    }
    for (int i = 0; i < 10; ++i) {
        v.push_back("line one\nline two " + std::to_string(i));
    }
    for (int i = 0; i < 10; ++i) {
        v.push_back("a\r\nb\r\n" + std::to_string(i));
    }
    // Backslash runs adjacent to CRLF: the '\n' of a "\r\n" pair collapses in
    // step 1, so the escape test in step 2 is applied to run + 'n' (this is
    // order-sensitive and the reference model handles it).
    v.push_back("a\\\r\nb");
    v.push_back("a\\\\\r\nb");
    v.push_back("a\\\\\\\r\nb");
    v.push_back("a\\\\\\\\\r\nb");
    v.push_back("\\\r\n");
    v.push_back("\\\\\r\n");
    v.push_back("\\\\\\\r\n");
    v.push_back("x\\\\\r\n\\n");
    for (int i = 0; i < 10; ++i) {
        v.push_back("foo\\nbar\nbaz " + std::to_string(i));
    }
    for (int i = 0; i < 12; ++i) {
        v.push_back("struct X {\\n  int field_" + std::to_string(i) +
                    ";\n};\\n trailing garbage " + std::to_string(i));
    }
    return v;
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "has_regex_newline"_test = [] {
        expect(!pattern_has_regex_newline(sv("abc")));
        expect(pattern_has_regex_newline(sv("a\nb")));           // literal
        expect(pattern_has_regex_newline(sv("a\\nb")));          // odd (1)
        expect(!pattern_has_regex_newline(sv("a\\\\nb")));       // even (2)
        expect(pattern_has_regex_newline(sv("a\\\\\\nb")));      // odd (3)
        expect(pattern_has_regex_newline(sv("a\nb\\nc")));
        expect(pattern_has_regex_newline(sv("\\n")));
        expect(!pattern_has_regex_newline(sv("\\\\n")));
        expect(pattern_has_regex_newline(sv("\\\\\\n")));
        expect(pattern_has_regex_newline(sv("x\\ny")));
        expect(pattern_has_regex_newline(sv("a\r\nb")));         // literal \n
    };

    "multiline_no_newline"_test = [] {
        expect((multiline_pattern(sv("abc")) == "abc"));
        // even backslashes: literal backslash+n search, unchanged
        expect((multiline_pattern(sv("a\\\\nb")) == "a\\\\nb"));
    };

    "multiline_real_newlines"_test = [] {
        expect((multiline_pattern(sv("a\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("a\r\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("a\nb\\nc")) == "a\\r?\\nb\\r?\\nc"));
    };

    "multiline_regex_escapes"_test = [] {
        expect((multiline_pattern(sv("a\\nb")) == "a\\r?\\nb"));
        expect((multiline_pattern(sv("\\n")) == "\\r?\\n"));
        expect((multiline_pattern(sv("\\\\\\n")) == "\\r?\\n"));
    };

    // -----------------------------------------------------------------------
    // Benchmarks - newline-aware pattern analysis (kimix_bench contract).
    // Pattern analysis runs once per grep pattern at setup time, so the
    // workloads sweep a battery of many patterns and report per-pattern cost.
    // Every case also asserts each result against the reference model.
    // -----------------------------------------------------------------------

    "bench_pattern_has_newline_battery"_test = [] {
        const std::vector<std::string> pats = pattern_battery();
        size_t bytes = 0;
        for (const auto& p : pats) {
            bytes += p.size();
        }
        size_t hits = 0;
        kimix_bench::run("tools/pattern_has_newline_battery",
                         [&] {
                             size_t h = 0;
                             for (const auto& p : pats) {
                                 if (pattern_has_regex_newline(
                                         kimix::string_view(p))) {
                                     ++h;
                                 }
                             }
                             hits += h;
                         },
                         pats.size(), static_cast<double>(bytes));
        for (const auto& p : pats) {
            expect(eq(pattern_has_regex_newline(kimix::string_view(p)),
                      has_newline_ref(p)));
        }
        kimix_bench::sink(hits);
    };

    "bench_multiline_pattern_battery"_test = [] {
        const std::vector<std::string> pats = pattern_battery();
        size_t bytes = 0;
        for (const auto& p : pats) {
            bytes += p.size();
        }
        size_t out_bytes = 0;
        kimix_bench::run("tools/multiline_pattern_battery",
                         [&] {
                             size_t ob = 0;
                             for (const auto& p : pats) {
                                 const kimix::string m =
                                     multiline_pattern(kimix::string_view(p));
                                 ob += m.size();
                             }
                             out_bytes += ob;
                         },
                         pats.size(), static_cast<double>(bytes));
        for (const auto& p : pats) {
            const kimix::string m = multiline_pattern(kimix::string_view(p));
            const std::string ref = multiline_ref(p);
            expect(eq(kimix::string_view(m), kimix::string_view(ref)));
        }
        kimix_bench::sink(out_bytes);
    };
}
