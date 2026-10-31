/*
 * shared_function.h — kimix::SharedFunction<Sig>, a ref-counted callable.
 *
 * Why this exists: marl::schedule() (and therefore every kimix::fiber task
 * submission) stores its callback in a copy-constructible function wrapper, but
 * the useful task closures are not copyable — they capture a fiber wait-group
 * handle plus a non-copyable atomic counter. std::function would then refuse to
 * compile, and kimix cannot use exceptions to report that. SharedFunction fixes
 * it the same way luisa does: the closure is allocated once (mimalloc), held
 * behind an atomic ref-count, and copying a SharedFunction is just a ref-count
 * bump — so a SharedFunction<void()> is cheap to copy into every scheduled task
 * while the captured state stays shared.
 *
 *   kimix::SharedFunction<void(uint32_t)> f{[n](uint32_t i) noexcept { ... }};
 *   auto g = f;   // same target, ref == 2
 *   g(0);         // invokes f's closure
 *
 * Not thread-safe for RESEATING the same object (copy/assign from several worker
 * threads at once) — the ref-count is atomic, the wrapper itself is not. Invoking
 * a shared target from many fibers at a time is exactly what it is for.
 *
 * TWO LEGS
 * standalone     the implementation below: one block from the kimix memory
 *                gateway (core/memory.h → mimalloc), an atomic ref-count, the
 *                callable in place.
 * third-party    luisa::SharedFunction (LC's, same design, LC's allocator) with a
 *                thin kimix wrapper that keeps the two things LC does not have:
 *                `explicit operator bool()` and the report-and-abort path when an
 *                EMPTY wrapper is invoked (LC documents that as undefined: it only
 *                has LUISA_ASSUME). Nothing else differs — both legs allocate the
 *                closure once and copy by ref-count bump, so move-only captures
 *                work in both.
 *
 * Header-only. Lives outside the kimix_core.h umbrella (include it directly).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "stl/memory.h"

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/shared_function.h>
#endif

namespace kimix {

template<typename Signature>
class SharedFunction;

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

/// Ref-counted type-erased callable for Signature = Ret(Args...) — LC's
/// implementation PLUS kimix's emptiness contract (see the file comment).
///
/// It HOLDS a luisa::SharedFunction rather than deriving from it, and that is
/// not cosmetic: LC's converting constructor is
///     template<typename F> requires(!is_same_v<remove_cvref_t<F>, SharedFunction>)
///                                                 && is_invocable_r_v<Ret, F, Args&&...>
/// so a DERIVED type is still "some other callable" — the template is an exact
/// match for it and beats the copy constructor, i.e. copying a derived wrapper
/// would wrap it again, recursively (measured: instant stack overflow). Holding
/// the member makes every copy go through member-to-member copy construction,
/// where the argument type IS luisa::SharedFunction and the template is excluded.
///
/// `_engaged` mirrors LC's private "_base != nullptr"; every constructor and
/// assignment below keeps the two in step, so `operator bool()` and the
/// report-and-abort path behave exactly like the standalone implementation.
template<typename Ret, typename... Args>
class SharedFunction<Ret(Args...)> {

    using base_type = luisa::SharedFunction<Ret(Args...)>;

public:
    SharedFunction() noexcept = default;

    SharedFunction(SharedFunction const &rhs) noexcept = default;

    SharedFunction(SharedFunction &&rhs) noexcept
        : _fn{std::move(rhs._fn)}, _engaged{rhs._engaged} {
        rhs._engaged = false;
    }

    SharedFunction &operator=(SharedFunction const &rhs) noexcept = default;

    SharedFunction &operator=(SharedFunction &&rhs) noexcept {
        if (std::addressof(rhs) == this) [[unlikely]] { return *this; }
        _fn = std::move(rhs._fn);
        _engaged = rhs._engaged;
        rhs._engaged = false;
        return *this;
    }

    /// Wrap any callable invocable as Ret(Args&&...).
    template<typename F>
        requires((!std::is_same_v<std::remove_cvref_t<F>, SharedFunction>) &&
                 (std::is_invocable_r_v<Ret, F, Args &&...>))
    SharedFunction(F &&f) noexcept
        : _fn{std::forward<F>(f)}, _engaged{true} {}

    /// Wrap a plain (captured-less) function pointer.
    template<typename FuncPtr_Ret, typename... FuncPtr_Args>
        requires(std::is_invocable_r_v<Ret, FuncPtr_Ret (*)(FuncPtr_Args...), Args &&...>)
    SharedFunction(FuncPtr_Ret (*func_ptr)(FuncPtr_Args...)) noexcept
        : _fn{func_ptr}, _engaged{true} {}

    /// Invoke the wrapped callable. An empty (default-constructed) wrapper is a
    /// programming error: report and abort (no exceptions in kimix) — the same
    /// contract the standalone implementation documents.
    Ret operator()(Args... args) const noexcept {
        if (!_engaged) [[unlikely]] {
            std::fputs("kimix: call of an empty SharedFunction; aborting\n", stderr);
            std::fflush(stderr);
            std::abort();
        }
        return _fn(std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return _engaged; }

private:
    base_type _fn;
    bool _engaged = false;
};

#else

/// Ref-counted type-erased callable for Signature = Ret(Args...).
template<typename Ret, typename... Args>
class SharedFunction<Ret(Args...)> {

    struct Base {
        using dtor_type = void (*)(void *);
        std::atomic_uint32_t ref{1u};
        std::uint32_t align{};
        dtor_type dtor{nullptr};
    };
    Base *_base;
    Ret (*_func_ptr)(void *, Args &&...);

    void _dispose() noexcept {
        if (!_base) return;
        if (--_base->ref == 0u) {
            if (_base->dtor) { _base->dtor(_base); }
            // Allocated via kimix::allocate_aligned_at() → kimix::deallocate() releases it.
            detail::allocator_deallocate(_base);
        }
        _base = nullptr;
    }

public:
    SharedFunction() noexcept : _base{nullptr}, _func_ptr{} {}

    ~SharedFunction() noexcept { _dispose(); }

    SharedFunction(SharedFunction const &rhs) noexcept : _base{rhs._base}, _func_ptr{rhs._func_ptr} {
        if (_base) { ++_base->ref; }
    }
    SharedFunction(SharedFunction &&rhs) noexcept : _base{rhs._base}, _func_ptr{rhs._func_ptr} {
        rhs._base = nullptr;
    }
    SharedFunction &operator=(SharedFunction const &rhs) noexcept {
        if (std::addressof(rhs) == this) [[unlikely]] { return *this; }
        _dispose();
        _base = rhs._base;
        _func_ptr = rhs._func_ptr;
        if (_base) { ++_base->ref; }
        return *this;
    }
    SharedFunction &operator=(SharedFunction &&rhs) noexcept {
        if (std::addressof(rhs) == this) [[unlikely]] { return *this; }
        _dispose();
        _base = rhs._base;
        _func_ptr = rhs._func_ptr;
        rhs._base = nullptr;
        return *this;
    }

    /// Wrap any callable invocable as Ret(Args&&...).
    template<typename F>
        requires((!std::is_same_v<std::remove_cvref_t<F>, SharedFunction>) && (std::is_invocable_r_v<Ret, F, Args...>))
    SharedFunction(F &&f) noexcept {
        using Func = std::remove_cvref_t<F>;
        struct Derived : Base {
            Func func;
        };
        auto *derived = static_cast<Derived *>(detail::allocator_allocate(sizeof(Derived), alignof(Derived)));
        // A null block from the gateway means OOM: kimix has no exceptions, so
        // report and abort exactly like kimix::allocator does.
        if (derived == nullptr) [[unlikely]] { allocation_failure(); }
        new (derived) Derived{Base{1u, static_cast<std::uint32_t>(alignof(Derived)), nullptr}, Func(std::forward<F>(f))};
        _base = derived;
        if constexpr (!std::is_trivially_destructible_v<Func>) {
            _base->dtor = [](void *p) noexcept -> void {
                std::destroy_at(reinterpret_cast<Derived *>(p));
            };
        }
        _func_ptr = [](void *p, Args &&...args) noexcept -> Ret {
            auto &func = reinterpret_cast<Derived *>(p)->func;
            if constexpr (std::is_same_v<Ret, void>) {
                func(std::forward<Args>(args)...);
            } else {
                return func(std::forward<Args>(args)...);
            }
        };
    }

    /// Wrap a plain (captured-less) function pointer.
    template<typename FuncPtr_Ret, typename... FuncPtr_Args>
        requires(std::is_invocable_r_v<Ret, FuncPtr_Ret (*)(FuncPtr_Args...), Args...>)
    SharedFunction(FuncPtr_Ret (*func_ptr)(FuncPtr_Args...)) noexcept {
        using FuncPtr = decltype(func_ptr);
        struct Derived : Base {
            FuncPtr ptr;
        };
        auto *derived = static_cast<Derived *>(detail::allocator_allocate(sizeof(Derived), alignof(Derived)));
        if (derived == nullptr) [[unlikely]] { allocation_failure(); }
        new (derived) Derived{Base{1u, static_cast<std::uint32_t>(alignof(Derived)), nullptr}, func_ptr};
        _base = derived;
        _func_ptr = [](void *p, Args &&...args) noexcept -> Ret {
            auto func = reinterpret_cast<Derived *>(p)->ptr;
            if constexpr (std::is_same_v<Ret, void>) {
                func(std::forward<Args>(args)...);
            } else {
                return func(std::forward<Args>(args)...);
            }
        };
    }

    /// Reseating a live wrapper goes through construction + move-assignment:
    /// `f = kimix::SharedFunction<void()>{[...]{...}}`. A templated `operator=`
    /// taking any callable would fight with the copy/move assignment overloads
    /// (std::is_invocable_r_v is true for the wrapper itself), so there is none.

    /// Invoke the wrapped callable. An empty (default-constructed) wrapper is a
    /// programming error: report and abort (no exceptions in kimix).
    Ret operator()(Args... args) const noexcept {
        if (_base == nullptr) [[unlikely]] {
            std::fputs("kimix: call of an empty SharedFunction; aborting\n", stderr);
            std::fflush(stderr);
            std::abort();
        }
        if constexpr (std::is_same_v<Ret, void>) {
            _func_ptr(_base, std::forward<Args>(args)...);
        } else {
            return _func_ptr(_base, std::forward<Args>(args)...);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept { return _base != nullptr; }
};

#endif // KIMIX_EXTERNAL_THIRDPARTY

}// namespace kimix
