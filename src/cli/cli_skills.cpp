// cli/cli_skills.cpp - Skill directory discovery + skill scanning (see
// cli_skills.h for the port map and the documented deviations).
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `clisk_` prefix.

#include "cli/cli_skills.h"

#include <algorithm>
#include <system_error>

#include <core/kimix_core.h>

#include "cli/cli_common.h"
#include "cli/cli_print.h"

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Small filesystem helpers (non-throwing, like the reference's OSError
// tolerance)
// ---------------------------------------------------------------------------

bool clisk_stat(kimix::string_view path, kimix::filesystem::path &out) {
    return kimix::path_from_narrow(path, out);
}

// Python Path.exists(): true for files and directories.
bool clisk_exists(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!clisk_stat(path, p)) {
        return false;
    }
    std::error_code ec;
    return kimix::filesystem::exists(p, ec) && !ec;
}

bool clisk_is_dir(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!clisk_stat(path, p)) {
        return false;
    }
    std::error_code ec;
    return kimix::filesystem::is_directory(p, ec) && !ec;
}

bool clisk_is_regular_file(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!clisk_stat(path, p)) {
        return false;
    }
    std::error_code ec;
    return kimix::filesystem::is_regular_file(p, ec) && !ec;
}

kimix::string clisk_to_string(const kimix::filesystem::path &p) {
    return kimix::to_string(p);
}

// Append `name` to `out` when not already present (exact-match dedupe, the
// cheap local equivalent of the reference's canonical-path dedupe).
void clisk_push_unique(kimix::vector<kimix::string> &out, const kimix::string &path) {
    for (const kimix::string &existing : out) {
        if (existing == path) {
            return;
        }
    }
    out.push_back(path);
}

// ---------------------------------------------------------------------------
// COMMON_SKILL_DIRS (kimix/base.py, verbatim order)
// ---------------------------------------------------------------------------

const char *const k_clisk_common_skill_dirs[] = {
    ".agents/skills",
    ".config/.agents/skills",
    ".opencode/skills",
    ".claude/skills",
    ".codex/skills",
    ".skills",
    "skills",
};

// ---------------------------------------------------------------------------
// .kimix/skill.json (kimix/utils/config.py::_load_skill_json)
// ---------------------------------------------------------------------------

void clisk_load_skill_json(const kimix::string &cwd, kimix::vector<kimix::string> &out) {
    const kimix::string config_path = join_path(cwd, ".kimix/skill.json");
    if (!clisk_exists(config_path)) {
        return;
    }
    print_debug(".kimix/skill.json exists.");

    kimix::string text;
    kimix::string read_error;
    if (!read_file(config_path, text, read_error)) {
        print_warning("Failed to read skill_dir from .kimix/skill.json: " + read_error);
        return;
    }
    yyjson_doc *doc =
        yyjson_read_opts(text.data(), text.size(), 0, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        print_warning("Failed to read skill_dir from .kimix/skill.json: invalid JSON");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *skill_dir = yyjson_is_obj(root) ? yyjson_obj_get(root, "skill_dir") : nullptr;

    kimix::vector<kimix::string> entries;
    if (yyjson_is_str(skill_dir)) {
        entries.emplace_back(yyjson_get_str(skill_dir), yyjson_get_len(skill_dir));
    } else if (yyjson_is_arr(skill_dir)) {
        size_t idx, max;
        yyjson_val *value;
        yyjson_arr_foreach(skill_dir, idx, max, value) {
            if (yyjson_is_str(value)) {
                entries.emplace_back(yyjson_get_str(value), yyjson_get_len(value));
            }
        }
    }

    for (const kimix::string &sd : entries) {
        if (sd.empty()) {
            continue;
        }
        // Non-absolute entries resolve against the CWD (then normalised,
        // matching Path.resolve()).
        const kimix::string resolved = absolute_path(sd);
        if (clisk_is_dir(resolved)) {
            clisk_push_unique(out, resolved);
            print_debug("Skill dir from config: " + resolved);
        } else {
            print_warning("Skill dir from config not found: " + resolved);
        }
    }
    yyjson_doc_free(doc);
}

// ---------------------------------------------------------------------------
// SKILL.md frontmatter (kimi_cli.utils.frontmatter + parse_skill_text)
// ---------------------------------------------------------------------------

constexpr size_t k_clisk_description_fallback_max = 240;

// Parse the simple `key: value` lines between the leading `---` fences.
// `body_start` receives the first body line: 0 when there is no frontmatter
// (strip_frontmatter's no-op keeps the whole text as the body), or close+1
// after a closed fence.
void clisk_parse_frontmatter(const kimix::vector<kimix::string> &lines,
                             kimix::string &name, kimix::string &description,
                             size_t &body_start) {
    body_start = 0;
    if (lines.empty() || !(trim(lines[0]) == "---")) {
        return;
    }
    size_t close = 1;
    while (close < lines.size() && !(trim(lines[close]) == "---")) {
        ++close;
    }
    if (close >= lines.size()) {
        return; // an opener that never closes: no frontmatter, whole text is body
    }
    body_start = close + 1;
    for (size_t i = 1; i < close; ++i) {
        const kimix::string_view line = trim(lines[i]);
        for (const char *key : {"name", "description"}) {
            const kimix::string prefix = kimix::string(key) + ":";
            if (!starts_with(line, prefix)) {
                continue;
            }
            kimix::string value(trim(line.substr(prefix.size())));
            // YAML would unquote a plain scalar wrapped in matching quotes.
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }
            if (key[0] == 'n') {
                name = std::move(value);
            } else {
                description = std::move(value);
            }
            break;
        }
    }
}

