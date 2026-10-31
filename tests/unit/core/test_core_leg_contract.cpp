// Test for the two build legs of kimix-core (the xmake option
// kimix_external_thirdparty: standalone vendored src/ext vs the external
// third-party tree's lc-core) -- the CORE CONTRACT that has to hold in both.
// This test covers:
// - the container/allocator contract every upper-level target (kimix_api,
//   kimix-llm, runtime_py, all the unit suites) relies on, in BOTH legs:
//   vector/string/map/set/unordered_map/unordered_set/deque/list/optional/variant
//   semantics, the three helpers (enlarge_by / vector_resize / size_bytes),
//   kimix::format, kimix::string_hash (the SAME value in both legs: the hash
//   functors are kimix's own and a persisted digest must not change), and
//   copy/move of kimix::map (LC's B+tree is move-only; the kimix wrapper restores
//   copy) with insertion-order-independent lookup.
// - the container ABI across the kimix-core dll boundary: a kimix::string built by
//   THIS translation unit is consumed by an exported core function
//   (kimix::get_md5_from_string) and must produce the canonical MD5 vector for
//   "abc"; a kimix::allocate block is owned by mimalloc (mi_check_owned). Both
//   prove the one-heap rule the dll contract needs, and in the external leg they
//   prove it for an eastl::basic_string / luisa::allocator block.
// - the LEG DISCRIMINATOR itself, asserted at COMPILE time in both directions:
//   standalone kimix:: names the std containers over kimix::allocator, external
//   kimix:: names luisa:: / EASTL. A leg that silently stopped mapping (or a macro
//   that stopped being defined) fails to compile here rather than 200 suites later.
// - external leg only: the Luisa logging door (core/logging.h) -- the message text
//   and the level really reach an spdlog sink -- and the magic_enum door
//   (core/magic_enum.h) -- enum name/parse/count/contains and the underlying
//   value. Neither exists in the standalone leg by design (both headers expand to
//   nothing there), so those two cases are guarded by the same macro the doors are.
// Nothing here is external-only in a way that leaves the standalone leg with zero
// assertions: the shared cases always run and every leg-only case is paired with a
// compile-time assertion about the types that case is about.

#include "ut/ut.hpp"

