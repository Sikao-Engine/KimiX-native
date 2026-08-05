// Test for core/logging.h (spdlog-backed logging).
// This test covers:
// - basic logging calls (functions and KIMIX_* macros) without crash
// - *_WITH_LOCATION macros append " [file:line]" to the message
// - custom spdlog sink captures messages and levels
// - level filtering (verbose/info dropped at warning level)
// - formatted arguments in logged messages
// - level transitions and log_flush
// - empty, special-char and long messages

#include "ut/ut.hpp"
#include <core/logging.h>
#include <core/stl/string.h>

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

struct CapturedMessage {
    spdlog::level::level_enum level;
    std::string message;
};

// A test-local spdlog sink that records every message it receives.
class capture_sink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<CapturedMessage> messages;

    [[nodiscard]] bool contains(std::string_view needle) const {
        return std::any_of(messages.begin(), messages.end(), [&](const CapturedMessage &m) {
            return m.message.find(needle) != std::string::npos;
        });
    }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        messages.push_back({msg.level, std::string{msg.payload.data(), msg.payload.size()}});
    }
    void flush_() override {}
};

// RAII helper: temporarily replace the default logger's sinks with ours.
struct sink_scope {
    std::shared_ptr<capture_sink> sink{std::make_shared<capture_sink>()};
    std::vector<spdlog::sink_ptr> original_sinks;

