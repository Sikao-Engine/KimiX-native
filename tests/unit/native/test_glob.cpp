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
#include "bench_util.h"
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

// --- benchmark data builders (see bench_util.h contract) ---

// 10 realistic rules (which the synthetic path sets actually exercise) plus
// count-10 synthetic rules (dir-only, anchored, wildcards, negation, **,
// bracket classes).
static kimix::string bench_gitignore_content(size_t count) {
  kimix::string out;
  out.reserve(count * 32);
  out += "build/\n";
  out += "/src/generated/\n";
  out += "*.pyc\n";
  out += "node_modules/\n";
  out += "dist/\n";
  out += "**/__pycache__/\n";
  out += "*.log\n";
  out += "!keep.log\n";
  out += "docs/**/*.md\n";
  out += "tmp/\n";
  for (size_t i = 0; i + 10 < count; ++i) {
    const size_t kind = i % 8;
    switch (kind) {
    case 0:
      out += kimix::format("build_{}/\n", i);
      break;
    case 1:
      out += kimix::format("/anchored_{}/temp\n", i);
      break;
    case 2:
      out += kimix::format("src/*.cpp_{}\n", i);
      break;
    case 3:
      out += kimix::format("*.log_{}\n", i);
      break;
    case 4:
      out += kimix::format("!keep_{}.log_{}\n", i, i);
      break;
    case 5:
      out += kimix::format("**/node_modules_{}\n", i);
      break;
    case 6:
      out += kimix::format("[A-Za-z]_{}*\n", i);
      break;
    default:
      out += kimix::format("cache_{}/**\n", i);
      break;
    }
  }
  return out;
}

