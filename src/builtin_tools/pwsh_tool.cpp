// pwsh_tool.cpp - Self-kill guard kernels (pwsh tool namespace).
//
// Exact port of C:/dev/kimi-agent/src/kimix/tools/file/bash/safety.py
// (self-kill guard, lines 48-70 + 272-806; see pwsh_tool.h for the
// function-by-function map). The helper scanners below replicate the
// `regex` module semantics for the reference patterns: leftmost match,
// finditer resuming after each match end, re.split(..., maxsplit=1) taking
// the earliest separator position, and ASCII `\b` = [A-Za-z0-9_].
//
// ASCII-only contract: non-ASCII input and pkill patterns with regex
// metacharacters are signalled through tool_status::unsupported so the shim
// falls back to the Python mirror; std::regex is never used.

#include "builtin_tools/pwsh_tool.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "builtin_tools/process_runner.h" // proc::run_process / task registry
#include "builtin_tools/python_tool.h"    // session_output_block (shared shape)
#include "builtin_tools/run_tool.h" // which() + is_file_probe (shared PATH walk)
#include "builtin_tools/utf8_util.h" // code-point count + UTF-8 decode

// The PowerShell hosts the availability probe accepts, in preference order.
namespace {
constexpr const char *kPwshExecutable = "pwsh";
constexpr const char *kWindowsPowerShellExecutable = "powershell";
} // namespace

namespace kimix::builtin_tools::pwsh {
namespace {

// ---- ASCII character classes ------------------------------------------------
inline bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
inline bool is_ascii(char c) noexcept {
  return static_cast<unsigned char>(c) < 0x80u;
}
// ASCII \w = [A-Za-z0-9_].
inline bool is_word(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}
inline bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline char lower_ascii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}
inline bool has_alnum(kimix::string_view s) noexcept {
  for (char c : s) {
    if (is_alpha(c) || is_digit(c)) {
      return true;
    }
  }
  return false;
}

bool is_ascii_text(kimix::string_view s) noexcept {
  for (char c : s) {
    if (!is_ascii(c)) {
      return false;
    }
  }
  return true;
}

kimix::string lower_copy(kimix::string_view s) {
  kimix::string out(s.data(), s.size());
  for (char &c : out) {
    c = lower_ascii(c);
  }
  return out;
}

// Strip whitespace, then strip any combination of leading/trailing quotes
// (Python str.strip(chars)).
kimix::string_view strip_quotes(kimix::string_view s) noexcept {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && is_space(s[b])) {
    ++b;
  }
  while (e > b && is_space(s[e - 1])) {
    --e;
  }
  while (b < e && (s[b] == '"' || s[b] == '\'')) {
    ++b;
  }
  while (e > b && (s[e - 1] == '"' || s[e - 1] == '\'')) {
    --e;
  }
  return s.substr(b, e - b);
}

// ---- _segment_text / _segment_tokens (safety.py 438-440, 73-81) ------------
//
// re.split(r";|\|\||&&|\||\n", tail, maxsplit=1)[0]: the split index is the
// earliest position of ';', "||", "&&", '|' or '\n' (the "||" alternative
// implies a '|' at the same position; match length is irrelevant because
// only the pre-split text is used). A single '&' does NOT split.
size_t segment_split_pos(kimix::string_view text, size_t start) noexcept {
  for (size_t i = start; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ';' || c == '|' || c == '\n') {
      return i;
    }
    if (c == '&' && i + 1 < text.size() && text[i + 1] == '&') {
      return i;
    }
  }
  return text.size();
}

kimix::string_view segment_text(kimix::string_view text,
                                size_t start) noexcept {
  return text.substr(start, segment_split_pos(text, start) - start);
}

// Whitespace-separated token views of text[start : split_pos).
kimix::vector<kimix::string_view> segment_tokens(kimix::string_view text,
                                                 size_t start) {
  kimix::vector<kimix::string_view> toks;
  const size_t e = segment_split_pos(text, start);
  size_t i = start;
  while (i < e) {
    while (i < e && is_space(text[i])) {
      ++i;
    }
    if (i >= e) {
      break;
    }
    size_t j = i;
    while (j < e && !is_space(text[j])) {
      ++j;
    }
    toks.push_back(text.substr(i, j - i));
    i = j;
  }
  return toks;
}

// ---- _looks_like_flag (safety.py 84-91) -------------------------------------
bool looks_like_flag(kimix::string_view token) noexcept {
  if (token.size() <= 1) {
    return false;
  }
  if (token[0] == '-') {
    return true;
  }
  if (token[0] == '/') {
    for (size_t i = 1; i < token.size(); ++i) {
      if (!is_alpha(token[i])) {
        return false;
      }
    }
    return true;
  }
  return false;
}

// ---- word scanner
// ------------------------------------------------------------ Leftmost `word`
// occurrences in `text` starting at `from`, with ASCII word boundaries on both
// sides (re.finditer(r"\b<word>\b", text)). `text` is already lowercased;
// `word` must be lowercase.
struct word_match {
  size_t begin = 0;
  size_t end = 0;
};

kimix::vector<word_match> find_word(kimix::string_view text,
                                    kimix::string_view word, size_t from = 0) {
  kimix::vector<word_match> out;
  size_t pos = from;
  while (pos + word.size() <= text.size()) {
    pos = text.find(word, pos);
    if (pos == kimix::string_view::npos) {
      break;
    }
    const bool lb = pos == 0 || !is_word(text[pos - 1]);
    const size_t end = pos + word.size();
    const bool rb = end == text.size() || !is_word(text[end]);
    if (lb && rb) {
      out.push_back({pos, end});
      pos = end == pos ? end + 1 : end; // finditer resumes at match end
    } else {
      pos += 1;
    }
  }
  return out;
}

// find_word for the `\b<word>(?:\.exe)?\b` family: the greedy `(?:\.exe)?`
// suffix is consumed when ".exe" follows and a word boundary holds after it;
// otherwise the plain word match stands (regex backtracking on \b).
kimix::vector<word_match> find_word_opt_exe(kimix::string_view text,
                                            kimix::string_view word) {
  kimix::vector<word_match> matches = find_word(text, word);
  for (word_match &m : matches) {
    if (m.end + 4 <= text.size() && text.substr(m.end, 4) == ".exe" &&
        (m.end + 4 == text.size() || !is_word(text[m.end + 4]))) {
      m.end += 4;
    }
  }
  return matches;
}

// ---- collapse + lowercase (safety.py 629: " ".join(command.split()).lower())
// -
kimix::string collapse_lower(kimix::string_view s) {
  kimix::string out;
  out.reserve(s.size());
  bool first = true;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && is_space(s[i])) {
      ++i;
    }
    if (i >= s.size()) {
      break;
    }
    if (!first) {
      out.push_back(' ');
    }
    first = false;
    while (i < s.size() && !is_space(s[i])) {
      out.push_back(lower_ascii(s[i]));
      ++i;
    }
  }
  return out;
}

// ---- _numeric_pid_targets (safety.py 443-460)
// -------------------------------- Non-flag tokens; comma-split; strip
// whitespace+quotes+parens; plain ASCII digits; PowerShell expression style
// ^(\d+)[).]. Python's int() is unbounded, so an ASCII digit run longer than 18
// digits can never equal an int64_t protected PID and is skipped (faithful:
// Python finds no hit either).
kimix::vector<int64_t>
numeric_pid_targets(kimix::span<const kimix::string_view> tokens) {
  kimix::vector<int64_t> pids;
  for (kimix::string_view token : tokens) {
    if (looks_like_flag(token)) {
      continue;
    }
    size_t begin = 0;
    while (true) {
      size_t comma = token.find(',', begin);
      if (comma == kimix::string_view::npos) {
        comma = token.size();
      }
      kimix::string_view part = token.substr(begin, comma - begin);
      begin = comma + 1;
      // part.strip().strip("\"'()")
      size_t b = 0;
      size_t e = part.size();
      while (b < e && is_space(part[b])) {
        ++b;
      }
      while (e > b && is_space(part[e - 1])) {
        --e;
      }
      while (b < e && (part[b] == '"' || part[b] == '\'' || part[b] == '(' ||
                       part[b] == ')')) {
        ++b;
      }
      while (e > b && (part[e - 1] == '"' || part[e - 1] == '\'' ||
                       part[e - 1] == '(' || part[e - 1] == ')')) {
        --e;
      }
      part = part.substr(b, e - b);
      if (part.empty()) {
        if (comma >= token.size()) {
          break;
        }
        continue;
      }
      size_t digits = 0;
      while (digits < part.size() && is_digit(part[digits])) {
        ++digits;
      }
      if (digits == part.size()) {
        if (digits <= 18) {
          int64_t value = 0;
          for (size_t k = 0; k < digits; ++k) {
            value = value * 10 + (part[k] - '0');
          }
          pids.push_back(value);
        }
        // >18 digits: Python int() keeps it, but it can never equal
        // an int64_t protected PID — skipped.
      } else if (digits > 0 && digits < part.size() &&
                 (part[digits] == ')' || part[digits] == '.')) {
        // ^(\d+)[).] — leading digits followed by ')' or '.'
        if (digits <= 18) {
          int64_t value = 0;
          for (size_t k = 0; k < digits; ++k) {
            value = value * 10 + (part[k] - '0');
          }
          pids.push_back(value);
        }
      }
      if (comma >= token.size()) {
        break;
      }
    }
  }
  return pids;
}

// ---- _loop_pid_sources (safety.py 469-505)
// ----------------------------------- Loop headers that bind a variable to a
// literal PID list. Variable names are lowercased; entries containing $ * ? [ `
// ~ are skipped (unresolvable); only plain-digit parts are kept (deduped per
// variable). Insertion order is preserved because the hit descriptions
// enumerate the bound PIDs.
//
// bash:      \bfor\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([^;]+)   (list ends at
// ';') PowerShell:\bforeach\s*\(\s*\$([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([^)]+)\)
using loop_sources =
    kimix::vector<std::pair<kimix::string, kimix::vector<int64_t>>>;

kimix::vector<int64_t> &loop_source_get(loop_sources &sources,
                                        kimix::string_view var) {
  for (auto &entry : sources) {
    if (entry.first == var) {
      return entry.second;
    }
  }
  sources.emplace_back(kimix::string(var.data(), var.size()),
                       kimix::vector<int64_t>{});
  return sources.back().second;
}

const kimix::vector<int64_t> *loop_source_find(const loop_sources &sources,
                                               kimix::string_view var) {
  for (const auto &entry : sources) {
    if (entry.first == var) {
      return &entry.second;
    }
  }
  return nullptr;
}

