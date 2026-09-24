// Test for the python built-in tool kernels (builtin_tools/python_tool.h).
// This test covers the plan's §7.1 list:
// - ScriptFileWriter / plan_script_path: deterministic naming, monotonic
//   index, thread-safe concurrent planning (unique paths)
// - resolve_python_exe: override > walk-up > VIRTUAL_ENV > fallback
//   precedence, Windows + POSIX candidate names, stops at first hit
// - scrub_child_env: safe-prefix keep, secret-substring drop, order, case
// - prepare_python_env: fast path (None), prepend order, venv detection via
//   pyvenv.cfg probe, PATH dedup, separator handling, empty PATH
// - module_not_found_hint: single/double quotes, byte-exact message, no match
// - build_session_output_block: full field order, nulls, empty output,
//   2-space indent predicate, elapsed :.2f golden vectors
// - extract_export_path: the 4 markers, rstrip of ] and `, no marker
// - classify_wait_pattern / match_wait_pattern: literal substring, glob
//   subset, unsupported escape hatch, empty pattern -> invalid_input
// - python_goldens_* : byte-exact replay of the vectors generated from the
//   kimi-agent Python reference by scripts/gen_python_goldens.py
//   (tests/unit/builtin_tools/python_goldens.inc), covering every kernel above
//   plus the pydantic-free contract cases (see the .inc header).
//
// The kernels build paths with the host separator (mirroring pathlib on the
// host OS), so every path-like literal in this file goes through `host()`
// which converts POSIX-style literals to the host form.
#include "ut/ut.hpp"

#include "builtin_tools/python_tool.h"

// Golden vectors generated from the kimi-agent reference by
// scripts/gen_python_goldens.py. Never edit by hand.
#include "python_goldens.inc"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools::python;
// The Tool class contract tests drive the real Tool (rust-class API from
// builtin_tools/tool.h); python_tool.h only re-exports its own vocabulary.
using kimix::builtin_tools::Session;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

namespace {

// Existence probe over a fixed allow-list (deterministic, no filesystem).
struct fake_fs {
    kimix::vector<kimix::string> files;

