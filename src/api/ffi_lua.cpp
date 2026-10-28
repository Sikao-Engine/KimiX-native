/*
 * ffi_lua.cpp -- implementation of the Lua script sandbox C ABI (api/ffi_lua.h).
 *
 * One job: own ONE kimix::script::Sandbox per handle plus the last Result, and
 * copy every variable-length answer into caller storage so no library pointer is
 * ever handed out and never has to be returned (api/ffi_common.h, one-heap rule).
 *
 * The whole sandbox semantics live in src/script (kimix-script); this file adds
 * no policy at all.  Three translation-unit-local conventions keep it honest:
 *
 *   - EVERY accessor goes through `copy_out`, so the two-phase protocol is
 *     implemented once: NULL/too-small -> KIMIX_ERR_OUT_OF_RANGE with the
 *     required length still written, otherwise a NUL-terminated copy whose
 *     reported length excludes the terminator.
 *   - `sb->last` is a per-handle struct member, never a global, so two handles
 *     used on two threads never share anything.
 *   - the handle derives kimix::IOperatorNewBase (core/memory.h): it lives on
 *     the mimalloc heap, the SAME heap as every kimix:: container and string
 *     inside it.  A handle on the CRT heap holding mimalloc-backed members is
 *     an accounting split mimalloc cannot see (the object itself would be
 *     invisible to mi_usable_size / a heap walk), so the operator new override
 *     is part of the one-heap rule, not a micro-optimisation.
 *
 * THE HOST SURFACE (ffi_lua.h section 6)
 * --------------------------------------
 * kimix_lua_host_bind stores the caller's C function in a per-handle SLOT and
 * binds ONE C++ trampoline (sb_host_adapter) to the sandbox, so the sandbox's
 * own host machinery does the marshalling, the budget accounting, the reserved
 * name rule and the raise.  Two consequences worth knowing:
 *
 *   - the pointer the sandbox keeps as the binding's `user` is the HANDLE, not
 *     the slot: kimix::vector reallocates on push_back, so an address inside it
 *     would dangle, while the handle is stable for its whole life.  The adapter
 *     therefore resolves its slot by NAME (call.name, which the trampoline keeps
 *     alive for the duration of the call) -- a handful of bytes compared per
 *     host call, and a rebind between calls is picked up automatically.
 *   - a host C function sees `kimix_lua_value` arrays that the ADAPTER builds:
 *     the argument view out of HostCall::args, the result slots out of
 *     HostCall::rets.  Both are handle-owned (sb->arg_view / sb->ret_view) and
 *     reset per call, never stack arrays: a raise from the trampoline is a
 *     longjmp through this frame, and a local kimix::vector would be skipped.
 *     The adapter itself is POD-only (one hand-filled POD ctx + size_t locals +
 *     the two pre-resized views), so nothing it owns needs unwinding.
 *
 * The error text goes the other way: the sandbox hands out HostCall::err /
 * err_cap, a POD array in SandboxState (script_state.h), and the adapter points
 * kimix_lua_host_ctx::err at it.  No allocation per call, nothing for the host
 * to free, and a stale message cannot survive into the next call.
 */
#include <api/ffi_lua.h>

#include <cstddef>
#include <cstring>
#include <new>

#include <core/memory.h>     /* kimix::IOperatorNewBase */
#include <core/stl/memory.h> /* kimix::unique_ptr */
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <script/script_sandbox.h>
#include <script/script_status.h>
#include <script/script_value.h>

/* The published layouts are part of the ABI (ffi_common.h: a POD struct field
 * added = a new KIMIX_API_ABI_VERSION), so a drift is a build error here rather
 * than a silently mis-parsed struct in a foreign binding. */
static_assert(sizeof(kimix_lua_value) == 48, "kimix_lua_value must stay 48 bytes on x64/arm64");
static_assert(sizeof(kimix_lua_limits) == 96, "kimix_lua_limits is 96 bytes since ABI 3 (88 in ABI 2)");
static_assert(sizeof(kimix_lua_result) == 64, "kimix_lua_result must stay 64 bytes on x64/arm64");