void loop_source_add_pids(loop_sources &sources, kimix::string_view var,
                          kimix::string_view list) {
  auto &pids = loop_source_get(sources, var);
  // Whitespace-tokenize the list, then comma-split each token.
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && is_space(list[i])) {
      ++i;
    }
    if (i >= list.size()) {
      break;
    }
    size_t j = i;
    while (j < list.size() && !is_space(list[j])) {
      ++j;
    }
    const kimix::string_view token = list.substr(i, j - i);
    i = j;
    bool unresolvable = false;
    for (char c : token) {
      if (c == '$' || c == '*' || c == '?' || c == '[' || c == '`' ||
          c == '~') {
        unresolvable = true;
        break;
      }
    }
    if (unresolvable) {
      continue;
    }
    size_t begin = 0;
    while (true) {
      size_t comma = token.find(',', begin);
      if (comma == kimix::string_view::npos) {
        comma = token.size();
      }
      kimix::string_view part = token.substr(begin, comma - begin);
      begin = comma + 1;
      part = strip_quotes(part);
      bool all_digits = !part.empty();
      int64_t value = 0;
      for (char c : part) {
        if (!is_digit(c)) {
          all_digits = false;
          break;
        }
        value = value * 10 + (c - '0');
      }
      if (all_digits) {
        bool seen = false;
        for (int64_t pid : pids) {
          if (pid == value) {
            seen = true;
            break;
          }
        }
        if (!seen) {
          pids.push_back(value);
        }
      }
      if (comma >= token.size()) {
        break;
      }
    }
  }
}

