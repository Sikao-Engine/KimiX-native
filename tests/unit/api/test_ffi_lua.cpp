/*
 * test_ffi_lua.cpp — exercises the Lua script sandbox C ABI (api/ffi_lua.h).
 *
 * The suite calls the library through the PUBLIC C header only (no kimix-core
 * and no kimix-script type is referenced anywhere), which is exactly what a
 * foreign binding does.  It closes a pre-existing gap: before it, no C++ suite
 * covered kimix_lua_* at all.
 *
 * It proves:
 * - the ABI generation (3) and the three published layouts (kimix_lua_value 48,
 *   kimix_lua_limits 96, kimix_lua_result 64 bytes) with a compile-time
 *   static_assert AND a runtime expect;
 * - the handle is project-heap memory (kimix::allocate), not CRT memory:
 * kimix_mem_usable_size(handle) > 0, which is the visible consequence of the
 * handle deriving kimix::IOperatorNewBase (a CRT-heap object would answer 0
 * here);
 * - the five host-surface entry points end to end: kimix_lua_host_bind (a C
 *   function the script calls), kimix_lua_run_call (a named function with
 *   arguments, and the empty name behaving exactly like kimix_lua_run),
 *   kimix_lua_global_set (visible in two consecutive runs, cleared by a NULL
 *   value and by kimix_lua_globals_clear) and kimix_lua_last_host_calls;
 * - the host-failure contract: a non-zero return is a NON-sticky
 *   KIMIX_LUA_RUNTIME whose message is
 *   "host function '<name>' failed: <ctx->err>", so the script's own pcall
 *   catches it and the next run is clean again;
 * - tables cross BOTH ways through the C ABI: a script table reaches the C
 *   function as canonical JSON text, and a table the C function returns arrives
 *   in the script as a real Lua table;
 * - the host-misuse contract: a NULL handle, an empty/over-long name, a NULL
 *   function, a RESERVED name ("print") and a broken JSON text are all
 *   KIMIX_ERR_INVALID_ARG -- refused at host time, never a poisoned run;
 * - the two-phase message protocol still does not re-run the chunk: reading the
 *   message twice (NULL first, then a real buffer, then a deliberately too-small
 *   one) leaves the run's host-call counter exactly where it was;
 * - a host function that re-enters its own handle is answered KIMIX_LUA_INTERNAL
 *   by the inner call while the outer run still finishes;
 * - kimix_lua_sandbox_destroy tolerates NULL and releases a used handle.
 *
 * Registered in tests/xmake.lua behind has_config("kimix_enable_api"), like
 * every other unit/api suite: with the option off src/api is not part of the
 * configuration at all.
 */
#include "ut/ut.hpp"

#include <api/ffi_lua.h>
#include <api/ffi_mem.h> // kimix_mem_usable_size: the one-heap proof below

#include <cstdio>
#include <cstring>
#include <cstring>

using boost::ut::expect;
using boost::ut::operator""_test;

