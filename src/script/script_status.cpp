/*
 * script_status.cpp -- the small vocabulary helpers (Status names/prefixes) and
 * the reset methods of Violation / Result.
 */
#include <script/script_status.h>

namespace kimix::script {

kimix::string_view prefix_for(Status status) noexcept {
    switch (status) {
    case Status::ok: return {};
    case Status::syntax: return kSyntaxPrefix;
    case Status::runtime: return kRuntimePrefix;
    case Status::denied: return kDeniedPrefix;
    case Status::memory: return kMemoryPrefix;
    case Status::timeout: return kTimeoutPrefix;
    case Status::behaviour: return kBehaviourPrefix;
    case Status::cancelled: return kCancelPrefix;
    case Status::limit: return kLimitPrefix;
    case Status::internal: return kInternalPrefix;
    }
    return {};
}

const char *status_name(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::syntax: return "syntax";
    case Status::runtime: return "runtime";
    case Status::denied: return "denied";
    case Status::memory: return "memory";
    case Status::timeout: return "timeout";
    case Status::behaviour: return "behaviour";
    case Status::cancelled: return "cancelled";
    case Status::limit: return "limit";
    case Status::internal: return "internal";
    }
    return "unknown";
}

void Violation::clear() noexcept {
    kind = Status::ok;
    api.clear();
    category.clear();
    detail.clear();
    chunk.clear();
    line = 0;
}

void Result::clear() noexcept {
    status = Status::ok;
    message.clear();
    traceback.clear();
    violation.clear();
    output.clear();
    results.clear();
    memory_peak = 0;
    instructions = 0;
    host_calls = 0;
    seconds = 0.0;
}

} // namespace kimix::script
