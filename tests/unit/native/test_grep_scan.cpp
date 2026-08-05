// Test for src/runtime/tools/grep_scan.h (plan 013 single-pass line scan).
// This test covers:
// - splitlines() semantics for LF input (trailing newline, blank lines)
// - incremental byte offsets (no count("\n") per match)
// - matcher predicate receives the line WITHOUT its terminator + 0-based index
// - CRLF input (line retains the '\r' - documented in the header)

#include "ut/ut.hpp"
#include <runtime/tools/grep_scan.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }
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
}
