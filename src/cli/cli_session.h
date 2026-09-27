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
    // approval{yolo,afk,auto_approve_actions} (kimi_cli/session_state.py
    // ApprovalStateData). auto_approve_actions is the approve-for-session
    // grant set: the action names the user chose "always" for (G2), persisted
    // as the JSON array of names, byte-compatible with the reference's
    // set[str] field. yolo/afk are the persisted mode flags.
    bool yolo = true, afk = false;
    kimix::vector<kimix::string> auto_approve_actions;
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

// The context meta records the store persists alongside the messages
// (B5/B8/B9): kimi_cli/soul/context_records.py's SystemPromptRecord
// ({"role":"_system_prompt","content":...}), UsageRecord
// ({"role":"_usage","token_count":n}) and CheckpointRecord
// ({"role":"_checkpoint","id":n}) - in the DB backend the system_prompt /
// usage_snapshots / checkpoints tables (context_db.py).  `checkpoints` /
// `usages` are the ordered ids / snapshots of the store (the structured
// export's projection); `next_checkpoint_id` is the reference's
// Context._next_checkpoint_id (n_checkpoints), restored on resume.
struct session_meta {
    bool has_system_prompt = false;
    kimix::string system_prompt;
    bool has_usage = false;
    int64_t usage_tokens = 0; // the LATEST usage snapshot (restore_full)
    int64_t next_checkpoint_id = 0;
    kimix::vector<int64_t> checkpoints;
    kimix::vector<int64_t> usages;
};

// B9: the structured export of a session store
// (kimi_cli/soul/context_records.py::ExportedContext) - the system prompt,
// every message, every checkpoint id and every usage snapshot in one read.
struct exported_context {
    bool has_system_prompt = false;
    kimix::string system_prompt;
    kimix::vector<kimix::llm::Message> messages;
    kimix::vector<int64_t> checkpoints;
    kimix::vector<int64_t> usages;
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

    // The context history under <session dir>/context.jsonl (the reference's
    // legacy format: one Message.model_dump_json(exclude_none=True) record per
    // line) or <session dir>/context.db (the SQLite ContextDb store), selected
    // per session the way session.py does - ".db first, then .jsonl" (B4/B6):
    // a directory whose context.db exists reads and writes through ContextDb,
    // anything else uses the JSONL backend (fully working, auto-detected).
    // load_history also performs the reference's JSONL->DB auto-migration on
    // access (the JSONL is renamed to .jsonl.bak).  wire.jsonl (the protocol
    // header followed by one {"timestamp","message"} record per message - the
    // CLI's own transcript) keeps its legacy history-derived fallback write
    // when no live wire file exists.
    bool save_history(const kimix::vector<kimix::llm::Message> &h,
                      kimix::string &error) const;
    bool load_history(kimix::vector<kimix::llm::Message> &h,
                      kimix::string &error) const;

    // ── B5: meta-record write-through (context.py:303-310, 727-730, 826-831) ──
    // The meta the store holds, adopted on resume by load_history()
    // (restore_full). `checkpoints` / `usages` feed the structured export.
    const session_meta &meta() const noexcept { return _meta; }
    // The persisted system prompt, or nullopt (the reference's
    // get_system_prompt() -> str | None).
    kimix::optional<kimix::string> stored_system_prompt() const;
    // The latest persisted usage snapshot, or nullopt (get_latest_usage).
    kimix::optional<int64_t> last_usage() const;
    // Writes a system-prompt meta record ONCE PER SESSION, content-addressed:
    // a no-op when the content is unchanged (prefix-cache continuity - the
    // reference's set_system_prompt prepends a _system_prompt line /
    // upserts the singleton row; skipping an unchanged write keeps the store
    // byte-stable). Updates meta().
    bool set_system_prompt(kimix::string_view content, kimix::string &error);
    // Appends a usage meta record after each token-ledger update (the
    // reference's record_usage). Updates meta().
    bool record_usage(int64_t token_count, kimix::string &error);

