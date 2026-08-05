/*
 * line_processor.cpp — implementation of the line stream processor.
 *
 * Compiled into runtime.dll by the recursive glob in src/ext/xmake.lua.
 */

#include <runtime/stream/line_processor.h>

#include <runtime/common/utf8.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace stream {

namespace {

// Normalize CRLF / lone CR to LF in one pass. Mirror of
// `text.replace("\r\n", "\n").replace("\r", "\n")` — a lone '\r' always
// becomes '\n' regardless of what follows (the two replaces in that order).
void crlf_normalize(kimix::string_view utf8, kimix::string& out) {
    const char* it = utf8.data();
    const char* end = it + utf8.size();
    while (it < end) {
        const char b = *it;
        if (b == '\r') {
            out.push_back('\n');
            if (it + 1 < end && it[1] == '\n') {
                ++it; // CRLF -> single LF
            }
        } else {
            out.push_back(b);
        }
        ++it;
    }
}

bool block_eq(const kimix::vector<kimix::string>& lines, size_t a, size_t b, size_t h) {
    for (size_t k = 0; k < h; ++k) {
        if (lines[a + k] != lines[b + k]) {
            return false;
        }
    }
    return true;
}

// `f"{line}  ({cnt} repeats)"` — the dedup annotation (two spaces).
kimix::string annotate(kimix::string_view line, uint64_t cnt) {
    kimix::string out(line);
    out.append("  (");
    out.append(std::to_string(cnt));
    out.append(" repeats)");
    return out;
}

} // namespace

LineProcessor::LineProcessor(const process_options& opts) : opts_(opts) {}

void LineProcessor::reset() {
    ansi_.reset();
    line_buf_.clear();
    pending_cr_ = false;
    lines_.clear();
    budget_exhausted_ = false;
    bytes_written_ = 0;
    code_points_written_ = 0;
    lines_written_ = 0;
}

void LineProcessor::emit_segment(kimix::string_view segment,
                                 kimix::vector<kimix::string>* out_lines) {
    if (budget_exhausted_) {
        return;
    }
    if (opts_.max_lines > 0 && lines_written_ >= opts_.max_lines) {
        budget_exhausted_ = true;
        return;
    }
    if (opts_.max_bytes > 0 && bytes_written_ + segment.size() > opts_.max_bytes) {
        budget_exhausted_ = true;
        return;
    }
    if (out_lines != nullptr) {
        out_lines->emplace_back(segment);
    }
    bytes_written_ += segment.size();
    code_points_written_ += common::utf8_code_point_count(segment);
    ++lines_written_;
}

// Emit a line through fold (fold_col) and budget checks. fold_col == 0 keeps
// the line whole; fold_col > 0 cuts at code-point boundaries (never splitting
// a multi-byte char); an empty line yields one empty segment.
void LineProcessor::fold_and_emit(kimix::string_view line,
                                  kimix::vector<kimix::string>* out_lines) {
    if (opts_.fold_col == 0) {
        emit_segment(line, out_lines);
        return;
    }
    const char* it = line.data();
    const char* end = it + line.size();
    if (it == end) {
        emit_segment(kimix::string_view(), out_lines); // empty line
        return;
    }
    const char* seg_start = it;
    size_t cps = 0;
    while (it < end) {
        (void)common::decode_cp(it, end);
        ++cps;
        if (cps == opts_.fold_col) {
            emit_segment(kimix::string_view(seg_start, static_cast<size_t>(it - seg_start)),
                         out_lines);
            seg_start = it;
            cps = 0;
        }
    }
    if (seg_start < end) {
        emit_segment(kimix::string_view(seg_start, static_cast<size_t>(end - seg_start)),
                     out_lines);
    }
}

void LineProcessor::finalize_line(kimix::vector<kimix::string>* out_lines) {
    if (opts_.dedup_mode != 0) {
        // Buffered modes: keep the RAW line (dedup runs before fold).
        lines_.push_back(std::move(line_buf_));
        line_buf_.clear();
        return;
    }
    fold_and_emit(line_buf_, out_lines);
    line_buf_.clear();
}

void LineProcessor::process_cleaned(kimix::string_view cleaned,
                                    kimix::vector<kimix::string>* out_lines) {
    const char* it = cleaned.data();
    const char* end = it + cleaned.size();
    while (it < end) {
        const char b = *it;
        if (pending_cr_) {
            pending_cr_ = false;
            finalize_line(out_lines); // lone CR or CRLF = line terminator
            if (b == '\n') {
                ++it;
                continue;
            }
            // fall through and process `b` as a fresh byte
        }
        if (b == '\r') {
            pending_cr_ = true;
        } else if (b == '\n') {
            finalize_line(out_lines);
        } else {
            line_buf_.push_back(b);
        }
        ++it;
    }
}

