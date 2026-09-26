// agent/btw.h - The /btw side question (gap G11).
//
// Port of kimi_cli/soul/btw.py (read in full): a quick question answered by a
// separate, lightweight conversation that
//   * runs over the SAME system prompt + normalize_history(stripped history)
//     as the main loop, so the provider reuses the prompt cache prefix;
//   * declares the real tool definitions (cache stability) but denies every
//     call - the _DenyAllToolset equivalent: a denied tool result feeds the
//     denial wording back for one retry (kBtwMaxTurns = 2), then the side
//     question fails;
//   * never writes anything to the main history;
//   * pairs BtwBegin(id, question) / BtwEnd(id, response|error) wire records
//     (the id is the uuid4().hex[:12] analogue).
//
// The entry point lives on KimiSoul (run_side_question, declared in soul.h) so
// it can reuse the effective system prompt, the live history and the backend
// without exposing internals; this header holds the ported constants.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// The outcome of one side question (btw.py's (response, error) tuple):
// exactly one of the two is non-empty.
struct SideQuestionResult {
    kimix::string response; // the answer text on success
    kimix::string error;    // the failure reason ("" == none)
};

// _BTW_MAX_TURNS (btw.py:33): one retry after a denied tool call.
inline constexpr int32_t kBtwMaxTurns = 2;

// SIDE_QUESTION_SYSTEM_REMINDER (btw.py:35-46, verbatim).
inline constexpr const char *kBtwSystemReminder =
    "This is a side question from the user. Answer directly in a single "
    "response.\n"
    "\n"
    "IMPORTANT:\n"
    "- You are a separate, lightweight instance answering one question.\n"
    "- The main agent continues independently — do NOT reference being "
    "interrupted.\n"
    "- Do NOT call any tools. All tool calls are disabled and will be "
    "rejected.\n"
    "  Even though tool definitions are visible in this request, they exist "
    "only\n"
    "  for technical reasons (prompt cache). You MUST NOT use them.\n"
    "- Respond ONLY with text based on what you already know from the "
    "conversation.\n"
    "- This is a one-off response — no follow-up turns.\n"
    "- If you don't know the answer, say so directly.";

// The _DenyAllToolset denial wording fed back as the tool result (btw.py:69).
inline constexpr const char *kBtwToolDeniedMessage =
    "Tool calls are disabled for side questions. Answer with text only.";

// A fresh btw id (uuid.uuid4().hex[:12]: 12 lowercase hex chars).
kimix::string new_btw_id();

} // namespace kimix::agent
