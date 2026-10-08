#!/usr/bin/env python3
"""Install the official SQLite amalgamation into src/ext/sqlite_amalgamation/.

The single SQLite input of this repository is the checked-in, pre-generated
amalgamation at ``src/ext/sqlite_amalgamation/sqlite3.{c,h}``.  Nothing in the
build consumes SQLite source fragments: ``src/ext/sqlite_xmake.lua`` compiles
``sqlite3.c`` into the static library ``kimix-sqlite3`` (FTS5 + math functions +
column metadata enabled, no loadable extensions), and every consumer
(``kimix-llm`` and the test suites) links that library and includes the public
``sqlite3.h``.  Bumping SQLite is therefore exactly "drop in a new amalgamation".

This script fetches that amalgamation from sqlite.org and installs it: it
replaces ``sqlite3.c`` / ``sqlite3.h`` and rewrites the ``VERSION.txt`` pin next
to them.  (The pin is ``VERSION.txt``, not ``VERSION``: the directory is a
public include dir of ``kimix-sqlite3``, and a file literally named ``VERSION``
would shadow the C++20 standard header ``<version>`` on case-insensitive
filesystems.)  It is the successor of the removed ``scripts/gen_sqlite_amalgamation.sh``
- the repository no longer vendors the SQLite Fossil source tree as a submodule,
so there is no ``lemon`` / ``tclsh`` pipeline to run; the upstream-released
amalgamation is used directly (and FTS5 has been part of it since 3.9.0).

Usage::

    python scripts/fetch_sqlite_amalgamation.py                 # the pinned VERSION.txt
    python scripts/fetch_sqlite_amalgamation.py --version 3.54.0
    python scripts/fetch_sqlite_amalgamation.py --dry-run       # resolve only, no writes
    python scripts/fetch_sqlite_amalgamation.py --url <zip-url-or-path>
    python scripts/fetch_sqlite_amalgamation.py --sha3 <hex-of-sqlite3.c>

The script runs from anywhere (paths are anchored at the repo root, like the
other scripts in this directory) and needs only Python 3 plus network access for
the default download - no bash, tclsh, C compiler or unzip.

The installed ``sqlite3.c`` / ``sqlite3.h`` are public-domain third-party code:
they are the authoritative copies and must NEVER be hand-edited.

Bump procedure: run this script, then ``xmake f -c`` + rebuild and run the
SQLite suites (``test_native_sqlite_history_index``, ``test_builtin_retrieve_sqlite``,
``test_cli_prune`` and ``test_sqlite``).
"""

import argparse
import datetime
import hashlib
import io
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import urljoin

PROJECT_ROOT = Path(__file__).resolve().parent.parent
AMALGAMATION_DIR = PROJECT_ROOT / "src" / "ext" / "sqlite_amalgamation"
VERSION_FILE = AMALGAMATION_DIR / "VERSION.txt"
SQLITE_C_FILE = AMALGAMATION_DIR / "sqlite3.c"
SQLITE_H_FILE = AMALGAMATION_DIR / "sqlite3.h"

DOWNLOAD_PAGE_URL = "https://sqlite.org/download.html"
DOWNLOAD_BASE_URL = "https://sqlite.org/"
AMALGAMATION_ZIP_PREFIX = "sqlite-amalgamation-"

# The amalgamation banner every upstream sqlite3.c starts with, and the FTS5
# entry point the history-index suites (src/runtime/index/sqlite_history_index)
# depend on (sqlite_xmake.lua enables it with -DSQLITE_ENABLE_FTS5).
AMALGAMATION_BANNER = b"amalgamation of many separate C source files from SQLite"
FTS5_MARKER = b"sqlite3Fts5Init"

USER_AGENT = "kimix-base-fetch-sqlite-amalgamation/1.0"


def warn(message: str) -> None:
    print(f"[!!] {message}", file=sys.stderr)


def info(message: str) -> None:
    print(f"[fetch_sqlite] {message}")


def parse_version(text: str) -> tuple[int, int, int, int]:
    """Parse ``X.Y.Z`` (optionally ``X.Y.Z.W``) into a 4-tuple."""
    parts = text.strip().split(".")
    if not 3 <= len(parts) <= 4 or not all(p.isdigit() for p in parts):
        raise ValueError(f"not a valid SQLite version: {text!r}")
    numbers = [int(p) for p in parts]
    numbers += [0] * (4 - len(numbers))
    return (numbers[0], numbers[1], numbers[2], numbers[3])


