"""Differential parity tests for the fetch_url builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/fetch_url_tool.cpp`` ports the pure/CPU kernels of the
``fetch_url`` agent tool.  This module compares every kernel the ``runtime_py``
extension exposes through ``runtime_py.builtin_tools.web`` (the fetch_url half of
that submodule) against the *original* implementation in the kimi-agent checkout
(``C:/dev/kimi-agent``, override with ``KIMI_AGENT_ROOT``):

* ``url_safety.py`` -- the security-critical SSRF/credential kernels (FOCUS 1):
  ``normalize_url_for_request``, ``sensitive_query_param_name``,
  ``url_contains_secret``, ``is_blocked_hostname``, ``classify_resolved_address``
  (``_is_blocked_ip`` + ``ipaddress``), ``is_always_blocked_address``
  (``_ALWAYS_BLOCKED_IPS``/``_NETWORKS``) and ``is_safe_url_decision``
  (``is_safe_url`` with the DNS resolution injected, so no network is touched).
* ``web_fetcher/fetcher.py`` -- ``_html_to_markdown`` (bs4 ``html.parser`` +
  markdownify), ``_LOGIN_PATTERNS`` and the ``len(x.replace(" ","").replace(
  "\\n",""))`` text statistic (FOCUS 2/3).
* ``web/content.py`` -- ``convert_base64_images_to_links``,
  ``truncate_with_footer`` and the per-page char budget clamp (FOCUS 3).

Provenance rules (same as ``test_parity_bash.py``):

* Import the freshly built kimix-base extension BEFORE any kimi-agent module:
  ``kimi_cli.native_loader`` inserts ``<kimi-agent>/bin`` (holding a released
  ``runtime_py.pyd``) at ``sys.path[0]``, and a later ``import runtime_py`` would
  silently compare the port against itself.
* ``web_fetcher/fetcher.py`` is loaded *by path* (it has no relative imports);
  it uses the installed bs4/markdownify, which is what the reference runtime
  uses -- verified to agree with the kimi-agent virtualenv
  (bs4 4.15.0 / markdownify 1.2.3) on this whole HTML corpus by
  ``test_html_corpus_matches_kimi_agent_venv``.
* ``truncate_with_footer`` stores the full text through ``content.store_full_text``;
  the parity test pins that helper so the footer's stored-path branch is
  deterministic (no files are written).

Kernels with no Python reference (designed in the plan, not ported from Python)
are covered by the C++ Boost.UT target instead and listed at the bottom of this
module: ``pick_encoding``.
"""

from __future__ import annotations

import ipaddress
import json
import os
import random
import re
import socket
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]


def _kimix_base_bin_dir():
    """The kimix-base build directory holding ``runtime_py``."""
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = _KIMIX_BASE_ROOT / "bin" / mode
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    for cand in sorted((_KIMIX_BASE_ROOT / "bin").glob("*")):
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    return None


# IMPORT ORDER IS LOAD-BEARING: pin OUR extension before touching kimi-agent.
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

from _parity_ref import KIMIX_SRC, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

WEB = runtime_py.builtin_tools.web