    kimix::function<bool(kimix::string_view)> probe() {
        return [this](kimix::string_view path) {
            for (const auto &f : files) {
                if (f == path) {
                    return true;
                }
            }
            return false;
        };
    }
};

kimix::string s(std::string_view v) { return kimix::string(v.data(), v.size()); }

std::string_view sv_of(const kimix::string &v) {
    return std::string_view(v.data(), v.size());
}

constexpr char sep_c =
#ifdef KIMIX_PLATFORM_WINDOWS
    '\\';
#else
    '/';
#endif

// Convert a POSIX-style path literal to the host separator form so tests are
// exact on Windows and POSIX alike (the kernels join with the host separator).
std::string host(std::string_view v) {
    std::string out(v.data(), v.size());
#ifdef KIMIX_PLATFORM_WINDOWS
    for (auto &c : out) {
        if (c == '/') {
            c = '\\';
        }
    }
#endif
    return out;
}

// Join components with the host separator (mirrors what the kernels build).
std::string pj(std::initializer_list<std::string_view> parts) {
    std::string out;
    bool first = true;
    for (auto p : parts) {
        if (!first) {
            out.push_back(sep_c);
        }
        out.append(host(p));
        first = false;
    }
    return out;
}

// Build a search-bases span for resolve_python_exe.  MSVC's std::span cannot
// be constructed from a braced-init-list directly, so tests go through a
// vector (host-normalized, so the kernel's joined candidates match exactly).
kimix::vector<kimix::string> bases(std::initializer_list<const char *> parts) {
    kimix::vector<kimix::string> out;
    out.reserve(parts.size());
    for (const char *p : parts) {
        std::string h = host(p);
        out.emplace_back(h.data(), h.size());
    }
    return out;
}

const char *path_sep =
#ifdef KIMIX_PLATFORM_WINDOWS
    ";";
#else
    ":";
#endif

// ---------------------------------------------------------------------------
// Golden-replay helpers (see python_goldens.inc)
// ---------------------------------------------------------------------------

// Existence probe over a '\n'-separated allow-list of exact path strings — the
// same fake-filesystem contract the generator drives the reference with.
kimix::function<bool(kimix::string_view)> exists_from_list(const char *list) {
    kimix::vector<kimix::string> allowed;
    kimix::string_view rest(list);
    while (!rest.empty()) {
        size_t nl = rest.find('\n');
        kimix::string_view line =
            (nl == kimix::string_view::npos) ? rest : rest.substr(0, nl);
        allowed.emplace_back(line.data(), line.size());
        if (nl == kimix::string_view::npos) {
            break;
        }
        rest.remove_prefix(nl + 1);
    }
    return [allowed](kimix::string_view path) {
        for (const auto &a : allowed) {
            if (a.size() == path.size() &&
                std::string_view(a.data(), a.size()) ==
                    std::string_view(path.data(), path.size())) {
                return true;
            }
        }
        return false;
    };
}

// "NAME\tVALUE" lines -> named_value vector (values may be empty).
kimix::vector<named_value> named_values_from_lines(const char *lines) {
    kimix::vector<named_value> out;
    kimix::string_view rest(lines);
    while (!rest.empty()) {
        size_t nl = rest.find('\n');
        kimix::string_view line =
            (nl == kimix::string_view::npos) ? rest : rest.substr(0, nl);
        size_t tab = line.find('\t');
        named_value nv;
        if (tab == kimix::string_view::npos) {
            nv.name.assign(line.data(), line.size());
        } else {
            nv.name.assign(line.data(), tab);
            nv.value.assign(line.data() + tab + 1, line.size() - tab - 1);
        }
        out.push_back(nv);
        if (nl == kimix::string_view::npos) {
            break;
        }
        rest.remove_prefix(nl + 1);
    }
    return out;
}

// Join the names of an env vector with '\n' (the golden's `expected` format).
kimix::string join_names(const kimix::vector<named_value> &env) {
    kimix::string out;
    for (size_t i = 0; i < env.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out.append(env[i].name.data(), env[i].name.size());
    }
    return out;
}

bool ends_with(kimix::string_view text, kimix::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ------------------------------------------------------------------ 1
    "plan_script_path_naming"_test = [] {
        expect(eq(plan_script_path(host("C:/tmp/x"), 0), s(pj({"C:/tmp/x", "0.py"}))));
        expect(eq(plan_script_path(host("C:/tmp/x"), 1), s(pj({"C:/tmp/x", "1.py"}))));
        expect(eq(plan_script_path(host("C:/tmp/x"), 42, ".txt"),
                  s(pj({"C:/tmp/x", "42.txt"}))));
        // trailing separator must not double up
        expect(eq(plan_script_path(host("C:/tmp/x/"), 3), s(pj({"C:/tmp/x", "3.py"}))));
    };

    "script_file_writer_monotonic"_test = [] {
        ScriptFileWriter w(host("C:/tmp/cache"), 0);
        expect(eq(w.next_index(), uint64_t(0)));
        expect(eq(w.plan_path(), s(pj({"C:/tmp/cache", "0.py"}))));
        expect(eq(w.plan_path(), s(pj({"C:/tmp/cache", "1.py"}))));
        expect(eq(w.next_index(), uint64_t(2)));
        ScriptFileWriter w2(host("C:/tmp/cache"), 7);
        expect(eq(w2.plan_path(".sh"), s(pj({"C:/tmp/cache", "7.sh"}))));
    };

    "script_file_writer_concurrent"_test = [] {
        ScriptFileWriter w(host("C:/tmp/cache"), 0);
        constexpr int k_threads = 10;
        kimix::vector<kimix::string> paths;
        paths.resize(k_threads);
        std::vector<std::thread> ts;
        std::atomic<size_t> slot{0};
        for (int i = 0; i < k_threads; ++i) {
            ts.emplace_back([&] {
                size_t id = slot.fetch_add(1);
                paths[id] = w.plan_path();
            });
        }
        for (auto &t : ts) {
            t.join();
        }
        // all 10 paths must be unique and cover indices 0..9
        for (size_t i = 0; i < paths.size(); ++i) {
            for (size_t j = i + 1; j < paths.size(); ++j) {
                expect(neq(paths[i], paths[j])) << "duplicate planned path";
            }
        }
        expect(eq(w.next_index(), uint64_t(k_threads)));
    };

    // ------------------------------------------------------------------ 2
    "resolve_python_override_wins"_test = [] {
        fake_fs fs;
        fs.files.push_back(s(host("C:/custom/python.exe")));
        fs.files.push_back(s(host("C:/proj/.venv/Scripts/python.exe")));
        auto r = resolve_python_exe(host("C:/custom/python.exe"),
                                    bases({"C:/proj"}), "", host("C:/sys/python.exe"),
                                    fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("C:/custom/python.exe"))));
    };

    "resolve_python_override_missing_ignored"_test = [] {
        fake_fs fs;
        fs.files.push_back(s(host("C:/proj/.venv/Scripts/python.exe")));
        auto r = resolve_python_exe(host("C:/gone/python.exe"), bases({"C:/proj"}), "",
                                    host("C:/sys/python.exe"), fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("C:/proj/.venv/Scripts/python.exe"))));
    };

    "resolve_python_walk_up"_test = [] {
        fake_fs fs;
        // venv two levels up from the session dir
        fs.files.push_back(s(host("C:/proj/.venv/Scripts/python.exe")));
        auto r = resolve_python_exe("", bases({"C:/proj/sub/deep"}), "",
                                    host("C:/sys/python.exe"), fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("C:/proj/.venv/Scripts/python.exe"))));
    };

    "resolve_python_posix_name"_test = [] {
        fake_fs fs;
        fs.files.push_back(s(host("C:/proj/.venv/bin/python")));
        auto r = resolve_python_exe("", bases({"C:/proj"}), "", host("C:/sys/py"),
                                    fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("C:/proj/.venv/bin/python"))));
    };

    "resolve_python_virtual_env"_test = [] {
        fake_fs fs;
        // VIRTUAL_ENV points at the venv root: candidates are
        // <venv>/Scripts/python.exe and <venv>/bin/python (no ".venv" prefix).
        fs.files.push_back(s(host("V:/env/Scripts/python.exe")));
        auto r = resolve_python_exe("", bases({}), host("V:/env"), host("C:/sys/python.exe"),
                                    fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("V:/env/Scripts/python.exe"))));
    };

    "resolve_python_fallback"_test = [] {
        fake_fs fs; // nothing exists
        auto r = resolve_python_exe("", bases({"C:/proj"}), host("V:/env"),
                                    host("C:/sys/python.exe"), fs.probe());
        expect(r.has_value());
        expect(eq(*r, s(host("C:/sys/python.exe"))));
        // no fallback at all -> nullopt
        auto r2 = resolve_python_exe("", bases({}), "", "", fs.probe());
        expect(!r2.has_value());
    };

    "resolve_python_precedence"_test = [] {
        fake_fs fs;
        fs.files.push_back(s(host("C:/custom/py.exe")));
        fs.files.push_back(s(host("C:/proj/.venv/Scripts/python.exe")));
        fs.files.push_back(s(host("V:/env/bin/python")));
        // override beats walk
        auto r1 = resolve_python_exe(host("C:/custom/py.exe"), bases({"C:/proj"}),
                                     host("V:/env"), host("C:/sys"), fs.probe());
        expect(eq(*r1, s(host("C:/custom/py.exe"))));
        // walk beats VIRTUAL_ENV
        auto r2 = resolve_python_exe("", bases({"C:/proj"}), host("V:/env"),
                                     host("C:/sys"), fs.probe());
        expect(eq(*r2, s(host("C:/proj/.venv/Scripts/python.exe"))));
        // VIRTUAL_ENV beats fallback
        auto r3 = resolve_python_exe("", bases({}), host("V:/env"), host("C:/sys"), fs.probe());
        expect(eq(*r3, s(host("V:/env/bin/python"))));
    };

    "resolve_python_stops_at_first_hit"_test = [] {
        fake_fs fs;
        // both session dir and cwd have a venv; session dir comes first
        fs.files.push_back(s(host("A/.venv/Scripts/python.exe")));
        fs.files.push_back(s(host("B/.venv/Scripts/python.exe")));
        auto r = resolve_python_exe("", bases({"A", "B"}), "",
                                    host("C:/sys/python.exe"), fs.probe());
        expect(eq(*r, s(host("A/.venv/Scripts/python.exe"))));
    };

    // ------------------------------------------------------------------ 3
    "scrub_child_env_rules"_test = [] {
        kimix::vector<named_value> env = {
            {"PATH", "/usr/bin"},
            {"HOME", "/h"},
            {"VIRTUAL_ENV", "/v"},
            {"AWS_SECRET_ACCESS_KEY", "x"}, // secret substrings -> drop
            {"MY_TOKEN", "x"},              // drop
            {"DATABASE_URL", "postgres://x"}, // no secret name -> keep
            {"KIMIX_TOKEN", "x"},           // KIMIX_ safe prefix -> keep
            {"PYTHONPATH", "/p"},           // PYTHON prefix -> keep
            {"GITHUB_AUTH_TOKEN", "x"},     // not a safe prefix; AUTH/TOKEN -> drop
            {"GIT_TOKEN", "x"},             // GIT_ safe prefix -> keep
            {"lowercase_key", "x"},         // upper -> KEY -> drop
            {"SSH_AUTH_SOCK", "/s"},        // SSH_ prefix -> keep
            {"UV_TOKEN", "x"},              // UV_ prefix -> keep
            {"NUMBER_OF_PROCESSORS", "4"},  // safe
        };
        auto out = scrub_child_env(env);
        kimix::vector<kimix::string> names;
        for (const auto &e : out) {
            names.push_back(e.name);
        }
        expect(eq(names.size(), size_t(10)));
        // order preserved exactly
        expect(eq(names[0], s("PATH")));
        expect(eq(names[1], s("HOME")));
        expect(eq(names[2], s("VIRTUAL_ENV")));
        expect(eq(names[3], s("DATABASE_URL")));
        expect(eq(names[4], s("KIMIX_TOKEN")));
        expect(eq(names[5], s("PYTHONPATH")));
        expect(eq(names[6], s("GIT_TOKEN")));
        expect(eq(names[7], s("SSH_AUTH_SOCK")));
        expect(eq(names[8], s("UV_TOKEN")));
        expect(eq(names[9], s("NUMBER_OF_PROCESSORS")));
    };

    "prepare_python_env_fast_path"_test = [] {
        fake_fs fs; // no pyvenv.cfg anywhere
        auto probe = fs.probe();
        // bin already first -> None
        auto r = prepare_python_env(host("C:/sys/python.exe"), host("C:/share/bin"),
                                    s(host("C:/share/bin")) + path_sep + s(host("C:/other")),
                                    path_sep, probe);
        expect(!r.has_value()) << "bin already first must take fast path";
        // PATH == bin exactly -> None
        auto r2 = prepare_python_env(host("C:/sys/python.exe"), host("C:/share/bin"),
                                     host("C:/share/bin"), path_sep, probe);
        expect(!r2.has_value());
    };

    "prepare_python_env_prepend"_test = [] {
        fake_fs fs;
        auto probe = fs.probe();
        auto r = prepare_python_env(host("C:/sys/python.exe"), host("C:/share/bin"),
                                    s(host("C:/a")) + path_sep + s(host("C:/b")),
                                    path_sep, probe);
        expect(r.has_value());
        expect(eq(r->size(), size_t(1)));
        expect(eq((*r)[0].name, s("PATH")));
        expect(eq((*r)[0].value,
                  s(host("C:/share/bin")) + path_sep + s(host("C:/a")) + path_sep +
                      s(host("C:/b"))));
    };

    "prepare_python_env_dedup"_test = [] {
        fake_fs fs;
        auto probe = fs.probe();
        // duplicate share_bin_dir entries and empty entries are removed
        auto r = prepare_python_env(
            host("C:/sys/python.exe"), host("C:/share/bin"),
            s(host("C:/a")) + path_sep + path_sep + s(host("C:/share/bin")) + path_sep +
                s(host("C:/b")),
            path_sep, probe);
        expect(r.has_value());
        expect(eq((*r)[0].value,
                  s(host("C:/share/bin")) + path_sep + s(host("C:/a")) + path_sep +
                      s(host("C:/b"))));
    };

    "prepare_python_env_empty_path"_test = [] {
        fake_fs fs;
        auto probe = fs.probe();
        auto r = prepare_python_env(host("C:/sys/python.exe"), host("C:/share/bin"), "",
                                    path_sep, probe);
        expect(r.has_value());
        expect(eq((*r)[0].name, s("PATH")));
        expect(eq((*r)[0].value, s(host("C:/share/bin"))));
    };

    "prepare_python_env_venv"_test = [] {
        fake_fs fs;
        fs.files.push_back(s(host("C:/proj/.venv/pyvenv.cfg")));
        auto probe = fs.probe();
        auto r = prepare_python_env(host("C:/proj/.venv/Scripts/python.exe"),
                                    host("C:/share/bin"),
                                    s(host("C:/a")) + path_sep + s(host("C:/b")),
                                    path_sep, probe);
        expect(r.has_value());
        expect(eq(r->size(), size_t(2)));
        // VIRTUAL_ENV assigned first, then PATH
        expect(eq((*r)[0].name, s("VIRTUAL_ENV")));
        expect(eq((*r)[0].value, s(host("C:/proj/.venv"))));
        expect(eq((*r)[1].name, s("PATH")));
        expect(eq((*r)[1].value,
                  s(host("C:/share/bin")) + path_sep +
                      s(host("C:/proj/.venv/Scripts")) + path_sep + s(host("C:/a")) +
                      path_sep + s(host("C:/b"))));
    };

    "prepare_python_env_no_pyvenv_cfg"_test = [] {
        // exe inside Scripts/ but pyvenv.cfg missing -> not a venv
        fake_fs fs;
        auto probe = fs.probe();
        auto r = prepare_python_env(host("C:/proj/.venv/Scripts/python.exe"),
                                    host("C:/share/bin"), host("C:/a"), path_sep, probe);
        expect(r.has_value());
        expect(eq(r->size(), size_t(1))) << "no VIRTUAL_ENV without pyvenv.cfg";
        expect(eq((*r)[0].name, s("PATH")));
    };

    // ------------------------------------------------------------------ 4
    "module_not_found_hint_single_quote"_test = [] {
        auto hint = module_not_found_hint(
            "Traceback (most recent call last):\n"
            "ModuleNotFoundError: No module named 'numpy'\n",
            "C:/py/python.exe");
        expect(eq(hint, s(" Hint: the script ran with interpreter "
                          "'C:/py/python.exe'. If you installed the package "
                          "with plain 'pip install', it may have gone to a "
                          "different environment. Retry with "
                          "'C:/py/python.exe' -m pip install numpy.")));
    };

    "module_not_found_hint_double_quote"_test = [] {
        auto hint = module_not_found_hint(
            "ModuleNotFoundError: No module named \"pandas\"", "P");
        expect(eq(hint, s(" Hint: the script ran with interpreter 'P'. If "
                          "you installed the package with plain 'pip "
                          "install', it may have gone to a different "
                          "environment. Retry with 'P' -m pip install "
                          "pandas.")));
    };

    "module_not_found_hint_no_match"_test = [] {
        expect(eq(module_not_found_hint("all good", "P").size(), size_t(0)));
        expect(eq(module_not_found_hint("", "P").size(), size_t(0)));
        expect(eq(
            module_not_found_hint(
                "ImportError: cannot import name 'x'", "P")
                .size(),
            size_t(0)));
        // empty module name does not match (regex requires [^'"]+)
        expect(eq(module_not_found_hint(
                      "ModuleNotFoundError: No module named ''", "P")
                      .size(),
                  size_t(0)));
    };

    // ------------------------------------------------------------------ 5
    "session_block_full"_test = [] {
        session_output_block b;
        b.task_id = "t-123";
        b.status = "completed";
        b.output = "line1\nline2\n";
        b.wait_matched = true;
        b.elapsed_seconds = 1.234;
        b.exit_code = 0;
        b.exit_code_meaning = s("success");
        b.output_path = s("C:/x/out.txt");
        b.output_truncated = false;
        auto got = build_session_output_block(b);
        expect(eq(got, s("task_id: t-123\n"
                         "status: completed\n"
                         "exit_code: 0\n"
                         "exit_code_meaning: success\n"
                         "failure_hint: null\n"
                         "output: |\n"
                         "  line1\n"
                         "  line2\n"
                         "output_truncated: false\n"
                         "output_path: C:/x/out.txt\n"
                         "wait_matched: true\n"
                         "elapsed_seconds: 1.23\n"
                         "original_path: null")));
    };

    "session_block_empty_output_nulls"_test = [] {
        session_output_block b;
        b.task_id = "abc";
        b.status = "running";
        auto got = build_session_output_block(b);
        expect(eq(got, s("task_id: abc\n"
                         "status: running\n"
                         "exit_code: null\n"
                         "exit_code_meaning: null\n"
                         "failure_hint: null\n"
                         "output: |\n"
                         "  (no output)\n"
                         "output_truncated: false\n"
                         "output_path: null\n"
                         "wait_matched: null\n"
                         "elapsed_seconds: null\n"
                         "original_path: null")));
    };

    "session_block_indent_predicate"_test = [] {
        // textwrap.indent's default predicate indents every line that is not
        // whitespace-only — including lines that already start with
        // whitespace (e.g. " beta" -> "   beta") — and leaves blank /
        // whitespace-only lines untouched.
        session_output_block b;
        b.task_id = "x";
        b.status = "completed";
        b.output = "alpha\n\n beta\ngamma\n\n\n";
        b.wait_matched = false;
        b.elapsed_seconds = 0.005;
        auto got = build_session_output_block(b);
        expect(eq(got, s("task_id: x\n"
                         "status: completed\n"
                         "exit_code: null\n"
                         "exit_code_meaning: null\n"
                         "failure_hint: null\n"
                         "output: |\n"
                         "  alpha\n"
                         "\n"
                         "   beta\n"
                         "  gamma\n"
                         "output_truncated: false\n"
                         "output_path: null\n"
                         "wait_matched: false\n"
                         "elapsed_seconds: 0.01\n"
                         "original_path: null")));
    };

    "session_block_elapsed_formatting"_test = [] {
        auto render = [](double v) {
            session_output_block b;
            b.task_id = "t";
            b.status = "completed";
            b.elapsed_seconds = v;
            auto got = build_session_output_block(b);
            auto pos = got.find("elapsed_seconds: ");
            return got.substr(pos, got.find('\n', pos) - pos);
        };
        expect(eq(render(1.234), s("elapsed_seconds: 1.23")));
        expect(eq(render(1.5), s("elapsed_seconds: 1.50")));
        expect(eq(render(0.005), s("elapsed_seconds: 0.01")));
        expect(eq(render(123456.789), s("elapsed_seconds: 123456.79")));
        expect(eq(render(60.0), s("elapsed_seconds: 60.00")));
        expect(eq(render(99.999), s("elapsed_seconds: 100.00")));
        expect(eq(render(2.675), s("elapsed_seconds: 2.67")));
        expect(eq(render(0.0), s("elapsed_seconds: 0.00")));
    };

    // ------------------------------------------------------------------ 6
    "extract_export_path_markers"_test = [] {
        // Reference markers (kimi-agent common.py _extract_export_path, in
        // order): "exported to file `" / "added to file `" (WITH a backtick)
        // then the ": " variants the export pipeline actually emits
        // ("[Output too large, exported to file: X]").
        expect(eq(*extract_export_path(
                      "[Output too large, exported to file: C:/t/0.txt]"),
                  s("C:/t/0.txt")));
        expect(eq(*extract_export_path(
                      "[Output too large, added to file: C:/t/1.txt]"),
                  s("C:/t/1.txt")));
        expect(eq(*extract_export_path("exported to file `C:/t/2.txt`"),
                  s("C:/t/2.txt")));
        expect(eq(*extract_export_path("added to file `C:/t/3.txt`"),
                  s("C:/t/3.txt")));
        // The backtick is part of the marker: without it the marker does not
        // match at all (the reference returns None).
        expect(!extract_export_path(
                    "[Output too large, exported to file C:/t/4.txt]")
                    .has_value());
        // rstrip("]`") strips ALL trailing ']' and '`' characters
        expect(eq(*extract_export_path("exported to file `a`]]`"), s("a")));
        // a marker with an empty tail yields an empty string, not nullopt
        expect(eq(*extract_export_path("exported to file: "), s("")));
        expect(!extract_export_path("plain output").has_value());
        expect(!extract_export_path("").has_value());
    };

    // ------------------------------------------------------------------ 7
    "wait_pattern_literal"_test = [] {
        bool m = false;
        auto r = match_wait_pattern("ready", "not ready yet", m);
        expect(!r.failed());
        expect(m);
        r = match_wait_pattern("ready", "nope", m);
        expect(!r.failed());
        expect(!m);
        // literal with regex-adjacent-but-literal chars
        r = match_wait_pattern("hello world", "say hello world now", m);
        expect(!r.failed());
        expect(m);
    };

    "wait_pattern_glob_metachars_are_not_native"_test = [] {
        // The reference compiles wait_for_pattern with the *regex* engine and
        // calls pattern.search(buffer), so "ready*" matches "read done" (the
        // '*' quantifies the preceding 'y'). A fnmatch-style engine matches the
        // literal "ready" followed by anything and answers false, i.e. it
        // silently disagreed with the reference; the kernel must therefore
        // refuse every glob metacharacter and let the Python engine decide.
        // (python_goldens.inc row `regex_star_quantifier` pins the reference
        // result: matched == true for buffer "read done".)
        bool m = true;
        auto r = match_wait_pattern("ready*", "read done", m);
        expect(r.status == tool_status::unsupported);
        expect(!m);
        expect(classify_wait_pattern("ready*") == wait_pattern_kind::unsupported);
        expect(classify_wait_pattern("*done*") == wait_pattern_kind::unsupported);
        expect(classify_wait_pattern("a?c") == wait_pattern_kind::unsupported);
        expect(classify_wait_pattern("a[!b]c") == wait_pattern_kind::unsupported);
        expect(classify_wait_pattern("err: [0-9]") ==
               wait_pattern_kind::unsupported);
        // ']' is a literal in BOTH regex and fnmatch, so it stays native.
        expect(classify_wait_pattern("a]b") == wait_pattern_kind::literal);
        bool l = false;
        expect(!match_wait_pattern("a]b", "x a]b y", l).failed());
        expect(l);
    };

    "wait_pattern_unsupported"_test = [] {
        bool m = false;
        // regex-only metacharacters route to the Python engine
        for (auto p : {"a.c", "a+b", "^start", "line$", "x|y", "(grp)",
                       "a{2}b", "esc\\d"}) {
            auto r = match_wait_pattern(p, "whatever", m);
            expect(r.status == tool_status::unsupported)
                << "pattern " << p << " must be unsupported";
        }
        // non-ASCII routes to Python too
        auto r2 = match_wait_pattern("caf\xC3\xA9", "un caf\xC3\xA9 ici", m);
        expect(r2.status == tool_status::unsupported);
        // classify agrees
        expect(classify_wait_pattern("plain text") == wait_pattern_kind::literal);
        expect(classify_wait_pattern("ready*") == wait_pattern_kind::unsupported);
        expect(classify_wait_pattern("a.b") == wait_pattern_kind::unsupported);
    };

    "wait_pattern_empty"_test = [] {
        bool m = false;
        auto r = match_wait_pattern("", "buffer", m);
        expect(r.status == tool_status::invalid_input);
    };

    // -----------------------------------------------------------------------
    // Golden replay: every row comes from scripts/gen_python_goldens.py, which
    // executes the kimi-agent Python reference (never a kimix_native mirror).
    // -----------------------------------------------------------------------

    "python_goldens_script_paths"_test = [] {
        for (const auto &g : k_python_path_goldens) {
            expect(eq(plan_script_path(g.base_dir, g.index, g.ext), s(g.expected)))
                << g.name;
        }
    };

    "python_goldens_resolve_python_exe"_test = [] {
        for (const auto &g : k_python_resolve_goldens) {
            kimix::vector<kimix::string> search_bases;
            search_bases.emplace_back(g.session_dir);
            search_bases.emplace_back(g.cwd);
            auto r = resolve_python_exe(g.override_exe, search_bases, g.virtual_env,
                                        g.fallback, exists_from_list(g.existing));
            if (g.expected[0] == '\0') {
                expect(!r.has_value()) << g.name;
            } else {
                expect(r.has_value()) << g.name;
                if (r.has_value()) {
                    expect(eq(*r, s(g.expected))) << g.name;
                }
            }
        }
    };

    "python_goldens_scrub_child_env"_test = [] {
        for (const auto &g : k_python_scrub_goldens) {
            auto out = scrub_child_env(named_values_from_lines(g.env));
            expect(eq(join_names(out), s(g.expected))) << g.name;
        }
    };

    "python_goldens_prepare_python_env"_test = [] {
        for (const auto &g : k_python_env_goldens) {
            kimix::string probed;
            auto probe = [&g, &probed](kimix::string_view path) {
                probed.assign(path.data(), path.size());
                return g.pyvenv_cfg_exists;
            };
            auto d = prepare_python_env(g.python_exe, g.share_bin_dir,
                                        g.current_path, path_sep, probe);
            if (!probed.empty()) {
                expect(ends_with(probed, "pyvenv.cfg")) << g.name;
            }
            if (g.expect_fast_path) {
                expect(!d.has_value()) << g.name;
                continue;
            }
            expect(d.has_value()) << g.name;
            if (!d.has_value()) {
                continue;
            }
            kimix::string got_path;
            kimix::string got_venv(g.parent_virtual_env);
            size_t path_changes = 0;
            for (const auto &c : *d) {
                expect(c.name == "PATH" || c.name == "VIRTUAL_ENV") << g.name;
                if (c.name == "PATH") {
                    got_path = c.value;
                    ++path_changes;
                } else {
                    got_venv = c.value;
                }
            }
            expect(eq(path_changes, size_t(1))) << g.name;
            expect(eq(got_path, s(g.expected_path))) << g.name;
            if (g.expect_virtual_env_change) {
                expect(eq(got_venv, s(g.expected_virtual_env))) << g.name;
            } else {
                // the delta must not touch VIRTUAL_ENV
                expect(eq(got_venv, s(g.parent_virtual_env))) << g.name;
            }
        }
    };

    "python_goldens_module_not_found_hint"_test = [] {
        for (const auto &g : k_python_hint_goldens) {
            expect(eq(module_not_found_hint(g.output, g.python_exe), s(g.expected)))
                << g.name;
        }
    };

    "python_goldens_session_output_block"_test = [] {
        for (const auto &g : k_python_block_goldens) {
            session_output_block b;
            b.task_id = g.task_id;
            b.status = g.status;
            b.output = g.output;
            if (g.has_exit_code) {
                b.exit_code = static_cast<int32_t>(g.exit_code);
            }
            if (g.has_exit_code_meaning) {
                b.exit_code_meaning = s(g.exit_code_meaning);
            }
            if (g.has_failure_hint) {
                b.failure_hint = s(g.failure_hint);
            }
            if (g.has_wait_matched) {
                b.wait_matched = g.wait_matched;
            }
            if (g.has_elapsed_seconds) {
                b.elapsed_seconds = g.elapsed_seconds;
            }
            if (g.has_output_path) {
                b.output_path = s(g.output_path);
            }
            b.output_truncated = g.output_truncated;
            if (g.has_original_path) {
                b.original_path = s(g.original_path);
            }
            expect(eq(build_session_output_block(b), s(g.expected))) << g.name;
        }
    };

    "python_goldens_extract_export_path"_test = [] {
        for (const auto &g : k_python_export_goldens) {
            auto r = extract_export_path(g.output);
            expect(eq(r.has_value(), g.expected_present)) << g.name;
            if (r.has_value()) {
                expect(eq(*r, s(g.expected))) << g.name;
            }
        }
    };

    "python_goldens_wait_pattern"_test = [] {
        bool m = false;
        for (const auto &g : k_python_pattern_goldens) {
            // An invalid reference regex can only appear among the patterns
            // that need the Python engine.
            if (g.reference_invalid_regex) {
                expect(g.needs_regex_engine) << g.name;
            }
            auto r = match_wait_pattern(g.pattern, g.buffer, m);
            if (g.pattern[0] == '\0') {
                // Documented deviation: the reference compiles "" (which
                // matches everything); the kernel refuses the degenerate input
                // rather than guessing between "no pattern" and "empty regex".
                expect(r.status == tool_status::invalid_input) << g.name;
                continue;
            }
            if (g.needs_regex_engine) {
                expect(r.status == tool_status::unsupported) << g.name;
                expect(!m) << g.name;
                expect(classify_wait_pattern(g.pattern) ==
                       wait_pattern_kind::unsupported)
                    << g.name;
            } else {
                expect(!r.failed()) << g.name;
                expect(eq(m, g.reference_matched)) << g.name;
                expect(classify_wait_pattern(g.pattern) ==
                       wait_pattern_kind::literal)
                    << g.name;
            }
        }
    };

    // -----------------------------------------------------------------------
    // Tool class contract (kimi-agent py/__init__.py __call__ /
    // _execute_code).  The tests below need a real interpreter on PATH plus a
    // native_io session; the pure kernels are covered above.
    // -----------------------------------------------------------------------

    "python_tool_class_missing_code_message"_test = [] {
        // Reference Params._validate_source: "`code` must be provided (unless
        // mode='interactive' or task_id is set)."
        Session session;
        session.native_io = true;
        Python tool(&session);
        ToolParams params;
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find(
                   "`code` must be provided (unless mode='interactive' or "
                   "task_id is set).") != std::string::npos)
            << sv_of(json);
        expect(sv_of(json).find("\"invalid_input\"") != std::string::npos);
    };

    "python_tool_class_task_id_without_code_message"_test = [] {
        // Reference: "code cannot be empty when continuing a session via
        // task_id" (the second half of Params._validate_source).
        Session session;
        session.native_io = true;
        Python tool(&session);
        ToolParams params;
        params.values["task_id"] = ValueElement::make_string(s("t_1"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find(
                   "code cannot be empty when continuing a session via "
                   "task_id") != std::string::npos)
            << sv_of(json);
    };

    "python_tool_class_file_alias_binds_to_code"_test = [] {
        // The reference Params aliases `code` (code | source_code | file), so
        // `file=` must behave exactly like `code=`: an empty `code` with only
        // `file` set is still "code provided".
        Session session;
        session.native_io = true;
        Python tool(&session);
        ToolParams params;
        params.values["file"] = ValueElement::make_string(s("print(1)"));
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("must be provided") == std::string::npos)
            << sv_of(json);
    };