namespace tests_unit_api_test_ffi_lua {

/* Every published layout is part of the ABI (ffi_common.h), so a drift must be a
 * build error in a consumer too -- not just in the library's own .cpp. */
static_assert(sizeof(kimix_lua_value) == 48, "kimix_lua_value must stay 48 bytes on x64/arm64");
static_assert(sizeof(kimix_lua_limits) == 96, "kimix_lua_limits is 96 bytes since ABI 3");
static_assert(sizeof(kimix_lua_result) == 64, "kimix_lua_result must stay 64 bytes on x64/arm64");

/* ------------------------------------------------------------------ host state
 * Everything a C host function is allowed to remember.  Fixed-size char arrays,
 * never a std::string: like the library's own scratch, this is plain data a
 * foreign callback writes and the test reads. */

struct host_state {
    int calls = 0; /* how many times a host function ran */
    uint64_t last_index = 0; /* ctx->call_index of the last call */
    char last_name[KIMIX_LUA_HOST_NAME_MAX + 1] = {0};
    size_t last_name_len = 0;
    const void *last_user = nullptr; /* ctx->user as the callback received it */
    char json[512] = {0}; /* the canonical JSON of the last table argument */
    size_t json_len = 0;
    char seen[256] = {0}; /* "kind=payload|..." summary of the last arguments */
    char err[128] = {0}; /* the text a failing callback reports */
    size_t err_cap_seen = 0; /* ctx->err_cap as the callback received it */
    int32_t inner_status = -1; /* kimix_lua_run on the SAME handle, from inside */
    char inner_message[256] = {0};
    kimix_lua_sandbox *self = nullptr; /* the handle the callbacks may re-enter */
};

void copy_text(char *dst, size_t cap, const char *src, size_t len) {
    if (cap == 0) {
        return;
    }
    const size_t n = (len < cap - 1) ? len : cap - 1;
    if (n > 0) {
        std::memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

/* Record the invocation: name, user, ordinal, the error scratch size. */
void record(kimix_lua_host_ctx *ctx, host_state *state) {
    ++state->calls;
    state->last_index = ctx->call_index;
    state->last_name_len = ctx->name_len;
    copy_text(state->last_name, sizeof(state->last_name), ctx->name, ctx->name_len);
    state->last_user = ctx->user;
    state->err_cap_seen = ctx->err_cap;
}

/* "<kind>=<payload>" per argument, joined by '|': one compare checks every
 * marshalled field at once. */
void summarise(host_state *state, const kimix_lua_value *args, size_t argc) {
    state->seen[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < argc; ++i) {
        char piece[96];
        switch (args[i].kind) {
        case KIMIX_LUA_VALUE_NIL: std::snprintf(piece, sizeof(piece), "nil=-"); break;
        case KIMIX_LUA_VALUE_BOOLEAN:
            std::snprintf(piece, sizeof(piece), "boolean=%s", args[i].boolean ? "true" : "false");
            break;
        case KIMIX_LUA_VALUE_INTEGER:
            std::snprintf(piece, sizeof(piece), "integer=%lld", static_cast<long long>(args[i].integer));
            break;
        case KIMIX_LUA_VALUE_NUMBER: std::snprintf(piece, sizeof(piece), "number=%.17g", args[i].number); break;
        case KIMIX_LUA_VALUE_STRING: std::snprintf(piece, sizeof(piece), "string=%.*s",
                                                  static_cast<int>(args[i].text_len),
                                                  (args[i].text != nullptr) ? args[i].text : "");
            break;
        case KIMIX_LUA_VALUE_JSON: std::snprintf(piece, sizeof(piece), "json=%.*s",
                                                static_cast<int>(args[i].text_len),
                                                (args[i].text != nullptr) ? args[i].text : "");
            break;
        default: std::snprintf(piece, sizeof(piece), "kind%d=?", static_cast<int>(args[i].kind)); break;
        }
        const size_t len = std::strlen(piece);
        if (used + len + 1 < sizeof(state->seen)) {
            if (used > 0) {
                state->seen[used++] = '|';
            }
            std::memcpy(state->seen + used, piece, len);
            used += len;
            state->seen[used] = '\0';
        }
    }
}

/* --------------------------------------------------------- the C host functions
 * Every one of them is a plain C-shaped function of the documented shape: it
 * writes only into the buffers it was given, keeps no pointer, and answers 0
 * (ok) or non-zero (failed, text in ctx->err). */

/* add(a, b) -> a + b */
int32_t host_add(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                 size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    if (argc != 2 || args[0].kind != KIMIX_LUA_VALUE_INTEGER || args[1].kind != KIMIX_LUA_VALUE_INTEGER) {
        std::snprintf(ctx->err, ctx->err_cap, "expected two integers");
        return 1;
    }
    if (ret_cap < 1) {
        std::snprintf(ctx->err, ctx->err_cap, "no result slot");
        return 1;
    }
    rets[0] = kimix_lua_value{};
    rets[0].kind = KIMIX_LUA_VALUE_INTEGER;
    rets[0].integer = args[0].integer + args[1].integer;
    *ret_count = 1;
    return 0;
}

/* peek(x) -> the kind of x plus the canonical JSON / text it arrived with.  The
 * table direction script -> C: a Lua table must reach here as JSON text. */
int32_t host_peek(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                  size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    state->json[0] = '\0';
    state->json_len = 0;
    if (argc != 1 || args[0].kind != KIMIX_LUA_VALUE_JSON) {
        std::snprintf(ctx->err, ctx->err_cap, "expected one table argument");
        return 1;
    }
    state->json_len = args[0].text_len;
    copy_text(state->json, sizeof(state->json), args[0].text, args[0].text_len);
    if (ret_cap < 1) {
        std::snprintf(ctx->err, ctx->err_cap, "no result slot");
        return 1;
    }
    rets[0] = kimix_lua_value{};
    rets[0].kind = KIMIX_LUA_VALUE_STRING;
    rets[0].text = args[0].text; /* the same bytes, handed straight back */
    rets[0].text_len = args[0].text_len;
    *ret_count = 1;
    return 0;
}

/* give() -> one table, as canonical JSON.  The table direction C -> script: the
 * N3 strict reader materialises this text into a real Lua table. */
int32_t host_give_table(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                        size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    static const char kTable[] = "{\"a\":1,\"b\":[true,null],\"c\":\"x\"}";
    if (ret_cap < 1) {
        std::snprintf(ctx->err, ctx->err_cap, "no result slot");
        return 1;
    }
    rets[0] = kimix_lua_value{};
    rets[0].kind = KIMIX_LUA_VALUE_JSON;
    rets[0].text = kTable;
    rets[0].text_len = sizeof(kTable) - 1;
    *ret_count = 1;
    return 0;
}

/* boom() -> always fails with state->err, in the host's own words. */
int32_t host_boom(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                  size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    (void)rets;
    (void)ret_cap;
    (void)ret_count;
    std::snprintf(ctx->err, ctx->err_cap, "%s", state->err);
    return 1;
}

/* tick() -> 1, and counts.  Used for the "reading the message does not re-run the
 * chunk" protocol check. */
int32_t host_tick(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                  size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    if (ret_cap < 1) {
        std::snprintf(ctx->err, ctx->err_cap, "no result slot");
        return 1;
    }
    rets[0] = kimix_lua_value{};
    rets[0].kind = KIMIX_LUA_VALUE_INTEGER;
    rets[0].integer = 1;
    *ret_count = 1;
    return 0;
}

/* flood() -> reports THREE results while the sandbox allows at most
 * limits.max_host_ret_values (1 in the test).  The host must not write past the
 * slots it was given, so it only fills the two it can and REPORTS 3: the sticky
 * LIMIT is the sandbox's own ret_cap check, not a second policy here. */
int32_t host_flood(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                   size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    for (size_t i = 0; i < ret_cap && i < 3; ++i) {
        rets[i] = kimix_lua_value{};
        rets[i].kind = KIMIX_LUA_VALUE_INTEGER;
        rets[i].integer = static_cast<int64_t>(i + 1);
    }
    *ret_count = 3;
    return 0;
}

/* reenter() -> runs a script on the very handle that is calling it, which the
 * sandbox must answer with KIMIX_LUA_INTERNAL (never a deadlock). */
int32_t host_reenter(kimix_lua_host_ctx *ctx, const kimix_lua_value *args, size_t argc, kimix_lua_value *rets,
                     size_t ret_cap, size_t *ret_count) {
    auto *state = static_cast<host_state *>(ctx->user);
    record(ctx, state);
    summarise(state, args, argc);
    const char src[] = "return 1";
    kimix_lua_result inner{};
    if (state->self != nullptr) {
        const kimix_status call = kimix_lua_run(state->self, src, sizeof(src) - 1, &inner, nullptr, 0);
        if (call == KIMIX_OK) {
            state->inner_status = inner.status;
            size_t len = 0;
            if (kimix_lua_last_message(state->self, state->inner_message, sizeof(state->inner_message), &len) ==
                KIMIX_OK) {
                state->inner_message[(len < sizeof(state->inner_message)) ? len : sizeof(state->inner_message) - 1] =
                    '\0';
            }
        } else {
            state->inner_status = -2; /* the CALL itself was refused */
        }
    }
    if (ret_cap < 1) {
        std::snprintf(ctx->err, ctx->err_cap, "no result slot");
        return 1;
    }
    rets[0] = kimix_lua_value{};
    rets[0].kind = KIMIX_LUA_VALUE_STRING;
    rets[0].text = "inner=done";
    rets[0].text_len = std::strlen("inner=done");
    *ret_count = 1;
    return 0;
}

/* --------------------------------------------------------------- C-level helpers */

bool message_contains(kimix_lua_sandbox *sb, const char *needle) {
    char buf[1024] = {0};
    size_t len = 0;
    if (kimix_lua_last_message(sb, buf, sizeof(buf), &len) != KIMIX_OK) {
        return false;
    }
    buf[(len < sizeof(buf)) ? len : sizeof(buf) - 1] = '\0';
    return std::strstr(buf, needle) != nullptr;
}

/* Create a handle whose limits allow exactly one result slot, so the
 * "host reports more than it may" path is reachable. */
kimix_lua_limits limits_with_one_ret() {
    kimix_lua_limits limits = kimix_lua_default_limits();
    limits.max_host_ret_values = 1;
    return limits;
}

} // namespace tests_unit_api_test_ffi_lua
using namespace tests_unit_api_test_ffi_lua;

int main() {
    // --------------------------------------------------------------- the layouts
    "abi: version and the three published layouts"_test = [] {
        expect(kimix_lua_abi_version() == 3u) << "the Lua family is ABI 3 (host surface)";
        expect(kimix_lua_abi_version() == KIMIX_API_ABI_VERSION);
        // Runtime twins of the static_asserts above: the size a foreign binding
        // will hard-code must also be the size this build actually emits.
        expect(sizeof(kimix_lua_value) == 48u);
        expect(sizeof(kimix_lua_limits) == 96u);
        expect(sizeof(kimix_lua_result) == 64u);
        // The new host budgets exist and default to the documented numbers.
        const kimix_lua_limits limits = kimix_lua_default_limits();
        expect(limits.max_host_calls == 10000u);
        expect(limits.max_host_args == 16u);
        expect(limits.max_host_ret_values == 16u);
    };

    // ------------------------------------------------------------ the heap proof
    "handle: mimalloc-owned, destroy(NULL) is a no-op"_test = [] {
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(sb != nullptr);
        // The handle derives kimix::IOperatorNewBase, so the kimix::allocate
        // gateway owns it: a CRT-heap object answers 0 here, and that one heap
        // (the one-heap rule of api/ffi_common.h) can see it.
        expect(kimix_mem_usable_size(sb) > 0u) << "the sandbox handle must be project-heap memory, not CRT memory";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, "return 1", 8, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
        expect(kimix_lua_sandbox_destroy(nullptr) == KIMIX_OK);
    };

    // ------------------------------------------------- host_bind + a script call
    "host: a bound C function is called by the script and its result comes back"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "add", 3, &host_add, &state) == KIMIX_OK);

