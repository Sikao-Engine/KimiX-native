"""Differential parity tests for the web_search builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/web_search_tool.{h,cpp}`` ports the pure kernels of
kimi-agent's ``web_search`` tool.  This module compares every kernel the
``runtime_py`` extension exposes through ``runtime_py.builtin_tools.web``
against the *original* implementation in the kimi-agent checkout
(``C:/dev/kimi-agent``, override with ``KIMI_AGENT_ROOT``):

* ``build_search_output`` (FOCUS 1) -- result shaping / numbering / title-URL-
  snippet layout / per-item content cap / the ToolResultBuilder total cap /
  truncation marker / deduplication / summary block / byte-exact messages.  The
  reference is the *real* ``SearchWeb.__call__`` (search.py) driven by a stub
  provider, so the comparison includes ``kimi_cli.tools.utils.ToolResultBuilder``
  exactly as the tool runs it.
* ``convert_base64_images_to_links`` (FOCUS 2) -- ``content.py``: the three
  ``regex`` substitutions, the ``[IMAGE: alt]`` / ``[IMAGE]`` replacement text,
  mime-type handling, malformed/oversized payloads and the whitespace classes
  (``regex`` ``\\s`` vs ``str.isspace()``).
* ``truncate_with_footer`` (FOCUS 3) -- ``content.py``: the char budget, the
  head/tail newline snapping, the stored-path / not-stored footer branches and
  the side effect on ``cache/web``.
* ``make_cache_slug`` / ``make_cache_file_name`` / ``store_full_text``
  (FOCUS 4) -- ``content.py``: ``xxhash.xxh64(url).hexdigest()[:10]`` (XXH64, NOT
  ``kimix::hash64``/XXH3), the urlparse hostname + slug pipeline, and the stored
  file bytes.
* ``clamp_search_limit`` / ``clamp_extract_char_limit`` (FOCUS 5) -- search.py
  ``Params`` bounds + ``content.py get_extract_char_limit``.

Provenance rules (learned the hard way):

* ``kimi_cli.native_loader`` inserts ``<kimi-agent>/bin`` (an older *released*
  ``runtime_py.pyd``) at ``sys.path[0]``, so ``runtime_py`` is imported FIRST and
  its ``__file__`` is asserted to be this checkout's build.
* The reference modules are resolved through ``_parity_ref.ref()``, which
  verifies the resolved ``__file__`` is under the kimi-agent checkout (a
  ``kimi_cli``/``kimix`` shim could otherwise shadow them).
* ``kimi_cli.tools.web.content`` / ``search`` are pure Python (no native gate) --
  no ``pure_python()`` wrapper is needed; the only thing that could short-circuit
  is the *search provider*, which this module replaces with a stub.

Known, documented deviations (asserted explicitly below so they cannot rot):

* ``store_full_text`` -- Python writes through ``Path.write_text`` in *text*
  mode, which translates ``\\n`` -> ``\\r\\n`` on Windows; the native writer
  emits the UTF-8 bytes verbatim (matching CPython on POSIX).  The tests assert
  exactly that relation and nothing else.
* ``clamp_search_limit`` -- pydantic *rejects* an out-of-range ``limit``
  (ValidationError); the kernel clamps (it is used by callers that skip
  validation), and the in-range mapping is identical.
* ``dedup_urls`` / ``summary`` / ``max_content_chars`` are opt-in extensions
  (search.py renders every item, has no summary and never caps one item's
  content); their default-off behaviour is asserted to be the reference one.
"""

from __future__ import annotations

import asyncio
import os
import random
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]


def _kimix_base_bin_dir():
    """The kimix-base build directory holding ``runtime_py`` (conftest's order)."""
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = _KIMIX_BASE_ROOT / "bin" / mode
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    for cand in sorted((_KIMIX_BASE_ROOT / "bin").glob("*")):
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    return None


# IMPORT ORDER IS LOAD-BEARING: import the freshly built kimix-base extension
# *before* touching the kimi-agent reference (see the module docstring).
_BIN_DIR = _kimix_base_bin_dir()
if _BIN_DIR is not None:
    sys.path.insert(0, str(_BIN_DIR))
import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip("no kimix-base runtime_py build found under bin/",
                allow_module_level=True)

_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build "
    f"{_BIN_DIR} -- a staged copy shadowed it; parity results would be bogus")

from _parity_ref import KIMI_AGENT_ROOT, KIMI_CLI_SRC, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

WEB = runtime_py.builtin_tools.web

# ---------------------------------------------------------------------------
# reference modules (loaded with explicit provenance)
# ---------------------------------------------------------------------------

