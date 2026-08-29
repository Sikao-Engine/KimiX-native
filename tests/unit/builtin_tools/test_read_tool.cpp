// Test for the `read` built-in tool kernels (builtin_tools/read_tool.h).
//
// Covers (plan read.md §7):
//   - validate_int_option: byte-exact ValueError messages from Params
//   - truncate_line_read: "..." marker + preserved line-break run, code-point
//     counting, budget raise for tiny max_len
//   - split_lines: LF/CRLF/lone-CR normalization, trailing-line handling,
//     errors="replace" for malformed UTF-8
//   - render_forward: "%6d\t" prefix, offset/limit, MAX_LINES/byte budgets,
//     end-of-file, truncated line numbers, byte-exact messages
//   - render_tail: ring-buffer tail window, byte-budget reverse scan,
//     start_line / total_lines
//   - apply_char_window: head/middle/tail NOTE text byte-exact
//   - compute_line_hashes: chained xxHash32 parity with hash_line.py
//   - collapse_repeated_lines: run collapsing with "(N repeats)"
//   - render_cpu_profile: V8 .cpuprofile summarizer goldens (samples +
//     timeDeltas, hitCount fallback, idle exclusion, "%.2f%%" formatting)
//   - render_sample_profile: macOS sample parser goldens (preamble, threads,
//     decorators, wait-frame exclusion, demangling, unrecognized input)
//   - markdown_to_text: each regex pass golden + blank collapse
#include "ut/ut.hpp"

#include "builtin_tools/read_tool.h"
#include "builtin_tools/tool_types.h"
#include "builtin_tools/utf8_util.h"

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::read;

namespace {

kimix::vector<kimix::string> to_vec(std::initializer_list<std::string> in) {
    kimix::vector<kimix::string> out;
    out.reserve(in.size());
    for (const auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

std::string s_of(const kimix::string &s) { return std::string(s.data(), s.size()); }

kimix::string k_of(std::string_view s) { return kimix::string(s.data(), s.size()); }

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ── Parameter validation ────────────────────────────────────────────────

    "validate_int_option"_test = [] {
        expect(!validate_int_option("offset", 1).failed());
        expect(!validate_int_option("offset", -5000).failed());
        expect(!validate_int_option("limit", 1).failed());
        expect(!validate_int_option("max_char", 0).failed());
        expect(!validate_int_option("char_offset", 0).failed());

        tool_error e = validate_int_option("offset", 0);
        expect(e.failed());
        expect(e.status == tool_status::invalid_input);
        expect(eq(e.message, kimix::string("offset cannot be 0; use 1 for the "
                                           "first line or -1 for the last line")));

        e = validate_int_option("offset", -5001);
        expect(e.failed());
        expect(e.message.find("offset cannot be less than -5000") !=
               kimix::string::npos);

        e = validate_int_option("limit", 0);
        expect(e.failed());
        expect(eq(e.message, kimix::string("limit must be >= 1.")));

        e = validate_int_option("max_char", -1);
        expect(e.failed());
        expect(eq(e.message, kimix::string("max_char must be >= 0.")));

        e = validate_int_option("char_offset", -3);
        expect(e.failed());
        expect(eq(e.message, kimix::string("char_offset must be >= 0.")));
    };

    // ── truncate_line_read ──────────────────────────────────────────────────

    "truncate_line_read_basic"_test = [] {
        kimix::string out;
        truncate_line_read("short", 10, out);
        expect(eq(out, kimix::string("short"))) << "under budget unchanged";
        truncate_line_read(kimix::string(4000, 'c'), 4000, out);
        expect(eq(utf8_code_point_count(out), size_t(4000))) << "at budget";

        // 4005 'a' -> 4000 chars total: head + "..." (marker, 3 cp)
        truncate_line_read(kimix::string(4005, 'a'), 4000, out);
        expect(eq(utf8_code_point_count(out), size_t(4000)));
        expect(out.ends_with("...")) << "utils.py marker is three dots";

        // 4001 'd' -> removed = 1
        truncate_line_read(kimix::string(4001, 'd'), 4000, out);
        expect(eq(utf8_code_point_count(out), size_t(4000)));
        expect(out.ends_with("..."));
    };

    "truncate_line_read_linebreak"_test = [] {
        // Line-break run at the end is preserved and budget is raised to fit
        // marker + linebreak (utils.py: max_length = max(max_length, len(end))).
        kimix::string out;
        kimix::string line = kimix::string(50, 'x') + "\n";
        truncate_line_read(line, 10, out);
        // end = "...\n" (4 cp); keep = 10-4 = 6 'x' + "..." + "\n"
        expect(eq(out, kimix::string("xxxxxx") + "...\n"))
            << "line break preserved after marker";

        // Tiny budget raised to len(end)
        kimix::string line2 = kimix::string(50, 'y') + "\r\n";
        truncate_line_read(line2, 1, out);
        // end = "...\r\n" (5 cp); budget raised to 5; keep 0 + marker + "\r\n"
        expect(eq(out, kimix::string("...\r\n")))
            << "budget raised to marker+linebreak length";

        // Multi-byte: code-point counting, byte-boundary slicing
        kimix::string u8;
        for (int i = 0; i < 2500; i++) {
            u8 += "\xE6\xB1\x89\xE5\xAD\x97"; // 汉字 (2 cp, 6 bytes)
        }
        truncate_line_read(u8, 4000, out);
        expect(eq(utf8_code_point_count(out), size_t(4000)));
        expect(utf8_validate(out)) << "never splits a sequence";
    };

    // ── split_lines ─────────────────────────────────────────────────────────

    "split_lines_endings"_test = [] {
        auto lines = split_lines("a\nb\nc\n");
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a\n")));
        expect(eq(lines[2], kimix::string("c\n")));

        lines = split_lines("a\nb\nc");
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[2], kimix::string("c"))) << "no trailing newline kept";

        lines = split_lines("");
        expect(lines.empty());
    };

