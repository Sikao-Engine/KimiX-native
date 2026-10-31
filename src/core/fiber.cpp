/*
 * fiber.cpp — the shared fiber pool, defined out of line.
 *
 * fiber.h is header-only except for this one object: detail::shared_scheduler_pool()
 * owns the lazily created, deliberately never-destroyed marl::Scheduler that
 * shared_scheduler() / schedule_background() submit to. It lives here — not as
 * an inline function in the header — so there is one home per kimix-core image:
 * process-wide in the external shared-core leg, per final binary in standalone.
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
        marl::Scheduler *const scheduler = kimix::new_with_allocator<marl::Scheduler>(config);
        // Deliberately never destroyed (see the file comment above): declare it
        // process-lifetime so a KIMIX_MEMORY_STRICT leak report does not list it.
        kimix::memory_mark_permanent(scheduler);
        return scheduler;
    }();
    return pool;
}

} // namespace kimix::fiber::detail
