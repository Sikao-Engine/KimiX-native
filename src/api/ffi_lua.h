/*
 * ffi_lua.h -- the Lua script sandbox (kimix-script) as a plain C ABI.
 *
 * WHAT THIS FAMILY IS
 * -------------------
 * A foreign caller (C, C#, Zig, Rust, Python ctypes/cffi, Julia, Node ffi-napi)
 * creates ONE opaque sandbox handle and runs Lua chunks through it.  Each run
 * compiles and executes one chunk in a throw-away, budgeted lua_State whose
 * environment holds only a vetted subset of the standard library: a script
 * cannot read or write a file, spawn a process, load a module, reach debug
 * introspection, obtain an address, exit the process, read the environment, or
 * allocate/run without a bound.  An attempt is a LOUD denial that ABORTS the run
 * and comes back here as `status == KIMIX_LUA_DENIED` plus a message of the
 * documented shape
 *
 *     <chunk>:<line>: DENIED: <api> <EM DASH> <why>
 *
 * where <EM DASH> is U+2014 (the three UTF-8 bytes E2 80 94, the separator
 * src/script/script_error.cpp emits via SB_DASH), e.g.
 *
 *     script.lua:7: DENIED: io.open \xE2\x80\x94 file I/O is not available in the sandbox
 *
 * It is NOT the ASCII "--": a foreign caller matches on the "DENIED: " prefix or
 * on the api name, never on the separator, and treats every message as UTF-8.
 *
 * even when the script wrapped the call in its own pcall/xpcall/coroutine.resume.
 * The full model (five layers, capability matrix, residual risks) is
 * src/script/README.md; the C++ twin of this surface is
 * <script/script_sandbox.h>.
 *
 * TWO-PHASE, CALLER-OWNED BUFFERS
 * -------------------------------
 * There is NO library allocation on any read path.  Every variable-length result
 * (message, output, traceback, violation strings, a string value) uses the same
 * protocol as the rest of kimix_api: pass NULL/0 to learn the required length,
 * then pass caller storage.  A buffer that is too small answers
 * KIMIX_ERR_OUT_OF_RANGE and STILL reports the required length, so a caller can
 * resize and retry.  Text is copied with a NUL terminator (the length excludes
 * it); a payload is never NUL-free on this surface, because every string here is
 * either a message or a Lua string value.
 *
 * OWNERSHIP
 * ---------
 * The ONLY object this area allocates is the sandbox handle
 * (kimix_lua_sandbox_create); it is released by exactly one
 * kimix_lua_sandbox_destroy.  Everything the caller receives is copied into the
 * caller's own buffers, so no pointer from this library ever has to be handed
 * back -- the one-heap rule of api/ffi_common.h holds trivially here.  The
 * handle itself is MIMALLOC memory inside this image (the same heap as every
 * container it holds), so it must be given back through
 * kimix_lua_sandbox_destroy and never through the caller's free()/delete.
 * `kimix_lua_status_name` and `kimix_lua_version_string` return static storage
 * owned by the library; never free them.
 *
 * THREADING
 * ---------
 * A sandbox handle is NOT thread-safe: a Lua state is single-threaded and the
 * handle keeps one per run.  A concurrent kimix_lua_run answers
 * KIMIX_LUA_INTERNAL ("concurrent run on one Sandbox").  kimix_lua_cancel is the
 * ONLY call that may be made from another thread while a run is in flight; the
 * count hook then aborts the run with KIMIX_LUA_CANCELLED.  One handle per worker
 * thread is the intended deployment.
 *
 * THE HOST SURFACE (section 6)
 * ----------------------------
 * The host can extend a run WITHOUT weakening it.  kimix_lua_host_bind installs
 * a plain C function under a Lua name; the script sees an ordinary function,
 * never a pointer into the host, and the host function never sees the
 * interpreter, a lua_State or any library-owned pointer that outlives the call.
 * kimix_lua_global_set installs a COPIED constant, kimix_lua_globals_clear drops
 * them all, and kimix_lua_run_call runs a chunk and then calls one of its
 * functions with arguments -- so a caller can drive a long-lived Lua library
 * without ever splicing text into the source.
 *
 * What may cross, in both directions: nil, boolean, integer, number, string and
 * a table (carried as canonical JSON text: minified, object keys byte-sorted).
 * A function, a thread and a userdata NEVER cross -- a script handing one to a
 * host function is a sticky KIMIX_LUA_LIMIT, and the host can only hand back the
 * six kinds, so the pointer-control rule holds on the host path too.  A table's
 * JSON is produced and consumed by the same writer/reader pair (src/script), so
 * `json` and `table` mean literally the same bytes in both directions.
 *
 * RESERVED NAMES
 * --------------
 * The host may not bind or set a name the environment itself installs ("print",
 * "pcall", "io", "string", "_G", ... -- the authoritative list is one table in
 * src/script/script_env.cpp).  Overwriting one would delete a capability or
 * poison every later run, so the two host entry points answer
 * KIMIX_ERR_INVALID_ARG for it.  This is host time: never a raise, never a run.
 *
 * STATUS CONTRACT
 * ---------------
 * - kimix_lua_sandbox_create / _destroy / _cancel / the accessors return the
 *   house `kimix_status` (KIMIX_OK == 0).
 * - kimix_lua_run / kimix_lua_run_call return `kimix_status` for the CALL (bad
 *   arguments, no handle) and report the CHUNK's outcome in `result->status`,
 *   one of the KIMIX_LUA_* codes below -- KIMIX_LUA_OK == 0, so
 *   `if (res.status)` reads as "the run failed".
 * - kimix_lua_host_bind answers KIMIX_ERR_INVALID_ARG for a NULL handle, a NULL
 *   `fn`, an empty/over-long name (KIMIX_LUA_HOST_NAME_MAX), and a reserved
 *   name.
 * - kimix_lua_global_set answers KIMIX_ERR_INVALID_ARG for a NULL handle, an
 *   empty name, a reserved name, a NULL/out-of-range `value->kind`, and a
 *   `json` text that is not strict JSON -- every one of those is a HOST mistake,
 *   so it is reported here instead of poisoning a later run.  A NULL `value` or
 *   KIMIX_LUA_VALUE_NIL clears that global.
 * - kimix_lua_run_call with an empty `fn_name` behaves EXACTLY like
 *   kimix_lua_run (same chunk name, same results, same message).
 * - A host function that returns non-zero fails the run with
 *   KIMIX_LUA_RUNTIME and the message
 *       <chunk>:<line>: RUNTIME: host function '<name>' failed: <ctx->err>
 *   which is a NORMAL Lua error: a `pcall` in the script catches it and the run
 *   can still finish KIMIX_LUA_OK.
 * - A host budget (limits.max_host_calls / max_host_args /
 *   max_host_ret_values) exceeded, or an argument/result that cannot cross
 *   (a function, a thread, a userdata, a cyclic or over-cap table, a `json` text
 *   that is not strict JSON), is a STICKY KIMIX_LUA_LIMIT: the script's own
 *   pcall cannot swallow it, and the run's results are discarded.
 * - A host function that re-enters the same handle (kimix_lua_run/_run_call on
 *   the handle it is called from) is answered KIMIX_LUA_INTERNAL by the inner
 *   call; the outer run continues normally.
 * - A `fn_name` that is not a function is KIMIX_LUA_RUNTIME.
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_LUA_H
#define KIMIX_API_FFI_LUA_H

#include <api/ffi_common.h>

KIMIX_FFI_BEGIN

/* The opaque handle: one per worker thread, created by
 * kimix_lua_sandbox_create and released by kimix_lua_sandbox_destroy. */
