---
name: pybind
description: pybind11 usage guide for Python bindings. Use when writing or editing pybind11 C++ binding code (e.g. src/runtime/py/*.cpp) — module/class/function binding, STL/NumPy casts, GIL, exceptions.
---

# pybind11 — Python Binding Reference

Namespace `pybind11`, conventionally aliased `namespace py = pybind11;`. Header-only; include what you use:

`pybind11.h` (core) · `attr.h` (annotations) · `cast.h` (casting, `py::arg`) · `pytypes.h` (Python object wrappers) · `stl.h` / `stl_bind.h` (STL) · `numpy.h` · `gil.h` · `eval.h` · `embed.h` · `functional.h` · `chrono.h` · `complex.h` · `options.h` · `iostream.h` · `operators.h`.

## 1. Module setup

```cpp
#include <pybind11/pybind11.h>
namespace py = pybind11;

PYBIND11_MODULE(mymod, m) {          // ONE PYBIND11_MODULE per extension
    m.doc() = "module docstring";
    m.def("add", [](int a, int b) { return a + b; }, "adds two ints",
          py::arg("a"), py::arg("b"));
    m.attr("VERSION") = 42;                       // module attribute
    auto sub = m.def_submodule("text", "doc");    // submodule
    m.add_object("obj", py::cast(some_value));    // attach arbitrary object
    py::module_::import("json");                  // import existing module
}
```

- `module_::import(name)` throws `py::error_already_set` on failure; `import("json").attr("loads")(bytes)` calls Python from C++.
- `m.def_submodule(name, doc)` returns a `module_&` you can keep registering into.
- `m.doc()` is read/write (`__doc__`).

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
- Default args are also written as plain positional defaults after `py::arg(...)` in `.def(py::init<...>(), ...)` chains; keep the docstring as a plain string literal argument.

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
    .def(py::init([](int x) { return new Widget(x); }));  // factory init
```

- Template args: holder (`std::unique_ptr<T>` default, `std::shared_ptr<T>`, `py::smart_holder`), alias/trampoline, bases for inheritance: `py::class_<Derived, Base, std::shared_ptr<Derived>>`.
- Class extras: `py::dynamic_attr()`, `py::multiple_inheritance`, `py::module_local()`, `py::is_final`, `py::buffer_protocol()`, `py::metaclass(...)`.
- Operators: `.def(py::self + py::self)` (also `- * / % << >> & | ^ == != < <= > >=` and in-place `+=` etc.), unary `py::neg(py::self)`, `py::pos`, `py::abs`, `py::hash`.
- Trampolines (virtuals overridable from Python): define `class PyAnimal : public Animal { using Animal::Animal; std::string go(int n) override { PYBIND11_OVERRIDE_PURE(std::string, Animal, go, n); } };` then `py::class_<Animal, PyAnimal>(m, "Animal")`. `PYBIND11_OVERRIDE_NAME(ret, cls, pyName, fn, ...)` when names differ.

```cpp
py::enum_<Color>(m, "Color", py::arithmetic())
    .value("RED", Color::RED, "doc")
    .export_values();          // also exposes RED at module scope
```

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

## 6. NumPy (numpy.h)

```cpp
#include <pybind11/numpy.h>
py::array_t<double> a({3, 4});            // C-contiguous array
auto buf = a.request();                   // py::buffer_info
double* p = a.mutable_data();             // const T* data() if read-only
py::array_t<float, py::array::f_style | py::array::forcecast> f(...);
py::array arr = py::array::ensure(obj, py::array::forcecast); // convert or nullptr
```

- Flags: `array::c_style`, `array::f_style`, `array::forcecast`; `array_t<T, ExtraFlags = forcecast>`.
- `py::dtype::of<T>()`; `py::array` methods: `dtype()`, `size()`, `ndim()`, `shape()`, `strides()`, `writeable()`, `reshape()`, `view(dtype)`, `request(writable)`, `unchecked<T,Dims>()`.
- `buffer_info` fields: `ptr, itemsize, size, ndim, format, shape, strides, readonly`; `py::memoryview(info)` wraps it.

## 7. GIL (gil.h)

```cpp
py::gil_scoped_acquire acquire;            // safe from any thread
py::gil_scoped_release release;            // PRECONDITION: GIL held
m.def("long_op", &long_op, py::call_guard<py::gil_scoped_release>());
```

- NEVER create or touch Python objects while the GIL is released. Pattern: extract `string_view`/buffers first, run the kernel inside `{ release; ... }`, build `py::*` results after the scope closes.
- `py::gil_safe_call_once_and_store<T>` — thread-safe lazy static for Python objects.

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

- Plain `std::runtime_error`/`std::exception` are auto-translated to `RuntimeError`.
- On `nullptr` from CPython APIs (e.g. `PyBytes_AsStringAndSize`), throw `py::error_already_set()` to surface the pending exception.

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

`chrono.h`: `std::chrono::duration` ⇄ `timedelta`, `system_clock::time_point` ⇄ `datetime`. `complex.h`: `std::complex<T>` ⇄ Python `complex`.

## 10. Custom type casters & iterators

```cpp
namespace pybind11 { namespace detail {
template <> struct type_caster<MyType> {
    PYBIND11_TYPE_CASTER(MyType, const_name("MyType"));  // declares value/name/cast/load
    bool load(handle src, bool convert);
    static handle cast(const MyType& src, return_value_policy, handle parent);
};
}}
py::make_iterator(first, last);          // bind a range as __iter__/__next__
py::make_key_iterator(first, last);      // keys of a map range
```

## 11. KimixBase project conventions (src/runtime/py)

- **One TU owns `PYBIND11_MODULE`** (`module.cpp`); every domain file exposes `void py_register_<domain>(py::module_&)` and is called from the module init to fill a `m.def_submodule(...)`.
- **Bytes at the boundary**: text is passed as `py::bytes` (UTF-8) — never `py::str` — with helpers `bytes_view(py::bytes, kimix::string_view&)` (via `PyBytes_AsStringAndSize`) and `to_bytes(const kimix::string&)` / `py::str(ptr, size)`. The Python shim (`python/kimix_native/`) owns str⇄bytes decoding (often `surrogatepass`).
- **GIL policy**: every kernel call is wrapped in `kimix::runtime::common::gil_scoped_release` (a thin wrapper over `py::gil_scoped_release` in `runtime/common/gil.h`, which includes `<pybind11/pybind11.h>` and must only be included by binding TUs). Extract views BEFORE releasing; build Python objects only AFTER the guard's scope closes. Never call back into Python while released (exception: `scan_lines_cb` deliberately keeps the GIL to call a Python callback).
- **Errors**: `throw py::type_error(...)` / `py::value_error(...)` for argument validation; `throw py::error_already_set()` after a failed CPython call; return `py::none()` for "no result" (e.g. not-found lookups) instead of throwing.
- **Results**: build `py::list`/`py::dict`/`py::make_tuple(...)` from kernel `kimix::vector`s after the release scope; `py::class_` exposes stateful kernel objects (`WireMergeBuffer`, `HistoryIndex`, `LineProcessor`, ...) with `py::init<>` / factory-init lambdas and plain `.def("method", &T::method)` for simple getters.
- **Casting**: `item.cast<std::string>()`, `py::cast<bool>(h)`, `py::isinstance<py::str>(obj)`; `py::module_::import("json").attr("loads")(bytes)` for JSON decode inside bindings.
- Includes: `#include <pybind11/pybind11.h>` (plus `<pybind11/stl.h>` when returning `std::vector`/`std::string`); headers come from the `kimix-pybind11` target (`src/ext/pybind11/include`), merged into `runtime_py` with no unity build (see `src/xmake.lua`).

## 12. Reference files

- Real usage: `src/runtime/py/*.cpp` (17 binding TUs) — follow their structure for new domains.
- Vendored headers: `src/ext/pybind11/include/pybind11/` — `pybind11.h`, `attr.h`, `cast.h`, `pytypes.h`, `stl.h`, `stl_bind.h`, `numpy.h`, `gil.h`, `eval.h`, `embed.h`, `functional.h`, `chrono.h`, `complex.h`, `options.h`, `iostream.h`, `operators.h`, `buffer_info.h`, `detail/init.h`, `detail/class.h`.
- Fork extras in this tree: `native_enum.h` (bind to real `enum.Enum`), `smart_holder` (`py::classh<T>`), `subinterpreter.h`, `conduit/pybind11_conduit_v1.h`, `gil_safe_call_once.h`, `typing.h`.
