/*
 * shell_scanner.h - Bash / PowerShell command scanners (kimix::runtime::parse).
 *
 * Plan 012: native ports of the four per-character command scanners in
 * kimi-agent:
 *   - BASH_FIX              bash_fix.py::_Scanner._scan_range_inner
 *                           (Windows-path rewrites + cd /d drops + fallback
 *                           wrapper-runner replacements; emits edits, fallback
 *                           names and path notes)
 *   - BASH_PROCESS_UNQUOTED bash_tool.py::_process_unquoted
 *                           (unquoted '\' -> '/' conversions inside command
 *                           substitution / backticks)
 *   - PWSH_FIX              pwsh_fix.py::_Scanner.fix
 *                           (repairs unclosed quotes/comments/here-strings by
 *                           appending closers; emits ONE edit at EOF, or no
 *                           edits when valid/unrepairable)
 *   - PWSH_TRANSFORM        process_pwsh.py::pwsh_transform
 *                           (PS7 -> PS5.1 rewrites: ??= ?. ?[ ?? ternary ?: and
 *                           && / || chain operators, per logical line)
 *
 * Semantics are verified against the reference sources (golden vectors). The
 * BASH_FIX port uses depth-bounded recursion; on overflow the command is
 * returned unchanged exactly like the reference RecursionError path. The
 * bound is 256 (documented deviation: the reference's _MAX_NESTING_DEPTH is
 * 1024, but Python's interpreter limit fires first in practice, and the C++
 * frames are far larger than Python frames, so 1024 would overflow a 1 MiB
 * thread stack when the kernel is called from Python).
 *
 * PWSH_TRANSFORM note: the reference rebuilds the region mask after every
 * operator rewrite (the replacement text can itself contain quotes/comments,
 * so the mask must be revalidated for byte-exact output). This port does the
 * same; the RegionMask helper keeps the rebuild O(line) with no per-char
 * Python objects.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace parse {

struct edit {
    uint32_t start;              // byte offset
    uint32_t end;                // byte offset (exclusive)
    kimix::string replacement;   // replacement text
};

enum class shell_dialect : uint8_t {
    BASH_FIX,
    BASH_PROCESS_UNQUOTED,
    PWSH_FIX,
    PWSH_TRANSFORM
};

// Bitmask over byte positions: mark() clears the "code" bit for [start,end);
// is_code(i) reports whether position i is executable code (not inside a
// string/comment region). Used by the pwsh transformer.
class KIMIX_RUNTIME_API RegionMask {
public:
    explicit RegionMask(uint32_t n);
    void mark(uint32_t start, uint32_t end);
    bool is_code(uint32_t i) const noexcept;
    uint32_t size() const noexcept { return size_; }

private:
    uint32_t size_;
    kimix::vector<uint64_t> bits_;
};

// One forward pass. `edits` is cleared first and appended in source order.
// Optional outputs:
//   transformed - final text after applying the edits (computed by the kernel
//                 for BASH_PROCESS_UNQUOTED, PWSH_FIX and PWSH_TRANSFORM; for
//                 BASH_FIX the kernel cannot build the fallback-definitions
//                 prefix (the data lives in the Python shim), so `transformed`
//                 is left empty and the shim composes prefix + edits).
//   names       - BASH_FIX only: fallback command names (prefix sources).
//   notes       - BASH_FIX only: original raw words rewritten (path notes).
//   warning_code - PWSH_FIX only: repair outcome code. 0 = valid, 1..9 = the
//                 warning kinds (dq/sq/hdq/hsq/block/trailing-comment/
//                 stop-parsing/comment-only/trailing-continuation), bit 0x10
//                 set when a trailing-continuation newline was appended,
//                 -1 = unrepairable (None).
//   warnings    - PWSH_TRANSFORM only: human-readable "Line N: ..." messages
//                 describing each operator rewrite, in order.
KIMIX_RUNTIME_API void scan_shell(shell_dialect dialect, kimix::string_view cmd,
                                  kimix::vector<edit>& edits,
                                  kimix::string* transformed = nullptr,
                                  kimix::vector<kimix::string>* names = nullptr,
                                  kimix::vector<kimix::string>* notes = nullptr,
                                  int* warning_code = nullptr,
                                  kimix::vector<kimix::string>* warnings = nullptr);

} // namespace parse
} // namespace runtime
} // namespace kimix
