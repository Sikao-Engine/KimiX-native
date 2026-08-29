/*
 * py_builtin_web.cpp - Python bindings for the builtin web / context / retrieval
 * tool kernels (runtime_py.web).
 *
 * BINDING-LAYER ONLY: links against kimix-llm (pure C++ kernels) and pybind11.
 * Every kernel call releases the GIL via kimix::runtime::common::gil_scoped_release;
 * Python objects are built only after the release scope closes.
 *
 * API:
 *   web.normalize_url_for_request(url: str) -> str
 *   web.sensitive_query_param_name(url: str) -> str | None
 *   web.url_contains_secret(url: str) -> bool
 *   web.is_blocked_hostname(hostname: str) -> bool
 *   web.classify_resolved_address(ip: str) -> int
 *   web.is_always_blocked_address(ip: str) -> bool
 *   web.is_safe_url_decision(url: str, allow_all_private=False,
 *                            proxy_configured=False,
 *                            resolved=None) -> bool
 *   web.idna_encode_host(host: str) -> str | None
 *   web.pick_encoding(content_type: str, meta_candidates: list[str]) -> str
 *   web.len_without_ws(text: str) -> int
 *   web.has_login_wall(text: str) -> bool
 *   web.html_to_markdown(html: str, extract=True) -> str
 *
 *   web.convert_base64_images_to_links(text: str) -> str
 *   web.make_cache_slug(url: str) -> str
 *   web.make_cache_file_name(url: str) -> str
 *   web.truncate_with_footer(content: str, url: str, char_limit: int,
 *                            include_content=True, cache_dir=None) -> dict
 *   web.clamp_search_limit(limit: int) -> int
 *   web.clamp_extract_char_limit(char_limit: int) -> int
 *   web.build_search_output(items: list[dict], opts=None) -> dict
 *
 *   web.format_retrieve_result(turns: list[dict], ref_id="") -> str
 *   web.parse_turn_reference(ref: str) -> int | None
 *   web.apply_recency_boost(turns: list[dict], recency_weight: float,
 *                           now: float) -> list[dict]
 *   web.sort_and_truncate(turns: list[dict], top_k: int) -> list[dict]
 *
 *   web.extract_text(message: dict, sep="") -> str
 *   web.has_think_part(message: dict) -> bool
 *   web.is_user_or_assistant(message: dict) -> bool
 *   web.should_auto_compact(token_count: int, **kwargs) -> bool
 *   web.build_compaction_prompt(to_compact: list[dict], options=None,
 *                               custom_instruction="", prompt_compact="",
 *                               prompt_compact_cascade="") -> dict
 *   web.build_compact_message_text(to_compact: list[dict], prompt_text: str) -> str
 *   web.prepare_compaction_input(messages: list[dict], preserve_start_index: int,
 *                                options=None, custom_instruction="",
 *                                prompt_compact="",
 *                                prompt_compact_cascade="") -> dict
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <builtin_tools/tool_types.h>
#include <builtin_tools/fetch_url_tool.h>
#include <builtin_tools/web_search_tool.h>
#include <builtin_tools/retrieve_tool.h>
#include <builtin_tools/compact_tool.h>

namespace py = pybind11;

namespace {

// ---------------------------------------------------------------------------
// String conversion helpers (UTF-8 str <-> kimix::string).
// ---------------------------------------------------------------------------

bool str_to_string(py::handle obj, kimix::string &out) {
    if (!PyUnicode_Check(obj.ptr())) {
        return false;
    }
    Py_ssize_t len = 0;
    const char *cstr = PyUnicode_AsUTF8AndSize(obj.ptr(), &len);
    if (cstr == nullptr) {
        return false;
    }
    out.assign(cstr, static_cast<size_t>(len));
    return true;
}

py::str to_py_str(const kimix::string &s) {
    return py::str(s.data(), s.size());
}

py::object opt_str_to_obj(const kimix::optional<kimix::string> &o) {
    if (o.has_value()) {
        return to_py_str(*o);
    }
    return py::none();
}

py::object opt_int64_to_obj(const kimix::optional<int64_t> &o) {
    if (o.has_value()) {
        return py::int_(*o);
    }
    return py::none();
}

kimix::string require_str(py::handle obj, const char *what) {
    kimix::string s;
    if (!str_to_string(obj, s)) {
        throw py::type_error(what);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Dict helpers.
// ---------------------------------------------------------------------------

template <typename T>
T dict_get(py::dict d, const char *key, T default_value) {
    PyObject *val = PyDict_GetItemString(d.ptr(), key);
    if (val == nullptr || val == Py_None) {
        return default_value;
    }
    return py::handle(val).cast<T>();
}

kimix::optional<kimix::string> dict_opt_str(py::dict d, const char *key) {
    PyObject *val = PyDict_GetItemString(d.ptr(), key);
    if (val == nullptr || val == Py_None) {
        return {};
    }
    kimix::string s;
    if (!str_to_string(py::handle(val), s)) {
        throw py::type_error(std::string(key) + " must be str or None");
    }
    return s;
}

kimix::string dict_str(py::dict d, const char *key, const kimix::string &default_value = {}) {
    PyObject *val = PyDict_GetItemString(d.ptr(), key);
    if (val == nullptr || val == Py_None) {
        return default_value;
    }
    kimix::string s;
    if (!str_to_string(py::handle(val), s)) {
        throw py::type_error(std::string(key) + " must be str");
    }
    return s;
}

// ---------------------------------------------------------------------------
// compact::message <-> Python dict.
// ---------------------------------------------------------------------------

bool parse_content_part(py::handle obj, kimix::builtin_tools::compact::content_part &part) {
    if (!py::isinstance<py::dict>(obj)) {
        return false;
    }
    py::dict d = obj.cast<py::dict>();
    part.type = dict_str(d, "type", "other");
    part.text = dict_str(d, "text", {});
    return true;
}

bool parse_message(py::handle obj, kimix::builtin_tools::compact::message &msg) {
    if (!py::isinstance<py::dict>(obj)) {
        return false;
    }
    py::dict d = obj.cast<py::dict>();
    msg.role = dict_str(d, "role", {});
    msg.content.clear();

    PyObject *content = PyDict_GetItemString(d.ptr(), "content");
    if (content != nullptr && content != Py_None) {
        if (!py::isinstance<py::list>(py::handle(content))) {
            return false;
        }
        py::list parts = py::handle(content).cast<py::list>();
        msg.content.reserve(parts.size());
        for (py::handle h : parts) {
            kimix::builtin_tools::compact::content_part part;
            if (!parse_content_part(h, part)) {
                return false;
            }
            msg.content.push_back(std::move(part));
        }
    }
    return true;
}

py::dict message_to_dict(const kimix::builtin_tools::compact::message &msg) {
    py::dict d;
    d["role"] = to_py_str(msg.role);
    py::list content;
    for (const auto &part : msg.content) {
        py::dict pd;
        pd["type"] = to_py_str(part.type);
        pd["text"] = to_py_str(part.text);
        content.append(pd);
    }
    d["content"] = content;
    return d;
}

py::list messages_to_list(const kimix::vector<kimix::builtin_tools::compact::message> &msgs) {
    py::list out;
    for (const auto &msg : msgs) {
        out.append(message_to_dict(msg));
    }
    return out;
}

bool parse_messages(py::handle obj, kimix::vector<kimix::builtin_tools::compact::message> &out) {
    if (!py::isinstance<py::list>(obj)) {
        return false;
    }
    py::list lst = obj.cast<py::list>();
    out.clear();
    out.reserve(lst.size());
    for (py::handle h : lst) {
        kimix::builtin_tools::compact::message msg;
        if (!parse_message(h, msg)) {
            return false;
        }
        out.push_back(std::move(msg));
    }
    return true;
}

kimix::vector<kimix::builtin_tools::compact::message> require_messages(py::handle obj, const char *what) {
    kimix::vector<kimix::builtin_tools::compact::message> msgs;
    if (!parse_messages(obj, msgs)) {
        throw py::type_error(what);
    }
    return msgs;
}

kimix::builtin_tools::compact::compaction_options parse_compaction_options(py::dict d) {
    using namespace kimix::builtin_tools::compact;
    compaction_options opts;
    opts.avoid_cascade = dict_get<bool>(d, "avoid_cascade", false);
    opts.mode = parse_compact_mode(dict_str(d, "mode", "balanced"));
    opts.todos_max_items = dict_get<int32_t>(d, "todos_max_items", 20);
    opts.preserve_depth_override = dict_get<int32_t>(d, "preserve_depth_override", -1);
    opts.decision_section_enabled = dict_get<bool>(d, "decision_section_enabled", false);
    return opts;
}

// ---------------------------------------------------------------------------
// retrieve::history_turn <-> Python dict.
// ---------------------------------------------------------------------------

bool parse_history_turn(py::handle obj, kimix::builtin_tools::retrieve::history_turn &turn) {
    if (!py::isinstance<py::dict>(obj)) {
        return false;
    }
    py::dict d = obj.cast<py::dict>();
    turn.turn_id = dict_get<int64_t>(d, "turn_id", -1);
    turn.role = dict_str(d, "role", {});
    turn.text = dict_str(d, "text", {});
    turn.timestamp = dict_get<double>(d, "timestamp", 0.0);
    turn.score = dict_get<double>(d, "score", 0.0);
    turn.is_compacted = dict_get<bool>(d, "is_compacted", false);
    turn.boosted_score = dict_get<double>(d, "boosted_score", 0.0);
    return true;
}

py::dict history_turn_to_dict(const kimix::builtin_tools::retrieve::history_turn &turn) {
    py::dict d;
    d["turn_id"] = turn.turn_id;
    d["role"] = to_py_str(turn.role);
    d["text"] = to_py_str(turn.text);
    d["timestamp"] = turn.timestamp;
    d["score"] = turn.score;
    d["is_compacted"] = turn.is_compacted;
    d["boosted_score"] = turn.boosted_score;
    return d;
}

kimix::vector<kimix::builtin_tools::retrieve::history_turn> require_history_turns(py::handle obj, const char *what) {
    if (!py::isinstance<py::list>(obj)) {
        throw py::type_error(what);
    }
    py::list lst = obj.cast<py::list>();
    kimix::vector<kimix::builtin_tools::retrieve::history_turn> out;
    out.reserve(lst.size());
    for (py::handle h : lst) {
        kimix::builtin_tools::retrieve::history_turn turn;
        if (!parse_history_turn(h, turn)) {
            throw py::type_error(what);
        }
        out.push_back(std::move(turn));
    }
    return out;
}

py::list history_turns_to_list(const kimix::vector<kimix::builtin_tools::retrieve::history_turn> &turns) {
    py::list out;
    for (const auto &turn : turns) {
        out.append(history_turn_to_dict(turn));
    }
    return out;
}

// ---------------------------------------------------------------------------
// web_search::web_item <-> Python dict.
// ---------------------------------------------------------------------------

bool parse_web_item(py::handle obj, kimix::builtin_tools::web_search::web_item &item) {
    if (!py::isinstance<py::dict>(obj)) {
        return false;
    }
    py::dict d = obj.cast<py::dict>();
    item.site_name = dict_str(d, "site_name", {});
    item.title = dict_str(d, "title", {});
    item.url = dict_str(d, "url", {});
    item.snippet = dict_str(d, "snippet", {});
    item.content = dict_str(d, "content", {});
    item.date = dict_str(d, "date", {});
    item.icon = dict_str(d, "icon", {});
    item.mime = dict_str(d, "mime", {});
    return true;
}

kimix::vector<kimix::builtin_tools::web_search::web_item> require_web_items(py::handle obj, const char *what) {
    if (!py::isinstance<py::list>(obj)) {
        throw py::type_error(what);
    }
    py::list lst = obj.cast<py::list>();
    kimix::vector<kimix::builtin_tools::web_search::web_item> out;
    out.reserve(lst.size());
    for (py::handle h : lst) {
        kimix::builtin_tools::web_search::web_item item;
        if (!parse_web_item(h, item)) {
            throw py::type_error(what);
        }
        out.push_back(std::move(item));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Error translation.
// ---------------------------------------------------------------------------

void throw_tool_error(const kimix::builtin_tools::tool_error &err) {
    throw std::runtime_error(std::string(err.message.data(), err.message.size()));
}

} // namespace

// ===========================================================================
// Public registration entry point.
// ===========================================================================

void py_register_builtin_web(py::module_ &m) {
    auto web = m.def_submodule("web",
                               "Built-in web, context, and retrieval tool kernels.");

    // -----------------------------------------------------------------------
    // fetch_url -- pure URL/text kernels (no network I/O).
    // -----------------------------------------------------------------------
    web.def("normalize_url_for_request",
            [](py::str url) -> py::str {
                kimix::string u = require_str(url, "url must be str");
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::normalize_url_for_request(u);
                }
                return to_py_str(out);
            },
            "ASCII-safe HTTP URL normalization (IDNA, percent-encoding).",
            py::arg("url"));

    web.def("sensitive_query_param_name",
            [](py::str url) -> py::object {
                kimix::string u = require_str(url, "url must be str");
                kimix::optional<kimix::string> out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::sensitive_query_param_name(u);
                }
                return opt_str_to_obj(out);
            },
            "First sensitive query parameter name, or None.",
            py::arg("url"));

    web.def("url_contains_secret",
            [](py::str url) -> bool {
                kimix::string u = require_str(url, "url must be str");
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::url_contains_secret(u);
                }
                return out;
            },
            "True when the URL appears to contain a credential.",
            py::arg("url"));

    web.def("is_blocked_hostname",
            [](py::str hostname) -> bool {
                kimix::string h = require_str(hostname, "hostname must be str");
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::is_blocked_hostname(h);
                }
                return out;
            },
            "True for always-blocked hostnames (cloud metadata endpoints).",
            py::arg("hostname"));

    web.def("classify_resolved_address",
            [](py::str ip) -> int {
                kimix::string s = require_str(ip, "ip must be str");
                kimix::builtin_tools::fetch_url::addr_class cls;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    cls = kimix::builtin_tools::fetch_url::classify_resolved_address(s);
                }
                return static_cast<int>(static_cast<uint8_t>(cls));
            },
            "Classify an IPv4/IPv6 address (returns underlying addr_class value).",
            py::arg("ip"));

    web.def("is_always_blocked_address",
            [](py::str ip) -> bool {
                kimix::string s = require_str(ip, "ip must be str");
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::is_always_blocked_address(s);
                }
                return out;
            },
            "True for addresses that are always blocked.",
            py::arg("ip"));

    web.def("is_safe_url_decision",
            [](py::str url, bool allow_all_private, bool proxy_configured,
               py::object resolved) -> bool {
                kimix::string u = require_str(url, "url must be str");
                kimix::builtin_tools::fetch_url::resolve_outcome outcome;
                if (!resolved.is_none()) {
                    if (!py::isinstance<py::dict>(resolved)) {
                        throw py::type_error("resolved must be dict or None");
                    }
                    py::dict d = resolved.cast<py::dict>();
                    outcome.dns_failed = dict_get<bool>(d, "dns_failed", false);
                    PyObject *addrs = PyDict_GetItemString(d.ptr(), "addresses");
                    if (addrs != nullptr && addrs != Py_None) {
                        if (!py::isinstance<py::list>(py::handle(addrs))) {
                            throw py::type_error("resolved['addresses'] must be list");
                        }
                        py::list lst(py::handle(addrs), true);
                        for (py::handle h : lst) {
                            outcome.addresses.push_back(require_str(h, "address must be str"));
                        }
                    }
                }
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::is_safe_url_decision(
                        u, allow_all_private, proxy_configured, outcome);
                }
                return out;
            },
            "Fail-closed safe-URL decision using injected DNS resolution data.",
            py::arg("url"),
            py::arg("allow_all_private") = false,
            py::arg("proxy_configured") = false,
            py::arg("resolved") = py::none());

    web.def("idna_encode_host",
            [](py::str host) -> py::object {
                kimix::string h = require_str(host, "host must be str");
                kimix::string out;
                bool ok;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    ok = kimix::builtin_tools::fetch_url::idna_encode_host(h, out);
                }
                if (ok) {
                    return to_py_str(out);
                }
                return py::none();
            },
            "IDNA-encode a hostname, or None on failure.",
            py::arg("host"));

    web.def("pick_encoding",
            [](py::str content_type, py::list meta_candidates) -> py::str {
                kimix::string ct = require_str(content_type, "content_type must be str");
                kimix::vector<kimix::string> temp;
                kimix::vector<kimix::string_view> views;
                temp.reserve(meta_candidates.size());
                views.reserve(meta_candidates.size());
                for (py::handle h : meta_candidates) {
                    temp.push_back(require_str(h, "meta candidate must be str"));
                    views.push_back(temp.back());
                }
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::pick_encoding(ct, views);
                }
                return to_py_str(out);
            },
            "Choose an HTML decoding encoding from Content-Type + <meta> candidates.",
            py::arg("content_type"),
            py::arg("meta_candidates") = py::list());

    web.def("len_without_ws",
            [](py::str text) -> int64_t {
                kimix::string t = require_str(text, "text must be str");
                size_t out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::len_without_ws(t);
                }
                return static_cast<int64_t>(out);
            },
            "Number of code points excluding spaces and newlines.",
            py::arg("text"));

    web.def("has_login_wall",
            [](py::str text) -> bool {
                kimix::string t = require_str(text, "text must be str");
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::fetch_url::has_login_wall(t);
                }
                return out;
            },
            "True when the text matches login-wall keyword heuristics.",
            py::arg("text"));

    web.def("html_to_markdown",
            [](py::str html, bool extract) -> py::str {
                kimix::string h = require_str(html, "html must be str");
                kimix::string out;
                kimix::builtin_tools::tool_error err;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    err = kimix::builtin_tools::fetch_url::html_to_markdown(h, out, extract);
                }
                if (err.failed()) {
                    throw_tool_error(err);
                }
                return to_py_str(out);
            },
            "Convert HTML to Markdown (pure CPU; network stays in Python).",
            py::arg("html"),
            py::arg("extract") = true);

    // -----------------------------------------------------------------------
    // web_search -- pure result-shaping kernels.
    // -----------------------------------------------------------------------
    web.def("convert_base64_images_to_links",
            [](py::str text) -> py::str {
                kimix::string t = require_str(text, "text must be str");
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::web_search::convert_base64_images_to_links(t);
                }
                return to_py_str(out);
            },
            "Replace inline base64 data URLs with [IMAGE] placeholders.",
            py::arg("text"));

    web.def("make_cache_slug",
            [](py::str url) -> py::str {
                kimix::string u = require_str(url, "url must be str");
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::web_search::make_cache_slug(u);
                }
                return to_py_str(out);
            },
            "XXH64 hex digest prefix used for web_search cache keys.",
            py::arg("url"));

    web.def("make_cache_file_name",
            [](py::str url) -> py::str {
                kimix::string u = require_str(url, "url must be str");
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::web_search::make_cache_file_name(u);
                }
                return to_py_str(out);
            },
            "Cache file name \"<host-slug>-<digest>.md\" for a URL.",
            py::arg("url"));

    web.def("truncate_with_footer",
            [](py::str content, py::str url, int64_t char_limit,
               bool include_content, py::object cache_dir) -> py::dict {
                kimix::string c = require_str(content, "content must be str");
                kimix::string u = require_str(url, "url must be str");
                kimix::string cd;
                if (!cache_dir.is_none()) {
                    cd = require_str(cache_dir, "cache_dir must be str or None");
                }
                char_limit = kimix::builtin_tools::web_search::clamp_extract_char_limit(char_limit);

                kimix::string store_dir = std::move(cd);
                kimix::optional<kimix::string> stored_path_out;
                kimix::builtin_tools::web_search::store_full_text_fn store_fn =
                    [&store_dir, &stored_path_out](kimix::string_view store_url,
                                                   kimix::string_view store_content)
                        -> kimix::optional<kimix::string> {
                    if (store_dir.empty()) {
                        return {};
                    }
                    kimix::string path;
                    bool ok = kimix::builtin_tools::web_search::store_full_text(
                        store_url, store_content, store_dir, path);
                    if (!ok) {
                        return {};
                    }
                    stored_path_out = path;
                    return path;
                };

                if (!include_content) {
                    // Match Python semantics: no full-text storage side effect
                    // when the caller does not want the full content preserved.
                    store_fn = [](kimix::string_view, kimix::string_view)
                                   -> kimix::optional<kimix::string> { return {}; };
                }

                kimix::builtin_tools::web_search::truncate_with_footer_result r;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    r = kimix::builtin_tools::web_search::truncate_with_footer(
                        c, u, char_limit, store_fn);
                }
                py::dict d;
                d["text"] = to_py_str(r.text);
                d["was_truncated"] = r.was_truncated;
                d["stored_path"] = opt_str_to_obj(stored_path_out);
                return d;
            },
            "Truncate long text with a head+tail footer.  When include_content is "
            "true and cache_dir is set, the full text is stored and the path is "
            "returned in stored_path.",
            py::arg("content"),
            py::arg("url"),
            py::arg("char_limit"),
            py::arg("include_content") = true,
            py::arg("cache_dir") = py::none());

    web.def("clamp_search_limit",
            [](int64_t limit) -> int {
                int out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::web_search::clamp_search_limit(limit);
                }
                return out;
            },
            "Clamp a search result limit to the supported [1, 20] range.",
            py::arg("limit"));

    web.def("clamp_extract_char_limit",
            [](int64_t char_limit) -> int64_t {
                int64_t out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::web_search::clamp_extract_char_limit(char_limit);
                }
                return out;
            },
            "Clamp an extract character limit to the supported [2000, 500000] range.",
            py::arg("char_limit"));

    web.def("build_search_output",
            [](py::list items, py::object opts) -> py::dict {
                kimix::vector<kimix::builtin_tools::web_search::web_item> web_items =
                    require_web_items(items, "items must be a list of dicts");

                kimix::builtin_tools::web_search::build_search_output_options ws_opts;
                if (!opts.is_none()) {
                    if (!py::isinstance<py::dict>(opts)) {
                        throw py::type_error("opts must be dict or None");
                    }
                    py::dict d = opts.cast<py::dict>();
                    ws_opts.include_content = dict_get<bool>(d, "include_content", false);
                    ws_opts.max_content_chars = static_cast<size_t>(
                        dict_get<int64_t>(d, "max_content_chars", 0));
                    int64_t max_output_bytes = dict_get<int64_t>(
                        d, "max_output_bytes", static_cast<int64_t>(kimix::builtin_tools::k_max_output_bytes));
                    if (max_output_bytes < 0) {
                        max_output_bytes = static_cast<int64_t>(kimix::builtin_tools::k_max_output_bytes);
                    }
                    ws_opts.max_output_bytes = static_cast<size_t>(max_output_bytes);
                    ws_opts.summary = dict_opt_str(d, "summary");
                }

                kimix::builtin_tools::web_search::build_search_output_result r;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    r = kimix::builtin_tools::web_search::build_search_output(
                        web_items, ws_opts);
                }
                py::dict d;
                d["text"] = to_py_str(r.text);
                d["truncated"] = r.truncated;
                d["omitted_items"] = static_cast<int64_t>(r.omitted_items);
                return d;
            },
            "Render web search results to the canonical markdown text. "
            "opts keys: include_content, max_content_chars, max_output_bytes, summary.",
            py::arg("items"),
            py::arg("opts") = py::none());

    // -----------------------------------------------------------------------
    // retrieve -- pure text/ranking kernels (no vector-store calls).
    // -----------------------------------------------------------------------
    web.def("format_retrieve_result",
            [](py::list turns, py::str ref_id) -> py::str {
                kimix::vector<kimix::builtin_tools::retrieve::history_turn> hs =
                    require_history_turns(turns, "turns must be a list of dicts");
                kimix::string rid = require_str(ref_id, "ref_id must be str");
                kimix::builtin_tools::retrieve::retrieve_result r;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    kimix::builtin_tools::retrieve::format_output(hs, rid, r);
                }
                return to_py_str(r.output);
            },
            "Format history turns as the retrieve tool markdown output.",
            py::arg("turns"),
            py::arg("ref_id") = "");

    web.def("parse_turn_reference",
            [](py::str ref) -> py::object {
                kimix::string s = require_str(ref, "ref must be str");
                int64_t out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::retrieve::parse_turn_reference(s);
                }
                if (out < 0) {
                    return py::none();
                }
                return py::int_(out);
            },
            "Parse a turn reference (\"42\" or \"prune_42\"); returns None if invalid.",
            py::arg("ref"));

    web.def("apply_recency_boost",
            [](py::list turns, double recency_weight, double now) -> py::list {
                kimix::vector<kimix::builtin_tools::retrieve::history_turn> hs =
                    require_history_turns(turns, "turns must be a list of dicts");
                {
                    kimix::runtime::common::gil_scoped_release release;
                    kimix::builtin_tools::retrieve::apply_recency_boost(hs, recency_weight, now);
                }
                return history_turns_to_list(hs);
            },
            "Apply a recency boost to ranked history turns in place.",
            py::arg("turns"),
            py::arg("recency_weight"),
            py::arg("now"));

    web.def("sort_and_truncate",
            [](py::list turns, int32_t top_k) -> py::list {
                kimix::vector<kimix::builtin_tools::retrieve::history_turn> hs =
                    require_history_turns(turns, "turns must be a list of dicts");
                {
                    kimix::runtime::common::gil_scoped_release release;
                    kimix::builtin_tools::retrieve::sort_and_truncate(hs, top_k);
                }
                return history_turns_to_list(hs);
            },
            "Stable-sort history turns by boosted score and truncate to top_k.",
            py::arg("turns"),
            py::arg("top_k"));

    // -----------------------------------------------------------------------
    // compact -- pure decision / prompt-assembly kernels.
    // -----------------------------------------------------------------------
    web.def("extract_text",
            [](py::dict message, py::str sep) -> py::str {
                kimix::builtin_tools::compact::message msg;
                if (!parse_message(message, msg)) {
                    throw py::type_error("message must be a dict with role/content");
                }
                kimix::string s = require_str(sep, "sep must be str");
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::compact::extract_text(msg, s);
                }
                return to_py_str(out);
            },
            "Join the text parts of a Kosong-style message with sep.",
            py::arg("message"),
            py::arg("sep") = "");

    web.def("has_think_part",
            [](py::dict message) -> bool {
                kimix::builtin_tools::compact::message msg;
                if (!parse_message(message, msg)) {
                    throw py::type_error("message must be a dict with role/content");
                }
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::compact::has_think_part(msg);
                }
                return out;
            },
            "True when the message contains a think part.",
            py::arg("message"));

    web.def("is_user_or_assistant",
            [](py::dict message) -> bool {
                kimix::builtin_tools::compact::message msg;
                if (!parse_message(message, msg)) {
                    throw py::type_error("message must be a dict with role/content");
                }
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::compact::is_user_or_assistant(msg);
                }
                return out;
            },
            "True when the message role is user or assistant.",
            py::arg("message"));

    web.def("should_auto_compact",
            [](int64_t token_count, py::dict cfg) -> bool {
                kimix::builtin_tools::compact::compaction_trigger_config c;
                c.trigger_ratio = dict_get<double>(cfg, "trigger_ratio", 0.75);
                c.max_context_size = dict_get<int64_t>(cfg, "max_context_size", 128000);
                c.reserved_context_size = dict_get<int64_t>(cfg, "reserved_context_size", 8192);
                c.max_tokens = dict_get<int64_t>(cfg, "max_tokens", 0);
                c.tool_call_buffer_tokens = dict_get<int64_t>(cfg, "tool_call_buffer_tokens", 0);
                c.safety_margin_tokens = dict_get<int64_t>(cfg, "safety_margin_tokens", 1024);
                bool out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::compact::should_auto_compact(token_count, c);
                }
                return out;
            },
            "Return true when token count crosses the auto-compaction threshold. "
            "cfg keys: trigger_ratio, max_context_size, reserved_context_size, "
            "max_tokens, tool_call_buffer_tokens, safety_margin_tokens.",
            py::arg("token_count"),
            py::arg("cfg") = py::dict());

    web.def("build_compaction_prompt",
            [](py::list to_compact, py::dict options, py::str custom_instruction,
               py::str prompt_compact, py::str prompt_compact_cascade) -> py::dict {
                kimix::vector<kimix::builtin_tools::compact::message> msgs =
                    require_messages(to_compact, "to_compact must be a list of message dicts");
                kimix::builtin_tools::compact::compaction_options opts =
                    parse_compaction_options(options);
                kimix::string custom = require_str(custom_instruction, "custom_instruction must be str");
                kimix::string prompt = require_str(prompt_compact, "prompt_compact must be str");
                kimix::string prompt_cascade = require_str(prompt_compact_cascade, "prompt_compact_cascade must be str");

                kimix::builtin_tools::compact::compaction_prompt_input in;
                in.to_compact = msgs;
                in.options = opts;
                in.custom_instruction = custom;
                in.prompt_compact = prompt;
                in.prompt_compact_cascade = prompt_cascade;

                kimix::builtin_tools::compact::compaction_prompt_output out;
                kimix::builtin_tools::tool_error err;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    err = kimix::builtin_tools::compact::build_compaction_prompt(in, out);
                }
                if (err.failed()) {
                    throw_tool_error(err);
                }
                py::dict d;
                d["prompt_text"] = to_py_str(out.prompt_text);
                d["cascade_depth"] = out.cascade_depth;
                return d;
            },
            "Assemble the compaction instruction prompt text.",
            py::arg("to_compact"),
            py::arg("options") = py::dict(),
            py::arg("custom_instruction") = "",
            py::arg("prompt_compact") = "",
            py::arg("prompt_compact_cascade") = "");

    web.def("build_compact_message_text",
            [](py::list to_compact, py::str prompt_text) -> py::str {
                kimix::vector<kimix::builtin_tools::compact::message> msgs =
                    require_messages(to_compact, "to_compact must be a list of message dicts");
                kimix::string pt = require_str(prompt_text, "prompt_text must be str");
                kimix::builtin_tools::compact::compact_message_request req;
                req.to_compact = msgs;
                req.prompt_text = pt;
                kimix::string out;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    out = kimix::builtin_tools::compact::build_compact_message_text(req);
                }
                return to_py_str(out);
            },
            "Build the legacy flattened compact_message text.",
            py::arg("to_compact"),
            py::arg("prompt_text"));

    web.def("prepare_compaction_input",
            [](py::list messages, int64_t preserve_start_index, py::dict options,
               py::str custom_instruction, py::str prompt_compact,
               py::str prompt_compact_cascade) -> py::dict {
                kimix::vector<kimix::builtin_tools::compact::message> msgs =
                    require_messages(messages, "messages must be a list of message dicts");
                kimix::builtin_tools::compact::compaction_options opts =
                    parse_compaction_options(options);
                kimix::string custom = require_str(custom_instruction, "custom_instruction must be str");
                kimix::string prompt = require_str(prompt_compact, "prompt_compact must be str");
                kimix::string prompt_cascade = require_str(prompt_compact_cascade, "prompt_compact_cascade must be str");

                kimix::builtin_tools::compact::prepare_request req;
                req.messages = msgs;
                if (preserve_start_index < 0) {
                    req.preserve_start_index = 0;
                } else if (static_cast<size_t>(preserve_start_index) > msgs.size()) {
                    req.preserve_start_index = msgs.size();
                } else {
                    req.preserve_start_index = static_cast<size_t>(preserve_start_index);
                }
                req.options = opts;
                req.custom_instruction = custom;
                req.prompt_compact = prompt;
                req.prompt_compact_cascade = prompt_cascade;

                kimix::builtin_tools::compact::prepare_result out;
                kimix::builtin_tools::tool_error err;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    err = kimix::builtin_tools::compact::prepare_compaction_input(req, out);
                }
                if (err.failed()) {
                    throw_tool_error(err);
                }
                py::dict d;
                d["to_compact"] = messages_to_list(out.to_compact);
                d["to_preserve"] = messages_to_list(out.to_preserve);
                d["compact_message_text"] = to_py_str(out.compact_message_text);
                d["prompt_text"] = to_py_str(out.prompt_text);
                d["cascade_depth"] = out.cascade_depth;
                return d;
            },
            "Slice history and build the legacy compact message + prompt.",
            py::arg("messages"),
            py::arg("preserve_start_index"),
            py::arg("options") = py::dict(),
            py::arg("custom_instruction") = "",
            py::arg("prompt_compact") = "",
            py::arg("prompt_compact_cascade") = "");
}
