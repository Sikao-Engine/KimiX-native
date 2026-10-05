/*
 * ffi_parallel.h -- kimix::fiber (src/core/fiber.h, the vendored marl scheduler)
 * exported to C: run N independent C-callable jobs over a worker pool.
 *
 * SCOPE
 * -----
 * One job shape and two entry points.  A caller hands over a plain C function
 * pointer plus a `void *context` and a job count, and this area calls
 * fn(context, i) for every i in [0, job_count) -- possibly on several OS threads
 * at once.  Nothing else is exported from the fiber layer: no scheduler handle,
 * no event/counter/future object, no fiber-local storage.  Those are C++ types
 * with ref-counted handles and marl-owned lifetimes; none of them is
 * representable in C (see api/ffi_common.h contract rule 3), and a foreign host
 * that only wants "map this loop over the pool" does not need them.
 *
 * WHO OWNS THE POOL (the rule this area follows from src/core/fiber.h)
 * --------------------------------------------------------------------
 * A marl pool is bound per THREAD, and libraries never create or own one: a root
 * main() binds the process-wide shared pool, and a foreign thread that has none
 * transiently binds `kimix::fiber::shared_scheduler()` for the duration of the
 * run and unbinds it again on the way out -- exactly the pattern of
 * src/builtin_tools/grep_engine.cpp (bind, never create; the shared pool is
 * intentionally never destroyed, so unbinding can never hang).  kimix_fiber_run()
 * therefore:
 *   - keeps the caller's AMBIENT pool when the calling thread already has one
 *     (a host that bound the shared pool, or a C++ test with its own scheduler),
 *   - transiently binds the shared pool when the calling thread has none (the
 *     normal case for a C#, Python or Rust host thread), and unbinds it before
 *     the call returns, on every exit path,
 *   - never binds at all for a run that cannot fan out (job_count < 2, or
 *     task_limit == 1): those jobs run inline on the calling thread, so a
 *     degenerate call does not pay for a pool.
 * After the call returns nothing is left bound that was not bound before, and
 * the function has blocked until every job it started has finished.
 *
 * THREADING CONTRACT FOR THE CALLBACK
 * -----------------------------------
 * The callback is invoked from several workers CONCURRENTLY, with a different
 * job_index on each call, exactly like the body of kimix::fiber::parallel():
 * make `context` thread-safe -- index disjoint elements of a shared array, or
 * lock what you share (a plain std atomic counter is fine).  The library holds
 * no lock around the call and never calls the two callbacks "in order".  The
 * callback must not throw (this build has no exceptions) and must not block a
 * worker on a std wait for a long time -- a fiber that parks its OS thread
 * starves the fibers queued behind it on that thread (see .agents/skills/fiber).
 * The callback is never called from more than one thread at the SAME job index,
 * and every callback returns before kimix_fiber_run() returns, so `context` may
 * point at caller stack memory of the calling frame.
 *
 * CANCELLATION
 * ------------
 * `cancel` is a KIMIX_NULLABLE pointer to a caller-owned ONE-byte flag
 * (`bool`/`_Atomic bool` on the caller side, read here as `const volatile bool *`).
 * Before each job the flag is tested; when it is true, that job and every job
 * that has not started yet are skipped.  The check is per job and happens under
 * the split's atomic claim cursor, so it is cheap and exact-enough, but it is
 * deliberately NOT a barrier: a job that has already started always runs to
 * completion, and jobs claimed by other workers at the moment the flag goes up
 * may still run.  Cancellation is a "stop starting new work" request, never a
 * kill.  The caller must keep the flag alive for the whole call and must not
 * free it while any job may still read it.
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_PARALLEL_H
#define KIMIX_API_FFI_PARALLEL_H

#include <api/ffi_common.h>

/* One job.  `context` is the pointer handed to kimix_fiber_run() unchanged;
 * `job_index` is the id this call is responsible for, always < job_count and
 * never delivered twice by the same run.  The function returns no status: a job
 * that can fail reports it through its own context (an error slot, a status
 * array), because a failing job does not stop the other jobs. */
