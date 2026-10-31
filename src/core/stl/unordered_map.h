#pragma once

/*
 * unordered_map.h — kimix::unordered_map / kimix::unordered_set.
 *
 * Both legs use ankerl::unordered_dense (robin-hood backward-shift deletion,
 * densely stored). The only thing that changes is WHICH copy of the header and
 * which allocator/vector back it:
 *
 *   standalone  ankerl from stl/unordered_dense.h, kimix::allocator buckets,
 *               kimix::vector value container.
 *   third-party ankerl from <luisa/core/stl/unordered_dense.h> (LC's copy — one
 *               implementation in the process, no second ankerl), LC's allocator
 *               and luisa::vector value container, i.e. the one mimalloc heap.
 *
 * The Hash default stays kimix::hash<Key> in both legs: it is kimix's own xxHash3
 * wrapper, so a container built here and one built in the standalone build hash
 * their keys identically.
 *
 * WHY THE EXTERNAL LEG WRAPS INSTEAD OF ALIASING
 * luisa::unordered_map / luisa::unordered_set are aliases of ankerl's
 * `detail::table` instantiated with `luisa::vector` (= eastl::vector). Two
 * properties of that instantiation make the alias unusable as-is, and both are
 * fixed WITHOUT leaving LC (the table, its allocator and its value container all
 * stay LC's — one ankerl implementation, one heap):
 *
 *  1) `table::swap()` is unusable. Its body is `using std::swap; swap(other,
 *     *this);`, and because the value container is an EASTL vector, ADL on the
 *     table's template arguments also finds `eastl::swap` — two equally good
 *     candidates, C2668. (Standalone the ADL set is empty and std::swap wins,
 *     which is why this only bites in the external leg.) The wrappers below
 *     declare their own swap() that exchanges the contents through the table's
 *     move operations, i.e. the same observable result in O(1) handle moves.
 *  2) `table::insert(std::initializer_list)` and `table::insert(It, It)` are
 *     unusable. Their bodies forward to `insert(first, last)`, and the
 *     hint+value overload `insert(const_iterator, P&&)` also matches when the
 *     range iterators are raw pointers (in this leg `const_iterator` IS the
 *     value container's pointer): `luisa::is_constructible_v<value_type, P&&>`
 *     is eastl's trait, which answers true for a scalar `value_type`, so both
 *     candidates are exact matches (C2668). `unordered_set<int>{1, 2, 3}`,
 *     `set.insert({1, 2, 3})` and `set.insert(first, last)` all go through it.
 *     The wrappers therefore declare those two overloads themselves and
 *     forward element by element to the unambiguous single-value
 *     `insert(value_type const&)`. ankerl's constructor set is likewise wider
 *     than std's — `table(size_type, Hash const&, KeyEqual const&,
 *     allocator const&)` next to `table(std::initializer_list<value_type>)` —
 *     so the wrappers declare the std-shaped constructor set (default, bucket
 *     count, initializer list, iterator range) instead of inheriting it.
 *
 * Nothing else is wrapped: find/erase/at/operator[]/iterators, emplace and the
 * operators are the LC table's, reached by inheritance. The one free function
 * that is NOT: std::erase_if. It is a function template the standard defines for
 * the std containers, and template deduction does not look through a derived
 * class, so `std::erase_if(kimix_unordered_map, pred)` cannot even be deduced in
 * this leg (in the standalone leg ankerl's own std::erase_if extension only
 * matches because kimix::unordered_map IS the table there). Inheritance cannot
 * fix that — a free function is not inherited — so BOTH legs provide
 * kimix::erase_if(container, pred) at the bottom of this header, implemented with
 * each leg's own facility, and that is the one spelling kimix code uses.
 */

#include "hash.h"
#include "vector.h"
#include "functional.h"

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/unordered_map.h>
#else
#include "unordered_dense.h"
#endif

namespace kimix {

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

namespace detail {

/// Exchange the contents of two ankerl tables through their move operations.
/// Not named `swap` on purpose: this must never be reachable by ADL from inside
/// the ankerl header (that is the ambiguity the wrappers exist to avoid).
template<typename Table>
void unordered_exchange(Table &a, Table &b) noexcept {
    if (std::addressof(a) == std::addressof(b)) {
        return;
    }
    Table tmp{std::move(a)};
    a = std::move(b);
    b = std::move(tmp);
}

}// namespace detail

// ---------------------------------------------------------------------------
// unordered_map — dense robin-hood map (see the header comment)
// ---------------------------------------------------------------------------

template<typename Key, typename Value,
         typename Hash = hash<Key>,
         typename KeyEqual = std::equal_to<>>
class unordered_map : public luisa::unordered_map<Key, Value, Hash, KeyEqual> {

public:
    using base_type = luisa::unordered_map<Key, Value, Hash, KeyEqual>;
    using typename base_type::value_type;
    // Every base overload except the two declared below (see the header comment).
    using base_type::insert;

    unordered_map() = default;

    /// `unordered_map<...> m{64u}` — bucket count, exactly as in the standalone
    /// leg (explicit, so a brace with one integral argument is never read as an
    /// initializer list).
    explicit unordered_map(size_t bucket_count,
                           Hash const &hash = Hash(),
                           KeyEqual const &equal = KeyEqual())
        : base_type(bucket_count, hash, equal) {}