    // ── B8: checkpoints (context.py:777-820 + context_db.py:732-787) ──────────
    // The id the next checkpoint gets (Context.n_checkpoints).
    int64_t n_checkpoints() const noexcept { return _meta.next_checkpoint_id; }
    // Context.checkpoint: records the checkpoint in the store and, when
    // `add_user_message`, appends the synthetic
    // <system>CHECKPOINT n</system> user record to BOTH the given history and
    // the store (so the caller's in-memory history stays in sync with the
    // persisted one). `checkpoint_id` receives the id used.
    bool create_checkpoint(kimix::vector<kimix::llm::Message> &h,
                           bool add_user_message, int64_t &checkpoint_id,
                           kimix::string &error);
    // Context.revert_to: rewinds the store to just before the checkpoint
    // record, then restores `h` from the store (with the stale
    // <system-reminder> strip). An unknown id fails with the reference's
    // "Checkpoint {id} does not exist".
    bool revert_to(int64_t checkpoint_id,
                   kimix::vector<kimix::llm::Message> &h, kimix::string &error);

    // ── B12: replace_history / restore guard / backend detect ────────────────
    // Context.replace_history (context.py:748-771): atomically clear + rewrite
    // the persisted history (system prompt kept, messages replaced,
    // checkpoints and usage records reset). `token_count` is the caller's
    // recorded token count (the reference's self._token_count), updated in
    // place: the estimate can only LOWER it - a higher value is left for the
    // next API usage update to correct (the reference's only-lower rule).
    // Pass nullopt to skip the token-count adjustment.
    bool replace_history(const kimix::vector<kimix::llm::Message> &h,
                         kimix::optional<int64_t> new_token_estimate,
                         int64_t &token_count, kimix::string &error);
    // Context.restore (context.py:685-721) with its mutation guard: refuses
    // with the reference's "The context storage is already modified" when
    // this store already wrote (or restored) the context in this process -
    // a save after an external mutation is refused, never silently merged.
    // On success the restored meta is adopted exactly like load_history.
    bool restore_history(kimix::vector<kimix::llm::Message> &h,
                         kimix::string &error);
    // Backend auto-detect for a session directory: the context file this
    // store reads/writes - context.db when present (the reference's
    // ".db first" suffix rule), else context.jsonl, else "" (an empty
    // session). When BOTH exist the newer file wins (a directory left with
    // both by an interrupted migration keeps its latest state); the reference
    // resolves purely by suffix, so this only differs on that edge case.
    static kimix::string detect_context_file(const kimix::string &session_dir);

    // ── B9: structured export (context_records.py ExportedContext) ───────────
    // system_prompt + messages + checkpoints + usages in one read (both
    // backends). The markdown export itself goes through the byte-exact
    // runtime export builder (src/runtime/tools/export_builder.h).
    bool export_context(exported_context &out, kimix::string &error) const;

    // Record the current context usage so list() and /context can use it.
    void set_usage(double ratio, int64_t tokens, bool known = true);
    // The usage recorded by the last set_usage() (state.json's context_usage /
    // context_tokens).  `known == false` mirrors the reference's
    // usage-unknown (-1.0) sentinel; /clear and /reflection use this as the
    // recorded-usage tier of the empty-context check.
    void usage(double &ratio, int64_t &tokens, bool &known) const;
    // True when the persisted context store holds at least one record (a
    // non-empty context.jsonl or a context.db file) - the wire/db/jsonl tier
    // of the reference's Session.is_empty().
    bool has_context_records() const;

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
    // B5/B8: the context meta adopted from the store (load_history /
    // restore_history) and updated by the meta writers.  Mutable so the
    // const read path (load_history) can adopt what it loaded.
    mutable session_meta _meta;
    // B12: true once this process wrote (or restored) the context through
    // this store - the reference's "already modified" guard input.  Mutable
    // so the const write path (save_history) can raise it.
    mutable bool _mutated = false;
};

} // namespace kimix::cli
