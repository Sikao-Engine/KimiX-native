// cli/cli_init_wizard.h - the /init interactive provider wizard (H1).
//
// Port of kimix/cli_impl/init.py (revision 86b7bf6): the three provider
// templates (kimi / deepseek / minimax), the validated question walk (model
// name, model type, API key with env detection, context size, max tokens,
// thinking effort, capabilities, URL, optional sub-provider block), the
// merge-loaded-config-over-template defaults, the pretty-JSON save, the
// success line and the OS file reveal.
//
// Every question is read through the caller's `_input` primitive (the
// scripted-input queue first, then the real stdin), so the wizard is drivable
// from tests and from non-interactive runs.
//
// Rules (see .agents/skills/cpp): namespace kimix::cli, kimix:: containers,
// no exceptions (a Ctrl-C / EOF aborts with the reference's "keyboard
// interruped." warning and keeps the partially collected config on disk when
// the caller already had one).

#pragma once

#include <core/kimix_core.h>

#include "cli/cli_app.h"

namespace kimix::cli {

// The wizard's input source: pop the pending queue first (printing nothing),
// otherwise print `prompt` and read one line from `in`.  False on EOF.
using init_input_fn = kimix::function<bool(kimix::string_view prompt, kimix::string &line)>;

// Run the wizard.  `initialize == false` (the boot-time auto-init path) first
// asks the reference's "default config not found, initialize? you can use
// /init any time. (y/n)" gate (empty answer == yes).  Writes the merged
// config JSON (orjson OPT_INDENT_2 shape: two-space indent) to `config_path`,
// prints the reference's "Configuration saved successfully to {path}." line
// and reveals the file with the OS default application (os.startfile / open /
// xdg-open).  `open_after` false skips the reveal (tests).
// Returns false when the user aborted (Ctrl-C / EOF) - the reference prints
// "keyboard interruped." and returns without saving.
bool run_init_wizard(app_context &app, const kimix::string &config_path,
                     bool initialize, bool open_after, const init_input_fn &input);

// The kimi template (init.py:37-48 kimi_default_config): written verbatim by
// the non-TTY boot auto-init path (args.py:86-96) with the api_key filled
// from KIMI_API_KEY/KIMIX_API_KEY.
extern const char *const k_init_default_config_template;

// Reveal `path` with the OS default application (os.startfile on Windows,
// `open` on macOS, `xdg-open` elsewhere).  Best effort - never fails the
// caller (the reference wraps the call in try/except too).
void open_with_default_app(const kimix::string &path);

} // namespace kimix::cli
