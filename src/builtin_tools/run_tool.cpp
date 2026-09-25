// run_tool.cpp - C++ port of the kimi-agent `Run` tool (see run_tool.h for the
// reference line map).
//
// Unity-build rules: every TU-local helper lives in an anonymous namespace
// inside kimix::builtin_tools::run and carries the `rn_` prefix.
//
// Reuse (never re-implemented here):
// * proc::run_process / start_task / send_task / wait_task / read_task /
//   query_task / list_tasks / remove_task  - the reproc spawn + drain layer
// * bash::interpret_exit_code / is_expected_exit / truncate_lines /
//   find_error_line_index                  - exit-code semantics + folding
// * python::build_session_output_block     - the YAML-ish result block
// * python::classify_wait_pattern / match_wait_pattern - wait_for_pattern
// * kimix::runtime::tools::check_hardline_blocked / annotate_failure /
//   foreground_background_guidance         - shell_safety.cpp is linked into
//   kimix-llm (the same arrangement pwsh_tool.cpp uses)
#include "builtin_tools/run_tool.h"

#include <cstdio>

#include <core/clock.h>
#include <runtime/tools/shell_safety.h>

#include "builtin_tools/bash_tool.h"
#include "builtin_tools/process_runner.h"
#include "builtin_tools/python_tool.h"
#include "builtin_tools/tool_registry.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::run {

namespace {

// ---------------------------------------------------------------------------
// shlex state machine (CPython shlex.read_token with whitespace_split=True,
// commenters='' and punctuation_chars='')
// ---------------------------------------------------------------------------
constexpr kimix::string_view k_shlex_whitespace = " \t\r\n";
constexpr kimix::string_view k_shlex_quotes = "'\"";

bool rn_is_whitespace(char c) noexcept {
  return k_shlex_whitespace.find(c) != kimix::string_view::npos;
}

bool rn_is_quote(char c) noexcept {
  return k_shlex_quotes.find(c) != kimix::string_view::npos;
}

// One shlex.split() call. Returns false with the CPython ValueError wording on
// malformed input.
bool rn_shlex_split(kimix::string_view text, bool posix,
                    kimix::vector<kimix::string> &out, kimix::string &error) {
  out.clear();
  error.clear();
  size_t pos = 0;
  const size_t n = text.size();
  // ' ' == whitespace state, 'a' == word state, '\\' == escape state,
  // '\'' / '"' == inside that quote.
  char state = ' ';
  kimix::string token;
  bool quoted = false;
  char escapedstate = ' ';
  bool have_token = false; // a token is open (state != ' ' or token non-empty)

  auto emit = [&]() {
    // get_token(): `if self.posix and not quoted and result == '': result =
    // None`
    if (!(posix && !quoted && token.empty())) {
      out.push_back(token);
    }
    token.clear();
    quoted = false;
    state = ' ';
    have_token = false;
  };

  while (true) {
    const bool eof = (pos >= n);
    const char c = eof ? '\0' : text[pos];

    if (state == ' ') {
      if (eof) {
        // Non-posix stops on the empty token (eof == ''); posix maps an
        // unquoted empty token to None. Either way nothing is emitted.
        return true;
      }
      if (rn_is_whitespace(c)) {
        ++pos;
        if (!token.empty() || (posix && quoted)) {
          emit();
        }
        continue;
      }
      if (posix && c == '\\') {
        escapedstate = 'a';
        state = '\\';
        have_token = true;
        ++pos;
        continue;
      }
      if (rn_is_quote(c)) {
        if (!posix) {
          token.push_back(c);
        }
        state = c;
        have_token = true;
        ++pos;
        continue;
      }
      // wordchars / whitespace_split: everything else opens a word.
      token.push_back(c);
      state = 'a';
      have_token = true;
      ++pos;
      continue;
    }

    if (state == '\'' || state == '"') {
      quoted = true;
      if (eof) {
        error = "No closing quotation";
        return false;
      }
      if (c == state) {
        ++pos;
        if (!posix) {
          token.push_back(c);
          emit(); // non-posix closes the token right away
          continue;
        }
        state = 'a'; // posix keeps accumulating into the same token
        have_token = true;
        continue;
      }
      if (posix && c == '\\' && state == '"') { // escapedquotes == '"'
        escapedstate = state;
        state = '\\';
        ++pos;
        continue;
      }
      token.push_back(c);
      ++pos;
      continue;
    }

    if (state == '\\') {
      if (eof) {
        error = "No escaped character";
        return false;
      }
      // "In posix shells, only the quote itself or the escape character
      // may be escaped within quotes."
      if ((escapedstate == '\'' || escapedstate == '"') && c != '\\' &&
          c != escapedstate) {
        token.push_back('\\');
      }
      token.push_back(c);
      ++pos;
      state = escapedstate;
      continue;
    }

    // state == 'a'
    if (eof) {
      if (!token.empty() || (posix && quoted)) {
        emit();
      }
      (void)have_token;
      return true;
    }
    if (rn_is_whitespace(c)) {
      ++pos;
      state = ' ';
      if (!token.empty() || (posix && quoted)) {
        emit();
      }
      continue;
    }
    if (posix && rn_is_quote(c)) {
      state = c;
      ++pos;
      continue;
    }
    if (posix && c == '\\') {
      escapedstate = 'a';
      state = '\\';
      ++pos;
      continue;
    }
    token.push_back(c);
    ++pos;
  }
}

// Python str.split() with no argument: split on runs of ASCII/Unicode
// whitespace and drop empty fields.
kimix::vector<kimix::string> rn_whitespace_split(kimix::string_view text) {
  kimix::vector<kimix::string> out;
  size_t i = 0;
  const size_t n = text.size();
  auto is_space = [&](size_t k) {
    const char c = text[k];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
        c == '\f') {
      return true;
    }
    return false;
  };
  while (i < n) {
    while (i < n && is_space(i)) {
      ++i;
    }
    const size_t start = i;
    while (i < n && !is_space(i)) {
      ++i;
    }
    if (i > start) {
      out.emplace_back(text.substr(start, i - start));
    }
  }
  return out;
}

// " ".join(parts)
kimix::string rn_join_space(kimix::span<const kimix::string> parts) {
  kimix::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += ' ';
    }
    out += parts[i];
  }
  return out;
}

// Collapse every whitespace run to a single space and trim the ends
// (Python " ".join(s.split())).
kimix::string rn_collapse_whitespace(kimix::string_view text) {
  return rn_join_space(rn_whitespace_split(text));
}

const char *rn_status_string(tool_status status) noexcept {
  switch (status) {
  case tool_status::ok:
    return "ok";
  case tool_status::invalid_input:
    return "invalid_input";
  case tool_status::not_found:
    return "not_found";
  case tool_status::no_change:
    return "no_change";
  case tool_status::ambiguous:
    return "ambiguous";
  case tool_status::blocked:
    return "blocked";
  case tool_status::too_large:
    return "too_large";
  case tool_status::unsupported:
    return "unsupported";
  case tool_status::external_library:
    return "external_library";
  }
  return "unknown";
}

// shlex.quote's _find_unsafe = re.compile(r'[^\w@%+=:,./-]', re.ASCII): the
// safe set is ASCII word characters plus @%+=:,./- .
bool rn_quote_is_safe(kimix::string_view text) noexcept {
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool word = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                      (u >= '0' && u <= '9') || u == '_';
    if (word) {
      continue;
    }
    if (kimix::string_view("@%+=:,./-").find(c) != kimix::string_view::npos) {
      continue;
    }
    return false;
  }
  return true;
}

bool rn_has_path_separator(kimix::string_view text) noexcept {
#ifdef KIMIX_PLATFORM_WINDOWS
  return text.find('\\') != kimix::string_view::npos ||
         text.find('/') != kimix::string_view::npos;
#else
  return text.find('/') != kimix::string_view::npos;
#endif
}

// Path separator used to split PATH.
char rn_path_separator() noexcept {
#ifdef KIMIX_PLATFORM_WINDOWS
  return ';';
#else
  return ':';
#endif
}

// Join a directory and a file name with the platform separator.
kimix::string rn_join_path(kimix::string_view dir, kimix::string_view name) {
  if (dir.empty()) {
    return kimix::string(name);
  }
  kimix::string out(dir);
  const char last = out.back();
#ifdef KIMIX_PLATFORM_WINDOWS
  if (last != '\\' && last != '/') {
    out += '\\';
  }
#else
  if (last != '/') {
    out += '/';
  }
#endif
  out.append(name.data(), name.size());
  return out;
}

