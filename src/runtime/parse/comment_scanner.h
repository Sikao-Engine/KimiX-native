/*
 * comment_scanner.h - Comment extraction for 7 languages (kimix::runtime::parse).
 *
 * Plan 011: native port of the seven per-character Python comment parsers in
 * kimi-agent src/kimix/parser/ (c_parser.py, py_parser.py, shell_parser.py,
 * sql_parser.py, html_parser.py, lisp_parser.py, pascal_parser.py). One core
 * per-language state machine; results are byte-offset SPANS into the input
 * (the Python shim slices content and computes 1-based line/column by code
 * point, mirroring the reference Comment contract exactly).
 *
 * Span semantics (verified against the reference sources):
 *   C      line   [start+2, newline/EOF)   content excludes the line marker
 *          block  [start+2, '*')           content excludes the block markers
 *          doc    [start+3, '*')           content excludes the doc markers;
 *                 the empty doc form is a BLOCK (empty), not a doc comment
 *   Python line   [start, newline/EOF)     content INCLUDES '#'
 *          doc    [quote_start, after closing triple quote) or [start, EOF)
 *                 content INCLUDES the quotes; only triple-quoted strings
 *                 WITHOUT a prefix (r,b,f,rb,br,rf,fr) become doc comments
 *   Shell  line   [start, newline/EOF)     content INCLUDES '#'; '#!' on
 *                 line 1 -> kind doc (shebang)
 *   SQL    line   dash [start+2, newline/EOF), hash [start+1, newline/EOF);
 *                 dash requires a following space/tab/newline/CR/EOF; content
 *                 excludes the marker but INCLUDES a trailing '\r'
 *          block  [start+2, '*')           content excludes the block markers;
 *                 NESTED block pairs (PostgreSQL style) are in the content
 *   HTML   block  [start+4, '-')           content excludes "<!--" "-->"
 *          doc    [start+2, '?')           processing instructions "<? ... ?>"
 *                 Unclosed constructs at EOF emit NO comment (HTML only)
 *   Lisp   line   [start, newline/EOF)     content INCLUDES ';'
 *          block  [start, '|' + 2)         content INCLUDES "#|" and "|#";
 *                 NOT nested - the first "|#" closes (matches reference)
 *   Pascal brace  [start+1, '}')           content excludes "{" "}"
 *          paren  [start+2, '*')           content excludes "(*" "*)"
 *          line   [start+2, newline/EOF)   content excludes "//"
 *
 * kind: 0 = line, 1 = block, 2 = doc.
 *
 * Unicode note: the C-family regex-literal heuristic and the Lisp "#\name"
 * character-literal scan use Unicode isalnum/isspace/isalpha in the Python
 * reference. The kernel implements the ASCII subset (bytes >= 0x80 count as
 * non-space, non-alnum, non-alpha). The Python shim routes C and LISP input
 * with any non-ASCII byte to the _compat mirror, so end-to-end parity holds
 * for every input; Python/Shell/SQL/HTML/Pascal scanners contain no
 * Unicode-sensitive decisions and run natively on any UTF-8 input.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace parse {

enum class lang_kind : uint8_t { C, PYTHON, SHELL, SQL, HTML, LISP, PASCAL_LANG };

struct lang_rules {
    bool line_comments;   // // , # , ; , -- (per language)
    bool block_comments;  // /* */ , (* *) , <!-- --> , #| |# , { }
    bool strings_aware;   // skip content inside "..." '...' (with escapes)
    bool raw_strings;     // python r"..." / triple quotes; rust r#"..."#
    bool doc_comments;    // /** , python triple-quote, shebang, sql variant
};

struct comment_span {
    uint32_t start;  // byte offset of the content (markers excluded per table)
    uint32_t end;    // byte offset one past the content
    uint32_t kind;   // 0 = line, 1 = block, 2 = doc
};

// Per-language rule table (informational; the scanners implement the exact
// reference state machines, including every quirk above).
KIMIX_RUNTIME_API lang_rules rules_for(lang_kind lang) noexcept;

// One pass; appends every comment span in source order to `out` (UTF-8 byte
// offsets into `input`). `out` is cleared first.
KIMIX_RUNTIME_API void scan_comments(lang_kind lang, kimix::string_view input,
                                     kimix::vector<comment_span>& out);

} // namespace parse
} // namespace runtime
} // namespace kimix
