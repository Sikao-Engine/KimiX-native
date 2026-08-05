"""Parity tests for kimix_native.json plan-016 additions (JsonStore,
SchemaOps, notification batch scan).

Native kernels vs the pure-Python ``_compat`` mirrors (and, when importable,
against the real kosong/jsonschema reference) on:
- JsonStore op sequences: get() bytes equal, merged dict equal, save_atomic
  file bytes equal (orjson OPT_INDENT_2 style)
- schema deref with registry + cycle parity (vs kosong jsonschema reference)
- ensure_property_types parity
- notification scan ordering + merged view dict equality
"""

import json
import os
import random
import sys
import tempfile

import pytest

from kimix_native import json as kj

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KOSONG = r"C:\dev\kimi-agent\kimi-cli\packages\kosong\src"

_REF = None


def _ref_module():
    global _REF
    if _REF is not None:
        return _REF
    try:
        if KOSONG not in sys.path:
            sys.path.insert(0, KOSONG)
        from kosong.utils.jsonschema import deref_json_schema, ensure_property_types  # noqa

        _REF = {"deref": deref_json_schema, "ensure": ensure_property_types}
    except Exception:
        _REF = None
    return _REF


def _dec(b):
    return b.decode("utf-8", "surrogatepass")


# ---------------------------------------------------------------------------
# JsonStore op-sequence parity
# ---------------------------------------------------------------------------


def test_json_store_op_sequence_parity():
    rng = random.Random(5)
    native = kj.JsonStore()
    compat_doc: dict = {}

    def apply(store, op, data):
        if op == "load":
            store.load(data)
            return
        store.update(data)

    def compat_apply(doc, op, data):
        if op == "load":
            try:
                parsed = json.loads(_dec(data))
                return parsed if isinstance(parsed, dict) else {}
            except ValueError:
                return {}
        try:
            parsed = json.loads(_dec(data))
        except ValueError:
            return doc
        if not isinstance(parsed, dict):
            return doc

        def merge(d, s):
            out = dict(d)
            for k, v in s.items():
                if isinstance(out.get(k), dict) and isinstance(v, dict):
                    out[k] = merge(out[k], v)
                else:
                    out[k] = v
            return out

        return merge(doc, parsed)

    ops = [
        ("load", b'{"spec": {"id": "t1", "kind": "bash"}, "runtime": {"status": "starting"}}'),
        ("update", b'{"runtime": {"status": "running", "pid": 123}, "control": {"kill_requested_at": null}}'),
        ("update", b'{"runtime": {"status": "completed", "exit_code": 0}}'),
        ("update", b'{"consumer": {"offset": 42}}'),
        ("load", b'{"fresh": {"a": [1, 2, 3]}}'),
        ("update", b'{"fresh": {"b": {"deep": {"x": true}}}}'),
        ("update", b"not json"),
        ("load", b"also not json"),
    ]
    for op, data in ops:
        apply(native, op, data)
        compat_doc = compat_apply(compat_doc, op, data)
        native_bytes = native.get()
        compat_bytes = kj._compat_indent2(compat_doc)
        assert native_bytes == compat_bytes, (op, data, native_bytes, compat_bytes)
        assert json.loads(_dec(native_bytes)) == compat_doc

    # keys() order parity
    assert native.keys() == list(compat_doc.keys())

    # Random op sequences.
    pool = [b'{"a": 1}', b'{"a": {"x": 1}}', b'{"b": [1, 2]}', b'{"c": null}',
            b'{"c": {"nested": {"deep": "v"}}}', b'{"d": 1.5}', b'{"e": true}']
    for seed in range(10):
        r = random.Random(seed)
        n = kj.JsonStore()
        c: dict = {}
        for _ in range(8):
            op = r.choice(["load", "update"])
            data = r.choice(pool)
            apply(n, op, data)
            c = compat_apply(c, op, data)
            assert n.get() == kj._compat_indent2(c)
            assert json.loads(_dec(n.get())) == c