#ifdef KIMIX_PLATFORM_WINDOWS
    "python_tool_class_runs_existing_py_file"_test = [] {
        // _resolve_script_source priority 1: `code` naming an existing ".py"
        // file is executed as a *file* (source_label "File").  Before the fix
        // the class always wrote the text to a temp script, so a path like
        // C:\...\hello.py was compiled as Python source and failed.
        std::error_code ec;
        const kimix::filesystem::path dir =
            kimix::filesystem::temp_directory_path(ec) / "kimix_py_tool_file_mode";
        kimix::filesystem::remove_all(dir, ec);
        kimix::filesystem::create_directories(dir, ec);
        const kimix::filesystem::path script = dir / "hello.py";
        {
            std::FILE *f = std::fopen(script.string().c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                const char *body = "print('marker_from_file_mode')\n";
                std::fwrite(body, 1, std::strlen(body), f);
                std::fclose(f);
            }
        }
        Session session;
        session.native_io = true;
        session.work_dir = kimix::to_string(dir);
        Python tool(&session);
        ToolParams params;
        params.values["code"] = ValueElement::make_string(kimix::to_string(script));
        params.values["timeout"] = ValueElement::make_int(60);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("marker_from_file_mode") != std::string::npos)
            << sv_of(json);
        expect(sv_of(json).find("File: `") != std::string::npos) << sv_of(json);
        expect(sv_of(json).find("status: completed") != std::string::npos)
            << sv_of(json);
        kimix::filesystem::remove_all(dir, ec);
    };

    "python_tool_class_module_not_found_hint_in_message"_test = [] {
        // The reference appends _module_not_found_hint to the failure message.
        std::error_code ec;
        const kimix::filesystem::path dir =
            kimix::filesystem::temp_directory_path(ec) / "kimix_py_tool_hint";
        kimix::filesystem::remove_all(dir, ec);
        kimix::filesystem::create_directories(dir, ec);
        Session session;
        session.native_io = true;
        session.work_dir = kimix::to_string(dir);
        Python tool(&session);
        ToolParams params;
        params.values["code"] =
            ValueElement::make_string(s("import definitely_missing_pkg_xyz"));
        params.values["timeout"] = ValueElement::make_int(60);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("interpreter:") != std::string::npos) << sv_of(json);
        expect(sv_of(json).find("-m pip install definitely_missing_pkg_xyz") !=
               std::string::npos)
            << sv_of(json);
        expect(sv_of(json).find("status: failed") != std::string::npos)
            << sv_of(json);
        // The child's stderr is merged into the capture and its exit code is
        // propagated into the block (ModuleNotFoundError -> exit status 1).
        expect(sv_of(json).find("ModuleNotFoundError: No module named") !=
               std::string::npos)
            << sv_of(json);
        expect(sv_of(json).find("exit_code: 1") != std::string::npos)
            << sv_of(json);
        kimix::filesystem::remove_all(dir, ec);
    };

    "python_tool_class_timeout_kills_child"_test = [] {
        // timeout_ms bounds the run: the runner terminates the child and the
        // block reports status "timeout" with no exit code.
        std::error_code ec;
        const kimix::filesystem::path dir =
            kimix::filesystem::temp_directory_path(ec) / "kimix_py_tool_timeout";
        kimix::filesystem::remove_all(dir, ec);
        kimix::filesystem::create_directories(dir, ec);
        Session session;
        session.native_io = true;
        session.work_dir = kimix::to_string(dir);
        Python tool(&session);
        ToolParams params;
        params.values["code"] =
            ValueElement::make_string(s("import time\ntime.sleep(30)\n"));
        params.values["timeout"] = ValueElement::make_int(1);
        tool(&params);
        const kimix::string json(tool.serialized_result().data(),
                                 tool.serialized_result().size());
        expect(sv_of(json).find("status: timeout") != std::string::npos)
            << sv_of(json);
        expect(sv_of(json).find("use `job_output`") != std::string::npos)
            << sv_of(json);
        kimix::filesystem::remove_all(dir, ec);
    };
