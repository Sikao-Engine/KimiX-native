/*
 * script_state.cpp -- state lifetime, the panic/warning handlers, teardown, and
 * the Sandbox object itself.
 *
 * STATE CREATION (L0)
 * -------------------
 * luaL_newstate() is never used.  It mixes a stack address and time(NULL) into
 * the seed (lauxlib.c luai_makeseed), installs a panic that prints to stderr and
 * a warn function that writes "Lua warning: ..." to the process streams.  The
 * sandbox builds its state explicitly instead:
 *
 *     lua_newstate(budget_alloc, self, fixed_seed)   -- deterministic, budgeted
 *     extraspace <- self                             -- one header for all threads
 *     lua_atpanic(host-bug panic)                    -- never returns, names itself
 *     lua_setwarnf(capture)                          -- warnings land in output
 *     luaL_openselectedlibs(safe mask, 0)            -- L1: no io/os/pkg/debug
 *
 * The mask is base | string | table | math | utf8 (lualib.h's LUA_*LIBK), and
 * the preload mask is 0, so `require` has nothing to find and loadlib never
 * exists.  Everything else the plan denies is installed as a LOUD tripwire by
 * script_env.cpp, because "absent" fails with a cryptic "attempt to index a nil
 * value" while a tripwire names the API and the category.
 */
#include <script/script_state.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <script/script_alloc.h>
#include <script/script_error.h>
#include <script/script_host.h>
#include <script/script_lua.h>

