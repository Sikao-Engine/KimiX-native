// error_log.cpp - Reflection-mode tool-error log (see error_log.h).
//
// Unity build: every TU-local helper lives in an anonymous namespace with the
// `el_` prefix.

#include "builtin_tools/error_log.h"

#include <cstdio>
#include <mutex>

#include <mimalloc.h>
#include <yyjson.h>

#include "llm/yyjson_alc.h" // kimix::llm::kYYJsonAlcMi (mimalloc-backed)

#include <core/spin_mutex.h>

namespace kimix::builtin_tools {

namespace {

// Path-hostile characters of a session id replaced by '_' (the id comes from
// the session store, but a hand-rolled id must not escape the log directory).
kimix::string el_file_stem(kimix::string_view session_id) {
    kimix::string stem;
    if (session_id.empty()) {
        return kimix::string("default");
    }
    stem.reserve(session_id.size());
    for (char c : session_id) {
        stem.push_back((c == '/' || c == '\\' || c == ':' || c == '?' ||
                        c == '*' || c == '"' || c == '<' || c == '>' ||
                        c == '|')
                           ? '_'
                           : c);
    }
    if (stem.empty()) {
        return kimix::string("default");
    }
    return stem;
}

// Builds one JSONL line (a compact JSON object + '\n') from the record. The
// arguments are embedded as PARSED JSON when they parse to a value, so the
// log stays queryable; anything else degrades to the raw string. "" on a
// serialization failure.
kimix::string el_build_line(const tool_error_record &record) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    if (doc == nullptr) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strncpy(doc, root, "tool", record.tool.data(),
                               record.tool.size());
    yyjson_doc *args_doc = yyjson_read_opts(
        const_cast<char *>(record.arguments.data()), record.arguments.size(),
        YYJSON_READ_STOP_WHEN_DONE, &kimix::llm::kYYJsonAlcMi, nullptr);
    if (args_doc != nullptr) {
        yyjson_val *args = yyjson_doc_get_root(args_doc);
        yyjson_mut_val *copy =
            args != nullptr ? yyjson_val_mut_copy(doc, args) : nullptr;
        yyjson_doc_free(args_doc);
        yyjson_mut_obj_add_val(doc, root, "arguments",
                               copy != nullptr ? copy
                                               : yyjson_mut_null(doc));
    } else {
        yyjson_mut_obj_add_strncpy(doc, root, "arguments",
                                   record.arguments.data(),
                                   record.arguments.size());
    }
    yyjson_mut_obj_add_real(doc, root, "elapsed_ms", record.elapsed_ms);
    yyjson_mut_obj_add_strncpy(doc, root, "message", record.message.data(),
                               record.message.size());
    yyjson_mut_obj_add_strncpy(doc, root, "output", record.output.data(),
                               record.output.size());

    size_t len = 0;
    // The write buffer comes from the mimalloc allocator (mi_free contract);
    // compact, then the caller's "\n" terminates the JSONL line.
    char *json = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG /* compact */,
                                       &kimix::llm::kYYJsonAlcMi, &len,
                                       nullptr);
    yyjson_mut_doc_free(doc);
    if (json == nullptr) {
        return {};
    }
    kimix::string line;
    line.reserve(len + 1);
    line.append(json, len);
    line.push_back('\n');
    mi_free(json);
    return line;
}

// Serializes whole-line appends: two parallel-dispatch workers must not
// interleave bytes of one record within the file.
kimix::spin_mutex &el_write_mutex() {
    static kimix::spin_mutex mutex;
    return mutex;
}

} // namespace

void tool_error_log_append(kimix::string_view work_dir,
                           kimix::string_view session_id,
                           const tool_error_record &record) {
    const kimix::string line = el_build_line(record);
    if (line.empty()) {
        return;
    }
    namespace fs = kimix::filesystem;
    std::error_code ec;
    const fs::path root =
        !work_dir.empty() ? fs::path(kimix::string(work_dir)) : fs::path(".");
    const fs::path dir = root / ".kimix_cache" / "error_log";
    fs::create_directories(dir, ec);
    if (ec) {
        return;
    }
    const kimix::string path =
        kimix::to_string(dir / fs::path(el_file_stem(session_id) + ".jsonl"));
    std::lock_guard<kimix::spin_mutex> guard(el_write_mutex());
    std::FILE *f = std::fopen(path.c_str(), "ab");
    if (f == nullptr) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);
}

} // namespace kimix::builtin_tools
