// tool.cpp - Generic tool-parameter + tool-base infrastructure.
//
// Implements ToolParams::serialize / deserialize (and the try_deserialize
// convenience) with the vendored yyjson library using the shared
// mimalloc-backed allocator kimix::llm::kYYJsonAlcMi (D3), plus the
// recursive ValueElement <-> yyjson converters and the out-of-line Tool
// destructor (vtable anchor).
//
// Error reporting is by return value, not by exceptions: kimix is built with
// kimix_enable_exception=false, so serialize()/deserialize() return false and
// (optionally) fill an error message instead of throwing std::runtime_error.
//
// Unity-build rules (see tool.h): every helper here is file-local (anonymous
// namespace) and prefixed `tl_` so the concatenated kimix-llm translation
// unit cannot collide with other tools.

#include "builtin_tools/tool.h"

#include <mimalloc.h>
#include <yyjson.h>
#include "builtin_tools/utf8_util.h" // code-point count / prefix for the display clamp
#include "llm/yyjson_alc.h" // kimix::llm::kYYJsonAlcMi (mimalloc-backed)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kimix::builtin_tools {

namespace {

// ── Recursive ValueElement <-> yyjson converters (TU-local, tl_ prefix) ─────

// Forward declaration of the mutually recursive converter (the object branch
// below needs tl_obj_add, which needs tl_to_json).
yyjson_mut_val *tl_to_json(yyjson_mut_doc *doc, const ValueElement &e);

// Adds one key/value pair to a mutable object. `yyjson_mut_obj_add_val` takes a
// NUL-terminated key (strlen), so it would silently truncate a key carrying an
// embedded NUL -- and `{"a\u0000b":1}` parses into exactly such a key.
// Building the key as a yyjson string with an explicit length keeps it whole.
void tl_obj_add(yyjson_mut_doc *doc, yyjson_mut_val *obj, const kimix::string &key,
                const ValueElement &value) {
    yyjson_mut_val *key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
    yyjson_mut_obj_add(obj, key_val, tl_to_json(doc, value));
}

// Serializes one ValueElement into a mutable yyjson value owned by `doc`.
yyjson_mut_val *tl_to_json(yyjson_mut_doc *doc, const ValueElement &e) {
    if (e.is_null()) {
        return yyjson_mut_null(doc);
    }
    if (e.is_bool()) {
        return yyjson_mut_bool(doc, e.as_bool());
    }
    if (e.is_int()) {
        return yyjson_mut_int(doc, e.as_int());
    }
    if (e.is_uint()) {
        return yyjson_mut_uint(doc, e.as_uint());
    }
    if (e.is_real()) {
        return yyjson_mut_real(doc, e.as_real());
    }
    if (e.is_string()) {
        const kimix::string &s = e.as_string();
        // Copies the bytes (embedded NULs are preserved); the writer escapes
        // control characters when emitting.
        return yyjson_mut_strncpy(doc, s.data(), s.size());
    }
    if (e.is_array()) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (const ValueElement &elem : e.as_array()) {
            yyjson_mut_arr_append(arr, tl_to_json(doc, elem));
        }
        return arr;
    }
    // Object -> nested ToolParams.
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    const ToolParams *inner = e.as_object();
    if (inner != nullptr) {
        for (const auto &[k, v] : inner->values) {
            // The doc owns a copy of the key bytes (NUL-safe, see tl_obj_add).
            tl_obj_add(doc, obj, k, v);
        }
    }
    return obj;
}

