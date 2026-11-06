/*
 * kmb2.h - the packed message-structure boundary ABI ("KMB2").
 *
 * Report §2 rule: a native entry point takes BYTES + SPAN TABLES or a
 * persistent native handle, never a `py::list[py::dict]` (measured: a dict
 * bridge costs 5.7x the peak of the Python path it replaces). KMB2 is the one
 * packed representation every soul kernel in this module parses.
 *
 * Layout (little-endian, 4-byte aligned, every offset relative to the
 * companion `history` bytes):
 *
 *   header (32 B)
 *     0  u32 magic 'KMB2'        4  u32 version = 1
 *     8  u32 msg_count          12  u32 part_total
 *    16  u32 call_total         20  u32 flags (bit0 = history is UTF-8)
 *    24  u32 reserved           28  u32 reserved
 *
 *   message table (32 B each, msg_count entries, at offset 32)
 *     0  u8  role (0 system 1 user 2 assistant 3 tool)
 *     1  u8  flags (bit0 tool_call_id present, bit1 tool_calls present,
 *                   bit2 has a think part)
 *     2  u16 reserved
 *     4  u32 part_first          8  u32 part_count
 *    12  u32 call_first         16  u32 call_count
 *    20  i32 tcid_start         24  i32 tcid_end      (-1/-1 = None)
 *    28  u32 reserved
 *
 *   part table (12 B each, part_total entries, after the message table)
 *     0  i32 start    4  i32 end    8  u8 kind (part_kind)   9  u8 flags
 *    10  u16 reserved
 *
 *   call table (24 B each, call_total entries, after the part table)
 *     0  i32 id_s      4  i32 id_e      8  i32 name_s    12  i32 name_e
 *    16  i32 args_s   20  i32 args_e    (args_s == -1 => arguments is None)
 *
 * A 750-message history (1 500 parts, 750 calls) is 32 + 24 000 + 18 000 +
 * 18 000 = 60 032 B in ONE Python object, against ~1.2 MB of tuples/lists for
 * the legacy dict bridge. Both decode to the same
 * `kimix::span<const message_view>`, which is what every kernel consumes.
 *
 * Records are read with memcpy-based accessors, never by dereferencing a cast
 * pointer: a Python `bytes` payload is not guaranteed 4-byte aligned.
 *
 * Example:
 *   kimix::string structure, history;
 *   kimix::runtime::soul::kmb2_build(views, structure, history);
 *   kimix::runtime::soul::kmb2_view v;
 *   if (kmb2_parse(structure, history, v)) {
 *       kmb2_message_record m;
 *       if (v.message(0, m)) { ... }
 *   }
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports. Every count and span coming from
 * Python is bounds-checked; kmb2_parse() is total (false = malformed).
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

inline constexpr uint32_t kKmb2Magic = 0x32424D4Bu; // 'K','M','B','2' (LE)
inline constexpr uint32_t kKmb2Version = 1u;
inline constexpr size_t kKmb2HeaderBytes = 32;
inline constexpr size_t kKmb2MessageBytes = 32;
inline constexpr size_t kKmb2PartBytes = 12;
inline constexpr size_t kKmb2CallBytes = 24;

// Message-record flags (plain constants, not an anonymous enum: a unity batch
// that merges several files defining `enum : uint8_t` is a redefinition).
inline constexpr uint8_t kKmb2MsgHasToolCallId = 1u << 0;
inline constexpr uint8_t kKmb2MsgHasToolCalls = 1u << 1;
inline constexpr uint8_t kKmb2MsgHasThink = 1u << 2;
inline constexpr uint32_t kKmb2FlagUtf8 = 1u << 0;

// One message record, laid out exactly as on the wire.
struct kmb2_message_record {
    uint8_t role;
    uint8_t flags;
    uint16_t reserved;
    uint32_t part_first;
    uint32_t part_count;
    uint32_t call_first;
    uint32_t call_count;
    int32_t tcid_start;
    int32_t tcid_end;
    uint32_t reserved2;
};

struct kmb2_part_record {
    int32_t start;
    int32_t end;
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
};

struct kmb2_call_record {
    int32_t id_s;
    int32_t id_e;
    int32_t name_s;
    int32_t name_e;
    int32_t args_s; // -1 => None
    int32_t args_e;
};

static_assert(sizeof(kmb2_message_record) == kKmb2MessageBytes,
              "KMB2 message record must stay 32 bytes");
static_assert(sizeof(kmb2_part_record) == kKmb2PartBytes,
              "KMB2 part record must stay 12 bytes");
static_assert(sizeof(kmb2_call_record) == kKmb2CallBytes,
              "KMB2 call record must stay 24 bytes");

// Borrowed decode: every span refers into `history`, whose bytes the caller
// must keep alive (and unmodified) for as long as `out` is used.
struct kmb2_view {
    kimix::string_view structure;
    kimix::string_view history;
    const char *message_table = nullptr;
    const char *part_table = nullptr;
    const char *call_table = nullptr;
    uint32_t msg_count = 0;
    uint32_t part_total = 0;
    uint32_t call_total = 0;
    uint32_t flags = 0;

    KIMIX_RUNTIME_API bool message(uint32_t index,
                                   kmb2_message_record &out) const noexcept;
    KIMIX_RUNTIME_API bool part(uint32_t index, kmb2_part_record &out) const noexcept;
    KIMIX_RUNTIME_API bool call(uint32_t index, kmb2_call_record &out) const noexcept;

    // Span of `history` addressed by an (start, end) pair already validated
    // by kmb2_parse(); returns an empty view for a negative start.
    KIMIX_RUNTIME_API kimix::string_view span(int32_t start, int32_t end) const noexcept;
};

// Decode + fully validate a KMB2 blob pair. False on any malformed input
// (wrong magic/version, truncated tables, a span outside `history`, a part
// kind outside part_kind, a half-present tool_call_id). Never allocates.
// `out` is only written on success.
KIMIX_RUNTIME_API bool kmb2_parse(kimix::string_view structure,
                                  kimix::string_view history,
                                  kmb2_view &out) noexcept;

// True when the blob starts with the KMB2 magic and has a full header.
KIMIX_RUNTIME_API bool kmb2_is_packed(kimix::string_view structure) noexcept;

// Incremental encoder. Records are accumulated in the writer and one blob is
// written by finish(), so a caller may add messages in a loop and pay one
// allocation at the end.
class KIMIX_RUNTIME_API kmb2_writer {
public:
    kmb2_writer(kimix::string &structure, kimix::string &history) noexcept;

    // Copy the referenced bytes into `history` and record the resulting spans.
    void add(uint8_t role, kimix::span<const part_view> parts,
             kimix::span<const tool_call_view> calls,
             kimix::string_view tool_call_id,
             bool has_tool_call_id) noexcept;

    // Record spans that ALREADY point into the caller's `history` buffer (the
    // dict -> packed re-encoder, whose history object is not ours).
    void add_raw(uint8_t role, kimix::span<const kmb2_part_record> parts,
                 kimix::span<const kmb2_call_record> calls,
                 int32_t tcid_start, int32_t tcid_end) noexcept;

    // Write header + tables into `structure` (which is cleared first).
    void finish() noexcept;

    uint32_t msg_count() const noexcept { return _msg_count; }

private:
    int32_t put(kimix::string_view v) noexcept;

    kimix::string &_structure;
    kimix::string &_history;
    kimix::vector<kmb2_message_record> _msgs;
    kimix::vector<kmb2_part_record> _parts;
    kimix::vector<kmb2_call_record> _calls;
    uint32_t _msg_count = 0;
    uint32_t _part_total = 0;
    uint32_t _call_total = 0;
    bool _overflow = false;
};

// Encode one message_view (copies its spans into the writer's history).
KIMIX_RUNTIME_API void kmb2_encode_message(kmb2_writer &w,
                                           const message_view &mv) noexcept;

// Encode a whole span into a fresh blob pair: both strings are cleared first.
KIMIX_RUNTIME_API void kmb2_build(kimix::span<const message_view> msgs,
                                  kimix::string &structure,
                                  kimix::string &history) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
