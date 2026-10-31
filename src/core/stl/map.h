#pragma once

/*
 * map.h — kimix::map<Key, Value, Compare> and kimix::set<T, Compare>.
 *
 * Standalone: std::map / std::set with kimix::allocator (mimalloc).
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 * The storage is luisa::map / luisa::set — LC's B+tree containers
 * (frozenca::BTreeMap / BTreeSet) over LC's allocator, i.e. the one mimalloc
 * heap, and ONE ordered-map implementation for the whole process. They are
 * wrapped in kimix classes rather than aliased because LC's containers are
 * MOVE-ONLY (frozenca::BTreeBase declares the copy constructor and the copy
 * assignment `= delete`), while kimix code copies sets and maps — `set<int64_t>
 * excluded = protected_indices;`, returning a set by value, assigning a member
 * map. Copyability is an upper-level property of kimix::set / kimix::map, so the
 * wrappers restore it with a deep copy (element-by-element insert) and leave
 * every other operation — operator[], find, contains, count, erase, emplace,
 * insert, begin/end, size/empty/clear, at — to the LC container through
 * inheritance and a using-declaration.
 *
 * The template parameter ORDER is unchanged (Key, Value, Compare) / (T, Compare),
 * and so is the DEFAULT comparator (std::less<Key> / std::less<T>).
 *
 * RESTRICTION (external leg only, checked at compile time): the comparator must
 * be STATELESS. frozenca's B+tree does not store a comparator instance at all —
 * it default-constructs Comp{} at every use (fc/btree.h: `Comp{}(other, key)`) —
 * so a stateful functor's state would be silently dropped here while it keeps
 * working in the standalone leg. That is a leg-dependent behaviour change, which
 * is exactly what this mapping must not introduce, so the wrappers below
 * static_assert(std::is_empty_v<Compare>) instead. Every kimix call site uses the
 * default comparator, so this changes nothing today; a future need for a
 * custom order has to be an explicit key type (or a standalone-only container),
 * never a stateful functor that silently does nothing.
 *
 * Where the two differ observably (both are documented, neither is new):
 * - iterator/reference stability differs (a B+tree may move elements on
 *   insert), so do not hold an iterator across an insertion;
 * - luisa::map::at() on a missing key is a fatal precondition failure
 *   (LUISA_ERROR + abort), while std::map::at() is a throwing call this project
 *   never uses (exceptions are off, so it aborts as well).
 *
 * Both legs also expose kimix::erase_if(container, pred) — see the bottom of this
 * header: std::erase_if does not deduce against the external leg's wrappers (and
 * against the standalone leg's allocator-carrying aliases it only does by ADL),
 * so kimix code has ONE spelling that works in both.
 */

#include "memory.h"

#include <functional>
#include <map>
#include <set>

#include "vector.h"

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/map.h>
#endif

namespace kimix {

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

// ---------------------------------------------------------------------------
// map — B+tree ordered map (see the header comment; copyable by construction)
// ---------------------------------------------------------------------------
template<typename Key, typename Value,
         typename Compare = std::less<Key>>
class map : public luisa::map<Key, Value, Compare> {

public:
    using base_type = luisa::map<Key, Value, Compare>;
    // The std-shaped member types (a call site that says `map::key_type` or
    // `map::size_type` keeps compiling in both legs). value_type is the base's
    // pair type — frozenca's BTreePair, which is what operator*/begin() yield —
    // and iterator/const_iterator are the base's, because nothing here replaces
    // them.
    using key_type = typename base_type::key_type;
    using mapped_type = typename base_type::mapped_type;
    using typename base_type::value_type;
    using typename base_type::size_type;
    using typename base_type::iterator;
    using typename base_type::const_iterator;

    // See the header comment: frozenca default-constructs Compare{} at every use,
    // so a comparator with state would be silently ignored -- fail the build
    // instead.
    static_assert(std::is_empty_v<Compare>,
                  "kimix::map (third-party leg) cannot carry a stateful comparator: the "
                  "B+tree it wraps does not store one (it default-constructs Compare{}), so "
                  "the state would be silently dropped. Use the default std::less<Key> or an "
                  "empty comparator type.");

    // Every base constructor (the allocator form LC's containers take).
    using base_type::base_type;

    map() = default;
    map(map &&) noexcept = default;
    map &operator=(map &&) noexcept = default;

    /// Deep copy: LC's container is move-only, kimix::map is not.
    /// Element-wise on purpose: frozenca's insert_range() takes the min/max of
    /// the range (undefined for an empty one) and kimix copies empty maps.
    map(map const &other) : base_type{} {
        for (auto const &kv : other) {
            base_type::insert(kv);
        }
    }
    map &operator=(map const &other) {
        if (std::addressof(other) == this) {
            return *this;
        }
        this->clear();
        for (auto const &kv : other) {
            base_type::insert(kv);
        }
        return *this;
    }

    /// `map<K, V> m{{k, v}, {k2, v2}}` (std::map has it, so kimix::map does).
    map(std::initializer_list<value_type> ilist) : base_type{} {
        for (auto const &kv : ilist) {
            base_type::insert(kv);
        }
    }
    map &operator=(std::initializer_list<value_type> ilist) {
        this->clear();
        for (auto const &kv : ilist) {
            base_type::insert(kv);
        }
        return *this;
    }

