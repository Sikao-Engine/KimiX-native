// Test for stl/functional.h (kimix::function and kimix::move_only_function).
// This test covers:
// - kimix::function: default/nullptr construction and emptiness, construction
//   from function pointers / lambdas / stateful functors, invocation with args
//   and return values, const- and noexcept-qualified specializations, copy and
//   move semantics (incl. self-copy-assign), assignment from callable/nullptr,
//   swap, operator bool, shared vs. deep-copied functor state, large (heap /
//   mimalloc) payloads, nesting inside another function and inside a
//   kimix::vector, void return with multiple args, returning references,
//   == / != against nullptr_t (both operand orders), and the type-trait
//   contract (copyable / nothrow-movable; non-callable and move-only targets
//   rejected).
// - kimix::move_only_function: default/nullptr construction and emptiness,
//   move-only (unique_ptr-capturing) targets, non-copyability, function
//   pointers / stateful / large (mimalloc) payloads, ref-qualified
//   specializations (invoke + std::is_invocable contracts), const-qualified
//   specialization (mutable-only target rejected by is_constructible),
//   noexcept specialization (nothrow-invocable), the full 12-specialization
//   sweep, assignment (callable / nullptr / from-empty move), swap,
//   move-leaves-source-empty, nesting in kimix::vector and inside another
//   move_only_function, and == / != against nullptr_t (both operand orders).
//
// Notes:
// - Test targets build with exceptions ON, but empty-invocation behavior is
//   deliberately NOT exercised (no calling empty wrappers at runtime); all
//   negative cases are compile-time static_asserts.
// - No typeid / dynamic_cast anywhere (RTTI is off in the project).
// - The wrappers are the project's own mimalloc-backed implementation (see
//   src/core/stl/functional.h); negative contracts (non-callable / move-only
//   targets, wrong cv-ref qualifiers) are asserted compile-time only.
#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include <core/stl/functional.h>

#include <array>
#include <memory>
#include <type_traits>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace tests_unit_core_test_stl_functional {

// Plain free functions usable as function-pointer targets.
int add_one(int x) {
    return x + 1;
}

int add_two(int a, int b) {
    return a + b;
}

int ret_zero() {
    return 0;
}

int ret_one() {
    return 1;
}

int noexcept_ret_three() noexcept {
    return 3;
}

kimix::string echo_string(const kimix::string& s) {
    return s;
}

// Identity that deduces its argument type, so it is invocable with any
// allocator-qualified string type (portable across std/kimix::string).
template <typename T>
T echo_any(const T& s) {
    return s;
}

// A global address used by the reference-returning tests.
int g_ref_target = 7;

int& get_ref() {
    return g_ref_target;
}

// Stateful, copyable functor. Each copy keeps an independent state.
struct stateful_functor {
    int value = 0;
    int call_count = 0;
    int operator()(int x) {
        ++call_count;
        value += x;
        return value;
    }
};

// Functor holding a shared_ptr: all copies share one state object.
struct shared_state_functor {
    std::shared_ptr<int> counter = std::make_shared<int>(0);
    int operator()() const {
        ++*counter;
        return *counter;
    }
};

// Const-callable functor — safe for const-qualified specializations.
struct const_functor {
    int base = 10;
    int operator()() const {
        return base;
    }
    int operator()(int x) const {
        return base + x;
    }
};

// Mutable-only functor: operator() is NOT const — must be rejected by
// `...() const` specializations (compile-time contract).
struct mutable_only_functor {
    int n = 0;
    int operator()() {
        return ++n;
    }
};

// Const-callable AND noexcept functor for the noexcept/const sweeps.
struct const_noexcept_functor {
    int operator()() const noexcept {
        return 5;
    }
    int operator()(int x) const noexcept {
        return x;
    }
};

// Functor with a large buffer — forces the heap (mimalloc) storage path.
struct large_functor {
    std::array<char, 256> buf{};
    int operator()(int x) const {
        return x + static_cast<int>(buf.size());
    }
};

