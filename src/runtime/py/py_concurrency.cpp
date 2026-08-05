/*
 * py_concurrency.cpp -- Python bindings for the concurrency kernels
 * (runtime_py.concurrency).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release so the event bus stays
 * responsive while Python threads wait.
 *
 * CRITICAL: no Python object may be created while the GIL is released --
 * kernels run inside `{ gil_scoped_release release; ... }` and Python
 * objects are built only after the scope closes (GIL reacquired).
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/concurrency/event_bus.h>
#include <runtime/concurrency/id_gen.h>

namespace py = pybind11;

namespace {

bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

py::bytes to_bytes(const kimix::string& s) {
    return py::bytes(s.data(), s.size());
}

} // namespace

void py_register_concurrency(py::module_& m) {
    m.doc() = "Concurrency kernels: bounded MPSC event bus, atomic ID generator";

    // ------------------------------------------------------------------
    // MpscEventBus
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::concurrency::MpscEventBus>(m, "MpscEventBus",
        "Bounded single-producer/multi-consumer event bus. emit() is O(1) "
        "regardless of subscriber count; bounded ring with DROP_OLDEST "
        "policy (a slow subscriber skips dropped events and stays "
        "consistent).")
        .def(py::init<size_t>(), py::arg("capacity"))
        .def("emit",
             [](kimix::runtime::concurrency::MpscEventBus& bus, py::bytes data) {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 bus.emit(view);
             },
             "Publish one event (never blocks; drops oldest when full).",
             py::arg("event_bytes"))
        .def("subscribe",
             [](kimix::runtime::concurrency::MpscEventBus& bus) -> uint64_t {
                 return bus.subscribe();
             },
             "Register a subscriber starting at the current tail; returns a "
             "monotonically increasing id (never reused).")
        .def("unsubscribe",
             [](kimix::runtime::concurrency::MpscEventBus& bus, uint64_t id) {
                 kimix::runtime::common::gil_scoped_release release;
                 bus.unsubscribe(id);
             },
             py::arg("id"))
        .def("poll",
             [](kimix::runtime::concurrency::MpscEventBus& bus, uint64_t id) -> py::object {
                 kimix::string out;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = bus.poll(id, out);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 return to_bytes(out);
             },
             "Next unseen event for the subscriber, or None when caught up / "
             "unknown id.",
             py::arg("id"))
        .def("seq",
             [](const kimix::runtime::concurrency::MpscEventBus& bus) -> uint64_t {
                 return bus.seq();
             },
             "Total events emitted since construction (monotonic).")
        .def("capacity", &kimix::runtime::concurrency::MpscEventBus::capacity);

    // ------------------------------------------------------------------
    // IdGenerator
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::concurrency::IdGenerator>(m, "IdGenerator",
        "Thread-safe monotonic counter: next() = one atomic fetch_add; "
        "reserve(n) = n consecutive ids from a single atomic RMW.")
        .def(py::init<uint64_t>(), py::arg("seed") = 0)
        .def("next",
             [](kimix::runtime::concurrency::IdGenerator& gen) -> uint64_t {
                 return gen.next();
             })
        .def("reserve",
             [](kimix::runtime::concurrency::IdGenerator& gen, uint64_t n) -> py::list {
                 kimix::vector<uint64_t> ids;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     gen.reserve(n, ids);
                 }
                 py::list out;
                 for (uint64_t v : ids) {
                     out.append(v);
                 }
                 return out;
             },
             "Reserve n consecutive ids (list of ints).",
             py::arg("n"));
}
