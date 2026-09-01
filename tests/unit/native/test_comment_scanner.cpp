// Test for src/runtime/parse/comment_scanner.h (plan 011 comment parsers).
// This test covers:
// - per-language golden spans (content extents) for all 7 languages
// - edge cases from the reference conformance matrix: C regex/raw strings,
//   python f-strings and triple quotes, shell heredocs/backticks/$(), SQL
//   nested block comments and "--" vs "--x", HTML unclosed comments, Lisp
//   "#\;" character literals, Pascal { } / (* *) / //

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/parse/comment_scanner.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::parse;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

// content of a span in the input
std::string content_of(const std::string& src, const comment_span& sp) {
    return src.substr(sp.start, sp.end - sp.start);
}
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "c_line_block_doc"_test = [] {
        const std::string src = "// hi\nint x; /* b */\n/** doc */\nchar *u = \"http://x\";\n// tail";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(src), spans);
        expect(eq(spans.size(), 4u));
        expect(eq(content_of(src, spans[0]), std::string(" hi")));
        expect(eq(spans[0].kind, 0u));
        expect(eq(content_of(src, spans[1]), std::string(" b ")));
        expect(eq(spans[1].kind, 1u));
        expect(eq(content_of(src, spans[2]), std::string(" doc ")));
        expect(eq(spans[2].kind, 2u));
        expect(eq(content_of(src, spans[3]), std::string(" tail")));
        expect(eq(spans[3].kind, 0u));
        // URL string: "//" inside a string is NOT a comment
        const std::string url = "char *u = \"http://example.com/path\";\n";
        spans.clear();
        scan_comments(lang_kind::C, sv(url), spans);
        expect(spans.empty());
    };

    "c_empty_doc_and_regex"_test = [] {
        // "/**/" is a BLOCK (empty content), not a doc comment
        const std::string e = "/**/";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(e), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(spans[0].kind, 1u));
        expect(eq(spans[0].start, spans[0].end));
        // regex literal: var re = /a\/b/;  -- the // inside is regex content
        const std::string re = "var re = /a\\/b/; // real\n";
        spans.clear();
        scan_comments(lang_kind::C, sv(re), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(re, spans[0]), std::string(" real")));
    };

    "c_raw_string_and_comment_in_string"_test = [] {
        // Rust raw string r#"..."# - "//" inside is not a comment
        const std::string raw = "let s = r#\"a // b\"#; /* c */\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(raw), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(raw, spans[0]), std::string(" c ")));
        // unclosed comment at EOF is emitted
        const std::string un = "x /* never closed";
        spans.clear();
        scan_comments(lang_kind::C, sv(un), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(un, spans[0]), std::string(" never closed")));
    };

    "python_line_and_docstrings"_test = [] {
        const std::string src = "x = \"# not\"\n# real\n\"\"\"doc\"\"\"\n'''sd'''\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::PYTHON, sv(src), spans);
        expect(eq(spans.size(), 3u));
        expect(eq(content_of(src, spans[0]), std::string("# real")));
        expect(eq(spans[0].kind, 0u));
        expect(eq(content_of(src, spans[1]), std::string("\"\"\"doc\"\"\"")));
        expect(eq(spans[1].kind, 2u));
        expect(eq(content_of(src, spans[2]), std::string("'''sd'''")));
        expect(eq(spans[2].kind, 2u));
        // hash inside a string and inside an f-string expression is NOT a comment
        const std::string f = "url = \"http://x#f\"\ny = f\"{1 + 1} # no\" # yes\n";
        spans.clear();
        scan_comments(lang_kind::PYTHON, sv(f), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(f, spans[0]), std::string("# yes")));
    };

    "python_prefix_and_unclosed"_test = [] {
        // prefixed triple-quoted strings are NOT doc comments
        const std::string p = "x = r\"\"\"raw\"\"\"\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::PYTHON, sv(p), spans);
        expect(spans.empty());
        // unclosed line comment at EOF
        const std::string u = "a = 1\n# tail";
        spans.clear();
        scan_comments(lang_kind::PYTHON, sv(u), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(u, spans[0]), std::string("# tail")));
        // unclosed triple-quoted docstring at EOF (no prefix)
        const std::string t = "a = 1\n\"\"\"unclosed";
        spans.clear();
        scan_comments(lang_kind::PYTHON, sv(t), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(t, spans[0]), std::string("\"\"\"unclosed")));
        expect(eq(spans[0].kind, 2u));
    };

    "shell_comments_and_heredoc"_test = [] {
        const std::string src =
            "#!/bin/bash\necho hi # c\ncat <<EOF\n# not a comment\nEOF\nx=$(echo '# sub') # after\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::SHELL, sv(src), spans);
        // shebang (doc), "# c" (line), "# after" (line); "# not a comment" is
        // heredoc content; "# sub" is inside single quotes (not a comment)
        expect(eq(spans.size(), 3u));
        expect(eq(content_of(src, spans[0]), std::string("#!/bin/bash")));
        expect(eq(spans[0].kind, 2u));
        expect(eq(content_of(src, spans[1]), std::string("# c")));
        expect(eq(spans[1].kind, 0u));
        expect(eq(content_of(src, spans[2]), std::string("# after")));
        expect(eq(spans[2].kind, 0u));
        // '#' inside $(...) IS a comment (comment runs to end of line)
        const std::string sub = "x=$(echo # sub)\n";
        spans.clear();
        scan_comments(lang_kind::SHELL, sv(sub), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(sub, spans[0]), std::string("# sub)")));
        // quote awareness: "#" inside double quotes is not a comment
        const std::string q = "echo \"http://x#f\"\n";
        spans.clear();
        scan_comments(lang_kind::SHELL, sv(q), spans);
        expect(spans.empty());
    };

    "sql_nested_and_dash_rules"_test = [] {
        const std::string src = "SELECT 1 -- c\n--x\nSELECT 'str -- not'\n/* a /* nested */ b */\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::SQL, sv(src), spans);
        expect(eq(spans.size(), 2u));
        // "-- c" (dash comment; content excludes "--")
        expect(eq(content_of(src, spans[0]), std::string(" c")));
        // "--x" (no trailing space) is NOT a comment
        // "str -- not" inside quotes is not a comment
        expect(eq(content_of(src, spans[1]), std::string(" a /* nested */ b ")));
        expect(eq(spans[1].kind, 1u));
        // "#" mysql comment
        const std::string h = "SELECT 1 # note\n";
        spans.clear();
        scan_comments(lang_kind::SQL, sv(h), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(h, spans[0]), std::string(" note")));
        // "--" at EOF is a comment (EOF counts as terminator)
        const std::string e = "SELECT 1 --";
        spans.clear();
        scan_comments(lang_kind::SQL, sv(e), spans);
        expect(eq(spans.size(), 1u));
        expect(content_of(e, spans[0]).empty());
    };

    "html_comments_pi_cdata"_test = [] {
        const std::string src = "<!-- a -->\n<?xml?>\n<![CDATA[<!-- not -->]]>\n<div a='<!-- nope -->'></div>\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::HTML, sv(src), spans);
        expect(eq(spans.size(), 2u));
        expect(eq(content_of(src, spans[0]), std::string(" a ")));
        expect(eq(spans[0].kind, 1u));
        expect(eq(content_of(src, spans[1]), std::string("xml")));
        expect(eq(spans[1].kind, 2u));
        // unclosed comment at EOF emits NOTHING (HTML only)
        const std::string u = "<!-- unclosed";
        spans.clear();
        scan_comments(lang_kind::HTML, sv(u), spans);
        expect(spans.empty());
    };

    "lisp_char_literal_and_block"_test = [] {
        const std::string src = "; line\n#| block |#\n#\\; char\n\"str ; no\"\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::LISP, sv(src), spans);
        expect(eq(spans.size(), 2u));
        expect(eq(content_of(src, spans[0]), std::string("; line")));
        expect(eq(spans[0].kind, 0u));
        // content INCLUDES the #| and |# markers
        expect(eq(content_of(src, spans[1]), std::string("#| block |#")));
        expect(eq(spans[1].kind, 1u));
        // NOT nested: the first |# closes
        const std::string n = "#| a #| b |# c |#\n";
        spans.clear();
        scan_comments(lang_kind::LISP, sv(n), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(n, spans[0]), std::string("#| a #| b |#")));
    };

    "pascal_brace_paren_line"_test = [] {
        const std::string src = "{ a }\n(* b *)\n// c\ns := '{ not }';\n";
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::PASCAL_LANG, sv(src), spans);
        expect(eq(spans.size(), 3u));
        expect(eq(content_of(src, spans[0]), std::string(" a ")));
        expect(eq(spans[0].kind, 1u));
        expect(eq(content_of(src, spans[1]), std::string(" b ")));
        expect(eq(spans[1].kind, 1u));
        expect(eq(content_of(src, spans[2]), std::string(" c")));
        expect(eq(spans[2].kind, 0u));
        // unclosed at EOF
        const std::string u = "x := 1; (* open";
        spans.clear();
        scan_comments(lang_kind::PASCAL_LANG, sv(u), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(content_of(u, spans[0]), std::string(" open")));
    };

    "rules_for_table"_test = [] {
        expect(rules_for(lang_kind::C).line_comments);
        expect(rules_for(lang_kind::C).block_comments);
        expect(rules_for(lang_kind::PYTHON).raw_strings);
        expect(!rules_for(lang_kind::HTML).line_comments);
        expect(rules_for(lang_kind::HTML).block_comments);
        expect(!rules_for(lang_kind::LISP).doc_comments);
        expect(rules_for(lang_kind::PASCAL_LANG).line_comments);
    };

    // ------------------------------------------------------------------
    // Benchmarks (kimix_bench harness; timings on stderr as [bench] ...).
    // Each case reuses the correctness expectations above as guards: we
    // only time scans that produce the expected span counts.
    // ------------------------------------------------------------------

    "bench_comment_c_realistic_1mb"_test = [] {
        std::string src;
        src.reserve(1u << 20);
        const char* block =
            "#include <stdio.h>\n"
            "// parse_config: load and validate settings\n"
            "#define BUFSZ 256\n"
            "int main(void) {\n"
            "    /* read the input file */\n"
            "    const char *path = \"/etc/app.conf\";  // default\n"
            "    char buf[BUFSZ];\n"
            "    /** doc: main loop */\n"
            "    while (fgets(buf, sizeof buf, stdin) != 0) {\n"
            "        // skip http://example.com style urls\n"
            "        if (buf[0] == '#') continue;\n"
            "        printf(\"%s\", buf);\n"
            "    }\n"
            "    return 0; /* done */\n"
            "}\n";
        size_t blocks = 0;
        while (src.size() < (size_t{1} << 20)) {
            src += block;
            ++blocks;
        }
        // 6 comments per block (2 line, 2 block, 1 doc, 1 line in string guard)
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(src), spans);
        expect(eq(spans.size(), blocks * 6));
        expect(eq(spans[0].kind, 0u));
        kimix_bench::run("comment/scan_c_realistic_1mb",
                         [&] { scan_comments(lang_kind::C, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_c_dense_lines_1mb"_test = [] {
        // Worst case for span emission: ~1 MiB of short line comments
        // (262144 spans). Also stresses the LINE_COMMENT scan run.
        constexpr size_t kLineLen = 4; // "//c\n"
        const size_t lines = (size_t{1} << 20) / kLineLen;
        std::string src;
        src.reserve(lines * kLineLen);
        for (size_t i = 0; i < lines; ++i) {
            src += "//c\n";
        }
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(src), spans);
        expect(eq(spans.size(), lines));
        expect(eq(content_of(src, spans[0]), std::string("c")));
        expect(eq(content_of(src, spans[lines - 1]), std::string("c")));
        kimix_bench::run("comment/scan_c_dense_lines_1mb",
                         [&] { scan_comments(lang_kind::C, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_python_realistic_1mb"_test = [] {
        std::string src;
        src.reserve(1u << 20);
        const char* block =
            "import os\n"
            "# top-level comment\n"
            "def load(path):\n"
            "    \"\"\"docstring for load\"\"\"\n"
            "    data = open(path).read()  # read it\n"
            "    url = \"http://example.com/x#frag\"  # hash in string\n"
            "    return f\"{len(data)} bytes\"  # f-string\n"
            "# tail\n";
        size_t blocks = 0;
        while (src.size() < (size_t{1} << 20)) {
            src += block;
            ++blocks;
        }
        // 5 line comments + 1 docstring per block
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::PYTHON, sv(src), spans);
        expect(eq(spans.size(), blocks * 6));
        expect(eq(spans[0].kind, 0u));
        expect(eq(spans[1].kind, 2u));
        kimix_bench::run("comment/scan_python_realistic_1mb",
                         [&] { scan_comments(lang_kind::PYTHON, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_html_realistic_1mb"_test = [] {
        std::string src;
        src.reserve(1u << 20);
        const char* block =
            "<!DOCTYPE html>\n"
            "<html><head><!-- meta --><title>t</title></head>\n"
            "<body class='main'>\n"
            "<!-- body comment -->\n"
            "<?pi data?>\n"
            "<![CDATA[<b>raw <!-- not a comment --></b>]]>\n"
            "<div data-a=\"x\">text</div>\n"
            "</body></html>\n";
        size_t blocks = 0;
        while (src.size() < (size_t{1} << 20)) {
            src += block;
            ++blocks;
        }
        // 2 block comments + 1 PI doc per block; CDATA content is skipped
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::HTML, sv(src), spans);
        expect(eq(spans.size(), blocks * 3));
        expect(eq(spans[0].kind, 1u));
        expect(eq(spans[1].kind, 1u));
        kimix_bench::run("comment/scan_html_realistic_1mb",
                         [&] { scan_comments(lang_kind::HTML, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_sql_realistic_1mb"_test = [] {
        std::string src;
        src.reserve(1u << 20);
        const char* block =
            "SELECT id, name -- c1\n"
            "FROM users /* a /* nested */ b */ WHERE id = 'x--y'\n"
            "DELETE FROM t # c2\n"
            "UPDATE u SET v = 1; -- end\n";
        size_t blocks = 0;
        while (src.size() < (size_t{1} << 20)) {
            src += block;
            ++blocks;
        }
        // 2 dash line comments + 1 hash line + 1 nested block per block
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::SQL, sv(src), spans);
        expect(eq(spans.size(), blocks * 4));
        expect(eq(spans[1].kind, 1u));
        kimix_bench::run("comment/scan_sql_realistic_1mb",
                         [&] { scan_comments(lang_kind::SQL, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_c_strings_heavy_1mb"_test = [] {
        // Adversarial: most bytes live inside double-quoted string runs.
        std::string src;
        src.reserve(1u << 20);
        const char* block =
            "const char *s = \"some longish literal text with # hash and "
            "/* not a comment */ inside the string\";\n";
        while (src.size() < (size_t{1} << 20)) {
            src += block;
        }
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(src), spans);
        expect(spans.empty());
        kimix_bench::run("comment/scan_c_strings_heavy_1mb",
                         [&] { scan_comments(lang_kind::C, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_c_unterminated_block_1mb"_test = [] {
        // Adversarial: one giant unterminated block comment (raw scan run to
        // EOF; exactly one span emitted).
        std::string src = "/*";
        src.append((size_t{1} << 20) - 2, 'a');
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::C, sv(src), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(spans[0].kind, 1u));
        expect(eq(spans[0].start, 2u));
        expect(eq(spans[0].end, src.size()));
        kimix_bench::run("comment/scan_c_unterminated_block_1mb",
                         [&] { scan_comments(lang_kind::C, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };

    "bench_comment_python_unterminated_triple_1mb"_test = [] {
        // Adversarial: unclosed triple-quoted string scanned to EOF.
        std::string src = "\"\"\"";
        src.append((size_t{1} << 20) - 3, 'a');
        kimix::vector<comment_span> spans;
        scan_comments(lang_kind::PYTHON, sv(src), spans);
        expect(eq(spans.size(), 1u));
        expect(eq(spans[0].kind, 2u));
        expect(eq(spans[0].end, src.size()));
        kimix_bench::run("comment/scan_python_unterminated_triple_1mb",
                         [&] { scan_comments(lang_kind::PYTHON, sv(src), spans); },
                         1, static_cast<double>(src.size()));
        kimix_bench::sink(spans.size());
    };
}
