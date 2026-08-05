/*
 * frame_writer.h -- JSON-RPC frame writer + jsonl recorder (kimix::runtime::codec).
 *
 * Plan 007: serialize once, fan out to N sinks (socket + file). The
 * reference framing (verified in wire/server.py:182 and wire/file.py:155):
 *
 *   - JSON-RPC over stdio is NEWLINE-DELIMITED JSON:
 *       self._writer.write(msg.model_dump_json().encode("utf-8") + b"\n")
 *     so JsonRpcFrameWriter.write(payload) = payload + "\n" (no length
 *     prefix -- TCP length-prefixing is a separate path, see recv_buffer.h).
 *   - wire.jsonl records are one JSON object per line:
 *       _dump_line(model) = orjson.dumps(model.model_dump(mode="json")) + "\n"
 *     JsonlRecorder.record(appends the given line + "\n" -- the caller
 *     composes the full record JSON (e.g. {"timestamp": ..., "message": ...})
 *     once; the recorder adds the newline in the same write.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

// One newline-delimited JSON frame per write (JSON-RPC stdio framing).
class KIMIX_RUNTIME_API JsonRpcFrameWriter {
public:
    // frame = payload + "\n" (a single logical write to `out`).
    void write(kimix::span<const char> payload, kimix::string& frame) const noexcept;

    // Convenience overload for string-like payloads.
    void write(kimix::string_view payload, kimix::string& frame) const noexcept;
};

// One jsonl line per record (wire.jsonl recorder).
class KIMIX_RUNTIME_API JsonlRecorder {
public:
    // line = frame + "\n" -- the caller passes the already-serialized record
    // JSON (envelope or timestamp-wrapped envelope); single write, no
    // re-serialization.
    void record(kimix::string_view envelope_frame, kimix::string& out) const noexcept;
};

} // namespace codec
} // namespace runtime
} // namespace kimix