def _load_by_path(path: Path, name: str):
    """Load a standalone reference module by file path (no package context)."""
    import importlib.util

    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        pytest.skip(f"cannot load reference module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


URL_SAFETY = ref("kimi_cli.tools.web.url_safety")
CONTENT = ref("kimi_cli.tools.web.content")
FETCHER = _load_by_path(
    KIMIX_SRC / "kimix" / "tools" / "web" / "web_fetcher" / "fetcher.py",
    "_parity_fetch_url_fetcher")


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

#: kimi-agent's own cases (kimi-cli/tests/tools/test_url_safety.py) plus an
#: adversarial set: schemes, userinfo, ports, IPv6 brackets, backslashes,
#: embedded tab/CR/LF, C0 controls, IDN/homoglyph hosts, entity/percent
#: escapes, empty query values and secret-bearing query parameters.
URL_CORPUS = [
    # --- reference suite -------------------------------------------------
    "https://münchen.de/path",
    "https://wttr.in/Köln",
    "https:// docs.example",
    "ftp://example.com/file",
    "https://example.com/?q=a b&x=1",
    "",
    "https://x.com/?token=abc",
    "https://x.com/?api_key=abc",
    "https://x.com/?q=hello",
    "ftp://x.com/?token=abc",
    "https://x.com/noquery",
    "https://x.com/?token=",
    "https://x.com/?k=sk-ABC1234567890",
    "https://x.com/?k=ghp_1234567890",
    "https://x.com/?k=AIzaSy0123456789_abcdefghijklmnopqrstuvwxyz",
    "https://x.com/",
    "https://x.com/?k=sk-%41BCDEFGHIJKL",
    "http://93.184.216.34/",
    "http://127.0.0.1/",
    "http://169.254.169.254/",
    "http://10.0.0.1/",
    "https://example.com/",
    "http://metadata.google.internal/",
    "https:///path",
    # --- schemes / shapes ------------------------------------------------
    "HTTP://Example.COM/A B",
    "HtTpS://example.com/x",
    "javascript:alert(1)",
    "data:text/html,<b>x</b>",
    "file:///C:/x",
    "gopher://example.com/",
    "blob:https://example.com/uuid",
    "//example.com/path",
    "http:/example.com/",
    "http:example.com",
    "http:foo",
    "https:",
    "http:",
    "https://",
    "http://",
    "   ",
    "\t",
    "https://example.com\\path",
    "https://example.com/a|b^c{d}",
    "https://example.com/path;a=b,c=d",
    "https://example.com/?q=%C3%96&r=Ö#Ö",
    "https://example.com/#Ö",
    "https://example.com/Ö",
    "https://example.com/%",
    "https://example.com/%2",
    "https://example.com/%zz",
    "https://example.com/?a=1&b=&c&=d",
    "https://example.com/?token%3D=1",
    "https://example.com/?access%5Ftoken=v",
    "https://example.com/?ToKeN=v",
    "https://example.com/?x-amz-signature=sig",
    "https://example.com/?a=1&token=b",
    "https://example.com/?a=1#frag?token=b",
    "https://user:pass@example.com:8080/p?q=1#f",
    "https://user@example.com/",
    "http://@example.com/",
    "http://:80/",
    "http://example.com:8080/",
    "https://[::1]/",
    "https://[::ffff:127.0.0.1]/",
    "https://[fe80::1%25eth0]/",
    "https://[2001:db8::1]/",
    "http://[foo]/",
    "http://[1.2.3.4]/",
    "http://[v1.x]/",
    "http://[v]/",
    "http://x[::1]/",
    "http://[::1]x/",
    "http://[::1]80/",
    "http://[::1",
    "http://]/",
    "http://[ü]/",
    "https://example.com.",
    "https://example.com..",
    "https://example.com./p",
    "http://example.com.?x=1",
    # --- whitespace / control characters inside the URL -------------------
    "https://example.com\t/",
    "https://exa\tmple.com/",
    "https://exa\nmple.com/",
    "https://exa\rmple.com/",
    "https://example.com/\tpath",
    "https://\ndocs.example",
    "\x01http://example.com/",
    "\x0bhttp://example.com/",
    "http://example.com\xa0",
    "http://example.com\u3000/",
    "http://exa mple.com/",
    "  https://example.com/  ",
    "HTTPS:// docs.example",
    # --- hosts: IP literals, IDN, homoglyphs ------------------------------
    "http://2130706433/",
    "http://0177.0.0.1/",
    "http://0x7f.0.0.1/",
    "http://127.1/",
    "http://0/",
    "http://01.2.3.4/",
    "http://例え.テスト/パス",
    "https://xn--r8jz45g.xn--zckzah/",
    "https://bücher.example/",
    "https://mañana.com/",
    "https://hätte.de/",
    "https://straße.de/",
    "https://MÜNCHEN.de/",
    "https://münchen.de:8080/p?q=ö#f",
    "https://xn--mnchen-3ya.de/",
    "http://аpple.com/",
    "http://a\u2100b/",
    "http://a／b/",
    "http://a：80/",
    "http://metadata.goog/",
    "http://METADATA.GOOGLE.INTERNAL./",
    "http://metadata.google.internal /",
    "http://metadata.google.internal\t/",
    "https://[::ffff:169.254.169.254]/",
    "http://100.100.100.200/",
    "http://100.64.0.1/",
    "http://[fd00:ec2::254]/",
    "http://198.18.0.1/",
    "http://192.0.2.1/",
    "http://[2002::1]/",
    "http://[3fff::1]/",
    "http://[64:ff9b::1]/",
]

#: query strings for sensitive_query_param_name (the reference returns the
#: unquoted key with its original case, and only for non-empty values).
SENSITIVE_CORPUS = [
    "https://x.com/?token=abc",
    "https://x.com/?ToKeN=abc",
    "https://x.com/?%74oken=abc",
    "https://x.com/?token=abc&api_key=def",
    "https://x.com/?api_key=def&token=abc",
    "https://x.com/?q=hello&token=abc",
    "https://x.com/?q=hello&token=",
    "https://x.com/?token=%20",
    "https://x.com/?token=+",
    "https://x.com/?token",
    "https://x.com/?=token",
    "https://x.com/?&&token=a",
    "https://x.com/?x-amz-signature=sig",
    "https://x.com/?x_amz_signature=sig",
    "https://x.com/?awsaccesskeyid=AKIA",
    "https://x.com/?session_id=1",
    "https://x.com/?code=1&key=2&auth=3&session=4&sig=5",
    "https://x.com/?credential=x",
    "https://x.com/?passwd=x",
    "https://x.com/?jwt=x",
    "ftp://x.com/?token=abc",
    "https://x.com/noquery",
    "https://x.com/",
    "",
    "https://x.com/?token=abc#frag",
    "https://x.com/#?token=abc",
    "https://x.com/?a=1;token=b",
    "HTTPS://X.COM/?TOKEN=abc",
    "https://x.com/?token%2B=x",
    "\x01https://x.com/?token=a",
    "https://x.com/?token=a b",
]

SECRET_CORPUS = [
    "https://x.com/",
    "https://x.com/?q=hello",
    "https://x.com/?k=sk-ABC1234567890",
    "https://x.com/?k=sk-%41BCDEFGHIJKL",
    "https://x.com/?k=ghp_1234567890",
    "https://x.com/?k=AIzaSy0123456789_abcdefghijklmnopqrstuvwxyz",
    "https://x.com/?k=AKIAIOSFODNN7EXAMPLE",
    "https://x.com/?k=AKIAIOSFODNN7EXAMPL",
    "https://x.com/?k=AKIAIOSFODNN7EXAMPLES",
    "https://x.com/?k=AKIAiosfodnn7example",
    "https://x.com/?k=xoxb-1234567890",
    "https://x.com/?k=xoxq-1234567890",
    "https://x.com/?k=xapp-1-abcdefghij",
    "https://x.com/?k=xapp-abcdefghij",
    "https://x.com/?k=glpat-abcdefghij",
    "https://x.com/?k=GR1348941abcdefghij",
    "https://x.com/?k=glrt-abc.def-ghi",
    "https://x.com/?k=sk_live_abcdefghij",
    "https://x.com/?k=sk_abcdefghij",
    "https://x.com/?k=xxsk-abcdefghij",
    "https://x.com/?k=-sk-abcdefghij",
    "https://x.com/?k=sk-abcdefghij",
    "https://x.com/?k=sk-abcdefghij-",
    "https://x.com/?k=SG.abcdefghij",
    "https://x.com/?k=gAAAAabcdefghijklmnopqrst",
    "https://x.com/?k=gAAAA",
    "https://x.com/?k=fw_abcdefghijklmnopqrstuvwxyz1234",
    "https://x.com/a/sk-abcdefghij",
    "https://sk-abcdefghij.com/",
    "https://x.com/?k=sk-%41",
    "https://x.com/?k=%73k-abcdefghij",
    "https://x.com/?k=AKIAABCDEFGHIJKLMNOP",
    "https://x.com/?token=",
    "sk-abcdefghij",
    "not a url sk-abcdefghij",
    "https://x.com/?k=bb_live_abcdefghij",
    "https://x.com/?k=retaindb_abcdefghij",
    "https://x.com/?k=mem0_abcdefghij",
    "https://x.com/?k=brv_abcdefghij",
    "https://x.com/?k=xai-abcdefghijklmnopqrstuvwxyz123456",
    "https://x.com/?k=fpk_abcdefghijklmnopqrstuvwxyz123456",
    "https://x.com/?k=fal_abcdefghij",
    "https://x.com/?k=pplx-abcdefghij",
    "https://x.com/?k=fc-abcdefghij",
    "https://x.com/?k=hf_abcdefghij",
    "https://x.com/?k=r8_abcdefghij",
    "https://x.com/?k=npm_abcdefghij",
    "https://x.com/?k=pypi-abcdefghij",
    "https://x.com/?k=dop_v1_abcdefghij",
    "https://x.com/?k=doo_v1_abcdefghij",
    "https://x.com/?k=am_abcdefghij",
    "https://x.com/?k=tvly-abcdefghij",
    "https://x.com/?k=exa_abcdefghij",
    "https://x.com/?k=gsk_abcdefghij",
    "https://x.com/?k=syt_abcdefghij",
    "https://x.com/?k=hsk-abcdefghij",
    "https://x.com/?k=ntn_abcdefghij",
    "https://x.com/?k=zap_abcdefghij",
    "https://x.com/?k= github_pat_abcdefghij",
    "https://x.com/?k=gloas-abcdefghij",
    "https://x.com/?k=gldt-abcdefghij",
    "https://x.com/?k=glrtr-abcdefghij",
    "https://x.com/?k=glcbt-abcdefghij",
    "https://x.com/?k=glptt-abcdefghij",
    "https://x.com/?k=glft-abcdefghij",
    "https://x.com/?k=glimt-abcdefghij",
    "https://x.com/?k=glagent-abcdefghij",
    "https://x.com/?k=glsoat-abcdefghij",
    "https://x.com/?k=glffct-abcdefghij",
    "https://x.com/?k=glwt-abcdefghij",
]

BLOCKED_HOSTNAME_CORPUS = [
    "metadata.google.internal",
    "METADATA.GOOGLE.INTERNAL",
    "metadata.google.internal.",
    "metadata.google.internal..",
    "metadata.google.internal ",
    " metadata.google.internal",
    "metadata.google.internal\t",
    "metadata.goog",
    "METADATA.GOOG.",
    "metadata.goog..",
    "example.com",
    "metadata.googlex",
    "metadatagoog",
    "",
    ".",
    "..",
    "münchen.de",
    "metadata.google.internal:80",
    "a.metadata.google.internal",
]

#: IPv4/IPv6 addresses: boundary sweep of every network in
#: ipaddress.IPv4Address/IPv6Address._constants plus textual edge cases.
IP_CORPUS = [
    # IPv4 boundaries
    "0.0.0.0", "0.0.0.1", "0.255.255.255", "1.0.0.1", "8.8.8.8", "9.255.255.255",
    "10.0.0.0", "10.255.255.255", "11.0.0.0", "100.63.255.255", "100.64.0.0",
    "100.127.255.255", "100.128.0.0", "126.255.255.255", "127.0.0.0", "127.0.0.1",
    "127.255.255.255", "128.0.0.0", "169.253.255.255", "169.254.0.0",
    "169.254.169.253", "169.254.169.254", "169.254.170.2", "169.254.255.255",
    "169.255.0.0", "172.15.255.255", "172.16.0.0", "172.31.255.255", "172.32.0.0",
    "192.0.0.0", "192.0.0.8", "192.0.0.9", "192.0.0.10", "192.0.0.11",
    "192.0.0.169", "192.0.0.170", "192.0.0.171", "192.0.0.255", "192.0.1.0",
    "192.0.2.0", "192.0.2.255", "192.0.3.0", "192.88.99.1", "192.168.0.0",
    "192.168.1.1", "192.168.255.255", "192.169.0.0", "198.17.255.255",
    "198.18.0.0", "198.19.255.255", "198.20.0.0", "198.51.99.255", "198.51.100.0",
    "198.51.100.255", "198.51.101.0", "203.0.112.255", "203.0.113.0", "203.0.113.255",
    "203.0.114.0", "223.255.255.255", "224.0.0.0", "224.0.0.1", "239.255.255.255",
    "240.0.0.0", "240.0.0.1", "255.255.255.254", "255.255.255.255",
    "100.100.100.200", "93.184.216.34", "1.2.3.4",
    # IPv4 textual forms ipaddress rejects
    "01.2.3.4", "1.2.3.04", "010.0.0.1", "1.2.3", "1.2.3.4.5", "999.1.1.1",
    "1.2.3.256", "0x7f.0.0.1", "2130706433", "127.1", "1.2.3.4 ", " 1.2.3.4",
    "1.2.3.4\n", "1.2.3.４", "", "abc", "1.2.3.4%eth0", "1.2.3.4%",
    # IPv6 boundaries
    "::", "::1", "::2", "0:0:0:0:0:0:0:1", "0:0:0:0:0:0:0:2",
    "::ffff:0:0", "::ffff:0.0.0.0", "::ffff:1.2.3.4", "::ffff:127.0.0.1",
    "::ffff:10.0.0.1", "::ffff:169.254.169.254", "::ffff:93.184.216.34",
    "::ffff:0:1.2.3.4", "::FFFF:7F00:1", "::1.2.3.4", "::0.0.0.1",
    "64:ff9b::", "64:ff9b::1", "64:ff9b::1:0", "64:ff9b:1::", "64:ff9b:1::1",
    "100::", "100::1", "100:0:0:0:ffff:ffff:ffff:ffff", "101::1",
    "2001::", "2001::1", "2001:0:ffff:ffff:ffff:ffff:ffff:ffff", "2001:1::",
    "2001:1::1", "2001:1::2", "2001:1::3", "2001:2::1", "2001:3::1",
    "2001:4:112::1", "2001:4:113::1", "2001:20::1", "2001:2f::1", "2001:30::1",
    "2001:3f::1", "2001:db8::", "2001:db8::1", "2001:db8:ffff::1", "2001:db9::1",
    "2001:4860:4860::8888", "2002::", "2002::1", "2003::1", "2620:0:2d0::1",
    "2fff::1", "3fff::", "3fff::1", "3fff:ffff::1", "4000::1", "2400::1",
    "fc00::", "fc00::1", "fd00::1", "fdff::1", "fe00::1", "fe7f::1", "fe80::",
    "fe80::1", "febf::1", "fec0::1", "ff00::", "ff02::1", "ffff::1",
    "fd00:ec2::254", "fd00:ec2::253", "fd00:ec2:0:0:0:1::254",
    # IPv6 textual edge cases
    "2001:db8::1%eth0", "fe80::1%eth0", "fe80::1%3", "fe80::1%", "fe80::1%%2",
    "::1%0", "2001:db8::1 ", " 2001:db8::1", "2001:DB8::1", "12345::",
    "1::2::3", ":::", "g::1", "2001", "2001:", ":", ":::1", "1:2:3:4:5:6:7",
    "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8:9:10",
    "::ffff:999.1.1.1", "::ffff:1.2.3.4.5", "::ffff:1.2.3", "0::ffff:1.2.3.4",
    "2001:db8::%1", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
    "a" * 46 + "::1", "::abcd:efgh",
]

#: (url, resolved addresses, dns_failed) triples for is_safe_url_decision.
SAFE_URL_CORPUS = [
    ("http://example.com/", ["93.184.216.34"], False),
    ("https://example.com/", ["93.184.216.34"], False),
    ("http://93.184.216.34/", ["93.184.216.34"], False),
    ("http://127.0.0.1/", ["127.0.0.1"], False),
    ("http://10.0.0.1/", ["10.0.0.1"], False),
    ("http://169.254.169.254/", ["169.254.169.254"], False),
    ("http://metadata.google.internal/", ["93.184.216.34"], False),
    ("http://metadata.google.internal./", ["93.184.216.34"], False),
    ("http://metadata.google.internal /", ["93.184.216.34"], False),
    ("http://metadata.google.internal\t/", ["93.184.216.34"], False),
    ("http://METADATA.GOOG/", ["93.184.216.34"], False),
    ("http://metadata.goog/", ["93.184.216.34"], False),
    ("ftp://example.com/", ["93.184.216.34"], False),
    ("https:///path", ["93.184.216.34"], False),
    ("", ["93.184.216.34"], False),
    ("http:///", ["93.184.216.34"], False),
    ("https://example.com\t/", ["93.184.216.34"], False),
    ("https://exa\tmple.com/", ["93.184.216.34"], False),
    ("\x01http://example.com/", ["93.184.216.34"], False),
    ("http://01.2.3.4/", ["1.2.3.4"], False),
    ("http://0x7f.0.0.1/", ["127.0.0.1"], False),
    ("http://[foo]/", ["93.184.216.34"], False),
    ("http://[::1]/", ["::1"], False),
    ("http://[1.2.3.4]/", ["1.2.3.4"], False),
    ("http://x[::1]/", ["93.184.216.34"], False),
    ("http://[::1", ["93.184.216.34"], False),
    ("http://2001:db8::1/", ["93.184.216.34"], False),
    ("http://user:pass@example.com/", ["93.184.216.34"], False),
    ("http://example.com:8080/", ["93.184.216.34"], False),
    ("https://[::ffff:127.0.0.1]/", ["::ffff:127.0.0.1"], False),
    ("https://[2002::1]/", ["2002::1"], False),
    ("https://münchen.de/", ["93.184.216.34"], False),
    ("https://öäü.de/", ["93.184.216.34"], False),
    ("http://example.com.?x=1", ["93.184.216.34"], False),
    ("http://example.com\u3000/", ["93.184.216.34"], False),
    ("http://exa mple.com/", ["93.184.216.34"], False),
    ("http://example.com/?token=abc", ["93.184.216.34"], False),
    ("https://example.com/%20", ["93.184.216.34"], False),
    ("javascript:alert(1)", ["93.184.216.34"], False),
    ("data:text/html,x", ["93.184.216.34"], False),
    ("file:///C:/x", ["93.184.216.34"], False),
    ("//example.com/", ["93.184.216.34"], False),
    ("http://100.64.0.1/", ["100.64.0.1"], False),
    ("http://198.18.0.1/", ["198.18.0.1"], False),
    ("http://192.0.2.1/", ["192.0.2.1"], False),
    ("http://example.com/", [], False),
    ("http://example.com/", ["93.184.216.34", "10.0.0.1"], False),
    ("http://example.com/", ["10.0.0.1", "93.184.216.34"], False),
    ("http://example.com/", ["93.184.216.34", "not-an-ip"], False),
    ("http://example.com/", ["fe80::1%eth0"], False),
    ("http://example.com/", ["::ffff:127.0.0.1"], False),
    ("http://example.com/", ["1.2.3.4%eth0"], False),
    ("http://example.com/", ["2001:db8::1"], False),
    # DNS failures: only non-literal hosts may be delegated to a proxy
    ("https://example.com/", [], True),
    ("https://example.com/", [], True),
    ("http://127.0.0.1/", [], True),
    ("http://10.0.0.1/", [], True),
    ("http://01.2.3.4/", [], True),
    ("http://0x7f.0.0.1/", [], True),
    ("http://2130706433/", [], True),
    ("http://127.1/", [], True),
    ("http://[fe80::1%25eth0]/", [], True),
    ("http://[::1]/", [], True),
    ("https://münchen.de/", [], True),
    ("http://例え.テスト/", [], True),
    ("http://metadata.google.internal /", [], True),
    ("https://example.com\t/", [], True),
    ("https:///path", [], True),
    ("http://2001:db8::1/", [], True),
    ("ftp://example.com/", [], True),
]

#: HTML documents for html_to_markdown (structure, entities, whitespace,
#: malformed markup, raw-text/RCDATA elements, main/body selection).
HTML_CORPUS = {
    "simple": "<html><head><title>T</title></head><body><h1>Hi</h1>"
              "<p>Hello <b>world</b>.</p></body></html>",
    "headings": "<h1>a</h1><h2>b</h2><h3>c</h3><h4>d</h4><h5>e</h5><h6>f</h6>",
    "para_nl": "<p>one</p>\n<p>two</p>",
    "inline": "<p><em>i</em> <strong>b</strong> <code>c</code> <del>d</del> "
              "<s>s</s> <q>q</q> <sub>1</sub><sup>2</sup></p>",
    "links": '<p><a href="http://x.com/">x</a> '
             '<a href="http://x.com/">http://x.com/</a> '
             '<a href="mailto:a@b.c">a@b.c</a></p>',
    "img": '<p><img src="a.png" alt="A"><img src="b.png"></p>',
    "lists": "<ul><li>a</li><li>b<ul><li>c</li></ul></li></ul>"
             "<ol start='3'><li>x</li><li>y</li></ol>",
    "list_bare_li": "<li>a</li>",
    "list_nested_ul": "<ul><ul><li>a</li></ul></ul>",
    "list_nested_li_ul": "<ul><li><ul><li>a</li></ul></li></ul>",
    "code_li": "<p><code>a<li>c</li></code></p>",
    "table": "<table><thead><tr><th>H1</th><th>H2</th></tr></thead><tbody>"
             "<tr><td>a</td><td>b</td></tr></tbody></table>",
    "table_noth": "<table><tr><td>a</td><td>b</td></tr>"
                  "<tr><td>c</td><td>d</td></tr></table>",
    "table_colspan": '<table><tr><td colspan="2">a</td></tr>'
                     "<tr><td>b</td><td>c</td></tr></table>",
    "table_empty_head": "<table><thead><tr></tr></thead><tbody>"
                        "<tr><td>x</td></tr></tbody></table>",
    "table_thead_only": "<table><thead><tr><th>h</th></tr></thead></table>",
    "table_td_th": "<table><thead><tr><td>d</td></tr></thead><tbody>"
                   "<tr><th>t</th></tr></tbody></table>",
    "table_p": "<table><tr><td><p>a</p></td></tr></table>",
    "blockquote": "<blockquote><p>quoted</p></blockquote><p>after</p>",
    "hr": "<p>a</p><hr><p>b</p>",
    "pre": "<pre>line1\n  line2\n</pre>",
    "precode": '<pre><code class="language-py">x = 1\ny = 2</code></pre>',
    "pre_b": "<pre><b>a</b></pre>",
    "pre_code_b": "<pre><code><b>a</b></code></pre>",
    "code_b": "<p><code>a<b>c</b></code></p>",
    "code_em": "<p><code>a<em>c</em></code></p>",
    "code_s": "<p><code>a<s>c</s></code></p>",
    "code_a": "<p><code>a<a href='x'>c</a></code></p>",
    "code_code": "<p><code>a<code>c</code></code></p>",
    "code_kbd": "<p><code>a<kbd>c</kbd></code></p>",
    "code_h": "<p><code>a<h1>c</h1></code></p>",
    "code_pre": "<p><code>a<pre>c</pre></code></p>",
    "code_table": "<p><code><table><tr><td>c</td></tr></table></code></p>",
    "kbd_b": "<p><kbd>a<b>c</b></kbd></p>",
    "samp_b": "<p><samp>a<b>c</b></samp></p>",
    "strong_code": "<p><strong><code>x</code></strong></p>",
    "dl": "<dl><dt>term</dt><dd>def</dd><dt>t2</dt><dd>d2</dd></dl>",
    "dd_only": "<dl><dd>def</dd></dl>",
    "dd_multi": "<dl><dd>a</dd><dd>b</dd></dl>",
    "dd_p": "<dl><dd><p>a</p></dd></dl>",
    "dd_ul": "<dl><dd><ul><li>x</li></ul></dd></dl>",
    "dd_ws": "<dl><dd> a </dd></dl>",
    "entities": "<p>&amp; &lt; &gt; &quot; &nbsp; &copy; &#65; &#x42; "
                "&unknown; &amp;amp;</p>",
    "charref_invalid": "<p>&#1114112; &#0; &#xD800; &#x110000; &#999999999999;</p>",
    "charref_boundary": "<p>&#0;&#1;&#8;&#9;&#10;&#11;&#12;&#13;&#14;&#31;&#32;"
                        "&#127;&#128;&#129;&#130;&#150;&#159;&#160;&#255;</p>",
    "charref_max": "<p>&#x10FFFF;&#1114111;&#xD7FF;&#xE000;&#x110000;</p>",
    "charref_between": "<p>a&#0;b</p>",
    "charref_multiline": "<p>&#65;&#0;&#66;</p>",
    "c1_mapping": "<p>&#128;&#130;&#150;&#159;&#129;&#141;</p>",
    "entity_attr": "<a href='x?a=1&b=2&c=3'>l</a>",
    "script_style": "<script>var x = 1 < 2;</script><style>p{color:red}</style>"
                    "<p>keep</p>",
    "script_lt": "<script>if (a<b) {}</script>",
    "style_amp": "<style>a&amp;b{}</style>",
    "script_only": "<script>x</script>",
    "style_in_body": "<body><style>x</style><p>y</p></body>",
    "comment": "<!-- comment --><p>a</p><!--[if IE]>x<![endif]-->",
    "doctype": "<!DOCTYPE html><html><body><p>x</p></body></html>",
    "bogus_decl": "<!BOGUS><p>x</p>",
    "pi": "<?xml version='1.0'?><p>x</p>",
    "pi_only": "<?xml version='1.0'?>",
    "pi_php": "<?php echo 1; ?>",
    "pi_unclosed": "<?x",
    "cdata": "<![CDATA[x]]><p>y</p>",
    "main_small": "<main><p>tiny</p></main><body><p>bodytext</p></body>",
    "main_big": "<main><p>" + "word " * 120 + "</p></main><body><p>other</p></body>",
    "main_role": '<div role="main"><p>' + "word " * 120 + "</p></div>",
    "main_role_small": '<div role="main"><p>tiny</p></div><body><p>b</p></body>',
    "body_only": "<body><p>b</p></body>",
    "nested_div": "<div><div><p>a</p></div><section><article>b</article>"
                  "</section></div>",
    "br": "<p>a<br>b<br/>c</p>",
    "double_br": "<p>a<br><br>b</p>",
    "h_in_div": "<div><h2>x</h2>tail</div>",
    "textarea_title": "<textarea><b>x</b></textarea><title>ti</title><p>p</p>",
    "textarea_amp": "<textarea>&amp;</textarea>",
    "xmp": "<xmp><b>x</b></xmp>",
    "xmp_esc": "<xmp>&amp;</xmp>",
    "xmp_text": "<xmp>a < b</xmp>",
    "xmp_quotes": '<xmp>a&b"c>d<e</xmp>',
    "noembed": "<noembed><b>z</b></noembed>",
    "noframes": "<noframes><b>w</b></noframes>",
    "iframe": "<iframe><b>y</b></iframe>",
    "plaintext": "<plaintext><b>x</b>",
    "plaintext_text": "<plaintext>abc",
    "plaintext_end": "<plaintext>a</plaintext>b",
    "listing": "<listing><b>x</b></listing>",
    "noscript": "<noscript><p>ns</p></noscript><p>p</p>",
    "svg_math": "<svg><circle/></svg><math><mi>x</mi></math><p>y</p>",
    "form": "<form><input name='x'><button>b</button></form><p>p</p>",
    "nav_aside": "<nav>n</nav><aside>a</aside><footer>f</footer>"
                 "<header>h</header><p>p</p>",
    "figure": "<figure><img src='x'><figcaption>cap</figcaption></figure>",
    "malformed": "<div>x<y</div><p>a<p>b",
    "p_in_p": "<p>a<p>b<p>c",
    "li_unclosed": "<ul><li>a<li>b<li>c</ul>",
    "uppercase": "<DIV CLASS='X'><P>Hi</P></DIV>",
    "selfclose": "<p>a</p><br/><img src='x'/><span>s</span>",
    "attrs": "<a href='x' title=\"t\" data-a='b'>l</a>",
    "unicode": "<p>héllo wörld 日本語 🎉</p>",
    "ws_collapse": "<p>  a   b\n\n c  </p>",
    "nbsp_ws": "<p>a&nbsp;&nbsp;b</p>",
    "whitespace_only_p": "<p> </p>",
    "empty": "",
    "only_text": "just text",
    "deep": "<div>" * 40 + "deep" + "</div>" * 40,
    "mixed_markdown": "# not a heading\n<p>para</p>",
    "esc_gt": "<p>a > b</p>",
    "raw_lt": "<p>a < b</p>",
    "lt_entity": "<p>a &lt; b</p>",
    "nul": "<p>a\x00b</p>",
    "cr": "<p>a\r\nb</p>",
    "tab": "<p>a\tb</p>",
    "cr_entity": "<p>a&#13;b</p>",
    "nbsp_text": "<p>a\xa0b</p>",
}


# ---------------------------------------------------------------------------
# normalization / sensitive params / secrets
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("url", URL_CORPUS)
def test_normalize_url_for_request_parity(url):
    assert WEB.normalize_url_for_request(url) == URL_SAFETY.normalize_url_for_request(url)


def test_normalize_known_reference_cases():
    """The documented examples from kimi-agent's own suite."""
    cases = {
        "https://münchen.de/path": "https://xn--mnchen-3ya.de/path",
        "https://wttr.in/Köln": "https://wttr.in/K%C3%B6ln",
        "https:// docs.example": "https://docs.example",
        "ftp://example.com/file": "ftp://example.com/file",
        "https://example.com/?q=a b&x=1": "https://example.com/?q=a%20b&x=1",
        "": "",
    }
    for url, expected in cases.items():
        assert URL_SAFETY.normalize_url_for_request(url) == expected
        assert WEB.normalize_url_for_request(url) == expected


def test_normalize_control_characters_and_case():
    """Adversarial forms the naive port used to miss (regressions).

    ``urlsplit`` lstrips C0-control/space characters, removes TAB/CR/LF from the
    whole URL and lowercases the scheme, so these must round-trip the same way
    as the Python reference (which also means non-http(s) input is returned
    untouched).
    """
    cases = [
        "HTTP://Example.COM/A B",
        "https://example.com\t/",
        "https://exa\tmple.com/",
        "https://exa\nmple.com/",
        "https://exa\rmple.com/",
        "\x01http://example.com/",
        "\x0bhttp://example.com/",
        "https://example.com/\tpath",
        "https://\ndocs.example",
        "http:foo",
        "http:example.com",
        "http://example.com\xa0",
    ]
    for url in cases:
        expected = URL_SAFETY.normalize_url_for_request(url)
        assert WEB.normalize_url_for_request(url) == expected, url


@pytest.mark.parametrize("url", SENSITIVE_CORPUS)
def test_sensitive_query_param_name_parity(url):
    assert WEB.sensitive_query_param_name(url) == \
        URL_SAFETY.sensitive_query_param_name(url)


def test_sensitive_query_param_name_reference_cases():
    assert URL_SAFETY.sensitive_query_param_name("https://x.com/?token=abc") == "token"
    assert WEB.sensitive_query_param_name("https://x.com/?token=abc") == "token"
    assert URL_SAFETY.sensitive_query_param_name("https://x.com/?token=") is None
    assert WEB.sensitive_query_param_name("https://x.com/?token=") is None


@pytest.mark.parametrize("url", SECRET_CORPUS)
def test_url_contains_secret_parity(url):
    assert WEB.url_contains_secret(url) == URL_SAFETY.url_contains_secret(url)


def test_url_contains_secret_prefix_table_exhaustive():
    """Every literal prefix in the reference table is detected."""

    def _sample(pattern):
        """A string that matches ``pattern`` (classes first, then {n,m})."""
        s = pattern.replace("\\d+", "1").replace("\\d", "1")
        s = re.sub(r"\[([^\[\]]*)\]",
                   lambda m: next((c for c in m.group(1)
                                   if c.isalnum()), "a"), s)
        s = s.replace("\\-", "-")
        s = re.sub(r"\\(.)", r"\1", s)  # un-escape literal characters
        out = ""
        i = 0
        while i < len(s):
            if s[i] == "{":
                j = s.index("}", i)
                n = int(s[i + 1:j].split(",")[0])
                out += out[-1] * (n - 1)
                i = j + 1
            else:
                out += s[i]
                i += 1
        return out

    for pattern in URL_SAFETY._PREFIX_PATTERNS:
        literal = _sample(pattern)
        url = "https://x.com/?k=" + literal
        assert URL_SAFETY.url_contains_secret(url), (pattern, literal)
        assert WEB.url_contains_secret(url), (pattern, literal)


# ---------------------------------------------------------------------------
# hostname / address classification (SSRF core)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("host", BLOCKED_HOSTNAME_CORPUS)
def test_is_blocked_hostname_parity(host):
    expected = host.strip().lower().rstrip(".") in URL_SAFETY._BLOCKED_HOSTNAMES
    assert WEB.is_blocked_hostname(host) == expected


def test_is_blocked_hostname_whitespace_regression():
    """``is_safe_url`` strips the hostname before the blocked-hostname check."""
    url = "http://metadata.google.internal /"
    assert URL_SAFETY.is_safe_url.__doc__ is not None  # reference exists
    assert WEB.is_blocked_hostname("metadata.google.internal ") is True
    assert WEB.is_blocked_hostname("\tmetadata.goog\n") is True


def _py_ip(ip_str):
    """``ipaddress.ip_address`` after the getaddrinfo scope-id strip."""
    try:
        return ipaddress.ip_address(ip_str.split("%")[0])
    except ValueError:
        return None


@pytest.mark.parametrize("ip", IP_CORPUS)
def test_classify_resolved_address_parity(ip):
    """The C++ class must imply exactly the reference's blocked verdict.

    ``url_safety._is_blocked_ip`` is a boolean; the C++ exposes an ``addr_class``
    enum whose blocked members are all classes except ``invalid``/``public``.
    """
    py_ip = _py_ip(ip)
    cpp = WEB.classify_resolved_address(ip)
    if py_ip is None:
        assert cpp == 0, (ip, cpp)  # 0 == addr_class::invalid
        return
    if isinstance(py_ip, ipaddress.IPv6Address) and py_ip.ipv4_mapped is not None:
        embedded = py_ip.ipv4_mapped
        blocked = (embedded.is_private or embedded.is_loopback
                   or embedded.is_link_local or embedded.is_reserved
                   or embedded.is_multicast or embedded.is_unspecified
                   or embedded in URL_SAFETY._CGNAT_NETWORK)
    else:
        blocked = (py_ip.is_private or py_ip.is_loopback or py_ip.is_link_local
                   or py_ip.is_reserved or py_ip.is_multicast
                   or py_ip.is_unspecified or py_ip in URL_SAFETY._CGNAT_NETWORK)
    assert (cpp not in (0, 1)) == blocked, (ip, cpp, blocked)


#: (address, expected addr_class member) -- the C++ enum labels, derived from the
#: reference's is_private/is_loopback/is_link_local/is_multicast/is_unspecified/
#: is_reserved/100.64.0.0/10 flags with the documented precedence
#: unspecified > loopback > link-local > multicast > cgnat > reserved > private
#: > public (the mapped case recurses on the embedded IPv4).
ADDR_CLASS_GOLDENS = [
    ("93.184.216.34", 1), ("8.8.8.8", 1), ("1.2.3.4", 1),
    ("127.0.0.1", 3), ("127.255.255.255", 3),
    ("10.0.0.1", 2), ("172.16.0.1", 2), ("192.168.1.1", 2), ("0.1.2.3", 2),
    ("192.0.0.1", 2), ("192.0.0.8", 2), ("192.0.2.1", 2), ("198.18.0.1", 2),
    ("198.51.100.1", 2), ("203.0.113.1", 2),
    ("192.0.0.9", 1), ("192.0.0.10", 1),  # _private_networks_exceptions
    ("169.254.1.1", 4), ("100.64.0.1", 8), ("100.127.255.255", 8),
    ("100.128.0.0", 1), ("224.0.0.1", 5), ("239.255.255.255", 5),
    ("0.0.0.0", 6), ("240.0.0.1", 7), ("255.255.255.255", 7),
    ("::1", 3), ("::", 6), ("::2", 7),
    ("fe80::1", 4), ("febf::1", 4), ("fec0::1", 1),
    ("fc00::1", 2), ("fd00::1", 2),
    ("ff02::1", 5), ("ff00::1", 5),
    ("2001:db8::1", 2), ("2001:2::1", 2), ("2001:1::1", 1), ("2001:1::2", 1),
    ("2001:3::1", 1), ("2001:4:112::1", 1), ("2001:20::1", 1), ("2001:30::1", 1),
    ("2002::1", 2), ("3fff::1", 2), ("100::1", 7), ("64:ff9b::1", 7),
    ("::ffff:0:0", 6), ("2606:2800:220:1:248:1893:25c8:1946", 1),
    ("2400::1", 1), ("2001:4860:4860::8888", 1),
]


@pytest.mark.parametrize("ip,expected", ADDR_CLASS_GOLDENS)
def test_classify_resolved_address_class_goldens(ip, expected):
    assert WEB.classify_resolved_address(ip) == expected, ip


@pytest.mark.parametrize("ip", IP_CORPUS)
def test_is_always_blocked_address_parity(ip):
    py_ip = _py_ip(ip)
    if py_ip is None:
        expected = False  # the reference only ever sees parsed addresses
    else:
        expected = (py_ip in URL_SAFETY._ALWAYS_BLOCKED_IPS or
                    any(py_ip in net for net in URL_SAFETY._ALWAYS_BLOCKED_NETWORKS))
    assert WEB.is_always_blocked_address(ip) == expected, ip


# ---------------------------------------------------------------------------
# is_safe_url decision (DNS injected -- no network access)
# ---------------------------------------------------------------------------


class _FakeResolver:
    def __init__(self):
        self.addresses = ["93.184.216.34"]
        self.fail = False
        self.calls = []

    def getaddrinfo(self, host, port, *_args, **_kwargs):
        self.calls.append(host)
        if self.fail:
            raise socket.gaierror(f"Name or service not known: {host}")
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (a, 80))
                for a in self.addresses]


