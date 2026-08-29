// Test for the built-in agent tool "write" kernels
// (builtin_tools/write_tool.h + write_tool.cpp).
//
// Covers the plan write.md section 7 list mapped to the builtin_tools surface:
// - UTF-8 strict validation wrapper + expected post-write size
// - auto-generated-file guard: filename patterns, header markers
//   (all four, case-insensitivity, known generators, protoc-gen-* wildcard,
//   comment styles, shebang/BOM/block headers, header limits), refusal text
// - conflict scan/splice: two-way/diff3/multiple/empty/CRLF/malformed scans,
//   dangling openers, splice at anchor / shifted / altered / echo-trim / CRLF,
//   region present/equal, token expansion, region rendering, whole-file index,
//   conflict:// URI parsing, bulk directives, write-time guard refusals/notes
// - JSON format validation with line/col diagnostics (vendored yyjson) and
//   the YAML/TOML/XML unsupported dispatch
// - unified-diff emitter (difflib goldens), mkdir decision, verification and
//   success message composition
// - Tool class integration (Write::operator()): parameter validation, kernel
//   dispatch, result shaping
//
// Golden vectors were harvested from the Python reference:
//   D:/kimi-agent/kimi-cli/src/kimi_cli/tools/file/{write,auto_generated,
//   conflict_detect,check_fmt}.py and D:/kimi-agent/kimi-cli/src/kimi_cli/utils/diff.py
// (see tools/capture_goldens.py in the agent worktree).
#include "ut/ut.hpp"

#include "builtin_tools/write_tool.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::write;

