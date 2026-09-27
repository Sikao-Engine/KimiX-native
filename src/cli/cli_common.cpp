// cli/cli_common.cpp - implementation of the shared CLI helpers.
//
// Exception-free by construction: every filesystem call uses the
// std::error_code overloads and the narrow->path conversion goes through
// kimix::path_from_narrow, which reports failure instead of
// throwing (the project is compiled with exceptions disabled).
//
// Unity build note: helpers that are not part of the public API live in an
// anonymous namespace with the `clic_` prefix.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <random>
#include <system_error>

#include "cli/cli_common.h"
#include "builtin_tools/utf8_util.h" // kimix::builtin_tools::decode_code_point

#if defined(KIMIX_PLATFORM_WINDOWS)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <signal.h>
#include <cerrno>
#endif

namespace kimix::cli {

namespace {

// Python's str.isspace() for the code points a CLI input can realistically
// carry: the ASCII set (SP, \t\n\v\f\r and \x1c..\x1f) plus the Unicode spaces
// str.isspace() also accepts.  Mirrors soul.cpp's agent-side table so the CLI
// and the soul agree on what "blank input" means.
bool clic_is_py_space(uint32_t cp) noexcept {
    if (cp < 0x80) {
        return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x1F);
    }
    switch (cp) {
    case 0x85:   // NEL
    case 0xA0:   // NBSP
    case 0x1680: // OGHAM SPACE MARK
    case 0x2028: // LINE SEPARATOR
    case 0x2029: // PARAGRAPH SEPARATOR
    case 0x202F: // NARROW NBSP
    case 0x205F: // MEDIUM MATHEMATICAL SPACE
    case 0x3000: // IDEOGRAPHIC SPACE
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

bool clic_is_ascii_space(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return u == 0x20u || (u >= 0x09u && u <= 0x0Du) || (u >= 0x1Cu && u <= 0x1Fu);
}

// Decode the code point starting at `it`, advancing `it` (at least one byte,
// U+FFFD + one byte on malformed input - never an infinite loop).
uint32_t clic_decode(const char *&it, const char *end) noexcept {
    return kimix::builtin_tools::decode_code_point(it, end);
}

// Byte offset just past the last non-space code point of `text`.
size_t clic_trimmed_end(kimix::string_view text, bool ascii_only) noexcept {
    size_t end = text.size();
    while (end > 0) {
        // A UTF-8 continuation byte cannot start a code point: rewind to the
        // leading byte so the decoder sees a complete sequence.
        size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0u) == 0x80u) {
            --start;
        }
        uint32_t cp = 0;
        if (ascii_only) {
            const unsigned char u = static_cast<unsigned char>(text[start]);
            if (u < 0x80u) {
                if (!clic_is_ascii_space(text[start])) {
                    break;
                }
                end = start;
                continue;
            }
            break; // non-ASCII byte: stop (ASCII-only trim)
        }
        const char *it = text.data() + start;
        cp = clic_decode(it, text.data() + text.size());
        if (!clic_is_py_space(cp)) {
            break;
        }
        end = start;
    }
    return end;
}

size_t clic_trimmed_start(kimix::string_view text, bool ascii_only) noexcept {
    size_t start = 0;
    while (start < text.size()) {
        if (ascii_only) {
            if (!clic_is_ascii_space(text[start])) {
                break;
            }
            ++start;
            continue;
        }
        const char *it = text.data() + start;
        const char *end = text.data() + text.size();
        const uint32_t cp = clic_decode(it, end);
        if (!clic_is_py_space(cp)) {
            break;
        }
        start = static_cast<size_t>(it - text.data());
    }
    return start;
}

} // namespace

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
bool read_file(const kimix::string &path, kimix::string &out, kimix::string &error) {
    out.clear();
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        error = "cannot open file: " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(fp);
        error = "cannot determine file size: " + path;
        return false;
    }
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        const size_t rd = std::fread(out.data(), 1, static_cast<size_t>(size), fp);
        out.resize(rd);
    }
    std::fclose(fp);
    return true;
}