@pytest.mark.parametrize("url,addresses,dns_failed", SAFE_URL_CORPUS)
@pytest.mark.parametrize("allow_all_private", [False, True])
@pytest.mark.parametrize("proxy_configured", [False, True])
def test_is_safe_url_decision_parity(url, addresses, dns_failed,
                                     allow_all_private, proxy_configured):
    fake = _FakeResolver()
    fake.addresses = list(addresses)
    fake.fail = dns_failed
    with mock.patch.object(URL_SAFETY.socket, "getaddrinfo", fake.getaddrinfo), \
            mock.patch.object(URL_SAFETY, "_proxy_is_configured",
                              lambda: proxy_configured), \
            mock.patch.object(URL_SAFETY, "_global_allow_private_urls",
                              lambda: allow_all_private):
        expected = URL_SAFETY.is_safe_url(url)
    got = WEB.is_safe_url_decision(url, allow_all_private, proxy_configured,
                                   {"dns_failed": dns_failed,
                                    "addresses": [] if dns_failed else list(addresses)})
    assert got == expected, (url, addresses, dns_failed, allow_all_private,
                             proxy_configured)


def test_is_safe_url_reference_cases():
    """The reference suite's own expectations, driven through the C++ kernel."""
    cases = [
        # url, resolved address, allow_all_private, expected
        ("http://93.184.216.34/", "93.184.216.34", False, True),
        ("http://127.0.0.1/", "127.0.0.1", False, False),
        ("http://169.254.169.254/", "169.254.169.254", True, False),
        ("http://10.0.0.1/", "10.0.0.1", False, False),
        ("http://10.0.0.1/", "10.0.0.1", True, True),
        ("http://metadata.google.internal/", "93.184.216.34", True, False),
        ("ftp://example.com/file", "93.184.216.34", False, False),
        ("https:///path", "93.184.216.34", False, False),
        ("", "93.184.216.34", False, False),
    ]
    for url, addr, allow, expected in cases:
        got = WEB.is_safe_url_decision(url, allow, False, {"addresses": [addr]})
        assert got == expected, (url, addr, allow)