def test_json_store_save_atomic_parity():
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "task.json")
        native = kj.JsonStore()
        native.load(b'{"spec": {"id": "t1"}, "runtime": {"status": "running"}}')
        native.update(b'{"runtime": {"updated_at": 1.5}}')
        blob = native.save_atomic(path)
        with open(path, "rb") as f:
            assert f.read() == blob
        # _compat store writes identical bytes.
        compat = kj.JsonStore()
        compat.load(b'{"spec": {"id": "t1"}, "runtime": {"status": "running"}}')
        compat.update(b'{"runtime": {"updated_at": 1.5}}')
        assert compat.get() == blob
        # No leftover tmp files.
        assert not os.path.exists(path + ".tmp")


def test_json_store_number_formatting():
    store = kj.JsonStore()
    store.load(b'{"i": 42, "f": 1.5, "big": 1e20, "neg": -3, "zero": 0.0, "s": "x"}')
    out = store.get()
    assert b'"i": 42' in out
    assert b'"f": 1.5' in out
    assert b'"big": 1e+20' in out
    assert b'"neg": -3' in out
    assert b'"zero": 0.0' in out


# ---------------------------------------------------------------------------
# SchemaOps parity
# ---------------------------------------------------------------------------


_SCHEMAS = [
    b'{"$ref": "#/$defs/A", "minLength": 2, "$defs": {"A": {"type": "string"}}}',
    b'{"properties": {"a": {"$ref": "#/definitions/B"}},'
    b' "definitions": {"B": {"$ref": "#/definitions/C"}, "C": {"type": "integer"}}}',
    b'{"items": {"$ref": "#/$defs/list/0"}, "$defs": {"list": [{"type": "number"}]}}',
    b'{"properties": {"a": {"$ref": "#/missing"}, "b": {"$ref": "https://x/y"}}}',
    b'{"type": "object", "properties": {"x": {"type": "string"}}}',
    b'{"$defs": {"A": {"type": "string", "minLength": 1}}}',
]


def test_deref_json_schema_parity():
    mod = _ref_module()
    for schema in _SCHEMAS:
        native = kj.deref_json_schema(schema, [])
        if mod is not None:
            ref = mod["deref"](json.loads(_dec(schema)))
            assert json.loads(_dec(native)) == ref, (schema, native, ref)
        else:
            compat = kj._compat_deref_json_schema(json.loads(_dec(schema)))
            assert json.loads(_dec(native)) == compat, schema
    # registry param is accepted (reserved; local refs only).
    assert kj.deref_json_schema(_SCHEMAS[0], [b'{"x": 1}']) == kj.deref_json_schema(_SCHEMAS[0], [])


def test_deref_cycle_parity():
    schema = (b'{"properties": {"a": {"$ref": "#/$defs/A"}},'
              b' "$defs": {"A": {"type": "object",'
              b' "properties": {"self": {"$ref": "#/$defs/A"}}}}}')
    native = kj.deref_json_schema(schema, [])
    mod = _ref_module()
    if mod is not None:
        ref = mod["deref"](json.loads(_dec(schema)))
        assert json.loads(_dec(native)) == ref, (native, ref)
    else:
        compat = kj._compat_deref_json_schema(json.loads(_dec(schema)))
        assert json.loads(_dec(native)) == compat
    # The cyclic $ref must be preserved and the $defs bucket kept.
    assert b'"$ref"' in native
    assert b'"$defs"' in native


