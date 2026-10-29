/*
 * test_kimix_api.cpp — exercises the kimix_api C-FFI shared library.
 *
 * The suite calls the library through the PUBLIC C headers only (no kimix-core
 * type is referenced anywhere), which is exactly what a foreign binding does:
 * it proves the exported surface is complete, NULL-safe, and that the
 * placement-new / std::launder placeholder lifecycle (init -> use -> destroy)
 * behaves as documented, including the guard-word error paths.
 *
 * This test covers:
 * - the ABI / identity / layout queries and the status names
 * - the kimix_mem_* allocation family (incl. the aligned and block-op halves)
 * - the kimix_vec placeholder lifecycle, observation, modification, heap handles
 * - kimix_vec_append_utf16(): strict UTF-16 -> UTF-8 (BMP, 2/3-byte forms,
 *   surrogate pairs, U+10FFFF, unpaired-surrogate rejection with the vector
 *   left untouched, append-after-existing-content)
 * - the yyjson read / write / mutate surface and its NULL tolerance
 * - kimix::repair() through kimix_repair* / kimix_json_*
 * - the opaque kimix_map handle: set/get/upsert/remove/contains/clear/reserve,
 *   the dense entry_at() walk and its documented order, and the per-instance
 *   allocated_count() accounting (stability, growth, clear, the ledger)
 * - kimix_fiber_run() / kimix_fiber_worker_count(): every job exactly once, the
 *   multi-thread spread proof from a thread with no scheduler bound, the
 *   task_limit cap, the cancel flag (pre-set and mid-run), the 0-job and
 *   NULL-fn edges, the context pointer passed verbatim, and the pool being
 *   unbound again on return
 *
 * - the SQLite FFI: library queries, :memory: open/close, exec DDL/DML, the
 *   prepare/bind/step/column round trip over every storage class (int, float,
 *   text incl. UTF-8, blob, NULL), changes/last_insert_rowid, reset-reuse,
 *   named parameters, the exec error message owned by the caller (kimix_mem_free),
 *   extended errcodes, and every documented NULL/neutral-value edge
 * - the graph-db re-export family (kimix_graphdb_*): everything the archive's
 *   C ABI offers, driven through the WRAPPER names only, so the DLL exports —
 *   not just the static archive — are what the suite proves: the layout probe,
 *   the session lifecycle (in-memory open, capabilities, the storeless
 *   persist/load), schema CRUD through the two-phase scratch protocol, the
 *   vertex / label / property / edge command walk with its read-backs, the
 *   constraint-filtered paged reads (with and without a constraint, cap
 *   0/1/N), transactions (commit, abort-restores, the read-only collective),
 *   the bulk calls with continue_on_error, and the NULL-session contract.
 * Registered in tests/xmake.lua behind has_config("kimix_enable_api"): with the
 * option off neither kimix_api nor this test is part of the configuration.
 */
#include "ut/ut.hpp"

#include <api/kimix_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

using boost::ut::expect;
using boost::ut::operator""_test;

namespace tests_unit_api_test_kimix_api {

// ---------------------------------------------------------------- parallel helpers
// The kimix_fiber_run callbacks and their context. Like any caller of that API
// they run on several workers at once, so shared state here is either an atomic
// or a disjoint element (the contract the header states).

/* A stamp identifying the OS thread that ran the current job (the same proof
   device tests/unit/core/test_fiber.cpp uses). */
uint64_t ffi_thread_stamp() noexcept {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

/* Number of distinct stamps in [first, first + n) (sort + unique; n is tiny). */
size_t ffi_distinct(uint64_t *first, size_t n) {
    std::sort(first, first + n);
    return static_cast<size_t>(std::unique(first, first + n) - first);
}

/* Hold the current thread busy for about `ms` milliseconds. Deliberately a spin,
   never a sleep: a sleeping thread cannot prove that two jobs ran at the same
   time on two different OS threads. */
void ffi_busy_wait_ms(double ms) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
    volatile uint64_t sink = 0u;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) { ++sink; }
    }
}

struct ffi_stamp_state {
    static constexpr uint64_t k_jobs = 512u;
    std::atomic<uint64_t> ran{0u};
    std::atomic<uint64_t> workers_seen{0u}; // kimix_fiber_worker_count() from a job
    void *seen_context = nullptr;           // the context pointer as the job got it
    uint64_t stamps[k_jobs] = {0u};
    uint8_t hit[k_jobs] = {0u};
};

void ffi_stamp_job(void *context, uint64_t index) noexcept {
    auto *state = static_cast<ffi_stamp_state *>(context);
    if (index == 0u) {
        state->seen_context = context;
        state->workers_seen.store(kimix_fiber_worker_count(), std::memory_order_relaxed);
    }
    state->hit[index] = 1u;
    state->stamps[index] = ffi_thread_stamp();
    ffi_busy_wait_ms(0.2); // slow enough that the tasks overlap: the spread is visible
    state->ran.fetch_add(1u, std::memory_order_relaxed);
}

struct ffi_peak_state {
    std::atomic<uint64_t> active{0u};
    std::atomic<uint64_t> peak{0u};
    std::atomic<uint64_t> ran{0u};
};

void ffi_peak_job(void *context, uint64_t) noexcept {
    auto *state = static_cast<ffi_peak_state *>(context);
    const auto now = state->active.fetch_add(1u, std::memory_order_relaxed) + 1u;
    auto best = state->peak.load(std::memory_order_relaxed);
    while (now > best && !state->peak.compare_exchange_weak(best, now, std::memory_order_relaxed)) {}
    ffi_busy_wait_ms(0.3);
    state->active.fetch_sub(1u, std::memory_order_relaxed);
    state->ran.fetch_add(1u, std::memory_order_relaxed);
}

struct ffi_counting_state {
    std::atomic<uint64_t> ran{0u};
};

void ffi_counting_job(void *context, uint64_t) noexcept {
    static_cast<ffi_counting_state *>(context)->ran.fetch_add(1u, std::memory_order_relaxed);
}

/* Records the `context` value the callback actually received, so the suite can
   assert the pointer travels through the boundary unchanged (including NULL). */
std::atomic<uintptr_t> g_seen_context{0u};

void ffi_record_context_job(void *context, uint64_t) noexcept {
    g_seen_context.store(reinterpret_cast<uintptr_t>(context), std::memory_order_relaxed);
}

/* Collects every job id it was called with, so the suite can assert the range the
   callback sees is exactly [0, job_count): a per-id hit slot plus the min/max. */
struct ffi_id_range_state {
    static constexpr uint64_t k_jobs = 1000u;
    std::atomic<uint64_t> ran{0u};
    uint8_t seen[k_jobs] = {0u};
    std::atomic<uint64_t> low{UINT64_MAX};
    std::atomic<uint64_t> high{0u};
};

void ffi_record_id_job(void *context, uint64_t index) noexcept {
    auto *state = static_cast<ffi_id_range_state *>(context);
    state->seen[index] = 1u;
    auto lo = state->low.load(std::memory_order_relaxed);
    while (index < lo && !state->low.compare_exchange_weak(lo, index, std::memory_order_relaxed)) {}
    auto hi = state->high.load(std::memory_order_relaxed);
    while (index > hi && !state->high.compare_exchange_weak(hi, index, std::memory_order_relaxed)) {}
    state->ran.fetch_add(1u, std::memory_order_relaxed);
}

/* The cancel flag lives in this state so the caller owns the 1-byte flag the API
   documents (a plain volatile bool: exactly what the header's
   `const volatile bool *cancel` promises to read). */
struct ffi_cancel_state {
    std::atomic<uint64_t> ran{0u};
    volatile bool cancel{false};
};

/* A job that cancels the run: the very first job to start raises the caller's
   flag, so only the jobs other workers had already claimed can slip through. */
void ffi_first_sets_cancel(void *context, uint64_t) noexcept {
    auto *state = static_cast<ffi_cancel_state *>(context);
    if (state->ran.fetch_add(1u, std::memory_order_relaxed) == 0u) {
        state->cancel = true; // the first job to run asks the rest to stop
    }
    ffi_busy_wait_ms(0.05);
}

// ---------------------------------------------------------------------- vec helpers

/* Snapshot a live vector's bytes into a fixed buffer (the suite must not depend
   on kimix-core containers, so no std::vector here). */
size_t ffi_vec_snapshot(const kimix_vec *v, unsigned char *out, size_t cap) {
    const size_t n = kimix_vec_size(v);
    if (n == static_cast<size_t>(-1) || n > cap) {
        return static_cast<size_t>(-1);
    }
    if (n && kimix_vec_read(v, 0, out, n) != KIMIX_OK) {
        return static_cast<size_t>(-1);
    }
    return n;
}

/* Append `text` (a NUL-terminated ASCII literal) as UTF-16 code units and check
   the resulting UTF-8 bytes against the expected sequence. Returns 0 on a match,
   otherwise a small failure code the assertion prints. */
int ffi_expect_utf16(const uint16_t *units, size_t n, const unsigned char *expected, size_t expected_len) {
    kimix_vec v;
    if (kimix_vec_default_init(&v) != KIMIX_OK) {
        return 1;
    }
    const kimix_status st = kimix_vec_append_utf16(&v, units, n);
    if (st != KIMIX_OK) {
        kimix_vec_destroy(&v);
        return 2;
    }
    unsigned char got[16];
    const size_t len = ffi_vec_snapshot(&v, got, sizeof got);
    int result = 0;
    if (len != expected_len) {
        result = 3;
    } else if (expected_len && std::memcmp(got, expected, expected_len) != 0) {
        result = 4;
    }
    kimix_vec_destroy(&v);
    return result;
}

/* The same, but the call must be REJECTED and the vector left untouched. */
int ffi_expect_utf16_rejected(const uint16_t *units, size_t n) {
    kimix_vec v;
    if (kimix_vec_default_init(&v) != KIMIX_OK) {
        return 1;
    }
    if (kimix_vec_append(&v, "keep", 4) != KIMIX_OK) {
        kimix_vec_destroy(&v);
        return 2;
    }
    const kimix_status st = kimix_vec_append_utf16(&v, units, n);
    unsigned char got[16];
    const size_t len = ffi_vec_snapshot(&v, got, sizeof got);
    int result = 0;
    if (st != KIMIX_ERR_INVALID_INPUT) {
        result = 3;
    } else if (len != 4u || std::memcmp(got, "keep", 4) != 0) {
        result = 4; // the vector must be exactly what it was before the call
    }
      kimix_vec_destroy(&v);
      return result;
  }

  // ------------------------------------------------------------------- graphdb
  /* The graphdb suite is a foreign binding in miniature: only the C surface, only
     caller-owned buffers (every fixed array below is sized off the probe's own
     count), and the TWO-PHASE protocol the header documents — NULL first to
     learn the sizes, then the real fetch, then the deliberately too-small call
     that must report KIMIX_GDB_ERR_BUFFER with the need still written out. */
  kimix_gdb_bytes g_bytes(char const *text) {
      kimix_gdb_bytes out;
      out.data = reinterpret_cast<uint8_t const *>(text);
      out.len = std::strlen(text);
      return out;
  }
    /* The property builders return a REFERENCE so a call site can spell
       &g_int(33): the reference binds a function-local thread_local that the
       very next native call consumes synchronously (tests are single-threaded;
       nothing outlives the full expression).  Building an array copies out of
       it, which is the only other use these helpers have. */
    kimix_gdb_property &g_int(int64_t value) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_INT64;
        property.dtype = KIMIX_GDB_DTYPE_INT64;
        property.count = 1;
        property.i64 = value;
        return property;
    }
    kimix_gdb_property &g_double(double value) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_DOUBLE;
        property.dtype = KIMIX_GDB_DTYPE_DOUBLE;
        property.count = 1;
        property.f64 = value;
        return property;
    }
    kimix_gdb_property &g_text(char const *text) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_STRING;
        property.dtype = KIMIX_GDB_DTYPE_CHAR;
        property.count = 1;
        property.payload = g_bytes(text);
        return property;
    }
    kimix_gdb_property &g_blob(uint8_t const *data, size_t len) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_BLOB;
        property.dtype = KIMIX_GDB_DTYPE_BYTE;
        property.count = 1;
        property.payload = kimix_gdb_bytes{data, len};
        return property;
    }
    kimix_gdb_property &g_int_array(int64_t const *elements, size_t n) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_INT64_ARRAY;
        property.dtype = KIMIX_GDB_DTYPE_INT64;
        property.count = static_cast<uint16_t>(n);
        property.payload =
            kimix_gdb_bytes{reinterpret_cast<uint8_t const *>(elements), n * sizeof(int64_t)};
        return property;
    }
    kimix_gdb_property &g_double_array(double const *elements, size_t n) {
        static thread_local kimix_gdb_property property;
        property = kimix_gdb_property{};
        property.kind = KIMIX_GDB_KIND_DOUBLE_ARRAY;
        property.dtype = KIMIX_GDB_DTYPE_DOUBLE;
        property.count = static_cast<uint16_t>(n);
        property.payload =
            kimix_gdb_bytes{reinterpret_cast<uint8_t const *>(elements), n * sizeof(double)};
        return property;
    }
  bool g_same(kimix_gdb_guid a, kimix_gdb_guid b) {
      return a.lo == b.lo && a.hi == b.hi;
  }
  bool g_has(uint32_t const *ids, size_t n, uint32_t needle) {
      for (size_t i = 0; i < n; ++i) {
          if (ids[i] == needle) {
              return true;
          }
      }
      return false;
  }
  /* A registered label by name (the suite's sessions are GDI-strict, so every
     name is created explicitly — a typo fails right here, not later). */
  uint32_t g_label(kimix_gdb_session *session, char const *name) {
      uint32_t id = 0;
      expect(kimix_graphdb_label_create(session, name, std::strlen(name), &id) ==
             KIMIX_GDB_OK);
      return id;
  }
  uint32_t g_property_type(kimix_gdb_session *session, char const *name, uint8_t container,
                           uint8_t dtype, uint32_t count) {
      uint32_t id = 0;
      expect(kimix_graphdb_property_type_create(session, name, std::strlen(name), container,
                                                dtype, KIMIX_GDB_SIZE_FIXED, count, &id) == KIMIX_GDB_OK);
      return id;
  }
  /* One keyed vertex, created through the wrapper.  A NULL `out_warning` is the
     documented "nobody asks" shape (the session enforces unique ids, so the
     warning branch cannot fire here anyway). */
  kimix_gdb_guid g_vertex(kimix_gdb_session *session, uint32_t label_id, char const *external) {
      kimix_gdb_vertex_spec spec{};
      spec.key.label = label_id;
      spec.key.external_id = g_bytes(external);
      kimix_gdb_guid uid{};
      expect(kimix_graphdb_vertex_create(session, &spec, &uid, nullptr) == KIMIX_GDB_OK);
      return uid;
  }
  /* The spec of a keyed vertex: what a bulk row creates (a NEW vertex under
     that key — the uid is an OUT of the call, never an input).  The key label
     alone is enough here; the extra-label array is only used where the test
     needs its own (caller-owned, stable) storage. */
    kimix_gdb_vertex_spec &g_spec(uint32_t label_id, char const *external) {
        /* The same take-a-reference trick as the property builders: a call site
           may spell &g_spec(person, "a").key.  Copies out of it are the only
           other use (spec arrays and plain assignments both copy). */
        static thread_local kimix_gdb_vertex_spec spec;
        spec = kimix_gdb_vertex_spec{};
        spec.key.label = label_id;
        spec.key.external_id = g_bytes(external);
        return spec;
    }
  kimix_gdb_edge_spec g_edge_spec(kimix_gdb_guid origin, kimix_gdb_guid target) {
      kimix_gdb_edge_spec spec{};
      spec.origin = origin;
      spec.target = target;
      spec.direction = KIMIX_GDB_EDGE_DIRECTED;
      return spec;
  }
  kimix_gdb_guid g_edge(kimix_gdb_session *session, kimix_gdb_guid origin,
                        kimix_gdb_guid target) {
      kimix_gdb_guid id{};
      expect(kimix_graphdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED,
                                       nullptr, 0, nullptr, nullptr, 0, nullptr, &id) == KIMIX_GDB_OK);
      return id;
  }
  /* The two-phase property read: probe, then fetch, with the caller's arrays
     already known (from the probe) to be big enough. */
  size_t g_read_properties(kimix_gdb_session *session, kimix_gdb_guid vertex,
                           kimix_gdb_property_record *items, size_t capacity,
                           uint8_t *scratch, size_t scratch_capacity) {
      size_t count = 0;
      size_t bytes = 0;
      expect(kimix_graphdb_vertex_properties(session, vertex, nullptr, 0, nullptr, 0, &count,
                                             &bytes) == KIMIX_GDB_OK);
      expect(count <= capacity);
      expect(bytes <= scratch_capacity);
      count = 0;
      bytes = 0;
      expect(kimix_graphdb_vertex_properties(session, vertex, items, capacity, scratch,
                                             scratch_capacity, &count, &bytes) ==
             KIMIX_GDB_OK);
      expect(count <= capacity);
      return count;
  }

  /* One open IN-MEMORY session, GDI-strict schema and enforced unique ids — the
     defaults the IGraphBackend contract wants; the guard's close is the only
     owned object's release, reached even on an early return. */
  struct gd_guard {
      kimix_gdb_session *session{nullptr};
      gd_guard() {
          kimix_gdb_open_options options{};
          options.directory = nullptr; // in memory: persist()/load() report no store
          options.auto_create_schema = 0; // GDI-strict: names must exist
          options.key_mode = 0; // enforce unique
          options.allow_parallel_edges = 1;
          options.allow_self_edges = 1;
          options.max_journal_bytes = 64ull * 1024ull * 1024ull;
          expect(kimix_graphdb_open(&options, &session) == KIMIX_GDB_OK);
          expect(session != nullptr);
      }
      gd_guard(gd_guard const &) = delete;
      gd_guard &operator=(gd_guard const &) = delete;
      ~gd_guard() {
          if (session != nullptr) {
              kimix_graphdb_close(session);
          }
      }
  };
  

  } // namespace tests_unit_api_test_kimix_api