// Parses one immutable yyjson value into a ValueElement (deep copy).
ValueElement tl_from_json(const yyjson_val *v) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL:
            return ValueElement::make_null();
        case YYJSON_TYPE_BOOL:
            return ValueElement::make_bool(yyjson_get_bool(v));
        case YYJSON_TYPE_NUM:
            // yyjson stores every non-negative integer as uint, so keep
            // uint64_t only for values beyond INT64_MAX; everything else that
            // is integral maps to int64_t, and anything with a fraction or
            // exponent maps to double. Preserves 1 as int and 1.0 as real.
            if (yyjson_is_uint(v)) {
                const uint64_t u = yyjson_get_uint(v);
                if (u <= static_cast<uint64_t>(
                             std::numeric_limits<int64_t>::max())) {
                    return ValueElement::make_int(static_cast<int64_t>(u));
                }
                return ValueElement::make_uint(u);
            }
            if (yyjson_is_sint(v)) {
                return ValueElement::make_int(yyjson_get_sint(v));
            }
            return ValueElement::make_real(yyjson_get_real(v));
        case YYJSON_TYPE_STR:
            return ValueElement::make_string(
                kimix::string(yyjson_get_str(v), yyjson_get_len(v)));
        case YYJSON_TYPE_ARR: {
            ValueElement::Array arr;
            size_t i, n;
            yyjson_val *item;
            yyjson_arr_foreach(v, i, n, item) {
                arr.push_back(tl_from_json(item));
            }
            return ValueElement::make_array(std::move(arr));
        }
        case YYJSON_TYPE_OBJ: {
            kimix::shared_ptr<ToolParams> obj(new ToolParams());
            size_t i, n;
            yyjson_val *key;
            yyjson_val *val;
            yyjson_obj_foreach(v, i, n, key, val) {
                kimix::string k(yyjson_get_str(key), yyjson_get_len(key));
                obj->values[std::move(k)] = tl_from_json(val);
            }
            return ValueElement::make_object(std::move(obj));
        }
        default:
            // Unreachable for values produced by a successful yyjson parse.
            return ValueElement::make_null();
    }
}

// ── Fuzzy alias matching helpers (TU-local, tl_ prefix) ─────────────────────

// True when `name` is one of the separator-separated names in `list`,
// compared with the folded alias comparison (see alias_detail in tool.h).
bool tl_alias_listed(kimix::string_view list, kimix::string_view name) {
    bool found = false;
    alias_detail::for_each_alias_name(list, [&found, name](kimix::string_view existing) {
        if (!found && alias_detail::alias_name_equals(existing, name)) {
            found = true;
        }
    });
    return found;
}

} // namespace

void ToolParams::add_alias(kimix::string_view canonical,
                           kimix::string_view alternates) {
    if (canonical.empty() || alternates.empty()) {
        return;
    }
    kimix::string &record = alias_map[kimix::string(canonical)];
    // Collect the new names separately: `record` must not be read while it is
    // being appended to (its tokens are string views into it).
    kimix::string added;
    alias_detail::for_each_alias_name(
        alternates, [&record, &added](kimix::string_view name) {
            if (name.empty() || tl_alias_listed(record, name) ||
                tl_alias_listed(added, name)) {
                return; // already declared (folded comparison)
            }
            if (!added.empty()) {
                added.push_back(' ');
            }
            added.append(name.data(), name.size());
        });
    if (added.empty()) {
        return;
    }
    if (!record.empty()) {
        record.push_back(' ');
    }
    record += added;
}

const ValueElement *ToolParams::get_alias(kimix::string_view key) const {
    if (alias_map.empty()) {
        return nullptr;
    }
    auto it = alias_map.find(kimix::string(key));
    if (it == alias_map.end()) {
        return nullptr;
    }
    const kimix::string_view alternates = it->second;
    // Pass 1: the declared names, exactly as written, in declaration order.
    const ValueElement *hit = nullptr;
    alias_detail::for_each_alias_name(alternates, [this, &hit](kimix::string_view name) {
        if (hit != nullptr) {
            return;
        }
        auto v = values.find(kimix::string(name));
        if (v != values.end() && !v->second.is_null()) {
            hit = &v->second;
        }
    });
    if (hit != nullptr) {
        return hit;
    }
    // Pass 2: folded comparison (case and '_'/'-'/' ' ignored). `values`
    // iteration order is unspecified, so among several matching keys the
    // lexicographically smallest one wins - the result is deterministic.
    kimix::string_view best;
    bool have = false;
    for (const auto &entry : values) {
        if (entry.second.is_null()) {
            continue;
        }
        bool match = false;
        alias_detail::for_each_alias_name(
            alternates, [&entry, &match](kimix::string_view name) {
                if (!match && alias_detail::alias_name_equals(entry.first, name)) {
                    match = true;
                }
            });
        if (match && (!have || kimix::string_view(entry.first) < best)) {
            best = kimix::string_view(entry.first);
            have = true;
        }
    }
    return have ? get_exact(best) : nullptr;
}