/* One bound host function.  The name is the slot's identity (the adapter looks
 * the slot up by it), so a rebind of the same name replaces this entry instead
 * of appending a second one. */
struct kimix_lua_host_slot {
    kimix::string name;
    kimix_lua_host_fn fn = nullptr;
    void *user = nullptr;
};

/* The opaque C handle.  `last` keeps the previous run's Result alive so the
 * accessors can answer without re-running anything; the two views are the
 * per-call C-ABI scratch of the host surface (see the header comment). */
struct kimix_lua_sandbox : kimix::IOperatorNewBase {
    kimix::script::Sandbox sandbox;
    kimix::script::Result last;
    kimix::vector<kimix_lua_host_slot> host_slots;
    kimix::vector<kimix_lua_value> arg_view; /* HostCall::args, as C values */
    kimix::vector<kimix_lua_value> ret_view; /* the host's result slots, as C values */
};

namespace src_api_ffi_lua {

using kimix::script::HostCall;
using kimix::script::Limits;
using kimix::script::Value;

kimix_lua_limits sb_limits_from(const Limits &limits) {
    kimix_lua_limits out;
    std::memset(&out, 0, sizeof(out));
    out.max_memory_bytes = limits.max_memory_bytes;
    out.max_source_bytes = limits.max_source_bytes;
    out.max_string_bytes = limits.max_string_bytes;
    out.max_output_bytes = limits.max_output_bytes;
    out.max_result_bytes = limits.max_result_bytes;
    out.max_result_values = limits.max_result_values;
    out.max_result_depth = limits.max_result_depth;
    out.max_host_calls = limits.max_host_calls;
    out.max_host_args = limits.max_host_args;
    out.max_host_ret_values = limits.max_host_ret_values;
    out.hook_granularity = limits.hook_instruction_granularity;
    out.random_seed = limits.random_seed;
    out.seed = limits.seed;
    out.max_seconds = limits.max_seconds;
    out.max_instructions = limits.max_instructions;
    out.fresh_state_per_run = limits.fresh_state_per_run ? 1u : 0u;
    return out;
}

Limits sb_limits_to(const kimix_lua_limits *limits) {
    Limits out;
    if (limits == nullptr) {
        return out;
    }
    out.max_memory_bytes = limits->max_memory_bytes;
    out.max_source_bytes = limits->max_source_bytes;
    out.max_string_bytes = limits->max_string_bytes;
    out.max_output_bytes = limits->max_output_bytes;
    out.max_result_bytes = limits->max_result_bytes;
    out.max_result_values = limits->max_result_values;
    out.max_result_depth = limits->max_result_depth;
    out.max_host_calls = limits->max_host_calls;
    out.max_host_args = limits->max_host_args;
    out.max_host_ret_values = limits->max_host_ret_values;
    out.hook_instruction_granularity = limits->hook_granularity;
    out.random_seed = limits->random_seed;
    out.seed = limits->seed;
    out.max_seconds = limits->max_seconds;
    out.max_instructions = limits->max_instructions;
    out.fresh_state_per_run = limits->fresh_state_per_run != 0;
    return out;
}

/* The one two-phase copy: `out_len` always receives the required length
 * (excluding the NUL), and a NULL/too-small buffer answers
 * KIMIX_ERR_OUT_OF_RANGE without touching the payload. */
kimix_status copy_out(const kimix::string &text, char *buf, size_t cap, size_t *out_len) {
    if (out_len != nullptr) {
        *out_len = text.size();
    }
    if (buf == nullptr) {
        return KIMIX_OK; /* phase one: report the required length only */
    }
    if (cap < text.size() + 1) {
        return KIMIX_ERR_OUT_OF_RANGE; /* phase two with too little room */
    }
    if (!text.empty()) {
        std::memcpy(buf, text.data(), text.size());
    }
    buf[text.size()] = '\0';
    return KIMIX_OK;
}

void sb_fill_result(const kimix::script::Result &result, kimix_lua_result *out) {
    out->status = static_cast<int32_t>(result.status);
    out->line = result.violation.line;
    out->message_len = result.message.size();
    out->output_len = result.output.size();
    out->traceback_len = result.traceback.size();
    out->value_count = result.results.size();
    out->memory_peak = result.memory_peak;
    out->instructions = result.instructions;
    out->seconds = result.seconds;
}

/* The value vocabulary, both directions.  A `Kind` is a uint8_t enum of six
 * values, so "out of range" is a host mistake the C side can make with any
 * int32_t. */
int32_t sb_kind_to_c(Value::Kind kind) noexcept {
    switch (kind) {
    case Value::Kind::nil: return KIMIX_LUA_VALUE_NIL;
    case Value::Kind::boolean: return KIMIX_LUA_VALUE_BOOLEAN;
    case Value::Kind::integer: return KIMIX_LUA_VALUE_INTEGER;
    case Value::Kind::number: return KIMIX_LUA_VALUE_NUMBER;
    case Value::Kind::string: return KIMIX_LUA_VALUE_STRING;
    case Value::Kind::table: return KIMIX_LUA_VALUE_JSON;
    }
    return KIMIX_LUA_VALUE_NIL;
}

/* The script value -> C value view.  The text keeps pointing into the Value's
 * own bytes: the view is only ever handed to a host function for the duration of
 * one call, and HostCall::args (SandboxState::host_args) outlives it. */
void sb_value_to_c(const Value &value, kimix_lua_value &out) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.kind = sb_kind_to_c(value.kind);
    out.integer = value.i;
    out.number = value.d;
    out.boolean = value.b ? 1u : 0u;
    if (value.kind == Value::Kind::string || value.kind == Value::Kind::table) {
        out.text = value.s.data(); /* kimix::string keeps a NUL after size() */
        out.text_len = value.s.size();
    }
}