typedef struct kimix_lua_sandbox kimix_lua_sandbox;

/* ===========================================================================
 * 1. Vocabulary
 * ======================================================================== */

/* The outcome of one run.  Numerically identical to kimix::script::Status, so a
 * binding can switch on the same integers as the C++ side. */
typedef enum kimix_lua_status_code {
    KIMIX_LUA_OK = 0,         /* the chunk ran; results are available */
    KIMIX_LUA_SYNTAX = 1,     /* the chunk did not compile (Lua's own message) */
    KIMIX_LUA_RUNTIME = 2,    /* an ordinary Lua error */
    KIMIX_LUA_DENIED = 3,     /* a tripwire: a forbidden capability was called */
    KIMIX_LUA_MEMORY = 4,     /* the memory budget was exhausted */
    KIMIX_LUA_TIMEOUT = 5,    /* the deadline / instruction budget was exhausted */
    KIMIX_LUA_BEHAVIOUR = 6,  /* the script replaced a sandbox primitive */
    KIMIX_LUA_CANCELLED = 7,  /* kimix_lua_cancel was called during the run */
    KIMIX_LUA_LIMIT = 8,      /* an output / result / source cap was exceeded */
    KIMIX_LUA_INTERNAL = 9    /* host misuse or a Lua panic (never a script error) */
} kimix_lua_status_code;

