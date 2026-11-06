// Test for src/runtime/soul/elision_store.{h,cpp} — the content-addressed blob
// store behind the prune elision records (report candidate A2).
//
// Covers dedup hits, ref stability across save/load, slab growth past 1 MiB,
// unknown-ref handling, byte accounting, and the malformed-blob matrix.

#include "ut/ut.hpp"

#include <runtime/soul/elision_store.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::soul::ElisionStore;

namespace tests_unit_native_test_elision_store {

kimix::string make_text(size_t bytes, char fill) {
    kimix::string s;
    s.assign(bytes, fill);
    return s;
}

} // namespace tests_unit_native_test_elision_store
using namespace tests_unit_native_test_elision_store;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "elision_put_get"_test = [] {
        ElisionStore store;
        expect(store.count() == 0u);
        const uint64_t a = store.put("hello elided text");
        const uint64_t b = store.put("second blob");
        expect(a != b);
        kimix::string_view view;
        expect(store.get(a, view));
        expect(view == "hello elided text");
        expect(store.get(b, view));
        expect(view == "second blob");
        expect(store.count() == 2u);
        expect(store.length(a) == 17u);
        expect(store.bytes_live() == 17u + 11u);
        expect(store.bytes_owned() >= store.bytes_live());

        // Unknown ref.
        expect(!store.get(a + 1, view));
        expect(!store.has(a + 1));
        expect(store.length(a + 1) == 0u);

        // copy() materialises the bytes.
        kimix::string copied;
        expect(store.copy(b, copied));
        expect(copied == "second blob");
    };

    "elision_dedup"_test = [] {
        ElisionStore store;
        const uint64_t first = store.put("duplicated payload");
        const uint64_t again = store.put("duplicated payload");
        expect(first == again);
        expect(store.count() == 1u);
        expect(store.dedup_hits() == 1u);
        const uint64_t third = store.put("duplicated payload");
        expect(third == first);
        expect(store.dedup_hits() == 2u);
        // Ref stability: identical content always maps to the same ref.
        expect(store.put("other") != first);
        expect(store.count() == 2u);
    };

    "elision_ref_stability_across_store"_test = [] {
        ElisionStore a, b;
        const uint64_t ref_a = a.put("stable content");
        const uint64_t ref_b = b.put("stable content");
        expect(ref_a == ref_b); // content-addressed, deterministic
    };

    "elision_slab_growth"_test = [] {
        ElisionStore store;
        kimix::vector<uint64_t> refs;
        // 3 MiB of distinct payloads -> at least three 1 MiB slabs.
        for (int i = 0; i < 6; ++i) {
            const kimix::string blob = make_text(512 * 1024, static_cast<char>('a' + i));
            refs.push_back(store.put(blob));
        }
        expect(store.count() == 6u);
        kimix::string_view view;
        for (size_t i = 0; i < refs.size(); ++i) {
            expect(store.get(refs[i], view));
            expect(view.size() == 512u * 1024u);
            expect(view[0] == static_cast<char>('a' + i));
        }
        expect(store.bytes_owned() >= 3u * 1024u * 1024u);
    };

    "elision_save_load"_test = [] {
        ElisionStore store;
        const uint64_t a = store.put("alpha");
        const uint64_t b = store.put("beta beta");
        kimix::string blob;
        store.save(blob);
        expect(blob.size() > 8u);
        expect(std::memcmp(blob.data(), "KMEL1", 5) == 0);

        ElisionStore restored;
        expect(restored.load(blob));
        expect(restored.count() == 2u);
        kimix::string_view view;
        expect(restored.get(a, view));
        expect(view == "alpha");
        expect(restored.get(b, view));
        expect(view == "beta beta");

        // Malformed blobs are rejected and leave the store untouched.
        expect(!restored.load(""));
        expect(!restored.load("KMEL1"));
        kimix::string bad = blob;
        bad[0] = 'X';
        expect(!restored.load(bad));
        kimix::string truncated = blob.substr(0, blob.size() - 2);
        expect(!restored.load(truncated));
        kimix::string big_count = blob;
        big_count[8] = static_cast<char>(99);
        expect(!restored.load(big_count));
        expect(restored.count() == 2u);

        // A store with the same content dedups into the restored one.
        const uint64_t again = restored.put("alpha");
        expect(again == a);
        expect(restored.dedup_hits() == 1u);
    };

    "elision_empty_content"_test = [] {
        ElisionStore store;
        const uint64_t ref = store.put("");
        kimix::string_view view;
        expect(store.get(ref, view));
        expect(view.empty());
        expect(store.count() == 1u);
        const uint64_t again = store.put("");
        expect(again == ref);
        expect(store.dedup_hits() == 1u);
    };

    "elision_clear"_test = [] {
        ElisionStore store;
        store.put("x");
        store.clear();
        expect(store.count() == 0u);
        expect(store.bytes_live() == 0u);
        kimix::string_view view;
        expect(!store.get(store.put("y"), view) == false);
    };

    "bench_elision_put"_test = [] {
        ElisionStore store;
        kimix::string blob = make_text(4 * 1024, 'z');
        for (int i = 0; i < 100; ++i) {
            const uint64_t ref = store.put(blob);
            expect(store.dedup_hits() == static_cast<uint64_t>(i));
            kimix::string_view view;
            expect(store.get(ref, view));
        }
        expect(store.count() == 1u);
    };

    return 0;
}