_CONTENT = ref("kimi_cli.tools.web.content")
_SEARCH = ref("kimi_cli.tools.web.search")
_PROVIDERS = ref("kimi_cli.tools.web.providers")
_TOOL_UTILS = ref("kimi_cli.tools.utils")

for _mod, _name in ((_CONTENT, "kimi_cli.tools.web.content"),
                    (_SEARCH, "kimi_cli.tools.web.search"),
                    (_TOOL_UTILS, "kimi_cli.tools.utils")):
    assert str(KIMI_CLI_SRC.resolve()) in str(Path(_mod.__file__).resolve()), (
        f"reference {_name} resolved outside the kimi-agent checkout: "
        f"{_mod.__file__}")

#: search.py builds its result with ToolResultBuilder(max_line_length=None).
_TOOL_RESULT_MAX_CHARS = _TOOL_UTILS.DEFAULT_MAX_CHARS
_TOOL_RESULT_MARKER = _TOOL_UTILS.ToolResultBuilder(max_line_length=None)._marker

MOCK_GET_SHARE_DIR = "kimi_cli.config.get_share_dir"


class _StubTool:
    """Stands in for a SearchWeb instance (only ``_config`` is used)."""

    _config = None


class _StubProvider:
    """Provider returning a fixed provider-contract payload."""

    def __init__(self, web_items):
        self._items = web_items

    def search(self, query, limit=5, **kwargs):
        return {"success": True, "data": {"web": list(self._items)}}


def py_search(items, *, include_content=False, limit=5):
    """Run the *real* SearchWeb.__call__ rendering pipeline (search.py)."""
    provider = _StubProvider(items)
    with mock.patch.object(_PROVIDERS, "get_active_search_provider",
                           lambda _config=None: provider):
        return asyncio.run(
            _SEARCH.SearchWeb.__call__(
                _StubTool(),
                _SEARCH.Params(query="q", limit=limit,
                               include_content=include_content)))


def py_render(items, **kwargs):
    """Return the reference tool-result ``output`` for ``items``."""
    return py_search(items, **kwargs).output


def cpp_render(items, opts=None):
    """Render through the native kernel with provider-shaped dicts."""
    return WEB.build_search_output(items, opts)


def _py_text_bytes(cpp_bytes: bytes) -> bytes:
    """Python ``Path.write_text`` newline translation (CRLF on Windows)."""
    return cpp_bytes.replace(b"\n", b"\r\n")


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

#: ``kimi_cli/tests/tools/test_web_extract.py::TestConvertBase64ImagesToLinks``
KIMI_AGENT_BASE64_CASES = [
    "before ![alt text](data:image/png;base64,AAAA) after",
    "![ ](data:image/png;base64,AAAA)",
    "prefix data:image/png;base64,AAAA suffix",
    "![alt](https://example.com/img.png)",
]

