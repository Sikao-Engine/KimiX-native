/*
 * functional.h — kimix::function<Signature> and kimix::move_only_function<Signature>.
 *
 * Real type-erased polymorphic function wrappers.  Unlike std::function /
 * std::move_only_function, every byte of heap memory they use — both the
 * out-of-line overflow and any allocation a wrapped target performs through
 * kimix::allocator — is routed through the kimix::allocate / kimix::deallocate
 * gateway (core/memory.h, mimalloc) via the helpers in
 * stl/memory.h.  A Small Buffer Optimisation keeps
 * targets that fit in 3 * sizeof(void*) (aligned to alignof(std::max_align_t))
 * inline with no allocation at all; larger targets are constructed in place in
 * a kimix::allocate'd block whose pointer is kept in the same inline storage slot.
 *
 * API parity with the standard wrappers
 * (https://en.cppreference.com/cpp/utility/functional/function and
 *  https://en.cppreference.com/cpp/utility/functional/move_only_function):
 *   - function<Signature>           : R(Args...), R(Args...) const,
 *                                     R(Args...) noexcept,
 *                                     R(Args...) const noexcept.
 *   - move_only_function<Signature> : all 12 cv/ref/noexcept combinations of
 *                                     R(Args...): none, &, &&, and each with
 *                                     const, plus noexcept on every one.
 *
 * Deliberate deviations from the standard:
 *   - NO RTTI.  The project builds with /GR- (-fno-rtti), so target_type() and
 *     target<T>() are NOT provided; they require dynamic_cast/typeid.
 *   - NO std::hash specialisation (the project's hash machinery is separate).
 *   - Exceptions are OFF in library builds (KIMIX_NO_EXCEPTIONS,
 *     _HAS_EXCEPTIONS=0 on MSVC).  The forwarding constructors/assignments are
 *     constrained with SFINAE so that a non-callable initialiser is a
 *     compile-time error rather than the runtime std::bad_function_call that
 *     std::function would otherwise defer.  Invoking an EMPTY wrapper throws
 *     std::bad_function_call when exceptions are enabled, otherwise it prints
 *     to stderr and aborts — see detail::empty_function_abort().
 *   - Copying a kimix::function whose stored target is not copy-constructible
 *     is a compile-time error at the point the copy is instantiated: the
 *     per-target type-erased copy hook is generated only for copy-constructible
 *     targets, and the copy constructor / copy-assignment forward to it (its
 *     body is ill-formed for such a target, exactly like std::function).
 *
 * All symbols live in namespace kimix; the type-erasure machinery is in
 * kimix::detail.  This is a header-only, templates-only implementation.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY): kimix::function and
 * kimix::move_only_function stay THESE classes. LC's functional.h maps
 * luisa::function to eastl::function and luisa::move_only_function to a bundled
 * impl; neither has kimix's contract (no RTTI, empty call reports to stderr and
 * aborts, SFINAE-constrained construction, copy hook generated only for copyable
 * targets), and swapping them would change observable behaviour at every call
 * site. They are not a second heap either: their SBO overflow and their targets
 * go through kimix::allocate, which in that leg is LC's mimalloc.
 */
#pragma once

#include "memory.h"

#include <cstddef>
#include <cstdio>
#include <functional> // IWYU pragma: keep : std::bad_function_call (empty-call, exceptions build)
#include <new>        // IWYU pragma: keep : placement ::new for in-place construction
#include <type_traits>
#include <utility>

// Exception detection.  With exceptions disabled an empty call dispatches to
// abort; with them enabled it throws std::bad_function_call like the standard.
// (MSVC defines _CPPUNWIND only when /EH is on; __cpp_exceptions / __EXCEPTIONS
// cover clang / GCC; the library builds set none of them.)
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
#define KIMIX_FUNCTION_HAS_EXCEPTIONS 1
#else
#define KIMIX_FUNCTION_HAS_EXCEPTIONS 0
#endif