    "split_lines_crlf_cr"_test = [] {
        auto lines = split_lines("a\r\nb\r\nc");
        expect(eq(lines.size(), size_t(3)));
        expect(eq(lines[0], kimix::string("a\n"))) << "CRLF -> single LF";
        expect(eq(lines[1], kimix::string("b\n")));
        expect(eq(lines[2], kimix::string("c")));

        lines = split_lines("a\rb\rc");
        expect(eq(lines.size(), size_t(3))) << "lone CR is a line break";
        expect(eq(lines[0], kimix::string("a\n")));

        lines = split_lines("a\r\n");
        expect(eq(lines.size(), size_t(1)));
        expect(eq(lines[0], kimix::string("a\n")));
    };

    "split_lines_replace_errors"_test = [] {
        // Invalid UTF-8 decodes to U+FFFD (errors="replace"), never loops.
        auto lines = split_lines("ab\xFF" "cd\nnext\n");
        expect(eq(lines.size(), size_t(2)));
        // U+FFFD = EF BF BD
        expect(eq(lines[0], kimix::string("ab\xEF\xBF\xBD" "cd\n")));
        expect(eq(lines[1], kimix::string("next\n")));

        lines = split_lines("\xC3"); // truncated sequence
        expect(eq(lines.size(), size_t(1)));
        expect(eq(lines[0], kimix::string("\xEF\xBF\xBD")));
    };

    // ── render_forward ──────────────────────────────────────────────────────

    "render_forward_basic"_test = [] {
        auto lines = to_vec({"line1\n", "line2\n", "line3\n", "line4\n",
                             "line5\n", "line6\n", "line7\n"});
        auto r = render_forward(lines, "f.txt", 1, 2000);
        expect(eq(r.output, kimix::string("     1\tline1\n     2\tline2\n     3\tline3\n"
                                          "     4\tline4\n     5\tline5\n     6\tline6\n"
                                          "     7\tline7\n")))
            << "%6d gutter formatting";
        expect(eq(r.message, kimix::string("7 lines read from file starting "
                                           "from line 1. Total lines in file: "
                                           "7. End of file reached. Path: f.txt")));
        expect(eq(r.window_lines.size(), size_t(7)));
        expect(eq(r.window_lines[0], kimix::string("line1")))
            << "window lines are rstripped";
        expect(eq(r.start_line, int64_t(1)));
        expect(r.end_of_file);
        expect(!r.max_bytes_reached);
        expect(!r.max_lines_reached);
    };

    "render_forward_offset_limit"_test = [] {
        auto lines = to_vec({"line1\n", "line2\n", "line3\n", "line4\n",
                             "line5\n", "line6\n", "line7\n"});
        auto r = render_forward(lines, "f.txt", 3, 2);
        expect(eq(r.output, kimix::string("     3\tline3\n     4\tline4\n")));
        expect(eq(r.message, kimix::string("2 lines read from file starting "
                                           "from line 3. Path: f.txt")))
            << "no end-of-file note when the limit stopped the read";
        expect(!r.end_of_file);
        expect(eq(r.total_lines, int64_t(-1))) << "unknown when cut short";

        // Offset beyond EOF
        r = render_forward(lines, "f.txt", 100, 2000);
        expect(r.output.empty());
        expect(eq(r.message, kimix::string("No lines read from file. Total "
                                           "lines in file: 7. End of file "
                                           "reached. Path: f.txt")));
        expect(r.window_lines.empty());
    };