// PATHEXT list (Windows); empty on POSIX.
kimix::vector<kimix::string> rn_pathext(kimix::string_view pathext_env) {
  kimix::vector<kimix::string> exts;
#ifndef KIMIX_PLATFORM_WINDOWS
  (void)pathext_env;
  return exts;
#else
  kimix::string_view rest = pathext_env.empty()
                                ? kimix::string_view(".COM;.EXE;.BAT;.CMD")
                                : pathext_env;
  while (!rest.empty()) {
    const size_t semi = rest.find(';');
    const kimix::string_view one =
        (semi == kimix::string_view::npos) ? rest : rest.substr(0, semi);
    if (!one.empty()) {
      exts.emplace_back(one);
    }
    if (semi == kimix::string_view::npos) {
      break;
    }
    rest.remove_prefix(semi + 1);
  }
  return exts;
#endif
}

bool rn_has_extension(kimix::string_view name,
                      kimix::span<const kimix::string> exts) {
  for (const kimix::string &ext : exts) {
    if (name.size() > ext.size()) {
      const kimix::string_view tail = name.substr(name.size() - ext.size());
      bool same = true;
      for (size_t i = 0; i < tail.size(); ++i) {
        const char a = tail[i];
        const char b = ext[i];
        const char la = (a >= 'A' && a <= 'Z') ? char(a + 32) : a;
        const char lb = (b >= 'A' && b <= 'Z') ? char(b + 32) : b;
        if (la != lb) {
          same = false;
          break;
        }
      }
      if (same) {
        return true;
      }
    }
  }
  return false;
}

// Python repr() of one code point (validate_workdir's `{char!r}`).
kimix::string rn_repr_char(uint32_t cp) {
  // A lone single quote switches Python to double quotes so the character
  // itself stays unescaped.
  if (cp == '\'') {
    return "\"'\"";
  }
  kimix::string body;
  switch (cp) {
  case '\\':
    body = "\\\\";
    break;
  case '\n':
    body = "\\n";
    break;
  case '\r':
    body = "\\r";
    break;
  case '\t':
    body = "\\t";
    break;
  default:
    if (cp < 0x20 || cp == 0x7F) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "\\x%02x", cp);
      body = buf;
    } else if (cp < 0x80) {
      body.push_back(static_cast<char>(cp));
    } else if (cp < 0x10000) {
      // Printable non-ASCII is emitted literally by repr(); the
      // workdir allowed set is ASCII-only, so anything reaching here is
      // either printable (emit literally) or a format/control code
      // point (escape). Cc/Cf below 0x10000 are escaped as \uXXXX.
      const bool control =
          (cp >= 0x0080 && cp <= 0x009F) || cp == 0x00AD ||
          (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
          (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFEFF && cp <= 0xFEFF);
      if (control) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\\u%04x", cp);
        body = buf;
      } else {
        // Encode as UTF-8.
        if (cp < 0x800) {
          body.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          body.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          body.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          body.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          body.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
      }
    } else {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "\\U%08x", cp);
      body = buf;
    }
    break;
  }
  kimix::string out = "'";
  out += body;
  out += "'";
  return out;
}

// tools/security.py _WORKDIR_ALLOWED (shim tools.py 622-624).
bool rn_workdir_allowed(char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return true;
  }
  if (c >= 'a' && c <= 'z') {
    return true;
  }
  if (c >= '0' && c <= '9') {
    return true;
  }
  // " _.-\\/:~"
  return c == ' ' || c == '_' || c == '.' || c == '-' || c == '\\' ||
         c == '/' || c == ':' || c == '~';
}

// Read a string parameter, honouring one alias.
tool_error rn_string_param(const ToolParams *params, kimix::string_view name,
                           kimix::string_view alias, bool required,
                           kimix::string &out) {
  out.clear();
  if (params == nullptr) {
    return required
               ? tool_error{tool_status::invalid_input,
                            kimix::format("missing required field: {}", name)}
               : tool_error{tool_status::ok, {}};
  }
  const ValueElement *el = params->get(name);
  if ((el == nullptr || el->is_null()) && !alias.empty()) {
    el = params->get(alias);
  }
  if (el == nullptr || el->is_null()) {
    return required
               ? tool_error{tool_status::invalid_input,
                            kimix::format("missing required field: {}", name)}
               : tool_error{tool_status::ok, {}};
  }
  if (!el->is_string()) {
    return {tool_status::invalid_input,
            kimix::format("{} must be a string", name)};
  }
  out = el->as_string();
  return {tool_status::ok, {}};
}

// pydantic's lax `int` coercion of a string: `int(text.strip())` - an optional
// sign, digits, and underscores between digits. Overflow saturates so the
// clamp step below can still apply.
bool rn_py_int_from_string(kimix::string_view text, int64_t &out) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                   text[i] == '\r' || text[i] == '\f' || text[i] == '\v')) {
    ++i;
  }
  size_t end = n;
  while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                     text[end - 1] == '\n' || text[end - 1] == '\r' ||
                     text[end - 1] == '\f' || text[end - 1] == '\v')) {
    --end;
  }
  bool negative = false;
  if (i < end && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= end) {
    return false;
  }
  bool any_digit = false;
  bool last_underscore = true; // a leading underscore is invalid
  uint64_t magnitude = 0;
  for (; i < end; ++i) {
    const char c = text[i];
    if (c == '_') {
      if (last_underscore) {
        return false;
      }
      last_underscore = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return false;
    }
    any_digit = true;
    last_underscore = false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (magnitude > (UINT64_MAX - digit) / 10u) {
      magnitude = UINT64_MAX; // saturate
    } else {
      magnitude = magnitude * 10u + digit;
    }
  }
  if (!any_digit || last_underscore) {
    return false;
  }
  if (negative) {
    out = magnitude > static_cast<uint64_t>(INT64_MAX)
              ? INT64_MIN
              : -static_cast<int64_t>(magnitude);
  } else {
    out = magnitude > static_cast<uint64_t>(INT64_MAX)
              ? INT64_MAX
              : static_cast<int64_t>(magnitude);
  }
  return true;
}

// pydantic's lax `bool` coercion: ints / floats by truthiness, and the
// documented string table (case-insensitive, whitespace trimmed).
bool rn_py_bool_from_string(kimix::string_view text, bool &out) {
  size_t i = 0;
  size_t end = text.size();
  while (i < end && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                     text[i] == '\r')) {
    ++i;
  }
  while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                     text[end - 1] == '\n' || text[end - 1] == '\r')) {
    --end;
  }
  kimix::string lowered;
  lowered.reserve(end - i);
  for (size_t k = i; k < end; ++k) {
    const char c = text[k];
    lowered.push_back((c >= 'A' && c <= 'Z') ? char(c + 32) : c);
  }
  if (lowered == "true" || lowered == "yes" || lowered == "on" ||
      lowered == "1" || lowered == "t" || lowered == "y") {
    out = true;
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "off" ||
      lowered == "0" || lowered == "f" || lowered == "n") {
    out = false;
    return true;
  }
  return false;
}

// Bool parameter with the reference's lax coercion. `run_py` rejects an
// uncoercible value with a pydantic "Input should be a valid boolean" error.
struct bool_param_result {
  bool value = false;
  bool present = false;
  tool_error error;
};