#: Adversarial corpus: mime types, whitespace, malformed / unterminated blobs,
#: the three pattern shapes and the non-ASCII whitespace classes.
BASE64_CORPUS = KIMI_AGENT_BASE64_CASES + [
    "",
    "no data urls here at all",
    "![](data:image/png;base64,AAAA)",
    "![  spaced  ](data:image/png;base64,AAAA)",
    "![a](  data:image/png;base64,AAAA)",
    "![a](\n\t data:image/png;base64,AAAA)",
    "![a](data:image/png;base64,AA AA\nBB\tCC)",
    "(data:image/png;base64,AAAA)",
    "( data:image/png;base64,AAAA )",
    "(data:image/png;base64,AAAA",
    "(data:image/png;base64,)",
    "data:image/png;base64,AAAA",
    "data:image/png;base64,",
    "data:image/png;base64,AAAA!!!",
    "data:image/png;base64,AAAA+v==",
    "![a](data:image/png;base64,ZZZZ!!)",
    "data:image/svg+xml;charset=utf-8;base64,AAAA",
    "data:image/svg+xml;base64,AAAA",
    "data:image/gif;base64,R0lGODlhAQABAAAAACw=",
    "data:image/jpeg;base64,/9j/4AAQSkZJRg==",
    "data:image/webp;base64,UklGRhIAAABXRUJQ",
    "data:image/x-icon;base64,AAABAAEA",
    "data:;base64,AAAA",
    "data:image/png;base64,AA)BB)",
    "(data:image/png;base64,AA)(data:image/png;base64,BB)",
    "x(data:image/png;base64,AAAA)y",
    "![x(data:image/png;base64,AA)]",
    "![one ![two](data:image/png;base64,AAAA)](data:image/png;base64,BBBB)",
    "(data:image/png;base64,AA data:image/gif;base64,BB)",
    "[IMAGE: alt]",
    "DATA:image/png;base64,AAAA",
    "data:image//png;base64,AAAA",
    "data:image/png;foo,AAAA",
    "data:image/png;base64," + "A" * 5000,
    "![" + "a" * 500 + "](data:image/png;base64," + "B" * 5000 + ")",
    # Unicode whitespace: `regex` \s (the payload class + the space after '(').
    "(\u00a0data:image/png;base64,AAAA)",
    "![a](\u00a0data:image/png;base64,AAAA)",
    "![a](data:image/png;base64,AA\u2003BB)",
    "![a](data:image/png;base64,AA\u2028BB)",
    "![a](\u2028data:image/png;base64,AAAA)",
    "![a](data:image/png;base64,AA\u3000BB)",
    "data:image/png;base64,AA\u00a0BB",
    "![a]\u2002(data:image/png;base64,AAAA)",
    # str.strip() (the alt text) also strips \x1c-\x1f, which `regex` \s does not.
    "![\x1cx\x1c](data:image/png;base64,AAAA)",
    "![\x1fx\x1f](data:image/png;base64,AAAA)",
    "![a](\x1cdata:image/png;base64,AAAA)",
    "![\x0bx\x0b](data:image/png;base64,AAAA)",
    "![\u3000x\u3000](data:image/png;base64,AAAA)",
    # Empty mime type: `[^;]+` cannot match "", so the following data URL is the
    # one that gets replaced (regression: a blob scanner off-by-one swallowed
    # everything up to the next ";base64," as a single bogus blob).
    "data:image/;base64,data:image/gif;base64,AA",
    "data:image/;base64,AAAA",
    "![a](data:image/;base64,data:image/png;base64,AA)",
    "\u00a0![data:image/;base64,data:image/gif;base64,//++ \u3000",
]

#: Provider-shaped search results (the providers.py response contract:
#: title / url / description / content / position / date).
SEARCH_ITEMS_CORPUS = [
    [],
    [{}],
    [{"position": 1, "title": "Example Title", "url": "https://example.com/",
      "description": "Example summary"}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "date": "2024-01-01"},
     {"title": "B", "url": "https://b.example/", "description": "s2"}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": "FULL BODY"}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": ""}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": "\n"}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": "l1\nl2\n"}],
    # Duplicate URLs: the reference renders BOTH items.
    [{"title": "First", "url": "https://a.example/", "description": "one"},
     {"title": "Second", "url": "https://a.example/", "description": "two"},
     {"title": "Third", "url": "https://b.example/", "description": "three"}],
    # Missing / unexpected keys.
    [{"url": "https://a.example/"}, {"title": "t"}],
    [{"title": "t", "url": "https://a.example/", "snippet": "not the key"}],
    [{"title": "T" * 200, "url": "https://a.example/" + "u" * 200,
      "description": "s" * 200}],
    [{"title": f"T{i}", "url": f"https://e.example/{i}",
      "description": "S" * 60, "content": "C" * 400} for i in range(8)],
    # Non-ASCII payloads (code points, not bytes).
    [{"title": "\u00e9" * 50, "url": "https://\u4f8b.example/\u00fc",
      "description": "\u00fc" * 50, "content": "\u00f1" * 2000}],
    # Splitlines boundaries inside the content.
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": "one\x0btwo\x0cthree\nfour"}],
    [{"title": "A", "url": "https://a.example/", "description": "s",
      "content": "a\r\nb\rc\nd\re"}],
    # Over the ToolResultBuilder cap (50,000 code points).
    [{"title": f"T{i}", "url": f"https://e.example/{i}",
      "description": "S" * 100, "content": "C" * 3000} for i in range(30)],
    [{"title": "BIG", "url": "https://big.example/", "description": "s",
      "content": "x" * 60_000}],
    [{"title": "BIG2", "url": "https://big2.example/", "description": "s",
      "content": "\u00e9" * 30_000}],
]