typedef void (*kimix_fiber_job_fn)(void *context, uint64_t job_index);

KIMIX_FFI_BEGIN

/* ===========================================================================
 * Observation
 * ======================================================================== */

/* How many worker threads the CALLING thread can fan out over right now: the
 * worker count of the scheduler bound to it, else 1 (an unbound thread only has
 * itself, which is exactly why a run on such a thread without binding would be
 * serial).  Mirrors kimix::fiber::worker_thread_count(); never fails.
 *
 * This is NOT std::thread::hardware_concurrency(): it describes the pool this
 * call can actually use.  A host that never binds a pool sees 1 before its first
 * kimix_fiber_run() and the shared pool's width during one. */
KIMIX_FFI uint64_t kimix_fiber_worker_count(void);

/* ===========================================================================
 * Running jobs
 * ======================================================================== */

/* Run the jobs [0, job_count) concurrently: `fn(context, i)` for each i, spread
 * over the pool described above.  Blocks until every job that was started has
 * returned (kimix::fiber::parallel() only ever waits on a fiber counter, so
 * waiting costs no OS thread; nothing here parks a worker with a std wait).
 *
 *   job_count    Number of jobs.  0 is a no-op success: nothing is scheduled, no
 *                pool is bound, and *out_completed is written true.  Values
 *                above 2^32 are handled by running the ids in consecutive
 *                32-bit windows (the marl job-id split is uint32_t wide), which
 *                is invisible to the caller except that the cancel check also
 *                happens between windows.
 *   fn           The job callback.  KIMIX_ERR_INVALID_ARG when NULL (nothing is
 *                scheduled in that case).
 *   context      Opaque caller value passed through to fn VERBATIM on every
 *                call.  May be NULL.
 *   task_limit   0 = no explicit cap: the split uses the whole pool the calling
 *                thread can reach.  > 0 caps the number of tasks in flight, so
 *                at most task_limit jobs run at the same time -- that is the
 *                caller's concurrency budget knob (see the fan-out recipe in
 *                .agents/skills/fiber/SKILL.md).  A cap larger than the pool is
 *                clamped to the pool; 1 (like job_count < 2) runs everything
 *                inline without ever binding a pool.
 *   cancel       KIMIX_NULLABLE caller-owned 1-byte flag.  See the
 *                CANCELLATION block of this file's header comment: true before a
 *                job starts means that job and all later jobs are skipped; a job
 *                already running runs to completion.  NULL = never cancel.
 *   out_completed KIMIX_NULLABLE out-parameter.  On KIMIX_OK: true when every
 *                job in [0, job_count) actually ran, false when the run stopped
 *                early because *cancel was set.  It is NOT written on an error
 *                return.
 *
 * Errors: KIMIX_ERR_INVALID_ARG when `fn` is NULL.  This is the only failure --
 * the run itself cannot fail, since a job's own result is the caller's business
 * and the fiber split degrades to inline execution instead of aborting.
 *
 * Re-entrancy: kimix_fiber_run() may be called from several threads at once (each
 * caller transiently binds the shared pool, like the grep engine's guard does), and
 * a job body may itself call it, but a nested run consumes pool capacity in
 * addition to its parent's jobs -- with task_limit left at 0 that is safe
 * (workers claim work, they do not wait on a static partition), but a nested
 * pair of tight task_limit caps can queue behind each other.  No job body may
 * assume it owns a worker. */
KIMIX_FFI kimix_status kimix_fiber_run(uint64_t job_count,
                                       KIMIX_NOTNULL kimix_fiber_job_fn fn,
                                       KIMIX_NULLABLE void *context,
                                       uint64_t task_limit,
                                       KIMIX_NULLABLE const volatile bool *cancel,
                                       KIMIX_NULLABLE bool *out_completed);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_PARALLEL_H */