/* The C value -> script value copy.  text_len is authoritative and the bytes are
 * COPIED, so nothing the caller owns is referenced after the call returns.
 * false for a `kind` outside the vocabulary, or for text without bytes. */
bool sb_value_from_c(const kimix_lua_value &in, Value &out) {
    out.clear();
    switch (in.kind) {
    case KIMIX_LUA_VALUE_NIL:
        out.kind = Value::Kind::nil;
        return true;
    case KIMIX_LUA_VALUE_BOOLEAN:
        out.kind = Value::Kind::boolean;
        out.b = in.boolean != 0;
        return true;
    case KIMIX_LUA_VALUE_INTEGER:
        out.kind = Value::Kind::integer;
        out.i = in.integer;
        out.d = static_cast<double>(in.integer);
        return true;
    case KIMIX_LUA_VALUE_NUMBER:
        out.kind = Value::Kind::number;
        out.d = in.number;
        return true;
    case KIMIX_LUA_VALUE_STRING:
        out.kind = Value::Kind::string;
        break;
    case KIMIX_LUA_VALUE_JSON:
        out.kind = Value::Kind::table;
        break;
    default:
        return false; /* not a KIMIX_LUA_VALUE_*: a host mistake */
    }
    if (in.text == nullptr) {
        return in.text_len == 0; /* NULL with no bytes = the empty string */
    }
    out.s.assign(in.text, in.text_len);
    return true;
}

/* The per-handle slot behind `name`, or nullptr.  A rebind replaces the slot, so
 * the first match is the only match. */
kimix_lua_host_slot *sb_find_slot(kimix_lua_sandbox &sb, kimix::string_view name) noexcept {
    for (kimix_lua_host_slot &slot : sb.host_slots) {
        if (slot.name.size() == name.size() && std::memcmp(slot.name.data(), name.data(), name.size()) == 0) {
            return &slot;
        }
    }
    return nullptr;
}

