// test_cli_skills.cpp - Unit tests for the native CLI skill discovery
// (src/cli/cli_skills.h: the kimix/base.py COMMON_SKILL_DIRS pipeline, the
// .kimix/skill.json loader from kimix/utils/config.py::_load_skill_json, the
// kimi_cli/skill/__init__.py discover_skills layouts + frontmatter parsing,
// and format_skills_for_prompt's scope-grouped rendering).
//
// Every filesystem test works in a fresh directory under the system temp
// directory; the discovery pipeline is CWD-relative, so the tests park the
// process cwd with a restoring guard (same pattern as test_cli.cpp's
// resolve_config_path_search_order).
//
// Framework: Boost.UT (tests/ut/ut.hpp), main-scope test lambdas only.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "cli/cli_common.h"
#include "cli/cli_print.h"
#include "cli/cli_skills.h"

#include <system_error>

namespace {

namespace cli = kimix::cli;
using namespace boost::ut;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A fresh, empty workspace: <temp>/<name>, removed and recreated.
kimix::string ws_dir(const char *name) {
    std::error_code ec;
    const kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

bool write_text(const kimix::string &path, kimix::string_view text) {
    kimix::string error;
    return cli::write_file(path, kimix::string(text), error);
}

// Parks the process cwd in `dir` for the guard's lifetime, then restores it.
struct cwd_guard {
    kimix::filesystem::path backup;
    bool ok = false;
    cwd_guard() {
        std::error_code ec;
        backup = kimix::filesystem::current_path(ec);
        ok = !ec;
    }
    ~cwd_guard() {
        if (ok) {
            std::error_code ec;
            kimix::filesystem::current_path(backup, ec);
        }
    }
    bool go(const kimix::string &dir) {
        kimix::filesystem::path p;
        if (!kimix::path_from_narrow(dir, p)) {
            return false;
        }
        std::error_code ec;
        kimix::filesystem::current_path(p, ec);
        return !ec;
    }
};

// Joins path segments the way filesystem iteration renders them: one
// join_path per segment (join_path(a, "x/y") keeps the '/', like
// os.path.join; nesting per segment yields the all-native form).
kimix::string native_join(const kimix::string &only) { return only; }
template <typename... Rest>
kimix::string native_join(const kimix::string &first, const kimix::string &second,
                          Rest &&...rest) {
    return native_join(cli::join_path(first, second),
                       kimix::string(std::forward<Rest>(rest))...);
}

bool has_dir(const kimix::vector<kimix::string> &dirs, const kimix::string &path) {
    for (const kimix::string &d : dirs) {
        if (d == path) {
            return true;
        }
    }
    return false;
}

const cli::skill_info *find_skill(const kimix::vector<cli::skill_info> &skills,
                                  const char *name) {
    for (const cli::skill_info &skill : skills) {
        if (skill.name == name) {
            return &skill;
        }
    }
    return nullptr;
}

// Creates a subdirectory-form skill: <root>/<name>/SKILL.md with `content`.
bool make_subdir_skill(const kimix::string &root, const char *name, kimix::string_view content) {
    kimix::string error;
    const kimix::string dir = cli::join_path(root, name);
    if (!cli::make_dirs(dir, error)) {
        return false;
    }
    return write_text(cli::join_path(dir, "SKILL.md"), content);
}

} // namespace

int main() {
    using namespace boost::ut;
    // Mute the debug prints the discovery pipeline emits (their wording is
    // pinned by review against the reference; here we assert behaviour).
    cli::set_quiet(true);

    // =========================================================================
    // discover_skill_dirs: COMMON_SKILL_DIRS auto-detection (kimix/base.py
    // get_skill_dirs), the `*/skills` expansion, .kimix/skill.json
    // (_load_skill_json) and the explicit -s handling (config.py init step 4)
    // =========================================================================

    "skill_dirs_common_detection"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_common");
        kimix::string error;
        // Existing directories: "skills" and ".claude/skills" (a file at
        // ".codex/skills" also counts: the reference tests .exists()).
        expect(cli::make_dirs(cli::join_path(base, "skills"), error)) << error;
        expect(cli::make_dirs(cli::join_path(base, ".claude/skills"), error)) << error;
        expect(cli::make_dirs(cli::join_path(base, ".codex"), error)) << error;
        expect(write_text(cli::join_path(base, ".codex/skills"), "not a dir"));
        // ".agents/skills" and the rest do not exist -> skipped.

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

        const kimix::vector<kimix::string> dirs = cli::discover_skill_dirs({});
        // COMMON_SKILL_DIRS order: .claude/skills (idx 3) before
        // .codex/skills (4) before skills (6).
        expect(dirs.size() == 3u) << dirs.size();
        expect(dirs[0] == cli::join_path(base, ".claude/skills"));
        expect(dirs[1] == cli::join_path(base, ".codex/skills"));
        expect(dirs[2] == cli::join_path(base, "skills"));
    };

