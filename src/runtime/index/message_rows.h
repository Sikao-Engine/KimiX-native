/*
 * message_rows.h - history persistence rows over the arena
 * (kimix::runtime::index).
 *
 * Report candidate B2. `soul/context_db.py:320 append_messages` serialises each
 * message with `Message.model_dump_json(exclude_none=True)` and separately
 * extracts the FTS text (`extract_text_from_content`); the pair costs 2.07x the
 * source bytes (1 617 248 B serialised from 782 332 B of text). The restore side
 * (`get_messages`) then runs `Message.model_validate_json` per row, so a
 * 500-message load allocates 1 829 552 B and every string is a fresh copy.
 *
 * These two kernels replace exactly that translation layer:
 *
 *   context_rows(views)      -> ONE packed blob of (json row, fts text) pairs,
 *                               written in a single pass from the arena;
 *   parse_rows(blob, store)  -> decodes each stored row straight back into a
 *                               HistoryStore arena (one allocation) instead of
 *                               500 pydantic objects.
 *
 * Deliberately SERIALISATION-ONLY: the SQLite schema, the FTS5 triggers and the
 * migrations stay in Python (context_db.py owns them), so this delivery cannot
 * drift the database contract.
 *
 * Packed layout: `u32 count | per row {u32 json_len, u32 fts_len, json, fts}`.
 *
 * Row JSON contract (pinned by tests/unit/native/test_message_rows.cpp):
 *   {"role":str, "content":str|[part…], "tool_calls":[…], "tool_call_id":str}
 * - `content` is a plain string for a single TextPart (the reference
 *   `Message._serialize_content` rule), otherwise the part list;
 * - a think part serialises as {"type":"think","think":…,"encrypted":null}
 *   (model_dump, not model_dump_json: `encrypted` stays, as null) - the arena
 *   carries no signature, exactly like the message_view contract;
 * - every other part kind is embedded as the serialized JSON the arena holds
 *   (already exclude_none'd by the encoder);
 * - `tool_calls` / `tool_call_id` are omitted when absent (exclude_none).
 *
 * Example:
 *   kimix::string rows;
 *   kimix::runtime::index::context_rows(history.views(), rows);
 *   kimix::string_view view(rows.data(), rows.size());
 *   kimix::runtime::soul::HistoryStore restored;
 *   kimix::runtime::index::parse_rows(view, restored);
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/soul/history_store.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace index {

// One packed blob of (json row, FTS text) pairs - see the layout above.
KIMIX_RUNTIME_API void context_rows(
    kimix::span<const soul::message_view> msgs, kimix::string &packed) noexcept;

// Decode the packed rows into `out` (appending). Returns the number of messages
// appended. A truncated blob or a malformed ROW stops the decode: the rows
// decoded before the bad one stay appended and the return is SHORT of the
// header count, which is how the binding and the shim detect the failure (a
// restore path then discards the handle). A blob whose own framing is shorter
// than the header count appends nothing.
KIMIX_RUNTIME_API uint32_t parse_rows(kimix::string_view packed,
                                      soul::HistoryStore &out) noexcept;

} // namespace index
} // namespace runtime
} // namespace kimix
