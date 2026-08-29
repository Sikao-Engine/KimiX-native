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
//
// The kernels build paths with the host separator (mirroring pathlib on the
// host OS), so every path-like literal in this file goes through `host()`
// which converts POSIX-style literals to the host form.
#include "ut/ut.hpp"

#include "builtin_tools/python_tool.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools::python;

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
        // Plan marker order (no backticks in the marker itself):
        // 1. "exported to file "  2. "added to file "
        // 3. "exported to file: " 4. "added to file: "
        expect(eq(*extract_export_path(
                      "Output too large, exported to file C:/t/0.txt]"),
                  s("C:/t/0.txt")));
        expect(eq(*extract_export_path(
                      "Output too large, added to file C:/t/1.txt]"),
                  s("C:/t/1.txt")));
        expect(eq(*extract_export_path(
                      "[Output too large, exported to file: C:/t/2.txt]"),
                  s("C:/t/2.txt")));
        expect(eq(*extract_export_path(
                      "[Output too large, added to file: C:/t/3.txt]"),
                  s("C:/t/3.txt")));
        // rstrip("]`") strips ALL trailing ']' and '`' characters
        expect(eq(*extract_export_path("x exported to file a]]"), s("a")));
        // a marker with an empty tail yields an empty string, not nullopt
        expect(eq(*extract_export_path("exported to file: "), s("")));
        expect(!extract_export_path("plain output").has_value());
        expect(!extract_export_path("").has_value());
        // Backticks are *not* part of the marker; they remain in the tail.
        // Leading backtick stays, trailing backtick is stripped.
        expect(eq(*extract_export_path("exported to file `C:/t/4.txt`"),
                  s("`C:/t/4.txt")));
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

    "wait_pattern_glob"_test = [] {
        bool m = false;
        // '*' spans any chars (search semantics: wrapped in *)
        auto r = match_wait_pattern("ready*", "all ready to go", m);
        expect(!r.failed());
        expect(m);
        r = match_wait_pattern("*done*", "step done here", m);
        expect(!r.failed());
        expect(m);
        r = match_wait_pattern("err: [0-9]", "err: 7", m);
        expect(!r.failed());
        expect(m);
        r = match_wait_pattern("a?c", "abc", m);
        expect(!r.failed());
        expect(m);
        // negative case: class exclusion
        r = match_wait_pattern("a[!b]c", "abc", m);
        expect(!r.failed());
        expect(!m);
        r = match_wait_pattern("a[!b]c", "axc", m);
        expect(!r.failed());
        expect(m);
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
        expect(classify_wait_pattern("ready*") == wait_pattern_kind::glob);
        expect(classify_wait_pattern("a.b") == wait_pattern_kind::unsupported);
    };

    "wait_pattern_empty"_test = [] {
        bool m = false;
        auto r = match_wait_pattern("", "buffer", m);
        expect(r.status == tool_status::invalid_input);
    };

    return 0;
}
