// Test for spdlog (logging library).
// This test covers:
// - Creating a logger
// - Setting log level
// - Logging messages at various levels
// - Compile-time header inclusion

#include "ut/ut.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/common.h>

#include <memory>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "spdlog_basic"_test = [] {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("test_logger", null_sink);
        expect(logger != nullptr);

        logger->set_level(spdlog::level::debug);
        expect(eq(logger->level(), spdlog::level::debug));

        expect(logger->should_log(spdlog::level::debug));
        expect(logger->should_log(spdlog::level::info));
        expect(logger->should_log(spdlog::level::warn));
        expect(logger->should_log(spdlog::level::err));
        expect(eq(static_cast<int>(logger->level()), static_cast<int>(spdlog::level::debug)));

        logger->trace("trace message: {}", 42);
        logger->debug("debug message: value={}", 3.14);
        logger->info("info message: {}", "hello");
        logger->warn("warn message: {}", 123);
        logger->error("error message: {}", true);
        logger->critical("critical message");

        logger->set_level(spdlog::level::warn);
        expect(logger->should_log(spdlog::level::warn));
        expect(logger->should_log(spdlog::level::err));
        expect(!logger->should_log(spdlog::level::info));
        expect(!logger->should_log(spdlog::level::debug));
    };

    "spdlog_level_names"_test = [] {
        expect(eq(std::string{spdlog::level::to_short_c_str(spdlog::level::info)}, std::string{"I"}));
        expect(eq(std::string{spdlog::level::to_short_c_str(spdlog::level::warn)}, std::string{"W"}));
        expect(eq(std::string{spdlog::level::to_short_c_str(spdlog::level::err)}, std::string{"E"}));
        expect(eq(std::string{spdlog::level::to_short_c_str(spdlog::level::debug)}, std::string{"D"}));
        expect(eq(std::string{spdlog::level::to_short_c_str(spdlog::level::critical)}, std::string{"C"}));

        expect(eq(static_cast<int>(spdlog::level::trace), 0));
        expect(eq(static_cast<int>(spdlog::level::debug), 1));
        expect(eq(static_cast<int>(spdlog::level::info), 2));
        expect(eq(static_cast<int>(spdlog::level::warn), 3));
        expect(eq(static_cast<int>(spdlog::level::err), 4));
        expect(eq(static_cast<int>(spdlog::level::critical), 5));
        expect(eq(static_cast<int>(spdlog::level::off), 6));
    };

    "spdlog_version"_test = [] {
        int version = SPDLOG_VERSION;
        expect(gt(version, 0)) << "SPDLOG_VERSION should be positive";
    };
}
