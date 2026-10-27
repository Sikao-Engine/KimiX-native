/* bp_unquoted.cpp — the `_process_unquoted` emitter.
 *
 * Line-for-line port of kimi-agent `shell_compat/unquoted.py`
 * (`_process_unquoted`, `_dq_region`, `_nul_spans`, `_in_nul`) plus the
 * constants `_BASH_METACHARACTERS`, `_DQ_ESCAPED` and `_ESCAPED_NUL_RE` from
 * `shell_compat/bash_core.py`.  The region boundaries come from the shared
 * oracles in bp_core.h (never re-implemented here); the null-device word spans
 * are scanned explicitly over byte offsets as the reference regex would.
 *
 * Byte-exact semantics: every offset is a UTF-8 byte offset into the source;
 * non-ASCII bytes are opaque and copied through untouched.  The walker takes a
 * region boundary only when the region is properly closed (the `_find_*`
 * oracles return -1 on unterminated input, in which case the remainder is
 * copied verbatim).  Command/backtick substitutions and double-quoted
 * substitutions recurse through this same contract on their slices.
 */
#include "llm/bash_parser/bp_unquoted.h"

#include "llm/bash_parser/bp_core.h"

namespace kimix::llm::bash_parser {

namespace src_llm_bash_parser_bp_unquoted {

// `_BASH_METACHARACTERS` / `_DQ_ESCAPED` membership live in bp_core.h.

// A null-device word span `[lo, hi)` (a `_ESCAPED_NUL_RE` match).
struct NulSpan {
  size_t lo;
  size_t hi;
};

// ASCII byte lower-case, used for the case-insensitive `[nN] [uU] [lL]`
// letter classes of `_ESCAPED_NUL_RE`.
inline char lower_ascii(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
}

// The `regex` module's `\s` set (the reference imports `regex`, not stdlib
// `re`, so `\s` is these 25 Unicode codepoints — enumerated from
// `regex.match(r'\s', chr(c))`): the ASCII whitespace plus the Unicode
// spaces/line-separators.  The `_ESCAPED_NUL_RE` lookbehind `[\s;|&<>(){}]`
// matches ONE such character (which may be multi-byte in UTF-8) immediately
// before the nul word.
inline bool is_unicode_space(uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: // \t\n\v\f\r
    case 0x20:                                              // space
    case 0x85:                                              // NEL
    case 0xA0:                                              // NBSP
    case 0x1680:                                            // Ogham space mark
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A:                                            // En/Em/… spaces
    case 0x2028: case 0x2029:                               // line/para sep
    case 0x202F:                                            // narrow NBSP
    case 0x205F:                                            // medium math space
    case 0x3000:                                            // ideographic space
        return true;
    default:
        return false;
    }
}

// Decode the UTF-8 codepoint that ENDS immediately before byte offset `p`
// (i.e. the last codepoint of cmd[0:p]).  Returns false when `p == 0` or the
// preceding bytes are not a well-formed UTF-8 sequence ending at `p` (then
// the caller falls back to a single-byte test).  On success writes the
// codepoint to `*cp`.
inline bool decode_preceding_codepoint(kimix::string_view cmd, size_t p, uint32_t *cp) {
    if (p == 0 || p > cmd.size()) {
        return false;
    }
    // Walk back over continuation bytes (10xxxxxx) to the lead byte.
    size_t start = p;
    while (start > 0 && (static_cast<unsigned char>(cmd[start - 1]) & 0xC0) == 0x80) {
        --start;
    }
    if (start == 0) {
        return false; // all continuation bytes: malformed
    }
    --start; // now cmd[start] is the lead byte
    const unsigned char lead = static_cast<unsigned char>(cmd[start]);
    const size_t len = p - start;
    uint32_t value = 0;
    if (lead < 0x80) {
        if (len != 1) return false;
        value = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        if (len != 2) return false;
        value = (uint32_t)(lead & 0x1F) << 6;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 1]) & 0x3F);
        if (value < 0x80) return false; // overlong
    } else if ((lead & 0xF0) == 0xE0) {
        if (len != 3) return false;
        value = (uint32_t)(lead & 0x0F) << 12;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 1]) & 0x3F) << 6;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 2]) & 0x3F);
        if (value < 0x800) return false; // overlong
    } else if ((lead & 0xF8) == 0xF0) {
        if (len != 4) return false;
        value = (uint32_t)(lead & 0x07) << 18;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 1]) & 0x3F) << 12;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 2]) & 0x3F) << 6;
        value |= (uint32_t)(static_cast<unsigned char>(cmd[start + 3]) & 0x3F);
        if (value < 0x10000 || value > 0x10FFFF) return false; // overlong / out of range
    } else {
        return false; // invalid lead byte
    }
    *cp = value;
    return true;
}

