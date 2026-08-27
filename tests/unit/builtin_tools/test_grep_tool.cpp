// Test for builtin_tools/grep_tool.h + .cpp (kimix::builtin_tools::grep).
//
// Covers the plan's §7 test list for grep (plans/grep.md) with golden vectors
// harvested from the Python reference modules under
// C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/ (grep_selectors.py,
// grep_archive.py, grep_output.py, grep_recorder.py, output_utils.py,
// grep_local.py) and utils/sensitive.py — see src/builtin_tools/reports/grep.md
// for the function-by-function mapping.
//
//   selectors      chunk/range grammar, exact ValueError texts, merge and
//                  open-ended absorption, split_path_and_sel (drive / scheme /
//                  literal-probe guards), expand_path_entries (JSON / ';'),
//                  merge_ranges_into, entries_are_rich
//   archive        parse_archive_path_candidates (rightmost-first, nested),
//                  is_archive_path table, safe_scratch_name, remap_display,
//                  strip_key_for
//   content line   parse_content_line (match/context/separator/malformed,
//                  paths with digits and colons), line_path_shape (_RG_LINE_RE)
//   rendering      format_match_line, group_lines_by_file, format_grouped_output,
//                  group_line_indices_by_blank, should_group, range_filter_lines
//                  (out-of-range drop + orphan "--" pruning),
//                  reattach_single_file_prefix, strip_path_prefix,
//                  normalize_slashes_content, collect_record_files
//   rtk protocol   parse_rtk_rg_output (header + blank drop, per-file fold,
//                  files fold, tail-hint parse, passthrough tolerance) and
//                  rtk_fold_note message text
//   recorder       insertion-ordered dedup + cap with front-drop
//   sensitive      .env / .env.example exemption / id_rsa / .aws/credentials on
//                  both pathlib flavours + the warning message text
//   pattern        pattern_has_regex_newline / multiline_pattern (odd-backslash)
//   join           join_with_byte_limit byte budget + invalid-UTF-8 bail-out
//   ASCII gate     unsupported statuses for non-ASCII input
//
// Every vector below was produced by running the Python reference (see
// reports/grep.md "Golden vectors").

#include "ut/ut.hpp"

#include "builtin_tools/grep_tool.h"
#include "builtin_tools/tool_types.h"
#include "builtin_tools/utf8_util.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
namespace g = kimix::builtin_tools::grep;

