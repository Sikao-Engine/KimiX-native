// agent/compaction_ledger.cpp - see compaction_ledger.h (the
// kimi_cli/soul/compaction_ledger.py port).
//
// Byte shape notes: one JSON object per line, compact (no spaces), keys in
// _record_to_dict order, orjson float rendering (shortest round-trip, fixed
// notation while -5 <= exponent <= 15) and OPT_APPEND_NEWLINE (one trailing
// '\n' per record). record_end rewrites the whole file from the parsed record
// list, so a line that previously lost its "error" key keeps exactly that
// shape (the reference's rec.pop("error", None) / rec["error"] = ...).

#include "agent/compaction_ledger.h"

#include <charconv>
#include <cstdio>
#include <system_error>

#include <yyjson.h>

#include "core/json_repair.h"
#include "llm/yyjson_alc.h"

namespace kimix::agent {
namespace {

constexpr const char *kLedgerFileName = "compaction_ledger.jsonl";
constexpr const char *kLedgerCacheDir = ".kimix_cache";

// --- orjson-compatible scalar writers (see cli_session.cpp's clis_* twins) ---

void cled_append_int(kimix::string &out, int64_t value) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, static_cast<size_t>(res.ptr - buf));
}

// orjson/Python float rendering (Ryu d2s rule): fixed notation while
// -5 <= decimal exponent <= 15, scientific beyond, ".0" for integral values.
void cled_append_real(kimix::string &out, double value) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value,
                                   std::chars_format::scientific);
    if (res.ec != std::errc()) {
        out += "0.0";
        return;
    }
    const size_t len = static_cast<size_t>(res.ptr - buf);
    size_t i = 0;
    bool negative = false;
    if (i < len && buf[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= len || buf[i] < '0' || buf[i] > '9') {
        out += "0.0";
        return;
    }
    kimix::string digits;
    while (i < len && buf[i] != 'e' && buf[i] != 'E') {
        if (buf[i] != '.') {
            digits.push_back(buf[i]);
        }
        ++i;
    }
    if (i >= len || digits.empty()) {
        out += "0.0";
        return;
    }
    ++i;
    bool exp_negative = false;
    if (i < len && (buf[i] == '+' || buf[i] == '-')) {
        exp_negative = buf[i] == '-';
        ++i;
    }
    int32_t exp10 = 0;
    for (; i < len; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            exp10 = exp10 * 10 + (buf[i] - '0');
        }
    }
    if (exp_negative) {
        exp10 = -exp10;
    }
    if (negative) {
        out.push_back('-');
    }
    if (exp10 >= -5 && exp10 <= 15) {
        if (exp10 >= 0) {
            const size_t whole = static_cast<size_t>(exp10) + 1;
            for (size_t d = 0; d < whole; ++d) {
                out.push_back(d < digits.size() ? digits[d] : '0');
            }
            if (digits.size() > whole) {
                out.push_back('.');
                out += digits.substr(whole);
            } else {
                out += ".0";
            }
        } else {
            out += "0.";
            for (int32_t z = 0; z < -exp10 - 1; ++z) {
                out.push_back('0');
            }
            out += digits;
        }
        return;
    }
    out.push_back(digits[0]);
    if (digits.size() > 1) {
        out.push_back('.');
        out += digits.substr(1);
    }
    out.push_back('e');
    out += exp10 < 0 ? "-" : "+";
    cled_append_int(out, exp10 < 0 ? -static_cast<int64_t>(exp10)
                                   : static_cast<int64_t>(exp10));
}

// orjson/Python string escaping (raw UTF-8, \u00XX for the other controls).
void cled_append_escaped(kimix::string &out, kimix::string_view s) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20u) {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0Fu]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

