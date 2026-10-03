// regex_lite.h - Minimal backtracking regex engine for the native grep tool.
//
// The project does not vendor PCRE2 (see reports/grep.md), so the native
// Grep tool matches lines with this small engine instead. Supported syntax:
//   * literals and escapes (\n \t \r \f \v \d \D \w \W \s \S \. \\ ...)
//   * '.' (any char except '\n')
//   * character classes [abc], [a-z], [^abc] ('[!...]' negation also works)
//   * quantifiers * + ? {n} {n,} {n,m} and their lazy variants (*? +? ?? {n,m}?)
//   * groups ( ... ) and non-capturing (?: ... )
//   * alternation a|b
//   * anchors ^ and $ (position-based: '^' matches at text start or after
//     '\n', '$' at text end or before '\n')
// Not supported: back-references, look-around, possessive quantifiers,
// named groups. compile() rejects them with a descriptive error.
//
// Complexity note: backtracking with a total-work budget (steps AND recursion
// depth, both proportional to the text size). Benign patterns never notice
// it; adversarial ones ("(a+)+$" on a long non-matching line) trip the budget
// and the search is re-decided by the polynomial Thompson NFA simulation
// (search_nfa), so worst-case cost is O(states x text) with byte-identical
// answers for anything the backtracker would have finished.
//
// Namespace: kimix::builtin_tools::regex_lite. Unity-build safe: all symbols
// live inside this namespace.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::builtin_tools::regex_lite {

// Decode one UTF-8 code point at `pos` (single-byte fallback for invalid
// sequences). Advances `pos` past the code point. Exposed for testing.
char32_t decode_utf8(kimix::string_view text, size_t &pos);

// A compiled pattern. Compile once, match many times. Not thread-safe: one
// Regex object must not be used from multiple threads concurrently (mutable
// capture / repetition state).
class Regex {
public:
    Regex() = default;

    // Compile `pattern`. Returns false and fills `error` on syntax errors or
    // unsupported constructs. `ignore_case` folds ASCII A-Za-z on both sides.
    bool compile(kimix::string_view pattern, bool ignore_case,
                 kimix::string &error);

    bool valid() const { return _valid; }

    // Search `text` for the leftmost match (leftmost-first semantics like
    // Perl/PCRE). Returns true on match and fills [out_begin, out_end) byte
    // offsets (end exclusive). `start` biases the first attempt offset.
    //
    // The backtracker carries a STEP BUDGET (proportional to the text size):
    // benign patterns never come near it, and adversarial ones (nested
    // quantifiers, e.g. "(a+)+$") abort to search_nfa instead of burning
    // exponential time - same boolean answer, polynomial cost, byte-for-byte
    // identical results on everything the backtracker would have finished.
    bool search(kimix::string_view text, size_t &out_begin, size_t &out_end,
                size_t start = 0) const;

    // Thompson NFA simulation over the compiled program: O(states x text)
    // worst case, no backtracking. Priority-ordered thread list preserves
    // leftmost-first match selection (same match the backtracker would find),
    // but capturing groups are NOT tracked, so last_group() is unavailable on
    // matches found through this path. Exposed for testing and used as the
    // bounded-search fallback when the backtracker's step budget trips.
    bool search_nfa(kimix::string_view text, size_t &out_begin, size_t &out_end,
                    size_t start = 0) const;

    // Whole-string match (implicitly anchored at both ends).
    bool full_match(kimix::string_view text) const;

    // Capturing-group span of the last successful match on THIS object.
    // Index 0 = whole match. Returns false when the group did not participate
    // or the index is out of range.
    bool last_group(size_t index, size_t &out_begin, size_t &out_end) const;

    size_t group_count() const { return _groups; }

private:
    // One instruction of the compiled program. Sequence flows through `next`;
    // `alt` carries the second branch of alt/split_rep nodes. A node whose
    // continuation is still unknown keeps next/alt == -1 and is listed in the
    // fragment's dangling-tail set (patched when the continuation is known).
    enum class op_kind : uint8_t {
        empty,      // no-op
        char_,      // one literal code point
        any,        // '.' (not '\n')
        class_,     // character class (ranges + negation)
        group_open, // capturing (group >= 0) / non-capturing (group == -1)
        group_close,
        alt,        // try `next`, else `alt`
        split_rep,  // counted quantifier: body = `next`, cont = `alt`
        loop,       // end of a quantifier body: jump back to its split_rep
        anchor_bos, // '^'
        anchor_eos, // '$'
        match_end,  // accept
    };
    struct node {
        op_kind kind = op_kind::empty;
        char32_t ch = 0;
        kimix::vector<std::pair<char32_t, char32_t>> ranges; // class_
        bool negated = false;                                // class_
        bool lazy = false;                                   // split_rep
        int32_t group = -1;                                  // group_*
        int32_t rep_slot = -1;                               // split_rep counter index
        int32_t min_rep = 0;                                 // split_rep
        int32_t max_rep = -1;                                // split_rep; -1 == unbounded
        int32_t next = -1;
        int32_t alt = -1;
    };
    // A compiled fragment: an entry point plus the set of (node, field) pairs
    // whose continuation is still dangling. `is_alt` selects the `alt` field.
    struct frag {
        int32_t head = -1; // -1 == empty fragment (matches nothing, no nodes)
        kimix::vector<std::pair<int32_t, bool>> tails;
    };

    kimix::vector<node> _nodes;
    kimix::vector<int32_t> _split_pcs;  // every split_rep node index
    mutable kimix::vector<int32_t> _rep_counts; // per split_rep iteration count
    int32_t _head = -1;
    bool _valid = false;
    bool _ignore_case = false;
    size_t _groups = 0;
    mutable kimix::vector<size_t> _caps; // 2 slots per group; SIZE_MAX == unset
    mutable size_t _match_end = 0;
    mutable int64_t _consumed = 0; // total chars consumed (zero-width guard)
    mutable uint64_t _steps = 0;      // backtracker node visits this search()
    mutable uint64_t _step_limit = 0; // budget: trip -> NFA fallback
    mutable bool _aborted = false;    // the budget tripped
    mutable uint32_t _depth = 0;      // run() recursion depth (stack guard)

    int32_t alloc_node();
    void patch(const frag &f, int32_t target);
    frag concat(frag a, const frag &b);
    bool parse_alt(kimix::string_view pat, size_t &pos, frag &out,
                   kimix::string &error);
    bool parse_seq(kimix::string_view pat, size_t &pos, frag &out,
                   kimix::string &error);
    bool parse_quant(kimix::string_view pat, size_t &pos, frag &out,
                     kimix::string &error);
    bool parse_atom(kimix::string_view pat, size_t &pos, frag &out,
                    kimix::string &error);
    bool parse_class_body(kimix::string_view pat, size_t &pos, node &out,
                          kimix::string &error);
    bool run(kimix::string_view text, int32_t pc, size_t pos) const;
    bool run_impl(kimix::string_view text, int32_t pc, size_t pos) const;
    bool match_char(const node &n, char32_t c) const;
    void reset_state() const;
};

} // namespace kimix::builtin_tools::regex_lite
