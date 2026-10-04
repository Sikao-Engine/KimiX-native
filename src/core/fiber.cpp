/*
 * fiber.cpp — the process-wide shared fiber pool, defined out of line.
 *
 * fiber.h is header-only except for this one object: detail::shared_scheduler_pool()
 * owns the lazily created, deliberately never-destroyed marl::Scheduler that
 * shared_scheduler() / schedule_background() submit to. It lives here — not as
 * an inline function in the header — so the pool has exactly one home in the
 * kimix-core archive and, when kimix-core is built as a shared library, one
 * exported symbol (KIMIX_CORE_API). An inline function's function-local static
 * is only unique within a single binary: with kimix-core archived into several
 * modules (the CLI, runtime_py, kimix_api), each module would silently grow
 * its own pool, and fiber state bound in one pool could be resumed in another.
 *
 *   #include <core/fiber.h>   // NOT part of kimix_core.h
 */
#include <core/kimix_core.h>
#include <core/fiber.h>

namespace kimix::fiber::detail {

marl::Scheduler *shared_scheduler_pool() noexcept {
    static marl::Scheduler *pool = [] {
        auto config = marl::Scheduler::Config::allCores();
        if (auto const *raw = std::getenv("KIMIX_FIBER_WORKER_THREADS"); raw != nullptr) {
            long const n = std::strtol(raw, nullptr, 10);
            if (n > 0) { config.setWorkerThreadCount(static_cast<int>(n)); }
        } else if (auto const cores = std::thread::hardware_concurrency(); cores > 0 && cores < 4u) {
            config.setWorkerThreadCount(4);
        }
        return kimix::new_with_allocator<marl::Scheduler>(config);
    }();
    return pool;
}

} // namespace kimix::fiber::detail
