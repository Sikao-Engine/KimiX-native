/*
 * session_store.h - per-session part ring + session table (runtime_py.session).
 *
 * Report candidates A3 and C3. `kimix serve` keeps a second full copy of every
 * conversation: MessagePart/MessageInfo/MessageWithParts dataclasses rebuild
 * each event, with four `Dict[str, Any]` fields per part - 1 949 859 B of mirror
 * for 782 332 B of source text (2.49x) - and `SessionManager._sessions` is
 * never evicted.
 *
 * Here one native store owns, per session, a byte-budgeted ring of part
 * records (drop-oldest within the session budget) plus a total budget with LRU
 * eviction over whole sessions. `snapshot(session, since_seq)` renders the
 * opencode-compatible JSON in C++ (the SSE path writes the returned bytes
 * directly; `to_dict()` disappears from the hot path), and `part_json()` renders
 * one part for an incremental `message.part.updated` event.
 *
 * Record model (one part):
 *   seq        - monotonically increasing per session (the SSE resume cursor)
 *   kind       - text=0 reasoning=1 tool=2 step_start=3 step_finish=4
 *   role       - 0 user 1 assistant 2 system (MessageInfo.role)
 *   ids        - session / message / part / tool call ids
 *   text       - the part text (or the verbatim serialized payload)
 *   state/prompt/tokens/time - pre-serialized JSON objects, verbatim
 *   snapshot/reason/cost     - the step-start / step-finish fields
 *
 * Rendered JSON (byte order pinned by python/tests/data/session/*.json):
 *   snapshot = {"sessionID":str,"status":int,"seq":int,"parts":[<part>,...]}
 *   part     = {"id","sessionID","messageID","type",<kind fields>}
 * with the kind fields exactly as MessagePart.to_dict():
 *   text/reasoning : "text", ["time"]
 *   tool           : "callID", "tool", "state"
 *   step-start     : "snapshot"
 *   step-finish    : "reason", "snapshot", ["cost"], ["prompt"], ["tokens"]
 *
 * Example:
 *   kimix::runtime::soul::SessionStore store(4u << 20, 32u << 20);
 *   const uint32_t sid = store.create("ses_123");
 *   store.append(sid, record);
 *   kimix::string json;
 *   store.snapshot(sid, 0, json);
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports. Not thread-safe: a session manager
 * serialises access above this layer.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// Part kinds and session statuses (plain constants: an anonymous enum in a
// unity batch collides with every other `enum : uint8_t` in the batch).
inline constexpr uint8_t kPartText = 0;
inline constexpr uint8_t kPartReasoning = 1;
inline constexpr uint8_t kPartTool = 2;
inline constexpr uint8_t kPartStepStart = 3;
inline constexpr uint8_t kPartStepFinish = 4;

inline constexpr uint8_t kSessionIdle = 0;
inline constexpr uint8_t kSessionBusy = 1;
inline constexpr uint8_t kSessionRetry = 2;

struct part_record {
    uint32_t seq = 0;
    uint8_t kind = kPartText;
    uint8_t role = 1;
    kimix::string_view session_id;
    kimix::string_view message_id;
    kimix::string_view part_id;
    kimix::string_view call_id;
    kimix::string_view tool;
    kimix::string_view text;       // text/reasoning text, or verbatim payload
    kimix::string_view state_json; // tool state object
    kimix::string_view prompt_json;
    kimix::string_view tokens_json;
    kimix::string_view time_json;
    kimix::string_view snapshot;
    kimix::string_view reason;
    double cost = 0.0;
};

class KIMIX_RUNTIME_API SessionStore {
public:
    SessionStore(uint64_t per_session_budget_bytes, uint64_t total_budget_bytes);
    ~SessionStore() = default;
    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;

    // Idempotent create; returns the session handle (also the LRU key).
    uint32_t create(kimix::string_view session_id) noexcept;

    // Append one part. The ring drops the OLDEST parts of this session until it
    // fits `per_session_budget_bytes`; false when the single record is larger
    // than the whole per-session budget (nothing is stored then).
    bool append(uint32_t session, const part_record &rec) noexcept;

    // {"sessionID","status","seq","parts":[…]} for parts with seq >= since_seq.
    bool snapshot(uint32_t session, uint32_t since_seq,
                  kimix::string &out_json) noexcept;

    // One part rendered by part id (the incremental SSE event payload).
    bool part_json(uint32_t session, kimix::string_view part_id,
                   kimix::string &out_json) const noexcept;

    bool close(uint32_t session) noexcept;
    // Close every live session (the arena keeps its capacity).
    void clear() noexcept;
    // Drop least-recently-used sessions until the total budget fits; returns the
    // number of sessions evicted.
    uint32_t evict_to_budget() noexcept;

    uint32_t session_count() const noexcept;
    uint32_t part_count(uint32_t session) const noexcept;
    uint64_t bytes_owned() const noexcept;
    uint64_t bytes_live() const noexcept;
    uint64_t dropped_parts(uint32_t session) const noexcept;

    void set_status(uint32_t session, uint8_t status) noexcept;
    uint8_t status(uint32_t session) const noexcept;
    kimix::string_view session_id(uint32_t session) const noexcept;
    // Bumped on every append/evict/status change (a cheap resume guard).
    uint64_t generation(uint32_t session) const noexcept;

private:
    struct part_slot {
        uint32_t seq = 0;
        uint8_t kind = kPartText;
        uint8_t role = 1;
        int32_t id_start = 0; // offsets into `_arena`
        int32_t id_end = 0;
        int32_t message_start = 0;
        int32_t message_end = 0;
        int32_t part_start = 0;
        int32_t part_end = 0;
        int32_t call_start = 0;
        int32_t call_end = 0;
        int32_t tool_start = 0;
        int32_t tool_end = 0;
        int32_t text_start = 0;
        int32_t text_end = 0;
        int32_t state_start = 0;
        int32_t state_end = 0;
        int32_t prompt_start = 0;
        int32_t prompt_end = 0;
        int32_t tokens_start = 0;
        int32_t tokens_end = 0;
        int32_t time_start = 0;
        int32_t time_end = 0;
        int32_t snapshot_start = 0;
        int32_t snapshot_end = 0;
        int32_t reason_start = 0;
        int32_t reason_end = 0;
        double cost = 0.0;
    };

    struct session_slot {
        kimix::string id;
        kimix::vector<part_slot> parts; // ring order (oldest first)
        uint8_t status = kSessionIdle;
        uint32_t next_seq = 1;
        uint64_t bytes = 0;   // live text bytes of this session
        uint64_t dropped = 0; // parts dropped by the per-session ring
        uint64_t gen = 0;
        uint64_t lru = 0; // monotonic access counter
        bool live = false;
    };

    session_slot *find(uint32_t session) noexcept;
    const session_slot *find(uint32_t session) const noexcept;
    uint32_t find_by_id(kimix::string_view id) const noexcept;
    int32_t put(kimix::string_view v) noexcept;
    kimix::string_view view(int32_t start, int32_t end) const noexcept;
    uint64_t slot_bytes(const part_slot &p) const noexcept;
    void touch(session_slot &s) noexcept;

    // Shared live arena: the store is append-only, compacted when the ratio of
    // dead bytes gets high (the same policy as soul.History).
    kimix::string _arena;
    uint64_t _dead_bytes = 0;
    kimix::vector<session_slot> _sessions;
    kimix::vector<uint32_t> _free_slots;
    uint64_t _per_session_budget = 0;
    uint64_t _total_budget = 0;
    uint64_t _lru_counter = 0;
    uint32_t _live_sessions = 0;

    void compact_arena() noexcept;
};

} // namespace soul
} // namespace runtime
} // namespace kimix
