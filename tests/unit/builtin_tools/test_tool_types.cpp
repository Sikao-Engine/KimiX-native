// Test for the shared built-in tool kernels (builtin_tools/tool_types.h +
// utf8_util.h). This test covers:
// - truncate_line: no-op short lines, code-point counting for non-ASCII, the
//   "… [+K chars]" marker, and the marker-doesn't-fit fallback
// - fold_lines: pass-through, head+tail split with the omission marker,
//   over-budget head clamp
// - dedup_lines: run collapsing with the "(N repeats)" suffix, min_repeats
//   floor, saved count accounting
// - join_with_byte_limit: byte budget cut + omitted count reporting
// - utf8_util: code point counts, prefix slicing on boundaries, strict
//   validation with CPython DecoderError wording
#include "ut/ut.hpp"

#include "builtin_tools/tool_types.h"
#include "builtin_tools/utf8_util.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;

namespace {

kimix::vector<kimix::string> to_vec(std::vector<std::string> in) {
    kimix::vector<kimix::string> out;
    out.reserve(in.size());
    for (auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

std::vector<std::string> from_vec(kimix::span<const kimix::string> in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "utf8_helpers"_test = [] {
        expect(eq(utf8_code_point_count("hello"), size_t(5)));
        // "héllo→" = 6 code points, 9 bytes
        expect(eq(utf8_code_point_count("h\xC3\xA9llo\xE2\x86\x92"), size_t(6)));
        expect(eq(utf8_byte_offset_of_code_point("h\xC3\xA9llo", 2), size_t(3)));
        expect(eq(utf8_byte_offset_of_code_point("abc", 99), size_t(3)));
        // floor boundary never splits a sequence
        expect(eq(utf8_floor_boundary("\xE2\x86\x92x", 1), size_t(0)));
        expect(eq(utf8_floor_boundary("\xE2\x86\x92x", 2), size_t(0)));
        expect(eq(utf8_floor_boundary("\xE2\x86\x92x", 3), size_t(3)));
        expect(is_ascii("abc")) << "ascii fast path";
        expect(!is_ascii("\xC3\xA9")) << "non-ascii";

        // strict validation
        size_t off = 12345;
        kimix::string reason;
        expect(utf8_strict_error("valid \xE2\x86\x92 utf-8", off, reason))
            << "valid text accepted";
        expect(!utf8_strict_error("\x80", off, reason)) << "lone continuation";
        expect(eq(off, size_t(0))) << "python reports the lead index";
        expect(eq(reason, kimix::string("invalid start byte")));
        expect(!utf8_strict_error("\xC3", off, reason)) << "truncated sequence";
        expect(eq(reason, kimix::string("unexpected end of data")));
        expect(!utf8_strict_error("\xED\xA0\x80", off, reason)) << "surrogate";
        expect(eq(reason, kimix::string("invalid continuation byte")))
            << "CPython wording for an over-range first continuation";
        expect(!utf8_strict_error("ab\xE0\x80\x80", off, reason));
        expect(eq(off, size_t(2))) << "offset points at the lead byte";
        expect(!utf8_strict_error("\xC0\x80", off, reason)) << "overlong";
        expect(eq(reason, kimix::string("invalid start byte")));
        expect(!utf8_strict_error("\xF5\x80\x80\x80", off, reason)) << "above U+10FFFF";
        expect(eq(reason, kimix::string("invalid start byte")));
        expect(utf8_strict_error("", off, reason)) << "empty is valid";
    };

    "truncate_line_basic"_test = [] {
        kimix::string out;
        truncate_line("short", 10, out);
        expect(eq(out, kimix::string("short"))) << "under budget unchanged";
        truncate_line("short", 5, out);
        expect(eq(out, kimix::string("short"))) << "exactly at budget unchanged";

        kimix::string long_line(60, 'a');
        truncate_line(long_line, 20, out);
        // 20 code points total: 9 'a' + "… [+40 chars]" (13 cp)
        expect(eq(utf8_code_point_count(out), size_t(20))) << "fits the budget";
        expect(out.starts_with("aaaaaaa")) << "head preserved (20 - 13 marker cp)";
        expect(out.ends_with(" chars]")) << "marker appended";
        expect(out.find("[+40 chars]") != kimix::string::npos)
            << "removed count reported";
    };

    "truncate_line_edge_cases"_test = [] {
        kimix::string out;
        // marker too large to fit: raw cut, no marker
        truncate_line(kimix::string(30, 'x'), 5, out);
        expect(eq(utf8_code_point_count(out), size_t(5)));
        expect(out.find("chars]") == kimix::string::npos) << "no marker when it would not fit";

        // non-ASCII: counting is in code points, slicing on a byte boundary
        kimix::string u8;
        for (int i = 0; i < 10; i++) {
            u8 += "\xE2\x86\x92"; // U+2192, 3 bytes
        }
        truncate_line(u8, 6, out);
        expect(eq(utf8_code_point_count(out), size_t(6))) << "6 code points out";
        expect(eq(out.size(), size_t(18))) << "12-cp marker does not fit in 6, raw cut";
        expect(utf8_validate(out)) << "never splits a sequence";
    };

    "fold_lines_head_tail"_test = [] {
        auto lines = to_vec({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"});
        kimix::vector<kimix::string> out;
        size_t omitted = 999;
        fold_lines(lines, 4, 2, 2, out, omitted);
        expect(eq(out.size(), size_t(5))) << "head 2 + marker + tail 2";
        expect(eq(out[0], kimix::string("1")));
      expect(eq(out[1], kimix::string("2")));
      expect(eq(out[2], kimix::string("\xE2\x80\xA6 (6 lines omitted) \xE2\x80\xA6")));
      expect(eq(out[3], kimix::string("9")));
      expect(eq(out[4], kimix::string("10")));
        expect(eq(omitted, size_t(6)));

        // under budget: unchanged, zero omitted
        fold_lines(lines, 100, 50, 50, out, omitted);
        expect(eq(out.size(), size_t(10)));
        expect(eq(omitted, size_t(0)));

        // max_lines == 0 means unlimited
        fold_lines(lines, 0, 0, 0, out, omitted);
        expect(eq(out.size(), size_t(10))) << "0 == unlimited";

        // over-budget head clamps the tail to zero
        fold_lines(lines, 4, 4, 4, out, omitted);
        expect(eq(out.size(), size_t(5))) << "head 4 + marker, tail clamped to 0";
        expect(eq(out[3], kimix::string("4"))) << "caller head is kept";
        expect(eq(omitted, size_t(6)));
    };

    "dedup_lines_runs"_test = [] {
        auto lines =
            to_vec({"a", "b", "b", "b", "b", "c", "d", "d", "e"});
        kimix::vector<kimix::string> out;
        size_t saved = 999;
        dedup_lines(lines, 3, out, saved);
        expect(eq(out.size(), size_t(6))) << "b-run collapsed, d-run kept";
        expect(eq(out[0], kimix::string("a")));
        expect(eq(out[1], kimix::string("b  (3 repeats)")));
        expect(eq(saved, size_t(3))) << "4 b lines save 3";
        expect(eq(out[2], kimix::string("c")));
        expect(eq(out[3], kimix::string("d"))) << "run below min_repeats untouched";
        expect(eq(out[5], kimix::string("e")));

        // min_repeats floors at 2
        dedup_lines(lines, 1, out, saved);
        expect(eq(out[1], kimix::string("b  (3 repeats)")));
        expect(eq(out[3], kimix::string("d  (1 repeats)")))
            << "matches Python: 2-line run collapses at min_repeats=2";

        // single line / empty input pass through
        auto one = to_vec({"solo"});
        dedup_lines(one, 3, out, saved);
        expect(eq(out.size(), size_t(1)));
        expect(eq(saved, size_t(0)));
    };

    "join_with_byte_limit"_test = [] {
        auto lines = to_vec({"aaaa", "bbbb", "cccc", "dddd"});
        kimix::string out;
        bool truncated = false;
        size_t omitted = 0;
        join_with_byte_limit(lines, 1024, out, truncated, omitted);
        expect(eq(out, kimix::string("aaaa\nbbbb\ncccc\ndddd")));
        expect(!truncated) << "inside budget";
        expect(eq(omitted, size_t(0)));

        join_with_byte_limit(lines, 10, out, truncated, omitted);
        expect(eq(out, kimix::string("aaaa\nbbbb")));
        expect(truncated) << "budget exceeded";
        expect(eq(omitted, size_t(2)));

        // empty input
        kimix::vector<kimix::string> none;
        join_with_byte_limit(none, 10, out, truncated, omitted);
        expect(out.empty()) << "nothing to join";
        expect(!truncated);
    };

    "named_value_and_status_defaults"_test = [] {
        tool_error e;
        expect(!e.failed()) << "default status is ok";
        e.status = tool_status::external_library;
        expect(e.failed());
        expect(e.message.empty());

        byte_range r{4, 10};
        expect(eq(r.size(), uint64_t(6)));

        line_range lr;
        expect(!lr.end_line.has_value()) << "open ended by default";
        expect(lr == line_range{1, {}}) << "value equality";
        expect(named_value{"k", "v"}.name == "k");
    };
}
