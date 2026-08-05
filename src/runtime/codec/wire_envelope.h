/*
 * wire_envelope.h -- Wire message envelope codec (kimix::runtime::codec).
 *
 * Plan 007: native replacement for the wire serialization chain -- the
 * 40-way `issubclass` scan + `model_dump(mode="json")` in
 * WireMessageEnvelope.from_wire_message (wire/types.py) and the pydantic
 * dump + orjson re-encode in _dump_line (wire/file.py).
 *
 * The envelope is `{"type": <str>, "payload": <json value>}`. The type
 * registry is DATA (a Python list of type names passed to the codec at
 * init), not code: this kernel is schema-agnostic.
 *
 * - serialize_envelope  : ONE JSON pass -- the payload is inserted as a
 *   pre-parsed value (yyjson mut_read) so it is never string-escaped and
 *   never double-parsed. Key order follows the input payload's insertion
 *   order (yyjson preserves it), matching pydantic model_dump order.
 * - deserialize_envelope: parse frame -> (type, payload_json).
 * - canonicalize_payload: recursive object-key sort + compact re-encode,
 *   matching toolset._sort_json_value + orjson.dumps semantics (used for
 *   deterministic payloads / canonical tool arguments).
 *
 * Pure C++ kernel compiled into runtime.dll: no Python includes, no RTTI,
 * kimix containers, KIMIX_RUNTIME_API exports. yyjson is available via the
 * kimix-yyjson dependency of the runtime target.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

struct wire_envelope {
    kimix::string type;        // wire message type name, e.g. "StepBegin"
    kimix::string payload_json; // payload as JSON text (any JSON value)
};

// Serialize {type, payload} to `out`: {"type":..., "payload":...}.
// The payload is embedded as a pre-parsed JSON value (ONE pass). If the
// payload text is not valid JSON it is embedded as an escaped JSON string
// so the output envelope is always parseable (only reachable on invalid
// input -- normal callers pass payloads produced by a JSON writer).
KIMIX_RUNTIME_API void serialize_envelope(const wire_envelope& e,
                                          kimix::string& out) noexcept;

// Parse a frame into (type, payload_json). False on malformed input
// (not JSON, not an object, missing "type" string or "payload").
// payload_json is the compact JSON text of the payload value (yyjson
// re-serialization; round-trip serialize->deserialize->serialize is
// byte-identical for envelopes produced by serialize_envelope).
KIMIX_RUNTIME_API bool deserialize_envelope(kimix::string_view frame,
                                            wire_envelope& out) noexcept;

// Canonicalize a JSON payload: parse, recursively sort object keys
// (sorted(key) per level, lists recursed, scalars unchanged), write back
// compact. Matches toolset._sort_json_value + orjson.dumps. On parse
// failure `out` is left empty and false is returned.
KIMIX_RUNTIME_API bool canonicalize_payload(kimix::string_view json,
                                            kimix::string& out) noexcept;

} // namespace codec
} // namespace runtime
} // namespace kimix
