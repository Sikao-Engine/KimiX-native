/*
 * ffi_parallel.cpp -- implementation of the kimix::fiber job fan-out C FFI
 * (api/ffi_parallel.h).
 *
 * The whole area is ONE call shape: wrap {fn, context, cancel, ran} in a
 * stack object of the calling frame, hand the fiber split a body that claims one
 * job id at a time, and block in kimix::fiber::parallel() until the split has
 * drained.  Nothing is allocated (the state lives on the caller's stack, the
 * tasks live in marl), nothing is copied across the boundary, and the only
 * library call is the templated parallel() -- so there is no error path other
 * than the argument check, and nothing that can throw (this build has neither
 * exceptions nor RTTI, and a C callback cannot throw either).
 *
 * SCHEDULER BINDING (see the header's WHO OWNS THE POOL block)
 * ------------------------------------------------------------
 * par_fiber_bind_guard is the transient binding: it binds
 * kimix::fiber::shared_scheduler() when and only when the caller asked for a run
 * that can fan out AND the calling thread has no scheduler of its own, and it
 * unbinds exactly what it bound when the scope closes.  That is the object marl's
 * own schedule_background() performs by hand and the contract
 * src/builtin_tools/grep_engine.cpp implements: bind, never create, always
 * unbind on the way out.  The guard is declared BEFORE the split and outlives
 * it, so the pool is unbound only after every job has returned (parallel()
 * blocks until then).  The shared pool is intentionally never destroyed, so an
 * unbind here can never hang; a thread bound to its OWN scheduler is left
 * exactly as it was (marl allows exactly one scheduler per thread).
 *
 * THE uint32 WINDOWS
 * ------------------
 * marl's job-id split is uint32_t wide, the C surface takes a uint64_t job
 * count, so ids are dispatched in consecutive windows of at most UINT32_MAX ids
 * each, one blocking parallel() per window, with `base + i` handed to the
 * callback.  The loop also re-tests `cancel` between windows, which is what
 * makes "the remaining jobs are skipped" true for a run far larger than 2^32
 * without churning the claim cursor through all of it.  A window whose split
 * collapses to one task -- and every call on a thread whose pool has a single
 * worker -- runs inline on this thread instead: correct, just serial, which is
 * the documented degradation of the blocking parallel() forms.
 *
 * Unity-build safety (same rule as ffi_vec.cpp / ffi_mem.cpp / ffi_repair.cpp):
 * every file-local name is `par_`-prefixed inside an anonymous namespace, and the
 * exported definitions sit between KIMIX_FFI_BEGIN / KIMIX_FFI_END so they get
 * the C language linkage the header declared.
 */
#include <api/ffi_parallel.h>

#include <core/kimix_core.h> /* umbrella first: it fixes the winsock2.h order */
#include <core/fiber.h>

#include <atomic>
#include <cstdint>

namespace {

/* The highest job id one kimix::fiber::parallel() call can carry, and the same
 * ceiling for the caller's task_limit (a cap above the pool width is clamped to
 * the pool by the split anyway, so clamping here is unobservable). */
constexpr std::uint64_t k_par_max_u32 = 0xFFFFFFFFull;

/* Everything the job body needs.  One instance per kimix_fiber_run() call, in
 * the calling frame: the split blocks before that frame goes away, so the tasks
 * may point at it.  `base` is written between windows, never during one (the
 * previous parallel() has drained); `ran` is the only field several workers
 * touch -- a relaxed counter is exactly what the join at the end of each window
 * publishes. */
struct par_job_state {
    kimix_fiber_job_fn fn;
    void *context;
    const volatile bool *cancel;
    std::atomic<std::uint64_t> ran{0u};
    std::uint64_t base = 0u;
};

/* Transient binding of the process-wide shared pool for an unbound caller; a
 * no-op (and a no-cost one) when the calling thread already has a scheduler or
 * when `enable` says the run cannot fan out.  Unbinds only what it bound. */
class par_fiber_bind_guard {
public:
    explicit par_fiber_bind_guard(bool enable) noexcept {
        if (enable && !kimix::fiber::is_bound()) {
            kimix::fiber::shared_scheduler().bind();
            _bound = kimix::fiber::is_bound();
        }
    }
    par_fiber_bind_guard(const par_fiber_bind_guard &) = delete;
    par_fiber_bind_guard &operator=(const par_fiber_bind_guard &) = delete;
    ~par_fiber_bind_guard() noexcept {
        if (_bound) {
            kimix::fiber::shared_scheduler().unbind();
        }
    }

private:
    bool _bound = false;
};

/* One job: skip when the caller's flag is set, otherwise run and count.  The
 * check is per job because the split is driven with internal_jobs == 1, i.e. one
 * id per claim of the shared cursor. */
inline void par_run_one_job(par_job_state &state, std::uint32_t index) noexcept {
    if (state.cancel && *state.cancel) {
        return; // cancelled: this job and all later ones are skipped
    }
    state.fn(state.context, state.base + static_cast<std::uint64_t>(index));
    state.ran.fetch_add(1u, std::memory_order_relaxed);
}

} // namespace

KIMIX_FFI_BEGIN

uint64_t kimix_fiber_worker_count(void) {
    /* The pool the CALLING thread can spread work over: its bound scheduler's
     * worker count, or 1 when nothing is bound (never hardware_concurrency(),
     * which over-promises to a thread without workers). */
    return static_cast<uint64_t>(kimix::fiber::worker_thread_count());
}

kimix_status kimix_fiber_run(uint64_t job_count,
                             kimix_fiber_job_fn fn,
                             void *context,
                             uint64_t task_limit,
                             const volatile bool *cancel,
                             bool *out_completed) {
    if (!fn) {
        return KIMIX_ERR_INVALID_ARG; // nothing to run; nothing is scheduled
    }

    par_job_state state;
    state.fn = fn;
    state.context = context;
    state.cancel = cancel;

    /* Only a run that CAN fan out binds a pool: an empty range, a single job, or
     * an explicit one-task cap all execute inline on the calling thread, and
     * binding would spawn marl workers for nothing (the small-scan rule of
     * grep_engine.cpp). */
    const bool may_fanout = job_count > 1u && task_limit != 1u;
    const par_fiber_bind_guard fiber_binding(may_fanout);
    const auto clamped_limit = static_cast<std::uint32_t>(
        task_limit > k_par_max_u32 ? k_par_max_u32 : task_limit);

    std::uint64_t done = 0u;
    while (done < job_count) {
        if (cancel && *cancel) {
            break; // the rest of the range is skipped, later windows included
        }
        const std::uint64_t remaining = job_count - done;
        const auto window = static_cast<std::uint32_t>(
            remaining < k_par_max_u32 ? remaining : k_par_max_u32);
        state.base = done;
        /* The job-id split: one id per claim, the caller's cap as task_limit
         * (0 = no cap, the pool decides).  Blocks until every claim drained; a
         * one-task split runs inline instead. */
        kimix::fiber::parallel(window,
                               [&state](std::uint32_t i) noexcept { par_run_one_job(state, i); },
                               /*internal_jobs=*/1u,
                               /*task_limit=*/clamped_limit);
        done += window;
    }

    if (out_completed) {
        /* Every job that ran was counted exactly once, so "all of them ran" and
         * "the run stopped early on *cancel" are the same test. */
        *out_completed = (state.ran.load(std::memory_order_relaxed) == job_count);
    }
    return KIMIX_OK;
}

KIMIX_FFI_END
