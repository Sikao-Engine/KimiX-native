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

namespace kimix {

template<typename Signature>
class SharedFunction;

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
            // Allocated via mi_malloc_aligned_at() → a plain mi_free() releases it.
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
        // A null block from mimalloc means OOM: kimix has no exceptions, so
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

}// namespace kimix
