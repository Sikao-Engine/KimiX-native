/*
 * sse.h -- SSE frame builder (kimix::runtime::codec).
 *
 * Plan 008: one JSON payload -> one SSE frame, byte-matching the reference
 * BusEvent.to_sse (src/kimix/server/bus.py:61-67):
 *
 *     return f"event: message\nid: {self.id}\ndata: {self.to_json()}\n\n"
 *
 * Field order: `event:` first, then `id:`, then `data:`, frame ends with an
 * extra blank line (\n\n). Generalizations (the codec's contract):
 *   - `event:` line is emitted only when event_name is non-empty;
 *   - `id:` line is emitted only when id != 0;
 *   - multi-line data_json produces one `data:` line per line (RFC 8895 /
 *     SSE semantics -- the reference's orjson output is single-line, but a
 *     streamed payload may contain newlines).
 *
 * Header-only kernel (all inline): no .cpp, no DLL export needed.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

inline void build_sse_frame(kimix::string_view event_name,
                            kimix::string_view data_json, uint64_t id,
                            kimix::string& out) {
    out.clear();
    if (!event_name.empty()) {
        out.append("event: ");
        out.append(event_name.data(), event_name.size());
        out.push_back('\n');
    }
    if (id != 0) {
        out.append("id: ");
        char digits[24];
        size_t n = 0;
        uint64_t v = id;
        do {
            digits[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (n > 0) {
            out.push_back(digits[--n]);
        }
        out.push_back('\n');
    }
    // Multi-line payload -> repeated data: lines.
    size_t start = 0;
    while (start <= data_json.size()) {
        size_t nl = data_json.find('\n', start);
        if (nl == kimix::string_view::npos) {
            nl = data_json.size();
        }
        out.append("data: ");
        out.append(data_json.data() + start, nl - start);
        out.push_back('\n');
        if (nl == data_json.size()) {
            break;
        }
        start = nl + 1;
    }
    out.push_back('\n'); // blank line terminates the frame
}

} // namespace codec
} // namespace runtime
} // namespace kimix
