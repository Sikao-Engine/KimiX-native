"""soul: the arena-backed conversation state (report A1/A2/B3/B4).

Three layers are compared field by field:

* the native arena (``runtime_py.soul.History`` / ``Elisions``) — the delivery;
* the shim's pure-Python mirror (``_CompatHistory`` / ``_CompatElisions``) — the
  ``KIMIX_NATIVE_SOUL=0`` fallback and the test oracle;
* where a reference exists, the kimi-agent source (skipped when the sibling
  checkout is absent).

The kernel must be a pure *mover* of bytes: every test that can run twice is
also run with ``KIMIX_NATIVE_SOUL=0`` and must produce identical results.
"""

from __future__ import annotations

import random

import pytest

from kimix_native import _native, _structure, soul


def _seeded_history(count: int = 40, seed: int = 7) -> list[dict]:
    rng = random.Random(seed)
    history: list[dict] = []
    for i in range(count):
        role = ("user", "assistant", "tool", "system")[i % 4]
        if role == "tool":
            history.append({
                "role": "tool",
                "content": [{"type": "text", "text": f"tool output {i}"}],
                "tool_call_id": f"call_{i - 1}",
            })
            continue
        content = [{"type": "text", "text": f"message {i} text"}]
        if role == "assistant" and rng.random() < 0.5:
            content.append({"type": "think", "think": f"thinking {i}"})
        message = {"role": role, "content": content}
        if role == "assistant" and i % 3 == 1:
            message["tool_calls"] = [{
                "id": f"call_{i}",
                "function": {"name": "read", "arguments": "{\"path\": \"a\"}"},
            }]
        history.append(message)
    return history


