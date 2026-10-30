// Test for core/memory.h — the kimix::allocate / kimix::deallocate gateway.
// This test covers:
// - allocate / deallocate round trip, zero-filled and aligned flavours
// - allocate_aligned_at address congruence, usable_size / good_size
// - reallocate preserving the prefix, expand's in-place-or-nothing contract
// - duplicate_string / duplicate_string_n
// - kimix::allocator<T> and kimix::IOperatorNewBase routing through the gateway
// - the optional KIMIX_MEMORY_STRICT tracker: live counters, the leak report
//   text and a balanced process
//
// Every tracker assertion is guarded by kimix::memory_tracking_enabled(), so one
// source runs in all four configurations (debug/release x strict on/off): the
// untracked build answers "untracked" and the assertions simply do not fire.

#include "ut/ut.hpp"

#include <core/memory.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_core_test_memory {

// A pattern written into every block the tests touch: a byte that depends on
// the offset, so a shifted or truncated block is visible.
constexpr unsigned char pattern_byte(std::size_t i) noexcept {
    return static_cast<unsigned char>((i * 37u) & 0xFFu);
}

void fill_pattern(unsigned char *p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = pattern_byte(i);
    }
}

bool pattern_intact(const unsigned char *p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != pattern_byte(i)) {
            return false;
        }
    }
    return true;
}

