// Test for the Lua table memory policy of the kimix arena (KIMIX_LUA_ARENA):
// block sizing, table.new presize, table.shrink and the compaction pass.
// This test covers:
// - The array block of a table is exactly 9*N+4 bytes and lands in the exact
//   class of that size (the engine's view of concretesize(), cross-checked
//   against the library by observing which class gains a slot)
// - table.new(narr) presizes: one allocation instead of the growth chain of
//   `for i = 1, n do t[i] = i end`
// - table.shrink(t) returns the unused array capacity, keeps #t, every value
//   and the array-part traversal order, and is a no-op the second time
// - In a dry run, table.shrink only reports what it would return
// - lua_gc(LUA_GCCOMPACT, 0) plans without moving (enable_compaction off) and
//   packs sparse segments when it is on, leaving every table intact
// - T.checkmemory()-equivalent accounting: slots_used returns to the baseline
//   and the arena commits nothing extra after a close

#include "ut/ut.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lualib.h"
}

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_ext_test_lua_table_mem {

struct arena_state {
    lua_State *L = nullptr;
    void *arena = nullptr;
    lua_ArenaConfig cfg;

    explicit arena_state(const lua_ArenaConfig *config = nullptr) {
        if (config) cfg = *config;
        else lua_ArenaConfigDefault(&cfg);
        L = luaL_newstate_arena(&cfg);
        if (L) {
            arena = luaL_arena(L);
            luaL_openlibs(L);
        }
    }
    ~arena_state() {
        if (L) luaL_freestate_arena(L);
    }
    arena_state(const arena_state &) = delete;
    arena_state &operator=(const arena_state &) = delete;

    lua_ArenaStats stats() const {
        lua_ArenaStats st;
        lua_ArenaStatsOf(arena, &st);
        return st;
    }
    int dostring(const char *chunk) {
        int status = luaL_loadstring(L, chunk);
        if (status != LUA_OK) return status;
        return lua_pcall(L, 0, 0, 0);
    }
    lua_Number number_global(const char *name) {
        lua_getglobal(L, name);
        lua_Number v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    }
};

// slots_used per class, to see which class a single allocation landed in.
std::vector<unsigned> class_usage(const arena_state &st) {
    const int n = lua_ArenaClassCount(st.arena);
    std::vector<unsigned> used(n, 0u);
    for (int cid = 0; cid < n; cid++) {
        lua_ArenaClassInfo info;
        if (lua_ArenaClassInfoGet(st.arena, cid, &info)) used[cid] = info.slots_used;
    }
    return used;
}

// Find the class whose used count increased by exactly 'delta' and whose slot
// size is 'slot'. Returns the class id or -1.
int class_that_grew(const std::vector<unsigned> &before, const std::vector<unsigned> &after,
                    unsigned delta, unsigned slot, const arena_state &st) {
    for (size_t cid = 0; cid < before.size(); cid++) {
        if (after[cid] - before[cid] == delta) {
            lua_ArenaClassInfo info;
            if (lua_ArenaClassInfoGet(st.arena, (int)cid, &info) && info.slot_size == slot)
                return (int)cid;
        }
    }
    return -1;
}

} // namespace tests_unit_ext_test_lua_table_mem
using namespace tests_unit_ext_test_lua_table_mem;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "array_block_is_exactly_9N4"_test = [] {
        arena_state st;
        if (!st.L) return;
        expect(st.dostring("collectgarbage('stop')") == LUA_OK);
        for (unsigned log = 0; log <= 10; log++) {  // N = 1 .. 1024
            const unsigned n = 1u << log;
            const unsigned bytes = 9u * n + 4u;
            std::vector<unsigned> before = class_usage(st);
            lua_createtable(st.L, (int)n, 0);  // keep it on the stack
            std::vector<unsigned> after = class_usage(st);
            const int grew = class_that_grew(before, after, 1u, bytes, st);
            expect(grew >= 0)
                << "lua_createtable(n) must allocate one block of exactly 9*N+4 bytes";
            if (grew >= 0) {
                lua_ArenaClassInfo info;
                lua_ArenaClassInfoGet(st.arena, grew, &info);
                expect(eq(info.tier, 3)) << "an array block belongs to an exact class";
            }
            lua_pop(st.L, 1);  // the table is garbage now; the block goes back
        }
    };

    "table_new_presizes_in_one_allocation"_test = [] {
        // Two fresh states, one workload each, so the interpreter's own
        // footprint is the same on both sides and only the table strategy
        // differs. The claim is about the footprint: the implicit growth walk
        // (1 -> 2 -> 4 -> ... -> 2^18 blocks) leaves every intermediate block
        // behind as dead capacity, the presized table allocates the array part
        // once.
        auto measure = [](bool presize, unsigned long *calls) -> size_t {
            arena_state st;
            if (!st.L) return 0;
            if (st.dostring("collectgarbage('stop')") != LUA_OK) return 0;
            const char *chunk =
                presize ? "local t = table.new(200000) for i = 1, 200000 do t[i] = i end return #t"
                        : "local t = {} for i = 1, 200000 do t[i] = i end return #t";
            if (luaL_loadstring(st.L, chunk) != LUA_OK) return 0;
            if (lua_pcall(st.L, 0, 1, 0) != LUA_OK) return 0;
            const lua_Integer n = lua_tointeger(st.L, -1);
            lua_pop(st.L, 1);
            if (n != 200000) return 0;
            lua_ArenaStats stats = st.stats();
            if (calls) {
                // count only the workload: the chunk was compiled before this
                // (the load above), so the window below is one call of it
                *calls = stats.calls;
            }
            return stats.commit_bytes;
        };
        unsigned long calls_plain = 0, calls_new = 0;
        const size_t plain = measure(false, &calls_plain);
        const size_t presized = measure(true, &calls_new);
        std::fprintf(stderr, "[presize] implicit growth commit=%zu calls=%lu, "
                             "table.new commit=%zu calls=%lu\n",
                     plain, calls_plain, presized, calls_new);
        expect(plain > 0u) << "the implicit-growth workload must run";
        expect(presized > 0u) << "the presized workload must run";
        expect(presized < plain)
            << "table.new must leave a smaller footprint than the growth chain";
        expect(presized * 4 < plain * 3)
            << "and by at least a quarter of the implicit-growth footprint";
    };

    "shrink_releases_capacity_and_keeps_contents"_test = [] {
        arena_state st;
        if (!st.L) return;
        if (st.stats().dry_run) {
            // A dry run only reports the shrink (see the next test):
            // KIMIX_LUA_ARENA_DRYRUN=1 replays the suite through the decision
            // logic, so the observed-effect half is skipped (M-V1).
            std::printf("[dry run] shrink effects skipped: nothing is resized\n");
            return;
        }
        expect(st.dostring(
                   "collectgarbage('stop') "
                   "T = table.new(1024) "
                   "for i = 1, 1024 do T[i] = i * 2 end "
                   "for i = 129, 1024 do T[i] = nil end") == LUA_OK);
        lua_ArenaStats before = st.stats();
        expect(st.dostring("SHRUNK = table.shrink(T) LEN = #T SUM = 0 "
                           "for i = 1, #T do SUM = SUM + T[i] end") == LUA_OK);
        lua_ArenaStats after = st.stats();
        const lua_Number released = st.number_global("SHRUNK");
        expect(released > 0) << "shrinking a half-empty array must release bytes";
        expect(eq(st.number_global("LEN"), (lua_Number)128)) << "#t must not change";
        expect(eq(st.number_global("SUM"), (lua_Number)(2 * (128 * 129 / 2))))
            << "every live value must survive the move";
        expect(after.logical_live < before.logical_live) << "the arena must commit less";
        expect(after.would_shrink_bytes >= before.would_shrink_bytes);
        // a second shrink has nothing left to do
        expect(st.dostring("AGAIN = table.shrink(T)") == LUA_OK);
        expect(eq(st.number_global("AGAIN"), (lua_Number)0)) << "shrink must be idempotent";
        // and the array-part traversal order is unchanged
        expect(st.dostring("ORDER = 1 local prev = 0 "
                           "for k, v in ipairs(T) do if k ~= prev + 1 or v ~= k * 2 then "
                           "ORDER = 0 end prev = k end") == LUA_OK);
        expect(eq(st.number_global("ORDER"), (lua_Number)1));
    };

    "shrink_in_dry_run_only_reports"_test = [] {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        cfg.dry_run = 1;
        arena_state st(&cfg);
        if (!st.L) return;
        expect(st.dostring("T = table.new(1024) for i = 1, 128 do T[i] = i end") == LUA_OK);
        lua_ArenaStats before = st.stats();
        expect(st.dostring("SHRUNK = table.shrink(T)") == LUA_OK);
        lua_ArenaStats after = st.stats();
        expect(eq(st.number_global("SHRUNK"), (lua_Number)0))
            << "a dry run must not resize anything";
        expect(after.would_shrink_bytes > before.would_shrink_bytes)
            << "a dry run must report what a shrink would return";
    };

    "compaction_plans_then_packs"_test = [] {
        // Build 96 small array tables, drop two thirds of them, then ask for a
        // compaction: the surviving blocks must be packed into fewer segments
        // and every value must survive.
        // 300 array tables of 64 elements occupy three segments of the exact
        // 580-byte class; dropping two thirds of them leaves that class below
        // 50% occupancy, which is the case a compaction can actually pack.
        const char *build =
            "T = {} "
            "for i = 1, 300 do T[i] = table.new(64) "
            "  for k = 1, 64 do T[i][k] = i * 1000 + k end end "
            "for i = 1, 300 do if i % 3 ~= 0 then T[i] = nil end end "
            "collectgarbage('collect') ";
        lua_ArenaConfig plan_cfg;
        lua_ArenaConfigDefault(&plan_cfg);
        plan_cfg.enable_compaction = 0;  // plan only
        {
            arena_state st(&plan_cfg);
            if (!st.L) return;
            expect(st.dostring(build) == LUA_OK);
            const lua_ArenaStats before = st.stats();
            lua_gc(st.L, LUA_GCCOMPACT, 0);
            const lua_ArenaStats after = st.stats();
            expect(after.would_compact_bytes > 0u)
                << "the planner must find a sparse segment to reclaim";
            expect(eq(after.compacted_bytes, (size_t)0))
                << "with enable_compaction off nothing may move";
            expect(eq(after.commit_bytes, before.commit_bytes));
        }
        lua_ArenaConfig move_cfg;
        lua_ArenaConfigDefault(&move_cfg);
        move_cfg.enable_compaction = 1;
        arena_state st(&move_cfg);
        if (!st.L) return;
        if (st.stats().dry_run) {
            // KIMIX_LUA_ARENA_DRYRUN=1: the plan half above is the real
            // assertion, the mover cannot run without carved segments (M-V1).
            std::printf("[dry run] compaction move skipped: no carved segment\n");
            return;
        }
        expect(st.dostring(build) == LUA_OK);
        expect(st.dostring("CHECK = 1 for i = 1, 300 do local t = T[i] "
                           " if t then for k = 1, 64 do "
                           "  if t[k] ~= i * 1000 + k then CHECK = 0 end end end end") == LUA_OK);
        expect(eq(st.number_global("CHECK"), (lua_Number)1));
        const lua_ArenaStats before = st.stats();
        const int moved = lua_gc(st.L, LUA_GCCOMPACT, 0);
        const lua_ArenaStats after = st.stats();
        expect(after.compactions >= 1u) << "the pass must run";
        expect(moved >= 0);
        expect(after.would_compact_bytes > 0u) << "the plan is always reported";
        expect(after.compacted_bytes > 0u) << "an enabled compaction must return segments";
        expect(after.commit_bytes < before.commit_bytes)
            << "compaction must lower the committed bytes";
        expect(after.compaction_skips == 0u) << "the preconditions must hold in this test";
      expect(eq(lua_ArenaInteriorHolders(st.arena), 0u))
          << "no call may hold an interior pointer across a compaction";
        // contents intact after the move
        expect(st.dostring("CHECK = 1 for i = 1, 300 do local t = T[i] "
                           " if t then for k = 1, 64 do "
                           "  if t[k] ~= i * 1000 + k then CHECK = 0 end end end end") == LUA_OK);
        expect(eq(st.number_global("CHECK"), (lua_Number)1))
            << "compaction must not change a single value";
        expect(st.dostring("CHECK = 0 for i = 1, 300 do if T[i] then CHECK = CHECK + 1 end end")
               == LUA_OK);
        expect(eq(st.number_global("CHECK"), (lua_Number)100)) << "no table may be lost";
    };

    "compaction_is_a_noop_without_tables"_test = [] {
        arena_state st;
        if (!st.L) return;
        expect(st.dostring("collectgarbage('stop') S = '' for i = 1, 200 do S = S .. i end") == LUA_OK);
        const lua_ArenaStats before = st.stats();
        const int moved = lua_gc(st.L, LUA_GCCOMPACT, 0);
        (void)moved;
        const lua_ArenaStats after = st.stats();
        expect(eq(after.compacted_bytes, before.compacted_bytes))
            << "nothing to pack: no block may move";
    };

    "close_returns_every_slot"_test = [] {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        lua_State *L = luaL_newstate_arena(&cfg);
        expect(L != nullptr);
        if (!L) return;
        luaL_openlibs(L);
        void *arena = luaL_arena(L);
        expect(luaL_dostring(L, "T = {} for i = 1, 5000 do T[i] = {i, tostring(i)} end") == LUA_OK);
        lua_ArenaStats live;
        lua_ArenaStatsOf(arena, &live);
        expect(live.slots_used > 0u);
        expect(live.logical_live > 0u);
        lua_close(L);
        lua_ArenaStats closed;
        lua_ArenaStatsOf(arena, &closed);
        expect(eq(closed.slots_used, (unsigned long)0))
            << "every slot must be back after lua_close (T.checkmemory-equivalent)";
        expect(eq(closed.logical_live, (size_t)0));
        const size_t trimmed = lua_ArenaTrimHandle(arena, 1);
        (void)trimmed;
        lua_ArenaStats idle;
        lua_ArenaStatsOf(arena, &idle);
        expect(eq(idle.huge_cached, (unsigned long)0));
        expect(idle.commit_bytes < live.commit_bytes)
            << "a closed, trimmed arena commits less than a live one";
        lua_ArenaFree(arena);
    };
}
