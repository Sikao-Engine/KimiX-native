/*
 * py_soul.cpp - Python bindings for the soul kernels (runtime_py.soul).
 *
 * BINDING-LAYER ONLY: includes pybind11, releases the GIL around every kernel
 * call, and never touches a Python object while released. This TU owns the
 * `soul` submodule (which is what py_soul_bridge.h's docstring refers to).
 *
 * CONTRACT WITH THE SHIM (python/kimix_native/soul.py), all lengths/offsets in
 * the companion `history` bytes unless stated otherwise:
 *
 *   soul.pack_structure(structure, history) -> bytes
 *        any accepted structure shape (KMB2 bytes or the legacy dict) re-encoded
 *        as ONE KMB2 blob over the SAME history bytes.
 *   soul.build_export_markdown(structure, history, opts) -> bytes
 *   soul.count_tokens(structure, history) -> int
 *   soul.extract_text(structure, history, sep=b"") -> bytes
 *   soul.balanced_cut_indices(structure, history) -> (bytes cuts, bool, int)
 *        cuts = soul.pack_u32 layout: u32 count | count * u32
 *   soul.nearest_balanced_cut_before(structure, history, index) -> (int, bool)
 *   soul.resolve_preserve_split(structure, history, preserve_depth,
 *                               balanced_cuts=True) -> dict
 *   soul.should_auto_compact(token_count, cfg) -> bool
 *   soul.build_compaction_prompt(structure, history, options=b"",
 *                                custom=b"", prompt_compact=b"",
 *                                prompt_cascade=b"") -> (bytes, int)
 *        the text arguments are UTF-8 bytes (surrogatepass from the shim).
 *   soul.prepare_compaction_input(structure, history, preserve_start_index,
 *                                 options=b"", custom=b"", prompt_compact=b"",
 *                                 prompt_cascade=b"")
 *        -> (to_compact_struct, to_compact_hist, to_preserve_struct,
 *            to_preserve_hist, compact_message_text, prompt_text, cascade_depth)
 *        the two slices are produced from the input views at
 *        preserve_start_index (the kernel slices at exactly that index).
 *   soul.build_compact_message_text(structure, history, prompt_text) -> bytes
 *   soul.normalize_history(structure, history) -> (bytes structure, bytes hist)
 *   soul.repair_tool_pairing(structure, history) -> (bytes structure, bytes hist)
 *   soul.build_request(provider, structure, history, options) -> bytes
 *
 *   soul.History(reserve_bytes=0)         persistent native arena (A1)
 *     .append(structure, history) -> int      first live index appended
 *     .replace(index, structure, history) -> bool
 *     .drop(index) -> bool ; .truncate(n) ; .clear() ; .compact() -> int
 *     .pack(first=0, count=-1) -> (bytes structure, bytes history)
 *     .text_at(index) -> bytes
 *     .save() -> bytes ; .load(blob) -> bool
 *     .__len__/.size/.generation/.bytes_owned/.bytes_live
 *     .close()/__enter__/__exit__
 *
 *   soul.Elisions()                       content-addressed blob store (A2)
 *     .put(bytes) -> int ; .get(ref) -> bytes|None ; .has(ref) -> bool
 *     .length(ref) -> int ; .count/.dedup_hits/.bytes_owned/.bytes_live
 *     .stats() -> (count, dedup_hits, bytes_owned)
 *     .save() -> bytes ; .load(blob) -> bool ; .clear()
 *     .close()/__enter__/__exit__
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/runtime.h>
#include <runtime/soul/compaction_view.h>
#include <runtime/soul/elision_store.h>
#include <runtime/soul/history_store.h>
#include <runtime/soul/kmb2.h>
#include <runtime/soul/request_builder.h>
#include <runtime/tools/export_builder.h>

#include <runtime/py/py_soul_bridge.h>
#include "py_util.h"

using kimix_py_util::bytes_view;
using kimix_py_util::str_to_string;
using kimix_py_util::to_bytes;
using kimix_py_util::to_py_str;

namespace py = pybind11;

namespace src_runtime_py_py_soul {

using kimix::runtime::soul::ElisionStore;
using kimix::runtime::soul::HistoryStore;
using kimix::runtime::soul::message_view;

// Decode (structure, history) into assembled message views. Python objects are
// touched only here, with the GIL held.
//
// The assembled views point INTO the bridge's part/call vectors, so the bridge
// must outlive them: it is returned next to the vector (a move keeps both heap
// buffers, so the spans stay valid).
struct decoded {
    kimix_soul_bridge::bridge bridge;
    kimix::vector<message_view> msgs;
};

decoded decode(py::handle structure, py::handle history) {
    decoded d;
    if (!kimix_soul_bridge::parse_structure_any(structure, history, d.bridge)) {
        throw py::value_error(
            "invalid structure/history (expected a KMB2 bytes blob or the legacy "
            "dict {roles, parts, tool_calls, tool_call_ids})");
    }
    d.msgs = kimix_soul_bridge::assemble(d.bridge);
    return d;
}

// (structure, history) -> fresh KMB2 blob pair, as a Python tuple.
py::tuple packed_pair(kimix::string &structure, kimix::string &history) {
    return py::make_tuple(to_bytes(structure), to_bytes(history));
}

kimix::builtin_tools::compact::compaction_options parse_options(
    py::handle options) {
    kimix::builtin_tools::compact::compaction_options opts;
    if (options.is_none()) {
        return opts;
    }
    if (!py::isinstance<py::dict>(options)) {
        throw py::type_error("options must be dict");
    }
    py::dict d = py::reinterpret_borrow<py::dict>(options);
    if (d.contains("avoid_cascade")) {
        opts.avoid_cascade = d["avoid_cascade"].cast<bool>();
    }
    if (d.contains("mode")) {
        kimix::string mode;
        if (!str_to_string(d["mode"], mode)) {
            throw py::type_error("options['mode'] must be str");
        }
        opts.mode = kimix::builtin_tools::compact::parse_compact_mode(mode);
    }
    if (d.contains("todos_max_items")) {
        opts.todos_max_items = d["todos_max_items"].cast<int32_t>();
    }
    if (d.contains("preserve_depth_override")) {
        opts.preserve_depth_override = d["preserve_depth_override"].cast<int32_t>();
    }
    if (d.contains("decision_section_enabled")) {
        opts.decision_section_enabled = d["decision_section_enabled"].cast<bool>();
    }
    return opts;
}

kimix::builtin_tools::compact::compaction_trigger_config parse_trigger(
    py::handle cfg) {
    kimix::builtin_tools::compact::compaction_trigger_config c;
    if (cfg.is_none()) {
        return c;
    }
    if (!py::isinstance<py::dict>(cfg)) {
        throw py::type_error("cfg must be dict");
    }
    py::dict d = py::reinterpret_borrow<py::dict>(cfg);
    if (d.contains("trigger_ratio")) {
        c.trigger_ratio = d["trigger_ratio"].cast<double>();
    }
    if (d.contains("max_context_size")) {
        c.max_context_size = d["max_context_size"].cast<int64_t>();
    }
    if (d.contains("reserved_context_size")) {
        c.reserved_context_size = d["reserved_context_size"].cast<int64_t>();
    }
    if (d.contains("max_tokens")) {
        c.max_tokens = d["max_tokens"].cast<int64_t>();
    }
    if (d.contains("tool_call_buffer_tokens")) {
        c.tool_call_buffer_tokens = d["tool_call_buffer_tokens"].cast<int64_t>();
    }
    if (d.contains("safety_margin_tokens")) {
        c.safety_margin_tokens = d["safety_margin_tokens"].cast<int64_t>();
    }
    return c;
}

py::dict preserve_split_to_dict(
    const kimix::builtin_tools::compact::preserve_split &out) {
    py::dict d;
    d["compact"] = out.compact;
    d["preserve_start_index"] = static_cast<uint64_t>(out.preserve_start_index);
    d["keep_first_message"] = out.keep_first_message;
    d["unbalanced"] = out.unbalanced;
    d["recut_fallback"] = out.recut_fallback;
    return d;
}

} // namespace src_runtime_py_py_soul
using namespace src_runtime_py_py_soul;

void py_register_soul(py::module_& m) {
    m.doc() = "Soul kernels (arena-backed history, elisions, compaction, request "
              "building, export)";

    // ------------------------------------------------------------------
    // boundary helpers
    // ------------------------------------------------------------------
    m.def("pack_structure",
          [](py::object structure, py::object history) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              py::bytes packed;
              if (!kimix_soul_bridge::pack_structure(structure, history, packed)) {
                  throw py::value_error("invalid structure/history");
              }
              return packed;
          },
          "Re-encode any accepted structure shape into ONE KMB2 blob over the "
          "same history bytes.",
          py::arg("structure"), py::arg("history"));

    // ------------------------------------------------------------------
    // B4: export markdown on the packed bridge
    // ------------------------------------------------------------------
    m.def("build_export_markdown",
          [](py::object structure, py::object history, py::dict opts) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;

              kimix::string session_id, work_dir, exported_at;
              uint64_t token_count = 0;
              bool include_timestamps = false;
              if (opts.contains("session_id") &&
                  !str_to_string(opts["session_id"], session_id)) {
                  throw py::type_error("session_id must be str");
              }
              if (opts.contains("work_dir") && !str_to_string(opts["work_dir"], work_dir)) {
                  throw py::type_error("work_dir must be str");
              }
              if (opts.contains("exported_at") &&
                  !str_to_string(opts["exported_at"], exported_at)) {
                  throw py::type_error("exported_at must be str");
              }
              if (opts.contains("token_count")) {
                  token_count = opts["token_count"].cast<uint64_t>();
              }
              if (opts.contains("include_timestamps")) {
                  include_timestamps = opts["include_timestamps"].cast<bool>();
              }
              kimix::runtime::tools::export_options eo;
              eo.session_id = session_id;
              eo.work_dir = work_dir;
              eo.exported_at = exported_at;
              eo.token_count = token_count;
              eo.include_timestamps = include_timestamps;

              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::build_export_markdown(msgs, eo, out);
              }
              return to_bytes(out);
          },
          "Full session export markdown; opts keys: session_id, work_dir, "
          "exported_at, token_count, include_timestamps.",
          py::arg("structure"), py::arg("history"), py::arg("opts") = py::dict());

    // ------------------------------------------------------------------
    // B3: compaction / prune decision kernels on the arena
    // ------------------------------------------------------------------
    m.def("count_tokens",
          [](py::object structure, py::object history) -> int64_t {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              int64_t out = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  out = kimix::runtime::soul::count_tokens(msgs);
              }
              return out;
          },
          "Heuristic token estimate over every text part of the history.",
          py::arg("structure"), py::arg("history"));

    m.def("extract_text",
          [](py::object structure, py::object history, py::object sep) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              kimix::string_view sep_view;
              if (sep.is_none()) {
                  sep_view = kimix::string_view();
              } else if (!bytes_view(py::reinterpret_borrow<py::bytes>(sep), sep_view)) {
                  throw py::type_error("sep must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::extract_text(msgs, sep_view, out);
              }
              return to_bytes(out);
          },
          "Join every text part of the history with sep (bytes).",
          py::arg("structure"), py::arg("history"), py::arg("sep") = py::bytes());

    m.def("balanced_cut_indices",
          [](py::object structure, py::object history) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::vector<uint32_t> cuts;
              bool unbalanced = false;
              int64_t unbalanced_index = -1;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::balanced_cut_indices(msgs, cuts, unbalanced,
                                                             unbalanced_index);
              }
              return py::make_tuple(kimix_soul_bridge::pack_u32(cuts), unbalanced,
                                    unbalanced_index);
          },
          "tool_pairing.balanced_cut_indices -> (packed u32 cuts, unbalanced, "
          "unbalanced_index).",
          py::arg("structure"), py::arg("history"));

    m.def("nearest_balanced_cut_before",
          [](py::object structure, py::object history, uint32_t index) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              bool unbalanced = false;
              uint32_t cut = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  cut = kimix::runtime::soul::nearest_balanced_cut_before(msgs, index,
                                                                          unbalanced);
              }
              return py::make_tuple(cut, unbalanced);
          },
          "Largest balanced cut <= index -> (cut, unbalanced).",
          py::arg("structure"), py::arg("history"), py::arg("index"));

    m.def("resolve_preserve_split",
          [](py::object structure, py::object history, int32_t preserve_depth,
             bool balanced_cuts) -> py::dict {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::builtin_tools::compact::preserve_split out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::resolve_preserve_split(msgs, preserve_depth,
                                                               balanced_cuts, out);
              }
              return preserve_split_to_dict(out);
          },
          "SimpleCompaction.prepare's boundary math.",
          py::arg("structure"), py::arg("history"), py::arg("preserve_depth"),
          py::arg("balanced_cuts") = true);

    m.def("should_auto_compact",
          [](int64_t token_count, py::object cfg) -> bool {
              const auto parsed = parse_trigger(cfg);
              bool out = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  out = kimix::runtime::soul::should_auto_compact(token_count, parsed);
              }
              return out;
          },
          "True when the token count crosses the auto-compaction threshold.",
          py::arg("token_count"), py::arg("cfg") = py::dict());

    m.def("build_compaction_prompt",
          [](py::object structure, py::object history, py::object options,
             py::object custom, py::object prompt_compact,
             py::object prompt_cascade) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              const auto opts = parse_options(options);
              kimix::string custom_s, prompt_s, cascade_s;
              if (!custom.is_none() && !str_to_string(custom, custom_s)) {
                  throw py::type_error("custom_instruction must be str");
              }
              if (!prompt_compact.is_none() &&
                  !str_to_string(prompt_compact, prompt_s)) {
                  throw py::type_error("prompt_compact must be str");
              }
              if (!prompt_cascade.is_none() &&
                  !str_to_string(prompt_cascade, cascade_s)) {
                  throw py::type_error("prompt_compact_cascade must be str");
              }
              kimix::builtin_tools::compact::compaction_prompt_output out;
              bool ok = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  ok = kimix::runtime::soul::build_compaction_prompt(
                      msgs, opts, custom_s, prompt_s, cascade_s, out);
              }
              if (!ok) {
                  throw py::value_error("build_compaction_prompt failed");
              }
              return py::make_tuple(to_bytes(out.prompt_text), out.cascade_depth);
          },
          "Assemble the compaction instruction prompt -> (prompt_text bytes, "
          "cascade_depth).",
          py::arg("structure"), py::arg("history"), py::arg("options") = py::dict(),
          py::arg("custom_instruction") = "", py::arg("prompt_compact") = "",
          py::arg("prompt_compact_cascade") = "");

    m.def("build_compact_message_text",
          [](py::object structure, py::object history, py::object prompt_text) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::string prompt;
              if (!prompt_text.is_none() && !str_to_string(prompt_text, prompt)) {
                  throw py::type_error("prompt_text must be str");
              }
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::build_compact_message_text(msgs, prompt, out);
              }
              return to_bytes(out);
          },
          "Legacy flattened compact_message text.",
          py::arg("structure"), py::arg("history"), py::arg("prompt_text"));

    m.def("prepare_compaction_input",
          [](py::object structure, py::object history, int64_t preserve_start_index,
             py::object options, py::object custom, py::object prompt_compact,
             py::object prompt_cascade) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              const auto opts = parse_options(options);
              kimix::string custom_s, prompt_s, cascade_s;
              if (!custom.is_none() && !str_to_string(custom, custom_s)) {
                  throw py::type_error("custom_instruction must be str");
              }
              if (!prompt_compact.is_none() &&
                  !str_to_string(prompt_compact, prompt_s)) {
                  throw py::type_error("prompt_compact must be str");
              }
              if (!prompt_cascade.is_none() &&
                  !str_to_string(prompt_cascade, cascade_s)) {
                  throw py::type_error("prompt_compact_cascade must be str");
              }
              const size_t total = msgs.size();
              size_t cut = preserve_start_index < 0
                               ? 0
                               : static_cast<size_t>(preserve_start_index);
              if (cut > total) {
                  cut = total;
              }
              kimix::builtin_tools::compact::prepare_result out;
              bool ok = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  ok = kimix::runtime::soul::prepare_compaction_input(
                      msgs, cut, opts, custom_s, prompt_s, cascade_s, out);
              }
              if (!ok) {
                  throw py::value_error("prepare_compaction_input failed "
                                        "(nothing to compact)");
              }
              // The two slices come from the input views at the same index the
              // kernel sliced at, so the blobs stay byte-identical to the input.
              kimix::string c_struct, c_hist, p_struct, p_hist;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::kmb2_build(
                      kimix::span<const message_view>(msgs.data(), cut), c_struct,
                      c_hist);
                  kimix::runtime::soul::kmb2_build(
                      kimix::span<const message_view>(msgs.data() + cut, total - cut),
                      p_struct, p_hist);
              }
              return py::make_tuple(to_bytes(c_struct), to_bytes(c_hist),
                                    to_bytes(p_struct), to_bytes(p_hist),
                                    to_bytes(out.compact_message_text),
                                    to_bytes(out.prompt_text), out.cascade_depth);
          },
          "Slice + prompt + legacy compact message -> (to_compact_structure, "
          "to_compact_history, to_preserve_structure, to_preserve_history, "
          "compact_message_text, prompt_text, cascade_depth).",
          py::arg("structure"), py::arg("history"),
          py::arg("preserve_start_index"), py::arg("options") = py::dict(),
          py::arg("custom_instruction") = "", py::arg("prompt_compact") = "",
          py::arg("prompt_compact_cascade") = "");

    // ------------------------------------------------------------------
    // B1: normalize / repair / build_request
    // ------------------------------------------------------------------
    m.def("normalize_history",
          [](py::object structure, py::object history) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::string out_structure, out_history;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::normalize_history(msgs, out_structure,
                                                          out_history);
              }
              return packed_pair(out_structure, out_history);
          },
          "Merge adjacent user messages -> (structure, history).",
          py::arg("structure"), py::arg("history"));

    m.def("repair_tool_pairing",
          [](py::object structure, py::object history) -> py::tuple {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;
              kimix::string out_structure, out_history;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::repair_tool_pairing(msgs, out_structure,
                                                            out_history);
              }
              return packed_pair(out_structure, out_history);
          },
          "Repair assistant tool_call / tool-result pairing -> (structure, history).",
          py::arg("structure"), py::arg("history"));

    m.def("build_request",
          [](py::object structure, py::object history, py::dict options) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              decoded holder = decode(structure, history);
              kimix::span<const message_view> msgs = holder.msgs;

              // The option strings must outlive the kernel call.
              kimix::string model, system_prompt, tool_choice, reasoning_effort,
                  reasoning_key, tools_json;
              kimix::runtime::soul::request_options ro;
              if (options.contains("provider")) {
                  kimix::string name;
                  if (!str_to_string(options["provider"], name)) {
                      throw py::type_error("options['provider'] must be str");
                  }
                  if (!kimix::runtime::soul::parse_provider(name, ro.provider)) {
                      throw py::value_error("unknown provider name");
                  }
              }
              if (options.contains("preserve_thinking")) {
                  ro.preserve_thinking = options["preserve_thinking"].cast<bool>();
              }
              if (options.contains("exclude_none")) {
                  ro.exclude_none = options["exclude_none"].cast<bool>();
              }
              if (options.contains("stream")) {
                  ro.stream = options["stream"].cast<bool>();
              }
              if (options.contains("max_tokens")) {
                  ro.max_tokens = options["max_tokens"].cast<int64_t>();
              }
              if (options.contains("temperature")) {
                  ro.temperature = options["temperature"].cast<double>();
                  ro.has_temperature = true;
              }
              const auto set = [](py::dict d, const char *key, kimix::string &slot,
                                  kimix::string_view &view, const char *what) {
                  if (!d.contains(key)) {
                      return;
                  }
                  py::object v = d[key];
                  if (v.is_none()) {
                      return;
                  }
                  if (!str_to_string(v, slot)) {
                      throw py::type_error(std::string(what) + " must be str");
                  }
                  view = kimix::string_view(slot);
              };
              set(options, "model", model, ro.model, "options['model']");
              set(options, "system_prompt", system_prompt, ro.system_prompt,
                  "options['system_prompt']");
              set(options, "tool_choice", tool_choice, ro.tool_choice,
                  "options['tool_choice']");
              set(options, "reasoning_effort", reasoning_effort, ro.reasoning_effort,
                  "options['reasoning_effort']");
              set(options, "reasoning_key", reasoning_key, ro.reasoning_key,
                  "options['reasoning_key']");
              set(options, "tools_json", tools_json, ro.tools_json,
                  "options['tools_json']");

              kimix::string out;
              bool ok = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  ok = kimix::runtime::soul::build_request(msgs, ro, out);
              }
              if (!ok) {
                  kimix::string why = "build_request failed";
                  if (!out.empty()) {
                      why += ": ";
                      why.append(out);
                  }
                  throw py::value_error(why.c_str());
              }
              return to_bytes(out);
          },
          "Build the provider request body -> bytes. options keys: provider, "
          "preserve_thinking, exclude_none, stream, max_tokens, temperature, "
          "model, system_prompt, tool_choice, reasoning_effort, reasoning_key, "
          "tools_json.",
          py::arg("structure"), py::arg("history"), py::arg("options") = py::dict());

    // ------------------------------------------------------------------
    // A1: soul.History
    // ------------------------------------------------------------------
    py::class_<HistoryStore>(m, "History",
                             "Native conversation-history arena (report A1). "
                             "All indices are live ordinals.")
        .def(py::init<uint64_t>(), py::arg("reserve_bytes") = 0,
             "History(reserve_bytes=0)")
        .def("append",
             [](HistoryStore &h, py::object structure, py::object history) -> uint32_t {
                 if (!py::isinstance<py::bytes>(history)) {
                     throw py::type_error("history must be bytes");
                 }
                 py::bytes owned;
                 kimix::string_view packed_view;
                 kimix::string_view history_view;
                 if (!kimix_soul_bridge::packed_structure_of(structure, history, owned,
                                                             packed_view) ||
                     !kimix_soul_bridge::bridge_bytes_view(
                         py::reinterpret_borrow<py::bytes>(history), history_view)) {
                     throw py::value_error("invalid structure/history");
                 }
                 uint32_t first = 0;
                 uint32_t count = 0;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     first = h.size();
                     count = h.append_packed(packed_view, history_view);
                 }
                 if (count == 0) {
                     throw py::value_error("no messages appended (malformed blob)");
                 }
                 return first;
             },
             "Append every message of a KMB2 blob -> first live index.",
             py::arg("structure"), py::arg("history"))
        .def("replace",
             [](HistoryStore &h, uint32_t index, py::object structure,
                py::object history) -> bool {
                 if (!py::isinstance<py::bytes>(history)) {
                     throw py::type_error("history must be bytes");
                 }
                 py::bytes owned;
                 kimix::string_view packed_view;
                 kimix::string_view history_view;
                 if (!kimix_soul_bridge::packed_structure_of(structure, history, owned,
                                                             packed_view) ||
                     !kimix_soul_bridge::bridge_bytes_view(
                         py::reinterpret_borrow<py::bytes>(history), history_view)) {
                     throw py::value_error("invalid structure/history");
                 }
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = h.replace_packed(index, packed_view, history_view);
                 }
                 return ok;
             },
             "Replace one message in place.", py::arg("index"),
             py::arg("structure"), py::arg("history"))
        .def("drop", &HistoryStore::drop, py::arg("index"))
        .def("truncate", &HistoryStore::truncate, py::arg("count"))
        .def("clear", &HistoryStore::clear)
        .def("compact", &HistoryStore::compact,
             "Reclaim tombstones; returns the bytes freed.")
        .def("pack",
             [](const HistoryStore &h, uint32_t first, int64_t count) -> py::tuple {
                 kimix::string structure, history;
                 uint32_t take = count < 0 ? UINT32_MAX : static_cast<uint32_t>(count);
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.pack(first, take, structure, history);
                 }
                 return packed_pair(structure, history);
             },
             "pack(first=0, count=-1) -> (structure, history)", py::arg("first") = 0,
             py::arg("count") = -1)
        .def("text_at",
             [](const HistoryStore &h, uint32_t index) -> py::bytes {
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.text_of(index, out);
                 }
                 return to_bytes(out);
             },
             py::arg("index"))
        .def("save",
             [](const HistoryStore &h) -> py::bytes {
                 kimix::string blob;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.save(blob);
                 }
                 return to_bytes(blob);
             })
        .def("load",
             [](HistoryStore &h, py::bytes blob) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(blob, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 return h.load(view);
             },
             py::arg("blob"))
        .def("__len__", &HistoryStore::size)
        .def("size", &HistoryStore::size)
        .def("generation", &HistoryStore::generation)
        .def("bytes_owned", &HistoryStore::bytes_owned)
        .def("bytes_live", &HistoryStore::bytes_live)
        .def("close",
             [](HistoryStore &h) {
                 kimix::runtime::common::gil_scoped_release release;
                 h.clear();
             })
        .def("__enter__", [](HistoryStore &h) -> HistoryStore & { return h; })
        .def("__exit__",
             [](HistoryStore &h, py::object, py::object, py::object) {
                 h.clear();
                 return false;
             });

    // ------------------------------------------------------------------
    // A2: soul.Elisions
    // ------------------------------------------------------------------
    py::class_<ElisionStore>(m, "Elisions",
                             "Content-addressed blob store for pruned message "
                             "text (report A2).")
        .def(py::init<>())
        .def("put",
             [](ElisionStore &s, py::bytes text) -> py::int_ {
                 kimix::string_view view;
                 if (!bytes_view(text, view)) {
                     throw py::error_already_set();
                 }
                 uint64_t ref = 0;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ref = s.put(view);
                 }
                 return py::int_(ref);
             },
             "put(bytes) -> int (content-addressed, dedups)", py::arg("text"))
        .def("get",
             [](const ElisionStore &s, uint64_t ref) -> py::object {
                 kimix::string_view view;
                 if (!s.get(ref, view)) {
                     return py::none();
                 }
                 return py::bytes(view.data(), view.size());
             },
             py::arg("ref"))
        .def("has", [](const ElisionStore &s, uint64_t ref) -> bool {
            return s.has(ref);
        }, py::arg("ref"))
        .def("length", [](const ElisionStore &s, uint64_t ref) -> uint64_t {
            return s.length(ref);
        }, py::arg("ref"))
        .def("count", &ElisionStore::count)
        .def("dedup_hits", &ElisionStore::dedup_hits)
        .def("bytes_owned", &ElisionStore::bytes_owned)
        .def("bytes_live", &ElisionStore::bytes_live)
        .def("stats",
             [](const ElisionStore &s) -> py::tuple {
                 return py::make_tuple(s.count(), s.dedup_hits(), s.bytes_owned());
             },
             "stats() -> (count, dedup_hits, bytes_owned)")
        .def("save",
             [](const ElisionStore &s) -> py::bytes {
                 kimix::string blob;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     s.save(blob);
                 }
                 return to_bytes(blob);
             })
        .def("load",
             [](ElisionStore &s, py::bytes blob) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(blob, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 return s.load(view);
             },
             py::arg("blob"))
        .def("clear", &ElisionStore::clear)
        .def("close",
             [](ElisionStore &s) {
                 kimix::runtime::common::gil_scoped_release release;
                 s.clear();
             })
        .def("__enter__", [](ElisionStore &s) -> ElisionStore & { return s; })
        .def("__exit__",
             [](ElisionStore &s, py::object, py::object, py::object) {
                 s.clear();
                 return false;
             });
}
