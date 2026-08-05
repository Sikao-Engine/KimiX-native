/*
 * frame_writer.cpp -- see frame_writer.h (plan 007).
 *
 * Framing verified against the reference:
 *   - server.py:182  -> payload + b"\n"  (newline-delimited JSON-RPC)
 *   - file.py:155-156 -> record + "\n"   (jsonl)
 */

#include <runtime/codec/frame_writer.h>

namespace kimix {
namespace runtime {
namespace codec {

void JsonRpcFrameWriter::write(kimix::span<const char> payload,
                               kimix::string& frame) const noexcept {
    frame.clear();
    if (!payload.empty()) {
        frame.append(payload.data(), payload.size());
    }
    frame.push_back('\n');
}

void JsonRpcFrameWriter::write(kimix::string_view payload,
                               kimix::string& frame) const noexcept {
    frame.clear();
    if (!payload.empty()) {
        frame.append(payload.data(), payload.size());
    }
    frame.push_back('\n');
}

void JsonlRecorder::record(kimix::string_view envelope_frame,
                           kimix::string& out) const noexcept {
    out.clear();
    if (!envelope_frame.empty()) {
        out.append(envelope_frame.data(), envelope_frame.size());
    }
    out.push_back('\n');
}

} // namespace codec
} // namespace runtime
} // namespace kimix
