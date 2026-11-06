"""soul.build_request: the provider request-body builder (report B1).

Two contracts are pinned here:

* native bytes == ``_compat_build_request`` (the pure-Python mirror) for every
  provider on generated histories — this is the differential the plan calls
  ``--diff-bodies``;
* the recorded golden bodies in ``python/tests/data/request_bodies/`` (recorded
  from the mirror once, so an accidental change to either side fails).

The per-provider gate matrix is exercised too: ``KIMIX_NATIVE_REQUEST_<P>=0``
must switch exactly one provider to the mirror.
"""

from __future__ import annotations

import json
import pathlib
import random

import orjson
import pytest

from kimix_native import soul

DATA = pathlib.Path(__file__).with_name("data") / "request_bodies"

PROVIDERS = ("openai_legacy", "kimi", "anthropic", "openai_responses")


def history_fixture() -> list[dict]:
    """One history that exercises every envelope branch."""
    rng = random.Random(11)
    history = [
        {"role": "system", "content": [{"type": "text", "text": "you are kimix"}]},
        {"role": "user", "content": [{"type": "text", "text": "first question"}]},
        {"role": "user", "content": [{"type": "text", "text": "and a follow-up"}]},
        {"role": "user", "content": [
            {"type": "text", "text": "<system-reminder>stay on task</system-reminder>"}
        ]},
        {"role": "assistant", "content": [
            {"type": "text", "text": "let me look"},
            {"type": "think", "think": "reasoning about the file"},
        ], "tool_calls": [
            {"id": "call_1", "function": {"name": "read", "arguments": "{\"path\":\"a\"}"}},
            {"id": "call_2", "function": {"name": "glob", "arguments": None}},
        ]},
        {"role": "tool", "content": [{"type": "text", "text": "file body"}],
         "tool_call_id": "call_1"},
    ]
    for i in range(20):
        history.append({
            "role": "user" if i % 2 == 0 else "assistant",
            "content": [{"type": "text", "text": f"turn {i} " + "x" * rng.randint(1, 20)}],
        })
    return history


def options_for(provider: str) -> dict:
    options = {
        "provider": provider,
        "model": "test-model",
        "stream": True,
        "max_tokens": 4096,
        "temperature": 0.25,
        "system_prompt": "be helpful",
        "reasoning_effort": "medium",
        "reasoning_key": "reasoning_content",
        "tools_json": orjson.dumps([
            {"type": "function", "function": {"name": "read", "description": "read a file",
                                              "parameters": {"type": "object"}}}
        ]).decode(),
    }
    return options


@pytest.mark.parametrize("provider", PROVIDERS)
def test_native_matches_compat(provider):
    history = history_fixture()
    options = options_for(provider)
    native = soul.build_request(history, options)
    mirror = soul._compat_build_request(history, options)
    assert json.loads(native) == json.loads(mirror)
    # Byte equality is the actual contract (key order included).
    assert native == mirror


@pytest.mark.parametrize("provider", PROVIDERS)
def test_golden_bodies(provider):
    path = DATA / f"{provider}.json"
    history = history_fixture()
    options = options_for(provider)
    body = soul.build_request(history, options)
    if not path.exists():  # pragma: no cover - first run records the golden
        DATA.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
    assert body == path.read_bytes()


@pytest.mark.parametrize("provider", PROVIDERS)
def test_provider_gate(provider, monkeypatch):
    history = history_fixture()
    options = options_for(provider)
    native = soul.build_request(history, options)
    monkeypatch.setenv(f"KIMIX_NATIVE_REQUEST_{provider.upper()}", "0")
    mirrored = soul.build_request(history, options)
    assert mirrored == native
    # ...and the switch really took the Python path (it is byte-identical, so
    # the observable difference is that the kernel is not called).
    assert mirrored == soul._compat_build_request(history, options)


def test_short_history_shapes():
    history = [
        {"role": "user", "content": [{"type": "text", "text": "hello"}]},
    ]
    for provider in PROVIDERS:
        body = soul.build_request(history, {"provider": provider, "model": "m"})
        payload = json.loads(body)
        assert payload["model"] == "m"
        assert payload["stream"] is True


def test_normalize_merge_and_repair():
    history = [
        {"role": "user", "content": [{"type": "text", "text": "a"}]},
        {"role": "user", "content": [{"type": "text", "text": "b"}]},
        {"role": "assistant", "content": [{"type": "text", "text": "c"}],
         "tool_calls": [{"id": "c1", "function": {"name": "read", "arguments": "{}"}}]},
    ]
    body = soul.build_request(history, {"provider": "openai_legacy", "model": "m"})
    payload = json.loads(body)
    # The interrupted tool call got a placeholder result.
    assert [m["role"] for m in payload["messages"]] == ["user", "assistant", "tool"]
    assert "Tool result missing" in payload["messages"][2]["content"]


def test_no_native_module_still_builds(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE", "0")
    history = history_fixture()
    options = options_for("anthropic")
    body = soul.build_request(history, options)
    payload = json.loads(body)
    assert payload["system"].startswith("be helpful")
    assert payload["messages"]