TRUNCATE_CORPUS = [
    ("hello world", "https://example.com/", 2000),
    ("", "https://example.com/", 2000),
    ("a" * 2000, "https://example.com/", 2000),          # exactly at the limit
    ("a" * 2001, "https://example.com/", 2000),          # one over
    ("a" * 5000, "https://example.com/", 2000),          # one long line
    ("line of text\n" * 500, "https://example.com/", 2000),
    ("line of text\n" * 80, "https://example.com/", 2000),
    ("x\ny\n" * 2000, "https://example.com/", 2000),
    ("\u00e9" * 3000, "https://example.com/", 2000),     # 2-byte code points
    ("\u00e9\n" * 2000, "https://example.com/", 2500),
    ("head\n" * 100 + "z" * 9000, "https://example.com/", 3000),
    ("z" * 9000 + "\ntail\n" * 100, "https://example.com/", 3000),
    ("no newline at all in this text " * 200, "https://example.com/", 2500),
    ("short\nlines\n" * 300, "https://example.com:8080/p?a=b#c", 2000),
    ("x" * 5000, "not a url", 2000),
    ("x" * 5000, "https://[::1]:8080/x", 2000),
    ("a\r\nb\r\n" * 1000, "https://example.com/", 2000),  # CRLF content
    ("line\n" * 40 + "y" * 500, "https://example.com/", 2000),
]

SLUG_URLS = [
    "", "abc", "not a url", "https://example.com", "https://example.com/path",
    "https://www.google.com/search?q=kimix", "https://EXAMPLE.com/x",
    "https://USER:PW@Example.COM:8080/x",
    "https://en.wikipedia.org/wiki/XXH64",
    "https://news.ycombinator.com/item?id=12345",
    "https://[::1]:8080/x", "http://example.com:8080/x",
    "https://k\u00f6ln.example/x", "https://\u4f8b\u3048.jp/",
    "https://xn--80a.example/x",
    "https://verylonghostname12345678901234567890123456789012345678901234567890.example.com/",
    "The quick brown fox jumps over the lazy dog",
    "https://a.example/" + "p" * 300,
]


def _py_cache_file_name(url: str, share_dir: Path) -> str | None:
    """Reference file name, produced by running content.py store_full_text."""
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share_dir):
        path = _CONTENT.store_full_text(url, "x")
    return None if path is None else Path(path).name


# ---------------------------------------------------------------------------
# provenance
# ---------------------------------------------------------------------------


def test_native_provenance():
    assert Path(runtime_py.__file__).resolve() == (
        _BIN_DIR / Path(runtime_py.__file__).name).resolve()
    # The build must live inside THIS checkout -- asserted by location, not by
    # the directory's name (the clone may be called kimix-base, KimiX-native or
    # an agent worktree) nor by the kimi-agent copy it must never come from.
    assert _KIMIX_BASE_ROOT in _NATIVE_PATH.parents, str(_NATIVE_PATH)
    assert "kimi-agent" not in str(_NATIVE_PATH), str(_NATIVE_PATH)
    for name in ("convert_base64_images_to_links", "make_cache_slug",
                 "make_cache_file_name", "truncate_with_footer",
                 "clamp_search_limit", "clamp_extract_char_limit",
                 "build_search_output"):
        assert hasattr(WEB, name), f"runtime_py.builtin_tools.web.{name} missing"


def test_tool_result_cap_constants_match_the_reference():
    # ToolResultBuilder(max_line_length=None) -> DEFAULT_MAX_CHARS (50,000) and
    # the "[...truncated]" marker; both are mirrored by the C++ header.
    assert _TOOL_RESULT_MAX_CHARS == 50_000
    assert _TOOL_RESULT_MARKER == "[...truncated]"
    src = (_KIMIX_BASE_ROOT / "src" / "builtin_tools" /
           "web_search_tool.h").read_text(encoding="utf-8")
    assert "k_tool_result_max_chars = 50'000u" in src
    assert 'k_tool_result_truncation_marker =\n    "[...truncated]"' in src


def test_legacy_provider_preference_tables_match_the_reference():
    # resolve_active_provider() is not exposed to Python, so the order table is
    # cross-checked against providers.py by reading the kernel source; the
    # behaviour itself is pinned by the Boost.UT test
    # resolve_active_provider_reference_order / _extract_order.
    assert _PROVIDERS._SEARCH_LEGACY_PREFERENCE == (
        "kimi", "firecrawl", "parallel", "tavily", "exa", "searxng",
        "brave-free", "xai", "ddgs", "local")
    assert _PROVIDERS._EXTRACT_LEGACY_PREFERENCE == (
        "local", "kimi", "firecrawl", "parallel", "tavily", "exa")
    src = (_KIMIX_BASE_ROOT / "src" / "builtin_tools" /
           "web_search_tool.cpp").read_text(encoding="utf-8")
    for table in (_PROVIDERS._SEARCH_LEGACY_PREFERENCE,
                  _PROVIDERS._EXTRACT_LEGACY_PREFERENCE):
        for name in table:
            assert f'"{name}"' in src, f"legacy preference {name!r} missing"


# ---------------------------------------------------------------------------
# convert_base64_images_to_links
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("text", BASE64_CORPUS, ids=range(len(BASE64_CORPUS)))
def test_convert_base64_matches_reference(text):
    assert WEB.convert_base64_images_to_links(text) == \
        _CONTENT.convert_base64_images_to_links(text)