def test_is_safe_url_fail_closed_on_unparseable_address():
    """An unparseable resolved address blocks even with the private override."""
    assert WEB.is_safe_url_decision("http://example.com/", True, False,
                                    {"addresses": ["not-an-ip"]}) is False
    assert WEB.is_safe_url_decision("http://example.com/", False, False,
                                    {"addresses": ["not-an-ip"]}) is False


def test_is_safe_url_literal_ip_and_proxy_escape():
    """DNS failure: literal IPs never qualify for the proxy escape."""
    for host, literal in (("http://example.com/", False),
                          ("http://10.0.0.1/", True),
                          ("http://[fe80::1%25eth0]/", True),
                          ("http://[::1]/", True),
                          ("http://01.2.3.4/", False),
                          ("http://0x7f.0.0.1/", False),
                          ("http://2130706433/", False)):
        assert WEB.is_safe_url_decision(host, False, True,
                                        {"dns_failed": True}) is not literal, host


def test_is_blocked_hostname_and_whitespace_via_decision():
    """A trailing Unicode space must not defeat the metadata hostname block."""
    assert WEB.is_safe_url_decision("http://metadata.google.internal\u3000/",
                                    True, False,
                                    {"addresses": ["93.184.216.34"]}) is False


# ---------------------------------------------------------------------------
# text statistics
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("text", [
    "", " ", "\n", "a b\nc", "a\u00a0b", "a\tb", "\r\n", "héllo wörld", "\u3000",
    "a" * 100, "line1\nline2\nline3", "  \n  \n", "mix\t\xa0\n end",
])
def test_len_without_ws_parity(text):
    assert WEB.len_without_ws(text) == len(text.replace(" ", "").replace("\n", ""))