bool write_file(const kimix::string &path, kimix::string_view text,
                kimix::string &error, bool append) {
    std::FILE *fp = std::fopen(path.c_str(), append ? "ab" : "wb");
    if (fp == nullptr) {
        error = "cannot open file for writing: " + path;
        return false;
    }
    if (!text.empty()) {
        const size_t written = std::fwrite(text.data(), 1, text.size(), fp);
        if (written != text.size()) {
            std::fclose(fp);
            error = "short write: " + path;
            return false;
        }
    }
    std::fclose(fp);
    return true;
}

bool file_exists(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return false;
    }
    std::error_code ec;
    return kimix::filesystem::is_regular_file(p, ec) && !ec;
}

bool dir_exists(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return false;
    }
    std::error_code ec;
    return kimix::filesystem::is_directory(p, ec) && !ec;
}

bool make_dirs(const kimix::string &path, kimix::string &error) {
    if (path.empty()) {
        return true;
    }
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        error = "unrepresentable directory path: " + path;
        return false;
    }
    std::error_code ec;
    kimix::filesystem::create_directories(p, ec);
    if (ec && !kimix::filesystem::is_directory(p, ec)) {
        error = "cannot create directory: " + path;
        return false;
    }
    return true;
}

bool remove_all(const kimix::string &path, kimix::string &error) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return true; // unrepresentable -> treated as "nothing to remove"
    }
    std::error_code ec;
    kimix::filesystem::remove_all(p, ec);
    if (ec) {
        error = "cannot remove: " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
kimix::string current_dir() {
    std::error_code ec;
    const auto p = kimix::filesystem::current_path(ec);
    if (ec) {
        return {};
    }
    return kimix::to_string(p);
}

kimix::string absolute_path(kimix::string_view path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return kimix::string(path);
    }
    std::error_code ec;
    const auto abs = kimix::filesystem::absolute(p, ec);
    if (ec) {
        return kimix::string(path);
    }
    kimix::string out = kimix::to_string(abs.lexically_normal());
 // GCC's lexically_normal keeps a trailing separator when a trailing ".."
 // removed the last component ("a/b/.." -> "a/"); the CLI's path contract
 // carries no trailing separator (Windows pins compare against the bare dir).
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
 out.pop_back();
 }
 return out;
}

kimix::string join_path(kimix::string_view a, kimix::string_view b) {
    kimix::filesystem::path pa;
    kimix::filesystem::path pb;
    if (!kimix::path_from_narrow(a, pa) ||
        !kimix::path_from_narrow(b, pb)) {
        return kimix::string(a) + "/" + kimix::string(b);
    }
    return kimix::to_string(pa / pb);
}

kimix::string parent_path(kimix::string_view path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return {};
    }
    return kimix::to_string(p.parent_path());
}

kimix::string file_name(kimix::string_view path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return kimix::string(path);
    }
    return kimix::to_string(p.filename());
}

kimix::string extension(kimix::string_view path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return {};
    }
    return kimix::to_string(p.extension());
}

kimix::string with_file_name(kimix::string_view path, kimix::string_view name) {
    kimix::filesystem::path p;
    kimix::filesystem::path n;
    if (!kimix::path_from_narrow(path, p) ||
        !kimix::path_from_narrow(name, n)) {
        return kimix::string(path);
    }
    return kimix::to_string(p.parent_path() / n);
}

kimix::string expand_home(kimix::string_view path) {
    // Python Path.expanduser(): only a bare "~" or a "~<sep>" prefix expands.
    if (path.empty() || path[0] != '~') {
        return kimix::string(path);
    }
    if (path.size() > 1 && path[1] != '/' && path[1] != '\\') {
        // "~user" syntax is not supported (the reference's Windows build does
        // not resolve it either); return unchanged like expanduser does.
        return kimix::string(path);
    }
    kimix::string home;
#if defined(KIMIX_PLATFORM_WINDOWS)
    if (!get_env("USERPROFILE", home) || home.empty()) {
        kimix::string drive, profile;
        if (get_env("HOMEDRIVE", drive) && get_env("HOMEPATH", profile)) {
            home = drive + profile;
        }
    }
#else
    get_env("HOME", home);
#endif
    if (home.empty()) {
        return kimix::string(path); // os.path.expanduser leaves "~" alone
    }
    if (path.size() == 1) {
        return home;
    }
    return join_path(home, kimix::string(path.substr(2)));
}