// _first_meaningful_line: first non-empty body line that is not a stray "---".
kimix::string clisk_first_body_line(const kimix::vector<kimix::string> &lines, size_t body_start) {
    for (size_t i = body_start; i < lines.size(); ++i) {
        const kimix::string_view stripped = trim(lines[i]);
        if (!stripped.empty() && !(stripped == "---")) {
            return kimix::string(stripped);
        }
    }
    return {};
}

// _truncate: clip to the fallback budget, appending an ellipsis.
kimix::string clisk_truncate_description(const kimix::string &text) {
    if (text.size() <= k_clisk_description_fallback_max) {
        return text;
    }
    kimix::string out = text.substr(0, k_clisk_description_fallback_max - 1);
    // Hex-escaped U+2026 (UTF-8 E2 80 A6): a raw ellipsis misdecodes under a
    // legacy ANSI codepage (e.g. GBK) and breaks string-literal lexing (C2001).
    return kimix::string(trim(out)) + "\xE2\x80\xA6";
}

// parse_skill_text's name/description resolution for one markdown file.
// `default_name` is the directory name (subdir form) or the file stem (flat).
void clisk_parse_skill_text(const kimix::string &content, const kimix::string &default_name,
                            kimix::string &name, kimix::string &description) {
    kimix::vector<kimix::string> lines;
    split_lines(content, lines);
    size_t body_start = lines.size();
    kimix::string frontmatter_name, frontmatter_description;
    clisk_parse_frontmatter(lines, frontmatter_name, frontmatter_description, body_start);

    name = frontmatter_name.empty() ? default_name : frontmatter_name;
    if (!frontmatter_description.empty()) {
        description = frontmatter_description;
        return;
    }
    const kimix::string fallback = clisk_first_body_line(lines, body_start);
    description = fallback.empty() ? kimix::string("No description provided.")
                                   : clisk_truncate_description(fallback);
}

// ---------------------------------------------------------------------------
// discover_skills (kimi_cli/skill/__init__.py::discover_skills)
// ---------------------------------------------------------------------------

// True when `claimed` (lowercased names) already holds `key`.
bool clisk_claim(kimix::vector<kimix::string> &claimed, const kimix::string &key) {
    for (const kimix::string &existing : claimed) {
        if (existing == key) {
            return false;
        }
    }
    claimed.push_back(key);
    return true;
}