    sink_scope() {
        auto &logger = kimix::detail::default_logger();
        original_sinks = logger.sinks();
        logger.sinks().clear();
        logger.sinks().push_back(sink);
    }
    ~sink_scope() {
        kimix::detail::default_logger().sinks() = original_sinks;
        kimix::log_level_verbose();
    }
};

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "logging_basic_no_crash"_test = [] {
        kimix::log_level_verbose();
        kimix::log_verbose("Verbose message from test");
        kimix::log_info("Info message from test");
        kimix::log_warning("Warning message from test");
        kimix::log_verbose("Verbose with args: {}, {}", 42, 3.14);
        kimix::log_info("Info with args: {}, {}", "test_string", true);
        kimix::log_warning("Warning with args: {}, {}", 'a', 123u);
        kimix::log_flush();
        expect(true) << "basic logging calls completed without crash";
    };

    "logging_macros_no_crash"_test = [] {
        kimix::log_level_verbose();
        KIMIX_VERBOSE("Macro verbose message");
        KIMIX_INFO("Macro info message");
        KIMIX_WARNING("Macro warning message");
        KIMIX_VERBOSE("Verbose macro: {}, {}", 1, 2);
        KIMIX_INFO("Info macro: {}, {}, {}", "a", "b", "c");
        KIMIX_WARNING("Warning macro: value = {}", 3.14159);
        kimix::log_flush();
        expect(true) << "logging macros completed without crash";
    };

    "logging_location_macros_no_crash"_test = [] {
        kimix::log_level_verbose();
        KIMIX_VERBOSE_WITH_LOCATION("Verbose with location: {}", 100);
        KIMIX_INFO_WITH_LOCATION("Info with location: {}", 200);
        KIMIX_WARNING_WITH_LOCATION("Warning with location: {}", 300);
        KIMIX_WARNING_WITH_LOCATION("Location without args");
        kimix::log_flush();
        expect(true) << "location macros completed without crash";
    };

    "logging_level_transitions_no_crash"_test = [] {
        kimix::log_level_verbose();
        kimix::log_level_info();
        kimix::log_level_warning();
        kimix::log_level_error();
        kimix::log_level_verbose();
        kimix::log_level_warning();
        kimix::log_level_info();
        kimix::log_level_verbose();
        kimix::log_flush();
        expect(true) << "rapid level transitions completed without crash";
    };

    "logging_complex_format_strings"_test = [] {
        kimix::log_level_verbose();
        KIMIX_INFO("Integer: {}", -123456);
        KIMIX_INFO("Unsigned: {}", 123456u);
        KIMIX_INFO("Float: {}", 3.14159265f);
        KIMIX_INFO("Double: {}", 2.718281828459045);
        KIMIX_INFO("String: {}", "test_string");
        KIMIX_INFO("Boolean: {}", true);
        KIMIX_INFO("Pointer: {}", static_cast<void *>(nullptr));
        KIMIX_INFO("Hex: {:x}", 255);
        KIMIX_INFO("Binary: {:b}", 170);
        KIMIX_INFO("Fixed: {:.2f}", 3.14159);
        kimix::log_flush();
        expect(true) << "complex format strings completed without crash";
    };

    "logging_empty_and_special_messages"_test = [] {
        kimix::log_level_verbose();
        KIMIX_INFO("");
        KIMIX_INFO("Special chars: \t!@#$%^&*()_+-=[]|;':\",./<>?");
        KIMIX_INFO("Braces: {{escaped}}, {{{{nested}}}}");
        kimix::string long_message;
        for (int i = 0; i < 100; ++i) {
            long_message += "This is a long message repeated multiple times. ";
        }
        KIMIX_INFO("Long message: {}", long_message);
        kimix::log_flush();
        expect(long_message.size() > 4000u) << "long message should be substantial";
    };

    "logging_custom_sink_captures_messages"_test = [] {
        sink_scope scope;
        kimix::log_level_verbose();

        kimix::log_verbose("verbose_msg_42");
        kimix::log_info("info_msg_100");
        kimix::log_warning("warning_msg_xyz");
        kimix::log_flush();

        expect(scope.sink->messages.size() >= 3u) << "expected at least 3 captured messages, got "
                                                  << scope.sink->messages.size();
        expect(scope.sink->contains("verbose_msg_42")) << "custom sink should capture verbose message";
        expect(scope.sink->contains("info_msg_100")) << "custom sink should capture info message";
        expect(scope.sink->contains("warning_msg_xyz")) << "custom sink should capture warning message";
    };

    "logging_sink_captures_levels"_test = [] {
        sink_scope scope;
        kimix::log_level_verbose();

        kimix::log_verbose("level_probe_verbose");
        kimix::log_info("level_probe_info");
        kimix::log_warning("level_probe_warning");
        kimix::log_error("level_probe_error");
        kimix::log_flush();

        bool verbose_ok = false, info_ok = false, warning_ok = false, error_ok = false;
        for (const auto &m : scope.sink->messages) {
            if (m.message.find("level_probe_verbose") != std::string::npos &&
                m.level == spdlog::level::debug) { verbose_ok = true; }
            if (m.message.find("level_probe_info") != std::string::npos &&
                m.level == spdlog::level::info) { info_ok = true; }
            if (m.message.find("level_probe_warning") != std::string::npos &&
                m.level == spdlog::level::warn) { warning_ok = true; }
            if (m.message.find("level_probe_error") != std::string::npos &&
                m.level == spdlog::level::err) { error_ok = true; }
        }
        expect(verbose_ok) << "verbose should map to spdlog::level::debug";
        expect(info_ok) << "info should map to spdlog::level::info";
        expect(warning_ok) << "warning should map to spdlog::level::warn";
        expect(error_ok) << "error should map to spdlog::level::err";
    };

    "logging_level_filtering"_test = [] {
        sink_scope scope;

        kimix::log_level_warning();
        scope.sink->messages.clear();

        kimix::log_verbose("should_be_filtered_verbose");
        kimix::log_info("should_be_filtered_info");
        kimix::log_warning("should_appear_warning");
        kimix::log_flush();

        expect(!scope.sink->contains("should_be_filtered_verbose"))
            << "verbose should be filtered at warning level";
        expect(!scope.sink->contains("should_be_filtered_info"))
            << "info should be filtered at warning level";
        expect(scope.sink->contains("should_appear_warning"))
            << "warning should pass at warning level";
    };

    "logging_formatted_args_in_sink"_test = [] {
        sink_scope scope;
        kimix::log_level_verbose();

        kimix::log_info("answer is {}", 42);
        kimix::log_info("pi is {:.2f}", 3.14159);
        kimix::log_info("bool={}, str={}", true, "hello");
        kimix::log_flush();

        expect(scope.sink->contains("answer is 42")) << "formatted int arg should appear in sink";
        expect(scope.sink->contains("pi is 3.14")) << "formatted float arg should appear in sink";
        expect(scope.sink->contains("bool=true, str=hello")) << "formatted bool/string args should appear in sink";
    };

    "logging_location_appended_to_message"_test = [] {
        sink_scope scope;
        kimix::log_level_verbose();

        KIMIX_INFO_WITH_LOCATION("location_probe {}", 7);
        kimix::log_flush();

        expect(scope.sink->contains("location_probe 7")) << "formatted message should appear";
        expect(scope.sink->contains("test_logging.cpp")) << "file name should be appended";
        expect(scope.sink->contains("[") && scope.sink->contains("]")) << "location should be bracketed";
    };

    "logging_add_sink_alongside_default"_test = [] {
        // An extra sink added next to the default console sink still receives messages.
        auto extra = std::make_shared<capture_sink>();
        auto &logger = kimix::detail::default_logger();
        logger.sinks().push_back(extra);
        kimix::log_level_verbose();

        kimix::log_info("add_sink_test_msg");
        kimix::log_flush();

        auto &sinks = logger.sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), extra), sinks.end());

        expect(extra->contains("add_sink_test_msg")) << "added sink should capture messages";
    };

    "kimix_assert_passes_on_true_condition"_test = [] {
        KIMIX_ASSERT(1 + 1 == 2);
        KIMIX_ASSERT(true, "must not fire");
        KIMIX_ASSERT(42 > 0, "value = {}", 42);
        expect(true) << "KIMIX_ASSERT with true condition must not fire";
    };
}