def test_convert_base64_kimi_agent_cases_are_pinned():
    # The literal expectations of the reference's own test suite, so a shared
    # mistake between the two implementations would still fail here.
    assert _CONTENT.convert_base64_images_to_links(
        "before ![alt text](data:image/png;base64,AAAA) after") == \
        "before [IMAGE: alt text] after"
    assert WEB.convert_base64_images_to_links(
        "before ![alt text](data:image/png;base64,AAAA) after") == \
        "before [IMAGE: alt text] after"


def test_convert_base64_fuzz_matches_reference():
    rng = random.Random(20240617)
    alphabet = [
        "![", "]", "(", ")", " ", "\t", "\n", "\r", "data:image/png;base64,",
        "data:image/svg+xml;base64,", "data:image/", ";base64,", "AAAA", "==",
        "//++", "x", "-", ":", "IMAGE", "\u00a0", "\u2003", "\u2028", "\x1c",
        "[IMAGE]", "a)b", "(", "data:image/gif;base64,", "\u3000",
    ]
    for _ in range(400):
        text = "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 12)))
        assert WEB.convert_base64_images_to_links(text) == \
            _CONTENT.convert_base64_images_to_links(text), repr(text)


# ---------------------------------------------------------------------------
# truncate_with_footer
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("content,url,limit", TRUNCATE_CORPUS,
                         ids=range(len(TRUNCATE_CORPUS)))
def test_truncate_with_footer_matches_reference(tmp_path, content, url, limit):
    share = tmp_path / "share"
    cache_dir = share / "cache" / "web"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share):
        exp_text, exp_truncated = _CONTENT.truncate_with_footer(content, url, limit)
    got = WEB.truncate_with_footer(content, url, limit, True, str(cache_dir))
    assert got["text"] == exp_text
    assert got["was_truncated"] == exp_truncated
    cpp_path = got["stored_path"]
    if not exp_truncated:
        # Under (or exactly at) the budget nothing is stored: no footer path.
        assert cpp_path is None
        assert "Full text" not in got["text"]
    else:
        assert cpp_path is not None
        py_name = _py_cache_file_name(url, share)
        assert py_name is not None
        assert Path(cpp_path).name == py_name
        # The footer names the same absolute path in both implementations.
        assert f"Full text saved to: {cpp_path}" in got["text"]


def test_truncate_with_footer_no_store_branch(tmp_path):
    # include_content=False suppresses the storage side effect (and therefore
    # the "Full text saved to:" footer) - same as the reference when
    # store_full_text returns None.
    content = "line of text\n" * 500
    share = tmp_path / "share"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share), \
            mock.patch.object(_CONTENT, "store_full_text",
                              lambda *a, **k: None):
        exp_text, exp_truncated = _CONTENT.truncate_with_footer(
            content, "https://example.com/", 2000)
    got = WEB.truncate_with_footer(content, "https://example.com/", 2000, False,
                                  str(share / "cache" / "web"))
    assert got["was_truncated"] == exp_truncated
    assert got["stored_path"] is None
    assert got["text"] == exp_text
    assert "Full text could not be stored" in got["text"]


def test_truncate_with_footer_char_limit_is_not_clamped(tmp_path):
    # The reference truncate_with_footer applies char_limit verbatim; only its
    # caller (extract.py) pre-clamps via get_extract_char_limit.  Regression:
    # the binding used to clamp to [2000, 500000] internally, so a 100-char
    # budget silently returned the whole page.
    content = "a" * 500
    share = tmp_path / "share"
    cache_dir = share / "cache" / "web"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share):
        exp_text, exp_truncated = _CONTENT.truncate_with_footer(
            content, "https://e.example/", 100)
    got = WEB.truncate_with_footer(content, "https://e.example/", 100, True,
                                   str(cache_dir))
    assert exp_truncated is True
    assert got["was_truncated"] is True
    assert got["text"] == exp_text
    assert "Showing 75 chars (head) + 25 chars (tail) of 500 total" in got["text"]


def test_truncate_with_footer_unicode_budget(tmp_path):
    # Budgets are measured in code points (Python len(str)).
    content = "\u00e9" * 5000
    share = tmp_path / "share"
    cache_dir = share / "cache" / "web"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share):
        exp_text, exp_truncated = _CONTENT.truncate_with_footer(
            content, "https://e.example/", 2000)
    got = WEB.truncate_with_footer(content, "https://e.example/", 2000, True,
                                   str(cache_dir))
    assert got["was_truncated"] == exp_truncated is True
    assert got["text"] == exp_text