@pytest.mark.parametrize("text", [
    "", "plain text", "login", "LOGIN", "Login", "sign in", "Sign In", "SIGN IN",
    "log in", "Log In", "verification code", "Verification Code", "注册",
    "登录", "密码登录", "验证码登录", "短信验证码", "please Login to continue",
    "ſign in", "SİGN IN", "sign  in", "log#in", "x" * 50,
])
def test_has_login_wall_parity(text):
    expected = bool(FETCHER._LOGIN_PATTERNS.search(text))
    assert WEB.has_login_wall(text) == expected, text


# ---------------------------------------------------------------------------
# fetch_url's share of the web_search/content kernels
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("text", [
    "",
    "plain",
    "![alt](data:image/png;base64,AAAA)",
    "![alt]( data:image/png;base64,AAAA )",
    "![](data:image/png;base64,AAAA)",
    "(data:image/png;base64,AAAA)",
    "data:image/png;base64,AAAA",
    "data:image/svg+xml;base64,PHN2Zz48L3N2Zz4=",
    "![a](data:image/gif;base64,R0lGOD)",
    "![a](http://x.com/i.png)",
    "![a](data:text/plain;base64,AAAA)",
    "x ![a](data:image/png;base64,AA BB\nCC) y",
    "data:image/png;base64,AAAA trailing",
    "![unclosed](data:image/png;base64,AAAA",
    "![alt](data:image/png;base64,AAA-)",
])
def test_convert_base64_images_to_links_parity(text):
    assert WEB.convert_base64_images_to_links(text) == \
        CONTENT.convert_base64_images_to_links(text)


