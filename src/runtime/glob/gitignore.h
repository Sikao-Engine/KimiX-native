#pragma once

#include <core/dll_export.h>
#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace glob {

// Platform default for pattern matching: case-insensitive on Windows
// (mirrors fnmatch.fnmatch, which folds via os.path.normcase) and
// case-sensitive on POSIX.
inline constexpr bool default_case_insensitive() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

// A single parsed gitignore rule (mirrors _GitignoreRule in glob.py).
struct gitignore_rule {
  kimix::string pattern;
  bool negated = false;
  bool anchored = false;
  bool dir_only = false;
};

// Parse .gitignore bytes into rules.  source_dir is accepted for API symmetry
// with the Python reference but is not used by the kernel.
KIMIX_RUNTIME_API kimix::vector<gitignore_rule>
parse_gitignore(kimix::string_view content_bytes);

// Match a relative path against one rule.  *case_insensitive* defaults to the
// platform default (see default_case_insensitive): true on Windows, false
// elsewhere.
KIMIX_RUNTIME_API bool gitignore_match(kimix::string_view rel_path, bool is_dir,
                                       const gitignore_rule &rule,
                                       bool case_insensitive =
                                           default_case_insensitive());

// True if *rel_path* is ignored by the rule list (later rules override earlier
// ones).
KIMIX_RUNTIME_API bool
is_ignored_path(kimix::string_view rel_path, bool is_dir,
                const kimix::vector<gitignore_rule> &rules,
                bool case_insensitive = default_case_insensitive());

// Bulk filter: out[i] == is_ignored_path(paths[i], is_dir_mask[i], rules).
KIMIX_RUNTIME_API void filter_paths(const kimix::vector<kimix::string> &paths,
                                    const kimix::vector<bool> &is_dir_mask,
                                    const kimix::vector<gitignore_rule> &rules,
                                    kimix::vector<bool> &out,
                                    bool case_insensitive =
                                        default_case_insensitive());

// Hard-coded ignored-name fast path (mirrors file_filter.py::is_ignored).
KIMIX_RUNTIME_API bool is_ignored_name(kimix::string_view name);

// Parse NUL-delimited `git ls-files -z` output, synthesise directory entries,
// and optionally drop paths under ignored prefixes.
KIMIX_RUNTIME_API kimix::vector<kimix::string>
parse_ls_files_output(kimix::string_view stdout_bytes,
                      bool filter_ignored = true);

} // namespace glob
} // namespace runtime
} // namespace kimix
