/*
 * ffi_lua.cpp -- implementation of the Lua script sandbox C ABI (api/ffi_lua.h).
 *
 * One job: own ONE kimix::script::Sandbox per handle plus the last Result, and
 * copy every variable-length answer into caller storage so no library pointer is
 * ever handed out and never has to be returned (api/ffi_common.h, one-heap rule).
 *
 * The whole sandbox semantics live in src/script (kimix-script); this file adds
 * no policy at all.  Two translation-unit-local conventions keep it honest:
 *
 *   - EVERY accessor goes through `copy_out`, so the two-phase protocol is
 *     implemented once: NULL/too-small -> KIMIX_ERR_OUT_OF_RANGE with the
 *     required length still written, otherwise a NUL-terminated copy whose
 *     reported length excludes the terminator.
 *   - `g_last_result` is a per-handle struct member, never a global, so two
 *     handles used on two threads never share anything.
 */
#include <api/ffi_lua.h>

#include <cstring>
#include <new>

#include <script/script_sandbox.h>
#include <script/script_status.h>
#include <script/script_value.h>

/* The opaque C handle.  `last` keeps the previous run's Result alive so the
 * accessors can answer without re-running anything. */
struct kimix_lua_sandbox {
    kimix::script::Sandbox sandbox;
    kimix::script::Result last;
};

namespace src_api_ffi_lua {

using kimix::script::Limits;

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

kimix_status sb_run_common(kimix_lua_sandbox *sb, const char *src, size_t src_len, const char *chunk_name,
                           size_t chunk_name_len, kimix_lua_result *res, char *msg_buf, size_t msg_cap) {
    if (sb == nullptr || res == nullptr) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const kimix::string_view source((src != nullptr) ? src : "", (src != nullptr) ? src_len : 0);
    kimix::string_view name("script.lua");
    if (chunk_name != nullptr && chunk_name_len > 0) {
        name = kimix::string_view(chunk_name, chunk_name_len);
    }
    sb->last = sb->sandbox.run(source, name);
    sb_fill_result(sb->last, res);
    const kimix_status copied = copy_out(sb->last.message, msg_buf, msg_cap, &res->message_len);
    if (copied != KIMIX_OK) {
        return KIMIX_ERR_OUT_OF_RANGE; /* res is fully filled: the caller can resize */
    }
    return KIMIX_OK;
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
    kimix_lua_sandbox *handle = new (std::nothrow) kimix_lua_sandbox;
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
    *out = handle;
    return KIMIX_OK;
}

kimix_status kimix_lua_sandbox_destroy(kimix_lua_sandbox *sb) {
    delete sb;
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
        switch (value.kind) {
        case kimix::script::Value::Kind::nil: *kind = KIMIX_LUA_VALUE_NIL; break;
        case kimix::script::Value::Kind::boolean: *kind = KIMIX_LUA_VALUE_BOOLEAN; break;
        case kimix::script::Value::Kind::integer: *kind = KIMIX_LUA_VALUE_INTEGER; break;
        case kimix::script::Value::Kind::number: *kind = KIMIX_LUA_VALUE_NUMBER; break;
        case kimix::script::Value::Kind::string: *kind = KIMIX_LUA_VALUE_STRING; break;
        case kimix::script::Value::Kind::table: *kind = KIMIX_LUA_VALUE_JSON; break;
        }
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

} /* extern "C" */
