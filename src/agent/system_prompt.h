// agent/system_prompt.h - system prompt builder: a byte-faithful C++ port of
// kimi-agent's kimix/utils/system_prompt.py (get_system_prompt's returned
// closure). Given the same inputs it produces byte-identical text.
//
// Scope notes vs. the Python reference:
//   * The reference resolves the shell tool name from the enablement logic in
//     kimix.tools.file.bash (config-aware import-time detection). The C++ port
//     takes it as a plain input field (`shell_tool`) that the caller sets; the
//     default "bash" preserves the reference behaviour on bash-enabled setups.
//   * The reference reads <work_dir>/AGENTS.md itself and applies a token
//     budget (max_system_prompt_tokens) via a tokenizer. The C++ port takes
//     the pre-read AGENTS.md content as an input (caller reads the file) and
//     applies the byte-length > 4096 rule only; the token-budget fallback is
//     not ported (kept default-off, like the compact-export fields).

#pragma once

#include <core/kimix_core.h>

namespace kimix::agent {

enum class system_prompt_role {
    worker,
    todomaker,
    thinker,
    trivial_sub_agent,
    reader,
    supervisor,
    swarm_leader,
};

// Inputs of one build_system_prompt() call, mirroring the Python closure's
// captured state (yolo, work_dir/agent_md) and per-call args (runtime builtin
// args, is_sub_agent via the role, compact_export_path).
struct system_prompt_input {
    system_prompt_role role = system_prompt_role::worker;
    kimix::string os;          // args.KIMI_OS, e.g. "Windows" / "Linux" / "macOS"
    kimix::string work_dir;    // str(args.KIMI_WORK_DIR)
    bool yolo = true;          // base._default_yolo when unset in the reference
    bool is_sub_agent = false; // worker_logic's is_sub_agent (SUBAGENT clause only)
    kimix::string skills_text; // pre-formatted KIMI_SKILLS block, may be empty
    kimix::string agents_md;   // pre-read AGENTS.md content, may be empty
    bool compact_export_pending = false;// plumbed for the compaction flow; default-off
    kimix::string compact_export_path;  // non-empty -> extra "Pre-compaction ..." item
    // Shell tool name for sp_worker_core's {shell_tool} substitution. The
    // Python picks it from the bash/powershell enablement logic; here the
    // caller decides (see header comment).
    kimix::string shell_tool = "bash";
};

// Build the full system prompt for `in`. Byte-faithful port of the Python
// closure body (template assembly + final str.strip()).
kimix::string build_system_prompt(const system_prompt_input &in);

// True for the roles whose prompt embeds AGENTS.md (every role except reader).
// Lets the caller decide whether to read <work_dir>/AGENTS.md from disk.
bool system_prompt_role_uses_agent_md(system_prompt_role role) noexcept;

} // namespace kimix::agent
