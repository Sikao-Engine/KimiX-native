#include <runtime/glob/gitignore.h>

#include <cctype>
#include <regex>

namespace kimix {
namespace runtime {
namespace glob {
namespace {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

void rstrip_inplace(kimix::string &s) noexcept {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
}

kimix::string normalize_slashes(kimix::string_view s) {
  kimix::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(c == '\\' ? '/' : c);
  }
  return out;
}

kimix::vector<kimix::string_view> split_path(kimix::string_view s) {
  kimix::vector<kimix::string_view> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      if (i > start) {
        parts.emplace_back(s.data() + start, i - start);
      }
      start = i + 1;
    }
  }
  return parts;
}

kimix::string join_path(const kimix::vector<kimix::string_view> &parts,
                        size_t first, size_t last) {
  kimix::string out;
  for (size_t i = first; i < last; ++i) {
    if (i > first) {
      out.push_back('/');
    }
    out.append(parts[i].data(), parts[i].size());
  }
  return out;
}

// ASCII-only case folding for the case-insensitive matcher (mirrors
// find_str.cpp).  Python's fnmatch.fnmatch on Windows folds full Unicode via
// os.path.normcase; the native kernel folds A-Z only -- documented limitation,
// consistent with the find_str precedent.
inline uint8_t fold_ascii(uint8_t c) noexcept {
  return (c >= 'A' && c <= 'Z') ? uint8_t(c + 32) : c;
}

// ---------------------------------------------------------------------------
// Fast glob matcher (full match).  Mirrors fnmatch.fnmatch platform semantics:
// case-insensitive on Windows, case-sensitive on POSIX.  Supports * (any
// chars), ? (one char), and [...] / [!...] character classes.  Works on
// string_views to avoid allocations.
// ---------------------------------------------------------------------------

bool match_bracket(kimix::string_view pat, size_t &pi, char ch, bool ci) {
  if (pi >= pat.size()) {
    return ch == '[';
  }
  bool negated = false;
  if (pat[pi] == '!' || pat[pi] == '^') {
    negated = true;
    ++pi;
  }
  const uint8_t folded_ch =
      ci ? fold_ascii(static_cast<uint8_t>(ch)) : static_cast<uint8_t>(ch);
  bool matched = false;
  while (pi < pat.size() && pat[pi] != ']') {
    const char lo = pat[pi++];
    if (pi + 1 < pat.size() && pat[pi] == '-' && pat[pi + 1] != ']') {
      const char hi = pat[pi + 1];
      pi += 2;
      const uint8_t folded_lo =
          ci ? fold_ascii(static_cast<uint8_t>(lo)) : static_cast<uint8_t>(lo);
      const uint8_t folded_hi =
          ci ? fold_ascii(static_cast<uint8_t>(hi)) : static_cast<uint8_t>(hi);
      if (folded_ch >= folded_lo && folded_ch <= folded_hi) {
        matched = true;
      }
    } else {
      const uint8_t folded_lo =
          ci ? fold_ascii(static_cast<uint8_t>(lo)) : static_cast<uint8_t>(lo);
      if (folded_ch == folded_lo) {
        matched = true;
      }
    }
  }
  if (pi < pat.size() && pat[pi] == ']') {
    ++pi;
  }
  return negated ? !matched : matched;
}

bool match_one(kimix::string_view pat, size_t &pi, kimix::string_view text,
               size_t &ti, bool ci) {
  if (pi >= pat.size() || ti >= text.size()) {
    return false;
  }
  const char pc = pat[pi++];
  if (pc == '?') {
    ++ti;
    return true;
  }
  if (pc == '[') {
    return match_bracket(pat, pi, text[ti++], ci);
  }
  if (ci) {
    return fold_ascii(static_cast<uint8_t>(pc)) ==
           fold_ascii(static_cast<uint8_t>(text[ti++]));
  }
  return pc == text[ti++];
}

bool glob_match(kimix::string_view pat, kimix::string_view text, bool ci) {
  size_t pi = 0;
  size_t ti = 0;
  const size_t star_sentinel = pat.size();
  size_t star_pi = star_sentinel;
  size_t star_ti = 0;

  while (ti < text.size()) {
    if (pi < pat.size() && pat[pi] == '*') {
      star_pi = pi++;
      star_ti = ti;
      continue;
    }
    if (pi < pat.size() && match_one(pat, pi, text, ti, ci)) {
      continue;
    }
    if (star_pi != star_sentinel) {
      pi = star_pi + 1;
      ti = ++star_ti;
      continue;
    }
    return false;
  }

  while (pi < pat.size() && pat[pi] == '*') {
    ++pi;
  }
  return pi == pat.size();
}

// Match an unanchored pattern against the basename or any directory component.
bool match_unanchored(kimix::string_view pattern, kimix::string_view text,
                      bool ci) {
  const size_t last_slash = text.find_last_of('/');
  const kimix::string_view basename = (last_slash == kimix::string_view::npos)
                                          ? text
                                          : text.substr(last_slash + 1);
  if (glob_match(pattern, basename, ci)) {
    return true;
  }

  size_t start = 0;
  while (true) {
    const size_t slash = text.find('/', start);
    if (slash == kimix::string_view::npos) {
      break;
    }
    if (glob_match(pattern, text.substr(start, slash - start), ci)) {
      return true;
    }
    start = slash + 1;
  }
  return false;
}

// ---------------------------------------------------------------------------
// **-pattern matching (mirrors _gitignore_match in glob.py).
// ---------------------------------------------------------------------------

bool match_double_star(const gitignore_rule &rule,
                       kimix::string_view rel_path, bool ci) {
  const kimix::string_view pattern = rule.pattern;

  if (pattern == "**") {
    return true;
  }

  const auto parts = split_path(rel_path);

  if (pattern.size() >= 3 && pattern.compare(0, 3, "**/") == 0) {
    const kimix::string_view suffix = pattern.substr(3);
    for (size_t i = 0; i < parts.size(); ++i) {
      const kimix::string_view sub(
          parts[i].data(), rel_path.data() + rel_path.size() - parts[i].data());
      if (glob_match(suffix, sub, ci) || glob_match(suffix, parts.back(), ci)) {
        return true;
      }
    }
    return false;
  }

  if (pattern.size() >= 3 &&
      pattern.compare(pattern.size() - 3, 3, "/**") == 0) {
    const kimix::string_view prefix = pattern.substr(0, pattern.size() - 3);
    if (rel_path.size() >= prefix.size() + 1 &&
        rel_path.compare(0, prefix.size(), prefix) == 0 &&
        rel_path[prefix.size()] == '/') {
      return true;
    }
    return rel_path == prefix;
  }

  const size_t ds = pattern.find("/**/");
  if (ds != kimix::string::npos) {
    const kimix::string_view prefix = pattern.substr(0, ds);
    const kimix::string_view suffix = pattern.substr(ds + 4);
    kimix::string_view rest;
    bool has_rest = false;
    if (rel_path.size() >= prefix.size() + 1 &&
        rel_path.compare(0, prefix.size(), prefix) == 0 &&
        rel_path[prefix.size()] == '/') {
      rest = rel_path.substr(prefix.size() + 1);
      has_rest = true;
    } else if (rel_path == prefix) {
      rest = kimix::string_view();
      has_rest = true;
    }
    if (has_rest) {
      if (suffix.empty()) {
        return true;
      }
      const auto rest_parts = split_path(rest);
      for (size_t i = 0; i < rest_parts.size(); ++i) {
        const kimix::string_view sub(rest_parts[i].data(),
                                     rest.data() + rest.size() -
                                         rest_parts[i].data());
        if (glob_match(suffix, sub, ci) ||
            glob_match(suffix, rest_parts.back(), ci)) {
          return true;
        }
      }
    }
    return false;
  }

  // Generic ** fallback: ** acts like multiple * characters, which glob_match
  // already handles. Match against the full path and the basename.
  if (glob_match(pattern, rel_path, ci)) {
    return true;
  }
  if (!parts.empty()) {
    return glob_match(pattern, parts.back(), ci);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Single-rule match (non-dir-only or directory entry).  *rel_path* must already
// be normalised to forward slashes.
// ---------------------------------------------------------------------------

bool gitignore_match_internal(kimix::string_view rel_path, bool is_dir,
                              const gitignore_rule &rule, bool ci) {
  if (rule.dir_only && !is_dir) {
    // Check every ancestor directory prefix of the file.
    size_t pos = rel_path.find('/');
    while (pos != kimix::string_view::npos) {
      if (gitignore_match_internal(rel_path.substr(0, pos), true, rule, ci)) {
        return true;
      }
      pos = rel_path.find('/', pos + 1);
    }
    return false;
  }

  const kimix::string_view pattern = rule.pattern;
  if (pattern.find("**") != kimix::string_view::npos) {
    return match_double_star(rule, rel_path, ci);
  }

  if (rule.anchored) {
    return glob_match(pattern, rel_path, ci);
  }

  return match_unanchored(pattern, rel_path, ci);
}

// ---------------------------------------------------------------------------
// Ignored-name regex state (mirrors _IGNORED_PATTERNS in file_filter.py).
// ---------------------------------------------------------------------------

const std::regex &ignored_name_regex() {
  static const std::regex re(".*_cache$"
                             "|.*-cache$"
                             "|.*\\.egg-info$"
                             "|.*\\.dist-info$"
                             "|.*\\.py[co]$"
                             "|.*\\.class$"
                             "|.*\\.sw[po]$"
                             "|.*~$"
                             "|.*\\.(?:tmp|bak)$",
                             std::regex::icase | std::regex::ECMAScript);
  return re;
}

bool ignored_name_in_set(kimix::string_view name) {
  // Hard-coded set from file_filter.py::_IGNORED_NAMES.
  static const kimix::unordered_set<kimix::string, kimix::string_hash> names =
      [] {
        kimix::unordered_set<kimix::string, kimix::string_hash> set;
        const char *init[] = {".DS_Store",
                              ".bzr",
                              ".git",
                              ".hg",
                              ".svn",
                              ".build",
                              ".cache",
                              ".coverage",
                              ".fleet",
                              ".gradle",
                              ".idea",
                              ".ipynb_checkpoints",
                              ".pnpm-store",
                              ".pytest_cache",
                              ".pub-cache",
                              ".ruff_cache",
                              ".swiftpm",
                              ".tox",
                              ".venv",
                              ".vs",
                              ".vscode",
                              ".yarn",
                              ".yarn-cache",
                              ".next",
                              ".nuxt",
                              ".parcel-cache",
                              ".svelte-kit",
                              ".turbo",
                              ".vercel",
                              "node_modules",
                              "__pycache__",
                              "build",
                              "coverage",
                              "dist",
                              "htmlcov",
                              "pip-wheel-metadata",
                              "venv",
                              ".mvn",
                              "out",
                              "target",
                              "bin",
                              "cmake-build-debug",
                              "cmake-build-release",
                              "obj",
                              "bazel-bin",
                              "bazel-out",
                              "bazel-testlogs",
                              "buck-out",
                              ".dart_tool",
                              ".serverless",
                              ".stack-work",
                              ".terraform",
                              ".terragrunt-cache",
                              "DerivedData",
                              "Pods",
                              "deps",
                              "tmp",
                              "vendor"};
        for (const char *n : init) {
          set.emplace(n);
        }
        return set;
      }();
  return names.find(kimix::string(name.data(), name.size())) != names.end();
}

// ---------------------------------------------------------------------------
// Bulk helper: normalise once per path.
// ---------------------------------------------------------------------------

bool is_ignored_path_normalized(kimix::string_view norm, bool is_dir,
                                const kimix::vector<gitignore_rule> &rules,
                                bool ci) {
  bool ignored = false;
  for (const auto &rule : rules) {
    if (gitignore_match_internal(norm, is_dir, rule, ci)) {
      ignored = !rule.negated;
    }
  }
  return ignored;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

kimix::vector<gitignore_rule>
parse_gitignore(kimix::string_view content_bytes) {
  // Normalise line endings to '\n' (handles \n, \r\n and bare \r) so the
  // split below matches Python's str.splitlines().
  kimix::string content;
  content.reserve(content_bytes.size());
  for (size_t i = 0; i < content_bytes.size(); ++i) {
    char c = content_bytes[i];
    if (c == '\r') {
      if (i + 1 < content_bytes.size() && content_bytes[i + 1] == '\n') {
        ++i;
      }
      content.push_back('\n');
    } else {
      content.push_back(c);
    }
  }

  kimix::vector<gitignore_rule> rules;
  size_t start = 0;
  while (start <= content.size()) {
    size_t end = content.find('\n', start);
    if (end == kimix::string::npos) {
      end = content.size();
    }
    kimix::string line(content.data() + start, end - start);
    rstrip_inplace(line);
    start = end + 1;

    if (line.empty() || line.front() == '#') {
      continue;
    }

    gitignore_rule rule;
    if (line.front() == '!') {
      rule.negated = true;
      line.erase(line.begin());
    }
    if (line.empty()) {
      continue;
    }

    if (line.back() == '/') {
      rule.dir_only = true;
      line.pop_back();
    }

    rule.anchored = line.find('/') != kimix::string::npos;
    if (!line.empty() && line.front() == '/') {
      line.erase(line.begin());
      rule.anchored = true;
    }

    rule.pattern = std::move(line);
    rules.push_back(std::move(rule));
  }
  return rules;
}

bool gitignore_match(kimix::string_view rel_path, bool is_dir,
                     const gitignore_rule &rule, bool case_insensitive) {
  kimix::string norm = normalize_slashes(rel_path);
  return gitignore_match_internal(norm, is_dir, rule, case_insensitive);
}

bool is_ignored_path(kimix::string_view rel_path, bool is_dir,
                     const kimix::vector<gitignore_rule> &rules,
                     bool case_insensitive) {
  kimix::string norm = normalize_slashes(rel_path);
  return is_ignored_path_normalized(norm, is_dir, rules, case_insensitive);
}

void filter_paths(const kimix::vector<kimix::string> &paths,
                  const kimix::vector<bool> &is_dir_mask,
                  const kimix::vector<gitignore_rule> &rules,
                  kimix::vector<bool> &out, bool case_insensitive) {
  out.clear();
  const size_t n = paths.size();
  out.resize(n);
  const size_t mask_n = is_dir_mask.size();
  for (size_t i = 0; i < n; ++i) {
    const bool is_dir = i < mask_n ? is_dir_mask[i] : false;
    const kimix::string norm = normalize_slashes(paths[i]);
    out[i] =
        is_ignored_path_normalized(norm, is_dir, rules, case_insensitive);
  }
}

bool is_ignored_name(kimix::string_view name) {
  if (name.empty()) {
    return true;
  }
  if (ignored_name_in_set(name)) {
    return true;
  }
  return std::regex_match(kimix::string(name.data(), name.size()),
                          ignored_name_regex());
}

kimix::vector<kimix::string>
parse_ls_files_output(kimix::string_view stdout_bytes, bool filter_ignored) {
  kimix::vector<kimix::string> paths;
  kimix::unordered_set<kimix::string, kimix::string_hash> seen_dirs;
  kimix::unordered_set<kimix::string, kimix::string_hash> ignored_prefixes;

  size_t start = 0;
  while (start <= stdout_bytes.size()) {
    size_t end = start;
    while (end < stdout_bytes.size() && stdout_bytes[end] != '\0') {
      ++end;
    }
    if (end > start) {
      kimix::string entry(stdout_bytes.data() + start, end - start);
      if (!entry.empty()) {
        const auto parts = split_path(entry);
        bool skip = false;
        if (filter_ignored && !parts.empty()) {
          for (size_t i = 0; i < parts.size(); ++i) {
            kimix::string prefix = join_path(parts, 0, i + 1) + "/";
            if (ignored_prefixes.find(prefix) != ignored_prefixes.end()) {
              skip = true;
              break;
            }
            if (is_ignored_name(parts[i])) {
              ignored_prefixes.emplace(std::move(prefix));
              skip = true;
              break;
            }
          }
        }
        if (!skip) {
          for (size_t i = 1; i < parts.size(); ++i) {
            kimix::string dir_path = join_path(parts, 0, i) + "/";
            if (seen_dirs.find(dir_path) == seen_dirs.end()) {
              seen_dirs.emplace(dir_path);
              paths.push_back(std::move(dir_path));
            }
          }
          paths.push_back(std::move(entry));
        }
      }
    }
    start = end + 1;
  }
  return paths;
}

} // namespace glob
} // namespace runtime
} // namespace kimix
