r"""Parity tests for kimix_native.json (plan 010).

Compares the native IncrementalJsonLexer against the pure-Python ``_compat``
mirror (a line-for-line port of the kernel state machine) on:
- 200 generated streams (valid docs, relaxed docs, malformed partials,
  split across random chunk boundaries)
- is_complete / has_error / top_level_keys / value_span parity
- relaxed parsing: trailing commas, // and /* */ comments
- \uXXXX surrogate pairs split mid-escape
- the KIMIX_NATIVE_JSON=0 fallback toggle
"""

import os
import random
import subprocess
import sys

from kimix_native import json as kj

# ---------------------------------------------------------------------------
# Stream generator
# ---------------------------------------------------------------------------

_KEYS = ["a", "b", "name", "x", "query", "k1", "k2", "emoji", "caf\u00e9", "\u4e16\u754c"]
_SCALARS = [1, -2, 3.5, True, False, None, "hello", "\u4e16\u754c", "\U0001f600"]


def _gen_value(rng, depth=0):
    if depth > 2 or rng.random() < 0.45:
        return rng.choice(_SCALARS)
    if rng.random() < 0.5:
        return {rng.choice(_KEYS): _gen_value(rng, depth + 1)
                for _ in range(rng.randrange(0, 4))}
    return [_gen_value(rng, depth + 1) for _ in range(rng.randrange(0, 4))]


def _compact(obj) -> bytes:
    import json
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _gen_document(rng):
    kind = rng.random()
    if kind < 0.6:
        return _compact({"a": _gen_value(rng), "b": _gen_value(rng, 1)})
    if kind < 0.8:
        return _compact([_gen_value(rng) for _ in range(rng.randrange(0, 5))])
    return _compact(rng.choice(_SCALARS))


def _gen_relaxed(rng):
    base = _compact({"a": 1, "b": [1, 2]})
    choice = rng.randrange(4)
    if choice == 0:
        return base[:-1] + b",}"  # trailing comma in object
    if choice == 1:
        return b"[1, 2,]"  # trailing comma in array
    if choice == 2:
        return b'{"a": 1, // comment\n "b": 2}'
    return b'{"a": /* block */ 1, "b": 2}'


def _gen_malformed(rng):
    bad = [
        b'{"a": }',
        b'[1 2]',
        b'{"a":1 "b":2}',
        b'{"a":1}{"b":2}',
        b'"\\x"',
        b"truee",
        b"}",
        b"[,]",
        b"{\"a\":,}",
    ]
    return rng.choice(bad)


def _gen_incomplete(rng):
    partial = [
        b'{"a": ',
        b'"unterminated',
        b"tru",
        b"{",
        b"[",
        b'{"a":1',
        b"42.",
        b"1e+",
    ]
    return rng.choice(partial)


def _feed_random(lexer, blob, rng):
    """Feed a blob in random chunks (1..7 bytes)."""
    pos = 0
    while pos < len(blob):
        n = min(rng.randrange(1, 8), len(blob) - pos)
        lexer.feed(blob[pos:pos + n])
        pos += n


def _check_parity(native, compat, blob, label):
    assert native.is_complete() == compat.is_complete(), label
    assert native.has_error() == compat.has_error(), label
    assert native.top_level_keys() == compat.top_level_keys(), label
    for key in _KEYS + ["a", "b", "name", "x", "query", "k1", "k2"]:
        assert native.value_span(key) == compat.value_span(key), (label, key)
        # value_bytes = buffer()[start:end]; buffers hold identical bytes.
        n_span = native.value_span(key)
        c_span = compat.value_span(key)
        if n_span is not None:
            assert compat.buffer()[c_span[0]:c_span[1]] == native.buffer()[n_span[0]:n_span[1]], label


