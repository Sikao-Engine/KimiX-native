// Test for runtime/glob/gitignore.h
// This test covers:
// - parse_gitignore (empty, comments, negation, dir_only, anchored, leading
// slash)
// - gitignore matching (** patterns, anchored/unanchored, dir_only descendants,
// negation)
// - filter_paths bulk mask
// - is_ignored_name hard-coded set + regex patterns
// - parse_ls_files_output (NUL-delimited input, directory synthesis,
// ignored-prefix pruning)
// - Windows path separator normalization

#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include <runtime/glob/gitignore.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::runtime::glob;

static kimix::vector<gitignore_rule>
rules_from_bytes(kimix::string_view content) {
  return parse_gitignore(content);
}

int main(int argc, char *argv[]) {
  boost::ut::detail::cfg::parse_arg_with_fallback(
      argc, const_cast<const char **>(argv));

  "parse_gitignore_empty_and_comments"_test = [] {
    auto rules = rules_from_bytes("");
    expect(rules.empty());

    rules = rules_from_bytes("# comment\n\n# inline\n");
    expect(rules.empty());
  };

  "parse_gitignore_negation"_test = [] {
    auto rules = rules_from_bytes("!build/\n");
    expect(eq(rules.size(), size_t(1)));
    expect(rules[0].pattern == "build");
    expect(rules[0].negated);
    expect(!rules[0].anchored);
    expect(rules[0].dir_only);
  };

  "parse_gitignore_dir_only_and_anchored"_test = [] {
    auto rules = rules_from_bytes("dist/\nsrc/*.cpp\n/temp/\n");
    expect(eq(rules.size(), size_t(3)));

    expect(rules[0].pattern == "dist");
    expect(!rules[0].negated);
    expect(!rules[0].anchored);
    expect(rules[0].dir_only);

    expect(rules[1].pattern == "src/*.cpp");
    expect(rules[1].anchored);
    expect(!rules[1].dir_only);

    expect(rules[2].pattern == "temp");
    expect(rules[2].anchored);
    expect(rules[2].dir_only);
  };

  "match_double_star_all"_test = [] {
    auto rules = rules_from_bytes("**\n");
    expect(is_ignored_path("anything", false, rules));
    expect(is_ignored_path("a/b/c", true, rules));
  };

  "match_double_star_suffix"_test = [] {
    auto rules = rules_from_bytes("**/node_modules\n");
    expect(is_ignored_path("node_modules", true, rules));
    expect(is_ignored_path("src/node_modules", true, rules));
    expect(!is_ignored_path("src/node_modules/pkg.js", false, rules));
    expect(!is_ignored_path("node_modules_summary.txt", false, rules));
  };

  "match_double_star_prefix"_test = [] {
    auto rules = rules_from_bytes("build/**\n");
    expect(is_ignored_path("build", true, rules));
    expect(is_ignored_path("build/main.cpp", false, rules));
    expect(is_ignored_path("build/sub/x.py", false, rules));
    expect(!is_ignored_path("other/build/x.py", false, rules));
  };

  "match_double_star_middle"_test = [] {
    auto rules = rules_from_bytes("a/**/b\n");
    expect(is_ignored_path("a/b", true, rules));
    expect(is_ignored_path("a/x/b", true, rules));
    expect(is_ignored_path("a/x/y/b", true, rules));
    expect(!is_ignored_path("ab", false, rules));
    expect(!is_ignored_path("a/c", true, rules));
  };

  "match_anchored_vs_unanchored"_test = [] {
    auto rules = rules_from_bytes("build\n");
    expect(is_ignored_path("build", true, rules));
    expect(is_ignored_path("src/build", true, rules));
    expect(is_ignored_path("src/build/x.py", false, rules));

    rules = rules_from_bytes("/build\n");
    expect(is_ignored_path("build", true, rules));
    expect(!is_ignored_path("src/build", true, rules));
  };

  "match_dir_only_descendants"_test = [] {
    auto rules = rules_from_bytes(".venv/\n");
    expect(is_ignored_path(".venv", true, rules));
    expect(is_ignored_path(".venv/bin/python", false, rules));
    expect(is_ignored_path("src/.venv/lib/x.py", false, rules));
    expect(!is_ignored_path(".venv.txt", false, rules));
  };

  "match_negation"_test = [] {
    auto rules = rules_from_bytes("*.log\n!important.log\n");
    expect(is_ignored_path("debug.log", false, rules));
    expect(!is_ignored_path("important.log", false, rules));
    expect(is_ignored_path("other.log", false, rules));
  };

  "filter_paths_bulk"_test = [] {
    auto rules = rules_from_bytes("*.pyc\nbuild/\n!important.pyc\n");
    kimix::vector<kimix::string> paths = {
        "a.py", "a.pyc", "important.pyc", "build", "build/x.pyc", "src/b.pyc"};
    kimix::vector<bool> dirs = {false, false, false, true, false, false};
    kimix::vector<bool> mask;
    filter_paths(paths, dirs, rules, mask);
    expect(eq(mask.size(), paths.size()));
    expect(!mask[0]);
    expect(mask[1]);
    expect(!mask[2]);
    expect(mask[3]);
    expect(mask[4]);
    expect(mask[5]);
  };

  "is_ignored_name_set_and_regex"_test = [] {
    expect(is_ignored_name("node_modules"));
    expect(is_ignored_name("__pycache__"));
    expect(is_ignored_name(".git"));
    expect(!is_ignored_name("src"));

    expect(is_ignored_name("foo_cache"));
    expect(is_ignored_name("foo-cache"));
    expect(is_ignored_name("bar.egg-info"));
    expect(is_ignored_name("baz.pyo"));
    expect(is_ignored_name("backup~"));
    expect(is_ignored_name("temp.tmp"));
    expect(!is_ignored_name("foo.bar"));

    expect(is_ignored_name(""));
  };

  "parse_ls_files_output_synthesises_dirs"_test = [] {
    kimix::string input("src/a.py\0src/b.py\0tests/c.py",
                        sizeof("src/a.py\0src/b.py\0tests/c.py") - 1);
    auto out = parse_ls_files_output(input, true);
    kimix::vector<kimix::string> expected = {"src/", "src/a.py", "src/b.py",
                                             "tests/", "tests/c.py"};
    expect(eq(out.size(), expected.size()));
    bool same = true;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] != expected[i]) {
        same = false;
        break;
      }
    }
    expect(same);
  };

  "parse_ls_files_output_ignores_prefixes"_test = [] {
    kimix::string input("src/a.py\0node_modules/pkg.js\0src/b.py",
                        sizeof("src/a.py\0node_modules/pkg.js\0src/b.py") - 1);
    auto out = parse_ls_files_output(input, true);
    kimix::vector<kimix::string> expected = {"src/", "src/a.py", "src/b.py"};
    expect(eq(out.size(), expected.size()));
    bool same = true;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] != expected[i]) {
        same = false;
        break;
      }
    }
    expect(same);

    auto out_all = parse_ls_files_output(input, false);
    expect(out_all.size() > out.size());
  };

  "windows_separator_normalization"_test = [] {
    auto rules = rules_from_bytes("build/\n");
    expect(is_ignored_path(R"(build\x.py)", false, rules));

    rules = rules_from_bytes("src/*.pyc\n");
    expect(is_ignored_path(R"(src\a.pyc)", false, rules));
    expect(is_ignored_path("src/a.pyc", false, rules));
  };

  "match_case_insensitive_flag"_test = [] {
    auto rules = rules_from_bytes("*.PYC\n");
    expect(is_ignored_path("a.pyc", false, rules, true));
    expect(is_ignored_path("src/a.pyc", false, rules, true));
    expect(!is_ignored_path("a.pyc", false, rules, false));

    rules = rules_from_bytes("SRC/*.CPP\n");
    expect(is_ignored_path("src/a.cpp", false, rules, true));
    expect(!is_ignored_path("src/a.cpp", false, rules, false));

    rules = rules_from_bytes("[A-Z]*\n");
    expect(is_ignored_path("Abc.py", false, rules, true));
    expect(is_ignored_path("Abc.py", false, rules, false));
    expect(is_ignored_path("abc.py", false, rules, true));
    expect(!is_ignored_path("abc.py", false, rules, false));

    rules = rules_from_bytes("A?.PY\n");
    expect(is_ignored_path("Ab.py", false, rules, true));
    expect(!is_ignored_path("Ab.py", false, rules, false));

    // Negation interacts with the flag: an all-lowercase pattern stays
    // effective under both flags.
    rules = rules_from_bytes("*.LOG\n!IMPORTANT.LOG\n");
    expect(is_ignored_path("debug.log", false, rules, true));
    expect(!is_ignored_path("important.log", false, rules, true));
    expect(!is_ignored_path("important.log", false, rules, false));

    // filter_paths forwards the flag too.
    rules = rules_from_bytes("*.pyc\n");
    kimix::vector<kimix::string> paths = {"a.pyc", "a.PYC", "keep.py"};
    kimix::vector<bool> dirs = {false, false, false};
    kimix::vector<bool> mask;
    filter_paths(paths, dirs, rules, mask, true);
    expect(mask[0]);
    expect(mask[1]);
    expect(!mask[2]);
    filter_paths(paths, dirs, rules, mask, false);
    expect(mask[0]);
    expect(!mask[1]);
    expect(!mask[2]);
  };

  "match_platform_default"_test = [] {
    auto rules = rules_from_bytes("*.PY\n");
#ifdef _WIN32
    // Platform default: case-insensitive on Windows (fnmatch.fnmatch
    // semantics via os.path.normcase).
    expect(is_ignored_path("a.py", false, rules));
    expect(is_ignored_path("A.PY", false, rules));
#else
    // Platform default: case-sensitive on POSIX.
    expect(!is_ignored_path("a.py", false, rules));
    expect(is_ignored_path("A.PY", false, rules));
#endif
  };
}
