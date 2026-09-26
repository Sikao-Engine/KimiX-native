// agent/steer.cpp - SteerQueue + Steer implementation (see steer.h).

#include "agent/steer.h"

#include <chrono>
#include <mutex>

#include "agent/soul.h"

namespace kimix::agent {

namespace {

// Soul registry for Steer::from_session (steer.py:39-60). A plain pointer map
// guarded by a mutex; souls register/unregister on construction/destruction.
struct soul_registry {
    std::mutex mutex;
    kimix::unordered_map<AgentSession *, KimiSoul *> souls;
};
soul_registry &registry() {
    static soul_registry r;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// SteerQueue
// ---------------------------------------------------------------------------

bool SteerQueue::push(kimix::string_view content) {
    if (agent_user_input_is_empty(content)) {
        return false; // steer.py/kimisoul.py: never inject an empty message
    }
    {
        std::lock_guard<std::mutex> g(_mutex);
        if (_closed) {
            return false;
        }
        _queue.emplace_back(content);
    }
    return true;
}

bool SteerQueue::request(kimix::string_view content) {
    if (!push(content)) {
        return false;
    }
    // kimisoul.py:1029-1030 - enqueue THEN set the wake event, so a loop that
    // observes the wake always sees the steer.
    _wake.store(true, std::memory_order_release);
    return true;
}

bool SteerQueue::push_sync(kimix::string_view content, double timeout_s) {
    if (!request(content)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(_mutex);
    const auto drained = [this]() noexcept {
        return _queue.empty() || _closed;
    };
    if (timeout_s > 0) {
        _consumed.wait_for(lock, std::chrono::duration<double>(timeout_s),
                           drained);
    } else {
        _consumed.wait(lock, drained);
    }
    // Consumed == the queue drained while open; a closed queue means the turn
    // ended (possibly with the steer still pending -> not delivered).
    return _queue.empty() && !_closed;
}

kimix::vector<kimix::string> SteerQueue::drain() {
    std::lock_guard<std::mutex> g(_mutex);
    kimix::vector<kimix::string> out = std::move(_queue);
    _queue.clear();
    _consumed.notify_all(); // unblock push_sync waiters (consumed or turn end)
    return out;
}

size_t SteerQueue::pending() const {
    std::lock_guard<std::mutex> g(_mutex);
    return _queue.size();
}

void SteerQueue::clear() {
    std::lock_guard<std::mutex> g(_mutex);
    _queue.clear();
    _consumed.notify_all();
}

void SteerQueue::close() {
    std::lock_guard<std::mutex> g(_mutex);
    _closed = true;
    _consumed.notify_all();
}

void SteerQueue::reopen() noexcept {
    std::lock_guard<std::mutex> g(_mutex);
    _closed = false;
}

// ---------------------------------------------------------------------------
// Steer
// ---------------------------------------------------------------------------

kimix::optional<Steer> Steer::from_session(AgentSession &session) noexcept {
    std::lock_guard<std::mutex> g(registry().mutex);
    const auto it = registry().souls.find(&session);
    if (it == registry().souls.end() || it->second == nullptr) {
        return kimix::optional<Steer>();
    }
    return Steer(*it->second);
}

bool Steer::push(kimix::string_view content) {
    KimiSoul *soul = _soul;
    if (soul == nullptr) {
        return false;
    }
    if (!soul->is_running()) {
        // steer.py:95-99 - when the soul is not running the content is still
        // queued (discarded as a stale steer at the next turn init) and false
        // is returned.
        soul->steer(content);
        return false;
    }
    // steer.py:100-106 - a running soul gets the interrupting delivery
    // (request_steer: the wake event cancels the in-flight step).
    soul->request_steer(content);
    return true;
}

bool Steer::push_sync(kimix::string_view content, double timeout_s) {
    KimiSoul *soul = _soul;
    if (soul == nullptr) {
        return false;
    }
    // Delivered only when the turn consumes it before ending.
    const bool consumed = soul->push_steer_sync(content, timeout_s);
    return consumed && soul->is_running();
}

size_t Steer::pending() const {
    const KimiSoul *soul = _soul;
    return soul == nullptr ? 0 : soul->pending_steers();
}

void Steer::clear() {
    KimiSoul *soul = _soul;
    if (soul != nullptr) {
        soul->clear_steers();
    }
}

void steer_register_soul(AgentSession &session, KimiSoul &soul) noexcept {
    std::lock_guard<std::mutex> g(registry().mutex);
    registry().souls[&session] = &soul;
}

void steer_unregister_soul(AgentSession &session, KimiSoul &soul) noexcept {
    std::lock_guard<std::mutex> g(registry().mutex);
    const auto it = registry().souls.find(&session);
    if (it != registry().souls.end() && it->second == &soul) {
        registry().souls.erase(it);
    }
}

} // namespace kimix::agent
