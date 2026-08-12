"""Parity tests for kimix_native.todo (todo kernels).

Compares the native kernels against the pure-Python ``_compat`` mirrors
(python/kimix_native/todo.py fallback body) on:
- merge: append / overwrite / force_overwrite modes, duplicate titles,
  invalid status/title errors, regression clamping, auto-fix of multiple
  in_progress, archived-completed detection, fuzzy-warning forwarding
- status_counts: canonical statuses, invalid values skipped
- format_summary: pending/in_progress selection, code kind labels, notes
- the KIMIX_NATIVE_TODO=0 fallback toggle
"""
import os
import random
import subprocess
import sys

from kimix_native import todo

ITEM_SETS = [
    [],
    [{"title": "A", "status": "pending"}],
    [{"title": "A", "status": "done"}],
    [
        {"title": "A", "status": "pending"},
        {"title": "B", "status": "in_progress", "notes": "n1"},
        {"title": "C", "status": "done", "code": "!py x.py"},
    ],
    [
        {"title": "X", "status": "in_progress", "code": "x.py"},
        {"title": "Y", "status": "in_progress"},
    ],
    # tree: a parent with a nested child
    [
        {
            "title": "P",
            "status": "pending",
            "children": [
                {"title": "C1", "status": "pending"},
                {"title": "C2", "status": "done", "children": [{"title": "G", "status": "in_progress"}]},
            ],
        }
    ],
]

MODES = ["append", "overwrite", "force_overwrite"]


def test_merge_parity_all_modes():
    for mode in MODES:
        for old in ITEM_SETS:
            for new in ITEM_SETS:
                native = todo.merge(old, new, mode)
                # force the python fallback body via a fresh pure call
                fallback = _merge_fallback(old, new, mode)
                assert native == fallback, (mode, old, new)


def _merge_fallback(old, new, mode, fuzzy=None):
    """Direct call into the shim's pure-Python body (bypasses native gate)."""
    if fuzzy is None:
        fuzzy = {}
    saved = todo.use_native
    todo.use_native = lambda _k: False
    try:
        return todo.merge(old, new, mode, fuzzy)
    finally:
        todo.use_native = saved


def test_merge_duplicate_titles():
    dup = [
        {"title": "A", "status": "pending"},
        {"title": "A", "status": "pending"},
    ]
    native = todo.merge([], dup, "append")
    fallback = _merge_fallback([], dup, "append", {})
    assert native == fallback
    assert native["error"] and "Duplicate todo titles" in native["error"]


def test_merge_invalid_status():
    bad = [{"title": "A", "status": "weird"}]
    native = todo.merge([], bad, "append")
    fallback = _merge_fallback([], bad, "append", {})
    assert native == fallback
    assert native["error"] and "Invalid todo at index 0" in native["error"]


def test_merge_overwrite_blocked():
    old = [{"title": "A", "status": "pending"}]
    new = [{"title": "B", "status": "done"}]
    native = todo.merge(old, new, "overwrite")
    fallback = _merge_fallback(old, new, "overwrite", {})
    assert native == fallback
    assert native["error"] and "Cannot overwrite" in native["error"]
    # force_overwrite proceeds
    forced = todo.merge(old, new, "force_overwrite")
    assert forced["error"] is None
    # items are canonicalized (notes/code default to None, children to []), so
    # compare the canonical form.
    expected = [
        {"title": "B", "status": "done", "notes": None, "code": None, "children": []}
    ]
    assert forced["items"] == expected


def test_merge_regression_clamping():
    old = [{"title": "A", "status": "done"}, {"title": "B", "status": "done"}]
    new = [{"title": "A", "status": "pending"}, {"title": "B", "status": "done"}]
    native = todo.merge(old, new, "append")
    fallback = _merge_fallback(old, new, "append", {})
    assert native == fallback
    assert native["regressed"] == ["A"]
    by_title = {i["title"]: i["status"] for i in native["items"]}
    assert by_title["A"] == "done"


def test_merge_auto_fix_single_in_progress():
    old = [{"title": "A", "status": "pending"}, {"title": "B", "status": "pending"}]
    new = [{"title": "A", "status": "in_progress"}, {"title": "B", "status": "in_progress"}]
    native = todo.merge(old, new, "append")
    fallback = _merge_fallback(old, new, "append", {})
    assert native == fallback
    by_title = {i["title"]: i["status"] for i in native["items"]}
    assert list(by_title.values()).count("in_progress") == 1
    assert any("Auto-fixed" in w for w in native["warnings"])


def test_merge_fuzzy_warnings_and_archived():
    old = [{"title": "A", "status": "done"}]
    new = [{"title": "B", "status": "pending"}]
    fuzzy = {"B": ["did you mean X?"]}
    native = todo.merge(old, new, "append", fuzzy)
    fallback = _merge_fallback(old, new, "append", fuzzy)
    assert native == fallback
    assert native["warnings"] == ["did you mean X?"]
    # A stays in the list (kept title) so it is not archived.
    assert native["archived"] == []
    # A done item REMOVED from the kept list is archived (append + empty new
    # clears the list when nothing is unfinished).
    old2 = [{"title": "A", "status": "done"}]
    native2 = todo.merge(old2, [], "append")
    fallback2 = _merge_fallback(old2, [], "append", {})
    assert native2 == fallback2
    assert native2["archived"] == [
        {"title": "A", "status": "done", "notes": None, "code": None, "children": []}
    ]


