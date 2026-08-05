/*
 * py_codec.cpp -- Python bindings for the codec kernels (runtime_py.codec).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release. Contract: bytes in / bytes
 * out (type names are str, encoded UTF-8 at the boundary).
 *
 * CRITICAL: no Python object may be created while the GIL is released --
 * kernels run inside `{ gil_scoped_release release; ... }` and Python
 * objects are built only after the scope closes (GIL reacquired).
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/codec/wire_envelope.h>
#include <runtime/codec/merge_buffer.h>
#include <runtime/codec/args_buffer.h>
#include <runtime/codec/frame_writer.h>
#include <runtime/codec/recv_buffer.h>
#include <runtime/codec/sse.h>

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

// ArgsBuffer binding wrapper: keeps the watermark next to the kernel so the
// Python API is `delta_since() -> bytes` with no arguments.
struct PyArgsBuffer {
    kimix::runtime::codec::ArgsBuffer buf;
    size_t watermark = 0;
};

} // namespace

void py_register_codec(py::module_& m) {
    m.doc() = "Codec kernels: wire envelope, merge/args buffers, JSON-RPC + "
              "jsonl frames, TCP recv buffer, SSE frames";

    // ------------------------------------------------------------------
    // Wire envelope
    // ------------------------------------------------------------------
    m.def("serialize_envelope",
          [](py::str type_name, py::bytes payload) -> py::bytes {
              Py_ssize_t type_len = 0;
              const char* type_cstr = PyUnicode_AsUTF8AndSize(type_name.ptr(), &type_len);
              if (type_cstr == nullptr) {
                  throw py::error_already_set();
              }
              kimix::string_view payload_view;
              if (!bytes_view(payload, payload_view)) {
                  throw py::error_already_set();
              }
              kimix::runtime::codec::wire_envelope env;
              env.type.assign(type_cstr, static_cast<size_t>(type_len));
              env.payload_json.assign(payload_view.data(), payload_view.size());
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::codec::serialize_envelope(env, out);
              }
              return to_bytes(out);
          },
          "Serialize {type, payload} into an envelope frame: "
          "{\"type\":..., \"payload\":...} in ONE JSON pass.",
          py::arg("type_name"), py::arg("payload"))

        .def("deserialize_envelope",
             [](py::bytes frame) -> py::object {
                 kimix::string_view frame_view;
                 if (!bytes_view(frame, frame_view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::codec::wire_envelope out;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = kimix::runtime::codec::deserialize_envelope(frame_view, out);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 // GIL reacquired: build the Python objects now.
                 return py::make_tuple(py::str(out.type.data(), out.type.size()),
                                       to_bytes(out.payload_json));
             },
             "Parse an envelope frame -> (type_name, payload_bytes), or None "
             "when the frame is malformed.",
             py::arg("frame"))

        .def("canonicalize_payload",
             [](py::bytes data) -> py::object {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::string out;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = kimix::runtime::codec::canonicalize_payload(view, out);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 return to_bytes(out);
             },
             "Recursive object-key sort + compact re-encode (matches "
             "toolset._sort_json_value + orjson.dumps); None on invalid JSON.",
             py::arg("data"));

    // ------------------------------------------------------------------
    // WireMergeBuffer
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::codec::WireMergeBuffer>(m, "WireMergeBuffer",
        "Incremental wire merge buffer (same-kind text/args parts merge, "
        "others flush -- append returns False to signal a flush).")
        .def(py::init<>())
        .def("append",
             [](kimix::runtime::codec::WireMergeBuffer& buf, py::str kind,
                py::bytes delta) -> bool {
                 Py_ssize_t kind_len = 0;
                 const char* kind_cstr = PyUnicode_AsUTF8AndSize(kind.ptr(), &kind_len);
                 if (kind_cstr == nullptr) {
                     throw py::error_already_set();
                 }
                 kimix::string_view delta_view;
                 if (!bytes_view(delta, delta_view)) {
                     throw py::error_already_set();
                 }
                 bool merged = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     merged = buf.append(kimix::string_view(kind_cstr, static_cast<size_t>(kind_len)),
                                         delta_view);
                 }
                 return merged;
             },
             "Append one part; False when it is not mergeable with the "
             "current group (flush + retry).",
             py::arg("kind"), py::arg("delta"))
        .def("snapshot",
             [](const kimix::runtime::codec::WireMergeBuffer& buf) -> py::bytes {
                 return to_bytes(kimix::string(buf.snapshot()));
             })
        .def("reset", &kimix::runtime::codec::WireMergeBuffer::reset)
        .def("empty", &kimix::runtime::codec::WireMergeBuffer::empty);

    // ------------------------------------------------------------------
    // ArgsBuffer
    // ------------------------------------------------------------------
    py::class_<PyArgsBuffer>(m, "ArgsBuffer",
        "Incremental args buffer: append() accumulates, delta_since() "
        "returns only the bytes appended since the last call.")
        .def(py::init<>())
        .def("append",
             [](PyArgsBuffer& self, py::bytes delta) {
                 kimix::string_view view;
                 if (!bytes_view(delta, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 self.buf.append(view);
             },
             py::arg("delta"))
        .def("snapshot",
             [](const PyArgsBuffer& self) -> py::bytes {
                 return to_bytes(kimix::string(self.buf.snapshot()));
             })
        .def("delta_since",
             [](PyArgsBuffer& self) -> py::bytes {
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     const kimix::string_view d = self.buf.delta_since(self.watermark);
                     out.assign(d.data(), d.size());
                 }
                 return to_bytes(out);
             })
        .def("reset",
             [](PyArgsBuffer& self) {
                 self.buf.reset();
                 self.watermark = 0;
             });

    // ------------------------------------------------------------------
    // Frame writers
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::codec::JsonRpcFrameWriter>(m, "JsonRpcFrameWriter",
        "Newline-delimited JSON-RPC framing: write(payload) = payload + "
        "b'\\n' (matches wire/server.py).")
        .def(py::init<>())
        .def("write",
             [](const kimix::runtime::codec::JsonRpcFrameWriter& w,
                py::bytes payload) -> py::bytes {
                 kimix::string_view view;
                 if (!bytes_view(payload, view)) {
                     throw py::error_already_set();
                 }
                 kimix::string frame;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     w.write(view, frame);
                 }
                 return to_bytes(frame);
             },
             py::arg("payload"));

    py::class_<kimix::runtime::codec::JsonlRecorder>(m, "JsonlRecorder",
        "wire.jsonl line recorder: record(frame) = frame + b'\\n' (matches "
        "wire/file.py _dump_line; caller composes the record JSON once).")
        .def(py::init<>())
        .def("record",
             [](const kimix::runtime::codec::JsonlRecorder& r,
                py::bytes frame) -> py::bytes {
                 kimix::string_view view;
                 if (!bytes_view(frame, view)) {
                     throw py::error_already_set();
                 }
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     r.record(view, out);
                 }
                 return to_bytes(out);
             },
             py::arg("frame"));

    // ------------------------------------------------------------------
    // RecvBuffer (TCP frame extraction)
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::codec::RecvBuffer>(m, "RecvBuffer",
        "Growable TCP receive buffer: append() + take_frame_*() with "
        "BIG-ENDIAN length-prefixed (default 4-byte) or delimiter framing "
        "(matches tcp_client.py; max_frame=0 -> 10 MiB default).")
        .def(py::init<>())
        .def("append",
             [](kimix::runtime::codec::RecvBuffer& buf, py::bytes data) {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 buf.append(view);
             },
             py::arg("data"))
        .def("size", &kimix::runtime::codec::RecvBuffer::size)
        .def("peek",
             [](const kimix::runtime::codec::RecvBuffer& buf) -> py::bytes {
                 return to_bytes(kimix::string(buf.peek()));
             })
        .def("take_frame_length_prefixed",
             [](kimix::runtime::codec::RecvBuffer& buf, uint32_t header_size,
                size_t max_frame) -> py::object {
                 kimix::string out;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = buf.take_frame_length_prefixed(header_size, max_frame, out);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 return to_bytes(out);
             },
             "Extract one big-endian length-prefixed frame, or None when "
             "incomplete/oversize (nothing is consumed on None).",
             py::arg("header_size") = 4, py::arg("max_frame") = 0)
        .def("take_frame_delimiter",
             [](kimix::runtime::codec::RecvBuffer& buf, py::bytes delim,
                size_t max_frame) -> py::object {
                 kimix::string_view delim_view;
                 if (!bytes_view(delim, delim_view)) {
                     throw py::error_already_set();
                 }
                 kimix::string out;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = buf.take_frame_delimiter(delim_view, max_frame, out);
                 }
                 if (!ok) {
                     return py::none();
                 }
                 return to_bytes(out);
             },
             "Extract one delimiter-terminated frame (delimiter consumed, "
             "not included), or None when incomplete/oversize.",
             py::arg("delim"), py::arg("max_frame") = 0)
        .def("compact", &kimix::runtime::codec::RecvBuffer::compact)
        .def("clear", &kimix::runtime::codec::RecvBuffer::clear);

    // ------------------------------------------------------------------
    // SSE frame builder
    // ------------------------------------------------------------------
    m.def("build_sse_frame",
          [](py::str event_name, py::bytes data_json, uint64_t id) -> py::bytes {
              Py_ssize_t name_len = 0;
              const char* name_cstr = PyUnicode_AsUTF8AndSize(event_name.ptr(), &name_len);
              if (name_cstr == nullptr) {
                  throw py::error_already_set();
              }
              kimix::string_view data_view;
              if (!bytes_view(data_json, data_view)) {
                  throw py::error_already_set();
              }
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::codec::build_sse_frame(
                      kimix::string_view(name_cstr, static_cast<size_t>(name_len)),
                      data_view, id, out);
              }
              return to_bytes(out);
          },
          "Build one SSE frame: event:/id:/data: lines, ends with b'\\n\\n' "
          "(matches bus.py to_sse; multi-line data -> repeated data: lines).",
          py::arg("event_name"), py::arg("data_json"), py::arg("id") = 0);
}