def amalgamation_id(version: tuple[int, int, int, int]) -> int:
    """Encode a version the way sqlite.org names its archives (3.54.0 -> 3540000).

    Template ``3.X.Y`` becomes ``3XXYY00`` and branch ``3.X.Y.Z`` becomes
    ``3XXYYZZ``, i.e. ``major*1_000_000 + minor*10_000 + patch*100 + build``.
    """
    major, minor, patch, build = version
    return major * 1_000_000 + minor * 10_000 + patch * 100 + build


def version_text(version: tuple[int, int, int, int]) -> str:
    """Render a parsed version back to its canonical dotted form."""
    if version[3]:
        return f"{version[0]}.{version[1]}.{version[2]}.{version[3]}"
    return f"{version[0]}.{version[1]}.{version[2]}"


def read_pinned_version() -> tuple[int, int, int, int]:
    if not VERSION_FILE.is_file():
        raise FileNotFoundError(
            f"{VERSION_FILE} is missing - pass --version X.Y.Z explicitly"
        )
    return parse_version(VERSION_FILE.read_text(encoding="ascii"))


def fetch_bytes(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def fetch_text(url: str) -> str:
    return fetch_bytes(url).decode("utf-8", "replace")


def resolve_relative_url(page_html: str, version: str) -> str | None:
    """Find the amalgamation row of ``version`` in the download page's CSV block.

    sqlite.org embeds a machine-readable product table in an HTML comment whose
    lines are ``PRODUCT,VERSION,RELATIVE-URL,SIZE-IN-BYTES,SHA3-HASH``.  Return
    the relative URL of the ``sqlite-amalgamation-*.zip`` row, or ``None``.
    """
    for comment in re.finditer(r"<!--(.*?)-->", page_html, re.S):
        for line in comment.group(1).splitlines():
            line = line.strip()
            if not line.startswith("PRODUCT,"):
                continue
            columns = line.split(",")
            if len(columns) < 3:
                continue
            row_version, relative_url = columns[1].strip(), columns[2].strip()
            if row_version != version:
                continue
            if (
                Path(relative_url).name.startswith(AMALGAMATION_ZIP_PREFIX)
                and relative_url.endswith(".zip")
            ):
                return relative_url
    return None


def resolve_url(
    source_version: tuple[int, int, int, int], year: int | None, explicit_url: str | None
) -> str:
    """Resolve the download URL/path for the requested version."""
    if explicit_url:
        return explicit_url

    text = version_text(source_version)
    try:
        page_html = fetch_text(DOWNLOAD_PAGE_URL)
        relative_url = resolve_relative_url(page_html, text)
        if relative_url:
            return urljoin(DOWNLOAD_BASE_URL, relative_url)
        warn(
            f"SQLite {text} is not advertised on {DOWNLOAD_PAGE_URL}; "
            "falling back to the canonical archive URL (verify it exists)"
        )
    except (urllib.error.URLError, OSError) as exc:
        warn(f"could not read {DOWNLOAD_PAGE_URL} ({exc}); using the canonical URL")

    archive_year = year if year is not None else datetime.date.today().year
    archive_id = amalgamation_id(source_version)
    return (
        f"{DOWNLOAD_BASE_URL}{archive_year}/"
        f"{AMALGAMATION_ZIP_PREFIX}{archive_id}.zip"
    )


def load_zip_bytes(source: str) -> bytes:
    """Read the amalgamation ZIP from an explicit path or an http(s) URL."""
    path = Path(source)
    if path.is_file():
        return path.read_bytes()
    if source.startswith(("http://", "https://")):
        return fetch_bytes(source)
    raise FileNotFoundError(
        f"--url {source!r} is neither an existing file nor an http(s) URL"
    )


def extract_member(zip_file: zipfile.ZipFile, filename: str) -> bytes:
    matches = [
        name
        for name in zip_file.namelist()
        if name == filename or name.endswith("/" + filename)
    ]
    if not matches:
        raise FileNotFoundError(
            f"{filename} not found inside the archive {zip_file.filename!r}"
        )
    return zip_file.read(matches[0])


def extract_amalgamation(zip_bytes: bytes) -> tuple[bytes, bytes]:
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zip_file:
        sqlite_c = extract_member(zip_file, "sqlite3.c")
        sqlite_h = extract_member(zip_file, "sqlite3.h")
    return sqlite_c, sqlite_h


def validate(
    sqlite_c: bytes,
    sqlite_h: bytes,
    version: str,
    expected_sha3: str | None,
) -> list[str]:
    """Return the list of problems that make the download unusable (fail closed)."""
    problems: list[str] = []

    header = sqlite_h.decode("utf-8", "replace")
    version_define = re.compile(
        r"#define\s+SQLITE_VERSION\s+" + re.escape(f'"{version}"')
    )
    if not version_define.search(header):
        problems.append(
            f"sqlite3.h does not define SQLITE_VERSION \"{version}\" "
            "(mismatched or redirected download?)"
        )

    banner = sqlite_c[:4096]
    if AMALGAMATION_BANNER not in banner:
        problems.append("sqlite3.c does not start with the SQLite amalgamation banner")
    if f"version {version}".encode("ascii") not in banner:
        problems.append(f"sqlite3.c banner does not mention version {version}")

    if FTS5_MARKER not in sqlite_c:
        problems.append(
            "sqlite3.c has no FTS5 sources (sqlite3Fts5Init) - the history-index "
            "suites (sqlite_history_index / context_db) need FTS5, which "
            "sqlite_xmake.lua enables with -DSQLITE_ENABLE_FTS5"
        )

    if expected_sha3:
        digest = hashlib.sha3_256(sqlite_c).hexdigest()
        if digest.lower() != expected_sha3.strip().lower():
            problems.append(
                f"sqlite3.c SHA3-256 mismatch: expected {expected_sha3.strip().lower()}, "
                f"got {digest}"
            )

    return problems


def install(sqlite_c: bytes, sqlite_h: bytes, version: str) -> None:
    AMALGAMATION_DIR.mkdir(parents=True, exist_ok=True)
    SQLITE_C_FILE.write_bytes(sqlite_c)
    SQLITE_H_FILE.write_bytes(sqlite_h)
    VERSION_FILE.write_text(version + "\n", encoding="ascii", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--version",
        default=None,
        help="SQLite version X.Y.Z to install (default: the pin in "
        "src/ext/sqlite_amalgamation/VERSION.txt)",
    )
    parser.add_argument(
        "--year",
        type=int,
        default=None,
        help="calendar year of the sqlite.org archive (only used when the "
        "download page cannot be parsed)",
    )
    parser.add_argument(
        "--url",
        default=None,
        help="explicit amalgamation ZIP: an http(s) URL or a local path",
    )
    parser.add_argument(
        "--sha3",
        default=None,
        help="expected SHA3-256 of the resulting sqlite3.c (optional sanity check)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="resolve the download and report it without downloading or writing",
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="keep the temporary download directory",
    )
    args = parser.parse_args()

    try:
        version = parse_version(args.version) if args.version else read_pinned_version()
    except (ValueError, FileNotFoundError) as exc:
        warn(str(exc))
        return 1
    text = version_text(version)

    try:
        url = resolve_url(version, args.year, args.url)
    except (ValueError, FileNotFoundError) as exc:
        warn(str(exc))
        return 1

    info(f"version : {text} (amalgamation id {amalgamation_id(version)})")
    info(f"source  : {url}")
    info(f"target  : {AMALGAMATION_DIR}")

    if args.dry_run:
        info("dry run: nothing downloaded or written")
        return 0

    temp_dir = Path(tempfile.mkdtemp(prefix="sqlite_amalgamation_"))
    try:
        try:
            zip_bytes = load_zip_bytes(url)
        except (urllib.error.URLError, OSError, FileNotFoundError) as exc:
            warn(f"download failed: {exc}")
            return 1

        try:
            sqlite_c, sqlite_h = extract_amalgamation(zip_bytes)
        except (zipfile.BadZipFile, FileNotFoundError) as exc:
            warn(f"could not read the amalgamation archive: {exc}")
            return 1

        problems = validate(sqlite_c, sqlite_h, text, args.sha3)
        if problems:
            warn(f"refusing to install SQLite {text}:")
            for problem in problems:
                warn(f"  - {problem}")
            return 1

        try:
            install(sqlite_c, sqlite_h, text)
        except OSError as exc:
            warn(f"install failed: {exc}")
            return 1

        info(
            f"installed SQLite {text}: sqlite3.c {len(sqlite_c)} bytes, "
            f"sqlite3.h {len(sqlite_h)} bytes, VERSION.txt rewritten"
        )
        info("third-party public-domain sources - never hand-edit them")
    finally:
        if args.keep_temp:
            info(f"temporary directory kept: {temp_dir}")
        else:
            shutil.rmtree(temp_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
