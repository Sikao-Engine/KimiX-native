#pragma once

// pdqsort (pattern-defeating quicksort) by Orson Peters — vendored header-only
// copy under src/ext/pdqsort (same copy LuisaCompute ships at
// luisa/core/stl/pdqsort.h), reached through the kimix-pdqsort header-only
// target, which kimix-core depends on.
//
// Note: modern std::sort (MSVC STL, libstdc++, libc++) is itself a pdqsort
// variant. The benchmark tests/unit/core/test_pdqsort_bench.cpp
// (KIMIX_PDQSORT_BENCH=1) measured the vendored reference implementation
// against std::sort on int32_t kimix::vectors; kimix::sort below encodes the
// outcome:
// - arithmetic value + default compare (std::less/std::greater): use
//   pdqsort_branchless — 3.4-3.7x faster than std::sort on random data,
//   ~6-11x on sorted / few-distinct inputs at 100k-4M sizes (branchy
//   partitioning mispredicts constantly on plain keys);
// - everything else (custom comparators, struct payloads): plain pdqsort —
//   ~1.15-1.2x over std::sort on random, and the branchless variant is a
//   small loss there (organ-pipe / nearly-sorted patterns pay the CMov
//   throughput without the mispredict win).
//
// Prefer kimix::sort over std::sort in kimix code. The upstream names stay
// available in the global namespace as pdqsort() / pdqsort_branchless() (the
// vendored file is never edited).
//
// Third-party leg (KIMIX_EXTERNAL_THIRDPARTY): the same upstream file comes
// from the external tree as <luisa/core/stl/pdqsort.h> instead of the vendored
// src/ext/pdqsort target (there is no kimix-pdqsort target in that leg). It is
// byte-identical upstream code — same include guard PDQSORT_H, same global
// pdqsort() / pdqsort_branchless() — so kimix::sort below is unchanged.

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
    #include <luisa/core/stl/pdqsort.h>
#else
    #include <pdqsort.h> // src/ext/pdqsort/pdqsort.h (via the kimix-pdqsort include dir)
#endif

#include <algorithm> // std::less / std::greater detection below
#include <iterator>  // std::iterator_traits
#include <type_traits>

namespace kimix {

namespace sort_detail {
template <class T> struct is_default_compare : std::false_type {};
template <class T> struct is_default_compare<std::less<T>> : std::true_type {};
template <class T> struct is_default_compare<std::greater<T>> : std::true_type {};
} // namespace sort_detail

// kimix::sort — the best pdqsort implementation for this project. Sorts
// [begin, end) like std::sort (random-access iterators, strict weak ordering
// comp; the two-iterator form uses <). See the header comment for why the
// branchless variant is selected for arithmetic keys with a default compare.
template <class Iter, class Compare>
inline void sort(Iter begin, Iter end, Compare comp) {
    using value_type = typename std::iterator_traits<Iter>::value_type;
    if constexpr (sort_detail::is_default_compare<typename std::decay<Compare>::type>::value &&
                  std::is_arithmetic<value_type>::value) {
        pdqsort_branchless(begin, end, comp);
    } else {
        pdqsort(begin, end, comp);
    }
}

template <class Iter>
inline void sort(Iter begin, Iter end) {
    using value_type = typename std::iterator_traits<Iter>::value_type;
    kimix::sort(begin, end, std::less<value_type>());
}

} // namespace kimix