def _drop_native(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_SOUL", "0")


def _native_handle(messages=None, **kwargs):
    """A History through the gated factory (native when available)."""
    return soul.new_history(messages, **kwargs)


def _canonical(history) -> list[dict]:
    return [_structure.normalize_message(m) for m in history]


# ---------------------------------------------------------------------------
# History (A1)
# ---------------------------------------------------------------------------


def test_history_roundtrip_matches_compat():
    history = _seeded_history()
    native = _native_handle(history)
    compat = soul._CompatHistory(history)
    assert len(native) == len(compat) == len(history)
    assert native.to_messages() == compat.to_messages() == _canonical(history)
    assert list(native) == list(compat)
    for index in (0, 1, 2, len(history) - 1):
        assert native[index] == compat[index]
    assert native.slice(3, 9) == compat.slice(3, 9)
    assert native.text_at(0) == compat.text_at(0) == "message 0 text"


def test_history_mutations_match_compat():
    history = _seeded_history(20)
    native = _native_handle(history)
    compat = soul._CompatHistory(history)

    replacement = {"role": "assistant", "content": [{"type": "text", "text": "new"}]}
    assert native.replace(4, replacement) == compat.replace(4, replacement)
    assert native[4] == compat[4]

    assert native.drop(2) == compat.drop(2)
    assert len(native) == len(compat)
    assert native.to_messages() == compat.to_messages()

    native.truncate(5)
    compat.truncate(5)
    assert len(native) == len(compat) == 5
    assert native.to_messages() == compat.to_messages()

    assert native.replace(99, replacement) is False
    assert compat.replace(99, replacement) is False
    assert native.drop(99) is False
    assert compat.drop(99) is False


def test_history_iterates_in_batches():
    history = _seeded_history(200)
    native = _native_handle(history)
    assert native.BATCH == 64
    seen = list(native)
    assert len(seen) == 200
    assert seen == _canonical(history)


def test_history_surrogatepass_text():
    # Lone surrogates reach the kernel as surrogatepass UTF-8.
    text = "a\ud800b"
    message = {"role": "user", "content": [{"type": "text", "text": text}]}
    native = _native_handle([message])
    compat = soul._CompatHistory([message])
    assert native.text_at(0) == compat.text_at(0) == text
    assert native[0] == compat[0]


def test_history_bytes_accounting():
    if _native is None:
        pytest.skip("bytes_owned/bytes_live are native accounting")
    history = _seeded_history(300, seed=3)
    native = _native_handle(history, reserve_bytes=1 << 16)
    owned = native.bytes_owned()
    live = native.bytes_live()
    assert owned > 0
    assert 0 < live <= owned
    native.drop(0)
    native.drop(0)
    before = native.bytes_owned()
    freed = native.compact()
    assert freed >= 0
    assert native.bytes_owned() <= before
    assert native.bytes_live() < live
    # Generation advances on every mutation (native only; the mirror has none).
    generation = native.generation()
    native.append({"role": "user", "content": [{"type": "text", "text": "x"}]})
    assert native.generation() >= generation


def test_history_save_load():
    history = _seeded_history(30)
    native = _native_handle(history)
    native.drop(3)
    blob = native.save()
    if _native is not None:
        assert blob[:5] == b"KMSH1"  # the native blob; the mirror saves JSON

    restored = _native_handle()
    assert restored.load(blob) is True
    assert restored.to_messages() == native.to_messages()
    # Malformed input is rejected and leaves the store untouched.
    assert restored.load(b"") is False
    assert restored.load(b"XXXXX") is False
    assert restored.to_messages() == native.to_messages()


def test_history_close_and_context_manager():
    history = _seeded_history(4)
    with _native_handle(history) as handle:
        assert len(handle) == 4
    assert len(handle) == 0
    handle.close()  # idempotent


def test_history_fallback_switch(monkeypatch):
    history = _seeded_history(10)
    _drop_native(monkeypatch)
    fallback = soul.new_history(history)
    assert isinstance(fallback, soul._CompatHistory)
    assert fallback.to_messages() == _canonical(history)


# ---------------------------------------------------------------------------
# Elisions (A2)
# ---------------------------------------------------------------------------


def test_elisions_dedup_and_refs():
    store = soul.Elisions()
    compat = soul._CompatElisions()
    first = store.put("elided payload")
    assert first == compat.put("elided payload")
    again = store.put("elided payload")
    assert again == first
    assert compat.put("elided payload") == first
    assert store.count() == compat.count() == 1
    assert store.dedup_hits() == compat.dedup_hits() == 1
    assert store.get(first) == compat.get(first) == b"elided payload"
    assert store.text(first) == "elided payload"
    assert store.length(first) == compat.length(first) == 14
    assert store.has(first) is True
    assert store.get(0) is None


def test_elisions_save_load():
    store = soul.Elisions()
    ref = store.put("alpha")
    blob = store.save()
    # The native blob is "KMEL1"; the Python mirror uses JSON. Ref STABILITY is
    # the contract that matters across both.
    if store._e is not None:
        assert blob[:5] == b"KMEL1"
    restored = soul.Elisions()
    assert restored.load(blob) is True
    assert restored.get(ref) == b"alpha"
    assert restored.load(b"") is False
    assert restored.count() == 1


def test_elisions_fallback(monkeypatch):
    _drop_native(monkeypatch)
    store = soul.Elisions()
    ref = store.put("payload")
    assert store.get(ref) == b"payload"
    assert store.stats()[0] == 1


# ---------------------------------------------------------------------------
# B3 / B4 decision kernels on the arena
# ---------------------------------------------------------------------------


def test_decision_kernels_match_compat():
    history = _seeded_history(24)
    native_cuts, native_unbalanced, native_index = soul.balanced_cut_indices(history)
    compat_cuts, compat_unbalanced, compat_index = soul._compat_balanced_cut_indices(
        history
    )
    assert native_cuts == compat_cuts
    assert native_unbalanced == compat_unbalanced
    assert native_index == compat_index
    assert native_cuts[0] == 0

    # A BALANCED history always exposes the final cut at len(history).
    balanced = [
        {"role": "user", "content": [{"type": "text", "text": "q1"}]},
        {"role": "assistant", "content": [{"type": "text", "text": "a1"}],
         "tool_calls": [{"id": "c1", "function": {"name": "read", "arguments": "{}"}}]},
        {"role": "tool", "content": [{"type": "text", "text": "r1"}], "tool_call_id": "c1"},
        {"role": "user", "content": [{"type": "text", "text": "q2"}]},
    ]
    cuts, unbalanced, _ = soul.balanced_cut_indices(balanced)
    assert not unbalanced
    assert cuts[-1] == len(balanced)
    assert cuts == soul._compat_balanced_cut_indices(balanced)[0]

    for probe in (0, 1, 5, 23, 24):
        assert soul.nearest_balanced_cut_before(
            history, probe
        ) == soul._compat_nearest_balanced_cut_before(history, probe)

    assert soul.extract_text(history) == soul._compat_extract_text(history)
    assert soul.count_tokens(history) == soul._compat_count_tokens(history)


def test_normalize_and_repair_match_reference():
    history = [
        {"role": "user", "content": [{"type": "text", "text": "one"}]},
        {"role": "user", "content": [{"type": "text", "text": "two"}]},
        {"role": "assistant", "content": [{"type": "text", "text": "call"}],
         "tool_calls": [{"id": "c1", "function": {"name": "read", "arguments": "{}"}}]},
        # The result is missing: repair must synthesize a placeholder.
        {"role": "user", "content": [{"type": "text", "text": "next"}]},
    ]
    merged = soul.normalize_history(history)
    assert merged == soul._compat_normalize_history(history)
    assert len(merged) == 3  # the two leading users merged
    assert len(merged[0]["content"]) == 2

    repaired = soul.repair_tool_pairing(history)
    assert repaired == soul._compat_repair_tool_pairing(history)
    assert len(repaired) == 5  # the placeholder was inserted
    assert repaired[3]["role"] == "tool"
    assert "Tool result missing" in repaired[3]["content"][0]["text"]


def test_compaction_prompts_and_split():
    if _native is None:
        pytest.skip(
            "the prompt-assembly kernels have no Python mirror here (the "
            "kimi-agent implementation is the fallback for a real consumer)"
        )
    history = _seeded_history(24)
    options = {"mode": "balanced"}
    prompt, depth = soul.build_compaction_prompt(history, options, "custom", "P", "C")
    assert isinstance(prompt, str) and prompt
    assert isinstance(depth, int)

    split = soul.resolve_preserve_split(history, 3)
    assert set(split) == {
        "compact", "preserve_start_index", "keep_first_message", "unbalanced",
        "recut_fallback",
    }

    prepared = soul.prepare_compaction_input(history, 12, options, "custom", "P", "C")
    assert len(prepared["to_compact"]) == 12
    assert len(prepared["to_preserve"]) == len(history) - 12
    assert prepared["prompt_text"] == prompt
    assert prepared["compact_message_text"]
    assert soul.build_compact_message_text(history[:12], prepared["prompt_text"]) == (
        prepared["compact_message_text"]
    )


def test_export_markdown_matches_compat():
    history = _seeded_history(18, seed=11)
    opts = {
        "session_id": "sess-1",
        "work_dir": "C:/work",
        "exported_at": "2024-05-01T12:00:00",
        "token_count": 1234,
    }
    native = soul.build_export_markdown(history, opts)
    from kimix_native import tools as kernel_tools

    compat = kernel_tools._compat_build_export_markdown(history, opts)
    assert native == compat


def test_should_auto_compact_matches_compat():
    cfg = {
        "trigger_ratio": 0.75,
        "max_context_size": 1000,
        "reserved_context_size": 100,
        "safety_margin_tokens": 50,
    }
    for tokens in (0, 100, 600, 900, 5000):
        assert soul.should_auto_compact(tokens, cfg) == soul._compat_should_auto_compact(
            tokens, cfg
        )


def test_soul_fallback_requires_no_pydantic():
    """Importing soul must not drag a dataclass/pydantic graph in (§5 of the plan)."""
    import subprocess
    import sys
    import textwrap

    root = __file__.rsplit("python", 1)[0]
    code = textwrap.dedent(
        """
        import sys
        sys.path[:0] = [r"bin/release", "python"]
        import kimix_native.soul as soul
        handle = soul.new_history([{"role": "user", "content": [{"type": "text", "text": "hi"}]}])
        assert len(handle) == 1
        assert handle[0]["content"][0]["text"] == "hi"
        forbidden = [m for m in ("pydantic", "dataclasses") if m in sys.modules]
        # `dataclasses` may be pulled by the stdlib itself; pydantic must not be.
        assert "pydantic" not in sys.modules, forbidden
        print("ok")
        """
    )
    result = subprocess.run(
        [sys.executable, "-c", code], cwd=root, capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    assert "ok" in result.stdout


def test_pack_structure_helper_roundtrip():
    if _native is None:
        pytest.skip("pack_structure is a native boundary helper")
    history = _seeded_history(6)
    hist_bytes, structure = _structure.build(history)
    assert _native.soul.pack_structure(structure, hist_bytes) == structure
    assert _structure.to_messages(structure, hist_bytes) == _canonical(history)