@pytest.mark.parametrize("char_limit", [0, 1, 1999, 2000, 2001, 15000, 499999,
                                        500000, 500001, 10 ** 9, -5])
def test_clamp_extract_char_limit_parity(char_limit):
    class _Web:
        extract_char_limit = char_limit

    class _Config:
        web = _Web()

    expected = CONTENT.get_extract_char_limit(_Config())
    assert WEB.clamp_extract_char_limit(char_limit) == expected


def test_clamp_extract_char_limit_default():
    """No configured limit falls back to the reference default budget."""
    assert CONTENT.DEFAULT_EXTRACT_CHAR_LIMIT == 15000
    assert WEB.clamp_extract_char_limit(CONTENT.DEFAULT_EXTRACT_CHAR_LIMIT) == 15000


def _truncate_reference(content, url, char_limit, stored_path):
    """``content.truncate_with_footer`` with the storage step pinned."""
    with mock.patch.object(CONTENT, "store_full_text",
                           lambda _url, _content: stored_path):
        return CONTENT.truncate_with_footer(content, url, char_limit)


@pytest.mark.parametrize("limit", [2000, 2001, 5000])
def test_truncate_with_footer_parity(limit):
    """The un-stored branch: store_full_text returns None."""
    long_text = "\n".join(f"line {i} of the page body" for i in range(500))
    for content in ("short", long_text, long_text[:limit], long_text * 3):
        expected, expected_truncated = _truncate_reference(
            content, "https://example.com/page", limit, None)
        got = WEB.truncate_with_footer(content, "https://example.com/page", limit,
                                       False, None)
        assert got["text"] == expected, (limit, len(content))
        assert got["was_truncated"] == expected_truncated
        assert got["stored_path"] is None