loop_sources build_loop_pid_sources(kimix::string_view text) {
  loop_sources sources;
  // bash/POSIX: for <var> in <list up to ';'>
  for (const word_match &m : find_word(text, "for")) {
    size_t i = m.end;
    while (i < text.size() && is_space(text[i])) {
      ++i;
    }
    size_t vb = i;
    while (i < text.size() &&
           ((text[i] >= 'a' && text[i] <= 'z') ||
            (text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_')) {
      ++i;
    }
    if (i == vb) {
      continue;
    }
    size_t ve = i;
    while (i < text.size() &&
           ((text[i] >= 'a' && text[i] <= 'z') ||
            (text[i] >= 'A' && text[i] <= 'Z') ||
            (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) {
      ++i;
    }
    size_t k = i;
    while (k < text.size() && is_space(text[k])) {
      ++k;
    }
    if (k + 2 > text.size() || text[k] != 'i' || text[k + 1] != 'n' ||
        (k + 2 < text.size() && is_word(text[k + 2]))) {
      continue;
    }
    k += 2;
    while (k < text.size() && is_space(text[k])) {
      ++k;
    }
    size_t le = k;
    while (le < text.size() && text[le] != ';') {
      ++le;
    }
    // `text` is already lowercased; keep a stable copy (never bind a
    // string_view to the temporary returned by lower_copy).
    const kimix::string var = lower_copy(text.substr(vb, ve - vb));
    loop_source_add_pids(sources, var, text.substr(k, le - k));
  }
  // PowerShell: foreach ($<var> in <list up to ')'>)
  for (const word_match &m : find_word(text, "foreach")) {
    size_t i = m.end;
    while (i < text.size() && is_space(text[i])) {
      ++i;
    }
    if (i >= text.size() || text[i] != '(') {
      continue;
    }
    ++i;
    while (i < text.size() && is_space(text[i])) {
      ++i;
    }
    if (i >= text.size() || text[i] != '$') {
      continue;
    }
    ++i;
    size_t vb = i;
    while (i < text.size() &&
           ((text[i] >= 'a' && text[i] <= 'z') ||
            (text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_')) {
      ++i;
    }
    if (i == vb) {
      continue;
    }
    size_t ve = i;
    while (i < text.size() &&
           ((text[i] >= 'a' && text[i] <= 'z') ||
            (text[i] >= 'A' && text[i] <= 'Z') ||
            (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) {
      ++i;
    }
    size_t k = i;
    while (k < text.size() && is_space(text[k])) {
      ++k;
    }
    if (k + 2 > text.size() || text[k] != 'i' || text[k + 1] != 'n' ||
        (k + 2 < text.size() && is_word(text[k + 2]))) {
      continue;
    }
    k += 2;
    while (k < text.size() && is_space(text[k])) {
      ++k;
    }
    size_t le = k;
    while (le < text.size() && text[le] != ')') {
      ++le;
    }
    if (le >= text.size()) {
      continue;
    }
    const kimix::string var = lower_copy(text.substr(vb, ve - vb));
    loop_source_add_pids(sources, var, text.substr(k, le - k));
  }
  return sources;
}

// ---- _split_image_name (safety.py 388-398)
// -----------------------------------
struct image_name_parts {
  kimix::string base;
  kimix::string stem;
};

bool executable_suffix(kimix::string_view ext) noexcept {
  return ext == "exe" || ext == "com" || ext == "bat" || ext == "cmd" ||
         ext == "py" || ext == "sh";
}

image_name_parts split_image_name(kimix::string_view name) {
  // name.strip().strip("\"'")
  kimix::string_view s = strip_quotes(name);
  // basename: last component after '/' or '\'
  size_t slash = s.size();
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '/' || s[i] == '\\') {
      slash = i;
    }
  }
  image_name_parts parts;
  parts.base = lower_copy(slash < s.size() ? s.substr(slash + 1) : s);
  // rpartition("."): stem drops a known executable suffix only
  size_t dot = parts.base.rfind('.');
  if (dot != kimix::string::npos && dot > 0 && dot + 1 < parts.base.size() &&
      executable_suffix(kimix::string_view(parts.base).substr(dot + 1))) {
    parts.stem = parts.base.substr(0, dot);
  } else {
    parts.stem = parts.base;
  }
  return parts;
}

// ---- _name_kill_hit (safety.py 541-561)
// --------------------------------------- Returns the matched agent image name,
// or empty. `names` is the shim-provided set iterated in Python's unspecified
// set order; the kernel uses ascending order (documented deviation) so the
// result is deterministic.
kimix::string name_kill_hit(kimix::string_view token,
                            const kimix::vector<kimix::string> &names) {
  const image_name_parts parts = split_image_name(token);
  if (parts.base.empty() || !has_alnum(parts.base)) {
    return {};
  }
  if (!parts.base.empty() && parts.base.back() == '*') {
    const kimix::string_view prefix(parts.base.data(), parts.base.size() - 1);
    if (prefix.size() < 3) {
      return {};
    }
    for (const kimix::string &name : names) {
      if (name.size() >= prefix.size() &&
          kimix::string_view(name).substr(0, prefix.size()) == prefix) {
        return name;
      }
    }
    return {};
  }
  for (const kimix::string &name : names) {
    if (parts.base == name || parts.stem == name) {
      return name;
    }
  }
  return {};
}

// ---- _pattern_kill_hit, plain-substring subset (safety.py 564-584)
// ------------ Python: re.search(pattern, haystack, re.IGNORECASE) with
// substring fallback. The kernel implements only the plain case-insensitive
// substring subset; the metachar gate (below) routes everything else to Python.
kimix::string_view
pattern_kill_hit(kimix::string_view pattern,
                 kimix::span<const kimix::string> haystacks) {
  const kimix::string_view p = strip_quotes(pattern);
  if (p.empty() || !has_alnum(p)) {
    return {};
  }
  for (const kimix::string &haystack : haystacks) {
    if (haystack.empty()) {
      continue;
    }
    // case-insensitive substring search over ASCII text
    if (haystack.size() >= p.size()) {
      for (size_t i = 0; i + p.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < p.size(); ++k) {
          if (lower_ascii(haystack[i + k]) != lower_ascii(p[k])) {
            ok = false;
            break;
          }
        }
        if (ok) {
          return haystack;
        }
      }
    }
  }
  return {};
}

bool is_regex_metachar(char c) noexcept {
  switch (c) {
  case '[':
  case ']':
  case '(':
  case ')':
  case '{':
  case '}':
  case '+':
  case '?':
  case '|':
  case '^':
  case '$':
  case '\\':
  case '.':
    return true;
  default:
    return false;
  }
}

// ---- _pkill_full_match (safety.py 587-595)
// ------------------------------------
bool pkill_full_match(kimix::span<const kimix::string_view> tokens) noexcept {
  for (kimix::string_view token : tokens) {
    if (token == "--full") {
      return true;
    }
    if (token.size() >= 2 && token[0] == '-' && token[1] != '-') {
      for (size_t i = 1; i < token.size(); ++i) {
        if (token[i] == 'f') {
          return true;
        }
      }
    }
  }
  return false;
}

// ---- gate: pkill patterns with regex metacharacters
// ---------------------------- Any non-flag pkill pattern token containing a
// regex metacharacter routes the whole call to the Python mirror (plan §8): the
// kernel's substring subset would diverge from re.search on such patterns. Uses
// the same word scanner (with the optional ".exe" suffix) as detector 4 so the
// token stream agrees.
bool pkill_needs_python(kimix::string_view text) {
  for (const word_match &m : find_word_opt_exe(text, "pkill")) {
    for (kimix::string_view token : segment_tokens(text, m.end)) {
      if (looks_like_flag(token)) {
        continue;
      }
      const kimix::string_view p = strip_quotes(token);
      for (char c : p) {
        if (is_regex_metachar(c)) {
          return true;
        }
      }
    }
  }
  return false;
}

// ---- description builders (byte-exact ports of the Python f-strings)
// ----------
kimix::string pid_hit_description(int64_t pid, kimix::string_view via) {
  kimix::string out = "targets PID ";
  out += kimix::format("{}", pid);
  out += " via ";
  out += via;
  out += ", which is the agent process or one of its parent processes";
  return out;
}

kimix::string loop_pid_list(kimix::span<const int64_t> pids) {
  kimix::string out;
  for (size_t i = 0; i < pids.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += kimix::format("{}", pids[i]);
  }
  return out;
}

} // namespace

const char *const k_self_kill_guidance =
    "If you meant to stop a different process, re-check its PID first "
    "(`tasklist` / `Get-Process` / `ps aux`) and retry with a PID that does "
    "not belong to the agent. If the target merely shares the agent's image "
    "name, terminate that specific PID instead of a name/pattern match. If "
    "you really intend to stop or restart the agent itself, ask the user to "
    "do it from outside this session.";

void command_detection_variants(kimix::string_view command,
                                kimix::vector<kimix::string> &out) {
  out.clear();
  if (command.empty()) {
    return;
  }
  bool only_space = true;
  for (char c : command) {
    if (!is_space(c)) {
      only_space = false;
      break;
    }
  }
  if (only_space) {
    return;
  }
  // collapsed = " ".join(command.split()) — whitespace collapse, case kept.
  kimix::string collapsed;
  collapsed.reserve(command.size());
  {
    bool first = true;
    size_t i = 0;
    while (i < command.size()) {
      while (i < command.size() && is_space(command[i])) {
        ++i;
      }
      if (i >= command.size()) {
        break;
      }
      if (!first) {
        collapsed.push_back(' ');
      }
      first = false;
      while (i < command.size() && !is_space(command[i])) {
        collapsed.push_back(command[i]);
        ++i;
      }
    }
  }
  // deobfuscated = re.sub(r"[\\'\"]", "", collapsed).lower()
  kimix::string deobfuscated;
  deobfuscated.reserve(collapsed.size());
  for (char c : collapsed) {
    if (c != '\\' && c != '\'' && c != '"') {
      deobfuscated.push_back(lower_ascii(c));
    }
  }
  // lowered = collapsed.lower()
  kimix::string lowered = lower_copy(collapsed);
  auto push_unique = [&](const kimix::string &v) {
    if (v.empty()) {
      return;
    }
    for (const kimix::string &existing : out) {
      if (existing == v) {
        return;
      }
    }
    out.push_back(v);
  };
  push_unique(collapsed);
  push_unique(deobfuscated);
  push_unique(lowered);
}

namespace {

// Ordered-detector state shared between the optional<> and struct APIs.
struct detect_state {
  kimix::string_view text;
  const kimix::unordered_set<int64_t> *protected_pids = nullptr;
  kimix::vector<kimix::string> names; // lowercased + sorted (deterministic)
  kimix::string_view cmdline;
  loop_sources loops;
  tool_status status = tool_status::ok;
};

bool pid_protected(const detect_state &st, int64_t pid) {
  return st.protected_pids->find(pid) != st.protected_pids->end();
}

// _pid_hit over numeric targets of `tokens`.
kimix::optional<kimix::string>
pid_hit(detect_state &st, kimix::span<const kimix::string_view> tokens,
        kimix::string_view via) {
  const kimix::vector<int64_t> pids = numeric_pid_targets(tokens);
  for (int64_t pid : pids) {
    if (pid_protected(st, pid)) {
      return pid_hit_description(pid, via);
    }
  }
  return {};
}

// _variable_pid_hit (safety.py 508-538): `$var` / `${var}` tokens resolved
// through the loop sources.
kimix::optional<kimix::string>
variable_pid_hit(detect_state &st, kimix::span<const kimix::string_view> tokens,
                 kimix::string_view via) {
  for (kimix::string_view token : tokens) {
    const kimix::string_view stripped = strip_quotes(token);
    // fullmatch \$\{?([A-Za-z_][A-Za-z0-9_]*)\}?
    if (stripped.empty() || stripped[0] != '$') {
      continue;
    }
    size_t i = 1;
    bool open_brace = false;
    if (i < stripped.size() && stripped[i] == '{') {
      open_brace = true;
      ++i;
    }
    const size_t vb = i;
    while (i < stripped.size() &&
           ((stripped[i] >= 'a' && stripped[i] <= 'z') ||
            (stripped[i] >= 'A' && stripped[i] <= 'Z') || stripped[i] == '_')) {
      ++i;
    }
    if (i == vb) {
      continue;
    }
    size_t ve = i;
    while (i < stripped.size() &&
           ((stripped[i] >= 'a' && stripped[i] <= 'z') ||
            (stripped[i] >= 'A' && stripped[i] <= 'Z') ||
            (stripped[i] >= '0' && stripped[i] <= '9') || stripped[i] == '_')) {
      ++i;
    }
    // \}? is optional and independent of the leading brace —
    // ``${pid``, ``${pid}`` and ``$pid}`` all fullmatch.
    if (i < stripped.size() && stripped[i] == '}') {
      ++i;
    }
    if (i != stripped.size()) {
      continue;
    }
    const kimix::string var = lower_copy(stripped.substr(vb, ve - vb));
    const kimix::vector<int64_t> *pids = loop_source_find(st.loops, var);
    if (pids == nullptr || pids->empty()) {
      continue;
    }
    for (int64_t pid : *pids) {
      if (pid_protected(st, pid)) {
        const kimix::string_view var_original = stripped.substr(vb, ve - vb);
        kimix::string out = "kills PID ";
        out += kimix::format("{}", pid);
        out += " via `";
        out += via;
        out += "` through loop variable `$";
        out += var_original;
        out += "` (bound to PIDs ";
        out += loop_pid_list(
            kimix::span<const int64_t>(pids->data(), pids->size()));
        out += "), which is the agent process or one of its parent processes";
        return out;
      }
    }
  }
  return {};
}

} // namespace

self_kill_result detect_self_kill_ex(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline) {
  self_kill_result result;
  result.status = tool_status::ok;
  result.hit = false;

  // ASCII gate (plan §8): Python's str methods and \b are Unicode-aware.
  if (!is_ascii_text(command) || !is_ascii_text(cmdline)) {
    for (const kimix::string &name : image_names) {
      if (!is_ascii_text(name)) {
        result.status = tool_status::unsupported;
        return result;
      }
    }
    result.status = tool_status::unsupported;
    return result;
  }

  // safety.py 618-619: empty / whitespace-only command
  bool only_space = true;
  for (char c : command) {
    if (!is_space(c)) {
      only_space = false;
      break;
    }
  }
  if (command.empty() || only_space) {
    return result;
  }
  // safety.py 626-627: no protected pids -> None
  if (protected_pids.empty()) {
    return result;
  }

  detect_state st;
  st.protected_pids = &protected_pids;
  st.cmdline = cmdline;
  // image_names lowercased + sorted (Python: {n.lower() ...} set order is
  // unspecified; ascending order keeps wildcard first-match deterministic).
  {
    kimix::vector<kimix::string> names;
    names.reserve(image_names.size());
    for (const kimix::string &name : image_names) {
      if (!name.empty()) {
        names.push_back(lower_copy(name));
      }
    }
    std::sort(names.begin(), names.end());
    st.names = std::move(names);
  }

  const kimix::string text = collapse_lower(command);
  st.text = text;
  st.loops = build_loop_pid_sources(text);

  // Metachar gate: pkill patterns are full regexes in Python.
  if (pkill_needs_python(text)) {
    result.status = tool_status::unsupported;
    return result;
  }

  auto finish = [&](kimix::optional<kimix::string> desc,
                    const char *rule) -> bool {
    if (desc.has_value()) {
      result.hit = true;
      result.description = std::move(desc);
      result.rule_id = rule;
      return true;
    }
    return false;
  };

  // 1. POSIX kill / Windows tskill (safety.py 644-657).
  static const char *k_kill_skip[] = {"docker", "podman", "kubectl", "compose"};
  {
    kimix::vector<word_match> matches = find_word_opt_exe(text, "kill");
    for (const word_match &m : find_word_opt_exe(text, "tskill")) {
      matches.push_back(m);
    }
    std::sort(matches.begin(), matches.end(),
              [](const word_match &a, const word_match &b) {
                return a.begin < b.begin;
              });
    // finditer resumes after each match end; overlapping alternation
    // starts inside a consumed span are dropped (kill.exe starts inside
    // the "kill" match span only when kill matched plain - it does not,
    // because \b fails, so kill.exe matches are kept).
    for (const word_match &m : matches) {
      // `text` is a kimix::string; take subviews of its buffer directly
      // (text.substr would bind a view to a temporary -> dangling).
      const kimix::string_view word(text.data() + m.begin, m.end - m.begin);
      if (word.size() >= 4 && word.substr(0, 4) == "kill") {
        // skip docker/podman/kubectl/compose kill: the word before
        // the match (after trailing whitespace) is one of the skip
        // words. Work on sizes so the const view needs no mutation.
        const kimix::string_view before(text.data(), m.begin);
        size_t be = before.size();
        while (be > 0 && is_space(before[be - 1])) {
          --be;
        }
        const kimix::string_view trimmed = before.substr(0, be);
        size_t wb = trimmed.size();
        while (wb > 0 && is_word(trimmed[wb - 1])) {
          --wb;
        }
        const kimix::string_view prev = trimmed.substr(wb);
        bool skip = false;
        for (const char *s : k_kill_skip) {
          if (prev == s) {
            skip = true;
            break;
          }
        }
        if (skip) {
          continue;
        }
      }
      const kimix::vector<kimix::string_view> tokens =
          segment_tokens(text, m.end);
      const kimix::string via =
          kimix::string("`") + kimix::string(word.data(), word.size()) + "`";
      if (finish(pid_hit(st, tokens, via), "kill")) {
        return result;
      }
      if (st.status != tool_status::ok) {
        result.status = st.status;
        return result;
      }
      if (finish(variable_pid_hit(st, tokens, via), "kill")) {
        return result;
      }
    }
  }

  // 2. taskkill (safety.py 660-675).
  {
    kimix::vector<word_match> matches = find_word_opt_exe(text, "taskkill");
    std::sort(matches.begin(), matches.end(),
              [](const word_match &a, const word_match &b) {
                return a.begin < b.begin;
              });
    for (const word_match &m : matches) {
      const kimix::vector<kimix::string_view> tokens =
          segment_tokens(segment_text(text, m.end), 0);
      if (finish(pid_hit(st, tokens, "`taskkill`"), "taskkill")) {
        return result;
      }
      if (st.status != tool_status::ok) {
        result.status = st.status;
        return result;
      }
      if (finish(variable_pid_hit(st, tokens, "`taskkill`"), "taskkill")) {
        return result;
      }
      for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "/im") {
          const kimix::string name_hit = name_kill_hit(tokens[i + 1], st.names);
          if (!name_hit.empty()) {
            kimix::string desc = "kills by image name `";
            desc += name_hit;
            desc +=
                "` via `taskkill /IM`, which also matches the agent process";
            if (finish(kimix::optional<kimix::string>(std::move(desc)),
                       "taskkill")) {
              return result;
            }
          }
        }
      }
    }
  }

  // 3. Stop-Process -Id / -Name, Get-Process piped into a kill (safety.py
  // 677-714).
  {
    for (const word_match &m : find_word(text, "stop-process")) {
      const kimix::vector<kimix::string_view> tokens =
          segment_tokens(text, m.end);
      if (finish(pid_hit(st, tokens, "`Stop-Process`"), "stop-process")) {
        return result;
      }
      if (st.status != tool_status::ok) {
        result.status = st.status;
        return result;
      }
      if (finish(variable_pid_hit(st, tokens, "`Stop-Process`"),
                 "stop-process")) {
        return result;
      }
      for (kimix::string_view token : tokens) {
        if (looks_like_flag(token)) {
          continue;
        }
        const kimix::string name_hit = name_kill_hit(token, st.names);
        if (!name_hit.empty()) {
          kimix::string desc = "kills by process name `";
          desc += name_hit;
          desc += "` via `Stop-Process`, which also matches the agent process";
          if (finish(kimix::optional<kimix::string>(std::move(desc)),
                     "stop-process")) {
            return result;
          }
        }
      }
    }
  }
  if (text.find("stop-process") != kimix::string_view::npos ||
      text.find("| kill") != kimix::string_view::npos ||
      text.find(".kill()") != kimix::string_view::npos) {
    for (const word_match &m : find_word(text, "get-process")) {
      const kimix::vector<kimix::string_view> tokens =
          segment_tokens(text, m.end);
      if (finish(pid_hit(st, tokens, "`Get-Process` piped to a kill"),
                 "get-process")) {
        return result;
      }
      if (st.status != tool_status::ok) {
        result.status = st.status;
        return result;
      }
      if (finish(variable_pid_hit(st, tokens, "`Get-Process` piped to a kill"),
                 "get-process")) {
        return result;
      }
      for (kimix::string_view token : tokens) {
        if (looks_like_flag(token)) {
          continue;
        }
        const kimix::string name_hit = name_kill_hit(token, st.names);
        if (!name_hit.empty()) {
          kimix::string desc = "kills by process name `";
          desc += name_hit;
          desc += "` via `Get-Process` piped to a kill, which also matches the "
                  "agent process";
          if (finish(kimix::optional<kimix::string>(std::move(desc)),
                     "get-process")) {
            return result;
          }
        }
      }
    }
  }

  // 4. pkill / killall (safety.py 716-742).
  {
    struct tagged {
      word_match match;
      bool is_pkill;
    };
    kimix::vector<tagged> matches;
    for (const word_match &m : find_word_opt_exe(text, "pkill")) {
      matches.push_back({m, true});
    }
    for (const word_match &m : find_word_opt_exe(text, "killall")) {
      matches.push_back({m, false});
    }
    std::sort(matches.begin(), matches.end(),
              [](const tagged &a, const tagged &b) {
                return a.match.begin < b.match.begin;
              });
    for (const tagged &t : matches) {
      const kimix::vector<kimix::string_view> tokens =
          segment_tokens(text, t.match.end);
      const bool full =
          t.is_pkill && pkill_full_match(kimix::span<const kimix::string_view>(
                            tokens.data(), tokens.size()));
      for (kimix::string_view token : tokens) {
        if (looks_like_flag(token)) {
          continue;
        }
        if (t.is_pkill) {
          kimix::vector<kimix::string> haystacks = st.names; // sorted
          if (full && !st.cmdline.empty()) {
            haystacks.push_back(
                kimix::string(st.cmdline.data(), st.cmdline.size()));
          }
          const kimix::string_view hit =
              pattern_kill_hit(token, kimix::span<const kimix::string>(
                                          haystacks.data(), haystacks.size()));
          if (!hit.empty()) {
            const kimix::string_view display = strip_quotes(token);
            kimix::string desc = "kills processes matching `";
            desc += display;
            desc += "` via `pkill";
            if (full) {
              desc += " -f";
            }
            desc += "`, which also matches the agent process";
            if (finish(kimix::optional<kimix::string>(std::move(desc)),
                       "pkill")) {
              return result;
            }
          }
        } else {
          const kimix::string name_hit = name_kill_hit(token, st.names);
          if (!name_hit.empty()) {
            kimix::string desc = "kills by process name `";
            desc += name_hit;
            desc += "` via `killall`, which also matches the agent process";
            if (finish(kimix::optional<kimix::string>(std::move(desc)),
                       "killall")) {
              return result;
            }
          }
        }
      }
    }
  }

  // 5. wmic (safety.py 744-768).
  {
    const kimix::vector<word_match> matches = find_word_opt_exe(text, "wmic");
    for (const word_match &m : matches) {
      const kimix::string_view segment = segment_text(text, m.end);
      // \b(?:delete|terminate)\b must be present in the segment
      if (find_word(segment, "delete").empty() &&
          find_word(segment, "terminate").empty()) {
        continue;
      }
      // First re.search: processid\s*=\s*(\d+)
      // Second re.search: processid\s*=\s*\$\{?var\}?
      // They are independent scans (Python runs the var search even when
      // the numeric search matched but the PID was not protected).
      bool numeric_hit = false;
      {
        size_t pos = 0;
        while (pos < segment.size()) {
          pos = segment.find("processid", pos);
          if (pos == kimix::string_view::npos) {
            break;
          }
          size_t i = pos + 9;
          while (i < segment.size() && is_space(segment[i])) {
            ++i;
          }
          if (i >= segment.size() || segment[i] != '=') {
            pos += 9;
            continue;
          }
          ++i;
          while (i < segment.size() && is_space(segment[i])) {
            ++i;
          }
          size_t db = i;
          while (i < segment.size() && is_digit(segment[i])) {
            ++i;
          }
          if (i == db) {
            pos += 9; // not numeric; keep searching for another
            continue;
          }
          const kimix::string_view digits = segment.substr(db, i - db);
          // Python int() is unbounded; a run longer than 18 digits
          // never equals an int64_t protected PID, so skip it and
          // keep scanning for a later processid= token.
          if (digits.size() <= 18) {
            int64_t pid = 0;
            for (char c : digits) {
              pid = pid * 10 + (c - '0');
            }
            if (pid_protected(st, pid)) {
              kimix::string desc = "targets PID ";
              desc += kimix::string(digits.data(), digits.size());
              desc += " via `wmic`, which is the agent process or one of its "
                      "parent processes";
              if (finish(kimix::optional<kimix::string>(std::move(desc)),
                         "wmic")) {
                return result;
              }
            }
            numeric_hit = true; // re.search stops at first match
            break;
          }
          pos += 9;
        }
      }
      if (!numeric_hit) {
        size_t pos = 0;
        while (pos < segment.size()) {
          pos = segment.find("processid", pos);
          if (pos == kimix::string_view::npos) {
            break;
          }
          size_t i = pos + 9;
          while (i < segment.size() && is_space(segment[i])) {
            ++i;
          }
          if (i >= segment.size() || segment[i] != '=') {
            pos += 9;
            continue;
          }
          ++i;
          while (i < segment.size() && is_space(segment[i])) {
            ++i;
          }
          if (i >= segment.size() || segment[i] != '$') {
            pos += 9;
            continue;
          }
          ++i;
          if (i < segment.size() && segment[i] == '{') {
            ++i;
          }
          size_t vb = i;
          while (i < segment.size() &&
                 ((segment[i] >= 'a' && segment[i] <= 'z') ||
                  (segment[i] >= 'A' && segment[i] <= 'Z') ||
                  segment[i] == '_')) {
            ++i;
          }
          if (i == vb) {
            pos += 9;
            continue;
          }
          size_t ve = i;
          while (i < segment.size() &&
                 ((segment[i] >= 'a' && segment[i] <= 'z') ||
                  (segment[i] >= 'A' && segment[i] <= 'Z') ||
                  (segment[i] >= '0' && segment[i] <= '9') ||
                  segment[i] == '_')) {
            ++i;
          }
          // \}? optional, like the Python pattern
          const kimix::string var = lower_copy(segment.substr(vb, ve - vb));
          const kimix::vector<int64_t> *pids = loop_source_find(st.loops, var);
          if (pids != nullptr) {
            for (int64_t pid : *pids) {
              if (pid_protected(st, pid)) {
                const kimix::string_view var_original =
                    segment.substr(vb, ve - vb);
                kimix::string desc = "targets PID ";
                desc += kimix::format("{}", pid);
                desc += " via `wmic` through loop variable `$";
                desc += var_original;
                desc += "` (bound to PIDs ";
                desc += loop_pid_list(
                    kimix::span<const int64_t>(pids->data(), pids->size()));
                desc += "), which is the agent process or one of its parent "
                        "processes";
                if (finish(kimix::optional<kimix::string>(std::move(desc)),
                           "wmic")) {
                  return result;
                }
              }
            }
          }
          break; // first var match wins, like re.search
        }
      }
    }
  }

  return result;
}

kimix::optional<kimix::string> detect_self_kill(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline, tool_status &status) {
  const self_kill_result r =
      detect_self_kill_ex(command, protected_pids, image_names, cmdline);
  status = r.status;
  if (r.status != tool_status::ok) {
    return {};
  }
  return r.description;
}

kimix::optional<kimix::string> self_kill_hint(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline, int64_t agent_pid, tool_status &status) {
  status = tool_status::ok;
  bool only_space = true;
  for (char c : command) {
    if (!is_space(c)) {
      only_space = false;
      break;
    }
  }
  if (command.empty() || only_space) {
    return {};
  }
  kimix::vector<kimix::string> variants;
  command_detection_variants(command, variants);
  for (const kimix::string &variant : variants) {
    tool_status variant_status = tool_status::ok;
    const kimix::optional<kimix::string> desc = detect_self_kill(
        variant, protected_pids, image_names, cmdline, variant_status);
    if (variant_status == tool_status::unsupported) {
      status = tool_status::unsupported;
      return {};
    }
    if (desc.has_value()) {
      kimix::string out = "The command ";
      out += *desc;
      out += ". Executing it would terminate this agent session (current agent "
             "PID: ";
      out += kimix::format("{}", agent_pid);
      out += "). ";
      out += k_self_kill_guidance;
      return out;
    }
  }
  return {};
}

// ===========================================================================
// PowerShell 7.x -> 5.1 transform, fixer, hardline floor, RTK rewrite
// ===========================================================================

namespace {

inline bool pwsh_is_ascii(kimix::string_view s) noexcept {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80u) {
      return false;
    }
  }
  return true;
}

const char *const kW_NUL_REDIRECT =
    "Rewrote Windows-style null-device redirection target(s) `nul` to "
    "`$null` so PowerShell discards the output instead of creating a file "
    "named `nul`.";

// Python ``str.strip()`` emptiness (blank == no statement to repair).
inline bool pwsh_is_blank(kimix::string_view s) noexcept {
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' &&
        c != '\f') {
      return false;
    }
  }
  return true;
}

