/*
 * py_session.cpp - Python bindings for session.Store (runtime_py.session).
 *
 * BINDING-LAYER ONLY. Owns the `session` submodule.
 *
 * CONTRACT WITH THE SHIM (python/kimix_native/session.py::Store):
 *   session.Store(per_session_budget=4<<20, total_budget=64<<20)
 *     .create(session_id: str) -> int            # idempotent handle
 *     .append(session: int, kind: str, role: str = "assistant", ...) -> bool
 *         keyword ids are bytes (surrogatepass UTF-8 from the shim), the four
 *         *JSON arguments are pre-serialized objects embedded verbatim.
 *     .snapshot(session: int, since_seq: int = 0) -> bytes   # {"sessionID",
 *          "status", "seq", "parts":[...]}, MessagePart.to_dict() shape
 *     .part_json(session: int, part_id: bytes) -> bytes|None
 *     .sessions() -> int ; .part_count(session) -> int
 *     .dropped(session) -> int ; .generation(session) -> int
 *     .set_status(session, status: int) ; .status(session) -> int
 *     .evict() -> int ; .close(session) -> bool
 *     .bytes_owned() / .bytes_live() / .close()
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/runtime.h>
#include <runtime/soul/session_store.h>

#include "py_util.h"

using kimix_py_util::bytes_view;
using kimix_py_util::str_to_string;
using kimix_py_util::to_bytes;

namespace py = pybind11;

namespace src_runtime_py_py_session {

using kimix::runtime::soul::SessionStore;
using kimix::runtime::soul::part_record;

uint8_t kind_from_name(kimix::string_view name) {
    if (name == "reasoning") {
        return kimix::runtime::soul::kPartReasoning;
    }
    if (name == "tool") {
        return kimix::runtime::soul::kPartTool;
    }
    if (name == "step-start") {
        return kimix::runtime::soul::kPartStepStart;
    }
    if (name == "step-finish") {
        return kimix::runtime::soul::kPartStepFinish;
    }
    return kimix::runtime::soul::kPartText;
}

uint8_t role_from_name(kimix::string_view name) {
    if (name == "user") {
        return kimix::runtime::soul::kRoleUser;
    }
    if (name == "system") {
        return kimix::runtime::soul::kRoleSystem;
    }
    return kimix::runtime::soul::kRoleAssistant;
}

// One optional bytes keyword argument as a view (empty when absent).
void take_bytes(py::dict kwargs, const char *key, kimix::string_view &out) {
    if (!kwargs.contains(key)) {
        return;
    }
    py::object v = kwargs[key];
    if (v.is_none()) {
        return;
    }
    if (!py::isinstance<py::bytes>(v) ||
        !bytes_view(py::reinterpret_borrow<py::bytes>(v), out)) {
        throw py::type_error(std::string(key) + " must be bytes");
    }
}

} // namespace src_runtime_py_py_session
using namespace src_runtime_py_py_session;

void py_register_session(py::module_& m) {
    m.doc() = "Session store kernels (per-session part ring, session table, "
              "opencode-shaped JSON snapshots)";

    py::class_<SessionStore>(m, "Store",
                             "Per-session part ring + LRU session table with an "
                             "explicit byte budget (report A3 + C3).")
        .def(py::init<uint64_t, uint64_t>(),
             py::arg("per_session_budget") = 4ULL << 20,
             py::arg("total_budget") = 64ULL << 20,
             "Store(per_session_budget=4194304, total_budget=67108864)")
        .def("create",
             [](SessionStore &s, py::str session_id) -> uint32_t {
                 kimix::string id;
                 if (!str_to_string(session_id, id)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 return s.create(id);
             },
             "create(session_id) -> handle (idempotent)", py::arg("session_id"))
        .def("append",
             [](SessionStore &s, uint32_t session, py::str kind, py::str role,
                py::kwargs kwargs) -> bool {
                 kimix::string kind_s, role_s;
                 if (!str_to_string(kind, kind_s)) {
                     throw py::type_error("kind must be str");
                 }
                 if (!str_to_string(role, role_s)) {
                     throw py::type_error("role must be str");
                 }
                 part_record rec;
                 rec.kind = kind_from_name(kind_s);
                 rec.role = role_from_name(role_s);
                 py::dict kw = py::reinterpret_borrow<py::dict>(kwargs);
                 take_bytes(kw, "session_id", rec.session_id);
                 take_bytes(kw, "message_id", rec.message_id);
                 take_bytes(kw, "part_id", rec.part_id);
                 take_bytes(kw, "call_id", rec.call_id);
                 take_bytes(kw, "tool", rec.tool);
                 take_bytes(kw, "text", rec.text);
                 take_bytes(kw, "state_json", rec.state_json);
                 take_bytes(kw, "prompt_json", rec.prompt_json);
                 take_bytes(kw, "tokens_json", rec.tokens_json);
                 take_bytes(kw, "time_json", rec.time_json);
                 take_bytes(kw, "snapshot", rec.snapshot);
                 take_bytes(kw, "reason", rec.reason);
                 if (kw.contains("cost")) {
                     rec.cost = kw["cost"].cast<double>();
                 }
                 if (kw.contains("seq")) {
                     rec.seq = kw["seq"].cast<uint32_t>();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 return s.append(session, rec);
             },
             "append(session, kind, role='assistant', **ids_and_json) -> bool",
             py::arg("session"), py::arg("kind"), py::arg("role") = "assistant")
        .def("snapshot",
             [](SessionStore &s, uint32_t session, uint32_t since_seq) -> py::bytes {
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     if (!s.snapshot(session, since_seq, out)) {
                         throw py::value_error("no such session");
                     }
                 }
                 return to_bytes(out);
             },
             "snapshot(session, since_seq=0) -> bytes", py::arg("session"),
             py::arg("since_seq") = 0)
        .def("part_json",
             [](const SessionStore &s, uint32_t session, py::bytes part_id) -> py::object {
                 kimix::string_view id;
                 if (!bytes_view(part_id, id)) {
                     throw py::error_already_set();
                 }
                 kimix::string out;
                 bool found = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     found = s.part_json(session, id, out);
                 }
                 if (!found) {
                     return py::none();
                 }
                 return to_bytes(out);
             },
             "part_json(session, part_id) -> bytes|None", py::arg("session"),
             py::arg("part_id"))
        .def("sessions", &SessionStore::session_count)
        .def("part_count", &SessionStore::part_count, py::arg("session"))
        .def("dropped", &SessionStore::dropped_parts, py::arg("session"))
        .def("generation", &SessionStore::generation, py::arg("session"))
        .def("set_status", &SessionStore::set_status, py::arg("session"),
             py::arg("status"))
        .def("status", &SessionStore::status, py::arg("session"))
        .def("evict", &SessionStore::evict_to_budget,
             "Drop least-recently-used sessions until the total budget fits.")
        .def("close", &SessionStore::close, py::arg("session"))
        .def("bytes_owned", &SessionStore::bytes_owned)
        .def("bytes_live", &SessionStore::bytes_live)
        .def("clear", [](SessionStore &s) {
            kimix::runtime::common::gil_scoped_release release;
            s.clear();
        }, "Close every live session.");
}