void LineProcessor::feed(kimix::string_view chunk,
                         kimix::vector<kimix::string>* out_lines) {
    kimix::string cleaned;
    if (opts_.strip_ansi) {
        ansi_.feed(chunk, cleaned);
    } else if (!chunk.empty()) {
        cleaned.append(chunk.data(), chunk.size());
    }
    process_cleaned(cleaned, out_lines);
}

// Dedup emission (modes 1/2): exactly `_dedup_output` from tools/common.py.
// Python dispatches on `max_block_lines <= 1` -> counter mode, so dedup_mode=2
// with block_window <= 1 also uses the counter algorithm (parity requirement).
void LineProcessor::emit_deduped(kimix::vector<kimix::string>* out_lines) {
    const size_t n = lines_.size();
    if (n == 0) {
        return;
    }
    if (opts_.dedup_mode == 1 || opts_.block_window <= 1) {
        // Counter mode: Counter(lines) totals; keep the FIRST occurrence of
        // lines appearing more than `threshold` times, annotated.
        kimix::map<kimix::string, uint32_t> counts;
        for (const auto& line : lines_) {
            ++counts[line];
        }
        kimix::set<kimix::string> emitted;
        for (const auto& line : lines_) {
            const uint32_t cnt = counts[line];
            if (cnt > opts_.threshold) {
                if (!emitted.contains(line)) {
                    emitted.insert(line);
                    fold_and_emit(annotate(line, cnt), out_lines);
                }
            } else {
                fold_and_emit(line, out_lines);
            }
        }
    } else {
        // Block mode: greedy largest-block-first contiguous run detection
        // with `consumed` marking (exact Python algorithm, O(n·h) compares).
        kimix::bitvector consumed(n, false);
        kimix::vector<kimix::string> result;
        size_t i = 0;
        while (i < n) {
            if (consumed[i]) {
                ++i;
                continue;
            }
            bool collapsed = false;
            const size_t h_max =
                opts_.block_window < (n - i) ? opts_.block_window : (n - i);
            for (size_t h = h_max; h >= 1; --h) {
                // Count contiguous repeats starting at i. The content
                // comparison happens BEFORE the consumed check, exactly like
                // `while j + h <= n and tuple(lines[j:j+h]) == block:`.
                size_t j = i;
                size_t repeats = 0;
                while (j + h <= n && block_eq(lines_, i, j, h)) {
                    bool any_consumed = false;
                    for (size_t k = j; k < j + h; ++k) {
                        if (consumed[k]) {
                            any_consumed = true;
                            break;
                        }
                    }
                    if (any_consumed) {
                        break;
                    }
                    ++repeats;
                    j += h;
                }
                if (repeats > opts_.threshold) {
                    // Emit one copy of the block; the annotation goes on the
                    // last line of the kept block.
                    for (size_t k = 0; k + 1 < h; ++k) {
                        result.push_back(lines_[i + k]);
                        consumed[i + k] = true;
                    }
                    result.push_back(annotate(lines_[i + h - 1], repeats));
                    for (size_t k = 0; k < h * repeats; ++k) {
                        consumed[i + k] = true;
                    }
                    i = i + h * repeats;
                    collapsed = true;
                    break;
                }
            }
            if (!collapsed) {
                result.push_back(lines_[i]);
                consumed[i] = true;
                ++i;
            }
        }
        for (const auto& line : result) {
            fold_and_emit(line, out_lines);
        }
    }
    lines_.clear();
}

void LineProcessor::flush(kimix::vector<kimix::string>* out_lines) {
    // 1. ANSI stripper end-of-stream (emits bytes held by unterminated
    //    escapes — they may complete the final line).
    kimix::string cleaned;
    ansi_.flush(cleaned);
    process_cleaned(cleaned, out_lines);

    // 2. A pending lone CR at EOF is a line terminator.
    if (pending_cr_) {
        pending_cr_ = false;
        finalize_line(out_lines);
    }

    // 3. Final unterminated line (a trailing '\n' leaves line_buf_ empty and
    //    must NOT produce an extra empty line).
    if (!line_buf_.empty()) {
        finalize_line(out_lines);
    }

    // 4. Dedup modes: buffered lines are emitted at flush.
    if (opts_.dedup_mode != 0 && !lines_.empty()) {
        emit_deduped(out_lines);
    }
}

kimix::string filter_output(kimix::string_view utf8) {
    kimix::string stripped = strip_ansi(utf8);
    kimix::string out;
    crlf_normalize(stripped, out);
    return out;
}

} // namespace stream
} // namespace runtime
} // namespace kimix
