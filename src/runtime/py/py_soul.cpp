#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/soul/message_view.h>
#include <runtime/soul/payload_builder.h>
#include <runtime/soul/normalize_tool_call_ids.h>
#include <runtime/soul/normalize_plan.h>
#include <runtime/soul/prune_scanner.h>
#include <runtime/soul/reminder_stripper.h>
#include <runtime/soul/prompt_builder.h>
#include <runtime/py/py_soul_bridge.h>

namespace py = pybind11;

void py_register_soul(py::module_& m) {
    using namespace kimix_soul_bridge;
    m.doc() = "Soul kernels: payload conversion, tool-call id normalization, "
              "prune scans, reminder stripping, normalize plan, compaction "
              "prompt";

    m.def("build_payload",
          [](py::bytes history, py::dict structure, bool preserved_thinking) -> py::bytes {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              kimix::string out_json;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::build_payload(msgs, preserved_thinking, out_json);
              }
              return bridge_to_bytes(out_json);
          },
          "One-pass OpenAI ChatCompletionMessageParam JSON for the whole "
          "history (see module docstring for the bridge contract).",
          py::arg("history"), py::arg("structure"), py::arg("preserved_thinking") = false);

    m.def("normalize_tool_call_ids",
          [](py::bytes history, py::dict structure) -> py::list {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              kimix::vector<kimix::runtime::soul::id_fix> fixes;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::normalize_tool_call_ids(msgs, fixes);
              }
              py::list out;
              for (const auto& f : fixes) {
                  const int64_t call_index =
                      (f.call_index == UINT32_MAX) ? -1 : static_cast<int64_t>(f.call_index);
                  out.append(py::make_tuple(f.msg_index, call_index,
                                            py::bytes(f.new_id.data(), f.new_id.size())));
              }
              return out;
          },
          "Plan of tool-call id rewrites (msg_index, call_index, new_id); "
          "call_index -1 means the message tool_call_id field.",
          py::arg("history"), py::arg("structure"));

    m.def("prune_scan",
          [](py::bytes history, py::dict structure, py::object policy_obj) -> py::list {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              kimix::runtime::soul::prune_policy policy;
              if (!policy_obj.is_none()) {
                  py::dict d = py::reinterpret_borrow<py::dict>(policy_obj);
                  if (d.contains("stable_prefix_messages")) {
                      policy.stable_prefix_messages =
                          d["stable_prefix_messages"].cast<uint32_t>();
                  }
                  if (d.contains("recent_messages_protected")) {
                      policy.recent_messages_protected =
                          d["recent_messages_protected"].cast<uint32_t>();
                  }
                  if (d.contains("tool_output_min_tokens")) {
                      policy.tool_output_min_tokens =
                          d["tool_output_min_tokens"].cast<uint32_t>();
                  }
                  if (d.contains("current_turn_index")) {
                      py::object v = d["current_turn_index"];
                      if (v.is_none()) {
                          policy.current_turn_index = UINT32_MAX;
                      } else {
                          policy.current_turn_index = v.cast<uint32_t>();
                      }
                  }
                  if (d.contains("drop_notifications")) {
                      policy.drop_notifications = d["drop_notifications"].cast<bool>();
                  }
                  if (d.contains("drop_task_snapshots")) {
                      policy.drop_task_snapshots = d["drop_task_snapshots"].cast<bool>();
                  }
                  if (d.contains("drop_dmail")) {
                      policy.drop_dmail = d["drop_dmail"].cast<bool>();
                  }
                  if (d.contains("drop_checkpoints")) {
                      policy.drop_checkpoints = d["drop_checkpoints"].cast<bool>();
                  }
              }
              kimix::vector<kimix::runtime::soul::prune_action> actions;
              kimix::runtime::soul::PruneScanner scanner;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  scanner.scan(msgs, policy, actions);
              }
              py::list out;
              for (const auto& a : actions) {
                  out.append(py::make_tuple(a.index, a.reason));
              }
              return out;
          },
          "Prune candidate scan: one (index, reason) per message, index-"
          "ascending; reasons 0=superseded_read 1=resolved_error 2=compact "
          "(Tier A drop) 3=protect 4=oversized_output.",
          py::arg("history"), py::arg("structure"), py::arg("policy") = py::none());

    m.def("count_leading_reminders",
          [](py::bytes history, py::dict structure) -> uint32_t {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              uint32_t count = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  count = kimix::runtime::soul::count_leading_reminders(msgs);
              }
              return count;
          },
          "Number of consecutive leading system-reminder user messages.",
          py::arg("history"), py::arg("structure"));

    m.def("build_normalize_plan",
          [](py::bytes history, py::dict structure) -> py::list {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              kimix::vector<kimix::runtime::soul::normalize_step> steps;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::build_normalize_plan(msgs, steps);
              }
              py::list out;
              for (const auto& s : steps) {
                  out.append(py::make_tuple(s.index, s.op, s.target_index));
              }
              return out;
          },
          "normalize_history merge plan: (index, op, target_index); op 0=keep "
          "1=merge_into_target.",
          py::arg("history"), py::arg("structure"));

    m.def("build_compaction_prompt",
          [](py::bytes history, py::dict structure, py::bytes system_prompt) -> py::bytes {
              bridge b;
              if (!parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<message_view> msgs = assemble(b);
              kimix::string_view sp;
              if (!bridge_bytes_view(system_prompt, sp)) {
                  throw py::error_already_set();
              }
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::soul::build_compaction_prompt(msgs, sp, out);
              }
              return bridge_to_bytes(out);
          },
          "Compaction prompt text (SimpleCompaction.prepare defaults: "
          "balanced mode; system_prompt prepended when non-empty).",
          py::arg("history"), py::arg("structure"), py::arg("system_prompt") = py::bytes());
}
