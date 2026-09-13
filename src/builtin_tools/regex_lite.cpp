// regex_lite.cpp - Minimal backtracking regex engine implementation
// (see regex_lite.h for the supported syntax).
//
// Compilation is Thompson-style: each parse function returns a `frag` (entry
// node + dangling continuation tails). Fragments are concatenated / unioned by
// patching dangling `next` / `alt` fields, so no global "patch every -1"
// passes are needed. Matching is a recursive backtracking VM (Pike's machine,
// virtual-machine-interpretation style) with a per-split repetition counter so
// {n,m} bounds are enforced without node duplication.

#include "builtin_tools/regex_lite.h"

#include <cstddef>
#include <limits>
#include <utility>

namespace kimix::builtin_tools::regex_lite {

namespace {

constexpr size_t k_unset = std::numeric_limits<size_t>::max();
constexpr int32_t k_max_bound = 10000; // {n,m} bound cap

char32_t rx_fold(char32_t c) noexcept {
    return (c >= U'A' && c <= U'Z') ? (c - U'A' + U'a') : c;
}

} // namespace

char32_t decode_utf8(kimix::string_view text, size_t &pos) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const size_t n = text.size();
    if (pos >= n) {
        return 0;
    }
    const unsigned char b0 = bytes[pos];
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    size_t extra = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        extra = 1;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3;
        cp = b0 & 0x07u;
    } else {
        ++pos; // invalid lead byte: single-byte fallback
        return b0;
    }
    if (pos + extra >= n) {
        ++pos; // truncated sequence
        return b0;
    }
    for (size_t i = 1; i <= extra; ++i) {
        const unsigned char bi = bytes[pos + i];
        if ((bi & 0xC0) != 0x80) {
            ++pos;
            return b0;
        }
        cp = (cp << 6) | static_cast<char32_t>(bi & 0x3Fu);
    }
    pos += extra + 1;
    return cp;
}

int32_t Regex::alloc_node() {
    node n;
    _nodes.push_back(std::move(n));
    return static_cast<int32_t>(_nodes.size()) - 1;
}

void Regex::patch(const frag &f, int32_t target) {
    for (const auto &[pc, is_alt] : f.tails) {
        node &nd = _nodes[static_cast<size_t>(pc)];
        if (is_alt) {
            nd.alt = target;
        } else {
            nd.next = target;
        }
    }
}

// a followed by b: patch a's tails to b's head. An empty frag (head == -1)
// contributes no nodes; concatenation with it is the identity.
Regex::frag Regex::concat(frag a, const frag &b) {
    if (a.head == -1) {
        return b;
    }
    if (b.head == -1) {
        return a;
    }
    patch(a, b.head);
    a.tails = b.tails; // a's tails are resolved now; only b's dangle
    return a;
}

// alternation := seq ('|' seq)*
bool Regex::parse_alt(kimix::string_view pat, size_t &pos, frag &out,
                      kimix::string &error) {
    frag first;
    if (!parse_seq(pat, pos, first, error)) {
        return false;
    }
    if (pos >= pat.size() || pat[pos] != '|') {
        out = first;
        return true;
    }
    kimix::vector<frag> branches;
    branches.push_back(first);
    while (pos < pat.size() && pat[pos] == '|') {
        ++pos;
        frag next;
        if (!parse_seq(pat, pos, next, error)) {
            return false;
        }
        branches.push_back(next);
    }
    // Union right-to-left: alt(b0, alt(b1, ...)).
    frag acc = branches.back();
    for (size_t i = branches.size() - 1; i-- > 0;) {
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::alt;
        _nodes[static_cast<size_t>(a)].next = branches[i].head;
        _nodes[static_cast<size_t>(a)].alt = acc.head;
        frag u;
        u.head = a;
        u.tails = branches[i].tails;
        u.tails.insert(u.tails.end(), acc.tails.begin(), acc.tails.end());
        acc = std::move(u);
    }
    out = acc;
    return true;
}

// seq := quant*
bool Regex::parse_seq(kimix::string_view pat, size_t &pos, frag &out,
                      kimix::string &error) {
    out.head = -1;
    out.tails.clear();
    while (pos < pat.size() && pat[pos] != '|' && pat[pos] != ')') {
        frag atom;
        if (!parse_quant(pat, pos, atom, error)) {
            return false;
        }
        if (out.head == -1) {
            out = atom;
        } else {
            out = concat(std::move(out), atom);
        }
    }
    return true;
}