// The `_ESCAPED_NUL_RE` lookbehind class `[\s;|&<>(){}]`: a match may begin at
// offset 0 or after ONE character that is either Unicode whitespace (the
// `regex` `\s` set) or one of the shell boundary punctuation bytes.  The
// preceding character is decoded as UTF-8 so a multi-byte space (e.g. U+2028)
// is recognized exactly as the reference regex does.
inline bool nul_lookbehind_ok(kimix::string_view cmd, size_t p) {
    if (p == 0) {
        return true; // (?<=^)
    }
    uint32_t cp = 0;
    if (decode_preceding_codepoint(cmd, p, &cp)) {
        if (is_unicode_space(cp)) {
            return true;
        }
        // The punctuation class is ASCII-only; a decoded multi-byte codepoint
        // is never one of them.
        if (cp > 0x7F) {
            return false;
        }
    }
    const char prev = cmd[p - 1];
    switch (prev) {
    case ';': case '|': case '&': case '<': case '>':
    case '(': case ')': case '{': case '}':
        return true;
    case ' ': case '\t': case '\n': case '\r': case '\f': case '\v':
        // ASCII whitespace (a subset of `\s`); reached when the decode above
        // did not fire or returned an ASCII codepoint.
        return true;
    default:
        return false;
    }
}

// The `_ESCAPED_NUL_RE` lookahead class `[A-Za-z0-9_.\\]`: a match may NOT be
// followed by one of these bytes (a longer word or a Windows path).
inline bool is_nul_trailing(char ch) {
  return is_path_alpha(ch) || is_ascii_digit(ch) || ch == '_' || ch == '.' || ch == '\\';
}

// Try to match `\\?[nN]\\?[uU]\\?[lL]` starting at byte `p` of `cmd`, honouring
// the lookbehind (`p == 0` or the preceding byte is a word boundary) and the
// negative lookahead.  On success writes the span end to `*end` and returns
// true.  The greedy `\\?` consumes a backslash when present and then requires
// its letter (a backslash never substitutes for the letter), so the match is
// deterministic: "skip one optional backslash, then require the letter".
bool nul_word_at(kimix::string_view cmd, size_t p, size_t *end) {
  const size_t length = cmd.size();
  if (!nul_lookbehind_ok(cmd, p)) {
    return false;
  }
  size_t q = p;
  // \\?[nN]
  if (q < length && cmd[q] == '\\') {
    ++q;
  }
  if (q >= length || lower_ascii(cmd[q]) != 'n') {
    return false;
  }
  ++q;
  // \\?[uU]
  if (q < length && cmd[q] == '\\') {
    ++q;
  }
  if (q >= length || lower_ascii(cmd[q]) != 'u') {
    return false;
  }
  ++q;
  // \\?[lL]
  if (q < length && cmd[q] == '\\') {
    ++q;
  }
  if (q >= length || lower_ascii(cmd[q]) != 'l') {
    return false;
  }
  ++q;
  // (?![A-Za-z0-9_.\\])
  if (q < length && is_nul_trailing(cmd[q])) {
    return false;
  }
  *end = q;
  return true;
}