inline bool pwsh_at_token_start(kimix::string_view cmd, size_t i) noexcept {
  if (i == 0) {
    return true;
  }
  const char prev = cmd[i - 1];
  return !((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
           (prev >= '0' && prev <= '9') || prev == '_');
}

// Rewrite unquoted ``> nul``-style redirections to the ``$null`` sink.  Uses
// the shared RegionMask (strings/comments/here-strings are preserved), carves
// out ``--%`` stop-parsing lines (cmd.exe ``> NUL`` is valid there), collapses
// append operators (``>> nul`` -> ``> $null``) and preserves the fd/stream
// prefix (``2> $null`` / ``*> $null``).  Returns the rewritten command.
kimix::string rewrite_pwsh_nul_redirections(kimix::string_view cmd) {
  if (cmd.find('>') == kimix::string_view::npos) {
    return kimix::string(cmd);
  }
  const size_t n = cmd.size();
  kimix::runtime::parse::RegionMask mask(static_cast<uint32_t>(n));
  kimix::runtime::parse::build_pwsh_region_mask(cmd, mask);
  // Carve out --% stop-parsing lines (rest of the line is literal).
  for (size_t i = 0; i + 3 <= n;) {
    if (cmd[i] == '-' && cmd[i + 1] == '-' && cmd[i + 2] == '%' &&
        pwsh_at_token_start(cmd, i) && mask.is_code(static_cast<uint32_t>(i))) {
      size_t end = n;
      for (size_t j = i; j < n; ++j) {
        if (cmd[j] == '\n') {
          end = j;
          break;
        }
      }
      mask.mark(static_cast<uint32_t>(i), static_cast<uint32_t>(end));
      i = end;
      continue;
    }
    i += 1;
  }
  kimix::string out;
  out.reserve(n + 16);
  size_t prev = 0;
  bool changed = false;
  for (size_t i = 0; i + 1 < n;) {
    const char c = cmd[i];
    const size_t op_start = i;
    // operator: optional fd/stream prefix then one-or-more '>'
    if (c == '>' || (c >= '0' && c <= '9' && i + 1 < n && cmd[i + 1] == '>') ||
        (c == '*' && i + 1 < n && cmd[i + 1] == '>')) {
      size_t p = i;
      if (c != '>') {
        p += 1;
      }
      if (p >= n || cmd[p] != '>') {
        i += 1;
        continue;
      }
      const size_t op_end = p;
      size_t t = op_end;
      while (t < n && cmd[t] == '>') {
        ++t;
      }
      const size_t run_end = t;
      while (t < n && (cmd[t] == ' ' || cmd[t] == '\t' || cmd[t] == '\r')) {
        ++t;
      }
      if (t + 3 > n) {
        i += 1;
        continue;
      }
      char low[3];
      bool word = true;
      for (size_t q = 0; q < 3; ++q) {
        const char cc = cmd[t + q];
        low[q] = (cc >= 'A' && cc <= 'Z') ? static_cast<char>(cc + 32) : cc;
        if (!((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z'))) {
          word = false;
          break;
        }
      }
      if (!word || low[0] != 'n' || low[1] != 'u' || low[2] != 'l') {
        i += 1;
        continue;
      }
      // target must be a full word (not nul.txt / nul123)
      const size_t target_end = t + 3;
      if (target_end < n &&
          (((cmd[target_end] >= 'a' && cmd[target_end] <= 'z') ||
            (cmd[target_end] >= 'A' && cmd[target_end] <= 'Z') ||
            (cmd[target_end] >= '0' && cmd[target_end] <= '9') ||
            cmd[target_end] == '_' || cmd[target_end] == '.'))) {
        i += 1;
        continue;
      }
      if (!mask.is_code(static_cast<uint32_t>(op_start)) ||
          !mask.is_code(static_cast<uint32_t>(target_end - 1))) {
        i += 1;
        continue;
      }
      out.append(cmd.data() + prev, op_start - prev);
      // preserve fd/stream prefix, collapse '>>' to '>'
      kimix::string replacement;
      if (c != '>') {
        replacement.push_back(c);
      }
      replacement.push_back('>');
      // only preserve whitespace actually present between the operator
      // run and the target (`> nul` -> `> $null`, `>nul` -> `>$null`)
      const bool had_space = t > run_end;
      if (had_space) {
        replacement.push_back(' ');
      }
      replacement += "$null";
      out.append(replacement);
      prev = target_end;
      changed = true;
      i = target_end;
      continue;
    }
    i += 1;
  }
  if (!changed) {
    return kimix::string(cmd);
  }
  out.append(cmd.data() + prev, n - prev);
  return out;
}

const char *fix_warning_for_code(int code) {
  switch (code) {
  case 1:
    return "The command has an unclosed double-quoted string; appended a "
           "closing `\"` at the end to make it a legal PowerShell command.";
  case 2:
    return "The command has an unclosed single-quoted string; appended a "
           "closing `'` at the end to make it a legal PowerShell command.";
  case 3:
    return "The command has an unclosed double-quoted here-string; appended a "
           "newline and `\"@` at the end to close it.";
  case 4:
    return "The command has an unclosed single-quoted here-string; appended a "
           "newline and `'@` at the end to close it.";
  case 5:
    return "The command has an unclosed block comment `<#`; appended `#>` at "
           "the end to close it.";
  case 6:
    return "The command ends with a line comment; appended a newline so the "
           "trailing comment does not swallow the try/catch wrapper used to "
           "execute the command.";
  case 7:
    return "The command ends with the `--%` stop-parsing marker; appended a "
           "newline so the wrapper is not passed literally to the native "
           "command.";
  case 8:
    return "The command contains only comments; appended a newline and a no-op "
           "`$null` statement so the try/catch wrapper has a statement to "
           "execute.";
  case 9:
    return "The command ends with a backtick line-continuation; appended a "
           "newline so the continuation does not join with the try/catch "
           "wrapper used to execute the command.";
  default:
    return "";
  }
}

} // namespace

transform_result pwsh_transform(kimix::string_view code) {
  transform_result result;
  if (!pwsh_is_ascii(code)) {
    result.status = tool_status::unsupported;
    return result;
  }
  kimix::vector<kimix::runtime::parse::edit> edits;
  scan_shell(kimix::runtime::parse::shell_dialect::PWSH_TRANSFORM, code, edits,
             &result.command, nullptr, nullptr, nullptr, &result.warnings);
  result.status = tool_status::ok;
  return result;
}

fix_result fix_pwsh_command(kimix::string_view command) {
  fix_result result;
  if (!pwsh_is_ascii(command)) {
    result.valid = false;
    result.changed = false;
    return result;
  }
  // Run the null-device rewrite FIRST so plain ``echo hi > nul`` commands are
  // rewritten even though the PWSH_FIX fast path would return them unchanged,
  // then let the quote scanner validate/repair the rewritten text.  After the
  // scanner, run the rewrite AGAIN so any nul target exposed by an appended
  // closing quote/newline is handled too (mirror _shell_compat.py).
  //
  // The two rewrite passes must be reported as two *separate* booleans: the
  // reference composes
  //     warnings = [w for w in (nul_warning, scanner_warning, nul_after) if w]
  // so a scanner repair (which also changes the text) is never mistaken for a
  // nul rewrite, and the first-pass nul note precedes the scanner note.
  const kimix::string first_fixed = rewrite_pwsh_nul_redirections(command);
  const bool nul_changed_first = (first_fixed != kimix::string(command));
  if (pwsh_is_blank(first_fixed)) {
    result.valid = false;
    result.changed = false;
    return result;
  }
  kimix::vector<kimix::runtime::parse::edit> edits;
  kimix::string transformed;
  int warning_code = 0;
  scan_shell(kimix::runtime::parse::shell_dialect::PWSH_FIX, first_fixed, edits,
             &transformed, nullptr, nullptr, &warning_code, nullptr);
  if (warning_code == -1) {
    result.valid = false;
    result.changed = false;
    return result;
  }
  result.valid = true;
  result.command = rewrite_pwsh_nul_redirections(transformed);
  const bool nul_changed_after = (result.command != transformed);
  result.changed =
      (warning_code != 0) || nul_changed_first || nul_changed_after;
  kimix::string warning;
  if (nul_changed_first) {
    warning += kW_NUL_REDIRECT;
  }
  const int base = warning_code & 0x0F;
  if (base != 0) {
    if (!warning.empty()) {
      warning.push_back('\n');
    }
    warning += fix_warning_for_code(base);
  }
  if (warning_code & 0x10) {
    if (!warning.empty()) {
      warning.push_back('\n');
    }
    warning += "The command ends with a backtick line-continuation; "
               "appended a newline so the continuation does not join with "
               "the try/catch wrapper used to execute the command.";
  }
  if (nul_changed_after) {
    if (!warning.empty()) {
      warning.push_back('\n');
    }
    warning += kW_NUL_REDIRECT;
  }
  result.warning = std::move(warning);
  return result;
}

hardline_result check_hardline_blocked(kimix::string_view command) {
  hardline_result result;
  if (!pwsh_is_ascii(command)) {
    return result;
  }
  const kimix::runtime::tools::hardline_result hr =
      kimix::runtime::tools::check_hardline_blocked(command);
  result.blocked = hr.blocked;
  if (hr.description.has_value()) {
    result.description = hr.description.value();
  }
  return result;
}

kimix::builtin_tools::bash::rewrite_result
maybe_rewrite_with_rtk(kimix::string_view command, bool token_kill,
                       bool rtk_available, kimix::string_view rtk_binary_path,
                       bool exclude_read) {
  return kimix::builtin_tools::bash::maybe_rewrite_shell_command_with_rtk(
      command, token_kill, rtk_available, rtk_binary_path, exclude_read,
      /*pwsh=*/true);
}

Pwsh::Pwsh(kimix::builtin_tools::Session *session) : Tool(session) {}

// ---------------------------------------------------------------------------
// Availability probe (Tool::valid())
// ---------------------------------------------------------------------------

kimix::string detect_pwsh_path() {
  namespace fs = kimix::filesystem;
  const run::is_file_probe is_file = [](kimix::string_view p) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(kimix::string(p)), ec);
  };
  kimix::string path_env;
  if (const char *p = std::getenv("PATH"); p != nullptr) {
    path_env = kimix::string(p);
  }
  kimix::string pathext; // "" -> run::which applies the Windows default
  // PowerShell 7 (`pwsh`) first, then Windows PowerShell (`powershell`) -
  // the same preference the run tool's shell delegation uses.
  for (const char *name : {kPwshExecutable, kWindowsPowerShellExecutable}) {
    kimix::string hit = run::which(name, path_env, pathext, is_file);
    if (!hit.empty()) {
      return hit;
    }
  }
#ifdef KIMIX_PLATFORM_WINDOWS
  // Windows PowerShell ships in the box; a trimmed PATH can still hide it,
  // so fall back to the well-known System32 location.
  if (const char *root = std::getenv("SystemRoot");
      root != nullptr && *root != '\0') {
    std::error_code ec;
    const fs::path boxed = fs::path(kimix::string(root)) /
                           "System32/WindowsPowerShell/v1.0/powershell.exe";
    if (fs::is_regular_file(boxed, ec)) {
      return kimix::to_string(boxed);
    }
  }
#endif
  return {};
}

