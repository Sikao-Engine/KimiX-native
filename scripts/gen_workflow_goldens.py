#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/workflow_goldens.inc.

The workflow tool (``kimix.tools.swarm.AgentSwarm``) and its best-of-N
machinery (``kimix.tools.swarm.best_of_n``) live in the kimi-agent checkout
and are the ground truth for the C++ port in
``src/builtin_tools/workflow_tool.{h,cpp}``.  This script drives the *real*
Python implementation against stub sessions / runners / workspaces and records
every byte the port has to reproduce:

  * tool-level cases: the raw JSON arguments the model would send -> the
    ``ToolOk`` / ``ToolError`` envelope (is_error / message / brief / output)
    plus the exact list of prompts that were dispatched to the sub-agents
    (which pins the template expansion, the per-step prompt assembly, the
    resume ordering, the retry loop and the best-of-N selector review text);
  * kernel-level cases: ``_expand_template``, ``_validate_uniqueness``,
    ``_xml_escape``, ``_render_results``, the ``<best_of_n_result>`` renderer,
    ``is_rate_limit_error``, the retry backoff, the ``_RateLimiter`` token
    bucket, ``format_candidates_for_review``, the error-message helpers,
    ``select_best_candidate`` (self-eval / majority / tie-breaks / fallback),
    ``run_parallel_sample`` and ``best_of_n``.

The C++ port is replayed over the same inputs by
``tests/unit/builtin_tools/test_workflow_tool.cpp`` and must reproduce every
column.

Everything LLM-facing is stubbed exactly where the Python module makes it
injectable (``utils.prompt_async`` for a sub-agent turn, the best-of-N
workspace helpers for the worker workspaces), so the corpus is offline and
deterministic: the swarm module's ``time`` is replaced with a fixed-step clock
so the ``elapsed`` attribute of every sub-agent is reproducible, and the
recorded elapsed values are replayed into the C++ runner.

Usage::

    python scripts/gen_workflow_goldens.py            # rewrite the .inc
    python scripts/gen_workflow_goldens.py --check    # fail if out of date

``--python`` points at the kimi-agent virtualenv interpreter when the current
interpreter cannot import ``kimix``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_AGENT_PYTHON = KIMI_AGENT_ROOT / ".venv" / "Scripts" / "python.exe"
OUT_PATH = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "workflow_goldens.inc"

# Reuse the byte-exact C string-literal emitter of the todo goldens.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_todo_goldens import c_literal  # noqa: E402


def _ensure_reference_on_path() -> None:
    # "src" must end up *first*: kimi-cli/src/kimix is a shim package that would
    # otherwise shadow the real kimix (see python/tests/_parity_ref.py).
    for rel in ("kimi-cli/src", "src"):
        p = KIMI_AGENT_ROOT / rel
        if not p.is_dir():
            continue
        s = str(p)
        while s in sys.path:
            sys.path.remove(s)
        sys.path.insert(0, s)


def _import_reference():
    _ensure_reference_on_path()
    import kimix.utils as utils  # noqa: PLC0415
    import kimix.tools.swarm as swarm  # noqa: PLC0415
    import kimix.tools.swarm.best_of_n as bon  # noqa: PLC0415
    from kimix.ui.printing import MessageType  # noqa: PLC0415

    return {
        "utils": utils,
        "swarm": swarm,
        "bon": bon,
        "MessageType": MessageType,
    }


# ---------------------------------------------------------------------------
# Stub session / clock / one-shot rule engine
# ---------------------------------------------------------------------------


class _Session:
    def __init__(self, name: str = "main") -> None:
        self.session_id = name
        self.custom_config: dict = {}
        self.custom_data: dict = {}
        self.work_dir = "."


class _Clock:
    """Deterministic replacement for the swarm module's ``time``."""

    def __init__(self) -> None:
        self.now = 1000.0
        self.step = 0.35

    def monotonic(self) -> float:
        value = self.now
        self.now += self.step
        return value


class Rules:
    """Prompt -> scripted outcome, mirroring the C++ replay runner.

    ``groups`` is a list of ``(match, steps)``.  A prompt is served by the
    first group whose ``match`` is a substring of the prompt ("" matches
    anything, so it is used as the catch-all).  Each group consumes its steps
    in order and then keeps repeating the last one.
    """

    def __init__(self, rules: list[dict]) -> None:
        self.groups = [(r.get("match", ""), list(r["steps"])) for r in rules]
        self.used = [0] * len(self.groups)

    def step_for(self, prompt: str) -> dict:
        for i, (match, steps) in enumerate(self.groups):
            if match in prompt:
                pos = min(self.used[i], len(steps) - 1)
                self.used[i] += 1
                return steps[pos]
        return {"kind": "echo"}


_PICK_B = re.compile(r"Candidate B \(#(\d+)\)")


def _render(kind: str, text: str, prompt: str) -> str:
    return text.replace("{prompt}", prompt)


def _outcome(step: dict, prompt: str):
    """Return ("ok", text) / ("fail", message) / ("none", "") for the stub."""
    kind = step["kind"]
    if kind == "echo":
        return "ok", "report for " + prompt
    if kind == "text":
        return "ok", _render(kind, step.get("text", ""), prompt)
    if kind == "silent":
        return "none", ""
    if kind == "pick_b":
        match = _PICK_B.search(prompt)
        return "ok", match.group(1) if match else "0"
    if kind == "fail":
        return "fail", _render(kind, step.get("text", ""), prompt)
    raise AssertionError("unknown step kind %r" % kind)


def _effective_steps(rules: list[dict], path: str) -> list[dict]:
    """Translate a corpus rule list into the golden's runner script.

    ``path`` is "fanout" (``swarm._run_subagent_task``: the failure text is
    ``str(exc)``) or "sample" (``AgentSwarm._sample_runner``: the candidate
    error is ``f"{type(exc).__name__}: {exc}"``).
    """
    out: list[dict] = []
    for rule in rules:
        steps = []
        for i, step in enumerate(rule["steps"]):
            row = {
                "kind": step["kind"],
                "text": step.get("text", ""),
                "seq": i,
                "match": rule.get("match", ""),
            }
            if step["kind"] == "fail" and path == "sample":
                row["text"] = "RuntimeError: " + row["text"]
            steps.append(row)
        out.extend(steps)
    return out


# ---------------------------------------------------------------------------
# Tool-level corpus
# ---------------------------------------------------------------------------
# Each case: (name, args(dict), notes)
#   swarm_session / sub_agent  - the session flags handed to the C++ tool
#   rules                      - scripted sub-agent outcomes (see Rules)
#   path                       - "fanout" | "sample" (error-text formatting)
#   expect_status              - tool_status string the C++ port must report
#                                (Python has no analogue: ToolOk/ToolError)
#   expect_message_override    - the C++ wording, when it deliberately follows
#                                the project's pydantic-message convention
#                                instead of the pydantic text verbatim