/* The value kinds a run can return.  A function, a thread and a userdata NEVER
 * cross this boundary (the run answers KIMIX_LUA_LIMIT instead), which is the
 * outbound half of the pointer-control rule. */
typedef enum kimix_lua_value_kind {
    KIMIX_LUA_VALUE_NIL = 0,
    KIMIX_LUA_VALUE_BOOLEAN = 1,
    KIMIX_LUA_VALUE_INTEGER = 2,
    KIMIX_LUA_VALUE_NUMBER = 3,
    KIMIX_LUA_VALUE_STRING = 4,
    KIMIX_LUA_VALUE_JSON = 5 /* a table, re-emitted as canonical JSON text */
} kimix_lua_value_kind;

/* The C-ABI value.  Mirrors kimix::script::Value; 48 bytes on x64/arm64.
 * `text` is NUL-terminated for kind STRING/JSON; text_len excludes the NUL.
 *
 * The value is a PURE DATA snapshot: text_len is authoritative on the way in
 * (the bytes are COPIED before use, so the library never keeps a caller pointer
 * and `text` does not have to be NUL-terminated there), and every value the
 * library hands out is valid only for the duration of the call that produced it.
 * An out-of-range `kind` is a host mistake (KIMIX_ERR_INVALID_ARG); `_reserved`
 * and `_reserved2` must be 0 and are ignored. */
typedef struct kimix_lua_value {
    int32_t kind; /* KIMIX_LUA_VALUE_* (the same vocabulary as results) */
    int32_t _reserved; /* 0 */
    int64_t integer;
    double number;
    uint8_t boolean;
    uint8_t _reserved2[7];
    const char *text; /* string bytes / canonical JSON text */
    size_t text_len; /* excludes the terminator */
} kimix_lua_value;

/* Longest accepted host-function / global name, in bytes: the sandbox composes
 * its own messages from a bounded copy of the name, so a longer one is refused
 * with KIMIX_ERR_INVALID_ARG rather than being silently truncated. */
#define KIMIX_LUA_HOST_NAME_MAX 64

/* The resource contract of a run.  `kimix_lua_default_limits()` returns the
 * library defaults (16 MiB state, 1 MiB source, 2.000 s deadline, 256 KiB
 * results, 64 return values, 10'000 host calls per run, fresh state per run, no
 * coroutines, no collectgarbage).
 *
 * sizeof == 96 since ABI 3 (88 before): the three uint32_t host budgets sit
 * IMMEDIATELY after max_result_depth, so everything from hook_granularity on is
 * 12 bytes further along than in ABI 2 -- that field move is the ABI break, not
 * the size alone.  A binding must rebuild, not just resize. */