bool Pwsh::valid() const {
  return tool_valid("pwsh", !detect_pwsh_path().empty());
}

// ---------------------------------------------------------------------------
// Native subprocess management (the runner the bash / python / run tools use)
// ---------------------------------------------------------------------------
namespace {

// The mode strings the reference PowershellParams accept (pwsh_tool.py
// 275-292).
bool pwsh_is_exec_mode(const kimix::string &mode) {
  return mode == "execute" || mode == "send" || mode == "interactive";
}

void pwsh_str_param(const kimix::builtin_tools::ToolParams *p, const char *key,
                    kimix::string &out) {
  const auto *v = p->get(key);
  if (v != nullptr && v->is_string()) {
    out = v->as_string();
  }
}

int64_t pwsh_int_param(const kimix::builtin_tools::ToolParams *p,
                       const char *key, int64_t def) {
  const auto *v = p->get(key);
  return (v != nullptr && v->is_int()) ? v->as_int() : def;
}

bool pwsh_bool_param(const kimix::builtin_tools::ToolParams *p, const char *key,
                     bool def) {
  const auto *v = p->get(key);
  return (v != nullptr && v->is_bool()) ? v->as_bool() : def;
}

// This process id (safety.py os.getpid()) - the self-kill guard's default
// identity when the caller does not pass agent_pid.
int64_t pwsh_agent_pid() {
#ifdef KIMIX_PLATFORM_WINDOWS
  return static_cast<int64_t>(::GetCurrentProcessId());
#else
  return static_cast<int64_t>(::getpid());
#endif
}

// Child working directory: an explicit workdir (resolved against the session
// workspace when relative), else the session workspace ("").
kimix::string pwsh_native_cwd(const kimix::builtin_tools::Session *session,
                              kimix::string_view workdir) {
  const kimix::string session_dir =
      (session != nullptr) ? kimix::string(session->work_dir) : kimix::string();
  if (workdir.empty()) {
    return session_dir;
  }
  std::error_code ec;
  const kimix::filesystem::path wd =
      kimix::filesystem::path(kimix::string(workdir));
  if (wd.is_relative() && !session_dir.empty()) {
    return kimix::to_string(kimix::filesystem::path(session_dir) / wd);
  }
  return kimix::string(workdir);
}

// Read the caller-supplied self-kill identity (the same parameters the
// self_kill_hint kernel mode takes; the Python shim resolves them).
struct pwsh_guard_identity {
  int64_t agent_pid = 0;
  kimix::string cmdline;
  kimix::unordered_set<int64_t> protected_pids;
  kimix::unordered_set<kimix::string, kimix::string_hash> image_names;
};

pwsh_guard_identity pwsh_guard_read(const kimix::builtin_tools::ToolParams *p) {
  pwsh_guard_identity id;
  id.agent_pid = pwsh_int_param(p, "agent_pid", pwsh_agent_pid());
  pwsh_str_param(p, "cmdline", id.cmdline);
  const auto *pids = p->get("protected_pids");
  if (pids != nullptr && pids->is_array()) {
    for (const ValueElement &v : pids->as_array()) {
      if (v.is_int()) {
        id.protected_pids.insert(v.as_int());
      }
    }
  }
  const auto *names = p->get("image_names");
  if (names != nullptr && names->is_array()) {
    for (const ValueElement &v : names->as_array()) {
      if (v.is_string()) {
        id.image_names.insert(v.as_string());
      }
    }
  }
  return id;
}

// The two safety floors of the reference __call__, applied to one command line.
// Returns the blocking message when the command must not run.
kimix::optional<kimix::string>
pwsh_blocked_reason(kimix::string_view command, const pwsh_guard_identity &id,
                    bool &unsupported) {
  unsupported = false;
  const hardline_result hl = check_hardline_blocked(command);
  if (hl.blocked) {
    return hl.description;
  }
  tool_status st = tool_status::ok;
  const kimix::optional<kimix::string> hint = self_kill_hint(
      command, id.protected_pids, id.image_names, id.cmdline, id.agent_pid, st);
  if (st == tool_status::unsupported) {
    unsupported = true;
    return kimix::string(
        "Self-kill guard requires the Python mirror for this input.");
  }
  return hint;
}

// One captured foreground run -> the shared session output block (the same
// shape the bash tool renders, so the CLI and the model see one format).
kimix::string pwsh_render_run(const proc::run_result &rr,
                              const kimix::string &prepared,
                              const kimix::string &task_id, int64_t max_lines,
                              kimix::string &status_out) {
  kimix::string out = bash::truncate_lines(rr.output, max_lines, true, 2);
  kimix::optional<kimix::string> meaning;
  kimix::optional<kimix::string> hint;
  if (rr.killed) {
    status_out = "timeout";
  } else if (rr.exit_code.has_value() && *rr.exit_code != 0) {
    status_out = "failed";
    if (!bash::is_expected_exit(prepared, rr.exit_code)) {
      meaning = bash::interpret_exit_code(prepared, rr.exit_code);
      hint = bash::annotate_failure(rr.output, prepared, rr.exit_code);
      const kimix::optional<int64_t> eline =
          bash::find_error_line_index(rr.output);
      out += bash::process_exited_banner(*rr.exit_code, eline);
    }
  } else {
    status_out = "completed";
  }
  python::session_output_block block;
  block.task_id = task_id;
  block.status = status_out;
  block.output = std::move(out);
  block.exit_code =
      rr.exit_code.has_value()
          ? std::optional<int32_t>(static_cast<int32_t>(*rr.exit_code))
          : std::nullopt;
  block.exit_code_meaning = meaning;
  block.failure_hint = hint;
  block.wait_matched = rr.matched ? std::optional<bool>(true) : std::nullopt;
  block.elapsed_seconds = static_cast<double>(rr.elapsed_ms) / 1000.0;
  block.output_truncated = rr.truncated;
  return python::build_session_output_block(block);
}

} // namespace