// _record_to_dict + orjson.dumps(..., OPT_APPEND_NEWLINE): `with_error` false
// renders the shape AFTER a successful record_end (the key is popped).
void cled_render(const CompactionRecord &r, bool with_error, kimix::string &out) {
    out += "{\"compaction_id\":";
    cled_append_escaped(out, r.compaction_id);
    out += ",\"trigger\":";
    cled_append_escaped(out, r.trigger);
    out += ",\"started_at\":";
    cled_append_real(out, r.started_at);
    out += ",\"shadowed_range\":[";
    cled_append_int(out, r.shadowed_start);
    out.push_back(',');
    cled_append_int(out, r.shadowed_end);
    out += "],\"shadowed_tokens\":";
    cled_append_int(out, r.shadowed_tokens);
    out += ",\"summary_tokens\":";
    cled_append_int(out, r.summary_tokens);
    out += ",\"preserved_tokens\":";
    cled_append_int(out, r.preserved_tokens);
    out += ",\"shrank\":";
    out += r.shrank ? "true" : "false";
    if (with_error) {
        out += ",\"error\":";
        if (r.has_error) {
            cled_append_escaped(out, r.error);
        } else {
            out += "null";
        }
    }
    out += "}\n";
}

yyjson_doc *cled_parse(kimix::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(text.data()),
                                       text.size(), 0, &kimix::llm::kYYJsonAlcMi,
                                       nullptr);
    if (doc != nullptr) {
        return doc;
    }
    // loads_relaxed: strict parse first, json_repair fallback. A repaired line
    // is still a ledger line; an unrepairable one is skipped by the caller.
    const kimix::string repaired = kimix::repair(text);
    if (repaired.empty()) {
        return nullptr;
    }
    return yyjson_read_opts(const_cast<char *>(repaired.data()), repaired.size(),
                            0, &kimix::llm::kYYJsonAlcMi, nullptr);
}

const yyjson_val *cled_member(const yyjson_val *obj, const char *key) {
    if (obj == nullptr || !yyjson_is_obj(obj)) {
        return nullptr;
    }
    return yyjson_obj_get(obj, key);
}

kimix::string cled_str(const yyjson_val *obj, const char *key) {
    const yyjson_val *v = cled_member(obj, key);
    if (v == nullptr || !yyjson_is_str(v)) {
        return {};
    }
    return kimix::string(yyjson_get_str(v),
                         static_cast<size_t>(yyjson_get_len(v)));
}

int64_t cled_int(const yyjson_val *obj, const char *key, int64_t fallback) {
    const yyjson_val *v = cled_member(obj, key);
    if (v == nullptr || !yyjson_is_num(v)) {
        return fallback;
    }
    if (yyjson_is_uint(v)) {
        return static_cast<int64_t>(yyjson_get_uint(v));
    }
    if (yyjson_is_sint(v)) {
        return yyjson_get_sint(v);
    }
    return static_cast<int64_t>(yyjson_get_real(v));
}

double cled_real(const yyjson_val *obj, const char *key, double fallback) {
    const yyjson_val *v = cled_member(obj, key);
    if (v == nullptr || !yyjson_is_num(v)) {
        return fallback;
    }
    if (yyjson_is_real(v)) {
        return yyjson_get_real(v);
    }
    if (yyjson_is_uint(v)) {
        return static_cast<double>(yyjson_get_uint(v));
    }
    return static_cast<double>(yyjson_get_sint(v));
}

bool cled_bool(const yyjson_val *obj, const char *key, bool fallback) {
    const yyjson_val *v = cled_member(obj, key);
    if (v == nullptr || !yyjson_is_bool(v)) {
        return fallback;
    }
    return yyjson_is_true(v);
}

