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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Byte-exact expectations generated from the kimi-agent Python reference by
// scripts/gen_read_goldens.py (run it after changing any kernel). The file is
// pure ASCII (octal escapes) so no BOM/encoding surprises on MSVC.
#include "read_goldens.inc"

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

std::string s_of(kimix::string_view s) { return std::string(s.data(), s.size()); }

kimix::string k_of(std::string_view s) { return kimix::string(s.data(), s.size()); }

// ── golden helpers ─────────────────────────────────────────────────────────

// Short, escape-visible preview so a mismatch reports which byte differs
// without dumping a 100 KiB golden into the test log.
std::string rd_preview(const std::string &s, size_t max = 200) {
    std::string out;
    for (const char ch : s) {
        const unsigned char b = static_cast<unsigned char>(ch);
        if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (b < 0x20 || b >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", b);
            out += buf;
        } else {
            out += ch;
        }
        if (out.size() >= max) {
            out += "...";
            break;
        }
    }
    return out;
}

// "got[...] want[...] (got N bytes, want M bytes)"
std::string rd_diff(const std::string &got, const std::string &want) {
    std::string out = " got[" + rd_preview(got) + "] want[" + rd_preview(want) + "]";
    if (got.size() != want.size()) {
        out += " (got " + std::to_string(got.size()) + " bytes, want " +
               std::to_string(want.size()) + ")";
    }
    return out;
}

// Split a '\x01'-joined golden field into lines (empty field == no lines).
kimix::vector<kimix::string> rd_split01(kimix::string_view joined) {
    kimix::vector<kimix::string> out;
    if (joined.empty()) {
        return out;
    }
    size_t pos = 0;
    while (true) {
        const size_t sep = joined.find('\x01', pos);
        if (sep == kimix::string_view::npos) {
            out.emplace_back(joined.data() + pos, joined.size() - pos);
            break;
        }
        out.emplace_back(joined.data() + pos, sep - pos);
        pos = sep + 1;
    }
    return out;
}

// Recipes used by the generator for corpora too large to store twice:
//   "repeat\x02N\x02LINE"        N copies of LINE
//   "numbered\x02N\x02PREFIX[\n]" PREFIX + str(i) [+ "\n"] for i in 1..N
kimix::vector<kimix::string> rd_expand_spec(kimix::string_view spec) {
    kimix::vector<kimix::string> out;
    const size_t s1 = spec.find('\x02');
    if (s1 == kimix::string_view::npos) {
        return out;
    }
    const size_t s2 = spec.find('\x02', s1 + 1);
    if (s2 == kimix::string_view::npos) {
        return out;
    }
    const kimix::string_view kind = spec.substr(0, s1);
    int64_t count = 0;
    for (size_t i = s1 + 1; i < s2; i++) {
        count = count * 10 + (spec[i] - '0');
    }
    const kimix::string_view arg = spec.substr(s2 + 1);
    if (kind == "repeat") {
        out.reserve(static_cast<size_t>(count));
        for (int64_t i = 0; i < count; i++) {
            out.emplace_back(arg.data(), arg.size());
        }
    } else if (kind == "numbered") {
        kimix::string_view prefix = arg;
        const bool with_newline = !arg.empty() && arg.back() == '\n';
        if (with_newline) {
            prefix = arg.substr(0, arg.size() - 1);
        }
        out.reserve(static_cast<size_t>(count));
        for (int64_t i = 1; i <= count; i++) {
            kimix::string line(prefix.data(), prefix.size());
            line += std::to_string(i).c_str();
            if (with_newline) {
                line.push_back('\n');
            }
            out.push_back(std::move(line));
        }
    }
    return out;
}