    "render_forward_empty"_test = [] {
        kimix::vector<kimix::string> lines;
        auto r = render_forward(lines, "e.txt", 1, 2000);
        expect(r.output.empty());
        expect(eq(r.message, kimix::string("No lines read from file. Total "
                                           "lines in file: 0. End of file "
                                           "reached. Path: e.txt")));
        expect(eq(r.total_lines, int64_t(0)));
    };

    "render_forward_truncation_note"_test = [] {
        kimix::string long_line(4005, 'x');
        long_line += "\n";
        kimix::vector<kimix::string> lines;
        lines.push_back(long_line);
        lines.emplace_back("short\n");
        auto r = render_forward(lines, "l.txt", 1, 2000);
        expect(eq(r.message.find("Lines [1] were truncated."),
                  kimix::string::npos) != true)
            << "truncated line list rendered";
        expect(eq(r.truncated_line_numbers.size(), size_t(1)));
        expect(eq(r.truncated_line_numbers[0], int64_t(1)));

        // Two truncated lines -> Python list repr "[1, 2]"
        kimix::string long_line2(5000, 'y');
        long_line2 += "\n";
        kimix::vector<kimix::string> lines2;
        lines2.push_back(long_line);
        lines2.push_back(long_line2);
        auto r2 = render_forward(lines2, "l.txt", 1, 2000);
        expect(r2.message.find("Lines [1, 2] were truncated.") !=
               kimix::string::npos)
            << "list repr uses ', ' separators";
    };

    "render_forward_byte_budget"_test = [] {
        // 200 lines of 1000 'a' + '\n' (1001 bytes each) -> 102400-byte budget
        kimix::vector<kimix::string> lines;
        for (int i = 0; i < 200; i++) {
            lines.push_back(kimix::string(1000, 'a') + "\n");
        }
        auto r = render_forward(lines, "b.txt", 1, 5000);
        expect(eq(r.window_lines.size(), size_t(103)))
            << "103 lines * 1001 = 103103 >= 102400";
        expect(r.max_bytes_reached);
        expect(r.end_of_file)
            << "Python sets end_of_file = len(entries) < n_lines even when the "
               "byte budget stopped the read (103 < 5000)";
        expect(r.message.find("Max 102400 bytes reached.") !=
               kimix::string::npos);
        expect(eq(r.total_lines, int64_t(-1)));
    };

    "render_forward_no_line_numbers"_test = [] {
        auto lines = to_vec({"line1\n", "line2\n"});
        auto r = render_forward(lines, "f.txt", 1, 2000, false);
        expect(eq(r.output, kimix::string("line1\nline2\n")));
    };

    // ── render_tail ─────────────────────────────────────────────────────────

    "render_tail_basic"_test = [] {
        kimix::vector<kimix::string> lines;
        for (int i = 1; i <= 10; i++) {
            lines.push_back(kimix::format("t{}\n", i));
        }
        auto r = render_tail(lines, "t.txt", -3, 2000);
        expect(eq(r.output, kimix::string("     8\tt8\n     9\tt9\n    10\tt10\n")));
        expect(eq(r.message, kimix::string("3 lines read from file starting "
                                           "from line 8. Total lines in file: "
                                           "10. End of file reached. Path: t.txt")));
        expect(eq(r.start_line, int64_t(8)));
        expect(eq(r.total_lines, int64_t(10)));
        expect(eq(r.window_lines.size(), size_t(3)));
        expect(eq(r.window_lines[0], kimix::string("t8")));
    };

    "render_tail_limit"_test = [] {
        kimix::vector<kimix::string> lines;
        for (int i = 1; i <= 10; i++) {
            lines.push_back(kimix::format("t{}\n", i));
        }
        // Window of 8, but only 3 lines requested from the head of the window.
        auto r = render_tail(lines, "t.txt", -8, 3);
        expect(eq(r.output, kimix::string("     3\tt3\n     4\tt4\n     5\tt5\n")));
        expect(r.message.find("3 lines read from file starting from line 3.") ==
               size_t(0));
        expect(!r.end_of_file) << "3 < 8 window lines means more content";
        expect(eq(r.total_lines, int64_t(10)));
    };