// ---------------------------------------------------------------------------
// One-shot PowerShell argv (shell_common.py 30 + 100-151, pwsh_tool.py 70-74
// and 820-832). Pure: no process is spawned here.
// ---------------------------------------------------------------------------

const char *const k_pwsh_console_init =
    "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
    "$OutputEncoding=[System.Text.Encoding]::UTF8;"
    "try{[Console]::TreatControlCAsInput=$true}catch{};";

namespace {

const char k_pwsh_b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard padded base64, no line breaks (base64.b64encode).
void pwsh_base64(kimix::string_view bytes, kimix::string &out) {
  out.clear();
  const size_t n = bytes.size();
  out.reserve(((n + 2) / 3) * 4);
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t b0 = static_cast<uint8_t>(bytes[i]);
    const uint32_t b1 = (i + 1 < n) ? static_cast<uint8_t>(bytes[i + 1]) : 0;
    const uint32_t b2 = (i + 2 < n) ? static_cast<uint8_t>(bytes[i + 2]) : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(k_pwsh_b64_alpha[(v >> 18) & 63u]);
    out.push_back(k_pwsh_b64_alpha[(v >> 12) & 63u]);
    out.push_back(i + 1 < n ? k_pwsh_b64_alpha[(v >> 6) & 63u] : '=');
    out.push_back(i + 2 < n ? k_pwsh_b64_alpha[v & 63u] : '=');
  }
}

// UTF-8 text -> UTF-16LE bytes (what PowerShell decodes an -Enc payload as).
// Invalid UTF-8 bytes become U+FFFD, matching Python's
// decode(errors="replace").
void pwsh_utf16le(kimix::string_view text, kimix::string &out) {
  out.clear();
  out.reserve(text.size() * 2);
  const char *it = text.data();
  const char *end = it + text.size();
  while (it < end) {
    const uint32_t cp = kimix::builtin_tools::decode_code_point(it, end);
    if (cp >= 0x10000u) { // surrogate pair
      const uint32_t v = cp - 0x10000u;
      const uint16_t hi = static_cast<uint16_t>(0xD800u + (v >> 10));
      const uint16_t lo = static_cast<uint16_t>(0xDC00u + (v & 0x3FFu));
      out.push_back(static_cast<char>(hi & 0xFFu));
      out.push_back(static_cast<char>(hi >> 8));
      out.push_back(static_cast<char>(lo & 0xFFu));
      out.push_back(static_cast<char>(lo >> 8));
    } else {
      const uint16_t u = static_cast<uint16_t>(cp);
      out.push_back(static_cast<char>(u & 0xFFu));
      out.push_back(static_cast<char>(u >> 8));
    }
  }
}

// PWSH_ONESHOT_FLAGS (shell_common.py 30).
void pwsh_push_oneshot_flags(kimix::vector<kimix::string> &argv) {
  argv.push_back(kimix::string("-NoP"));
  argv.push_back(kimix::string("-NonI"));
  argv.push_back(kimix::string("-Exec"));
  argv.push_back(kimix::string("Bypass"));
  argv.push_back(kimix::string("-NoL"));
}

} // namespace

kimix::string wrap_pwsh_command(kimix::string_view command) {
  kimix::string out(k_pwsh_console_init);
  out += "try{";
  out.append(command.data(), command.size());
  out += "}catch{$_|Out-String|Write-Error;exit 1}";
  out += ";exit $LASTEXITCODE";
  return out;
}

pwsh_payload pwsh_maybe_encode(kimix::string_view raw) {
  // The reference compares len(full_cmd): CHARACTERS, not bytes.
  if (kimix::builtin_tools::utf8_code_point_count(raw) <=
      k_pwsh_encode_threshold) {
    pwsh_payload p;
    p.param = kimix::string(k_pwsh_param_command);
    p.value = kimix::string(raw);
    return p;
  }
  kimix::string utf16;
  pwsh_utf16le(raw, utf16);
  pwsh_payload p;
  p.param = kimix::string(k_pwsh_param_encoded);
  pwsh_base64(utf16, p.value);
  p.encoded = true;
  return p;
}