namespace kimix {
namespace detail {

// ---------------------------------------------------------------------------
// empty_function_abort — noreturn handler for invoking an empty wrapper.
// ---------------------------------------------------------------------------
[[noreturn]] inline void empty_function_abort() {
    static const char k_message[] = "kimix: attempted to call an empty function wrapper\n";
    std::fputs(k_message, stderr);
    std::fflush(stderr);
    std::abort();
}

// ---------------------------------------------------------------------------
// function_invoke_r — apply a callable and convert the result to R.
// Mirrors the standard INVOKE (return-type) rule: a reference or void result
// forwards the exact glvalue of the target's result (so a wrapper returning T&
// can hand back an lvalue from the target); everything else is converted with
// static_cast<R>.
// ---------------------------------------------------------------------------
template <typename R, typename F, typename... Args>
constexpr R function_invoke_r(F &&f, Args &&...args) {
    if constexpr (std::is_void_v<R>) {
        static_cast<void>(std::forward<F>(f)(std::forward<Args>(args)...));
    } else if constexpr (std::is_reference_v<R>) {
        return std::forward<F>(f)(std::forward<Args>(args)...);
    } else {
        return static_cast<R>(std::forward<F>(f)(std::forward<Args>(args)...));
    }
}

// ---------------------------------------------------------------------------
// RefQual — how the specialization's operator() ref-qualifies the invocation.
// None is used by kimix::function (operator() has no ref-qualifier); the
// move_only_function forms use LValue (&) or RValue (&&, and the unqualified
// form, which per P0288 invokes the stored target as an rvalue).
// ---------------------------------------------------------------------------
enum class RefQual : unsigned { None, LValue, RValue };

// ---------------------------------------------------------------------------
// function_subject_t<R, MoveOnly, IsConst, Ref, T> — the type used to *invoke*
// a decayed target T, per the wrapper's form.  It also governs the
// construct-from-T check the standard performs on the same "INVOKE the visible
// overload" rule.
//
//   kimix::function (MoveOnly = false):
//     const form -> const T& ; non-const form -> T&
//     (matches std::function: the stored object is invoked as an lvalue,
//      const-qualified iff the wrapper's operator() is const.)
//
//   kimix::move_only_function (MoveOnly = true):
//     &            -> T&   (operator() is non-const), const& -> const T&
//     &&           -> T&&  (operator() is non-const), const&& -> const T&&
//     unqualified  -> T&&  (operator() is non-const), const -> const T&&
//
// The wrapper's operator() cv-qualification applies directly (so a const
// specialization rejects a mutable-only target's non-const operator()), and
// an rvalue subject models P0288: the stored target is named with std::move in
// the unqualified and && forms, which makes a move-only target work and lets
// ref-qualified operator() overloads select correctly.
// ---------------------------------------------------------------------------
template <typename R, bool MoveOnly, bool IsConst, RefQual Ref, typename T>
using function_subject_t = std::conditional_t<
    MoveOnly,
    std::conditional_t<Ref == RefQual::LValue,
                       std::conditional_t<IsConst, const T &, T &>,
                       std::conditional_t<IsConst, const T &&, T &&>>,
    std::conditional_t<IsConst, const T &, T &>>;

// Cast raw target storage to the subject reference type to invoke through.
template <typename Subject>
constexpr Subject function_invoke_subject(void *p) noexcept {
    using obj = std::remove_const_t<std::remove_reference_t<Subject>>;
    return static_cast<Subject>(*static_cast<obj *>(p));
}

// ---------------------------------------------------------------------------
// function_base<R, IsConst, Ref, IsNoexcept, MoveOnly, Args...>
//
// Shared type-erased core.  A per-target vtable (Ops) holds plain function
// pointers for destroy / move / copy(if allowed) / invoke.  Storage is the
// inline small buffer or, for targets too large/over-aligned to inline, a
// mimalloc'd block whose pointer lives in that same buffer; _is_small records
// which.
//
// This base implements every operation except operator(): the concrete
// function_core / move_only_function_core add the literal cv/ref/noexcept-
// qualified operator() (a conditional cv-qualifier such as `const(IsConst)` is
// not valid C++), and the user-facing function<Sig> / move_only_function<Sig>
// specializations add the comparison friends.
// ---------------------------------------------------------------------------
template <typename R, bool IsConst, RefQual Ref, bool IsNoexcept, bool MoveOnly, typename... Args>
class function_base {
    static constexpr std::size_t k_sbo_size = 3 * sizeof(void *);
    static constexpr std::size_t k_sbo_align = alignof(std::max_align_t);
    static_assert(k_sbo_align >= alignof(void *), "SBO storage must be able to hold a pointer");

    template <typename T>
    static constexpr bool k_target_small = sizeof(T) <= k_sbo_size && alignof(T) <= k_sbo_align;

    // The subject reference used to invoke a decayed target T.
    template <typename T>
    using subject_t = function_subject_t<R, MoveOnly, IsConst, Ref, T>;

