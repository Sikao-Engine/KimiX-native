// agent/errors.h - Typed turn-failure taxonomy for the native agent loop.
//
// Port of the reference's typed soul exceptions (kimi_cli/soul/__init__.py:33-83)
// onto the exception-free native API: the C++ turn loop cannot throw across the
// kimix-llm boundary (kimix_enable_exception=false), so the reference's
//   MaxStepsReached(n)          ("Max number of steps reached: {n}")
//   SessionRestartRequired      ("Step N: <type> (status=...) - ...")
//   LLMNotSupported             ("LLM model '<model>' does not support ...")
// surface as TurnResult.error_kind plus the reference wording in
// TurnResult.error. The restart bookkeeping that kimi_agent_sdk._session.py
// performs around SessionRestartRequired (auto-restart bounded by
// max_session_restarts, "Connection lost ... Restarting session (attempt k/n)"
// user text) lives inside KimiSoul::turn (see soul.cpp).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// Why a turn ended without a final assistant answer. `none` means the turn
// succeeded (or was ignored as empty input). The reference-exception each kind
// ports is noted on the enumerator.
enum class TurnErrorKind : uint8_t {
    none = 0,
    // Unclassified LLM/chat failure (the historical "chat failed: ..." path;
    // ports the bare ChatProviderError surface).
    chat_failed,
    // The per-turn step cap was hit. Ports MaxStepsReached(n); the cap is
    // carried on TurnResult::max_steps and the wording is the reference's
    // "Max number of steps reached: {n}".
    max_steps_reached,
    // Step retries AND automatic session restarts are exhausted. Ports the
    // SessionRestartRequired propagation after the restart budget runs out
    // (kimi_agent_sdk._session.py: "Session restart limit reached (k/n)").
    session_restart_exhausted,
    // The model kept producing empty / think-only responses until the retry
    // budget and the x1.5 output-budget escalation ran out. Ports the
    // APIEmptyResponseError exhaustion path (kimisoul.py:1794-1806).
    empty_response_exhausted,
    // Capability pre-flight refused the request before it was sent. Ports
    // LLMNotSupported (soul/__init__.py:40-48) - today only the `thinking`
    // capability exists on the native message model.
    llm_not_supported,
    // G8: the turn was cancelled from the outside (run_soul's cancel_event /
    // RunCancelled, soul/__init__.py:200-203): the in-flight step aborted at
    // the next step boundary and its streaming request was interrupted; the
    // session/history is kept and the caller prints the reference's
    // "Keyboard Interrupt." warning.
    cancelled,
};

} // namespace kimix::agent
