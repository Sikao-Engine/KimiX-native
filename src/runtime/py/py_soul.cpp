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

namespace {

using namespace kimix::runtime::soul;

inline uint8_t role_from_py(py::object obj) {
    if (obj.is_none()) {
        return kRoleUser;
    }
    const std::string s = obj.cast<std::string>();
    if (s == "system") {
        return kRoleSystem;
    }
    if (s == "assistant") {
        return kRoleAssistant;
    }
    if (s == "tool") {
        return kRoleTool;
    }
    return kRoleUser;
}

inline part_kind part_kind_from_type(const std::string& t) {
    if (t == "text") {
        return part_kind::TEXT;
    }
    if (t == "think") {
        return part_kind::THINK;
    }
    if (t == "tool_call") {
        return part_kind::TOOL_CALL;
    }
    if (t == "image_url") {
        return part_kind::IMAGE;
    }
    if (t == "audio_url") {
        return part_kind::AUDIO;
    }
    if (t == "video_url" || t == "file") {
        return part_kind::FILE;
    }
    return part_kind::OTHER;
}

inline std::string extract_text(py::dict msg) {
    std::string out;
    py::object content_obj = msg.attr("get")("content", py::none());
    if (py::isinstance<py::str>(content_obj)) {
        out = content_obj.cast<std::string>();
    } else if (!content_obj.is_none()) {
        py::list content = content_obj.cast<py::list>();
        for (auto part_item : content) {
            py::dict part = part_item.cast<py::dict>();
            const std::string type_str =
                part.attr("get")("type", py::str("")).cast<std::string>();
            if (type_str == "text") {
                py::object text_obj = part.attr("get")("text", py::str(""));
                out += text_obj.cast<std::string>();
            }
        }
    }
    return out;
}

inline bool py_bool_or(py::dict d, const char* key, bool default_value) {
    if (d.contains(key)) {
        return d[key].cast<bool>();
    }
    return default_value;
}

inline uint32_t py_uint_or(py::dict d, const char* key, uint32_t default_value) {
    if (d.contains(key)) {
        return d[key].cast<uint32_t>();
    }
    return default_value;
}

prune_policy parse_prune_policy(py::dict d) {
    prune_policy policy;
    policy.stable_prefix_messages = py_uint_or(d, "stable_prefix_messages", 4);
    policy.recent_messages_protected = py_uint_or(d, "recent_messages_protected", 6);
    policy.tool_output_min_tokens = py_uint_or(d, "tool_output_min_tokens", 512);
    if (d.contains("current_turn_index")) {
        py::object v = d["current_turn_index"];
        if (v.is_none()) {
            policy.current_turn_index = UINT32_MAX;
        } else {
            policy.current_turn_index = v.cast<uint32_t>();
        }
    }
    policy.drop_notifications = py_bool_or(d, "drop_notifications", true);
    policy.drop_task_snapshots = py_bool_or(d, "drop_task_snapshots", true);
    policy.drop_dmail = py_bool_or(d, "drop_dmail", true);
    policy.drop_checkpoints = py_bool_or(d, "drop_checkpoints", false);
    policy.max_elision_tokens = py_uint_or(d, "max_elision_tokens", 0);
    policy.superseded_read_enabled = py_bool_or(d, "superseded_read_enabled", true);
    policy.oversized_output_enabled = py_bool_or(d, "oversized_output_enabled", true);
    policy.stale_tool_result_enabled = py_bool_or(d, "stale_tool_result_enabled", true);
    return policy;
}

// Owned message buffer for py::list-of-dicts input.  message_view spans point
// into buffer/parts/tool_calls, so all three must outlive the kernel call.
struct owned_message_buffer {
    kimix::string buffer;
    kimix::vector<part_view> parts;
    kimix::vector<tool_call_view> tool_calls;
    kimix::vector<message_view> msgs;
};

owned_message_buffer convert_py_messages(py::list messages) {
    // Two-pass conversion: first extract all strings into temporary owned
    // storage, then copy into one stable buffer and create views.  This keeps
    // every message_view span valid for the lifetime of owned_message_buffer.
    struct owned_part {
        part_kind kind;
        std::string text;
    };
    struct owned_tool_call {
        std::string id;
        std::string name;
        std::string arguments; // empty => None
    };
    struct owned_msg_temp {
        uint8_t role;
        std::string tool_call_id;
        std::vector<owned_part> parts;
        std::vector<owned_tool_call> calls;
    };

    std::vector<owned_msg_temp> temps;
    temps.reserve(messages.size());

    for (auto item : messages) {
        py::dict msg = item.cast<py::dict>();
        owned_msg_temp tm;
        tm.role = role_from_py(msg.attr("get")("role", py::str("user")));

        py::object tcid_obj = msg.attr("get")("tool_call_id", py::none());
        if (!tcid_obj.is_none()) {
            tm.tool_call_id = tcid_obj.cast<std::string>();
        }

        py::object content_obj = msg.attr("get")("content", py::none());
        if (py::isinstance<py::str>(content_obj)) {
            owned_part p;
            p.kind = part_kind::TEXT;
            p.text = content_obj.cast<std::string>();
            tm.parts.push_back(std::move(p));
        } else if (!content_obj.is_none()) {
            py::list content = content_obj.cast<py::list>();
            for (auto part_item : content) {
                py::dict part = part_item.cast<py::dict>();
                owned_part p;
                p.kind = part_kind_from_type(
                    part.attr("get")("type", py::str("")).cast<std::string>());
                if (p.kind == part_kind::TEXT) {
                    p.text = part.attr("get")("text", py::str("")).cast<std::string>();
                } else if (p.kind == part_kind::THINK) {
                    p.text = part.attr("get")("think", py::str("")).cast<std::string>();
                }
                tm.parts.push_back(std::move(p));
            }
        }

        py::object tcs_obj = msg.attr("get")("tool_calls", py::none());
        if (!tcs_obj.is_none()) {
            py::list tcs = tcs_obj.cast<py::list>();
            for (auto tc_item : tcs) {
                py::dict tc = tc_item.cast<py::dict>();
                owned_tool_call tc_owned;
                tc_owned.id = tc.attr("get")("id", py::str("")).cast<std::string>();
                py::dict fn = tc.attr("get")("function", py::dict()).cast<py::dict>();
                tc_owned.name = fn.attr("get")("name", py::str("")).cast<std::string>();
                py::object args_obj = fn.attr("get")("arguments", py::none());
                if (!args_obj.is_none()) {
                    tc_owned.arguments = args_obj.cast<std::string>();
                }
                tm.calls.push_back(std::move(tc_owned));
            }
        }

        temps.push_back(std::move(tm));
    }

    // Pass 2: copy all strings into one buffer and build views.
    owned_message_buffer st;
    size_t total_size = 0;
    size_t total_parts = 0;
    size_t total_calls = 0;
    for (const auto& tm : temps) {
        total_size += tm.tool_call_id.size();
        total_parts += tm.parts.size();
        for (const auto& p : tm.parts) {
            total_size += p.text.size();
        }
        total_calls += tm.calls.size();
        for (const auto& tc : tm.calls) {
            total_size += tc.id.size() + tc.name.size() + tc.arguments.size();
        }
    }
    st.buffer.reserve(total_size);
    st.parts.reserve(total_parts);
    st.tool_calls.reserve(total_calls);
    st.msgs.reserve(temps.size());

    for (const auto& tm : temps) {
        message_view mv;
        mv.role = tm.role;

        const size_t part_begin = st.parts.size();
        for (const auto& p : tm.parts) {
            part_view pv;
            pv.kind = p.kind;
            const size_t start = st.buffer.size();
            st.buffer.append(p.text.data(), p.text.size());
            pv.text = kimix::string_view(st.buffer.data() + start, p.text.size());
            st.parts.push_back(pv);
        }
        mv.parts = kimix::span<const part_view>(
            st.parts.data() + part_begin, st.parts.size() - part_begin);

        const size_t call_begin = st.tool_calls.size();
        for (const auto& tc : tm.calls) {
            tool_call_view tv;
            const size_t id_start = st.buffer.size();
            st.buffer.append(tc.id.data(), tc.id.size());
            tv.id = kimix::string_view(st.buffer.data() + id_start, tc.id.size());
            const size_t name_start = st.buffer.size();
            st.buffer.append(tc.name.data(), tc.name.size());
            tv.name = kimix::string_view(st.buffer.data() + name_start, tc.name.size());
            if (!tc.arguments.empty()) {
                const size_t args_start = st.buffer.size();
                st.buffer.append(tc.arguments.data(), tc.arguments.size());
                tv.arguments =
                    kimix::string_view(st.buffer.data() + args_start, tc.arguments.size());
            }
            st.tool_calls.push_back(tv);
        }
        mv.tool_calls = kimix::span<const tool_call_view>(
            st.tool_calls.data() + call_begin, st.tool_calls.size() - call_begin);

        if (!tm.tool_call_id.empty()) {
            const size_t tcid_start = st.buffer.size();
            st.buffer.append(tm.tool_call_id.data(), tm.tool_call_id.size());
            mv.tool_call_id =
                kimix::string_view(st.buffer.data() + tcid_start, tm.tool_call_id.size());
        }

        st.msgs.push_back(mv);
    }

    return st;
}

} // namespace

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

    m.def("prune_history",
          [](py::list messages, py::dict policy_dict) -> py::dict {
              owned_message_buffer st = convert_py_messages(messages);
              prune_policy policy = parse_prune_policy(policy_dict);
              prune_history_result result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  PruneScanner scanner;
                  scanner.prune_history(st.msgs, policy, result);
              }

              py::dict out;
              py::list out_messages;
              py::list out_elided;
              uint32_t ref_counter = 0;

              // result.actions is already sorted by index ascending; walk it
              // in parallel with the original messages.
              size_t action_idx = 0;
              for (size_t i = 0; i < static_cast<size_t>(messages.size()); ++i) {
                  if (action_idx >= result.actions.size() ||
                      result.actions[action_idx].index != i) {
                      out_messages.append(messages[static_cast<size_t>(i)]);
                      continue;
                  }

                  const prune_history_action& action = result.actions[action_idx];
                  ++action_idx;

                  // Tier A: drop the message entirely.
                  if (action.reason == kPruneCompact) {
                      continue;
                  }

                  // Tier B: elide with a stub.
                  std::string kind;
                  switch (action.reason) {
                  case kPruneSupersededRead:
                      kind = "superseded_read";
                      break;
                  case kPruneOversizedOutput:
                      kind = "oversized_output";
                      break;
                  case kPruneResolvedError:
                      kind = "resolved_error";
                      break;
                  default:
                      kind = "elided";
                      break;
                  }

                  const std::string ref = "prune_" + std::to_string(ref_counter++);
                  const std::string em_dash = "\xE2\x80\x94";
                  const std::string stub_text =
                      "<system>[context-elided: " + kind + " " + em_dash +
                      " content elided. ~" + std::to_string(action.savings) +
                      " tokens freed. Retrieve full content with Memory action='retrieve' id=" +
                      ref + "]</system>";

                  py::dict orig = messages[static_cast<size_t>(i)].cast<py::dict>();
                  const std::string original_text = extract_text(orig);
                  py::dict new_msg = orig.attr("copy")().cast<py::dict>();
                  py::dict text_part;
                  text_part["type"] = "text";
                  text_part["text"] = stub_text;
                  new_msg["content"] = py::list(py::make_tuple(text_part));
                  out_messages.append(new_msg);

                  py::dict elided_rec;
                  elided_rec["index"] = i;
                  elided_rec["role"] = orig.attr("get")("role", py::str(""));
                  elided_rec["kind"] = kind;
                  elided_rec["summary"] = kind + " at index " + std::to_string(i);
                  elided_rec["original_text"] = original_text;
                  elided_rec["ref"] = ref;
                  out_elided.append(elided_rec);
              }

              out["messages"] = out_messages;
              out["elided"] = out_elided;
              out["freed_tokens"] = result.freed_tokens;
              if (result.earliest_removed_index == UINT32_MAX) {
                  out["earliest_removed_index"] = py::none();
              } else {
                  out["earliest_removed_index"] = result.earliest_removed_index;
              }
              return out;
          },
          "Full prune_history pass: Tier A drops + Tier B elision stubs. "
          "Returns {messages, elided, freed_tokens, earliest_removed_index}.",
          py::arg("messages"), py::arg("policy"));

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
