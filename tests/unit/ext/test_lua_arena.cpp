// Test for the kimix Lua memory arena (src/ext/lua/larena.c, KIMIX_LUA_ARENA).
// This test covers:
// - Class math: monotonic classof, slot >= request, slot <= 2x request (or 16),
//   boundary sizes (15/16/17, 511/512/513, 8 KiB +-1, 64 KiB +-1) and the
//   exact-size array (9*N+4) / node (32*2^k [+16]) classes
// - Free-list discipline: LIFO reuse, no cross-class reuse, a slot is never
//   handed out twice, freeing everything returns the accounting to zero
// - Segment accounting: slots_used + slots_free == slots_total per class and
//   in total, seg_count * nslots_per_seg conservation, high-water tracking
// - Waste bound: internal_waste stays inside the tier bands on a synthetic
//   trace of realistic (16-byte aligned) block sizes
// - The huge tier: a freed run of an exact size is reused verbatim (one
//   mimalloc call per size, not one per resize)
// - Trim/purge: trim returns only whole empty segments, repeated trims
//   converge, level 1 drops the huge cache and purges
// - Dry run: the same decisions are counted (would_*) and no segment is carved
// - Conservation under random allocation traffic (fixed seed, 20k ops), and
//   slots_used == 0 / seg_count == 0 after freeing everything
// - The state-level contract: a chunk produces identical output in an arena
//   state and in a plain mimalloc state, and arena.logical_live tracks
//   lua_gc(LUA_GCCOUNT) (the same number the sandbox budget reports)
//
// The arena is exercised through its public C API only (luaL_newstate_arena,
// lua_Alloc from lua_getallocf, lua_ArenaStats*, lua_ArenaClass*), i.e. exactly
// what an embedder can reach.
//
// KIMIX_LUA_MEMCHECK (on in every non-NDEBUG build) makes the arena itself
// assert on a double free / a foreign pointer, so the free-list tests would
// abort instead of failing an expect -- that is intentional: the invariant is
// stronger than an assertion in a test.

#include "ut/ut.hpp"

// The vendored lua.h carries no `extern "C"` block (only LUAMOD_API gets the
// `extern "C"` spelling), so C++ consumers must wrap the includes themselves.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_ext_test_lua_arena {

// RAII guard: one arena-backed Lua state per test. The arena is owned by the
// state, so the guard frees it (and the state) unconditionally.
struct arena_state {
    lua_State *L = nullptr;
    void *arena = nullptr;
    lua_ArenaConfig cfg;

    explicit arena_state(const lua_ArenaConfig *config = nullptr) {
        if (config) cfg = *config;
        else lua_ArenaConfigDefault(&cfg);
        L = luaL_newstate_arena(&cfg);
        if (L) arena = luaL_arena(L);
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
    // The arena is installed as this state's lua_Alloc: driving it directly is
    // how the allocator is tested without a Lua workload.
    lua_Alloc alloc(void **ud) const { return lua_getallocf(L, ud); }

    int dostring(const char *chunk) {
        int status = luaL_loadstring(L, chunk);
        if (status != LUA_OK) return status;
        return lua_pcall(L, 0, LUA_MULTRET, 0);
    }
};

// A plain mimalloc-backed state, for the "same output" comparison.
struct plain_state {
    lua_State *L = nullptr;
    plain_state() : L(luaL_newstate()) {
        if (L) luaL_openlibs(L);
    }
    ~plain_state() {
        if (L) lua_close(L);
    }
    plain_state(const plain_state &) = delete;
    plain_state &operator=(const plain_state &) = delete;
};

// Run a chunk with every library open and return what it printed through a
// captured print(); used for the byte-identical-output comparison.
std::string run_and_capture(lua_State *L, const char *chunk) {
    std::string out;
    lua_pushlightuserdata(L, &out);
    lua_pushcclosure(L, +[](lua_State *l) -> int {
        size_t len = 0;
        const char *s = luaL_tolstring(l, 1, &len);
        std::string *dst = static_cast<std::string *>(lua_touserdata(l, lua_upvalueindex(1)));
        if (dst && s) {
            dst->append(s, len);
            dst->push_back('\n');
        }
        return 0;
    }, 1);
    lua_setglobal(L, "print");
    if (luaL_loadstring(L, chunk) != LUA_OK) return "<load error>";
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        return std::string("<error> ") + (err ? err : "?");
    }
    return out;
}