def test_ensure_property_types_parity():
    mod = _ref_module()
    cases = [
        b'{"properties": {"x": {"enum": ["a", "b"]}}}',
        b'{"properties": {"x": {"const": 3}}}',
        b'{"properties": {"x": {"enum": [1, 2.5]}}}',
        b'{"properties": {"x": {"properties": {}}}}',
        b'{"properties": {"x": {"items": {}}}}',
        b'{"properties": {"x": {"maxLength": 5}}}',
        b'{"properties": {"x": {"maximum": 5}}}',
        b'{"properties": {"x": {}}}',
        b'{"properties": {"x": {"type": "object", "enum": ["a", "b"]}}}',
        b'{"properties": {"x": {"anyOf": [{"enum": ["a"]}]}}}',
        b'{"$defs": {"A": {"properties": {"p": {"enum": [true]}}}}}',
        b'{"properties": {"x": {"enum": [true, 1]}}}',
        b'{"properties": {"x": {"const": null}}}',
        b'{"properties": {"x": {"type": "string", "enum": [1, 2]}}}',
        b'{"properties": {"x": {"type": "array", "minItems": 1}}}',
    ]
    for schema in cases:
        native = kj.ensure_property_types(schema)
        if mod is not None:
            ref = mod["ensure"](json.loads(_dec(schema)))
            assert json.loads(_dec(native)) == ref, (schema, native, ref)
        else:
            compat = kj._compat_ensure_property_types(json.loads(_dec(schema)))
            assert json.loads(_dec(native)) == compat, schema


# ---------------------------------------------------------------------------
# Notification batch scan
# ---------------------------------------------------------------------------


def test_scan_notifications_ordering():
    rng = random.Random(13)
    lines = []
    views = []
    for i in range(40):
        created = rng.uniform(0, 1000)
        event = {"id": f"n{i:02d}", "created_at": created, "title": f"T{i}",
                 "category": "task"}
        delivery = {"sinks": {"llm": {"status": "pending"}}} if i % 3 == 0 else {}
        lines.append(json.dumps({"event": event, "delivery": delivery}))
        views.append({"id": f"n{i:02d}", "created_at": created,
                      "event": event, "delivery": delivery})
    jsonl = ("\n".join(lines) + "\n").encode()
    rows = kj.scan_notifications(jsonl, 0)
    expected = sorted(views, key=lambda v: v["created_at"], reverse=True)
    assert len(rows) == len(expected)
    for row, exp in zip(rows, expected):
        assert row["id"] == exp["id"]
        assert abs(row["created_at"] - exp["created_at"]) < 1e-9
        assert row["event"] == exp["event"]
        assert row["delivery"] == exp["delivery"]


def test_scan_notifications_ties_and_malformed():
    jsonl = (
        '{"event": {"id": "a", "created_at": 1.0}}\n'
        '{"event": {"id": "b", "created_at": 3.0}}\n'
        'garbage\n'
        '{"event": {"id": "c", "created_at": 3.0}}\n'
        '{"event": {"id": "d", "created_at": 2.0}}\n'
    ).encode()
    rows = kj.scan_notifications(jsonl, 0)
    # Stable sort: 3.0 ties keep input order (b before c).
    assert [r["id"] for r in rows] == ["b", "c", "d", "a"]


def test_native_disabled_fallback_json():
    """KIMIX_NATIVE_JSON=0 keeps JsonStore/SchemaOps behavior identical."""
    import subprocess
    env = dict(os.environ, KIMIX_NATIVE_JSON="0")
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import json as kj\n"
        "s = kj.JsonStore()\n"
        "s.load(b'{\"a\": 1}')\n"
        "s.update(b'{\"b\": {\"x\": 2}}')\n"
        "assert b'\"a\": 1' in s.get()\n"
        "assert s.keys() == ['a', 'b']\n"
        "assert kj.deref_json_schema(b'{\"$ref\": \"#/$defs/A\", \"$defs\": {\"A\": {\"type\": \"string\"}}}', []) == b'{\"type\":\"string\"}'\n"
        "assert kj.ensure_property_types(b'{\"properties\": {\"x\": {\"enum\": [\"a\"]}}}') == b'{\"properties\":{\"x\":{\"enum\":[\"a\"],\"type\":\"string\"}}}'\n"
        "print('JSON_FALLBACK_OK')\n"
    ) % (os.path.join(ROOT, "python"), os.path.join(ROOT, "bin", "debug"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "JSON_FALLBACK_OK" in r.stdout