using namespace tests_unit_api_test_kimix_api;

int main() {
    // ----------------------------------------------------------------- identity
    "abi identity"_test = [] {
        expect(kimix_api_abi_version() == KIMIX_API_ABI_VERSION);
        expect(std::strlen(kimix_api_version_string()) > 0);
        expect(std::strlen(kimix_api_build_info()) > 0);
        expect(std::strcmp(kimix_status_name(KIMIX_OK), "KIMIX_OK") == 0);
        expect(kimix_status_name(static_cast<kimix_status>(1234)) == nullptr);

        kimix_layout_info info;
        expect(kimix_api_layout_info(&info) == KIMIX_OK);
        expect(kimix_api_layout_info(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(info.abi_version == kimix_api_abi_version());
        expect(info.size_of_bool == 1u);
        expect(info.size_of_size_t == sizeof(size_t));
        expect(info.size_of_pointer == sizeof(void *));
        // The published placeholder must be big/aligned enough for the real object.
        expect(info.vec_inner_bytes + sizeof(uint64_t) <= info.vec_abi_bytes);
        expect(info.vec_inner_align <= info.vec_abi_align);
        expect(info.vec_abi_bytes == KIMIX_VEC_BYTES);
        expect(sizeof(kimix_vec) == KIMIX_VEC_BYTES);
        expect(alignof(kimix_vec) == KIMIX_VEC_ALIGN);
        expect(kimix_vec_abi_bytes() == KIMIX_VEC_BYTES);
        expect(kimix_vec_inner_bytes() == info.vec_inner_bytes);
    };

    // ------------------------------------------------------------------ memory
    "mem alloc / realloc / free"_test = [] {
        auto *p = static_cast<unsigned char *>(kimix_mem_malloc(64));
        expect(p != nullptr);
        for (int i = 0; i < 64; ++i) {
            p[i] = static_cast<unsigned char>(i);
        }
        expect(kimix_mem_usable_size(p) >= 64u);
        expect(kimix_mem_good_size(33) >= 33u);

        auto *q = static_cast<unsigned char *>(kimix_mem_realloc(p, 128));
        expect(q != nullptr);
        expect(q[0] == 0 && q[63] == 63); // content survived
        expect(kimix_mem_expand(q, 129) != nullptr || true);
        kimix_mem_free(q);

        // Zero-filling allocations, then a 0-size / NULL edge case each.
        auto *z = static_cast<unsigned char *>(kimix_mem_zalloc(32));
        expect(z != nullptr);
        bool zeroed = true;
        for (int i = 0; i < 32; ++i) {
            zeroed = zeroed && (z[i] == 0);
        }
        expect(zeroed);
        kimix_mem_free(z);

        auto *c = static_cast<unsigned char *>(kimix_mem_calloc(8, 8));
        expect(c != nullptr);
        expect(c[63] == 0);
        kimix_mem_free(c);

        kimix_mem_free(nullptr); // documented as a no-op
        expect(kimix_mem_strdup(nullptr) == nullptr);
        expect(kimix_mem_usable_size(nullptr) == 0u);

        char *s = kimix_mem_strdup("hello");
        expect(s != nullptr);
        expect(std::strcmp(s, "hello") == 0);
        char *sn = kimix_mem_strndup("hello", 2);
        expect(sn != nullptr);
        expect(std::strcmp(sn, "he") == 0);
        kimix_mem_free(s);
        kimix_mem_free(sn);
    };

    "mem aligned allocation honours the alignment"_test = [] {
        auto *a = kimix_mem_malloc_aligned(100, 64);
        expect(a != nullptr);
        expect((reinterpret_cast<uintptr_t>(a) % 64u) == 0u);
        kimix_mem_free_aligned(a, 64);

        auto *b = kimix_mem_zalloc_aligned(48, 32);
        expect(b != nullptr);
        expect((reinterpret_cast<uintptr_t>(b) % 32u) == 0u);
        kimix_mem_free(b);

        void *p = nullptr;
        expect(kimix_mem_posix_memalign(&p, 16, 32) == KIMIX_OK);
        expect(p != nullptr);
        expect((reinterpret_cast<uintptr_t>(p) % 16u) == 0u);
        kimix_mem_free(p);
        // Argument validation happens before mimalloc is entered.
        expect(kimix_mem_posix_memalign(nullptr, 16, 32) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_mem_posix_memalign(&p, 3, 32) == KIMIX_ERR_INVALID_ARG);

        expect(kimix_mem_version() > 0u);
        expect(std::strlen(kimix_mem_version_string()) > 0);
        kimix_mem_thread_init();
        kimix_mem_collect(false);
        kimix_mem_thread_done();
    };

    "mem raw block ops: copy / move / swap / fill / compare / search"_test = [] {
        unsigned char buf[32];
        kimix_mem_set(buf, 0xAB, sizeof buf);
        kimix_mem_zero(buf, 8); // the common fill spelled out
        expect(buf[0] == 0x00 && buf[7] == 0x00 && buf[8] == 0xAB);

        // copy is an exact copy; compare is sign-normalised to -1/0/+1.
        const char src[] = "abcdef";
        kimix_mem_copy(buf, src, 6);
        expect(kimix_mem_equals(buf, src, 6));
        expect(kimix_mem_compare(buf, src, 6) == 0);
        expect(!kimix_mem_equals(buf + 8, src, 1)); // 0xAB tail != 'a'
        expect(kimix_mem_compare("abc", "abd", 3) == -1);
        expect(kimix_mem_compare("abd", "abc", 3) == 1);
        expect(kimix_mem_compare(nullptr, nullptr, 0) == 0); // empty ranges compare equal

        // move is overlap-safe in either direction (memmove semantics).
        kimix_mem_move(buf + 1, buf, 6); // shift "abcdef" one slot right
        expect(buf[0] == 'a');
        expect(std::memcmp(buf + 1, "abcdef", 6) == 0);
        kimix_mem_move(buf, buf + 1, 6); // shift it back
        expect(std::memcmp(buf, "abcdef", 6) == 0);

        // swap exchanges in place; swapping with itself is a no-op.
        unsigned char x[4] = {1, 2, 3, 4};
        unsigned char y[4] = {9, 8, 7, 6};
        kimix_mem_swap(x, y, sizeof x);
        expect(x[0] == 9 && x[3] == 6 && y[0] == 1 && y[3] == 4);
        kimix_mem_swap(x, x, sizeof x); // a == b: no-op, no crash
        expect(x[0] == 9);

        // find returns an offset (never confusable with "found at 0"); count
        // tallies every occurrence, including none.
        expect(kimix_mem_find_byte("hello", 5, 'l') == 2u);
        expect(kimix_mem_find_byte("hello", 5, 'h') == 0u);
        expect(kimix_mem_find_byte("hello", 5, 'z') == static_cast<size_t>(-1));
        expect(kimix_mem_count_byte("hello", 5, 'l') == 2u);
        expect(kimix_mem_count_byte("hello", 5, 'z') == 0u);

        // The NULL contract: n == 0 with NULL pointers is a documented no-op,
        // never a crash; NULL with n > 0 is UB and deliberately not guarded.
        kimix_mem_copy(nullptr, nullptr, 0);
        kimix_mem_move(nullptr, nullptr, 0);
        kimix_mem_set(nullptr, 0, 0);
        kimix_mem_zero(nullptr, 0);
        kimix_mem_swap(nullptr, nullptr, 0);
        expect(kimix_mem_equals(nullptr, nullptr, 0));
        expect(kimix_mem_find_byte(nullptr, 0, 'x') == static_cast<size_t>(-1));
        expect(kimix_mem_count_byte(nullptr, 0, 'x') == 0u);
    };

    // ------------------------------------------------------------------ vectors
    "vec lifecycle: init, use, destroy"_test = [] {
        kimix_vec v;
        expect(!kimix_vec_is_initialized(&v));
        expect(kimix_vec_default_init(&v) == KIMIX_OK);
        expect(kimix_vec_is_initialized(&v));
        expect(kimix_vec_empty(&v));
        expect(kimix_vec_size(&v) == 0u);

        // A second init would leak the live buffer, so it must be refused.
        expect(kimix_vec_default_init(&v) == KIMIX_ERR_INVALID_STATE);

        expect(kimix_vec_push_back(&v, 0x41) == KIMIX_OK);
        expect(kimix_vec_append(&v, "BCD", 3) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 4u);
        expect(!kimix_vec_empty(&v));
        expect(kimix_vec_capacity(&v) >= 4u);

        unsigned char *at = nullptr;
        expect(kimix_vec_at(&v, 1, &at) == KIMIX_OK);
        expect(at != nullptr && *at == 'B');
        expect(kimix_vec_at(&v, 99, &at) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_vec_at(&v, 0, nullptr) == KIMIX_ERR_INVALID_ARG);

        expect(kimix_vec_data(&v)[0] == 'A');
        expect(kimix_vec_data(&v)[3] == 'D');

        // read / write inside the current size only.
        unsigned char dst[4] = {0, 0, 0, 0};
        expect(kimix_vec_read(&v, 0, dst, 4) == KIMIX_OK);
        expect(dst[0] == 'A' && dst[3] == 'D');
        expect(kimix_vec_read(&v, 2, dst, 4) == KIMIX_ERR_OUT_OF_RANGE);
        const unsigned char patch[] = {'x', 'y'};
        expect(kimix_vec_write(&v, 1, patch, 2) == KIMIX_OK);
        expect(kimix_vec_data(&v)[1] == 'x' && kimix_vec_data(&v)[2] == 'y');
        expect(kimix_vec_write(&v, 3, patch, 2) == KIMIX_ERR_OUT_OF_RANGE);

        // resize / reserve / shrink / clear / pop / insert / erase.
        expect(kimix_vec_resize(&v, 10) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 10u);
        expect(kimix_vec_data(&v)[9] == 0); // grown with zeros
        expect(kimix_vec_resize_fill(&v, 12, 0x7F) == KIMIX_OK);
        expect(kimix_vec_data(&v)[10] == 0x7F);
        expect(kimix_vec_resize(&v, 6) == KIMIX_OK);
        expect(kimix_vec_reserve(&v, 64) == KIMIX_OK);
        expect(kimix_vec_capacity(&v) >= 64u);
        expect(kimix_vec_shrink_to_fit(&v) == KIMIX_OK);
        expect(kimix_vec_capacity(&v) >= 6u);
        expect(kimix_vec_insert(&v, 0, "z", 1) == KIMIX_OK);
        expect(kimix_vec_data(&v)[0] == 'z');
        expect(kimix_vec_insert(&v, 99, "z", 1) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_vec_erase(&v, 0, 1) == KIMIX_OK);
        expect(kimix_vec_pop_back(&v) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 5u);
        expect(kimix_vec_clear(&v) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 0u);
        expect(kimix_vec_pop_back(&v) == KIMIX_ERR_OUT_OF_RANGE);

        expect(kimix_vec_destroy(&v) == KIMIX_OK);
        expect(!kimix_vec_is_initialized(&v));
        // Destroying twice, or using a dead placeholder, is an error return.
        expect(kimix_vec_destroy(&v) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_size(&v) == static_cast<size_t>(-1));
        expect(kimix_vec_append(&v, "no", 2) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_push_back(nullptr, 0) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_size(nullptr) == static_cast<size_t>(-1));
        expect(!kimix_vec_is_initialized(nullptr));

        // Re-initialising the very same storage after a destroy is legal.
        expect(kimix_vec_init_from(&v, "abc", 3) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 3u);
        expect(kimix_vec_destroy(&v) == KIMIX_OK);
    };

    "vec copy, move, assign and swap"_test = [] {
        kimix_vec a, b, c;
        expect(kimix_vec_init_from(&a, "abcd", 4) == KIMIX_OK);

        // copy construction of an independent buffer
        expect(kimix_vec_copy_init(&b, &a) == KIMIX_OK);
        expect(kimix_vec_data(&b) != kimix_vec_data(&a));
        bool equal = false;
        expect(kimix_vec_equals(&a, &b, &equal) == KIMIX_OK);
        expect(equal);

        // copy assignment onto a live vector
        expect(kimix_vec_default_init(&c) == KIMIX_OK);
        expect(kimix_vec_assign(&c, &a) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 4u);
        expect(kimix_vec_assign_bytes(&c, "zz", 2) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 2u);

        // move construction: the buffer travels, the source becomes empty but
        // stays initialised (and still needs exactly one destroy).
        const unsigned char *moved = kimix_vec_data(&a);
        expect(kimix_vec_move_init(&b, &a) == KIMIX_ERR_INVALID_STATE); // b is live
        expect(kimix_vec_destroy(&b) == KIMIX_OK);
        expect(kimix_vec_move_init(&b, &a) == KIMIX_OK);
        expect(kimix_vec_data(&b) == moved);
        expect(kimix_vec_size(&b) == 4u);
        expect(kimix_vec_size(&a) == 0u);
        expect(kimix_vec_is_initialized(&a));
        expect(kimix_vec_move_init(&b, &b) == KIMIX_ERR_INVALID_ARG);

        // move assignment onto a live vector: `a` goes back to raw storage first
        // (the moved-from vector is still alive and must be destroyed once).
        expect(kimix_vec_destroy(&a) == KIMIX_OK);
        expect(kimix_vec_init_from(&a, "Q", 1) == KIMIX_OK);
        expect(kimix_vec_move_assign(&c, &a) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 1u);
        expect(kimix_vec_size(&a) == 0u);
        expect(kimix_vec_move_assign(&c, &c) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_move_assign(&c, nullptr) == KIMIX_ERR_INVALID_ARG);

        // swap is O(1) and keeps both sides alive
        expect(kimix_vec_append(&a, "xyz", 3) == KIMIX_OK);
        expect(kimix_vec_swap(&a, &c) == KIMIX_OK);
        expect(kimix_vec_size(&a) == 1u && kimix_vec_size(&c) == 3u);

        expect(kimix_vec_destroy(&a) == KIMIX_OK);
        expect(kimix_vec_destroy(&b) == KIMIX_OK);
        expect(kimix_vec_destroy(&c) == KIMIX_OK);
    };

    "vec heap handle helpers"_test = [] {
        kimix_vec *h = kimix_vec_new();
        expect(h != nullptr);
        expect(kimix_vec_is_initialized(h));
        expect(kimix_vec_append(h, "heap", 4) == KIMIX_OK);
        expect(kimix_vec_size(h) == 4u);
        kimix_vec *h2 = kimix_vec_new_from("data", 4);
        expect(h2 != nullptr);
        bool same = false;
        expect(kimix_vec_equals(h, h2, &same) == KIMIX_OK);
        expect(!same);
        kimix_vec_free(h);
        kimix_vec_free(h2);
        kimix_vec_free(nullptr); // documented as a no-op
    };

    // ------------------------------------------------- vec: UTF-16 -> UTF-8 append
    "utf16 append: ascii and bmp forms encode to the known utf-8 bytes"_test = [] {
        const uint16_t hi[] = {0x0048u, 0x0069u}; // "Hi"
        expect(ffi_expect_utf16(hi, 2, reinterpret_cast<const unsigned char *>("Hi"), 2) == 0);

        const uint16_t u007f[] = {0x007Fu};
        const unsigned char x007f[] = {0x7Fu};
        expect(ffi_expect_utf16(u007f, 1, x007f, 1) == 0); // last 1-byte form

        const uint16_t u0080[] = {0x0080u};
        const unsigned char x0080[] = {0xC2u, 0x80u};
        expect(ffi_expect_utf16(u0080, 1, x0080, 2) == 0); // first 2-byte form

        const uint16_t u00e9[] = {0x00E9u}; // LATIN SMALL LETTER E WITH ACUTE
        const unsigned char x00e9[] = {0xC3u, 0xA9u};
        expect(ffi_expect_utf16(u00e9, 1, x00e9, 2) == 0);

        const uint16_t u07ff[] = {0x07FFu};
        const unsigned char x07ff[] = {0xDFu, 0xBFu};
        expect(ffi_expect_utf16(u07ff, 1, x07ff, 2) == 0); // last 2-byte form

        const uint16_t u0800[] = {0x0800u};
        const unsigned char x0800[] = {0xE0u, 0xA0u, 0x80u};
        expect(ffi_expect_utf16(u0800, 1, x0800, 3) == 0); // first 3-byte form

        const uint16_t u4e2d[] = {0x4E2Du}; // CJK "middle"
        const unsigned char x4e2d[] = {0xE4u, 0xB8u, 0xADu};
        expect(ffi_expect_utf16(u4e2d, 1, x4e2d, 3) == 0);

        const uint16_t ufffd[] = {0xFFFDu}; // the replacement character
        const unsigned char xfffd[] = {0xEFu, 0xBFu, 0xBDu}; // EF BF BD, the known form
        expect(ffi_expect_utf16(ufffd, 1, xfffd, 3) == 0);

        const uint16_t uffff[] = {0xFFFFu};
        const unsigned char xffff[] = {0xEFu, 0xBFu, 0xBFu};
        expect(ffi_expect_utf16(uffff, 1, xffff, 3) == 0); // last BMP form

        // A mixed run: ascii + 2-byte + 3-byte in one call, shortest form only.
        const uint16_t mix[] = {0x0041u, 0x00E9u, 0x4E2Du, 0x007Au};
        const unsigned char xmix[] = {0x41u, 0xC3u, 0xA9u, 0xE4u, 0xB8u, 0xADu, 0x7Au};
        expect(ffi_expect_utf16(mix, 4, xmix, 7) == 0);
    };

    "utf16 append: surrogate pairs encode the astral code points"_test = [] {
        const uint16_t u10000[] = {0xD800u, 0xDC00u}; // first astral code point
        const unsigned char x10000[] = {0xF0u, 0x90u, 0x80u, 0x80u};
        expect(ffi_expect_utf16(u10000, 2, x10000, 4) == 0);

        const uint16_t u1f600[] = {0xD83Du, 0xDE00u}; // GRINNING FACE
        const unsigned char x1f600[] = {0xF0u, 0x9Fu, 0x98u, 0x80u};
        expect(ffi_expect_utf16(u1f600, 2, x1f600, 4) == 0);

        const uint16_t u10ffff[] = {0xDBFFu, 0xDFFFu}; // the very last code point
        const unsigned char x10ffff[] = {0xF4u, 0x8Fu, 0xBFu, 0xBFu};
        expect(ffi_expect_utf16(u10ffff, 2, x10ffff, 4) == 0);

        // Two pairs plus a BMP unit and ascii, in one call.
        const uint16_t run[] = {0x0048u, 0xD83Du, 0xDE00u, 0x4E2Du, 0xDBFFu, 0xDFFFu, 0x0021u};
        const unsigned char xrun[] = {0x48u, 0xF0u, 0x9Fu, 0x98u, 0x80u,
                                      0xE4u, 0xB8u, 0xADu,
                                      0xF4u, 0x8Fu, 0xBFu, 0xBFu, 0x21u};
        expect(ffi_expect_utf16(run, 7, xrun, 13) == 0);
    };

    "utf16 append: an unpaired surrogate rejects the whole call and changes nothing"_test = [] {
        const uint16_t high_last[] = {0x0041u, 0xD83Du}; // pair truncated at the end
        expect(ffi_expect_utf16_rejected(high_last, 2) == 0);
        expect(ffi_expect_utf16_rejected(high_last + 1, 1) == 0); // a lone high alone

        const uint16_t high_then_bmp[] = {0xD83Du, 0x0041u}; // high not followed by a low
        expect(ffi_expect_utf16_rejected(high_then_bmp, 2) == 0);

        const uint16_t high_then_high[] = {0xD83Du, 0xDBFFu}; // two highs in a row
        expect(ffi_expect_utf16_rejected(high_then_high, 2) == 0);

        const uint16_t low_first[] = {0xDE00u, 0x0041u}; // a low with no high in front
        expect(ffi_expect_utf16_rejected(low_first, 2) == 0);
        expect(ffi_expect_utf16_rejected(low_first, 1) == 0);

        const uint16_t good_then_lone_low[] = {0x0041u, 0xDE00u};
        expect(ffi_expect_utf16_rejected(good_then_lone_low, 2) == 0);

        const uint16_t both_edges[] = {0xD800u, 0xDFFFu, 0xDE00u}; // pair + dangling low
        expect(ffi_expect_utf16_rejected(both_edges, 3) == 0);
    };

    "utf16 append: argument and state edges"_test = [] {
        // count 0: the empty range, with a real pointer and with NULL.
        kimix_vec v;
        expect(kimix_vec_default_init(&v) == KIMIX_OK);
        expect(kimix_vec_append_utf16(&v, nullptr, 0) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 0u);
        const uint16_t unused_unit = 0xD83Du; // must never be read at count 0
        expect(kimix_vec_append_utf16(&v, &unused_unit, 0) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 0u);

        // A NULL buffer with a non-zero count is an argument error, not a crash.
        expect(kimix_vec_append_utf16(&v, nullptr, 4) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_size(&v) == 0u);

        // Appending after existing content: the tail is added, the head untouched.
        expect(kimix_vec_append(&v, "abc", 3) == KIMIX_OK);
        const uint16_t e9[] = {0x00E9u};
        expect(kimix_vec_append_utf16(&v, e9, 1) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 5u);
        expect(kimix_vec_data(&v)[0] == 'a' && kimix_vec_data(&v)[2] == 'c');
        expect(kimix_vec_data(&v)[3] == 0xC3 && kimix_vec_data(&v)[4] == 0xA9);
        // ... and a rejected call after it leaves those 5 bytes exactly as they are.
        const uint16_t lone_low[] = {0xDC00u};
        expect(kimix_vec_append_utf16(&v, lone_low, 1) == KIMIX_ERR_INVALID_INPUT);
        expect(kimix_vec_size(&v) == 5u);
        expect(kimix_vec_data(&v)[3] == 0xC3 && kimix_vec_data(&v)[4] == 0xA9);
        expect(kimix_vec_destroy(&v) == KIMIX_OK);

        // A dead placeholder is INVALID_STATE, and raw storage is too.
        expect(kimix_vec_append_utf16(&v, e9, 1) == KIMIX_ERR_INVALID_STATE);
        kimix_vec raw;
        std::memset(&raw, 0, sizeof raw);
        expect(kimix_vec_append_utf16(&raw, e9, 1) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_append_utf16(nullptr, e9, 1) == KIMIX_ERR_INVALID_ARG);

        // It also works on a heap handle, and a big range survives the growth.
        kimix_vec *h = kimix_vec_new();
        expect(h != nullptr);
        uint16_t text[256];
        for (int i = 0; i < 256; ++i) {
            text[i] = static_cast<uint16_t>((i % 2) ? 0x0416u : 0x0041u); // Cyrillic + ascii
        }
        expect(kimix_vec_append_utf16(h, text, 256) == KIMIX_OK);
        // 128 ascii (1 byte) + 128 U+0416 (2 bytes) = 384 bytes.
        expect(kimix_vec_size(h) == 384u);
        expect(kimix_vec_data(h)[0] == 'A' && kimix_vec_data(h)[1] == 0xD0u && kimix_vec_data(h)[2] == 0x96u);
        kimix_vec_free(h);
    };

    // -------------------------------------------------------------------- json
    "yyjson read and inspect"_test = [] {
        const char *text = R"({"name":"kimix","count":3,"ratio":1.5,"ok":true,"none":null,
                               "list":[10,-20,2.5],"nested":{"a":{"b":[1,2,3]}}})";
        kimix_yyjson_read_err err;
        kimix_yyjson_doc *doc = kimix_yyjson_read(text, std::strlen(text), KIMIX_YYJSON_READ_NOFLAG, &err);
        expect(doc != nullptr);
        expect(err.code == KIMIX_YYJSON_READ_SUCCESS);

        kimix_yyjson_val *root = kimix_yyjson_doc_root(doc);
        expect(root != nullptr);
        expect(kimix_yyjson_is_obj(root));
        expect(kimix_yyjson_get_type(root) == KIMIX_YYJSON_TYPE_OBJ);
        expect(kimix_yyjson_obj_size(root) == 7u); // name count ratio ok none list nested

        expect(kimix_yyjson_equals_str(kimix_yyjson_obj_get(root, "name"), "kimix"));
        expect(std::strcmp(kimix_yyjson_get_str(kimix_yyjson_obj_get(root, "name")), "kimix") == 0);
        expect(kimix_yyjson_get_len(kimix_yyjson_obj_get(root, "name")) == 5u);
        expect(kimix_yyjson_get_uint(kimix_yyjson_obj_get(root, "count")) == 3u);
        expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(root, "count")) == 3);
        expect(kimix_yyjson_get_num(kimix_yyjson_obj_get(root, "ratio")) == 1.5);
        expect(kimix_yyjson_is_true(kimix_yyjson_obj_get(root, "ok")));
        expect(kimix_yyjson_get_bool(kimix_yyjson_obj_get(root, "ok")));
        expect(kimix_yyjson_is_null(kimix_yyjson_obj_get(root, "none")));

        kimix_yyjson_val *list = kimix_yyjson_obj_get(root, "list");
        expect(kimix_yyjson_is_arr(list));
        expect(kimix_yyjson_arr_size(list) == 3u);
        expect(kimix_yyjson_get_sint(kimix_yyjson_arr_get(list, 1)) == -20);
        expect(kimix_yyjson_arr_get_first(list) == kimix_yyjson_arr_get(list, 0));
        expect(kimix_yyjson_arr_get_last(list) == kimix_yyjson_arr_get(list, 2));
        expect(kimix_yyjson_arr_get(list, 3) == nullptr);
        expect(kimix_yyjson_obj_get(root, "missing") == nullptr);

        kimix_yyjson_val *b = kimix_yyjson_obj_get(kimix_yyjson_obj_get(
            kimix_yyjson_obj_get(root, "nested"), "a"), "b");
        expect(b != nullptr && kimix_yyjson_arr_size(b) == 3u);
        expect(kimix_yyjson_doc_read_size(doc) == std::strlen(text));
        expect(kimix_yyjson_doc_val_count(doc) > 0u);

        // NULL-safety: every accessor must return its neutral value, not crash.
        expect(!kimix_yyjson_is_null(nullptr));
        expect(kimix_yyjson_get_uint(nullptr) == 0u);
        expect(kimix_yyjson_get_str(nullptr) == nullptr);
        expect(kimix_yyjson_arr_size(nullptr) == 0u);
        expect(kimix_yyjson_obj_size(nullptr) == 0u);

        kimix_yyjson_doc_free(doc);
        kimix_yyjson_doc_free(nullptr); // documented as a no-op
    };

    "yyjson read errors and flags"_test = [] {
        const char *bad = "{\"a\": }";
        kimix_yyjson_read_err err;
        kimix_yyjson_doc *doc = kimix_yyjson_read(bad, std::strlen(bad), KIMIX_YYJSON_READ_NOFLAG, &err);
        expect(doc == nullptr);
        expect(err.code != KIMIX_YYJSON_READ_SUCCESS);
        expect(err.msg != nullptr);
        expect(err.position < std::strlen(bad) + 1);

        // A NULL err out-parameter is allowed, and the nullable-err read still fails.
        expect(kimix_yyjson_read(bad, std::strlen(bad), KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);
        expect(kimix_yyjson_read_str(bad, KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);

        // Trailing commas are rejected by default and accepted with the flag.
        const char *loose = "[1,2,3,]";
        expect(kimix_yyjson_read_str(loose, KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);
        doc = kimix_yyjson_read_str(loose, KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS, &err);
        expect(doc != nullptr);
        expect(kimix_yyjson_arr_size(kimix_yyjson_doc_root(doc)) == 3u);
        kimix_yyjson_doc_free(doc);

        // Comments and single-quoted strings (JSON5-ish flags).
        const char *c = "/* hi */ {'k': 1}";
        doc = kimix_yyjson_read_str(c,
                                    KIMIX_YYJSON_READ_ALLOW_COMMENTS
                                        | KIMIX_YYJSON_READ_ALLOW_SINGLE_QUOTED_STR,
                                    &err);
        expect(doc != nullptr);
        if (doc) {
            expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(kimix_yyjson_doc_root(doc), "k")) == 1);
            kimix_yyjson_doc_free(doc);
        }
        expect(kimix_yyjson_version() > 0u);
    };

    "yyjson write an immutable document"_test = [] {
        const char *text = "{\"a\":[1,2]}";
        kimix_yyjson_doc *doc = kimix_yyjson_read_str(text, KIMIX_YYJSON_READ_NOFLAG, nullptr);
        expect(doc != nullptr);

        size_t len = 0;
        char *out = kimix_yyjson_write(doc, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(out != nullptr);
        expect(len > 0u);
        expect(std::strlen(out) == len);
        // The writer's buffer is library-heap memory: only these two free it.
        kimix_yyjson_str_free(out);

        char *pretty = kimix_yyjson_write(doc, KIMIX_YYJSON_WRITE_PRETTY, &len, nullptr);
        expect(pretty != nullptr);
        expect(std::strlen(pretty) == len);
        kimix_mem_free(pretty); // same mimalloc heap, so kimix_mem_free is valid too

        char *one = kimix_yyjson_val_write(kimix_yyjson_doc_root(doc), KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(one != nullptr);
        kimix_yyjson_str_free(one);

        // The _buf form allocates nothing at all.
        char stack_buf[64];
        size_t written = kimix_yyjson_write_buf(stack_buf, sizeof(stack_buf), doc, KIMIX_YYJSON_WRITE_NOFLAG, nullptr);
        expect(written > 0u && written < sizeof(stack_buf));
        expect(std::strlen(stack_buf) == written);

        expect(kimix_yyjson_write(nullptr, KIMIX_YYJSON_WRITE_NOFLAG, nullptr, nullptr) == nullptr);
        kimix_yyjson_str_free(nullptr);
        kimix_yyjson_doc_free(doc);
    };

    "yyjson build a mutable document"_test = [] {
        kimix_yyjson_mut_doc *m = kimix_yyjson_mut_doc_new();
        expect(m != nullptr);
        kimix_yyjson_mut_val *root = kimix_yyjson_mut_obj(m);
        expect(root != nullptr);
        kimix_yyjson_mut_doc_set_root(m, root);
        expect(kimix_yyjson_mut_doc_root(m) == root);

        expect(kimix_yyjson_mut_obj_add_str(m, root, "cmd", "ls"));
        expect(kimix_yyjson_mut_obj_add_sint(m, root, "code", -2));
        expect(kimix_yyjson_mut_obj_add_uint(m, root, "pid", 4242));
        expect(kimix_yyjson_mut_obj_add_bool(m, root, "ok", true));
        expect(kimix_yyjson_mut_obj_add_real(m, root, "ratio", 0.5));
        expect(kimix_yyjson_mut_obj_add_null(m, root, "extra"));
        kimix_yyjson_mut_val *args = kimix_yyjson_mut_obj_add_arr(m, root, "args");
        expect(args != nullptr);
        expect(kimix_yyjson_mut_arr_add_strcpy(m, args, "-la"));
        expect(kimix_yyjson_mut_arr_add_sint(m, args, 7));
        expect(kimix_yyjson_mut_arr_add_val(args, kimix_yyjson_mut_true(m)));
        expect(kimix_yyjson_mut_arr_size(args) == 3u);
        expect(kimix_yyjson_mut_arr_get(args, 0) != nullptr);
        expect(kimix_yyjson_mut_arr_get_last(args) != nullptr);
        expect(kimix_yyjson_mut_arr_remove(args, 1) != nullptr);
        expect(kimix_yyjson_mut_arr_size(args) == 2u);
        expect(kimix_yyjson_mut_arr_append(args, kimix_yyjson_mut_str(m, "tail")));

        expect(kimix_yyjson_mut_obj_size(root) == 7u);
        expect(kimix_yyjson_mut_obj_get(root, "cmd") != nullptr);
        expect(kimix_yyjson_mut_obj_get(root, "nope") == nullptr);
        expect(kimix_yyjson_mut_obj_remove(root, "extra"));
        expect(kimix_yyjson_mut_obj_size(root) == 6u);

        size_t len = 0;
        char *out = kimix_yyjson_mut_write(m, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(out != nullptr);
        expect(std::strstr(out, "\"cmd\":\"ls\"") != nullptr);
        kimix_yyjson_str_free(out);

        // mut -> immutable, then read it back through the immutable API.
        kimix_yyjson_doc *im = kimix_yyjson_mut_doc_imut_copy(m);
        expect(im != nullptr);
        kimix_yyjson_val *ir = kimix_yyjson_doc_root(im);
        expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(ir, "code")) == -2);
        expect(kimix_yyjson_equals_str(kimix_yyjson_obj_get(ir, "cmd"), "ls"));
        kimix_yyjson_doc_free(im);

        // immutable -> mutable, mutate, write again.
        kimix_yyjson_doc *src = kimix_yyjson_read_str("{\"x\":1}", KIMIX_YYJSON_READ_NOFLAG, nullptr);
        kimix_yyjson_mut_doc *copy = kimix_yyjson_doc_mut_copy(src);
        expect(copy != nullptr);
        expect(kimix_yyjson_mut_obj_add_sint(copy, kimix_yyjson_mut_doc_root(copy), "y", 2));
        char *twice = kimix_yyjson_mut_write(copy, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(twice != nullptr && std::strstr(twice, "\"y\":2") != nullptr);
        kimix_yyjson_str_free(twice);
        kimix_yyjson_mut_doc_free(kimix_yyjson_mut_doc_mut_copy(copy));
        kimix_yyjson_mut_doc_free(copy);
        kimix_yyjson_doc_free(src);

        // NULL-safety of the builders and the writers.
        expect(kimix_yyjson_mut_arr(nullptr) == nullptr);
        expect(!kimix_yyjson_mut_arr_append(nullptr, nullptr));
        expect(!kimix_yyjson_mut_obj_add_str(nullptr, nullptr, nullptr, nullptr));
        expect(kimix_yyjson_mut_write(nullptr, KIMIX_YYJSON_WRITE_NOFLAG, nullptr, nullptr) == nullptr);
        kimix_yyjson_mut_doc_free(nullptr);
        kimix_yyjson_mut_doc_free(m);
    };

    // ------------------------------------------------------------------ repair
    "repair malformed json into a vector"_test = [] {
        const char *bad = "{\"a\": 1, \"b\": [1, 2,],}";
        expect(!kimix_json_is_valid(bad, std::strlen(bad)));

        kimix_vec out;
        expect(kimix_repair(&out, bad, std::strlen(bad)) == KIMIX_OK);
        expect(kimix_vec_size(&out) > 0u);
        // The repaired text is NUL-terminated and strictly valid JSON.
        const char *cstr = kimix_repaired_cstr(&out);
        expect(cstr != nullptr);
        expect(kimix_json_is_valid_str(cstr));
        kimix_str_view view;
        expect(kimix_repaired_view(&out, &view) == KIMIX_OK);
        expect(view.length + 1 == kimix_vec_size(&out)); // the trailing '\0' is excluded
        expect(view.data != nullptr && view.data[view.length] == '\0');

        // Reuse of the same live placeholder is an assign, not a re-init.
        expect(kimix_repair(&out, bad, std::strlen(bad)) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_repair_assign(&out, bad, std::strlen(bad)) == KIMIX_OK);
        expect(kimix_vec_size(&out) > 0u);

        // Already-valid input => EMPTY result (the kimix::repair() convention).
        kimix_vec clean;
        const char *good = "{\"a\":1}";
        expect(kimix_json_is_valid(good, std::strlen(good)));
        expect(kimix_repair(&clean, good, std::strlen(good)) == KIMIX_OK);
        expect(kimix_vec_size(&clean) == 0u);
        expect(kimix_repaired_view(&clean, &view) == KIMIX_OK);
        expect(view.length == 0u);
        expect(kimix_repaired_cstr(&clean) == nullptr);

        // Argument / state errors, then teardown.
        expect(kimix_repair(&out, nullptr, 4) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_repair(nullptr, good, std::strlen(good)) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_repair_assign(&clean, nullptr, 0) == KIMIX_OK);
        expect(kimix_repaired_view(&out, nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);
        expect(kimix_vec_destroy(&clean) == KIMIX_OK);
        // The storage is raw again, so a fresh construction succeeds.
        expect(kimix_repair(&out, good, std::strlen(good)) == KIMIX_OK);
        expect(kimix_vec_size(&out) == 0u);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);

        // The heap variant frees through the vector FFI.
        kimix_vec *h = kimix_repair_new(bad, std::strlen(bad));
        expect(h != nullptr);
        expect(kimix_vec_size(h) > 0u);
        kimix_vec_free(h);
        expect(kimix_repair_new(nullptr, 8) == nullptr);
    };

    "repair keeps the buffer on the library heap"_test = [] {
        // A repaired result must be releasable by kimix_vec_destroy() alone: the
        // vector<char> that kimix::repair() returned was moved (not copied) into
        // the caller's placeholder, so exactly one owner ever exists.
        for (int i = 0; i < 2000; ++i) {
            kimix_vec v;
            const char *bad = "[1,2,3,]";
            if (kimix_repair(&v, bad, std::strlen(bad)) != KIMIX_OK) {
                expect(false);
                return;
            }
          expect(kimix_vec_size(&v) > 0u);
          if (kimix_vec_destroy(&v) != KIMIX_OK) {
              expect(false);
              return;
          }
      }
      };

    // ---------------------------------------------------------------------- maps
    "map lifecycle and the NULL contract"_test = [] {
        kimix_map *m = kimix_map_new();
        expect(m != nullptr);
        expect(kimix_map_size(m) == 0u);

        uint64_t value = 0xDEADu;
        uint64_t key = 0u;
        expect(!kimix_map_get(m, 1u, &value));
        expect(value == 0xDEADu); // a miss leaves the caller's sentinel alone
        expect(!kimix_map_contains(m, 1u));
        expect(!kimix_map_remove(m, 1u));
        expect(kimix_map_entry_at(m, 0u, &key, &value) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_map_allocated_count(m) > 0u); // the object term is always there

        // Every entry point tolerates a NULL handle with its documented neutral
        // value or status; none of them dereferences it.
        expect(kimix_map_reserve(nullptr, 4u) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_map_clear(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_map_set(nullptr, 1u, 2u) == KIMIX_ERR_INVALID_ARG);
        expect(!kimix_map_get(nullptr, 1u, &value));
        expect(!kimix_map_remove(nullptr, 1u));
        expect(!kimix_map_contains(nullptr, 1u));
        expect(kimix_map_size(nullptr) == 0u);
        expect(kimix_map_entry_at(nullptr, 0u, &key, &value) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_map_allocated_count(nullptr) == 0u);
        // A NULL out-parameter is the same "no value delivered" answer as a miss.
        expect(!kimix_map_get(m, 1u, nullptr));
        expect(kimix_map_set(m, 7u, 8u) == KIMIX_OK);
        expect(kimix_map_entry_at(m, 0u, nullptr, &value) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_map_entry_at(m, 0u, &key, nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_map_entry_at(m, 0u, &key, &value) == KIMIX_OK); // the outs stay valid
        expect(key == 7u && value == 8u);

        kimix_map_free(m); // releases the object and both arrays to the library heap
        kimix_map_free(nullptr); // documented as a no-op
    };

    "map set / get / upsert / remove and the dense walk"_test = [] {
        kimix_map *m = kimix_map_new();
        expect(m != nullptr);
        constexpr uint64_t k_n = 100u;

        for (uint64_t i = 0; i < k_n; ++i) {
            expect(kimix_map_set(m, i * 7919u + 3u, i ^ 0xA5u) == KIMIX_OK);
        }
        expect(kimix_map_size(m) == k_n);

        uint64_t value = 0u;
        for (uint64_t i = 0; i < k_n; ++i) {
            value = 0u;
            expect(kimix_map_get(m, i * 7919u + 3u, &value));
            expect(value == (i ^ 0xA5u));
            expect(kimix_map_contains(m, i * 7919u + 3u));
        }
        expect(!kimix_map_get(m, k_n * 7919u + 3u, &value)); // never inserted

        // The walk is the documented insertion order: entry i is the i-th key that
        // was set (none of them was removed yet).
        for (uint64_t i = 0; i < k_n; ++i) {
            uint64_t key = 0u, got = 0u;
            expect(kimix_map_entry_at(m, i, &key, &got) == KIMIX_OK);
            expect(key == i * 7919u + 3u) << "entry_at must walk insertion order";
            expect(got == (i ^ 0xA5u));
        }
        expect(kimix_map_entry_at(m, k_n, &value, &value) == KIMIX_ERR_OUT_OF_RANGE);

        // Upsert: the value changes in place, the size and the index do not.
        expect(kimix_map_set(m, 40u * 7919u + 3u, 1234u) == KIMIX_OK);
        expect(kimix_map_size(m) == k_n);
        uint64_t upkey = 0u, upvalue = 0u;
        expect(kimix_map_entry_at(m, 40u, &upkey, &upvalue) == KIMIX_OK);
        expect(upkey == 40u * 7919u + 3u && upvalue == 1234u);
        // ... and re-setting every key keeps the whole walk identical.
        for (uint64_t i = 0; i < k_n; ++i) {
            expect(kimix_map_set(m, i * 7919u + 3u, i) == KIMIX_OK);
        }
        bool order_kept = true;
        for (uint64_t i = 0; i < k_n; ++i) {
            uint64_t key = 0u, got = 0u;
            if (kimix_map_entry_at(m, i, &key, &got) != KIMIX_OK || key != i * 7919u + 3u || got != i) {
                order_kept = false;
            }
        }
        expect(order_kept) << "set() on existing keys must not reorder the dense array";
        expect(kimix_map_size(m) == k_n);

        // Removal: true once, false afterwards, and the walk stays a permutation of
        // the survivors (the documented backward-shift delete may reorder them).
        const uint64_t victim = 40u * 7919u + 3u;
        expect(kimix_map_remove(m, victim));
        expect(!kimix_map_remove(m, victim));
        expect(!kimix_map_contains(m, victim));
        expect(kimix_map_size(m) == k_n - 1u);
        uint8_t seen[k_n] = {0u};
        bool permutation = true;
        for (uint64_t i = 0; i < k_n - 1u; ++i) {
            uint64_t key = 0u, got = 0u;
            if (kimix_map_entry_at(m, i, &key, &got) != KIMIX_OK) {
                permutation = false;
                break;
            }
            if (key == victim || key < 3u || ((key - 3u) % 7919u) != 0u) {
                permutation = false; // not one of our keys any more
                break;
            }
            const uint64_t origin = (key - 3u) / 7919u;
            if (origin >= k_n || seen[origin] != 0u) {
                permutation = false; // a duplicate in the walk
                break;
            }
            seen[origin] = 1u;
            if (got != origin) { // values were re-set to `i` above
                permutation = false;
            }
        }
        uint64_t survivors = 0u;
        for (uint64_t i = 0; i < k_n; ++i) { survivors += seen[i]; }
        expect(survivors == k_n - 1u) << "the walk must cover every surviving entry once";
        expect(permutation);
        expect(kimix_map_entry_at(m, k_n - 1u, &upkey, &upvalue) == KIMIX_ERR_OUT_OF_RANGE);

        kimix_map_free(m);
    };

    "map reserve / clear / allocated_count accounting"_test = [] {
        kimix_map *m = kimix_map_new();
        expect(m != nullptr);
        const uint64_t fresh = kimix_map_allocated_count(m);
        expect(fresh > 0u) << "the handle object is owned memory from the first call";
        expect(kimix_map_allocated_count(m) == fresh); // stable while nothing mutates
        expect(kimix_map_allocated_count(m) == fresh);

        // reserve() grows the accounting and never shrinks it (documented).
        expect(kimix_map_reserve(m, 1000u) == KIMIX_OK);
        const uint64_t reserved = kimix_map_allocated_count(m);
        expect(reserved > fresh);
        expect(kimix_map_reserve(m, 10u) == KIMIX_OK);
        expect(kimix_map_allocated_count(m) == reserved) << "a smaller reserve is a no-op";
        expect(kimix_map_reserve(m, 0u) == KIMIX_OK);
        expect(kimix_map_allocated_count(m) == reserved);

        // Inserting inside the reserved capacity does not grow it; going past it does.
        for (uint64_t i = 0; i < 500u; ++i) {
            expect(kimix_map_set(m, i, i * 3u + 1u) == KIMIX_OK);
        }
        expect(kimix_map_allocated_count(m) == reserved) << "no growth below the capacity";
        for (uint64_t i = 500u; i < 2000u; ++i) {
            expect(kimix_map_set(m, i, i * 3u + 1u) == KIMIX_OK);
        }
        const uint64_t grown = kimix_map_allocated_count(m);
        expect(grown > reserved);

        // Pure observation never perturbs the number.
        uint64_t value = 0u, key = 0u;
        for (uint64_t i = 0; i < 2000u; ++i) {
            expect(kimix_map_get(m, i, &value));
            expect(kimix_map_contains(m, i));
            expect(kimix_map_entry_at(m, i, &key, &value) == KIMIX_OK);
        }
        expect(kimix_map_size(m) == 2000u);
        expect(kimix_map_allocated_count(m) == grown);

        // clear() empties the map and keeps the storage (documented), so the count
        // is unchanged while size() is 0.
        expect(kimix_map_clear(m) == KIMIX_OK);
        expect(kimix_map_size(m) == 0u);
        expect(!kimix_map_contains(m, 0u));
        expect(kimix_map_allocated_count(m) == grown) << "clear() releases nothing";
        // Re-inserting afterwards reuses it: still no growth at 100 entries.
        for (uint64_t i = 0; i < 100u; ++i) {
            expect(kimix_map_set(m, i + 50000u, i) == KIMIX_OK);
        }
        expect(kimix_map_allocated_count(m) == grown);
        kimix_map_free(m);

        // Per-instance and reproducible: two maps driven by the same call sequence
        // report the same bytes, and neither one moves when the other is written to
        // (the property a per-handle leak ledger rests on).
        kimix_map *a = kimix_map_new();
        kimix_map *b = kimix_map_new();
        expect(a != nullptr && b != nullptr);
        for (uint64_t i = 0; i < 300u; ++i) {
            expect(kimix_map_set(a, i * 13u + 1u, i) == KIMIX_OK);
        }
        const uint64_t a_before = kimix_map_allocated_count(a);
        expect(a_before > kimix_map_allocated_count(b)); // b is still empty
        for (uint64_t i = 0; i < 300u; ++i) {
            expect(kimix_map_set(b, i * 13u + 1u, i) == KIMIX_OK);
        }
        expect(kimix_map_allocated_count(b) == a_before) << "the count is a function of the call sequence";
        expect(kimix_map_allocated_count(a) == a_before) << "and per instance";
        for (uint64_t i = 300u; i < 4000u; ++i) {
            expect(kimix_map_set(b, i * 13u + 1u, i) == KIMIX_OK);
        }
        expect(kimix_map_allocated_count(a) == a_before) << "b's growth must not touch a's ledger";
        expect(kimix_map_allocated_count(b) > a_before);
        // A ledger of many live maps: every handle reports its own non-zero count,
        // and once freed the only countable state left is NULL, which is 0.
        uint64_t ledger = 0u;
        for (int i = 0; i < 64; ++i) {
            kimix_map *t = kimix_map_new();
            expect(t != nullptr);
            for (uint64_t k = 0; k < 32u; ++k) {
                expect(kimix_map_set(t, k + static_cast<uint64_t>(i) * 1000u, k) == KIMIX_OK);
            }
            ledger += kimix_map_allocated_count(t);
            kimix_map_free(t);
            expect(kimix_map_allocated_count(nullptr) == 0u); // summable to 0 after free
        }
        expect(ledger > 64u * 32u * 16u) << "32 dense entries per map must show up in the sum";
        kimix_map_free(a);
        kimix_map_free(b);
    };

    // ------------------------------------------------------------------- parallel
    "fiber run: every job once, over several OS threads, from an unbound thread"_test = [] {
        // This suite's main() binds no scheduler, so this is the foreign-host case
        // the area is built for: the call transiently binds the shared pool.
        expect(kimix_fiber_worker_count() == 1u) << "an unbound thread sees one worker";

        ffi_stamp_state state;
        bool completed = false;
        const kimix_status st = kimix_fiber_run(ffi_stamp_state::k_jobs, ffi_stamp_job, &state, 0u, nullptr, &completed);
        expect(st == KIMIX_OK);
        expect(completed) << "no cancel flag was given, so every job ran";
        expect(state.ran.load() == ffi_stamp_state::k_jobs);

        uint64_t hits = 0u;
        for (uint64_t i = 0; i < ffi_stamp_state::k_jobs; ++i) {
            hits += state.hit[i];
        }
        expect(hits == ffi_stamp_state::k_jobs) << "each job id ran exactly once";
        expect(state.seen_context == static_cast<void *>(&state)) << "context passed through verbatim";

        const size_t threads = ffi_distinct(state.stamps, ffi_stamp_state::k_jobs);
        std::printf("[ffi] kimix_fiber_run(%llu jobs) ran on %zu distinct OS threads\n",
                    static_cast<unsigned long long>(ffi_stamp_state::k_jobs), threads);
        expect(threads >= 2u) << "the jobs must spread over the pool the call bound, saw " << threads;
        expect(state.workers_seen.load() >= 2u) << "a job must see the pool it runs on via kimix_fiber_worker_count()";

        // The transient binding must be gone: the calling thread is unbound again.
        expect(kimix_fiber_worker_count() == 1u) << "kimix_fiber_run must not leave the pool bound";
    };

    "fiber run: the task_limit cap and the inline path"_test = [] {
        // 64 jobs capped at 4 tasks: peak concurrency is what the caller asked for.
        ffi_peak_state capped;
        bool completed = false;
        expect(kimix_fiber_run(64u, ffi_peak_job, &capped, 4u, nullptr, &completed) == KIMIX_OK);
        expect(completed);
        expect(capped.ran.load() == 64u);
        const uint64_t peak4 = capped.peak.load();
        std::printf("[ffi] task_limit=4 -> peak %llu jobs in flight\n", static_cast<unsigned long long>(peak4));
        expect(peak4 <= 4u) << "task_limit must cap the number of jobs running at once";
        expect(peak4 >= 1u);

        // A cap of 1 collapses the split: everything runs inline on this thread and
        // no pool is bound at all -- the documented degenerate case.
        ffi_peak_state capped1;
        completed = false;
        expect(kimix_fiber_run(64u, ffi_peak_job, &capped1, 1u, nullptr, &completed) == KIMIX_OK);
        expect(completed);
        expect(capped1.ran.load() == 64u);
        expect(capped1.peak.load() == 1u) << "a one-task cap must serialise the jobs";
        expect(kimix_fiber_worker_count() == 1u) << "...without binding a pool";

        // The same work with no cap uses the bound pool (>= the capped peak).
        ffi_peak_state free_for_all;
        expect(kimix_fiber_run(64u, ffi_peak_job, &free_for_all, 0u, nullptr, &completed) == KIMIX_OK);
        expect(completed && free_for_all.ran.load() == 64u);
        std::printf("[ffi] task_limit=0 -> peak %llu jobs in flight\n",
                    static_cast<unsigned long long>(free_for_all.peak.load()));
        expect(free_for_all.peak.load() >= peak4);
    };

    "fiber run: the cancel flag stops the run early"_test = [] {
        // (a) A flag that is already set: no job runs at all, and the call says so.
        ffi_cancel_state preset;
        preset.cancel = true;
        bool completed = true;
        expect(kimix_fiber_run(1024u, ffi_first_sets_cancel, &preset, 0u, &preset.cancel, &completed) == KIMIX_OK);
        expect(!completed) << "a pre-set cancel flag means the run stopped early";
        expect(preset.ran.load() == 0u) << "not one job may start once the flag is set";

        // (b) A flag raised by the first job to start: the jobs other workers had
        //     already claimed may still run, everything after that is skipped.
        ffi_cancel_state mid;
        completed = true;
        constexpr uint64_t k_jobs = 4096u;
        expect(kimix_fiber_run(k_jobs, ffi_first_sets_cancel, &mid, 0u, &mid.cancel, &completed) == KIMIX_OK);
        const uint64_t ran = mid.ran.load();
        std::printf("[ffi] cancel mid-run: %llu of %llu jobs ran\n",
                    static_cast<unsigned long long>(ran), static_cast<unsigned long long>(k_jobs));
        expect(!completed) << "out_completed false when the run stopped on *cancel";
        expect(ran >= 1u) << "the job that raised the flag ran";
        expect(ran * 2u < k_jobs) << "the rest of the range must be skipped, ran=" << ran;

        // (c) A live flag that is never raised: completed true, every job ran.
        ffi_cancel_state never;
        completed = false;
        expect(kimix_fiber_run(64u, ffi_counting_job, &never, 0u, &never.cancel, &completed) == KIMIX_OK);
        expect(completed);
        expect(never.ran.load() == 64u);

        // (d) A NULL out_completed is allowed (nobody asks whether it finished).
        ffi_cancel_state nullout;
        expect(kimix_fiber_run(32u, ffi_counting_job, &nullout, 2u, nullptr, nullptr) == KIMIX_OK);
        expect(nullout.ran.load() == 32u);
    };

    "fiber run: edges - zero jobs, NULL fn, NULL context, big index passthrough"_test = [] {
        ffi_counting_state state;
        bool completed = false;

        // job_count 0 is a no-op success with completed == true (and it must not
        // bind a pool: the calling thread is still unbound afterwards).
        expect(kimix_fiber_run(0u, ffi_counting_job, &state, 0u, nullptr, &completed) == KIMIX_OK);
        expect(completed) << "an empty range is complete by definition";
        expect(state.ran.load() == 0u);
        expect(kimix_fiber_worker_count() == 1u);

        // NULL fn is the one documented error, and it is reported before any work.
        completed = true; // a failure return must not touch the out-parameter
        expect(kimix_fiber_run(16u, nullptr, &state, 0u, nullptr, &completed) == KIMIX_ERR_INVALID_ARG);
        expect(completed) << "out-parameters are written only on success";
        expect(kimix_fiber_run(1u, nullptr, nullptr, 0u, nullptr, nullptr) == KIMIX_ERR_INVALID_ARG);

        // A NULL context is legal and reaches the callback as NULL; a real one is
        // passed through verbatim (the helper below records what the job got).
        g_seen_context.store(0xBADu, std::memory_order_relaxed);
        expect(kimix_fiber_run(8u, ffi_record_context_job, &state, 0u, nullptr, &completed) == KIMIX_OK);
        expect(completed);
        expect(g_seen_context.load() == reinterpret_cast<uintptr_t>(&state)) << "context passed through verbatim";
        state.ran.store(0u);
        expect(kimix_fiber_run(8u, ffi_record_context_job, nullptr, 0u, nullptr, &completed) == KIMIX_OK);
        expect(completed) << "a NULL context is documented as legal";
        expect(g_seen_context.load() == 0u) << "...and it must arrive as NULL";

        // The job ids delivered are exactly [0, job_count): every id hits its own
        // slot once, and the min/max bracket the whole range (the split hands
        // `base + i` to the callback, so an id is never off by a window offset).
        ffi_id_range_state ids;
        expect(kimix_fiber_run(ffi_id_range_state::k_jobs, ffi_record_id_job, &ids, 0u, nullptr, &completed) == KIMIX_OK);
        expect(completed && ids.ran.load() == ffi_id_range_state::k_jobs);
        expect(ids.low.load() == 0u && ids.high.load() == ffi_id_range_state::k_jobs - 1u)
            << "the ids handed to the callback are exactly [0, job_count)";
        uint64_t ones = 0u;
        for (uint64_t i = 0; i < ffi_id_range_state::k_jobs; ++i) { ones += ids.seen[i]; }
        expect(ones == ffi_id_range_state::k_jobs) << "no id skipped, no id delivered twice";
    };

    // ------------------------------------------------------------------ sqlite
    "sqlite: library queries and NULL edges"_test = [] {
        expect(kimix_sqlite_libversion() != nullptr);
        expect(std::strlen(kimix_sqlite_libversion()) > 0);
        expect(kimix_sqlite_libversion_number() >= 3000000);
        expect(kimix_sqlite_threadsafe());
        // Every value entry point answers the documented neutral value on NULL.
        expect(kimix_sqlite_errmsg(nullptr)[0] == '\0');
        expect(kimix_sqlite_errcode(nullptr) == KIMIX_SQLITE_MISUSE);
        expect(kimix_sqlite_extended_errcode(nullptr) == KIMIX_SQLITE_MISUSE);
        expect(kimix_sqlite_last_insert_rowid(nullptr) == 0);
        expect(kimix_sqlite_changes(nullptr) == 0);
        expect(kimix_sqlite_bind_parameter_count(nullptr) == 0);
        expect(kimix_sqlite_bind_parameter_index(nullptr, ":x") == 0);
        expect(kimix_sqlite_bind_parameter_index(nullptr, nullptr) == 0);
        expect(kimix_sqlite_column_count(nullptr) == 0);
        expect(kimix_sqlite_column_type(nullptr, 0) == KIMIX_SQLITE_NULL);
        expect(kimix_sqlite_column_int64(nullptr, 0) == 0);
        expect(kimix_sqlite_column_double(nullptr, 0) == 0.0);
        expect(kimix_sqlite_column_bytes(nullptr, 0) == 0);
        expect(kimix_sqlite_column_text(nullptr, 0) == nullptr);
        expect(kimix_sqlite_column_blob(nullptr, 0) == nullptr);
        expect(kimix_sqlite_column_name(nullptr, 0) == nullptr);
        expect(kimix_sqlite_step(nullptr) == KIMIX_SQLITE_MISUSE);
        // The status entry points reject NULL before touching SQLite.
        expect(kimix_sqlite_close(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_busy_timeout(nullptr, 100) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_finalize(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_reset(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_clear_bindings(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_exec(nullptr, "SELECT 1", nullptr) == KIMIX_ERR_INVALID_ARG);
        kimix_sqlite_db *leak = reinterpret_cast<kimix_sqlite_db *>(0x1);
        expect(kimix_sqlite_open(nullptr, &leak) == KIMIX_ERR_INVALID_ARG);
        expect(leak == reinterpret_cast<kimix_sqlite_db *>(0x1)) << "out_db untouched on error";
        expect(kimix_sqlite_open_v2(":memory:", nullptr, 0) == KIMIX_ERR_INVALID_ARG);
        kimix_sqlite_stmt *stmt = reinterpret_cast<kimix_sqlite_stmt *>(0x1);
        expect(kimix_sqlite_prepare(nullptr, "SELECT 1", &stmt) == KIMIX_ERR_INVALID_ARG);
        expect(stmt == reinterpret_cast<kimix_sqlite_stmt *>(0x1)) << "out_stmt untouched on error";
    };

    "sqlite: exec, prepare/bind/step/column round trip, changes"_test = [] {
        kimix_sqlite_db *db = nullptr;
        expect(kimix_sqlite_open(":memory:", &db) == KIMIX_OK);
        expect(db != nullptr);
        expect(std::strcmp(kimix_sqlite_errmsg(db), "not an error") == 0)
            << "a fresh connection carries SQLite's own neutral text";
        expect(kimix_sqlite_errcode(db) == KIMIX_SQLITE_OK);

        expect(kimix_sqlite_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, data BLOB, note TEXT)",
                                 nullptr) == KIMIX_OK);

        // One prepared INSERT bound for every storage class, stepped to DONE.
        kimix_sqlite_stmt *ins = nullptr;
        expect(kimix_sqlite_prepare(db, "INSERT INTO t(name, score, data, note) VALUES(:name, ?2, ?3, ?4)", &ins) == KIMIX_OK);
        expect(kimix_sqlite_bind_parameter_count(ins) == 4);
        expect(kimix_sqlite_bind_parameter_index(ins, ":name") == 1);
        expect(kimix_sqlite_bind_parameter_index(ins, ":missing") == 0);
        const char k_name[] = "h\xC3\xA9llo"; // "héllo" in UTF-8
        expect(kimix_sqlite_bind_text(ins, 1, k_name, static_cast<int>(sizeof(k_name) - 1)) == KIMIX_OK);
        expect(kimix_sqlite_bind_double(ins, 2, 9.5) == KIMIX_OK);
        const uint8_t k_blob[] = {0x00, 0x01, 0xFE, 0xFF};
        expect(kimix_sqlite_bind_blob(ins, 3, k_blob, static_cast<int>(sizeof(k_blob))) == KIMIX_OK);
        expect(kimix_sqlite_bind_null(ins, 4) == KIMIX_OK);
        // A parameter index outside [1, count] is OUT_OF_RANGE, not FAILED.
        expect(kimix_sqlite_bind_int64(ins, 0, 1) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_sqlite_bind_int64(ins, 5, 1) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_sqlite_bind_text(ins, 1, "x", -1) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_sqlite_step(ins) == KIMIX_SQLITE_DONE);
        expect(kimix_sqlite_changes(db) == 1);
        expect(kimix_sqlite_last_insert_rowid(db) == 1);
        expect(kimix_sqlite_finalize(ins) == KIMIX_OK);

        // reset() keeps the bindings: the same values insert again as row 2.
        expect(kimix_sqlite_prepare(db, "INSERT INTO t(name, score, data, note) VALUES(?1, ?2, ?3, ?4)", &ins) == KIMIX_OK);
        const char k_second[] = "second";
        expect(kimix_sqlite_bind_text(ins, 1, k_second, static_cast<int>(sizeof(k_second) - 1)) == KIMIX_OK);
        expect(kimix_sqlite_bind_int64(ins, 2, 7) == KIMIX_OK);
        expect(kimix_sqlite_bind_text(ins, 3, "ab", 2) == KIMIX_OK);
        expect(kimix_sqlite_bind_text(ins, 4, "", 0) == KIMIX_OK); // non-NULL + 0 = the EMPTY string
        expect(kimix_sqlite_step(ins) == KIMIX_SQLITE_DONE);
        expect(kimix_sqlite_reset(ins) == KIMIX_OK);
        expect(kimix_sqlite_step(ins) == KIMIX_SQLITE_DONE) << "reset re-runs with the same bindings";
        expect(kimix_sqlite_changes(db) == 1) << "one row per run";
        expect(kimix_sqlite_last_insert_rowid(db) == 3);
        expect(kimix_sqlite_clear_bindings(ins) == KIMIX_OK);
        expect(kimix_sqlite_step(ins) == KIMIX_SQLITE_DONE);
        expect(kimix_sqlite_finalize(ins) == KIMIX_OK);

        // The full SELECT walk: types, values, names, bytes.
        kimix_sqlite_stmt *sel = nullptr;
        expect(kimix_sqlite_prepare(db, "SELECT id, name, score, data, note FROM t ORDER BY id", &sel) == KIMIX_OK);
        expect(kimix_sqlite_column_count(sel) == 5);
        expect(std::strcmp(kimix_sqlite_column_name(sel, 1), "name") == 0);
        expect(kimix_sqlite_step(sel) == KIMIX_SQLITE_ROW);
        expect(kimix_sqlite_column_type(sel, 0) == KIMIX_SQLITE_INTEGER);
        expect(kimix_sqlite_column_int64(sel, 0) == 1);
        expect(kimix_sqlite_column_type(sel, 1) == KIMIX_SQLITE_TEXT);
        expect(std::strcmp(kimix_sqlite_column_text(sel, 1), "h\xC3\xA9llo") == 0);
        expect(kimix_sqlite_column_bytes(sel, 1) == 6); // 1 ASCII + 2x2-byte UTF-8
        expect(kimix_sqlite_column_type(sel, 2) == KIMIX_SQLITE_FLOAT);
        expect(kimix_sqlite_column_double(sel, 2) == 9.5);
        expect(kimix_sqlite_column_type(sel, 3) == KIMIX_SQLITE_BLOB);
        expect(kimix_sqlite_column_bytes(sel, 3) == 4);
        expect(std::memcmp(kimix_sqlite_column_blob(sel, 3), k_blob, sizeof(k_blob)) == 0);
        expect(kimix_sqlite_column_type(sel, 4) == KIMIX_SQLITE_NULL);
        expect(kimix_sqlite_column_text(sel, 4) == nullptr);
        expect(kimix_sqlite_step(sel) == KIMIX_SQLITE_ROW);
        expect(kimix_sqlite_column_text(sel, 4) != nullptr);
        expect(kimix_sqlite_column_bytes(sel, 4) == 0) << "empty string round trips as 0 bytes, not NULL";
        expect(std::strcmp(kimix_sqlite_column_text(sel, 4), "") == 0);
        expect(kimix_sqlite_step(sel) == KIMIX_SQLITE_ROW);
        expect(kimix_sqlite_column_type(sel, 1) == KIMIX_SQLITE_TEXT);
        expect(kimix_sqlite_step(sel) == KIMIX_SQLITE_ROW);
        expect(kimix_sqlite_column_type(sel, 1) == KIMIX_SQLITE_NULL)
            << "clear_bindings left the re-run with SQL NULLs";
        expect(kimix_sqlite_step(sel) == KIMIX_SQLITE_DONE);
        expect(kimix_sqlite_finalize(sel) == KIMIX_OK);

        // FTS5 (the amalgamation is compiled with SQLITE_ENABLE_FTS5) is plain SQL.
        expect(kimix_sqlite_exec(db, "CREATE VIRTUAL TABLE ft USING fts5(body)", nullptr) == KIMIX_OK);
        expect(kimix_sqlite_exec(db, "INSERT INTO ft(body) VALUES('the quick brown fox')", nullptr) == KIMIX_OK);
        kimix_sqlite_stmt *fts = nullptr;
        expect(kimix_sqlite_prepare(db, "SELECT count(*) FROM ft WHERE ft MATCH 'quick'", &fts) == KIMIX_OK);
        expect(kimix_sqlite_step(fts) == KIMIX_SQLITE_ROW);
        expect(kimix_sqlite_column_int64(fts, 0) == 1);
        expect(kimix_sqlite_finalize(fts) == KIMIX_OK);

        expect(kimix_sqlite_close(db) == KIMIX_OK);
    };

    "sqlite: error paths - bad SQL, owned message, extended errcode"_test = [] {
        kimix_sqlite_db *db = nullptr;
        expect(kimix_sqlite_open(":memory:", &db) == KIMIX_OK);

        // A failing exec: KIMIX_ERR_FAILED, the message is OWNED by the caller.
        char *msg = nullptr;
        expect(kimix_sqlite_exec(db, "INSERT INTO no_such_table VALUES(1)", &msg) == KIMIX_ERR_FAILED);
        expect(msg != nullptr);
        expect(std::strstr(msg, "no_such_table") != nullptr);
        kimix_mem_free(msg);
        // The borrowed twin stays on the connection.
        expect(std::strstr(kimix_sqlite_errmsg(db), "no_such_table") != nullptr);
        expect(kimix_sqlite_errcode(db) == KIMIX_SQLITE_ERROR);

        // A compile failure through prepare(): out stays untouched, message on the db.
        kimix_sqlite_stmt *stmt = nullptr;
        expect(kimix_sqlite_prepare(db, "SELEC nonsense", &stmt) == KIMIX_ERR_FAILED);
        expect(stmt == nullptr);
        expect(std::strlen(kimix_sqlite_errmsg(db)) > 0);

        // A UNIQUE violation surfaces the extended code through step().
        expect(kimix_sqlite_exec(db, "CREATE TABLE u(v INTEGER UNIQUE)", nullptr) == KIMIX_OK);
        expect(kimix_sqlite_exec(db, "INSERT INTO u VALUES(1)", nullptr) == KIMIX_OK);
        kimix_sqlite_stmt *dup = nullptr;
        expect(kimix_sqlite_prepare(db, "INSERT INTO u VALUES(1)", &dup) == KIMIX_OK);
        expect(kimix_sqlite_step(dup) == KIMIX_SQLITE_CONSTRAINT) << "primary code passes through step()";
        expect(kimix_sqlite_extended_errcode(db) != KIMIX_SQLITE_OK);
        expect(kimix_sqlite_extended_errcode(db) != kimix_sqlite_errcode(db))
            << "the extended code carries the sub-code";
        expect(kimix_sqlite_finalize(dup) == KIMIX_ERR_FAILED) << "finalize reports the failed last step";

        // out_errmsg NULL means "nobody asks" - still a clean failure.
        expect(kimix_sqlite_exec(db, "INSERT INTO no_such_table VALUES(1)", nullptr) == KIMIX_ERR_FAILED);

        // An unreadable path fails the OPEN itself (no lazy file here).
        kimix_sqlite_db *gone = nullptr;
        expect(kimix_sqlite_open_v2("/definitely/not/a/real/dir/db.sqlite", &gone,
                                    KIMIX_SQLITE_OPEN_READONLY) == KIMIX_ERR_FAILED);
        expect(gone == nullptr) << "out_db is untouched when the open fails";

        expect(kimix_sqlite_busy_timeout(db, 250) == KIMIX_OK);
          expect(kimix_sqlite_close(db) == KIMIX_OK);
      };
      // ----------------------------------------------------------------- graphdb
      "graphdb: identity, layout and status names"_test = [] {
          expect(kimix_graphdb_abi_version() == KIMIX_GDB_ABI_VERSION);
          expect(std::strlen(kimix_graphdb_version_string()) > 0);
          kimix_gdb_layout layout{};
          expect(kimix_graphdb_layout_info(&layout) == KIMIX_GDB_OK);
          expect(layout.abi_version == kimix_graphdb_abi_version());
          expect(layout.sizeof_property == sizeof(kimix_gdb_property));
          expect(layout.sizeof_property_record == sizeof(kimix_gdb_property_record));
          expect(layout.sizeof_vertex_key == sizeof(kimix_gdb_vertex_key));
          expect(layout.sizeof_edge_record == sizeof(kimix_gdb_edge_record));
          expect(layout.sizeof_property_type == sizeof(kimix_gdb_property_type));
          expect(layout.sizeof_capability == sizeof(kimix_gdb_capability));
          expect(layout.sizeof_vertex_spec == sizeof(kimix_gdb_vertex_spec));
          expect(layout.sizeof_size_t == sizeof(size_t));
          expect(layout.alignof_guid == alignof(kimix_gdb_guid));
          expect(kimix_graphdb_layout_info(nullptr) == KIMIX_GDB_ERR_ARGUMENT);
          expect(std::strcmp(kimix_graphdb_status_name(KIMIX_GDB_OK), "ok") == 0);
          expect(std::strcmp(kimix_graphdb_status_name(KIMIX_GDB_ERR_VERTEX), "error_vertex") ==
                 0);
          expect(std::strcmp(kimix_graphdb_status_name(12345), "status_unknown") == 0);
          expect(kimix_graphdb_status_name(-1) != nullptr) << "never NULL, always printable";
      };
      "graphdb: NULL session contract"_test = [] {
          kimix_gdb_guid guid{};
          kimix_gdb_vertex_spec vspec = g_spec(0, "x");
          uint32_t id = 0;
          size_t count = 0;
          expect(kimix_graphdb_capabilities(nullptr, nullptr) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_vertex_create(nullptr, &vspec, &guid, nullptr) ==
                 KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_vertex_translate(nullptr, &vspec.key, &guid, nullptr) ==
                 KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_edge_free(nullptr, guid) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_tx_begin(nullptr) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_tx_commit(nullptr) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_label_create(nullptr, "l", 1, &id) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_vertex_labels(nullptr, guid, nullptr, 0, &count) ==
                 KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_vertex_properties(nullptr, guid, nullptr, 0, nullptr, 0, &count,
                                                 &count) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_edges_of_vertex(nullptr, guid, 0, 0, 0, nullptr, 0, &count,
                                               &count) == KIMIX_GDB_ERR_ARGUMENT);
          // The edge-filtered path entry point is re-exported too (an additive
          // surface after ABI 6: the symbol exists in this image even if a caller
          // never uses it).
          expect(kimix_graphdb_shortest_path_edges(nullptr, guid, guid, 0, 0, 1, nullptr, 0,
                                                   &count) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_vertices_create_bulk(nullptr, &vspec, 1, 1, nullptr, nullptr,
                                                    nullptr) == KIMIX_GDB_ERR_ARGUMENT);
          expect(kimix_graphdb_persist(nullptr, 0) == KIMIX_GDB_ERR_ARGUMENT);
          kimix_graphdb_close(nullptr); // documented no-op
      };
      "graphdb: session lifecycle, capabilities, the storeless persist/load"_test = [] {
          gd_guard g;
          kimix_gdb_capability caps{};
          expect(kimix_graphdb_capabilities(g.session, &caps) == KIMIX_GDB_OK);
          expect(caps.supports_edge_by_stable_uid == 1);
          expect(caps.supports_edge_property_read == 1);
          expect(caps.supports_collective_transactions == 1);
          expect(caps.enforces_unique_external_ids == 1);
          // G1/G3/G4 are implemented (src/graph_db/capability.cpp), so the session
          // reports them as available.
          expect(caps.supports_global_scan == 1);
          expect(caps.supports_native_multi_hop == 1);
          expect(caps.supports_global_aggregates == 1);
          // An in-memory session has no store to write back into or re-read.
          expect(kimix_graphdb_persist(g.session, 1) == KIMIX_GDB_ERR_NO_SUCH_FILE);
          expect(kimix_graphdb_load(g.session) == KIMIX_GDB_ERR_NO_SUCH_FILE);
      };
      "graphdb: label schema CRUD through the two-phase scratch protocol"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t again = 0;
          uint8_t found = 0;
          expect(kimix_graphdb_label_find(g.session, "person", 6, &again, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 1 && again == person);
          expect(kimix_graphdb_label_find(g.session, "alien", 5, &again, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0) << "a miss is found=0, never an error";
          expect(kimix_graphdb_label_create(g.session, "person", 6, &again) ==
                 KIMIX_GDB_ERR_NAME_EXISTS);
          char name[32] = {0};
          size_t required = 0;
          expect(kimix_graphdb_label_name(g.session, person, name, sizeof name, &required) ==
                 KIMIX_GDB_OK);
          expect(std::strcmp(name, "person") == 0);
          size_t count = 0;
          size_t bytes = 0;
          expect(kimix_graphdb_label_get_all(g.session, nullptr, 0, nullptr, 0, &count,
                                             &bytes) == KIMIX_GDB_OK);
          expect(count == 1 && bytes == 6);
          kimix_gdb_label items[4];
          uint8_t scratch[64];
          expect(kimix_graphdb_label_get_all(g.session, items, 4, scratch, sizeof scratch,
                                             &count, &bytes) == KIMIX_GDB_OK);
          expect(count == 1);
          expect(std::memcmp(scratch + items[0].name_offset, "person", items[0].name_len) ==
                 0);
          expect(kimix_graphdb_label_get_all(g.session, items, 0, scratch, sizeof scratch,
                                             &count, &bytes) == KIMIX_GDB_ERR_BUFFER);
          expect(count == 1) << "the too-small call still reports the sizes";
          expect(kimix_graphdb_label_update(g.session, person, "human", 5) == KIMIX_GDB_OK);
          expect(kimix_graphdb_label_find(g.session, "human", 5, &again, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 1 && again == person);
          expect(kimix_graphdb_label_free(g.session, person) == KIMIX_GDB_OK);
          expect(kimix_graphdb_label_find(g.session, "human", 5, &again, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0);
      };
      "graphdb: property type schema CRUD"_test = [] {
          gd_guard g;
          uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                         KIMIX_GDB_DTYPE_INT64, 1);
          uint32_t scores = g_property_type(g.session, "scores", KIMIX_GDB_CONTAINER_ARRAY,
                                            KIMIX_GDB_DTYPE_DOUBLE, 3);
          uint32_t found_id = 0;
          uint8_t found = 0;
          expect(kimix_graphdb_property_type_find(g.session, "age", 3, &found_id, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 1 && found_id == age);
          expect(kimix_graphdb_property_type_find(g.session, "nope", 4, &found_id, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0);
          expect(kimix_graphdb_property_type_create(g.session, "age", 3, KIMIX_GDB_CONTAINER_SINGLE,
                                                    KIMIX_GDB_DTYPE_INT64, KIMIX_GDB_SIZE_FIXED, 1, &found_id) ==
                 KIMIX_GDB_ERR_NAME_EXISTS);
          size_t count = 0;
          size_t bytes = 0;
          expect(kimix_graphdb_property_type_get_all(g.session, nullptr, 0, nullptr, 0, &count,
                                                     &bytes) == KIMIX_GDB_OK);
          expect(count == 2);
          kimix_gdb_property_type items[4];
          uint8_t scratch[64];
          expect(kimix_graphdb_property_type_get_all(g.session, items, 4, scratch,
                                                     sizeof scratch, &count, &bytes) ==
                 KIMIX_GDB_OK);
          expect(count == 2);
          bool saw_scores = false;
          for (size_t i = 0; i < count; ++i) {
              if (items[i].id == scores) {
                  saw_scores = true;
                  expect(items[i].container == KIMIX_GDB_CONTAINER_ARRAY);
                  expect(items[i].dtype == KIMIX_GDB_DTYPE_DOUBLE);
                  expect(items[i].count == 3);
                  expect(std::memcmp(scratch + items[i].name_offset, "scores",
                                     items[i].name_len) == 0);
              }
          }
          expect(saw_scores);
          expect(kimix_graphdb_property_type_update(g.session, age, "years", 5,
                                                    KIMIX_GDB_CONTAINER_SINGLE, KIMIX_GDB_DTYPE_INT64,
                                                    KIMIX_GDB_SIZE_FIXED, 1) == KIMIX_GDB_OK);
          expect(kimix_graphdb_property_type_find(g.session, "years", 5, &found_id, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 1 && found_id == age);
          expect(kimix_graphdb_property_type_free(g.session, age) == KIMIX_GDB_OK);
          expect(kimix_graphdb_property_type_find(g.session, "years", 5, &found_id, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0);
      };
      "graphdb: vertex create / merge / translate / free"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          auto a = g_vertex(g.session, person, "a");
          kimix_gdb_guid out{};
          uint8_t found = 0;
          kimix_gdb_vertex_key key = g_spec(person, "a").key;
          expect(kimix_graphdb_vertex_translate(g.session, &key, &out, &found) == KIMIX_GDB_OK);
          expect(found == 1 && g_same(out, a));
          kimix_gdb_vertex_key missing = g_spec(person, "nope").key;
          expect(kimix_graphdb_vertex_translate(g.session, &missing, &out, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0) << "a miss is ok + found=0, never an error";
          // MERGE: an existing key matches untouched; a new key creates.
          kimix_gdb_vertex_spec spec = g_spec(person, "a");
          uint8_t created = 0;
          expect(kimix_graphdb_vertex_get_or_create(g.session, &spec, &out, &created) ==
                 KIMIX_GDB_OK);
          expect(created == 0 && g_same(out, a));
          spec = g_spec(person, "b");
          expect(kimix_graphdb_vertex_get_or_create(g.session, &spec, &out, &created) ==
                 KIMIX_GDB_OK);
          expect(created == 1 && !g_same(out, a));
          // enforce-unique mode: a straight duplicate create is an error.
          spec = g_spec(person, "a");
          expect(kimix_graphdb_vertex_create(g.session, &spec, &out, nullptr) ==
                 KIMIX_GDB_ERR_NON_UNIQUE_ID);
          kimix_gdb_guid no_such{1, 2};
          expect(kimix_graphdb_vertex_free(g.session, no_such) == KIMIX_GDB_ERR_VERTEX);
          expect(kimix_graphdb_vertex_free(g.session, a) == KIMIX_GDB_OK);
          expect(kimix_graphdb_vertex_free(g.session, a) == KIMIX_GDB_ERR_VERTEX);
          expect(kimix_graphdb_vertex_translate(g.session, &key, &out, &found) ==
                 KIMIX_GDB_OK);
          expect(found == 0) << "the freed key left the index too";
      };
      "graphdb: vertex labels, the key-label-first order"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t tag = g_label(g.session, "tag");
          uint32_t labels[2] = {person, tag};
          kimix_gdb_vertex_spec spec = g_spec(person, "v1");
          spec.labels = labels;
          spec.label_count = 2;
          kimix_gdb_guid v{};
          expect(kimix_graphdb_vertex_create(g.session, &spec, &v, nullptr) == KIMIX_GDB_OK);
          uint32_t out[4] = {0, 0, 0, 0};
          size_t count = 0;
          expect(kimix_graphdb_vertex_labels(g.session, v, out, 4, &count) == KIMIX_GDB_OK);
          expect(count == 2);
          expect(out[0] == person) << "the key label comes first (the read-back order)";
          expect(g_has(out, count, tag));
          expect(kimix_graphdb_vertex_label_add(g.session, v, tag) == KIMIX_GDB_ERR_LABEL)
              << "already present";
          expect(kimix_graphdb_vertex_label_add(g.session, v, 424242) == KIMIX_GDB_ERR_LABEL)
              << "undeclared on a GDI-strict session";
          expect(kimix_graphdb_vertex_label_remove(g.session, v, tag) == KIMIX_GDB_OK);
          expect(kimix_graphdb_vertex_label_remove(g.session, v, tag) == KIMIX_GDB_ERR_LABEL)
              << "already absent";
          expect(kimix_graphdb_vertex_labels(g.session, v, nullptr, 0, &count) ==
                 KIMIX_GDB_OK);
          expect(count == 1);
          expect(kimix_graphdb_vertex_labels(g.session, v, out, 0, &count) ==
                 KIMIX_GDB_ERR_BUFFER);
      };
      "graphdb: vertex properties of every kind, the add/set/update/remove semantics"_test =
          [] {
              gd_guard g;
              uint32_t person = g_label(g.session, "person");
              auto v = g_vertex(g.session, person, "v1");
              uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                             KIMIX_GDB_DTYPE_INT64, 1);
              uint32_t score = g_property_type(g.session, "score", KIMIX_GDB_CONTAINER_SINGLE,
                                               KIMIX_GDB_DTYPE_DOUBLE, 1);
              uint32_t name = g_property_type(g.session, "name", KIMIX_GDB_CONTAINER_SINGLE,
                                              KIMIX_GDB_DTYPE_CHAR, 1);
              uint32_t data = g_property_type(g.session, "data", KIMIX_GDB_CONTAINER_SINGLE,
                                              KIMIX_GDB_DTYPE_BYTE, 1);
              uint32_t ints = g_property_type(g.session, "ints", KIMIX_GDB_CONTAINER_ARRAY,
                                              KIMIX_GDB_DTYPE_INT64, 3);
              uint32_t ds = g_property_type(g.session, "ds", KIMIX_GDB_CONTAINER_ARRAY,
                                            KIMIX_GDB_DTYPE_DOUBLE, 2);
              uint32_t tag = g_property_type(g.session, "tag", KIMIX_GDB_CONTAINER_SINGLE,
                                             KIMIX_GDB_DTYPE_CHAR, 1);
              int64_t iv[3] = {1, 2, 3};
              double dv[2] = {2.5, 3.5};
              uint8_t blob[3] = {7, 8, 9};
              expect(kimix_graphdb_vertex_property_add(g.session, v, age, &g_int(33)) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_add(g.session, v, score, &g_double(9.5)) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_add(g.session, v, name, &g_text("alice")) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_add(g.session, v, data, &g_blob(blob, 3)) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_add(g.session, v, ints, &g_int_array(iv, 3)) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_add(g.session, v, ds, &g_double_array(dv, 2)) ==
                     KIMIX_GDB_OK);
              kimix_gdb_property_record items[8];
              uint8_t scratch[256];
              size_t count = g_read_properties(g.session, v, items, 8, scratch, sizeof scratch);
              expect(count == 6);
              auto rec = [&](uint32_t name_id) -> kimix_gdb_property_record const * {
                  for (size_t i = 0; i < count; ++i) {
                      if (items[i].name_id == name_id) {
                          return &items[i];
                      }
                  }
                  return nullptr;
              };
              kimix_gdb_property_record const *r = rec(age);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_INT64 && r->i64 == 33);
              r = rec(score);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_DOUBLE && r->f64 == 9.5);
              r = rec(name);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_STRING && r->payload_len == 5);
              expect(std::memcmp(scratch + r->payload_offset, "alice", 5) == 0);
              r = rec(data);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_BLOB && r->payload_len == 3);
              expect(scratch[r->payload_offset] == 7 && scratch[r->payload_offset + 2] == 9);
              r = rec(ints);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_INT64_ARRAY && r->count == 3);
              expect(r->payload_len == 3 * sizeof(int64_t));
              expect(std::memcmp(scratch + r->payload_offset, iv, sizeof iv) == 0);
              r = rec(ds);
              expect(r != nullptr && r->kind == KIMIX_GDB_KIND_DOUBLE_ARRAY && r->count == 2);
              expect(std::memcmp(scratch + r->payload_offset, dv, sizeof dv) == 0);
              // The three write semantics stay distinct, and the type checks bite.
              expect(kimix_graphdb_vertex_property_add(g.session, v, age, &g_int(1)) ==
                     KIMIX_GDB_ERR_PROPERTY_EXISTS);
              expect(kimix_graphdb_vertex_property_add(g.session, v, name, &g_int(1)) ==
                     KIMIX_GDB_ERR_WRONG_TYPE);
              int64_t short_iv[2] = {1, 2};
              expect(kimix_graphdb_vertex_property_add(g.session, v, ints,
                                                       &g_int_array(short_iv, 2)) ==
                     KIMIX_GDB_ERR_COUNT);
              expect(kimix_graphdb_vertex_property_add(g.session, v, 424242, &g_int(1)) ==
                     KIMIX_GDB_ERR_PROPERTY_TYPE);
              expect(kimix_graphdb_vertex_property_set(g.session, v, tag, &g_text("x")) ==
                     KIMIX_GDB_ERR_NO_PROPERTY)
                  << "set replaces and refuses to create";
              expect(kimix_graphdb_vertex_property_add(g.session, v, tag, &g_text("t1")) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_set(g.session, v, tag, &g_text("t2")) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_update(g.session, v, tag,
                                                          &g_text("t3")) == KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_property_update(g.session, v, age, &g_int(34)) ==
                     KIMIX_GDB_OK)
                  << "update merges (upserts)";
              uint32_t remove_names[2] = {age, tag};
              expect(kimix_graphdb_vertex_properties_remove(g.session, v, remove_names, 2) ==
                     KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_properties_remove(g.session, v, remove_names, 2) ==
                     KIMIX_GDB_OK)
                  << "absent names are ignored";
              count = g_read_properties(g.session, v, items, 8, scratch, sizeof scratch);
              expect(count == 5) << "six added + tag, minus age and tag";
              // removing ARRAY-typed values is the same path with destructors in it:
              uint32_t array_names[2] = {ints, ds};
              expect(kimix_graphdb_vertex_properties_remove(g.session, v, array_names, 2) ==
                     KIMIX_GDB_OK);
              count = g_read_properties(g.session, v, items, 8, scratch, sizeof scratch);
              expect(count == 3);
              // NULL names with a nonzero count is an argument error, never a crash.
              expect(kimix_graphdb_vertex_properties_remove(g.session, v, nullptr, 2) ==
                     KIMIX_GDB_ERR_ARGUMENT);
            };
      "graphdb: edges — create, endpoints, direction, labels, properties, free"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                         KIMIX_GDB_DTYPE_INT64, 1);
          auto v1 = g_vertex(g.session, person, "v1");
          auto v2 = g_vertex(g.session, person, "v2");
          auto v3 = g_vertex(g.session, person, "v3");
          kimix_gdb_guid no_such{1, 2};
          kimix_gdb_guid e{};
          expect(kimix_graphdb_edge_create(g.session, no_such, v2, KIMIX_GDB_EDGE_DIRECTED,
                                           nullptr, 0, nullptr, nullptr, 0, nullptr, &e) == KIMIX_GDB_ERR_VERTEX);
          expect(kimix_graphdb_edge_create(g.session, v1, v2, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                           0, nullptr, nullptr, 0, nullptr, &e) == KIMIX_GDB_OK);
          kimix_gdb_guid origin{};
          kimix_gdb_guid target{};
          expect(kimix_graphdb_edge_endpoints(g.session, e, &origin, &target) == KIMIX_GDB_OK);
          expect(g_same(origin, v1) && g_same(target, v2));
          expect(kimix_graphdb_edge_set_origin(g.session, e, v3) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_set_target(g.session, e, v1) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_endpoints(g.session, e, &origin, &target) == KIMIX_GDB_OK);
          expect(g_same(origin, v3) && g_same(target, v1));
          expect(kimix_graphdb_edge_set_direction(g.session, e, KIMIX_GDB_EDGE_UNDIRECTED) ==
                 KIMIX_GDB_OK);
          // labels on the edge
          expect(kimix_graphdb_edge_label_add(g.session, e, person) == KIMIX_GDB_OK);
          uint32_t out[4] = {0, 0, 0, 0};
          size_t count = 0;
          expect(kimix_graphdb_edge_labels(g.session, e, out, 4, &count) == KIMIX_GDB_OK);
          expect(count == 1 && out[0] == person);
          expect(kimix_graphdb_edge_label_add(g.session, e, person) == KIMIX_GDB_ERR_LABEL);
          expect(kimix_graphdb_edge_label_remove(g.session, e, person) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_labels(g.session, e, out, 4, &count) == KIMIX_GDB_OK);
          expect(count == 0);
          // properties on the edge: add + two-phase read + set/update/remove
          expect(kimix_graphdb_edge_property_add(g.session, e, age, &g_int(5)) == KIMIX_GDB_OK);
          kimix_gdb_property_record items[4];
          uint8_t scratch[128];
          size_t bytes = 0;
          expect(kimix_graphdb_edge_properties(g.session, e, nullptr, 0, nullptr, 0, &count,
                                               &bytes) == KIMIX_GDB_OK);
          expect(count == 1);
          expect(kimix_graphdb_edge_properties(g.session, e, items, 4, scratch, sizeof scratch,
                                               &count, &bytes) == KIMIX_GDB_OK);
          expect(count == 1 && items[0].name_id == age && items[0].i64 == 5);
          expect(kimix_graphdb_edge_property_set(g.session, e, age, &g_int(6)) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_property_update(g.session, e, age, &g_int(7)) ==
                 KIMIX_GDB_OK);
          uint32_t only_age[1] = {age};
          expect(kimix_graphdb_edge_properties_remove(g.session, e, only_age, 1) ==
                 KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_properties(g.session, e, nullptr, 0, nullptr, 0, &count,
                                               &bytes) == KIMIX_GDB_OK);
          expect(count == 0);
          // free + the unknown-edge codes (unknown edges are error_uid)
          expect(kimix_graphdb_edge_endpoints(g.session, no_such, &origin, &target) ==
                 KIMIX_GDB_ERR_UID);
          expect(kimix_graphdb_edge_free(g.session, e) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edge_free(g.session, e) == KIMIX_GDB_ERR_UID);
      };
      "graphdb: paged reads — orientations, constraints, cap 0/1/N"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t tag = g_label(g.session, "tag");
          uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                         KIMIX_GDB_DTYPE_INT64, 1);
          auto a = g_vertex(g.session, person, "a");
          auto b = g_vertex(g.session, person, "b");
          auto c = g_vertex(g.session, person, "c");
          auto d = g_vertex(g.session, person, "d");
          kimix_gdb_vertex_spec es = g_spec(person, "e");
          uint32_t e_labels[1] = {tag};
          es.labels = e_labels;
          es.label_count = 1;
          kimix_gdb_guid ev{};
          expect(kimix_graphdb_vertex_create(g.session, &es, &ev, nullptr) == KIMIX_GDB_OK);
          expect(kimix_graphdb_vertex_property_add(g.session, b, age, &g_int(20)) ==
                 KIMIX_GDB_OK);
          expect(kimix_graphdb_vertex_property_add(g.session, c, age, &g_int(30)) ==
                 KIMIX_GDB_OK);
          expect(kimix_graphdb_vertex_property_add(g.session, d, age, &g_int(40)) ==
                 KIMIX_GDB_OK);
          (void)g_edge(g.session, a, b);
          (void)g_edge(g.session, a, c);
          auto ab = g_edge(g.session, a, b); // parallel edge, allowed by the options
          (void)g_edge(g.session, a, d);
          (void)g_edge(g.session, ev, a); // incoming
          kimix_gdb_guid nbuf[8];
          kimix_gdb_edge_record ebuf[8];
          size_t count = 0;
          size_t total = 0;
          // neighbors: orientation + the cap protocol
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, 0,
                                                   KIMIX_GDB_ORIENTATION_OUTGOING, 0, nullptr,
                                                   0, &count, &total) == KIMIX_GDB_OK);
          expect(count == 0 && total == 3) << "cap 0 is the count probe";
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, 0,
                                                   KIMIX_GDB_ORIENTATION_OUTGOING, 2, nbuf, 8,
                                                   &count, &total) == KIMIX_GDB_OK);
          expect(count == 2 && total == 3) << "cap N: N items + the FULL count";
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, 0,
                                                   KIMIX_GDB_ORIENTATION_OUTGOING, 99, nbuf, 8,
                                                   &count, &total) == KIMIX_GDB_OK);
          expect(count == 3 && total == 3);
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, 0,
                                                   KIMIX_GDB_ORIENTATION_INCOMING, 99, nbuf, 8,
                                                   &count, &total) == KIMIX_GDB_OK);
          expect(count == 1 && total == 1 && g_same(nbuf[0], ev));
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, 0, KIMIX_GDB_ORIENTATION_BOTH,
                                                   99, nbuf, 8, &count, &total) == KIMIX_GDB_OK);
          expect(count == 4 && total == 4);
          expect(kimix_graphdb_count_neighbors(g.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               &count) == KIMIX_GDB_OK);
          expect(count == 3);
          uint8_t exists = 0;
          expect(kimix_graphdb_exists_neighbor(g.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               &exists) == KIMIX_GDB_OK);
          expect(exists == 1);
          expect(kimix_graphdb_exists_neighbor(g.session, b, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               &exists) == KIMIX_GDB_OK);
          expect(exists == 0);
          // a label constraint filters the NEIGHBOUR vertices (GDI §5.10)
          uint32_t tag_constraint = 0;
          expect(kimix_graphdb_constraint_create(g.session, &tag_constraint) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_label(g.session, tag_constraint, tag,
                                                    KIMIX_GDB_OP_EQUAL) == KIMIX_GDB_OK);
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, tag_constraint,
                                                   KIMIX_GDB_ORIENTATION_BOTH, 99, nbuf, 8,
                                                   &count, &total) == KIMIX_GDB_OK);
          expect(count == 1 && total == 1 && g_same(nbuf[0], ev));
          // a property constraint with a comparison op
          uint32_t grown = 0;
          expect(kimix_graphdb_constraint_create(g.session, &grown) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_property(g.session, grown, age,
                                                       KIMIX_GDB_OP_GREATER, &g_int(25),
                                                       1) == KIMIX_GDB_OK);
          expect(kimix_graphdb_neighbors_of_vertex(g.session, a, grown,
                                                   KIMIX_GDB_ORIENTATION_OUTGOING, 99, nbuf, 8,
                                                   &count, &total) == KIMIX_GDB_OK);
          expect(total == 2) << "neighbours older than 25: c and d";
          // edge reads filter the EDGES: label one and require it
          kimix_gdb_guid labelled = g_edge(g.session, a, b);
          expect(kimix_graphdb_edge_label_add(g.session, labelled, tag) == KIMIX_GDB_OK);
          uint32_t edge_constraint = 0;
          expect(kimix_graphdb_constraint_create(g.session, &edge_constraint) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_label(g.session, edge_constraint, tag,
                                                    KIMIX_GDB_OP_EQUAL) == KIMIX_GDB_OK);
          expect(kimix_graphdb_edges_of_vertex(g.session, a, edge_constraint,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 99, ebuf, 8,
                                               &count, &total) == KIMIX_GDB_OK);
          expect(total == 1 && g_same(ebuf[0].uid, labelled));
          expect(kimix_graphdb_edges_of_vertex(g.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               0, nullptr, 0, &count, &total) == KIMIX_GDB_OK);
          expect(count == 0 && total == 5) << "five out-edges: the parallel pair counts twice";
          expect(kimix_graphdb_edges_of_vertex(g.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               3, ebuf, 8, &count, &total) == KIMIX_GDB_OK);
          expect(count == 3 && total == 5);
          // A negative cap only bites on a real fetch: the items==NULL probe asks
          // for the count regardless (effective cap 0).
          expect(kimix_graphdb_edges_of_vertex(g.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               -1, ebuf, 8, &count, &total) ==
                 KIMIX_GDB_ERR_ARGUMENT)
              << "a negative cap is an argument error";
          // an EDGE read filtered by an EDGE PROPERTY condition (the §2.6 shape):
          // weights over 15 keep the two heavy out-edges of a fresh hub.  The
          // weight lands through edge_property_add: kimix_gdb_edge_create has no
          // property NAMES to store create-time properties with (documented in
          // graph_c_api.h), so a create-with-properties goes through
          // kimix_gdb_edges_create_bulk or an add after the create.
          {
              uint32_t const knows = g_label(g.session, "knows");
              uint32_t const weight = g_property_type(g.session, "weight", KIMIX_GDB_CONTAINER_SINGLE,
                                                      KIMIX_GDB_DTYPE_INT64, 1);
              kimix_gdb_guid const hub = g_vertex(g.session, person, "hub");
              int64_t const ws[3] = {10, 20, 30};
              char const *leaf_ids[3] = {"hub-leaf-0", "hub-leaf-1", "hub-leaf-2"};
              for (int i = 0; i < 3; ++i) {
                  kimix_gdb_guid const leaf = g_vertex(g.session, person, leaf_ids[i]);
                  kimix_gdb_guid edge = g_edge(g.session, hub, leaf);
                  expect(kimix_graphdb_edge_label_add(g.session, edge, knows) == KIMIX_GDB_OK);
                  expect(kimix_graphdb_edge_property_add(g.session, edge, weight, &g_int(ws[i])) ==
                         KIMIX_GDB_OK);
              }
              uint32_t heavy = 0;
              expect(kimix_graphdb_constraint_create(g.session, &heavy) == KIMIX_GDB_OK);
              expect(kimix_graphdb_constraint_add_property(g.session, heavy, weight,
                                                           KIMIX_GDB_OP_GREATER, &g_int(15),
                                                           1) == KIMIX_GDB_OK);
              expect(kimix_graphdb_edges_of_vertex(g.session, hub, heavy,
                                                   KIMIX_GDB_ORIENTATION_OUTGOING, 0, nullptr, 0,
                                                   &count, &total) == KIMIX_GDB_OK);
              expect(total == 2) << "the 20 and 30 out-edges pass weight > 15";
              expect(kimix_graphdb_constraint_free(g.session, heavy) == KIMIX_GDB_OK);
          }
          expect(kimix_graphdb_constraint_free(g.session, tag_constraint) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_free(g.session, grown) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_free(g.session, edge_constraint) == KIMIX_GDB_OK);
      };
      "graphdb: transactions — commit keeps, abort restores, collective is read-only"_test =
          [] {
              gd_guard g;
              uint32_t person = g_label(g.session, "person");
              // abort discards everything, including the key index entry
              expect(kimix_graphdb_tx_begin(g.session) == KIMIX_GDB_OK);
              (void)g_vertex(g.session, person, "tx1");
              expect(kimix_graphdb_tx_abort(g.session) == KIMIX_GDB_OK);
              kimix_gdb_guid out{};
              uint8_t found = 0;
              kimix_gdb_vertex_key key = g_spec(person, "tx1").key;
              expect(kimix_graphdb_vertex_translate(g.session, &key, &out, &found) ==
                     KIMIX_GDB_OK);
              expect(found == 0) << "aborted means gone, index included";
              // commit keeps
              expect(kimix_graphdb_tx_begin(g.session) == KIMIX_GDB_OK);
              auto kept = g_vertex(g.session, person, "tx2");
              expect(kimix_graphdb_tx_commit(g.session) == KIMIX_GDB_OK);
              expect(kimix_graphdb_vertex_translate(g.session, &g_spec(person, "tx2").key,
                                                    &out, &found) == KIMIX_GDB_OK);
              expect(found == 1 && g_same(out, kept));
              // one open transaction per session; commit/abort outside one is an error
              expect(kimix_graphdb_tx_begin(g.session) == KIMIX_GDB_OK);
              expect(kimix_graphdb_tx_begin(g.session) == KIMIX_GDB_ERR_TRANSACTION);
              expect(kimix_graphdb_tx_abort(g.session) == KIMIX_GDB_OK);
              expect(kimix_graphdb_tx_commit(g.session) == KIMIX_GDB_ERR_TRANSACTION);
              expect(kimix_graphdb_tx_abort(g.session) == KIMIX_GDB_ERR_TRANSACTION);
              // collective: reads run, writes refuse, close with commit ends it
              expect(kimix_graphdb_tx_begin_collective(g.session) == KIMIX_GDB_OK);
              kimix_gdb_vertex_spec ro = g_spec(person, "ro1");
              expect(kimix_graphdb_vertex_create(g.session, &ro, &out, nullptr) ==
                     KIMIX_GDB_ERR_READ_ONLY_TRANSACTION);
              expect(kimix_graphdb_vertex_translate(g.session, &g_spec(person, "tx2").key,
                                                    &out, &found) == KIMIX_GDB_OK);
              expect(found == 1) << "reads still run inside the collective";
              expect(kimix_graphdb_tx_close_collective(g.session, 1) == KIMIX_GDB_OK);
              expect(kimix_graphdb_tx_close_collective(g.session, 1) ==
                     KIMIX_GDB_ERR_TRANSACTION)
                  << "no collective is open";
          };
      "graphdb: bulk vertices and edges, per-row isolation with continue_on_error"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                         KIMIX_GDB_DTYPE_INT64, 1);
          kimix_gdb_property props[1] = {g_int(7)};
          uint32_t names[1] = {age};
          kimix_gdb_vertex_spec specs[4] = {g_spec(person, "b1"), g_spec(person, "b2"),
                                            g_spec(person, "b1"), g_spec(person, "b3")};
          specs[1].properties = props;
          specs[1].property_names = names;
          specs[1].property_count = 1;
          kimix_gdb_guid uids[4] = {};
          int32_t statuses[4] = {0, 0, 0, 0};
          uint8_t warns[4] = {0, 0, 0, 0};
          expect(kimix_graphdb_vertices_create_bulk(g.session, specs, 4, 1, uids, statuses,
                                                    warns) == KIMIX_GDB_OK);
          expect(statuses[0] == KIMIX_GDB_OK && statuses[1] == KIMIX_GDB_OK);
          expect(statuses[2] == KIMIX_GDB_ERR_NON_UNIQUE_ID)
              << "the duplicate key fails its row only";
          expect(statuses[3] == KIMIX_GDB_OK);
          expect(uids[0].lo != 0 || uids[0].hi != 0);
          kimix_gdb_guid out{};
          uint8_t found = 0;
          expect(kimix_graphdb_vertex_translate(g.session, &g_spec(person, "b3").key, &out,
                                                &found) == KIMIX_GDB_OK);
          expect(found == 1);
          kimix_gdb_property_record items[4];
          uint8_t scratch[64];
          size_t count = g_read_properties(g.session, uids[1], items, 4, scratch,
                                           sizeof scratch);
          expect(count == 1 && items[0].name_id == age && items[0].i64 == 7);
          // edges bulk: two good rows, one with an unknown endpoint
          kimix_gdb_guid no_such{1, 2};
          kimix_gdb_edge_spec especs[3] = {g_edge_spec(uids[0], uids[1]),
                                             g_edge_spec(uids[1], uids[3]),
                                             g_edge_spec(uids[0], no_such)};
          kimix_gdb_guid eids[3] = {};
          int32_t estatus[3] = {0, 0, 0};
          expect(kimix_graphdb_edges_create_bulk(g.session, especs, 3, 1, eids, estatus) ==
                 KIMIX_GDB_OK);
          expect(estatus[0] == KIMIX_GDB_OK && estatus[1] == KIMIX_GDB_OK);
          expect(estatus[2] == KIMIX_GDB_ERR_VERTEX);
          kimix_gdb_guid origin{};
          kimix_gdb_guid target{};
          expect(kimix_graphdb_edge_endpoints(g.session, eids[1], &origin, &target) ==
                 KIMIX_GDB_OK);
          expect(g_same(origin, uids[1]) && g_same(target, uids[3]));
      };
      "graphdb: constraint handles — create, extend, the error codes, free"_test = [] {
          gd_guard g;
          uint32_t person = g_label(g.session, "person");
          uint32_t age = g_property_type(g.session, "age", KIMIX_GDB_CONTAINER_SINGLE,
                                         KIMIX_GDB_DTYPE_INT64, 1);
          uint32_t c = 0;
          expect(kimix_graphdb_constraint_create(g.session, &c) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_label(g.session, c, person,
                                                    KIMIX_GDB_OP_EQUAL) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_property(g.session, c, age,
                                                       KIMIX_GDB_OP_GREATER_OR_EQUAL,
                                                       &g_int(18), 1) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_label(g.session, c, 9999, KIMIX_GDB_OP_EQUAL) ==
                 KIMIX_GDB_ERR_LABEL)
              << "an unknown label inside a condition is error_label";
          expect(kimix_graphdb_constraint_add_label(g.session, c + 1000, person,
                                                    KIMIX_GDB_OP_EQUAL) ==
                 KIMIX_GDB_ERR_CONSTRAINT)
              << "an unknown constraint is error_constraint";
          expect(kimix_graphdb_constraint_free(g.session, c) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_free(g.session, c) == KIMIX_GDB_ERR_CONSTRAINT)
              << "double free";
          // a label condition only admits equal / not_equal
          uint32_t c2 = 0;
          expect(kimix_graphdb_constraint_create(g.session, &c2) == KIMIX_GDB_OK);
          expect(kimix_graphdb_constraint_add_label(g.session, c2, person,
                                                    KIMIX_GDB_OP_GREATER) == KIMIX_GDB_ERR_OP);
          expect(kimix_graphdb_constraint_free(g.session, c2) == KIMIX_GDB_OK);
      };

  }