// Integer parameter with the reference's pydantic/call-layer semantics:
//
// * lax coercion (`_coerce` in kosong's argument-repair pass): bool -> 0/1, an
//   integral float is accepted (7.9 is not), a string is parsed with int().
// * clamping (`_clamp_numeric_value`): a value that was already numeric is
//   clamped into [ge, le] instead of being rejected -- `timeout: 0` runs with
//   1s, `timeout: 1000` with 900s, `max_lines: 2` folds to 3 lines.  A value
//   that only *became* numeric through coercion is validated strictly, exactly
//   like the reference (its clamp pass runs before the coercion pass, so
//   `timeout: "0"` is an error while `timeout: 0` is clamped).
tool_error rn_int_param(const ToolParams *params, kimix::string_view name,
                        int64_t fallback, kimix::optional<int64_t> ge,
                        kimix::optional<int64_t> le, int64_t &out,
                        bool &present) {
  out = fallback;
  present = false;
  if (params == nullptr) {
    return {tool_status::ok, {}};
  }
  const ValueElement *el = params->get(name);
  if (el == nullptr || el->is_null()) {
    return {tool_status::ok, {}};
  }
  int64_t value = 0;
  bool numeric = false; // already a number -> clampable
  if (el->is_int()) {
    value = el->as_int();
    numeric = true;
  } else if (el->is_uint()) {
    value = static_cast<int64_t>(el->as_uint());
    numeric = true;
  } else if (el->is_bool()) {
    value = el->as_bool() ? 1 : 0; // coerced, never clamped
  } else if (el->is_real()) {
    const double d = el->as_real();
    if (d != static_cast<double>(static_cast<int64_t>(d))) {
      return {tool_status::invalid_input,
              kimix::format("{} must be an integer", name)};
    }
    value = static_cast<int64_t>(d);
    numeric = true;
  } else if (el->is_string()) {
    if (!rn_py_int_from_string(el->as_string(), value)) {
      return {tool_status::invalid_input,
              kimix::format("{} must be an integer", name)};
    }
  } else {
    return {tool_status::invalid_input,
            kimix::format("{} must be an integer", name)};
  }
  if (numeric) {
    if (ge.has_value() && value < *ge) {
      value = *ge;
    }
    if (le.has_value() && value > *le) {
      value = *le;
    }
  } else {
    if (ge.has_value() && value < *ge) {
      return {
          tool_status::invalid_input,
          kimix::format("{} must be greater than or equal to {}", name, *ge)};
    }
    if (le.has_value() && value > *le) {
      return {tool_status::invalid_input,
              kimix::format("{} must be less than or equal to {}", name, *le)};
    }
  }
  out = value;
  present = true;
  return {tool_status::ok, {}};
}

bool_param_result rn_bool_param(const ToolParams *params,
                                kimix::string_view name, bool fallback) {
  bool_param_result r;
  r.value = fallback;
  if (params == nullptr) {
    return r;
  }
  const ValueElement *el = params->get(name);
  if (el == nullptr || el->is_null()) {
    // NOTE: explicit JSON null is rejected by the reference for these
    // (non-optional) fields; treated as absent here - see the report.
    return r;
  }
  if (el->is_bool()) {
    r.value = el->as_bool();
    r.present = true;
    return r;
  }
  if (el->is_int()) {
    r.value = el->as_int() != 0;
    r.present = true;
    return r;
  }
  if (el->is_uint()) {
    r.value = el->as_uint() != 0;
    r.present = true;
    return r;
  }
  if (el->is_real()) {
    r.value = el->as_real() != 0.0;
    r.present = true;
    return r;
  }
  if (el->is_string()) {
    bool parsed = false;
    if (rn_py_bool_from_string(el->as_string(), parsed)) {
      r.value = parsed;
      r.present = true;
      return r;
    }
  }
  r.error = {tool_status::invalid_input,
             kimix::format("{} must be a boolean", name)};
  return r;
}

// Serialize one result envelope.
void rn_serialize(kimix::vector<char> &sink, const ToolParams &result) {
  sink.clear();
  result.serialize(sink);
}

void rn_error_result(ToolParams &result, tool_status status,
                     kimix::string_view message, kimix::string_view brief) {
  result.values["ok"] = ValueElement::make_bool(false);
  result.values["status"] =
      ValueElement::make_string(kimix::string(rn_status_string(status)));
  result.values["message"] = ValueElement::make_string(kimix::string(message));
  result.values["output"] = ValueElement::make_string(kimix::string());
  result.values["brief"] = ValueElement::make_string(kimix::string(brief));
}

// The default filesystem probes.
bool rn_default_is_file(kimix::string_view path) {
  // Constructing a std::filesystem::path from arbitrary narrow bytes can fail
  // on Windows ("No mapping for the Unicode character exists in the target
  // multi-byte code page"), and tool kernels must never throw across the tool
  // boundary - so a path we cannot even represent is not a file.
  //
  // kimix is built without C++ exceptions (kimix_enable_exception=false), so
  // the former try/catch guard around the conversion is replaced by the
  // non-throwing kimix::path_from_narrow() helper, which reports exactly the
  // same condition through its return value.
  kimix::filesystem::path target;
  if (!kimix::path_from_narrow(path, target)) {
    return false;
  }
  std::error_code ec;
  return kimix::filesystem::is_regular_file(target, ec);
}

const char *rn_getenv(kimix::string_view name) {
  kimix::string key(name);
  return std::getenv(key.c_str());
}

kimix::string rn_env_or(kimix::string_view name, kimix::string_view fallback) {
  const char *v = rn_getenv(name);
  if (v == nullptr) {
    return kimix::string(fallback);
  }
  return kimix::string(v);
}

// Python `Path(token).stem` (WindowsPath flavour - the reference host).
// Separators and a drive prefix are not part of the final component, trailing
// separators are ignored, and a leading dot does not start a suffix:
// "git.exe" -> "git", "a.tar.gz" -> "a.tar", ".git" -> ".git",
// "git." -> "git.", "dir/git/" -> "git".
kimix::string rn_path_stem(kimix::string_view token) {
  kimix::string_view s = token;
  if (s.size() >= 2 && s[1] == ':' &&
      ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
    s.remove_prefix(2); // drive prefix
  }
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
    s.remove_suffix(1);
  }
  size_t cut = kimix::string_view::npos;
  for (size_t i = s.size(); i > 0; --i) {
    if (s[i - 1] == '/' || s[i - 1] == '\\') {
      cut = i - 1;
      break;
    }
  }
  const kimix::string_view name =
      (cut == kimix::string_view::npos) ? s : s.substr(cut + 1);
  if (name.empty() || name == "." || name == "..") {
    return kimix::string(name);
  }
  size_t dot = kimix::string_view::npos;
  for (size_t i = name.size(); i > 0; --i) {
    if (name[i - 1] == '.') {
      dot = i - 1;
      break;
    }
  }
  if (dot != kimix::string_view::npos && dot > 0 && dot < name.size() - 1) {
    return kimix::string(name.substr(0, dot));
  }
  return kimix::string(name);
}

// run.py 369-380: a resolved executable whose stem rtk knows is re-run as
// `[<rtk binary>, <executable>, <args...>]` (rtk stays out of the way when the
// executable IS rtk). `run_config::run_rtk_check` supplies the binary path for
// the stem (nullopt == the share-bin binary is unavailable).
bool rn_maybe_apply_rtk(const run_config &cfg, kimix::string &executable,
                        kimix::vector<kimix::string> &args) {
  if (!cfg.run_rtk_check || executable.empty()) {
    return false;
  }
  if (executable == "rtk" || executable == "rtk.exe") {
    return false;
  }
  const kimix::string stem = rn_path_stem(executable);
  if (!bash::is_known_rtk_command(stem)) {
    return false;
  }
  const kimix::optional<kimix::string> binary = cfg.run_rtk_check(stem);
  if (!binary.has_value() || binary->empty()) {
    return false;
  }
  args.insert(args.begin(), executable);
  executable = *binary;
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. shlex family
// ---------------------------------------------------------------------------

kimix::string shlex_quote(kimix::string_view text) {
  if (text.empty()) {
    return "''";
  }
  if (rn_quote_is_safe(text)) {
    return kimix::string(text);
  }
  kimix::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

kimix::string shlex_join(kimix::span<const kimix::string> argv) {
  kimix::string out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      out += ' ';
    }
    out += shlex_quote(argv[i]);
  }
  return out;
}

bool shlex_split(kimix::string_view text, bool posix,
                 kimix::vector<kimix::string> &out, kimix::string &error) {
  return rn_shlex_split(text, posix, out, error);
}

tool_error shlex_split_tool(kimix::string_view text, bool posix,
                            kimix::vector<kimix::string> &out) {
  kimix::string error;
  if (!rn_shlex_split(text, posix, out, error)) {
    return {tool_status::invalid_input, error};
  }
  return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// 2. Command-line decomposition
// ---------------------------------------------------------------------------

kimix::string strip_outer_double_quotes(kimix::string_view arg) {
  if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
    return kimix::string(arg.substr(1, arg.size() - 2));
  }
  return kimix::string(arg);
}