def tool_corpus() -> list[dict]:
    C: list[dict] = []
    add = C.append

    # ---- fanout: happy paths ---------------------------------------------
    add(dict(
        name="fanout_template_echo",
        args={"description": "audit <all> the things & stuff",
              "prompt_template": "Fix errors in {{item}}.",
              "items": ["a.py", "b\"c.py", "d'e.py"],
              "subagent_type": "explore"},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="fanout_prefix_suffix",
        args={"description": "migrate",
              "prompt_prefix": "Please fix: ",
              "prompt_suffix": " (keep the diff small)",
              "items": ["one.py", "two.py"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="fanout_template_repeated_placeholder",
        args={"description": "twice",
              "prompt_template": "A {{item}} B {{item}}",
              "items": ["x", "y"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="fanout_silent_subagent",
        args={"description": "no text output",
              "prompt_template": "quiet {{item}}",
              "items": ["a", "b"]},
        rules=[{"match": "", "steps": [{"kind": "silent"}]}],
    ))
    add(dict(
        name="fanout_unicode_passthrough",
        args={"description": "caf\u00e9 \u2713 <ok>",
              "prompt_template": "look at {{item}}",
              "items": ["\u6a21\u5757/a.py", "b\u00e9.py"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))

    # ---- fanout: failures / partial failure ------------------------------
    add(dict(
        name="fanout_partial_failure",
        args={"description": "fix the builds",
              "prompt_template": "Fix errors in {{item}}.",
              "items": ["a.cpp", "b.cpp", "c.cpp"],
              "subagent_type": "plan"},
        rules=[
            {"match": "b.cpp", "steps": [
                {"kind": "fail", "text": "compile error on {prompt}"}]},
            {"match": "", "steps": [{"kind": "echo"}]},
        ],
    ))
    add(dict(
        name="fanout_all_failed_escapes",
        args={"description": "why <not>",
              "prompt_template": "do {{item}}",
              "items": ["a&b", "c<d"]},
        rules=[{"match": "", "steps": [
            {"kind": "fail", "text": "boom <&> \"quote\" on {prompt}"}]}],
    ))

    # ---- fanout: resume ---------------------------------------------------
    add(dict(
        name="fanout_resume_ids",
        args={"description": "resume the failed ones",
              "items": ["fresh-a", "fresh-b"],
              "prompt_template": "process {{item}}",
              "resume_agent_ids": {"agent-1": "retry prompt one",
                                   "agent-2": "retry prompt two"}},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="fanout_single_item_with_resume",
        args={"description": "one new one resumed",
              "items": ["only"],
              "prompt_prefix": "work on ",
              "resume_agent_ids": {"agent-9": "resumed prompt"}},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))

    # ---- fanout: rate-limit retry loop ------------------------------------
    add(dict(
        name="fanout_retry_rate_limit",
        args={"description": "retry once",
              "prompt_template": "task {{item}}",
              "items": ["alpha", "beta"]},
        rules=[
            {"match": "alpha", "steps": [
                {"kind": "fail", "text": "429 too many requests on {prompt}"},
                {"kind": "echo"}]},
            {"match": "", "steps": [{"kind": "echo"}]},
        ],
    ))
    add(dict(
        name="fanout_retry_exhausted",
        args={"description": "always throttled",
              "prompt_template": "task {{item}}",
              "items": ["gamma", "delta"]},
        rules=[
            {"match": "gamma", "steps": [
                {"kind": "fail", "text": "rate limit exceeded"}]},
            {"match": "", "steps": [{"kind": "echo"}]},
        ],
    ))
    add(dict(
        name="fanout_no_retry_without_marker",
        args={"description": "plain failure",
              "prompt_template": "task {{item}}",
              "items": ["eps", "zeta"]},
        rules=[
            {"match": "eps", "steps": [
                {"kind": "fail", "text": "plain failure on {prompt}"}]},
            {"match": "", "steps": [{"kind": "echo"}]},
        ],
    ))

    # ---- fanout: invalid swarms ------------------------------------------
    add(dict(
        name="fanout_duplicate_prompts",
        args={"description": "dupes",
              "prompt_prefix": "fix ",
              "items": ["same", "same"]},
        expect_status="invalid_input",
        expect_message_override=(
            "Expanded prompts must be unique; duplicates: {'fix same'}"),
        rules=[],
    ))
    add(dict(
        name="fanout_duplicate_prompts_three",
        args={"description": "dupes",
              "prompt_template": "fix {{item}}",
              "items": ["a", "b", "a", "b"]},
        expect_status="invalid_input",
        expect_message_override=(
            "Expanded prompts must be unique; duplicates: {'fix a', 'fix b'}"),
        rules=[],
    ))

    # ---- parallel_sample (best-of-N) --------------------------------------
    add(dict(
        name="parallel_sample_self_eval",
        args={"description": "best of three",
              "mode": "parallel_sample",
              "prompt_template": "Refactor {{item}} carefully.",
              "sample_n": 3},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "2"}]},
            {"match": "", "steps": [{"kind": "text", "text": "sample {prompt}"}]},
        ],
    ))
    add(dict(
        name="parallel_sample_default_n",
        args={"description": "default sample count",
              "mode": "parallel_sample",
              "prompt_prefix": "do the thing"},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": " index 1 "}]},
            {"match": "", "steps": [{"kind": "text", "text": "done"}]},
        ],
    ))
    add(dict(
        name="parallel_sample_selector_fallback",
        args={"description": "invalid selector index",
              "mode": "parallel_sample",
              "prompt_template": "task",
              "sample_n": 3},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "99"}]},
            {"match": "", "steps": [
                {"kind": "text", "text": "ok {prompt}"},
                {"kind": "fail", "text": "sample exploded"},
                {"kind": "text", "text": "ok {prompt}"}]},
        ],
    ))
    add(dict(
        name="parallel_sample_majority",
        args={"description": "majority vote",
              "mode": "parallel_sample",
              "selector": "majority",
              "prompt_template": "solve it",
              "sample_n": 4},
        path="sample",
        rules=[
            {"match": "=== Candidate A (#", "steps": [{"kind": "pick_b"}]},
            {"match": "", "steps": [{"kind": "text", "text": "cand {prompt}"}]},
        ],
    ))
    add(dict(
        name="parallel_sample_majority_selector_mix",
        args={"description": "mixed votes",
              "mode": "parallel_sample",
              "selector": "majority",
              "prompt_prefix": "solve ",
              "sample_n": 3},
        path="sample",
        rules=[
            {"match": "=== Candidate A (#", "steps": [
                {"kind": "pick_b"},
                {"kind": "text", "text": "0"}]},
            {"match": "", "steps": [{"kind": "text", "text": "cand {prompt}"}]},
        ],
    ))
    add(dict(
        name="parallel_sample_single_run",
        args={"description": "n=1",
              "mode": "parallel_sample",
              "prompt_prefix": "single",
              "sample_n": 1},
        path="sample",
        rules=[{"match": "", "steps": [{"kind": "text", "text": "only"}]}],
    ))
    add(dict(
        name="parallel_sample_single_run_failed",
        args={"description": "n=1 fails",
              "mode": "parallel_sample",
              "prompt_prefix": "single",
              "sample_n": 1},
        path="sample",
        expect_status="external_library",
        rules=[{"match": "", "steps": [
            {"kind": "fail", "text": "worker blew up"}]}],
    ))
    add(dict(
        name="parallel_sample_all_failed",
        args={"description": "everything fails",
              "mode": "parallel_sample",
              "prompt_template": "task",
              "sample_n": 2},
        path="sample",
        expect_status="external_library",
        rules=[{"match": "", "steps": [
            {"kind": "fail", "text": "nope {prompt}"}]}],
    ))
    add(dict(
        # Regression: the failure text contains the word "verification", which
        # must NOT be mistaken for a VerificationRejectedError.
        name="parallel_sample_all_failed_looks_like_verification",
        args={"description": "misleading wording",
              "mode": "parallel_sample",
              "prompt_template": "task",
              "sample_n": 2},
        path="sample",
        expect_status="external_library",
        rules=[{"match": "", "steps": [
            {"kind": "fail", "text": "verification harness crashed"}]}],
    ))
    add(dict(
        name="parallel_sample_empty_sample_n",
        args={"description": "zero samples",
              "mode": "parallel_sample",
              "prompt_template": "task",
              "sample_n": 0},
        expect_status="invalid_input",
        rules=[],
    ))

    # ---- parameter validation (pure pydantic + the tool's _validate) ------
    add(dict(
        name="params_missing_description",
        args={"prompt_template": "do {{item}}", "items": ["a", "b"]},
        expect_status="invalid_input",
        expect_message_override="missing required field: description",
        rules=[],
    ))
    add(dict(
        name="params_mode_null",
        args={"description": "d", "mode": None, "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=None)"),
        rules=[],
    ))
    add(dict(
        name="params_mode_wrong_type",
        args={"description": "d", "mode": 7, "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=7)"),
        rules=[],
    ))
    add(dict(
        name="params_mode_bogus",
        args={"description": "d", "mode": "bogus", "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=bogus)"),
        rules=[],
    ))
    add(dict(
        name="params_selector_wrong_type",
        args={"description": "d", "mode": "parallel_sample", "selector": 7,
              "prompt_template": "do it"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'self_eval' or 'majority' (selector=7)"),
        rules=[],
    ))
    add(dict(
        name="params_selector_bogus",
        args={"description": "d", "mode": "parallel_sample", "selector": "nope",
              "prompt_template": "do it"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'self_eval' or 'majority' (selector=nope)"),
        rules=[],
    ))
    add(dict(
        name="params_items_null",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": None},
        expect_status="invalid_input",
        expect_message_override="items must be a list of strings",
        rules=[],
    ))
    add(dict(
        name="params_items_not_a_list",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": "ab"},
        expect_status="invalid_input",
        expect_message_override="items must be a list of strings",
        rules=[],
    ))
    add(dict(
        name="params_items_bad_element",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", 2]},
        expect_status="invalid_input",
        expect_message_override="items must be a list of strings",
        rules=[],
    ))
    add(dict(
        name="params_subagent_type_not_a_string",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"], "subagent_type": 5},
        expect_status="invalid_input",
        expect_message_override="subagent_type must be a string",
        rules=[],
    ))
    add(dict(
        name="params_subagent_type_null",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"], "subagent_type": None},
        expect_status="invalid_input",
        expect_message_override="subagent_type must be a string",
        rules=[],
    ))
    add(dict(
        name="params_description_not_a_string",
        args={"description": 5, "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        expect_status="invalid_input",
        expect_message_override="description must be a string",
        rules=[],
    ))
    add(dict(
        name="params_prompt_template_not_a_string",
        args={"description": "d", "prompt_template": 5, "items": ["a", "b"]},
        expect_status="invalid_input",
        expect_message_override="prompt_template must be a string",
        rules=[],
    ))
    add(dict(
        name="params_sample_n_string",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": "3"},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "0"}]},
            {"match": "", "steps": [{"kind": "text", "text": "ok"}]},
        ],
    ))
    add(dict(
        name="params_sample_n_integral_float",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": 2.0},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "1"}]},
            {"match": "", "steps": [{"kind": "text", "text": "ok"}]},
        ],
    ))
    add(dict(
        name="params_sample_n_fractional",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": 2.5},
        expect_status="invalid_input",
        expect_message_override="sample_n must be an integer",
        rules=[],
    ))
    add(dict(
        name="params_sample_n_bool",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": True},
        path="sample",
        rules=[{"match": "", "steps": [{"kind": "text", "text": "single"}]}],
    ))
    add(dict(
        name="params_sample_n_unparsable",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": "x"},
        expect_status="invalid_input",
        expect_message_override="sample_n must be an integer",
        rules=[],
    ))
    add(dict(
        name="params_sample_n_negative_in_fanout_is_ignored",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"], "sample_n": 0},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="params_fanout_needs_two_items",
        args={"description": "d", "prompt_prefix": "p", "items": ["only"]},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_fanout_template_and_prefix",
        args={"description": "d", "prompt_template": "do {{item}}",
              "prompt_prefix": "p", "items": ["a", "b"]},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_fanout_template_without_placeholder",
        args={"description": "d", "prompt_template": "no placeholder",
              "items": ["a", "b"]},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_fanout_template_without_placeholder_plus_prefix_ok",
        args={"description": "d", "prompt_template": "no placeholder",
              "prompt_prefix": "p", "items": ["a", "b"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="params_sub_agent_cap",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["i%03d" % i for i in range(129)]},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_sub_agent_cap_with_resume",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["i%03d" % i for i in range(127)],
              "resume_agent_ids": {"agent-a": "x", "agent-b": "y"}},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_unknown_keys_ignored",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"], "bogus": 1, "steps": [{"x": 1}]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="params_parallel_sample_needs_prompt",
        args={"description": "d", "mode": "parallel_sample"},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_parallel_sample_template_and_prefix",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "prompt_prefix": "p"},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="params_description_null",
        args={"description": None, "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        expect_status="invalid_input",
        expect_message_override="missing required field: description",
        rules=[],
    ))
    add(dict(
        name="params_resume_value_not_a_string",
        args={"description": "d", "items": ["a", "b"],
              "prompt_template": "do {{item}}", "resume_agent_ids": {"a": 5}},
        expect_status="invalid_input",
        expect_message_override="resume_agent_ids values must be strings",
        rules=[],
    ))
    add(dict(
        name="params_resume_not_a_mapping",
        args={"description": "d", "items": ["a", "b"],
              "prompt_template": "do {{item}}", "resume_agent_ids": ["a"]},
        expect_status="invalid_input",
        expect_message_override=(
            "resume_agent_ids must be a mapping of agent id to prompt"),
        rules=[],
    ))

    add(dict(
        name="params_mode_real",
        args={"description": "d", "mode": 1.5, "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=1.5)"),
        rules=[],
    ))
    add(dict(
        name="params_mode_bool",
        args={"description": "d", "mode": True, "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=True)"),
        rules=[],
    ))
    add(dict(
        name="params_mode_list",
        args={"description": "d", "mode": ["fanout"], "items": ["a", "b"],
              "prompt_template": "do {{item}}"},
        expect_status="invalid_input",
        expect_message_override=(
            "Input should be 'fanout' or 'parallel_sample' (mode=[fanout])"),
        rules=[],
    ))
    add(dict(
        name="params_items_element_null",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", None]},
        expect_status="invalid_input",
        expect_message_override="items must be a list of strings",
        rules=[],
    ))
    add(dict(
        name="params_subagent_type_list",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"], "subagent_type": []},
        expect_status="invalid_input",
        expect_message_override="subagent_type must be a string",
        rules=[],
    ))
    add(dict(
        name="params_selector_null_is_unset",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": 2, "selector": None},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "1"}]},
            {"match": "", "steps": [{"kind": "text", "text": "ok"}]},
        ],
    ))
    add(dict(
        name="params_sample_n_null_is_default",
        args={"description": "d", "mode": "parallel_sample",
              "prompt_template": "do it", "sample_n": None},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "0"}]},
            {"match": "", "steps": [{"kind": "text", "text": "ok"}]},
        ],
    ))
    add(dict(
        # A suffix without a prefix is accepted by the model (only a *prefix*
        # selects the prefix+suffix expansion path); _expand_template then
        # ignores the suffix because the template carries the placeholder.
        name="params_prompt_suffix_without_prefix",
        args={"description": "d", "prompt_template": "do {{item}}",
              "prompt_suffix": " s", "items": ["a", "b"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="params_empty_resume_mapping",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a"], "resume_agent_ids": {}},
        expect_status="invalid_input",
        rules=[],
    ))
    add(dict(
        name="fanout_duplicate_quoted_prompts",
        args={"description": "quoted dupes",
              "prompt_template": "fix {{item}}",
              "items": ["it's", "it's"]},
        expect_status="invalid_input",
        expect_message_override=(
            "Expanded prompts must be unique; duplicates: {\"fix it's\"}"),
        rules=[],
    ))
    add(dict(
        name="fanout_beyond_the_burst",
        args={"description": "six items",
              "prompt_template": "item {{item}}",
              "items": ["i1", "i2", "i3", "i4", "i5", "i6"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))
    add(dict(
        name="parallel_sample_majority_with_two_samples",
        args={"description": "majority needs three",
              "mode": "parallel_sample",
              "selector": "majority",
              "prompt_template": "solve it",
              "sample_n": 2},
        path="sample",
        rules=[
            {"match": "You are reviewing multiple", "steps": [
                {"kind": "text", "text": "1"}]},
            {"match": "", "steps": [{"kind": "text", "text": "cand"}]},
        ],
    ))
    add(dict(
        name="fanout_description_multiline",
        args={"description": "line one\nline two\ttabbed",
              "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        rules=[{"match": "", "steps": [{"kind": "echo"}]}],
    ))

    # ---- session gates ----------------------------------------------------
    add(dict(
        name="gate_recursive_sub_agent",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        sub_agent=True,
        expect_status="blocked",
        rules=[],
    ))
    add(dict(
        name="gate_not_a_swarm_session",
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        swarm_session=False,
        expect_status="unsupported",
        expect_message_override="workflow is only available in a swarm session",
        rules=[],
    ))
    add(dict(
        name="gate_not_a_swarm_session_recursive",
        # Python raises SkipThisTool in __init__, so the recursion guard is
        # unreachable: the gate is checked first, before any other guard.
        args={"description": "d", "prompt_template": "do {{item}}",
              "items": ["a", "b"]},
        swarm_session=False,
        sub_agent=True,
        expect_status="unsupported",
        expect_message_override="workflow is only available in a swarm session",
        rules=[],
    ))
    return C


async def _run_tool_case(ref: dict, case: dict) -> dict:
    swarm = ref["swarm"]
    utils = ref["utils"]
    MessageType = ref["MessageType"]

    rules = Rules(case.get("rules", []))
    calls: list[str] = []

    async def prompt_async(prompt_str=None, session=None, output_function=None,
                           **kwargs):
        calls.append(prompt_str)
        status, text = _outcome(rules.step_for(prompt_str), prompt_str)
        if status == "fail":
            raise RuntimeError(text)
        if status == "ok" and text:
            output_function(text, MessageType.Text)

    async def close_session_async(session):
        return None

    async def resolve(task, subagent_type, parent):
        # _resolve_subagent_session: session_id = task.agent_id or uuid4()
        sid = task.agent_id or ("agent-%d" % task.index)
        return _Session(sid), sid, task.prompt

    async def no_sleep(seconds, result=None):
        return None

    # The best-of-N worker workspaces are file-system / git effects; the port
    # takes them as injected `workspace_hooks`, so the corpus stubs them the
    # same way instead of creating real git worktrees.
    saved_ws, _applied = _sample_worker_stubs(ref["bon"])

    saved = (utils.prompt_async, utils.close_session_async,
             swarm._resolve_subagent_session, swarm.time, asyncio.sleep)
    utils.prompt_async = prompt_async
    utils.close_session_async = close_session_async
    swarm._resolve_subagent_session = resolve
    swarm.time = _Clock()
    # The retry backoff and the token bucket's sleep are timed by
    # `retry_delay_seconds` / the limiter golden, so the corpus does not wait.
    asyncio.sleep = no_sleep
    try:
        # Call-layer semantics (kosong CallableTool2.__call__): validate first,
        # and only fall back to the argument-repair pass when that fails.
        try:
            params = swarm.AgentSwarmParams.model_validate(case["args"])
            error = None
        except Exception as exc:  # pydantic ValidationError
            error = str(exc)
            params = None
        row = dict(case)
        row["name"] = case["name"]
        row["args_json"] = json.dumps(case["args"], ensure_ascii=False,
                                      separators=(",", ":"))
        row["swarm_session"] = case.get("swarm_session", True)
        row["sub_agent"] = case.get("sub_agent", False)
        row["path"] = case.get("path", "fanout")
        row["calls"] = calls
        row["rules_out"] = _effective_steps(case.get("rules", []), row["path"])
        row["elapsed"] = {}
        if error is not None:
            # A model-level `_validate` failure surfaces as a pydantic
            # value_error; its inner message is the verbatim ValueError text the
            # C++ port reproduces (the surrounding pydantic block is not).
            inner = re.search(r"Value error, (.*?) \[type=value_error",
                              error, re.S)
            row["python_error"] = error
            row["is_error"] = True
            row["message"] = (inner.group(1) if inner else
                              error.split("|")[0].strip())
            row["output"] = ""
            row["brief"] = ""
            row["expect_message"] = case.get(
                "expect_message_override",
                inner.group(1) if inner else row["message"])
            row["expect_brief"] = case.get("brief_override", "Invalid params")
            row["expect_status"] = case.get("expect_status", "invalid_input")
            return row
        session = _Session()
        if row["swarm_session"]:
            session.custom_data["is_swarm_session"] = True
        if row["sub_agent"]:
            session.custom_config["is_sub_agent"] = True
        try:
            tool = swarm.AgentSwarm(session)
        except Exception as exc:  # kimi_cli.tools.SkipThisTool
            row["is_error"] = True
            row["message"] = ""
            row["brief"] = ""
            row["output"] = ""
            row["python_error"] = ("%s (the tool is never offered at all)"
                                   % type(exc).__name__)
            row["expect_message"] = case.get("expect_message_override", "")
            row["expect_brief"] = case.get("brief_override", "invalid tool.")
            row["expect_status"] = case.get("expect_status", "unsupported")
            return row
        try:
            result = await tool(params)
        except Exception as exc:
            # _validate_uniqueness raises a ValueError out of _execute; the call
            # layer turns tool exceptions into a ToolError (message = str(exc)).
            row["is_error"] = True
            row["message"] = str(exc)
            row["brief"] = ""
            row["output"] = ""
            # The duplicate set repr order is unspecified in Python, so the
            # documentation column uses the same (sorted) text as the C++ port.
            row["python_error"] = "%s: %s" % (
                type(exc).__name__,
                case.get("expect_message_override", str(exc)))
            row["expect_message"] = case.get("expect_message_override",
                                             str(exc))
            row["expect_brief"] = case.get("brief_override",
                                           "duplicate prompts")
            row["expect_status"] = case.get("expect_status", "invalid_input")
            return row
        row["is_error"] = bool(result.is_error)
        row["message"] = result.message
        row["brief"] = result.brief
        row["output"] = result.output if isinstance(result.output, str) else ""
        row["python_error"] = ""
        row["expect_message"] = case.get("expect_message_override",
                                         result.message)
        if result.is_error:
            row["expect_brief"] = result.brief
            row["expect_status"] = case.get("expect_status", "error")
        else:
            # A Python ToolOk carries no brief; the C++ envelope always does
            # (same convention as the other ported tools), so the expected
            # wording is authored per mode.
            default_brief = ("best-of-N completed"
                             if row["path"] == "sample" else "Swarm completed")
            row["expect_brief"] = case.get("brief_override", default_brief)
            row["expect_status"] = case.get("expect_status", "ok")
        if not row["is_error"]:
            for index, text in _elapsed_by_index(row["output"]).items():
                row["elapsed"][index] = text
        return row
    finally:
        (utils.prompt_async, utils.close_session_async,
         swarm._resolve_subagent_session, swarm.time, asyncio.sleep) = saved
        _restore_workspaces(ref["bon"], saved_ws)


_ELAPSED_RE = re.compile(r'index="(\d+)"[^>]*elapsed="([^"]*)"')


def _elapsed_by_index(xml: str) -> dict[int, str]:
    return {int(m.group(1)): m.group(2) for m in _ELAPSED_RE.finditer(xml)}


async def _run_tool_corpus(ref: dict) -> list[dict]:
    rows = []
    for case in tool_corpus():
        rows.append(await _run_tool_case(ref, case))
    return rows


# ---------------------------------------------------------------------------
# Kernel corpus
# ---------------------------------------------------------------------------


def _expand_rows(ref: dict) -> list[dict]:
    fn = ref["swarm"]._expand_template
    rows = []
    cases = [
        ("placeholder", "process {{item}}", None, None, ["a", "b"]),
        ("placeholder_twice", "{{item}} and {{item}}", None, None, ["x"]),
        ("placeholder_empty_items", "do {{item}}", None, None, []),
        ("no_placeholder_prefix", "ignored", "pre:", None, ["a", "b"]),
        ("no_placeholder_prefix_suffix", "ignored", "pre:", ":post",
         ["a", "b"]),
        ("prefix_suffix", None, "pre:", ":post", ["a"]),
        ("prefix_only", None, "pre:", None, ["a", "b"]),
        ("suffix_without_prefix", None, None, ":post", ["a", "b"]),
        ("passthrough", None, None, None, ["raw one", "raw two"]),
        ("placeholder_with_prefix_suffix", "go {{item}}", "pre:", ":post",
         ["i"]),
    ]
    for name, tpl, prefix, suffix, items in cases:
        out = fn(tpl, items, prefix=prefix, suffix=suffix)
        rows.append(dict(name=name, tpl=tpl, prefix=prefix, suffix=suffix,
                         items=items, expect=list(out)))
    return rows


def _uniqueness_rows(ref: dict) -> list[dict]:
    fn = ref["swarm"]._validate_uniqueness
    rows = []
    for name, prompts in [
        ("unique", ["p1", "p2", "p3"]),
        ("single", ["only"]),
        ("empty", []),
        ("one_duplicate", ["p1", "p1"]),
        ("duplicate_twice", ["p1", "p1", "p1"]),
        ("two_duplicates", ["a", "b", "a", "b", "c"]),
        ("duplicate_with_quotes", ["it's", "it's"]),
        ("duplicate_with_newline", ["a\nb", "x", "a\nb"]),
    ]:
        message = ""
        try:
            fn(prompts)
        except ValueError:
            # The reference interpolates a *set* literal, whose iteration order
            # is unspecified (hash randomisation), so the golden records the
            # canonical form the port emits: the same reprs, sorted.
            seen: set[str] = set()
            dups: set[str] = set()
            for prompt in prompts:
                if prompt in seen:
                    dups.add(prompt)
                seen.add(prompt)
            message = ("Expanded prompts must be unique; duplicates: {" +
                       ", ".join(sorted(repr(d) for d in dups)) + "}")
        rows.append(dict(name=name, prompts=prompts, expect=message))
    return rows


def _escape_rows(ref: dict) -> list[dict]:
    fn = ref["swarm"]._xml_escape
    rows = []
    for name, text in [
        ("plain", "hello world"),
        ("amp", "a & b"),
        ("lt_gt", "<a> & <b>"),
        ("quotes", '"double" and \'single\''),
        ("already_escaped", "&amp; &lt;"),
        ("mixed", '<subagent id="x">&\'</subagent>'),
        ("empty", ""),
        ("newline_tab", "a\nb\tc"),
    ]:
        rows.append(dict(name=name, text=text, expect=fn(text)))
    return rows


def _render_rows(ref: dict) -> list[dict]:
    swarm = ref["swarm"]
    rows = []
    cases = [
        ("two_ok", [
            dict(index=0, agent_id="a1", output="all good", success=True),
            dict(index=1, agent_id="a2", output="me too", success=True,
                 elapsed=1.25),
        ]),
        ("one_failed", [
            dict(index=0, agent_id="a1", output="ok", success=True,
                 elapsed=0.0),
            dict(index=1, agent_id="a&2", output="bad <value>", success=False,
                 error="err <&> 'x'"),
        ]),
        ("empty", []),
        ("error_without_flag", [
            dict(index=3, agent_id="a3", output="out", success=True,
                 error="stale error", elapsed=12.04),
        ]),
        ("null_error", [
            dict(index=0, agent_id="a0", output="", success=False,
                 error=None),
        ]),
    ]
    for name, specs in cases:
        results = [
            swarm.SwarmSubagentResult(
                index=s["index"], agent_id=s["agent_id"], output=s["output"],
                success=s["success"], error=s.get("error"),
                elapsed=s.get("elapsed"))
            for s in specs
        ]
        rows.append(dict(name=name, description="desc <&> \"q\"",
                         results=specs, expect=swarm._render_results(
                             results, "desc <&> \"q\"")))
    return rows


def _bon_render_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []
    cases = [
        ("two_candidates", 1, "self-eval selection", [
            dict(index=0, success=True, error=None),
            dict(index=1, success=True, error=None)]),
        ("failed_candidate", 2, "only one viable candidate", [
            dict(index=0, success=False, error="boom <&> \"q\""),
            dict(index=1, success=True, error=None),
            dict(index=2, success=True, error=None)]),
        ("majority_reason", 0, "majority vote {0: 2, 1: 1}", [
            dict(index=0, success=True, error=None),
            dict(index=1, success=True, error=None)]),
        ("empty", 0, "n=1: no selection", []),
    ]
    for name, winner, reason, specs in cases:
        result = bon.BestOfNResult(winner_index=winner, selection_reason=reason,
                                   candidates=[
                                       bon.SampleCandidate(
                                           index=s["index"],
                                           work_dir=Path("/wf/w%d" % s["index"]),
                                           success=s["success"],
                                           error=s["error"])
                                       for s in specs])
        rows.append(dict(name=name, description="d & <e>", winner=winner,
                         reason=reason, candidates=specs,
                         expect=_tool_bon_render(ref, result, "d & <e>")))
    return rows


def _tool_bon_render(ref: dict, result, description: str) -> str:
    """Render <best_of_n_result> the way the tool does (AgentSwarm code path).

    ``_execute_parallel_sample`` inlines the renderer, so it is reproduced here
    verbatim from the reference source (swarm/__init__.py 322-335).
    """
    swarm = ref["swarm"]
    lines = [
        "<best_of_n_result>",
        "  <description>%s</description>" % swarm._xml_escape(description),
        "  <samples>%d</samples>" % len(result.candidates),
        "  <winner>%d</winner>" % result.winner_index,
        "  <selection>%s</selection>" % swarm._xml_escape(result.selection_reason),
    ]
    for candidate in result.candidates:
        status = ("ok" if candidate.success
                  else "failed: %s" % swarm._xml_escape(candidate.error or ""))
        lines.append('  <candidate index="%d" status="%s"/>'
                     % (candidate.index, swarm._xml_escape(status)))
    lines.append("</best_of_n_result>")
    return "\n".join(lines)


def _marker_rows(ref: dict) -> list[dict]:
    fn = ref["swarm"]._is_rate_limit_error
    rows = []
    for name, text in [
        ("rate_limit", "Rate limit exceeded"),
        ("rate_limit_dash", "rate-limit hit"),
        ("429", "HTTP 429 from the provider"),
        ("too_many_requests", "Too Many Requests"),
        ("capacity", "no CAPACITY left"),
        ("throttled", "Throttled by the gateway"),
        ("quota", "Quota Exceeded for this key"),
        ("quota_partial", "quota"),
        ("plain", "connection reset by peer"),
        ("empty", ""),
        ("upper_all", "RATE LIMIT"),
        ("class_prefix", "APIStatusError 429 rate limit exceeded"),
    ]:
        rows.append(dict(name=name, text=text, expect=bool(fn(RuntimeError(text)))))
    return rows


def _retry_rows(ref: dict) -> list[dict]:
    swarm = ref["swarm"]
    rows = []
    for attempt in range(5):
        rows.append(dict(attempt=attempt,
                         expect=swarm._RETRY_BASE_SECONDS * (2 ** attempt)))
    return rows


async def _limiter_rows(ref: dict) -> list[dict]:
    swarm = ref["swarm"]
    rows = []
    cases = [
        ("burst_immediate", 5, 0.7, [0.0, 0.0, 0.0]),
        ("refill", 2, 1.0, [0.0, 0.0, 0.5, 0.5, 2.0]),
        ("single_token", 1, 0.25, [0.0, 0.25, 0.25, 1.0]),
        ("burst_one_interval_zero_guard", 1, 0.5, [0.0, 0.5, 1.5]),
    ]
    for name, burst, interval, deltas in cases:
        clock = _Clock()
        clock.step = 0.0  # monotonic() returns the virtual clock unchanged
        real_sleep = asyncio.sleep
        real_time = swarm.time

        async def fake_sleep(seconds, _clock=clock):
            _clock.now += float(seconds or 0.0)

        asyncio.sleep = fake_sleep
        swarm.time = clock  # _RateLimiter reads time.monotonic() itself
        limiter = swarm._RateLimiter(burst, interval)
        try:
            waits = []
            for delta in deltas:
                clock.now += delta
                await limiter.acquire()
                waits.append(clock.now)
        finally:
            asyncio.sleep = real_sleep
            swarm.time = real_time
        rows.append(dict(name=name, burst=burst, interval=interval,
                         times=list(deltas), expects=list(waits)))
    return rows


def _review_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []
    cases = [
        ("two_ok", [
            dict(index=0, report="report zero", diff="[workspace:copy]\nd0",
                 steps=3),
            dict(index=1, report="report one", diff="[workspace:worktree]\nd1",
                 steps=0),
        ]),
        ("with_failure", [
            dict(index=0, report="", diff="", steps=0, success=False,
                 error="boom"),
            dict(index=2, report="ok", diff="d2", steps=11),
        ]),
        ("empty", []),
    ]
    for name, specs in cases:
        candidates = [
            bon.SampleCandidate(
                index=s["index"], work_dir=Path("/wf/w%d" % s["index"]),
                diff=s["diff"], self_report=s["report"], steps=s["steps"],
                success=s.get("success", True), error=s.get("error"))
            for s in specs
        ]
        for spec in specs:
            spec["success"] = spec.get("success", True)
        rows.append(dict(name=name, candidates=specs,
                         expect=bon.format_candidates_for_review(candidates)))
    return rows


def _message_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []

    def cand(index, success, error=None):
        return bon.SampleCandidate(index=index, work_dir=Path("/wf/w%d" % index),
                                   success=success, error=error)

    failed = [cand(0, False, "boom"), cand(1, False, "bang"), cand(2, False)]
    rows.append(dict(name="all_failed", kind="all_failed", input="",
                     index=0, expect=_all_failed_message(failed),
                     candidates=[dict(index=0, success=False, error="boom"),
                                 dict(index=1, success=False, error="bang"),
                                 dict(index=2, success=False, error=None)],
                     votes=[]))
    rows.append(dict(name="all_failed_single", kind="all_failed", input="",
                     index=0,
                     expect=_all_failed_message([cand(0, False, "x")]),
                     candidates=[dict(index=0, success=False, error="x")],
                     votes=[]))
    rows.append(dict(name="all_failed_no_error", kind="all_failed", input="",
                     index=0,
                     expect=_all_failed_message([cand(5, False)]),
                     candidates=[dict(index=5, success=False, error=None)],
                     votes=[]))
    rows.append(dict(name="single_run_failed", kind="single_run_failed",
                     input="boom", index=0, candidates=[], votes=[],
                     expect="single run failed: boom"))
    rows.append(dict(name="single_run_failed_empty", kind="single_run_failed",
                     input="", index=0, candidates=[], votes=[],
                     expect="single run failed: "))
    rows.append(dict(name="verification_rejected", kind="verification_rejected",
                     input="tests failed", index=2, candidates=[], votes=[],
                     expect="selected candidate #2 failed verification: tests "
                            "failed"))
    rows.append(dict(name="verification_rejected_empty",
                     kind="verification_rejected", input="", index=0,
                     candidates=[], votes=[],
                     expect="selected candidate #0 failed verification: "))
    rows.append(dict(name="votes_empty", kind="votes", input="", index=0,
                     votes=[], candidates=[], expect="{}"))
    rows.append(dict(name="votes_two", kind="votes", input="", index=0,
                     votes=[[0, 2], [1, 1]], candidates=[],
                     expect="{0: 2, 1: 1}"))
    rows.append(dict(name="votes_zero", kind="votes", input="", index=0,
                     votes=[[0, 0], [1, 0]], candidates=[],
                     expect="{0: 0, 1: 0}"))
    rows.append(dict(name="votes_unsorted_keys", kind="votes", input="",
                     index=0, votes=[[2, 1], [0, 3]], candidates=[],
                     expect="{2: 1, 0: 3}"))
    return rows


def _all_failed_message(candidates) -> str:
    return ("all %d sampled candidates failed: " % len(candidates)) + "; ".join(
        "#%d: %s" % (c.index, c.error) for c in candidates)


async def _select_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []
    cases = [
        # name, per-candidate success flags, strategy, selector replies
        ("self_eval_picks_second", [1, 1], "self_eval", [1]),
        ("self_eval_picks_first", [1, 1], "self_eval", [0]),
        ("self_eval_single_viable", [0, 1, 0], "self_eval", [99]),
        ("self_eval_invalid_index", [1, 1], "self_eval", [7]),
        ("self_eval_negative_index", [1, 1], "self_eval", [-1]),
        ("all_failed", [0, 0], "self_eval", []),
        ("majority_pick_b", [1, 1, 1], "majority", [1, 2, 2]),
        ("majority_pick_a", [1, 1, 1], "majority", [0, 0, 0]),
        ("majority_tie", [1, 1, 1], "majority", [0, 1, 0]),
        ("majority_two_viable_self_eval", [1, 1, 0], "majority", [2]),
        ("majority_single_reply_repeats", [1, 1, 1], "majority", [2]),
    ]
    for name, successes, strategy, replies in cases:
        candidates = [
            bon.SampleCandidate(index=i, work_dir=Path("/wf/w%d" % i),
                                diff="[workspace:copy]\nd%d" % i,
                                self_report="rep %d" % i, steps=i,
                                success=bool(s))
            for i, s in enumerate(successes)
        ]
        reviews: list[str] = []
        counter = {"n": 0}

        async def selector(prompt, review_text, _reviews=reviews,
                           _counter=counter, _replies=list(replies)):
            _reviews.append(review_text)
            pos = min(_counter["n"], len(_replies) - 1)
            _counter["n"] += 1
            return _replies[pos]

        try:
            winner, reason = await bon.select_best_candidate(
                "the task", candidates, selector, strategy=strategy)
            error = ""
        except Exception as exc:
            winner, reason = None, ""
            error = "%s: %s" % (type(exc).__name__, exc)
        rows.append(dict(name=name, strategy=strategy, replies=list(replies),
                         # The full candidate spec: the review text the selector
                         # receives embeds the self-report, the step count and
                         # the diff.
                         candidates=[dict(index=c.index, success=c.success,
                                          error=c.error,
                                          report=c.self_report, steps=c.steps,
                                          tokens=c.output_tokens, diff=c.diff)
                                     for c in candidates],
                         reviews=reviews, winner=winner, reason=reason,
                         error=error))
    return rows


def _sample_worker_stubs(bon, ref=None):
    """Patch the best-of-N workspace helpers with deterministic stubs.

    The Python reference isolates each sample in a real git worktree (or a
    copied directory); the C++ port receives those effects as an injected
    ``workspace_hooks`` struct, so both sides are driven with the same stubs.
    """
    saved = (bon.create_worker_workspace, bon.cleanup_worker_workspace,
             bon.collect_diff, bon.apply_diff_to_workspace,
             bon._snapshot_files)
    applied: list[str] = []

    def create(work_dir, index):
        return Path("/wf/w%d" % index), "copy"

    def cleanup(worker_path, kind, main_work_dir):
        return None

    def collect(worker_path, kind, before_snapshot=None):
        return "diff for worker %s\nline2\n" % Path(worker_path).name

    def apply(winner, main_work_dir, kind):
        applied.append("apply %d %s" % (winner.index, kind))

    def snapshot(root):
        return {}

    bon.create_worker_workspace = create
    bon.cleanup_worker_workspace = cleanup
    bon.collect_diff = collect
    bon.apply_diff_to_workspace = apply
    bon._snapshot_files = snapshot
    return saved, applied


def _restore_workspaces(bon, saved) -> None:
    (bon.create_worker_workspace, bon.cleanup_worker_workspace,
     bon.collect_diff, bon.apply_diff_to_workspace,
     bon._snapshot_files) = saved


async def _sample_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []
    cases = [
        ("all_ok", 3, {"": [{"kind": "text", "text": "rep {prompt}"}]},
         [10, 11, 12], None),
        ("one_failed", 3, {"": [
            {"kind": "text", "text": "rep0"},
            {"kind": "fail", "text": "boom"},
            {"kind": "text", "text": "rep2"}]}, [1, 2, 3], None),
        ("n_one", 1, {"": [{"kind": "text", "text": "only"}]}, [7], None),
    ]
    for name, n, script, steps, _unused in cases:
        saved, applied = _sample_worker_stubs(bon, ref)
        rules = Rules([{"match": m, "steps": s} for m, s in script.items()])
        calls: list[str] = []

        async def runner(prompt, worker_dir, _rules=rules, _calls=calls):
            _calls.append(prompt)
            status, text = _outcome(_rules.step_for(prompt), prompt)
            if status == "fail":
                raise RuntimeError(text)
            # A fixed (steps, tokens) pair keeps the replay reproducible; the
            # values only travel into the candidate objects.
            return text, 1, 2

        try:
            candidates = await bon.run_parallel_sample("do the task", n,
                                                       Path("/wf/main"), runner,
                                                       max_concurrency=1)
        finally:
            _restore_workspaces(bon, saved)
        rows.append(dict(
            name=name, n=n, task="do the task",
            rules=_effective_steps([{"match": m, "steps": s}
                                    for m, s in script.items()], "sample"),
            calls=list(calls), applied=list(applied),
            candidates=[dict(index=c.index, success=c.success, error=c.error,
                             report=c.self_report, steps=c.steps,
                             tokens=c.output_tokens, diff=c.diff)
                        for c in candidates]))
    return rows


async def _bon_rows(ref: dict) -> list[dict]:
    bon = ref["bon"]
    rows = []
    cases = [
        # name, n, strategy, script, replies, verify, expect
        ("self_eval_applies_winner", 3, "self_eval",
         {"": [{"kind": "text", "text": "rep {prompt}"}]}, [1], None),
        ("majority_winner", 4, "majority",
         {"": [{"kind": "text", "text": "rep {prompt}"}]}, [2, 3, 3, 3, 3, 3],
         None),
        ("degenerate_n1", 1, "self_eval",
         {"": [{"kind": "text", "text": "only"}]}, [], None),
        ("degenerate_n1_failed", 1, "self_eval",
         {"": [{"kind": "fail", "text": "worker boom"}]}, [], None),
        ("all_failed", 3, "self_eval",
         {"": [{"kind": "fail", "text": "boom {prompt}"}]}, [], None),
        ("verify_ok", 2, "self_eval",
         {"": [{"kind": "text", "text": "rep {prompt}"}]}, [0], (True, "ok!"),
         ),
        ("verify_rejected", 2, "self_eval",
         {"": [{"kind": "text", "text": "rep {prompt}"}]}, [1],
         (False, "tests failed")),
        ("selector_invalid_falls_back", 2, "self_eval",
         {"": [{"kind": "text", "text": "rep {prompt}"}]}, [42], None),
    ]
    for name, n, strategy, script, replies, verify, *_rest in cases:
        saved, applied = _sample_worker_stubs(bon, ref)
        rules = Rules([{"match": m, "steps": s} for m, s in script.items()])
        calls: list[str] = []
        reviews: list[str] = []

        async def runner(prompt, worker_dir, _rules=rules, _calls=calls):
            _calls.append(prompt)
            status, text = _outcome(_rules.step_for(prompt), prompt)
            if status == "fail":
                raise RuntimeError(text)
            return text, 1, 2

        counter = {"n": 0}

        async def selector(prompt, review_text, _reviews=reviews,
                           _counter=counter, _replies=list(replies)):
            _reviews.append(review_text)
            pos = min(_counter["n"], len(_replies) - 1)
            _counter["n"] += 1
            return _replies[pos]

        async def verify_fn(work_dir, _verdict=verify):
            return _verdict

        try:
            result = await bon.best_of_n(
                "the task", Path("/wf/main"), runner, selector, n=n,
                strategy=strategy, verify_fn=verify_fn if verify else None,
                max_concurrency=1)
            winner, reason = result.winner_index, result.selection_reason
            verified, detail = result.verified, result.verify_detail
            error = ""
            candidates = result.candidates
        except Exception as exc:
            winner, reason, verified, detail = None, "", False, ""
            error = "%s: %s" % (type(exc).__name__, exc)
            candidates = []
        finally:
            _restore_workspaces(bon, saved)
        rows.append(dict(
            name=name, n=n, strategy=strategy,
            has_verify=bool(verify),
            verify_ok=bool(verify and verify[0]),
            verify_detail=(verify[1] if verify else ""),
            rules=_effective_steps([{"match": m, "steps": s}
                                    for m, s in script.items()], "sample"),
            replies=list(replies), calls=list(calls), reviews=list(reviews),
            applied=list(applied), error=error, winner=winner, reason=reason,
            verified=verified, detail=detail,
            candidates=[dict(index=c.index, success=c.success, error=c.error,
                             report=c.self_report, steps=c.steps,
                             tokens=c.output_tokens, diff=c.diff)
                        for c in candidates]))
    return rows


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------


def _j(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _emit(tool_rows, expand, unique, escape, render, bon_render, markers,
          retries, limiter, review, messages, select, samples, bons) -> str:
    L: list[str] = []
    L.append("// GENERATED by scripts/gen_workflow_goldens.py from the Python reference")
    L.append("// implementation (kimi-agent: src/kimix/tools/swarm/__init__.py and")
    L.append("// src/kimix/tools/swarm/best_of_n.py) driven with stub sessions, sub-agent")
    L.append("// runners, selectors and worker workspaces. Do not edit by hand -")
    L.append("// regenerate with: python scripts/gen_workflow_goldens.py")
    L.append("//")
    L.append("// `python_error` is the verbatim pydantic/ValueError text the Python call")
    L.append("// layer produced (documentation only); `expect_message` is the wording the")
    L.append("// C++ port must produce, which follows the project's pydantic-message")
    L.append("// convention (\"<pydantic text> (<field>=<value>)\" / \"missing required")
    L.append("// field: <name>\") where the verbatim text is not reproducible.")
    L.append("")
    L.append("// One scripted sub-agent/selector outcome. `seq` is the position inside its")
    L.append("// group; the first group whose `match` is a substring of the prompt wins and")
    L.append("// consumes its steps in order (the last step repeats).")
    L.append("//   kind=\"echo\"   -> success, output \"report for <prompt>\"")
    L.append("//   kind=\"text\"   -> success, output = text ({prompt} substituted)")
    L.append("//   kind=\"silent\" -> success, no output text at all")
    L.append("//   kind=\"fail\"   -> failure, output = error = text (\"RuntimeError: \"")
    L.append("//                     already prefixed on the best-of-N path)")
    L.append("//   kind=\"pick_b\" -> success, output = the index in \"Candidate B (#n)\"")
    L.append("struct wf_golden_rule {")
    L.append("    const char *match;")
    L.append("    int32_t seq;")
    L.append("    const char *kind;")
    L.append("    const char *text;")
    L.append("};")
    L.append("")
    L.append("// One end-to-end tool call: raw JSON arguments -> result envelope.")
    L.append("// `elapsed` is a JSON object {index: \"<elapsed attribute text>\"}; the C++")
    L.append("// runner replays those values (the wall clock itself is not reproducible).")
    L.append("struct wf_golden_tool {")
    L.append("    const char *name;")
    L.append("    const char *args;            // raw JSON argument object")
    L.append("    bool swarm_session;")
    L.append("    bool sub_agent;")
    L.append("    const wf_golden_rule *rules;")
    L.append("    int32_t rule_count;")
    L.append("    const char *elapsed;         // JSON object index -> elapsed text")
    L.append("    const char *calls;           // JSON array of dispatched prompts")
    L.append("    bool is_error;")
    L.append("    const char *expect_status;")
    L.append("    const char *python_error;")
    L.append("    const char *expect_message;")
    L.append("    const char *expect_brief;")
    L.append("    const char *expect_output;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_expand {")
    L.append("    const char *name;")
    L.append("    const char *tpl;      // \"\" + tpl_null when the template is None")
    L.append("    bool tpl_null;")
    L.append("    const char *prefix;")
    L.append("    bool prefix_null;")
    L.append("    const char *suffix;")
    L.append("    bool suffix_null;")
    L.append("    const char *items;    // JSON array of strings")
    L.append("    const char *expect;   // JSON array of strings")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_uniqueness {")
    L.append("    const char *name;")
    L.append("    const char *prompts;  // JSON array of strings")
    L.append("    const char *expect;   // \"\" when the Python call succeeded")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_text {")
    L.append("    const char *name;")
    L.append("    const char *input;")
    L.append("    const char *expect;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_render {")
    L.append("    const char *name;")
    L.append("    const char *description;")
    L.append("    const char *results;   // JSON array of subagent result objects")
    L.append("    const char *expect;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_bon_render {")
    L.append("    const char *name;")
    L.append("    const char *description;")
    L.append("    int32_t winner;")
    L.append("    const char *reason;")
    L.append("    const char *candidates; // JSON array {index,success,error}")
    L.append("    const char *expect;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_marker {")
    L.append("    const char *name;")
    L.append("    const char *text;")
    L.append("    bool expect;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_retry {")
    L.append("    int32_t attempt;")
    L.append("    double expect;")
    L.append("};")
    L.append("")
    L.append("// Token bucket: `times` are the virtual clock offsets (seconds) before each")
    L.append("// acquire(); `expects` the virtual clock value after it (Python sleeps")
    L.append("// inside acquire, the C++ kernel returns the wait for the caller).")
    L.append("struct wf_golden_limiter {")
    L.append("    const char *name;")
    L.append("    int32_t burst;")
    L.append("    double interval;")
    L.append("    const char *times;    // JSON array of numbers")
    L.append("    const char *expects;  // JSON array of numbers")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_message {")
    L.append("    const char *name;")
    L.append("    const char *kind;       // all_failed | single_run_failed |")
    L.append("                            // verification_rejected | votes")
    L.append("    const char *input;      // error text / verify detail")
    L.append("    int32_t index;          // winner index for verification_rejected")
    L.append("    const char *candidates; // JSON array {index,success,error}")
    L.append("    const char *votes;      // JSON array of [index, votes] pairs")
    L.append("    const char *expect;")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_select {")
    L.append("    const char *name;")
    L.append("    const char *strategy;")
    L.append("    const char *candidates; // JSON array {index,success,error}")
    L.append("    const char *replies;    // JSON array of selector return values")
    L.append("    const char *reviews;    // JSON array of review texts the selector saw")
    L.append("    int32_t winner;")
    L.append("    const char *reason;")
    L.append("    const char *error;      // \"\" when the Python call returned")
    L.append("};")
    L.append("")
    L.append("// One sampled worker: index/success/error/self_report/steps/output_tokens/diff.")
    L.append("struct wf_golden_sample {")
    L.append("    const char *name;")
    L.append("    int32_t n;")
    L.append("    const char *task;")
    L.append("    const wf_golden_rule *rules;")
    L.append("    int32_t rule_count;")
    L.append("    const char *calls;      // JSON array of prompts handed to the runner")
    L.append("    const char *applied;    // JSON array of apply_diff_to_workspace calls")
    L.append("    const char *candidates; // JSON array of worker objects")
    L.append("};")
    L.append("")
    L.append("struct wf_golden_bon {")
    L.append("    const char *name;")
    L.append("    int32_t n;")
    L.append("    const char *strategy;")
    L.append("    bool has_verify;")
    L.append("    bool verify_ok;")
    L.append("    const char *verify_detail;")
    L.append("    const wf_golden_rule *rules;")
    L.append("    int32_t rule_count;")
    L.append("    const char *replies;    // JSON array of selector return values")
    L.append("    const char *calls;      // JSON array of prompts handed to the runner")
    L.append("    const char *reviews;    // JSON array of review texts")
    L.append("    const char *applied;    // JSON array of apply_diff_to_workspace calls")
    L.append("    const char *error;      // \"\" when the Python call returned")
    L.append("    int32_t winner;")
    L.append("    const char *reason;")
    L.append("    bool verified;")
    L.append("    const char *detail;")
    L.append("    const char *candidates; // JSON array of worker objects")
    L.append("};")
    L.append("")

    def rules_block(prefix: str, idx: int, rules: list[dict]) -> None:
        if not rules:
            return
        L.append("static const wf_golden_rule kWf%sRules%d[] = {" % (prefix, idx))
        for r in rules:
            L.append("    {%s, %d, %s, %s}," % (
                c_literal(r["match"]), r["seq"], c_literal(r["kind"]),
                c_literal(r.get("text", ""))))
        L.append("};")
        L.append("")

    for i, row in enumerate(tool_rows):
        rules_block("Tool", i, row["rules_out"])

    L.append("const wf_golden_tool kWfToolGoldens[] = {")
    for i, r in enumerate(tool_rows):
        L.append("    {%s, %s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s}," % (
            c_literal(r["name"]),
            c_literal(r["args_json"]),
            "true" if r["swarm_session"] else "false",
            "true" if r["sub_agent"] else "false",
            ("kWfToolRules%d" % i) if r["rules_out"] else "nullptr",
            len(r["rules_out"]),
            c_literal(_j({str(k): v for k, v in sorted(r["elapsed"].items())})),
            c_literal(_j(r["calls"])),
            "true" if r["is_error"] else "false",
            c_literal(r["expect_status"]),
            c_literal(r["python_error"]),
            c_literal(r["expect_message"]),
            c_literal(r["expect_brief"]),
            c_literal(r["output"]),
        ))
    L.append("};")
    L.append("const size_t kWfToolGoldenCount = sizeof(kWfToolGoldens) / "
             "sizeof(kWfToolGoldens[0]);")
    L.append("")

    L.append("const wf_golden_expand kWfExpandGoldens[] = {")
    for r in expand:
        L.append("    {%s, %s, %s, %s, %s, %s, %s, %s, %s}," % (
            c_literal(r["name"]),
            c_literal(r["tpl"] or ""), "true" if r["tpl"] is None else "false",
            c_literal(r["prefix"] or ""),
            "true" if r["prefix"] is None else "false",
            c_literal(r["suffix"] or ""),
            "true" if r["suffix"] is None else "false",
            c_literal(_j(r["items"])), c_literal(_j(r["expect"]))))
    L.append("};")
    L.append("const size_t kWfExpandGoldenCount = sizeof(kWfExpandGoldens) / "
             "sizeof(kWfExpandGoldens[0]);")
    L.append("")

    L.append("const wf_golden_uniqueness kWfUniquenessGoldens[] = {")
    for r in unique:
        L.append("    {%s, %s, %s}," % (
            c_literal(r["name"]), c_literal(_j(r["prompts"])),
            c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfUniquenessGoldenCount = sizeof(kWfUniquenessGoldens)"
             " / sizeof(kWfUniquenessGoldens[0]);")
    L.append("")

    L.append("const wf_golden_text kWfEscapeGoldens[] = {")
    for r in escape:
        L.append("    {%s, %s, %s}," % (c_literal(r["name"]),
                                        c_literal(r["text"]),
                                        c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfEscapeGoldenCount = sizeof(kWfEscapeGoldens) / "
             "sizeof(kWfEscapeGoldens[0]);")
    L.append("")

    L.append("const wf_golden_render kWfRenderGoldens[] = {")
    for r in render:
        L.append("    {%s, %s, %s, %s}," % (
            c_literal(r["name"]), c_literal(r["description"]),
            c_literal(_j(r["results"])), c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfRenderGoldenCount = sizeof(kWfRenderGoldens) / "
             "sizeof(kWfRenderGoldens[0]);")
    L.append("")

    L.append("const wf_golden_bon_render kWfBonRenderGoldens[] = {")
    for r in bon_render:
        L.append("    {%s, %s, %d, %s, %s, %s}," % (
            c_literal(r["name"]), c_literal(r["description"]), r["winner"],
            c_literal(r["reason"]), c_literal(_j(r["candidates"])),
            c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfBonRenderGoldenCount = sizeof(kWfBonRenderGoldens)"
             " / sizeof(kWfBonRenderGoldens[0]);")
    L.append("")

    L.append("const wf_golden_marker kWfMarkerGoldens[] = {")
    for r in markers:
        L.append("    {%s, %s, %s}," % (c_literal(r["name"]),
                                        c_literal(r["text"]),
                                        "true" if r["expect"] else "false"))
    L.append("};")
    L.append("const size_t kWfMarkerGoldenCount = sizeof(kWfMarkerGoldens) / "
             "sizeof(kWfMarkerGoldens[0]);")
    L.append("")

    L.append("const wf_golden_retry kWfRetryGoldens[] = {")
    for r in retries:
        L.append("    {%d, %s}," % (r["attempt"], repr(float(r["expect"]))))
    L.append("};")
    L.append("const size_t kWfRetryGoldenCount = sizeof(kWfRetryGoldens) / "
             "sizeof(kWfRetryGoldens[0]);")
    L.append("")

    L.append("const wf_golden_limiter kWfLimiterGoldens[] = {")
    for r in limiter:
        L.append("    {%s, %d, %s, %s, %s}," % (
            c_literal(r["name"]), r["burst"], repr(float(r["interval"])),
            c_literal(_j(r["times"])), c_literal(_j(r["expects"]))))
    L.append("};")
    L.append("const size_t kWfLimiterGoldenCount = sizeof(kWfLimiterGoldens) / "
             "sizeof(kWfLimiterGoldens[0]);")
    L.append("")

    L.append("const wf_golden_text kWfReviewGoldens[] = {")
    for r in review:
        L.append("    {%s, %s, %s}," % (
            c_literal(r["name"]), c_literal(_j(r["candidates"])),
            c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfReviewGoldenCount = sizeof(kWfReviewGoldens) / "
             "sizeof(kWfReviewGoldens[0]);")
    L.append("")

    L.append("const wf_golden_message kWfMessageGoldens[] = {")
    for r in messages:
        L.append("    {%s, %s, %s, %d, %s, %s, %s}," % (
            c_literal(r["name"]), c_literal(r["kind"]), c_literal(r["input"]),
            r["index"], c_literal(_j(r.get("candidates", []))),
            c_literal(_j(r.get("votes", []))), c_literal(r["expect"])))
    L.append("};")
    L.append("const size_t kWfMessageGoldenCount = sizeof(kWfMessageGoldens) / "
             "sizeof(kWfMessageGoldens[0]);")
    L.append("")

    L.append("const wf_golden_select kWfSelectGoldens[] = {")
    for r in select:
        L.append("    {%s, %s, %s, %s, %s, %d, %s, %s}," % (
            c_literal(r["name"]), c_literal(r["strategy"]),
            c_literal(_j(r["candidates"])), c_literal(_j(r["replies"])),
            c_literal(_j(r["reviews"])),
            -1 if r["winner"] is None else r["winner"],
            c_literal(r["reason"]), c_literal(r["error"])))
    L.append("};")
    L.append("const size_t kWfSelectGoldenCount = sizeof(kWfSelectGoldens) / "
             "sizeof(kWfSelectGoldens[0]);")
    L.append("")

    for i, row in enumerate(samples):
        rules_block("Sample", i, row["rules"])
    L.append("const wf_golden_sample kWfSampleGoldens[] = {")
    for i, r in enumerate(samples):
        L.append("    {%s, %d, %s, %s, %d, %s, %s, %s}," % (
            c_literal(r["name"]), r["n"], c_literal(r["task"]),
            ("kWfSampleRules%d" % i) if r["rules"] else "nullptr",
            len(r["rules"]), c_literal(_j(r["calls"])),
            c_literal(_j(r["applied"])), c_literal(_j(r["candidates"]))))
    L.append("};")
    L.append("const size_t kWfSampleGoldenCount = sizeof(kWfSampleGoldens) / "
             "sizeof(kWfSampleGoldens[0]);")
    L.append("")

    for i, row in enumerate(bons):
        rules_block("Bon", i, row["rules"])
    L.append("const wf_golden_bon kWfBonGoldens[] = {")
    for i, r in enumerate(bons):
        L.append(
            "    {%s, %d, %s, %s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %d, %s,"
            " %s, %s, %s}," % (
                c_literal(r["name"]), r["n"], c_literal(r["strategy"]),
                "true" if r["has_verify"] else "false",
                "true" if r["verify_ok"] else "false",
                c_literal(r["verify_detail"]),
                ("kWfBonRules%d" % i) if r["rules"] else "nullptr",
                len(r["rules"]), c_literal(_j(r["replies"])),
                c_literal(_j(r["calls"])), c_literal(_j(r["reviews"])),
                c_literal(_j(r["applied"])), c_literal(r["error"]),
                -1 if r["winner"] is None else r["winner"],
                c_literal(r["reason"]),
                "true" if r["verified"] else "false", c_literal(r["detail"]),
                c_literal(_j(r["candidates"]))))
    L.append("};")
    L.append("const size_t kWfBonGoldenCount = sizeof(kWfBonGoldens) / "
             "sizeof(kWfBonGoldens[0]);")
    L.append("")
    return "\n".join(L)


async def _build(ref: dict) -> dict:
    tool_rows = await _run_tool_corpus(ref)
    return dict(
        tool=tool_rows,
        expand=_expand_rows(ref),
        unique=_uniqueness_rows(ref),
        escape=_escape_rows(ref),
        render=_render_rows(ref),
        bon_render=_bon_render_rows(ref),
        markers=_marker_rows(ref),
        retries=_retry_rows(ref),
        limiter=await _limiter_rows(ref),
        review=_review_rows(ref),
        messages=_message_rows(ref),
        select=await _select_rows(ref),
        samples=await _sample_rows(ref),
        bons=await _bon_rows(ref),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when the checked-in .inc is out of date")
    parser.add_argument("--python", default=None,
                        help="interpreter able to import kimix (default: the "
                             "kimi-agent venv)")
    parser.add_argument("--out", default=str(OUT_PATH))
    args = parser.parse_args()

    try:
        ref = _import_reference()
    except Exception as exc:  # pragma: no cover - environment issue
        fallback = Path(args.python) if args.python else DEFAULT_AGENT_PYTHON
        if fallback.is_file() and Path(sys.executable) != fallback:
            print("re-running under %s (%s)" % (fallback, exc), file=sys.stderr)
            cmd = [str(fallback), str(Path(__file__).resolve()), *sys.argv[1:]]
            return subprocess.call(cmd)
        print("cannot import the kimi-agent reference: %s" % exc, file=sys.stderr)
        return 2

    data = asyncio.run(_build(ref))
    text = _emit(data["tool"], data["expand"], data["unique"], data["escape"],
                 data["render"], data["bon_render"], data["markers"],
                 data["retries"], data["limiter"], data["review"],
                 data["messages"], data["select"], data["samples"],
                 data["bons"])
    out_path = Path(args.out)
    if args.check:
        existing = out_path.read_text(encoding="utf-8") if out_path.exists() else ""
        if existing != text:
            print("%s is out of date" % out_path, file=sys.stderr)
            return 1
        print("%s is up to date (%d tool cases)" % (out_path, len(data["tool"])))
        return 0
    out_path.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d tool cases, %d kernel cases)" % (
        out_path, len(data["tool"]),
        sum(len(v) for k, v in data.items() if k != "tool")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