// Scan one root.  `claimed` holds the lowercased names already taken (across
// all roots, first occurrence wins); `out` collects in discovery order, the
// caller sorts.
void clisk_discover_in_root(const kimix::string &root, const kimix::string &scope,
                            kimix::vector<kimix::string> &claimed,
                            kimix::vector<skill_info> &out) {
    if (!clisk_is_dir(root)) {
        return;
    }
    kimix::filesystem::path root_path;
    if (!clisk_stat(root, root_path)) {
        return;
    }

    // Pass 1: subdirectory form <root>/<name>/SKILL.md (canonical).
    {
        std::error_code ec;
        kimix::filesystem::directory_iterator it(root_path, ec);
        const kimix::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const kimix::filesystem::path entry = it->path();
            it.increment(ec);
            std::error_code dir_ec;
            const bool is_dir = kimix::filesystem::is_directory(entry, dir_ec) && !dir_ec;
            if (!is_dir) {
                continue;
            }
            const kimix::filesystem::path md = entry / "SKILL.md";
            std::error_code file_ec;
            if (!kimix::filesystem::is_regular_file(md, file_ec) || file_ec) {
                continue;
            }
            kimix::string content, read_error;
            const kimix::string md_str = clisk_to_string(md);
            if (!read_file(md_str, content, read_error)) {
                continue; // the reference logs and skips unreadable entries
            }
            skill_info skill;
            skill.scope = scope;
            skill.md_file = md_str;
            clisk_parse_skill_text(content, clisk_to_string(entry.filename()), skill.name,
                                   skill.description);
            const kimix::string key = to_lower_ascii(skill.name);
            if (clisk_claim(claimed, key)) {
                out.push_back(std::move(skill));
            }
        }
    }

    // Pass 2: flat <root>/<name>.md; the subdirectory form wins on a name
    // clash.  A bare SKILL.md at the root is a stray marker, not a skill.
    {
        std::error_code ec;
        kimix::filesystem::directory_iterator it(root_path, ec);
        const kimix::filesystem::directory_iterator end;
        while (!ec && it != end) {
            const kimix::filesystem::path entry = it->path();
            it.increment(ec);
            std::error_code dir_ec;
            if (kimix::filesystem::is_directory(entry, dir_ec) || dir_ec) {
                continue;
            }
            const kimix::string file = clisk_to_string(entry.filename());
            const kimix::string lower = to_lower_ascii(file);
            if (!ends_with(lower, ".md") || lower == "skill.md") {
                continue;
            }
            kimix::string content, read_error;
            const kimix::string md_str = clisk_to_string(entry);
            if (!read_file(md_str, content, read_error)) {
                continue;
            }
            skill_info skill;
            skill.scope = scope;
            skill.md_file = md_str;
            // _strip_md_suffix: filename without the trailing ".md".
            clisk_parse_skill_text(content, file.substr(0, file.size() - 3), skill.name,
                                   skill.description);
            const kimix::string key = to_lower_ascii(skill.name);
            if (clisk_claim(claimed, key)) {
                out.push_back(std::move(skill));
            }
        }
    }
}