namespace {

// Local conversion helpers (test-local names, unity-friendly).
kimix::vector<kimix::string> kv(std::vector<std::string> in) {
    kimix::vector<kimix::string> out;
    out.reserve(in.size());
    for (auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

std::vector<std::string> sv(kimix::span<const kimix::string> in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

std::string s_of(const kimix::string &s) { return std::string(s.data(), s.size()); }

void assign_str(kimix::string &out, std::string_view v) { out.assign(v.data(), v.size()); }

std::string ranges_text(kimix::span<const line_range> ranges) {
    std::string out;
    for (const line_range &r : ranges) {
        if (!out.empty()) {
            out += "|";
        }
        out += std::to_string(r.start_line);
        out += "-";
        out += r.end_line.has_value() ? std::to_string(*r.end_line) : "open";
    }
    return out;
}

line_range range_of(uint32_t start, uint32_t end, bool open) {
    return open ? line_range{start, kimix::optional<uint32_t>{}} : line_range{start, end};
}

bool probe_false(kimix::string_view) noexcept { return false; }

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ---------------------------------------------------------------- selectors
    "parse_line_range_chunk_grammar"_test = [] {
        struct Case {
            std::string in;
            std::string want; // "none" | "err:..." | "start-end"
        };
        const std::vector<Case> cases = {
            {"50-100", "50-100"},
            {"50+10", "50-59"},
            {"301-", "301-open"},
            {"301", "301-open"},
            {"1-1", "1-1"},
            {"L42", "42-open"},
            {"L42-L50", "42-50"},
            {"42..100", "42-100"},
            {"42..", "42-open"},
            {"l42-L50", "42-50"},
            {" 50-100 ", "50-100"},
            {"50+", "50-open"},
            {"5-0007", "5-7"},
            {"5+0003", "5-7"},
            {"1000000-", "1000000-open"},
            {"garbage", "none"},
            {"", "none"},
            {"  ", "none"},
            {"5-L", "none"},
            {"-5", "none"},
            {"5..4", "err:Invalid range 5-4: end must be >= start."},
            {"0-5", "err:Line selector 0 is invalid; lines are 1-indexed. Use :1."},
            {"00", "err:Line selector 0 is invalid; lines are 1-indexed. Use :1."},
            {"L0-L3", "err:Line selector 0 is invalid; lines are 1-indexed. Use :1."},
            {"50-40", "err:Invalid range 50-40: end must be >= start."},
            {"50+0", "err:Invalid range 50+0: count must be >= 1."},
        };
        for (const Case &c : cases) {
            g::selector_result r = g::parse_line_range_chunk(c.in);
            if (c.want.rfind("err:", 0) == 0) {
                expect(((g::tool_status::invalid_input) == (r.status))) << c.in;
                expect(((std::string("err:") + s_of(r.message)) == (c.want))) << c.in;
                continue;
            }
            if (c.want == "none") {
                expect(r.status == g::tool_status::ok && r.no_value) << c.in;
                continue;
            }
            expect(r.status == g::tool_status::ok && !r.no_value) << c.in;
            expect(((ranges_text({&r.range, 1})) == (c.want))) << c.in;
        }
    };

    "parse_line_range_chunk_rejects_unicode_digits"_test = [] {
        // \d is Nd in Python: "١٠" (Arabic-Indic) would parse there.
        g::selector_result r = g::parse_line_range_chunk("\xD9\xA1\xD9\xA0");
        expect(((g::tool_status::unsupported) == (r.status)));
    };

    "parse_line_ranges_merge_and_absorb"_test = [] {
        struct Case {
            std::string in;
            std::string want; // "" == Python None
        };
        const std::vector<Case> cases = {
            {"5-16,960-973", "5-16|960-973"},
            {"1-3,3-5", "1-5"},
            {"1-3,4-6", "1-6"},
            {"10-,20-30", "10-open"},
            {"20-30,1-5", "1-5|20-30"},
            {"raw", ""},
            {"", ""},
            {" 1-3 , 5-6 ", "1-3|5-6"},
            {"5,5", "5-open"},
            {"5:raw", ""}, // ':' is not a range-list separator
            {"a,5", "5-open"},
            {"1-2,3-", "1-open"},
            {"9-9,1-1,12-14", "1-1|9-9|12-14"},
            {"1-3,L4-L6,7-", "1-open"}, // Python merges the adjacent open-ended tail
        };
        for (const Case &c : cases) {
            g::selector_result r = g::parse_line_ranges(c.in);
            expect(r.status == g::tool_status::ok) << c.in;
            if (c.want.empty()) {
                expect(r.no_value) << c.in;
                expect(r.ranges.empty()) << c.in;
                continue;
            }
            expect(!r.no_value) << c.in;
            expect(((ranges_text(r.ranges)) == (c.want))) << c.in;
        }
        g::selector_result bad = g::parse_line_ranges("50-40");
        expect(((g::tool_status::invalid_input) == (bad.status)));
        expect(((s_of(bad.message)) == (std::string("Invalid range 50-40: end must be >= start."))));
    };

    "is_line_in_ranges"_test = [] {
        const kimix::vector<line_range> ranges{range_of(5, 16, false), range_of(960, 973, false)};
        const kimix::span<const line_range> view{ranges};
        expect(!g::is_line_in_ranges(4, view));
        expect(g::is_line_in_ranges(5, view));
        expect(g::is_line_in_ranges(16, view));
        expect(!g::is_line_in_ranges(17, view));
        expect(!g::is_line_in_ranges(959, view));
        expect(g::is_line_in_ranges(960, view));
        expect(g::is_line_in_ranges(973, view));
        // None == unfiltered
        expect(g::is_line_in_ranges(12345, {}));
        const kimix::vector<line_range> open{range_of(301, 0, true)};
        expect(g::is_line_in_ranges(10'000'000, open));
        expect(!g::is_line_in_ranges(300, open));
    };

    "selector_line_ranges"_test = [] {
        struct Case {
            std::string in;
            std::string want;
        };
        const std::vector<Case> cases = {
            {"", ""},
            {"raw", ""},
            {"conflicts", ""},
            {"RAW", ""},
            {"raw:50-100", "50-100"},
            {"50-100:raw", "50-100"},
            {"1-5", "1-5"},
            {"5..", "5-open"},
            {"conflicts:1-2,3-4", "1-4"},
            {"junk:5-6", "5-6"},
        };
        for (const Case &c : cases) {
            g::selector_result r = g::selector_line_ranges(c.in);
            expect(r.status == g::tool_status::ok) << c.in;
            if (c.want.empty()) {
                expect(r.no_value) << c.in;
            } else {
                expect(((ranges_text(r.ranges)) == (c.want))) << c.in;
            }
        }
        g::selector_result bad = g::selector_line_ranges("50-40");
        expect(((g::tool_status::invalid_input) == (bad.status)));
        expect(((s_of(bad.message)) == (std::string("Invalid range 50-40: end must be >= start."))));
    };

    "split_path_and_sel_grammar"_test = [] {
        struct Case {
            std::string in;
            std::string want_path;
            std::string want_sel; // "" == None
        };
        const std::vector<Case> cases = {
            {"src/app.py", "src/app.py", ""},
            {"src/app.py:50-100", "src/app.py", "50-100"},
            {"a/b.py:5-16,960-973", "a/b.py", "5-16,960-973"},
            {"a/b.py:1-50:raw", "a/b.py", "1-50:raw"},
            {"a/b.py:raw:1-50", "a/b.py", "raw:1-50"},
            {"bundle.zip:src/foo.ts", "bundle.zip:src/foo.ts", ""}, // member, not selector
            {"C:\\dir\\f.txt:50-100", "C:\\dir\\f.txt", "50-100"},
            {"C:", "C:", ""}, // bare drive is never peeled
            {"C:/x:5", "C:/x", "5"},
            {"ssh://h:2222", "ssh://h:2222", ""}, // scheme://authority port
            {"ssh://h/f:1-5", "ssh://h/f", "1-5"},
            {"src/foo.py:hello world", "src/foo.py:hello world", ""},
            {"", "", ""},
            {":1-2", ":1-2", ""}, // idx <= 0 guard
            {"a.py:L5-L6", "a.py", "L5-L6"},
            {"a.py:5:", "a.py:5:", ""}, // trailing empty chunk: shape rejects
            {"x:1-2,3-4:raw", "x", "1-2,3-4:raw"},
            {"file.txt:1-", "file.txt", "1-"},
            {"file.txt:+5", "file.txt:+5", ""}, // "+5" has no leading digits
            {"//srv/share/f:5", "//srv/share/f", "5"},
            {"file.txt:5-6:7-8", "file.txt", "5-6:7-8"}, // two chunks peel, rejoined
        };
        for (const Case &c : cases) {
            g::path_selector ps;
            expect(g::split_path_and_sel(c.in, probe_false, ps) == tool_status::ok) << c.in;
            expect(((s_of(ps.path)) == (c.want_path))) << c.in;
            expect(((s_of(ps.selector)) == (c.want_sel))) << c.in;
            expect(ps.has_selector == !c.want_sel.empty()) << c.in;
        }
    };

    "split_path_and_sel_literal_probe_wins"_test = [] {
        const auto probe = [](kimix::string_view raw) noexcept {
            return raw == "test:1-2" || raw == "src/app.py:50-100";
        };
        g::path_selector a;
        expect(g::split_path_and_sel("test:1-2", probe, a) == tool_status::ok);
        expect(((s_of(a.path)) == (std::string("test:1-2"))));
        expect(!a.has_selector);

        g::path_selector b;
        expect(g::split_path_and_sel("src/app.py:50-100", probe, b) == tool_status::ok);
        expect(!b.has_selector);

        // The probe is only consulted for the raw string, never for a peeled head.
        g::path_selector c;
        expect(g::split_path_and_sel("a/b.py:1-50:raw", probe, c) == tool_status::ok);
        expect(((s_of(c.path)) == (std::string("a/b.py"))));
        expect(c.has_selector);

        // probe callback type is the injected kimix::function
        const kimix::function<bool(kimix::string_view)> fn = probe;
        g::path_selector d;
        expect(g::split_path_and_sel("x.py:3", fn, d) == tool_status::ok);
        expect(((s_of(d.path)) == (std::string("x.py"))));

        g::path_selector e;
        expect(g::split_path_and_sel("C:\\x", fn, e) == tool_status::ok);
        expect(((s_of(e.path)) == (std::string("C:\\x"))));
    };

    "split_path_and_sel_rejects_non_ascii"_test = [] {
        g::path_selector ps;
        expect(((g::split_path_and_sel("caf\xC3\xA9.py:1-2", probe_false, ps)) == (tool_status::unsupported)));
    };

    "expand_path_entries"_test = [] {
        struct Case {
            std::string in;
            std::vector<std::string> want;
        };
        const std::vector<Case> cases = {
            {"src; tests", {"src", "tests"}},
            {"src; ;tests", {"src", "tests"}},
            {"  src; ;tests ", {"src", "tests"}},
            {"a;;b", {"a", "b"}},
            {"single.py", {"single.py"}},
            {"", {}},
            {";", {}},
            {"src/a.py:1-2,3-4", {"src/a.py:1-2,3-4"}}, // commas never split
            {R"(["a.py", "b.py"])", {"a.py", "b.py"}},
            {R"(["a.py", "a.py", "b.py"])", {"a.py", "b.py"}},
            {R"([])", {}},
            {"[1, 2]", {"[1, 2]"}},                    // non-string items -> ';' path
            {R"(["a", 1])", {R"(["a", 1])"}},          // idem
            {"[1, 2]; x", {"[1, 2]", "x"}},            // idem, semicolon split kept
            {R"(["a"].)", {R"(["a"].)"}},              // trailing content -> not JSON
            {"[,]", {"[,]"}},                          // malformed
            {R"([" a ", "a"])", {"a"}},                // strip + dedupe
            {R"(["aA"])", {"aA"}},
            {R"([
x])", {"[\nx]"}}, // strict JSON with a newline is valid
        };
        for (const Case &c : cases) {
            kimix::vector<kimix::string> out;
            expect(g::expand_path_entries(c.in, out) == tool_status::ok) << c.in;
            expect(((sv(out)) == (c.want))) << c.in;
        }
        // list form
        kimix::vector<kimix::string> out;
        expect(g::expand_path_entries(kv({"a.py", "b.py"}), out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py", "b.py"})));
        expect(g::expand_path_entries(kv({"a.py", " a.py ", "b.py", "  "}), out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py", "b.py"})));
    };

    "merge_ranges_into_and_find"_test = [] {
        g::ranges_map map;
        const kimix::vector<line_range> first{range_of(1, 5, false)};
        const kimix::vector<line_range> second{range_of(10, 20, false)};
        g::merge_ranges_into(map, "/a.py", first);
        g::merge_ranges_into(map, "/a.py", second);
        g::merge_ranges_into(map, "/a.py", {}); // Python's `if not ranges: return`
        expect(((map.size()) == (size_t(1))));
        expect(((s_of(map[0].path)) == (std::string("/a.py"))));
        expect(((ranges_text(map[0].ranges)) == (std::string("1-5|10-20"))));
        expect(g::ranges_map_find(map, "/a.py") != nullptr);
        expect(g::ranges_map_find(map, "/b.py") == nullptr);
        g::merge_ranges_into(map, "/b.py", first);
        expect(((map.size()) == (size_t(2))));
    };

    "entries_are_rich"_test = [] {
        kimix::vector<kimix::string> multi{kimix::string("a.py"), kimix::string("b.py")};
        expect(g::entries_are_rich(multi, probe_false));
        kimix::vector<kimix::string> plain{kimix::string("src/app.py")};
        expect(!g::entries_are_rich(plain, probe_false));
        kimix::vector<kimix::string> sel{kimix::string("src/app.py:1-5")};
        expect(g::entries_are_rich(sel, probe_false));
        kimix::vector<kimix::string> arch{kimix::string("bundle.zip:src/foo.ts")};
        expect(g::entries_are_rich(arch, probe_false));
    };

    // ------------------------------------------------------------------ archive
    "archive_extensions_table"_test = [] {
        expect(g::is_archive_path("a.zip"));
        expect(g::is_archive_path("A.TGZ")); // case-insensitive
        expect(g::is_archive_path("a.tar.gz"));
        expect(g::is_archive_path("a.gz"));
        expect(g::is_archive_path("a.tar.xz"));
        expect(g::is_archive_path("a.apk"));
        expect(g::is_archive_path("a.ZST"));
        expect(g::is_archive_path(".zip"));
        expect(!g::is_archive_path("a.py"));
        expect(!g::is_archive_path("zip"));
        expect(!g::is_archive_path(""));
        expect(((g::archive_extensions().size()) == (size_t(23))));
    };

    "parse_archive_path_candidates"_test = [] {
        kimix::vector<g::archive_candidate> out;
        g::parse_archive_path_candidates("bundle.zip:src/foo.ts", out);
        expect(((out.size()) == (size_t(1))));
        expect(((s_of(out[0].archive)) == (std::string("bundle.zip"))));
        expect(((s_of(out[0].member)) == (std::string("src/foo.ts"))));

        // rightmost-first: the innermost member wins, then the outer layer
        g::parse_archive_path_candidates("a.zip:b.zip:c.ts", out);
        expect(((out.size()) == (size_t(2))));
        expect(((s_of(out[0].archive)) == (std::string("a.zip:b.zip"))));
        expect(((s_of(out[0].member)) == (std::string("c.ts"))));
        expect(((s_of(out[1].archive)) == (std::string("a.zip"))));
        expect(((s_of(out[1].member)) == (std::string("b.zip"))));

        g::parse_archive_path_candidates("/tmp/o.zip:a.zip:inner.txt", out);
        expect(((out.size()) == (size_t(2))));
        expect(((s_of(out[0].archive)) == (std::string("/tmp/o.zip:a.zip"))));
        expect(((s_of(out[1].member)) == (std::string("a.zip"))));

        g::parse_archive_path_candidates("x.tar.gz:inner/file.py", out);
        expect(((out.size()) == (size_t(1))));
        expect(((s_of(out[0].archive)) == (std::string("x.tar.gz"))));

        g::parse_archive_path_candidates("A.TGZ:b", out);
        expect(((out.size()) == (size_t(1))));
        expect(((s_of(out[0].archive)) == (std::string("A.TGZ"))));

        // non-archive left side stops the scan; empty member is rejected
        g::parse_archive_path_candidates("notanarchive:src/foo.ts", out);
        expect(out.empty());
        g::parse_archive_path_candidates("bundle.zip:", out);
        expect(out.empty());
        g::parse_archive_path_candidates("bundle.zip", out);
        expect(out.empty());
        g::parse_archive_path_candidates("zip:a", out);
        expect(out.empty());
    };

    "safe_scratch_name"_test = [] {
        struct Case {
            std::string in;
            std::string want;
        };
        const std::vector<Case> cases = {
            {"src/foo.ts", "foo.ts"},
            {"a/b/c.TXT", "c.TXT"},
            {"\\\\win\\path\\x.md", "x.md"},
            {"weird name (1).txt", "weird_name_1_.txt"},
            {"x y", "x_y"},
            {"___", "___"},
            {"a-b_c.d", "a-b_c.d"},
            {"***", "_"},
            {"src/../evil.txt", "evil.txt"},
            {"", "member"},
            {"/", "member"},
            {"a/", "member"},
        };
        for (const Case &c : cases) {
            kimix::string out;
            expect(g::safe_scratch_name(c.in, out) == tool_status::ok) << c.in;
            expect(((s_of(out)) == (c.want))) << c.in;
        }
        kimix::string out;
        expect(((g::safe_scratch_name("\xC3\xA9.txt", out)) == (tool_status::unsupported)));
    };

    "remap_display"_test = [] {
        g::display_map map;
        map.push_back(g::display_entry{kimix::string("C:\\tmp\\0-foo.ts"),
                                       kimix::string("bundle.zip:src/foo.ts")});
        kimix::vector<kimix::string> lines = kv({"C:\\tmp\\0-foo.ts:3:x", "untouched\\path"});
        kimix::vector<kimix::string> out;
        g::remap_display(lines, map, out);
        expect(((sv(out)) == (std::vector<std::string>{"bundle.zip:src/foo.ts:3:x", "untouched\\path"})));

        // empty map == Python's early return (verbatim copy)
        g::remap_display(lines, {}, out);
        expect(((sv(out)) == (sv(lines))));
    };

    "strip_key_for"_test = [] {
        struct Case {
            std::string in;
            std::string want;
        };
        const std::vector<Case> cases = {
            {"C:\\w\\a.py", "a.py"},
            {"C:/w/", ""},
            {"C:/w", "C:/w"},
            {"D:/x", "D:/x"},
        };
        for (const Case &c : cases) {
            kimix::string out;
            g::strip_key_for(c.in, "C:/w", out);
            expect(((s_of(out)) == (c.want))) << c.in;
        }
    };

    // -------------------------------------------------------------- content line
    "parse_content_line_goldens"_test = [] {
        struct Case {
            std::string in;
            std::string want; // "path|line|text|match" | "none"
        };
        const std::vector<Case> cases = {
            {"a:12:x", "a|12|x|1"},
            {"a-12-x", "a|12|x|0"},
            {"--", "none"},
            {"a:12", "none"},
            {":12:x", "none"}, // empty path
            {"a:12:", "a|12||1"},
            {"a:012:x", "a|12|x|1"},
            {"a:1:2:3", "a|1|2:3|1"},   // leftmost delimiter pair wins
            {"x:y:12:z", "x:y|12|z|1"}, // colons inside the path
            {"a:1-x", "none"},          // mismatched delimiters
            {"a-1-2:x", "a|1|2:x|0"},
            {"a--1--", "a-|1|-|0"},
            {"README.md", "none"},
            {"a:1:", "a|1||1"},
            {"a:0:x", "a|0|x|1"},
            {"a-0007-x", "a|7|x|0"},
            {"", "none"},
            {"a:12:x\ny", "a|12|x\ny|1"}, // re.DOTALL keeps the newline in text
            {"C:\\w\\a.py-3-ctx text", "C:\\w\\a.py|3|ctx text|0"},
        };
        for (const Case &c : cases) {
            g::content_line cl;
            bool no_match = false;
            expect(g::parse_content_line(c.in, cl, no_match) == tool_status::ok) << c.in;
            if (c.want == "none") {
                expect(no_match) << c.in;
                continue;
            }
            expect(!no_match) << c.in;
            const std::string got = s_of(cl.path) + "|" + std::to_string(cl.line_no) + "|" +
                                    s_of(cl.text) + "|" + (cl.is_match ? "1" : "0");
            expect(((got) == (c.want))) << c.in;
        }
      // Non-ASCII inside the (unconstrained) path or text is still native:
      // Python's `.*?` accepts it, so nothing hinges on the digit class there.
      g::content_line cl;
      bool no_match = false;
      expect(((g::parse_content_line("caf\xC3\xA9:1:x", cl, no_match)) == (tool_status::ok)));
      expect(!no_match);
      expect(((s_of(cl.path)) == (std::string("caf\xC3\xA9"))));
      expect(((cl.line_no) == (1u)));
      expect(((s_of(cl.text)) == (std::string("x"))));
      expect(((g::parse_content_line("\xD9\xA1:1:x", cl, no_match)) == (tool_status::ok)));
      expect(((s_of(cl.path)) == (std::string("\xD9\xA1"))));
      // A Unicode digit where Python's \d keeps matching but the ASCII scan
      // stops: the answers differ, so the shim must use its Python mirror.
      expect(((g::parse_content_line("a:\xD9\xA1:x", cl, no_match)) == (tool_status::unsupported)));
    };

    "line_path_shape_matches_rg_line_re"_test = [] {
        struct Case {
            std::string in;
            long want; // -1 == no match
        };
        const std::vector<Case> cases = {
            {"a:12:x", 1},
            {"a-12-x", 1},
            {"--", -1},
            {":12:x", 0}, // empty group 1 still matches (Python caller tests it)
            {"x:y:12:z", 3},
            {"a--1--", 2},
        {"README.md", -1},
        {"a:12", -1},
        {"a\xD9\xA1:1:x", 3}, // non-ASCII in the path is still an exact match
    };
    for (const Case &c : cases) {
        size_t len = 12345;
        bool no_match = false;
        expect(g::line_path_shape(c.in, len, no_match) == tool_status::ok) << c.in;
        expect((no_match ? -1L : static_cast<long>(len)) == c.want) << c.in;
    }
    size_t len = 0;
    bool no_match = false;
    // non-DOTALL: a newline before the delimiter kills the match
    expect(g::line_path_shape("abc\n:12:x", len, no_match) == tool_status::ok);
    expect(no_match);
    // a Unicode digit right after the delimiter: unknowable natively
    expect(g::line_path_shape("a:\xD9\xA1:x", len, no_match) == tool_status::unsupported);
};

    // ---------------------------------------------------------------- rendering
    "format_match_line"_test = [] {
        kimix::string out;
        g::format_match_line(12, "hello", true, out);
        expect(((s_of(out)) == (std::string("*12|hello"))));
        g::format_match_line(12, "hello", false, out);
        expect(((s_of(out)) == (std::string(" 12|hello"))));
        g::format_match_line(1, "", false, out); // no padding, empty text
        expect(((s_of(out)) == (std::string(" 1|"))));
    };

    "group_and_render_output"_test = [] {
        kimix::vector<kimix::string> lines =
            kv({"a.py:3:text", "a.py-4-ctx", "--", "b.py:9:x", "gap marker", "b.py:12:y"});
        kimix::vector<g::file_group> groups;
        expect(g::group_lines_by_file(lines, groups) == tool_status::ok);
        expect(((groups.size()) == (size_t(2))));
        expect(((s_of(groups[0].path)) == (std::string("a.py"))));
        expect(((groups[0].body.size()) == (size_t(3))));
        expect(((groups[0].body[0].line_no) == (3u)));
        expect(groups[0].body[0].is_match);
        expect(((groups[0].body[1].line_no) == (4u)));
        expect(!groups[0].body[1].is_match);
        expect(((groups[0].body[2].line_no) == (0u))); // separator sentinel
        expect(((s_of(groups[0].body[2].text)) == (std::string("--"))));
        expect(((s_of(groups[1].path)) == (std::string("b.py"))));
        expect(((groups[1].body.size()) == (size_t(3))));
        expect(((s_of(groups[1].body[1].text)) == (std::string("gap marker"))));

        kimix::vector<kimix::string> rendered;
        g::format_grouped_output(groups, rendered);
        expect(((sv(rendered)) == (std::vector<std::string>{"# a.py", "*3|text", " 4|ctx", "--", "",
                                                         "# b.py", "*9|x", "gap marker", "*12|y"})));

        // leading non-content lines are dropped, blank line only between groups
        kimix::vector<kimix::string> l2 = kv({"preamble", "a:1:x", "b-2-y", "sep", "b:3:z"});
        kimix::vector<g::file_group> g2;
        expect(g::group_lines_by_file(l2, g2) == tool_status::ok);
        kimix::vector<kimix::string> r2;
        g::format_grouped_output(g2, r2);
        expect(((sv(r2)) == (std::vector<std::string>{"# a", "*1|x", "", "# b", " 2|y", "sep", "*3|z"})));

        // no leading non-content lines: nothing emitted before the first header
        kimix::vector<kimix::string> l3 = kv({"x"});
        kimix::vector<g::file_group> g3;
        expect(g::group_lines_by_file(l3, g3) == tool_status::ok);
        expect(g3.empty());
    };

    "group_line_indices_by_blank"_test = [] {
        kimix::vector<kimix::vector<uint32_t>> groups;
        g::group_line_indices_by_blank(kv({"a", "", "b", "c", "  ", "d", ""}), groups);
        expect(((groups.size()) == (size_t(3))));
        expect(((groups[0].size()) == (size_t(1))) && ((groups[0][0]) == (0u)));
        expect(((groups[1].size()) == (size_t(2))) && ((groups[1][0]) == (2u)) && ((groups[1][1]) == (3u)));
        expect(((groups[2].size()) == (size_t(1))) && ((groups[2][0]) == (5u)));
        g::group_line_indices_by_blank({}, groups);
        expect(groups.empty());
        g::group_line_indices_by_blank(kv({"", "   ", "x"}), groups);
        expect(((groups.size()) == (size_t(1))) && ((groups[0].size()) == (size_t(1))) && ((groups[0][0]) == (2u)));
        // Unicode whitespace is blank in Python (str.strip) - no ASCII gate needed
        g::group_line_indices_by_blank(kv({"a", "\xC2\xA0", "b"}), groups); // NBSP
        expect(((groups.size()) == (size_t(2))));
        g::group_line_indices_by_blank(kv({"a", "\xE2\x80\x83", "b"}), groups); // U+2003
        expect(((groups.size()) == (size_t(2))));
    };

    "should_group_rule"_test = [] {
        expect(g::should_group(true, true, false));
        expect(!g::should_group(false, true, true)); // explicit wins over rich entries
        expect(g::should_group(false, false, true)); // auto + rich
        expect(!g::should_group(false, false, false));
    };

    "range_filter_lines"_test = [] {
        g::ranges_map map;
        const kimix::vector<line_range> ranges{range_of(3, 5, false)};
        map.push_back(g::path_ranges{kimix::string("a.py"), ranges});
        kimix::vector<kimix::string> lines =
            kv({"a.py:2:two", "a.py:3:three", "a.py:9:nine", "--", "a.py:4:four", "--", "--",
                "b.py:1:one"});
        kimix::vector<kimix::string> out;
        expect(g::range_filter_lines(lines, map, out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py:3:three", "--", "a.py:4:four", "--",
                                                    "b.py:1:one"})));
        // unfiltered map == passthrough
        expect(g::range_filter_lines(lines, {}, out) == tool_status::ok);
        expect(((sv(out)) == (sv(lines))));
        // orphan leading/trailing separators are pruned
        kimix::vector<kimix::string> l2 = kv({"--", "a.py:9:z", "--", "a.py:4:k"});
        expect(g::range_filter_lines(l2, map, out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py:4:k"})));
        // non-content lines are kept even with a filter in place
        kimix::vector<kimix::string> l3 = kv({"note", "a.py:4:k"});
        expect(g::range_filter_lines(l3, map, out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"note", "a.py:4:k"})));
    };

    "reattach_single_file_prefix"_test = [] {
        kimix::vector<kimix::string> lines = kv({"2:text", "--", "7-ctx", "bare", "12:x"});
        kimix::vector<kimix::string> out;
        expect(g::reattach_single_file_prefix(lines, "a.py", out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py:2:text", "--", "a.py-7-ctx", "bare",
                                                    "a.py:12:x"})));
        expect(g::reattach_single_file_prefix(lines, "", out) == tool_status::ok);
        expect(((sv(out)) == (sv(lines)))); // empty prefix passthrough
        // "0" and a bare number without a delimiter stay as they are;
        // a matched separator is re-emitted between prefix and line
        kimix::vector<kimix::string> l2 = kv({"0:", "0", "12", "3-"});
        expect(g::reattach_single_file_prefix(l2, "f", out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"f:0:", "0", "12", "f-3-"})));
    };