bool ToolParams::serialize(kimix::vector<char> &out, kimix::string *error) const {
    out.clear();
    if (error != nullptr) {
        error->clear();
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        if (error != nullptr) {
            *error = "ToolParams::serialize: failed to create yyjson document";
        }
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (const auto &[k, v] : values) {
        tl_obj_add(doc, root, k, v); // NUL-safe key (see tl_obj_add)
    }

    size_t len = 0;
    // Pass the mimalloc allocator explicitly: yyjson_mut_write() itself falls
    // back to YYJSON_DEFAULT_ALC (malloc), so the buffer must come from
    // write_opts with kYYJsonAlcMi to keep the mi_free contract (D3).
    char *json = yyjson_mut_write_opts(doc, 0 /* compact */,
                                       &kimix::llm::kYYJsonAlcMi, &len, nullptr);
    if (json == nullptr) {
        yyjson_mut_doc_free(doc);
        if (error != nullptr) {
            *error = "ToolParams::serialize: failed to serialize JSON";
        }
        return false;
    }
    // The write buffer was allocated through the mimalloc allocator passed to
    // write_opts: release with mi_free, never free(). Copy before freeing.
    out.assign(json, json + len);
    mi_free(json);
    yyjson_mut_doc_free(doc);
    return true;
}

bool ToolParams::deserialize(kimix::span<char const> in, kimix::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    yyjson_read_err err{};
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(in.data()), in.size(),
                                       0 /* no flags: strict, stop-on-error */,
                                       &kimix::llm::kYYJsonAlcMi, &err);
    if (doc == nullptr) {
        if (error != nullptr) {
            kimix::string msg = "ToolParams::deserialize: invalid JSON: ";
            msg += (err.msg != nullptr) ? err.msg : "unknown error";
            *error = std::move(msg);
        }
        return false;
    }
    // RAII-style cleanup: doc is released on every exit path.
    struct doc_guard {
        yyjson_doc *d;
        ~doc_guard() {
            if (d != nullptr) {
                yyjson_doc_free(d);
            }
        }
    } guard{doc};

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        if (error != nullptr) {
            *error = "ToolParams::deserialize: root must be a JSON object";
        }
        return false;
    }
    values.clear();
    size_t i, n;
    yyjson_val *key;
    yyjson_val *val;
    yyjson_obj_foreach(root, i, n, key, val) {
        kimix::string k(yyjson_get_str(key), yyjson_get_len(key));
        values[std::move(k)] = tl_from_json(val);
    }
    return true;
}

bool ToolParams::try_deserialize(kimix::span<char const> in,
                                 kimix::string &error) {
    return deserialize(in, &error);
}

// ── Availability overrides (the hook behind Tool::valid()) ─────────────────
// One process-wide table registry-key -> pinned answer, so a unit test can
// force the "python is not installed" / "no Git Bash on Windows" branches on a
// machine where they are installed (and vice versa). Meyers singleton +
// spin_mutex, mirroring ToolRegistry: valid() may be called from any thread.
namespace tool_availability {
namespace {

struct tl_availability_table {
    kimix::spin_mutex mutex;
    kimix::unordered_map<kimix::string, bool, kimix::string_hash> pins;
};

tl_availability_table &tl_availability() {
    static tl_availability_table table; // safe before and after main()
    return table;
}

} // namespace

void set_override(kimix::string_view key, bool available) {
    tl_availability_table &table = tl_availability();
    std::lock_guard<kimix::spin_mutex> guard(table.mutex);
    table.pins[kimix::string(key)] = available;
}

void clear_override(kimix::string_view key) {
    tl_availability_table &table = tl_availability();
    std::lock_guard<kimix::spin_mutex> guard(table.mutex);
    table.pins.erase(kimix::string(key));
}

void clear_all() {
    tl_availability_table &table = tl_availability();
    std::lock_guard<kimix::spin_mutex> guard(table.mutex);
    table.pins.clear();
}

kimix::optional<bool> override_of(kimix::string_view key) {
    tl_availability_table &table = tl_availability();
    std::lock_guard<kimix::spin_mutex> guard(table.mutex);
    const auto it = table.pins.find(kimix::string(key));
    if (it == table.pins.end()) {
        return kimix::optional<bool>{};
    }
    return kimix::optional<bool>{it->second};
}

} // namespace tool_availability

bool tool_valid(kimix::string_view key, bool probed) {
    const kimix::optional<bool> pinned = tool_availability::override_of(key);
    return pinned.has_value() ? *pinned : probed;
}

bool session_work_dir_usable(const Session *session) {
    if (session == nullptr || session->work_dir.empty()) {
        return true; // no work dir named: the process cwd applies
    }
    std::error_code ec;
    // work_dir follows the CLI's ANSI/lossy convention; the narrow path
    // constructor THROWS std::system_error on bytes the code page cannot
    // represent (fatal without C++ exceptions), so build wide instead - an
    // unrepresentable work dir is simply not usable.
    kimix::filesystem::path wd;
    if (!kimix::path_from_narrow(session->work_dir, wd)) {
        if (!kimix::path_from_utf8(session->work_dir, wd)) {
            return false;
        }
    }
    return kimix::filesystem::is_directory(wd, ec);
}