typedef struct kimix_lua_limits {
    size_t max_memory_bytes; /* whole-state budget, all threads */
    size_t max_source_bytes; /* the chunk, and what `load` may accumulate */
    size_t max_string_bytes; /* a single allocation cap */
    size_t max_output_bytes; /* captured print()/warn() text */
    size_t max_result_bytes; /* marshalled results (canonical JSON for tables) */
    uint32_t max_result_values;
    uint32_t max_result_depth; /* nesting cap for a returned table */
    uint32_t max_host_calls; /* script -> host calls per run (0 = no host call) */
    uint32_t max_host_args; /* arguments one host function accepts */
    uint32_t max_host_ret_values; /* values one host function may return */
    uint32_t hook_granularity; /* count-hook period (instructions) */
    uint32_t random_seed; /* math.randomseed, pinned for determinism */
    unsigned seed; /* interpreter-state seed, pinned for determinism */
    double max_seconds; /* wall-clock deadline, 0 = none */
    uint64_t max_instructions; /* instruction budget, 0 = unbounded */
    uint8_t fresh_state_per_run; /* 1 (default): perfect isolation */
    uint8_t allow_coroutines; /* 0 (default): no coroutine library */
    uint8_t allow_collectgarbage_count; /* 0 (default): collectgarbage denied */
    uint8_t _reserved; /* always 0 */
} kimix_lua_limits;

/* What one run produced.  The variable-length parts (message, output,
 * traceback, violation strings, values) are read through the accessors below --
 * this struct carries only lengths and counters. */
typedef struct kimix_lua_result {
    int32_t status;      /* one of KIMIX_LUA_* */
    int32_t line;        /* the script line of the failure, 0 when unknown */
    size_t message_len;  /* required bytes (excluding the NUL) */
    size_t output_len;   /* captured print()/warn() text */
    size_t traceback_len;
    size_t value_count;  /* number of return values marshalled */
    size_t memory_peak;  /* peak live bytes of the state during the run */
    uint64_t instructions;
    double seconds;
} kimix_lua_result;

/* ===========================================================================
 * 2. Library / ABI queries
 * ======================================================================== */

/* KIMIX_API_ABI_VERSION as compiled into this image; a binding asserts on it at
 * load time.  Never fails. */
KIMIX_FFI uint32_t kimix_lua_abi_version(void);

/* The kimix-core version string of this build.  Static storage; never free it. */
KIMIX_FFI const char *kimix_lua_version_string(void);

/* The textual name of a KIMIX_LUA_* code ("ok", "denied", ...); an unknown code
 * answers "unknown".  Static storage; never free it. */
KIMIX_FFI const char *kimix_lua_status_name(KIMIX_IN int32_t status);

/* The library defaults, as a value a caller can start from and edit. */
KIMIX_FFI kimix_lua_limits kimix_lua_default_limits(void);

/* ===========================================================================
 * 3. Sandbox lifecycle
 * ======================================================================== */

/* Create one sandbox -- the ONLY allocation of this family.  A NULL `limits`
 * uses the defaults.  `*out` is written only on KIMIX_OK.  Release with exactly
 * one kimix_lua_sandbox_destroy.  KIMIX_ERR_INVALID_ARG for a NULL `out`. */
KIMIX_FFI kimix_status kimix_lua_sandbox_create(KIMIX_IN KIMIX_NULLABLE const kimix_lua_limits *limits,
                                                KIMIX_OUT kimix_lua_sandbox **out);

/* Destroy the handle (a run in flight must have finished; cancel it first and
 * join your own thread).  A NULL handle is ignored. */
KIMIX_FFI kimix_status kimix_lua_sandbox_destroy(KIMIX_TRANSFER kimix_lua_sandbox *sb);

/* Ask the in-flight run to abort -- the only cross-thread call.  The count hook
 * raises KIMIX_LUA_CANCELLED; the flag is cleared at the start of the next run.
 * KIMIX_ERR_INVALID_ARG for a NULL handle. */
KIMIX_FFI kimix_status kimix_lua_cancel(KIMIX_IN kimix_lua_sandbox *sb);

/* ===========================================================================
 * 4. Running a chunk
 * ======================================================================== */