namespace {

kimix::vector<kimix::string> to_kvec(std::vector<std::string> in) {
    kimix::vector<kimix::string> out;
    out.reserve(in.size());
    for (auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

std::vector<std::string> from_kvec(kimix::span<const kimix::string> in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto &s : in) {
        out.emplace_back(s.data(), s.size());
    }
    return out;
}

conflict_entry entry_from_block(const conflict_block &b, int32_t id = 1,
                                const char *path = "/tmp/x.py") {
    conflict_entry e;
    e.start_line = b.start_line;
    e.separator_line = b.separator_line;
    e.end_line = b.end_line;
    e.base_line = b.base_line;
    e.ours_label = b.ours_label;
    e.base_label = b.base_label;
    e.theirs_label = b.theirs_label;
    e.ours_lines = b.ours_lines;
    e.base_lines = b.base_lines;
    e.theirs_lines = b.theirs_lines;
    e.id = id;
    e.absolute_path = path;
    e.display_path = path;
    return e;
}

std::string block_repr(const conflict_block &b) {
    std::string s = "(" + std::to_string(b.start_line) + "," +
                    std::to_string(b.separator_line) + "," +
                    std::to_string(b.end_line) + "," + std::to_string(b.base_line);
    s += b.ours_label.has_value() ? "," + *b.ours_label : ",None";
    s += b.base_label.has_value() ? "," + *b.base_label : ",None";
    s += b.theirs_label.has_value() ? "," + *b.theirs_label : ",None";
    s += ",[";
    for (size_t i = 0; i < b.ours_lines.size(); ++i) {
        if (i) {
            s += ",";
        }
        s += b.ours_lines[i];
    }
    s += "],[";
    for (size_t i = 0; i < b.base_lines.size(); ++i) {
        if (i) {
            s += ",";
        }
        s += b.base_lines[i];
    }
    s += "],[";
    for (size_t i = 0; i < b.theirs_lines.size(); ++i) {
        if (i) {
            s += ",";
        }
        s += b.theirs_lines[i];
    }
    s += "])";
    return s;
}

std::string dangling_repr(const kimix::vector<dangling_opener> &d) {
    std::string s = "[";
    for (size_t i = 0; i < d.size(); ++i) {
        if (i) {
            s += ", ";
        }
        s += "(" + std::to_string(d[i].line) + ", '" +
             std::string(d[i].marker_line.data(), d[i].marker_line.size()) + "')";
    }
    s += "]";
    return s;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ------------------------------------------------------------------
    // 1. UTF-8 strict validation wrapper + expected size
    // ------------------------------------------------------------------
    "utf8_decode_error_wrapper"_test = [] {
        expect(!utf8_decode_error("valid \xE2\x86\x92 utf-8").has_value())
            << "valid text accepted";
        expect(!utf8_decode_error("").has_value()) << "empty is valid";
        auto err = utf8_decode_error("\x80");
        expect(err.has_value()) << "lone continuation rejected";
        expect(eq(*err, kimix::string("utf-8 decoding error: invalid start byte")));
        err = utf8_decode_error("\xC3");
        expect(err.has_value());
        expect(eq(*err, kimix::string("utf-8 decoding error: unexpected end of data")));
        err = utf8_decode_error("\xED\xA0\x80");
        expect(err.has_value()) << "surrogate rejected";
        expect(eq(*err,
                  kimix::string("utf-8 decoding error: invalid continuation byte")));
        err = utf8_decode_error("\xE0\x80\x80");
        expect(err.has_value()) << "overlong rejected";
        expect(eq(*err, kimix::string("utf-8 decoding error: invalid continuation byte")));
        err = utf8_decode_error("\xF5\x80\x80\x80");
        expect(err.has_value()) << "above U+10FFFF rejected";
        expect(eq(*err, kimix::string("utf-8 decoding error: invalid start byte")));
    };

    "expected_write_size"_test = [] {
        auto s = expected_write_size(false, "", "", "abc\xE2\x86\x92");
        expect(s.has_value()) << "overwrite: valid utf-8";
        expect(eq(*s, uint64_t(6))) << "3 ascii + 3-byte arrow";
        expect(!expected_write_size(false, "", "", "a\x80" "b").has_value())
            << "invalid utf-8 -> nullopt";
        auto a = expected_write_size(true, "ab", "\xE2\x86\x92", "");
        expect(a.has_value()) << "append: valid utf-8";
        expect(eq(*a, uint64_t(5))) << "old 2 + content 3";
        expect(!expected_write_size(true, "a\x80", "b", "").has_value())
            << "append: invalid old text -> nullopt";
        expect(!expected_write_size(true, "a", "b\x80", "").has_value())
            << "append: invalid content -> nullopt";
    };

    // ------------------------------------------------------------------
    // 2. Auto-generated-file guard
    // ------------------------------------------------------------------
    "auto_generated_filename_patterns"_test = [] {
        struct case_t {
            const char *path;
            bool expected;
        };
        const case_t cases[] = {
            {"zz_generated.py", true},   {"zz_generated.foo", true},
            {"foo.pb.go", true},         {"foo.pb.cc", true},
            {"foo.pb.h", true},          {"foo.pb.c", true},
            {"foo.pb.js", true},         {"foo.pb.ts", true},
            {"foo_pb2.py", true},        {"foo_pb2_grpc.py", true},
            {"foo.gen.go", true},        {"foo.gen.ts", true},
            {"foo.gen.js", true},        {"foo.gen.py", true},
            {"generated.go", true},      {"generated.ts", true},
            {"generated.js", true},      {"generated.py", true},
            {"swagger.json", false},     {"api.swagger.json", true},
            {"api.openapi.json", true},  {"x.mock.go", true},
            {"x.mock.ts", true},         {"x.mocks.go", true},
            {"x.mocks.js", true},        {"x.mock.js", true},
            {"README.md", false},        {"main.py", false},
            {"foo.pb2", false},          {"foo.gen", false},
            {"x.mock.py", false},        {"x.mocks.py", false},
            {"my_generated_file.py", false},
            // basename-only semantics: directory components are ignored
            {"src/gen/foo.pb.go", true}, {"dir/README.md", false},
        };
        for (const auto &c : cases) {
            expect(eq(is_auto_generated_file_name(c.path), c.expected))
                << c.path << " expected " << (c.expected ? "generated" : "hand-written");
        }
    };

    "auto_generated_header_markers"_test = [] {
        struct case_t {
            const char *name;
            const char *content;
            const char *path;
            const char *expected; // nullptr == None
        };
        const case_t cases[] = {
            {"py_hash_marker",
             "#!/usr/bin/env python\n# @generated\n# by something\nx = 1\n", "/tmp/gen.py",
             "@generated"},
            {"py_code_generated",
             "# Code generated by protoc-gen-go. DO NOT EDIT.\n", "/tmp/a.py",
             "Code generated by protoc-gen-go."},
            {"ts_slash",
             "// Code generated by protoc-gen-go. DO NOT EDIT.\n", "/tmp/a.ts",
             "Code generated by protoc-gen-go."},
            {"sql_dashdash", "-- generated by sqlc\nSELECT 1;\n", "/tmp/a.sql",
             "generated by sqlc"},
            {"html_block",
             "<!-- Code generated by some tool. DO NOT EDIT. -->\n<html>", "/tmp/a.html",
             "Code generated by some"},
            {"bom_shebang",
             "\xEF\xBB\xBF#!/bin/sh\n# @generated\n", "/tmp/a.sh", "@generated"},
            {"this_file_was", "// this file was automatically generated\n", "/tmp/a.cpp",
             "this file was automatically generated"},
            {"protoc_gen_wild", "// generated by protoc-gen-foo-bar\n", "/tmp/a.go",
             "generated by protoc-gen-foo-bar"},
            {"deepcopy", "// Code generated by deepcopy-gen. DO NOT EDIT.\n", "/tmp/a.go",
             "Code generated by deepcopy-gen."},
            {"case_insensitive", "# CODE GENERATED BY PROTOC. DO NOT EDIT.\n",
             "/tmp/a.py", "CODE GENERATED BY PROTOC."},
            {"known_neg", "# generated by my_custom_script\n", "/tmp/a.py", nullptr},
            {"prose_neg", "# This file is auto-generated by hand\n", "/tmp/a.py", nullptr},
            {"no_header", "x = 1\n", "/tmp/a.py", nullptr},
            {"blank_before", "\n\n# @generated\n", "/tmp/a.py", "@generated"},
            {"block_comment_style",
             "/*\n * Code generated by swagger-codegen\n */\n", "/tmp/a.go",
             "Code generated by swagger-codegen"},
            {"marker_in_body", "x = 1\n# @generated\n", "/tmp/a.py", nullptr},
            {"dockerfile", "FROM ubuntu\n# @generated\n", "/tmp/Dockerfile", nullptr},
            {"makefile", "# generated by buf\nall:\n", "/tmp/Makefile",
             "generated by buf"},
            {"justfile", "# @generated\nfoo:\n", "/tmp/Justfile", "@generated"},
            {"swagger_openapi_gen", "# generated by swagger-codegen\n", "/tmp/a.py",
             "generated by swagger-codegen"},
            {"openapi_generator", "# generated by openapi-generator\n", "/tmp/a.py",
             "generated by openapi-generator"},
            {"napi_rs", "# generated by napi-rs\n", "/tmp/a.py", "generated by napi-rs"},
        };
        for (const auto &c : cases) {
            auto got = detect_auto_generated_marker(c.content, c.path);
            if (c.expected == nullptr) {
                expect(!got.has_value()) << c.name << " should be None, got "
                                         << (got.has_value() ? *got : kimix::string());
            } else {
                expect(got.has_value()) << c.name << " should detect a marker";
                if (got.has_value()) {
                    expect(eq(*got, kimix::string(c.expected))) << c.name;
                }
            }
        }
        // Filename patterns win: marker returned is the basename.
        auto by_name = detect_auto_generated_marker("anything\n", "/tmp/zz_generated.foo");
        expect(by_name.has_value());
        expect(eq(*by_name, kimix::string("zz_generated.foo")));
    };

    "auto_generated_header_limits"_test = [] {
        // Marker beyond the 40-line header limit is NOT matched.
        kimix::string over;
        for (int i = 0; i < 45; ++i) {
            over += "# a\n";
        }
        over += "# @generated\n";
        auto got = detect_auto_generated_marker(over, "/tmp/a.py");
        expect(!got.has_value()) << "marker past the 40-line cap ignored";

        // Marker within the limit IS matched.
        kimix::string within;
        for (int i = 0; i < 39; ++i) {
            within += "# a\n";
        }
        within += "# @generated\n";
        got = detect_auto_generated_marker(within, "/tmp/a.py");
        expect(got.has_value());
        expect(eq(*got, kimix::string("@generated")));

        // Marker beyond the 1024-byte prefix is NOT matched.
        kimix::string big(2048, 'x');
        big += "\n# @generated\n";
        got = detect_auto_generated_marker(big, "/tmp/a.py");
        expect(!got.has_value()) << "marker past the 1 KiB prefix ignored";
    };

    "auto_generated_comment_styles"_test = [] {
        kimix::vector<comment_style> styles;
        get_comment_styles_for_path("/tmp/a.py", styles);
        expect(eq(styles.size(), size_t(1)));
        expect(styles[0] == comment_style::hash);
        get_comment_styles_for_path("/tmp/a.ts", styles);
        expect(styles[0] == comment_style::slash);
        get_comment_styles_for_path("/tmp/a.sql", styles);
        expect(styles[0] == comment_style::sql);
        get_comment_styles_for_path("/tmp/a.html", styles);
        expect(styles[0] == comment_style::html);
        get_comment_styles_for_path("/tmp/Dockerfile", styles);
        expect(styles[0] == comment_style::hash);
        get_comment_styles_for_path("/tmp/Makefile", styles);
        expect(styles[0] == comment_style::hash);
        get_comment_styles_for_path("/tmp/Justfile", styles);
        expect(styles[0] == comment_style::hash);
        get_comment_styles_for_path("/tmp/a.xyz", styles);
        expect(eq(styles.size(), size_t(4))) << "unknown extension -> all styles";
    };

    "auto_generated_error_message"_test = [] {
        kimix::string msg =
            build_auto_generated_error("/tmp/x.py", "zz_generated.py");
        const kimix::string expected =
            "Cannot modify auto-generated file: /tmp/x.py\n\n"
            "This file appears to be automatically generated (detected marker: "
            "\"zz_generated.py\"). Changes will be overwritten the next time the "
            "code is regenerated. Edit the source (schema, template, or generator "
            "input) and regenerate instead, or pass allow_auto_generated=true to "
            "override.";
        expect(eq(msg, expected));
    };

    // ------------------------------------------------------------------
    // 3. Conflict-marker scan + splice
    // ------------------------------------------------------------------
    "conflict_marker_matching"_test = [] {
        expect(match_marker("<<<<<<<", k_ours_prefix).has_value());
        expect(eq(*match_marker("<<<<<<<", k_ours_prefix), kimix::string("")));
        auto label = match_marker("<<<<<<< HEAD", k_ours_prefix);
        expect(label.has_value());
        expect(eq(*label, kimix::string("HEAD")));
        expect(!match_marker("<<<<<<<  two", k_ours_prefix).has_value())
            << "label starting with space never matches";
        expect(!match_marker("<<<<<<<x", k_ours_prefix).has_value());
        expect(!match_marker(" x", k_ours_prefix).has_value());
        expect(is_separator("======="));
        expect(!is_separator("======= x"));
        expect(!is_separator("========"));
        auto base = match_marker("||||||| base", k_base_prefix);
        expect(base.has_value());
        expect(eq(*base, kimix::string("base")));
        auto theirs = match_marker(">>>>>>> theirs\r", k_theirs_prefix);
        expect(theirs.has_value()) << "trailing CR stripped";
        expect(eq(*theirs, kimix::string("theirs")));
        expect(!match_marker("", k_ours_prefix).has_value());
    };

    "conflict_scan_two_way"_test = [] {
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks("<<<<<<< HEAD\nours1\n=======\ntheirs1\n>>>>>>> branch\n",
                             blocks);
        expect(eq(blocks.size(), size_t(1)));
        if (!blocks.empty()) {
            expect(eq(block_repr(blocks[0]),
                      std::string("(1,3,5,-1,HEAD,None,branch,[ours1],[],[theirs1])")));
        }

        blocks.clear();
        scan_conflict_blocks("<<<<<<<\nours\n=======\ntheirs\n>>>>>>>\n", blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(1,3,5,-1,None,None,None,[ours],[],[theirs])")));

        blocks.clear();
        scan_conflict_blocks("", blocks);
        expect(blocks.empty());
        blocks.clear();
        scan_conflict_blocks("plain text\nno markers\n", blocks);
        expect(blocks.empty());
    };

    "conflict_scan_diff3_and_multiple"_test = [] {
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks("<<<<<<< HEAD\nours\n||||||| merged common ancestor\nbase\n"
                             "=======\ntheirs\n>>>>>>> branch\n",
                             blocks);
        expect(eq(blocks.size(), size_t(1)));
        if (!blocks.empty()) {
            expect(eq(block_repr(blocks[0]),
                      std::string("(1,5,7,3,HEAD,merged common ancestor,branch,"
                                  "[ours],[base],[theirs])")));
        }

        blocks.clear();
        scan_conflict_blocks("a\n<<<<<<< A\n1\n=======\n2\n>>>>>>> B\nb\n"
                             "<<<<<<< C\n3\n=======\n4\n>>>>>>> D\n",
                             blocks);
        expect(eq(blocks.size(), size_t(2)));
        if (blocks.size() == 2) {
            expect(eq(block_repr(blocks[0]),
                      std::string("(2,4,6,-1,A,None,B,[1],[],[2])")));
            expect(eq(block_repr(blocks[1]),
                      std::string("(8,10,12,-1,C,None,D,[3],[],[4])")));
        }

        // First-line offset: blank first line shifts the block.
        blocks.clear();
        scan_conflict_blocks("\n<<<<<<< A\nx\n=======\ny\n>>>>>>> B\n", blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(2,4,6,-1,A,None,B,[x],[],[y])")));

        // Empty ours/theirs sections.
        blocks.clear();
        scan_conflict_blocks("<<<<<<< A\n=======\n>>>>>>> B\n", blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(1,2,3,-1,A,None,B,[],[],[])")));
    };

    "conflict_scan_malformed"_test = [] {
        kimix::vector<conflict_block> blocks;
        // Unclosed tail is dropped.
        scan_conflict_blocks("<<<<<<< A\na\n=======\nb\n", blocks);
        expect(blocks.empty());
        // Separator without opener is ignored.
        blocks.clear();
        scan_conflict_blocks("=======\nfoo\n", blocks);
        expect(blocks.empty());
        // Malformed base reset + reprocess: the second opener becomes a 2-way.
        blocks.clear();
        scan_conflict_blocks("<<<<<<< A\na\n||||||| B\nb\n<<<<<<< C\nc\n=======\nd\n"
                             ">>>>>>> E\n",
                             blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(5,7,9,-1,C,None,E,[c],[],[d])")));
        // Nested opener restarts the block.
        blocks.clear();
        scan_conflict_blocks("<<<<<<< A\na\n<<<<<<< B\nb\n=======\nc\n>>>>>>> D\n",
                             blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(3,5,7,-1,B,None,D,[b],[],[c])")));
        // CRLF input matches with \r stripped.
        blocks.clear();
        scan_conflict_blocks("<<<<<<< A\r\na\r\n=======\r\nb\r\n>>>>>>> B\r\n", blocks);
        expect(eq(blocks.size(), size_t(1)));
        expect(eq(block_repr(blocks[0]),
                  std::string("(1,3,5,-1,A,None,B,[a],[],[b])")));
        // "prefix+label" without a separating space never matches.
        blocks.clear();
        scan_conflict_blocks("<<<<<<<A\nx\n=======\ny\n>>>>>>>B\n", blocks);
        expect(blocks.empty());
    };

    "conflict_dangling_openers"_test = [] {
        kimix::vector<dangling_opener> dangling;
        find_dangling_openers("<<<<<<< A\na\n", dangling);
        expect(eq(dangling_repr(dangling),
                  std::string("[(1, '<<<<<<< A')]")));
        dangling.clear();
        find_dangling_openers("<<<<<<< A\na\n=======\nb\n>>>>>>> B\n", dangling);
        expect(dangling.empty()) << "closed block -> no dangling opener";
        dangling.clear();
        find_dangling_openers("<<<<<<< A\n<<<<<<< B\nx\n", dangling);
        expect(eq(dangling_repr(dangling),
                  std::string("[(2, '<<<<<<< B')]")));
        dangling.clear();
        find_dangling_openers("<<<<<<< A\r\nx\r\n", dangling);
        expect(eq(dangling_repr(dangling),
                  std::string("[(1, '<<<<<<< A')]")));
        dangling.clear();
        find_dangling_openers("clean\n", dangling);
        expect(dangling.empty());
    };

    "conflict_splice_basic"_test = [] {
        const char *text = "pre\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n"
                           "post\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        expect(eq(blocks.size(), size_t(1)));
        conflict_entry entry = entry_from_block(blocks[0]);

        conflict_splice_result out;
        kimix::string err;
        kimix::string expanded;
        expect(!expand_content_tokens("@ours", entry, expanded).has_value());
        expect(splice_conflict(text, entry, expanded, out, err));
        expect(eq(out.text, kimix::string("pre\nours\npost\n")));
        expect(eq(out.trimmed_leading, int32_t(0)));
        expect(eq(out.trimmed_trailing, int32_t(0)));

        expect(!expand_content_tokens("@theirs", entry, expanded).has_value());
        expect(splice_conflict(text, entry, expanded, out, err));
        expect(eq(out.text, kimix::string("pre\ntheirs\npost\n")));

        auto base_err = expand_content_tokens("@base", entry, expanded);
        expect(base_err.has_value()) << "@base on 2-way conflict errors";
        expect(eq(*base_err,
                  kimix::string("@base is not available for conflict #1 \xE2\x80\x94 it "
                                "is a 2-way conflict (no ||||||| base section).")));

        expect(!expand_content_tokens("@both", entry, expanded).has_value());
        expect(splice_conflict(text, entry, expanded, out, err));
        expect(eq(out.text, kimix::string("pre\nours\ntheirs\npost\n")));

        expect(splice_conflict(text, entry, "new\ncontent", out, err));
        expect(eq(out.text, kimix::string("pre\nnew\ncontent\npost\n")));

        // Literal line that looks like a token but is a bare word passes through.
        expect(splice_conflict(text, entry, "ours", out, err));
        expect(eq(out.text, kimix::string("pre\nours\npost\n")));

        // Empty replacement deletes the region.
        expect(splice_conflict(text, entry, "", out, err));
        expect(eq(out.text, kimix::string("pre\npost\n")));
    };

    "conflict_splice_shifted_and_altered"_test = [] {
        const char *text =
            "head\npre\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\npost\ntail\n";
        // Recorded anchor is far away; nearest-occurrence fallback locates it.
        conflict_entry entry;
        entry.start_line = 10;
        entry.separator_line = 12;
        entry.end_line = 14;
        entry.base_line = -1;
        entry.ours_label = "HEAD";
        entry.theirs_label = "branch";
        entry.ours_lines = to_kvec({"ours"});
        entry.theirs_lines = to_kvec({"theirs"});
        entry.id = 2;
        entry.display_path = "/tmp/x.py";
        conflict_splice_result out;
        kimix::string err;
        expect(splice_conflict(text, entry, "NEW", out, err));
        expect(eq(out.text, kimix::string("head\npre\nNEW\npost\ntail\n")));
        expect(eq(out.trimmed_leading, int32_t(0)));
        expect(eq(out.trimmed_trailing, int32_t(0)));

        // Altered block (ours content differs) -> false + exact error.
        conflict_entry bad = entry;
        bad.id = 3;
        bad.ours_lines = to_kvec({"OURS"});
        expect(!splice_conflict(text, bad, "NEW", out, err));
        expect(eq(err,
                  kimix::string("Conflict #3 no longer matches the recorded block at "
                                "/tmp/x.py:10. Re-read the file to get a current "
                                "conflict id.")));
    };

    "conflict_splice_echo_trim"_test = [] {
        // Multi-line echo is always trimmed.
        const char *multi =
            "a\nb\nc\n<<<<<<< A\nb\nc\n=======\nd\n>>>>>>> B\nb\nc\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(multi, blocks);
        conflict_entry e4 = entry_from_block(blocks[0], 4);
        conflict_splice_result out;
        kimix::string err;
        expect(splice_conflict(multi, e4, "b\nc\nNEW", out, err));
        expect(eq(out.text, kimix::string("a\nb\nc\nNEW\nb\nc\n")));
        expect(eq(out.trimmed_leading, int32_t(2)));
        expect(eq(out.trimmed_trailing, int32_t(0)));

        // Single-line echo trimmed only when delimiter balance is 0.
        const char *single =
            "a\nb\n<<<<<<< A\nb\n=======\nc\n>>>>>>> B\nb\nc\n";
        blocks.clear();
        scan_conflict_blocks(single, blocks);
        conflict_entry e5 = entry_from_block(blocks[0], 5);
        expect(splice_conflict(single, e5, "b\nc\nNEW", out, err));
        expect(eq(out.text, kimix::string("a\nb\nc\nNEW\nb\nc\n")));
        expect(eq(out.trimmed_leading, int32_t(1)));
        expect(eq(out.trimmed_trailing, int32_t(0)));

        // Single-line echo with unbalanced delimiters is NOT trimmed.
        const char *paren = "f(\n<<<<<<< A\nf(\n=======\ng(\n>>>>>>> B\nf(\n";
        blocks.clear();
        scan_conflict_blocks(paren, blocks);
        conflict_entry e6 = entry_from_block(blocks[0], 6);
        expect(splice_conflict(paren, e6, "f(\nNEW", out, err));
        expect(eq(out.trimmed_leading, int32_t(0))) << "delimiter balance 1 keeps echo";
        expect(eq(out.text,
                  kimix::string("f(\nf(\nNEW\nf(\n")))
            << "leading echo kept when '(' is unbalanced";
    };

    "conflict_splice_crlf"_test = [] {
        const char *text = "pre\r\n<<<<<<< A\r\na\r\n=======\r\nb\r\n>>>>>>> B\r\npost\r\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        expect(eq(blocks.size(), size_t(1)));
        conflict_entry e = entry_from_block(blocks[0], 5);
        conflict_splice_result out;
        kimix::string err;
        expect(splice_conflict(text, e, "NEW", out, err));
        expect(eq(out.text, kimix::string("pre\r\nNEW\r\npost\r\n")))
            << "CRLF re-applied when the original used CRLF";
    };

    "conflict_region_semantics"_test = [] {
        const char *text = "pre\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n"
                           "post\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        conflict_entry e = entry_from_block(blocks[0]);
        expect(conflict_region_present(text, e)) << "region present verbatim";
        expect(conflict_regions_equal(e, e)) << "region equals itself";
        conflict_entry other = e;
        other.ours_lines = to_kvec({"OTHER"});
        expect(!conflict_regions_equal(e, other)) << "different region not equal";
        // CRLF-normalized content still contains the LF region.
        kimix::string crlf = kimix::string(text);
        kimix::string norm;
        for (size_t i = 0; i < crlf.size(); ++i) {
            if (crlf[i] == '\n') {
                norm += "\r\n";
            } else {
                norm += crlf[i];
            }
        }
        expect(conflict_region_present(norm, e)) << "CRLF content matches LF region";
    };

    "conflict_expand_tokens"_test = [] {
        const char *text = "a\n<<<<<<< HEAD\n1\n2\n=======\n3\n>>>>>>> branch\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        conflict_entry e = entry_from_block(blocks[0], 7);
        kimix::string out;
        expect(!expand_content_tokens("keep\n@ours\n@theirs\n@both\nend", e, out)
                    .has_value());
        const kimix::string expected = "keep\n1\n2\n3\n1\n2\n3\nend";
        expect(eq(out, expected));

        // Non-token lines pass through verbatim (incl. token-looking text).
        expect(!expand_content_tokens("prefix @ours suffix", e, out).has_value());
        expect(eq(out, kimix::string("prefix @ours suffix")));

        // diff3 @base works.
        kimix::vector<conflict_block> blocks3;
        scan_conflict_blocks("<<<<<<< A\n1\n||||||| B\nb\n=======\n2\n>>>>>>> C\n",
                             blocks3);
        conflict_entry e3 = entry_from_block(blocks3[0], 8);
        expect(!expand_content_tokens("@base", e3, out).has_value());
        expect(eq(out, kimix::string("b")));
        expect(!expand_content_tokens("@ours", e3, out).has_value());
        expect(eq(out, kimix::string("1")));
        expect(!expand_content_tokens("@theirs", e3, out).has_value());
        expect(eq(out, kimix::string("2")));
    };

    "conflict_render_region"_test = [] {
        const char *text = "pre\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        conflict_entry e = entry_from_block(blocks[0]);
        kimix::vector<kimix::string> lines;
        int32_t start = 0;

        expect(!render_conflict_region(e, "", lines, start).has_value());
        expect(eq(start, int32_t(2)));
        expect((from_kvec(lines) == std::vector<std::string>(
                   {"<<<<<<< HEAD", "ours", "=======", "theirs", ">>>>>>> branch"})));

        expect(!render_conflict_region(e, "ours", lines, start).has_value());
        expect(eq(start, int32_t(3)));
        expect((from_kvec(lines) == std::vector<std::string>({"ours"})));

        expect(!render_conflict_region(e, "theirs", lines, start).has_value());
        expect(eq(start, int32_t(5)));
        expect((from_kvec(lines) == std::vector<std::string>({"theirs"})));

        auto base_err = render_conflict_region(e, "base", lines, start);
        expect(base_err.has_value());
        expect(eq(*base_err,
                  kimix::string("Conflict #1 is a 2-way conflict \xE2\x80\x94 no base "
                                "section. Use /ours or /theirs.")));

        auto unknown = render_conflict_region(e, "bogus", lines, start);
        expect(unknown.has_value());
        expect(eq(*unknown, kimix::string("Unknown conflict scope 'bogus'.")));
    };

    "conflict_format_summary"_test = [] {
        const char *text = "pre\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n";
        kimix::vector<conflict_block> blocks;
        scan_conflict_blocks(text, blocks);
        conflict_entry e = entry_from_block(blocks[0], 1, "/tmp/x.py");
        kimix::vector<conflict_entry> entries;
        entries.push_back(e);
        const kimix::string expected =
            "\xE2\x9A\xA0 1 unresolved conflict in /tmp/x.py\n"
            "- ours = HEAD\n"
            "- theirs = branch\n"
            "NOTICE: Bulk-resolve with `write({ path: \"conflict://*\", content })`, "
            "or address a single block with `write({ path: \"conflict://<N>\", "
            "content })`. A line exactly `@ours` / `@theirs` / `@base` / `@both` "
            "expands to that recorded section; non-token lines pass through "
            "verbatim.\n"
            "#1  L2-6";
        expect(eq(format_conflict_summary(entries, "/tmp/x.py", false), expected));

        // diff3 entry gets the (3-way) suffix.
        kimix::vector<conflict_block> blocks3;
        scan_conflict_blocks("<<<<<<< A\n1\n||||||| B\nb\n=======\n2\n>>>>>>> C\n",
                             blocks3);
        conflict_entry e3 = entry_from_block(blocks3[0], 2, "/tmp/x.py");
        kimix::vector<conflict_entry> entries3;
        entries3.push_back(e3);
        expect(format_conflict_summary(entries3, "/tmp/x.py", false)
                   .find("(3-way)") != kimix::string::npos)
            << "diff3 blocks carry the (3-way) suffix";

        // Empty list.
        kimix::vector<conflict_entry> none;
        expect(eq(format_conflict_summary(none, "/tmp/x.py", false),
                  kimix::string("No unresolved git merge conflicts in /tmp/x.py.")));

        // Byte-cap note.
        expect(format_conflict_summary(entries, "/tmp/x.py", true)
                   .find("file scan hit the byte cap") != kimix::string::npos);
    };

    "conflict_parse_uri"_test = [] {
        parsed_conflict_uri out;
        expect(!parse_conflict_uri("conflict://1", out).has_value());
        expect(!out.is_star);
        expect(eq(out.id, int64_t(1)));
        expect(out.scope.empty());
        expect(!out.recovered_prefix.has_value());

        expect(!parse_conflict_uri("conflict://1/ours", out).has_value());
        expect(eq(out.id, int64_t(1)));
        expect(eq(out.scope, kimix::string("ours")));

        expect(!parse_conflict_uri("conflict://1/theirs", out).has_value());
        expect(eq(out.scope, kimix::string("theirs")));
        expect(!parse_conflict_uri("conflict://1/base", out).has_value());
        expect(eq(out.scope, kimix::string("base")));

        expect(!parse_conflict_uri("conflict://*", out).has_value());
        expect(out.is_star);
        expect(out.scope.empty());

        expect(!parse_conflict_uri("/tmp/x.py:conflict://2", out).has_value());
        expect(eq(out.id, int64_t(2)));
        expect(out.recovered_prefix.has_value());
        expect(eq(*out.recovered_prefix, kimix::string("/tmp/x.py")));

        auto err = parse_conflict_uri("conflict://1/bogus", out);
        expect(err.has_value());
        expect(eq(*err,
                  kimix::string("Invalid conflict scope 'bogus'. Valid scopes: ours, "
                                "theirs, base.")));
        err = parse_conflict_uri("conflict://0", out);
        expect(err.has_value());
        expect(eq(*err,
                  kimix::string("Invalid conflict id '0' \xE2\x80\x94 ids start at 1.")));
        err = parse_conflict_uri("conflict://abc", out);
        expect(err.has_value());
        expect(eq(*err,
                  kimix::string("Invalid conflict id 'abc' in 'conflict://abc'. "
                                "Expected conflict://<N> or "
                                "conflict://<N>/<ours|theirs|base>.")));
        err = parse_conflict_uri("conflict://*/ours", out);
        expect(err.has_value());
        expect(eq(*err,
                  kimix::string("conflict://* does not accept a scope \xE2\x80\x94 it "
                                "resolves every registered conflict.")));

        // Non-conflict paths return nullopt without error.
        expect(!parse_conflict_uri("/tmp/x.py", out).has_value());
        expect(!parse_conflict_uri("", out).has_value());
        expect(!parse_conflict_uri("xconflict://1", out).has_value());
        expect(!parse_conflict_uri(":conflict://1", out).has_value());
    };

    "conflict_parse_bulk_directives"_test = [] {
        kimix::vector<std::pair<int32_t, kimix::string>> out;
        expect(parse_bulk_directives("1: @ours\n2: @theirs\n", out));
        expect(eq(out.size(), size_t(2)));
        expect(eq(out[0].first, int32_t(1)));
        expect(eq(out[0].second, kimix::string("ours")));
        expect(eq(out[1].first, int32_t(2)));
        expect(eq(out[1].second, kimix::string("theirs")));

        out.clear();
        expect(parse_bulk_directives("1: @base\n", out));
        expect(eq(out[0].second, kimix::string("base")));

        out.clear();
        expect(parse_bulk_directives("1: @ours\n 2 : @both \n", out));
        expect(eq(out.size(), size_t(2)));
        expect(eq(out[1].second, kimix::string("both")));

        out.clear();
        expect(!parse_bulk_directives("junk\n", out)) << "non-directive line";
        expect(!parse_bulk_directives("1: @ours\n2: junk\n", out));
        expect(!parse_bulk_directives("\n\n", out)) << "only blank lines -> None";
        expect(!parse_bulk_directives("", out));
        expect(!parse_bulk_directives("1: @bogus\n", out));
    };

    "conflict_guard_refusals"_test = [] {
        // Overwrite with new marker blocks refuses unless allowed.
        auto r = run_conflict_guard("/tmp/x.py", "old\n", "<<<<<<< A\n1\n=======\n2\n"
                                                       ">>>>>>> B\n",
                                    /*append=*/false, /*file_existed=*/true,
                                    /*allow_conflicts=*/false);
        expect(r.error.has_value());
        if (r.error.has_value()) {
            const kimix::string expected =
                "Conflict markers detected in `/tmp/x.py`; refusing to write.\n"
                "  line 1: <<<<<<< marker block start\n"
                "  line 5: >>>>>>> marker block end\n"
                "The content still contains 1 unresolved conflict marker block(s). "
                "Resolve them first (e.g. read `/tmp/x.py:conflicts`, then "
                "`write({ path: \"conflict://<N>\", content })`), or set "
                "allow_conflicts=True to write anyway.";
            expect(eq(*r.error, expected));
        }
        expect(r.note.empty());

        // allow_conflicts=true -> note instead of refusal.
        r = run_conflict_guard("/tmp/x.py", "old\n",
                               "<<<<<<< A\n1\n=======\n2\n>>>>>>> B\n",
                               /*append=*/false, /*file_existed=*/true,
                               /*allow_conflicts=*/true);
        expect(!r.error.has_value());
        expect(eq(r.note,
                  kimix::string(" Warning: written content still contains 1 unresolved "
                                "conflict marker block(s) (allow_conflicts=True).")));
        expect(!r.old_had_blocks);

        // Append onto a file ending inside an unclosed block refuses.
        r = run_conflict_guard("/tmp/x.py", "<<<<<<< A\na\n", "more\n",
                               /*append=*/true, /*file_existed=*/true,
                               /*allow_conflicts=*/false);
        expect(r.error.has_value());
        if (r.error.has_value()) {
            const kimix::string expected =
                "Conflict markers detected in `/tmp/x.py`; refusing to append: the "
                "file ends inside an unclosed conflict block.\n"
                "  line 1: <<<<<<< A\n"
                "Resolve the conflict first, or set allow_conflicts=True to append "
                "anyway.";
            expect(eq(*r.error, expected));
        }

        // Append with marker content refuses.
        r = run_conflict_guard("/tmp/x.py", "old\n", "<<<<<<< A\n=======\n>>>>>>> B\n",
                               /*append=*/true, /*file_existed=*/true,
                               /*allow_conflicts=*/false);
        expect(r.error.has_value());
        expect(r.error->find("refusing to write") != kimix::string::npos);

        // New file (not existing): overwrite semantics apply.
        r = run_conflict_guard("/tmp/x.py", "", "clean\n", /*append=*/false,
                               /*file_existed=*/false, /*allow_conflicts=*/false);
        expect(!r.error.has_value());
        expect(r.note.empty());
        expect(!r.old_had_blocks);
    };

    "conflict_guard_notes"_test = [] {
        // Append clean content to a file that still contains blocks.
        auto r = run_conflict_guard("/tmp/x.py",
                                    "a\n<<<<<<< A\n1\n=======\n2\n>>>>>>> B\n",
                                    "clean\n",
                                    /*append=*/true, /*file_existed=*/true,
                                    /*allow_conflicts=*/true);
        expect(!r.error.has_value());
        expect(r.old_had_blocks);
        expect(eq(r.note,
                  kimix::string(" Note: `/tmp/x.py` still contains 1 unresolved "
                                "conflict marker block(s); the appended text was "
                                "clean. Resolve them via `read <path>:conflicts` + "
                                "`write({ path: \"conflict://<N>\", content })`.")));

        // Append with markers and allow_conflicts -> warning note.
        r = run_conflict_guard("/tmp/x.py", "old\n", "<<<<<<< A\n1\n=======\n2\n"
                                                     ">>>>>>> B\n",
                               /*append=*/true, /*file_existed=*/true,
                               /*allow_conflicts=*/true);
        expect(!r.error.has_value());
        expect(eq(r.note,
                  kimix::string(" Warning: appended content still contains 1 "
                                "unresolved conflict marker block(s) "
                                "(allow_conflicts=True).")));

        // Clean overwrite of a conflicted file: no error, old_had_blocks true.
        r = run_conflict_guard("/tmp/x.py",
                               "<<<<<<< A\n1\n=======\n2\n>>>>>>> B\n", "clean\n",
                               /*append=*/false, /*file_existed=*/true,
                               /*allow_conflicts=*/false);
        expect(!r.error.has_value());
        expect(r.old_had_blocks);
        expect(r.note.empty());
    };

    // ------------------------------------------------------------------
    // 4. JSON format validation
    // ------------------------------------------------------------------
    "json_valid"_test = [] {
        expect(!check_json_format("{\"a\": 1, \"b\": 2}").has_value());
        expect(!check_json_format("[1, 2, 3]").has_value());
        expect(!check_json_format("\"hello\"").has_value());
        expect(!check_json_format("null").has_value());
        expect(!check_json_format("123").has_value());
        expect(!check_json_format("true").has_value());
        expect(!check_json_format("{\"a\": \"\xE2\x80\x94\"}").has_value())
            << "unicode value valid";
        expect(!check_json_format("{\"nested\": [1, {\"x\": \"y\"}]}").has_value());
    };

    "json_invalid_diagnostics"_test = [] {
        // (text, expected line, expected column) - orjson golden positions.
        // Decision parity is mandatory; message wording is yyjson's (matches
        // orjson for these cases).
        struct case_t {
            const char *text;
            int line;
            int col;
        };
        const case_t cases[] = {
            {"{\"a\": 1 \"b\": 2}", 1, 9},
            {"{\"a\": 1,}", 1, 8},
            {"{\"a\": 1, \"b\": 2} extra", 1, 18},
            {"{\"a\": [1, 2", 1, 12},
            {"{\"a\": }", 1, 7},
            {"{a: 1}", 1, 2},
            {"{\"a\": 01}", 1, 7},
            {"{\n  \"a\": 1,\n  \"b\": [1, 2,\n}\n", 4, 1},
            {"{\"a\": \"unterminated}", 1, 21},
            {"{\"a\": \"line\nbreak\"}", 1, 12},
            {"   ", 1, 1},
            {"{\"a\": \xC3\xA9}", 1, 7}, // multi-byte char before error: col is code points
        };
        for (const auto &c : cases) {
            auto err = check_json_format(c.text);
            expect(err.has_value()) << "invalid JSON should error: " << c.text;
            if (err.has_value()) {
                const kimix::string prefix = kimix::format(
                    "JSON decode error at line {}, column {}: ", c.line, c.col);
                expect(err->starts_with(prefix))
                    << "line/col diagnostic for: " << c.text << " got: " << *err;
            }
        }
        // Exact full message for a canonical case (yyjson wording == orjson).
        auto err = check_json_format("{\"a\": 1, \"b\": 2} extra");
        expect(err.has_value());
        if (err.has_value()) {
            expect(eq(*err,
                      kimix::string("JSON decode error at line 1, column 18: "
                                    "unexpected content after document")));
        }
        err = check_json_format("");
        expect(err.has_value());
        if (err.has_value()) {
            expect(eq(*err,
                      kimix::string("JSON decode error at line 1, column 1: Input is "
                                    "a zero-length, empty document")));
        }
    };

    "format_validate_by_path"_test = [] {
        kimix::string fmt_error;
        // JSON invalid -> ok status + error message set (write still proceeds,
        // Python reports "File successfully written, but <fmt_error>").
        expect(validate_format_by_path("/tmp/a.json", "{bad", fmt_error) ==
               tool_status::ok);
        expect(!fmt_error.empty());
        // JSON valid -> ok + no message.
        expect(validate_format_by_path("/tmp/a.json", "{\"a\": 1}", fmt_error) ==
               tool_status::ok);
        expect(fmt_error.empty());
        // YAML/TOML/XML -> unsupported (Python-side validation).
        expect(validate_format_by_path("/tmp/a.yaml", "a: 1", fmt_error) ==
               tool_status::unsupported);
        expect(validate_format_by_path("/tmp/a.yml", "a: 1", fmt_error) ==
               tool_status::unsupported);
        expect(validate_format_by_path("/tmp/a.toml", "a=1", fmt_error) ==
               tool_status::unsupported);
        expect(validate_format_by_path("/tmp/a.xml", "<a/>", fmt_error) ==
               tool_status::unsupported);
        // No format gate for other extensions.
        expect(validate_format_by_path("/tmp/a.txt", "anything", fmt_error) ==
               tool_status::ok);
        expect(fmt_error.empty());
    };

    // ------------------------------------------------------------------
    // 5. unified diff / mkdir / verification / success messages
    // ------------------------------------------------------------------
    "unified_diff_goldens"_test = [] {
        struct case_t {
            const char *name;
            const char *old_text;
            const char *new_text;
            const char *expected; // full diff with file header
        };
        const case_t cases[] = {
            {"simple", "a\nb\nc\n", "a\nc\n",
             "--- a/test.py\n+++ b/test.py\n@@ -1,3 +1,2 @@\n a\n-b\n c\n"},
            {"append", "a\nb\n", "a\nb\nc\n",
             "--- a/test.py\n+++ b/test.py\n@@ -1,2 +1,3 @@\n a\n b\n+c\n"},
            {"no_trailing_newline", "a\nb", "a\nb\nc",
             "--- a/test.py\n+++ b/test.py\n@@ -1,2 +1,3 @@\n a\n b\n+c\n"},
            {"empty_old", "", "x\ny\n",
             "--- a/test.py\n+++ b/test.py\n@@ -0,0 +1,2 @@\n+x\n+y\n"},
            {"empty_new", "x\ny\n", "",
             "--- a/test.py\n+++ b/test.py\n@@ -1,2 +0,0 @@\n-x\n-y\n"},
            {"replace_same_len", "1\n2\n3\n4\n", "1\n2\n3\nX\n",
             "--- a/test.py\n+++ b/test.py\n@@ -1,4 +1,4 @@\n 1\n 2\n 3\n-4\n+X\n"},
            {"insert_middle", "1\n2\n4\n5\n", "1\n2\n3\n4\n5\n",
             "--- a/test.py\n+++ b/test.py\n@@ -1,4 +1,5 @@\n 1\n 2\n+3\n 4\n 5\n"},
        };
        for (const auto &c : cases) {
            expect(eq(build_unified_diff(c.old_text, c.new_text, "test.py", true),
                      kimix::string(c.expected)))
                << c.name;
        }
        // Identical text -> empty diff.
        expect(build_unified_diff("x\n", "x\n", "test.py", true).empty());

        // Multi-hunk case (3 context lines).
        const char *old_m =
            "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n";
        const char *new_m =
            "a\nb\nc\nd\ne\nf\nX\ng\nh\ni\nj\nk\nl\nm\nn\no\nY\n";
        const kimix::string expected_m =
            "--- a/test.py\n+++ b/test.py\n"
            "@@ -4,6 +4,7 @@\n d\n e\n f\n+X\n g\n h\n i\n"
            "@@ -13,4 +14,4 @@\n m\n n\n o\n-p\n+Y\n";
        expect(eq(build_unified_diff(old_m, new_m, "test.py", true), expected_m));

        // include_file_header=false drops the ---/+++ lines.
        const kimix::string no_header =
            "@@ -1,3 +1,2 @@\n a\n-b\n c\n";
        expect(eq(build_unified_diff("a\nb\nc\n", "a\nc\n", "test.py", false),
                  no_header));
    };

    "mkdir_and_verification_messages"_test = [] {
        // Parent exists -> proceed.
        auto d = decide_parent_dir(true, false, "/tmp/a/b.txt", "/tmp/a", {});
        expect(d.status == tool_status::ok);
        expect(d.message.empty());

        // mkdir=False and parent missing -> exact refusal.
        d = decide_parent_dir(false, false, "/tmp/a/b.txt", "/tmp/a", {});
        expect(d.status == tool_status::not_found);
        expect(eq(d.message,
                  kimix::string("Parent directory does not exist: /tmp/a. Set "
                                "mkdir=True to create it.")));

        // mkdir=True and creation succeeded -> proceed.
        d = decide_parent_dir(false, true, "/tmp/a/b.txt", "/tmp/a", {});
        expect(d.status == tool_status::ok);

        // mkdir=True but creation failed -> exact refusal.
        d = decide_parent_dir(false, true, "/tmp/a/b.txt", "/tmp/a",
                              kimix::optional<kimix::string>("permission denied"));
        expect(d.status == tool_status::invalid_input);
        expect(eq(d.message,
                  kimix::string("Failed to create parent directory for /tmp/a/b.txt: "
                                "permission denied")));

        // Verification failure messages (with and without out-of-workdir prefix).
        expect(eq(verification_failed_error("/tmp/a.txt", "boom", false),
                  kimix::string("Write verification failed for /tmp/a.txt: boom.")));
        expect(eq(verification_failed_error("/tmp/a.txt", "boom", true),
                  kimix::string("[out of work-dir] Write verification failed for "
                                "/tmp/a.txt: boom.")));
        expect(eq(size_mismatch_error("/tmp/a.txt", 10, 8, false),
                  kimix::string("Write verification failed (size mismatch): expected "
                                "10 bytes, got 8 bytes. Path: /tmp/a.txt")));
        expect(eq(size_mismatch_error("/tmp/a.txt", 10, 8, true),
                  kimix::string("[out of work-dir] Write verification failed (size "
                                "mismatch): expected 10 bytes, got 8 bytes. Path: "
                                "/tmp/a.txt")));
    };

    "success_messages"_test = [] {
        expect(eq(success_message("/tmp/x.py", 12, "overwritten", "", ""),
                  kimix::string("File successfully overwritten. Current size: 12 "
                                "bytes. Path: /tmp/x.py Verified: size matches.")));
        expect(eq(success_message("/tmp/x.py", 5, "appended to", "", ""),
                  kimix::string("File successfully appended to. Current size: 5 "
                                "bytes. Path: /tmp/x.py Verified: size matches.")));
        // Conflict note + drift note are appended verbatim (notes carry their
        // own leading space).
        expect(eq(success_message("/tmp/x.py", 3, "overwritten",
                                  " Warning: written content still contains 1 "
                                  "unresolved conflict marker block(s) "
                                  "(allow_conflicts=True).",
                                  "Note: file changed since last read; snapshot "
                                  "recorded."),
                  kimix::string("File successfully overwritten. Current size: 3 "
                                "bytes. Path: /tmp/x.py Verified: size matches. "
                                "Warning: written content still contains 1 "
                                "unresolved conflict marker block(s) "
                                "(allow_conflicts=True). Note: file changed since "
                                "last read; snapshot recorded.")));
        // Conflict resolution message + boundary-echo note.
        expect(eq(conflict_resolved_message(1, 2, 6, "/tmp/x.py", 0),
                  kimix::string("Resolved conflict #1 at line(s) L2-6 in /tmp/x.py.")));
        expect(eq(conflict_resolved_message(1, 2, 6, "/tmp/x.py", 3),
                  kimix::string("Resolved conflict #1 at line(s) L2-6 in /tmp/x.py. "
                                "Note: dropped 3 content line(s) that duplicated the "
                                "code adjacent to the conflict region "
                                "(boundary-echo repair).")));
    };

    // ------------------------------------------------------------------
    // 7. Tool class integration (Write::operator())
    // ------------------------------------------------------------------

    "write_operator_null_params"_test = [] {
        Write w(nullptr);
        w(nullptr);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("invalid_input")));
        expect(eq(res.values.at("message").as_string(), kimix::string("missing parameters")));
    };

    "write_operator_missing_content"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("invalid_input")));
    };

    "write_operator_invalid_mode"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] = ValueElement::make_string("hello");
        p.values["mode"] = ValueElement::make_string("bogus");
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("invalid_input")));
    };

    "write_operator_empty_path"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("");
        p.values["content"] = ValueElement::make_string("hello");
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("invalid_input")));
    };

    "write_operator_invalid_utf8"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] = ValueElement::make_string("\x80");
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("invalid_input")));
    };

    "write_operator_auto_generated"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/zz_generated.py");
        p.values["content"] = ValueElement::make_string("new");
        p.values["file_existed"] = ValueElement::make_bool(true);
        p.values["old_text"] = ValueElement::make_string("old");
        Write w(nullptr);
        w(&p);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("blocked")));
        expect(res.values.at("message").as_string().find(
                   "Cannot modify auto-generated file") != kimix::string::npos);
    };

    "write_operator_conflict_markers"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] =
            ValueElement::make_string("<<<<<<< A\n1\n=======\n2\n>>>>>>> B\n");
        p.values["mode"] = ValueElement::make_string("overwrite");
        Write w(nullptr);
        w(&p);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("blocked")));
        expect(res.values.at("message").as_string().find("refusing to write") !=
               kimix::string::npos);
    };

    "write_operator_parent_missing"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] = ValueElement::make_string("hello");
        p.values["mkdir"] = ValueElement::make_bool(false);
        p.values["parent_exists"] = ValueElement::make_bool(false);
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("not_found")));
    };

    "write_operator_unsupported_format"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.yaml");
        p.values["content"] = ValueElement::make_string("a: 1");
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("unsupported")));
    };

    "write_operator_success_overwrite"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] = ValueElement::make_string("new\ncontent\n");
        p.values["mode"] = ValueElement::make_string("overwrite");
        Write w(nullptr);
        w(&p);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("ok")));
        expect(eq(res.values.at("new_text").as_string(),
                  kimix::string("new\ncontent\n")));
        expect(eq(res.values.at("expected_size").as_uint(), uint64_t(12)));
        expect(res.values.at("output").as_string().empty());
        expect(res.values.at("message").as_string().find("File successfully overwritten") !=
               kimix::string::npos);
    };

    "write_operator_success_append"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.txt");
        p.values["content"] = ValueElement::make_string("extra");
        p.values["mode"] = ValueElement::make_string("append");
        p.values["file_existed"] = ValueElement::make_bool(true);
        p.values["old_text"] = ValueElement::make_string("base\n");
        Write w(nullptr);
        w(&p);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("ok")));
        expect(eq(res.values.at("new_text").as_string(), kimix::string("base\nextra")));
        expect(eq(res.values.at("expected_size").as_uint(), uint64_t(10)));
        expect(res.values.at("message").as_string().find("appended to") !=
               kimix::string::npos);
    };

    "write_operator_show_diff"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("x.txt");
        p.values["content"] = ValueElement::make_string("b\nc\n");
        p.values["mode"] = ValueElement::make_string("overwrite");
        p.values["old_text"] = ValueElement::make_string("a\nb\n");
        p.values["show_diff"] = ValueElement::make_bool(true);
        Write w(nullptr);
        w(&p);
        const ToolParams &res = w.last_result();
        expect(eq(res.values.at("status").as_string(), kimix::string("ok")));
        expect(!res.values.at("output").as_string().empty());
        expect(res.values.at("output").as_string().find("@@") != kimix::string::npos);
    };

    "write_operator_json_fmt_error"_test = [] {
        ToolParams p;
        p.values["file_path"] = ValueElement::make_string("/tmp/x.json");
        p.values["content"] = ValueElement::make_string("{bad");
        p.values["auto_fix_json"] = ValueElement::make_bool(false);
        Write w(nullptr);
        w(&p);
        expect(eq(w.last_result().values.at("status").as_string(),
                  kimix::string("invalid_input")));
    };
}