/* THE C -> C++ adapter: the one function handed to Sandbox::bind_host_function.
 * Its `user` is the handle (see the file header), it owns nothing with a
 * destructor, and it never raises -- it answers through HostCall and lets the
 * trampoline compose the raise, so every policy (the sticky LIMIT of an
 * uncrossable value, the ret_cap check, the RUNTIME message) stays in one
 * place. */
bool sb_host_adapter(void *user, HostCall &call) {
    auto *sb = static_cast<kimix_lua_sandbox *>(user);
    kimix_lua_host_slot *slot = sb_find_slot(*sb, call.name);
    if (slot == nullptr || slot->fn == nullptr) {
        /* Only reachable if the C caller rebound/cleared names mid-call, which
         * the header forbids; report it as an ordinary host failure. */
        call.error.assign("host binding is no longer installed");
        return false;
    }

    /* Build the argument view: one POD slot per marshalled argument, pointing at
     * the state-owned copies the trampoline already made.  Handle-owned scratch,
     * reset per call -- never a stack array. */
    sb->arg_view.clear();
    if (call.argc > 0) {
        sb->arg_view.resize(call.argc);
        for (size_t i = 0; i < call.argc; ++i) {
            sb_value_to_c(call.args[i], sb->arg_view[i]);
        }
    }
    /* …and the result slots, zeroed so a host that writes fewer values than it
     * reports cannot leak a previous call's payload into the script. */
    sb->ret_view.clear();
    if (call.ret_cap > 0) {
        sb->ret_view.resize(call.ret_cap);
        std::memset(sb->ret_view.data(), 0, sb->ret_view.size() * sizeof(kimix_lua_value));
    }

    kimix_lua_host_ctx ctx;
    ctx.user = slot->user;
    ctx.name = call.name.data(); /* the Sandbox's own kimix::string: NUL-terminated */
    ctx.name_len = call.name.size();
    ctx.err = call.err; /* SandboxState::host_err, reset per call */
    ctx.err_cap = call.err_cap;
    ctx.call_index = call.index;
    if (ctx.err != nullptr && ctx.err_cap > 0) {
        ctx.err[0] = '\0';
    }

    size_t ret_count = 0;
    const int32_t failed = slot->fn(&ctx, sb->arg_view.empty() ? nullptr : sb->arg_view.data(), call.argc,
                                    sb->ret_view.empty() ? nullptr : sb->ret_view.data(), call.ret_cap,
                                    &ret_count);
    if (failed != 0) {
        /* The host's own words win; an empty message still fails the call with
         * the sandbox's "host function '<name>' failed: " prefix.  The scan is
         * bounded by err_cap, so a host that forgot the terminator gets a
         * truncated message instead of an over-read. */
        size_t len = 0;
        if (ctx.err != nullptr) {
            while (len < ctx.err_cap && ctx.err[len] != '\0') {
                ++len;
            }
        }
        call.error.assign((ctx.err != nullptr) ? ctx.err : "", len);
        return false;
    }

    /* An over-run is REPORTED, not clamped: `produced` carries the host's own
     * count so the trampoline's ret_cap check turns it into the documented sticky
     * LIMIT, and no second policy lives here.  Nothing is written past call.rets
     * either way -- only the slots that fit are converted. */
    size_t copy = (ret_count > call.ret_cap) ? call.ret_cap : ret_count;
    for (size_t i = 0; i < copy; ++i) {
        if (!sb_value_from_c(sb->ret_view[i], call.rets[i])) {
            /* An out-of-range `kind` from the host is not a value that can cross:
             * report it as the host's failure rather than half-filling rets. */
            call.error.assign("host function returned a value kind outside KIMIX_LUA_VALUE_*");
            return false;
        }
    }
    call.produced = ret_count;
    return true;
}