    "strip_path_prefix"_test = [] {
        kimix::vector<kimix::string> lines = kv({"C:/w/a.py:3:x", "C:\\w\\b\\c.py", "other"});
        kimix::vector<kimix::string> out;
        g::strip_path_prefix(lines, "C:\\w", out);
        expect(((sv(out)) == (std::vector<std::string>{"a.py:3:x", "b\\c.py", "other"})));
        g::strip_path_prefix(lines, "C:\\w\\", out); // trailing slash stripped
        expect(((sv(out)) == (std::vector<std::string>{"a.py:3:x", "b\\c.py", "other"})));
        kimix::vector<kimix::string> l2 = kv({"a/b/c.py"});
        g::strip_path_prefix(l2, "", out); // prefix "/" never matches
        expect(((sv(out)) == (std::vector<std::string>{"a/b/c.py"})));
    };

    "normalize_slashes_content"_test = [] {
        kimix::vector<kimix::string> lines =
            kv({"C:\\w\\a.py:007:x", "C:\\w\\a.py-9-y", "C:\\w\\plain", "a:1:b"});
        kimix::vector<kimix::string> out;
        expect(g::normalize_slashes_content(lines, "content", true, out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"C:/w/a.py:7:x", "C:/w/a.py-9-y",
                                                    "C:\\w\\plain", "a:1:b"})));
        expect(g::normalize_slashes_content(lines, "files_with_matches", true, out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"C:/w/a.py:7:x", "C:/w/a.py-9-y", "C:/w/plain",
                                                    "a:1:b"})));
        // POSIX passthrough
        expect(g::normalize_slashes_content(lines, "content", false, out) == tool_status::ok);
        expect(((sv(out)) == (sv(lines))));
    };

    "collect_record_files"_test = [] {
        kimix::vector<kimix::string> lines = kv({"a.py:3:x", "a.py-4-y", "--", "b.py:1:z", "c.txt"});
        kimix::vector<kimix::string> out;
        expect(g::collect_record_files(lines, "content", out) == tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py", "b.py"})));
        expect(g::collect_record_files(kv({"a.py:3", "b.py:0", ":5", "plain"}), "count_matches", out) ==
               tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py", "b.py", ":5", "plain"})));
        expect(g::collect_record_files(kv({"a.py", "a.py", "b.py"}), "files_with_matches", out) ==
               tool_status::ok);
        expect(((sv(out)) == (std::vector<std::string>{"a.py", "b.py"})));
    };

    // --------------------------------------------------------------- rtk protocol
    "parse_rtk_rg_output"_test = [] {
        kimix::vector<kimix::string> lines = kv({
            "42 matches in 3 files:",
            "",
            "src/a.py:3:hit",
            "  +37 more in src/a.py [see remaining: tail -n +26 /tmp/rtk/a.log]",
            "src/b.py:9:hit",
            "+133 more files [see remaining: tail -n +300 /tmp/rtk/all.log]",
            "src/c.py:1:hit",
            "plain text line",
        });
        kimix::vector<kimix::string> cleaned;
        g::rtk_meta meta;
        expect(g::parse_rtk_rg_output(lines, cleaned, meta) == tool_status::ok);
        expect(((sv(cleaned)) == (std::vector<std::string>{"src/a.py:3:hit", "src/b.py:9:hit",
                                                        "src/c.py:1:hit", "plain text line"})));
        expect(meta.total_matches.has_value() && *meta.total_matches == 42u);
        expect(meta.total_files.has_value() && *meta.total_files == 3u);
        expect(((meta.folded_files.size()) == (size_t(1))));
        expect(((s_of(meta.folded_files[0].path)) == (std::string("src/a.py"))));
        expect(((meta.folded_files[0].count) == (37u)));
        expect(meta.folded_files[0].start_line.has_value() && *meta.folded_files[0].start_line == 26u);
        expect(((s_of(meta.folded_files[0].log)) == (std::string("/tmp/rtk/a.log"))));
        expect(meta.skipped_files.has_value() && *meta.skipped_files == 133u);
        expect(meta.has_skipped_log);
        expect(((s_of(meta.skipped_log)) == (std::string("/tmp/rtk/all.log"))));

        // plain rg output passes through with empty metadata
        expect(g::parse_rtk_rg_output(kv({"src/a.py:1:x"}), cleaned, meta) == tool_status::ok);
        expect(((sv(cleaned)) == (std::vector<std::string>{"src/a.py:1:x"})));
        expect(meta.empty());

        // hint that is not a tail command: log = trimmed hint, no start_line
        expect(g::parse_rtk_rg_output(
                   kv({"  +2 more in a [see remaining: raw-hint]", "+3 more files [see remaining: ]"}),
                   cleaned, meta) == tool_status::ok);
        expect(cleaned.empty());
        expect(((meta.folded_files.size()) == (size_t(1))));
        expect(((s_of(meta.folded_files[0].log)) == (std::string("raw-hint"))));
        expect(!meta.folded_files[0].start_line.has_value());
        expect(meta.skipped_files.has_value() && *meta.skipped_files == 3u);
        expect(!meta.has_skipped_log);

        // the blank line after a header is dropped only right after a header
        expect(g::parse_rtk_rg_output(kv({"", "x", ""}), cleaned, meta) == tool_status::ok);
        expect(((sv(cleaned)) == (std::vector<std::string>{"", "x", ""})));
        // a header with no following blank line still parses
        expect(g::parse_rtk_rg_output(kv({"7 matches in 1 files:", "a:1:x"}), cleaned, meta) ==
               tool_status::ok);
        expect(((sv(cleaned)) == (std::vector<std::string>{"a:1:x"})));
        expect(meta.total_matches.has_value() && *meta.total_matches == 7u);
        // tolerant: near-miss protocol text is a real line
        expect(g::parse_rtk_rg_output(kv({"42 matches in 3 files extra"}), cleaned, meta) ==
               tool_status::ok);
        expect(((sv(cleaned)) == (std::vector<std::string>{"42 matches in 3 files extra"})));
        expect(meta.empty());
    };

    "rtk_fold_note_messages"_test = [] {
        kimix::vector<kimix::string> cleaned;
        g::rtk_meta meta;
        g::parse_rtk_rg_output(
            kv({"42 matches in 3 files:", "", "  +37 more in src/a.py [see remaining: tail -n +26 /tmp/rtk/a.log]",
                "+133 more files [see remaining: tail -n +300 /tmp/rtk/all.log]"}),
            cleaned, meta);
        kimix::string note;
        g::rtk_fold_note(meta, "", note);
        expect(((s_of(note)) == (std::string(
                   "rtk folded output: 37 more lines in src/a.py; 133 more files. Full log: tail -n +26 /tmp/rtk/a.log"))));
        g::rtk_fold_note(meta, "C:\\tmp\\rg.out", note);
        expect(((s_of(note)) == (std::string(
                   "rtk folded output: 37 more lines in src/a.py; 133 more files. Full log: tail -n +26 /tmp/rtk/a.log Original output: C:/tmp/rg.out"))));

        // no markers -> None (empty string)
        g::rtk_meta empty_meta;
        g::rtk_fold_note(empty_meta, "x", note);
        expect(note.empty());

        // folded file with a plain log and no start line
        g::rtk_meta m2;
        m2.folded_files.push_back(g::rtk_folded_file{kimix::string("a.py"), 2u, kimix::string("/tmp/x.log"), true,
                                                    kimix::optional<uint32_t>{}});
        g::rtk_fold_note(m2, "", note);
        expect(((s_of(note)) == (std::string("rtk folded output: 2 more lines in a.py. Full log: /tmp/x.log"))));

        // skipped-files only
        g::rtk_meta m3;
        m3.skipped_files = 4u;
        m3.has_skipped_log = true;
        assign_str(m3.skipped_log, "tail -n +9 /tmp/y.log");
        g::rtk_fold_note(m3, "", note);
        expect(((s_of(note)) == (std::string("rtk folded output: 4 more files. Full log: tail -n +9 /tmp/y.log"))));
    };

    // ------------------------------------------------------------------ recorder
    "recorder_merge_order_dedup_cap"_test = [] {
        kimix::vector<kimix::string> out;
        g::recorder_merge(kv({"a", "b"}), kv({"b", "c", ""}), g::k_recorder_cap, out);
        expect(((sv(out)) == (std::vector<std::string>{"a", "b", "c"})));
        g::recorder_merge(kv({"a"}), kv({"a", "a"}), g::k_recorder_cap, out);
        expect(((sv(out)) == (std::vector<std::string>{"a"})));
        g::recorder_merge({}, {}, g::k_recorder_cap, out);
        expect(out.empty());

        // cap 500 with front-drop (Python: merged[-RECORDER_CAP:])
        kimix::vector<kimix::string> existing;
        for (uint32_t i = 0; i < 505u; i++) {
            existing.push_back(kimix::string(kimix::format("f{}", i)));
        }
        kimix::vector<kimix::string> fresh;
        for (uint32_t i = 500u; i < 510u; i++) {
            fresh.push_back(kimix::string(kimix::format("f{}", i)));
        }
        g::recorder_merge(existing, fresh, g::k_recorder_cap, out);
        expect(((out.size()) == (size_t(500))));
        expect(((s_of(out.front())) == (std::string("f10"))));
        expect(((s_of(out.back())) == (std::string("f509"))));
        g::recorder_merge(existing, fresh, 3u, out);
        expect(((sv(out)) == (std::vector<std::string>{"f507", "f508", "f509"})));
    };

    "recorder_record_dedup"_test = [] {
        kimix::vector<kimix::string> existing;
        g::recorder_record(existing, "a.py");
        g::recorder_record(existing, "b.py");
        g::recorder_record(existing, "a.py");
        g::recorder_record(existing, "");
        expect(((sv(existing)) == (std::vector<std::string>{"a.py", "b.py"})));
    };

    // ----------------------------------------------------------------- sensitive
    "is_sensitive_path_table"_test = [] {
        struct Case {
            std::string in;
            bool posix;
            bool win;
        };
        const std::vector<Case> cases = {
            {".env", true, true},
            {".env.local", true, true},
            {".env.example", false, false},
            {".env.sample", false, false},
            {".env.template", false, false},
            {"./config/.env", true, true},
            {"/home/u/.ssh/id_rsa", true, true},
            {"id_ed25519", true, true},
            {"proj/.aws/credentials", true, true},
            {"credentials", true, true},
            {"src/credentials.py", false, false},
            {"foo/credentials", true, true},
            {"src/id_rsa.pub", false, false},
            {"README.md", false, false},
            {"id_ecdsa", true, true},
            {"x/.gcp/credentials", true, true},
            {".envx", false, false},
            {"a/b/.env", true, true},
            {"ENV", false, false},
            {".aws/credentials", true, true},
            // Windows-only differences: fnmatch folds case and '\\' is a separator
            {".ENV", false, true},
            {"a/.ENV", false, true},
            {"C:\\p\\.env", false, true},
            {"foo\\id_rsa", false, true},
            {"C:\\u\\.aws\\credentials", false, true},
        };
        for (const Case &c : cases) {
            bool out = false;
            expect(g::is_sensitive_path(c.in, false, out) == tool_status::ok) << c.in;
            expect(((out) == (c.posix))) << c.in << " posix";
            expect(g::is_sensitive_path(c.in, true, out) == tool_status::ok) << c.in;
            expect(((out) == (c.win))) << c.in << " windows";
        }
        bool out = true;
        expect(((g::is_sensitive_path("caf\xC3\xA9.env", false, out)) == (tool_status::unsupported)));
        expect(!out);
    };

    "basename_flavours"_test = [] {
        struct Case {
            std::string in;
            std::string posix;
            std::string win;
        };
        const std::vector<Case> cases = {
            {"a/b/c.py", "c.py", "c.py"},
            {"a\\b\\c.py", "a\\b\\c.py", "c.py"},
            {"", "", ""},
            {"x", "x", "x"},
            {"/", "", ""},
            {"./config/.env", ".env", ".env"},
            {"C:", "C:", ""},          // posix keeps the drive as the name
            {"C:\\", "C:\\", ""},
            {"C:foo", "C:foo", "foo"}, // posix keeps the drive in the name
            {"C:/a/b.txt", "b.txt", "b.txt"},
            {"//srv/share", "share", ""}, // UNC root has no name on Windows
            {"//srv/share/f", "f", "f"},
            {"\\\\srv\\share\\f", "\\\\srv\\share\\f", "f"},
            {"a/b/", "b", "b"},
            {"a/b//", "b", "b"},
            {"a/b/.", "b", "b"},
            {"a/..", "..", ".."},
            {":a", ":a", ":a"},
            {"a:", "a:", ""},
        };
        for (const Case &c : cases) {
            kimix::string p;
            kimix::string w;
            g::posix_basename(c.in, p);
            g::windows_basename(c.in, w);
            expect(((s_of(p)) == (c.posix))) << c.in << " posix";
            expect(((s_of(w)) == (c.win))) << c.in << " windows";
        }
    };

    "sensitive_file_warning_text"_test = [] {
        kimix::string out;
        expect(g::sensitive_file_warning(kv({"a/.env", "b/id_rsa", ".env"}), false, out) ==
               tool_status::ok);
        expect(((s_of(out)) == (std::string(
                   "Skipped 3 sensitive file(s) (.env, id_rsa) to protect secrets. These files may contain credentials or private keys."))));
        expect(g::sensitive_file_warning(kv({"a/.env"}), false, out) == tool_status::ok);
        expect(((s_of(out)) == (std::string(
                   "Skipped 1 sensitive file(s) (.env) to protect secrets. These files may contain credentials or private keys."))));
        // distinct names are sorted and capped at 5 with an overflow suffix
        kimix::vector<kimix::string> many;
        for (uint32_t i = 0; i < 6u; i++) {
            many.push_back(kimix::string(kimix::format("d{}/a{}.env", i, i)));
        }
        expect(g::sensitive_file_warning(many, false, out) == tool_status::ok);
        expect(((s_of(out)) == (std::string(
                   "Skipped 6 sensitive file(s) (a0.env, a1.env, a2.env, a3.env, a4.env, ... (6 files total)) to protect secrets. These files may contain credentials or private keys."))));
        // duplicate basenames collapse to one entry
        kimix::vector<kimix::string> dup;
        for (uint32_t i = 0; i < 7u; i++) {
            dup.push_back(kimix::string(kimix::format("f{}/.env", i)));
        }
        expect(g::sensitive_file_warning(dup, false, out) == tool_status::ok);
        expect(((s_of(out)) == (std::string(
                   "Skipped 7 sensitive file(s) (.env) to protect secrets. These files may contain credentials or private keys."))));
        expect(((g::sensitive_file_warning(kv({"caf\xC3\xA9/.env"}), false, out)) == (tool_status::unsupported)));
    };

    // ------------------------------------------------------------------- pattern
    "pattern_has_regex_newline"_test = [] {
        struct Case {
            std::string in;
            bool want;
        };
        const std::vector<Case> cases = {
            {"a\\nb", true},   // a\n escape
            {"a\\\\nb", false},        // an\\nb: even backslashes
            {"x\ny", true},            // literal newline
            {"a", false},
            {"\\\\\\\\n", false},      // four backslashes + n
                    {"a\\\\\\\\n", false}, // a + four backslashes + n: even run, no escape
            {"\\n", true},
            {"\\\\n", false},
            {"n", false},
            {"\\", false},
        };
        for (const Case &c : cases) {
            bool out = false;
            expect(g::pattern_has_regex_newline(c.in, out) == tool_status::ok) << c.in;
            expect(((out) == (c.want))) << c.in;
        }
        bool out = false;
        expect(((g::pattern_has_regex_newline("\xC3\xA9\\n", out)) == (tool_status::unsupported)));
    };

    "multiline_pattern_rewrite"_test = [] {
        struct Case {
            std::string in;
            std::string want;
        };
        const std::vector<Case> cases = {
            {"a\\nb", "a\\r?\\nb"},
            {"x\ny", "x\\r?\\ny"},
            {"plain", "plain"},
            {"a\\nb\nc", "a\\r?\\nb\\r?\\nc"},
            {"\\\\\\\\n", "\\\\\\\\n"},
            {"a\\r\\nb", "a\\r\\r?\\nb"}, // \r\n -> \n first, then rewrite
            {"\\n\\n", "\\r?\\n\\r?\\n"},
        };
        for (const Case &c : cases) {
            kimix::string out;
            expect(g::multiline_pattern(c.in, out) == tool_status::ok) << c.in;
            expect(((s_of(out)) == (c.want))) << c.in;
        }
        kimix::string out;
        expect(((g::multiline_pattern("\xC3\xA9\\n", out)) == (tool_status::unsupported)));
    };

    // ---------------------------------------------------------------------- join
    "join_with_byte_limit"_test = [] {
        kimix::string out;
        bool truncated = false;
        size_t omitted = 0;
        expect(g::join_with_byte_limit(kv({"abc", "def"}), 10, out, truncated, omitted));
        expect(((s_of(out)) == (std::string("abc\ndef"))));
        expect(!truncated && omitted == 0u);
        expect(g::join_with_byte_limit(kv({"abc", "def"}), 6, out, truncated, omitted));
        expect(((s_of(out)) == (std::string("abc\ndef")))); // the crossing line is kept
        expect(truncated && omitted == 0u);
        expect(g::join_with_byte_limit(kv({"abc", "def"}), 3, out, truncated, omitted));
        expect(((s_of(out)) == (std::string("abc"))));
        expect(truncated && omitted == 1u);
        expect(g::join_with_byte_limit({}, 5, out, truncated, omitted));
        expect(out.empty() && !truncated && omitted == 0u);
        expect(g::join_with_byte_limit(kv({"x"}), 0, out, truncated, omitted));
        expect(((s_of(out)) == (std::string("x"))) && truncated);
        // UTF-8 measured in bytes, not code points
        expect(g::join_with_byte_limit(kv({"\xC3\xA9\xC3\xA9"}), 3, out, truncated, omitted));
        expect(((s_of(out)) == (std::string("\xC3\xA9\xC3\xA9"))) && truncated);
        // invalid UTF-8 would raise in Python -> the shim keeps that line
        expect(!g::join_with_byte_limit(kv({"ok", "\xFF\xFE"}), 100, out, truncated, omitted));
    };

    // ------------------------------------------------------------- ASCII gating
    "regex_matcher_entry_point_is_blocked"_test = [] {
        // PCRE2 is not vendored (issue/grep.md): the entry point must refuse
        // rather than silently approximate with std::regex.
        kimix::vector<g::grep_hit> hits;
        expect(((g::grep_search_lines("a\nb", "a", false, false, hits)) ==
                (tool_status::unsupported)));
        expect(hits.empty());
    };

    "ascii_gate_returns_unsupported"_test = [] {
        // The gate is what lets the shim route to the Python mirror; each of
        // these must report `unsupported` rather than a wrong answer.
        g::selector_result r = g::parse_line_ranges("\xD9\xA1");
        expect(((g::tool_status::unsupported) == (r.status)));
        expect(((g::tool_status::unsupported) == (g::selector_line_ranges("\xD9\xA1-5").status)));

          // Non-ASCII path/text with an ASCII-closed digit pair parses natively
          // (the bounded gate: Python gives the same answer, e.g. cafe:1:U+0661).
          kimix::vector<kimix::string> ok_lines = kv({"caf\xC3\xA9:1:\xD9\xA1"});
          kimix::vector<g::file_group> ok_groups;
          expect(g::group_lines_by_file(ok_lines, ok_groups) == tool_status::ok);
          // The gate fires when the decisive region contains non-ASCII bytes:
          // a Unicode digit in the line-number slot is unknowable natively.
          kimix::vector<kimix::string> lines = kv({"caf\xC3\xA9:\xD9\xA1:x"});
          kimix::vector<g::file_group> groups;
          expect(((g::group_lines_by_file(lines, groups)) == (tool_status::unsupported)));
          kimix::vector<kimix::string> plain;
          g::ranges_map map;
          map.push_back(g::path_ranges{kimix::string("caf\xC3\xA9"),
                                       kimix::vector<line_range>{range_of(1, 2, false)}});
          expect(((g::range_filter_lines(lines, map, plain)) == (tool_status::unsupported)));
          expect(((g::normalize_slashes_content(lines, "content", true, plain)) == (tool_status::unsupported)));
          expect(((g::collect_record_files(lines, "content", plain)) == (tool_status::unsupported)));
          kimix::vector<kimix::string> out;
          // a digit run stopped by a non-ASCII byte: Python's Nd may continue
          expect(((g::reattach_single_file_prefix(kv({"3\xD9\xA1:x"}), "p", out)) == (tool_status::unsupported)));
        g::rtk_meta meta;
        kimix::vector<kimix::string> cleaned;
        expect(((g::parse_rtk_rg_output(kv({"9 matches in \xD9\xA1 files:"}), cleaned, meta)) == (tool_status::unsupported)));
    };

    "unity_build_coexists_with_shared_kernels"_test = [] {
        // The shared kernels stay reachable and unmodified from this TU.
          kimix::string t;
          truncate_line("abcdefgh", 5, t);
          // marker does not fit in 5 chars -> bare cut, no marker (Python parity)
          expect(((s_of(t)) == (std::string("abcde"))));
          expect(((size_t(500)) == (g::k_recorder_cap)));
          expect(((k_record_cap) == (size_t(500))));
    };
}