    "render_tail_empty"_test = [] {
        kimix::vector<kimix::string> lines;
        auto r = render_tail(lines, "e.txt", -5, 2000);
        expect(r.output.empty());
        expect(eq(r.message, kimix::string("No lines read from file. Total "
                                           "lines in file: 0. End of file "
                                           "reached. Path: e.txt")));
        expect(eq(r.start_line, int64_t(1))) << "total_lines + 1 == 1";
    };

    "render_tail_byte_budget"_test = [] {
        // 200 lines of 1000 'b' + '\n'; tail window 150 -> byte budget trims
        // from the head, keeping the newest lines.
        kimix::vector<kimix::string> lines;
        for (int i = 0; i < 200; i++) {
            lines.push_back(kimix::string(1000, 'b') + "\n");
        }
        auto r = render_tail(lines, "b.txt", -150, 5000);
        expect(r.max_bytes_reached);
        expect(eq(r.window_lines.size(), size_t(102)))
            << "102 lines * 1001 = 102102 <= 102400";
        expect(eq(r.start_line, int64_t(99))) << "newest lines kept";
        expect(eq(r.total_lines, int64_t(200)));
        expect(r.message.find("Max 102400 bytes reached.") !=
               kimix::string::npos);
        // First rendered line number is 99.
        expect(r.output.starts_with("    99\t"));
    };

    "render_tail_ring_buffer_matches_deque"_test = [] {
        // Random-ish stress: the ring buffer must keep exactly the last
        // tail_count lines, matching a naive reference model.
        kimix::vector<kimix::string> lines;
        for (int i = 1; i <= 37; i++) {
            lines.push_back(kimix::format("x{}\n", i));
        }
        for (const int64_t window : {int64_t(1), int64_t(5), int64_t(36),
                                     int64_t(37), int64_t(50)}) {
            auto r = render_tail(lines, "t.txt", -window, 2000);
            const int64_t expected_count = std::min<int64_t>(window, 37);
            const int64_t first = 37 - expected_count + 1;
            expect(eq(r.total_lines, int64_t(37)));
            if (expected_count > 0) {
                expect(eq(r.start_line, first)) << "window=" << window;
                expect(eq(static_cast<int64_t>(r.window_lines.size()),
                          expected_count))
                    << "window=" << window;
            }
        }
    };

    // ── apply_char_window ───────────────────────────────────────────────────

    "apply_char_window_notes"_test = [] {
        const kimix::string text = "0123456789ABCDEFGHIJ"; // 20 cp
        auto w = apply_char_window(text, 0, 10);
        expect(eq(w.output, kimix::string("0123456789")));
        expect(eq(w.note, kimix::string(" NOTE: output window shows head "
                                        "chars 0..10 of 20 (content after is "
                                        "hidden); max_char=10. Raise max_char "
                                        "/ adjust char_offset to read the rest.")));

        w = apply_char_window(text, 5, 10);
        expect(eq(w.output, kimix::string("56789ABCDE")));
        expect(eq(w.note, kimix::string(" NOTE: output window shows middle "
                                        "chars 5..15 of 20 (content before "
                                        "and after is hidden); max_char=10. "
                                        "Raise max_char / adjust char_offset "
                                        "to read the rest.")));

        w = apply_char_window(text, 5, 100);
        expect(eq(w.output, kimix::string("56789ABCDEFGHIJ")));
        expect(eq(w.note, kimix::string(" NOTE: output window shows tail "
                                        "chars 5..20 of 20 (content before is "
                                        "hidden); max_char=100. Raise max_char "
                                        "/ adjust char_offset to read the rest.")));

        w = apply_char_window(text, 0, 100);
        expect(eq(w.output, text));
        expect(w.note.empty()) << "nothing hidden -> no note";
    };

    "apply_char_window_unicode"_test = [] {
        // Code-point (not byte) slicing across multi-byte chars.
        const kimix::string text = "\xE2\x86\x92\xE2\x86\x92\xE2\x86\x92"
                                   "\xF0\x9F\x98\x80"; // →→→😀 = 4 cp
        auto w = apply_char_window(text, 1, 2);
        expect(eq(w.output, kimix::string("\xE2\x86\x92\xE2\x86\x92")));
        expect(utf8_validate(w.output));
    };

    // ── compute_line_hashes ─────────────────────────────────────────────────

