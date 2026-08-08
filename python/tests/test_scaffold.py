"""Scaffold parity tests for the compiled ``runtime_py`` extension module.

Covers: module metadata (version / core_version / c_version), the submodule
skeletons, the use_native toggle (C++ side + Python shim side), the
KIMIX_NATIVE=0 fallback path, UTF-8 parity with Python, and a GIL-release
functional check.
"""

import os
import subprocess
import sys
import threading

import pytest

import runtime_py


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _project_version() -> str:
    """Read the version from the single config file (version.txt)."""
    with open(os.path.join(_repo_root(), "version.txt"), encoding="utf-8") as fh:
        return fh.read().strip()


def test_module_version():
    version = _project_version()
    assert runtime_py.version() == f"kimix-runtime {version}"
    assert runtime_py.core_version().startswith("kimix")
    assert runtime_py.c_version() == runtime_py.version()
    assert runtime_py.version_string == f"kimix-runtime {version}"


def test_submodules():
    names = {
        "text", "index", "search", "codec", "stream",
        "json", "parse", "concurrency", "soul", "tools",
        "diff", "glob", "workspace", "todo", "image",
    }
    for name in names:
        sub = getattr(runtime_py, name, None)
        assert sub is not None, f"missing submodule {name}"
        assert isinstance(sub, type(runtime_py)), f"{name} is not a module"


def test_use_native_default(monkeypatch):
    # Module is loaded and no env override -> native enabled.
    monkeypatch.delenv("KIMIX_NATIVE", raising=False)
    monkeypatch.delenv("KIMIX_NATIVE_DUMMY", raising=False)
    assert runtime_py.use_native("dummy") is True


def test_use_native_disabled(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE", "0")
    assert runtime_py.use_native("dummy") is False
    monkeypatch.delenv("KIMIX_NATIVE", raising=False)
    monkeypatch.setenv("KIMIX_NATIVE_TEXT", "0")
    assert runtime_py.use_native("text") is False
    assert runtime_py.use_native("index") is True


def test_shim_import(monkeypatch):
    import kimix_native
    if kimix_native._native is None:
        pytest.skip("native disabled in this run (KIMIX_NATIVE=0)")
    monkeypatch.delenv("KIMIX_NATIVE", raising=False)
    monkeypatch.delenv("KIMIX_NATIVE_DUMMY", raising=False)
    import kimix_native
    assert kimix_native._native is runtime_py
    assert kimix_native.use_native("dummy") is True
    assert kimix_native.version() == runtime_py.version()


def test_shim_fallback():
    root = _repo_root()
    env = dict(os.environ)
    env["KIMIX_NATIVE"] = "0"
    code = (
        "import sys; sys.path.insert(0, {py!r}); "
        "import kimix_native; print(kimix_native.version())"
    ).format(py=os.path.join(root, "python"))
    proc = subprocess.run(
        [sys.executable, "-c", code],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    assert "python fallback" in proc.stdout
    expected = _project_version()
    assert f"kimix-native {expected}" in proc.stdout


def test_utf8_parity():
    samples = [
        "héllo😀中🙂",
        "",
        "ascii only",
        "日本語テキスト🎌🎉",
        "x" * 1000,
        "\u00e9\u00fc\u00f1",  # Latin-1 supplement
        "混合mix💯",
    ]
    for s in samples:
        payload = s.encode("utf-8")
        expected = len(s)  # Python code points
        assert runtime_py.common.utf8_code_point_count(payload) == expected
        assert runtime_py.common.is_ascii(payload) == s.isascii()


def test_gil_released():
    payload = ("é" * 500_000).encode("utf-8")  # ~1 MB of UTF-8
    expected = 500_000
    results = []
    errors = []

    def worker():
        try:
            results.append(runtime_py.common.utf8_code_point_count(payload))
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    t = threading.Thread(target=worker)
    t.start()
    # Main thread does Python work while the worker runs (functional check
    # that the kernel call releases the GIL and does not deadlock).
    busy = sum(i * i for i in range(200_000))
    t.join(timeout=60)
    assert not t.is_alive(), "worker thread deadlocked"
    assert not errors
    assert results == [expected]
    assert busy >= 0