resolved_command resolve_executable(kimix::span<const kimix::string> parts,
                                    bool posix, const is_file_probe &is_file) {
  resolved_command r;
  if (parts.empty()) {
    return r;
  }
  kimix::string first = parts[0];
  if (!posix) {
    first = strip_outer_double_quotes(first);
  }
  r.executable = first;
  r.consumed_tokens = 1;
  for (size_t i = 1; i < parts.size(); ++i) {
    r.args.push_back(parts[i]);
  }
  if (parts.size() <= 1) {
    return r;
  }
  // Progressive prefix lookup for unquoted paths containing spaces.
  for (size_t i = 2; i <= parts.size(); ++i) {
    kimix::string candidate =
        rn_join_space(kimix::span<const kimix::string>(parts.data(), i));
    if (!posix) {
      candidate = strip_outer_double_quotes(candidate);
    }
    if (is_file && is_file(candidate)) {
      r.executable = std::move(candidate);
      r.consumed_tokens = i;
      r.args.clear();
      for (size_t k = i; k < parts.size(); ++k) {
        r.args.push_back(parts[k]);
      }
      break;
    }
  }
  if (!posix) {
    for (kimix::string &arg : r.args) {
      arg = strip_outer_double_quotes(arg);
    }
  }
  return r;
}

kimix::string which(kimix::string_view name, kimix::string_view path_env,
                    kimix::string_view pathext_env,
                    const is_file_probe &is_file) {
  if (name.empty()) {
    return {};
  }
  const kimix::vector<kimix::string> exts = rn_pathext(pathext_env);
  auto matches = [&](kimix::string_view candidate) {
    if (!is_file) {
      return false;
    }
    if (is_file(candidate)) {
      return true;
    }
    for (const kimix::string &ext : exts) {
      kimix::string with_ext(candidate);
      with_ext += ext;
      if (is_file(with_ext)) {
        return true;
      }
    }
    return false;
  };
  if (rn_has_path_separator(name)) {
    return matches(name) ? kimix::string(name) : kimix::string();
  }
  // shutil.which prepends os.curdir on Windows.
#ifdef KIMIX_PLATFORM_WINDOWS
  // shutil.which searches os.curdir first on Windows.
  {
    const kimix::string curdir = rn_join_path(".", name);
    if (matches(curdir)) {
      return curdir;
    }
  }
#endif
  kimix::string_view rest = path_env;
  const char sep = rn_path_separator();
  while (!rest.empty()) {
    const size_t colon = rest.find(sep);
    const kimix::string_view dir =
        (colon == kimix::string_view::npos) ? rest : rest.substr(0, colon);
    const kimix::string candidate = rn_join_path(dir, name);
    if (matches(candidate)) {
      return candidate;
    }
    if (colon == kimix::string_view::npos) {
      break;
    }
    rest.remove_prefix(colon + 1);
  }
  return {};
}

executable_check check_executable(kimix::string_view executable,
                                  const is_file_probe &is_file,
                                  kimix::string_view path_env,
                                  kimix::string_view python_fallback) {
  executable_check r;
  r.executable = kimix::string(executable);
  // NOTE: keep these as kimix::string - rn_env_or returns a temporary, and
  // binding a string_view to it would dangle.
  const kimix::string pathext = rn_env_or("PATHEXT", ".COM;.EXE;.BAT;.CMD");
  const bool bare_python =
      (executable == "python" || executable == "python.exe");
  if (bare_python && !python_fallback.empty()) {
    const kimix::string on_path = which(executable, path_env, pathext, is_file);
    const bool local_dot =
        is_file ? is_file(kimix::string("./") + kimix::string(executable))
                : false;
    if (on_path.empty() && !local_dot) {
      r.executable = kimix::string(python_fallback);
      r.is_process = true;
      r.is_python_fallback = true;
      return r;
    }
  }
  if (rn_has_path_separator(executable)) {
    r.is_process = is_file ? is_file(executable) : false;
    return r;
  }
  r.is_process = !which(executable, path_env, pathext, is_file).empty();
  return r;
}

// ---------------------------------------------------------------------------
// 3. Environment handling
// ---------------------------------------------------------------------------

env_parse_result parse_env(kimix::string_view text, bool is_string,
                           kimix::span<const kimix::string> list_form,
                           bool posix) {
  env_parse_result r;
  kimix::vector<kimix::string> items;
  if (is_string) {
    kimix::string error;
    if (!rn_shlex_split(text, posix, items, error)) {
      r.error = {tool_status::invalid_input, error};
      return r;
    }
    // Re-join the `A = B` triple into `A=B`.
    kimix::vector<kimix::string> merged;
    size_t i = 0;
    while (i < items.size()) {
      if (i + 2 < items.size() && items[i + 1] == "=" &&
          items[i].find('=') == kimix::string::npos) {
        merged.push_back(items[i] + "=" + items[i + 2]);
        i += 3;
      } else {
        merged.push_back(items[i]);
        i += 1;
      }
    }
    items = std::move(merged);
  } else {
    for (const kimix::string &item : list_form) {
      items.push_back(item);
    }
  }
  for (const kimix::string &item : items) {
    const size_t eq = item.find('=');
    if (eq == kimix::string::npos) {
      r.values.push_back({item, kimix::string("1")});
    } else {
      r.values.push_back({kimix::string(item.substr(0, eq)),
                          kimix::string(item.substr(eq + 1))});
    }
  }
  return r;
}

kimix::vector<kimix::string>
env_to_extra_env(kimix::span<const kimix::builtin_tools::named_value> values) {
  kimix::vector<kimix::string> out;
  out.reserve(values.size());
  for (const kimix::builtin_tools::named_value &nv : values) {
    out.push_back(nv.name + "=" + nv.value);
  }
  return out;
}

// ---------------------------------------------------------------------------
// 4. Shell delegation
// ---------------------------------------------------------------------------

kimix::string cd_prefix(kimix::string_view cwd, kimix::string_view shell) {
  if (cwd.empty()) {
    return {};
  }
  if (shell == "pwsh") {
    kimix::string quoted = "'";
    for (const char c : cwd) {
      if (c == '\'') {
        quoted += "''";
      } else {
        quoted.push_back(c);
      }
    }
    quoted += "'";
    return "cd " + quoted + "; ";
  }
  return "cd " + shlex_quote(cwd) + " && ";
}

// ---------------------------------------------------------------------------
// 5. Safety floors
// ---------------------------------------------------------------------------

kimix::string py_repr_char(uint32_t code_point) {
  return rn_repr_char(code_point);
}

kimix::optional<kimix::string> validate_workdir(kimix::string_view workdir) {
  if (workdir.empty()) {
    return std::nullopt;
  }
  const char *it = workdir.data();
  const char *end = it + workdir.size();
  while (it < end) {
    if (static_cast<unsigned char>(*it) < 0x80u) {
      const char c = *it;
      if (!rn_workdir_allowed(c)) {
        return kimix::string("Invalid workdir: character ") +
               rn_repr_char(
                   static_cast<uint32_t>(static_cast<unsigned char>(c))) +
               " is not allowed.";
      }
      ++it;
      continue;
    }
    const uint32_t cp = decode_code_point(it, end);
    return kimix::string("Invalid workdir: character ") + rn_repr_char(cp) +
           " is not allowed.";
  }
  return std::nullopt;
}

kimix::vector<kimix::string>
normalize_forbidden(kimix::span<const kimix::string> raw) {
  kimix::vector<kimix::string> out;
  for (const kimix::string &cmd : raw) {
    if (cmd.empty()) {
      continue;
    }
    const kimix::string normalized = rn_collapse_whitespace(cmd);
    if (normalized.empty()) {
      continue;
    }
    bool seen = false;
    for (const kimix::string &existing : out) {
      if (existing == normalized) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      out.push_back(normalized);
    }
  }
  return out;
}

kimix::string find_forbidden(kimix::string_view command,
                             kimix::span<const kimix::string> keywords) {
  if (keywords.empty() || command.empty()) {
    return {};
  }
  const kimix::string normalized = rn_collapse_whitespace(command);
  for (const kimix::string &keyword : keywords) {
    if (!keyword.empty() && normalized.find(keyword) != kimix::string::npos) {
      return keyword;
    }
  }
  return {};
}

kimix::string forbidden_message(kimix::string_view full_command) {
  kimix::string out = "Command `";
  out.append(full_command.data(), full_command.size());
  out += "` is forbidden by config rule.";
  return out;
}

kimix::string shell_not_supported_message() {
  // run.py 338: the reference message starts with a space.
  return " This tool does not support shell commands; use the `bash` tool.";
}

// ---------------------------------------------------------------------------
// 6. Parameters
// ---------------------------------------------------------------------------

