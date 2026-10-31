/*
 * script_alloc.h -- INTERNAL: the budgeted lua_Alloc of the sandbox (L4).
 *
 * One allocator, one budget.  Every byte the interpreter asks for -- strings,
 * tables, closures, the VM stack, the upvalue arrays, the parser buffers --
 * passes through `sb_alloc`, which
 *
 *   - keeps the live-byte count in state->alloc_used (and its peak in
 *     alloc_peak, reported as Result::memory_peak),
 *   - refuses a request that would take the state over Limits::max_memory_bytes,
 *   - refuses a single allocation over Limits::max_string_bytes (an earlier,
 *     clearer error than a slowly exhausted budget),
 *   - marks the run with a sticky Status::memory before returning NULL, so the
 *     "not enough memory" error Lua raises can never be swallowed (a script that
 *     retries in a loop aborts on the first retry),
 * - and backs the actual bytes with the same mimalloc heap that this module's
 * kimix containers use (per-image standalone, process-wide external).
 *
 * The budget is HIGH-WATER based, not per-run: with fresh_state_per_run=false the
 * live state is already charged when the next run starts, so `used` includes the
 * bootstrap state (~14 KB) throughout.
 */
#pragma once
#ifndef KIMIX_SCRIPT_ALLOC_H
#define KIMIX_SCRIPT_ALLOC_H

#include <cstddef>

struct lua_State;

namespace kimix::script {

struct SandboxState;

/* The lua_Alloc of every sandbox state.  `ud` is the SandboxState. */
void *sb_alloc(void *ud, void *ptr, size_t osize, size_t nsize) noexcept;

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_ALLOC_H */