kimix::string pwsh_base64_string(kimix::string_view bytes) {
  kimix::string out;
  pwsh_base64(bytes, out);
  return out;
}

kimix::string pwsh_utf16le_string(kimix::string_view text) {
  kimix::string out;
  pwsh_utf16le(text, out);
  return out;
}

bool pwsh_is_windows_powershell(kimix::string_view executable) {
  size_t base = 0;
  for (size_t i = 0; i < executable.size(); ++i) {
    if (executable[i] == '/' || executable[i] == '\\') {
      base = i + 1;
    }
  }
  kimix::string name(executable.substr(base));
  for (char &c : name) {
    c = lower_ascii(c);
  }
  // Strip a trailing .exe before the name test.
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
    name.resize(name.size() - 4);
  }
  return name.compare(0, 10, "powershell") == 0;
}

pwsh_argv_result build_pwsh_argv(kimix::string_view command,
                                 kimix::string_view pwsh_path) {
  pwsh_argv_result r;
  if (command.empty()) {
    r.status = tool_status::invalid_input;
    r.message = "Empty command.";
    return r;
  }
  const kimix::string exe =
      pwsh_path.empty() ? detect_pwsh_path() : kimix::string(pwsh_path);
  if (exe.empty()) {
    r.status = tool_status::unsupported;
    r.message = "no PowerShell executable found on this system";
    return r;
  }
  // Parser repair first: an unrepairable command must never reach a process.
  // The kernels are ASCII-gated (non-ASCII answers invalid/unsupported so the
  // Python mirror can take over); a PowerShell 7 host needs no downgrade, so
  // such a command is still executed verbatim.
  const bool ascii_command = is_ascii_text(command);
  kimix::string inner(command);
  if (ascii_command) {
    const fix_result fr = fix_pwsh_command(command);
    if (!fr.valid) {
      r.status = tool_status::invalid_input;
      r.message = "Invalid PowerShell command.";
      return r;
    }
    inner = fr.command;
    if (fr.changed) {
      r.warning += "\n[WARNING] " + fr.warning;
    }
  }
  if (pwsh_is_windows_powershell(exe)) {
    // Windows PowerShell 5.1: downgrade PowerShell 7 syntax.
    const transform_result tr = pwsh_transform(inner);
    if (tr.status == tool_status::ok) {
      if (tr.command != inner) {
        inner = tr.command;
      }
      for (const kimix::string &w : tr.warnings) {
        r.warning += "\n[WARNING]" + w;
      }
    } else if (ascii_command) {
      r.status = tool_status::unsupported;
      r.message = "pwsh syntax transform needs the Python mirror";
      return r;
    }
  }
  const pwsh_payload payload = pwsh_maybe_encode(wrap_pwsh_command(inner));
  r.prepared = inner;
  r.executable = exe;
  r.argv.push_back(exe);
  pwsh_push_oneshot_flags(r.argv);
  r.argv.push_back(payload.param);
  r.argv.push_back(payload.value);
  r.status = tool_status::ok;
  return r;
}

kimix::vector<kimix::string>
build_pwsh_interactive_argv(kimix::string_view command,
                            kimix::string_view pwsh_path) {
  const kimix::string exe =
      pwsh_path.empty() ? detect_pwsh_path() : kimix::string(pwsh_path);
  kimix::vector<kimix::string> argv;
  argv.push_back(exe);
  // pwsh_tool.py 531: the REPL keeps an interactive console, so -NonI is
  // omitted and -NoExit holds the session open.
  argv.push_back(kimix::string("-NoP"));
  argv.push_back(kimix::string("-Exec"));
  argv.push_back(kimix::string("Bypass"));
  argv.push_back(kimix::string("-NoL"));
  argv.push_back(kimix::string("-NoExit"));
  argv.push_back(kimix::string("-Command"));
  kimix::string payload(k_pwsh_console_init);
  payload.append(command.data(), command.size());
  argv.push_back(std::move(payload));
  return argv;
}

static const kimix::builtin_tools::param_alias k_pwsh_aliases[] = {
    {"command", "cmd cmdline command_line script"},
    {"mode", "pwsh_mode run_mode operation"},
    {"timeout", "timeout_seconds timeout_sec"},
    {"workdir", "cwd working_dir work_directory"},
    {"task_id", "session_id job_id"},
    {"wait_for_pattern", "until_pattern wait_pattern"},
    {"max_lines", "output_lines line_limit"},
    {"token_kill", "rtk_token_kill token_kill_enabled"},
    {"rtk_available", "rtk rtk_enabled"},
    {"rtk_binary_path", "rtk_path rtk_binary"},
    {"exclude_read", "exclude_read_tool skip_read"},
    {"agent_pid", "pid agent_process_id"},
    {"cmdline", "process_cmdline agent_cmdline"},
    {"image_names", "process_images images"},
    {"protected_pids", "protected_processes protected"},
};