    "compute_line_hashes_parity"_test = [] {
        // Golden vectors generated from kimi_cli.tools.file.hash_line
        // _cumulative_hashes. The kernel splits on '\n' and drops the
        // trailing empty element (same convention as the runtime bulk kernel
        // in src/runtime/tools/line_hash.cpp), so a trailing '\n' does not
        // produce an extra hash.
        auto check = [](kimix::string_view content,
                        std::initializer_list<std::string> expected) {
            auto got = compute_line_hash_strings(content);
            expect(eq(got.size(), expected.size())) << "count";
            size_t i = 0;
            for (const auto &e : expected) {
                if (i < got.size()) {
                    expect(eq(got[i], kimix::string(e))) << "line " << i;
                }
                i++;
            }
        };
        check("line one\nline two\nline three", {"MN", "BK", "JW"});
        check("a", {"RW"});
        check("", {}); // empty content yields no lines
        check("   \n  \nx=1\n\ny", {"KM", "WV", "YH", "TX", "JY"});
        check("caf\xC3\xA9 \xE2\x86\x92 unicode\nfoo", {"KN", "PT"});
        check("a\r\nb\r\n", {"RW", "YW"}); // trailing newline drops empty tail
    };

    "collapse_repeated_lines"_test = [] {
        auto lines = to_vec({"a", "b", "b", "b", "b", "c", "d", "d", "e"});
        // Independent (non-chained) hashes so identical lines compare equal.
        kimix::vector<uint32_t> hashes;
        for (const auto &l : lines) {
            hashes.push_back(line_hash_independent(l));
        }
        expect(eq(hashes[1], hashes[2])) << "identical lines hash identically";
        kimix::vector<kimix::string> out;
        size_t saved = 999;
        collapse_repeated_lines(hashes, lines, 3, out, saved);
        expect(eq(out.size(), size_t(6)));
        expect(eq(out[1], kimix::string("b  (3 repeats)")));
        expect(eq(saved, size_t(3)));
        expect(eq(out[3], kimix::string("d"))) << "run below min kept";

        collapse_repeated_lines(hashes, lines, 2, out, saved);
        expect(eq(out[3], kimix::string("d  (1 repeats)")));
    };

    // ── render_cpu_profile ──────────────────────────────────────────────────

