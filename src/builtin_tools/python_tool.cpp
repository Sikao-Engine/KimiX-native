// python_tool.cpp - implementations for python_tool.h.
//
// See the header for the source-of-truth mapping.  All kernels are pure CPU
// work: the filesystem is only reached through the injected `exists` /
// `is_file` probes (the reference's Path.is_file() calls), so every kernel is
// deterministically unit-testable without fixtures.

#include "builtin_tools/python_tool.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

#include <core/stl/format.h>
#include <core/string_scratch.h>

namespace kimix::builtin_tools::python {

namespace {

constexpr char k_host_sep =
#ifdef KIMIX_PLATFORM_WINDOWS
    '\\';
#else
    '/';
#endif

// Join a directory and a single path component the way `Path / child` does
// for a simple child (no leading separator in `child`).
kimix::string join_path(kimix::string_view dir, kimix::string_view child) {
    kimix::string out;
    out.reserve(dir.size() + 1 + child.size());
    out.append(dir.data(), dir.size());
    if (!out.empty() && out.back() != k_host_sep && out.back() != '/') {
        out.push_back(k_host_sep);
    }
    out.append(child.data(), child.size());
    return out;
}

// Python str.upper() for ASCII names (the scrub mirror gates on isascii()).
char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool starts_with(kimix::string_view s, kimix::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// security.py _SAFE_ENV_PREFIXES (40-44).
constexpr kimix::string_view k_safe_env_prefixes[] = {
    "PATH",       "HOME",       "USER",      "LANG",    "LC_",
    "TERM",       "TMP",        "TEMP",      "SHELL",   "LOGNAME",
    "XDG_",       "PYTHON",     "VIRTUAL_ENV", "CONDA", "KIMIX_",
    "PROCESSOR_", "PROGRAMFILES", "APPDATA",  "LOCALAPPDATA",
    "HOMEDRIVE",  "HOMEPATH",   "SYSTEM",    "WINDIR",  "COMSPEC",
    "PATHEXT",    "NUMBER_OF_PROCESSORS", "OS", "COMPUTERNAME",
    "USERPROFILE", "TZ",        "PWD",       "SHLVL",   "SSH_",
    "GIT_",       "UV_",        "PIP_"};

// security.py _SECRET_SUBSTRINGS (37-38).
constexpr kimix::string_view k_secret_substrings[] = {
    "KEY",    "TOKEN",   "SECRET",   "PASSWORD",  "PASSWD", "CREDENTIAL",
    "AUTH",   "DSN",     "WEBHOOK",  "CREDS",     "BEARER", "APIKEY"};

} // namespace

// ---------------------------------------------------------------------------
// 1. Script path planning
// ---------------------------------------------------------------------------

ScriptFileWriter::ScriptFileWriter(kimix::string_view base_dir, uint64_t start_index)
    : _base_dir(base_dir.data(), base_dir.size()), _next_index(start_index) {}

kimix::string ScriptFileWriter::plan_path(kimix::string_view ext) {
    uint64_t index = 0;
    {
        std::lock_guard<kimix::spin_mutex> lock(_mutex);
        index = _next_index;
        _next_index += 1;
    }
    return plan_script_path(_base_dir, index, ext);
}

uint64_t ScriptFileWriter::next_index() const {
    std::lock_guard<kimix::spin_mutex> lock(_mutex);
    return _next_index;
}

kimix::string plan_script_path(kimix::string_view base_dir, uint64_t index,
                               kimix::string_view ext) {
    // <base_dir>/<index><ext> — mirrors `_temp_folder / (str(id) + ext)`.
    kimix::string name;
    name.reserve(24 + ext.size());
    {
        char buf[24];
        auto res = std::to_chars(buf, buf + sizeof(buf), index);
        name.append(buf, static_cast<size_t>(res.ptr - buf));
    }
    name.append(ext.data(), ext.size());
    return join_path(base_dir, name);
}

// ---------------------------------------------------------------------------
// 2. Interpreter resolution
// ---------------------------------------------------------------------------

kimix::optional<kimix::string>
resolve_python_exe(kimix::string_view override,
                   kimix::span<const kimix::string> search_bases,
                   kimix::string_view virtual_env,
                   kimix::string_view fallback,
                   const kimix::function<bool(kimix::string_view)> &exists) {
    // 1. explicit override (KIMIX_PYTHON_EXECUTABLE)
    if (!override.empty() && exists(override)) {
        return kimix::string(override.data(), override.size());
    }
    // 2. project .venv next to each base, walking up.  The reference probes
    //    <parent>/.venv/Scripts/python.exe (Windows) then
    //    <parent>/.venv/bin/python (POSIX) for the base itself and every
    //    ancestor (Path.parents stops at the filesystem root).  Candidate
    //    paths are built with the host separator, exactly like pathlib does.
    const kimix::string k_venv_win =
        kimix::string(".venv") + k_host_sep + "Scripts" + k_host_sep + "python.exe";
    const kimix::string k_venv_posix =
        kimix::string(".venv") + k_host_sep + "bin" + k_host_sep + "python";
    const kimix::string_view k_venv_candidates[] = {k_venv_win, k_venv_posix};
    for (const auto &base : search_bases) {
        // Normalise a trailing separator away so the strip loop sees the last
        // component (pathlib treats "C:\dev\" and "C:\dev" identically).
        kimix::string dir(base.data(), base.size());
        while (dir.size() > 1 && (dir.back() == k_host_sep || dir.back() == '/')) {
            dir.pop_back();
        }
        for (;;) {
            for (auto cand : k_venv_candidates) {
                kimix::string full = join_path(dir, cand);
                if (exists(full)) {
                    return full;
                }
            }
            // Move to the parent: cut the last component.
            size_t cut = dir.size();
            while (cut > 0 && dir[cut - 1] != k_host_sep && dir[cut - 1] != '/') {
                --cut;
            }
            if (cut == 0) {
                break; // no separator found -> no more parents
            }
            // Drop the separator(s) at the cut point, keeping a bare root
            // ("/" or "C:\") intact so join_path still works.
            size_t keep = cut;
            while (keep > 1 && (dir[keep - 1] == k_host_sep || dir[keep - 1] == '/')) {
                --keep;
            }
            if (keep == dir.size()) {
                break; // defensive: no progress
            }
            dir.resize(keep);
            if (dir.empty()) {
                break;
            }
        }
    }
    // 3. VIRTUAL_ENV: probe <venv>/Scripts/python.exe then <venv>/bin/python
    //    directly (no ".venv" prefix — the variable already points at the
    //    virtualenv root).
    const kimix::string k_venv_direct_win =
        kimix::string("Scripts") + k_host_sep + "python.exe";
    const kimix::string k_venv_direct_posix =
        kimix::string("bin") + k_host_sep + "python";
    const kimix::string_view k_venv_direct_candidates[] = {k_venv_direct_win,
                                                           k_venv_direct_posix};
    if (!virtual_env.empty()) {
        for (auto cand : k_venv_direct_candidates) {
            kimix::string full = join_path(virtual_env, cand);
            if (exists(full)) {
                return full;
            }
        }
    }
    // 4. fallback (sys.executable)
    if (!fallback.empty()) {
        return kimix::string(fallback.data(), fallback.size());
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 3. Child environment assembly
// ---------------------------------------------------------------------------

kimix::vector<named_value> scrub_child_env(kimix::span<const named_value> env) {
    kimix::vector<named_value> scrubbed;
    scrubbed.reserve(env.size());
    for (const auto &entry : env) {
        kimix::string upper;
        upper.reserve(entry.name.size());
        for (char c : entry.name) {
            upper.push_back(ascii_upper(c));
        }
        bool safe = false;
        for (auto prefix : k_safe_env_prefixes) {
            if (starts_with(upper, prefix)) {
                safe = true;
                break;
            }
        }
        if (safe) {
            scrubbed.push_back(entry);
            continue;
        }
        bool secret = false;
        for (auto sub : k_secret_substrings) {
            if (upper.find(sub) != kimix::string::npos) {
                secret = true;
                break;
            }
        }
        if (!secret) {
            scrubbed.push_back(entry);
        }
    }
    return scrubbed;
}

kimix::optional<kimix::vector<env_change>>
prepare_python_env(kimix::string_view python_exe,
                   kimix::string_view share_bin_dir,
                   kimix::string_view current_path,
                   kimix::string_view path_sep,
                   const kimix::function<bool(kimix::string_view)> &is_file) {
    // already_first — the reference checks the raw parent PATH:
    //   current_path.startswith(bin_dir + path_sep) or current_path == bin_dir
    const bool already_first =
        current_path == share_bin_dir ||
        (current_path.size() > share_bin_dir.size() &&
         starts_with(current_path, share_bin_dir) &&
         current_path.substr(share_bin_dir.size(), path_sep.size()) == path_sep);

    // venv detection: exe.parent.name in ("Scripts", "bin") and
    // (exe.parent.parent / "pyvenv.cfg").is_file()
    kimix::string exe(python_exe.data(), python_exe.size());
    size_t sep_pos = exe.size();
    while (sep_pos > 0 && exe[sep_pos - 1] != k_host_sep && exe[sep_pos - 1] != '/') {
        --sep_pos;
    }
    kimix::string parent_dir;
    if (sep_pos > 0) {
        parent_dir.assign(exe.data(), sep_pos - 1); // drop the separator too
    }
    // parent name (last component of parent_dir)
    kimix::string_view parent_name(parent_dir);
    if (!parent_name.empty()) {
        size_t last = parent_name.size();
        while (last > 0 && parent_name[last - 1] != k_host_sep &&
               parent_name[last - 1] != '/') {
            --last;
        }
        parent_name.remove_prefix(last);
    }
    // grandparent directory (parent of parent_dir)
    kimix::string grandparent_dir;
    {
        size_t p = parent_dir.size();
        while (p > 0 && parent_dir[p - 1] != k_host_sep && parent_dir[p - 1] != '/') {
            --p;
        }
        if (p > 0) {
            grandparent_dir.assign(parent_dir.data(), p - 1);
        }
    }
    const bool is_venv =
        (parent_name == "Scripts" || parent_name == "bin") &&
        is_file(join_path(grandparent_dir, "pyvenv.cfg"));

    // PATH assembly helper: prepend entries then the current PATH split on
    // path_sep with empty entries and bin_dir itself removed.
    auto build_path = [&](kimix::span<const kimix::string_view> prepend) {
        kimix::string out;
        bool first = true;
        auto append_entry = [&](kimix::string_view e) {
            if (!first) {
                out.append(path_sep.data(), path_sep.size());
            }
            out.append(e.data(), e.size());
            first = false;
        };
        for (auto e : prepend) {
            append_entry(e);
        }
        size_t pos = 0;
        while (pos <= current_path.size()) {
            size_t next = current_path.find(path_sep, pos);
            kimix::string_view entry =
                next == kimix::string_view::npos
                    ? current_path.substr(pos)
                    : current_path.substr(pos, next - pos);
            if (!entry.empty() && entry != share_bin_dir) {
                append_entry(entry);
            }
            if (next == kimix::string_view::npos) {
                break;
            }
            pos = next + path_sep.size();
        }
        return out;
    };

    if (!is_venv) {
        if (already_first) {
            return std::nullopt; // zero-copy fast path (_build_env -> None)
        }
        kimix::vector<env_change> delta;
        kimix::string_view prepend[] = {share_bin_dir};
        delta.push_back(env_change{kimix::string("PATH"), build_path(prepend)});
        return delta;
    }

    // venv case: VIRTUAL_ENV first, then PATH = [bin_dir, venv_bin] + entries
    kimix::vector<env_change> delta;
    delta.push_back(env_change{kimix::string("VIRTUAL_ENV"), grandparent_dir});
    kimix::string_view prepend[] = {share_bin_dir, kimix::string_view(parent_dir)};
    delta.push_back(env_change{kimix::string("PATH"), build_path(prepend)});
    return delta;
}

// ---------------------------------------------------------------------------
// 4. Module-not-found hint
// ---------------------------------------------------------------------------

kimix::string module_not_found_hint(kimix::string_view output,
                                    kimix::string_view python_exe) {
    // re.search(r"ModuleNotFoundError: No module named ['"]([^'"]+)['"]", out)
    static constexpr kimix::string_view k_marker =
        "ModuleNotFoundError: No module named ";
    size_t pos = output.find(k_marker);
    while (pos != kimix::string_view::npos) {
        size_t q = pos + k_marker.size();
        if (q < output.size() && (output[q] == '\'' || output[q] == '"')) {
            size_t end = q + 1;
            while (end < output.size() && output[end] != '\'' &&
                   output[end] != '"') {
                ++end;
            }
            if (end < output.size()) {
                kimix::string_view module_name = output.substr(q + 1, end - q - 1);
                // The Python regex requires [^'"]+ (non-empty module name).
                if (!module_name.empty()) {
                    kimix::StringScratch s;
                    s << " Hint: the script ran with interpreter '" << python_exe
                      << "'. If you installed the package with plain 'pip "
                         "install', it may have gone to a different "
                         "environment. Retry with '"
                      << python_exe << "' -m pip install " << module_name << ".";
                    return kimix::string(s.string());
                }
            }
        }
        pos = output.find(k_marker, pos + 1);
    }
    return {};
}

// ---------------------------------------------------------------------------
// 5. Session output block
// ---------------------------------------------------------------------------

namespace {

// Render an optional string field with Python truthiness: nullopt OR empty
// string -> "null", else the value verbatim.
kimix::string_view field_or_null(const kimix::optional<kimix::string> &v) {
    if (v.has_value() && !v->empty()) {
        return *v;
    }
    return "null";
}

// Format a double exactly like Python's f"{v:.2f}".  CPython delegates to
// correctly-rounded decimal formatting; std::format("{:.2f}") (C++20,
// P0677R1 no-precision-loss rule) produces the same result on every
// conforming implementation, so we delegate directly.
kimix::string format_elapsed(double v) {
    return kimix::format("{:.2f}", v);
}

// Python str.splitlines() restricted to the ASCII separators (\n, \r\n, \r,
// \x0b, \x0c).  Non-ASCII separators (\x1c-\x1e, \x85, U+2028/2029) are left
// inside the line — callers gate on ASCII when that matters.
kimix::vector<kimix::string_view> splitlines_ascii(kimix::string_view text) {
    kimix::vector<kimix::string_view> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = pos;
        while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r' &&
               text[pos] != '\x0b' && text[pos] != '\x0c') {
            ++pos;
        }
        lines.push_back(text.substr(start, pos - start));
        if (pos < text.size()) {
            if (text[pos] == '\r' && pos + 1 < text.size() &&
                text[pos + 1] == '\n') {
                pos += 2;
            } else {
                pos += 1;
            }
        }
    }
    return lines;
}

// textwrap.indent indents only lines that contain at least one non-whitespace
// character.  Python's predicate: any(c not in string.whitespace for c in line)
// where whitespace = " \t\n\r\x0b\x0c".
bool has_non_whitespace(kimix::string_view line) {
    for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\x0b' &&
            c != '\x0c') {
            return true;
        }
    }
    return false;
}

} // namespace

