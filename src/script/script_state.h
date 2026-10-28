/*
 * script_state.h -- INTERNAL: the per-sandbox state header and the private
 * implementation type of kimix::script::Sandbox.
 *
 * Everything in this header is internal to src/script: it is NOT part of the
 * public surface (a consumer includes script_sandbox.h and never sees it), and
 * no lua_State -- and no Lua header -- leaks out of the module (README
 * §"Native-side rules").
 *
 * The state object lives inside the Sandbox (one per Sandbox, not per
 * lua_State): lua_getextraspace(L) holds a pointer to it, and lua_newthread
 * COPIES the extraspace (lstate.c), so a coroutine resolves the exact same
 * header.  That is what makes the sticky violation flag, the allocator budget
 * and the count hook work on every thread of a run.
 */
#pragma once
#ifndef KIMIX_SCRIPT_STATE_H
#define KIMIX_SCRIPT_STATE_H

#include <atomic>
#include <cstdint>

#include <core/clock.h>
/* kimix::IOperatorNewBase: Sandbox::Impl is heap-allocated through mimalloc, so
 * it never touches the CRT heap (src/core/memory.h). */
#include <core/memory.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <script/script_sandbox.h>
#include <script/script_status.h>
#include <script/script_value.h>

struct lua_State;
struct lua_Debug;

namespace kimix::script {

/* Forward-declared here (defined below) because SandboxState points at the
 * Sandbox's lists of host-provided natives and installed globals. */
struct SandboxNative;
struct SandboxGlobal;

/* Fixed size of the composed message body ("DENIED: api -- detail").  Keeping
 * it a member array (not a kimix::string) is what lets every C function in this
 * module be POD-only: nothing needs a destructor if a tripwire longjmps out. */
constexpr size_t k_body_capacity = 512;

/* Extra budget granted while a message/finalizer is being composed, so an
 * out-of-budget state can still report itself (bounded, ~64 KB). */
constexpr size_t k_report_slack_bytes = 64 * 1024;

/* Cap for the bounded traceback attached to a failure. */
constexpr size_t k_traceback_bytes = 4096;

/* Size of SandboxState::host_err, the POD error scratch a host function writes
 * into (HostCall::err / the C ABI's kimix_lua_host_ctx::err).  A plain member
 * array, never a kimix::string: a host failure can be raised (longjmp) right
 * after it is read, so it must need no destructor. */
constexpr size_t k_host_err_bytes = 512;

/* Number of upvalues a replacement primitive carries: [1] = its tag. */
constexpr int k_primitive_upvalues = 1;

/* Number of upvalues a host-function binding carries: [1] = the host's user
 * pointer (light userdata), [2] = the binding's index in
 * SandboxState::host_bindings.  Together they let sb_host_trampoline resolve the
 * binding without a pointer that could outlive it, and let it PROVE the binding
 * is still the one this closure was created for. */
constexpr int k_host_binding_upvalues = 2;

/* Per-primitive identity tags: `lua_tocfunction` alone cannot tell two closures
 * of the same C function apart, so every sandbox primitive carries its tag as
 * upvalue 1 and the env integrity post-check compares both. */
enum SbTag : int32_t {
    kTagNone = 0,
    kTagLoad,
    kTagPcall,
    kTagXpcall,
    kTagPrint,
    kTagWarn,
    kTagTostring,
    kTagCollectgarbage,
    kTagFormat,
    kTagStringDump,
    kTagCoroutineResume,
    kTagCoroutineClose,
    kTagTripwireFunction, /* dofile / loadfile / require / loadlib / string.dump */
    kTagTripwireIndex,    /* io / os / package / debug metatable __index */
    kTagTripwireTostring,
};

/* One host-provided native.  `add_native` fills fn/upvalues (a raw
 * lua_CFunction that sees the interpreter); `bind_host_function` fills
 * host/host_fn/user and installs `sb_host_trampoline` instead, so the host
 * callback is never reachable from script code as a C function pointer. */
struct SandboxNative {
    kimix::string name;
    int (*fn)(lua_State *) = nullptr;
    int upvalues = 0;
    void *user = nullptr;           /* the opaque pointer handed back to HostFn */
    bool host = false;              /* true = a bind_host_function binding */
    HostFn host_fn = nullptr;       /* the host callback (host == true only) */
};

/* One global installed by Sandbox::set_global.  The value is a COPY, so nothing
 * the host owns is referenced across a run. */
struct SandboxGlobal {
    kimix::string name;
    Value value;
};

/* The complete mutable state of one Sandbox.  POD-ish by design: the only
 * non-trivial members are kimix::strings/vectors, which are only ever touched
 * from native code OUTSIDE a raising window (README §"Native-side rules"). */
struct SandboxState {
    /* ---- per-run outcome (reset at the start of every run) ---- */
    Status sticky_kind = Status::ok;
    kimix::string sticky_msg; /* the final, already-located message */
    char body[k_body_capacity] = {0};
    Violation violation;
    kimix::string chunk_name;
    kimix::string traceback;
    kimix::string output;
    uint64_t instructions = 0;
    uint64_t max_instructions = 0;
    kimix::Clock clock;
    double deadline_s = 0.0;

