/*
 * script_status.h -- Status, the message prefixes, Violation and Result.
 *
 * Every failure of the sandbox is one of the Status codes below plus a MESSAGE
 * that always has the same shape (see src/script/README.md §"Message format"):
 *
 *     <chunk>:<line>: <PREFIX>: <api> -- <detail>
 *
 * e.g.
 *     script.lua:7: DENIED: io.open -- file I/O is not available in the sandbox
 *
 * The prefixes are stable, greppable constants so a gateway (or a test) can
 * classify a failure without parsing prose.  Nothing here allocates on the
 * hot path: the formatter that builds the message is POD-only (script_error.h).
 */
#pragma once
#ifndef KIMIX_SCRIPT_STATUS_H
#define KIMIX_SCRIPT_STATUS_H

#include <cstdint>

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <script/script_api.h>
#include <script/script_value.h>

namespace kimix::script {

/* The outcome of one Sandbox::run().  `ok` is the only success value. */
enum class Status : uint8_t {
    ok = 0,
    syntax,    /* the chunk did not compile (Lua's own line-numbered message) */
    runtime,   /* an ordinary Lua error: nil index, bad argument, error() */
    denied,    /* a tripwire: a forbidden capability was called */
    memory,    /* the allocator budget was exhausted */
    timeout,   /* the deadline / instruction budget was exhausted */
    behaviour, /* the env integrity post-check failed (a primitive was replaced) */
    cancelled, /* the host called Sandbox::cancel() */
    limit,     /* result / output / source limit exceeded */
    internal,  /* host misuse or a Lua panic -- never script-reachable */
};

/* Message prefixes.  `kDeniedPrefix` etc. are what a caller greps for; the
 * leading <chunk>:<line>: is always present when the chunk had line info. */
constexpr kimix::string_view kDeniedPrefix = "DENIED: ";
constexpr kimix::string_view kTimeoutPrefix = "TIMEOUT: ";
constexpr kimix::string_view kMemoryPrefix = "MEMORY: ";
constexpr kimix::string_view kLimitPrefix = "LIMIT: ";
constexpr kimix::string_view kLoadErrPrefix = "LOADERR: ";
constexpr kimix::string_view kRuntimePrefix = "RUNTIME: ";
constexpr kimix::string_view kCancelPrefix = "CANCELLED: ";
constexpr kimix::string_view kBehaviourPrefix = "BEHAVIOUR: ";
constexpr kimix::string_view kInternalPrefix = "INTERNAL: ";
constexpr kimix::string_view kSyntaxPrefix = "SYNTAX: ";

/* The prefix that belongs to a Status ("" for ok). */
KIMIX_SCRIPT_API kimix::string_view prefix_for(Status status) noexcept;

/* The short name of a Status ("ok", "denied", ...); never NULL. */
KIMIX_SCRIPT_API const char *status_name(Status status) noexcept;

/* What the script tried to do.  Filled once (first violation wins) -- a later
 * denial cannot overwrite the record of the first one. */
struct Violation {
    Status kind = Status::ok;
    kimix::string api;      /* "io.open", "os.execute", "string.dump", "load" */
    kimix::string category; /* "file-io", "process", "env", "time", "module",
                             * "introspection", "bytecode", "pointer" */
    kimix::string detail;   /* "file I/O is not available in the sandbox" */
    kimix::string chunk;    /* the FULL chunk name (LUA_IDSIZE truncates the
                             * copy embedded in `message`) */
    int32_t line = 0;       /* 0 when the chunk had no line information */

    void clear() noexcept;
};

/* Everything a run produced.  `message` is always non-empty unless
 * status == ok (acceptance criterion 1 of the plan). */
struct Result {
    Status status = Status::ok;
    kimix::string message;
    kimix::string traceback; /* bounded: <= k_last_traceback_bytes of luaL_traceback */
    Violation violation;
    kimix::string output; /* captured print()/warn(), never the process streams */
    kimix::vector<Value> results;
    size_t memory_peak = 0;
    uint64_t instructions = 0;
    /* How many times this run called into host code (a bind_host_function
     * callback).  0 when the run never reached the host surface, including for
     * every failure that happened before the first call.  Carried here -- not in
     * the C ABI's kimix_lua_result -- so that struct stays 64 bytes. */
    uint64_t host_calls = 0;
    double seconds = 0.0;

    bool ok() const noexcept { return status == Status::ok; }
    void clear() noexcept;
};

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_STATUS_H */