namespace kimix::script {

namespace src_script_script_state {

/* A panic means the HOST called the Lua API wrong (an unprotected call, or a
 * memory error outside any protected boundary) -- never something a script can
 * provoke.  Name it, flush, and abort: continuing would leave the state in an
 * undefined condition, exactly like kimix::allocation_failure(). */
int sb_panic(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (msg == nullptr) {
        msg = "(no message)";
    }
    std::fprintf(stderr, "kimix-script PANIC: %s (this is a host bug, not a script error)\n", msg);
    std::fflush(stderr);
    std::abort();
    return 0; /* not reached */
}

/* lua_warning / "error in __gc" go into the captured output, never to stderr. */
void sb_warnf(void *ud, const char *msg, int tocont) {
    SandboxState *s = static_cast<SandboxState *>(ud);
    (void)tocont; /* Lua calls this once per fragment; we append them verbatim */
    if (s == nullptr || msg == nullptr) {
        return;
    }
    const size_t len = std::strlen(msg);
    const size_t room = (s->output.size() < s->max_output) ? (s->max_output - s->output.size()) : 0;
    if (room > 0) {
        s->output.append(msg, (len < room) ? len : room);
    }
}

/* The final collect: run every pending __gc INSIDE a protected call, so a
 * raising finalizer is a captured error instead of a panic. */
int sb_final_collect(lua_State *L) {
    lua_gc(L, LUA_GCCOLLECT);
    return 0;
}

} // namespace src_script_script_state
using namespace src_script_script_state;

SandboxState *sb_from(lua_State *L) noexcept {
    return *static_cast<SandboxState **>(lua_getextraspace(L));
}

void SandboxState::reset_run(const Limits &limits) noexcept {
    sticky_kind = Status::ok;
    sticky_msg.clear();
    body[0] = '\0';
    violation.clear();
    traceback.clear();
    output.clear();
    load_buffer.clear();
    results.clear();
    instructions = 0;
    max_instructions = limits.max_instructions;
    deadline_s = limits.max_seconds;
    clock.reset();
    alloc_peak = 0;
    pending_bytes = 0;
    pending_used = 0;
    pending_max = 0;
    reporting = false;
    env_ref = -2; /* LUA_NOREF */
    id_counter = 0;
    random_seed = limits.random_seed;
    hook_granularity = (limits.hook_instruction_granularity > 0) ? limits.hook_instruction_granularity : 1;
    cancel.store(false, std::memory_order_relaxed);
    max_output = limits.max_output_bytes;
    max_source = limits.max_source_bytes;
    max_result_bytes = limits.max_result_bytes;
    max_result_values = limits.max_result_values;
    max_result_depth = limits.max_result_depth;
    max_memory = limits.max_memory_bytes;
    max_string = limits.max_string_bytes;
    /* The host surface: the per-call scratch must start empty (a stale error
     * would leak into the first call of the run) and the three budgets come from
     * Limits exactly like the caps above. */
    host_calls = 0;
    host_args.clear();
    host_rets.clear();
    host_error.clear();
    host_err[0] = '\0'; /* the POD error scratch starts empty too */
    call_name.clear();
    call_args.clear();
    max_host_calls = limits.max_host_calls;
    max_host_args = limits.max_host_args;
    max_host_ret_values = limits.max_host_ret_values;
    coroutines = false; /* re-set by the caller from Impl */
    gc_count = false;
}

lua_State *sb_state_open(SandboxState *s, const Limits &limits) noexcept {
    s->max_memory = limits.max_memory_bytes;
    s->max_string = limits.max_string_bytes;
    s->max_output = limits.max_output_bytes;
    s->max_source = limits.max_source_bytes;
    s->max_result_bytes = limits.max_result_bytes;
    s->max_result_values = limits.max_result_values;
    s->max_result_depth = limits.max_result_depth;
    s->max_instructions = limits.max_instructions;
    s->deadline_s = limits.max_seconds;
    s->max_host_calls = limits.max_host_calls; /* a reused state keeps the run's
                                                * host budget, like every cap */
    s->max_host_args = limits.max_host_args;
    s->max_host_ret_values = limits.max_host_ret_values;

    lua_State *L = lua_newstate(&sb_alloc, s, limits.seed);
    if (L == nullptr) {
        return nullptr; /* the budget is smaller than a bootstrap state */
    }
    *static_cast<SandboxState **>(lua_getextraspace(L)) = s;
    lua_atpanic(L, &sb_panic);
    lua_setwarnf(L, &sb_warnf, s);

    int mask = LUA_GLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_MATHLIBK | LUA_UTF8LIBK;
    if (s->coroutines) {
        mask |= LUA_COLIBK;
    }
    luaL_openselectedlibs(L, mask, 0);
    return L;
}

void sb_state_close(lua_State *L, SandboxState *s) noexcept {
    if (L == nullptr) {
        return;
    }
    /* No deadline may fire while finalizers run: the run's Result is already
     * final, and a __gc that burns a moment must not turn a green run red. */
    lua_sethook(L, nullptr, 0, 0);
    lua_pushcfunction(L, &sb_final_collect);
    (void)lua_pcall(L, 0, 0, 0); /* a raising finalizer is captured, not fatal */
    lua_close(L);
    if (s->alloc_used != 0) {
        std::fprintf(stderr, "[kimix-script] %zu byte(s) still allocated after lua_close\n", s->alloc_used);
        std::fflush(stderr);
        s->alloc_used = 0; /* report once, never abort (release builds keep running) */
    }
}

/* ---- Sandbox ---------------------------------------------------------- */

Sandbox::Sandbox(const Limits &limits) {
    /* Impl derives kimix::IOperatorNewBase, so this `new` is mimalloc memory,
     * and the unique_ptr owns it from here on (no manual delete anywhere). */
    _impl.reset(new Impl());
    _impl->limits = limits;
    _impl->state.reset_run(limits);
}

Sandbox::~Sandbox() {
    if (_impl == nullptr) {
        return;
    }
    if (_impl->L != nullptr) {
        sb_state_close(_impl->L, &_impl->state);
        _impl->L = nullptr;
    }
    /* _impl (a kimix::unique_ptr) releases the Impl at member teardown. */
}

Sandbox::Sandbox(Sandbox &&other) noexcept : _impl(std::move(other._impl)) {
    /* std::move leaves `other` empty, i.e. moved-from Sandboxes report
     * Status::internal for every run() -- the documented behaviour. */
    if (_impl != nullptr && _impl->L != nullptr) {
        *static_cast<SandboxState **>(lua_getextraspace(_impl->L)) = &_impl->state;
    }
}

Sandbox &Sandbox::operator=(Sandbox &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (_impl != nullptr && _impl->L != nullptr) {
        sb_state_close(_impl->L, &_impl->state);
        _impl->L = nullptr;
    }
    _impl = std::move(other._impl); /* deletes this side's Impl, takes the other's */
    if (_impl != nullptr && _impl->L != nullptr) {
        *static_cast<SandboxState **>(lua_getextraspace(_impl->L)) = &_impl->state;
    }
    return *this;
}

bool Sandbox::add_native(kimix::string_view name, NativeFn fn, int upvalues) {
    if (_impl == nullptr || fn == nullptr || upvalues < 0) {
        return false;
    }
    /* The name has to be usable as a global: [A-Za-z_][A-Za-z0-9_]* (the same
     * rule bind_host_function/set_global apply -- script_host.cpp). */
    if (!sb_valid_name(name)) {
        return false;
    }
    SandboxNative entry;
    entry.name.assign(name.data(), name.size());
    entry.fn = fn;
    entry.upvalues = upvalues;
    _impl->natives.push_back(static_cast<SandboxNative &&>(entry));
    return true;
}

bool Sandbox::allow_coroutines(bool enable) noexcept {
    if (_impl == nullptr) {
        return false;
    }
    if (_impl->L != nullptr) {
        return false; /* the mask is fixed at state creation */
    }
    _impl->coroutines = enable;
    return true;
}

bool Sandbox::allow_collectgarbage_count(bool enable) noexcept {
    if (_impl == nullptr) {
        return false;
    }
    _impl->gc_count = enable;
    return true;
}

void Sandbox::cancel() noexcept {
    if (_impl != nullptr) {
        _impl->state.cancel.store(true, std::memory_order_relaxed);
    }
}

bool Sandbox::busy() const noexcept {
    return _impl != nullptr && _impl->busy.load(std::memory_order_acquire);
}

const Limits &Sandbox::limits() const noexcept {
    static const Limits k_default{};
    return (_impl != nullptr) ? _impl->limits : k_default;
}

} // namespace kimix::script
