/*
 * py_soul_bridge.h - shared message-view bridge parser (binding layer).
 *
 * BINDING-LAYER ONLY (includes pybind11; the runtime_py target has no unity
 * build, but the helpers are inline so multiple TUs may include this header).
 *
 * Parses the bridge `structure` dict (see py_soul.cpp docstring) into C++
 * message_view spans pointing into the caller's UTF-8 bytes buffer. The
 * pybind11::bytes `history` object must stay alive for the kernel call (it is the
 * call argument), so all spans remain valid while the kernel runs.
 */

#pragma once

#include <pybind11/pybind11.h>

#include <runtime/soul/message_view.h>

namespace kimix_soul_bridge {

using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::tool_call_view;

inline bool bridge_bytes_view(pybind11::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

inline pybind11::bytes bridge_to_bytes(const kimix::string& s) {
    return pybind11::bytes(s.data(), s.size());
}

inline bool bridge_get_int(PyObject* obj, int64_t& out) {
    if (!PyLong_Check(obj)) {
        return false;
    }
    out = PyLong_AsLongLong(obj);
    return !(out == -1 && PyErr_Occurred());
}

inline bool bridge_parse_span(PyObject* item, int64_t& s, int64_t& e,
                              int64_t* kind = nullptr) {
    if (!PyTuple_Check(item)) {
        return false;
    }
    const Py_ssize_t n = PyTuple_GET_SIZE(item);
    const Py_ssize_t need = kind != nullptr ? 3 : 2;
    if (n != need) {
        return false;
    }
    int64_t vals[3] = {0, 0, 0};
    for (Py_ssize_t i = 0; i < need; ++i) {
        if (!bridge_get_int(PyTuple_GET_ITEM(item, i), vals[i])) {
            return false;
        }
    }
    s = vals[0];
    e = vals[1];
    if (kind != nullptr) {
        *kind = vals[2];
    }
    return true;
}

// Assembled bridge state: spans point into the caller's bytes buffer and
// into the vectors below (both stay alive for the kernel call).
struct bridge {
    const char* buffer = nullptr;
    size_t buffer_len = 0;
    kimix::vector<uint8_t> roles;
    kimix::vector<part_view> parts;
    kimix::vector<tool_call_view> tool_calls;
    kimix::vector<uint32_t> part_begin;
    kimix::vector<uint32_t> part_count;
    kimix::vector<uint32_t> call_begin;
    kimix::vector<uint32_t> call_count;
    kimix::vector<kimix::string_view> call_ids; // empty view = None
};

inline bool parse_structure(pybind11::handle structure, pybind11::handle history, bridge& b) {
    if (!PyDict_Check(structure.ptr()) || !PyBytes_Check(history.ptr())) {
        return false;
    }
    char* buf = nullptr;
    Py_ssize_t blen = 0;
    if (PyBytes_AsStringAndSize(history.ptr(), &buf, &blen) < 0) {
        return false;
    }
    b.buffer = buf;
    b.buffer_len = static_cast<size_t>(blen);

    pybind11::dict d = pybind11::reinterpret_borrow<pybind11::dict>(structure);

    // roles -------------------------------------------------------------
    if (!d.contains("roles")) {
        return false;
    }
    pybind11::list roles = pybind11::reinterpret_borrow<pybind11::list>(d["roles"]);
    const Py_ssize_t n = PyList_GET_SIZE(roles.ptr());
    b.roles.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        int64_t v = 0;
        if (!bridge_get_int(PyList_GET_ITEM(roles.ptr(), i), v)) {
            return false;
        }
        b.roles.push_back(static_cast<uint8_t>(v));
    }

    // parts -------------------------------------------------------------
    b.part_begin.assign(static_cast<size_t>(n), 0);
    b.part_count.assign(static_cast<size_t>(n), 0);
    pybind11::object parts = pybind11::none();
    if (d.contains("parts")) {
        parts = d["parts"];
    }
    if (!parts.is_none()) {
        pybind11::list parts_list = pybind11::reinterpret_borrow<pybind11::list>(parts);
        if (PyList_GET_SIZE(parts_list.ptr()) != n) {
            return false;
        }
        for (Py_ssize_t i = 0; i < n; ++i) {
            b.part_begin[static_cast<size_t>(i)] =
                static_cast<uint32_t>(b.parts.size());
            pybind11::object pl =
                pybind11::reinterpret_borrow<pybind11::object>(
                    PyList_GET_ITEM(parts_list.ptr(), i));
            if (pl.is_none()) {
                continue;
            }
            pybind11::list plist = pybind11::reinterpret_borrow<pybind11::list>(pl);
            const Py_ssize_t m = PyList_GET_SIZE(plist.ptr());
            for (Py_ssize_t j = 0; j < m; ++j) {
                int64_t s = 0, e = 0, kind = 0;
                if (!bridge_parse_span(PyList_GET_ITEM(plist.ptr(), j), s, e, &kind)) {
                    return false;
                }
                if (s < 0 || e < s || static_cast<size_t>(e) > b.buffer_len) {
                    return false;
                }
                part_view pv;
                pv.kind = static_cast<part_kind>(static_cast<uint8_t>(kind));
                pv.text = kimix::string_view(b.buffer + s, static_cast<size_t>(e - s));
                b.parts.push_back(pv);
            }
            b.part_count[static_cast<size_t>(i)] = static_cast<uint32_t>(
                b.parts.size() - b.part_begin[static_cast<size_t>(i)]);
        }
    }

    // tool_calls ---------------------------------------------------------
    b.call_begin.assign(static_cast<size_t>(n), 0);
    b.call_count.assign(static_cast<size_t>(n), 0);
    pybind11::object calls = pybind11::none();
    if (d.contains("tool_calls")) {
        calls = d["tool_calls"];
    }
    if (!calls.is_none()) {
        pybind11::list calls_list = pybind11::reinterpret_borrow<pybind11::list>(calls);
        if (PyList_GET_SIZE(calls_list.ptr()) != n) {
            return false;
        }
        for (Py_ssize_t i = 0; i < n; ++i) {
            b.call_begin[static_cast<size_t>(i)] =
                static_cast<uint32_t>(b.tool_calls.size());
            pybind11::object cl =
                pybind11::reinterpret_borrow<pybind11::object>(PyList_GET_ITEM(calls_list.ptr(), i));
            if (cl.is_none()) {
                continue;
            }
            pybind11::list clist = pybind11::reinterpret_borrow<pybind11::list>(cl);
            const Py_ssize_t m = PyList_GET_SIZE(clist.ptr());
            for (Py_ssize_t j = 0; j < m; ++j) {
                int64_t ids = 0, ide = 0, ns = 0, ne = 0, as_ = 0, ae = 0;
                PyObject* item = PyList_GET_ITEM(clist.ptr(), j);
                if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 6 ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 0), ids) ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 1), ide) ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 2), ns) ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 3), ne) ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 4), as_) ||
                    !bridge_get_int(PyTuple_GET_ITEM(item, 5), ae)) {
                    return false;
                }
                const auto check_span = [&](int64_t s, int64_t e) {
                    return s >= 0 && e >= s && static_cast<size_t>(e) <= b.buffer_len;
                };
                if (!check_span(ids, ide) || !check_span(ns, ne)) {
                    return false;
                }
                if (as_ >= 0 && (ae < as_ || static_cast<size_t>(ae) > b.buffer_len)) {
                    return false;
                }
                tool_call_view tc;
                tc.id = kimix::string_view(b.buffer + ids, static_cast<size_t>(ide - ids));
                tc.name = kimix::string_view(b.buffer + ns, static_cast<size_t>(ne - ns));
                if (as_ >= 0) {
                    tc.arguments =
                        kimix::string_view(b.buffer + as_, static_cast<size_t>(ae - as_));
                }
                b.tool_calls.push_back(tc);
            }
            b.call_count[static_cast<size_t>(i)] = static_cast<uint32_t>(
                b.tool_calls.size() - b.call_begin[static_cast<size_t>(i)]);
        }
    }

    // tool_call_ids ------------------------------------------------------
    b.call_ids.assign(static_cast<size_t>(n), kimix::string_view());
    pybind11::object cids = pybind11::none();
    if (d.contains("tool_call_ids")) {
        cids = d["tool_call_ids"];
    }
    if (!cids.is_none()) {
        pybind11::list cids_list = pybind11::reinterpret_borrow<pybind11::list>(cids);
        if (PyList_GET_SIZE(cids_list.ptr()) != n) {
            return false;
        }
        for (Py_ssize_t i = 0; i < n; ++i) {
            pybind11::object item =
                pybind11::reinterpret_borrow<pybind11::object>(PyList_GET_ITEM(cids_list.ptr(), i));
            if (item.is_none()) {
                continue;
            }
            int64_t s = 0, e = 0;
            if (!bridge_parse_span(item.ptr(), s, e)) {
                return false;
            }
            if (s < 0 || e < s || static_cast<size_t>(e) > b.buffer_len) {
                return false;
            }
            b.call_ids[static_cast<size_t>(i)] =
                kimix::string_view(b.buffer + s, static_cast<size_t>(e - s));
        }
    }
    return true;
}

// Assemble message_view spans from the parsed bridge state.
inline kimix::vector<message_view> assemble(const bridge& b) {
    kimix::vector<message_view> msgs;
    msgs.reserve(b.roles.size());
    for (size_t i = 0; i < b.roles.size(); ++i) {
        message_view mv;
        mv.role = b.roles[i];
        mv.parts = kimix::span<const part_view>(
            b.parts.data() + b.part_begin[i], b.part_count[i]);
        mv.tool_calls = kimix::span<const tool_call_view>(
            b.tool_calls.data() + b.call_begin[i], b.call_count[i]);
        mv.tool_call_id = b.call_ids[i];
        msgs.push_back(mv);
    }
    return msgs;
}

} // namespace kimix_soul_bridge
