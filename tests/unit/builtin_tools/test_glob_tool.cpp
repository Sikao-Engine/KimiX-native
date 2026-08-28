// Test for builtin_tools/glob_tool.h (namespace kimix::builtin_tools::glob).
// This test covers the plan's §7 matrix (C:/dev/kimi-agent/plans/glob.md):
// - fnmatch core: literal/*/?/[..]/[!..], unterminated '[', ']' as first
//   member, ranges and reversed ranges, trailing '-', consecutive '*' collapse,
//   literal backslashes, '*' matching '/' (full-string), case-insensitive flag
// - path-glob parsing: '/' split, '.' and empty segments dropped, absolute /
//   empty pattern errors, trailing-slash (dir_only), anchored vs basename
// - `**`: zero-level (a/**/b vs a/b), multi-level, leading '**/', trailing
//   '/**', dotfiles, non-followed symlinked directories
// - is_unsafe_recursive_pattern (**, **/*, **/**, **\*, and the negatives)
// - ignore filter: parse rules, dir-only descendants, negation, anchored,
//   leading '/', '**' rules, multi-source_dir scoping
// - walker (in-memory tree via the injectable lister/stat probes):
//   include_dirs on/off, max_matches cap + truncated flag, deadline abort,
//   permission-denied listing skip, ignore filter-after-walk, dir pruning
//   (asserted by counting visited directories), deterministic sorted output
// - real filesystem wrapper walk_matches_fs() against a temp tree it creates
// - result shaping: sort/dedup/strip_prefix/mtime top-k/pagination, verbose
//   line building, byte cap, head+tail fold, top-dirs summary and the exact
//   Glob message assembly
#include "ut/ut.hpp"

#include "builtin_tools/glob_tool.h"
#include "builtin_tools/utf8_util.h"

#include <core/kimix_core.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::glob;

namespace {

// ---------------------------------------------------------------------------
// In-memory tree used by the walker tests.
// ---------------------------------------------------------------------------

// kimix::string from std::string / const char* (test-side convenience).
kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}
kimix::string kix(const char *sv) { return kimix::string(sv); }

struct mem_dir {
    std::string rel;             // '' == root
    std::vector<dirent_info> entries;
    bool listable = true;        // false simulates EACCES / vanished
};

struct mem_tree {
    std::vector<mem_dir> dirs;
    std::vector<std::pair<std::string, entry_stat>> stats;

    mem_dir &add_dir(const std::string &rel) {
        for (auto &d : dirs) {
            if (d.rel == rel) {
                return d;
            }
        }
        dirs.push_back(mem_dir{rel, {}, true});
        return dirs.back();
    }

    // Make sure every ancestor prefix of `rel_path` exists as a directory
    // listing *and* as an entry of its own parent.
    void ensure_ancestors(const std::string &rel_path) {
        if (rel_path.empty()) {
            add_dir("");
            return;
        }
        const std::string parent = parent_of(rel_path);
        ensure_ancestors(parent);
        bool has = false;
        for (const auto &e : add_dir(parent).entries) {
            if (std::string(e.name.data(), e.name.size()) == base_of(rel_path)) {
                has = true;
                break;
            }
        }
        if (!has) {
            add_dir(parent).entries.push_back(
                dirent_info{kix(base_of(rel_path)), true, false});
        }
        add_dir(rel_path);
    }

    void add_file(const std::string &rel_path, int64_t size = 1,
                  double mtime = 0.0) {
        ensure_ancestors(parent_of(rel_path));
        add_dir(parent_of(rel_path)).entries.push_back(
            dirent_info{kix(base_of(rel_path)), false, false});
        stats.emplace_back(rel_path, entry_stat{false, size, mtime});
    }

    void add_subdir(const std::string &rel_path, bool symlink = false) {
        ensure_ancestors(parent_of(rel_path));
        add_dir(parent_of(rel_path)).entries.push_back(
            dirent_info{kix(base_of(rel_path)), true, symlink});
        add_dir(rel_path);
        stats.emplace_back(rel_path, entry_stat{true, -1, 0.0});
    }

    void set_unlistable(const std::string &rel) { add_dir(rel).listable = false; }

    list_dir_fn lister() const {
        return [this](kimix::string_view dir_rel,
                      kimix::vector<dirent_info> &out) -> tool_error {
            const std::string key(dir_rel.data(), dir_rel.size());
            for (const auto &d : dirs) {
                if (d.rel == key) {
                    if (!d.listable) {
                        return tool_error{
                            tool_status::blocked,
                            kimix::string("permission denied")};
                    }
                    for (const auto &e : d.entries) {
                        out.push_back(e);
                    }
                    return tool_error{};
                }
            }
            return tool_error{tool_status::not_found,
                              kimix::string("no such directory")};
        };
    }

    stat_fn probe() const {
        return [this](kimix::string_view rel, entry_stat &out) -> bool {
            const std::string key(rel.data(), rel.size());
            for (const auto &kv : stats) {
                if (kv.first == key) {
                    out = kv.second;
                    return true;
                }
            }
            return false;
        };
    }

    static std::string parent_of(const std::string &p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos ? std::string{} : p.substr(0, s);
    }
    static std::string base_of(const std::string &p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos ? p : p.substr(s + 1);
    }
};

// Collect the rel_path of every entry into std::string for easy comparisons.
std::vector<std::string> rel_paths(const walk_result &r) {
    std::vector<std::string> out;
    out.reserve(r.entries.size());
    for (const auto &e : r.entries) {
        out.emplace_back(e.rel_path.data(), e.rel_path.size());
    }
    return out;
}

std::vector<std::string> v(std::initializer_list<const char *> in) {
    std::vector<std::string> out;
    for (auto s : in) {
        out.emplace_back(s);
    }
    return out;
}

path_glob_pattern must_parse(std::string_view pattern,
                             bool case_insensitive = false) {
    path_glob_pattern pat;
    const auto err =
        parse_pattern(kimix::string_view(pattern.data(), pattern.size()),
                      case_insensitive, pat);
    expect(!err.failed()) << "pattern must parse: " << pattern;
    return pat;
}

bool matches(std::string_view pattern, std::string_view path,
             bool case_insensitive = false) {
    const auto pat = must_parse(pattern, case_insensitive);
    return match_path(pat, kimix::string_view(path.data(), path.size()));
}