#include <core/kimix_core.h>
// Both doors are included UNCONDITIONALLY: standalone they define nothing, so a
// translation unit may include them and only CALL them under the macro.
#include <core/logging.h>
#include <core/magic_enum.h>
#include <core/md5.h>
#include <core/memory.h>
#include <core/stl/deque.h>
#include <core/stl/format.h>
#include <core/stl/hash.h>
#include <core/stl/list.h>
#include <core/stl/map.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/variant.h>
#include <core/stl/vector.h>
#include <mimalloc.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
#include <utility>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
// The external leg's types ARE these; including them here is what makes the
// static_asserts below meaningful (a renamed alias fails to compile).
#include <luisa/core/stl/vector.h>
#include <luisa/core/stl/string.h>
#include <luisa/core/stl/optional.h>
#include <luisa/core/stl/variant.h>
#include <luisa/core/stl/map.h>
#include <luisa/core/stl/unordered_map.h>
#include <EASTL/vector.h>
#endif

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace tests_unit_core_test_core_leg_contract {

// ---------------------------------------------------------------------------
// The leg discriminator, at compile time, in BOTH directions.
// ---------------------------------------------------------------------------
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
// External: one STL per process -- LC's. kimix::vector IS eastl::vector (which is
// what luisa::vector is), so a value built here and one built inside
// luisa-core.dll are the same type and the same heap.
static_assert(std::is_same_v<kimix::vector<int>, luisa::vector<int>>,
              "external leg: kimix::vector must be luisa::vector");
static_assert(std::is_same_v<kimix::vector<int>, eastl::vector<int>>,
              "external leg: kimix::vector must be an EASTL vector (eastl::allocator)");
static_assert(std::is_same_v<kimix::string, luisa::string>,
              "external leg: kimix::string must be luisa::string");
static_assert(std::is_same_v<kimix::optional<int>, luisa::optional<int>>,
              "external leg: kimix::optional must be luisa::optional");
static_assert(std::is_same_v<kimix::variant<int, float>, luisa::variant<int, float>>,
              "external leg: kimix::variant must be luisa::variant");
static_assert(std::is_same_v<kimix::span<const int>, luisa::span<const int>>,
              "external leg: kimix::span must be luisa::span");
// The ordered/unordered containers are kimix WRAPPERS (LC's are move-only), so
// the assertion is inheritance, not identity: the storage is LC's B+tree /
// ankerl table, the copyability is kimix's.
static_assert(std::is_base_of_v<luisa::map<int, int>, kimix::map<int, int>>,
              "external leg: kimix::map must hold luisa::map");
static_assert(std::is_base_of_v<luisa::set<int>, kimix::set<int>>,
              "external leg: kimix::set must hold luisa::set");
// The dense containers spell their hash out because kimix keeps kimix::hash as the
// default template argument in both legs (luisa::unordered_map defaults to
// luisa::hash), so the base instantiation is named exactly.
static_assert(
    std::is_base_of_v<luisa::unordered_map<int, int, kimix::hash<int>, std::equal_to<>>,
                      kimix::unordered_map<int, int>>,
    "external leg: kimix::unordered_map must hold luisa::unordered_map");
static_assert(
    std::is_base_of_v<luisa::unordered_set<int, kimix::hash<int>, std::equal_to<>>,
                      kimix::unordered_set<int>>,
    "external leg: kimix::unordered_set must hold luisa::unordered_set");
static_assert(std::is_copy_constructible_v<kimix::map<int, int>> &&
                  std::is_copy_constructible_v<kimix::set<int>> &&
                  std::is_copy_constructible_v<kimix::unordered_map<int, int>>,
              "external leg: the kimix wrappers must stay copyable (LC's are not)");
#else
// Standalone: the std containers over the kimix (mimalloc) allocator, and NOT the
// LC ones -- the same assertions, stated for this leg.
static_assert(std::is_same_v<kimix::vector<int>, std::vector<int, kimix::allocator<int>>>,
              "standalone leg: kimix::vector must be std::vector over kimix::allocator");
static_assert(std::is_same_v<kimix::string,
                             std::basic_string<char, std::char_traits<char>,
                                              kimix::allocator<char>>>,
              "standalone leg: kimix::string must be std::basic_string over kimix::allocator");
static_assert(std::is_same_v<kimix::optional<int>, std::optional<int>>,
              "standalone leg: kimix::optional must be std::optional");
static_assert(std::is_same_v<kimix::variant<int, float>, std::variant<int, float>>,
              "standalone leg: kimix::variant must be std::variant");
static_assert(std::is_same_v<kimix::span<const int>, std::span<const int>>,
              "standalone leg: kimix::span must be std::span (no std::span in that leg "
              "would mean the alias block stopped mapping it)");
static_assert(std::is_copy_constructible_v<kimix::map<int, int>> &&
                  std::is_copy_constructible_v<kimix::set<int>> &&
                  std::is_copy_constructible_v<kimix::unordered_map<int, int>>,
              "standalone leg: kimix::map/set/unordered_map are copyable");
#endif
// Both legs: string_view stays the std one (nothing allocates through it).
static_assert(std::is_same_v<kimix::string_view, std::string_view>);

// Both legs: the ordered containers expose the std-shaped member-type names, so a
// call site that says map::key_type / map::mapped_type / set::value_type /
// ::size_type / ::iterator / ::const_iterator compiles either way. In the
// third-party leg these come from the wrapper (frozenca spells its iterators
// iterator_type / const_iterator_type, and luisa::set does not re-export them
// under the std names); in the standalone leg they come from std::map/std::set.
// value_type is NOT asserted: it is the base container's pair type (frozenca's
// BTreePair in that leg, std::pair<const K, V> here), which is documented in
// core/stl/map.h rather than hidden.
static_assert(std::is_same_v<kimix::map<int, int>::key_type, int>,
              "kimix::map must expose key_type");
static_assert(std::is_same_v<kimix::map<int, int>::mapped_type, int>,
              "kimix::map must expose mapped_type");
static_assert(std::is_same_v<kimix::map<int, int>::size_type, std::size_t>,
              "kimix::map::size_type must be the std one");
static_assert(std::is_same_v<kimix::set<int>::key_type, int>,
              "kimix::set must expose key_type");
static_assert(std::is_same_v<kimix::set<int>::value_type, int>,
              "kimix::set::value_type must be the key type");
static_assert(std::is_same_v<kimix::set<int>::size_type, std::size_t>,
              "kimix::set::size_type must be the std one");
static_assert(std::is_convertible_v<kimix::map<int, int>::iterator,
                                    kimix::map<int, int>::const_iterator>,
              "kimix::map::iterator must convert to const_iterator");
static_assert(std::is_convertible_v<kimix::set<int>::iterator,
                                    kimix::set<int>::const_iterator>,
              "kimix::set::iterator must convert to const_iterator");

// kimix::string_hash is kimix's own xxHash3 wrapper in BOTH legs -- it must keep
// producing the same value, because a persisted digest or cache key is derived
// from it. The value below was measured (it is the same in both legs).
// kimix::string_hash{}(kimix::string{"hello"}) -- an xxHash3 of the five bytes with
// kimix::hash64_default_seed. Measured on the release builds of BOTH legs (they
// agree, which is the property this pin protects).
constexpr uint64_t kHelloStringHash = 0xa70b2fd377430ef8ULL;

// The canonical MD5 of "abc" (RFC 1321 test vector).
constexpr uint8_t kMd5Abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                                 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------
inline kimix::string describe_hash(uint64_t value) {
    return kimix::format("0x{:016x}", value);
}

// The KIMIX_MEMORY_STRICT tracker's documented contract, asserted for the leg this
// binary was built in. The previous shape of this check ("tracking is off OR
// live_allocation_bytes() >= 0") could not fail -- an unsigned counter is never
// negative and a no-op stub returns 0 -- so it is stated as the two real cases:
//   tracked   (debug + KIMIX_MEMORY_STRICT) a live block is COUNTED and the
//             counter returns to its previous value when the block is released;
//   untracked every tracking query reports 0 (the documented "0 when untracked"),
//             while allocate/deallocate still round-trip through mimalloc.
inline void expect_tracker_contract() {
    size_t const before = kimix::live_allocation_bytes();
    void *const block = kimix::allocate(4096);
    expect(block != nullptr) << "kimix::allocate must hand out a block";
    if (kimix::memory_tracking_enabled()) {
        size_t const live = kimix::live_allocation_bytes();
        expect(live >= before + 4096) << "a live 4096-byte block must be counted by the tracker";
        expect(kimix::live_allocation_count() >= 1_u) << "a live block must be counted, not only sized";
        kimix::deallocate(block);
        expect(eq(kimix::live_allocation_bytes(), before))
            << "the tracked byte count must drop back once the block is freed";
    } else {
        expect(eq(before, 0_u)) << "untracked builds must report 0 live bytes, not a stale count";
        expect(eq(kimix::live_allocation_bytes(), 0_u))
            << "untracked builds must report 0 live bytes for a live block as well";
        expect(eq(kimix::live_allocation_count(), 0_u))
            << "untracked builds must report 0 live blocks";
        kimix::deallocate(block);
    }
}

// A small enum with a value outside magic_enum's default [-128, 128] range is not
// needed here, but the values are deliberately dense and named so the door's
// name/parse round trip is exact.
enum class LegDoorColor : int { red = 1, green = 2, blue = 3 };

} // namespace tests_unit_core_test_core_leg_contract