// quant := atom quantifier*
bool Regex::parse_quant(kimix::string_view pat, size_t &pos, frag &out,
                        kimix::string &error) {
    if (!parse_atom(pat, pos, out, error)) {
        return false;
    }
    while (pos < pat.size()) {
        const char c = pat[pos];
        int32_t min_rep = 0;
        int32_t max_rep = -1;
        bool is_quant = true;
        switch (c) {
        case '*':
            min_rep = 0;
            max_rep = -1;
            ++pos;
            break;
        case '+':
            min_rep = 1;
            max_rep = -1;
            ++pos;
            break;
        case '?':
            min_rep = 0;
            max_rep = 1;
            ++pos;
            break;
        case '{': {
            size_t j = pos + 1;
            int32_t lo = 0;
            bool have_lo = false;
            while (j < pat.size() && pat[j] >= '0' && pat[j] <= '9') {
                lo = lo * 10 + (pat[j] - '0');
                have_lo = true;
                ++j;
            }
            if (!have_lo) {
                is_quant = false; // literal '{'
                break;
            }
            int32_t hi = lo;
            if (j < pat.size() && pat[j] == ',') {
                ++j;
                hi = -1;
                int32_t hi_val = 0;
                bool have_hi = false;
                while (j < pat.size() && pat[j] >= '0' && pat[j] <= '9') {
                    hi_val = hi_val * 10 + (pat[j] - '0');
                    have_hi = true;
                    ++j;
                }
                if (have_hi) {
                    hi = hi_val;
                }
            }
            if (j >= pat.size() || pat[j] != '}') {
                is_quant = false;
                break;
            }
            if (hi >= 0 && hi < lo) {
                error = "invalid repetition range (max < min)";
                return false;
            }
            if (lo > k_max_bound || (hi > k_max_bound)) {
                error = "quantifier bounds too large";
                return false;
            }
            min_rep = lo;
            max_rep = hi;
            pos = j + 1;
            break;
        }
        default:
            is_quant = false;
            break;
        }
        if (!is_quant) {
            break;
        }
        bool lazy = false;
        if (pos < pat.size() && pat[pos] == '?') {
            lazy = true;
            ++pos;
        } else if (pos < pat.size() && pat[pos] == '+') {
            error = "possessive quantifiers are not supported";
            return false;
        }
        // split_rep -> body -> loop(back to split_rep); cont = split_rep.alt.
        // Empty bodies (e.g. `(a*)*` outer over an empty inner) can spin; the
        // VM breaks the loop when a body iteration does not consume input and
        // min_rep was already satisfied (same guard semantics as PCRE).
        const int32_t split = alloc_node();
        node &sn = _nodes[static_cast<size_t>(split)];
        sn.kind = op_kind::split_rep;
        sn.next = out.head == -1 ? -1 : out.head;
        sn.alt = -1; // continuation, dangling
        sn.min_rep = min_rep;
        sn.max_rep = max_rep;
        sn.lazy = lazy;
        sn.rep_slot = static_cast<int32_t>(_split_pcs.size());
        _split_pcs.push_back(split);
        if (out.head == -1) {
            // Empty body: the split is its own tail (loop skipped).
            out.head = split;
            out.tails.push_back({split, true}); // alt (cont) dangling
        } else {
            const int32_t lp = alloc_node();
            node &ln = _nodes[static_cast<size_t>(lp)];
            ln.kind = op_kind::loop;
            ln.next = split; // jump back
            patch(out, lp);
            frag f;
            f.head = split;
            f.tails.push_back({split, true}); // cont (alt) dangling
            out = std::move(f);
        }
    }
    return true;
}