// IOperatorNewBase probe: a type whose new/delete must land on the gateway.
struct GatewayProbe : kimix::IOperatorNewBase {
    int value = 0;
    double pad = 0.0;
};

} // namespace tests_unit_core_test_memory
using namespace tests_unit_core_test_memory;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "allocate_deallocate_roundtrip"_test = [] {
        // 1 is the smallest interesting request; 4096 crosses mimalloc's size
        // classes; 1 MiB is a large-object request.
        const std::size_t sizes[] = {1u, 7u, 64u, 4096u, std::size_t{1} << 20};
        for (std::size_t size : sizes) {
            auto *p = static_cast<unsigned char *>(kimix::allocate(size));
            expect(p != nullptr) << "allocate must return a block";
            fill_pattern(p, size);
            expect(pattern_intact(p, size)) << "the block must be writable end to end";
            kimix::deallocate(p);
        }
        kimix::deallocate(nullptr); // documented no-op, must not crash
        expect(true) << "deallocate(nullptr) is safe";
    };

    "allocate_zeroed_is_zeroed"_test = [] {
        constexpr std::size_t n = 1024;
        auto *p = static_cast<unsigned char *>(kimix::allocate_zeroed(n));
        expect(p != nullptr);
        bool all_zero = true;
        for (std::size_t i = 0; i < n; ++i) {
            all_zero = all_zero && (p[i] == 0u);
        }
        expect(all_zero) << "allocate_zeroed must return zero-filled bytes";
        kimix::deallocate(p);

        // allocate_n is the calloc-shaped flavour: count * size zeroed bytes.
        auto *q = static_cast<std::uint32_t *>(kimix::allocate_n(16, sizeof(std::uint32_t)));
        expect(q != nullptr);
        bool n_zero = true;
        for (std::size_t i = 0; i < 16; ++i) {
            n_zero = n_zero && (q[i] == 0u);
        }
        expect(n_zero) << "allocate_n must zero count*size bytes";
        kimix::deallocate(q);
    };

    "allocate_aligned_respects_alignment"_test = [] {
        const std::size_t alignments[] = {16u, 64u, 256u, 4096u};
        for (std::size_t alignment : alignments) {
            void *p = kimix::allocate_aligned(128, alignment);
            expect(p != nullptr);
            expect((reinterpret_cast<std::uintptr_t>(p) % alignment) == 0_u)
                << "the block must satisfy the requested alignment";
            kimix::deallocate(p);
        }

        auto *z = static_cast<unsigned char *>(kimix::allocate_zeroed_aligned(64, 64));
        expect(z != nullptr);
        bool zero = true;
        for (std::size_t i = 0; i < 64; ++i) {
            zero = zero && (z[i] == 0u);
        }
        expect(zero);
        kimix::deallocate_aligned(z, 64); // the aligned release ignores the alignment in release builds
    };

    "allocate_aligned_at_respects_offset"_test = [] {
        // mimalloc's `_at` contract (its own test/test-api.c asserts exactly
        // this): the returned address plus `offset` is aligned, i.e. the address
        // is congruent to -offset modulo the alignment. That is the idiom for
        // "align the payload that follows an N-byte header".
        constexpr std::size_t alignment = 64;
        constexpr std::size_t offset = 16;
        auto *p = static_cast<unsigned char *>(kimix::allocate_aligned_at(128, alignment, offset));
        expect(p != nullptr);
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
        expect(((addr + offset) % alignment) == 0_u)
            << "(address + offset) must be a multiple of the alignment";
        p[0] = 1; // the block is writable from its first byte
        kimix::deallocate(p);
    };

    "usable_size_is_positive"_test = [] {
        const std::size_t requests[] = {1u, 100u, 4096u};
        for (std::size_t request : requests) {
            void *p = kimix::allocate(request);
            expect(p != nullptr);
            const std::size_t usable = kimix::usable_size(p);
            expect(usable >= request) << "usable size must cover the request";
            kimix::deallocate(p);
        }
        expect(eq(kimix::usable_size(nullptr), 0_u)) << "usable_size(nullptr) is 0";
    };

    "good_size_rounds_up"_test = [] {
        const std::size_t requests[] = {1u, 7u, 100u, 1000u};
        for (std::size_t request : requests) {
            expect(kimix::good_size(request) >= request)
                << "the size class must not shrink the request";
        }
        expect(kimix::good_size(16) <= kimix::good_size(4096))
            << "the size classes must not decrease with the request";
    };

    "reallocate_grows_and_preserves"_test = [] {
        constexpr std::size_t initial = 64;
        auto *p = static_cast<unsigned char *>(kimix::allocate(initial));
        expect(p != nullptr);
        fill_pattern(p, initial);

        auto *grown = static_cast<unsigned char *>(kimix::reallocate(p, 4096));
        expect(grown != nullptr) << "reallocating to a larger size must succeed";
        expect(pattern_intact(grown, initial)) << "the original prefix must survive a realloc";
        fill_pattern(grown, 4096);
        expect(pattern_intact(grown, 4096));
        kimix::deallocate(grown);

        // expand is in place or nothing: either the same block (its record
        // updated) or nullptr with the original block untouched.
        auto *base = static_cast<unsigned char *>(kimix::allocate(32));
        expect(base != nullptr);
        fill_pattern(base, 32);
        void *const expanded = kimix::expand(base, 64);
        expect(expanded == base || expanded == nullptr)
            << "expand either keeps the block or refuses";
        if (expanded != nullptr) {
            expect(pattern_intact(static_cast<unsigned char *>(expanded), 32));
            expect(kimix::usable_size(expanded) >= 64) << "a successful expand covers the new size";
        }
        kimix::deallocate(base); // valid in both outcomes (expand never moves the block)
    };

    "duplicate_string_roundtrip"_test = [] {
        constexpr const char *source = "kimix gateway";
        char *copy = kimix::duplicate_string(source);
        expect(copy != nullptr);
        expect(copy != source) << "duplicate_string must copy";
        expect(eq(std::strcmp(copy, source), 0));
        kimix::deallocate(copy);

        char *short_copy = kimix::duplicate_string_n("abcdef", 3);
        expect(short_copy != nullptr);
        expect(eq(std::strcmp(short_copy, "abc"), 0)) << "duplicate_string_n truncates at n";
        expect(eq(short_copy[3], '\0')) << "and NUL-terminates";
        kimix::deallocate(short_copy);

        expect(kimix::duplicate_string(nullptr) == nullptr);
        expect(kimix::duplicate_string_n(nullptr, 4) == nullptr);
    };

    "allocator_and_operator_new_base_route_through_the_gateway"_test = [] {
        const std::size_t before = kimix::live_allocation_count();

        kimix::allocator<int> alloc;
        int *ints = alloc.allocate(8);
        expect(ints != nullptr);
        for (int i = 0; i < 8; ++i) {
            ints[i] = i;
        }
        if (kimix::memory_tracking_enabled()) {
            expect(eq(kimix::live_allocation_count(), before + 1))
                << "kimix::allocator<T>::allocate must be tracked";
            expect(kimix::live_allocation_bytes() >= 8u * sizeof(int));
        }
        alloc.deallocate(ints, 8);

        auto *probe = new GatewayProbe();
        probe->value = 42;
        if (kimix::memory_tracking_enabled()) {
            expect(eq(kimix::live_allocation_count(), before + 1))
                << "IOperatorNewBase must allocate through the gateway";
        }
        expect(eq(probe->value, 42));
        delete probe;

        expect(eq(kimix::live_allocation_count(), before))
            << "every gateway block released leaves the live count unchanged";
    };

    "leak_report_lists_a_leaked_block"_test = [] {
        // The tracker exists only in a debug build configured with
        // --kimix_memory_strict=true; everywhere else the report is empty and
        // this test only proves the call is safe.
        const std::size_t before = kimix::live_allocation_count();
        void *leaked = kimix::allocate(1024);
        expect(leaked != nullptr);

        std::FILE *out = std::tmpfile();
        expect(out != nullptr);
        if (out == nullptr) {
            kimix::deallocate(leaked);
            return;
        }

        kimix::memory_leak_report(out);
        std::rewind(out);
        char text[4096] = {};
        const std::size_t read = std::fread(text, 1, sizeof(text) - 1, out);
        std::fclose(out);

        if (kimix::memory_tracking_enabled()) {
            expect(read > 0u) << "a deliberately leaked block must be reported";
            expect(std::strstr(text, "leak check") != nullptr) << "the report names the leak check";
            expect(std::strstr(text, "leaked block 0x") != nullptr) << "and lists the block address";
        } else {
            expect(eq(read, 0_u)) << "an untracked build reports nothing";
        }

        kimix::deallocate(leaked);
        expect(eq(kimix::live_allocation_count(), before))
            << "releasing the block takes it out of the live set";

        // A fresh file, so the earlier text cannot be re-read: with nothing
        // leaked, nothing is listed. (Only asserted when this suite alone owns
        // the live set -- another suite's static may still be alive.)
        if (kimix::memory_tracking_enabled() && before == 0u) {
            std::FILE *clean_out = std::tmpfile();
            expect(clean_out != nullptr);
            if (clean_out != nullptr) {
                kimix::memory_leak_report(clean_out);
                std::rewind(clean_out);
                char clean[4096] = {};
                const std::size_t clean_read = std::fread(clean, 1, sizeof(clean) - 1, clean_out);
                expect(eq(clean_read, 0_u)) << "a balanced process reports no leaked block";
                std::fclose(clean_out);
            }
        }
    };

    "balanced_process_reports_nothing"_test = [] {
        // Delta assertions only: other threads/fibers may allocate concurrently.
        const std::size_t before = kimix::live_allocation_count();
        const std::size_t before_bytes = kimix::live_allocation_bytes();
        {
            kimix::string text = "the gateway backs every kimix:: container";
            kimix::vector<int> numbers(64, 7);
            expect(!text.empty());
            expect(eq(numbers.size(), 64_u));
            expect(eq(numbers[63], 7));
        }
        expect(eq(kimix::live_allocation_count(), before))
            << "scoped containers must release everything they took";
        expect(eq(kimix::live_allocation_bytes(), before_bytes));
    };
}