// `_nul_spans(cmd)`: the non-overlapping `_ESCAPED_NUL_RE` match spans, scanned
// left to right like `finditer` (after a match the next scan resumes at its
// end; on a failed start the next candidate is one byte later).  Empty when the
// command holds no backslash (the reference's early return).
kimix::vector<NulSpan> nul_spans(kimix::string_view cmd) {
  kimix::vector<NulSpan> spans;
  if (cmd.find('\\') == kimix::string_view::npos) {
    return spans;
  }
  const size_t length = cmd.size();
  size_t p = 0;
  while (p < length) {
    size_t e = 0;
    if (nul_word_at(cmd, p, &e)) {
      spans.push_back(NulSpan{p, e});
      p = e;
    } else {
      ++p;
    }
  }
  return spans;
}

// `_in_nul(i, nul_spans)`: true when byte index `i` lies inside an escaped-nul
// word span.
bool in_nul(const kimix::vector<NulSpan> &spans, size_t i) {
  for (const NulSpan &s : spans) {
    if (s.lo <= i && i < s.hi) {
      return true;
    }
  }
  return false;
}

// `_UNQUOTED_SPECIAL_RE = [\\'"$\`]`: the index of the next special byte in the
// half-open range `[lo, hi)`, or -1 when there is none.
int64_t find_special(kimix::string_view cmd, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; ++i) {
    const char c = cmd[i];
    if (c == '\\' || c == '\'' || c == '"' || c == '$' || c == '`') {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

kimix::string dq_region(kimix::string_view cmd, size_t dq_start, size_t dq_end, const kimix::vector<NulSpan> &nul);

} // namespace src_llm_bash_parser_bp_unquoted
using namespace src_llm_bash_parser_bp_unquoted;

kimix::string process_unquoted_impl(kimix::string_view cmd) {
  kimix::string result;
  const size_t length = cmd.size();
  result.reserve(length);
  // Word spans that are escaped spellings of `nul`; backslashes inside them are
  // preserved (not converted to `/`).  Computed once per call on the whole
  // `cmd`; each recursion recomputes its own spans on its slice.
  const kimix::vector<NulSpan> nul = nul_spans(cmd);
  size_t i = 0;
  while (i < length) {
    // ---- find the next special character ----
    const int64_t m = find_special(cmd, i, length);
    if (m >= 0) {
      const size_t nxt = static_cast<size_t>(m);
      if (nxt > i) {
        result.append(cmd.data() + i, nxt - i);
        i = nxt;
      }
    } else {
      result.append(cmd.data() + i, length - i);
      break;
    }
    if (i >= length) {
      break;
    }
    const char char_at = cmd[i];
    if (char_at == '\'') {
      const size_t end = cmd.find('\'', i + 1);
      if (end == kimix::string_view::npos) {
        result.append(cmd.data() + i, length - i);
        break;
      }
      result.append(cmd.data() + i, end + 1 - i);
      i = end + 1;
    } else if (char_at == '"') {
      const int64_t dq_end = find_dq_end(cmd, static_cast<int64_t>(i) + 1);
      if (dq_end == -1) {
        result.append(cmd.data() + i, length - i);
        break;
      }
      result += dq_region(cmd, i, static_cast<size_t>(dq_end), nul);
      i = static_cast<size_t>(dq_end);
    } else if (char_at == '$' && i + 1 < length && cmd[i + 1] == '\'') {
      const int64_t ac_end = find_ansi_c_end(cmd, static_cast<int64_t>(i) + 2);
      if (ac_end == -1) {
        result.append(cmd.data() + i, length - i);
        break;
      }
      result.append(cmd.data() + i, static_cast<size_t>(ac_end) - i);
      i = static_cast<size_t>(ac_end);
    } else if (char_at == '`') {
      const int64_t bt_end = find_backtick_end(cmd, static_cast<int64_t>(i) + 1);
      if (bt_end == -1) {
        result.append(cmd.data() + i, length - i);
        break;
      }
      result.push_back('`');
      // Recurse on the content slice (legacy `_process_unquoted(slice)`); the
      // slice's own null-device spans are computed inside the call.
      result += process_unquoted_impl(cmd.substr(i + 1, static_cast<size_t>(bt_end - 1) - (i + 1)));
      result.push_back('`');
      i = static_cast<size_t>(bt_end);
    } else if (char_at == '\\') {
      if (i + 1 < length && in_bash_metacharacters(cmd[i + 1])) {
        result.push_back('\\');
        result.push_back(cmd[i + 1]);
        i += 2;
      } else if (i + 1 < length && !nul.empty() && in_nul(nul, i)) {
        result.push_back('\\');
        result.push_back(cmd[i + 1]);
        i += 2;
      } else {
        result.push_back('/');
        i += 1;
      }
    } else {
      // `$` not followed by `'` (covers top-level `$(…)`, whose content the flat
      // walk consumes: backslashes convert with the whole-string nul spans).
      // Emit the single character and advance one.
      result.push_back(char_at);
      i += 1;
    }
  }
  return result;
}

namespace src_llm_bash_parser_bp_unquoted {

// `_dq_region(cmd, dq_start, dq_end, nul)`: process the closed double-quoted
// region `cmd[dq_start:dq_end]`.  `dq_start` is the opening `"` and `dq_end` is
// one past the closing `"` (both copied verbatim except where a substitution is
// recursed).  Within the content the walker bulk-copies literal text, preserves
// `\X` pairs (X in `_DQ_ESCAPED`), treats a literal `$'` as ordinary text, and
// recurses on the slice of any `$(…)` / backtick substitution.  A substitution
// deemed unterminated or out-of-region copies the rest of the region verbatim.
// `nul` is the caller's whole-string span list: the DQ content never consults
// it directly (only `\X` pairs and recursed substitutions occur here), kept for
// signature parity with the reference.
kimix::string dq_region(kimix::string_view cmd, size_t dq_start, size_t dq_end, const kimix::vector<NulSpan> &nul) {
  (void)nul;
  kimix::string out;
  out.push_back(cmd[dq_start]); // opening "
  size_t j = dq_start + 1;
  size_t chunk_start = j;
  while (j < dq_end) {
    const int64_t m2 = find_special(cmd, j, dq_end);
    if (m2 >= 0) {
      const size_t nxt2 = static_cast<size_t>(m2);
      if (nxt2 > j) {
        j = nxt2;
      }
    } else {
      j = dq_end;
      break;
    }
    const char c = cmd[j];
    if (c == '\\' && j + 1 < dq_end && in_dq_escaped(cmd[j + 1])) {
      j += 2;
    } else if (c == '$' && j + 1 < dq_end && cmd[j + 1] == '(') {
      const int64_t paren_end = find_matching_paren(cmd, static_cast<int64_t>(j) + 1);
      if (paren_end == -1 || static_cast<size_t>(paren_end) >= dq_end) {
        j = dq_end;
        break;
      }
      out.append(cmd.data() + chunk_start, j - chunk_start);
      out.append("$(", 2);
      out += process_unquoted_impl(cmd.substr(j + 2, static_cast<size_t>(paren_end) - (j + 2)));
      out.push_back(')');
      j = static_cast<size_t>(paren_end) + 1;
      chunk_start = j;
    } else if (c == '$' && j + 1 < dq_end && cmd[j + 1] == '\'') {
      // Literal `$'` inside `"..."`: advance one so both `$` and the following
      // `'` stay in the chunk, keeping a real `$(…)` / backtick between them
      // active.
      j += 1;
    } else if (c == '`') {
      const int64_t bt_end = find_backtick_end(cmd, static_cast<int64_t>(j) + 1);
      if (bt_end == -1 || static_cast<size_t>(bt_end) > dq_end) {
        j = dq_end;
        break;
      }
      out.append(cmd.data() + chunk_start, j - chunk_start);
      out.push_back('`');
      out += process_unquoted_impl(cmd.substr(j + 1, static_cast<size_t>(bt_end - 1) - (j + 1)));
      out.push_back('`');
      j = static_cast<size_t>(bt_end);
      chunk_start = j;
    } else {
      // bare `'` or `"` inside a closed region, or a `$` / `\` that did not
      // start a handled pair: copy as ordinary text.
      j += 1;
    }
  }
  out.append(cmd.data() + chunk_start, dq_end - chunk_start); // trailing text + closing "
  return out;
}

} // namespace src_llm_bash_parser_bp_unquoted
using namespace src_llm_bash_parser_bp_unquoted;

} // namespace kimix::llm::bash_parser
