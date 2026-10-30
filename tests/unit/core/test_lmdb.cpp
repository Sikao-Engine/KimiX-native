// Test for kimix::LMDB (core/lmdb.h) — the read path.
// This test covers:
// - read() reports "absent" for every rc that is not a successful get, including
//   a REFUSED get (an empty key is MDB_BAD_VALSIZE), instead of handing out a span
//   over an uninitialized MDB_val
// - read_into() copies the value OUT of the mapping inside its read transaction, so
//   the bytes survive later writes that reuse the pages
// - read_into() clears the output, so a failed lookup can never leave a caller
//   decoding stale bytes
// - read() and read_into() agree over many keys and many absent keys

#include "ut/ut.hpp"

#include <core/kimix_core.h>
#include <core/lmdb.h>

#include <core/stl/filesystem.h>
#include <core/stl/format.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace src_tests_unit_core_test_lmdb {

// A temp directory that removes itself (the core suites have no shared fixture).
class temp_dir {
public:
    explicit temp_dir(kimix::string_view tag = "lmdb") {
        static std::atomic<uint64_t> counter{0};
        std::error_code ec;
        kimix::filesystem::path const base = kimix::filesystem::temp_directory_path(ec);
        if (ec) {
            return;
        }
        auto const stamp = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        kimix::string name;
        name.append("kimix_");
        name.append(tag.data(), tag.size());
        name.append(kimix::format("{}", stamp));
        name.append("_");
        name.append(kimix::format("{}", counter.fetch_add(1)));
        kimix::filesystem::path const target = base / kimix::filesystem::path{name};
        kimix::filesystem::create_directories(target, ec);
        if (!ec) {
            _path = target;
        }
    }
    temp_dir(temp_dir const &) = delete;
    temp_dir &operator=(temp_dir const &) = delete;
    ~temp_dir() {
        if (!_path.empty()) {
            std::error_code ec;
            kimix::filesystem::remove_all(_path, ec);
        }
    }
    [[nodiscard]] bool ok() const noexcept { return !_path.empty(); }
    [[nodiscard]] kimix::filesystem::path const &path() const noexcept { return _path; }

private:
    kimix::filesystem::path _path;
};

// A byte blob with a position-dependent pattern, so a torn / shifted copy cannot
// compare equal by accident.
kimix::vector<std::byte> pattern_bytes(size_t size, uint8_t seed) {
    kimix::vector<std::byte> out;
    out.resize(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 31u + seed) & 0xFFu);
    }
    return out;
}