/* Run `spec` and publish the outcome through the one two-phase path.  The
 * caller's buffers are the only thing that can fail here. */
kimix_status sb_run_and_publish(kimix_lua_sandbox *sb, const kimix::script::Sandbox::RunSpec &spec,
                                kimix_lua_result *res, char *msg_buf, size_t msg_cap) {
    sb->last = sb->sandbox.run(spec);
    sb_fill_result(sb->last, res);
    if (copy_out(sb->last.message, msg_buf, msg_cap, &res->message_len) != KIMIX_OK) {
        return KIMIX_ERR_OUT_OF_RANGE; /* res is fully filled: the caller can resize */
    }
    return KIMIX_OK;
}

kimix_status sb_run_common(kimix_lua_sandbox *sb, const char *src, size_t src_len, const char *chunk_name,
                           size_t chunk_name_len, kimix_lua_result *res, char *msg_buf, size_t msg_cap) {
    if (sb == nullptr || res == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    kimix::script::Sandbox::RunSpec spec;
    spec.source = kimix::string_view((src != nullptr) ? src : "", (src != nullptr) ? src_len : 0);
    if (chunk_name != nullptr && chunk_name_len > 0) {
        spec.chunk_name = kimix::string_view(chunk_name, chunk_name_len);
    }
    return sb_run_and_publish(sb, spec, res, msg_buf, msg_cap);
}

} // namespace src_api_ffi_lua
using namespace src_api_ffi_lua;

/* ------------------------------------------------------------------------- */
/* 2. Library / ABI queries                                                  */
/* ------------------------------------------------------------------------- */

extern "C" {

uint32_t kimix_lua_abi_version(void) {
    return KIMIX_API_ABI_VERSION;
}

const char *kimix_lua_version_string(void) {
    return kimix_api_version_string();
}

const char *kimix_lua_status_name(int32_t status) {
    return kimix::script::status_name(static_cast<kimix::script::Status>(status));
}

kimix_lua_limits kimix_lua_default_limits(void) {
    return sb_limits_from(Limits{});
}

/* ------------------------------------------------------------------------- */
/* 3. Sandbox lifecycle                                                      */
/* ------------------------------------------------------------------------- */

kimix_status kimix_lua_sandbox_create(const kimix_lua_limits *limits, kimix_lua_sandbox **out) {
    if (out == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    *out = nullptr;
    /* The handle is mimalloc memory (IOperatorNewBase), and it stays owned by a
     * kimix::unique_ptr until `*out` takes it: every early return above/below
     * therefore frees it through the same operator delete that a later
     * kimix_lua_sandbox_destroy uses. */
    kimix::unique_ptr<kimix_lua_sandbox> handle(new (std::nothrow) kimix_lua_sandbox);
    if (handle == nullptr) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    /* The constructor already applied the defaults; re-apply the caller's
     * Limits through a fresh Sandbox so the constructor argument is the one and
     * only configuration path. */
    const Limits parsed = sb_limits_to(limits);
    handle->sandbox = kimix::script::Sandbox(parsed);
    if (limits != nullptr) {
        handle->sandbox.allow_coroutines(limits->allow_coroutines != 0);
        handle->sandbox.allow_collectgarbage_count(limits->allow_collectgarbage_count != 0);
    }
    *out = handle.release();
    return KIMIX_OK;
}

kimix_status kimix_lua_sandbox_destroy(kimix_lua_sandbox *sb) {
    delete sb; /* NULL is a no-op; operator delete is the mimalloc one */
    return KIMIX_OK;
}

kimix_status kimix_lua_cancel(kimix_lua_sandbox *sb) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    sb->sandbox.cancel();
    return KIMIX_OK;
}

/* ------------------------------------------------------------------------- */
/* 4. Running a chunk                                                        */
/* ------------------------------------------------------------------------- */

kimix_status kimix_lua_run(kimix_lua_sandbox *sb, const char *src, size_t src_len, kimix_lua_result *res,
                           char *msg_buf, size_t msg_cap) {
    return sb_run_common(sb, src, src_len, nullptr, 0, res, msg_buf, msg_cap);
}

kimix_status kimix_lua_run_named(kimix_lua_sandbox *sb, const char *src, size_t src_len, const char *chunk_name,
                                 size_t chunk_name_len, kimix_lua_result *res, char *msg_buf, size_t msg_cap) {
    return sb_run_common(sb, src, src_len, chunk_name, chunk_name_len, res, msg_buf, msg_cap);
}

/* ------------------------------------------------------------------------- */
/* 5. Reading the last run                                                   */
/* ------------------------------------------------------------------------- */

kimix_status kimix_lua_last_message(kimix_lua_sandbox *sb, char *buf, size_t cap, size_t *out_len) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return copy_out(sb->last.message, buf, cap, out_len);
}