    // Is `declval<subject_t<T>>()` callable producing R, and — for the move-only
    // wrapper — is T move-constructible (the standard's constructor constraint)?
    template <typename T>
    static constexpr bool k_target_acceptable = std::is_invocable_r_v<R, subject_t<T>, Args...> &&
                                                (MoveOnly ? std::is_move_constructible_v<T>
                                                           : std::is_constructible_v<T, T &&>);

    struct Ops {
        void (*destroy)(void *buffer, bool is_small) noexcept;
        // move-construct from src buffer into dst buffer; sets dst's is_small
        void (*move)(void *dst, void *src, bool src_small, bool *dst_small) noexcept;
        // copy-construct (nullptr for MoveOnly or non-copyable targets)
        void (*copy)(void *dst, const void *src, bool src_small, bool *dst_small);
        R (*invoke)(void *buffer, bool is_small, Args... args) noexcept(IsNoexcept);
    };

    // ----- per-target hooks -----
    template <typename T>
    static void _destroy_op(void *buffer, bool is_small) noexcept {
        if (is_small) {
            static_cast<T *>(buffer)->~T();
        } else {
            kimix::delete_with_allocator(static_cast<T *>(*static_cast<void **>(buffer)));
        }
    }

    template <typename T>
    static void _move_op(void *dst, void *src, bool src_small, bool *dst_small) noexcept {
        void *src_ptr = src_small ? src : *static_cast<void **>(src);
        if constexpr (k_target_small<T>) {
            *dst_small = true;
            ::new (dst) T(std::move(*static_cast<T *>(src_ptr)));
        } else {
            *dst_small = false;
            *static_cast<void **>(dst) = kimix::new_with_allocator<T>(std::move(*static_cast<T *>(src_ptr)));
        }
        if (src_small) {
            static_cast<T *>(src_ptr)->~T();
        } else {
            kimix::delete_with_allocator(static_cast<T *>(src_ptr));
        }
    }

    template <typename T>
    static void _copy_op(void *dst, const void *src, bool src_small, bool *dst_small) {
        const void *src_ptr = src_small ? src : *static_cast<void *const *>(src);
        if constexpr (k_target_small<T>) {
            *dst_small = true;
            ::new (dst) T(*static_cast<const T *>(src_ptr));
        } else {
            *dst_small = false;
            *static_cast<void **>(dst) = kimix::new_with_allocator<T>(*static_cast<const T *>(src_ptr));
        }
    }

    template <typename T>
    static R _invoke_op(void *buffer, bool is_small, Args... args) noexcept(IsNoexcept) {
        void *p = is_small ? buffer : *static_cast<void **>(buffer);
        return function_invoke_r<R>(function_invoke_subject<subject_t<T>>(p), std::forward<Args>(args)...);
    }

    template <typename T>
    static const Ops *_ops_for() noexcept {
        // The copy hook is named only when the target is actually copyable and
        // the wrapper is copyable.  A runtime ternary would still ODR-use
        // &_copy_op<T> and therefore instantiate its body — hard error for a
        // move-only or non-copyable target — so the branch is `if constexpr`.
        if constexpr (MoveOnly || !std::is_copy_constructible_v<T>) {
            static const Ops s_ops{
                &_destroy_op<T>,
                &_move_op<T>,
                nullptr,
                &_invoke_op<T>,
            };
            return &s_ops;
        } else {
            static const Ops s_ops{
                &_destroy_op<T>,
                &_move_op<T>,
                &_copy_op<T>,
                &_invoke_op<T>,
            };
            return &s_ops;
        }
    }

protected:
    const Ops *_ops = nullptr;
    bool _is_small = false;
    alignas(k_sbo_align) unsigned char _buffer[k_sbo_size]{};

    void _reset() noexcept {
        if (_ops) {
            _ops->destroy(_buffer, _is_small);
            _ops = nullptr;
            _is_small = false;
        }
    }

    // Construct a decayed target in place from a forwarded initialiser.
    template <typename F>
    void _construct_from(F &&f) {
        using T = std::decay_t<F>;
        if constexpr (k_target_small<T>) {
            _is_small = true;
            ::new (static_cast<void *>(_buffer)) T(std::forward<F>(f));
        } else {
            _is_small = false;
            *static_cast<void **>(static_cast<void *>(_buffer)) = kimix::new_with_allocator<T>(std::forward<F>(f));
        }
        _ops = _ops_for<T>();
    }

