// Test for src/runtime/tools/grep_scan.h (plan 013 single-pass line scan).
// This test covers:
// - splitlines() semantics for LF input (trailing newline, blank lines)
// - incremental byte offsets (no count("\n") per match)
// - matcher predicate receives the line WITHOUT its terminator + 0-based index
// - CRLF input (line retains the '\r' - documented in the header)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/tools/grep_scan.h>

#include <regex>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

// Log-shaped lines of ~line_len bytes + '\n'. Every `match_modulo`-th line
// carries a (error|warning) "NNNms" phrase (match_modulo == 1 -> all lines
// match); the rest are plain INFO lines.
static std::string grep_content(size_t line_count, size_t line_len,
                                size_t match_modulo) {
    std::string out;
    out.reserve(line_count * (line_len + 1));
    for (size_t i = 0; i < line_count; ++i) {
        std::string line = "2026-08-31 12:00:00 ";
        if (i % match_modulo == 0) {
            line += "error \"";
            line += std::to_string((i * 7) % 900 + 1);
            line += "ms\" task=build path=/src/main.cpp";
        } else if (i % match_modulo == 1) {
            line += "warning \"";
            line += std::to_string((i * 3) % 500 + 1);
            line += "ms\" task=link path=/src/mem.cpp";
        } else {
            line += "INFO task=batch completed status=ok code=0";
        }
        if (line.size() > line_len) {
            line.resize(line_len);
        } else {
            line.append(line_len - line.size(), ' ');
        }
        out += line;
        out.push_back('\n');
    }
    return out;
}

// Token-dense corpus: every line contains an id_<n> token plus other text.
static std::string token_content(size_t line_count, size_t line_len) {
    std::string out;
    out.reserve(line_count * (line_len + 1));
    for (size_t i = 0; i < line_count; ++i) {
        std::string line = "INFO task=batch id_";
        line += std::to_string(i);
        line += " token=";
        line += std::to_string(i * 3);
        if (line.size() < line_len) {
            line.append(line_len - line.size(), ' ');
        }
        out += line;
        out.push_back('\n');
    }
    return out;
}