kimix::string build_session_output_block(const session_output_block &block) {
    kimix::StringScratch s;
    s << "task_id: " << block.task_id << "\n";
    s << "status: " << block.status << "\n";
    s << "exit_code: ";
    if (block.exit_code.has_value()) {
        s << *block.exit_code;
    } else {
        s << "null";
    }
    s << "\n";
    s << "exit_code_meaning: " << field_or_null(block.exit_code_meaning) << "\n";
    s << "failure_hint: " << field_or_null(block.failure_hint) << "\n";
    s << "output: |\n";
    if (!block.output.empty()) {
        // textwrap.indent(output.rstrip("\n"), "  ").splitlines()
        kimix::string_view body = block.output;
        while (!body.empty() && body.back() == '\n') {
            body.remove_suffix(1);
        }
        for (auto line : splitlines_ascii(body)) {
            if (has_non_whitespace(line)) {
                s << "  ";
            }
            s << line << "\n";
        }
    } else {
        s << "  (no output)\n";
    }
    s << "output_truncated: " << (block.output_truncated ? "true" : "false")
      << "\n";
    s << "output_path: " << field_or_null(block.output_path) << "\n";
    s << "wait_matched: ";
    if (block.wait_matched.has_value()) {
        s << (*block.wait_matched ? "true" : "false");
    } else {
        s << "null";
    }
    s << "\n";
    s << "elapsed_seconds: ";
    if (block.elapsed_seconds.has_value()) {
        s << format_elapsed(*block.elapsed_seconds);
    } else {
        s << "null";
    }
    s << "\n";
    s << "original_path: " << field_or_null(block.original_path);
    return kimix::string(s.string());
}