#endif

    "python_tool_class_detect_python_exe_override"_test = [] {
        // KIMIX_PYTHON_EXECUTABLE is the reference's override
        // (_resolve_python_uncached step 1); a non-existent value is ignored.
        std::error_code ec;
        const kimix::filesystem::path dir =
            kimix::filesystem::temp_directory_path(ec) / "kimix_py_tool_exe";
        kimix::filesystem::remove_all(dir, ec);
        kimix::filesystem::create_directories(dir, ec);
        const kimix::filesystem::path fake = dir / "fake_python";
        {
            std::FILE *f = std::fopen(fake.string().c_str(), "wb");
            if (f != nullptr) {
                std::fclose(f);
            }
        }
        const kimix::string fake_s = kimix::to_string(fake);
        Session session;
        session.native_io = true;
        Python tool(&session);
#ifdef KIMIX_PLATFORM_WINDOWS
        ::_putenv_s("KIMIX_PYTHON_EXECUTABLE", fake_s.c_str());
#else
        ::setenv("KIMIX_PYTHON_EXECUTABLE", fake_s.c_str(), 1);
#endif
        expect(eq(tool.detect_python_exe(kimix::string_view()), fake_s));
#ifdef KIMIX_PLATFORM_WINDOWS
        ::_putenv_s("KIMIX_PYTHON_EXECUTABLE", "");
#else
        ::unsetenv("KIMIX_PYTHON_EXECUTABLE");
#endif
        kimix::filesystem::remove_all(dir, ec);
    };

    "python_tool_class_detect_python_exe_venv_walk"_test = [] {
        // _resolve_python_uncached step 2: walk up from the session dir (here
        // the session work dir) probing <base>/.venv/Scripts/python.exe.
#ifdef KIMIX_PLATFORM_WINDOWS
        std::error_code ec;
        const kimix::filesystem::path root =
            kimix::filesystem::temp_directory_path(ec) / "kimix_py_tool_venv";
        kimix::filesystem::remove_all(root, ec);
        const kimix::filesystem::path scripts = root / ".venv" / "Scripts";
        kimix::filesystem::create_directories(scripts, ec);
        const kimix::filesystem::path venv_py = scripts / "python.exe";
        {
            std::FILE *f = std::fopen(venv_py.string().c_str(), "wb");
            if (f != nullptr) {
                std::fclose(f);
            }
        }
        Session session;
        session.native_io = true;
        Python tool(&session);
        ::_putenv_s("KIMIX_PYTHON_EXECUTABLE", "");
        ::_putenv_s("PYTHON_EXE", "");
        ::_putenv_s("VIRTUAL_ENV", "");
        expect(eq(tool.detect_python_exe(kimix::string_view(kimix::to_string(root))),
                  kimix::to_string(venv_py)));
        kimix::filesystem::remove_all(root, ec);
#endif
    };

    return 0;
}