// atom := '(' alt ')' | '[' class ']' | '.' | '^' | '$' | escape | literal
bool Regex::parse_atom(kimix::string_view pat, size_t &pos, frag &out,
                       kimix::string &error) {
    out.head = -1;
    out.tails.clear();
    if (pos >= pat.size()) {
        error = "unexpected end of pattern";
        return false;
    }
    const char c = pat[pos];
    switch (c) {
    case '(': {
        ++pos;
        bool capturing = true;
        if (pos + 1 < pat.size() && pat[pos] == '?') {
            if (pat[pos + 1] == ':') {
                capturing = false;
                pos += 2;
            } else {
                error = "unsupported group syntax (look-around / named groups)";
                return false;
            }
        }
        int32_t group_index = -1;
        if (capturing) {
            group_index = static_cast<int32_t>(++_groups); // 1-based (PCRE)
        }
        const int32_t open = alloc_node();
        _nodes[static_cast<size_t>(open)].kind = op_kind::group_open;
        _nodes[static_cast<size_t>(open)].group = group_index;
        frag inner;
        if (!parse_alt(pat, pos, inner, error)) {
            return false;
        }
        if (pos >= pat.size() || pat[pos] != ')') {
            error = "missing closing ')'";
            return false;
        }
        ++pos;
        const int32_t close = alloc_node();
        _nodes[static_cast<size_t>(close)].kind = op_kind::group_close;
        _nodes[static_cast<size_t>(close)].group = group_index;
        frag f_open;
        f_open.head = open;
        // open's tail is `next`.
        f_open.tails.push_back({open, false});
        frag combined = concat(std::move(f_open), inner);
        combined = concat(std::move(combined), [&] {
            frag f_close;
            f_close.head = close;
            f_close.tails.push_back({close, false});
            return f_close;
        }());
        out = combined;
        return true;
    }
    case '^': {
        ++pos;
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::anchor_bos;
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    case '$': {
        ++pos;
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::anchor_eos;
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    case '.': {
        ++pos;
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::any;
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    case '[': {
        ++pos;
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::class_;
        if (!parse_class_body(pat, pos, _nodes[static_cast<size_t>(a)], error)) {
            return false;
        }
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    case '\\': {
        ++pos;
        if (pos >= pat.size()) {
            error = "trailing backslash";
            return false;
        }
        const char e = pat[pos];
        ++pos;
        const int32_t a = alloc_node();
        node &nd = _nodes[static_cast<size_t>(a)];
        switch (e) {
        case 'd':
        case 'D':
            nd.kind = op_kind::class_;
            nd.ranges = {{U'0', U'9'}};
            nd.negated = (e == 'D');
            break;
        case 'w':
        case 'W':
            nd.kind = op_kind::class_;
            nd.ranges = {{U'0', U'9'}, {U'A', U'Z'}, {U'_', U'_'}, {U'a', U'z'}};
            nd.negated = (e == 'W');
            break;
        case 's':
        case 'S':
            nd.kind = op_kind::class_;
            nd.ranges = {{U'\t', U'\r'}, {U' ', U' '}, {0x85, 0x85}, {0xA0, 0xA0}};
            nd.negated = (e == 'S');
            break;
        case 'n':
            nd.kind = op_kind::char_;
            nd.ch = U'\n';
            break;
        case 't':
            nd.kind = op_kind::char_;
            nd.ch = U'\t';
            break;
        case 'r':
            nd.kind = op_kind::char_;
            nd.ch = U'\r';
            break;
        case 'f':
            nd.kind = op_kind::char_;
            nd.ch = U'\f';
            break;
        case 'v':
            nd.kind = op_kind::char_;
            nd.ch = U'\v';
            break;
        case '0':
            nd.kind = op_kind::char_;
            nd.ch = U'\0';
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            error = "back-references are not supported";
            return false;
        case 'p':
        case 'P':
        case 'b':
        case 'B':
        case 'A':
        case 'Z':
        case 'z':
            error = kimix::string("unsupported escape: \\") + e;
            return false;
        default:
            nd.kind = op_kind::char_;
            nd.ch = static_cast<char32_t>(static_cast<unsigned char>(e));
            break;
        }
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    case '*':
    case '+':
    case '?':
        error = "quantifier without operand";
        return false;
    case ')':
        error = "unmatched ')'";
        return false;
    default: {
        // Literal: decode one UTF-8 code point (multi-byte safe).
        const char32_t cp = decode_utf8(pat, pos);
        const int32_t a = alloc_node();
        _nodes[static_cast<size_t>(a)].kind = op_kind::char_;
        _nodes[static_cast<size_t>(a)].ch = cp;
        out.head = a;
        out.tails.push_back({a, false});
        return true;
    }
    }
}

bool Regex::parse_class_body(kimix::string_view pat, size_t &pos, node &out,
                             kimix::string &error) {
    out.ranges.clear();
    out.negated = false;
    if (pos < pat.size() && (pat[pos] == '^' || pat[pos] == '!')) {
        out.negated = true;
        ++pos;
    }
    bool first = true;
    while (pos < pat.size()) {
        if (pat[pos] == ']' && !first) {
            ++pos;
            return true;
        }
        first = false;
        char32_t lo = 0;
        if (pat[pos] == '\\') {
            ++pos;
            if (pos >= pat.size()) {
                error = "trailing backslash in class";
                return false;
            }
            const char e = pat[pos];
            ++pos;
            switch (e) {
            case 'n':
                lo = U'\n';
                break;
            case 't':
                lo = U'\t';
                break;
            case 'r':
                lo = U'\r';
                break;
            case 'f':
                lo = U'\f';
                break;
            case 'v':
                lo = U'\v';
                break;
            case 'd':
                out.ranges.push_back({U'0', U'9'});
                continue;
            case 'w':
                out.ranges.push_back({U'0', U'9'});
                out.ranges.push_back({U'A', U'Z'});
                out.ranges.push_back({U'_', U'_'});
                out.ranges.push_back({U'a', U'z'});
                continue;
            case 's':
                out.ranges.push_back({U'\t', U'\r'});
                out.ranges.push_back({U' ', U' '});
                continue;
            default:
                lo = static_cast<char32_t>(static_cast<unsigned char>(e));
                break;
            }
        } else {
            lo = decode_utf8(pat, pos);
        }
        // Range?
        if (pos + 1 < pat.size() && pat[pos] == '-' && pat[pos + 1] != ']') {
            ++pos; // '-'
            char32_t hi = 0;
            if (pat[pos] == '\\') {
                ++pos;
                if (pos >= pat.size()) {
                    error = "trailing backslash in class range";
                    return false;
                }
                const char e = pat[pos];
                ++pos;
                switch (e) {
                case 'n':
                    hi = U'\n';
                    break;
                case 't':
                    hi = U'\t';
                    break;
                case 'r':
                    hi = U'\r';
                    break;
                default:
                    hi = static_cast<char32_t>(static_cast<unsigned char>(e));
                    break;
                }
            } else {
                hi = decode_utf8(pat, pos);
            }
            if (hi < lo) {
                error = "invalid class range (end before start)";
                return false;
            }
            out.ranges.push_back({lo, hi});
        } else {
            out.ranges.push_back({lo, lo});
        }
    }
    error = "missing closing ']'";
    return false;
}

bool Regex::compile(kimix::string_view pattern, bool ignore_case,
                    kimix::string &error) {
    error.clear();
    _nodes.clear();
    _split_pcs.clear();
    _valid = false;
    _ignore_case = ignore_case;
    _groups = 0;
    size_t pos = 0;
    frag body;
    if (!parse_alt(pattern, pos, body, error)) {
        return false;
    }
    if (pos != pattern.size()) {
        error = "trailing garbage in pattern";
        return false;
    }
    const int32_t end = alloc_node();
    _nodes[static_cast<size_t>(end)].kind = op_kind::match_end;
    patch(body, end);
    if (body.head == -1) {
        _head = end; // empty pattern matches everywhere
    } else {
        _head = body.head;
    }
    _caps.assign((_groups + 1) * 2, k_unset);
    _rep_counts.assign(_split_pcs.size(), 0);
    _valid = true;
    return true;
}

bool Regex::match_char(const node &n, char32_t c) const {
    if (n.kind == op_kind::char_) {
        if (n.ch == c) {
            return true;
        }
        return _ignore_case && rx_fold(n.ch) == rx_fold(c);
    }
    bool inside = false;
    for (const auto &[lo, hi] : n.ranges) {
        if (c >= lo && c <= hi) {
            inside = true;
            break;
        }
        if (_ignore_case) {
            const char32_t f = rx_fold(c);
            if (f >= rx_fold(lo) && f <= rx_fold(hi)) {
                inside = true;
                break;
            }
            if (rx_fold(c) >= lo && rx_fold(c) <= hi) {
                inside = true;
                break;
            }
        }
    }
    return n.negated ? !inside : inside;
}

void Regex::reset_state() const {
    for (size_t &c : _caps) {
        c = k_unset;
    }
    for (int32_t &r : _rep_counts) {
        r = 0;
    }
    _consumed = 0;
}

// Backtracking VM.
bool Regex::run(kimix::string_view text, int32_t pc, size_t pos) const {
    while (true) {
        if (pc < 0) {
            return false;
        }
        const node &n = _nodes[static_cast<size_t>(pc)];
        switch (n.kind) {
        case op_kind::empty:
            pc = n.next;
            continue;
        case op_kind::char_:
        case op_kind::class_: {
            if (pos >= text.size()) {
                return false;
            }
            size_t next_pos = pos;
            const char32_t c = decode_utf8(text, next_pos);
            if (!match_char(n, c)) {
                return false;
            }
            pos = next_pos;
            ++_consumed;
            pc = n.next;
            continue;
        }
        case op_kind::any: {
            if (pos >= text.size()) {
                return false;
            }
            size_t next_pos = pos;
            const char32_t c = decode_utf8(text, next_pos);
            if (c == U'\n') {
                return false;
            }
            pos = next_pos;
            ++_consumed;
            pc = n.next;
            continue;
        }
        case op_kind::group_open: {
            if (n.group < 0) {
                pc = n.next;
                continue;
            }
            const size_t slot = static_cast<size_t>(n.group) * 2;
            const size_t saved = _caps[slot];
            _caps[slot] = pos;
            const bool r = run(text, n.next, pos);
            if (!r) {
                _caps[slot] = saved;
            }
            return r;
        }
        case op_kind::group_close: {
            if (n.group < 0) {
                pc = n.next;
                continue;
            }
            const size_t slot = static_cast<size_t>(n.group) * 2 + 1;
            const size_t saved = _caps[slot];
            _caps[slot] = pos;
            const bool r = run(text, n.next, pos);
            if (!r) {
                _caps[slot] = saved;
            }
            return r;
        }
        case op_kind::alt: {
            if (run(text, n.next, pos)) {
                return true;
            }
            pc = n.alt;
            continue;
        }
        case op_kind::split_rep: {
            int32_t &count = _rep_counts[static_cast<size_t>(n.rep_slot)];
            const int32_t body = n.next;
            const int32_t cont = n.alt;
            if (body < 0) {
                // Empty body: iterating would spin forever; zero reps.
                pc = cont;
                continue;
            }
            const bool under_min = (count < n.min_rep);
            const bool over_max = (n.max_rep >= 0 && count >= n.max_rep);
            if (!over_max) {
                if (n.lazy && !under_min) {
                    if (run(text, cont, pos)) {
                        return true;
                    }
                }
                ++count;
                const int64_t consumed_before = _consumed;
                bool r = run(text, body, pos);
                --count;
                if (r && _consumed == consumed_before) {
                    // Zero-width body iteration: looping again would spin, so
                    // accept through the continuation instead.
                    r = run(text, cont, pos);
                }
                if (r) {
                    return true; // body success already includes `cont`
                }
                if (under_min) {
                    return false;
                }
            }
            pc = cont; // greedy exhaustion / lazy-after-body-failure
            continue;
        }
        case op_kind::loop:
            pc = n.next; // jump back to the split_rep node
            continue;
        case op_kind::anchor_bos: {
            if (pos != 0 && text[pos - 1] != '\n') {
                return false;
            }
            pc = n.next;
            continue;
        }
        case op_kind::anchor_eos: {
            if (pos != text.size() && !(pos < text.size() && text[pos] == '\n')) {
                return false;
            }
            pc = n.next;
            continue;
        }
        case op_kind::match_end:
            _match_end = pos;
            return true;
        }
        return false;
    }
}

bool Regex::search(kimix::string_view text, size_t &out_begin, size_t &out_end,
                   size_t start) const {
    if (!_valid) {
        return false;
    }
    for (size_t pos = start; pos <= text.size();) {
        reset_state();
        _match_end = pos;
        if (run(text, _head, pos)) {
            out_begin = pos;
            out_end = _match_end;
            _caps[0] = pos;
            _caps[1] = _match_end;
            return true;
        }
        if (pos == text.size()) {
            break;
        }
        decode_utf8(text, pos); // advance one code point
    }
    return false;
}

bool Regex::full_match(kimix::string_view text) const {
    size_t b = 0;
    size_t e = 0;
    if (!search(text, b, e)) {
        return false;
    }
    return b == 0 && e == text.size();
}

bool Regex::last_group(size_t index, size_t &out_begin, size_t &out_end) const {
    if (index > _groups || _caps.size() < (index + 1) * 2) {
        return false;
    }
    out_begin = _caps[index * 2];
    out_end = _caps[index * 2 + 1];
    return out_begin != k_unset && out_end != k_unset;
}

} // namespace kimix::builtin_tools::regex_lite