// ---------------------------------------------------------------------------
// 6. Export-path extraction
// ---------------------------------------------------------------------------

kimix::optional<kimix::string> extract_export_path(kimix::string_view output) {
    if (output.empty()) {
        return std::nullopt;
    }
    static constexpr kimix::string_view k_markers[] = {
        "exported to file `",
        "added to file `",
        "exported to file: ",
        "added to file: ",
    };
    for (auto marker : k_markers) {
        size_t pos = output.find(marker);
        if (pos != kimix::string_view::npos) {
            // output.split(marker, 1)[-1] -> everything after the first marker
            kimix::string_view tail = output.substr(pos + marker.size());
            // .rstrip("]`")
            while (!tail.empty() && (tail.back() == ']' || tail.back() == '`')) {
                tail.remove_suffix(1);
            }
            return kimix::string(tail.data(), tail.size());
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 7. wait_for_pattern matching
// ---------------------------------------------------------------------------

namespace {

// Glob metacharacters (fnmatch): *, ?, [ ... ].  Every other byte is literal.
constexpr bool is_glob_meta(char c) {
    return c == '*' || c == '?' || c == '[';
}

// Regex metacharacters that are NOT glob metacharacters.  Their presence
// means the pattern needs the full Python regex engine.  (`]` is a literal
// outside a character class in BOTH regex and fnmatch, so it is not listed
// here — a lone `]` keeps literal semantics on either side.)
constexpr bool is_regex_only_meta(char c) {
    switch (c) {
    case '.':
    case '^':
    case '$':
    case '+':
    case '{':
    case '}':
    case '\\':
    case '|':
    case '(':
    case ')':
        return true;
    default:
        return false;
    }
}

// fnmatch-style character class membership (the body between '[' and ']',
// i.e. without the brackets or a leading '!').  Supports ranges ("0-9",
// "a-z"); a '-' at the start or end of the class is a literal (fnmatch
// rule, e.g. "[a-]" matches 'a' or '-').  An inverted class ("[!seq]") is
// handled by the caller via `negate`.
bool class_matches(kimix::string_view cls, char c) {
    size_t i = 0;
    while (i < cls.size()) {
        if (i + 2 < cls.size() && cls[i + 1] == '-' && cls[i + 2] != ']') {
            char lo = cls[i];
            char hi = cls[i + 2];
            if (c >= lo && c <= hi) {
                return true;
            }
            i += 3;
        } else {
            if (cls[i] == c) {
                return true;
            }
            i += 1;
        }
    }
    return false;
}

// fnmatch-style whole-string match.  `pattern` uses fnmatch semantics:
//   *      matches zero or more of any character (including newlines)
//   ?      matches exactly one of any character (including newlines)
//   [seq]  matches any single character in seq (ranges supported)
//   [!seq] matches any single character not in seq
//   An unmatched '[' or a trailing ']' is treated literally (fnmatch rule).
// Recursive-descent with backtracking on '*', O(n*m) worst case.
bool glob_match_here(kimix::string_view pat, kimix::string_view text) {
    size_t pi = 0, ti = 0;
    size_t star_pi = kimix::string_view::npos, star_ti = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && pat[pi] == '*') {
            star_pi = pi;
            star_ti = ti;
            ++pi;
        } else if (pi < pat.size() && pat[pi] == '?') {
            ++pi;
            ++ti;
        } else if (pi < pat.size() && pat[pi] == '[') {
            // find the closing ']' — fnmatch treats the very first char after
            // '[' (or '[!') as a literal ']' if it is one, and scans forward.
            size_t j = pi + 1;
            bool negate = false;
            if (j < pat.size() && pat[j] == '!') {
                negate = true;
                ++j;
            }
            size_t class_start = j;
            if (j < pat.size() && pat[j] == ']') {
                ++j; // leading ']' is literal inside the class
            }
            while (j < pat.size() && pat[j] != ']') {
                ++j;
            }
            if (j >= pat.size()) {
                // no closing ']' — treat '[' as a literal character
                if (text[ti] != '[') {
                    // backtrack through the last '*'
                    if (star_pi == kimix::string_view::npos) {
                        return false;
                    }
                    pi = star_pi + 1;
                    ++star_ti;
                    ti = star_ti;
                } else {
                    ++pi;
                    ++ti;
                }
                continue;
            }
            // class is pat[class_start..j)
            kimix::string_view cls = pat.substr(class_start, j - class_start);
            bool in_class = class_matches(cls, text[ti]);
            bool matched = negate ? !in_class : in_class;
            if (matched) {
                pi = j + 1;
                ++ti;
            } else {
                if (star_pi == kimix::string_view::npos) {
                    return false;
                }
                pi = star_pi + 1;
                ++star_ti;
                ti = star_ti;
            }
        } else if (pi < pat.size()) {
            // literal character
            if (pat[pi] == text[ti]) {
                ++pi;
                ++ti;
            } else {
                if (star_pi == kimix::string_view::npos) {
                    return false;
                }
                pi = star_pi + 1;
                ++star_ti;
                ti = star_ti;
            }
        } else {
            // pattern exhausted but text remains — backtrack through last '*'
            if (star_pi == kimix::string_view::npos) {
                return false;
            }
            pi = star_pi + 1;
            ++star_ti;
            ti = star_ti;
        }
    }
    // consume trailing '*' in pattern
    while (pi < pat.size() && pat[pi] == '*') {
        ++pi;
    }
    return pi == pat.size();
}

} // namespace

wait_pattern_kind classify_wait_pattern(kimix::string_view pattern) {
    bool has_glob = false;
    for (char c : pattern) {
        if (static_cast<unsigned char>(c) > 0x7F) {
            return wait_pattern_kind::unsupported; // Unicode-aware engine needed
        }
        if (is_glob_meta(c)) {
            has_glob = true;
        } else if (is_regex_only_meta(c)) {
            return wait_pattern_kind::unsupported;
        }
    }
    return has_glob ? wait_pattern_kind::glob : wait_pattern_kind::literal;
}

tool_error match_wait_pattern(kimix::string_view pattern,
                              kimix::string_view buffer, bool &matched) {
    matched = false;
    if (pattern.empty()) {
        return tool_error{tool_status::invalid_input,
                          kimix::string("wait_for_pattern must not be empty")};
    }
    switch (classify_wait_pattern(pattern)) {
    case wait_pattern_kind::literal:
        // regex.search over a pattern with no special characters is an exact
        // substring search.
        matched = buffer.find(pattern) != kimix::string_view::npos;
        return tool_error{tool_status::ok, {}};
    case wait_pattern_kind::glob:
        // The reference uses pattern.search(buffer), i.e. find any substring
        // that matches.  For a glob that is equivalent to matching
        // "*" + pattern + "*" against the whole buffer (fnmatch semantics).
        {
            kimix::string wrapped;
            wrapped.reserve(pattern.size() + 2);
            wrapped.push_back('*');
            wrapped.append(pattern.data(), pattern.size());
            wrapped.push_back('*');
            matched = glob_match_here(wrapped, buffer);
            return tool_error{tool_status::ok, {}};
        }
    case wait_pattern_kind::unsupported:
        return tool_error{
            tool_status::unsupported,
            kimix::string("wait_for_pattern needs the Python regex engine")};
    }
    return tool_error{tool_status::unsupported, {}};
}

} // namespace kimix::builtin_tools::python