// ── CLI display line (Tool::operator()'s display_str) ──────────────────────

void tool_display_append(kimix::string &line, kimix::string_view part) {
    // Surrounding ASCII whitespace is dropped, then an empty part is skipped:
    // callers pass the fields they happen to have and let the joiner decide.
    size_t begin = 0;
    size_t end = part.size();
    while (begin < end && static_cast<unsigned char>(part[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(part[end - 1]) <= ' ') {
        --end;
    }
    if (begin >= end) {
        return;
    }
    if (!line.empty()) {
        line += " | "; // the reference's brief join (tools/common.py:338)
    }
    line.append(part.data() + begin, end - begin);
}

kimix::string tool_display_join(std::initializer_list<kimix::string_view> parts) {
    kimix::string line;
    for (const kimix::string_view part : parts) {
        tool_display_append(line, part);
    }
    tool_display_finish(line);
    return line;
}

void tool_display_finish(kimix::string &line) {
    // One printable line: every ASCII control byte (newline, tab, CR, DEL)
    // becomes at most one space, and the line is clamped by CODE POINTS so a
    // multi-byte character is never split (utf8_util.h).
    kimix::string folded;
    folded.reserve(line.size());
    bool pending_space = false;
    for (const char c : line) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= ' ' || u == 0x7f) {
            pending_space = !folded.empty();
            continue;
        }
        if (pending_space) {
            folded.push_back(' ');
            pending_space = false;
        }
        folded.push_back(c);
    }
    if (utf8_code_point_count(folded) > kToolDisplayMaxChars) {
        const size_t cut = utf8_byte_offset_of_code_point(
            folded, kToolDisplayMaxChars - 3);
        folded.resize(cut);
        while (!folded.empty() && folded.back() == ' ') {
            folded.pop_back();
        }
        folded += "...";
    }
    line = std::move(folded);
}

