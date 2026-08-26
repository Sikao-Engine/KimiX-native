/*
 * py_tools.cpp - Python bindings for the tool kernels (runtime_py.tools).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built only
 * after the release scope closes.
 *
 * API:
 *   tools.line_hash(line: bytes, seed: int) -> int
 *       compute_line_hash(line, seed) & 0xFF (exact xxh32 recipe).
 *   tools.line_hashes(content: bytes, seed: int) -> list[int]
 *       chained per-line hashes (reference seed semantics).
 *   tools.find_in_file(content, needle, case_insensitive=True) -> list[(int,int,int)]
 *       (line_index 0-based, col 0-based, length) per line, overlapping,
 *       readlines() terminator semantics. ASCII-only folding; the shim routes
 *       non-ASCII to the _compat mirror.
 *   tools.scan_lines(content, pattern, case_insensitive=True) -> list[(int,int,int)]
 *       (line_index 0-based, byte_offset, line_len) for lines containing the
 *       literal pattern (substring matcher, ASCII fold).
 *   tools.scan_lines_cb(content, callback) -> list[(int,int,int)]
 *       same offsets, but the matcher is a Python callable invoked per line
 *       (GIL held; full regex semantics stay in Python, offsets stay native).
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/tools/line_hash.h>
#include <runtime/tools/find_str.h>
#include <runtime/tools/grep_scan.h>
#include <runtime/tools/compress.h>
#include <runtime/tools/export_builder.h>
#include <runtime/tools/security.h>
#include <runtime/tools/shell_safety.h>
#include <runtime/tools/grep_pattern.h>
#include <runtime/py/py_soul_bridge.h>

namespace py = pybind11;

namespace {

// Convert a Python str to UTF-8 bytes held in a kimix::string (surrogate-ok
// via PyUnicode_AsUTF8AndSize -- strings are caller-provided).
bool str_to_string(py::handle obj, kimix::string& out) {
    if (!PyUnicode_Check(obj.ptr())) {
        return false;
    }
    Py_ssize_t len = 0;
    const char* cstr = PyUnicode_AsUTF8AndSize(obj.ptr(), &len);
    if (cstr == nullptr) {
        return false;
    }
    out.assign(cstr, static_cast<size_t>(len));
    return true;
}

bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

inline uint8_t fold_ascii(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

py::list matches_to_list(const kimix::vector<kimix::runtime::tools::find_match>& ms) {
    py::list out;
    for (const auto& m : ms) {
        out.append(py::make_tuple(m.line_index, m.col, m.length));
    }
    return out;
}

py::list hits_to_list(const kimix::vector<kimix::runtime::tools::grep_hit>& hs) {
    py::list out;
    for (const auto& h : hs) {
        out.append(py::make_tuple(h.line_index, h.byte_offset, h.line_len));
    }
    return out;
}

// UTF-8 bytes -> Python str (the kernels return UTF-8 kimix::string).
py::str to_py_str(const kimix::string& s) {
    return py::str(s.data(), s.size());
}

// optional<kimix::string> -> str or None.
py::object opt_str_to_obj(const kimix::optional<kimix::string>& o) {
    if (o.has_value()) {
        return to_py_str(*o);
    }
    return py::none();
}

// hardline_result -> (bool, str|None).
py::tuple hardline_to_tuple(const kimix::runtime::tools::hardline_result& r) {
    if (r.blocked && r.description.has_value()) {
        return py::make_tuple(true, to_py_str(*r.description));
    }
    return py::make_tuple(false, py::none());
}

} // namespace

void py_register_tools(py::module_& m) {
    m.doc() = "Tool kernels (line hashing, string find, grep line scan)";

    m.def("line_hash",
          [](py::bytes line, uint32_t seed) -> uint32_t {
              kimix::string_view view;
              if (!bytes_view(line, view)) {
                  throw py::error_already_set();
              }
              uint32_t h = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  h = kimix::runtime::tools::compute_line_hash(view, seed);
              }
              return h;
          },
          "Exact hash_line recipe for one line with a final 32-bit seed: "
          "strip trailing CR, filter whitespace, xxh32 & 0xFF.",
          py::arg("line"), py::arg("seed"));

    m.def("line_hashes",
          [](py::bytes content, uint32_t seed) -> py::list {
              kimix::string_view view;
              if (!bytes_view(content, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<uint32_t> hashes;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::compute_line_hashes(view, seed, hashes);
              }
              py::list out;
              for (uint32_t h : hashes) {
                  out.append(h);
              }
              return out;
          },
          "Chained per-line hashes (reference seed semantics).",
          py::arg("content"), py::arg("seed"));

    m.def("find_in_file",
          [](py::bytes content, py::bytes needle, bool case_insensitive) -> py::list {
              kimix::string_view cview, nview;
              if (!bytes_view(content, cview) || !bytes_view(needle, nview)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::tools::find_match> ms;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::find_in_file(cview, nview, case_insensitive, ms);
              }
              return matches_to_list(ms);
          },
          "All per-line occurrences of the needle (overlapping; lines keep "
          "their terminators). ASCII-only case folding.",
          py::arg("content"), py::arg("needle"), py::arg("case_insensitive") = true);

    m.def("scan_lines",
          [](py::bytes content, py::bytes pattern, bool case_insensitive) -> py::list {
              kimix::string_view cview, pview;
              if (!bytes_view(content, cview) || !bytes_view(pattern, pview)) {
                  throw py::error_already_set();
              }
              kimix::vector<uint8_t> pf;
              pf.reserve(pview.size());
              for (unsigned char c : pview) {
                  pf.push_back(case_insensitive ? fold_ascii(c) : c);
              }
              kimix::vector<kimix::runtime::tools::grep_hit> hits;
              const bool empty_pattern = pf.empty();
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::scan_lines(
                      cview,
                      [&](kimix::string_view line, uint32_t) -> bool {
                          if (empty_pattern) {
                              return false; // backup_grep rejects empty patterns
                          }
                          // substring test with on-the-fly ASCII folding
                          const size_t n = line.size();
                          const size_t m = pf.size();
                          if (m > n) {
                              return false;
                          }
                          for (size_t i = 0; i + m <= n; ++i) {
                              size_t k = 0;
                              while (k < m) {
                                  uint8_t lc = static_cast<uint8_t>(line[i + k]);
                                  if (case_insensitive) {
                                      lc = fold_ascii(lc);
                                  }
                                  if (lc != pf[k]) {
                                      break;
                                  }
                                  ++k;
                              }
                              if (k == m) {
                                  return true;
                              }
                          }
                          return false;
                      },
                      hits);
              }
              return hits_to_list(hits);
          },
          "Lines containing the literal pattern (substring matcher, ASCII "
          "fold). Returns (line_index 0-based, byte_offset, line_len).",
          py::arg("content"), py::arg("pattern"), py::arg("case_insensitive") = true);

    m.def("scan_lines_cb",
          [](py::bytes content, py::object callback) -> py::list {
              kimix::string_view cview;
              if (!bytes_view(content, cview)) {
                  throw py::error_already_set();
              }
              if (!PyCallable_Check(callback.ptr())) {
                  throw py::type_error("callback must be callable");
              }
              kimix::vector<kimix::runtime::tools::grep_hit> hits;
              // GIL is held for the whole call: the callback needs it, and the
              // kernel itself never touches Python.
              kimix::runtime::tools::scan_lines(
                  cview,
                  [&](kimix::string_view line, uint32_t line_index) -> bool {
                      py::object result =
                          callback(py::bytes(line.data(), line.size()), line_index);
                      return result.cast<bool>();
                  },
                  hits);
              return hits_to_list(hits);
          },
          "Per-line matcher callback (called with (line_bytes, line_index)); "
          "offsets are computed natively.",
          py::arg("content"), py::arg("callback"));

    // ------------------------------------------------------------------
    // build_export_markdown (plan 016) -- full session export markdown.
    // ------------------------------------------------------------------
    m.def("build_export_markdown",
          [](py::bytes history, py::dict structure, py::dict opts) -> py::bytes {
              kimix_soul_bridge::bridge b;
              if (!kimix_soul_bridge::parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<kimix::runtime::soul::message_view> msgs =
                  kimix_soul_bridge::assemble(b);

              kimix::string session_id, work_dir, exported_at;
              uint64_t token_count = 0;
              bool include_timestamps = false;
              if (opts.contains("session_id") &&
                  !str_to_string(opts["session_id"], session_id)) {
                  throw py::type_error("session_id must be str");
              }
              if (opts.contains("work_dir") &&
                  !str_to_string(opts["work_dir"], work_dir)) {
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
              return kimix_soul_bridge::bridge_to_bytes(out);
          },
          "Full session export markdown (reference export.py build_export_"
          "markdown); opts keys: session_id, work_dir, exported_at, "
          "token_count, include_timestamps.",
          py::arg("history"), py::arg("structure"), py::arg("opts"));

    // ------------------------------------------------------------------
    // Security kernels (plan: commit 0582e09 "Study from hermes").
    // ASCII-only contracts; the shim routes non-ASCII input to the
    // pure-Python mirrors.
    // ------------------------------------------------------------------
    m.def("redact_sensitive_output",
          [](py::str output) -> py::str {
              kimix::string out;
              if (!str_to_string(output, out)) {
                  throw py::type_error("output must be str");
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::redact_sensitive_output(out);
              }
              return to_py_str(result);
          },
          "10 chained redactions (URL userinfo, JWT, PEM, tokens, auth "
          "headers, assignments, bearer). ASCII input only.",
          py::arg("output"));

    m.def("scrub_child_env",
          [](py::dict env) -> py::dict {
              kimix::vector<kimix::runtime::tools::env_entry> in, out;
              in.reserve(env.size());
              for (auto item : env) {
                  if (!py::isinstance<py::str>(item.first)) {
                      throw py::type_error("scrub_child_env keys must be str");
                  }
                  kimix::string name;
                  if (!str_to_string(item.first, name)) {
                      throw py::error_already_set();
                  }
                  in.push_back({std::move(name), kimix::string()});
              }
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::scrub_child_env(in, out);
              }
              py::dict result;
              for (const auto& e : out) {
                  py::str name(e.name.data(), e.name.size());
                  result[name] = env[name]; // original value object, untouched
              }
              return result;
          },
          "Copy *env* keeping safe-prefixed names and dropping names "
          "containing secret substrings; insertion order preserved, values "
          "never inspected. Keys must be ASCII str.",
          py::arg("env"));

    m.def("validate_workdir",
          [](py::object workdir) -> py::object {
              if (workdir.is_none()) {
                  return py::none();
              }
              kimix::string wd;
              if (!str_to_string(workdir, wd)) {
                  throw py::type_error("workdir must be str or None");
              }
              kimix::optional<kimix::string> err;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  err = kimix::runtime::tools::validate_workdir(wd);
              }
              return opt_str_to_obj(err);
          },
          "None when the workdir is safe, else the reference error message "
          "(Python repr of the first offending character). Any UTF-8 input.",
          py::arg("workdir"));

    m.def("bounded_append",
          [](py::str content, py::str text, int64_t cap) -> py::tuple {
              kimix::string c, t;
              if (!str_to_string(content, c) || !str_to_string(text, t)) {
                  throw py::type_error("content/text must be str");
              }
              kimix::runtime::tools::bounded_result r;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  r = kimix::runtime::tools::bounded_append(c, t, cap);
              }
              return py::make_tuple(to_py_str(r.content), r.truncated);
          },
          "bounded_append(content, text, cap) -> (new_content, truncated); "
          "head 40% / tail 60% split with the reference marker line.",
          py::arg("content"), py::arg("text"), py::arg("cap"));

    m.def("command_detection_variants",
          [](py::str command) -> py::list {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::vector<kimix::string> out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::command_detection_variants(cmd, out);
              }
              py::list result;
              for (const auto& v : out) {
                  result.append(to_py_str(v));
              }
              return result;
          },
          "Deduped deobfuscation variants (at most 3). ASCII input only.",
          py::arg("command"));

    m.def("detect_hardline_command",
          [](py::str command) -> py::tuple {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::runtime::tools::hardline_result r;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  r = kimix::runtime::tools::detect_hardline_command(cmd);
              }
              return hardline_to_tuple(r);
          },
          "(True, description) when the command matches a hardline pattern "
          "(7 ordered checks). ASCII input only.",
          py::arg("command"));

    m.def("check_hardline_blocked",
          [](py::str command) -> py::tuple {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::runtime::tools::hardline_result r;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  r = kimix::runtime::tools::check_hardline_blocked(cmd);
              }
              return hardline_to_tuple(r);
          },
          "Run detect_hardline_command over every deobfuscation variant.",
          py::arg("command"));

    m.def("foreground_background_guidance",
          [](py::str command) -> py::object {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::optional<kimix::string> hint;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  hint = kimix::runtime::tools::foreground_background_guidance(cmd);
              }
              return opt_str_to_obj(hint);
          },
          "Long-running-process hint or None. ASCII input only.",
          py::arg("command"));

    m.def("base_command_name",
          [](py::str command) -> py::str {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::string name;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  name = kimix::runtime::tools::base_command_name(cmd);
              }
              return to_py_str(name);
          },
          "First non-assignment command word, directory-stripped, .exe removed.",
          py::arg("command"));

    m.def("interpret_exit_code",
          [](py::str command, py::object exit_code) -> py::object {
              kimix::string cmd;
              if (!str_to_string(command, cmd)) {
                  throw py::type_error("command must be str");
              }
              kimix::optional<int64_t> code;
              if (!exit_code.is_none()) {
                  if (!py::isinstance<py::int_>(exit_code)) {
                      throw py::type_error("exit_code must be int or None");
                  }
                  code = exit_code.cast<int64_t>();
              }
              kimix::optional<kimix::string> msg;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  msg = kimix::runtime::tools::interpret_exit_code(cmd, code);
              }
              return opt_str_to_obj(msg);
          },
          "Explanation for well-known non-zero exit codes, else None.",
          py::arg("command"), py::arg("exit_code"));

    m.def("annotate_failure",
          [](py::str output, py::str command, py::object exit_code) -> py::object {
              kimix::string out, cmd;
              if (!str_to_string(output, out) || !str_to_string(command, cmd)) {
                  throw py::type_error("output/command must be str");
              }
              kimix::optional<int64_t> code;
              if (!exit_code.is_none()) {
                  if (!py::isinstance<py::int_>(exit_code)) {
                      throw py::type_error("exit_code must be int or None");
                  }
                  code = exit_code.cast<int64_t>();
              }
              kimix::optional<kimix::string> hint;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  hint = kimix::runtime::tools::annotate_failure(out, cmd, code);
              }
              return opt_str_to_obj(hint);
          },
          "Single actionable hint for common failure signatures, else None. "
          "ASCII output only.",
          py::arg("output"), py::arg("command"), py::arg("exit_code"));

    m.def("pattern_has_regex_newline",
          [](py::str pattern) -> bool {
              kimix::string pat;
              if (!str_to_string(pattern, pat)) {
                  throw py::type_error("pattern must be str");
              }
              bool has = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  has = kimix::runtime::tools::pattern_has_regex_newline(pat);
              }
              return has;
          },
          "True when the pattern contains a literal newline or an odd-backslash "
          "regex \\n escape. ASCII input only.",
          py::arg("pattern"));

    m.def("multiline_pattern",
          [](py::str pattern) -> py::str {
              kimix::string pat;
              if (!str_to_string(pattern, pat)) {
                  throw py::type_error("pattern must be str");
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::multiline_pattern(pat);
              }
              return to_py_str(result);
          },
          "Rewrite newline constructs so the pattern also matches CRLF. "
          "ASCII input only.",
          py::arg("pattern"));

    // ------------------------------------------------------------------
    // Micro-compression kernels (plan 016).
    // ------------------------------------------------------------------
    m.def("compress_intra_line_dedup",
          [](py::str text, int threshold, int max_unit) -> py::str {
              // Borrow the str's UTF-8 bytes directly instead of copying.
              Py_ssize_t len = 0;
              const char* cstr = PyUnicode_AsUTF8AndSize(text.ptr(), &len);
              if (cstr == nullptr) {
                  throw py::error_already_set();
              }
              kimix::string_view view(cstr, static_cast<size_t>(len));
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::compress_intra_line_dedup(view, threshold, max_unit);
              }
              return to_py_str(result);
          },
          "Intra-line repeating-unit dedup (UTF-8 bytes).",
          py::arg("text"), py::arg("threshold") = 2000, py::arg("max_unit") = 2048);

    m.def("compress_collapse_whitespace",
          [](py::str text, py::str kind, bool lossless_only, bool strip_trailing_ws,
             int blank_line_collapse, bool common_indent_factor, bool prefix_fold) -> py::str {
              kimix::string in, kind_str;
              if (!str_to_string(text, in) || !str_to_string(kind, kind_str)) {
                  throw py::type_error("text and kind must be str");
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::compress_collapse_whitespace(
                      in, kind_str, lossless_only, strip_trailing_ws,
                      blank_line_collapse, common_indent_factor, prefix_fold);
              }
              return to_py_str(result);
          },
          "Collapse whitespace (UTF-8 bytes).",
          py::arg("text"), py::arg("kind") = "log", py::arg("lossless_only") = false,
          py::arg("strip_trailing_ws") = true, py::arg("blank_line_collapse") = 1,
          py::arg("common_indent_factor") = true, py::arg("prefix_fold") = true);

    m.def("compress_renumber_lines",
          [](py::str text) -> py::str {
              kimix::string in;
              if (!str_to_string(text, in)) {
                  throw py::type_error("text must be str");
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::compress_renumber_lines(in);
              }
              return to_py_str(result);
          },
          "Compact fixed-width leading line numbers (UTF-8 bytes).",
          py::arg("text"));

    m.def("compress_strip_control_noise",
          [](py::str text) -> py::str {
              kimix::string in;
              if (!str_to_string(text, in)) {
                  throw py::type_error("text must be str");
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::tools::compress_strip_control_noise(in);
              }
              return to_py_str(result);
          },
          "Strip ANSI/OSC/DCS escapes and collapse CR progress-bar chains (UTF-8 bytes).",
          py::arg("text"));
}