// Scope classification for the ported pipeline: a root under the process CWD
// is "project", everything else (skill.json / -s extras) is "extra".
kimix::string clisk_scope_for(const kimix::string &root, const kimix::string &cwd) {
    if (cwd.empty()) {
        return kimix::string("extra");
    }
    const kimix::string abs = absolute_path(root);
    if (abs == cwd || starts_with(abs, cwd + "/")) {
        return kimix::string("project");
    }
#ifdef KIMIX_PLATFORM_WINDOWS
    if (starts_with(abs, cwd + "\\")) {
        return kimix::string("project");
    }
#endif
    return kimix::string("extra");
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

kimix::vector<kimix::string> discover_skill_dirs(const kimix::vector<kimix::string> &explicit_dirs) {
    kimix::vector<kimix::string> dirs;
    const kimix::string cwd = current_dir();

    // 1. Auto-detected COMMON_SKILL_DIRS under the CWD, with the `*/skills`
    //    expansion: when <p>/skills is a directory, each existing
    //    <p>/<sub>/skills takes its place (the reference's literal glob,
    //    expanded eagerly - see cli_skills.h).
    for (const char *rel : k_clisk_common_skill_dirs) {
        const kimix::string p = join_path(cwd, rel);
        if (!clisk_exists(p)) {
            continue;
        }
        if (clisk_is_dir(join_path(p, "skills"))) {
            kimix::filesystem::path p_path;
            if (!clisk_stat(p, p_path)) {
                continue;
            }
            std::error_code ec;
            kimix::filesystem::directory_iterator it(p_path, ec);
            const kimix::filesystem::directory_iterator end;
            while (!ec && it != end) {
                const kimix::filesystem::path sub = it->path();
                it.increment(ec);
                const kimix::string expanded = clisk_to_string(sub / "skills");
                if (clisk_is_dir(expanded)) {
                    clisk_push_unique(dirs, expanded);
                    print_debug("skill dir: " + expanded);
                }
            }
        } else {
            clisk_push_unique(dirs, p);
            print_debug("skill dir: " + p);
        }
    }

    // 2. .kimix/skill.json "skill_dir" entries (string or list).
    clisk_load_skill_json(cwd, dirs);

    // 3. Explicit -s/--skill-dir arguments.
    for (const kimix::string &sd : explicit_dirs) {
        if (sd.empty()) {
            continue;
        }
        const kimix::string resolved = absolute_path(sd);
        if (clisk_is_dir(resolved)) {
            clisk_push_unique(dirs, resolved);
            print_debug("Skill dir added: " + resolved);
        } else {
            print_warning("Skill dir not found: " + resolved);
        }
    }
    return dirs;
}

kimix::vector<skill_info> discover_skills(const kimix::vector<kimix::string> &roots) {
    kimix::vector<skill_info> skills;
    kimix::vector<kimix::string> claimed;
    const kimix::string cwd = current_dir();
    for (const kimix::string &root : roots) {
        clisk_discover_in_root(root, clisk_scope_for(root, cwd), claimed, skills);
    }
    // sorted(skills, key=lambda s: s.name) - ASCII name order.
    std::sort(skills.begin(), skills.end(),
              [](const skill_info &a, const skill_info &b) { return a.name < b.name; });
    return skills;
}

kimix::string format_skills_for_prompt(const kimix::vector<skill_info> &skills) {
    // _SCOPE_HEADINGS order: Project > User > Extra > Built-in.
    const std::pair<const char *, const char *> headings[] = {
        {"project", "Project"},
        {"user", "User"},
        {"extra", "Extra"},
        {"builtin", "Built-in"},
    };
    kimix::vector<kimix::string> sections;
    for (const auto &[scope, heading] : headings) {
        kimix::vector<const skill_info *> bucket;
        for (const skill_info &skill : skills) {
            if (skill.scope == scope) {
                bucket.push_back(&skill);
            }
        }
        if (bucket.empty()) {
            continue;
        }
        std::sort(bucket.begin(), bucket.end(),
                  [](const skill_info *a, const skill_info *b) { return a->name < b->name; });
        kimix::string section = kimix::string("### ") + heading;
        for (const skill_info *skill : bucket) {
            section += "\n- " + skill->name;
            section += "\n  - Path: " + skill->md_file;
            section += "\n  - Description: " + skill->description;
        }
        sections.push_back(std::move(section));
    }
    if (sections.empty()) {
        return kimix::string("No skills found.");
    }
    kimix::string out;
    for (size_t i = 0; i < sections.size(); ++i) {
        if (i != 0) {
            out += "\n\n";
        }
        out += sections[i];
    }
    return out;
}

skill_bundle init_skill_bundle(const kimix::vector<kimix::string> &explicit_dirs) {
    skill_bundle bundle;
    bundle.dirs = discover_skill_dirs(explicit_dirs);
    bundle.prompt_text = format_skills_for_prompt(discover_skills(bundle.dirs));
    return bundle;
}

} // namespace kimix::cli