void Pwsh::operator()(const kimix::builtin_tools::ToolParams *parameters) {
  // Fuzzy alias matching (tool.h): wrong-but-reasonable argument names
  // ("command" for "cmd") are accepted; the canonical name always wins.
  const kimix::builtin_tools::ToolParams k_resolved =
      kimix::builtin_tools::ToolParams::with_aliases(parameters,
                                                     k_pwsh_aliases);
  if (parameters != nullptr) {
    parameters = &k_resolved;
  }
  using namespace kimix::builtin_tools;
  _last_result.clear();
  ToolParams result;
  if (parameters == nullptr) {
    result.values["status"] =
        ValueElement::make_string(kimix::string("invalid_input"));
    result.values["message"] =
        ValueElement::make_string(kimix::string("no parameters provided"));
    result.serialize(_last_result);
    return;
  }

  const auto *mode_val = parameters->get("mode");
  kimix::string mode = "transform";
  if (mode_val != nullptr && mode_val->is_string()) {
    mode = mode_val->as_string();
  }
  // The registry schema of the pwsh tool is {command, timeout} and the
  // reference tool EXECUTES by default (PowershellParams.mode defaults to
  // "execute"); the analysis kernels are reached by naming a mode. So a
  // native session that passes no mode is asking for a run - answering it
  // with the transform kernel would silently spawn nothing.
  const bool native_session = (_session != nullptr && _session->native_io);
  if (mode_val == nullptr && native_session) {
    mode = "execute";
  }

  const auto *cmd_val = parameters->get("command");
  if (cmd_val == nullptr || !cmd_val->is_string()) {
    result.values["status"] =
        ValueElement::make_string(kimix::string("invalid_input"));
    result.values["message"] = ValueElement::make_string(
        kimix::string("missing or invalid 'command'"));
    result.serialize(_last_result);
    return;
  }
  const kimix::string_view command = cmd_val->as_string();

  // ── native execution modes (the tool contract of the reference Powershell
  // tool): drive a real PowerShell process through the reproc runner. ──
  if (pwsh_is_exec_mode(mode)) {
    kimix::string task_id, wait_pattern, workdir;
    pwsh_str_param(parameters, "task_id", task_id);
    pwsh_str_param(parameters, "wait_for_pattern", wait_pattern);
    if (pwsh_str_param(parameters, "workdir", workdir), workdir.empty()) {
      pwsh_str_param(parameters, "cwd", workdir);
    }
    const int64_t timeout_s = pwsh_int_param(parameters, "timeout", 30);
    const int64_t max_lines = pwsh_int_param(parameters, "max_lines", 500);
    // prompt_common.timeout_field(): ge=1, le=900.
    if (timeout_s < 1 || timeout_s > 900) {
      result.values["status"] =
          ValueElement::make_string(kimix::string("invalid_input"));
      result.values["message"] = ValueElement::make_string(
          kimix::string("timeout must be between 1 and 900 seconds"));
      result.values["mode"] = ValueElement::make_string(mode);
      result.serialize(_last_result);
      return;
    }
    // run_in_background is the deprecated spelling of mode="send".
    if (pwsh_bool_param(parameters, "run_in_background", false) &&
        task_id.empty()) {
      mode = "send";
    }

    const bool native_io = (_session != nullptr && _session->native_io);
    if (!native_io) {
      result.values["status"] =
          ValueElement::make_string(kimix::string("unsupported"));
      result.values["message"] = ValueElement::make_string(kimix::string(
          "pwsh execution needs a native session (Session::native_io); "
          "the Python mirror runs the command otherwise"));
      result.values["mode"] = ValueElement::make_string(mode);
      result.serialize(_last_result);
      return;
    }

    // Safety floors first, on the RAW command (reference __call__ order).
    const pwsh_guard_identity identity = pwsh_guard_read(parameters);
    bool guard_unsupported = false;
    if (!command.empty()) {
      const auto blocked =
          pwsh_blocked_reason(command, identity, guard_unsupported);
      if (blocked.has_value()) {
        result.values["status"] = ValueElement::make_string(
            kimix::string(guard_unsupported ? "unsupported" : "blocked"));
        result.values["message"] =
            ValueElement::make_string(kimix::string(*blocked));
        result.values["mode"] = ValueElement::make_string(mode);
        result.serialize(_last_result);
        return;
      }
    }

    // Optional RTK rewrite (the shim owns the rtk availability flag).
    kimix::string prepared(command);
    {
      kimix::string rtk_path;
      pwsh_str_param(parameters, "rtk_binary_path", rtk_path);
      const auto rr = maybe_rewrite_with_rtk(
          prepared, pwsh_bool_param(parameters, "token_kill", true),
          pwsh_bool_param(parameters, "rtk_available", false), rtk_path,
          pwsh_bool_param(parameters, "exclude_read", false));
      prepared = rr.segment;
    }

    // A continuation of a running task sends the command to its stdin; the
    // reference dispatches on task_id before building any argv.
    if (!task_id.empty()) {
      tool_error serr{tool_status::ok, {}};
      if (!prepared.empty()) {
        serr = proc::send_task(task_id, prepared, true);
      }
      if (serr.failed()) {
        result.values["status"] =
            ValueElement::make_string(kimix::string("invalid_input"));
        result.values["message"] = ValueElement::make_string(serr.message);
        result.values["task_id"] = ValueElement::make_string(task_id);
        result.serialize(_last_result);
        return;
      }
      const int64_t wait_ms = timeout_s > 0
                                  ? timeout_s * 1000
                                  : (wait_pattern.empty() ? 5000 : 30000);
      const proc::task_wait_result tw =
          proc::wait_task(task_id, wait_pattern, wait_ms);
      kimix::string out;
      proc::read_task(task_id, out);
      out = bash::truncate_lines(out, max_lines, true, 2);
      const proc::task_status_info info = proc::query_task(task_id);
      python::session_output_block block;
      block.task_id = task_id;
      block.status = tw.exited ? "completed" : "running";
      block.output = std::move(out);
      if (tw.exited && info.exit_code.has_value()) {
        block.exit_code = static_cast<int32_t>(*info.exit_code);
      }
      block.wait_matched =
          tw.matched ? std::optional<bool>(true) : std::nullopt;
      block.elapsed_seconds = static_cast<double>(tw.elapsed_ms) / 1000.0;
      result.values["status"] = ValueElement::make_string(kimix::string("ok"));
      result.values["message"] = ValueElement::make_string(kimix::format(
          "Data sent to `{}`. Status: {}.", task_id, block.status));
      result.values["output_block"] =
          ValueElement::make_string(python::build_session_output_block(block));
      result.values["mode"] = ValueElement::make_string(mode);
      result.values["task_id"] = ValueElement::make_string(task_id);
      result.serialize(_last_result);
      return;
    }

    // Build the child command line (repair + downgrade + wrapper).
    proc::run_options opts;
    kimix::string warning;
    if (mode == "interactive") {
      const kimix::string exe = detect_pwsh_path();
      if (exe.empty()) {
        result.values["status"] =
            ValueElement::make_string(kimix::string("unsupported"));
        result.values["message"] = ValueElement::make_string(
            kimix::string("no PowerShell executable found on this system"));
        result.serialize(_last_result);
        return;
      }
      opts.argv = build_pwsh_interactive_argv(prepared, exe);
    } else {
      const pwsh_argv_result argv = build_pwsh_argv(prepared);
      if (argv.status != tool_status::ok) {
        result.values["status"] =
            ValueElement::make_string(argv.status == tool_status::unsupported
                                          ? kimix::string("unsupported")
                                          : kimix::string("invalid_input"));
        result.values["message"] =
            ValueElement::make_string(argv.message + argv.warning);
        result.values["mode"] = ValueElement::make_string(mode);
        result.serialize(_last_result);
        return;
      }
      // Re-check the floors on the text that is about to run (rtk rewrite
      // and the 5.1 downgrade both change it).
      bool recheck_unsupported = false;
      const auto blocked =
          pwsh_blocked_reason(argv.prepared, identity, recheck_unsupported);
      if (blocked.has_value()) {
        result.values["status"] = ValueElement::make_string(
            kimix::string(recheck_unsupported ? "unsupported" : "blocked"));
        result.values["message"] =
            ValueElement::make_string(kimix::string(*blocked));
        result.values["mode"] = ValueElement::make_string(mode);
        result.serialize(_last_result);
        return;
      }
      opts.argv = argv.argv;
      warning = argv.warning;
    }
    opts.working_directory = pwsh_native_cwd(_session, workdir);
    opts.output_cap_chars = 200000;
    opts.requested_task_id = "pwsh";
    opts.wait_pattern = wait_pattern;

    if (mode == "interactive" || mode == "send") {
      // Long-lived task: a REPL (interactive) or a background job (send).
      opts.timeout_ms = 0;
      proc::task_handle handle;
      const tool_error terr = proc::start_task(opts, handle);
      if (terr.failed()) {
        result.values["status"] =
            ValueElement::make_string(kimix::string("invalid_input"));
        result.values["message"] = ValueElement::make_string(terr.message);
        result.serialize(_last_result);
        return;
      }
      python::session_output_block block;
      block.task_id = handle.task_id;
      block.status = "running";
      block.output =
          kimix::format("PowerShell task started (pid {})", handle.pid);
      result.values["status"] = ValueElement::make_string(kimix::string("ok"));
      result.values["message"] = ValueElement::make_string(kimix::format(
          "{}PowerShell running in the background. task_id: `{}`. Use "
          "`job_output` to read output and the same tool with task_id to "
          "send more input.",
          warning, handle.task_id));
      result.values["output_block"] =
          ValueElement::make_string(python::build_session_output_block(block));
      result.values["mode"] = ValueElement::make_string(mode);
      result.values["task_id"] = ValueElement::make_string(handle.task_id);
      result.serialize(_last_result);
      return;
    }

    opts.timeout_ms = timeout_s > 0 ? timeout_s * 1000 : 0;
    const proc::run_result rr = proc::run_process(opts);
    if (!rr.spawn_error.empty()) {
      result.values["status"] =
          ValueElement::make_string(kimix::string("invalid_input"));
      result.values["message"] =
          ValueElement::make_string(rr.spawn_error + warning);
      result.values["mode"] = ValueElement::make_string(mode);
      result.serialize(_last_result);
      return;
    }
    kimix::string status_str;
    kimix::string block;
    if (rr.still_running) {
      // Quiet but alive: the runner adopted the child into the task
      // registry, so report the real id (job_output can read/stop it).
      status_str = "running";
      block = pwsh_render_run(rr, prepared,
                              task_id.empty() ? kimix::string("pwsh") : task_id,
                              max_lines, status_str);
      result.values["status"] = ValueElement::make_string(kimix::string("ok"));
      result.values["message"] = ValueElement::make_string(kimix::format(
          "{}Running in background. task_id: `{}`. Use `job_output` to "
          "read output or to stop it.",
          warning,
          rr.task_id.empty() ? kimix::string_view("(detached)")
                             : kimix::string_view(rr.task_id)));
    } else {
      block = pwsh_render_run(rr, prepared,
                              task_id.empty() ? kimix::string("pwsh") : task_id,
                              max_lines, status_str);
      result.values["status"] = ValueElement::make_string(kimix::string("ok"));
      result.values["message"] = ValueElement::make_string(kimix::string(
          status_str == "timeout"
              ? "Command timed out"
              : (status_str == "failed" ? "PowerShell command failed"
                                        : "Command completed")));
    }
    result.values["output_block"] = ValueElement::make_string(block);
    if (!warning.empty()) {
      result.values["warning"] = ValueElement::make_string(warning);
    }
    result.values["mode"] = ValueElement::make_string(mode);
    result.serialize(_last_result);
    return;
  }

  if (mode == "transform") {
    const transform_result tr = pwsh_transform(command);
    result.values["status"] = ValueElement::make_string(
        tr.status == tool_status::ok ? "ok" : "unsupported");
    result.values["command"] = ValueElement::make_string(tr.command);
    kimix::vector<ValueElement> warns;
    warns.reserve(tr.warnings.size());
    for (const kimix::string &w : tr.warnings) {
      warns.push_back(ValueElement::make_string(w));
    }
    result.values["warnings"] = ValueElement::make_array(std::move(warns));
  } else if (mode == "fix") {
    const fix_result fr = fix_pwsh_command(command);
    result.values["status"] =
        ValueElement::make_string(fr.valid ? "ok" : "error");
    result.values["valid"] = ValueElement::make_bool(fr.valid);
    result.values["changed"] = ValueElement::make_bool(fr.changed);
    result.values["command"] = ValueElement::make_string(fr.command);
    result.values["warning"] = ValueElement::make_string(fr.warning);
  } else if (mode == "hardline") {
    const hardline_result hr = check_hardline_blocked(command);
    result.values["status"] = ValueElement::make_string("ok");
    result.values["blocked"] = ValueElement::make_bool(hr.blocked);
    result.values["description"] = ValueElement::make_string(hr.description);
  } else if (mode == "rtk_rewrite") {
    bool token_kill = true;
    bool rtk_available = false;
    kimix::string rtk_binary_path;
    bool exclude_read = false;
    const auto *token_kill_val = parameters->get("token_kill");
    if (token_kill_val != nullptr && token_kill_val->is_bool()) {
      token_kill = token_kill_val->as_bool();
    }
    const auto *rtk_available_val = parameters->get("rtk_available");
    if (rtk_available_val != nullptr && rtk_available_val->is_bool()) {
      rtk_available = rtk_available_val->as_bool();
    }
    const auto *rtk_path_val = parameters->get("rtk_binary_path");
    if (rtk_path_val != nullptr && rtk_path_val->is_string()) {
      rtk_binary_path = rtk_path_val->as_string();
    }
    const auto *exclude_read_val = parameters->get("exclude_read");
    if (exclude_read_val != nullptr && exclude_read_val->is_bool()) {
      exclude_read = exclude_read_val->as_bool();
    }
    const auto rr = maybe_rewrite_with_rtk(command, token_kill, rtk_available,
                                           rtk_binary_path, exclude_read);
    result.values["status"] = ValueElement::make_string("ok");
    result.values["command"] = ValueElement::make_string(rr.segment);
    result.values["changed"] = ValueElement::make_bool(rr.changed);
  } else if (mode == "self_kill_hint") {
    int64_t agent_pid = 0;
    kimix::unordered_set<int64_t> protected_pids;
    kimix::unordered_set<kimix::string, kimix::string_hash> image_names;
    kimix::string cmdline;
    const auto *agent_pid_val = parameters->get("agent_pid");
    if (agent_pid_val != nullptr && agent_pid_val->is_int()) {
      agent_pid = agent_pid_val->as_int();
    }
    const auto *pids_val = parameters->get("protected_pids");
    if (pids_val != nullptr && pids_val->is_array()) {
      for (const ValueElement &v : pids_val->as_array()) {
        if (v.is_int()) {
          protected_pids.insert(v.as_int());
        }
      }
    }
    const auto *names_val = parameters->get("image_names");
    if (names_val != nullptr && names_val->is_array()) {
      for (const ValueElement &v : names_val->as_array()) {
        if (v.is_string()) {
          image_names.insert(v.as_string());
        }
      }
    }
    const auto *cmdline_val = parameters->get("cmdline");
    if (cmdline_val != nullptr && cmdline_val->is_string()) {
      cmdline = cmdline_val->as_string();
    }
    tool_status status = tool_status::ok;
    const auto hint = self_kill_hint(command, protected_pids, image_names,
                                     cmdline, agent_pid, status);
    result.values["status"] = ValueElement::make_string(
        status == tool_status::ok ? "ok" : "unsupported");
    result.values["blocked"] = ValueElement::make_bool(hint.has_value());
    if (hint.has_value()) {
      result.values["description"] = ValueElement::make_string(*hint);
    }
  } else {
    result.values["status"] =
        ValueElement::make_string(kimix::string("invalid_input"));
    result.values["message"] =
        ValueElement::make_string(kimix::string("unknown mode"));
  }
  result.serialize(_last_result);
}

} // namespace kimix::builtin_tools::pwsh