kimix_status kimix_lua_last_output(kimix_lua_sandbox *sb, char *buf, size_t cap, size_t *out_len) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return copy_out(sb->last.output, buf, cap, out_len);
}

kimix_status kimix_lua_last_traceback(kimix_lua_sandbox *sb, char *buf, size_t cap, size_t *out_len) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    return copy_out(sb->last.traceback, buf, cap, out_len);
}

kimix_status kimix_lua_last_violation(kimix_lua_sandbox *sb, char *api, size_t api_cap, size_t *api_len, char *category,
                                      size_t category_cap, size_t *category_len, char *detail, size_t detail_cap,
                                      size_t *detail_len, char *chunk, size_t chunk_cap, size_t *chunk_len) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const kimix::script::Violation &violation = sb->last.violation;
    kimix_status status = copy_out(violation.api, api, api_cap, api_len);
    if (copy_out(violation.category, category, category_cap, category_len) != KIMIX_OK) {
        status = KIMIX_ERR_OUT_OF_RANGE;
    }
    if (copy_out(violation.detail, detail, detail_cap, detail_len) != KIMIX_OK) {
        status = KIMIX_ERR_OUT_OF_RANGE;
    }
    if (copy_out(violation.chunk, chunk, chunk_cap, chunk_len) != KIMIX_OK) {
        status = KIMIX_ERR_OUT_OF_RANGE;
    }
    return status;
}

kimix_status kimix_lua_last_value(kimix_lua_sandbox *sb, size_t index, int32_t *kind, uint8_t *b, int64_t *i, double *d,
                                  char *str_buf, size_t str_cap, size_t *str_len) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (index >= sb->last.results.size()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    const kimix::script::Value &value = sb->last.results[index];
    if (kind != nullptr) {
        *kind = sb_kind_to_c(value.kind);
    }
    if (b != nullptr) {
        *b = value.b ? 1u : 0u;
    }
    if (i != nullptr) {
        *i = value.i;
    }
    if (d != nullptr) {
        *d = value.d;
    }
    return copy_out(value.s, str_buf, str_cap, str_len);
}

/* ------------------------------------------------------------------------- */
/* 6. The host surface                                                       */
/* ------------------------------------------------------------------------- */