using namespace tests_unit_core_test_core_leg_contract;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "<containers_behave_identically>"_test = [] {
        kimix::vector<int> v{3, 1, 2};
        v.push_back(4);
        expect(eq(v.size(), 4_u));
        expect(kimix::size_bytes(v) == v.size() * sizeof(int));
        // enlarge_by value-initialises and returns void in BOTH legs (luisa's
        // returns a pointer, kimix keeps the standalone semantics).
        kimix::enlarge_by(v, 2);
        expect(eq(v.size(), 6_u));
        expect(eq(v[4], 0));
        expect(eq(v[5], 0));
        kimix::vector_resize(v, 2);
        expect(eq(v.size(), 2_u));

        // Copy and move of a vector keep the elements (and, in the external leg,
        // move the eastl buffer instead of copying it).
        kimix::vector<int> copied{v};
        expect(eq(copied.size(), 2_u));
        kimix::vector<int> moved{std::move(copied)};
        expect(eq(moved.size(), 2_u));
        expect(eq(moved[0], 3));

        kimix::string s = "hello";
        s += " world";
        expect(eq(s.find("world"), 6_u));
        expect(eq(s.size(), 11_u));
        expect(kimix::string{s.substr(0, 5)} == kimix::string{"hello"});
        // An embedded NUL and a long (heap) string both survive: the allocator is
        // the leg's, the semantics are std::basic_string's in both legs.
        kimix::string long_text(200, 'x');
        expect(eq(long_text.size(), 200_u));
        kimix::string with_nul{"a\0b", 3};
        expect(eq(with_nul.size(), 3_u));

        // kimix::format is the same formatter in both legs (std::format).
        expect(kimix::format("{}:{}:{}", 7, s, 2.5) == kimix::string{"7:hello world:2.5"});

        // map / set: ordered, copyable (the external leg is a B+tree wrapper).
        kimix::map<kimix::string, int> ordered;
        ordered.emplace("b", 2);
        ordered.emplace("a", 1);
        expect(eq(ordered.size(), 2_u));
        expect(eq(ordered["a"], 1));
        kimix::map<kimix::string, int> ordered_copy{ordered};
        expect(eq(ordered_copy.size(), 2_u));
        expect(eq(ordered_copy.at("b"), 2));
        expect(ordered.find("nope") == ordered.end());
        kimix::set<int> ordered_set{3, 1, 2, 1};
        expect(eq(ordered_set.size(), 3_u));
        kimix::set<int> set_copy{ordered_set};
        expect(eq(set_copy.count(2), 1_u));

        // unordered_map / unordered_set: same hash functor (kimix::hash), so the
        // same keys land in the same buckets in both legs.
        // A string-keyed dense table spells its hash out (kimix::hash has no
        // string specialization; kimix::string_hash is the documented functor), and
        // that functor is kimix's own in both legs -- see the hash case below.
        kimix::unordered_map<kimix::string, int, kimix::string_hash> dense;
        dense.emplace("one", 1);
        dense.emplace("two", 2);
        expect(eq(dense.size(), 2_u));
        expect(eq(dense.at("two"), 2));
        kimix::unordered_map<kimix::string, int, kimix::string_hash> dense_copy{dense};
        expect(eq(dense_copy.size(), 2_u));
        kimix::unordered_set<int> dense_set{1, 2, 3};
        expect(eq(dense_set.size(), 3_u));

        kimix::deque<int> dq{1, 2};
        dq.push_front(0);
        expect(eq(dq.front(), 0));
        kimix::list<int> lst{1, 2};
        lst.push_back(3);
        expect(eq(lst.size(), 3_u));

        kimix::optional<int> some{5};
        expect(some.has_value());
        expect(eq(*some, 5));
        kimix::optional<int> none;
        expect(!none.has_value());
        kimix::variant<int, kimix::string> var{kimix::string{"text"}};
        expect(kimix::holds_alternative<kimix::string>(var));
        expect(kimix::get<kimix::string>(var) == kimix::string{"text"});
    };

    // ------------------------------------------------------------------
    // kimix::erase_if -- the leg-neutral erase for the ordered AND the
    // unordered containers. std::erase_if cannot be spelled here: in the
    // standalone leg it is only reachable through ADL and, in the third-party
    // leg, template deduction does not look through the kimix wrappers (see the
    // header comments of core/stl/map.h and core/stl/unordered_map.h).
    // ------------------------------------------------------------------
    "<erase_if_is_leg_neutral>"_test = [] {
        kimix::unordered_map<int, int> dense;
        for (int i = 0; i < 10; ++i) {
            dense.emplace(i, i * i);
        }
        // The unordered containers: erase the odd keys, keep 5 of the 10.
        size_t const removed_dense =
            kimix::erase_if(dense, [](kimix::unordered_map<int, int>::value_type const &kv) {
                return kv.first % 2 != 0;
            });
        expect(eq(removed_dense, 5_u)) << "erase_if must return the number of erased elements";
        expect(eq(dense.size(), 5_u));
        expect(eq(dense.count(1), 0_u)) << "an erased key must be gone";
        expect(eq(dense.at(4), 16)) << "a kept key must still carry its value";

        kimix::unordered_set<int> dense_set{1, 2, 3, 4, 5};
        expect(eq(kimix::erase_if(dense_set, [](int value) { return value > 3; }), 2_u));
        expect(eq(dense_set.size(), 3_u));
        expect(eq(dense_set.count(5), 0_u));
        expect(eq(dense_set.count(3), 1_u));

        // The ordered containers (std::map/set here, the B+tree wrappers there).
        kimix::map<int, int> ordered;
        ordered.emplace(1, 10);
        ordered.emplace(2, 20);
        ordered.emplace(3, 30);
        expect(eq(kimix::erase_if(ordered, [](auto const &entry) { return entry.first == 2; }), 1_u));
        expect(eq(ordered.size(), 2_u));
        expect(ordered.find(2) == ordered.end()) << "the erased key must be gone";
        expect(eq(ordered.at(3), 30));

        kimix::set<int> ordered_set{1, 2, 3, 4};
        expect(eq(kimix::erase_if(ordered_set, [](int value) { return value <= 2; }), 2_u));
        expect(eq(ordered_set.size(), 2_u));
        expect(eq(ordered_set.count(1), 0_u));
        expect(eq(ordered_set.count(4), 1_u));
    };

    "<string_hash_is_leg_independent>"_test = [] {
        kimix::string const text{"hello"};
        uint64_t const via_string = kimix::string_hash{}(text);
        uint64_t const via_view = kimix::string_hash{}(kimix::string_view{text});
        uint64_t const via_std_view = kimix::hash<std::string_view>{}(std::string_view{text});
        std::fprintf(stderr, "[leg] string_hash(\"hello\") = %s\n",
                     describe_hash(via_string).c_str());
        expect(eq(via_string, via_view)) << "the view and the string hash alike";
        expect(eq(via_string, via_std_view)) << "kimix::hash<std::string_view> hashes the same bytes";
        // The pinned value: identical in the standalone and the external leg,
        // because the functor is kimix's own in both (only the allocator differs).
        expect(eq(via_string, kHelloStringHash))
            << "string_hash(\"hello\") changed -- a persisted digest would break: "
            << describe_hash(via_string).c_str();
    };

    "<container_abi_reaches_the_core_dll>"_test = [] {
        // A kimix::string built in THIS module is consumed by an exported
        // kimix-core function. In the external leg that string is an
        // eastl-backed luisa::string, so this is also the ABI check for the mapped
        // container: a layout/allocator mismatch would produce a wrong digest or
        // an invalid free.
        kimix::string const text{"abc"};
        auto const digest = kimix::get_md5_from_string(text);
        expect(eq(digest[0], kMd5Abc[0]));
        expect(eq(digest[1], kMd5Abc[1]));
        bool all_match = true;
        for (size_t i = 0; i < digest.size(); i++) {
            if (digest[i] != kMd5Abc[i]) {
                all_match = false;
            }
        }
        expect(all_match) << "MD5(\"abc\") must be the canonical vector";

        // One heap: the block comes from kimix::allocate and mimalloc recognizes
        // it as its own -- in the external leg that is luisa-core's mimalloc, which
        // is the same one heap the FFI hands out.
        void *block = kimix::allocate(64);
        expect(block != nullptr);
        expect(mi_check_owned(block)) << "kimix::allocate must come from mimalloc";
        static_cast<char *>(block)[0] = 'x';
        kimix::deallocate(block);
        expect_tracker_contract();
    };

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
    // ---- external leg only: the two doors -------------------------------------
    "<external_logging_door_reaches_the_sink>"_test = [] {
        // The door is luisa::log_* under kimix:: names. Capture with a sink so the
        // assertion is about the MESSAGE, not about a line on stdout: the level and
        // the formatted text have to arrive, which is the whole point of reusing
        // LC's single spdlog backend instead of starting a second one.
        auto captured = kimix::string{};
        auto sink = luisa::detail::create_sink_with_callback(
            [&captured](const char *level, const char *message) {
                captured += level == nullptr ? "?" : level;
                captured += ": ";
                captured += message == nullptr ? "" : message;
                captured += "\n";
            });
        expect(sink != nullptr);
        // Replace (not add) the default console sink so the suite stays quiet; this
        // process is the only consumer of the LC logger and kimix code logs nothing
        // else, so the replacement is invisible to the rest of the binary.
        luisa::detail::default_logger_set_sink(sink);
        kimix::log_level_info();
        kimix::log_info("leg contract message");
        kimix::log_warning("leg contract warning");
        kimix::log_flush();
        expect(captured.find("leg contract message") != kimix::string::npos)
            << "the info line must reach the sink: "
            << captured.c_str();
        expect(captured.find("leg contract warning") != kimix::string::npos)
            << "the warning line must reach the sink: "
            << captured.c_str();
        // The level is reported too -- spdlog's SHORT level name ("I" for info,
        // "W" for warning), which is what the sink callback receives.
        expect(captured.find("I: ") != kimix::string::npos)
            << "the sink must report the level: "
            << captured.c_str();
        expect(captured.find("W: ") != kimix::string::npos)
            << "the sink must report the warning level: "
            << captured.c_str();
        // The door also re-exports the level handle itself.
        static_assert(std::is_same_v<kimix::log_level, spdlog::level::level_enum>,
                      "the logging door must expose spdlog's level enum");
        static_assert(std::is_same_v<kimix::logger, spdlog::logger>,
                      "the logging door must expose spdlog's logger type");
        kimix::log_level_warning();
        kimix::log_level_info();
    };

    "<external_magic_enum_door>"_test = [] {
        // kimix::to_string(enum) is luisa::to_string -> magic_enum::enum_name. In
        // this leg magic_enum is configured with luisa's string_view / optional
        // aliases, so the name comes back as a kimix::string_view (=== the std one)
        // and a parse comes back as a kimix::optional -- no second string type.
        static_assert(std::is_same_v<decltype(kimix::to_string(LegDoorColor::red)),
                                     kimix::string_view>,
                      "the magic_enum door must return a kimix::string_view");
        expect(kimix::to_string(LegDoorColor::green) == kimix::string_view{"green"});
        expect(kimix::string{kimix::to_string(LegDoorColor::red)} == kimix::string{"red"});
        expect(magic_enum::enum_name(LegDoorColor::blue) == kimix::string_view{"blue"});
        auto const parsed = magic_enum::enum_cast<LegDoorColor>("blue");
        expect(parsed.has_value());
        expect(parsed.value() == LegDoorColor::blue);
        auto const missing = magic_enum::enum_cast<LegDoorColor>("not-a-color");
        expect(!missing.has_value());
        expect(magic_enum::enum_contains(LegDoorColor::red));
        expect(eq(static_cast<int>(magic_enum::enum_count<LegDoorColor>()), 3));
        expect(eq(magic_enum::enum_integer(LegDoorColor::green), 2));
    };
#else
    // ---- standalone leg: the same invariants, stated for this leg -------------
    "<standalone_leg_has_no_lc_doors>"_test = [] {
        // The two doors define NOTHING here (logging.h / magic_enum.h expand to
        // nothing without the macro) -- that is the documented contract, and it is
        // what lets a call site include them unconditionally. The runtime statement
        // of it is the type check above plus these container facts: the standalone
        // leg's allocator is kimix's, and its vector has no eastl identity.
        // (`vector<int>` vs `vector<long>` would be vacuously true for ANY
        // implementation, so the assertion is about the ALLOCATOR, which is what the
        // leg actually decides.)
        static_assert(std::is_same_v<kimix::vector<int>::allocator_type,
                                     kimix::allocator<int>>,
                      "standalone leg: kimix::vector must be over kimix::allocator");
        static_assert(std::is_same_v<kimix::map<int, int>::key_type, int>,
                      "standalone leg: kimix::map must still be the std-shaped map");
        kimix::vector<int> v{1};
        void *const buffer = static_cast<void *>(v.data());
        expect(mi_check_owned(buffer)) << "standalone kimix::vector must allocate on mimalloc";
        expect_tracker_contract();
    };
#endif
}