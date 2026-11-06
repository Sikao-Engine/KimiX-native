/*
 * py_soul_bridge.h - shared message-view bridge parser (binding layer).
 *
 * BINDING-LAYER ONLY (includes pybind11; the runtime_py target builds with
 * unity build, and the helpers are inline in a named namespace so multiple
 * merged TUs may include this header — same contract as py_util.h).
 *
 * Two input shapes reach the kernels:
 *
 *   1. `structure` = ONE packed KMB2 `bytes` (see runtime/soul/kmb2.h) plus the
 *      companion `history` bytes. This is the contract every new entry point
 *      uses (report §2: bytes + span tables, never list[dict]).
 *   2. `structure` = the legacy dict {roles, parts, tool_calls, tool_call_ids}
 *      plus `history`. Kept as the fallback and as the parity reference for
 *      the shim's `_compat` path (python/kimix_native/_structure.py).
 *
 * Both decoders converge on the same `kimix::vector<message_view>` through
 * assemble(), so a kernel cannot tell which side encoded the history.
 *
 * The pybind11::bytes `history` object must stay alive for the kernel call (it
 * is the call argument), so all spans remain valid while the kernel runs.
 */

#pragma once

#include <pybind11/pybind11.h>

#include <runtime/common/le_bytes.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>

#include <climits>

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

// ── KMB2 (packed) decoder ────────────────────────────────────────────────────

// Decode the packed KMB2 pair into `b`. Same post-condition as
// parse_structure(): `b` is filled only on success, every span is validated,
// false means malformed input (the caller turns that into py::value_error).
inline bool parse_structure_packed(pybind11::handle structure,
                                   pybind11::handle history, bridge& b) {
    if (!PyBytes_Check(structure.ptr()) || !PyBytes_Check(history.ptr())) {
        return false;
    }
    char* buf = nullptr;
    Py_ssize_t blen = 0;
    if (PyBytes_AsStringAndSize(history.ptr(), &buf, &blen) < 0) {
        return false;
    }
    kimix::runtime::soul::kmb2_view v;
    kimix::string_view structure_view;
    if (!bridge_bytes_view(
            pybind11::reinterpret_borrow<pybind11::bytes>(structure),
            structure_view)) {
        return false;
    }
    if (!kimix::runtime::soul::kmb2_parse(structure_view,
                                          kimix::string_view(buf, static_cast<size_t>(blen)),
                                          v)) {
        return false;
    }

    b.buffer = buf;
    b.buffer_len = static_cast<size_t>(blen);
    const size_t n = v.msg_count;
    if (n > static_cast<size_t>(INT32_MAX)) {
        return false;
    }
    b.roles.assign(n, 0);
    b.part_begin.assign(n, 0);
    b.part_count.assign(n, 0);
    b.call_begin.assign(n, 0);
    b.call_count.assign(n, 0);
    b.call_ids.assign(n, kimix::string_view());
    b.parts.clear();
    b.parts.reserve(v.part_total);
    b.tool_calls.clear();
    b.tool_calls.reserve(v.call_total);

    for (size_t i = 0; i < n; ++i) {
        kimix::runtime::soul::kmb2_message_record m;
        if (!v.message(static_cast<uint32_t>(i), m)) {
            return false;
        }
        b.roles[i] = m.role;
        b.part_begin[i] = static_cast<uint32_t>(b.parts.size());
        b.part_count[i] = m.part_count;
        b.call_begin[i] = static_cast<uint32_t>(b.tool_calls.size());
        b.call_count[i] = m.call_count;
        if ((m.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) != 0) {
            b.call_ids[i] = v.span(m.tcid_start, m.tcid_end);
        }
        for (uint32_t j = 0; j < m.part_count; ++j) {
            kimix::runtime::soul::kmb2_part_record pr;
            if (!v.part(m.part_first + j, pr)) {
                return false;
            }
            part_view pv;
            pv.kind = static_cast<part_kind>(pr.kind);
            pv.text = v.span(pr.start, pr.end);
            b.parts.push_back(pv);
        }
        for (uint32_t j = 0; j < m.call_count; ++j) {
            kimix::runtime::soul::kmb2_call_record cr;
            if (!v.call(m.call_first + j, cr)) {
                return false;
            }
            tool_call_view tc;
            tc.id = v.span(cr.id_s, cr.id_e);
            tc.name = v.span(cr.name_s, cr.name_e);
            if (cr.args_s >= 0) {
                tc.arguments = v.span(cr.args_s, cr.args_e);
            }
            b.tool_calls.push_back(tc);
        }
    }
    return true;
}

// Dispatch helper every binding uses: KMB2 bytes -> packed decoder, anything
// else -> the legacy dict decoder (which also rejects a non-dict).
inline bool parse_structure_any(pybind11::handle structure,
                                pybind11::handle history, bridge& b) {
    if (PyBytes_Check(structure.ptr())) {
        return parse_structure_packed(structure, history, b);
    }
    return parse_structure(structure, history, b);
}

