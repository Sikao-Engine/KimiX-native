---
name: pybind
description: pybind11 3.0.x usage guide for this repo's runtime_py Python bindings. Use when writing or editing pybind11 C++ binding code (e.g. src/runtime/py/*.cpp) — module/class/function binding, STL/bytes casts, GIL, exceptions.
---

# pybind11 — Python Binding Reference

**Vendored version: pybind11 3.0.2a0** (`src/ext/pybind11/include/pybind11/detail/common.h:20-30`), unmodified upstream (`src/ext/` is DO NOT EDIT). It is a header-only xmake target, `kimix-pybind11` (`src/ext/xmake.lua:67-74`), declared as a dep of `kimix-core` — every third-party dep is declared there and other targets inherit it (`src/xmake.lua:15-21`), so `runtime_py` compiles against it transitively. Behaviour notes below are checked against that tree, not against pybind11 2.x.

Namespace `pybind11`, conventionally aliased `namespace py = pybind11;`. Header-only; include what you use:

`pybind11.h` (core) · `attr.h` (annotations) · `cast.h` (casting, `py::arg`) · `pytypes.h` (Python object wrappers) · `stl.h` / `stl_bind.h` (STL) · `numpy.h` · `gil.h` / `gil_simple.h` / `critical_section.h` · `eval.h` · `embed.h` · `functional.h` · `chrono.h` · `complex.h` · `options.h` · `iostream.h` · `operators.h` · `native_enum.h` · `typing.h` · `warnings.h` · `type_caster_pyobject_ptr.h` · `subinterpreter.h` · `conduit/`.

## 1. Module setup

```cpp
#include <pybind11/pybind11.h>
namespace py = pybind11;

PYBIND11_MODULE(runtime_py, m) { // ONE PYBIND11_MODULE per extension (module.cpp)
    m.doc() = "module docstring";
    m.def("add", [](int a, int b) { return a + b; }, "adds two ints",
          py::arg("a"), py::arg("b"));
    m.attr("VERSION") = 42; // module attribute
    auto sub = m.def_submodule("text", "doc"); // submodule
    m.add_object("obj", py::cast(some_value)); // attach arbitrary object
    py::module_::import("sys"); // import existing module
}
```

- `module_::import(name)` throws `py::error_already_set` on failure; `import("sys").attr("stdout")` calls Python from C++ (real use: `py_print.cpp:117` compares `file is sys.stdout`, then falls back to `builtins.print` via `PyObject_Call`). JSON is **not** decoded by importing Python's `json` — the kernels parse it with yyjson in C++.
- `m.def_submodule(name, doc)` returns a `module_&` you can keep registering into.
- `m.doc()` is read/write (`__doc__`).
- 3.x also accepts module options as a third macro argument — `PYBIND11_MODULE(name, m, py::mod_gil_not_used())` (`pybind11.h:1339,1615`). `runtime_py` does **not** use it: the extension is always called with the GIL held and never runs Python code from a detached thread.

## 2. Functions & arguments

```cpp
m.def("f", &free_func, py::arg("x"));            // free function or lambda
m.def("f", [](py::object o) { return o; },
      py::arg("x") = 5,                          // default value (-> py::arg_v)
      py::arg("y") = py::none(),                 // optional arg
      py::arg("z").noconvert(),                  // forbid implicit conversion
      py::kw_only(),                             // following args keyword-only
      py::pos_only(),                            // previous args positional-only
      py::return_value_policy::copy,             // return policy override
      py::call_guard<py::gil_scoped_release>(),  // release GIL during call
      py::keep_alive<0, 1>(),                    // return keeps arg #1 alive (0=return)
      py::doc("help text"));                     // docstring
```