    /// `unordered_map<...> m{{k, v}, {k2, v2}}` / `= {{k, v}}`.
    /// NOT `: base_type(ilist)` — ankerl's initializer_list constructor inserts
    /// through the unusable insert(initializer_list) (see the header comment).
    unordered_map(std::initializer_list<value_type> ilist) {
        for (auto const &value : ilist) {
            base_type::insert(value);
        }
    }

    /// `unordered_map<...> m{first, last}` — element-wise for the same reason.
    template<typename It>
    unordered_map(It first, It last) {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }

    /// `m.insert({{k, v}, {k2, v2}})` — element-wise, see the header comment.
    void insert(std::initializer_list<value_type> ilist) {
        for (auto const &value : ilist) {
            base_type::insert(value);
        }
    }

    /// `m.insert(first, last)` — element-wise, see the header comment.
    template<typename It>
    void insert(It first, It last) {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }

    /// Exchange the contents (see the header comment: LC's table::swap() cannot
    /// be called in this leg).
    void swap(unordered_map &other) noexcept {
        detail::unordered_exchange(static_cast<base_type &>(*this), static_cast<base_type &>(other));
    }
};

template<typename K, typename V, typename H, typename E>
inline void swap(unordered_map<K, V, H, E> &a, unordered_map<K, V, H, E> &b) noexcept {
    a.swap(b);
}

// ---------------------------------------------------------------------------
// unordered_set — dense robin-hood set
// ---------------------------------------------------------------------------

template<typename Key,
         typename Hash = hash<Key>,
         typename KeyEqual = std::equal_to<>>
class unordered_set : public luisa::unordered_set<Key, Hash, KeyEqual> {

public:
    using base_type = luisa::unordered_set<Key, Hash, KeyEqual>;
    using typename base_type::value_type;
    // Every base overload except the two declared below (see the header comment).
    using base_type::insert;

    unordered_set() = default;

    explicit unordered_set(size_t bucket_count,
                           Hash const &hash = Hash(),
                           KeyEqual const &equal = KeyEqual())
        : base_type(bucket_count, hash, equal) {}

    /// See unordered_map's initializer_list constructor: element-wise on
    /// purpose (ankerl's own would call the unusable insert(initializer_list)).
    unordered_set(std::initializer_list<value_type> ilist) {
        for (auto const &value : ilist) {
            base_type::insert(value);
        }
    }

    template<typename It>
    unordered_set(It first, It last) {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }

    /// `s.insert({1, 2, 3})` — element-wise, see the header comment.
    void insert(std::initializer_list<value_type> ilist) {
        for (auto const &value : ilist) {
            base_type::insert(value);
        }
    }

    /// `s.insert(first, last)` — element-wise, see the header comment.
    template<typename It>
    void insert(It first, It last) {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }

    void swap(unordered_set &other) noexcept {
        detail::unordered_exchange(static_cast<base_type &>(*this), static_cast<base_type &>(other));
    }
};

template<typename K, typename H, typename E>
inline void swap(unordered_set<K, H, E> &a, unordered_set<K, H, E> &b) noexcept {
    a.swap(b);
}

#else

template<typename Key, typename Value,
         typename Hash = hash<Key>,
         typename KeyEqual = std::equal_to<>>
using unordered_map = ankerl::unordered_dense::map<
    Key, Value, Hash, KeyEqual,
    allocator<std::pair<Key, Value>>,
    vector<std::pair<Key, Value>>>;

template<typename Key,
         typename Hash = hash<Key>,
         typename KeyEqual = std::equal_to<>>
using unordered_set = ankerl::unordered_dense::set<
    Key, Hash, KeyEqual,
    allocator<Key>,
    vector<Key>>;

#endif

// ---------------------------------------------------------------------------
// kimix::erase_if(container, pred) — the std::erase_if recipe, both legs
// ---------------------------------------------------------------------------
//
// One definition, because both legs' kimix::unordered_map/unordered_set expose
// the same iterator-erase contract (`erase(iterator) -> iterator`, ankerl's
// backward-shift recipe); the external leg reaches it through the wrapper, the
// standalone leg is the table itself. Returns the number of erased elements,
// like std::erase_if. Why this exists at all — std::erase_if does not deduce
// against the external wrappers — is in the header comment.
//
// Qualify the call (kimix::erase_if(m, pred)). An UNQUALIFIED `erase_if(m, pred)`
// would, in the standalone leg, also find ankerl's own std::erase_if extension
// through ADL (a `std::equal_to` template argument makes namespace std an
// associated namespace), and the two exact-match overloads would be ambiguous.
// A qualified call performs no ADL, so it always picks this one.
template<typename Key, typename Value, typename Hash, typename KeyEqual, typename Pred>
size_t erase_if(unordered_map<Key, Value, Hash, KeyEqual> &container, Pred pred) {
    size_t removed = 0;
    for (auto it = container.begin(); it != container.end();) {
        if (pred(*it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

template<typename Key, typename Hash, typename KeyEqual, typename Pred>
size_t erase_if(unordered_set<Key, Hash, KeyEqual> &container, Pred pred) {
    size_t removed = 0;
    for (auto it = container.begin(); it != container.end();) {
        if (pred(*it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}// namespace kimix