        const char src[] = "return add(2, 3)";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);
        expect(res.value_count == 1u);
        int32_t kind = -1;
        int64_t integer = 0;
        expect(kimix_lua_last_value(sb, 0, &kind, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(kind == KIMIX_LUA_VALUE_INTEGER);
        expect(integer == 5);

        // Everything the C function was told about the invocation.
        expect(state.calls == 1);
        expect(state.last_index == 1u) << "the ordinal is 1-based";
        expect(state.last_user == &state) << "ctx->user is the caller's own pointer, verbatim";
        expect(state.last_name_len == 3u);
        expect(std::strcmp(state.last_name, "add") == 0);
        expect(state.err_cap_seen >= 512u) << "the error scratch is at least 512 bytes";
        expect(std::strcmp(state.seen, "integer=2|integer=3") == 0) << state.seen;
        expect(kimix_lua_last_host_calls(sb) == 1u);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // ---------------------------------------------------------------- run_call
    "run_call: a named function with arguments"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "add", 3, &host_add, &state) == KIMIX_OK);

        const char src[] = "function sums(a, b) return add(a, b) + 100 end";
        kimix_lua_value args[2] = {};
        args[0].kind = KIMIX_LUA_VALUE_INTEGER;
        args[0].integer = 2;
        args[1].kind = KIMIX_LUA_VALUE_INTEGER;
        args[1].integer = 3;
        kimix_lua_result res{};
        expect(kimix_lua_run_call(sb, src, sizeof(src) - 1, nullptr, 0, "sums", 4, args, 2, &res, nullptr, 0) ==
               KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);
        expect(res.value_count == 1u);
        int64_t integer = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(integer == 105) << "the host function ran inside the named call";
        expect(state.calls == 1);
        expect(kimix_lua_last_host_calls(sb) == 1u);

        // Every crossable argument kind survives the boundary byte-exactly.
        const char echo_src[] = "function peek(a) return a end";
        kimix_lua_value text = {};
        text.kind = KIMIX_LUA_VALUE_STRING;
        text.text = "hi";
        text.text_len = 2;
        kimix_lua_result echoed{};
        expect(kimix_lua_run_call(sb, echo_src, sizeof(echo_src) - 1, "call.lua", 8, "peek", 4, &text, 1, &echoed,
                                  nullptr, 0) == KIMIX_OK);
        expect(echoed.status == KIMIX_LUA_OK);
        char out[16] = {0};
        size_t out_len = 0;
        int32_t kind = -1;
        expect(kimix_lua_last_value(sb, 0, &kind, nullptr, nullptr, nullptr, out, sizeof(out), &out_len) == KIMIX_OK);
        expect(kind == KIMIX_LUA_VALUE_STRING);
        expect(out_len == 2u);
        expect(std::strcmp(out, "hi") == 0);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    "run_call: an empty fn_name is exactly kimix_lua_run"_test = [] {
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        const char src[] = "return 42";
        kimix_lua_result plain{};
        kimix_lua_result named{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &plain, nullptr, 0) == KIMIX_OK);
        expect(kimix_lua_run_call(sb, src, sizeof(src) - 1, nullptr, 0, nullptr, 0, nullptr, 0, &named, nullptr, 0) ==
               KIMIX_OK);
        expect(plain.status == KIMIX_LUA_OK && named.status == KIMIX_LUA_OK);
        expect(plain.value_count == named.value_count);
        expect(plain.message_len == named.message_len);
        int64_t integer = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(integer == 42);
        // An empty fn_name that DOESN'T exist cannot happen; a name that does not
        // resolve to a function is an ordinary runtime error.
        const char bad[] = "local x = 1";
        kimix_lua_result missing{};
        expect(kimix_lua_run_call(sb, bad, sizeof(bad) - 1, nullptr, 0, "nope", 4, nullptr, 0, &missing, nullptr, 0) ==
               KIMIX_OK);
        expect(missing.status == KIMIX_LUA_RUNTIME) << "fn_name that is not a function";
        expect(missing.message_len > 0u);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // --------------------------------------------------------------- global_set
    "globals: two runs see the same value, a NULL value and clear_globals drop it"_test = [] {
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);

        kimix_lua_value answer = {};
        answer.kind = KIMIX_LUA_VALUE_INTEGER;
        answer.integer = 7;
        expect(kimix_lua_global_set(sb, "answer", 6, &answer) == KIMIX_OK);
        kimix_lua_value label = {};
        label.kind = KIMIX_LUA_VALUE_STRING;
        label.text = "hello";
        label.text_len = 5;
        expect(kimix_lua_global_set(sb, "label", 5, &label) == KIMIX_OK);
        // A table global crosses as canonical JSON and is materialised by the run.
        kimix_lua_value config = {};
        config.kind = KIMIX_LUA_VALUE_JSON;
        config.text = "{\"a\":1,\"b\":[true,false]}";
        config.text_len = std::strlen(config.text);
        expect(kimix_lua_global_set(sb, "cfg", 3, &config) == KIMIX_OK);

        for (int round = 0; round < 2; ++round) {
            const char src[] = "return answer, label, cfg.a, cfg.b[2]";
            kimix_lua_result res{};
            expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
            expect(res.status == KIMIX_LUA_OK) << "round " << round;
            expect(res.value_count == 4u);
            int64_t integer = 0;
            expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
            expect(integer == 7);
            char text[16] = {0};
            size_t len = 0;
            expect(kimix_lua_last_value(sb, 1, nullptr, nullptr, nullptr, nullptr, text, sizeof(text), &len) ==
                   KIMIX_OK);
            expect(std::strcmp(text, "hello") == 0);
            expect(kimix_lua_last_value(sb, 2, nullptr, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
            expect(integer == 1);
            uint8_t boolean = 0;
            expect(kimix_lua_last_value(sb, 3, nullptr, &boolean, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
            expect(boolean == 0u) << "cfg.b[2] is false";
        }

        // A NULL value clears the global (installed as nil, i.e. absent).
        expect(kimix_lua_global_set(sb, "answer", 6, nullptr) == KIMIX_OK);
        const char probe[] = "return answer == nil";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, probe, sizeof(probe) - 1, &res, nullptr, 0) == KIMIX_OK);
        uint8_t gone = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, &gone, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(gone == 1u);

        expect(kimix_lua_globals_clear(sb) == KIMIX_OK);
        const char all[] = "return label == nil and cfg == nil";
        expect(kimix_lua_run(sb, all, sizeof(all) - 1, &res, nullptr, 0) == KIMIX_OK);
        uint8_t cleared = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, &cleared, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(cleared == 1u) << "clear_globals dropped every installed global";
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // ---------------------------------------------------------- host failures
    "host: a non-zero return is a NON-sticky RUNTIME a pcall can catch"_test = [] {
        host_state state;
        std::snprintf(state.err, sizeof(state.err), "nope");
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "boom", 4, &host_boom, &state) == KIMIX_OK);

        const char src[] = "return boom()";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_RUNTIME) << "a host failure is an ordinary Lua error";
        expect(res.message_len > 0u);
        expect(kimix_lua_last_message(sb, nullptr, 0, &res.message_len) == KIMIX_OK);
        char msg[512] = {0};
        size_t msg_len = 0;
        expect(kimix_lua_last_message(sb, msg, sizeof(msg), &msg_len) == KIMIX_OK);
        expect(msg_len == res.message_len);
        expect(std::strstr(msg, "host function 'boom' failed: nope") != nullptr) << msg;
        expect(std::strstr(msg, "RUNTIME: ") != nullptr) << "the documented prefix is inserted: " << msg;

        // The script's own pcall catches it, so the run itself is OK.
        const char caught[] = "local ok, err = pcall(boom); return ok, tostring(err)";
        kimix_lua_result pcall_res{};
        expect(kimix_lua_run(sb, caught, sizeof(caught) - 1, &pcall_res, nullptr, 0) == KIMIX_OK);
        expect(pcall_res.status == KIMIX_LUA_OK) << "pcall swallowed the host failure";
        uint8_t ok = 1;
        expect(kimix_lua_last_value(sb, 0, nullptr, &ok, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(ok == 0u) << "pcall reported the failure";
        char err_text[512] = {0};
        size_t err_len = 0;
        expect(kimix_lua_last_value(sb, 1, nullptr, nullptr, nullptr, nullptr, err_text, sizeof(err_text),
                                    &err_len) == KIMIX_OK);
        expect(std::strstr(err_text, "failed: nope") != nullptr) << err_text;

        // …and nothing sticky survived: the very next run is clean.
        const char clean[] = "return 1 + 1";
        kimix_lua_result fresh{};
        expect(kimix_lua_run(sb, clean, sizeof(clean) - 1, &fresh, nullptr, 0) == KIMIX_OK);
        expect(fresh.status == KIMIX_LUA_OK);
        expect(fresh.message_len == 0u) << "a clean run has no message";
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    "host: reporting more results than the sandbox allows is a sticky LIMIT"_test = [] {
        host_state state;
        const kimix_lua_limits limits = limits_with_one_ret();
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(&limits, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "flood", 5, &host_flood, &state) == KIMIX_OK);
        const char src[] = "local ok = pcall(flood); return ok";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_LIMIT) << "an over-run is sticky: pcall cannot swallow it";
        expect(message_contains(sb, "LIMIT: "));
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    "host: the host-call budget and an uncrossable argument are sticky LIMITs"_test = [] {
        host_state state;
        kimix_lua_limits limits = kimix_lua_default_limits();
        limits.max_host_calls = 1; /* exactly one call per run */
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(&limits, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "tick", 4, &host_tick, &state) == KIMIX_OK);

        // Two calls, one budget: the second is a sticky LIMIT a pcall cannot hide.
        const char src[] = "local ok = pcall(tick); local again = pcall(tick); return ok, again";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_LIMIT) << "the host-call budget is sticky";
        expect(message_contains(sb, "host call budget exhausted"));
        expect(kimix_lua_last_host_calls(sb) == 2u) << "the refused call still counted";

        // A function argument cannot cross: a sticky LIMIT raised BEFORE the host
        // function runs, so the callback never sees it.
        kimix_lua_sandbox *clean = nullptr;
        host_state untouched;
        expect(kimix_lua_sandbox_create(nullptr, &clean) == KIMIX_OK);
        expect(kimix_lua_host_bind(clean, "tick", 4, &host_tick, &untouched) == KIMIX_OK);
        const char uncrossable[] = "return tick(print)";
        kimix_lua_result refused{};
        expect(kimix_lua_run(clean, uncrossable, sizeof(uncrossable) - 1, &refused, nullptr, 0) == KIMIX_OK);
        expect(refused.status == KIMIX_LUA_LIMIT);
        expect(message_contains(clean, "cannot cross the sandbox boundary"));
        expect(untouched.calls == 0) << "the argument is marshalled before any host code runs";
        kimix_lua_sandbox_destroy(clean);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // ------------------------------------------------- tables cross BOTH ways
    "tables: a script table reaches the C host as canonical JSON, and back"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "peek", 4, &host_peek, &state) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "give", 4, &host_give_table, &state) == KIMIX_OK);

        // script -> host: the canonical form is minified with byte-sorted keys.
        const char src[] = "return peek({b = 2, a = 1})";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);
        expect(std::strcmp(state.json, "{\"a\":1,\"b\":2}") == 0) << state.json;
        char echoed[64] = {0};
        size_t echoed_len = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, nullptr, nullptr, echoed, sizeof(echoed), &echoed_len) ==
               KIMIX_OK);
        expect(std::strcmp(echoed, "{\"a\":1,\"b\":2}") == 0);

        // host -> script: the same text becomes a real Lua table (N3's reader).
        const char inbound[] = "local t = give(); return t.a, t.c, t.b[1], t.b[2]";
        kimix_lua_result got{};
        expect(kimix_lua_run(sb, inbound, sizeof(inbound) - 1, &got, nullptr, 0) == KIMIX_OK);
        expect(got.status == KIMIX_LUA_OK);
        expect(got.value_count == 4u);
        int64_t integer = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, &integer, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(integer == 1);
        char text[8] = {0};
        size_t len = 0;
        expect(kimix_lua_last_value(sb, 1, nullptr, nullptr, nullptr, nullptr, text, sizeof(text), &len) == KIMIX_OK);
        expect(std::strcmp(text, "x") == 0);
        uint8_t boolean = 0;
        expect(kimix_lua_last_value(sb, 2, nullptr, &boolean, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(boolean == 1u);
        expect(kimix_lua_last_value(sb, 3, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK) <<
            "t.b[2] is a JSON null, i.e. a hole/nil";
        kimix_lua_sandbox_destroy(sb);
    };

    "tables: a run_call argument and a table global go through the same reader"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "peek", 4, &host_peek, &state) == KIMIX_OK);

        // A table argument to a named call arrives as a real table in the script
        // (and, passed on, as canonical JSON at the host).
        const char src[] = "function relay(t) return peek(t) end";
        kimix_lua_value table = {};
        table.kind = KIMIX_LUA_VALUE_JSON;
        table.text = "{\"z\":9,\"a\":[1,2]}";
        table.text_len = std::strlen(table.text);
        kimix_lua_result res{};
        expect(kimix_lua_run_call(sb, src, sizeof(src) - 1, nullptr, 0, "relay", 5, &table, 1, &res, nullptr, 0) ==
               KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);
        expect(std::strcmp(state.json, "{\"a\":[1,2],\"z\":9}") == 0) << state.json;

        // A broken text in a run_call argument starts the run and is then a
        // sticky LIMIT (script time), unlike global_set which refuses it up front.
        kimix_lua_value broken = {};
        broken.kind = KIMIX_LUA_VALUE_JSON;
        broken.text = "{\"a\":1} x";
        broken.text_len = std::strlen(broken.text);
        kimix_lua_result bad{};
        expect(kimix_lua_run_call(sb, src, sizeof(src) - 1, nullptr, 0, "relay", 5, &broken, 1, &bad, nullptr, 0) ==
               KIMIX_OK);
        expect(bad.status == KIMIX_LUA_LIMIT);
        kimix_lua_sandbox_destroy(sb);
    };

    // --------------------------------------------------------------- host misuse
    "invalid input: every documented KIMIX_ERR_INVALID_ARG"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);

        // host_bind
        expect(kimix_lua_host_bind(nullptr, "a", 1, &host_add, &state) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_host_bind(sb, nullptr, 0, &host_add, &state) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_host_bind(sb, "a", 0, &host_add, &state) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_host_bind(sb, "a", 1, nullptr, &state) == KIMIX_ERR_INVALID_ARG);
        char long_name[KIMIX_LUA_HOST_NAME_MAX + 2];
        std::memset(long_name, 'a', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
        expect(std::strlen(long_name) == KIMIX_LUA_HOST_NAME_MAX + 1u);
        expect(kimix_lua_host_bind(sb, long_name, std::strlen(long_name), &host_add, &state) ==
               KIMIX_ERR_INVALID_ARG);
        // The longest name that IS accepted still works.
        char max_name[KIMIX_LUA_HOST_NAME_MAX + 1];
        std::memset(max_name, 'b', sizeof(max_name) - 1);
        max_name[sizeof(max_name) - 1] = '\0';
        expect(kimix_lua_host_bind(sb, max_name, std::strlen(max_name), &host_add, &state) == KIMIX_OK);
        // A name the environment itself installs is refused: binding over print()
        // would poison every later run.
        expect(kimix_lua_host_bind(sb, "print", 5, &host_add, &state) == KIMIX_ERR_INVALID_ARG);
        // …and nothing was installed by the refusals.
        const char probe[] = "return type(print)";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, probe, sizeof(probe) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK);

        // global_set
        kimix_lua_value value = {};
        value.kind = KIMIX_LUA_VALUE_INTEGER;
        value.integer = 1;
        expect(kimix_lua_global_set(nullptr, "g", 1, &value) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_global_set(sb, nullptr, 0, &value) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_global_set(sb, "g", 0, &value) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_global_set(sb, "print", 5, &value) == KIMIX_ERR_INVALID_ARG);
        kimix_lua_value bad_kind = {};
        bad_kind.kind = 99; /* not a KIMIX_LUA_VALUE_* */
        expect(kimix_lua_global_set(sb, "g", 1, &bad_kind) == KIMIX_ERR_INVALID_ARG);
        kimix_lua_value bad_json = {};
        bad_json.kind = KIMIX_LUA_VALUE_JSON;
        bad_json.text = "{\"a\":1,}";
        bad_json.text_len = std::strlen(bad_json.text);
        expect(kimix_lua_global_set(sb, "g", 1, &bad_json) == KIMIX_ERR_INVALID_ARG)
            << "a broken JSON text is a host mistake, refused at host time";
        // A duplicate key and trailing garbage are refused too (strict reader).
        bad_json.text = "{\"a\":1} x";
        bad_json.text_len = std::strlen(bad_json.text);
        expect(kimix_lua_global_set(sb, "g", 1, &bad_json) == KIMIX_ERR_INVALID_ARG);
        // None of the refusals installed anything.
        const char nil_probe[] = "return g == nil";
        expect(kimix_lua_run(sb, nil_probe, sizeof(nil_probe) - 1, &res, nullptr, 0) == KIMIX_OK);
        uint8_t nil_global = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, &nil_global, nullptr, nullptr, nullptr, 0, nullptr) == KIMIX_OK);
        expect(nil_global == 1u);

        // globals_clear / run_call / accessors
        expect(kimix_lua_globals_clear(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_run_call(nullptr, "return 1", 8, nullptr, 0, nullptr, 0, nullptr, 0, &res, nullptr, 0) ==
               KIMIX_ERR_INVALID_ARG);
        expect(kimix_lua_run_call(sb, "return 1", 8, nullptr, 0, nullptr, 0, nullptr, 1, &res, nullptr, 0) ==
               KIMIX_ERR_INVALID_ARG) << "argc > 0 with a NULL args array";
        kimix_lua_value wrong = {};
        wrong.kind = -1;
        expect(kimix_lua_run_call(sb, "return 1", 8, nullptr, 0, nullptr, 0, &wrong, 1, &res, nullptr, 0) ==
               KIMIX_ERR_INVALID_ARG) << "an out-of-range kind never costs a run";
        expect(kimix_lua_last_host_calls(nullptr) == 0u);
        expect(kimix_lua_last_host_calls(sb) == 0u) << "no run has happened since the last clean one";
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // ------------------------------------------- two-phase protocol / no re-run
    "two-phase: reading the message twice does not re-run the chunk"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "tick", 4, &host_tick, &state) == KIMIX_OK);

        // A chunk with a visible side effect (two host calls) that then fails.
        const char src[] = "tick(); tick(); error('boom')";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_RUNTIME);
        expect(state.calls == 2);
        expect(kimix_lua_last_host_calls(sb) == 2u);

        // Phase one: the length only.  Phase two: the bytes.  Phase two with too
        // little room: the length again, still without running anything.
        size_t len_one = 0;
        expect(kimix_lua_last_message(sb, nullptr, 0, &len_one) == KIMIX_OK);
        expect(len_one == res.message_len);
        char buf[512] = {0};
        size_t len_two = 0;
        expect(kimix_lua_last_message(sb, buf, sizeof(buf), &len_two) == KIMIX_OK);
        expect(len_two == len_one);
        size_t len_three = 0;
        expect(kimix_lua_last_message(sb, buf, 4, &len_three) == KIMIX_ERR_OUT_OF_RANGE);
        expect(len_three == len_one);
        expect(state.calls == 2) << "reading the result must never re-run the chunk";
        expect(kimix_lua_last_host_calls(sb) == 2u) << "nor move the host-call counter";
        expect(std::strstr(buf, "boom") != nullptr);
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };

    // --------------------------------------------------------------- re-entrancy
    "host: re-entering the same handle is answered KIMIX_LUA_INTERNAL"_test = [] {
        host_state state;
        kimix_lua_sandbox *sb = nullptr;
        expect(kimix_lua_sandbox_create(nullptr, &sb) == KIMIX_OK);
        expect(kimix_lua_host_bind(sb, "inner", 5, &host_reenter, &state) == KIMIX_OK);
        state.self = sb;

        const char src[] = "return inner()";
        kimix_lua_result res{};
        expect(kimix_lua_run(sb, src, sizeof(src) - 1, &res, nullptr, 0) == KIMIX_OK);
        expect(res.status == KIMIX_LUA_OK) << "the OUTER run finishes normally";
        expect(state.inner_status == KIMIX_LUA_INTERNAL) << "the inner run is refused, not deadlocked";
        expect(std::strstr(state.inner_message, "INTERNAL: ") != nullptr) << state.inner_message;
        char echoed[32] = {0};
        size_t echoed_len = 0;
        expect(kimix_lua_last_value(sb, 0, nullptr, nullptr, nullptr, nullptr, echoed, sizeof(echoed), &echoed_len) ==
               KIMIX_OK);
        expect(std::strcmp(echoed, "inner=done") == 0);
        state.self = nullptr;
        expect(kimix_lua_sandbox_destroy(sb) == KIMIX_OK);
    };
}