kimix::vector<kimix::string> rd_golden_lines(char const *literal,
                                             char const *spec,
                                             int64_t expected_count = -1) {
    kimix::vector<kimix::string> lines;
    if (spec != nullptr && spec[0] != '\0') {
        lines = rd_expand_spec(spec);
    } else {
        lines = rd_split01(literal);
    }
    // An empty '\x01'-joined field cannot distinguish "no lines" from "one
    // empty line"; the golden carries the expected count for that reason.
    if (expected_count == 1 && lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

uint64_t rd_total_bytes(const kimix::vector<kimix::string> &lines) {
    uint64_t total = 0;
    for (const auto &line : lines) {
        total += line.size();
    }
    return total;
}

std::string rd_join01(const kimix::vector<kimix::string> &lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i != 0) {
            out += '\x01';
        }
        out.append(lines[i].data(), lines[i].size());
    }
    return out;
}

std::string rd_ints_to_string(const kimix::vector<int64_t> &values) {
    std::string out;
    for (size_t i = 0; i < values.size(); i++) {
        if (i != 0) {
            out += ' ';
        }
        out += std::to_string(values[i]);
    }
    return out;
}

void rd_check_render(const rd_g_render &c, const render_result &r) {
    expect(eq(s_of(r.output), std::string(c.output)))
        << c.name << " output" << rd_diff(s_of(r.output), std::string(c.output));
    expect(eq(s_of(r.message), std::string(c.message)))
        << c.name << " message" << rd_diff(s_of(r.message), std::string(c.message));
    const std::string want_window =
        rd_join01(rd_golden_lines(c.window, c.window_spec, c.window_lines));
    expect(eq(r.window_lines.size(), static_cast<size_t>(c.window_lines)))
        << c.name << " window line count";
    expect(eq(rd_join01(r.window_lines), want_window))
        << c.name << " window"
        << rd_diff(rd_join01(r.window_lines), want_window);
    expect(eq(r.start_line, static_cast<int64_t>(c.start_line)))
        << c.name << " start_line";
    expect(eq(r.total_lines, static_cast<int64_t>(c.total_lines)))
        << c.name << " total_lines";
    expect(eq(r.max_lines_reached, c.max_lines_reached != 0)) << c.name << " max_lines";
    expect(eq(r.max_bytes_reached, c.max_bytes_reached != 0)) << c.name << " max_bytes";
    expect(eq(r.end_of_file, c.end_of_file != 0)) << c.name << " end_of_file";
    expect(eq(rd_ints_to_string(r.truncated_line_numbers), std::string(c.truncated)))
        << c.name << " truncated " << rd_ints_to_string(r.truncated_line_numbers)
        << " want " << c.truncated;
}

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

    // ═══════════════════════════════════════════════════════════════════════
    // Differential goldens (scripts/gen_read_goldens.py)
    //
    // Every expectation below was produced by running the *real* Python
    // implementation in the kimi-agent checkout, not by reading this code.
    // ═══════════════════════════════════════════════════════════════════════

    "golden_validate_int_option"_test = [] {
        for (const auto &c : rd_validate_goldens) {
            const tool_error e = validate_int_option(c.name, c.value);
            expect(eq(!e.failed(), c.ok != 0))
                << "validate " << c.name << "=" << c.value;
            if (e.failed()) {
                expect(eq(s_of(e.message), std::string(c.message)))
                    << "validate " << c.name << "=" << c.value
                    << rd_diff(s_of(e.message), std::string(c.message));
            } else {
                expect(e.status == tool_status::ok) << "validate ok status";
            }
        }
    };

    "golden_truncate_line"_test = [] {
        for (const auto &c : rd_truncate_goldens) {
            kimix::string out;
            truncate_line_read(c.text, c.max_len, out);
            expect(eq(s_of(out), std::string(c.expected)))
                << "truncate " << c.name << " max_len=" << c.max_len
                << rd_diff(s_of(out), std::string(c.expected));
        }
    };

    "golden_split_lines"_test = [] {
        for (const auto &c : rd_split_goldens) {
            const auto lines = split_lines(c.input);
            expect(eq(rd_join01(lines), std::string(c.expected)))
                << "split " << c.name
                << rd_diff(rd_join01(lines), std::string(c.expected));
        }
    };

    "golden_render_forward"_test = [] {
        for (const auto &c : rd_forward_goldens) {
            const auto lines = rd_golden_lines(c.lines, c.lines_spec, c.input_lines);
            expect(eq(rd_total_bytes(lines), static_cast<uint64_t>(c.input_len)))
                << "render input recipe " << c.name;
            expect(eq(lines.size(), static_cast<size_t>(c.input_lines)))
                << "render input line count " << c.name;
            const render_result r =
                render_forward(lines, c.display_path, c.offset, c.n_lines,
                               c.show_line_numbers != 0, c.note);
            rd_check_render(c, r);
        }
    };

    "golden_render_tail"_test = [] {
        for (const auto &c : rd_tail_goldens) {
            const auto lines = rd_golden_lines(c.lines, c.lines_spec, c.input_lines);
            expect(eq(rd_total_bytes(lines), static_cast<uint64_t>(c.input_len)))
                << "render input recipe " << c.name;
            expect(eq(lines.size(), static_cast<size_t>(c.input_lines)))
                << "render input line count " << c.name;
            const render_result r =
                render_tail(lines, c.display_path, c.offset, c.n_lines,
                            c.show_line_numbers != 0, c.note);
            rd_check_render(c, r);
        }
    };

    "golden_apply_char_window"_test = [] {
        for (const auto &c : rd_charwin_goldens) {
            const char_window w =
                apply_char_window(c.output, c.char_offset, c.max_char);
            expect(eq(s_of(w.output), std::string(c.expected_output)))
                << "charwin " << c.name << " offset=" << c.char_offset
                << " max_char=" << c.max_char
                << rd_diff(s_of(w.output), std::string(c.expected_output));
            expect(eq(s_of(w.note), std::string(c.expected_note)))
                << "charwin note " << c.name
                << rd_diff(s_of(w.note), std::string(c.expected_note));
        }
    };

    "golden_line_hashes"_test = [] {
        for (const auto &c : rd_hash_goldens) {
            const auto got = compute_line_hash_strings(c.input);
            std::string joined;
            for (size_t i = 0; i < got.size(); i++) {
                if (i != 0) {
                    joined += ' ';
                }
                joined += s_of(got[i]);
            }
            expect(eq(joined, std::string(c.expected)))
                << "hashes " << c.name << rd_diff(joined, std::string(c.expected));
        }
    };

    "golden_cpu_profiles"_test = [] {
        for (const auto &c : rd_cpu_goldens) {
            kimix::string out;
            const bool ok = render_cpu_profile(c.input, out);
            expect(eq(ok, c.ok != 0)) << "cpu " << c.name << " ok";
            if (ok) {
                expect(eq(s_of(out), std::string(c.expected)))
                    << "cpu " << c.name << rd_diff(s_of(out), std::string(c.expected));
            }
        }
    };

    "golden_cpu_python_raises"_test = [] {
        // Shapes for which the *Python* reference raises AttributeError
        // (read_profiles.py:213 reads `n.hitCount` on a dict) while the C++
        // kernel implements the intended `n.get("hitCount", 0)` behavior. The
        // expected summary for these inputs is the rd_cpu_goldens row with the
        // same name (generated from a source-patched reference).
        for (const auto &c : rd_cpu_python_raises) {
            expect(std::string(c.exception_text).find("AttributeError") !=
                   std::string::npos)
                << "documented Python crash " << c.name;
            kimix::string out;
            expect(render_cpu_profile(c.input, out)) << "cpu intended " << c.name;
        }
    };

    "golden_sample_profiles"_test = [] {
        for (const auto &c : rd_sample_goldens) {
            kimix::string out;
            const bool ok = render_sample_profile(c.input, out);
            expect(eq(ok, c.ok != 0)) << "sample " << c.name << " ok";
            if (ok) {
                expect(eq(s_of(out), std::string(c.expected)))
                    << "sample " << c.name
                    << rd_diff(s_of(out), std::string(c.expected));
            }
        }
    };

    "golden_markdown_to_text"_test = [] {
        for (const auto &c : rd_markdown_goldens) {
            const kimix::string got = markdown_to_text(c.input);
            expect(eq(s_of(got), std::string(c.expected)))
                << "markdown " << c.name << " input[" << rd_preview(c.input, 60)
                << "]" << rd_diff(s_of(got), std::string(c.expected));
        }
    };

    "golden_tool_text_path"_test = [&] {
        for (const auto &c : rd_tool_goldens) {
            Read tool(nullptr);
            kimix::builtin_tools::ToolParams params;
            params.values["content"] = ValueElement::make_string(
                kimix::string(c.content, std::strlen(c.content)));
            params.values["display_path"] =
                ValueElement::make_string(k_of("sample.txt"));
            params.values["offset"] = ValueElement::make_int(c.offset);
            params.values["limit"] = ValueElement::make_int(c.limit);
            params.values["max_char"] = ValueElement::make_int(c.max_char);
            params.values["char_offset"] = ValueElement::make_int(c.char_offset);
            params.values["show_line_numbers"] =
                ValueElement::make_bool(c.show_line_numbers != 0);
            tool(&params);
            const auto result = deserialize_result(tool.serialized_result());
            expect_status(tool.serialized_result(), "ok");
            const ValueElement *out = result.get("output");
            expect(out != nullptr && out->is_string()) << "tool " << c.name;
            if (out != nullptr && out->is_string()) {
                expect(eq(s_of(out->as_string()), std::string(c.output)))
                    << "tool " << c.name
                    << rd_diff(s_of(out->as_string()), std::string(c.output));
            }
            const ValueElement *msg = result.get("message");
            expect(msg != nullptr && msg->is_string()) << "tool " << c.name;
            if (msg != nullptr && msg->is_string()) {
                expect(eq(s_of(msg->as_string()), std::string(c.message)))
                    << "tool " << c.name
                    << rd_diff(s_of(msg->as_string()), std::string(c.message));
            }
            auto expect_int = [&](const char *field, int64_t want) {
                const ValueElement *el = result.get(field);
                expect(el != nullptr && el->is_int()) << "tool " << c.name << field;
                if (el != nullptr && el->is_int()) {
                    expect(eq(el->as_int(), want))
                        << "tool " << c.name << " " << field << " got "
                        << el->as_int() << " want " << want;
                }
            };
            expect_int("start_line", c.start_line);
            expect_int("total_lines", c.total_lines);
            auto expect_bool = [&](const char *field, bool want) {
                const ValueElement *el = result.get(field);
                expect(el != nullptr && el->is_bool())
                    << "tool " << c.name << field;
                if (el != nullptr && el->is_bool()) {
                    expect(eq(el->as_bool(), want))
                        << "tool " << c.name << " " << field;
                }
            };
            expect_bool("max_lines_reached", c.max_lines_reached != 0);
            expect_bool("max_bytes_reached", c.max_bytes_reached != 0);
            expect_bool("end_of_file", c.end_of_file != 0);
            const ValueElement *trunc = result.get("truncated_line_numbers");
            expect(trunc != nullptr && trunc->is_array())
                << "tool " << c.name << " truncated_line_numbers";
            if (trunc != nullptr && trunc->is_array()) {
                std::string joined;
                const auto &arr = trunc->as_array();
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i != 0) {
                        joined += ' ';
                    }
                    joined += std::to_string(arr[i].as_int());
                }
                expect(eq(joined, std::string(c.truncated)))
                    << "tool " << c.name << " truncated got[" << joined
                    << "] want[" << c.truncated << "]";
            }
        }
    };
}
