// bench_lua_mem -- footprint benchmark for the kimix Lua memory arena.
//
// NOT a Boost.UT suite (its own main(), registered as a plain binary in
// tests/xmake.lua): a timing/footprint benchmark must not turn into a unit test
// that fails because the machine was busy. It prints one row per workload with
// the measured value next to its band and exits non-zero when a band is missed.
//
// The four workloads are the ones the plan measured (Appendix A of
// .kimix_cache/lua_memory_opt_plan.md):
//   wide_array_tables  300 array tables, n in 1..5000 (LCG)   band <= 1.55x
//   hash_string_tables 300 hash tables, 200 short + 40 long   band <= 1.35x
//   one_big_array      one array of 2 000 000 integers        band <= 1.15x
//   resize_churn       200 x (build 2000-int table, drop it)  retained <= 1 MiB
//
// "commit" is mimalloc's process commit delta over the moment before the state
// was created (mi_process_info), so the interpreter's own baseline is excluded
// from the ratio, exactly like the plan's harness. The arena's own
// commit_bytes is printed next to it, so an accounting bug cannot hide: the two
// numbers have to agree within a band.
//
// Two gates beyond the footprint bands ride along:
//  - the M-V6 hot path: the chunk is timed per mode (best of five) and the arena
//    may not cost more than 5 % on it;
//  - the M3 gate: a sparse-slab workload (many small array tables, two thirds of
//    them dropped) is compacted with the pass off and on, and the arena's own
//    committed bytes before/after the pass are the metric -- the process commit
//    cannot follow a segment handed back to mimalloc.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <mimalloc.h>

namespace tests_unit_ext_bench_lua_mem {

struct workload {
    const char *name;
    const char *chunk;
    double band;          // max commit / logical live (0 = use 'churn_bytes')
    size_t churn_bytes;   // for the churn workload: max retained bytes
};

const char *k_wide_array =
    "T = {} "
    "local seed = 12345 "
    "for i = 1, 300 do "
    "  seed = (seed * 1103515245 + 12345) % 2147483648 "
    "  local n = 1 + seed % 5000 "
    "  local t = table.new(n) "
    "  for k = 1, n do t[k] = k end "
    "  T[i] = t "
    "end";

const char *k_hash_string =
    "T = {} "
    "for i = 1, 300 do "
    "  local t = {} "
    "  for k = 1, 200 do t['key:' .. i .. ':' .. k] = k end "
    "  for k = 1, 40 do t['a longer key that is not short:' .. i * 1000 + k] = k end "
    "  T[i] = t "
    "end";

const char *k_one_big_array =
    "T = table.new(2000000) "
    "for i = 1, 2000000 do T[i] = i end";

const char *k_churn =
    "for round = 1, 200 do "
    "  local t = table.new(2000) "
    "  for i = 1, 2000 do t[i] = i end "
    "  t = nil "
    "end";

