// cli/cli_session.h - the native CLI's session store (src/cli/PLAN.md §3.6).
//
// Port of the session layer the Python CLI sits on:
//   * kimi_cli/session.py            (Session.create / find / list / copy, dir layout)
//   * kimi_cli/session_state.py      (state.json + load/save_session_state)
//   * kimi_cli/metadata.py           (KIMIX_CACHE_DIR_NAME = ".kimix_cache")
//   * kimi_cli/utils/io.py           (atomic_json_write: tmp file + os.replace)
//   * kimi_cli/soul/context.py       (JsonlContextStorage: context.jsonl records)
//   * kimi_cli/wire/file.py          (wire.jsonl header + {timestamp,message} records)
//   * kimi_cli/utils/export.py       (build_export_markdown, reduced - see below)
//   * kimix/utils/session.py         (create_session / close_session semantics)
//   * kimix/utils/_globals.py        (_cli_sessions scan: title / updated_at)
//
// Layout (reference-compatible): cache root <work_dir>/.kimix_cache, one
// directory per session named exactly by its id (random_hex(16) = 32 hex chars,
// the shape of Python's uuid4().hex; a user-supplied name is used verbatim),
// holding state.json, context.jsonl and wire.jsonl.
//
// Rules (see src/cli/PLAN.md and .agents/skills/cpp): namespace kimix::cli,
// kimix:: containers in every public API, no RTTI, no exceptions (failures
// travel through bool + `error` out-parameters), JSON through the vendored
// yyjson with the mimalloc allocator (kimix::llm::kYYJsonAlcMi; writer buffers
// are released with mi_free(), never free()), unity build (every TU-local
// helper is static / anonymous with the `clis_` prefix, no file-scope
// `using namespace`).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/llm.h"

namespace kimix::cli {

// <session_dir>/state.json (kimi_cli/session_state.py::SessionState).
//
// The keys the Python model declares but the native CLI does not model
// (version, wire_mtime, archived_at, archived_todos, todo_stack) and any
// unknown key a Python writer added are read back verbatim from the existing
// file by save_state(), so a Python-written state.json survives a native
// rewrite of the modelled fields.
struct session_state {
    kimix::string custom_title; // "" == the reference's None -> JSON null
    bool title_generated = false;
    int32_t title_generate_attempts = 0;
    // approval{yolo,afk,auto_approve_actions}.  The reference types
    // auto_approve_actions as set[str] (action names owned by Python); the
    // native flag is a bool, so it is never written as a JSON bool — the
    // on-disk array is preserved ([] when absent).
    bool yolo = true, afk = false, auto_approve_actions = false;
    kimix::vector<kimix::string> additional_dirs;
    bool archived = false, auto_archive_exempt = false;
    // Raw `todos` array written verbatim (semantically: parsed and re-emitted,
    // which keeps member order and unknown item keys) as state.json's "todos"
    // member, so S5 can hand the todo tool's serialized list straight through.
    // Must be a JSON array; save_state() fails with an explicit error when it
    // is not (default "[]").
    kimix::string todos_json = "[]";
};

// One row of session_store::list() (the /sessions table).
struct session_info {
    kimix::string id, title;
    int64_t updated_at = 0; // unix seconds
    double context_usage = 0.0;
    int64_t context_tokens = 0;
    // True when state.json carried context_usage/context_tokens (the native
    // store records them there via set_usage/save_state); false means "usage
    // unknown" and mirrors the reference's -1.0 cache default.
    bool usage_known = false;
};

// The session store: one open session at a time (the CLI's "current session").
// Nothing is saved implicitly - the caller saves state/history first, exactly
// like the reference's /exit path.
class session_store {
public:
    session_store();

    // Open (and create) a session.  `id` empty -> a fresh random anonymous id;
    // otherwise the id is used verbatim (named session).  `resume == false`
    // resets the directory contents the store owns (state.json, context.jsonl,
    // wire.jsonl); `resume == true` reopens an existing directory, creating it
    // when absent (the reference's Session.create/find split).  Never touches
    // another session's files or unrelated files of the same directory.
    bool open(kimix::string_view work_dir, kimix::string_view id, bool resume,
              kimix::string &error);