    /// `map<K, V> m{first, last}`.
    template<typename It>
    map(It first, It last) : base_type{} {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }
};

// ---------------------------------------------------------------------------
// set — B+tree ordered set (see the header comment; copyable by construction)
// ---------------------------------------------------------------------------
template<typename T,
         typename Compare = std::less<T>>
class set : public luisa::set<T, Compare> {

public:
    using base_type = luisa::set<T, Compare>;
    // The std-shaped member types. frozenca spells the iterator types
    // iterator_type / const_iterator_type and luisa::set (unlike luisa::map) does
    // not re-export them under the std names, so the wrapper names them here.
    using key_type = typename base_type::key_type;
    using typename base_type::value_type;
    using typename base_type::size_type;
    using iterator = typename base_type::iterator_type;
    using const_iterator = typename base_type::const_iterator_type;

    // See the header comment (and kimix::map's): frozenca default-constructs
    // Compare{}, so a stateful comparator would be silently ignored.
    static_assert(std::is_empty_v<Compare>,
                  "kimix::set (third-party leg) cannot carry a stateful comparator: the "
                  "B+tree it wraps does not store one (it default-constructs Compare{}), so "
                  "the state would be silently dropped. Use the default std::less<T> or an "
                  "empty comparator type.");

    using base_type::base_type;

    set() = default;
    set(set &&) noexcept = default;
    set &operator=(set &&) noexcept = default;

    /// Deep copy: LC's container is move-only, kimix::set is not.
    /// Element-wise on purpose (see map's copy constructor).
    set(set const &other) : base_type{} {
        for (auto const &v : other) {
            base_type::insert(v);
        }
    }
    set &operator=(set const &other) {
        if (std::addressof(other) == this) {
            return *this;
        }
        this->clear();
        for (auto const &v : other) {
            base_type::insert(v);
        }
        return *this;
    }

    /// `set<T> s{1, 2, 3}` (std::set has it, so kimix::set does).
    set(std::initializer_list<T> ilist) : base_type{} {
        for (auto const &v : ilist) {
            base_type::insert(v);
        }
    }
    set &operator=(std::initializer_list<T> ilist) {
        this->clear();
        for (auto const &v : ilist) {
            base_type::insert(v);
        }
        return *this;
    }

    /// `set<T> s{first, last}`.
    template<typename It>
    set(It first, It last) : base_type{} {
        for (; first != last; ++first) {
            base_type::insert(*first);
        }
    }
};

#else

// ---------------------------------------------------------------------------
// map with mimalloc allocator
// ---------------------------------------------------------------------------
template<typename Key, typename Value,
         typename Compare = std::less<Key>>
using map = std::map<
    Key, Value,
    Compare,
    allocator<std::pair<const Key, Value>>>;

// ---------------------------------------------------------------------------
// set with mimalloc allocator
// ---------------------------------------------------------------------------
template<typename T,
         typename Compare = std::less<T>>
using set = std::set<
    T,
    Compare,
    allocator<T>>;

#endif

// ---------------------------------------------------------------------------
// kimix::erase_if(container, pred) — the std::erase_if recipe, one spelling for
// both legs
// ---------------------------------------------------------------------------
//
// std::erase_if(container, pred) is only reachable for the STANDALONE types, and
// only through ADL: those aliases carry kimix::allocator/std::less as template
// arguments, so unqualified `erase_if(m, pred)` finds std's overload. In the
// third-party leg kimix::map / kimix::set are CLASSES over LC's containers, so
// std::erase_if's deduction from a derived type to the std (or LC) container
// fails outright. A call site therefore cannot spell std::erase_if and stay
// leg-neutral — hence these wrappers, which each leg implements with its own
// container's native facility (std::erase_if there; here the same recipe spelled
// out over cbegin/cend, because frozenca's erase_if() member does not compile for
// this instantiation).
//
// Qualify the call (kimix::erase_if(m, pred)): unqualified lookup would, in the
// standalone leg, also find std::erase_if for exactly the same types and be
// ambiguous. Both return the number of erased elements.
// The loop is the std::erase_if recipe itself, spelled out over CONST iterators
// (cbegin/cend), because that is the one shape both legs accept: std::map's
// erase(const_iterator) returns iterator, and frozenca's erase(const_iterator)
// returns const_iterator, so `it = container.erase(it)` type-checks on both.
// frozenca DOES have its own erase_if() member, but it does not compile for this
// instantiation (it walks begin_, a const_iterator_type, and compares it with
// end(), which returns iterator_type -- no mixed iterator comparison exists), so
// the wrapper does not call it.
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template<typename Key, typename Value, typename Compare, typename Pred>
size_t erase_if(map<Key, Value, Compare> &container, Pred pred) {
    size_t removed = 0;
    for (auto it = container.cbegin(); it != container.cend();) {
        if (pred(*it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

template<typename T, typename Compare, typename Pred>
size_t erase_if(set<T, Compare> &container, Pred pred) {
    size_t removed = 0;
    for (auto it = container.cbegin(); it != container.cend();) {
        if (pred(*it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}
#else
template<typename Key, typename Value, typename Compare, typename Alloc, typename Pred>
size_t erase_if(std::map<Key, Value, Compare, Alloc> &container, Pred pred) {
    return std::erase_if(container, std::move(pred));
}

template<typename T, typename Compare, typename Alloc, typename Pred>
size_t erase_if(std::set<T, Compare, Alloc> &container, Pred pred) {
    return std::erase_if(container, std::move(pred));
}
#endif

}// namespace kimix