@pytest.mark.parametrize("limit", [2000, 2001, 5000])
def test_truncate_with_footer_stored_path_parity(tmp_path, limit):
    """The stored branch: the footer names the same file the C++ writes."""
    long_text = "\n".join(f"line {i} of the page body" for i in range(500))
    url = "https://example.com/page"
    stored = str(tmp_path / WEB.make_cache_file_name(url))
    for content in (long_text, long_text[:limit], long_text * 3):
        expected, expected_truncated = _truncate_reference(content, url, limit,
                                                           stored)
        got = WEB.truncate_with_footer(content, url, limit, True, str(tmp_path))
        assert got["text"] == expected, (limit, len(content))
        assert got["was_truncated"] == expected_truncated
        if expected_truncated:
            assert got["stored_path"] == stored
        else:
            # pages at or under the budget are returned whole and not stored
            assert got["stored_path"] is None


def test_truncate_with_footer_stores_full_text(tmp_path):
    """The stored-path footer branch (file side effect stays in Python)."""
    long_text = "\n".join(f"line {i}" for i in range(2000))
    url = "https://example.com/page"
    file_name = WEB.make_cache_file_name(url)
    stored = str(Path(tmp_path) / file_name)
    expected, expected_truncated = _truncate_reference(long_text, url, 2000, stored)
    got = WEB.truncate_with_footer(long_text, url, 2000, True, str(tmp_path))
    assert got["was_truncated"] == expected_truncated is True
    assert got["text"] == expected
    assert got["stored_path"] is not None
    assert Path(got["stored_path"]).read_text(encoding="utf-8") == long_text


# ---------------------------------------------------------------------------
# HTML -> Markdown
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", sorted(HTML_CORPUS))
def test_html_to_markdown_parity(name):
    html = HTML_CORPUS[name]
    assert WEB.html_to_markdown(html, True) == FETCHER._html_to_markdown(html)


def test_html_to_markdown_reference_examples():
    """kimi-agent's own fetch tests' expectations (HTML -> Markdown)."""
    html = """<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Sample Bug Report</title></head>
<body>
<article>
    <h1>Sample Bug Report</h1>
    <p>The default parameter value for <code>optimizer</code> should probably be
    <code>adamw</code> instead of <code>adamW</code>.</p>
</article>
</body>
</html>"""
    out = WEB.html_to_markdown(html, True)
    assert out == FETCHER._html_to_markdown(html)
    for needle in ("optimizer", "adamw", "adamW", "# Sample Bug Report"):
        assert needle in out
    assert "<article>" not in out and "<code>" not in out


def test_html_to_markdown_invalid_numeric_charrefs():
    """&#0;, surrogates and > U+10FFFF decode to U+FFFD (regression)."""
    assert WEB.html_to_markdown("<p>&#0;</p>", True) == "\ufffd"
    assert WEB.html_to_markdown("<p>&#xD800;</p>", True) == "\ufffd"
    assert WEB.html_to_markdown("<p>&#1114112;</p>", True) == "\ufffd"
    assert WEB.html_to_markdown("<p>&#99999999999999999999999;</p>", True) == "\ufffd"
    assert WEB.html_to_markdown("<p>&#x110000;</p>", True) == "\ufffd"
    # C1 range maps through windows-1252
    assert WEB.html_to_markdown("<p>&#128;&#150;</p>", True) == "\u20ac\u2013"