kimix::span<const std::byte> as_span(kimix::vector<std::byte> const &bytes) {
    return kimix::span<const std::byte>{bytes.data(), bytes.size()};
}

} // namespace src_tests_unit_core_test_lmdb
using namespace src_tests_unit_core_test_lmdb;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "read_refused_get_reports_absent"_test = [] {
        // An EMPTY key makes mdb_get return MDB_BAD_VALSIZE, not MDB_NOTFOUND. Before
        // the fix the wrapper returned a span over an untouched MDB_val, i.e. a
        // garbage length and an indeterminate pointer.
        temp_dir dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }
        LMDB db{dir.path(), 126ull, 64ull * 1024ull * 1024ull};
        expect(static_cast<bool>(db)) << "the environment must open";
        auto const empty = db.read(kimix::string_view{});
        expect(empty.empty()) << "a refused get must report an empty span";
        auto const again = db.read(kimix::string_view{});
        expect(again.empty()) << "and do the same on the second call (no stale length)";
        kimix::vector<std::byte> out = pattern_bytes(16, 7);
        expect(!db.read_into(kimix::string_view{}, out))
            << "read_into on a refused get reports false";
        expect(out.empty()) << "and leaves no stale bytes behind";
    };

    "read_into_owns_its_bytes"_test = [] {
        temp_dir dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LMDB db{dir.path(), 126ull, 256ull * 1024ull * 1024ull};
        expect(static_cast<bool>(db));
        kimix::string_view const key{"blob/big"};
        auto const payload = pattern_bytes(1024 * 1024, 11); // 1 MiB
        db.write(key, as_span(payload));
        kimix::vector<std::byte> copy;
        expect(db.read_into(key, copy)) << "the key is present";
        expect(eq(copy.size(), payload.size()));
        // Reuse the pages the value lived in: 10k overwrites of another key.
        auto const churn = pattern_bytes(2048, 200);
        for (int i = 0; i < 10000; ++i) {
            kimix::string other{"blob/churn"};
            db.write(other, as_span(churn));
        }
        expect(std::equal(copy.begin(), copy.end(), payload.begin(), payload.end()))
            << "the copy is independent of the mapping after later writes";
        // The span form is the contrast: it points into the map, so it is only valid
        // until the next write can reuse those pages (documented on read()).
        auto const span = db.read(key);
        expect(eq(span.size(), payload.size())) << "read() still reports the length";
    };

    "read_into_missing_key_leaves_out_empty"_test = [] {
        temp_dir dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LMDB db{dir.path(), 126ull, 64ull * 1024ull * 1024ull};
        expect(static_cast<bool>(db));
        kimix::vector<std::byte> out = pattern_bytes(32, 3);
        expect(!db.read_into(kimix::string_view{"absent"}, out))
            << "an absent key reports false";
        expect(out.empty()) << "and the output is cleared, never stale bytes";
    };

    "iterator_increment_past_end_is_a_noop"_test = [] {
        // A hand-written loop (or a std::move'd iterator) may step past the end. Before
        // the fix that called mdb_cursor_get(nullptr, ...) on the released cursor.
        temp_dir dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LMDB db{dir.path(), 126ull, 64ull * 1024ull * 1024ull};
        expect(static_cast<bool>(db));
        auto const payload = pattern_bytes(16, 1);
        for (auto const key : {"a", "b", "c"}) {
            db.write(kimix::string_view{key}, as_span(payload));
        }
        size_t visited = 0;
        for (auto it = db.begin(); it != db.end(); ++it) {
            ++visited;
        }
        expect(eq(visited, size_t{3})) << "the three keys are visited";
        auto it = db.begin();
        expect(it != db.end());
        ++it;
        ++it;
        ++it;
        expect(it == db.end()) << "the fourth step ends the walk";
        ++it;
        ++it;
        ++it;
        expect(it == db.end()) << "further steps are no-ops, not a null-cursor deref";
        expect((*it).key.empty()) << "a finished iterator dereferences to an empty value";
    };

    "iterator_outliving_its_env_finishes"_test = [] {
        // The iterator stores raw MDB_env*/MDB_cursor*: it must not touch them once the
        // environment is gone. The liveness token degrades it to "finished" instead, so
        // this is a deterministic use-after-free test (run it under ASan too).
        temp_dir dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        auto const payload = pattern_bytes(16, 5);
        auto *db = new LMDB{dir.path(), 126ull, 64ull * 1024ull * 1024ull};
        expect(static_cast<bool>(*db));
        db->write(kimix::string_view{"only"}, as_span(payload));
        auto it = db->begin();
        expect(it != LMDBIteratorEndTag{}) << "the one key is visible";
        delete db; // ~LMDB marks the token closed, then munmaps
        ++it;      // must not dereference the dead cursor
        expect(it == LMDBIteratorEndTag{}) << "the iterator degrades to finished";
        ++it;
        expect((*it).value.empty()) << "and its value is empty, not a dangling span";
    };

    "read_matches_read_into_over_many_keys"_test = [] {
        temp_dir dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LMDB db{dir.path(), 126ull, 512ull * 1024ull * 1024ull};
        expect(static_cast<bool>(db));
        constexpr size_t kKeys = 10000;
        kimix::vector<kimix::vector<std::byte>> values;
        values.reserve(kKeys);
        for (size_t i = 0; i < kKeys; ++i) {
            kimix::string const key = kimix::format("k/{}", i);
            values.push_back(pattern_bytes(64 + (i % 512), static_cast<uint8_t>(i)));
            db.write(key, as_span(values.back()));
        }
        size_t compared = 0;
        for (size_t i = 0; i < kKeys; ++i) {
            kimix::string const key = kimix::format("k/{}", i);
            kimix::vector<std::byte> copy;
            expect(db.read_into(key, copy)) << "every written key reads back";
            auto const span = db.read(key);
            expect(eq(span.size(), copy.size())) << "both read paths agree on the length";
            expect(std::equal(copy.begin(), copy.end(), span.begin(), span.end()))
                << "and on the bytes";
            ++compared;
        }
        expect(eq(compared, kKeys));
        size_t absent = 0;
        for (size_t i = 0; i < kKeys; ++i) {
            kimix::string const key = kimix::format("missing/{}", i);
            kimix::vector<std::byte> copy;
            if (!db.read_into(key, copy)) {
                ++absent;
            }
            expect(db.read(key).empty()) << "an absent key has an empty span";
        }
        expect(eq(absent, kKeys)) << "every absent key reports absent";
    };
}