static const kimix::builtin_tools::param_alias k_run_aliases[] = {
    {"command", "cmd cmdline command_line script"},
    {"mode", "execution_mode run_mode"},
    {"shell", "use_shell via_shell through_shell"},
    {"timeout", "timeout_seconds timeout_sec"},
    {"output_path", "output output_file save_path out_path"},
    {"cwd", "workdir working_dir working_directory dir directory"},
    {"env", "environment env_vars environment_variables envs"},
    {"run_in_background", "background async run_async in_background"},
    {"task_id", "job_id job task"},
    {"wait_for_pattern", "wait_pattern pattern wait_for wait_until"},
    {"max_lines", "max_output_lines output_lines lines"},
};

tool_error parse_params(const ToolParams *params, run_params &out) {
  // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
  // ("command" for "cmd") are accepted; the canonical name always wins.
  const kimix::builtin_tools::ToolParams k_resolved =
      kimix::builtin_tools::ToolParams::with_aliases(params, k_run_aliases);
  if (params != nullptr) {
    params = &k_resolved;
  }
  out = run_params{};
  tool_error err =
      rn_string_param(params, "command", "cmd", false, out.command);
  if (err.failed()) {
    return err;
  }
  kimix::string mode;
  err = rn_string_param(params, "mode", {}, false, mode);
  if (err.failed()) {
    return err;
  }
  if (!mode.empty()) {
    // prompt_common.normalize_mode_validator: run -> execute,
    // background -> send; interactive=True -> interactive (not supported
    // by Run, which uses run_in_background instead).
    if (mode == "run") {
      mode = "execute";
    } else if (mode == "background") {
      mode = "send";
    }
    if (mode != "execute" && mode != "send") {
      return {tool_status::invalid_input,
              kimix::format("Input should be 'execute' or 'send' "
                            "(mode={})",
                            kimix::string_view(mode))};
    }
    out.mode = mode;
  }
  const bool_param_result shell_res = rn_bool_param(params, "shell", false);
  if (shell_res.error.failed()) {
    return shell_res.error;
  }
  out.shell = shell_res.value;
  const bool_param_result bg_res =
      rn_bool_param(params, "run_in_background", false);
  if (bg_res.error.failed()) {
    return bg_res.error;
  }
  out.run_in_background = bg_res.value;

  bool present = false;
  err = rn_int_param(params, "timeout", k_default_timeout_seconds,
                     k_min_timeout_seconds, k_max_timeout_seconds,
                     out.timeout_seconds, present);
  if (err.failed()) {
    return err;
  }
  kimix::string output_path;
  err = rn_string_param(params, "output_path", {}, false, output_path);
  if (err.failed()) {
    return err;
  }
  if (!output_path.empty()) {
    out.output_path = output_path;
  }
  kimix::string cwd;
  err = rn_string_param(params, "cwd", "workdir", false, cwd);
  if (err.failed()) {
    return err;
  }
  if (params != nullptr && params->get("cwd") == nullptr &&
      params->get("workdir") != nullptr) {
    // alias already handled by rn_string_param
  }
  if (!cwd.empty()) {
    out.cwd = cwd;
  } else if (params != nullptr &&
             (params->contains("cwd") || params->contains("workdir"))) {
    // An explicit empty string is still "provided" for validate_workdir.
    out.cwd = cwd;
  }

  // env: string or list.
  if (params != nullptr) {
    const ValueElement *env_el = params->get("env");
    if (env_el != nullptr && !env_el->is_null()) {
      if (env_el->is_string()) {
        out.has_env = true;
        out.env_is_string = true;
        out.env_string = env_el->as_string();
      } else if (env_el->is_array()) {
        out.has_env = true;
        out.env_is_string = false;
        for (const ValueElement &item : env_el->as_array()) {
          if (!item.is_string()) {
            return {tool_status::invalid_input,
                    "env list items must be strings"};
          }
          out.env_list.push_back(item.as_string());
        }
      } else {
        return {tool_status::invalid_input,
                "env must be a string or a list of strings"};
      }
    }
    const ValueElement *task_el = params->get("task_id");
    if (task_el != nullptr && !task_el->is_null()) {
      if (!task_el->is_string()) {
        return {tool_status::invalid_input, "task_id must be a string"};
      }
      out.task_id = task_el->as_string();
    }
    const ValueElement *wait_el = params->get("wait_for_pattern");
    if (wait_el != nullptr && !wait_el->is_null()) {
      if (!wait_el->is_string()) {
        return {tool_status::invalid_input,
                "wait_for_pattern must be a string"};
      }
      out.wait_for_pattern = wait_el->as_string();
    }
    int64_t max_lines = 0;
    err = rn_int_param(params, "max_lines", 0, k_min_max_lines, std::nullopt,
                       max_lines, present);
    if (err.failed()) {
      return err;
    }
    if (present) {
      out.max_lines = max_lines;
    }
  }

  // RunParams._infer_mode: task_id set + default mode -> "send".
  if (out.task_id.has_value() && out.mode == "execute") {
    out.mode = "send";
  }
  // RunParams._validate_cmd.
  if (out.mode == "execute" && out.command.empty()) {
    return {tool_status::invalid_input,
            "command cannot be empty when mode='execute'"};
  }
  if (out.mode == "send") {
    if (out.command.empty()) {
      return {tool_status::invalid_input,
              "command cannot be empty when mode='send'"};
    }
    if (!out.task_id.has_value() || out.task_id->empty()) {
      return {tool_status::invalid_input,
              "mode='send' requires task_id to identify the target "
              "session"};
    }
  }
  if (out.task_id.has_value() && out.mode != "send") {
    return {tool_status::invalid_input, "task_id requires mode='send'"};
  }
  return {tool_status::ok, {}};
}

// ---------------------------------------------------------------------------
// 6b. Output shaping (_token_filter_output's portable stages)
// ---------------------------------------------------------------------------

kimix::string dedup_output(kimix::string_view output, int64_t threshold) {
  if (output.empty()) {
    return {};
  }
  // str.splitlines() over the documented ASCII terminator set: a '\n', a
  // '\r\n' pair or a lone '\r' ends a line, and a trailing terminator does
  // not produce a final empty line ("a\nb\n" -> ["a", "b"]).
  kimix::vector<kimix::string> lines;
  size_t start = 0;
  const size_t n = output.size();
  for (size_t i = 0; i < n; ++i) {
    const char c = output[i];
    if (c == '\n' || c == '\r') {
      lines.emplace_back(output.substr(start, i - start));
      if (c == '\r' && i + 1 < n && output[i + 1] == '\n') {
        ++i;
      }
      start = i + 1;
    }
  }
  if (start < n) {
    lines.emplace_back(output.substr(start, n - start));
  }
  // Counter(lines) - total occurrences anywhere in the output.
  kimix::unordered_map<kimix::string, int64_t, kimix::string_hash> counts;
  for (const kimix::string &line : lines) {
    ++counts[line];
  }
  kimix::unordered_map<kimix::string, bool, kimix::string_hash> emitted;
  kimix::string out;
  bool first = true;
  for (size_t i = 0; i < lines.size(); ++i) {
    const kimix::string &line = lines[i];
    const int64_t cnt = counts[line];
    const bool collapse = cnt > threshold;
    if (collapse && emitted.find(line) != emitted.end()) {
      continue; // already annotated; the duplicate is dropped
    }
    if (!first) {
      out.push_back('\n');
    }
    first = false;
    if (collapse) {
      emitted[line] = true;
      out += line;
      out += kimix::format("  ({} repeats)", cnt);
    } else {
      out += line;
    }
  }
  return out;
}

shaped_output shape_output(kimix::string_view output,
                           const kimix::optional<int64_t> &max_lines,
                           bool token_kill, bool rtk_rewritten) {
  shaped_output r;
  r.text = kimix::string(output);
  const bool apply_dedup = token_kill && !rtk_rewritten;
  if (!apply_dedup && !max_lines.has_value()) {
    return r; // has_filter == false: the reference returns the input as-is
  }
  // Step 1 (rich ANSI strip) and step 2.5 (micro_compress) stay in Python:
  // both need the `rich` ANSI parser / the micro_compress module, which are
  // not vendored here. See the run report for the resulting deviation.
  if (apply_dedup) {
    r.text = dedup_output(r.text);
  }
  if (max_lines.has_value()) {
    // preserve_errors / error_context_lines keep the reference defaults:
    // a diagnostic line that would fall inside the fold is kept with two
    // lines of context, and the fold marker gains the
    // " (N error-context line(s) preserved)" note.
    r.text = bash::truncate_lines(r.text, *max_lines,
                                  /*preserve_errors=*/true,
                                  /*error_context_lines=*/2);
  }
  r.changed = kimix::string_view(r.text.data(), r.text.size()) != output;
  return r;
}