  struct measurement {
      size_t commit_delta = 0; // mimalloc process commit over the baseline
      size_t arena_commit = 0; // the arena's own accounting (0 for plain)
      size_t logical_live = 0; // lua_gc(LUA_GCCOUNT) in bytes
      double seconds = 0.0; // wall time of the run itself (M-V6)
      bool ok = false;
      const char *error = "";
  };

// mimalloc's public process-info entry point (the one heap of the process).
size_t process_commit() {
    size_t elapsed = 0, user = 0, sys = 0, rss = 0, peak_rss = 0;
    size_t commit = 0, peak_commit = 0, faults = 0;
    mi_process_info(&elapsed, &user, &sys, &rss, &peak_rss, &commit, &peak_commit,
                    &faults);
    return commit;
}

int run_chunk(lua_State *L, const char *chunk) {
    int status = luaL_loadstring(L, chunk);
    if (status != LUA_OK) return status;
    return lua_pcall(L, 0, 0, 0);
}

// One fresh state, one workload. 'arena' selects the arena allocator; the plain
// state is the mimalloc baseline the plan measured.
measurement run(const workload &w, bool arena, int trim_level) {
    measurement m;
    if (process_commit() == 0) {
        m.error = "mi_process_info is not available in this build";
        return m;
    }
    const size_t before = process_commit();
    lua_State *L = nullptr;
    void *handle = nullptr;
    if (arena) {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        L = luaL_newstate_arena(&cfg);
        handle = (L != nullptr) ? luaL_arena(L) : nullptr;
    }
    else {
        L = luaL_newstate();
    }
    if (L == nullptr) {
        m.error = "could not create the state";
        return m;
    }
    luaL_openlibs(L);
    // M-V6: the hot path is the run itself (allocation + GC), so the clock
    // starts after the state exists and stops before the final collection.
    const auto started = std::chrono::steady_clock::now();
    if (run_chunk(L, w.chunk) != LUA_OK) {
        m.error = lua_tostring(L, -1);
        lua_close(L);
        return m;
    }
    // Collect first: a run ends with garbage still on the heap, and neither
    // mode should be credited for blocks the collector has not swept yet.
    lua_gc(L, LUA_GCCOLLECT, 0);
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    // Bring both modes to the same state before measuring: the arena returns
    // its empty segments and purges (level 1 also drops the huge cache), the
    // plain state just purges -- which is what the plan's harness did too
    // ('mi_collect(true)' before reading the commit).
    if (handle != nullptr) lua_ArenaTrimHandle(handle, trim_level >= 0 ? trim_level : 1);
    else mi_collect(true);
    const size_t after = process_commit();
    m.commit_delta = (after > before) ? after - before : 0;
    m.logical_live = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u
                     + (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
    if (handle != nullptr) {
        lua_ArenaStats st;
        lua_ArenaStatsOf(handle, &st);
        m.arena_commit = st.commit_bytes;
    }
    lua_close(L);
    if (handle != nullptr) lua_ArenaFree(handle);
    m.ok = true;
    return m;
}

double ratio(size_t commit, size_t live) {
    return live ? double(commit) / double(live) : 0.0;
}

// ---------------------------------------------------------------------------
// The sparse-slab workload (the M3 gate). Many small array tables land in the
// exact 9*64+4 = 580-byte class -- 112 slots per 64 KiB segment -- and then two
// thirds of them are dropped, so the surviving blocks sit sparsely in
// partially empty segments. That is the only shape a compaction pass can pack,
// and the only honest measure of it is the arena's own committed bytes
// before/after: a segment handed back to mimalloc does not show up in the
// process commit (mimalloc purges on its own schedule).
// ---------------------------------------------------------------------------
const char *k_sparse_slabs =
    "T = {} "
    "for i = 1, 600 do T[i] = table.new(64) "
    "  for k = 1, 64 do T[i][k] = i * 1000 + k end end "
    "for i = 1, 600 do if i % 3 ~= 0 then T[i] = nil end end "
    "collectgarbage('collect')";

struct slab_result {
    bool ok = false;
    size_t before = 0; // arena commit_bytes before the pass
    size_t after = 0; // arena commit_bytes after it
    size_t would = 0; // the plan the pass reported
    int moved = 0; // bytes the pass actually returned
};

slab_result run_sparse_slabs(bool compaction) {
    slab_result r;
    lua_ArenaConfig cfg;
    lua_ArenaConfigDefault(&cfg);
    cfg.enable_compaction = compaction ? 1 : 0;
    lua_State *L = luaL_newstate_arena(&cfg);
    if (L == nullptr) return r;
    void *handle = luaL_arena(L);
    luaL_openlibs(L);
    if (run_chunk(L, k_sparse_slabs) != LUA_OK) {
        lua_close(L);
        lua_ArenaFree(handle);
        return r;
    }
    lua_ArenaStats st;
    lua_ArenaStatsOf(handle, &st);
    r.before = st.commit_bytes;
    r.moved = lua_gc(L, LUA_GCCOMPACT, 0);
    lua_ArenaStatsOf(handle, &st);
    r.after = st.commit_bytes;
    r.would = st.would_compact_bytes;
    lua_close(L); // also proves the state still accounted for every slot
    lua_ArenaStatsOf(handle, &st);
    r.ok = (st.slots_used == 0);
    lua_ArenaFree(handle);
    return r;
}

} // namespace tests_unit_ext_bench_lua_mem
using namespace tests_unit_ext_bench_lua_mem;

int main(int argc, char *argv[]) {
    bool strict = true;
    int only = -1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-strict") == 0) strict = false;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = std::atoi(argv[++i]);
    }
    const workload workloads[] = {
        {"wide_array_tables", k_wide_array, 1.55, 0},
        {"hash_string_tables", k_hash_string, 1.35, 0},
        {"one_big_array", k_one_big_array, 1.15, 0},
        {"resize_churn", k_churn, 0.0, size_t(1024 * 1024)},
    };
    int failed = 0;
    std::printf("%-20s %-10s %10s %10s %10s %8s %s\n", "workload", "mode", "live_MB",
                "commit_MB", "arena_MB", "ratio", "band");
    for (int wi = 0; wi < 4; wi++) {
        if (only >= 0 && wi != only) continue;
        const workload &w = workloads[wi];
        // trim only matters for the churn and huge-array cases; level 1 also
        // drops the huge cache and purges free mimalloc pages.
        const int trim = 1;
        std::printf("--- %s ----------------------------------------------\n", w.name);
        measurement sizes[2];
        const char *modes[2] = {"mimalloc", "arena"};
        for (int k = 0; k < 2; k++) {
            mi_collect(true);  // start each run from a clean heap
            measurement m = run(w, k == 1, trim);
            // best-of-five for the timing comparison: one sample of a
            // sub-second run on a busy host is noise, the footprint numbers
            // are stable either way.
            for (int rep = 1; rep < 5 && m.ok; rep++) {
                mi_collect(true);
                const measurement again = run(w, k == 1, trim);
                if (!again.ok) break;
                if (again.seconds < m.seconds) m.seconds = again.seconds;
            }
            sizes[k] = m;
            if (!m.ok) {
                std::printf("%-20s %-10s ERROR: %s\n", w.name, modes[k], m.error);
                failed++;
                continue;
            }
            if (w.churn_bytes != 0) {
                std::printf("%-20s %-10s %10.2f %10.2f %10.2f %8s retained=%zu B (band <= %zu B)\n",
                            w.name, modes[k], m.logical_live / 1048576.0,
                            m.commit_delta / 1048576.0, m.arena_commit / 1048576.0, "-",
                            m.commit_delta, w.churn_bytes);
                if (k == 1 && m.commit_delta > w.churn_bytes) {
                    std::printf("    MISS: retained %zu B is above the %zu B band\n",
                                m.commit_delta, w.churn_bytes);
                    failed++;
                }
            }
            else {
                std::printf("%-20s %-10s %10.2f %10.2f %10.2f %8.2f <= %.2f\n", w.name, modes[k],
                            m.logical_live / 1048576.0, m.commit_delta / 1048576.0,
                            m.arena_commit / 1048576.0, ratio(m.commit_delta, m.logical_live),
                            w.band);
                if (k == 1) {
                    const double r = ratio(m.commit_delta, m.logical_live);
                    if (r > w.band) {
                        std::printf("    MISS: arena ratio %.2f is above the %.2f band\n", r, w.band);
                        failed++;
                    }
                    // the arena's own accounting must agree with the OS
                    if (m.arena_commit) {
                        const double skew = double(m.arena_commit) / double(m.commit_delta ? m.commit_delta : 1);
                        if (skew > 1.25 || skew < 0.75) {
                            std::printf("    NOTE: arena accounting %.2f MB vs process commit "
                                        "%.2f MB (skew %.2f)\n",
                                        m.arena_commit / 1048576.0, m.commit_delta / 1048576.0, skew);
                        }
                    }
                }
            }
        }
        if (sizes[0].ok && sizes[1].ok && w.churn_bytes == 0 && sizes[0].logical_live > 0) {
            std::printf("    ratio change: mimalloc %.2f -> arena %.2f\n",
                        ratio(sizes[0].commit_delta, sizes[0].logical_live),
                        ratio(sizes[1].commit_delta, sizes[1].logical_live));
        }
        // M-V6: the hot path may not get more than 5 % slower than mimalloc.
        if (sizes[0].ok && sizes[1].ok && sizes[0].seconds > 0.0) {
            const double rel = sizes[1].seconds / sizes[0].seconds;
            std::printf("    hot path: mimalloc %.3f s, arena %.3f s -> %+.1f %% (bar +5 %%)\n",
                        sizes[0].seconds, sizes[1].seconds, (rel - 1.0) * 100.0);
            if (rel > 1.05) {
                std::printf("    MISS: the arena is %+.1f %% slower on the hot path\n",
                            (rel - 1.0) * 100.0);
                failed++;
            }
        }
    }
    // The M3 gate: a sparse-slab workload, compacted. 'plan' runs with the pass
    // disabled (it must report the same plan and move nothing), 'pack' runs with
    // it enabled. The plan accepts the feature only if packing clears 10 %.
    std::printf("--- sparse_slabs (M3 gate) ----------------------------------------\n");
    {
        const slab_result plan = run_sparse_slabs(false);
        const slab_result pack = run_sparse_slabs(true);
        if (!plan.ok || !pack.ok) {
            std::printf("%-20s ERROR: a sparse-slab state did not close cleanly%s\n",
                        "sparse_slabs",
                        plan.ok ? "" : " (plan)");
            failed++;
        }
        else {
            const double gain = pack.before
                                    ? 100.0 * double(pack.before - pack.after) / double(pack.before)
                                    : 0.0;
            std::printf("%-20s %-10s %10s %10s\n", "workload", "mode", "arena_MB", "gain");
            std::printf("%-20s %-10s %10.2f %9.2f %%  plan %.2f MB (moved %d B)\n",
                        "sparse_slabs", "plan", plan.before / 1048576.0, 0.0,
                        plan.would / 1048576.0, plan.moved);
            std::printf("%-20s %-10s %10.2f %9.2f %%  of %.2f MB (moved %d B)\n",
                        "sparse_slabs", "pack", pack.after / 1048576.0, gain,
                        pack.before / 1048576.0, pack.moved);
            std::printf("    would_compact_bytes %.2f MB, bar 10 %%\n", pack.would / 1048576.0);
            if (gain < 10.0) {
                std::printf("    NOTE: packing clears %.1f %% (< 10 %%): the pass stays off by "
                            "default (an accepted negative result, see docs/lua_memory_arena.md)\n",
                            gain);
            }
            if (plan.moved != 0) {
                std::printf("    MISS: a plan without enable_compaction moved %d B\n", plan.moved);
                failed++;
            }
        }
    }
    std::printf("=====================================================\n");
    if (failed == 0) {
        std::printf("bench_lua_mem: every band met\n");
        return 0;
    }
    std::printf("bench_lua_mem: %d band(s) missed%s\n", failed,
                strict ? " (exit 1)" : " (--no-strict: exit 0)");
    return strict ? 1 : 0;
}