// Move-only, const-callable functor (holds a unique_ptr).
struct move_only_functor {
    std::unique_ptr<int> data;
    move_only_functor() : data(std::make_unique<int>(100)) {}
    move_only_functor(const move_only_functor&) = delete;
    move_only_functor& operator=(const move_only_functor&) = delete;
    move_only_functor(move_only_functor&&) noexcept = default;
    move_only_functor& operator=(move_only_functor&&) noexcept = default;
    int operator()() const {
        return *data;
    }
};

}  // namespace tests_unit_core_test_stl_functional
using namespace tests_unit_core_test_stl_functional;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    // ======================================================================
    // kimix::function — construction & emptiness
    // ======================================================================

    "function_default_construct_is_empty"_test = [] {
        function<int(int)> f;
        expect(!f) << "default-constructed function should be empty";
        expect(f == nullptr) << "empty function compares equal to nullptr";
        expect(nullptr == f) << "nullptr == empty function";
        expect(static_cast<bool>(f) == false) << "operator bool is false";
        expect(static_cast<bool>(f != nullptr) == false)
            << "empty function != nullptr is false";
    };

    "function_nullptr_construct_is_empty"_test = [] {
        function<void()> f{nullptr};
        expect(!f) << "function built from nullptr is empty";
        expect(f == nullptr) << "nullptr-constructed function == nullptr";
        function<int(int)> g = nullptr;
        expect(!g) << "copy-init from nullptr is empty";
    };

    "function_from_function_pointer"_test = [] {
        function<int(int)> f(add_one);
        expect(static_cast<bool>(f)) << "function from function pointer is non-empty";
        expect(eq(f(41), 42)) << "invoke function pointer target";
        expect(eq(f(0), 1)) << "second invocation";
        function<int(int, int)> g(add_two);
        expect(eq(g(20, 22), 42)) << "two-arg function pointer target";
    };

    "function_from_lambda"_test = [] {
        function<int(int, int)> f = [](int a, int b) { return a + b; };
        expect(static_cast<bool>(f)) << "function from lambda is non-empty";
        expect(eq(f(2, 3), 5)) << "invoke lambda target";
        expect(eq(f(-7, 7), 0)) << "invoke lambda with negatives";
    };

    "function_from_stateful_functor"_test = [] {
        function<int(int)> f(stateful_functor{});
        expect(static_cast<bool>(f)) << "function from functor is non-empty";
        expect(eq(f(3), 3)) << "first call accumulates";
        expect(eq(f(4), 7)) << "second call accumulates on the stored target";
    };

    "function_void_invocation_with_counter"_test = [] {
        int counter = 0;
        function<void()> f = [&counter] { ++counter; };
        f();
        f();
        f();
        expect(eq(counter, 3)) << "void() side-effect ran three times";
    };

    "function_void_return_multiple_args"_test = [] {
        kimix::string sink;
        function<void(const kimix::string&, int)> f =
            [&](const kimix::string& s, int n) {
                sink = s;
                sink += (n > 0) ? ":pos" : ":nonpos";
            };
        f("val", 1);
        expect(sink == "val:pos") << "void(...) with multiple args captured both";
    };

    "function_string_return_by_value"_test = [] {
        function<kimix::string(const kimix::string&)> f(echo_string);
        kimix::string input = "hello kimix";
        kimix::string out = f(input);
        expect(out == input) << "string round-trips through function (plain fn)";
        expect(not out.empty()) << "returned string is non-empty";
        // generic identity also adapts regardless of string aliasing
        function<kimix::string(const kimix::string&)> g(echo_any<kimix::string>);
        expect(g(input) == input) << "string round-trips through function (template fn)";
        function<kimix::string()> h = [] { return kimix::string{"made"}; };
        expect(h() == "made") << "string-returning lambda round-trips";
    };

    "function_default_constructed_args_by_value"_test = [] {
        // invocation with default-constructed arguments
        function<kimix::string(kimix::string)> f = [](kimix::string s) {
            s += "!";
            return s;
        };
        kimix::string out = f(kimix::string{});
        expect(out == "!") << "default-constructed arg passed by value";
    };

    "function_returning_reference"_test = [] {
        function<int&()> f(get_ref);
        expect(static_cast<bool>(f)) << "reference-returning function is non-empty";
        int& r = f();
        expect(eq(r, 7)) << "returned reference reads the global";
        r = 42;
        expect(eq(g_ref_target, 42)) << "mutation through returned reference is visible";
    };

    // ======================================================================
    // kimix::function — const / noexcept specializations
    // ======================================================================

    "function_const_invocation"_test = [] {
        function<int() const> f(const_functor{});
        // invoke through a const wrapper object
        const function<int() const>& cf = f;
        expect(eq(cf(), 10)) << "const-qualified specialization invokes const functor";
        function<int(int) const> g(const_functor{});
        expect(eq(g(5), 15)) << "const-qualified specialization with args";
    };

    "function_noexcept_specialization"_test = [] {
        function<int() noexcept> f(noexcept_ret_three);
        expect(eq(f(), 3)) << "noexcept function pointer target invokes";
        function<int() noexcept> g = []() noexcept { return 8; };
        expect(eq(g(), 8)) << "noexcept lambda target invokes";
        function<int(int) noexcept> h = [](int x) noexcept { return 2 * x; };
        expect(eq(h(21), 42)) << "noexcept specialization with args";
    };

    // ======================================================================
    // kimix::function — copy / move / assignment / swap / bool
    // ======================================================================

    "function_copy_semantics_independent"_test = [] {
        function<int(int)> f(stateful_functor{});
        function<int(int)> g = f;  // copy
        expect(static_cast<bool>(g)) << "copy is non-empty";
        // each copy holds its own deep-copied functor state
        expect(eq(f(3), 3)) << "original first call";
        expect(eq(f(3), 6)) << "original accumulates independently";
        expect(eq(g(3), 3)) << "copy starts from fresh state";
    };

    "function_copy_empty"_test = [] {
        function<int(int)> empty;
        function<int(int)> copy = empty;
        expect(!copy) << "copy of empty function is empty";
        expect(copy == nullptr) << "copied-empty compares equal to nullptr";
    };

    "function_copy_assign"_test = [] {
        function<int(int)> a(add_one);
        function<int(int)> b = [](int x) { return x * 10; };
        b = a;  // copy-assign
        expect(eq(b(10), 11)) << "assigned copy now calls add_one";
        expect(static_cast<bool>(a)) << "source stays valid after copy-assign";
        expect(eq(a(10), 11)) << "source still callable";
    };

    "function_self_copy_assign_stays_valid"_test = [] {
        function<int(int)> f(add_one);
        f = f;  // self copy-assignment must remain valid
        expect(static_cast<bool>(f)) << "self-assigned function still non-empty";
        expect(eq(f(1), 2)) << "self-assigned function still callable";
    };

    "function_move_semantics"_test = [] {
        function<int(int)> src(add_one);
        function<int(int)> dst = std::move(src);
        expect(static_cast<bool>(dst)) << "move-constructed target is non-empty";
        expect(eq(dst(10), 11)) << "moved function invokes";
        expect(!src) << "move source becomes empty";
        expect(src == nullptr) << "move source compares equal to nullptr";
    };

    "function_move_assign"_test = [] {
        function<int(int)> src(add_one);
        function<int(int)> dst = nullptr;
        dst = std::move(src);
        expect(eq(dst(9), 10)) << "move-assigned target invokes";
        expect(!src) << "move-assign source is empty";
    };

    "function_move_from_empty"_test = [] {
        function<int(int)> empty;
        function<int(int)> dst(add_one);
        dst = std::move(empty);  // move-assign from empty
        expect(!dst) << "moving empty over target yields empty";
        function<int(int)> moved_from_empty{std::move(dst)};
        expect(!moved_from_empty) << "move-construct from empty yields empty";
    };

    "function_assign_from_callable"_test = [] {
        function<int(int)> f;
        f = add_one;
        expect(eq(f(5), 6)) << "assign from function pointer works";
        f = [](int x) { return x * 2; };
        expect(eq(f(5), 10)) << "assign over previous with lambda works";
    };

    "function_assign_from_nullptr"_test = [] {
        function<int(int)> f(add_one);
        expect(static_cast<bool>(f)) << "precondition: non-empty";
        f = nullptr;
        expect(!f) << "assign nullptr makes function empty";
        expect(f == nullptr) << "empty after nullptr-assign compares equal";
    };

    "function_swap_targets"_test = [] {
        function<int(int)> a(add_one);
        function<int(int)> b = [](int x) { return x * 10; };
        a.swap(b);
        expect(eq(a(2), 20)) << "a now holds the lambda target";
        expect(eq(b(10), 11)) << "b now holds add_one";
    };

    "function_swap_with_empty"_test = [] {
        function<int(int)> a(add_one);
        function<int(int)> empty;
        a.swap(empty);
        expect(!a) << "a became empty after swap";
        expect(static_cast<bool>(empty)) << "empty became the non-empty target";
        expect(eq(empty(1), 2)) << "swapped-in target is callable";
    };

    "function_operator_bool_nonempty"_test = [] {
        function<void()> f = [] {};
        expect(static_cast<bool>(f)) << "non-empty function converts to true";
        expect(f != nullptr) << "non-empty function != nullptr";
        expect(nullptr != f) << "nullptr != non-empty function";
    };

    // ======================================================================
    // kimix::function — state sharing, large payloads, nesting, traits
    // ======================================================================

    "function_shared_state_by_pointer"_test = [] {
        function<int()> f(shared_state_functor{});
        function<int()> g = f;  // copies share the same underlying state
        expect(eq(f(), 1)) << "first call increments shared counter";
        expect(eq(g(), 2)) << "copy sees shared counter increment";
        expect(eq(f(), 3)) << "original sees the shared increment too";
    };

    "function_deep_copy_independent_state"_test = [] {
        stateful_functor src;
        src.value = 0;
        function<int(int)> f(src);
        function<int(int)> g = f;  // deep copy: independent functor state
        expect(eq(f(5), 5)) << "original accumulates";
        expect(eq(f(5), 10)) << "original keeps accumulating";
        expect(eq(g(1), 1)) << "copy is independent (fresh value)";
    };

    "function_large_payload_heap_path"_test = [] {
        // large_functor holds a 256-byte buffer -> exercises the mimalloc heap path
        function<int(int)> f(large_functor{});
        expect(eq(f(1), 257)) << "large payload target invokes";
        function<int(int)> g = f;  // copy the heap-stored payload
        expect(eq(g(2), 258)) << "copied large payload invokes";
        expect(static_cast<bool>(f)) << "original still valid after copy";
        kimix::vector<function<int(int)>> v;
        v.emplace_back(large_functor{});  // copy into a vector, destroy the original
        expect(eq(v[0](3), 259)) << "large payload survives a copy into a container";
    };

    "function_nested_in_another_function"_test = [] {
        function<function<int(int)>(int)> outer = [](int seed) {
            return [seed](int x) { return x + seed; };
        };
        function<int(int)> inner = outer(100);
        expect(eq(inner(1), 101)) << "nested function returned and invoked";
        expect(eq(outer(5)(5), 10)) << "inline nested invocation";
    };

    "function_stored_in_vector"_test = [] {
        kimix::vector<function<int(int)>> funcs;
        funcs.push_back(add_one);
        funcs.emplace_back([](int x) { return x * 3; });
        funcs.push_back(stateful_functor{});
        expect(eq(funcs.size(), 3_u)) << "three functions stored";
        expect(eq(funcs[0](4), 5)) << "function pointer target in vector";
        expect(eq(funcs[1](4), 12)) << "lambda target in vector";
        expect(static_cast<bool>(funcs[2])) << "functor target in vector is callable";
        int sum = 0;
        for (auto& fn : funcs) {
            if (fn) sum += fn(2);
        }
        expect(sum > 0) << "loop invocation produced a sum";
    };

    "function_static_asserts"_test = [] {
        using F = function<int(int)>;
        using V = function<void()>;
        static_assert(std::is_default_constructible_v<V>,
                      "function is default-constructible");
        static_assert(std::is_copy_constructible_v<V>, "function is copy-constructible");
        static_assert(std::is_copy_assignable_v<V>, "function is copy-assignable");
        static_assert(std::is_move_constructible_v<V>, "function is move-constructible");
        static_assert(std::is_move_assignable_v<V>, "function is move-assignable");
        static_assert(std::is_nothrow_move_constructible_v<V>,
                      "function move-construct is noexcept");
        static_assert(std::is_constructible_v<F, int (*)(int)>,
                      "constructible from matching function pointer");
        static_assert(std::is_invocable_r_v<int, F, int>,
                      "matching call returns int");
        // non-callable target rejected
        static_assert(!std::is_constructible_v<F, void*>,
                      "function not constructible from void*");
        // move-only (non-copyable) target rejected for kimix::function
        static_assert(!std::is_constructible_v<V, std::unique_ptr<int>>,
                      "function rejects move-only target");
        expect(true) << "function static_assert contract compiled";
    };

    // ======================================================================
    // kimix::move_only_function — construction & emptiness
    // ======================================================================

    "move_only_function_default_construct_is_empty"_test = [] {
        move_only_function<int(int)> f;
        expect(!f) << "default-constructed move_only_function is empty";
        expect(f == nullptr) << "empty move_only_function == nullptr";
        expect(nullptr == f) << "nullptr == empty move_only_function";
        expect(static_cast<bool>(f) == false) << "operator bool is false";
    };

    "move_only_function_nullptr_construct_is_empty"_test = [] {
        move_only_function<void()> f{nullptr};
        expect(!f) << "nullptr-constructed move_only_function is empty";
        expect(f == nullptr) << "empty after nullptr ctor";
    };

    "move_only_function_from_unique_ptr_lambda"_test = [] {
        auto make = [] {
            return [p = std::make_unique<int>(21)]() { return *p * 2; };
        };
        move_only_function<int()> f(make());
        expect(static_cast<bool>(f)) << "move-only target is non-empty";
        expect(eq(f(), 42)) << "invoke dereferences captured unique_ptr";
        move_only_function<int()> g = std::move(f);
        expect(eq(g(), 42)) << "move-constructed target invokes";
        expect(!f) << "source empty after move-construct";
        move_only_function<int()> h;
        h = std::move(g);
        expect(eq(h(), 42)) << "move-assigned target invokes";
        expect(!g) << "source empty after move-assign";
    };

    "move_only_function_from_function_pointer"_test = [] {
        move_only_function<int(int)> f(add_one);
        expect(eq(f(9), 10)) << "invoke function-pointer target";
        move_only_function<int(int, int)> g(add_two);
        expect(eq(g(20, 22), 42)) << "invoke two-arg function-pointer target";
    };

    "move_only_function_from_stateful_functor"_test = [] {
        move_only_function<int()> f(move_only_functor{});
        expect(eq(f(), 100)) << "invoke move-only stateful functor";
        move_only_function<int(int)> g(stateful_functor{});
        expect(eq(g(3), 3)) << "invoke stateful functor through rvalue-qualified path";
    };

    "move_only_function_large_payload"_test = [] {
        // 256-byte buffer target -> exercises the mimalloc heap storage path
        move_only_function<int(int)> f(large_functor{});
        expect(eq(f(1), 257)) << "large (heap / mimalloc) payload invokes";
        move_only_function<int(int)> g = std::move(f);
        expect(eq(g(2), 258)) << "moved large payload invokes";
        expect(!f) << "source empty after move";
    };

    "move_only_function_noncopyable_static_asserts"_test = [] {
        using MF = move_only_function<int()>;
        static_assert(!std::is_copy_constructible_v<MF>,
                      "move_only_function is not copy-constructible");
        static_assert(!std::is_copy_assignable_v<MF>,
                      "move_only_function is not copy-assignable");
        static_assert(std::is_move_constructible_v<MF>,
                      "move_only_function is move-constructible");
        static_assert(std::is_move_assignable_v<MF>,
                      "move_only_function is move-assignable");
        static_assert(std::is_nothrow_move_constructible_v<MF>,
                      "move_only_function move-construct is noexcept");
        static_assert(std::is_nothrow_move_assignable_v<MF>,
                      "move_only_function move-assign is noexcept");
        expect(true) << "move_only_function trait contract compiled";
    };

    // ======================================================================
    // kimix::move_only_function — ref / cv / noexcept specializations
    // ======================================================================

    "move_only_function_unqualified_callable_on_lvalue_and_rvalue"_test = [] {
        using F = move_only_function<int()>;
        static_assert(std::is_invocable_v<F&>, "unqualified: invocable on lvalue");
        static_assert(std::is_invocable_v<F&&>, "unqualified: invocable on rvalue");
        F f(ret_zero);
        expect(eq(f(), 0)) << "call on lvalue";
        F built(ret_one);
        expect(eq(std::move(built)(), 1)) << "call on rvalue";
    };

    "move_only_function_lvalue_ref_qualifier"_test = [] {
        using F = move_only_function<int()&>;
        static_assert(std::is_invocable_v<F&>, "&: invocable on lvalue");
        static_assert(!std::is_invocable_v<F&&>, "&: not invocable on rvalue");
        F f(ret_zero);
        expect(eq(f(), 0)) << "lvalue-qualified call works";
        // constructing from an rvalue target is fine; only *calling* is lvalue-only
        F g(const_functor{});
        expect(eq(g(), 10)) << "lvalue-qualified wrapper from rvalue target";
    };

    "move_only_function_rvalue_ref_qualifier"_test = [] {
        using F = move_only_function<int()&&>;
        static_assert(std::is_invocable_v<F&&>, "&&: invocable on rvalue");
        static_assert(!std::is_invocable_v<F&>, "&&: not invocable on lvalue");
        F f(ret_zero);
        expect(eq(std::move(f)(), 0)) << "rvalue-qualified call via std::move";
    };

    "move_only_function_const_invocation"_test = [] {
        move_only_function<int() const> f(const_functor{});
        expect(eq(f(), 10)) << "const-qualified specialization invokes const functor";
        // a mutable-only functor must be rejected by a const-qualified signature
        static_assert(!std::is_constructible_v<move_only_function<int() const>,
                                               mutable_only_functor>,
                      "const specialization rejects mutable-only functor");
        static_assert(std::is_constructible_v<move_only_function<int() const>,
                                              const_functor>,
                      "const specialization accepts const-callable functor");
    };

    "move_only_function_noexcept_specialization"_test = [] {
        using F = move_only_function<int() noexcept>;
        static_assert(std::is_nothrow_invocable_v<F&>,
                      "noexcept specialization is nothrow-invocable");
        F f(const_noexcept_functor{});
        expect(eq(f(), 5)) << "noexcept specialization invokes";
        using G = move_only_function<int(int) noexcept>;
        static_assert(std::is_nothrow_invocable_v<G&, int>,
                      "noexcept specialization with args is nothrow-invocable");
        G g{[](int x) noexcept { return 2 * x; }};  // direct-init (forwarding ctor is explicit)
        expect(eq(g(21), 42)) << "noexcept specialization with args invokes";
    };

    "move_only_function_all_twelve_specializations"_test = [] {
        // Build each of the 12 specializations of int(int) and invoke it.
        move_only_function<int(int)> plain(const_noexcept_functor{});
        move_only_function<int(int) const> cq(const_noexcept_functor{});
        move_only_function<int(int)&> lq(const_noexcept_functor{});
        move_only_function<int(int) const&> clq(const_noexcept_functor{});
        move_only_function<int(int)&&> rq(const_noexcept_functor{});
        move_only_function<int(int) const&&> crq(const_noexcept_functor{});
        move_only_function<int(int) noexcept> ne(const_noexcept_functor{});
        move_only_function<int(int) const noexcept> cne(const_noexcept_functor{});
        move_only_function<int(int)& noexcept> lne(const_noexcept_functor{});
        move_only_function<int(int) const& noexcept> clne(const_noexcept_functor{});
        move_only_function<int(int)&& noexcept> rne(const_noexcept_functor{});
        move_only_function<int(int) const&& noexcept> crne(const_noexcept_functor{});

        bool all_ok = true;
        all_ok = all_ok && (plain(1) == 1);
        all_ok = all_ok && (cq(2) == 2);
        all_ok = all_ok && (lq(3) == 3);
        all_ok = all_ok && (clq(4) == 4);
        all_ok = all_ok && (std::move(rq)(5) == 5);
        all_ok = all_ok && (std::move(crq)(6) == 6);
        all_ok = all_ok && (ne(7) == 7);
        all_ok = all_ok && (cne(8) == 8);
        all_ok = all_ok && (lne(9) == 9);
        all_ok = all_ok && (clne(10) == 10);
        all_ok = all_ok && (std::move(rne)(11) == 11);
        all_ok = all_ok && (std::move(crne)(12) == 12);
        expect(all_ok) << "all 12 cv/ref/noexcept specializations instantiate and invoke";
        using plain_t = move_only_function<int(int)>;
        using ne_t = move_only_function<int(int) noexcept>;
        static_assert(std::is_nothrow_invocable_v<ne_t&, int>,
                      "noexcept specialization is nothrow-invocable");
        static_assert(!std::is_nothrow_invocable_v<plain_t&, int>,
                      "plain specialization is not required to be nothrow-invocable");
    };

    // ======================================================================
    // kimix::move_only_function — assignment / swap / bool / nesting
    // ======================================================================

    "move_only_function_assign_from_callable"_test = [] {
        move_only_function<int(int)> f;
        f = add_one;
        expect(eq(f(5), 6)) << "assign from function pointer";
        f = [p = std::make_unique<int>(2)](int x) { return x * *p; };
        expect(eq(f(5), 10)) << "assign move-only lambda over previous target";
    };

    "move_only_function_assign_from_nullptr"_test = [] {
        move_only_function<int(int)> f(add_one);
        expect(static_cast<bool>(f)) << "precondition: non-empty";
        f = nullptr;
        expect(!f) << "assign nullptr empties the wrapper";
        expect(f == nullptr) << "empty after nullptr-assign compares equal";
    };

    "move_only_function_move_assign_from_empty"_test = [] {
        move_only_function<int()> f(ret_zero);
        expect(static_cast<bool>(f)) << "precondition: non-empty";
        move_only_function<int()> empty;
        f = std::move(empty);
        expect(!f) << "move-assign from empty leaves target empty";
    };

    "move_only_function_swap_targets"_test = [] {
        move_only_function<int()> a(ret_one);
        move_only_function<int()> b(const_functor{});
        a.swap(b);
        expect(eq(a(), 10)) << "a took b's const-functor target";
        expect(eq(b(), 1)) << "b took a's ret_one target";
    };

    "move_only_function_swap_with_empty"_test = [] {
        move_only_function<int()> a(ret_one);
        move_only_function<int()> empty;
        a.swap(empty);
        expect(!a) << "a became empty after swap";
        expect(static_cast<bool>(empty)) << "empty took the target";
        expect(eq(empty(), 1)) << "swapped-in target invokes";
    };

    "move_only_function_equality_with_nullptr"_test = [] {
        move_only_function<int()> f;
        expect(f == nullptr) << "empty == nullptr";
        expect(nullptr == f) << "nullptr == empty";
        expect(!(f != nullptr)) << "empty != nullptr is false";
        expect(!(nullptr != f)) << "nullptr != empty is false";
        move_only_function<int()> g(ret_one);
        expect(g != nullptr) << "non-empty != nullptr";
        expect(nullptr != g) << "nullptr != non-empty";
        expect(!(g == nullptr)) << "non-empty == nullptr is false";
    };

    "move_only_function_move_leaves_source_empty"_test = [] {
        move_only_function<int()> a(ret_one);
        move_only_function<int()> b = std::move(a);
        expect(static_cast<bool>(b)) << "destination holds the target";
        expect(!a) << "source empty after move";
        expect(a == nullptr) << "moved-from source == nullptr";
    };

    "move_only_function_nested_in_another"_test = [] {
        // The outer's call result must be *convertible* to the wrapper's return
        // type, and move_only_function's forwarding constructor is explicit — so
        // the lambda returns move_only_function<int()> directly (direct-init
        // inside the body), which is the standard-conformant nesting pattern
        // (std::is_invocable_r_v agrees; a raw move-only lambda return would
        //  need an implicit conversion and is ill-formed for both std and kimix).
        using inner_fn = move_only_function<int()>;
        move_only_function<inner_fn(int)> outer{
            [](int seed) -> inner_fn {
                return inner_fn{[p = std::make_unique<int>(seed)]() { return *p; }};
            }};
        inner_fn inner = outer(9);  // move-ctor (non-explicit)
        expect(eq(inner(), 9)) << "inner move_only_function (move-only capture) invokes";
    };

    "move_only_function_stored_in_vector"_test = [] {
        kimix::vector<move_only_function<int(int)>> funcs;
        funcs.emplace_back(add_one);
        funcs.emplace_back([](int x) { return x - 1; });
        funcs.emplace_back(
            [p = std::make_unique<int>(1000)](int x) { return x + *p; });
        expect(eq(funcs.size(), 3_u)) << "three move-only functions stored";
        expect(eq(funcs[0](1), 2)) << "first invokes";
        expect(eq(funcs[1](5), 4)) << "second invokes";
        expect(eq(funcs[2](1), 1001)) << "third (move-only lambda) invokes";
        int sum = 0;
        for (auto& fn : funcs) {
            if (fn) sum += fn(0);
        }
        expect(sum > 0) << "loop invocation over vector produced a sum";
    };

    return 0;
}