/* Compile and run `src` (length-based: an embedded NUL is just a byte) in the
 * sandbox environment, with the chunk name "script.lua".  The source is copied
 * before use.
 *
 *   sb       the handle (KIMIX_ERR_INVALID_ARG when NULL)
 *   src      the chunk source; NULL is treated as an empty chunk
 *   src_len  its length in bytes
 *   res      REQUIRED out-parameter: the run's outcome (written on KIMIX_OK and
 *            on KIMIX_ERR_OUT_OF_RANGE, so a two-phase caller always learns the
 *            required message length)
 *   msg_buf  caller storage for the message, or NULL to only learn its length
 *            (kimix_lua_last_message then copies it out without re-running)
 *   msg_cap  its capacity in bytes (NUL terminator included)
 *
 * Returns KIMIX_OK when the call itself succeeded (INSPECT res->status for the
 * chunk), KIMIX_ERR_OUT_OF_RANGE when msg_buf is too small (res->message_len is
 * still filled), KIMIX_ERR_INVALID_ARG for a NULL sb/res.
 *
 * This is the documented surface of the plan; use kimix_lua_run_named when the
 * message should name a real file. */
KIMIX_FFI kimix_status kimix_lua_run(KIMIX_IN kimix_lua_sandbox *sb,
                                     KIMIX_IN KIMIX_NULLABLE const char *src,
                                     KIMIX_IN size_t src_len,
                                     KIMIX_OUT kimix_lua_result *res,
                                     KIMIX_OUT KIMIX_NULLABLE char *msg_buf,
                                     KIMIX_IN size_t msg_cap);

/* Same as kimix_lua_run, with an explicit chunk name (it appears in every
 * message; keep it short -- LUA_IDSIZE is 60).  A NULL/empty name means
 * "script.lua". */
KIMIX_FFI kimix_status kimix_lua_run_named(KIMIX_IN kimix_lua_sandbox *sb,
                                           KIMIX_IN KIMIX_NULLABLE const char *src,
                                           KIMIX_IN size_t src_len,
                                           KIMIX_IN KIMIX_NULLABLE const char *chunk_name,
                                           KIMIX_IN size_t chunk_name_len,
                                           KIMIX_OUT kimix_lua_result *res,
                                           KIMIX_OUT KIMIX_NULLABLE char *msg_buf,
                                           KIMIX_IN size_t msg_cap);

/* ===========================================================================
 * 5. Reading the last run (two-phase, caller-owned buffers)
 * ======================================================================== */

/* The message of the last run -- the documented "<chunk>:<line>: <PREFIX>: ..."
 * text that kimix_lua_run copies into its `msg_buf`, kept here so a caller that
 * ran with msg_buf == NULL (phase one) can read it WITHOUT running the chunk a
 * second time.  Empty after a clean run (a successful run has no message).
 * KIMIX_ERR_OUT_OF_RANGE when `buf` is too small (with `*out_len` still filled);
 * KIMIX_OK with `buf == NULL` reports the length only. */
KIMIX_FFI kimix_status kimix_lua_last_message(KIMIX_IN kimix_lua_sandbox *sb,
                                              KIMIX_OUT KIMIX_NULLABLE char *buf,
                                              KIMIX_IN size_t cap,
                                              KIMIX_OUT size_t *out_len);

/* The captured print()/warn() text of the last run.  KIMIX_ERR_OUT_OF_RANGE when
 * `buf` is too small (with `*out_len` still filled); KIMIX_OK with `buf == NULL`
 * reports the length only. */
KIMIX_FFI kimix_status kimix_lua_last_output(KIMIX_IN kimix_lua_sandbox *sb,
                                             KIMIX_OUT KIMIX_NULLABLE char *buf,
                                             KIMIX_IN size_t cap,
                                             KIMIX_OUT size_t *out_len);

/* The bounded traceback of the last failure (empty after a clean run). */
KIMIX_FFI kimix_status kimix_lua_last_traceback(KIMIX_IN kimix_lua_sandbox *sb,
                                                KIMIX_OUT KIMIX_NULLABLE char *buf,
                                                KIMIX_IN size_t cap,
                                                KIMIX_OUT size_t *out_len);

/* The violation record of the last DENIED run: the exact api ("io.open"), its
 * category ("file-io"), the human detail and the full chunk name.  Any of the
 * buffers may be NULL to skip that field; every `*_len` is always written. */