// ── KMB2 (packed) re-encoder ─────────────────────────────────────────────────

// Re-encode any accepted input shape into ONE KMB2 blob. The spans already
// point into `history`, whose bytes the caller keeps, so only the table half is
// rebuilt. Returns false on malformed input; `out` is untouched then.
inline bool pack_structure(pybind11::handle structure, pybind11::handle history,
                           pybind11::bytes& out) {
    bridge b;
    if (!parse_structure_any(structure, history, b)) {
        return false;
    }
    kimix::string packed;
    kimix::string unused_history;
    kimix::runtime::soul::kmb2_writer w(packed, unused_history);

    const auto offset_of = [&b](kimix::string_view v) -> int32_t {
        if (v.data() == nullptr) {
            return -1;
        }
        const ptrdiff_t off = v.data() - b.buffer;
        if (off < 0 || static_cast<size_t>(off) > b.buffer_len) {
            return -1;
        }
        return static_cast<int32_t>(off);
    };

    kimix::vector<kimix::runtime::soul::kmb2_part_record> parts;
    kimix::vector<kimix::runtime::soul::kmb2_call_record> calls;
    for (size_t i = 0; i < b.roles.size(); ++i) {
        parts.clear();
        parts.reserve(b.part_count[i]);
        for (uint32_t j = 0; j < b.part_count[i]; ++j) {
            const part_view& pv = b.parts[b.part_begin[i] + j];
            kimix::runtime::soul::kmb2_part_record pr{};
            pr.start = offset_of(pv.text);
            pr.end = pr.start >= 0
                         ? pr.start + static_cast<int32_t>(pv.text.size())
                         : -1;
            if (pr.start < 0) {
                return false;
            }
            pr.kind = static_cast<uint8_t>(pv.kind);
            parts.push_back(pr);
        }
        calls.clear();
        calls.reserve(b.call_count[i]);
        for (uint32_t j = 0; j < b.call_count[i]; ++j) {
            const tool_call_view& tv = b.tool_calls[b.call_begin[i] + j];
            kimix::runtime::soul::kmb2_call_record cr{};
            cr.id_s = offset_of(tv.id);
            if (cr.id_s < 0) {
                return false;
            }
            cr.id_e = cr.id_s + static_cast<int32_t>(tv.id.size());
            cr.name_s = offset_of(tv.name);
            if (cr.name_s < 0) {
                return false;
            }
            cr.name_e = cr.name_s + static_cast<int32_t>(tv.name.size());
            if (tv.arguments.data() == nullptr) {
                cr.args_s = -1;
                cr.args_e = -1;
            } else {
                cr.args_s = offset_of(tv.arguments);
                if (cr.args_s < 0) {
                    return false;
                }
                cr.args_e = cr.args_s + static_cast<int32_t>(tv.arguments.size());
            }
            calls.push_back(cr);
        }
        int32_t tcid_s = -1;
        int32_t tcid_e = -1;
        if (b.call_ids[i].data() != nullptr) {
            tcid_s = offset_of(b.call_ids[i]);
            if (tcid_s < 0) {
                return false;
            }
            tcid_e = tcid_s + static_cast<int32_t>(b.call_ids[i].size());
        }
        w.add_raw(b.roles[i], parts, calls, tcid_s, tcid_e);
    }
    w.finish();
    out = pybind11::bytes(packed.data(), packed.size());
    return true;
}

// ── packed integer arrays ────────────────────────────────────────────────────

// `u32 count | count * u32` - one Python object instead of a list[int], so a
// cut-index array (251 entries on a 500-message history) never materialises as
// 251 boxed ints. Decoded in the shim with memoryview/array.
inline pybind11::bytes pack_u32(const kimix::vector<uint32_t>& values) {
    kimix::string buf;
    buf.resize(4 + values.size() * 4);
    kimix::runtime::common::put_le32(buf.data(),
                                     static_cast<uint32_t>(values.size()));
    char* p = buf.data() + 4;
    for (uint32_t v : values) {
        kimix::runtime::common::put_le32(p, v);
        p += 4;
    }
    return pybind11::bytes(buf.data(), buf.size());
}

// The KMB2 blob of `structure`, whatever shape it arrived in: an already-packed
// blob is borrowed in place, the legacy dict is re-encoded into `owned`. `owned`
// must outlive the returned view.
inline bool packed_structure_of(pybind11::handle structure,
                                pybind11::handle history,
                                pybind11::bytes& owned,
                                kimix::string_view& out) {
    if (PyBytes_Check(structure.ptr())) {
        return bridge_bytes_view(
            pybind11::reinterpret_borrow<pybind11::bytes>(structure), out);
    }
    if (!pack_structure(structure, history, owned)) {
        return false;
    }
    return bridge_bytes_view(owned, out);
}

} // namespace kimix_soul_bridge