kimix::string tool_display_size(kimix::string_view text) {
    if (text.empty()) {
        return {};
    }
    size_t lines = 1;
    for (const char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    const size_t bytes = text.size();
    if (bytes >= 1024u * 1024u) {
        return kimix::format("{} lines, {:.1f} MB", lines,
                             static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024u) {
        return kimix::format("{} lines, {:.1f} KB", lines,
                             static_cast<double>(bytes) / 1024.0);
    }
    return kimix::format("{} lines, {} B", lines, bytes);
}

kimix::string_view tool_display_field(const ToolParams &result,
                                      kimix::string_view key) {
    const ValueElement *value = result.get_exact(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return value->as_string();
}

int64_t tool_display_int(const ToolParams &result, kimix::string_view key,
                         int64_t fallback) {
    const ValueElement *value = result.get_exact(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_int()) {
        return value->as_int();
    }
    if (value->is_uint()) {
        return static_cast<int64_t>(value->as_uint());
    }
    return fallback;
}

kimix::string Tool::display_line() const {
    kimix::vector<char> payload;
    result_json(payload);
    if (payload.empty()) {
        return {};
    }
    ToolParams result;
    kimix::string error;
    if (!result.try_deserialize(
            kimix::span<char const>(payload.data(), payload.size()), error)) {
        return {};
    }
    return tool_display_of(result);
}

// The display line of a result object that carries the reference's status /
// brief / message fields. "ok"/"success" carries no news, so the status word is
// only printed for a failure; `extra` (the tool's own count / path / exit code)
// leads the line. Empty when the object says nothing printable.
kimix::string tool_display_of(const ToolParams &result,
                              kimix::string_view extra) {
    // The three fields every tool payload may carry: the failure status (an
    // "ok" call says nothing by itself), the reference-style brief, and the
    // human message. The full "output" is deliberately NOT part of the line -
    // that is exactly what the display line replaces.
    kimix::string line;
    tool_display_append(line, extra);
    if (const ValueElement *status = result.get_exact("status");
        status != nullptr && status->is_string()) {
        const kimix::string &text = status->as_string();
        if (!(text == "ok" || text == "success")) {
            tool_display_append(line, text);
        }
    }
    // `brief` is the reference's one-line display text and `message` the human
    // sentence; both join the line, because the CLI has nothing else to show:
    // its detail line is parsed from the tool MESSAGE, whose body is the
    // envelope (not the payload), so a failure's text reaches the terminal
    // through here or not at all.
    if (const ValueElement *brief = result.get_exact("brief");
        brief != nullptr && brief->is_string()) {
        tool_display_append(line, brief->as_string());
    }
    if (const ValueElement *message = result.get_exact("message");
        message != nullptr && message->is_string()) {
        tool_display_append(line, message->as_string());
    }
    tool_display_finish(line);
    return line;
}

// ── Oversized-result spill (tool_output_spill_scope) ─────────────────────────
namespace {
// Temp-file location of a spilled result: the ag_default_save_prompt pattern
// (agent_tool.cpp) - a per-process-millis tmp_<millis> dir under the work
// dir's .kimix_cache (the process cwd when the session names no usable work
// dir), a process-local sequence in the file name. The returned path is
// forward-slashed (ag_default_save_prompt's display convention); "" when the
// directory cannot be created.
kimix::string tl_output_spill_path(const Session *session) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
      // work_dir follows the CLI's ANSI/lossy convention; the narrow path
      // constructor THROWS std::system_error on bytes the code page cannot
      // represent (fatal without C++ exceptions), so build wide instead.
      fs::path root;
      if (session != nullptr && !session->work_dir.empty() &&
          session_work_dir_usable(session)) {
          if (!kimix::path_from_narrow(session->work_dir, root)) {
              kimix::path_from_utf8(session->work_dir, root);
          }
      }
      if (root.empty()) {
          root = fs::path(".");
      }
    const int64_t millis = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const fs::path dir =
        root / ".kimix_cache" / kimix::format("tmp_{}", millis);
    fs::create_directories(dir, ec);
    if (ec) {
        return {};
    }
    static std::atomic<uint64_t> index{0};
    const uint64_t n = index.fetch_add(1);
    kimix::string out = kimix::to_string(dir / fs::path(
        kimix::format("output_{}.txt", n)));
    for (char &c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}
} // namespace

tool_output_spill_scope::~tool_output_spill_scope() {
    // Measure the serialized payload the tool produced. The ToolParams style
    // serializes once here (its result_json serializes again later, on the
    // small pointer payload after a spill - the oversized text exists in
    // memory only until this destructor decides).
    kimix::vector<char> serialized;
    const kimix::vector<char> *measured = _buffer;
    if (_buffer == nullptr) {
        if (!_params->serialize(serialized)) {
            return; // cannot measure: leave the result untouched
        }
        measured = &serialized;
    }
    if (measured->size() <= kToolOutputSpillMaxBytes) {
        return; // within budget: the common case costs one size check
    }
    // Over budget: dump the FULL payload to a temp file (best effort) and
    // hand the model a pointer payload instead. A failed dump keeps the
    // oversized payload - the soul's per-tool output budget truncates it
    // inline (soul.cpp F3) - so the result is never lost.
    const kimix::string path = tl_output_spill_path(_tool.session());
    if (path.empty()) {
        return;
    }
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    if (!measured->empty()) {
        std::fwrite(measured->data(), 1, measured->size(), f);
    }
    std::fclose(f);
    kimix::string msg = "output too long (";
    msg += std::to_string(measured->size());
    msg += " bytes), saved to ";
    msg += path;
    // status "ok": the tool SUCCEEDED - only the delivery was redirected.
    // "error" would render the soul's "unexpected error" sentence, which is
    // wrong here. The pointer sentence rides the `message` field (the
    // envelope's <system> line), exactly where a tool report puts its summary.
    ToolParams pointer;
    pointer["status"] = ValueElement::make_string("ok");
    pointer["message"] = ValueElement::make_string(std::move(msg));
    kimix::vector<char> replacement;
    if (!pointer.serialize(replacement)) {
        return; // keep the oversized payload (F3 fallback) on serial failure
    }
    if (_buffer != nullptr) {
        *_buffer = std::move(replacement);
    } else {
        *_params = std::move(pointer);
    }
}

// Out-of-line: anchors the vtable in kimix-llm - and unregisters the instance
// from the session's tool-pointer map. The map is non-owning (raw pointers),
// so without this a destroyed tool would leave a dangling entry behind and a
// later Session::tool_pointer() lookup would hand it out (two-stage init
// contract: the map only ever names LIVE instances).
Tool::~Tool() {
    if (_session == nullptr) {
        return;
    }
    for (auto it = _session->tool_pointers.begin();
         it != _session->tool_pointers.end();) {
        if (it->second == this) {
            it = _session->tool_pointers.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace kimix::builtin_tools