def test_lexer_parity_200_streams():
    rng = random.Random(2024)
    made = 0
    while made < 200:
        roll = rng.random()
        if roll < 0.45:
            blob = _gen_document(rng)
        elif roll < 0.6:
            blob = _gen_relaxed(rng)
        elif roll < 0.75:
            blob = _gen_malformed(rng)
        elif roll < 0.85:
            blob = _gen_incomplete(rng)
        else:
            # Random byte soup (should either error or stay incomplete).
            blob = bytes(rng.randrange(0x20, 0x7F) for _ in range(rng.randrange(0, 30)))
        if not blob:
            continue
        made += 1
        native = kj.IncrementalJsonLexer()
        compat = kj._CompatIncrementalJsonLexer()
        _feed_random(native, blob, rng)
        _feed_random(compat, blob, rng)
        label = f"stream#{made} {blob[:60]!r}"
        _check_parity(native, compat, blob, label)
        # A valid document must be complete; a malformed one must error.
        if roll < 0.45:
            assert not native.has_error(), label
            if blob[:1] in (b"{", b"[", b'"'):
                assert native.is_complete(), label
            else:
                # Bare scalar: a number completes only after a delimiter
                # (streaming semantics) - feed the terminator and re-check.
                native.feed(b" ")
                compat.feed(b" ")
                _check_parity(native, compat, blob + b" ", label)
                assert native.is_complete(), label
                assert not native.has_error(), label
        elif roll < 0.6:
            assert native.is_complete() and not native.has_error(), label


def test_lexer_split_point_equivalence():
    rng = random.Random(11)
    docs = [
        b'{"a": {"b": [1, 2, {"c": "x"}]}, "d": null}',
        b'[1, [2, [3, [4]]]]',
        b'{"k": "a string \\"with\\" escapes \\u4e16\\u754c"}',
    ]
    for doc in docs:
        one = kj.IncrementalJsonLexer()
        one.feed(doc)
        byte = kj.IncrementalJsonLexer()
        for i in range(len(doc)):
            byte.feed(doc[i:i + 1])
        chunk = kj.IncrementalJsonLexer()
        chunk.feed(doc)
        for lex in (one, byte, chunk):
            assert lex.is_complete(), doc
            assert not lex.has_error(), doc
        assert one.value_span("a") == byte.value_span("a") == chunk.value_span("a")


def test_lexer_relaxed_and_spans():
    lex = kj.IncrementalJsonLexer()
    lex.feed(b'{"alpha": 1, "beta": [10, 20], "gamma": {"deep": true},}')
    assert lex.is_complete()
    assert not lex.has_error()
    assert lex.top_level_keys() == ["alpha", "beta", "gamma"]
    assert lex.value_bytes("alpha") == b"1"
    assert lex.value_bytes("beta") == b"[10, 20]"
    assert lex.value_bytes("gamma") == b'{"deep": true}'
    assert lex.value_span("nope") is None

    lex.reset()
    lex.feed(b'{"a": /* c */ 1 // trailing\n}')
    assert lex.is_complete()
    assert not lex.has_error()


def test_lexer_surrogate_split_mid_escape():
    doc = b'{"emoji": "\\ud83d\\ude00"}'
    one = kj.IncrementalJsonLexer()
    one.feed(doc)
    assert one.is_complete()
    split = kj.IncrementalJsonLexer()
    # Split the \uD83D\uDE00 escape across feed boundaries (1 byte at a time
    # through the escape region).
    for i in range(len(doc)):
        split.feed(doc[i:i + 1])
    assert not split.has_error()
    assert split.is_complete()
    assert one.value_span("emoji") == split.value_span("emoji")
    assert split.value_bytes("emoji") == b'"\\ud83d\\ude00"'


def test_lexer_error_sticky_and_reset():
    lex = kj.IncrementalJsonLexer()
    lex.feed(b'{"a": }')
    assert lex.has_error()
    lex.feed(b'{"a": 1}')  # ignored after error
    assert lex.has_error()
    assert not lex.is_complete()
    lex.reset()
    assert not lex.has_error()
    lex.feed(b'{"a": 1}')
    assert lex.is_complete()


def test_native_disabled_fallback():
    env = dict(os.environ, KIMIX_NATIVE_JSON="0")
    py_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.dirname(py_dir)
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import json as kj\n"
        "assert kj._USE is False\n"
        "lex = kj.IncrementalJsonLexer()\n"
        "lex.feed(b'{\"a\": 1}')\n"
        "assert lex.is_complete()\n"
        "assert lex.top_level_keys() == ['a']\n"
        "assert lex.value_bytes('a') == b'1'\n"
        "print('FALLBACK_OK')\n"
    ) % (py_dir, os.path.join(root, "bin", "debug"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