def test_html_to_markdown_rawtext_and_pi():
    """xmp/noembed/noframes/plaintext are NOT raw text for ``html.parser``.

    CPython's ``html.parser`` switches content model only for
    ``CDATA_CONTENT_ELEMENTS`` (script, style) and ``RCDATA_CONTENT_ELEMENTS``
    (textarea, title) — the HTML5 raw-text list does not apply.  So markup
    inside ``<xmp>`` stays markup and character references inside it are
    decoded, exactly like the bs4 + markdownify reference behaves (verified
    against ``FETCHER._html_to_markdown`` by ``test_html_to_markdown_parity``).
    """
    assert WEB.html_to_markdown("<xmp><b>x</b></xmp>", True) == "**x**"
    assert WEB.html_to_markdown("<xmp>&amp;</xmp>", True) == "&"
    assert WEB.html_to_markdown("<noembed><b>z</b></noembed>", True) == "**z**"
    assert WEB.html_to_markdown("<noframes><b>w</b></noframes>", True) == "**w**"
    assert WEB.html_to_markdown("<plaintext>abc", True) == "abc"
    assert WEB.html_to_markdown("<plaintext>a</plaintext>b", True) == "ab"
    # a processing instruction is text for markdownify
    assert WEB.html_to_markdown("<?php echo 1; ?>", True) == "php echo 1; ?"


def test_html_to_markdown_noformat_and_lists():
    """Inline markup inside preformatted elements, and the bullet depth rule."""
    assert WEB.html_to_markdown("<p><code>a<b>c</b></code></p>", True) == "`ac`"
    assert WEB.html_to_markdown("<pre><b>a</b></pre>", True) == "```\na\n```"
    assert WEB.html_to_markdown("<li>a</li>", True) == "- a"
    assert WEB.html_to_markdown("<ul><li>a</li></ul>", True) == "* a"
    assert WEB.html_to_markdown("<ul><ul><li>a</li></ul></ul>", True) == "+ a"
    assert WEB.html_to_markdown("<dl><dd>def</dd></dl>", True) == ":   def"


def test_html_to_markdown_extract_false():
    """``extract=False`` converts the whole document without decomposing."""
    for name in ("simple", "headings", "inline", "lists", "entities", "unicode",
                 "ws_collapse", "table", "pre", "br", "malformed", "only_text"):
        html = HTML_CORPUS[name]
        got = WEB.html_to_markdown(html, False)
        assert isinstance(got, str)


def test_html_corpus_matches_kimi_agent_venv():
    """The installed bs4/markdownify must agree with kimi-agent's virtualenv.

    The port was written against the virtualenv the reference runs in
    (bs4 4.15 / markdownify 1.2.3); this pins that assumption so a bs4 upgrade
    on the CI interpreter cannot silently move the goldens.
    """
    venv = KIMIX_SRC.parent / ".venv" / "Scripts" / "python.exe"
    if not venv.is_file():
        pytest.skip(f"kimi-agent virtualenv python not found at {venv}")
    script = (
        "import json,sys\n"
        f"sys.path.insert(0, r'{KIMIX_SRC}')\n"
        "from kimix.tools.web.web_fetcher.fetcher import _html_to_markdown as ref\n"
        "cases = json.load(open(sys.argv[1], encoding='utf-8'))\n"
        "out = {}\n"
        "for k, v in cases.items():\n"
        "    try:\n"
        "        out[k] = ref(v)\n"
        "    except Exception as exc:\n"
        "        out[k] = 'EXC:' + type(exc).__name__\n"
        "json.dump(out, open(sys.argv[2], 'w', encoding='utf-8'))\n")
    in_path = KIMIX_SRC.parent / ".kimix_cache" / "_parity_fetch_url_cases.json"
    out_path = KIMIX_SRC.parent / ".kimix_cache" / "_parity_fetch_url_out.json"
    in_path.parent.mkdir(parents=True, exist_ok=True)
    in_path.write_text(json.dumps(HTML_CORPUS), encoding="utf-8")
    proc = subprocess.run([str(venv), "-c", script, str(in_path), str(out_path)],
                          capture_output=True, text=True)
    if proc.returncode != 0:  # pragma: no cover - environment problem
        pytest.skip(f"kimi-agent venv run failed: {proc.stderr[-500:]}")
    venv_out = json.loads(out_path.read_text(encoding="utf-8"))
    mismatched = [k for k, v in venv_out.items()
                  if not v.startswith("EXC:")
                  and v != FETCHER._html_to_markdown(HTML_CORPUS[k])]
    assert not mismatched, mismatched


# ---------------------------------------------------------------------------
# randomised sanity sweep (deterministic seed)
# ---------------------------------------------------------------------------


def test_normalize_url_fuzz_parity():
    rnd = random.Random(0xF37C4)
    pieces = ["https", "http", "HTTP", "ftp", ":", "//", "/", "?", "#", "&", "=",
              "@", ":", "doc", "s.example", "例", "%41", "%", " ", "\t", "\n",
              "\x01", "\xa0", "[", "]", "user", "pass", ".", ".example", "xn--x"]
    for _ in range(400):
        url = "".join(rnd.choice(pieces) for _ in range(rnd.randint(1, 8)))
        assert WEB.normalize_url_for_request(url) == \
            URL_SAFETY.normalize_url_for_request(url), url
        assert WEB.sensitive_query_param_name(url) == \
            URL_SAFETY.sensitive_query_param_name(url), url
        assert WEB.url_contains_secret(url) == URL_SAFETY.url_contains_secret(url), url


def test_address_fuzz_parity():
    rnd = random.Random(0xADD7)
    parts = ["0", "1", "9", "10", "100", "127", "169", "172", "192", "2001",
             "2002", "fe80", "fc00", "3fff", "ffff", "d800", "64", "ff9b", ".",
             ":", "::", "%1", "eth0", "a", "g", "12345", "255", "256", "x"]
    for _ in range(600):
        candidate = "".join(rnd.choice(parts) for _ in range(rnd.randint(1, 6)))
        py_ip = _py_ip(candidate)
        cpp = WEB.classify_resolved_address(candidate)
        if py_ip is None:
            assert cpp == 0, candidate
        else:
            if isinstance(py_ip, ipaddress.IPv6Address) and py_ip.ipv4_mapped is not None:
                ip = py_ip.ipv4_mapped
            else:
                ip = py_ip
            blocked = (ip.is_private or ip.is_loopback or ip.is_link_local
                       or ip.is_reserved or ip.is_multicast
                       or ip.is_unspecified or ip in URL_SAFETY._CGNAT_NETWORK)
            assert (cpp not in (0, 1)) == blocked, (candidate, cpp, blocked)


# ---------------------------------------------------------------------------
# documented gaps (kernels with no Python reference / known approximations)
# ---------------------------------------------------------------------------


def test_pick_encoding_has_no_python_reference():
    """``pick_encoding`` is a designed helper -- no kimi-agent counterpart.

    kimi-agent decodes HTTP bodies through its own charset detection; the C++
    kernel exists so the port could expose a deterministic charset decision.
    It is covered by the ``test_builtin_fetch_url`` Boost.UT target instead
    (grep the kimi-agent checkout for ``pick_encoding``: no hits).
    """
    import subprocess

    grep = subprocess.run(
        ["git", "grep", "-l", "pick_encoding", "--", "*.py"],
        cwd=str(KIMIX_SRC.parent), capture_output=True, text=True)
    assert grep.stdout.strip() == ""
    # still assert the documented contract on the C++ side
    assert WEB.pick_encoding("text/html; charset=utf-8", []) == "utf-8"
    assert WEB.pick_encoding("", ["gbk"]) == "gbk"