// Naive per-line reference: splitlines() for LF input + std::regex_search
// on the line WITHOUT its terminator (same contract as scan_lines).
static size_t ref_regex_count(const std::string& content,
                              const std::regex& re) {
    size_t count = 0;
    size_t ls = 0;
    while (ls < content.size()) {
        const size_t nl = content.find('\n', ls);
        const size_t le = (nl == std::string::npos) ? content.size() : nl;
        const kimix::string_view line(content.data() + ls, le - ls);
        if (std::regex_search(line.begin(), line.end(), re)) {
            ++count;
        }
        ls = (nl == std::string::npos) ? content.size() : nl + 1;
    }
    return count;
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "scan_lines_offsets"_test = [] {
        const std::string content = "aaa\nbb\naaa\n";
        kimix::vector<grep_hit> hits;
        scan_lines(sv(content),
                   [](kimix::string_view line, uint32_t) {
                       return line == "aaa";
                   },
                   hits);
        expect(eq(hits.size(), 2u));
        expect(eq(hits[0].line_index, 0u));
        expect(eq(hits[0].byte_offset, 0u));
        expect(eq(hits[0].line_len, 3u));
        expect(eq(hits[1].line_index, 2u));
        expect(eq(hits[1].byte_offset, 7u)); // "aaa\n" + "bb\n" = 7
        expect(eq(hits[1].line_len, 3u));
    };

    "scan_lines_splitlines_semantics"_test = [] {
        // "a\nb\n" -> ["a", "b"] (no trailing empty line)
        kimix::vector<grep_hit> hits;
        scan_lines(sv("a\nb\n"),
                   [](kimix::string_view, uint32_t) { return true; }, hits);
        expect(eq(hits.size(), 2u));
        // "a\n\n" -> ["a", ""]
        hits.clear();
        scan_lines(sv("a\n\n"),
                   [](kimix::string_view, uint32_t) { return true; }, hits);
        expect(eq(hits.size(), 2u));
        expect(eq(hits[1].line_len, 0u));
        // "" -> no lines
        hits.clear();
        scan_lines(sv(""),
                   [](kimix::string_view, uint32_t) { return true; }, hits);
        expect(hits.empty());
        // "\n" -> [""]
        hits.clear();
        scan_lines(sv("\n"),
                   [](kimix::string_view, uint32_t) { return true; }, hits);
        expect(eq(hits.size(), 1u));
        expect(eq(hits[0].byte_offset, 0u));
    };

    "scan_lines_line_index_order"_test = [] {
        const std::string content = "x\ny\nz\n";
        kimix::vector<grep_hit> hits;
        kimix::vector<uint32_t> seen;
        scan_lines(sv(content),
                   [&](kimix::string_view, uint32_t line_index) {
                       seen.push_back(line_index);
                       return true;
                   },
                   hits);
        expect(eq(seen.size(), 3u));
        expect(eq(seen[0], 0u));
        expect(eq(seen[1], 1u));
        expect(eq(seen[2], 2u));
        expect(eq(hits.size(), 3u));
    };

    "scan_lines_crlf"_test = [] {
        // CRLF: the line retains its '\r' (documented; parity tests use the
        // shim which normalizes before scanning)
        const std::string content = "a\r\nb\r\n";
        kimix::vector<grep_hit> hits;
        scan_lines(sv(content),
                   [](kimix::string_view line, uint32_t) { return line == "a\r"; },
                   hits);
        expect(eq(hits.size(), 1u));
        expect(eq(hits[0].line_len, 2u));
        expect(eq(hits[0].byte_offset, 0u));
    };

    "scan_lines_empty_matcher"_test = [] {
        const std::string content = "a\nb\n";
        kimix::vector<grep_hit> hits;
        // a null matcher accepts nothing
        scan_lines(sv(content), kimix::function<bool(kimix::string_view, uint32_t)>(), hits);
        expect(hits.empty());
    };

    // -----------------------------------------------------------------------
    // Benchmarks - scan_lines (kimix_bench contract). 10k x ~80-char log
    // lines; the regex is compiled ONCE and captured (production shape: the
    // binding compiles the pattern once and scans many lines). Hit counts are
    // asserted against a naive per-line reference. Includes dense, sparse and
    // pure-scanner (no regex) workloads.
    // -----------------------------------------------------------------------

    "bench_scan_regex_error_sparse"_test = [] {
        const std::string content = grep_content(10000, 80, 97);
        const std::regex re(R"((error|warning) "[0-9]+ms")");
        kimix::vector<grep_hit> hits;
        size_t total = 0;
        kimix_bench::run("tools/scan_regex_error_sparse",
                         [&] {
                             hits.clear();
                             scan_lines(
                                 kimix::string_view(content),
                                 [&](kimix::string_view line, uint32_t) {
                                     return std::regex_search(line.begin(),
                                                              line.end(), re);
                                 },
                                 hits);
                             total += hits.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(hits.size(), ref_regex_count(content, re)));
        kimix_bench::sink(total);
    };

    "bench_scan_regex_error_dense"_test = [] {
        const std::string content = grep_content(10000, 80, 2);
        const std::regex re(R"((error|warning) "[0-9]+ms")");
        kimix::vector<grep_hit> hits;
        size_t total = 0;
        kimix_bench::run("tools/scan_regex_error_dense",
                         [&] {
                             hits.clear();
                             scan_lines(
                                 kimix::string_view(content),
                                 [&](kimix::string_view line, uint32_t) {
                                     return std::regex_search(line.begin(),
                                                              line.end(), re);
                                 },
                                 hits);
                             total += hits.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(hits.size(), ref_regex_count(content, re)));
        kimix_bench::sink(total);
    };

    "bench_scan_regex_token"_test = [] {
        const std::string content = token_content(10000, 80);
        const std::regex re(R"(id_[0-9]+)");
        kimix::vector<grep_hit> hits;
        size_t total = 0;
        kimix_bench::run("tools/scan_regex_token",
                         [&] {
                             hits.clear();
                             scan_lines(
                                 kimix::string_view(content),
                                 [&](kimix::string_view line, uint32_t) {
                                     return std::regex_search(line.begin(),
                                                              line.end(), re);
                                 },
                                 hits);
                             total += hits.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(hits.size(), ref_regex_count(content, re)));
        kimix_bench::sink(total);
    };

    "bench_scan_pure_scanner"_test = [] {
        const std::string content = grep_content(10000, 80, 97);
        kimix::vector<grep_hit> hits;
        size_t total = 0;
        kimix_bench::run("tools/scan_all_lines",
                         [&] {
                             hits.clear();
                             scan_lines(
                                 kimix::string_view(content),
                                 [](kimix::string_view, uint32_t) { return true; },
                                 hits);
                             total += hits.size();
                         },
                         1, static_cast<double>(content.size()));
        expect(eq(hits.size(), size_t(10000)));
        // first and last hit offsets are exact (incremental scanner contract)
        if (!hits.empty()) {
            expect(eq(hits[0].byte_offset, 0u));
            expect(eq(hits.back().byte_offset, content.size() - 81u));
            expect(eq(hits.back().line_index, uint32_t(9999)));
        }
        kimix_bench::sink(total);
    };
}