// One parsed ledger line -> CompactionRecord (missing fields keep the
// struct defaults, exactly like a dict with absent keys would KeyError in
// the reference - we prefer skipping to crashing). `has_error_key` reports
// whether the line carries the "error" KEY at all ("error": null included):
// record_end's rewrite must preserve per-line key presence, because the
// reference mutates a parsed dict (pop / assign) instead of re-rendering a
// fixed shape.
bool cled_record_from_val(const yyjson_val *root, CompactionRecord &out,
                          bool &has_error_key) {
    if (root == nullptr || !yyjson_is_obj(root)) {
        return false;
    }
    const yyjson_val *range = cled_member(root, "shadowed_range");
    if (range != nullptr && yyjson_is_arr(range) &&
        yyjson_arr_size(range) >= 2) {
        const yyjson_val *a = yyjson_arr_get(range, 0);
        const yyjson_val *b = yyjson_arr_get(range, 1);
        if (a != nullptr && yyjson_is_num(a)) {
            out.shadowed_start = static_cast<int64_t>(yyjson_get_num(a));
        }
        if (b != nullptr && yyjson_is_num(b)) {
            out.shadowed_end = static_cast<int64_t>(yyjson_get_num(b));
        }
    }
    out.compaction_id = cled_str(root, "compaction_id");
    out.trigger = cled_str(root, "trigger");
    out.started_at = cled_real(root, "started_at", 0.0);
    out.shadowed_tokens = cled_int(root, "shadowed_tokens", 0);
    out.summary_tokens = cled_int(root, "summary_tokens", 0);
    out.preserved_tokens = cled_int(root, "preserved_tokens", 0);
    out.shrank = cled_bool(root, "shrank", false);
    const yyjson_val *err = cled_member(root, "error");
    has_error_key = err != nullptr;
    out.has_error = err != nullptr && !yyjson_is_null(err);
    if (out.has_error) {
        if (yyjson_is_str(err)) {
            out.error.assign(yyjson_get_str(err),
                             static_cast<size_t>(yyjson_get_len(err)));
        } else {
            // A non-string error (orjson would have written a string; a
            // hand-edited line may not) is still "an error happened".
            out.error = "unknown error";
        }
    } else {
        out.error.clear();
    }
    return !out.compaction_id.empty();
}

// _read_records(): every well-formed record, malformed lines skipped.
// `error_keys` (when non-null) receives the per-record "error"-key presence.
void cled_read_records(const kimix::string &path,
                       kimix::vector<CompactionRecord> &out,
                       kimix::vector<bool> *error_keys = nullptr) {
    out.clear();
    if (error_keys != nullptr) {
        error_keys->clear();
    }
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return; // a missing file is an empty ledger (the reference's exists())
    }
    kimix::string text;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        if (end == kimix::string::npos) {
            end = text.size();
        }
          const kimix::string_view line(text.data() + begin, end - begin);
          begin = end + 1;
        // strip(): blank lines are skipped.
        size_t b = 0;
        size_t e = line.size();
        while (b < e && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) {
            ++b;
        }
        while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' ||
                         line[e - 1] == '\r')) {
            --e;
        }
        if (b >= e) {
            continue;
        }
        yyjson_doc *doc = cled_parse(line.substr(b, e - b));
        const yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc)
                                                : nullptr;
        CompactionRecord rec;
        bool has_error_key = false;
        if (cled_record_from_val(root, rec, has_error_key)) {
            out.push_back(std::move(rec));
            if (error_keys != nullptr) {
                error_keys->push_back(has_error_key);
            }
        }
        if (doc != nullptr) {
            yyjson_doc_free(doc);
        }
    }
}

bool cled_write_records(const kimix::string &path,
                        const kimix::vector<CompactionRecord> &records,
                        kimix::vector<bool> with_error, kimix::string &error) {
    kimix::string text;
    for (size_t i = 0; i < records.size(); ++i) {
        const bool keep_error = i < with_error.size() && with_error[i];
        cled_render(records[i], keep_error, text);
    }
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (written != text.size()) {
        error = "short write on " + path;
        return false;
    }
    return true;
}

} // namespace

CompactionLedger::CompactionLedger(kimix::string path) : _path(std::move(path)) {}