KIMIX_FFI kimix_status kimix_lua_last_violation(KIMIX_IN kimix_lua_sandbox *sb,
                                                KIMIX_OUT KIMIX_NULLABLE char *api,
                                                KIMIX_IN size_t api_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *api_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *category,
                                                KIMIX_IN size_t category_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *category_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *detail,
                                                KIMIX_IN size_t detail_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *detail_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *chunk,
                                                KIMIX_IN size_t chunk_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *chunk_len);

/* One marshalled return value of the last run.  `kind` receives a
 * KIMIX_LUA_VALUE_*; integers/numbers land in `i`/`d`, booleans in `b`, and a
 * string or a JSON table lands in `str_buf` (two-phase, like every other buffer:
 * NULL reports the length in `*str_len`).  KIMIX_ERR_OUT_OF_RANGE for an index
 * >= value_count or a too-small buffer; a NULL `kind` is allowed. */
KIMIX_FFI kimix_status kimix_lua_last_value(KIMIX_IN kimix_lua_sandbox *sb,
                                            KIMIX_IN size_t index,
                                            KIMIX_OUT KIMIX_NULLABLE int32_t *kind,
                                            KIMIX_OUT KIMIX_NULLABLE uint8_t *b,
                                            KIMIX_OUT KIMIX_NULLABLE int64_t *i,
                                            KIMIX_OUT KIMIX_NULLABLE double *d,
                                            KIMIX_OUT KIMIX_NULLABLE char *str_buf,
                                            KIMIX_IN size_t str_cap,
                                            KIMIX_OUT KIMIX_NULLABLE size_t *str_len);

/* ===========================================================================
 * 6. The host surface (see "THE HOST SURFACE" above)
 * ======================================================================== */

/* Handed to a host function.  Library-owned, valid ONLY for the duration of the
 * call; ctx->err is library scratch: write a UTF-8 message there and return
 * non-zero.  Nothing here may be stored across the call -- `user` is the caller's
 * own pointer, `err` dies with the call, and `name` is the sandbox's own stable
 * copy of the bound name (do not free it, do not write to it). */
typedef struct kimix_lua_host_ctx {
    void *user; /* as passed to kimix_lua_host_bind */
    const char *name; /* the bound name, NUL-terminated */
    size_t name_len;
    char *err; /* >= 512 bytes; NUL-terminate before returning */
    size_t err_cap;
    uint64_t call_index; /* 1-based ordinal within the run */
} kimix_lua_host_ctx;

/* 0 = ok, *ret_count results were written; non-zero = failed, ctx->err explains.
 * Must not call back into this sandbox and must not keep any pointer it is given.
 * `args`/`rets` are an array view owned by the library and valid only for this
 * call; ret_cap is what the sandbox allows (limits.max_host_ret_values). */
typedef int32_t (*kimix_lua_host_fn)(kimix_lua_host_ctx *ctx,
                                     const kimix_lua_value *args, size_t argc,
                                     kimix_lua_value *rets, size_t ret_cap,
                                     size_t *ret_count);

/* Install `name` into every subsequent run's environment as a host function.
 * Re-binding a name replaces the function and the user pointer.
 *
 * name the bound name (an identifier: [A-Za-z_][A-Za-z0-9_]*, 1..
 * KIMIX_LUA_HOST_NAME_MAX bytes); the bytes are copied
 * fn the C function; NULL is KIMIX_ERR_INVALID_ARG
 * user handed back verbatim as ctx->user -- the library never touches it
 *
 * KIMIX_OK, or KIMIX_ERR_INVALID_ARG for a NULL handle/NULL name/empty or
 * over-long name/a NULL `fn`/a name the environment itself installs (a reserved
 * name, see "RESERVED NAMES").  Host time: no run happens, nothing is raised.
 * Must not be called concurrently with a run on the same handle. */
