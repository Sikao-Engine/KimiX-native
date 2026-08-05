#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <core/logging.h>
#include <core/stl/string.h>
#include <mutex>

namespace kimix {

namespace detail {

static std::mutex LOGGER_MUTEX;

spdlog::logger &default_logger() noexcept {
    static auto logger = [] {
        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::logger l{"kimix-console", sink};
        l.flush_on(spdlog::level::err);
#ifndef NDEBUG
        spdlog::level::level_enum log_level = spdlog::level::debug;
#else
        spdlog::level::level_enum log_level = spdlog::level::info;
#endif
        if (auto env_level_c_str = getenv("KIMIX_LOG_LEVEL")) {
            kimix::string env_level{env_level_c_str};
            for (auto &c : env_level) { c = static_cast<char>(tolower(c)); }
            if (env_level == "verbose") {
                log_level = spdlog::level::debug;
            } else if (env_level == "info") {
                log_level = spdlog::level::info;
            } else if (env_level == "warning") {
                log_level = spdlog::level::warn;
            } else if (env_level == "error") {
                log_level = spdlog::level::err;
            }
        }
        l.set_level(log_level);
        return l;
    }();
    return logger;
}

} // namespace detail

void log_level_verbose() noexcept { detail::default_logger().set_level(spdlog::level::debug); }
void log_level_info() noexcept { detail::default_logger().set_level(spdlog::level::info); }
void log_level_warning() noexcept { detail::default_logger().set_level(spdlog::level::warn); }
void log_level_error() noexcept { detail::default_logger().set_level(spdlog::level::err); }

void log_flush() noexcept { detail::default_logger().flush(); }

namespace detail {

void log_message(spdlog::level::level_enum level, const char *file, int line,
                 const char *function, kimix::string_view message) noexcept {
    std::lock_guard lock{LOGGER_MUTEX};
    auto &logger = default_logger();
    spdlog::source_loc loc{file, line, function};
    logger.log(loc, level, message);
}

} // namespace detail

} // namespace kimix