# ---------------------------------------------------------------------------
# make_cache_slug / make_cache_file_name / store_full_text
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("url", SLUG_URLS, ids=range(len(SLUG_URLS)))
def test_make_cache_slug_matches_xxhash(url):
    xxhash = pytest.importorskip("xxhash")
    assert WEB.make_cache_slug(url) == \
        xxhash.xxh64(url.encode("utf-8")).hexdigest()[:10]


def test_make_cache_slug_fuzz_matches_xxhash():
    xxhash = pytest.importorskip("xxhash")
    rng = random.Random(4242)
    for _ in range(200):
        url = "".join(rng.choice("abcXYZ019-._:/?#%&=+ \u00e9\u4f8b")
                      for _ in range(rng.randint(0, 120)))
        assert WEB.make_cache_slug(url) == \
            xxhash.xxh64(url.encode("utf-8")).hexdigest()[:10], repr(url)


@pytest.mark.parametrize("url", SLUG_URLS, ids=range(len(SLUG_URLS)))
def test_make_cache_file_name_matches_reference(tmp_path, url):
    assert WEB.make_cache_file_name(url) == _py_cache_file_name(url, tmp_path)


def test_store_full_text_matches_reference(tmp_path):
    content = "single line content"
    url = "https://example.com/page"
    share = tmp_path / "share"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share):
        py_path = _CONTENT.store_full_text(url, content)
    py_bytes = Path(py_path).read_bytes()  # read before the native run overwrites
    assert py_bytes == b"single line content"  # single line: no CRLF translation
    cache_dir = share / "cache" / "web"
    got = WEB.truncate_with_footer("x" * 5000, url, 2000, True, str(cache_dir))
    assert got["stored_path"] is not None
    cpp_path = Path(got["stored_path"])
    assert cpp_path.name == Path(py_path).name
    # Both implementations store the *full* page (the model text is what gets
    # truncated), so the file holds the untruncated content.
    assert cpp_path.read_bytes() == b"x" * 5000


def test_store_full_text_cap_marker_matches_reference(tmp_path):
    big = "y" * (2_000_000 + 1)
    share = tmp_path / "share"
    with mock.patch(MOCK_GET_SHARE_DIR, return_value=share):
        py_path = _CONTENT.store_full_text("https://example.com/", big)
    py_bytes = Path(py_path).read_bytes()
    cache_dir = share / "cache" / "web"
    got = WEB.truncate_with_footer(big, "https://example.com/", 500_000, True,
                                   str(cache_dir))
    cpp_path = Path(got["stored_path"])
    assert cpp_path.name == Path(py_path).name
    cpp_bytes = cpp_path.read_bytes()
    # Documented deviation: Python's Path.write_text translates \n -> \r\n on
    # Windows; the native writer stores the UTF-8 bytes verbatim (CPython on
    # POSIX). The *only* difference is that translation.
    assert py_bytes == _py_text_bytes(cpp_bytes)
    assert py_bytes.replace(b"\r\n", b"\n") == cpp_bytes
    assert cpp_bytes.endswith(
        b"\n\n[... stored copy truncated at 2,000,000 chars of 2,000,001; "
        b"re-extract a more specific URL for the rest ...]")


# ---------------------------------------------------------------------------
# clamp_search_limit / clamp_extract_char_limit
# ---------------------------------------------------------------------------


def test_clamp_search_limit_matches_params():
    # search.py Params.limit: ge=1, le=20, default 5.
    assert _SEARCH.Params.model_fields["limit"].default == 5
    assert _SEARCH.Params(query="q").limit == 5
    for limit in range(1, 21):
        assert WEB.clamp_search_limit(limit) == \
            _SEARCH.Params(query="q", limit=limit).limit
    assert WEB.clamp_search_limit(0) == 1
    assert WEB.clamp_search_limit(-5) == 1
    assert WEB.clamp_search_limit(10_000) == 20
    # Documented deviation: pydantic *rejects* an out-of-range limit.
    with pytest.raises(Exception):
        _SEARCH.Params(query="q", limit=0)
    with pytest.raises(Exception):
        _SEARCH.Params(query="q", limit=21)


def test_clamp_extract_char_limit_matches_config():
    from kimi_cli.config import Config

    # Default (no config value) == DEFAULT_EXTRACT_CHAR_LIMIT.
    assert _CONTENT.DEFAULT_EXTRACT_CHAR_LIMIT == 15_000
    assert _CONTENT.get_extract_char_limit(Config()) == 15_000
    assert WEB.clamp_extract_char_limit(15_000) == 15_000
    for value in (0, 1, 100, 1999, 2000, 2001, 15_000, 499_999, 500_000,
                  500_001, 999_999, 10 ** 9):
        config = Config()
        config.web.extract_char_limit = value
        assert WEB.clamp_extract_char_limit(value) == \
            _CONTENT.get_extract_char_limit(config), value


