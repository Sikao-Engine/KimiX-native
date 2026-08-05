"""pytest fixture: make the compiled extension and the shim importable.

The compiled extension module ``runtime_py.pyd`` lives next to ``runtime.dll``
in the xmake targetdir (bin/<mode>, default ``bin/debug`` for debug builds).
The shim package lives in ``python/kimix_native``.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root


def _prepend(path):
    if path and path not in sys.path:
        sys.path.insert(0, path)


# Locate the xmake targetdir: prefer bin/debug, fall back to any bin/<mode>.
BIN = None
for mode in ("debug", "releasedbg", "release", "check"):
    cand = os.path.join(ROOT, "bin", mode)
    if os.path.isdir(cand):
        BIN = cand
        break
if BIN is None:
    bin_root = os.path.join(ROOT, "bin")
    if os.path.isdir(bin_root):
        for entry in sorted(os.listdir(bin_root)):
            cand = os.path.join(bin_root, entry)
            if os.path.isdir(cand):
                BIN = cand
                break
if BIN:
    _prepend(BIN)  # runtime_py.pyd + runtime.dll

_prepend(os.path.join(ROOT, "python"))  # kimix_native shim