bool is_within_directory(kimix::string_view child, kimix::string_view parent) {
    // PurePath.relative_to parity: lexical comparison of normalised absolute
    // forms (case-insensitive on Windows, like PureWindowsPath).
    const kimix::string c = absolute_path(child);
    const kimix::string p = absolute_path(parent);
    if (c.empty() || p.empty()) {
        return false;
    }
#if defined(KIMIX_PLATFORM_WINDOWS)
    const kimix::string cl = to_lower_ascii(c);
    const kimix::string pl = to_lower_ascii(p);
    if (cl == pl) {
        return true;
    }
    const kimix::string prefixed = pl + (ends_with(pl, "\\") ? kimix::string() : kimix::string("\\"));
    return starts_with(cl, prefixed);
#else
    if (c == p) {
        return true;
    }
    const kimix::string prefixed = p + (ends_with(p, "/") ? kimix::string() : kimix::string("/"));
    return starts_with(c, prefixed);
#endif
}

// ---------------------------------------------------------------------------
// ASCII / UTF-8 string helpers
// ---------------------------------------------------------------------------
kimix::string_view trim(kimix::string_view text) {
    const size_t end = clic_trimmed_end(text, false);
    const size_t start = clic_trimmed_start(text, false);
    if (start >= end) {
        return {};
    }
    return text.substr(start, end - start);
}

kimix::string_view trim_ascii(kimix::string_view text) {
    const size_t end = clic_trimmed_end(text, true);
    const size_t start = clic_trimmed_start(text, true);
    if (start >= end) {
        return {};
    }
    return text.substr(start, end - start);
}

bool is_blank(kimix::string_view text) {
    return trim(text).empty();
}

bool starts_with(kimix::string_view text, kimix::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(kimix::string_view text, kimix::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(kimix::string_view text, kimix::string_view needle) {
    return text.find(needle) != kimix::string_view::npos;
}

size_t find(kimix::string_view text, kimix::string_view needle, size_t from) {
    return text.find(needle, from);
}

size_t rfind(kimix::string_view text, kimix::string_view needle) {
    return text.rfind(needle);
}

kimix::string to_lower_ascii(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

kimix::string to_upper_ascii(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

void split(kimix::string_view text, char delimiter, kimix::vector<kimix::string> &out,
           bool keep_empty) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = (i == text.size());
        if (!at_end && text[i] != delimiter) {
            continue;
        }
        if (i > start || keep_empty) {
            out.push_back(kimix::string(text.substr(start, i - start)));
        }
        start = i + 1;
    }
}

void split_lines(kimix::string_view text, kimix::vector<kimix::string> &out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = (i == text.size());
        if (!at_end && text[i] != '\n') {
            continue;
        }
        kimix::string_view line = text.substr(start, i - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // A trailing newline does not introduce an extra empty line.
        if (!(at_end && line.empty() && i > 0)) {
            out.push_back(kimix::string(line));
        }
        start = i + 1;
    }
}

kimix::string join(const kimix::vector<kimix::string> &parts, kimix::string_view separator) {
    kimix::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.append(separator);
        }
        out.append(parts[i]);
    }
    return out;
}

kimix::string replace_all(kimix::string_view text, kimix::string_view from,
                          kimix::string_view to) {
    if (from.empty()) {
        return kimix::string(text);
    }
    kimix::string out;
    size_t pos = 0;
    for (;;) {
        const size_t hit = text.find(from, pos);
        if (hit == kimix::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Ids / time
// ---------------------------------------------------------------------------
kimix::string random_hex(size_t bytes) {
    static const char *const kHex = "0123456789abcdef";
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        const uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^
                              static_cast<uint64_t>(rd()) ^
                              static_cast<uint64_t>(
                                  std::chrono::high_resolution_clock::now()
                                      .time_since_epoch()
                                      .count());
        return seed;
    }());
    kimix::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const uint64_t v = static_cast<uint64_t>(rng() & 0xFFull);
        out.push_back(kHex[(v >> 4) & 0xFu]);
        out.push_back(kHex[v & 0xFu]);
    }
    return out;
}