    "cpu_profile_samples_golden"_test = [] {
        // Realistic V8 profile with samples + timeDeltas.
        const char *json = R"json({
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "compute", "url": "core.js", "lineNumber": 42}},
                {"id": 4, "parent": 2, "callFrame": {"functionName": "inner", "url": "core.js", "lineNumber": 44}}
            ],
            "samples": [2, 2, 4, 2, 4],
            "timeDeltas": [100, 200, 100, 300, 100],
            "startTime": 0,
            "endTime": 800
        })json";
        kimix::string out;
        expect(render_cpu_profile(json, out)) << "valid profile parses";
        const kimix::string expected =
            "V8 CPU profile: 800\xCE\xBCs wall clock, 5 samples (avg interval "
            "320\xCE\xBCs)\n"
            "\n"
            "## Hot paths\n"
            "\n"
            "## Top functions by self time\n"
            "1. compute (core.js:42) \xE2\x80\x94 480\xCE\xBCs (30.00%)\n"
            "2. inner (core.js:44) \xE2\x80\x94 320\xCE\xBCs (20.00%)\n"
            "\n"
            "[Summarized view of CPU profile. Use profile_raw=True to read the "
            "original JSON.]";
        expect(eq(out, expected)) << "byte-exact summary";
    };

    "cpu_profile_hot_paths"_test = [] {
        // A tree whose root children fall BELOW the prune threshold (the
        // reference adds 3*avg_interval AND re-counts every sample into
        // total_micros, so the threshold ends up above each child's total).
        // Hot paths is then empty; "(idle)" stays excluded from top functions.
        const char *json = R"json({
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "main", "url": "app.js", "lineNumber": 1}},
                {"id": 3, "parent": 2, "callFrame": {"functionName": "hot", "url": "app.js", "lineNumber": 10}},
                {"id": 4, "parent": 1, "callFrame": {"functionName": "(idle)", "url": "", "lineNumber": -1}}
            ],
            "samples": [3, 3, 3, 3, 4],
            "timeDeltas": [1000, 1000, 1000, 1000, 1000],
            "startTime": 0,
            "endTime": 5000
        })json";
        kimix::string out;
        expect(render_cpu_profile(json, out));
        expect(out.find("## Hot paths") != kimix::string::npos);
        expect(out.find("main (app.js:1)") == kimix::string::npos)
            << "children below threshold are pruned from hot paths";
        expect(out.find("(idle)") == kimix::string::npos)
            << "idle excluded from top functions";
        // total_micros counts deltas (5000) plus one avg_interval per sample
        // (5*1000), giving 10000; hot = 4*1000 = 4000 -> 40.00%.
        expect(out.find("1. hot (app.js:10) \xE2\x80\x94 4000\xCE\xBCs (40.00%)") !=
               kimix::string::npos);
        expect(out.find("avg interval 2000\xCE\xBCs") != kimix::string::npos);
    };

    "cpu_profile_invalid"_test = [] {
        kimix::string out;
        expect(!render_cpu_profile("not json at all", out));
        expect(!render_cpu_profile("[1,2,3]", out)) << "non-object root";
        expect(!render_cpu_profile(R"json({"nodes": "nope", "samples": []})json", out))
            << "nodes not an array";
        expect(!render_cpu_profile(R"json({"nodes": [], "samples": []})json", out))
            << "empty node map";
    };

    "cpu_profile_hitcount_fallback"_test = [] {
        // hitCount-only profile (no samples). The Python reference crashes on
        // this shape (read_profiles.py:213 uses n.hitCount on a dict); the C++
        // port implements the intended dict.get("hitCount") behavior — see
        // reports/read.md deviations.
        const char *json = R"json({
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}, "hitCount": 0},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "work", "url": "a.js", "lineNumber": 3}, "hitCount": 2}
            ],
            "samples": [],
            "timeDeltas": [],
            "startTime": 1000000,
            "endTime": 1004000
        })json";
        kimix::string out;
        expect(render_cpu_profile(json, out)) << "hitCount fallback handled";
        expect(out.find("2 samples") != kimix::string::npos);
        expect(out.find("work (a.js:3)") != kimix::string::npos);
    };

    // ── render_sample_profile ───────────────────────────────────────────────

    "sample_profile_golden"_test = [] {
        const char *sample =
            "Analysis of sampling python3 1234 every 1 millisecond\n"
            "Process:         python3 [1234]\n"
            "Path:            /usr/bin/python3\n"
            "Duration:        1.05s\n"
            "\n"
            "Call graph:\n"
            "    5 Thread_123   DispatchQueue_1\n"
            "    + 5 start_wakeup (in libdyld.dylib) + 1 [0x7fff]\n"
            "    +   5 _pthread_start (in libsystem_pthread.dylib) + 123 [0x7fff]\n"
            "    +     3 hot_loop (in python3) + 42 [0x100]\n"
            "    +     ! 2 _ZN6kernel9hot_loop2Ev (in python3) + 10 [0x101]\n"
            "    +     ! 1 _ZN6kernel9hot_loop2Ev (in python3) + 10 [0x101]\n"
            "    +     1 __pthread_cond_wait (in libsystem_pthread.dylib) + 5 [0x102]\n"
            "\n"
            "Total number in stack (recursive count):\n";
        kimix::string out;
        expect(render_sample_profile(sample, out));
        const kimix::string expected =
            "macOS sample profile: 7 samples across 1 thread(s), 28.6% in "
            "wait/idle frames\n"
            "\n"
            "## Top functions by self samples\n"
            "1. 5 start_wakeup \xE2\x80\x94 1 self samples (14.29%)\n"
            "2. 5 _pthread_start \xE2\x80\x94 1 self samples (14.29%)\n"
            "3. 3 hot_loop \xE2\x80\x94 1 self samples (14.29%)\n"
            "4. ! 2 _ZN6kernel9hot_loop2Ev \xE2\x80\x94 1 self samples (14.29%)\n"
            "5. ! 1 _ZN6kernel9hot_loop2Ev \xE2\x80\x94 1 self samples (14.29%)\n"
            "\n"
            "[Summarized view of sample profile. Use profile_raw=True to read "
            "the original text.]";
        expect(eq(out, expected)) << "byte-exact sample summary";
    };

    "sample_profile_multi_thread"_test = [] {
        const char *sample =
            "Analysis of sampling python3 1234 every 1 millisecond\n"
            "Process:         python3 [1234]\n"
            "Path:            /usr/bin/python3\n"
            "Duration:        1.05s\n"
            "\n"
            "Call graph:\n"
            "    5 Thread_123   DispatchQueue_1\n"
            "    +   5 start_wakeup (in libdyld.dylib) + 1 [0x7fff]\n"
            "    +       3 hot_loop (in python3) + 42 [0x100]\n"
            "    +       2 __pthread_cond_wait (in libsystem_pthread.dylib) + 5 [0x102]\n"
            "    3 Thread_456\n"
            "    +   3 select (in libsystem_kernel.dylib) + 9 [0x200]\n"
            "\n"
            "Total number in stack (recursive count):\n";
        kimix::string out;
        expect(render_sample_profile(sample, out));
        const kimix::string expected =
            "macOS sample profile: 5 samples across 1 thread(s), 60.0% in "
            "wait/idle frames\n"
            "\n"
            "## Top functions by self samples\n"
            "1. 5 start_wakeup \xE2\x80\x94 1 self samples (20.00%)\n"
            "2. 3 hot_loop \xE2\x80\x94 1 self samples (20.00%)\n"
            "\n"
            "[Summarized view of sample profile. Use profile_raw=True to read "
            "the original text.]";
        expect(eq(out, expected)) << "matches Python reference exactly";
    };

    "sample_profile_unrecognized"_test = [] {
        kimix::string out;
        expect(!render_sample_profile("just some text\nno profile here", out));
        expect(!render_sample_profile("Call graph:\nnothing else", out))
            << "preamble required";
        expect(!render_sample_profile("", out));
    };

    // ── markdown_to_text ────────────────────────────────────────────────────

    "markdown_headings_emphasis"_test = [] {
        expect(eq(markdown_to_text("# Heading\n\nSome **bold** and *italic* text."),
                  kimix::string("Heading\n\nSome bold and italic text.")));
        expect(eq(markdown_to_text("## Sub __bold__ heading\n\nlist:\n- a\n- b"),
                  kimix::string("Sub bold heading\n\nlist:\n- a\n- b")));
    };

    "markdown_code_blocks"_test = [] {
        expect(eq(markdown_to_text("```python\nprint(1)\nprint(2)\n```\n\nafter"),
                  kimix::string("[code block: 3 lines]\n\nafter")))
            << "fence count = newlines inside the matched region";
        // Inline code protected from emphasis passes and restored.
        expect(eq(markdown_to_text("See `foo_bar` and _emphasis_ but not foo_bar baz."),
                  kimix::string("See foo_bar and emphasis but not foo_bar baz.")))
            << "underscore word-boundary rule keeps identifiers";
    };

    "markdown_links_images"_test = [] {
        // The reference applies the LINK pass before the IMAGE pass, so an
        // image with NON-empty alt text is consumed by the link regex and
        // rendered as "!alt (url)".  Only an EMPTY-alt image survives to the
        // image pass and becomes "[image: url]".  (Recorded deviation: the
        // plan's prose "images [image: url]" applies only to the empty-alt
        // case; the Python reference is authoritative.)
        expect(eq(markdown_to_text("[link](http://x.com) and ![img](http://img.png)"),
                  kimix::string("link (http://x.com) and !img (http://img.png)")))
            << "link pass runs first and consumes the image alt text too";
        expect(eq(markdown_to_text("![img](http://img.png)"),
                  kimix::string("!img (http://img.png)")))
            << "non-empty-alt image is taken by the link pass";
        expect(eq(markdown_to_text("![](http://img.png)"),
                  kimix::string("[image: http://img.png]")))
            << "empty-alt image reaches the image pass";
    };

    "markdown_hr_and_blank_collapse"_test = [] {
        expect(eq(markdown_to_text("---\n\ntext\n\n\n\nmore"),
                  kimix::string("text\n\nmore")));
        expect(eq(markdown_to_text("  \n\nx\n\n\n\n\ny\n\n  "),
                  kimix::string("x\n\ny")));
    };

    "markdown_empty"_test = [] {
        expect(eq(markdown_to_text(""), kimix::string("")));
        expect(eq(markdown_to_text("plain text"), kimix::string("plain text")));
    };

    // ── Read Tool class (CallableTool2-style binding) ────────────────────────

    auto deserialize_result = [](const kimix::vector<char> &buf) {
        kimix::builtin_tools::ToolParams result;
        result.deserialize(kimix::span<char const>(buf.data(), buf.size()));
        return result;
    };

    auto expect_status = [&](const kimix::vector<char> &buf,
                             kimix::string_view status) {
        const auto result = deserialize_result(buf);
        const ValueElement *st = result.get("status");
        expect(st != nullptr && st->is_string());
        if (st != nullptr && st->is_string()) {
            expect(eq(st->as_string(), kimix::string(status)));
        }
    };

    "read_tool_null_params"_test = [&] {
        Read tool(nullptr);
        tool(nullptr);
        expect(!tool.serialized_result().empty());
        expect_status(tool.serialized_result(), "invalid_input");
    };

    "read_tool_missing_content"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["display_path"] = ValueElement::make_string(k_of("x.txt"));
        tool(&params);
        expect_status(tool.serialized_result(), "invalid_input");
    };

    "read_tool_missing_display_path"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of("hello"));
        tool(&params);
        expect_status(tool.serialized_result(), "invalid_input");
    };

    "read_tool_validation_error"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of("a\nb\n"));
        params.values["display_path"] = ValueElement::make_string(k_of("x.txt"));
        params.values["offset"] = ValueElement::make_int(0);
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        const ValueElement *st = result.get("status");
        expect(st != nullptr && st->is_string());
        if (st != nullptr && st->is_string()) {
            expect(eq(st->as_string(), kimix::string("invalid_input")));
        }
        const ValueElement *msg = result.get("message");
        expect(msg != nullptr && msg->is_string());
        if (msg != nullptr && msg->is_string()) {
            expect(msg->as_string().find("offset cannot be 0") !=
                   kimix::string::npos);
        }
    };

    "read_tool_forward_text"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] =
            ValueElement::make_string(k_of("line1\nline2\nline3\n"));
        params.values["display_path"] = ValueElement::make_string(k_of("f.txt"));
        params.values["offset"] = ValueElement::make_int(1);
        params.values["limit"] = ValueElement::make_int(10);
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(eq(out->as_string(),
                      kimix::string("     1\tline1\n     2\tline2\n     3\tline3\n")));
        }
        const ValueElement *msg = result.get("message");
        expect(msg != nullptr && msg->is_string());
        if (msg != nullptr && msg->is_string()) {
            expect(msg->as_string().find("End of file reached") !=
                   kimix::string::npos);
        }
    };

    "read_tool_tail_text"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of("a\nb\nc\n"));
        params.values["display_path"] = ValueElement::make_string(k_of("t.txt"));
        params.values["offset"] = ValueElement::make_int(-2);
        params.values["limit"] = ValueElement::make_int(10);
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(eq(out->as_string(),
                      kimix::string("     2\tb\n     3\tc\n")));
        }
    };

    "read_tool_char_window"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] =
            ValueElement::make_string(k_of("0123456789ABCDEFGHIJ"));
        params.values["display_path"] = ValueElement::make_string(k_of("w.txt"));
        params.values["max_char"] = ValueElement::make_int(5);
        params.values["char_offset"] = ValueElement::make_int(3);
        params.values["show_line_numbers"] = ValueElement::make_bool(false);
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(eq(out->as_string(), kimix::string("34567")));
        }
        const ValueElement *msg = result.get("message");
        expect(msg != nullptr && msg->is_string());
        if (msg != nullptr && msg->is_string()) {
            expect(msg->as_string().find("output window shows middle chars") !=
                   kimix::string::npos);
        }
    };

    "read_tool_markdown_mode"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] =
            ValueElement::make_string(k_of("# Hello\n\n**bold**"));
        params.values["display_path"] = ValueElement::make_string(k_of("m.md"));
        params.values["mode"] = ValueElement::make_string(k_of("markdown"));
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(eq(out->as_string(), kimix::string("Hello\n\nbold")));
        }
    };

    "read_tool_cpu_profile_mode"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of(R"json({
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "work", "url": "a.js", "lineNumber": 3}}
            ],
            "samples": [2, 2],
            "timeDeltas": [100, 100],
            "startTime": 0,
            "endTime": 200
        })json"));
        params.values["display_path"] =
            ValueElement::make_string(k_of("profile.cpuprofile"));
        params.values["mode"] = ValueElement::make_string(k_of("cpu_profile"));
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(out->as_string().find("work (a.js:3)") !=
                   kimix::string::npos);
        }
    };

    "read_tool_sample_profile_mode"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of(
            "Analysis of sampling python3 1234 every 1 millisecond\n"
            "Call graph:\n"
            " 1 Thread_1\n"
            " + 1 main (in a.out) + 0 [0x1]\n"
            "\n"
            "Total number in stack (recursive count):\n"));
        params.values["display_path"] =
            ValueElement::make_string(k_of("sample.sample.txt"));
        params.values["mode"] =
            ValueElement::make_string(k_of("sample_profile"));
        tool(&params);
        const auto result = deserialize_result(tool.serialized_result());
        expect_status(tool.serialized_result(), "ok");
        const ValueElement *out = result.get("output");
        expect(out != nullptr && out->is_string());
        if (out != nullptr && out->is_string()) {
            expect(out->as_string().find("Top functions by self samples") !=
                   kimix::string::npos);
        }
    };

    "read_tool_invalid_profile_mode"_test = [&] {
        Read tool(nullptr);
        kimix::builtin_tools::ToolParams params;
        params.values["content"] = ValueElement::make_string(k_of("not a profile"));
        params.values["display_path"] =
            ValueElement::make_string(k_of("profile.cpuprofile"));
        params.values["mode"] = ValueElement::make_string(k_of("cpu_profile"));
        tool(&params);
        expect_status(tool.serialized_result(), "unsupported");
    };
}