CompactionLedger CompactionLedger::for_session(kimix::string_view session_dir,
                                               bool enabled) {
    if (!enabled || session_dir.empty()) {
        return CompactionLedger(); // the reference's CompactionLedger(None)
    }
    const kimix::string dir =
        kimix::string(session_dir) + "/" + kLedgerCacheDir;
    std::error_code ec;
    kimix::filesystem::path fs_dir;
    if (!kimix::path_from_narrow(dir, fs_dir)) {
        return CompactionLedger();
    }
    kimix::filesystem::create_directories(fs_dir, ec);
    std::error_code dir_ec;
    const bool is_dir =
        !ec && kimix::filesystem::is_directory(fs_dir, dir_ec) && !dir_ec;
    if (!is_dir) {
        // "Cannot create compaction ledger directory ...; disabling the
        // ledger for this session" -> the no-op ledger (failure isolation).
        return CompactionLedger();
    }
    return CompactionLedger(dir + "/" + kLedgerFileName);
}

bool CompactionLedger::record_start(const CompactionRecord &record,
                                    kimix::string &error) {
    error.clear();
    if (_path.empty()) {
        return true; // no-op ledger
    }
    // record_start always renders the error key (the dataclass default None).
    kimix::string line;
    cled_render(record, /*with_error=*/true, line);
    std::FILE *f = std::fopen(_path.c_str(), "ab");
    if (f == nullptr) {
        error = "cannot open " + _path;
        return false;
    }
    const size_t written = std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);
    if (written != line.size()) {
        error = "short write on " + _path;
        return false;
    }
    return true;
}

bool CompactionLedger::record_end(kimix::string_view compaction_id,
                                  bool has_error, kimix::string_view error,
                                  bool update_summary, int64_t summary_tokens,
                                  bool update_shrank, bool shrank,
                                  kimix::string &error_out) {
    error_out.clear();
    if (_path.empty()) {
        return true;
    }
    // Which lines keep their "error" key: a line that HAD one keeps it (set or
    // cleared); a line that never had one does not gain it (the reference
    // mutates the parsed dict: rec.pop("error", None) / rec["error"] = ...).
    kimix::vector<CompactionRecord> records;
    kimix::vector<bool> with_error;
    cled_read_records(_path, records, &with_error);
    bool found = false;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].compaction_id != kimix::string_view(compaction_id)) {
            continue;
        }
        // The key is rendered exactly when this call SETS an error - the
        // reference mutates the parsed dict, so rec.pop("error", None)
        // (a successful end) removes a previously written "error": null and
        // rec["error"] = ... (a failure) adds/sets it.
        with_error[i] = has_error;
        if (has_error) {
            records[i].has_error = true;
            records[i].error.assign(error.data(), error.size());
        } else {
            records[i].has_error = false;
            records[i].error.clear();
        }
        if (update_summary) {
            records[i].summary_tokens = summary_tokens;
        }
        if (update_shrank) {
            records[i].shrank = shrank;
        }
        found = true;
        break;
    }
    if (!found) {
        // "Cannot finalize compaction {cid}: no start record in ledger {path}":
        // the file is left untouched - we never invent data for a transaction
        // we did not record.
        error_out = "no start record in ledger for compaction " +
                    kimix::string(compaction_id);
        return false;
    }
    return cled_write_records(_path, records, with_error, error_out);
}

kimix::optional<CompactionRecord> CompactionLedger::latest() const {
    kimix::vector<CompactionRecord> records;
    cled_read_records(_path, records);
    if (records.empty()) {
        return kimix::optional<CompactionRecord>();
    }
    return kimix::optional<CompactionRecord>(std::move(records.back()));
}

kimix::vector<CompactionRecord> CompactionLedger::records() const {
    kimix::vector<CompactionRecord> out;
    cled_read_records(_path, out);
    return out;
}

} // namespace kimix::agent