- Overloads: `m.def("f", py::overload_cast<int>(&C::f)); m.def("f", py::overload_cast<double>(&C::f));` — const member: `py::overload_cast<int>(&C::f, py::const_)`.
- `return_value_policy`: `automatic` (default), `automatic_reference`, `take_ownership`, `copy`, `move`, `reference`, `reference_internal`.
- Default args are also written as plain positional defaults after `py::arg(...)` in `.def(py::init<...>(), ...)` chains; keep the docstring as a plain string literal argument (real example: `LineProcessor`'s factory init + 7 defaulted `py::arg`s, `py_stream.cpp:91-110`).
- 3.x renders integral parameters as `typing.SupportsInt | typing.SupportsIndex` in `__doc__` signatures and names object parameters `object` (e.g. `count_tokens(data: bytes, model: object = None)`).

## 3. Classes, properties, enums

```cpp
py::class_<Pet, std::shared_ptr<Pet>>(m, "Pet", "docstring")
    .def(py::init<const std::string &>())         // bind a constructor
    .def("setName", &Pet::setName, py::arg("name"))
    .def_static("create", &Pet::create)
    .def_readwrite("name", &Pet::name)            // public member
    .def_readonly("id", &Pet::id)
    .def_property("weight", &Pet::getWeight, &Pet::setWeight)
    .def_property_readonly("age", &Pet::getAge)   // default reference_internal
    .def_property_readonly_static("MAX", [](py::object) { return Pet::MAX; })
    .def("__repr__", [](const Pet& p) { return "<Pet>"; })
    .def(py::pickle(                              // __getstate__ / __setstate__
        [](const Pet& p) { return py::make_tuple(p.name); },
        [](py::tuple t) { return new Pet(t[0].cast<std::string>()); }))
    .def(py::init([](const std::string &n) { return new Pet(n); })); // factory init
```

The real analogue in this repo is `LineProcessor`: a `py::class_` whose `.def(py::init([](...){...}))` factory fills an options struct and returns the object by value, with the docstring + 7 defaulted `py::arg`s after the lambda (`py_stream.cpp:91-110`).

- Template args: holder (`std::unique_ptr<T>` default, `std::shared_ptr<T>`, `py::smart_holder`), alias/trampoline, bases for inheritance: `py::class_<Derived, Base, std::shared_ptr<Derived>>`.
- Class extras: `py::dynamic_attr()`, `py::multiple_inheritance`, `py::module_local()`, `py::is_final`, `py::buffer_protocol()`, ~~`py::metaclass(...)`~~ (deprecated no-op unless given a custom metaclass handle — `attr.h:89-97`).
- Operators: `.def(py::self + py::self)` (also `- * / % << >> & | ^ == != < <= > >=` and in-place `+=` etc.), unary `py::neg(py::self)`, `py::pos`, `py::abs`, `py::hash`.
- Trampolines (virtuals overridable from Python): define `class PyAnimal : public Animal { using Animal::Animal; std::string go(int n) override { PYBIND11_OVERRIDE_PURE(std::string, Animal, go, n); } };` then `py::class_<Animal, PyAnimal>(m, "Animal")`. `PYBIND11_OVERRIDE_NAME(ret, cls, pyName, fn, ...)` when names differ. In 3.x an alias bound with `py::smart_holder` must additionally inherit `pybind11::trampoline_self_life_support` (static_assert at `pybind11.h:2144-2148`). Nothing in `src/runtime/py/` binds a virtual or a trampoline — every bound kernel type is non-polymorphic (RTTI is compiled out project-wide, see §11).

```cpp
py::enum_<Color>(m, "Color", py::arithmetic())
    .value("RED", Color::RED, "doc")
    .export_values(); // also exposes RED at module scope
```

Enums are bound as plain ints here: `py::enum_` / `py::native_enum` appear 0 times in `src/runtime/py/` (e.g. `LineProcessor(dedup_mode=0)` and `part_kind` bridge spans take `uint32_t`/`int64_t`), and `native_enum.h` is not included by any binding TU.

## 4. Python object types & casting (pytypes.h)

```cpp
py::object o = py::cast(42);              // C++ -> Python
int i = obj.cast<int>();                  // Python -> C++ (throws cast_error)
py::str s("hi");  py::int_ i(42);  py::float_ f(1.5);  py::bool_ b(true);
py::bytes by(ptr, len);  py::list l; l.append(v);
py::dict d; d["k"] = v;  d.contains("k");
py::tuple t(3); t[0] = x;  py::set st; st.add("x");
py::none();  py::make_tuple(a, b, c);     // tuple from values
py::isinstance<py::str>(obj);  py::isinstance(obj, py::type::of(other));
py::hasattr(obj, "attr");  py::getattr(obj, "name", py::none());  py::setattr(obj, "n", v);
py::len(obj);  py::print(args...);
obj.attr("method")(arg1, arg2);           // call Python callable
obj.attr("get")("key", py::none());       // dict.get pattern
py::reinterpret_borrow<py::dict>(h);      // borrow a ref
py::reinterpret_steal<py::str>(obj);      // take ownership of a new ref
```

- `handle` (borrowed): `ptr()`, `cast<T>()`, `operator bool`. `object` (owned): `release()`, auto refcount.
- `py::str(ptr, size)` / `py::bytes(ptr, size)` build from (data, len); `py::str(s)` also from `std::string`.
- Dict iteration yields `(key, value)` pairs: `for (auto item : d) { handle k = item.first; handle v = item.second; }`.
- `"key"_a = value` kwargs via `using namespace py::literals;` (e.g. `py::dict("error"_a = msg)`).

## 5. STL & container conversions

`#include <pybind11/stl.h>` auto-converts: `std::vector/deque/list` ⇄ list, `std::array` ⇄ tuple/list, `std::set/unordered_set` ⇄ set, `std::map/unordered_map` ⇄ dict, `std::optional` ⇄ None, `std::variant` (+`monostate`), `std::pair`/`std::tuple`, `std::string`/`string_view`.

`#include <pybind11/stl_bind.h>` binds containers as real Python classes:

```cpp
py::bind_vector<std::vector<double>>(m, "DoubleVector");          // list-like
py::bind_map<std::map<std::string, int>>(m, "StringIntMap");      // dict-like
```

`PYBIND11_MAKE_OPAQUE(T)` disables automatic conversion (opaque pointers pass through).

**In this repo** only 3 of the 16 binding TUs include `stl.h` (`py_diff.cpp`, `py_builtin_python.cpp`, `py_builtin_web.cpp`), and only `py_diff.cpp` actually relies on a caster (`std::string` args/defaults at `:68-88`, a `std::vector<int>` return at `:150-162`). Kernel containers are `kimix::vector` / `kimix::string`, which have no caster — they are converted by hand (see §11). `bind_vector` / `bind_map` / `PYBIND11_MAKE_OPAQUE` are never used here.

## 6. NumPy (numpy.h)

> **Not used by this project.** No file under `src/runtime/py/` includes `numpy.h`, and `runtime_py` does not link or import NumPy (the only NumPy in the repo is a reference implementation inside `python/tests/test_search.py`, and `python/tests/test_parity_python.py` merely asserts on a `ModuleNotFoundError: No module named "numpy.core"` string). Keep kernels on the bytes/list contract below; do not add a NumPy dependency to a binding without a concrete need.

```cpp
#include <pybind11/numpy.h>
py::array_t<double> a({3, 4});            // C-contiguous array
auto buf = a.request();                   // py::buffer_info
double* p = a.mutable_data();             // const T* data() if read-only
py::array_t<float, py::array::f_style | py::array::forcecast> f(...);
py::array arr = py::array::ensure(obj, py::array::forcecast); // convert or nullptr
```

- Flags: `array::c_style`, `array::f_style`, `array::forcecast`; `array_t<T, ExtraFlags = forcecast>` (`numpy.h:1026-1028,1358`).
- `py::dtype::of<T>()`; `py::array` methods: `dtype()`, `size()`, `ndim()`, `shape()`, `strides()`, `writeable()`, `reshape()`, `view(dtype)`, `request(writable)`, `unchecked<T,Dims>()`. Conversion helper: `array::ensure(h, ExtraFlags)` / `array_t<T>::ensure(h)` return a null object and *clear* the Python error on failure (`numpy.h:1301,1477`); the `array_t(handle, bool is_borrowed)` constructor is deprecated in favour of them (`numpy.h:1379`).
- `buffer_info` fields: `ptr, itemsize, size, ndim, format, shape, strides, readonly`; `py::memoryview(info)` wraps it.

## 7. GIL (gil.h)

```cpp
py::gil_scoped_acquire acquire;            // safe from any thread
py::gil_scoped_release release;            // PRECONDITION: GIL held
m.def("long_op", &long_op, py::call_guard<py::gil_scoped_release>());
```

- NEVER create or touch Python objects while the GIL is released. Pattern: extract `string_view`/buffers first, run the kernel inside `{ release; ... }`, build `py::*` results after the scope closes.
- `py::gil_safe_call_once_and_store<T>` — thread-safe lazy static for Python objects (`gil_safe_call_once.h:72`). 3.x also ships `py::gil_scoped_acquire_simple` / `gil_scoped_release_simple` (`gil_simple.h:13,23`, thin `PyGILState_Ensure/Release` wrappers) and `py::scoped_critical_section` (`critical_section.h:13` — a no-op while the GIL exists, an object lock on free-threaded builds); none are used here.
- `py::call_guard<py::gil_scoped_release>` is **not** used in this repo — the guard is constructed explicitly inside each lambda instead, because most bindings do CPython work both before and after the kernel call (see §11).

## 8. Exceptions

```cpp
throw py::value_error("bad");   // type_error, index_error, key_error, stop_iteration,
                                // attribute_error, import_error, buffer_error,
                                // cast_error, reference_cast_error
py::set_error(PyExc_TypeError, "msg");        // PyErr_SetString interop
py::raise_from(PyExc_TypeError, "cause");
throw py::error_already_set();                // rethrow a pending Python error
py::register_exception<MyCppError>(m, "MyCppError");   // translate via what()
py::register_exception_translator([](std::exception_ptr p) { ... });
py::implicitly_convertible<From, To>();       // register implicit conversion
```

- The default translator maps `std::bad_alloc`→`MemoryError`, `std::domain_error`/`invalid_argument`/`length_error`/`range_error`→`ValueError`, `std::out_of_range`→`IndexError`, `std::overflow_error`→`OverflowError`, and any other `std::exception` (incl. `std::runtime_error`) or unknown throw→`RuntimeError` (`detail/internals.h:505-547`). `py::cast_error` / `py::reference_cast_error` are themselves `RuntimeError`-backed (`detail/common.h:1102-1105`).
- On `nullptr` / `-1` from CPython APIs (e.g. `PyBytes_AsStringAndSize`, `PyUnicode_AsUTF8AndSize`, `PyObject_Call`), throw `py::error_already_set()` to surface the pending exception (and `Py_DECREF` any owned reference first — `py_print.cpp:59-67,129-134`).

## 9. eval / embed / functional / options / iostream

```cpp
py::object r = py::eval("1 + 2");             // expression
py::exec("x = 42");                           // statements
py::eval<py::eval_statements>("a = 1");
py::dict g = py::globals();                   // current scope dict
py::exec("y = x", g, py::dict());             // explicit globals/locals

py::scoped_interpreter guard{};               // embedded interpreter (embed.h)
PYBIND11_EMBEDDED_MODULE(mymod, m) { ... }    // built-in module when embedding

#include <pybind11/functional.h>
m.def("apply", [](std::function<int(int)> f, int x) { return f(x); }); // callable <-> std::function

#include <pybind11/options.h>
py::options opts; opts.disable_function_signatures();   // RAII doc tweaks

#include <pybind11/iostream.h>
py::scoped_ostream_redirect out;              // std::cout -> sys.stdout (RAII)
py::add_ostream_redirect(m, "ostream_redirect"); // Python context manager
```

`chrono.h`: `std::chrono::duration` ⇄ `timedelta`, `system_clock::time_point` ⇄ `datetime`. `complex.h`: `std::complex<T> ⇄ Python complex`.

> **None of this section is used in `src/runtime/py/`** (0 hits for `py::eval`/`py::exec`/`scoped_interpreter`/`functional.h`/`options.h`/`iostream.h`). The project never embeds CPython — `runtime_py` is a plain extension imported by an external interpreter, so `embed.h` / `PYBIND11_EMBEDDED_MODULE` do not apply (nothing under `src/` calls `Py_Initialize`). `py::module_::import` (§1) is the only way a binding reaches back into Python.

## 10. Custom type casters & iterators

```cpp
namespace pybind11 { namespace detail {
template <> struct type_caster<MyType> {
    PYBIND11_TYPE_CASTER(MyType, const_name("MyType"));  // declares value/name/cast/load
    bool load(handle src, bool convert);
    static handle cast(const MyType& src, return_value_policy, handle parent);
};
}}
py::make_iterator(first, last); // bind a range as __iter__/__next__
py::make_key_iterator(first, last); // keys of a map range
```

- 3.x `make_iterator` is `make_iterator(Iterator first, Sentinel last, Extra &&...extra)` (`pybind11.h:3213`) — the old positional `return_value_policy` / size arguments are gone; pass `py::return_value_policy<>()` as an extra if needed.
- No custom `type_caster`, `make_iterator` or `make_key_iterator` exists in `src/runtime/py/`: conversions are plain helper functions (§11), and results are materialized into `py::list`.

## 11. Project conventions (`src/runtime/py`)

- **One TU owns `PYBIND11_MODULE`** — `PYBIND11_MODULE(runtime_py, m)` in `module.cpp:93`. Every domain file exposes `void py_register_<domain>(py::module_&)`, forward-declared at global scope in `module.cpp:77-91`. The TU↔submodule map is **not** 1:1: `py_register_index` + `py_register_history` both fill `index` (`module.cpp:117-121`), and the four `py_register_builtin_*` all receive the `builtin_tools` module and open their own nested submodule (`py_builtin_file.cpp:171-173`, `py_builtin_shell.cpp:129-130`, `py_builtin_web.cpp:386-388`, `py_builtin_python.cpp:170-173`). Domain TUs also overwrite the `m.doc()` that `module.cpp` passed to `def_submodule`, and register functions by chaining `m.def(...).def(...)` (`py_codec.cpp:58-125`).
- **Boundary types**: `py::bytes` carries document payload bytes; `py::str` carries text (paths, commands, patterns, markdown) — 95 `py::bytes` parameters vs 125 `py::str` parameters, so *both* are idiomatic. Convert explicitly, never through a caster: `bytes_view(py::bytes, kimix::string_view&)` via `PyBytes_AsStringAndSize`, `str_to_string` / `require_str` via `PyUnicode_AsUTF8AndSize`; out via `to_bytes(kimix::string)` = `py::bytes(ptr,size)` or `py::str(ptr,size)`. These helpers are duplicated per TU inside an anonymous namespace (`py_codec.cpp:26-49`, `py_builtin_python.cpp:49-75`, …); the only shared header is `py_soul_bridge.h` (`bridge_bytes_view`, `bridge_to_bytes`, `parse_structure`), used by `py_tools.cpp:253`.
- `kimix::string` is `std::basic_string<char, std::char_traits<char>, kimix::allocator<char>>` (`src/core/stl/string.h:49`) — **not** `std::string`, so `stl.h`'s string caster will not accept it. `kimix::string_view` *is* `std::string_view` (`string.h:59`) and `kimix::vector` is `std::vector` with the kimix allocator (`src/core/stl/vector.h:46`); both are still converted by hand on purpose so the boundary type is spelled out in the signature.
- **GIL policy**: every kernel call is wrapped in `kimix::runtime::common::gil_scoped_release` — a one-field RAII wrapper over `pybind11::gil_scoped_release` in `src/runtime/common/gil.h:39-44`, which includes `<pybind11/pybind11.h>` and is BINDING-LAYER ONLY (kernels must never include it). 179 guards are constructed across the 16 TUs; `py::call_guard` and `py::gil_scoped_acquire` are used nowhere. Extract views BEFORE releasing, keep the release in a nested `{ }` around only the kernel call, and build `py::*` results AFTER the scope closes. Never call back into Python while released (exceptions: `scan_lines_cb` keeps the GIL for its Python callback, `py_tools.cpp:222-247`; `print.native_print` keeps it for `PyObject_Call`, `py_print.cpp:117-134`).
- **Errors**: `throw py::type_error(...)` / `py::value_error(...)` for argument validation (84 / 11 uses, message text names the expected Python type, e.g. `"rules must be list[tuple[str, bool, bool, bool]]"`); `throw py::error_already_set()` after a failed CPython call (74 uses); return `py::none()` for "no result" (not-found / malformed input) instead of throwing. `py::register_exception`, `set_error`, `raise_from` and `implicitly_convertible` appear nowhere in this layer.
- **Results**: build `py::list` / `py::dict` / `py::make_tuple(...)` from kernel `kimix::vector`s after the release scope. The result-side helpers are per-TU too (`lines_to_list` `py_stream.cpp:38`, `hits_to_list` `py_tools.cpp:81`, `turn_to_dict` `py_history.cpp:81`, `variants_to_list` `py_builtin_shell.cpp:101`, `history_turn_to_dict` `py_builtin_web.cpp:284`); `kimix::optional` maps to `None` through `opt_str_to_obj` / `opt_int_to_obj` rather than `std::optional` + `stl.h`. Input-side mirrors: `str_list`, `env_from_dict`, `parse_ignore_rules_py`.
- **Bound classes** (10 — matches the import dump): `WireMergeBuffer`, `ArgsBuffer`, `JsonRpcFrameWriter`, `JsonlRecorder`, `RecvBuffer` (`py_codec.cpp:130-247`), `NgramTokenizer`, `InvertedIndex` (`py_index.cpp:87,145`), `HistoryIndex` (`py_history.cpp:97`), `SymmetricDeleteIndex` (`py_search.cpp:248`), `LineProcessor` (`py_stream.cpp:91`). All keep the default `std::unique_ptr` holder, are constructed with `py::init<>()` / `py::init<uint32_t>()` / a factory-init lambda (`py_stream.cpp:92-110`), and expose plain `.def("method", &T::method)` getters. Only two property bindings exist (`def_property_readonly` `py_index.cpp:139`, `def_property_readonly_static` `py_history.cpp:99`); there is no `def_readwrite`/`def_readonly`, `def_static`, `py::pickle`, `keep_alive` or `return_value_policy` anywhere in this layer.
- **Casting**: `obj.cast<kimix::string>()` / `item.cast<std::string>()`, `py::cast<bool>(h)`, `result.cast<bool>()`, `py::isinstance<py::list|py::dict|py::str|py::bytes|py::int_>(obj)`; `PyDict_Check` / `PyList_GET_SIZE` / `PyTuple_GET_ITEM` fast paths when the whole dict is walked (`py_soul_bridge.h:87-140`).
- **Exceptions / RTTI gate**: the project is compiled with C++ exceptions **off** (`kimix_enable_exception` default false, `xmake.lua:58-66`); only the targets in `kimix_exceptions_targets` — default `"runtime_py,test_pybind11"` (`xmake.lua:67-75`, applied in `scripts/xmake_func.lua:274-327`) — get `-fexceptions` / `/EHsc`. So throwing belongs in `src/runtime/py/*.cpp` only. Two traps: (a) `runtime_py` recompiles `runtime/**.cpp` (`src/xmake.lua:222`), so a kernel TU is built *twice* — once with exceptions on, once (via kimix-core/kimix-llm) with `throw` a hard error plus `_HAS_EXCEPTIONS=0` / `KIMIX_NO_EXCEPTIONS`; kernel code must stay throw-free. (b) RTTI is off for every target (`kimix_rtti` default false → `/GR-`, `scripts/xmake_func.lua:431-459`), so no `dynamic_cast`/`typeid` in bindings and no bound type may be polymorphic — pybind11 would pull `dynamic_cast` in through `detail/dynamic_raw_ptr_cast_if_possible.h`.
- **Includes**: `#include <pybind11/pybind11.h>` is the only pybind11 include 16/16 TUs need; add `<pybind11/stl.h>` only when a `std::string`/`std::vector` signature really uses the caster (`py_diff.cpp:68-88,150-162`; `py_builtin_python.cpp` / `py_builtin_web.cpp` include it but bind no STL signature). Headers come from the `kimix-pybind11` headeronly target (`src/ext/xmake.lua:67-74`); the `runtime_py` target deliberately has **no unity build** (`src/xmake.lua:220`) so the `PYBIND11_MODULE` TU stays isolated from `Python.h`, and the interpreter's include dir + import library are probed through `sysconfig` at build time (`src/xmake.lua:256-293`).
- **Naming / artifacts**: the import name is exactly `runtime_py` (`PYBIND11_MODULE(runtime_py, m)`), and the file is `bin/<mode>/runtime_py.pyd` — the target forces `set_extension(".pyd")` on every platform (`src/xmake.lua:213-219`); on Linux `set_prefixname("")` plus an `after_link` copy publish `runtime_py.so` (CPython only imports `*.so`) and a `libruntime_py.so` link for the C++ test binaries (`src/xmake.lua:295-312`). `publish.py:104-105` packages `runtime_py.pyd` (Windows) / `runtime_py.so` (Linux) from `bin/release`. Python never imports it directly: `python/kimix_native/__init__.py:24-32` lazily does `import runtime_py as _native`, gated by `KIMIX_NATIVE` = `0`/`1`/`auto` (default `auto`) and per-kernel `KIMIX_NATIVE_<KERNEL>`; `runtime_py.use_native(kernel)` is the C++ mirror (`module.cpp:60-69`, `GetEnvironmentVariableA` on Windows because the MDd extension and MD `python.exe` keep separate `getenv` tables).

## 12. Reference files

- Real usage: `src/runtime/py/*.cpp` — **16 binding TUs** (`module.cpp` + 15 `py_*.cpp`) plus the shared `py_soul_bridge.h`. Follow their structure for new domains.
- Vendored headers: `src/ext/pybind11/include/pybind11/` — `pybind11.h`, `attr.h`, `cast.h`, `pytypes.h`, `stl.h`, `stl_bind.h`, `numpy.h`, `gil.h`, `gil_simple.h`, `critical_section.h`, `eval.h`, `embed.h`, `functional.h`, `chrono.h`, `complex.h`, `options.h`, `iostream.h`, `operators.h`, `buffer_info.h`, `native_enum.h`, `subinterpreter.h`, `typing.h`, `warnings.h`, `type_caster_pyobject_ptr.h`, `eigen.h`, `detail/init.h`, `detail/class.h`, `detail/descr.h`.
- `native_enum` (real `enum.Enum`), `py::smart_holder` / `py::classh<T>` (`pybind11.h:2675`; the default holder stays `std::unique_ptr` — `pybind11.h:2117`), `subinterpreter.h`, `conduit/pybind11_conduit_v1.h`, `gil_safe_call_once.h`, `typing.h` all ship upstream.

## 13. Verifying a binding change

```bash
python bootstrap.py                 # documented build entry (AGENTS.md); --debug for bin/debug
xmake f -m debug -c -y && xmake build runtime_py   # -> bin/<mode>/runtime_py.pyd
python -m pytest python/tests/test_ansi.py -x -q # one small kernel test file
python -m pytest python/tests -q # whole pytest suite
xmake run test_pybind11 # include-path smoke test (tests/xmake.lua:99)
```

- `python/tests/conftest.py:48-67` picks the first `bin/<mode>` in the order `release, releasedbg, debug, check` that actually holds a `runtime_py.(pyd|so|dylib)`, then prepends it plus `python/` to `sys.path` — a stale `bin/release` therefore shadows a fresh debug build. `_EXT` provenance is asserted at `conftest.py:120-132` (two `runtime_py` objects in one process would make every comparison the port against itself).
- Two distinct suites cover a binding: `python/tests/test_<kernel>.py` checks the native kernel against the shim's own `_compat_*` fallback with the toggle (`monkeypatch.setenv("KIMIX_NATIVE_STREAM", "0")`, `test_ansi.py:161-162`), while `python/tests/test_parity_*.py` checks it against the original kimi-agent source via `_parity_ref.ref(...)` / `pure_python(...)` (`_parity_ref.py:292,356`), which needs a kimi-agent checkout at `KIMI_AGENT_ROOT` (default `C:/dev/kimi-agent`, `_parity_ref.py:59`). A new kernel needs all three: C++ binding, `python/kimix_native/<area>.py` fallback, tests on both sides.
- To list what a build actually exports (no rebuild needed once a `bin/<mode>` exists):
  `python -c "import sys; sys.path.insert(0,'bin/release'); import runtime_py; print(dir(runtime_py))"`
- Do **not** run `tests/verify_workspace_parity.py`: it hard-codes `D:\KimiX-native` into `sys.path` (lines 12-13) and imports `kimix_native.workspace`, which no longer exists. It is broken in this tree.

Confirmed export surface of the current `bin/release/runtime_py.pyd` (single-phase init, so `import` + `dir()` is enough):

- top level: `version`, `core_version`, `c_version`, `use_native`, `version_string` + submodules `text`, `index`, `search`, `codec`, `stream`, `parse`, `tools`, `diff`, `glob`, `print`, `common`, `builtin_tools`.
- `builtin_tools` nests `shell`, `file`, `web`, `python`. Names per submodule: text 10, index 3 (`HistoryIndex`, `InvertedIndex`, `NgramTokenizer`), search 14 (`SymmetricDeleteIndex` + 13 functions), codec 9 (5 classes + 4 functions), stream 3 (`LineProcessor` + `filter_output`, `strip_ansi`), parse 4, tools 22, diff 4, glob 5, print 1 (`native_print`), common 2 (`is_ascii`, `utf8_code_point_count`), builtin_tools 17/17/34/9.
- Behaviour spot-checked live: `use_native("text")` flips to `False` under `KIMIX_NATIVE_TEXT=0` and `KIMIX_NATIVE=0`; `codec.deserialize_envelope(b"not json")` returns `None`; a wrong-typed `py::list` parameter raises pybind11's own overload-resolution `TypeError` (the hand-written `py::type_error` messages only cover manually validated arguments).
- Signatures render the boundary types, e.g. `append(self: runtime_py.codec.WireMergeBuffer, kind: str, delta: bytes) -> bool` — proof that `str` and `bytes` sit side by side in one call.