int64_t now_unix_seconds() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

kimix::string format_utc(int64_t unix_seconds, const char *fmt) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(KIMIX_PLATFORM_WINDOWS)
    if (gmtime_s(&tm, &t) != 0) {
        return {};
    }
#else
    if (gmtime_r(&t, &tm) == nullptr) {
        return {};
    }
#endif
    char buf[64] = {};
    const size_t n = std::strftime(buf, sizeof(buf), fmt, &tm);
    return kimix::string(buf, n);
}

kimix::string format_local(int64_t unix_seconds, const char *fmt) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(KIMIX_PLATFORM_WINDOWS)
    if (localtime_s(&tm, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &tm) == nullptr) {
        return {};
    }
#endif
    char buf[64] = {};
    const size_t n = std::strftime(buf, sizeof(buf), fmt, &tm);
    return kimix::string(buf, n);
}

kimix::string format_duration_hm(int64_t seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const int64_t hours = seconds / 3600;
    const int64_t minutes = (seconds % 3600) / 60;
    const int64_t secs = seconds % 60;
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                  static_cast<long long>(minutes), static_cast<long long>(secs));
    return kimix::string(buf);
}

int64_t file_mtime_unix(const kimix::string &path) {
    kimix::filesystem::path p;
    if (!kimix::path_from_narrow(path, p)) {
        return 0;
    }
    std::error_code ec;
    const auto ftime = kimix::filesystem::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
#else
    (void)ftime;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Environment / process
// ---------------------------------------------------------------------------
bool get_env(const char *name, kimix::string &out) {
    out.clear();
    if (name == nullptr) {
        return false;
    }
    const char *v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    out.assign(v);
    return true;
}

bool set_env(const char *name, kimix::string_view value) {
    if (name == nullptr) {
        return false;
    }
    const kimix::string text(value);
#if defined(KIMIX_PLATFORM_WINDOWS)
    return _putenv_s(name, text.c_str()) == 0;
#else
    return ::setenv(name, text.c_str(), 1) == 0;
#endif
}

kimix::string first_env(const char *const *names, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        kimix::string value;
        if (get_env(names[i], value)) {
            return value;
        }
    }
    return {};
}

bool enable_console_ansi() {
#if defined(KIMIX_PLATFORM_WINDOWS)
    static bool configured = false;
    static bool ansi_ok = false;
    if (configured) {
        return ansi_ok;
    }
    configured = true;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    ansi_ok = true;
    const DWORD handles[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (const DWORD which : handles) {
        const HANDLE h = GetStdHandle(which);
        if (h == nullptr || h == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) {
            continue; // redirected to a file/pipe: no ANSI needed
        }
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
            if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                ansi_ok = false;
            }
        }
    }
    return ansi_ok;
#else
    return true;
#endif
}

bool stream_is_console(std::FILE *stream) {
    if (stream == nullptr) {
        return false;
    }
#if defined(KIMIX_PLATFORM_WINDOWS)
    return _isatty(_fileno(stream)) != 0;
#else
    return ::isatty(::fileno(stream)) != 0;
#endif
}

int terminal_columns(std::FILE *stream) {
    // shutil.get_terminal_size(): the COLUMNS environment override first.
    kimix::string columns;
    if (get_env("COLUMNS", columns) && !columns.empty()) {
        char *end = nullptr;
        const long value = std::strtol(columns.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && value > 0 && value <= 100000) {
            return static_cast<int>(value);
        }
    }
#if defined(KIMIX_PLATFORM_WINDOWS)
    if (stream != nullptr) {
        const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            const int width =
                static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
            if (width > 0) {
                return width;
            }
        }
    }
#else
    if (stream != nullptr) {
        struct winsize ws {};
        if (::ioctl(::fileno(stream), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            return static_cast<int>(ws.ws_col);
        }
    }
#endif
    return 80; // shutil's fallback
}

