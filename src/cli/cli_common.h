// cli/cli_common.h - Small shared helpers for the native CLI (src/cli).
//
// Everything here is a thin, exception-free wrapper over the platform / STL
// facilities the CLI modules need: file I/O, path handling (through the
// non-throwing kimix::filesystem::path_from_narrow), ASCII string helpers,
// time formatting, environment access and the per-process console setup.
//
// Rules (see src/cli/PLAN.md and .agents/skills/cpp):
// * namespace kimix::cli, kimix:: containers in every public API.
// * No exceptions: every fallible operation returns bool plus an `error`
//   out-parameter (or an empty string / 0 / false on failure).
// * Unity build: no file-scope `using namespace`; TU-local helpers live in an
//   anonymous namespace with the `clic_` prefix in cli_common.cpp.

#pragma once

#include <cstdint>
#include <cstdio>
#include <core/kimix_core.h>

namespace kimix::cli {

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
// Read the whole file as bytes.  Returns false with `error` set when the file
// is missing or unreadable.  `out` is cleared first.
bool read_file(const kimix::string &path, kimix::string &out, kimix::string &error);

// Write `text` to `path` (truncating, or appending when `append` is true).
// Returns false with `error` set when the file cannot be opened/written.
bool write_file(const kimix::string &path, kimix::string_view text,
                kimix::string &error, bool append = false);

bool file_exists(const kimix::string &path);
bool dir_exists(const kimix::string &path);

// Create `path` and every missing parent.  Succeeds (true) when the directory
// already exists.
bool make_dirs(const kimix::string &path, kimix::string &error);

// Recursively delete `path`.  A missing path is success.
bool remove_all(const kimix::string &path, kimix::string &error);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
// Process current working directory; empty when it cannot be determined.
kimix::string current_dir();
// Absolute, lexically normalised form of `path` (no-op when already absolute).
// Returns `path` unchanged when the conversion is impossible.
kimix::string absolute_path(kimix::string_view path);
// `a/b` with exactly one separator, using the native separator.
kimix::string join_path(kimix::string_view a, kimix::string_view b);
kimix::string parent_path(kimix::string_view path);
kimix::string file_name(kimix::string_view path);
// File extension including the leading dot ("", ".py", ".json").
kimix::string extension(kimix::string_view path);
// Replace the file name of `path` with `name`.
kimix::string with_file_name(kimix::string_view path, kimix::string_view name);

// ---------------------------------------------------------------------------
// ASCII / UTF-8 string helpers
// ---------------------------------------------------------------------------
// Python str.strip() whitespace for the ASCII range (SP, \t..\r and \x1c..\x1f,
// which str.isspace() also accepts) plus the Unicode spaces the soul's
// agent_user_input_is_empty() already recognises (U+0085, U+00A0, U+1680,
// U+2000..U+200A, U+2028, U+2029, U+202F, U+205F, U+3000).
kimix::string_view trim(kimix::string_view text);
// Python str.strip() for the ASCII range only (never touches non-ASCII bytes).
kimix::string_view trim_ascii(kimix::string_view text);
bool is_blank(kimix::string_view text);

bool starts_with(kimix::string_view text, kimix::string_view prefix);
bool ends_with(kimix::string_view text, kimix::string_view suffix);
bool contains(kimix::string_view text, kimix::string_view needle);
// First occurrence of `needle`, or npos.
size_t find(kimix::string_view text, kimix::string_view needle, size_t from = 0);
// Last occurrence of `needle`, or npos.
size_t rfind(kimix::string_view text, kimix::string_view needle);
// ASCII-only lowercase / uppercase (locale independent).
kimix::string to_lower_ascii(kimix::string_view text);
kimix::string to_upper_ascii(kimix::string_view text);
// Split on a single-character delimiter; `keep_empty` mirrors Python's
// `str.split(sep)` (false) vs `splitlines`-style retention (true).
void split(kimix::string_view text, char delimiter, kimix::vector<kimix::string> &out,
           bool keep_empty = true);
// Split into lines, dropping a trailing '\r' and a final empty line.
void split_lines(kimix::string_view text, kimix::vector<kimix::string> &out);
// Join with `separator`.
kimix::string join(const kimix::vector<kimix::string> &parts, kimix::string_view separator);
// Replace every occurrence of `from` with `to`.
kimix::string replace_all(kimix::string_view text, kimix::string_view from, kimix::string_view to);

// ---------------------------------------------------------------------------
// Ids / time
// ---------------------------------------------------------------------------
// `bytes` random bytes rendered as lowercase hex (16 -> 32 chars, the shape of
// Python's uuid4().hex used for session ids).
kimix::string random_hex(size_t bytes = 16);
// Seconds since the Unix epoch (UTC), and the local-offset-free UTC formatter.
int64_t now_unix_seconds();
// strftime in UTC; `fmt` defaults to "%Y-%m-%d %H:%M:%S" (the /sessions list).
kimix::string format_utc(int64_t unix_seconds, const char *fmt = "%Y-%m-%d %H:%M:%S");
// "H:MM:SS" (the /compact duration line).
kimix::string format_duration_hm(int64_t seconds);
// Last-write time in Unix seconds; 0 when the path does not exist.
int64_t file_mtime_unix(const kimix::string &path);

// ---------------------------------------------------------------------------
// Environment / process
// ---------------------------------------------------------------------------
// True when the variable is set (possibly to an empty string); fills `out`.
bool get_env(const char *name, kimix::string &out);
bool set_env(const char *name, kimix::string_view value);
// The first set variable out of a NUL-terminated name list, or "" when none.
kimix::string first_env(const char *const *names, size_t count);

// Configure the process console for UTF-8 ANSI output (Windows: output code
// page 65001 + ENABLE_VIRTUAL_TERMINAL_PROCESSING on stdout/stderr).  Safe to
// call multiple times and a no-op on POSIX.  Returns true when colour escapes
// can be emitted to stdout.
bool enable_console_ansi();
// True when `stream` is attached to a console (isatty equivalent).
bool stream_is_console(std::FILE *stream);

// Flush every stdio stream (used before spawning/after a turn).
void flush_streams();

} // namespace kimix::cli