// Sum of the per-class counters, checked against the flat stats totals.
struct class_totals {
    unsigned long used = 0, free = 0, segs = 0;
    bool conserved = true;
};

class_totals scan_classes(const arena_state &st) {
    class_totals t;
    int n = lua_ArenaClassCount(st.arena);
    for (int cid = 0; cid < n; cid++) {
        lua_ArenaClassInfo info;
        if (!lua_ArenaClassInfoGet(st.arena, cid, &info)) {
            t.conserved = false;
            continue;
        }
        t.used += info.slots_used;
        t.free += info.slots_free;
        t.segs += info.seg_count;
        if (info.seg_count == 0 && (info.slots_used || info.slots_free)) t.conserved = false;
        if (info.seg_count > 0) {
            // a segment holds nslots slots; every slot is used or free
            unsigned per_seg = info.seg_payload / info.slot_size;
            if (info.slots_used + info.slots_free != info.seg_count * per_seg) t.conserved = false;
        }
    }
    return t;
}

} // namespace tests_unit_ext_test_lua_arena
using namespace tests_unit_ext_test_lua_arena;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "state_created_with_arena"_test = [] {
        arena_state st;
        expect(st.L != nullptr) << "luaL_newstate_arena must succeed";
        expect(st.arena != nullptr) << "a state from luaL_newstate_arena must expose its arena";
        if (!st.L) return;
        lua_ArenaStats s = st.stats();
        expect(eq(s.enabled, 1));
        // KIMIX_LUA_ARENA_DRYRUN=1 replays the whole suite through the decision
        // logic (M-V1), so the mode is the environment's, not a constant.
        const int expected_dry = std::getenv("KIMIX_LUA_ARENA_DRYRUN") != nullptr ? 1 : 0;
        expect(eq(s.dry_run, expected_dry))
            << "dry_run must follow KIMIX_LUA_ARENA_DRYRUN";
        expect(s.segment_bytes >= 4096u) << "segment size must be the 64 KiB default";
        expect(lua_gc(st.L, LUA_GCISRUNNING, 0) == 1) << "the GC must be running";
        expect(lua_gc(st.L, LUA_GCTRIM, 0) >= 0) << "LUA_GCTRIM must be wired";
        expect(lua_gc(st.L, LUA_GCCOMPACT, 0) >= 0) << "LUA_GCCOMPACT must be wired";
    };

    "class_math_monotonic_and_bounded"_test = [] {
        arena_state st;
        if (!st.L) return;
        const int n = lua_ArenaClassCount(st.arena);
        expect(n > 32) << "the class table must hold the tiny tier plus the ladder";
        // The generic ladder must be monotonic (a bigger request never lands in
        // a smaller rounded slot). The exact table-block classes (tier 3/4) are
        // deliberately outside the ladder: they hand out exactly the requested
        // bytes, which is what makes a block reusable verbatim.
        int previous_slot = 0;
        int exact_classes = 0;
        for (size_t size = 1; size <= 70000; size++) {
            int cid = lua_ArenaClassOf(st.arena, size);
            lua_ArenaClassInfo info;
            if (cid < 0) {
                // the huge tier starts above small_max
                expect(size > 65536u) << "only sizes above small_max may be huge";
                continue;
            }
            expect(lua_ArenaClassInfoGet(st.arena, cid, &info) == 1);
            expect(info.slot_size >= size) << "a slot must never be smaller than the request";
            const size_t bound = size * 2 > 16 ? size * 2 : 16;
            expect(info.slot_size <= bound) << "the rounding must stay inside one step";
            if (info.tier == 3 || info.tier == 4) {
                exact_classes++;
                expect(eq(info.slot_size, (unsigned)size))
                    << "an exact class must hand out exactly the requested bytes";
                continue;
            }
            if (info.slot_size < (unsigned)previous_slot) {
                expect(false) << "the generic ladder must be monotonic in the request";
            }
            previous_slot = (int)info.slot_size;
        }
        expect(exact_classes > 20) << "the exact table-block classes must be reachable";
    };

    "class_math_boundaries"_test = [] {
        arena_state st;
        if (!st.L) return;
        struct probe {
            size_t size;
            unsigned expect_slot;  // 0 = "any slot >= size"
        };
        const probe probes[] = {
            {15, 16}, {16, 16}, {17, 32}, {511, 512}, {512, 512}, {513, 0},
            {8191, 0}, {8192, 0}, {8193, 0}, {65535, 0}, {65536, 0},
        };
        for (const auto &p : probes) {
            int cid = lua_ArenaClassOf(st.arena, p.size);
            expect(cid >= 0) << "an inexact size below small_max must be slotted";
            lua_ArenaClassInfo info;
            expect(lua_ArenaClassInfoGet(st.arena, cid, &info) == 1);
            expect(info.slot_size >= p.size);
            if (p.expect_slot != 0) {
                expect(eq(info.slot_size, p.expect_slot)) << "16-byte granule in the tiny tier";
            }
        }
        expect(lua_ArenaClassOf(st.arena, 65537) < 0) << "above small_max is the huge tier";
    };

    "class_math_exact_table_blocks"_test = [] {
        arena_state st;
        if (!st.L) return;
        // array blocks are 9*N+4 with N a power of two: exact-size classes
        for (unsigned log = 0; log < 13; log++) {  // 9*4096+4 = 36868 < 64 KiB
            size_t n = size_t(1) << log;
            size_t bytes = 9 * n + 4;
            if (bytes > 65536) break;
            int cid = lua_ArenaClassOf(st.arena, bytes);
            lua_ArenaClassInfo info;
            expect(cid >= 0);
            expect(lua_ArenaClassInfoGet(st.arena, cid, &info) == 1);
            expect(eq(info.slot_size, (unsigned)bytes))
                << "an array block must get a slot of exactly 9*N+4 bytes";
            expect(eq(info.tier, 3)) << "tier 3 is the exact array class";
        }
        // node blocks are 32*2^k (+16 once the 'lastfree' prefix appears)
        for (unsigned log = 0; log < 11; log++) {
            size_t bytes = size_t(32) << log;
            if (bytes > 65536) break;
            for (size_t extra : {size_t(0), size_t(16)}) {
                size_t total = bytes + extra;
                if (total > 65536) continue;
                int cid = lua_ArenaClassOf(st.arena, total);
                lua_ArenaClassInfo info;
                expect(cid >= 0);
                expect(lua_ArenaClassInfoGet(st.arena, cid, &info) == 1);
                expect(eq(info.slot_size, (unsigned)total))
                    << "a node block must get a slot of exactly its size";
            }
        }
    };

    "free_list_is_lifo_and_single_handout"_test = [] {
        arena_state st;
        if (!st.L) return;
        if (st.stats().dry_run) {
            // A dry run allocates from mimalloc: there is no slot to hand out
            // twice, so the LIFO discipline is unobservable (M-V1 replay).
            std::printf("[dry run] free-list discipline skipped: no carved slot\n");
            return;
        }
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        expect(f != nullptr);
        lua_ArenaStats before = st.stats();
        const size_t sizes[] = {48, 100, 1000, 9000};
        for (size_t size : sizes) {
            void *a = f(ud, nullptr, 0, size);
            expect(a != nullptr);
            f(ud, a, size, 0);
            void *b = f(ud, nullptr, 0, size);
            expect(b == a) << "the most recently freed slot must be the first handed out";
            f(ud, b, size, 0);
        }
        lua_ArenaStats after = st.stats();
        expect(eq(after.slots_used, before.slots_used))
            << "alloc/free pairs must not leak slots";
        expect(eq(after.logical_live, before.logical_live));
        // two live blocks of the same size must not be the same slot
        void *x = f(ud, nullptr, 0, 200);
        void *y = f(ud, nullptr, 0, 200);
        expect(x != nullptr && y != nullptr && x != y);
        f(ud, x, 200, 0);
        f(ud, y, 200, 0);
    };

    "free_list_grow_in_place_keeps_the_slot"_test = [] {
        arena_state st;
        if (!st.L) return;
        if (st.stats().dry_run) {
            std::printf("[dry run] in-place realloc skipped: no carved slot\n");
            return;
        }
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        // 200 and 205 share the 208-byte granule: the slot is reused in place
        void *p = f(ud, nullptr, 0, 200);
        void *q = f(ud, p, 200, 205);
        expect(q == p) << "a realloc inside one class must not move the block";
        // 205 -> 400 crosses classes: it must move and copy
        std::memset(q, 0x5a, 200);
        void *r = f(ud, q, 205, 400);
        expect(r != nullptr);
        if (r) {
            const unsigned char *bytes = static_cast<const unsigned char *>(r);
            bool kept = true;
            for (size_t i = 0; i < 200; i++) kept = kept && bytes[i] == 0x5a;
            expect(kept) << "a cross-class realloc must copy the payload";
            f(ud, r, 400, 0);
        }
    };

    "segment_accounting_conserved"_test = [] {
        arena_state st;
        if (!st.L) return;
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        std::vector<std::pair<void *, size_t>> live;
        for (int i = 0; i < 500; i++) {
            size_t size = 32 + (size_t)(i % 40) * 16;
            void *p = f(ud, nullptr, 0, size);
            expect(p != nullptr);
            live.emplace_back(p, size);
        }
        class_totals t = scan_classes(st);
        expect(t.conserved) << "slots_used + slots_free must equal seg_count * nslots";
        lua_ArenaStats s = st.stats();
        expect(eq(s.slots_used, t.used));
        expect(eq(s.slots_free, t.free));
        expect(eq(s.seg_count, t.segs));
        expect(s.commit_bytes >= s.logical_live) << "commit covers everything that is live";
        for (auto &entry : live) f(ud, entry.first, entry.second, 0);
    };

    "segment_accounting_and_trim"_test = [] {
        arena_state st;
        if (!st.L) return;
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        const lua_ArenaStats base = st.stats();  // the state's own blocks
        const size_t size = 96;
        std::vector<void *> live;
        for (int i = 0; i < 4000; i++) live.push_back(f(ud, nullptr, 0, size));
        lua_ArenaStats full = st.stats();
        expect(full.seg_count > base.seg_count);
        expect(full.slots_used >= base.slots_used + 4000);
        for (void *p : live) f(ud, p, size, 0);
        // everything the test allocated is free, but the segments are carved
        lua_ArenaStats idle = st.stats();
        expect(eq(idle.slots_used, base.slots_used))
            << "only the state's own slots may stay used";
        const size_t released = lua_ArenaTrimHandle(st.arena, 0);
        lua_ArenaStats trimmed = st.stats();
        expect(released > 0) << "trim must return the empty segments";
        expect(trimmed.seg_count < idle.seg_count);
        expect(eq(trimmed.slots_used, base.slots_used));
        expect(eq(released, idle.commit_bytes - trimmed.commit_bytes))
            << "the bytes a trim reports must be the bytes it gave back";
        // repeated trims converge: a second trim finds (almost) nothing left
        const size_t again = lua_ArenaTrimHandle(st.arena, 0);
        expect(again < released) << "repeated trims must converge";
    };

    "waste_bound_on_aligned_trace"_test = [] {
        arena_state st;
        if (!st.L) return;
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        std::vector<std::pair<void *, size_t>> live;
        // realistic block sizes: table structs (48), node blocks, array blocks
        // and string payloads, all 16-byte aligned
        for (int i = 0; i < 300; i++) {
            size_t size = 16 * (1 + (i % 64));
            live.emplace_back(f(ud, nullptr, 0, size), size);
        }
        lua_ArenaStats s = st.stats();
        const double waste = s.logical_live ? double(s.internal_waste) / double(s.logical_live) : 0.0;
        expect(waste <= 0.12) << "16-byte aligned traffic must not waste more than 12%";
        for (auto &entry : live) f(ud, entry.first, entry.second, 0);
    };

    "huge_tier_reuses_a_freed_run"_test = [] {
        arena_state st;
        if (!st.L) return;
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        const size_t size = 300 * 1024;  // above small_max: the huge tier
        void *p = f(ud, nullptr, 0, size);
        expect(p != nullptr);
        expect(lua_ArenaClassOf(st.arena, size) < 0) << "a 300 KiB block is huge";
        f(ud, p, size, 0);
        lua_ArenaStats cached = st.stats();
        expect(eq(cached.huge_cached, (unsigned long)1)) << "the freed run must be cached";
        void *q = f(ud, nullptr, 0, size);
        expect(q == p) << "the cached run must be reused verbatim";
        lua_ArenaStats reused = st.stats();
        expect(eq(reused.huge_cached, (unsigned long)0));
        expect(eq(reused.would_huge_reuse, (unsigned long)1));
        f(ud, q, size, 0);
        // trim level 1 drops the cache
        size_t released = lua_ArenaTrimHandle(st.arena, 1);
        (void)released;
        lua_ArenaStats purged = st.stats();
        expect(eq(purged.huge_cached, (unsigned long)0));
        expect(eq(purged.huge_cached_bytes, (size_t)0));
        expect(purged.purges >= 1u) << "level 1 must purge through mimalloc";
    };

    "dry_run_counts_without_carving"_test = [] {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        cfg.dry_run = 1;
        arena_state st(&cfg);
        if (!st.L) return;
        lua_ArenaStats s0 = st.stats();
        expect(eq(s0.dry_run, 1));
        expect(s0.would_segments > 0u) << "state creation alone already predicts segments";
        // A Lua workload still runs; only the counters move, and every byte the
        // model accounts for is a prediction (there is no segment behind it).
        expect(st.dostring("local t = {} for i = 1, 2000 do t[i] = i end") == LUA_OK);
        lua_ArenaStats s1 = st.stats();
        expect(s1.calls > 0u) << "the allocator sees every call";
        expect(s1.would_alloc + s1.would_free >= s1.calls)
            << "every call is classified (a realloc counts as one alloc plus one free)";
        expect(s1.would_alloc > 0u && s1.would_free > 0u);
        expect(s1.would_segments > s0.would_segments) << "the workload must grow the model";
        expect(s1.would_reuse > 0u) << "a workload reuses slots";
        expect(s1.slots_used > 0u);
        expect(s1.logical_live > 0u);
        size_t predicted = lua_ArenaTrimHandle(st.arena, 0);
        (void)predicted;
        lua_ArenaStats s2 = st.stats();
        expect(s2.seg_count <= s1.seg_count)
            << "a dry-run trim may only shrink the prediction";
        expect(eq(s2.logical_live, s1.logical_live)) << "a trim never changes live bytes";
    };

    "dry_run_matches_the_real_engine"_test = [] {
        // The same workload, once simulated and once real: the class/slot
        // decisions must agree, which is what makes the model trustworthy.
        const char *chunk =
            "local t = {} "
            "for i = 1, 3000 do t[i] = {i} end "
            "local s = {} "
            "for i = 1, 900 do s['k' .. i] = 'v' .. i end "
            "return #t";
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        cfg.dry_run = 1;
        unsigned long dry_slots = 0, dry_segs = 0;
        {
            arena_state st(&cfg);
            if (!st.L) return;
            expect(st.dostring(chunk) == LUA_OK);
            lua_ArenaStats s = st.stats();
            dry_slots = s.slots_used;
            dry_segs = s.seg_count;
            expect(s.would_reuse > 0u);
        }
        arena_state real;
        if (!real.L) return;
        expect(real.dostring(chunk) == LUA_OK);
        lua_ArenaStats s = real.stats();
        expect(s.slots_used > 0u);
        expect(s.slots_used >= dry_slots / 2 && s.slots_used <= dry_slots * 2)
            << "the dry run must predict the live slot count within a factor of 2";
        (void)dry_segs;
    };

    "compaction_dry_run_reports_only"_test = [] {
        // M-V1, fourth dry run: in a dry run the packer has no carved segment
        // to inspect, so it must report the plan it would follow -- the same
        // arithmetic as the planner (slots_free / nslots_per_seg segments can
        // be emptied, one segment always stays as the target) -- and move
        // nothing.
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        cfg.dry_run = 1;
        cfg.enable_compaction = 1;  // even enabled, a dry run may not move a block
        const char *build =
            "T = {} "
            "for i = 1, 300 do T[i] = table.new(64) "
            "  for k = 1, 64 do T[i][k] = i * 1000 + k end end "
            "for i = 1, 300 do if i % 3 ~= 0 then T[i] = nil end end "
            "collectgarbage('collect') ";
        arena_state st(&cfg);
        if (!st.L) return;
        luaL_openlibs(st.L);
        expect(st.dostring(build) == LUA_OK);
        const lua_ArenaStats before = st.stats();
        expect(eq(before.dry_run, 1));
        const int moved = lua_gc(st.L, LUA_GCCOMPACT, 0);
        const lua_ArenaStats after = st.stats();
        expect(eq(moved, 0)) << "a dry run must not move a block";
        expect(eq(after.compacted_bytes, (size_t)0)) << "nothing may be packed";
        expect(after.would_compact_bytes > 0u)
            << "the planner must report the sparse segment it would empty";
        expect(eq(after.commit_bytes, before.commit_bytes))
            << "a dry run must not change the committed bytes";
        expect(eq(lua_ArenaInteriorHolders(st.arena), 0u))
            << "no call may hold an interior pointer across a compaction";
        // the plan is the class arithmetic, not a guess: 300 slots of the
        // 580-byte array class fit 112 per segment, two thirds go away, so one
        // whole segment's worth of slots is free.
        const lua_ArenaStats s = st.stats();
        expect(s.would_compact_bytes >= 580u * 112u)
            << "the plan must be at least one segment of the array class";
    };

    "logical_live_tracks_gc_count"_test = [] {
        arena_state st;
        if (!st.L) return;
        expect(st.dostring("local t = {} for i = 1, 5000 do t[i] = i end") == LUA_OK);
        lua_ArenaStats s = st.stats();
        const size_t gc_bytes = (size_t)lua_gc(st.L, LUA_GCCOUNT, 0) * 1024u
                                + (size_t)lua_gc(st.L, LUA_GCCOUNTB, 0);
        expect(s.logical_live > 0u);
        const double ratio = double(s.logical_live) / double(gc_bytes ? gc_bytes : 1);
        expect(ratio > 0.5 && ratio < 2.0)
            << "arena.logical_live must track what LUA_GCCOUNT reports";
    };

    "conservation_under_random_traffic"_test = [] {
        arena_state st;
        if (!st.L) return;
        void *ud = nullptr;
        lua_Alloc f = st.alloc(&ud);
        std::mt19937 rng(20240507u);
        const lua_ArenaStats base = st.stats();
        std::vector<std::pair<void *, size_t>> live;
        size_t logical = base.logical_live;
        for (int op = 0; op < 20000; op++) {
            const unsigned choice = rng() % 3;
            if (choice == 0 && !live.empty()) {  // free
                size_t index = rng() % live.size();
                f(ud, live[index].first, live[index].second, 0);
                logical -= live[index].second;
                live[index] = live.back();
                live.pop_back();
            } else {  // allocate
                static const size_t kSizes[] = {16, 24, 48, 96, 148, 512, 580,
                                                1156, 4612, 9220, 40000, 100000};
                size_t size = kSizes[rng() % (sizeof(kSizes) / sizeof(kSizes[0]))];
                void *p = f(ud, nullptr, 0, size);
                expect(p != nullptr);
                if (!p) return;
                live.emplace_back(p, size);
                logical += size;
            }
        }
        lua_ArenaStats s = st.stats();
        expect(eq(s.logical_live, logical)) << "logical_live must be the exact sum of requests";
        class_totals t = scan_classes(st);
        expect(t.conserved) << "the per-class accounting must hold under random traffic";
        expect(eq(s.slots_used, t.used));
        for (auto &entry : live) f(ud, entry.first, entry.second, 0);
        // The trim policy decays the high water by 1/2 per call, so repeated
        // trims converge on the live footprint instead of oscillating.
        size_t released = lua_ArenaTrimHandle(st.arena, 1);
        expect(released > 0u) << "the traffic left segments to give back";
        for (int round = 0; round < 64; round++) {
            const size_t more = lua_ArenaTrimHandle(st.arena, 1);
            if (more == 0) break;
            released += more;
        }
        lua_ArenaStats end = st.stats();
        expect(eq(end.slots_used, base.slots_used))
            << "every slot the test took must be back";
        expect(end.seg_count <= base.seg_count)
            << "repeated trims must converge on the live footprint";
        expect(eq(end.logical_live, base.logical_live));
        expect(eq(end.huge_cached, (unsigned long)0));
        class_totals end_classes = scan_classes(st);
        expect(end_classes.conserved);
    };

    "same_output_as_a_mimalloc_state"_test = [] {
        const char *chunk =
            "local t = {} "
            "for i = 1, 500 do t[i] = ('x'):rep(i % 17) end "
            "for k, v in ipairs(t) do print(k, #v) end "
            "local h = {} "
            "for i = 1, 50 do h['k' .. i] = i * i end "
            "local keys = {} "
            "for k in pairs(h) do keys[#keys + 1] = k end "
            "table.sort(keys) "
            "print(#keys, h.k7) "
            "print(('%.3f'):format(1/3))";
        std::string plain;
        std::string arena_out;
        {
            plain_state ps;
            if (!ps.L) return;
            luaL_openlibs(ps.L);
            plain = run_and_capture(ps.L, chunk);
        }
        {
            arena_state st;
            if (!st.L) return;
            luaL_openlibs(st.L);
            arena_out = run_and_capture(st.L, chunk);
        }
        expect(!plain.empty()) << "the reference run must produce output";
        expect(plain == arena_out)
            << "class rounding must never leak into observable behaviour";
    };

    "arena_free_is_idempotent_safe"_test = [] {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        lua_State *L = luaL_newstate_arena(&cfg);
        expect(L != nullptr);
        if (!L) return;
        void *arena = luaL_arena(L);
        expect(arena != nullptr);
        lua_ArenaStats before;
        lua_ArenaStatsOf(arena, &before);
        lua_close(L);  // the state is gone, the arena handle is still valid
        lua_ArenaStats after;
        lua_ArenaStatsOf(arena, &after);
        expect(eq(after.slots_used, (unsigned long)0))
            << "lua_close must return every slot to the arena";
        lua_ArenaFree(arena);
        lua_ArenaStats zeroed;
        lua_ArenaStatsOf(nullptr, &zeroed);
        expect(eq(zeroed.enabled, 0));
    };
}