// ---------------------------------------------------------------------------
// Shared tool temp folder (common.py _temp_folder + cleanup_temp_folder)
// ---------------------------------------------------------------------------
namespace {

// Windows recycles pids: a tmp_<pid> folder untouched for a day belongs to a
// long-dead process even when the pid is in use again (common.py
// _STALE_TEMP_FOLDER_MAX_AGE).
constexpr int64_t kStaleTempFolderMaxAge = 24 * 60 * 60;

bool clic_has_fresh_file(const kimix::string &dir) {
    std::error_code ec;
    const kimix::filesystem::path base{kimix::string(dir)};
    if (!kimix::filesystem::is_directory(base, ec) || ec) {
        return true; // cannot inspect -> keep (conservative)
    }
    const auto cutoff = std::chrono::system_clock::now() -
                        std::chrono::seconds(kStaleTempFolderMaxAge);
    for (kimix::filesystem::recursive_directory_iterator it(base, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            return true;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const auto ftime = kimix::filesystem::last_write_time(it->path(), file_ec);
        if (file_ec) {
            continue;
        }
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        if (sys > cutoff) {
            return true;
        }
#else
        return true; // clock_cast unavailable: keep conservatively
#endif
    }
    return false;
}

} // namespace

kimix::string cli_temp_dir(const kimix::string &work_dir) {
    const kimix::string base = work_dir.empty() ? current_dir() : work_dir;
    int64_t pid = 0;
#if defined(KIMIX_PLATFORM_WINDOWS)
    pid = static_cast<int64_t>(::GetCurrentProcessId());
#else
    pid = static_cast<int64_t>(::getpid());
#endif
    return join_path(join_path(base, ".kimix_cache"),
                     kimix::format("tmp_{}", pid));
}

bool process_alive(int64_t pid) {
    if (pid <= 0) {
        return false;
    }
#if defined(KIMIX_PLATFORM_WINDOWS)
    constexpr uint32_t kProcessQueryLimitedInformation = 0x1000;
    constexpr uint32_t kErrorAccessDenied = 5;
    const HANDLE handle = ::OpenProcess(kProcessQueryLimitedInformation, FALSE,
                                        static_cast<DWORD>(pid));
    if (handle != nullptr) {
        ::CloseHandle(handle);
        return true;
    }
    return ::GetLastError() == kErrorAccessDenied;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool cleanup_temp_folder(const kimix::string &work_dir, kimix::string &error) {
    error.clear();
    const kimix::string own = cli_temp_dir(work_dir);
    const kimix::string cache_root = parent_path(own);
    // 1. This process's own folder (best effort).
    if (dir_exists(own)) {
        kimix::string rm_error;
        if (!remove_all(own, rm_error)) {
            error = rm_error; // keep going: the stale sweep still runs
        }
    }
    // 2. Stale tmp_<pid> siblings of dead processes.
    if (!dir_exists(cache_root)) {
        return error.empty();
    }
    kimix::filesystem::path root;
    if (!kimix::path_from_narrow(cache_root, root)) {
        return false;
    }
    std::error_code ec;
    for (kimix::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec) || ec) {
            continue;
        }
        const kimix::string name = kimix::to_string(it->path().filename());
        if (!starts_with(name, "tmp_")) {
            continue;
        }
        const kimix::string digits = name.substr(4);
        bool all_digits = !digits.empty();
        for (const char ch : digits) {
            if (ch < '0' || ch > '9') {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) {
            continue;
        }
        const kimix::string dir = kimix::to_string(it->path());
        if (dir == own) {
            continue;
        }
        const int64_t pid = std::atoll(digits.c_str());
        if (process_alive(pid) && clic_has_fresh_file(dir)) {
            continue; // a live process's fresh folder
        }
        kimix::string rm_error;
        remove_all(dir, rm_error); // best effort, never reported
    }
    return error.empty();
}

void flush_streams() {
    std::fflush(stdout);
    std::fflush(stderr);
}

} // namespace kimix::cli