    void _move_from(function_base &&other) noexcept {
        if (other._ops) {
            other._ops->move(_buffer, other._buffer, other._is_small, &_is_small);
            _ops = other._ops;
            other._ops = nullptr;
            other._is_small = false;
        }
    }

    void _copy_from(const function_base &other) {
        if (other._ops) {
            other._ops->copy(_buffer, other._buffer, other._is_small, &_is_small);
            _ops = other._ops;
        }
    }

    R _do_invoke(Args... args) const noexcept(IsNoexcept) {
        if (!_ops) {
#if KIMIX_FUNCTION_HAS_EXCEPTIONS
            // std::function throws std::bad_function_call on an empty call.  A
            // noexcept specialization cannot propagate that (the throw would
            // immediately terminate), so for those forms we report+abort instead
            // — matching the exceptions-off behaviour and keeping the build
            // warning-clean (-Wexceptions: throw from a noexcept function).
            if constexpr (IsNoexcept) {
                empty_function_abort();
            } else {
                throw std::bad_function_call();
            }
#else
            empty_function_abort();
#endif
        }
        // The static invoke hook owns const/ref handling; the cast here is
        // internal-only and faithful to what operator() already guaranteed.
        return _ops->invoke(const_cast<void *>(static_cast<const void *>(_buffer)), _is_small,
                            std::forward<Args>(args)...);
    }

protected:
    bool _is_empty() const noexcept { return _ops == nullptr; }

public:
    // Forwarding ctor constraint: a callable, constructible target that is
    // neither this class itself nor an in_place_type_t.  Public so the
    // function_core / move_only_function_core wrappers can build an equally
    // constrained (SFINAE-friendly) forwarding ctor — that keeps
    // std::is_constructible_v<function<Sig>, Bad> == false (ill-formed uses are
    // compile errors / detected as non-constructible; no runtime
    // std::bad_function_call when exceptions are off).
    //
    // Types derived from function_base (any function / move_only_function
    // specialization) are excluded: wrapper-to-wrapper construction and
    // assignment must go through the exact copy/move operations of the derived
    // class, not this forwarding path.  (Without the exclusion, self-assignment
    // f = f would resolve the templated operator= — an identity match beats the
    // base-class copy-assign's derived-to-base conversion — and _reset() would
    // destroy the target before _construct_from() copies from it.  The standard
    // libraries exclude the wrapper types from their operator=(F&&) the same
    // way; as a consequence, cross-specialization construction, e.g.
    // function<int(int)> from function<double(double)>, is not supported.)
    template <typename F>
    static constexpr bool accepts_target() noexcept {
        using T = std::decay_t<F>;
        return !std::is_same_v<T, function_base> && !std::is_base_of_v<function_base, T> &&
               !std::is_same_v<T, std::in_place_type_t<T>> && k_target_acceptable<T>;
    }
    function_base() noexcept = default;
    function_base(std::nullptr_t) noexcept {}

    template <typename F, std::enable_if_t<accepts_target<F>(), int> = 0>
    function_base(F &&f) {
        _construct_from(std::forward<F>(f));
    }

    function_base(const function_base &other) { _copy_from(other); }

    function_base(function_base &&other) noexcept { _move_from(std::move(other)); }

    ~function_base() { _reset(); }

    function_base &operator=(const function_base &other) {
        if (this != &other) {
            _reset();
            _copy_from(other);
        }
        return *this;
    }

    function_base &operator=(function_base &&other) noexcept {
        if (this != &other) {
            _reset();
            _move_from(std::move(other));
        }
        return *this;
    }

    function_base &operator=(std::nullptr_t) noexcept {
        _reset();
        return *this;
    }

    template <typename F, std::enable_if_t<accepts_target<F>(), int> = 0>
    function_base &operator=(F &&f) {
        _reset();
        _construct_from(std::forward<F>(f));
        return *this;
    }

    // Swap needs a move-constructible temporary; for the move-only core this is
    // the natural, exception-free three-move cycle.
    void swap(function_base &other) noexcept {
        if (this != &other) {
            function_base tmp(std::move(other));
            other = std::move(*this);
            *this = std::move(tmp);
        }
    }