    "skill_dirs_glob_expansion"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_glob");
        kimix::string error;
          // The reference (base.py get_skill_dirs) only applies the
          // `p/"*/skills"` glob when a literal `<p>/skills` directory exists;
          // then every `<p>/<sub>/skills` takes the entry's place.
          expect(cli::make_dirs(cli::join_path(base, ".agents/skills/skills"), error)) << error;
          expect(cli::make_dirs(cli::join_path(base, ".agents/skills/alpha/skills"), error)) << error;
        expect(cli::make_dirs(cli::join_path(base, ".agents/skills/gamma/skills"), error)) << error;
        expect(cli::make_dirs(cli::join_path(base, ".agents/skills/beta"), error)) << error;
        // .config/.agents/skills/skills exists but no child has skills/ ->
        // the glob matches nothing and the dir contributes nothing.
        expect(cli::make_dirs(cli::join_path(base, ".config/.agents/skills/skills"), error)) << error;
        // .opencode/skills has no `skills` subdirectory -> used as-is.
        expect(cli::make_dirs(cli::join_path(base, ".opencode/skills"), error)) << error;

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

    const kimix::vector<kimix::string> dirs = cli::discover_skill_dirs({});
    expect(dirs.size() == 3u) << dirs.size();
    // The expansion joins on the COMMON_SKILL_DIRS entry, which keeps its
    // original separators (pathlib behaves the same way), so build the
    // expectation from the same construction.
            const kimix::string agents = cli::join_path(base, ".agents/skills");
            expect(has_dir(dirs, cli::join_path(cli::join_path(agents, "alpha"), "skills")));
            expect(has_dir(dirs, cli::join_path(cli::join_path(agents, "gamma"), "skills")));
            expect(!has_dir(dirs, native_join(base, ".agents", "skills", "beta")));
            expect(!has_dir(dirs,
                            native_join(base, ".config", ".agents", "skills", "skills")));
            expect(has_dir(dirs, cli::join_path(base, ".opencode/skills")));
      };

    "skill_dirs_skill_json_forms"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_json");
        kimix::string error;
        // String form, relative to the CWD.
        expect(cli::make_dirs(cli::join_path(base, "from_json_str"), error)) << error;
        // List form: one relative entry, one absolute entry.
        expect(cli::make_dirs(cli::join_path(base, "from_json_list"), error)) << error;
          const kimix::string abs_dir = ws_dir("kimix_cli_sk_json_abs");
          // JSON string escaping for the Windows path (backslash -> \\\\).
          const kimix::string abs_dir_json = cli::replace_all(abs_dir, "\\", "\\\\");
          expect(cli::make_dirs(cli::join_path(base, ".kimix"), error)) << error;
          expect(write_text(cli::join_path(base, ".kimix/skill.json"),
                            "{\"skill_dir\": [\"from_json_str\", \"missing_one\", "
                            "\"from_json_list\", 42, \"" + abs_dir_json + "\"]}"));

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

    const kimix::vector<kimix::string> dirs = cli::discover_skill_dirs({});
    expect(dirs.size() == 3u) << dirs.size();
        expect(has_dir(dirs, cli::join_path(base, "from_json_str")));
        expect(has_dir(dirs, cli::join_path(base, "from_json_list")));
        expect(has_dir(dirs, abs_dir)); // absolute entries are used as-is
        expect(!has_dir(dirs, cli::join_path(base, "missing_one"))); // warned + skipped
    };

    "skill_dirs_skill_json_tolerance"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_json_bad");
        kimix::string error;
        // Malformed JSON: warning, no dirs, no crash.
        expect(cli::make_dirs(cli::join_path(base, ".kimix"), error)) << error;
        expect(write_text(cli::join_path(base, ".kimix/skill.json"), "{not json"));
        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));
        expect(cli::discover_skill_dirs({}).empty());

        // A missing skill.json in an empty cwd: nothing, no crash.
        const kimix::string other = ws_dir("kimix_cli_sk_json_none");
        expect(guard.go(other));
        expect(cli::discover_skill_dirs({}).empty());
    };

    "skill_dirs_explicit_and_dedupe"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_explicit");
        kimix::string error;
        expect(cli::make_dirs(cli::join_path(base, "skills"), error)) << error;
        expect(cli::make_dirs(cli::join_path(base, "extra_one"), error)) << error;

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

        kimix::vector<kimix::string> explicit_dirs;
        explicit_dirs.push_back("extra_one");                 // relative -> cwd
        explicit_dirs.push_back("does_not_exist");            // warned + skipped
        explicit_dirs.push_back(cli::join_path(base, "skills")); // dup of auto
        const kimix::vector<kimix::string> dirs =
            cli::discover_skill_dirs(explicit_dirs);
        expect(dirs.size() == 2u) << dirs.size();
        expect(has_dir(dirs, cli::join_path(base, "skills"))); // deduped to one
        expect(has_dir(dirs, cli::join_path(base, "extra_one")));
        size_t skills_count = 0;
        for (const kimix::string &d : dirs) {
            if (d == cli::join_path(base, "skills")) {
                ++skills_count;
            }
        }
        expect(skills_count == 1u);
    };

    // =========================================================================
    // discover_skills: the two layouts, frontmatter parsing, dedupe, sort
    // =========================================================================

    "discover_skills_layouts_and_parse"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_scan");
        kimix::string error;
        const kimix::string root = cli::join_path(base, "root");
        expect(cli::make_dirs(root, error)) << error;

        // Subdirectory form with frontmatter (quoted values are unquoted).
        expect(make_subdir_skill(root, "zebra", "---\nname: Zebra\n"
                                              "description: \"Stripes.\"\n---\nbody\n"));
        // Flat form with frontmatter.
        expect(write_text(cli::join_path(root, "apple.md"),
                          "---\nname: Apple\ndescription: Fruit.\n---\n# Apple\n"));
        // Subdirectory form without frontmatter: dir-name fallback + first
        // body line as the description.
        expect(make_subdir_skill(root, "mango", "# Mango Title\n\nmore text\n"));
        // Name clash: the subdirectory form wins over the flat one.
        expect(make_subdir_skill(root, "Banana", "---\nname: Banana\n---\nx\n"));
        expect(write_text(cli::join_path(root, "banana.md"),
                          "---\nname: banana\ndescription: flat shadow\n---\n"));
        // A bare SKILL.md at the root is a stray marker, not a skill.
        expect(write_text(cli::join_path(root, "SKILL.md"),
                          "---\nname: Stray\n---\n"));
        // Non-markdown files are ignored.
        expect(write_text(cli::join_path(root, "notes.txt"), "name: Nope\n"));

          const kimix::vector<cli::skill_info> skills = cli::discover_skills({root});
          expect(skills.size() == 4u) << skills.size();
        // Sorted by name, ASCII order (uppercase before lowercase, exactly
        // like Python's code-point ordering).
        expect(skills[0].name == "Apple");
        expect(skills[1].name == "Banana");
        expect(skills[2].name == "Zebra");
        expect(skills[3].name == "mango");

        const cli::skill_info *apple = find_skill(skills, "Apple");
        expect(apple != nullptr);
        expect(apple->description == "Fruit.");
        expect(apple->md_file == cli::join_path(root, "apple.md"));
        expect(apple->scope == "extra"); // temp root is outside the cwd

        const cli::skill_info *zebra = find_skill(skills, "Zebra");
        expect(zebra != nullptr);
        expect(zebra->description == "Stripes.");
                  // md_file is a filesystem-native path (one join per segment).
          expect(zebra->md_file == native_join(root, "zebra", "SKILL.md"));

        const cli::skill_info *mango = find_skill(skills, "mango");
        expect(mango != nullptr);
        expect(mango->description == "# Mango Title");

        // The subdir Banana beat the flat banana.md (first occurrence wins).
        const cli::skill_info *banana = find_skill(skills, "Banana");
        expect(banana != nullptr);
                  expect(banana->md_file == native_join(root, "Banana", "SKILL.md"));
        expect(banana->description == "x"); // body fallback, no frontmatter desc
    };

    "discover_skills_project_scope"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_scope");
        kimix::string error;
        expect(make_subdir_skill(base, "proj_skill",
                                 "---\nname: ProjSkill\ndescription: d.\n---\n")) << error;

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

        const kimix::vector<kimix::string> dirs{base};
        const kimix::vector<cli::skill_info> skills = cli::discover_skills(dirs);
        expect(skills.size() == 1u) << skills.size();
        expect(skills[0].scope == "project"); // a root under the cwd
    };

    "discover_skills_description_fallbacks"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_fallback");
        kimix::string error;
        const kimix::string root = cli::join_path(base, "root");
        expect(cli::make_dirs(root, error)) << error;
        // Empty body, no frontmatter description -> the fixed fallback text.
        expect(make_subdir_skill(root, "nodesc", "---\nname: NoDesc\n---\n"));
        // No frontmatter at all on a flat file: stem name + body line.
        expect(write_text(cli::join_path(root, "plain.md"), "first line here\n\nsecond\n"));
        // A long body line is truncated to the fallback budget + ellipsis.
        kimix::string long_line(300, 'x');
        expect(write_text(cli::join_path(root, "long.md"), long_line + "\n"));

        const kimix::vector<cli::skill_info> skills = cli::discover_skills({root});
        const cli::skill_info *nodesc = find_skill(skills, "NoDesc");
        expect(nodesc != nullptr);
        expect(nodesc->description == "No description provided.");
        const cli::skill_info *plain = find_skill(skills, "plain");
        expect(plain != nullptr);
        expect(plain->description == "first line here");
        const cli::skill_info *long_skill = find_skill(skills, "long");
        expect(long_skill != nullptr);
        expect(long_skill->description.size() <= 242u) << long_skill->description.size();
        // U+2026 as hex escapes: a raw ellipsis misdecodes under legacy ANSI
      // codepages (GBK etc.) and breaks string-literal lexing (C2001).
      expect(cli::ends_with(long_skill->description, "\xE2\x80\xA6"));
    };

    // =========================================================================
    // format_skills_for_prompt: the scope-grouped golden rendering
    // =========================================================================

    "skills_format_golden"_test = [] {
        kimix::vector<cli::skill_info> skills;
        {
            cli::skill_info &s = skills.emplace_back();
            s.name = "beta";
            s.description = "Second.";
            s.md_file = "/ext/beta.md";
            s.scope = "extra";
        }
        {
            cli::skill_info &s = skills.emplace_back();
            s.name = "alpha";
            s.description = "First.";
            s.md_file = "/proj/alpha/SKILL.md";
            s.scope = "project";
        }

        const kimix::string text = cli::format_skills_for_prompt(skills);
        const kimix::string expected =
            "### Project\n"
            "- alpha\n"
            "  - Path: /proj/alpha/SKILL.md\n"
            "  - Description: First.\n"
            "\n"
            "### Extra\n"
            "- beta\n"
            "  - Path: /ext/beta.md\n"
            "  - Description: Second.";
        expect(text == expected) << text;

        // Empty scopes are omitted entirely.
        expect(cli::format_skills_for_prompt({}) == "No skills found.");
        kimix::vector<cli::skill_info> one_scope;
        {
            cli::skill_info &s = one_scope.emplace_back();
            s.name = "solo";
            s.description = "d";
            s.md_file = "m";
            s.scope = "builtin";
        }
        const kimix::string single = cli::format_skills_for_prompt(one_scope);
        expect(single == "### Built-in\n- solo\n  - Path: m\n  - Description: d");
    };

    "skills_format_sorts_within_scope"_test = [] {
        kimix::vector<cli::skill_info> skills;
        for (const char *name : {"zed", "mid", "abc"}) {
            cli::skill_info &s = skills.emplace_back();
            s.name = name;
            s.description = "d";
            s.md_file = "m";
            s.scope = "project";
        }
        const kimix::string text = cli::format_skills_for_prompt(skills);
        expect(cli::find(text, "- abc") < cli::find(text, "- mid"));
        expect(cli::find(text, "- mid") < cli::find(text, "- zed"));
    };

    // =========================================================================
    // init_skill_bundle: the whole pipeline in one call
    // =========================================================================

    "skill_bundle_end_to_end"_test = [] {
        const kimix::string base = ws_dir("kimix_cli_sk_bundle");
        kimix::string error;
        expect(cli::make_dirs(cli::join_path(base, "skills"), error)) << error;
        expect(make_subdir_skill(cli::join_path(base, "skills"), "bundled",
                                 "---\nname: Bundled\ndescription: From the bundle.\n---\n"))
            << error;

        cwd_guard guard;
        expect(guard.ok);
        expect(guard.go(base));

        const cli::skill_bundle bundle = cli::init_skill_bundle({});
        expect(bundle.dirs.size() == 1u) << bundle.dirs.size();
        expect(bundle.dirs[0] == cli::join_path(base, "skills"));
          const kimix::string expected =
              "### Project\n"
              "- Bundled\n"
              "  - Path: " + native_join(base, "skills", "bundled", "SKILL.md") + "\n"
              "  - Description: From the bundle.";
          expect(bundle.prompt_text == expected) << bundle.prompt_text;
    };
}