KIMIX_FFI kimix_status kimix_lua_host_bind(KIMIX_IN kimix_lua_sandbox *sb,
                                           KIMIX_IN const char *name,
                                           KIMIX_IN size_t name_len,
                                           KIMIX_IN kimix_lua_host_fn fn,
                                           KIMIX_IN KIMIX_NULLABLE void *user);

/* Install (or overwrite) one global in every subsequent run's environment.  The
 * value is COPIED.  A KIMIX_LUA_VALUE_JSON value is carried as its canonical
 * JSON text and materialised into a real Lua table by the run, so a host hands
 * over a table exactly the way it receives one.
 *
 * `value` NULL or KIMIX_LUA_VALUE_NIL CLEARS that global.
 *
 * KIMIX_OK, or KIMIX_ERR_INVALID_ARG for a NULL handle, an empty/over-long name,
 * a reserved name, a `kind` that is not a KIMIX_LUA_VALUE_*, or a `json` text
 * that is not strict JSON (trailing garbage, comments, NaN, duplicate keys, over
 * the limits' result/depth/string caps).  Every one of those is a HOST mistake,
 * so it is refused here -- never a sticky LIMIT that would poison a later run.
 * Must not be called concurrently with a run on the same handle. */
KIMIX_FFI kimix_status kimix_lua_global_set(KIMIX_IN kimix_lua_sandbox *sb,
                                            KIMIX_IN const char *name,
                                            KIMIX_IN size_t name_len,
                                            KIMIX_IN KIMIX_NULLABLE const kimix_lua_value *value);

/* Drop every global installed with kimix_lua_global_set (host functions stay
 * bound).  KIMIX_ERR_INVALID_ARG for a NULL handle. */
KIMIX_FFI kimix_status kimix_lua_globals_clear(KIMIX_IN kimix_lua_sandbox *sb);

/* Run `src` and then CALL one of its functions, with arguments.
 *
 * The chunk's own return values are the setup prelude and are dropped; the
 * answer is what `fn_name` returns.  Everything else is kimix_lua_run: same
 * chunk-name default, same limits, same two-phase message protocol, same
 * result->status vocabulary.  An EMPTY fn_name is exactly kimix_lua_run.
 *
 * fn_name  the function to call, looked up in the run environment (not in the
 *          interpreter's global table); not a function => KIMIX_LUA_RUNTIME
 * args/argc the arguments, copied before use; NULL with argc == 0 is fine.
 *          An out-of-range kind or a `json` text that is not strict JSON is a
 *          sticky KIMIX_LUA_LIMIT (the run started), unlike kimix_lua_global_set
 *          which refuses the same text at host time.
 *
 * KIMIX_OK / KIMIX_ERR_OUT_OF_RANGE / KIMIX_ERR_INVALID_ARG exactly like
 * kimix_lua_run (a NULL `args` with argc > 0 is KIMIX_ERR_INVALID_ARG). */
KIMIX_FFI kimix_status kimix_lua_run_call(KIMIX_IN kimix_lua_sandbox *sb,
                                          KIMIX_IN KIMIX_NULLABLE const char *src,
                                          KIMIX_IN size_t src_len,
                                          KIMIX_IN KIMIX_NULLABLE const char *chunk_name,
                                          KIMIX_IN size_t chunk_name_len,
                                          KIMIX_IN KIMIX_NULLABLE const char *fn_name,
                                          KIMIX_IN size_t fn_name_len,
                                          KIMIX_IN KIMIX_NULLABLE const kimix_lua_value *args,
                                          KIMIX_IN size_t argc,
                                          KIMIX_OUT kimix_lua_result *res,
                                          KIMIX_OUT KIMIX_NULLABLE char *msg_buf,
                                          KIMIX_IN size_t msg_cap);

/* Host calls made during the last run (0 on a NULL handle, and 0 before the
 * first run).  A counter, not a field of kimix_lua_result, so that struct stays
 * 64 bytes; read it after kimix_lua_run/_run_call. */
KIMIX_FFI uint64_t kimix_lua_last_host_calls(KIMIX_IN KIMIX_NULLABLE kimix_lua_sandbox *sb);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_LUA_H */