    explicit operator bool() const noexcept { return !_is_empty(); }
};

// ---------------------------------------------------------------------------
// function_core — the implementation type behind kimix::function<Sig>.  It
// inherits the (now-public) type-erased operations from function_base and adds
// the operator() carrying the specialization's literal cv/noexcept qualifiers.
// ---------------------------------------------------------------------------
template <typename R, bool IsConst, bool IsNoexcept, typename... Args>
class function_core : public function_base<R, IsConst, RefQual::None, IsNoexcept, false, Args...> {
    using base = function_base<R, IsConst, RefQual::None, IsNoexcept, false, Args...>;

public:
    // Forwarding / copy / move constructors forward to base (the base copy
    // operation is ill-formed at instantiation for a non-copy-constructible
    // target, exactly like std::function).
    function_core() noexcept = default;
    function_core(std::nullptr_t) noexcept : base(nullptr) {}
    template <typename F, std::enable_if_t<base::template accepts_target<F>() &&
                                               !std::is_same_v<std::decay_t<F>, function_core>,
                                           int> = 0>
    function_core(F &&f) : base(std::forward<F>(f)) {}
    function_core(const function_core &other) : base(other) {}
    function_core(function_core &&other) noexcept : base(std::move(other)) {}

    function_core &operator=(std::nullptr_t) noexcept {
        base::operator=(nullptr);
        return *this;
    }
    template <typename F, std::enable_if_t<base::template accepts_target<F>() &&
                                               !std::is_same_v<std::decay_t<F>, function_core>,
                                           int> = 0>
    function_core &operator=(F &&f) {
        base::operator=(std::forward<F>(f));
        return *this;
    }
    function_core &operator=(const function_core &other) {
        base::operator=(other);
        return *this;
    }
    function_core &operator=(function_core &&other) noexcept {
        base::operator=(std::move(other));
        return *this;
    }

    void swap(function_core &other) noexcept { base::swap(other); }
    using base::operator bool;

