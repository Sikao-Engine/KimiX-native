/*
 * script_alloc.cpp -- the budgeted lua_Alloc (see script_alloc.h for the
 * contract).
 *
 * Two rules make this file's placement of the sticky flag non-negotiable:
 *
 *   - It runs on EVERY allocation, including ones made while an error is being
 *     raised, so it must never allocate kimix memory and never longjmp.  It only
 *     records the denial (numbers + the Status) and returns NULL; Lua turns that
 *     into "not enough memory" and unwinds, and the sticky flag makes the
 *     unwinding final (script_env.cpp's pcall wrapper re-raises it).
 *   - state->reporting widens the budget by k_report_slack_bytes while the
 *     message for a memory denial is being composed, so an exhausted state can
 *     still describe itself.  The slack is bounded and only granted on the error
 *     path, so it can never be used to run script code.
 */
#include <script/script_alloc.h>
#include <core/memory.h>

#include <cstdio>


#include <script/script_error.h>
#include <script/script_state.h>

namespace kimix::script {

namespace src_script_script_alloc {

/* Record the denial once (first violation wins) and answer NULL. */
void *sb_alloc_deny(SandboxState *s, size_t nsize) noexcept {
    if (s->sticky_kind == Status::ok) {
        s->sticky_kind = Status::memory;
        s->violation.kind = Status::memory;
        s->violation.category.assign("memory");
        s->violation.chunk.assign(s->chunk_name);
        s->pending_bytes = nsize;
        s->pending_used = s->alloc_used;
        s->pending_max = s->max_memory;
        /* The body needs the numbers this call just recorded; composing it with
         * snprintf into the state's own buffer keeps this path allocation-free. */
        char text[192];
        std::snprintf(text, sizeof(text),
                      "allocation of %zu bytes exceeds the sandbox budget (used %zu of %zu)",
                      s->pending_bytes, s->pending_used, s->pending_max);
        s->violation.detail.assign(text);
        sb_compose_body(s->body, sizeof(s->body), Status::memory, nullptr, text);
    }
    return nullptr;
}

} // namespace src_script_script_alloc
using namespace src_script_script_alloc;

void *sb_alloc(void *ud, void *ptr, size_t osize, size_t nsize) noexcept {
    SandboxState *s = static_cast<SandboxState *>(ud);
    if (s == nullptr) {
        return nullptr;
    }
    /* osize is a TYPE TAG (not a size) when ptr is NULL: the Lua contract. */
    const size_t old_size = (ptr == nullptr) ? 0 : osize;

    if (nsize == 0) { /* free */
        if (ptr != nullptr) {
            kimix::deallocate(ptr);
            s->alloc_used = (s->alloc_used >= old_size) ? (s->alloc_used - old_size) : 0;
        }
        return nullptr;
    }

    const size_t limit = s->max_memory + (s->reporting ? k_report_slack_bytes : 0);
    if (nsize > s->max_string && !s->reporting) {
        return sb_alloc_deny(s, nsize);
    }
    const size_t after = (s->alloc_used - old_size) + nsize;
    if (after > limit) {
        return sb_alloc_deny(s, nsize);
    }

    void *result = (ptr == nullptr) ? kimix::allocate(nsize) : kimix::reallocate(ptr, nsize);
    if (result == nullptr) {
        /* The host heap itself is out: report it as the same budget condition
         * rather than letting Lua see a bare OOM without a violation record. */
        return sb_alloc_deny(s, nsize);
    }
    s->alloc_used = after;
    if (s->alloc_used > s->alloc_peak) {
        s->alloc_peak = s->alloc_used;
    }
    return result;
}

} // namespace kimix::script