# ---------------------------------------------------------------------------
# build_search_output (FOCUS 1) -- the reference is the real SearchWeb.__call__
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("items", SEARCH_ITEMS_CORPUS,
                         ids=range(len(SEARCH_ITEMS_CORPUS)))
def test_build_search_output_matches_reference(items):
    expected = py_search(items)
    got = cpp_render(items)
    assert got["text"] == expected.output
    # "Output truncated." is ToolResultBuilder's truncation flag.
    assert got["truncated"] == (expected.message == "Output truncated."), (
        f"truncated flag mismatch: {got['truncated']} vs {expected.message!r}")


@pytest.mark.parametrize("items", SEARCH_ITEMS_CORPUS,
                         ids=range(len(SEARCH_ITEMS_CORPUS)))
def test_include_content_flag_does_not_gate_rendering(items):
    # search.py renders any non-empty item content; include_content only asks
    # the *provider* for content.  Regression: the kernel dropped the block when
    # include_content was false.
    expected = py_search(items, include_content=True).output
    assert py_search(items, include_content=False).output == expected
    assert cpp_render(items)["text"] == expected
    assert cpp_render(items, {"include_content": True})["text"] == expected
    assert cpp_render(items, {"include_content": False})["text"] == expected


def test_build_search_output_keeps_duplicate_urls_by_default():
    items = [{"title": "First", "url": "https://a.example/", "description": "one"},
             {"title": "Second", "url": "https://a.example/", "description": "two"}]
    expected = py_search(items)
    assert expected.output.count("Title: First") == 1
    assert expected.output.count("Title: Second") == 1
    got = cpp_render(items)
    assert got["text"] == expected.output
    assert got["omitted_items"] == 0


def test_dedup_urls_extension_is_opt_in():
    items = [{"title": "First", "url": "https://a.example/", "description": "one"},
             {"title": "Second", "url": "https://a.example/", "description": "two"},
             {"title": "Third", "url": "https://b.example/", "description": "three"}]
    got = cpp_render(items, {"dedup_urls": True})
    # Deduping is "render the first occurrence only" = the reference rendering of
    # the de-duplicated list.
    deduped = [items[0], items[2]]
    assert got["text"] == py_search(deduped).output
    assert got["omitted_items"] == 1


def test_summary_extension_is_opt_in():
    items = [{"title": "A", "url": "https://a.example/", "description": "s"}]
    assert "summary" not in _SEARCH.Params.model_fields  # no reference equivalent
    assert cpp_render(items)["text"] == py_search(items).output
    got = cpp_render(items, {"summary": "Answer summary."})
    assert got["text"] == "Answer summary.\n\n" + py_search(items).output


def test_max_content_chars_extension_is_opt_in():
    items = [{"title": "A", "url": "https://a.example/", "description": "s",
              "content": "0123456789"}]
    assert cpp_render(items)["text"] == py_search(items).output
    got = cpp_render(items, {"max_content_chars": 4})
    capped = [dict(items[0], content="0123")]
    assert got["text"] == py_search(capped).output


def test_build_search_output_cap_matches_tool_result_builder():
    items = [{"title": f"T{i}", "url": f"https://e.example/{i}",
              "description": "S" * 100, "content": "C" * 3000}
             for i in range(30)]
    expected = py_search(items)
    assert len(expected.output) == _TOOL_RESULT_MAX_CHARS
    assert expected.message == "Output truncated."
    assert expected.output.endswith("C" * 0 + "[...truncated]\n")
    got = cpp_render(items)
    assert got["text"] == expected.output
    assert got["truncated"] is True
    assert got["omitted_items"] > 0


def test_build_search_output_cap_counts_code_points():
    # 60k two-byte characters: byte-capped code would keep fewer characters.
    items = [{"title": "U", "url": "https://u.example/", "description": "s",
              "content": "\u00e9" * 30_000}]
    expected = py_search(items)
    got = cpp_render(items)
    assert got["text"] == expected.output
    assert got["truncated"] == (expected.message == "Output truncated.")
    assert len(got["text"]) > 20_000  # not capped by *bytes*