    const kimix::string &id() const { return _id; }
    const kimix::string &dir() const { return _dir; }
    const kimix::string &work_dir() const { return _work_dir; }
    bool anonymous() const { return _anonymous; }

    // state.json.  A missing file loads the reference defaults and returns
    // true; an unreadable or non-object file returns false with `error` set
    // (the reference silently falls back to defaults - the native API reports
    // it so the caller can decide).
    bool load_state(session_state &out, kimix::string &error) const;
    // Atomic (state.json.tmp + replace; the target is removed first when the
    // platform refuses the replacing rename, as on Windows).  Preserves every
    // key it does not model, including unknown ones.
    bool save_state(const session_state &st, kimix::string &error) const;

    // context.jsonl (the reference's legacy history format: one
    // Message.model_dump_json(exclude_none=True) record per line) plus
    // wire.jsonl (the protocol header followed by one {"timestamp","message"}
    // record per message - the CLI's own transcript).  save_history rewrites
    // both files; load_history reads context.jsonl back.  A session whose
    // history lives only in context.db (SQLite) returns false with an explicit
    // error naming the limitation (PLAN.md §5.4) instead of an empty history.
    bool save_history(const kimix::vector<kimix::llm::Message> &h,
                      kimix::string &error) const;
    bool load_history(kimix::vector<kimix::llm::Message> &h,
                      kimix::string &error) const;

    // Record the current context usage so list() and /context can use it.
    void set_usage(double ratio, int64_t tokens, bool known = true);

    // /store:<id>: copy this session's directory to <cache_root>/<new_id>
    // (fails when the target exists, like shutil.copytree); this session stays
    // open and current.
    bool store_as(kimix::string_view new_id, kimix::string &error);
    // /load:<id>: copy the *named* existing session into this store's
    // directory (which is/becomes a fresh anonymous one).  The current files
    // the store owns are cleared first; this store's id/directory are kept.
    bool copy_into(kimix::string_view new_id, kimix::string &error);

    // Save nothing implicitly; release what is held and delete the session
    // directory when `delete_if_anonymous && anonymous()` (the reference's
    // Session.close).
    bool close(bool delete_if_anonymous, kimix::string &error);

    // /clear: drop context.jsonl / wire.jsonl / context.db / state.json and the
    // recorded usage, keeping the directory and the id.  (The reference's
    // Session.clear deletes state.json too, which is also what clears the
    // persisted usage; the caller re-saves its in-memory state when it wants
    // it back.)
    bool clear_context(kimix::string &error);

    // /export: markdown export.  A reduced build_export_markdown: the
    // reference front-matter keys, "# Kimi Session Export", the Overview block
    // and one heading + body section per message (no Turn grouping).  An empty
    // `path` writes <session_dir>/export_<YYYYMMDD-HHMMSS>.md; a relative path
    // resolves against the work directory.  An empty history fails with the
    // reference's "No messages to export.".
    bool export_markdown(const kimix::vector<kimix::llm::Message> &h,
                         kimix::string_view path, kimix::string &error) const;

    // Scan <work_dir>/.kimix_cache/*/ (directories only): id = directory name,
    // title = custom_title else "Untitled", updated_at = max mtime of
    // state.json / context.db / context.jsonl (0 when none), context usage from
    // state.json when present.  Sorted by updated_at descending, ties by id
    // ascending (deterministic).
    static kimix::vector<session_info> list(const kimix::string &work_dir);

    // <work_dir>/.kimix_cache (the reference's KIMIX_CACHE_DIR_NAME).
    static kimix::string cache_root(const kimix::string &work_dir);
    // <work_dir>/.kimix_cache/<id>.
    static kimix::string session_dir(const kimix::string &work_dir,
                                     kimix::string_view id);

private:
    kimix::string _work_dir; // absolute, lexically normalised
    kimix::string _id;
    kimix::string _dir;
    bool _anonymous = false;
    bool _open = false;
    double _usage = 0.0;
    int64_t _usage_tokens = 0;
    bool _usage_known = false;
};

} // namespace kimix::cli