    // operator() is provided by each function<Sig> specialization (see the
    // KIMIX_DEFINE_FUNCTION macro) with the correct literal cv/noexcept
    // qualifiers, forwarding to base::_do_invoke — a conditional cv-qualifier
    // `const(IsConst)` is not valid C++.
};

// ---------------------------------------------------------------------------
// move_only_function_core — the implementation type behind
// kimix::move_only_function<Sig>.  Move-only (copy deleted), forwarding ctor is
// explicit (per the standard), and operator() carries the specialization's
// literal cv/ref/noexcept qualifiers.
// ---------------------------------------------------------------------------
template <typename R, bool IsConst, RefQual Ref, bool IsNoexcept, typename... Args>
class move_only_function_core
    : public function_base<R, IsConst, Ref, IsNoexcept, true, Args...> {
    using base = function_base<R, IsConst, Ref, IsNoexcept, true, Args...>;

public:
    move_only_function_core() noexcept = default;
    move_only_function_core(std::nullptr_t) noexcept : base(nullptr) {}
    template <typename F, std::enable_if_t<base::template accepts_target<F>() &&
                                               !std::is_same_v<std::decay_t<F>, move_only_function_core>,
                                           int> = 0>
    explicit move_only_function_core(F &&f) : base(std::forward<F>(f)) {}
    move_only_function_core(const move_only_function_core &) = delete;
    move_only_function_core(move_only_function_core &&other) noexcept : base(std::move(other)) {}

    move_only_function_core &operator=(const move_only_function_core &) = delete;
    move_only_function_core &operator=(std::nullptr_t) noexcept {
        base::operator=(nullptr);
        return *this;
    }
    template <typename F, std::enable_if_t<base::template accepts_target<F>() &&
                                               !std::is_same_v<std::decay_t<F>, move_only_function_core>,
                                           int> = 0>
    move_only_function_core &operator=(F &&f) {
        base::operator=(std::forward<F>(f));
        return *this;
    }
    move_only_function_core &operator=(move_only_function_core &&other) noexcept {
        base::operator=(std::move(other));
        return *this;
    }

    void swap(move_only_function_core &other) noexcept { base::swap(other); }
    using base::operator bool;

    // operator() is provided by each move_only_function<Sig> specialization
    // (see the KIMIX_DEFINE_MOVE_ONLY_FUNCTION macro) with the correct literal
    // cv/ref/noexcept qualifiers, forwarding to base::_do_invoke — a conditional
    // cv-qualifier `const(IsConst)` and a templated ref-qualifier are not valid
    // C++, so each form is generated from one template-argument set.
};

} // namespace detail

// ===========================================================================
// kimix::function<Signature>
// ===========================================================================
template <typename Signature>
class function;

// operator() must carry literal cv/noexcept qualifiers, so each of the 4 forms
// is generated from the matching function_core plus its comparison friends.
// std::function's operator() is always const& (const& + const&& in std, but we
// expose only const& which covers both invocations) — the const/noexcept on
// the signature controls how the *stored target* is invoked (via function_base
// subject_t), not the wrapper's own operator() qualification.
#define KIMIX_DEFINE_FUNCTION(QUAL, CONST, NOEX, OPQ)                                                          \
    template <typename R, typename... Args>                                                                    \
    class function<R(Args...) QUAL> : public detail::function_core<R, CONST, NOEX, Args...> {                  \
        using base = detail::function_core<R, CONST, NOEX, Args...>;                                           \
                                                                                                    \
    public:                                                                                                    \
        using base::base;                                                                                      \
        using base::operator=;                                                                                 \
        using base::swap;                                                                                      \
                                                                                                    \
        R operator()(Args... args) const OPQ { return this->_do_invoke(std::forward<Args>(args)...); }         \
                                                                                                    \
        friend bool operator==(const function &f, std::nullptr_t) noexcept { return !static_cast<bool>(f); }    \
        friend bool operator==(std::nullptr_t, const function &f) noexcept { return !static_cast<bool>(f); }    \
        friend bool operator!=(const function &f, std::nullptr_t) noexcept { return static_cast<bool>(f); }     \
        friend bool operator!=(std::nullptr_t, const function &f) noexcept { return static_cast<bool>(f); }     \
    };

//           QUAL             CONST  NOEX  OPQ (extra operator() trailer; always const first)
KIMIX_DEFINE_FUNCTION(, false, false, )
KIMIX_DEFINE_FUNCTION(const, true, false, )
KIMIX_DEFINE_FUNCTION(noexcept, false, true, noexcept)
KIMIX_DEFINE_FUNCTION(const noexcept, true, true, noexcept)

#undef KIMIX_DEFINE_FUNCTION

// ===========================================================================
// kimix::move_only_function<Signature>
// ===========================================================================
template <typename Signature>
class move_only_function;

// 12 forms (cv x ref x noexcept).  The unqualified and && forms invoke the
// stored target as an rvalue (T&& / const T&&); the & forms as lvalues — this
// is the key behavioural difference from function (e.g. move_only_function<void
// () const> rejects a mutable-only target, and <void()&> cannot be invoked on
// an rvalue).
#define KIMIX_DEFINE_MOVE_ONLY_FUNCTION(QUAL, CONST, REF, NOEX, OPQ)                                   \
    template <typename R, typename... Args>                                                            \
    class move_only_function<R(Args...) QUAL>                                                          \
        : public detail::move_only_function_core<R, CONST, detail::RefQual::REF, NOEX, Args...> {      \
        using base = detail::move_only_function_core<R, CONST, detail::RefQual::REF, NOEX, Args...>;    \
                                                                                                       \
    public:                                                                                            \
        using base::base;                                                                              \
        using base::operator=;                                                                         \
        using base::swap;                                                                              \
                                                                                                       \
        R operator()(Args... args) OPQ { return this->_do_invoke(std::forward<Args>(args)...); }       \
                                                                                                       \
        friend bool operator==(const move_only_function &f, std::nullptr_t) noexcept { return !static_cast<bool>(f); } \
        friend bool operator==(std::nullptr_t, const move_only_function &f) noexcept { return !static_cast<bool>(f); } \
        friend bool operator!=(const move_only_function &f, std::nullptr_t) noexcept { return static_cast<bool>(f); }   \
        friend bool operator!=(std::nullptr_t, const move_only_function &f) noexcept { return static_cast<bool>(f); }   \
    };

//           QUAL                 CONST  REF     NOEX  OPQ
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(, false, RValue, false, )
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(noexcept, false, RValue, true, noexcept)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(&, false, LValue, false, &)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(& noexcept, false, LValue, true, & noexcept)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(&&, false, RValue, false, &&)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(&& noexcept, false, RValue, true, && noexcept)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const, true, RValue, false, const)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const noexcept, true, RValue, true, const noexcept)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const &, true, LValue, false, const &)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const & noexcept, true, LValue, true, const & noexcept)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const &&, true, RValue, false, const &&)
KIMIX_DEFINE_MOVE_ONLY_FUNCTION(const && noexcept, true, RValue, true, const && noexcept)

#undef KIMIX_DEFINE_MOVE_ONLY_FUNCTION

} // namespace kimix