// ---------------------------------------------------------------------------
// 6c. Result messages
// ---------------------------------------------------------------------------

kimix::string success_message(bool success, bool rtk_rewritten,
                              const kimix::optional<kimix::string> &meaning) {
  if (success) {
    return rtk_rewritten ? kimix::string("[rtk] success")
                         : kimix::string("success");
  }
  if (meaning.has_value()) {
    return *meaning;
  }
  return "expected non-zero exit";
}

kimix::string failure_message(bool rtk_rewritten,
                              const kimix::optional<kimix::string> &hint) {
  kimix::string message = rtk_rewritten ? "[rtk] failed" : "failed";
  if (hint.has_value()) {
    message += " Hint: ";
    message += *hint;
  }
  return message;
}

// ---------------------------------------------------------------------------
// 7. Display command
// ---------------------------------------------------------------------------

display_command build_display_command(kimix::string_view executable,
                                      kimix::span<const kimix::string> args,
                                      bool rtk_rewritten) {
  display_command d;
  d.rtk_rewritten = rtk_rewritten;
  const kimix::string shown_executable =
      rtk_rewritten ? kimix::string("rtk") : kimix::string(executable);
  kimix::vector<kimix::string> shown_args;
  shown_args.reserve(args.size());
  for (const kimix::string &arg : args) {
    if (utf8_code_point_count(arg) > 100) {
      const size_t cut = utf8_byte_offset_of_code_point(arg, 100);
      kimix::string clipped(arg.substr(0, cut));
      clipped += "...";
      shown_args.push_back(std::move(clipped));
    } else {
      shown_args.push_back(arg);
    }
  }
  kimix::vector<kimix::string> joined;
  joined.reserve(shown_args.size() + 1);
  joined.push_back(shown_executable);
  for (kimix::string &a : shown_args) {
    joined.push_back(std::move(a));
  }
  const kimix::string cmd_str =
      shlex_join(kimix::span<const kimix::string>(joined));
  d.command =
      (cmd_str.size() > k_huge_cmd_threshold) ? shown_executable : cmd_str;
  return d;
}

// ---------------------------------------------------------------------------
// 8. Shell detection
// ---------------------------------------------------------------------------

kimix::string Run::detect_bash_path() {
  namespace fs = kimix::filesystem;
  const auto is_file = [](kimix::string_view p) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(kimix::string(p)), ec);
  };
#ifdef KIMIX_PLATFORM_WINDOWS
  static const char *kCandidates[] = {
      "C:\\Program Files\\Git\\bin\\bash.exe",
      "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
      "C:\\msys64\\usr\\bin\\bash.exe",
      "C:\\msys32\\usr\\bin\\bash.exe",
      "C:\\cygwin64\\bin\\bash.exe",
  };
  for (const char *c : kCandidates) {
    if (is_file(c)) {
      return kimix::string(c);
    }
  }
#else
  if (is_file("/bin/bash")) {
    return "/bin/bash";
  }
#endif
  const kimix::string path_env = rn_env_or("PATH", "");
  const kimix::string pathext = rn_env_or("PATHEXT", ".COM;.EXE;.BAT;.CMD");
  kimix::string found =
      which("bash", path_env, pathext,
            [](kimix::string_view p) { return rn_default_is_file(p); });
  return found;
}

kimix::string Run::detect_pwsh_path() {
  const kimix::string path_env = rn_env_or("PATH", "");
  const kimix::string pathext = rn_env_or("PATHEXT", ".COM;.EXE;.BAT;.CMD");
  const auto probe = [](kimix::string_view p) { return rn_default_is_file(p); };
  kimix::string found = which("pwsh", path_env, pathext, probe);
  if (!found.empty()) {
    return found;
  }
  return which("powershell", path_env, pathext, probe);
}

// ---------------------------------------------------------------------------
// 9. Tool class
// ---------------------------------------------------------------------------

Run::Run(kimix::builtin_tools::Session *session, run_config cfg)
    : kimix::builtin_tools::Tool(session), _cfg(std::move(cfg)) {}

Run::Run(kimix::builtin_tools::Session *session)
    : kimix::builtin_tools::Tool(session) {}

bool Run::valid() const {
  // Direct process execution has no external dependency: the reproc runner
  // is linked in, and spawning is available on every supported platform.
  return tool_valid("run", true);
}

tool_error Run::prepare(const run_params &params, kimix::string &output_block) {
  output_block.clear();
  // Hardline safety floor (Run._hardline_blocked).
  if (_cfg.hardline_enabled && !params.command.empty() &&
      is_ascii(params.command)) {
    const kimix::runtime::tools::hardline_result hr =
        kimix::runtime::tools::check_hardline_blocked(params.command);
    if (hr.blocked) {
      kimix::string msg = "Blocked (hardline): ";
      if (hr.description.has_value()) {
        msg += *hr.description;
      }
      msg += ". This command cannot be executed via the agent.";
      return {tool_status::blocked, msg};
    }
  }
  // Working-directory floor.
  if (params.cwd.has_value()) {
    const kimix::optional<kimix::string> wd = validate_workdir(*params.cwd);
    if (wd.has_value()) {
      return {tool_status::invalid_input, *wd};
    }
  }
  if (params.mode == "send") {
    if (!params.task_id.has_value() || params.task_id->empty()) {
      return {tool_status::invalid_input,
              "mode='send' requires task_id to identify the target "
              "session."};
    }
    return {tool_status::ok, {}};
  }
  if (params.shell) {
#ifdef KIMIX_PLATFORM_WINDOWS
    const kimix::string shell_command =
        cd_prefix(params.cwd.value_or(kimix::string()), "pwsh") +
        params.command;
#else
    const kimix::string shell_command =
        cd_prefix(params.cwd.value_or(kimix::string()), "bash") +
        params.command;
#endif
    output_block = shell_command;
    return {tool_status::ok, {}};
  }
  // Forbidden commands.
  if (!params.command.empty()) {
    const kimix::string hit =
        find_forbidden(params.command, _cfg.forbidden_keywords);
    if (!hit.empty()) {
      return {tool_status::blocked, forbidden_message(params.command)};
    }
  }
  // Decompose.
  const bool posix =
#ifdef KIMIX_PLATFORM_WINDOWS
      false; // use_posix = sys.platform != "win32"
#else
      true;
#endif
  kimix::vector<kimix::string> parts;
  kimix::string split_error;
  if (!rn_shlex_split(params.command, posix, parts, split_error)) {
    return {tool_status::invalid_input, split_error};
  }
  if (parts.empty()) {
    return {tool_status::invalid_input, "Empty command."};
  }
  const resolved_command resolved = resolve_executable(
      kimix::span<const kimix::string>(parts), posix,
      [](kimix::string_view p) { return rn_default_is_file(p); });
  const kimix::string python_exe = _cfg.python_exe.empty()
                                       ? python::Python::detect_python_exe()
                                       : _cfg.python_exe;
  const executable_check check = check_executable(
      resolved.executable,
      [](kimix::string_view p) { return rn_default_is_file(p); },
      rn_env_or("PATH", ""), python_exe);
  if (!check.is_process) {
    return {tool_status::unsupported, shell_not_supported_message()};
  }
  // RTK rewrite (run.py 369-380): rtk-known commands are re-run through the
  // share-bin `rtk` binary. The gate only fires when the host installed the
  // callback, so the default behaviour is unchanged.
  kimix::string exec_name = check.executable;
  kimix::vector<kimix::string> exec_args = resolved.args;
  const bool rtk_rewritten = rn_maybe_apply_rtk(_cfg, exec_name, exec_args);
  const display_command display = build_display_command(
      exec_name, kimix::span<const kimix::string>(exec_args), rtk_rewritten);
  output_block = display.command;
  return {tool_status::ok, {}};
}

