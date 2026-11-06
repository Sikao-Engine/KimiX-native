/*
 * history_store.h - the native conversation-history arena (runtime_py.soul).
 *
 * Report candidate A1. Today the history is a pydantic Message/ContentPart
 * graph: 4 663.8 B/msg on a realistic 750-message conversation (3.50 MB). This
 * store keeps the same information in ONE text arena plus offset tables -
 * 1 052 B/msg for the same payload - and hands `kimix::span<const
 * message_view>` to every consumer (payload builder, export, compaction,
 * persistence), so no Python object ever participates in a pass.
 *
 * Layout:
 *   _arena   : every string of every message, appended (UTF-8 bytes)
 *   _slots   : one kmb2_message_record per SLOT (role + flags + part/call
 *              ranges + tool_call_id span), exactly the KMB2 record layout
 *   _parts   : kmb2_part_record (start/end/kind) - 12 B each
 *   _calls   : kmb2_call_record (id/name/arguments spans) - 24 B each
 *   _order   : slot index of every LIVE message, in conversation order
 *
 * Indices in the public API are LIVE ORDINALS (Python list semantics), not
 * slot numbers: after drop() the ordinals stay 0..size()-1.
 *
 * Views returned by view() / views() point into the arena and are INVALIDATED
 * by every mutation; generation() increments on each one, which is how a
 * caller (and the tests) detects a stale span.
 *
 * compact() is the GC of this design: it rebuilds the arena and the tables
 * keeping only live bytes (tombstones from drop/replace/truncate disappear).
 * Nothing else ever rewrites the whole buffer, so a steady-state append storm
 * is amortised O(1) per byte.
 *
 * Example:
 *   kimix::runtime::soul::HistoryStore h;
 *   auto first = h.append_packed(structure, history);      // KMB2 -> arena
 *   for (const auto& mv : h.views()) { ... }               // zero-copy spans
 *   kimix::string structure2, history2;
 *   h.pack(0, h.size(), structure2, history2);             // KMB2 again
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports. Not thread-safe: one handle belongs
 * to one thread at a time (the binding layer releases the GIL around calls).
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

class KIMIX_RUNTIME_API HistoryStore {
public:
    explicit HistoryStore(uint64_t reserve_bytes = 0);
    ~HistoryStore() = default;
    HistoryStore(const HistoryStore &) = delete;
    HistoryStore &operator=(const HistoryStore &) = delete;

    // ---- mutate (invalidates every outstanding view; bumps generation()) ----

    // Append one message. Returns its live ordinal. The spans are copied into
    // the arena, so the caller's buffer may die immediately.
    uint32_t append(uint8_t role, kimix::span<const part_view> parts,
                    kimix::span<const tool_call_view> calls,
                    kimix::string_view tool_call_id) noexcept;

    // Append every message of a KMB2 blob pair (the Python boundary). Returns
    // the number of messages appended; 0 when the blob is malformed (nothing
    // is appended in that case).
    uint32_t append_packed(kimix::string_view structure,
                           kimix::string_view history) noexcept;

    // Replace the message at `index` in place (same conversation position).
    bool replace(uint32_t index, uint8_t role, kimix::span<const part_view> parts,
                 kimix::span<const tool_call_view> calls,
                 kimix::string_view tool_call_id) noexcept;
    bool replace_packed(uint32_t index, kimix::string_view structure,
                        kimix::string_view history) noexcept;

    // Tombstone the message at `index`. Its bytes are reclaimed by compact().
    bool drop(uint32_t index) noexcept;
    // Keep the first `count` live messages, tombstone the rest.
    void truncate(uint32_t count) noexcept;
    void clear() noexcept;
    // Rebuild the arena + tables dropping dead bytes; returns the bytes freed.
    uint64_t compact() noexcept;

    // ---- read ----

    uint32_t size() const noexcept { return static_cast<uint32_t>(_order.size()); }
    uint64_t generation() const noexcept { return _gen; }
    bool view(uint32_t index, message_view &out) const noexcept;
    // Live messages in order. The span is cached until the next mutation.
    kimix::span<const message_view> views() const noexcept;

    // ---- export / persistence ----

    // Re-encode [first, first+count) of the live messages into a KMB2 blob
    // pair. Out-of-range arguments are clamped (empty blob for an empty range).
    void pack(uint32_t first, uint32_t count, kimix::string &structure,
              kimix::string &history) const noexcept;
    // "\n".join of the message's TEXT parts (the reference Message.extract_text
    // with sep="\n").
    void text_of(uint32_t index, kimix::string &out) const noexcept;

    // "KMSH1" blob: header | arena | slot table | part table | call table.
    void save(kimix::string &blob) const noexcept;
    bool load(kimix::string_view blob) noexcept;

    // Arena + table capacity (what the handle holds), and the live span bytes.
    uint64_t bytes_owned() const noexcept;
    uint64_t bytes_live() const noexcept;

private:
    struct slot_state {
        kmb2_message_record rec;
        uint8_t live = 0;
    };

    void invalidate() noexcept { ++_gen; }
    uint32_t push_slot(uint8_t role, kimix::span<const part_view> parts,
                       kimix::span<const tool_call_view> calls,
                       kimix::string_view tool_call_id) noexcept;
    bool write_message(uint32_t slot, uint8_t role,
                       kimix::span<const part_view> parts,
                       kimix::span<const tool_call_view> calls,
                       kimix::string_view tool_call_id) noexcept;
    void build_views() const noexcept;
    int32_t append_bytes(kimix::string_view v, bool *ok) noexcept;

    kimix::string _arena;
    kimix::vector<slot_state> _slots;
    kimix::vector<kmb2_part_record> _parts;
    kimix::vector<kmb2_call_record> _calls;
    kimix::vector<uint32_t> _order; // slot index per live ordinal
    uint64_t _gen = 0;

    // Called-scratch reused by append_packed/replace_packed (no per-message
    // allocation on a 750-message append).
    kimix::vector<part_view> _scratch_parts;
    kimix::vector<tool_call_view> _scratch_calls;

    // Cached view tables (see views()).
    mutable kimix::vector<message_view> _views;
    mutable kimix::vector<part_view> _view_parts;
    mutable kimix::vector<tool_call_view> _view_calls;
    mutable uint64_t _views_gen = ~static_cast<uint64_t>(0);
};

} // namespace soul
} // namespace runtime
} // namespace kimix