def test_build_search_output_clean_boundary_cut_has_no_marker():
    # A cap hit exactly on a chunk boundary drops items without shortening a
    # line: the reference reports no truncation (ToolResultBuilder only sets the
    # flag in truncate_line), the native kernel must report the same.
    block = "Title: A\nDate: \nURL: https://a.example/\nSummary: s\n\n"
    items = [{"title": "A", "url": "https://a.example/", "description": "s"},
             {"title": "B", "url": "https://b.example/", "description": "s2"}]
    assert len(block) == 52
    got = cpp_render(items, {"max_output_chars": 52})
    assert got["text"] == block
    assert got["truncated"] is False
    assert got["omitted_items"] == 1
    # One code point more of budget -> the marker-only cut fires (and, exactly
    # like truncate_line, the result may end up longer than the cap).
    got2 = cpp_render(items, {"max_output_chars": 62})
    assert got2["truncated"] is True
    assert got2["text"] == block + "---\n\n[...truncated]\n"
    assert len(got2["text"]) == 72


def test_build_search_output_provider_key_mapping():
    # providers.py's response contract uses "description"; search.py reads
    # item["description"].  A "snippet"-only dict renders an empty Summary in
    # the reference, and the native kernel matches (the key mapping lives in the
    # binding, which must not invent a different key).
    items = [{"position": 1, "title": "T", "url": "https://a.example/",
              "description": "from description", "content": "BODY"}]
    assert cpp_render(items)["text"] == py_search(items).output
    assert "Summary: from description" in cpp_render(items)["text"]
    snippet_only = [{"title": "T", "url": "https://a.example/",
                     "snippet": "from snippet"}]
    assert cpp_render(snippet_only)["text"] == py_search(snippet_only).output
    assert "Summary: \n" in cpp_render(snippet_only)["text"]


def test_build_search_output_fuzz_matches_reference():
    rng = random.Random(987654321)
    for _ in range(120):
        items = []
        for index in range(rng.randint(0, 6)):
            item = {
                "title": rng.choice(["", "T", "T" * 40, "\u00e9" * 20]),
                "url": rng.choice(["", "https://a.example/", "https://a.example/",
                                   "https://b.example/" + "u" * 30]),
                "description": rng.choice(["", "s", "s" * 200]),
            }
            if rng.random() < 0.6:
                item["content"] = rng.choice(
                    ["", "\n", "BODY", "x" * 400, "y" * rng.randint(0, 30_000),
                     "l1\nl2\n" * 200, "\u00e9" * 1000, "a\x0bb\x0cc"])
            if rng.random() < 0.3:
                item["date"] = "2024-01-01"
            items.append(item)
        expected = py_search(items)
        got = cpp_render(items)
        assert got["text"] == expected.output, repr(items)
        assert got["truncated"] == (expected.message == "Output truncated.")


def test_build_search_output_limit_is_not_part_of_rendering():
    # The provider call carries the limit (search.py provider.search(query,
    # limit)); the renderer only sees the returned items, so the rendered text
    # must not depend on it.
    items = [{"title": "A", "url": "https://a.example/", "description": "s"}]
    assert py_search(items, limit=1).output == py_search(items, limit=20).output
    assert cpp_render(items)["text"] == py_search(items, limit=1).output


def test_build_search_output_is_not_numbered():
    # The task paraphrase mentions numbered sources ("N. title, url, snippet");
    # the reference emits blocks without any index, and so must the kernel.
    items = [{"title": "A", "url": "https://a.example/", "description": "s"},
             {"title": "B", "url": "https://b.example/", "description": "s2"}]
    out = cpp_render(items)["text"]
    assert out == py_search(items).output
    assert out.startswith("Title: A\nDate: \nURL: https://a.example/\n"
                          "Summary: s\n\n---\n\nTitle: B\n")
    assert "1. " not in out
    assert "1. title" not in out.lower()


def test_build_search_output_non_string_fields_are_rejected():
    # Documented deviation: search.py is duck-typed (``f"{item.get('title')}"``
    # would render 42 / None / 7), while the native kernel's typed web_item
    # contract rejects non-string fields with a TypeError instead of coercing
    # arbitrary Python objects through their repr.  The provider contract
    # guarantees strings, so this only affects malformed provider payloads.
    bad = [{"title": 42, "url": None, "description": 7}]
    assert "Title: 42" in py_search(bad).output
    with pytest.raises(TypeError):
        cpp_render(bad)
    # A missing key is not an error in either implementation.
    assert cpp_render([{"title": "x"}])["text"] == py_search([{"title": "x"}]).output


def test_build_search_output_empty_inputs():
    assert cpp_render([])["text"] == ""
    assert cpp_render([])["omitted_items"] == 0
    assert py_search([]).output == ""
    assert py_render([{}]) == cpp_render([{}])["text"]