kimix_status kimix_lua_host_bind(kimix_lua_sandbox *sb, const char *name, size_t name_len, kimix_lua_host_fn fn,
                                 void *user) {
    if (sb == nullptr || name == nullptr || name_len == 0 || fn == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (name_len > static_cast<size_t>(KIMIX_LUA_HOST_NAME_MAX)) {
        /* The sandbox composes its own messages from a bounded copy of the name,
         * so a longer one is refused instead of silently truncated. */
        return KIMIX_ERR_INVALID_ARG;
    }
    const kimix::string_view view(name, name_len);
    /* Order matters: bind first, so a name the C++ layer refuses (an invalid
     * identifier or a RESERVED name) leaves no slot behind, and the handle's
     * slot list only ever holds names the sandbox actually installed. */
    if (!sb->sandbox.bind_host_function(view, &sb_host_adapter, sb)) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (kimix_lua_host_slot *slot = sb_find_slot(*sb, view)) {
        slot->fn = fn; /* re-binding replaces the function and the user pointer */
        slot->user = user;
        return KIMIX_OK;
    }
    kimix_lua_host_slot slot;
    slot.name.assign(view.data(), view.size());
    slot.fn = fn;
    slot.user = user;
    sb->host_slots.push_back(static_cast<kimix_lua_host_slot &&>(slot));
    return KIMIX_OK;
}

kimix_status kimix_lua_global_set(kimix_lua_sandbox *sb, const char *name, size_t name_len,
                                  const kimix_lua_value *value) {
    if (sb == nullptr || name == nullptr || name_len == 0) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const kimix::string_view view(name, name_len);
    if (value == nullptr || value->kind == KIMIX_LUA_VALUE_NIL) {
        /* "Clear it": a Kind::nil global is installed as nil, which removes the
         * name from the run environment (and refuses a reserved name, so
         * set_global("print", nil) cannot delete a capability). */
        return sb->sandbox.set_global(view, Value{}) ? KIMIX_OK : KIMIX_ERR_INVALID_ARG;
    }
    Value converted;
    if (!sb_value_from_c(*value, converted)) {
        return KIMIX_ERR_INVALID_ARG; /* a kind that cannot cross: a host mistake */
    }
    /* set_global validates a Kind::table text at HOST time (sb_json_check, same
     * caps and duplicate-key rule as the materialisation), so a broken JSON text
     * is refused here instead of poisoning a later run with a sticky LIMIT. */
    return sb->sandbox.set_global(view, converted) ? KIMIX_OK : KIMIX_ERR_INVALID_ARG;
}

kimix_status kimix_lua_globals_clear(kimix_lua_sandbox *sb) {
    if (sb == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    sb->sandbox.clear_globals();
    return KIMIX_OK;
}

kimix_status kimix_lua_run_call(kimix_lua_sandbox *sb, const char *src, size_t src_len, const char *chunk_name,
                                size_t chunk_name_len, const char *fn_name, size_t fn_name_len,
                                const kimix_lua_value *args, size_t argc, kimix_lua_result *res, char *msg_buf,
                                size_t msg_cap) {
    if (sb == nullptr || res == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (argc > 0 && args == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* The converted arguments live on THIS frame, which is enough: Sandbox::run
     * copies them into the sandbox state before the chunk runs, and nothing it
     * does can longjmp past this frame (the raise happens inside the run's own
     * lua_pcallk).  An invalid kind is refused before the run starts, so a host
     * mistake never costs a run. */
    kimix::vector<Value> converted;
    if (argc > 0) {
        converted.resize(argc);
        for (size_t i = 0; i < argc; ++i) {
            if (!sb_value_from_c(args[i], converted[i])) {
                return KIMIX_ERR_INVALID_ARG;
            }
        }
    }
    kimix::script::Sandbox::RunSpec spec;
    spec.source = kimix::string_view((src != nullptr) ? src : "", (src != nullptr) ? src_len : 0);
    if (chunk_name != nullptr && chunk_name_len > 0) {
        spec.chunk_name = kimix::string_view(chunk_name, chunk_name_len);
    }
    if (fn_name != nullptr && fn_name_len > 0) {
        /* An empty name leaves call_name empty, which is exactly kimix_lua_run:
         * run the chunk and marshal ITS results. */
        spec.call_name = kimix::string_view(fn_name, fn_name_len);
    }
    if (!converted.empty()) {
        spec.args = converted.data();
        spec.argc = converted.size();
    }
    return sb_run_and_publish(sb, spec, res, msg_buf, msg_cap);
}

uint64_t kimix_lua_last_host_calls(kimix_lua_sandbox *sb) {
    return (sb != nullptr) ? sb->last.host_calls : 0u;
}

} /* extern "C" */