void Run::operator()(const ToolParams *parameters) {
  _result.clear();
  ToolParams result;
  run_params params;
  const tool_error perr = parse_params(parameters, params);
  if (perr.failed()) {
    rn_error_result(result, perr.status, perr.message, "Invalid params");
    rn_serialize(_result, result);
    return;
  }

  const bool native_io =
      (_session != nullptr && _session->native_io && _cfg.native_execute);

  // ---- mode == "send": continue an existing session -------------------
  if (params.mode == "send") {
    const kimix::string task_id = *params.task_id;
    const proc::task_status_info info = proc::query_task(task_id);
    if (!info.exists) {
      kimix::string message =
          kimix::format("Task '{}' not found. No running tasks.", task_id);
      const kimix::vector<proc::task_summary> all = proc::list_tasks();
      if (!all.empty()) {
        kimix::string names;
        for (size_t i = 0; i < all.size(); ++i) {
          if (i != 0) {
            names += ", ";
          }
          names += all[i].task_id;
        }
        message = kimix::format("Task '{}' not found. Available tasks: [{}]",
                                task_id, kimix::string_view(names));
      }
      rn_error_result(result, tool_status::not_found, message,
                      kimix::format("Task '{}' not found", task_id));
      rn_serialize(_result, result);
      return;
    }
    if (!native_io) {
      rn_error_result(result, tool_status::unsupported,
                      "native run execution requires a native_io session",
                      "Unsupported");
      rn_serialize(_result, result);
      return;
    }
    // Discard prior output so only the new output is reported.
    kimix::string discarded;
    proc::read_task(task_id, discarded);
    kimix::string input = params.command;
    if (input.empty() || input.back() != '\n') {
      input += "\n";
    }
    const tool_error serr = proc::send_task(task_id, input, false);
    if (serr.failed()) {
      rn_error_result(
          result, serr.status,
          kimix::format("Failed to send input to task '{}'", task_id),
          "Send input failed");
      rn_serialize(_result, result);
      return;
    }
    const kimix::string pattern =
        params.wait_for_pattern.value_or(kimix::string());
    const proc::task_wait_result tw =
        proc::wait_task(task_id, pattern, params.timeout_seconds * 1000);
    kimix::string output;
    proc::read_task(task_id, output);
    const proc::task_status_info after = proc::query_task(task_id);
    const bool alive = !after.exited;
    python::session_output_block block;
    block.task_id = task_id;
    block.status = alive ? "running" : "completed";
    block.output = output;
    if (!alive && after.exit_code.has_value()) {
      block.exit_code = static_cast<int32_t>(*after.exit_code);
      block.exit_code_meaning =
          bash::interpret_exit_code(params.command, after.exit_code);
      block.failure_hint = kimix::runtime::tools::annotate_failure(
          output, params.command, after.exit_code);
    }
    block.wait_matched = tw.matched ? std::optional<bool>(true) : std::nullopt;
    block.elapsed_seconds = static_cast<double>(tw.elapsed_ms) / 1000.0;
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(
        kimix::format("Data sent to `{}`. Status: {}.", task_id,
                      alive ? "running" : "completed"));
    result.values["output"] =
        ValueElement::make_string(python::build_session_output_block(block));
    result.values["brief"] = ValueElement::make_string(
        kimix::string("Data sent and output retrieved"));
    result.values["mode"] = ValueElement::make_string(kimix::string("send"));
    result.values["task_id"] = ValueElement::make_string(task_id);
    rn_serialize(_result, result);
    return;
  }

  // ---- hardline + workdir + forbidden floors --------------------------
  kimix::string output_block;
  const tool_error prep = prepare(params, output_block);
  if (prep.failed()) {
    const kimix::string brief =
        (prep.status == tool_status::blocked)
            ? kimix::string(prep.message.compare(0, 18, "Blocked (hardline)") ==
                                    0
                                ? "Blocked (hardline)"
                                : "Forbidden command")
            : kimix::string("Invalid command");
    rn_error_result(result, prep.status, prep.message, brief);
    rn_serialize(_result, result);
    return;
  }

  if (!native_io) {
    // Pure-kernel mode: hand the caller the command that would run.
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] =
        ValueElement::make_string(kimix::string("prepared"));
    result.values["output"] = ValueElement::make_string(output_block);
    result.values["command"] = ValueElement::make_string(output_block);
    result.values["brief"] =
        ValueElement::make_string(kimix::string("Command prepared"));
    result.values["mode"] = ValueElement::make_string(params.mode);
    rn_serialize(_result, result);
    return;
  }

  // ---- build argv ----------------------------------------------------
  const bool posix =
#ifdef KIMIX_PLATFORM_WINDOWS
      false;
#else
      true;