def test_merge_random_parity():
    rng = random.Random(42)
    statuses = ["pending", "in_progress", "done", "PENDING", "in-progress"]
    for _ in range(50):
        old = [
            {"title": f"T{rng.randrange(5)}", "status": rng.choice(statuses)}
            for _ in range(rng.randrange(0, 4))
        ]
        new = [
            {"title": f"T{rng.randrange(5)}", "status": rng.choice(statuses)}
            for _ in range(rng.randrange(0, 4))
        ]
        mode = rng.choice(MODES)
        native = todo.merge(old, new, mode)
        fallback = _merge_fallback(old, new, mode, {})
        assert native == fallback, (mode, old, new)


def test_merge_preserves_children_on_same_title_update():
    """A same-title update without children keeps the old subtree; an update
    that carries its own (non-empty) children replaces it."""
    old = [{"title": "P", "status": "pending", "children": [{"title": "C", "status": "pending"}]}]
    # same-title update, no children -> old children preserved
    native = todo.merge(old, [{"title": "P", "status": "done"}], "append")
    fallback = _merge_fallback(old, [{"title": "P", "status": "done"}], "append", {})
    assert native == fallback
    assert native["items"][0]["children"] == [
        {"title": "C", "status": "pending", "notes": None, "code": None, "children": []}
    ]
    # same-title update WITH children -> children replaced
    native2 = todo.merge(
        old, [{"title": "P", "status": "pending", "children": [{"title": "C2", "status": "pending"}]}], "append"
    )
    fallback2 = _merge_fallback(
        old, [{"title": "P", "status": "pending", "children": [{"title": "C2", "status": "pending"}]}], "append", {}
    )
    assert native2 == fallback2
    assert native2["items"][0]["children"] == [
        {"title": "C2", "status": "pending", "notes": None, "code": None, "children": []}
    ]
    # force_overwrite keeps the incoming children verbatim (canonicalized)
    forced = todo.merge([], old, "force_overwrite")
    fallback_forced = _merge_fallback([], old, "force_overwrite", {})
    assert forced == fallback_forced
    assert forced["items"][0]["children"][0]["title"] == "C"


def test_merge_canonicalizes_nested_children():
    """Nested children are canonicalized recursively (status normalization,
    notes/code/children defaults)."""
    items = [
        {
            "title": "P",
            "status": "In-Progress",
            "children": [{"title": "C", "status": "DONE", "code": "x.py"}],
        }
    ]
    native = todo.merge([], items, "append")
    fallback = _merge_fallback([], items, "append", {})
    assert native == fallback
    assert native["items"] == [
        {
            "title": "P",
            "status": "in_progress",
            "notes": None,
            "code": None,
            "children": [
                {"title": "C", "status": "done", "notes": None, "code": "x.py", "children": []}
            ],
        }
    ]


def test_merge_invalid_child_status_errors():
    """An invalid status inside a child errors identically to a top-level item."""
    items = [
        {"title": "P", "status": "pending", "children": [{"title": "C", "status": "bogus"}]}
    ]
    native = todo.merge([], items, "append")
    fallback = _merge_fallback([], items, "append", {})
    assert native == fallback
    assert native["error"] and "Invalid status" in native["error"]


def test_status_counts_parity():
    items = [
        {"title": "A", "status": "pending"},
        {"title": "B", "status": "in_progress"},
        {"title": "C", "status": "done"},
        {"title": "D", "status": "bogus"},
    ]
    native = todo.status_counts(items)
    assert native == {"pending": 1, "in_progress": 1, "done": 1}
    assert todo.status_counts([]) == {"pending": 0, "in_progress": 0, "done": 0}


def test_format_summary_parity():
    items = [
        {"title": "A", "status": "pending"},
        {"title": "B", "status": "in_progress", "notes": "note", "code": "!run"},
        {"title": "C", "status": "done"},
        {"title": "D", "status": "pending", "code": "x.py"},
    ]
    native = todo.format_summary(items, 50)
    fallback = _format_summary_fallback(items, 50)
    assert native == fallback
    assert "pending" in native and "in progress" in native
    assert "done" not in native
    assert "Notes: note" in native
    # max_items truncation
    many = [{"title": f"T{i}", "status": "pending"} for i in range(10)]
    assert len(todo.format_summary(many, 3).splitlines()) == 3
    # empty / all-done
    assert todo.format_summary([]) == ""
    assert todo.format_summary([{"title": "A", "status": "done"}]) == ""


def _format_summary_fallback(items, max_items):
    saved = todo.use_native
    todo.use_native = lambda _k: False
    try:
        return todo.format_summary(items, max_items)
    finally:
        todo.use_native = saved


def test_native_disabled_fallback():
    """With KIMIX_NATIVE_TODO=0 the shim must behave identically."""
    env = dict(os.environ, KIMIX_NATIVE_TODO="0")
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import todo\n"
        "assert todo.use_native('TODO') is False\n"
        "r = todo.merge([{'title':'A','status':'pending'}], "
        "[{'title':'A','status':'done'}], 'append')\n"
        "assert r['error'] is None and r['items'][0]['status'] == 'done', r\n"
        "assert todo.status_counts([]) == {'pending':0,'in_progress':0,'done':0}\n"
        "assert todo.format_summary([]) == ''\n"
        "print('FALLBACK_OK')\n"
    ) % (os.path.join(root, "python"), os.path.join(root, "bin", "release"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