static kimix::vector<kimix::string> bench_make_paths(size_t n,
                                                     bool with_dirs) {
  kimix::vector<kimix::string> paths;
  paths.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    kimix::string p;
    switch (i % 8) {
    case 0:
      p = kimix::format("src/unit/module_{}/file_{}.py", i % 100, i);
      break;
    case 1:
      p = kimix::format("src/__pycache__/mod_{}.pyc", i);
      break;
    case 2:
      p = kimix::format("build/pkg_{}/obj_{}.o", i % 100, i);
      break;
    case 3:
      p = kimix::format("logs/debug_{}.log", i);
      break;
    case 4:
      p = kimix::format("keep_{}.log", i);
      break;
    case 5:
      p = kimix::format("tests/case_{}.py", i);
      break;
    case 6:
      p = kimix::format("a/b/c/d/file_{}.txt", i);
      break;
    default:
      p = "keep.log";
      break;
    }
    if (with_dirs && i % 3 == 0) {
      p.push_back('/');
    }
    paths.push_back(std::move(p));
  }
  return paths;
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

  // --- benchmarks (see bench_util.h contract) ---
  // No hard timing assertions; expect() guards verify the measured path.

  "bench_glob_parse_500_rules"_test = [] {
    const auto content = bench_gitignore_content(500);
    kimix::vector<gitignore_rule> rules;
    size_t total_rules = 0;
    kimix_bench::time_op("glob/parse_500_rules", [&] {
      rules = parse_gitignore(content);
      total_rules += rules.size();
    });
    kimix_bench::sink(total_rules);
    expect(eq(rules.size(), size_t(500)));
    expect(total_rules % 500 == 0 && total_rules > 0);
    expect(!rules[3].negated);
    expect(rules[7].negated); // "!keep.log"
    expect(rules[0].dir_only);
  };

  "bench_glob_match_10k_mixed"_test = [] {
    const auto rules = parse_gitignore(bench_gitignore_content(500));
    const auto paths = bench_make_paths(10000, true);
    kimix::vector<bool> dirs;
    dirs.reserve(paths.size());
    size_t bytes = 0;
    for (const auto &p : paths) {
      dirs.push_back(!p.empty() && p.back() == '/');
      bytes += p.size() + 1;
    }
    size_t ignored = 0;
    kimix_bench::run("glob/match_10k_mixed", [&] {
      ignored = 0;
      for (size_t i = 0; i < paths.size(); ++i) {
        ignored += is_ignored_path(paths[i], dirs[i], rules) ? 1u : 0u;
      }
    }, paths.size(), double(bytes));
    kimix_bench::sink(ignored);
    expect(ignored > 0);
    expect(ignored < paths.size());
    // spot-check known semantics against the rule list.
    expect(is_ignored_path("build/x/obj.o", false, rules));
    expect(is_ignored_path("src/__pycache__/m.pyc", false, rules));
    expect(is_ignored_path("logs/debug.log", false, rules));
    expect(!is_ignored_path("keep.log", false, rules));
    expect(!is_ignored_path("src/main.py", false, rules));
  };

  "bench_glob_match_10k_files_nested"_test = [] {
    // Deep file-only paths: exercises unanchored patterns and dir-only
    // ancestor checks along nested components.
    const auto rules = parse_gitignore(bench_gitignore_content(500));
    kimix::vector<kimix::string> paths;
    paths.reserve(10000);
    size_t bytes = 0;
    for (size_t i = 0; i < 10000; ++i) {
      kimix::string p;
      if (i % 11 == 0) {
        p = kimix::format("src/__pycache__/mod_{}.pyc", i);
      } else if (i % 17 == 0) {
        p = kimix::format("build/pkg_{}/obj_{}.o", i % 100, i);
      } else if (i % 29 == 0) {
        p = "keep.log";
      } else {
        p = kimix::format("src/{}/module_{}/sub/file_{}.cpp", i % 50,
                          i % 500, i);
      }
      bytes += p.size() + 1;
      paths.push_back(std::move(p));
    }
    size_t ignored = 0;
    kimix_bench::run("glob/match_10k_files_nested", [&] {
      ignored = 0;
      for (const auto &p : paths) {
        ignored += is_ignored_path(p, false, rules) ? 1u : 0u;
      }
    }, paths.size(), double(bytes));
    kimix_bench::sink(ignored);
    expect(ignored > 0);
    expect(ignored < paths.size());
    expect(is_ignored_path("src/deep/x.pyc", false, rules));
    expect(!is_ignored_path("src/deep/main.cpp", false, rules));
  };

  "bench_glob_gitignore_match_1rule_10k"_test = [] {
    // Single-rule API over 10k paths with a nested ** pattern.
    const auto rules = rules_from_bytes("**/node_modules\n");
    const auto &rule = rules.front();
    kimix::vector<kimix::string> paths;
    paths.reserve(10000);
    size_t bytes = 0;
    for (size_t i = 0; i < 10000; ++i) {
      paths.emplace_back(
          kimix::format("src/packages/{}/node_modules", i % 1000));
      bytes += paths.back().size() + 1;
    }
    size_t ignored = 0;
    kimix_bench::run("glob/gitignore_match_1rule_10k", [&] {
      ignored = 0;
      for (const auto &p : paths) {
        ignored += gitignore_match(p, true, rule) ? 1u : 0u;
      }
    }, paths.size(), double(bytes));
    kimix_bench::sink(ignored);
    expect(eq(ignored, paths.size()));
    expect(gitignore_match("node_modules", true, rule));
    expect(!gitignore_match("src/node_modules/pkg.js", false, rule));
  };

  "bench_glob_filter_50k"_test = [] {
    const auto rules = parse_gitignore(bench_gitignore_content(500));
    const auto paths = bench_make_paths(50000, true);
    kimix::vector<bool> dirs;
    dirs.reserve(paths.size());
    size_t bytes = 0;
    for (const auto &p : paths) {
      dirs.push_back(!p.empty() && p.back() == '/');
      bytes += p.size() + 1;
    }
    kimix::vector<bool> mask;
    size_t checksum = 0;
    kimix_bench::run("glob/filter_paths_50k", [&] {
      filter_paths(paths, dirs, rules, mask);
      checksum += mask.size();
    }, 1, double(bytes));
    kimix_bench::sink(checksum);
    expect(eq(mask.size(), paths.size()));
    // Bulk API must agree with the per-path API on a 5k sample.
    size_t agree = 0;
    for (size_t i = 0; i < 5000; ++i) {
      const bool a = is_ignored_path(paths[i], dirs[i], rules);
      agree += (a == mask[i]) ? 1u : 0u;
    }
    expect(eq(agree, size_t(5000)));
  };

  "bench_glob_ignored_name_10k"_test = [] {
    kimix::vector<kimix::string> names;
    names.reserve(10000);
    for (size_t i = 0; i < 10000; ++i) {
      switch (i % 5) {
      case 0:
        names.emplace_back(kimix::format("foo{}_cache", i));
        break; // regex path (.*_cache$)
      case 1:
        names.emplace_back(kimix::format("file{}.pyc", i));
        break; // set-adjacent regex path (.*\.py[co]$)
      case 2:
        names.emplace_back(kimix::format("data{}.txt", i));
        break; // kept
      case 3:
        names.emplace_back(kimix::format("pkg{}.egg-info", i));
        break; // regex path
      default:
        names.emplace_back(kimix::format("package{}.json", i));
        break; // kept
      }
    }
    size_t ignored = 0;
    size_t bytes = 0;
    for (const auto &n : names) {
      bytes += n.size() + 1;
    }
    kimix_bench::run("glob/is_ignored_name_10k", [&] {
      ignored = 0;
      for (const auto &n : names) {
        ignored += is_ignored_name(n) ? 1u : 0u;
      }
    }, names.size(), double(bytes));
    kimix_bench::sink(ignored);
    expect(ignored > 0);
    expect(ignored < names.size());
    expect(is_ignored_name("foo_cache"));
    expect(is_ignored_name("x.pyc"));
    expect(is_ignored_name(""));
    expect(!is_ignored_name("main.cpp"));
  };
}