#endif
  kimix::vector<kimix::string> argv;
  bool rtk_rewritten = false;
  kimix::string display_text = output_block;
  if (params.shell) {
#ifdef KIMIX_PLATFORM_WINDOWS
    kimix::string pwsh =
        _cfg.pwsh_path.empty() ? detect_pwsh_path() : _cfg.pwsh_path;
    if (pwsh.empty()) {
      rn_error_result(result, tool_status::external_library,
                      "PowerShell is not available on this system.",
                      "PowerShell unavailable");
      rn_serialize(_result, result);
      return;
    }
    argv.push_back(pwsh);
    argv.push_back("-NoProfile");
    argv.push_back("-Command");
    argv.push_back(output_block);
#else
    kimix::string bash =
        _cfg.bash_path.empty() ? detect_bash_path() : _cfg.bash_path;
    if (bash.empty()) {
      rn_error_result(result, tool_status::external_library,
                      "Bash is not available on this system.",
                      "Bash unavailable");
      rn_serialize(_result, result);
      return;
    }
    argv.push_back(bash);
    argv.push_back("-c");
    argv.push_back(output_block);
#endif
    display_text = output_block;
  } else {
    kimix::vector<kimix::string> parts;
    kimix::string split_error;
    rn_shlex_split(params.command, posix, parts, split_error);
    const resolved_command resolved = resolve_executable(
        kimix::span<const kimix::string>(parts), posix,
        [](kimix::string_view p) { return rn_default_is_file(p); });
    const kimix::string python_exe = _cfg.python_exe.empty()
                                         ? python::Python::detect_python_exe()
                                         : _cfg.python_exe;
    const executable_check check = check_executable(
        resolved.executable,
        [](kimix::string_view p) { return rn_default_is_file(p); },
        rn_env_or("PATH", ""), python_exe);
    kimix::string exec_name = check.executable;
    kimix::vector<kimix::string> exec_args = resolved.args;
    rtk_rewritten = rn_maybe_apply_rtk(_cfg, exec_name, exec_args);
    argv.push_back(exec_name);
    for (const kimix::string &arg : exec_args) {
      argv.push_back(arg);
    }
    display_text = build_display_command(
                       exec_name, kimix::span<const kimix::string>(exec_args),
                       rtk_rewritten)
                       .command;
  }

  // ---- environment ---------------------------------------------------
  kimix::vector<kimix::string> extra_env;
  if (params.has_env) {
    const env_parse_result env =
        parse_env(params.env_string, params.env_is_string,
                  kimix::span<const kimix::string>(params.env_list), posix);
    if (env.error.failed()) {
      rn_error_result(result, env.error.status, env.error.message,
                      "Invalid env");
      rn_serialize(_result, result);
      return;
    }
    extra_env = env_to_extra_env(
        kimix::span<const kimix::builtin_tools::named_value>(env.values));
  }

  // ---- working directory --------------------------------------------
  kimix::string workdir;
  if (params.cwd.has_value() && !params.cwd->empty()) {
    workdir = *params.cwd;
    const kimix::filesystem::path wd = kimix::filesystem::path(workdir);
    if (wd.is_relative() && _session != nullptr &&
        !_session->work_dir.empty()) {
      workdir =
          kimix::to_string(kimix::filesystem::path(_session->work_dir) / wd);
    }
  } else if (_session != nullptr) {
    workdir = _session->work_dir;
  }

  proc::run_options opts;
  opts.argv = argv;
  opts.working_directory = workdir;
  opts.extra_env = extra_env;
  opts.timeout_ms = params.timeout_seconds * 1000;
  opts.output_cap_chars = 200000;
  opts.wait_pattern = params.wait_for_pattern.value_or(kimix::string());

  // Task id: run.py uses generate_task_id(session, "run", stem).
  {
    const kimix::filesystem::path exe_path(argv.empty() ? kimix::string()
                                                        : argv[0]);
    kimix::string stem = kimix::to_string(exe_path.stem());
    if (stem.empty()) {
      stem = "run";
    }
    opts.requested_task_id = "run_" + stem;
  }

  // ---- background ----------------------------------------------------
  if (params.run_in_background) {
    proc::task_handle handle;
    const tool_error serr = proc::start_task(opts, handle);
    if (serr.failed()) {
      rn_error_result(result, serr.status, serr.message,
                      "Failed to start background task");
      rn_serialize(_result, result);
      return;
    }
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(
        kimix::format("{}Running in background. task_id: `{}`. Use "
                      "`job_output` tool to retrieve output.",
                      rtk_rewritten ? "[rtk] " : "", handle.task_id));
    result.values["output"] = ValueElement::make_string(kimix::string());
    result.values["brief"] =
        ValueElement::make_string(kimix::string("Background task started"));
    result.values["task_id"] = ValueElement::make_string(handle.task_id);
    result.values["command"] = ValueElement::make_string(display_text);
    rn_serialize(_result, result);
    return;
  }

  // ---- foreground ----------------------------------------------------
  const proc::run_result rr = proc::run_process(opts);
  if (!rr.spawn_error.empty()) {
    rn_error_result(result, tool_status::external_library, rr.spawn_error,
                    "Spawn failed");
    rn_serialize(_result, result);
    return;
  }
  if (rr.still_running) {
    // Early stop with the child still alive: run.py reports it as a
    // background hand-off (ToolError with the guidance message). The runner
    // adopted the child into the task registry, so `rr.task_id` names a
    // real job that job_output can read and stop.
    const kimix::string_view adopted_id = rr.task_id.empty()
                                              ? kimix::string_view("(detached)")
                                              : kimix::string_view(rr.task_id);
    kimix::string message = kimix::format(
        "Running in background. task_id: `{}`. Use `job_output` to read "
        "output or to stop it.",
        adopted_id);
    const kimix::optional<kimix::string> guidance =
        kimix::runtime::tools::foreground_background_guidance(params.command);
    if (guidance.has_value()) {
      message = kimix::format("Running in background. task_id: `{}`. {}",
                              adopted_id, kimix::string_view(*guidance));
    }
    rn_error_result(result, tool_status::blocked, message, "Timeout");
    result.values["output"] = ValueElement::make_string(rr.output);
    if (!rr.task_id.empty()) {
      result.values["task_id"] = ValueElement::make_string(rr.task_id);
    }
    rn_serialize(_result, result);
    return;
  }

  kimix::string output = rr.output;
  if (_cfg.redact_secrets && !output.empty() && is_ascii(output)) {
    if (_cfg.redact_output) {
      output = _cfg.redact_output(output);
    }
  }
  // Token-filter pipeline (common.py _token_filter_output): the ANSI strip
  // and micro_compress stages stay in Python, the dedup + head/tail fold are
  // ported in shape_output(). The reference always runs the post-process
  // pipeline for Run (`token_kill=True`, `rtk_rewritten` only for an rtk
  // rewrite), which also normalizes line endings and drops the trailing
  // newline.
  output =
      shape_output(output, params.max_lines, /*token_kill=*/true, rtk_rewritten)
          .text;

  // Optional export to output_path.
  kimix::optional<kimix::string> output_path;
  bool output_truncated = false;
  if (params.output_path.has_value()) {
    const kimix::filesystem::path out_path(*params.output_path);
    std::error_code ec;
    const kimix::filesystem::path parent = out_path.parent_path();
    if (!parent.empty()) {
      kimix::filesystem::create_directories(parent, ec);
    }
    std::FILE *f = std::fopen(kimix::to_string(out_path).c_str(), "wb");
    if (f == nullptr) {
      rn_error_result(
          result, tool_status::external_library,
          kimix::format("cannot write output_path: {}", *params.output_path),
          "Export failed");
      rn_serialize(_result, result);
      return;
    }
    if (!output.empty()) {
      std::fwrite(output.data(), 1, output.size(), f);
    }
    std::fclose(f);
    kimix::string shown = *params.output_path;
    for (char &ch : shown) {
      if (ch == '\\') {
        ch = '/';
      }
    }
    output_path = shown;
    output = kimix::format("saved to file `{}`", kimix::string_view(shown));
  } else if (output.size() > k_large_output_chars) {
    output_truncated = true;
  }

  const bool success = rr.exit_code.has_value() && *rr.exit_code == 0;
  const kimix::optional<kimix::string> meaning =
      bash::interpret_exit_code(params.command, rr.exit_code);
  // run.py 527: the hint is derived from the *post-process* output (after the
  // dedup/fold stages and the `saved to file` replacement), not the raw
  // capture. annotate_failure only scans the first 4000 characters, so this
  // matters exactly when a filter changed that window.
  const kimix::optional<kimix::string> hint =
      kimix::runtime::tools::annotate_failure(output, params.command,
                                              rr.exit_code);
  const bool expected = bash::is_expected_exit(params.command, rr.exit_code);

  python::session_output_block block;
  block.task_id = opts.requested_task_id;
  block.status = "completed";
  block.output = output;
  if (rr.exit_code.has_value()) {
    block.exit_code = static_cast<int32_t>(*rr.exit_code);
  }
  block.exit_code_meaning = meaning;
  block.failure_hint = hint;
  block.wait_matched = rr.matched ? std::optional<bool>(true) : std::nullopt;
  block.elapsed_seconds = static_cast<double>(rr.elapsed_ms) / 1000.0;
  block.output_path = output_path;
  block.output_truncated = output_truncated;
  const kimix::string rendered = python::build_session_output_block(block);

  if (!success && !expected) {
    const kimix::string message = failure_message(rtk_rewritten, hint);
    result.values["ok"] = ValueElement::make_bool(false);
    result.values["status"] =
        ValueElement::make_string(kimix::string("external_library"));
    result.values["message"] = ValueElement::make_string(message);
    result.values["output"] = ValueElement::make_string(rendered);
    result.values["brief"] =
        ValueElement::make_string(kimix::string("Command execution failed"));
  } else {
    const kimix::string message =
        success_message(success, rtk_rewritten, meaning);
    result.values["ok"] = ValueElement::make_bool(true);
    result.values["status"] = ValueElement::make_string(kimix::string("ok"));
    result.values["message"] = ValueElement::make_string(message);
    result.values["output"] = ValueElement::make_string(rendered);
    result.values["brief"] = ValueElement::make_string(
        kimix::string("Command executed successfully"));
  }
  result.values["command"] = ValueElement::make_string(display_text);
  result.values["mode"] = ValueElement::make_string(params.mode);
  if (rr.exit_code.has_value()) {
    result.values["exit_code"] = ValueElement::make_int(*rr.exit_code);
  }
  rn_serialize(_result, result);
}

// Static registration: the registry key is the lowercase "run" (matching
// the agent-facing tool name in kimix.tools.file.run); "Run" is a declared
// alias.
KIMIX_REGISTER_TOOL_NAMED_ALIASED(
    Run, "run",
    "Run an executable or bash command. For long runs: run_in_background=True, "
    "then task_id=<id> to continue; wait_for_pattern to block; job_output to "
    "monitor.",
    R"JSON({"type":"object","properties":{"command":{"type":"string","description":"Executable command line - real executables only, no shell syntax (pipes, redirects, &&, ||, variables). Example: `python -c \"print(1)\"` or `git status`. Accepts `command` or `cmd`."},"mode":{"type":"string","enum":["execute","send"],"description":"'execute': run as a direct process. 'send': send `command` as stdin to the `task_id` session."},"shell":{"type":"boolean","description":"True: run via the system shell (bash on Linux/macOS, powershell on Windows) with pipes/redirects/variables. False (default): direct process, no shell interpretation."},"timeout":{"type":"integer","description":"Timeout in seconds.","minimum":1,"maximum":900},"output_path":{"type":"string","description":"Output file path."},"cwd":{"type":"string","description":"Working directory. Accepts `cwd` or `workdir`."},"env":{"description":"Environment variables to set for the subprocess.","oneOf":[{"type":"string"},{"type":"array","items":{"type":"string"}}]},"run_in_background":{"type":"boolean","description":"Run the process in the background and return immediately."},"task_id":{"type":"string","description":"Continue existing session. When set, sends 'command' to stdin instead of executing."},"wait_for_pattern":{"type":"string","description":"Pattern to wait for in the tool output."},"max_lines":{"type":"integer","description":"Max lines to return. None = unlimited.","minimum":3}},"required":["command"]})JSON",
    "Run");

} // namespace kimix::builtin_tools::run