std::vector<std::string> to_std(const kimix::vector<kimix::string> &in) {
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

    // ------------------------------------------------------------------
    // §3.1 fnmatch core
    // ------------------------------------------------------------------
    "fnmatch_literals_and_wildcards"_test = [] {
        expect(fnmatch_match("", "", false)) << "empty pattern matches empty";
        expect(!fnmatch_match("", "a", false)) << "empty pattern is full-match";
        expect(fnmatch_match("*", "", false)) << "'*' matches the empty string";
        expect(!fnmatch_match("?", "", false)) << "'?' needs one character";
        expect(fnmatch_match("setup.py", "setup.py", false));
        expect(!fnmatch_match("setup.py", "setup.pyc", false));
        expect(fnmatch_match("*.py", "setup.py", false));
        expect(fnmatch_match("*.py", ".hidden.py", false))
            << "dotfiles are not special (pathlib/fnmatch, not shells)";
        expect(!fnmatch_match("*.py", "a.PY", false)) << "case-sensitive flag";
        expect(fnmatch_match("*.py", "a.PY", true)) << "ASCII case folding";
        expect(fnmatch_match("*.py", "a/B.py", false))
            << "the core is not segment aware: '*' crosses '/'";
        expect(fnmatch_match("a*b", "ab", false))
            << "'**' collapses into one '*' (CPython translate)";
        expect(fnmatch_match("a**b", "aXXXb", false));
        expect(fnmatch_match("?x?", "axb", false));
        expect(!fnmatch_match("?x?", "axxb", false));
    };

    "fnmatch_brackets"_test = [] {
        expect(fnmatch_match("[a-c].py", "b.py", false));
        expect(!fnmatch_match("[a-c].py", "d.py", false));
        expect(fnmatch_match("[!a-c].py", "d.py", false));
        expect(!fnmatch_match("[!a-c].py", "b.py", false));
        expect(fnmatch_match("[abc]x", "bx", false));
        // CPython semantics for the awkward class spellings.
        expect(fnmatch_match("[]]", "]", false))
            << "']' right after '[' is a member";
        expect(!fnmatch_match("[]]", "a", false));
        expect(fnmatch_match("[!]]", "a", true)) << "negated ']' member";
        expect(!fnmatch_match("[!]]", "]", false));
        expect(fnmatch_match("[a", "[a", false))
            << "unterminated '[' is a literal '[' ... CPython emits '\\[' and "
               "the rest stays literal";
        expect(!fnmatch_match("[a", "a", false));
        expect(fnmatch_match("[", "[", false));
        expect(!fnmatch_match("[]", "a", false)) << "'[]' becomes '(?!)'";
        expect(!fnmatch_match("[!]", "a", false))
            << "'[!]' has no terminator after the leading ']' rule, so CPython "
               "emits a literal '[' and the class never forms";
        expect(fnmatch_match("[!]", "[!]", false)) << "...the text is literal";
        expect(fnmatch_match("[a-]", "-", false));
        expect(fnmatch_match("[-a]", "-", false));
        expect(!fnmatch_match("[e-a]", "a", false))
            << "reversed range is dropped (CPython removes empty ranges)";
        expect(!fnmatch_match("[e-a]", "e", false))
            << "the whole class collapses to '(?!)' -> never matches";
        expect(fnmatch_match("[!e-a]", "e", false))
            << "the negated form collapses to '.' -> matches any char";
        expect(!fnmatch_match("[a\\-c]", "-", false))
            << "backslash joins the chunk before '-': CPython splits "
               "[a\\-c] into chunks 'a\\' + 'c', so '-' is NOT a member";
        expect(fnmatch_match("[a\\-c]", "\\", false))
            << "...the backslash itself IS a member";
        expect(fnmatch_match("[a\\-c]", "a", false));
        expect(fnmatch_match("[a\\-c]", "c", false));
        expect(!fnmatch_match("a\\*b", "axb", false))
            << "'\\' is literal, so the '*' after it still wildcards";
        expect(fnmatch_match("a\\*b", "a\\b", false));
        expect(fnmatch_match("[A-Z]*", "Abc", true)) << "folded class bounds";
        expect(fnmatch_match("[A-Z]*", "abc", true))
            << "folding applies to the bounds as well, so a-z is accepted";
        expect(!fnmatch_match("[A-Z]*", "abc", false))
            << "case-sensitive [A-Z] rejects a lowercase name";
    };

    "fnmatch_case_folding"_test = [] {
        expect(fnmatch_match("BUILD/*.PY", "build/x.py", true));
        expect(!fnmatch_match("BUILD/*.PY", "build/x.py", false));
        expect(fnmatch_match("*.Py", "A.PY", true));
        // Non-ASCII bytes are compared as-is (ASCII-only folding).
        expect(fnmatch_match("\xC3\xA9*", "\xC3\xA9x", true));
        expect(!fnmatch_match("E*", "\xC3\x89", true))
            << "documented limitation: no Unicode folding table";
    };

    // ------------------------------------------------------------------
    // §3.2 path-glob parsing
    // ------------------------------------------------------------------
    "parse_pattern_shape"_test = [] {
        path_glob_pattern pat;
        expect(!parse_pattern("**/*.py", false, pat).failed());
        expect(eq(pat.size(), size_t(2)));
        expect(pat.recursive) << "** recorded";
        expect(pat.anchored) << "a '/' in the pattern anchors it";
        expect(!pat.dir_only);
        expect(eq(kimix::string(pat.segments[1]), kimix::string("*.py")));
        expect(pat.kinds[0] == segment_kind::double_star);
        expect(pat.kinds[1] == segment_kind::wildcard);

        expect(!parse_pattern("src", false, pat).failed());
        expect(pat.kinds[0] == segment_kind::literal) << "no metacharacters";
        expect(!pat.anchored) << "no separator";

        expect(!parse_pattern("a//b", false, pat).failed());
        expect(eq(pat.size(), size_t(2))) << "empty segments dropped (pathlib)";
        expect(!parse_pattern("./a/./b", false, pat).failed());
        expect(eq(pat.size(), size_t(2))) << "'.' segments dropped";

        // dir_only via trailing separator (GH-65238: pathlib keeps an empty
        // final part and yields directories only).
        expect(!parse_pattern("src/", false, pat).failed());
        expect(eq(pat.size(), size_t(1)));
        expect(pat.dir_only);

    };

    "parse_pattern_errors"_test = [] {
        path_glob_pattern pat;
        auto e1 = parse_pattern("", false, pat);
        expect(e1.status == tool_status::invalid_input)
            << "pathlib: ValueError('Unacceptable pattern')";
        expect(e1.message.find("Unacceptable pattern") != kimix::string::npos);

        auto e2 = parse_pattern("./", false, pat);
        expect(e2.status == tool_status::invalid_input)
            << "'./' has no real segment -> pathlib raises";

        auto e3 = parse_pattern("/etc/passwd", false, pat);
        expect(e3.status == tool_status::invalid_input);
        expect(e3.message.find("Non-relative patterns are unsupported") !=
               kimix::string::npos) << "pathlib NotImplementedError wording";

        auto e4 = parse_pattern("C:/Windows/*.ini", false, pat);
        expect(e4.failed()) << "drive-qualified pattern rejected too";

        auto e5 = parse_pattern("//srv/share/*", false, pat);
        expect(e5.failed()) << "UNC root rejected too";

        // Errors must not leave a half-parsed pattern behind.
        expect(pat.empty()) << "out is reset on failure";
    };

    "matcher_matrix"_test = [] {
        // literal segments
        expect(matches("src/main.py", "src/main.py"));
        expect(!matches("src/main.py", "src/main/app.py"));
        expect(!matches("main.py", "src/main.py"))
            << "a '/'-free pattern is anchored at the search root (pathlib)";
        // '*' never crosses a segment boundary in the path layer
        expect(matches("*.py", "setup.py"));
        expect(!matches("*.py", "src/setup.py"));
        expect(matches("src/*.py", "src/main.py"));
        expect(!matches("src/*.py", "src/main/app.py"));
        // '?'
        expect(matches("?.md", "a.md"));
        expect(!matches("?.md", "ab.md"));
        // [...]
        expect(matches("file[1-2].py", "file1.py"));
        expect(!matches("file[1-2].py", "file3.py"));
        expect(matches("src/[!a]*.py", "src/b.py"));
        expect(!matches("src/[!a]*.py", "src/a.py"));
        // '**' zero / multi level
        expect(matches("a/**/b", "a/b")) << "zero level";
        expect(matches("a/**/b", "a/x/y/b")) << "multi level";
        expect(matches("**/*.py", "setup.py")) << "leading ** matches root files";
        expect(matches("**/*.py", "src/main/app.py"));
        expect(!matches("**/*.py", "src/main/app.txt"));
        expect(matches("src/**/*.py", "src/main.py")) << "'**' zero level";
        expect(matches("src/**/*.py", "src/main/app.py"));
        expect(matches("src/**/*.py", "src/.hidden/app.py"))
            << "hidden directories are traversed";
        expect(matches("src/**", "src")) << "pathlib yields the dir itself";
        expect(matches("src/**", "src/a/b.py"));
        expect(matches("**/main/*.py", "src/main/app.py"));
        expect(!matches("docs/**/main/*.py", "src/main/app.py"));
        expect(matches("**/**/*.py", "a.py")) << "'**' twice is still '**'";
        // trailing '**'
        expect(matches("a/**", "a/b/c"));
        // dotfiles
        expect(matches("*.yml", ".gitlab-ci.yml"));
        expect(matches("**/*.py", ".hidden/x.py"));
        // no match cases
        expect(!matches("src/test/test_*.py", "src/main/app.py"));
        expect(matches("**", "anything/here/again")) << "'**' matches any path";
    };

    "matcher_case_and_separators"_test = [] {
        expect(matches("BUILD/*.PY", "build/x.py", true));
        expect(!matches("BUILD/*.PY", "build/x.py", false));
        expect(matches("sub/*.py", "sub/A.PY", true)) << "fold the name too";
        // Windows separators normalize to '/' on both sides.
        expect(matches(R"(build\x.py)", R"(build\x.py)")) << "backslashes";
        expect(matches(R"(src\**\*.py)", "src/a/b.py"))
            << "backslash separators in the pattern";
        expect(matches("**/*.py", R"(src\main\app.py)"))
            << "a Windows result path matches a POSIX pattern";
        expect(!matches("src\\main.py", "src/other.py"));
    };

    "trailing_slash_dir_only"_test = [] {
        // The matcher itself is separator-agnostic; the walker enforces the
        // directory-only rule (pathlib yields dirs only for 'src/').
        const auto pat = must_parse("src/");
        expect(pat.dir_only);
        expect(match_path(pat, "src"));
        mem_tree tree;
        tree.add_subdir("src");
        tree.add_file("src/a.py");
        tree.add_file("top.py");
        walk_options opts;
        const auto res = walk_matches(tree.lister(), stat_fn{}, pat, opts);
        expect(rel_paths(res) == v({"src"})) << "files are filtered out";
    };

    "basename_at_any_depth"_test = [] {
        // The tool description promises this (glob.py:425-428) even though the
        // shipped pathlib walk does not do it; the kernel exposes both rules.
        const auto pat = must_parse("*.ts");
        expect(!match_path(pat, "src/a.ts")) << "pathlib-anchored";
        expect(match_basename_at_any_depth(pat, "src/a.ts")) << "any-depth rule";
        expect(match_basename_at_any_depth(pat, "a.ts"));
        expect(is_basename_pattern("*.ts"));
        expect(!is_basename_pattern("src/*.ts"));
        expect(is_basename_pattern(R"(src\*.ts)") == false);
        const auto rec = must_parse("**/*.ts");
        expect(!is_basename_pattern("**/*.ts"));
        expect(match_basename_at_any_depth(rec, "src/a.ts"))
            << "anchored patterns fall back to the pathlib rule";
    };

    "unsafe_recursive_pattern"_test = [] {
        expect(is_unsafe_recursive_pattern("**"));
        expect(is_unsafe_recursive_pattern("**/*"));
        expect(is_unsafe_recursive_pattern("**/**"));
        expect(is_unsafe_recursive_pattern(R"(**\*)")) << "Windows separator";
        expect(is_unsafe_recursive_pattern("*/**"));
        expect(is_unsafe_recursive_pattern("**/*/*"));
        expect(is_unsafe_recursive_pattern("./**")) << "leading './' stripped";
        expect(is_unsafe_recursive_pattern("./**/*"));
        // Safe: at least one non-wildcard segment.
        expect(!is_unsafe_recursive_pattern("src/**/*.py"));
        expect(!is_unsafe_recursive_pattern("**/*.py"));
        expect(!is_unsafe_recursive_pattern("**/main"));
        expect(!is_unsafe_recursive_pattern("*"));
        expect(!is_unsafe_recursive_pattern("*.py"));
        expect(!is_unsafe_recursive_pattern(""));
        expect(!is_unsafe_recursive_pattern("**.py"))
            << "'**.py' is a wildcard segment, not a bare '**'";

        // Byte-exact ToolError text from glob.py:513-522.
        kimix::string brief;
        const auto e = make_unsafe_pattern_error("**/*", brief);
        expect(e.status == tool_status::invalid_input);
        expect(eq(e.message,
                  kimix::string(
                      "Unsafe pattern `**/*` \xE2\x80\x94 this would "
                      "recursively match all files/dirs under the search root, "
                      "which is meaningless and can be extremely slow. Use a "
                      "more specific pattern (e.g. `src/**/*.py`).")));
        expect(eq(brief, kimix::string("Unsafe pattern: **/*")));
    };

    // ------------------------------------------------------------------
    // ignore filter
    // ------------------------------------------------------------------
    "ignore_parse_rules"_test = [] {
        const auto rules = parse_ignore_rules(
            "# comment\n\nbuild/\n*.pyc\n!keep.pyc\n/root-only.txt\n"
            "src/*.md\n  spaced.txt  \n",
            "");
        std::vector<std::string> got;
        for (const auto &r : rules) {
            got.emplace_back(std::string(r.pattern) +
                             (r.negated ? "|neg" : "") +
                             (r.anchored ? "|anch" : "") +
                             (r.dir_only ? "|dir" : ""));
        }
        expect(got == std::vector<std::string>{"build|dir", "*.pyc",
                                               "keep.pyc|neg",
                                               "root-only.txt|anch",
                                               "src/*.md|anch",
                                               "  spaced.txt"});
        expect(eq(rules.size(), size_t(6))) << "blank + comment lines skipped";
        expect(rules[0].dir_only && !rules[0].anchored);
        expect(rules[3].anchored) << "leading '/' anchors and is stripped";
        expect(rules[4].anchored) << "interior '/' anchors";
        expect(!rules[1].anchored && !rules[1].dir_only);
        // Python str.rstrip() only strips the trailing end; leading spaces are
        // part of the pattern (glob.py:140).
        expect(eq(rules[5].pattern, kimix::string("  spaced.txt")));
        auto weird = parse_ignore_rules("!\n/\n#\n", "sub");
        expect(eq(weird.size(), size_t(1))) << "'!' and '#' alone are dropped";
        expect(eq(weird[0].source_dir, kimix::string("sub")));
    };

    "ignore_rule_match_semantics"_test = [] {
        ignore_rule star;
        star.pattern = "*.pyc";
        expect(ignore_rule_match("a.pyc", false, star, false));
        expect(ignore_rule_match("src/deep/a.pyc", false, star, false))
            << "unanchored rules match the basename at any depth";
        expect(!ignore_rule_match("a.py", false, star, false));

        ignore_rule dir_only;
        dir_only.pattern = "build";
        dir_only.dir_only = true;
        expect(ignore_rule_match("build", true, dir_only, false));
        expect(!ignore_rule_match("build", false, dir_only, false))
            << "a file called 'build' is not a directory";
        expect(ignore_rule_match("build/x.py", false, dir_only, false))
            << "dir-only rules exclude descendants";
        expect(ignore_rule_match("a/b/build/c/d.py", false, dir_only, false));
        expect(ignore_rule_match("a/b/build", true, dir_only, false));

        ignore_rule anchored;
        anchored.pattern = "src/*.py";
        anchored.anchored = true;
        expect(ignore_rule_match("src/a.py", false, anchored, false));
        expect(ignore_rule_match("src/deep/a.py", false, anchored, false))
            << "Python fnmatch's '*' crosses '/', so an anchored rule DOES "
               "sink into sub-directories (glob.py:231 uses fnmatch.fnmatch)";
        expect(!ignore_rule_match("other/src/a.py", false, anchored, false));

        ignore_rule rec;
        rec.pattern = "**/node_modules/*.js";
        expect(ignore_rule_match("node_modules/a.js", false, rec, false));
        expect(ignore_rule_match("x/y/node_modules/a.js", false, rec, false));
        ignore_rule tail;
        tail.pattern = "docs/**";
        expect(ignore_rule_match("docs/a/b.md", false, tail, false));
        expect(ignore_rule_match("docs", false, tail, false))
            << "rel_path == prefix counts too";
        expect(!ignore_rule_match("docsx/a", false, tail, false));
        ignore_rule mid;
        mid.pattern = "a/**/b.py";
        expect(ignore_rule_match("a/x/y/b.py", false, mid, false));
        expect(ignore_rule_match("a/b.py", false, mid, false));
        ignore_rule all;
        all.pattern = "**";
        expect(ignore_rule_match("whatever/you/mean.py", false, all, false));
        ignore_rule simple;
        simple.pattern = "a**b";
        // '**' degenerates to a run of '*' (pattern.replace('**', '*') ->
        // 'a*b'), which fnmatch matches against the whole path OR the basename.
        // 'a*b' does NOT match 'aXXXb.py' (trailing '.py'), but does match a
        // path whose last segment is exactly 'aXXXb'.
        expect(!ignore_rule_match("aXXXb.py", false, simple, false))
            << "'a*b' needs the string to end in 'b'";
        expect(ignore_rule_match("aXXXb", false, simple, false))
            << "'**' fallback collapses to '*'";
        expect(ignore_rule_match("x/aXXXb", false, simple, false))
            << "... or the basename";
        // case flag
        ignore_rule up;
        up.pattern = "*.LOG";
        expect(ignore_rule_match("a.log", false, up, true));
        expect(!ignore_rule_match("a.log", false, up, false));
    };

    "is_ignored_negation_and_order"_test = [] {
        const auto rules =
            parse_ignore_rules("*.pyc\n!keep.pyc\nbuild/\n!build/keep.txt\n",
                               "");
        expect(is_ignored("a.pyc", false, rules, false));
        expect(!is_ignored("keep.pyc", false, rules, false))
            << "later negation un-ignores";
        expect(is_ignored("build/x.o", false, rules, false));
        expect(!is_ignored("build/keep.txt", false, rules, false))
            << "a negated dir-only rule un-ignores the directory contents";
        expect(!is_ignored("readme.md", false, rules, false));
        // Rule ordering decides: the last matching rule wins.
        const auto flip = parse_ignore_rules("!keep.pyc\n*.pyc\n", "");
        expect(is_ignored("keep.pyc", false, flip, false));
    };

    "is_ignored_multi_source_dirs"_test = [] {
        // Rules from a nested .gitignore apply only below their source dir.
        kimix::vector<ignore_rule> rules;
        auto root_rules = parse_ignore_rules("*.log\n", "");
        auto sub_rules = parse_ignore_rules("temp/\nsecret.txt\n", "packages/a");
        for (auto &r : root_rules) {
            rules.push_back(std::move(r));
        }
        for (auto &r : sub_rules) {
            rules.push_back(std::move(r));
        }
        expect(is_ignored("a.log", false, rules, false));
        expect(is_ignored("packages/a/b.log", false, rules, false));
        expect(is_ignored("packages/a/temp/x.py", false, rules, false));
        expect(!is_ignored("temp/x.py", false, rules, false))
            << "the nested rule does not escape its source dir";
        expect(!is_ignored("packages/secret.txt", false, rules, false));
        expect(is_ignored("packages/a/secret.txt", false, rules, false));
    };

    // ------------------------------------------------------------------
    // §3.3 walker (in-memory)
    // ------------------------------------------------------------------
    "walker_basic"_test = [] {
        mem_tree tree;
        tree.add_file("README.md");
        tree.add_file("setup.py");
        tree.add_subdir("src");
        tree.add_file("src/main.py");
        tree.add_file("src/utils.py");
        tree.add_subdir("src/main");
        tree.add_file("src/main/app.py");
        tree.add_subdir("src/test");
        tree.add_file("src/test/test_app.py");
        tree.add_subdir("docs");
        tree.add_file("docs/guide.md");

        const auto py = must_parse("**/*.py");
        walk_options opts;
        const auto res = walk_matches(tree.lister(), stat_fn{}, py, opts);
        expect(rel_paths(res) ==
               v({"setup.py", "src/main.py", "src/main/app.py", "src/test/"
                                                          "test_app.py",
                  "src/utils.py"}))
            << "sorted, '/'-normalized, files only";
        expect(!res.truncated && !res.timed_out);
        expect(eq(res.ignored_count, size_t(0)));

        const auto root_py = must_parse("*.py");
        const auto res2 =
            walk_matches(tree.lister(), stat_fn{}, root_py, walk_options{});
        expect(rel_paths(res2) == v({"setup.py"}))
            << "pathlib anchors a '/'-free pattern at the search root";

        const auto src_rec = must_parse("src/**/*.py");
        const auto res3 =
            walk_matches(tree.lister(), stat_fn{}, src_rec, walk_options{});
        expect(rel_paths(res3) ==
               v({"src/main.py", "src/main/app.py", "src/test/test_app.py",
                  "src/utils.py"}));

        const auto specific = must_parse("src/**/test_*.py");
        const auto res4 =
            walk_matches(tree.lister(), stat_fn{}, specific, walk_options{});
        expect(rel_paths(res4) == v({"src/test/test_app.py"}));
    };

    "walker_include_dirs"_test = [] {
        mem_tree tree;
        tree.add_file("test_file.txt");
        tree.add_subdir("test_dir");
        tree.add_file("other.txt");
        const auto pat = must_parse("test_*");

        const auto files_only =
            walk_matches(tree.lister(), stat_fn{}, pat, walk_options{});
        expect(rel_paths(files_only) == v({"test_file.txt"}));

        walk_options with_dirs;
        with_dirs.include_dirs = true;
        const auto both =
            walk_matches(tree.lister(), stat_fn{}, pat, with_dirs);
        expect(rel_paths(both) == v({"test_dir", "test_file.txt"}));
        expect(both.entries[0].is_dir);
        expect(!both.entries[1].is_dir);
    };

    "walker_max_matches_and_sort"_test = [] {
        mem_tree tree;
        std::vector<std::string> insertion;
        for (int i = 0; i < 30; i++) {
            const std::string name = "file_" + std::to_string(i) + ".txt";
            tree.add_file(name);
            insertion.push_back(name);
        }

        walk_options opts;
        opts.max_matches = 25;
        const auto res =
            walk_matches(tree.lister(), stat_fn{}, must_parse("*.txt"), opts);
        expect(eq(res.entries.size(), size_t(25))) << "hard cap honoured";
        expect(res.truncated) << "an overflow candidate was seen";
        // glob.py:597-600 pop-on-overflow: the cap is applied in walk
        // (collection) order, THEN the kept matches are sorted. So the result
        // is the first 25 collected entries, sorted — not the head of the
        // fully-sorted 30-entry list.
        std::vector<std::string> head(insertion.begin(), insertion.begin() + 25);
        std::sort(head.begin(), head.end());
        expect(rel_paths(res) == head) << "first 25 collected, then sorted";

        // Exactly max_matches matches is NOT capped (glob.py pop-on-overflow).
        walk_options exact;
        exact.max_matches = 30;
        const auto res2 =
            walk_matches(tree.lister(), stat_fn{}, must_parse("*.txt"), exact);
        expect(eq(res2.entries.size(), size_t(30)));
        expect(!res2.truncated) << "no 31st candidate existed";

        walk_options all;
        all.max_matches = 0;
        const auto res3 =
            walk_matches(tree.lister(), stat_fn{}, must_parse("*.txt"), all);
        expect(eq(res3.entries.size(), size_t(30))) << "0 == unlimited";
        expect(!res3.truncated);
    };

    "walker_gitignore_filter_after_walk"_test = [] {
        mem_tree tree;
        tree.add_file("src/keep.py");
        tree.add_file(".venv/lib/site.py");
        tree.add_file(".venv/pyvenv.cfg");
        tree.add_file("node_modules/pkg/index.js");
        tree.add_file("src/.gitignore");
        const auto rules =
            parse_ignore_rules(".venv/\nnode_modules/\n!.venv/pyvenv.cfg\n",
                               "");
        walk_options opts;
        opts.ignore_rules = &rules;
        const auto res =
            walk_matches(tree.lister(), stat_fn{}, must_parse("**/*.py"), opts);
        expect(rel_paths(res) == v({"src/keep.py"}))
            << "ignored subtrees never make it into the results; "
               "src/.gitignore and node_modules/pkg/index.js are not '**/*.py' "
               "matches, and .venv/pyvenv.cfg is not a '.py' match either";
        expect(eq(res.ignored_count, size_t(1)))
            << "only .venv/lib/site.py matched the pattern AND an ignore rule "
               "(verified against the Python reference)";
        expect(eq(res.visited_dirs, size_t(6)))
            << "no pruning: root + src + .venv + .venv/lib + node_modules + "
               "node_modules/pkg were all listed";
    };

    "walker_prune_ignored_dirs"_test = [] {
        mem_tree tree;
        tree.add_file("src/keep.py");
        tree.add_file(".venv/lib/site.py");
        tree.add_file(".venv/lib/deep/more.py");
        tree.add_file("big/aaa.py");
        // Sentinel inside the ignored subtree must never be visited.
        tree.add_file(".venv/sentinel.py");
        const auto rules = parse_ignore_rules(".venv/\n", "");
        walk_options opts;
        opts.ignore_rules = &rules;
        opts.prune_ignored_dirs = true;
        const auto res =
            walk_matches(tree.lister(), stat_fn{}, must_parse("**/*.py"), opts);
        std::vector<std::string> paths = rel_paths(res);
        expect(paths == v({"big/aaa.py", "src/keep.py"}));
        bool sentinel = false;
        for (const auto &p : paths) {
            sentinel = sentinel || p.find("sentinel") != std::string::npos;
        }
        expect(!sentinel) << "the ignored subtree is not reported";
        expect(res.listed_entries > 0) << "diagnostics recorded";
        const auto unpruned =
            walk_matches(tree.lister(), stat_fn{}, must_parse("**/*.py"),
                         [&] {
                             walk_options o;
                             o.ignore_rules = &rules;
                             return o;
                         }());
        expect(unpruned.visited_dirs > res.visited_dirs)
            << "pruning visits strictly fewer directories";
        expect(eq(res.ignored_count, size_t(0)))
            << "pruned entries are never even tested as matches";
    };

    "walker_symlinks_not_followed"_test = [] {
        mem_tree tree;
        tree.add_file("a.py");
        tree.add_subdir("real");
        tree.add_file("real/b.py");
        tree.add_subdir("link", true); // symlinked directory
        tree.add_file("link/c.py");
        const auto res = walk_matches(tree.lister(), stat_fn{},
                                      must_parse("**/*.py"), walk_options{});
        expect(rel_paths(res) == v({"a.py", "real/b.py"}))
            << "'**' does not descend into a symlinked directory "
               "(pathlib >= 3.13 policy)";
        const auto with_dirs = walk_matches(
            tree.lister(), stat_fn{}, must_parse("**"), [] {
                walk_options o;
                o.include_dirs = true;
                return o;
            }());
        expect(rel_paths(with_dirs) == v({"a.py", "link", "real", "real/b.py"}))
            << "the symlink entry itself is still reported as a match";
    };

    "walker_skips_unlistable_dirs"_test = [] {
        mem_tree tree;
        tree.add_file("a.py");
        tree.add_subdir("locked");
        tree.add_file("locked/b.py");
        tree.add_subdir("ok");
        tree.add_file("ok/c.py");
        tree.set_unlistable("locked");
        const auto res = walk_matches(tree.lister(), stat_fn{},
                                      must_parse("**/*.py"), walk_options{});
        expect(rel_paths(res) == v({"a.py", "ok/c.py"}))
            << "a permission-denied directory is skipped silently";
        expect(eq(res.skipped_dirs, size_t(1)));
        expect(eq(res.visited_dirs, size_t(2))) << "root + ok";
    };

    "walker_collect_stats"_test = [] {
        mem_tree tree;
        tree.add_file("a.py", 1234, 1700000000.5);
        tree.add_file("sub/b.py", 42, 1700000100.25);
        const auto dir_pat = must_parse("**/*.py");
        walk_options opts;
        opts.collect_stats = true;
        const auto res = walk_matches(tree.lister(), tree.probe(), dir_pat, opts);
        expect(eq(res.entries.size(), size_t(2)));
        expect(eq(res.entries[0].size, int64_t(1234)));
        expect(eq(res.entries[0].mtime, 1700000000.5));
        expect(eq(res.entries[1].size, int64_t(42)));
        expect(!res.entries[0].is_dir);

        walk_options nostat;
        const auto bare = walk_matches(tree.lister(), tree.probe(), dir_pat,
                                       nostat);
        expect(eq(bare.entries[0].size, int64_t(-1)))
            << "size stays -1 unless stats were requested";
        expect(eq(bare.entries[0].mtime, 0.0));
    };

    "walker_deadline"_test = [] {
        // A tree wide enough that a 0 ms budget always aborts after the root
        // listing, while still returning the partial result set.
        mem_tree tree;
        for (int i = 0; i < 40; i++) {
            tree.add_subdir("d" + std::to_string(i));
            tree.add_file("d" + std::to_string(i) + "/f.py");
        }
        walk_options opts;
        opts.deadline_ms = 0; // "no deadline"
        const auto full =
            walk_matches(tree.lister(), stat_fn{}, must_parse("**/*.py"), opts);
        expect(eq(full.entries.size(), size_t(40)));
        expect(!full.timed_out) << "0 disables the deadline";

        // Injected lister that burns wall time so a real deadline can fire.
        walk_options tight;
        tight.deadline_ms = 1;
        list_dir_fn slow_lister =
            [](kimix::string_view dir_rel,
              kimix::vector<dirent_info> &out) -> tool_error {
            volatile int sink = 0;
            for (long i = 0; i < 20000000L; i++) {
                sink += static_cast<int>(i & 3);
            }
            (void)sink;
            if (!dir_rel.empty()) {
                return tool_error{tool_status::not_found, kimix::string("")};
            }
            out.emplace_back(kimix::string("a"), false, false);
            out.emplace_back(kimix::string("b"), false, false);
            return tool_error{};
        };
        walk_options zero;
        zero.deadline_ms = 1;
        const auto timed =
            walk_matches(slow_lister, stat_fn{}, must_parse("*"), zero);
        expect(timed.timed_out) << "cooperative abort observed";
        expect(timed.entries.empty() || timed.entries.size() <= 2)
            << "partial results are still returned";
    };

    "walker_empty_pattern_is_no_match"_test = [] {
        mem_tree tree;
        tree.add_file("a.py");
        path_glob_pattern blank;
        const auto res =
            walk_matches(tree.lister(), stat_fn{}, blank, walk_options{});
        expect(res.entries.empty()) << "an unparsed pattern never walks";
        const auto nolister =
            walk_matches(list_dir_fn{}, stat_fn{}, must_parse("*"),
                         walk_options{});
        expect(nolister.entries.empty()) << "a missing lister is not a crash";
    };

    // ------------------------------------------------------------------
    // real filesystem wrapper
    // ------------------------------------------------------------------
    "walk_matches_fs_real_tree"_test = [] {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        const auto base = fs::temp_directory_path(ec);
        if (ec) {
            return;
        }
        const fs::path root =
            base / "kimix_glob_tool_selftest" / "tree";
        fs::remove_all(root.parent_path(), ec);
        fs::create_directories(root / "src" / "main", ec);
        fs::create_directories(root / "src" / "test", ec);
        fs::create_directories(root / "docs", ec);
        if (ec) {
            return;
        }
        const auto touch = [&](const fs::path &p, const char *body) {
            fs::path full = root / p;
            std::ofstream out(full.native(), std::ios::binary | std::ios::trunc);
            out << body;
        };
        touch("README.md", "# README");
        touch("setup.py", "setup");
        touch("src/main.py", "main");
        touch("src/main/app.py", "app");
        touch("src/test/test_app.py", "test app");
        touch("docs/guide.md", "guide");
        touch(".hidden.py", "hidden");

        tool_error err;
        walk_options opts;
        const auto res = walk_matches_fs(
            kimix::string_view(reinterpret_cast<const char *>(root.string().c_str()),
                               root.string().size()),
            kimix::string_view("**/*.py", 7), opts, err);
        expect(!err.failed()) << err.message;
        std::vector<std::string> got = rel_paths(res);
        expect(got == v({".hidden.py", "setup.py", "src/main.py",
                         "src/main/app.py", "src/test/test_app.py"}))
            << "real walk, '/'-normalized and sorted";

        // include_dirs keeps directories, stats are filled from the FS.
        walk_options dirs_opts;
        dirs_opts.include_dirs = true;
        dirs_opts.collect_stats = true;
        const auto dres = walk_matches_fs(
            kimix::filesystem::path(root.string()), must_parse("src/**"),
            dirs_opts);
        std::vector<std::string> dgot = rel_paths(dres);
        expect(dgot == v({"src", "src/main", "src/main.py", "src/main/app.py",
                          "src/test", "src/test/test_app.py"}));
        for (const auto &e : dres.entries) {
            expect(e.mtime >= 0.0) << "mtime came from the filesystem";
            if (!e.is_dir) {
                expect(e.size > 0) << "file size collected";
            } else {
                expect(eq(e.size, int64_t(-1))) << "directories have no size";
            }
        }

        // gitignore filtering on a real tree.
        touch(".gitignore", "docs/\n*.md\n");
        path_glob_pattern md;
        expect(!parse_pattern("**/*.md", false, md).failed());
        walk_options filtered;
        filtered.ignore_rules =
            new kimix::vector<ignore_rule>(
                parse_ignore_rules("docs/\n*.md\n", ""));
        const auto fres = walk_matches_fs(fs::path(root.string()), md, filtered);
        expect(fres.entries.empty()) << "all .md files are ignored";
        expect(eq(fres.ignored_count, size_t(2))) << "README.md + docs/guide.md";
        delete filtered.ignore_rules;

        // missing root: silent, no exception
        const auto missing = walk_matches_fs(
            fs::path((root.parent_path() / "does-not-exist").native()),
            must_parse("*.py"), walk_options{});
        expect(missing.entries.empty());
        expect(eq(missing.skipped_dirs, size_t(1)));

        fs::remove_all(root.parent_path(), ec);
    };

    // ------------------------------------------------------------------
    // result shaping
    // ------------------------------------------------------------------
    "shaping_sort_dedup_prefix"_test = [] {
        kimix::vector<walk_entry> entries;
        entries.push_back(walk_entry{kix("src/b.py"), false, 1, 5.0});
        entries.push_back(walk_entry{kix("src/a.py"), false, 2, 9.0});
        entries.push_back(walk_entry{kix("a.py"), false, 3, 7.0});
        sort_entries(entries);
        expect(eq(entries[0].rel_path, kix("a.py")));
        expect(eq(entries[1].rel_path, kix("src/a.py")));
        expect(eq(entries[2].rel_path, kix("src/b.py")));

        entries.push_back(entries[1]);
        const size_t removed = dedup_entries(entries);
        expect(eq(removed, size_t(1))) << "duplicate rel_path dropped";
        expect(eq(entries.size(), size_t(3)));

        kimix::vector<walk_entry> prefixed;
        prefixed.push_back(walk_entry{kix("work/src/a.py"), false, 1, 1.0});
        prefixed.push_back(walk_entry{kix("work/x.py"), false, 1, 1.0});
        prefixed.push_back(walk_entry{kix("other.py"), false, 1, 1.0});
        expect(eq(strip_prefix(prefixed, kix("work")), size_t(2)));
        expect(eq(prefixed[0].rel_path, kix("src/a.py")));
        expect(eq(prefixed[1].rel_path, kix("x.py")));
        expect(eq(prefixed[2].rel_path, kix("other.py")))
            << "unrelated paths are untouched";
        // Windows separators in the prefix normalize first.
        kimix::vector<walk_entry> p2;
        p2.push_back(walk_entry{kix("a/b.py"), false, 1, 1.0});
        expect(eq(strip_prefix(p2, kix("a\\b.py")), size_t(1)));
        expect(p2[0].rel_path.empty()) << "exact prefix collapses to ''";
    };

    "shaping_mtime_top_k"_test = [] {
        kimix::vector<walk_entry> entries;
        entries.push_back(walk_entry{kix("a"), false, 1, 100.0});
        entries.push_back(walk_entry{kix("b"), false, 1, 300.0});
        entries.push_back(walk_entry{kix("c"), false, 1, 200.0});
        entries.push_back(walk_entry{kix("d"), false, 1, 300.0});
        order_by_mtime_top_k(entries, 0);
        expect(eq(entries[0].rel_path, kix("b")))
            << "newest first, ties keep the path order (stable)";
        expect(eq(entries[1].rel_path, kix("d")));
        expect(eq(entries[2].rel_path, kix("c")));
        expect(eq(entries[3].rel_path, kix("a")));
        order_by_mtime_top_k(entries, 2);
        expect(eq(entries.size(), size_t(2))) << "top-k slice";
    };

    "shaping_pagination"_test = [] {
        kimix::vector<walk_entry> entries;
        for (int i = 0; i < 10; i++) {
            entries.push_back(
                walk_entry{kix("f" + std::to_string(i)), false, 1, 0.0});
        }
        kimix::vector<walk_entry> out;
        size_t total = 0;
        size_t omitted = 99;
        paginate_entries(kimix::span<const walk_entry>(entries), 4, 2, out,
                         total, omitted);
        expect(eq(out.size(), size_t(4)));
        expect(eq(out[0].rel_path, kix("f2")));
        expect(eq(out[3].rel_path, kix("f5")));
        expect(eq(total, size_t(10)));
        expect(eq(omitted, size_t(4)))
            << "8 entries after the offset, 4 shown -> 4 dropped from the tail";
        paginate_entries(kimix::span<const walk_entry>(entries), 0, 0, out,
                         total, omitted);
        expect(eq(out.size(), size_t(10))) << "head_limit 0 == unlimited";
        expect(eq(omitted, size_t(0)));
        paginate_entries(kimix::span<const walk_entry>(entries), 4, 9, out,
                         total, omitted);
        expect(eq(out.size(), size_t(1))) << "window clipped at the end";
        paginate_entries(kimix::span<const walk_entry>(entries), 4, 99, out,
                         total, omitted);
        expect(out.empty()) << "offset past the end";
        expect(eq(omitted, size_t(0)));
    };

    "shaping_output_fold_and_bytes"_test = [] {
        kimix::vector<walk_entry> entries;
        for (int i = 0; i < 1000; i++) {
            entries.push_back(
                walk_entry{kix("dir/f" + std::to_string(i) + ".py"), false, 10,
                           0.0});
        }
        shape_options opts;
        shaped_output out;
        shape_output(kimix::span<const walk_entry>(entries), opts, out);
        expect(eq(out.lines.size(), size_t(501)))
            << "500 real lines + one fold marker";
        expect(eq(out.omitted_by_fold, size_t(500)));
        expect(eq(out.shown_count, size_t(500)));
        // The marker sits between the head (250) and tail (250) halves.
        expect(out.lines[250].find("lines omitted") != kimix::string::npos);
        expect(out.lines[0] == "dir/f0.py");
        expect(!out.truncated_by_bytes) << "1000 short paths stay < 100 KiB";

        shape_options unlimited;
        unlimited.max_results = 0;
        shape_output(kimix::span<const walk_entry>(entries), unlimited, out);
        expect(eq(out.lines.size(), size_t(1000))) << "0 == no fold";
        expect(eq(out.omitted_by_fold, size_t(0)));

        // Byte cap: a small budget stops mid-list.
        shape_options tiny;
        tiny.max_results = 0;
        tiny.max_bytes = 100;
        shape_output(kimix::span<const walk_entry>(entries), tiny, out);
        expect(out.truncated_by_bytes);
        expect(out.lines.size() < 12) << "one line per ~14 bytes";
        expect(out.total_bytes >= 100);

        // Verbose lines with a caller-supplied mtime renderer.
        kimix::vector<walk_entry> two;
        two.push_back(walk_entry{kix("a.py"), false, 12, 1.0});
        two.push_back(walk_entry{kix("b"), true, -1, 2.0});
        shape_options verb;
        verb.verbose = true;
        verb.max_results = 0;
        verb.format_mtime = [](const walk_entry &) {
            return kimix::string("2024-01-01 00:00:00");
        };
        shape_output(kimix::span<const walk_entry>(two), verb, out);
        expect(eq(out.lines[0], kix("a.py  (12 bytes, file, 2024-01-01 00:00:00)")));
        expect(eq(out.lines[1], kix("b  (? bytes, dir, 2024-01-01 00:00:00)")))
            << "an unknown size is reported as '?'";

        // A very long single line is truncated instead of hogging the budget.
        kimix::vector<walk_entry> huge;
        huge.push_back(walk_entry{kix(std::string(900, 'x')), false, 1, 0.0});
        shape_options hs;
        hs.max_results = 0;
        hs.max_bytes = 0; // no byte cap, only the per-line cap
        shape_output(kimix::span<const walk_entry>(huge), hs, out);
        expect(eq(utf8_code_point_count(out.lines[0]), size_t(500)))
            << "truncate_line budget";
        expect(out.lines[0].find("chars]") != kimix::string::npos);
    };

    "shaping_top_dirs_summary"_test = [] {
        kimix::vector<walk_entry> entries;
        for (int i = 0; i < 900; i++) {
            entries.push_back(
                walk_entry{kix(".venv/f" + std::to_string(i)), false, 1, 0.0});
        }
        for (int i = 0; i < 40; i++) {
            entries.push_back(
                walk_entry{kix("src/f" + std::to_string(i)), false, 1, 0.0});
        }
        for (int i = 0; i < 27; i++) {
            entries.push_back(
                walk_entry{kix("tests/f" + std::to_string(i)), false, 1, 0.0});
        }
        for (int i = 0; i < 5; i++) {
            entries.push_back(
                walk_entry{kix("aaa/f" + std::to_string(i)), false, 1, 0.0});
        }
        entries.push_back(walk_entry{kix("root_file.py"), false, 1, 0.0});
        const auto summary =
            top_dirs_summary(kimix::span<const walk_entry>(entries));
        expect(eq(summary, kix("top dirs: .venv (900), src (40), "
                                     "tests (27)")))
            << "top 3 by count, root files not counted";
        expect(eq(top_dirs_summary(kimix::span<const walk_entry>(entries), 4),
                  kix("top dirs: .venv (900), src (40), tests (27), aaa (5)")));
        kimix::vector<walk_entry> only_root;
        only_root.push_back(walk_entry{kix("a.py"), false, 1, 0.0});
        expect(top_dirs_summary(kimix::span<const walk_entry>(only_root))
                   .empty())
            << "nothing to summarize";
    };

    "shaping_message_bytes_exact"_test = [] {
        message_input in;
        in.pattern = "**/*.py";
        in.total = 7;
        expect(eq(build_result_message(in),
                  kix("Found 7 matches for pattern `**/*.py`.")));

        in.total = 0;
        expect(eq(build_result_message(in),
                  kix("No matches found for pattern `**/*.py`.")));
        in.ignored_count = 3;
        expect(eq(build_result_message(in),
                  kix("No matches found for pattern `**/*.py`. 3 path(s) "
                      "matched but were excluded by .gitignore \xE2\x80\x94 "
                      "pass respect_gitignore=False to include them.")));
        in.respect_gitignore = false;
        expect(eq(build_result_message(in),
                  kix("No matches found for pattern `**/*.py`.")))
            << "no exclusion note when gitignore is off";
        in.respect_gitignore = true;
        in.timed_out = true;
        expect(eq(build_result_message(in),
                  kix("No matches found for pattern `**/*.py`. Search timed "
                      "out after 10s; showing matches collected so far.")))
            << "the exclusion note is suppressed while timing out";
        in.timed_out = false;
        in.timeout_seconds = 30;

        in.total = 1000;
        in.shown_count = 500;
        in.omitted_by_fold = 500;
        in.truncated = true;
        in.with_top_dirs = true;
        in.top_dirs = "top dirs: .venv (900)";
        in.timed_out = true;
        in.truncated_by_bytes = true;
        expect(eq(build_result_message(in),
                  kix("Found 1000 matches for pattern `**/*.py`. Showing 500 "
                      "of 1000 (head+tail fold). Use max_results=0 or a more "
                      "specific pattern to see more. top dirs: .venv (900) "
                      "Search capped at 1000 matches. Search timed out after "
                      "30s; showing matches collected so far. Output truncated "
                    "to 102400 bytes.")))
           << "all notes in the Python order";
        in.truncated_by_bytes = true;
        expect(build_result_message(in).find(
                   "Output truncated to 102400 bytes.") != kimix::string::npos)
            << "100 KiB is reported as 102400 bytes like MAX_BYTES";
    };

    "shaping_end_to_end_pipeline"_test = [] {
        // Mirror the tool's pipeline: unsafe guard -> walk -> shape -> message,
        // including the MAX_MATCHES + fold interaction from glob.py:597-645.
        mem_tree tree;
        for (int i = 0; i < 1050; i++) {
            tree.add_file("file_" + std::to_string(i) + ".txt");
        }
        expect(!is_unsafe_recursive_pattern("*.txt"));
        walk_options opts;
        opts.max_matches = k_max_matches;
        const auto res =
            walk_matches(tree.lister(), stat_fn{}, must_parse("*.txt"), opts);
        expect(eq(res.entries.size(), size_t(1000))) << "MAX_MATCHES collected";
        expect(res.truncated) << "the 1001st match was seen";

        shape_options so;
        so.max_results = 500;
        so.max_bytes = 0;
        shaped_output out;
        shape_output(kimix::span<const walk_entry>(res.entries), so, out);
        message_input in;
        in.pattern = "*.txt";
        in.total = res.entries.size();
        in.shown_count = out.shown_count;
        in.omitted_by_fold = out.omitted_by_fold;
        in.truncated = res.truncated;
        in.with_top_dirs = true;
        in.top_dirs = top_dirs_summary(
            kimix::span<const walk_entry>(res.entries));
        const auto message = build_result_message(in);
        expect(eq(message,
                  kix("Found 1000 matches for pattern `*.txt`. Showing 500 of "
                      "1000 (head+tail fold). Use max_results=0 or a more "
                      "specific pattern to see more. Search capped at 1000 "
                      "matches.")));
        expect(out.lines.size() == size_t(501));
        expect(in.top_dirs.empty()) << "flat results have no top dirs";
    };
}