    /* ---- allocation budget (script_alloc.cpp) ---- */
    size_t alloc_used = 0;
    size_t alloc_peak = 0;
    size_t max_memory = 0;
    size_t max_string = 0;
    size_t pending_bytes = 0; /* the denied request, for the MEMORY message */
    size_t pending_used = 0;
    size_t pending_max = 0;
    bool reporting = false; /* composing a message: grant k_report_slack_bytes */

    /* ---- caps (copied from Limits at run start) ---- */
    size_t max_output = 0;
    size_t max_source = 0;
    size_t max_result_bytes = 0;
    uint32_t max_result_values = 0;
    uint32_t max_result_depth = 0;

    /* ---- host-function surface (script_host.cpp) -------------------------
     * `host_calls` is both the running count and the 1-based ordinal of the
     * call in flight (HostCall::index), which is what the amplification cap
     * counts against. */
    uint64_t host_calls = 0;
    uint32_t max_host_calls = 0;
    uint32_t max_host_args = 0;
    uint32_t max_host_ret_values = 0;
    /* Per-CALL scratch: STATE-owned, never stack locals.  All three can be live
     * when the trampoline raises (a bad argument type, a rejected result), and a
     * raise is a longjmp that would skip a kimix::string destructor. */
    kimix::vector<Value> host_args;
    kimix::vector<Value> host_rets;
    kimix::string host_error;
    /* The POD twin of host_error, handed to the host as HostCall::err / err_cap
     * (and from there to the C ABI as kimix_lua_host_ctx::err).  Reset per CALL
     * (sb_host_trampoline) and once per RUN (reset_run) so a stale message can
     * never be read back by the next host function. */
    char host_err[k_host_err_bytes] = {0};
    /* Per-RUN inputs, filled by Sandbox::run() before the boundary: the Lua
     * global to call after the setup chunk, its copied arguments, and the two
     * lists the trampoline / the env builder read (they point into the Impl, so
     * a rebind during a run is visible to a stale closure -- which is exactly
     * what the trampoline's stale-binding check catches). */
    kimix::string call_name;
    kimix::vector<Value> call_args;
    const kimix::vector<SandboxNative> *host_bindings = nullptr;
    const kimix::vector<SandboxGlobal> *globals = nullptr;

    /* ---- run plumbing ---- */
    const char *source = nullptr; /* points into the caller's/Impl's copy */
    size_t source_len = 0;
    kimix::string load_buffer; /* `load` reader-function accumulation */
    kimix::vector<Value> results;
    int env_ref = -2;          /* LUA_NOREF while no env is installed */
    uint32_t id_counter = 0;   /* redaction ids: "table: #3" */
    uint32_t random_seed = 0;  /* math.randomseed, pinned per run */
    uint32_t hook_granularity = 1;
    const kimix::vector<SandboxNative> *natives = nullptr; /* add_native list */
    std::atomic<bool> cancel{false};
    bool coroutines = false;
    bool gc_count = false;

    /* ---- marshalling scratch (script_value.cpp) ---------------------------
     * Kept in the state, not on the stack: the canonical-JSON walk can raise a
     * LIMIT, and a raise must never skip a kimix::string destructor. */
    kimix::vector<const void *> json_path; /* cycle detection on the visited path */
    /* One key list PER NESTING LEVEL: an object's keys must survive while its
     * children are walked, and a child object collects its own keys. */
    kimix::vector<kimix::vector<kimix::string>> json_key_stack;
    uint32_t json_depth_cap = 0;
    kimix::string json_buffer;
    size_t json_nodes = 0;
    bool json_failed = false;
    char json_detail[192] = {0};

    void reset_run(const Limits &limits) noexcept;
};

/* The SandboxState of a lua_State (see the header comment: a coroutine resolves
 * its parent's header through the copied extraspace). */
SandboxState *sb_from(lua_State *L) noexcept;

/* Create a lua_State wired to `s` (budgeted allocator, fixed seed, panic, warnf,
 * selected-libs mask).  Returns nullptr when even the bootstrap state does not
 * fit the budget -- `s` then carries the MEMORY detail. */
lua_State *sb_state_open(SandboxState *s, const Limits &limits) noexcept;

/* Protected final GC + lua_close, then the leak check. */
void sb_state_close(lua_State *L, SandboxState *s) noexcept;

/* The count hook (script_hook.cpp): deadline, instruction budget, cancel. */
void sb_hook(lua_State *L, lua_Debug *ar) noexcept;

/* The private implementation of one Sandbox.  Derives kimix::IOperatorNewBase
 * (core/memory.h) so the object -- and the state header it carries -- is
 * mimalloc memory, the same heap as every kimix:: container. */
struct Sandbox::Impl : kimix::IOperatorNewBase {
    Limits limits;
    SandboxState state;
    lua_State *L = nullptr; /* non-null only when !fresh_state_per_run */
    std::atomic<bool> busy{false};
    bool coroutines = false;
    bool gc_count = false;
    kimix::string source_copy;
    kimix::vector<SandboxNative> natives;       /* add_native (raw lua_CFunction) */
    kimix::vector<SandboxNative> host_bindings; /* bind_host_function (trampolined) */
    kimix::vector<SandboxGlobal> globals;       /* set_global */
};

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_STATE_H */
