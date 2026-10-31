#!/usr/bin/env python3
"""
publish.py — Build and package release archives for kimix-base.

Builds the project in release mode for x64 with:
  Windows: MSVC via xmake (native host build)
  Linux:   GCC  via xmake (native on Linux, or through WSL when run on Windows)

Then packages the result (``runtime_py.pyd`` on Windows / ``runtime_py.so`` on
Linux, plus the ``kimix-core`` shared library it links against dynamically,
  from ``bin/release``) into a ZIP archive named
  ``kimix_base-<platform>-<arch>-<version>.zip``. A build made with the
  external third-party leg (``--kimix_external_thirdparty=true``) additionally
  leaves ``luisa-core.dll`` / ``libluisa-core.so`` in ``bin/release``: that
  companion is packaged too when it is there, and omitted exactly as before
  when it is not (a standalone build never produces one).


The archive is a plain ZIP (built with 7-Zip's `-tzip` / Deflate), NOT a 7z:
the previous `.7z` release used the BCJ2 + LZMA2 filters that `py7zr` cannot
decompress, so the release format was switched to standard ZIP.

The version string is read from `version.txt` in the project root — the single
config file for the version. Nothing in the repo hard-codes it: xmake generates
the C++ version header from it at build time, and the Python shim / tests read
it directly. `publish.py` refuses to run if it is missing or not in `X.Y.Z`
form.

Usage examples:
  python publish.py                          # build + package + upload, all platforms
  python publish.py --platform windows       # Windows MSVC only
  python publish.py --platform linux         # Linux GCC only (native or via WSL)
  python publish.py --dry-run                # build + package + verify, never upload
  python publish.py --skip-build             # reuse bin/release (fast re-upload)
  python publish.py --platform all --no-verify
  python publish.py --clean --jobs 8
  python publish.py --no-upload              # skip the gh release upload step

Build scope: publish.py builds ONLY what it ships
(`bootstrap.py --target runtime_py`), not the CLI and the ~60 test binaries the
bare `bootstrap.py` builds, so a publish run stays short and cannot be blocked
by an unrelated test binary that fails to compile.

After every platform that builds, packages and verifies cleanly, its archive
is uploaded with the GitHub CLI (`gh release upload`) to the standing release
<https://github.com/Sikao-Engine/KimiX-native/releases/tag/Release>. The
upload uses --clobber (re-running the same version replaces the same-name
asset) and is retried on transient failures.

Credential resolution — first usable hit wins, and the secret is never printed
or passed on a command line:

  1. ``GH_TOKEN`` / ``GITHUB_TOKEN`` in the environment,
  2. an existing ``gh auth login`` (gh's own keyring entry),
  3. the git credential helper for github.com (GitHub Desktop / Git Credential
     Manager store a token there) — handed to gh as ``GH_TOKEN`` for this run.

Only when none of them yields a working credential does the run stop, and it
stops BEFORE building. ``--no-upload`` / ``--dry-run`` skip the step entirely.

Verification (per platform, before the upload):
  * the archive contains exactly the expected artifacts,
  * the packaged binary is extracted from the archive and imported (Windows on
    the host, Linux through WSL or on a native Linux host) to prove the shipped
    file loads and reports the version from version.txt,
  * after the upload, the release asset's size and sha256 digest are compared
    with the local archive, so a truncated/partial upload fails the run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import ClassVar

# =============================================================================
# Global configuration — all paths, URLs, and magic values live here.
# =============================================================================


class _Term:
    """Terminal colour escapes."""
    RED: ClassVar[str] = "\033[91m"
    GREEN: ClassVar[str] = "\033[92m"
    YELLOW: ClassVar[str] = "\033[93m"
    BOLD: ClassVar[str] = "\033[1m"
    RESET: ClassVar[str] = "\033[0m"


class Config:
    """Central configuration — no ad-hoc strings anywhere else."""

    # ------------------------------------------------------------------
    # Platform
    # ------------------------------------------------------------------
    IS_WINDOWS: ClassVar[bool] = sys.platform == "win32"
    IS_LINUX: ClassVar[bool] = sys.platform == "linux"
    CPU_COUNT: ClassVar[int] = os.cpu_count() or 4

    # ------------------------------------------------------------------
    # Build matrix
    # ------------------------------------------------------------------
    ARCH: ClassVar[str] = "x64"              # only x64 is published
    MODE: ClassVar[str] = "release"          # only release is published
    # Only this xmake target is built: publish.py ships the extension module and
    # nothing else, so the CLI and the ~60 test executables are out of scope
    # (bare bootstrap.py builds every target; `--target` narrows it).
    BUILD_TARGETS: ClassVar[tuple[str, ...]] = ("runtime_py",)

    # platform -> xmake toolchain name
    PLATFORM_TOOLCHAIN: ClassVar[dict[str, str]] = {
        "windows": "msvc",
        "linux": "gcc",
    }
    # platform -> OS host(s) able to build it
    PLATFORM_HOSTS: ClassVar[dict[str, tuple[str, ...]]] = {
        "windows": ("win32",),
        "linux": ("win32", "linux"),         # win32 -> via WSL
    }

    # ------------------------------------------------------------------
    # Version config (marked in the project root)
    # ------------------------------------------------------------------
    # version.txt is the SINGLE config file for the version. Nothing else in
    # the repo hard-codes it: xmake generates the C++ version header from it
    # at build time, and the Python shim / tests read it directly.
    ROOT_VERSION_FILE: ClassVar[str] = "version.txt"
    VERSION_PATTERN: ClassVar[str] = r"^\d+\.\d+\.\d+$"

    # ------------------------------------------------------------------
    # Directories
    # ------------------------------------------------------------------
    RELEASE_DIR: ClassVar[str] = "bin/release"          # xmake release targetdir
    STAGING_DIR: ClassVar[str] = "build/publish"        # pre-archive staging area
    ARCHIVE_DIR: ClassVar[str] = "bin/release"          # where the .zip is written
    # where verify() extracts the packaged binary before importing it
    VERIFY_SUBDIR: ClassVar[str] = "verify"
    # probe written next to the extracted binary (avoids quoting Python code
    # inside a shell command line, and Windows backslash paths in -c strings)
    PROBE_NAME: ClassVar[str] = "verify_probe.py"

    # Artifacts packaged into the archive (relative to RELEASE_DIR).
    # Windows ships the CPython extension under its native .pyd name; Linux
    # must use the .so suffix (CPython on Linux only imports *.so modules).
    # kimix-core is the shared base library (it carries the single mimalloc
    # heap) that runtime_py now links dynamically, so its dll/.so ships next
    # to the module in the same archive.
    ARTIFACTS_WINDOWS: ClassVar[tuple[str, ...]] = (
        "runtime_py.pyd",
        "kimix-core.dll",
    )
    ARTIFACTS_LINUX: ClassVar[tuple[str, ...]] = (
        "runtime_py.so",
        "libkimix-core.so",
    )
    # OPTIONAL companions: produced only by the external third-party leg
    # (xmake option kimix_external_thirdparty=true, thirdparty/link.lua names
    # the tree). There kimix-core's third-party base IS LuisaCompute's lc-core,
    # so kimix-core imports the luisa-core library at LOAD time and the archive
    # must carry it or the packaged module cannot be imported. A standalone
    # build never creates one, so these names are packaged only when the recorded
    # configuration is external AND the file exists -- a stale companion from an
    # earlier external build cannot leak into a standalone archive.
    OPTIONAL_ARTIFACTS_WINDOWS: ClassVar[tuple[str, ...]] = ("luisa-core.dll",)
    OPTIONAL_ARTIFACTS_LINUX: ClassVar[tuple[str, ...]] = ("libluisa-core.so",)

    @staticmethod
    def artifacts_for(platform: str) -> tuple[str, ...]:
        """Required artifact names to package for *platform* (relative to release dir)."""
        if platform == "windows":
            return Config.ARTIFACTS_WINDOWS
        return Config.ARTIFACTS_LINUX

    @staticmethod
    def optional_artifacts_for(platform: str) -> tuple[str, ...]:
        """Companion names that are packaged only when the build produced them."""
        if platform == "windows":
            return Config.OPTIONAL_ARTIFACTS_WINDOWS
        return Config.OPTIONAL_ARTIFACTS_LINUX

    @classmethod
    def external_leg_enabled(cls) -> bool:
        """Read the current xmake leg, ignoring stale artifacts from older builds."""
        confs = sorted(Path(".xmake").glob("*/*/xmake.conf"),
                       key=lambda path: path.stat().st_mtime, reverse=True)
        if not confs:
            return False
        text = confs[0].read_text(encoding="utf-8", errors="replace")
        return re.search(r"kimix_external_thirdparty\s*=\s*true\b", text) is not None

    @classmethod
    def packaged_artifacts(cls, platform: str) -> tuple[str, ...]:
        """Required artifacts plus companions belonging to the current build leg."""
        release_dir = Path(cls.RELEASE_DIR)
        external = cls.external_leg_enabled()
        return (
            *cls.artifacts_for(platform),
            *(name for name in cls.optional_artifacts_for(platform)
              if external and (release_dir / name).is_file()),
        )

    # Archive naming rule
    ARCHIVE_NAME_TEMPLATE: ClassVar[str] = (
        "kimix_base-{platform}-{arch}-{version}.zip"
    )

    # ------------------------------------------------------------------
    # 7-Zip — used to create the ZIP release archive
    # ------------------------------------------------------------------
    SEVENZ_NAMES: ClassVar[tuple[str, ...]] = ("7z", "7zz", "7za", "7zr")
    SEVENZ_SEARCH_WINDOWS: ClassVar[tuple[str, ...]] = (
        r"C:\Program Files\7-Zip\7z.exe",
        r"C:\Program Files (x86)\7-Zip\7z.exe",
    )
    # ZIP (Deflate), NOT 7z: 7-Zip's default 7z container uses the BCJ2+LZMA2
    # filters, which py7zr cannot decompress. Plain ZIP is universally readable.
    ZIP_ARGS: ClassVar[tuple[str, ...]] = ("a", "-tzip", "-mx=9", "-y")

    # ------------------------------------------------------------------
    # WSL (Linux builds from a Windows host)
    # ------------------------------------------------------------------
    WSL_EXE: ClassVar[str] = "wsl.exe"
    LINUX_BUILD_SCRIPT: ClassVar[str] = "linux_build.sh"

    # ------------------------------------------------------------------
    # GitHub release upload (gh CLI) — after a platform builds, packages
    # and verifies, its archive is uploaded to the standing "Release" tag:
    # https://github.com/Sikao-Engine/KimiX-native/releases/tag/Release
    # ------------------------------------------------------------------
    GH_EXE: ClassVar[str] = "gh"
    GH_REPO: ClassVar[str] = "Sikao-Engine/KimiX-native"
    GH_TAG: ClassVar[str] = "Release"
    GH_HOST: ClassVar[str] = "github.com"
    # Environment variables gh itself honours, tried in this order.
    GH_TOKEN_ENV_VARS: ClassVar[tuple[str, ...]] = ("GH_TOKEN", "GITHUB_TOKEN")
    # Transient-failure policy for the upload itself.
    UPLOAD_ATTEMPTS: ClassVar[int] = 3
    UPLOAD_BACKOFF_S: ClassVar[float] = 5.0
    # gh release upload <tag> <files>... --repo <owner>/<repo> --clobber
    # (--clobber: re-running the same version replaces the same-name asset)
    UPLOAD_ARGS: ClassVar[tuple[str, ...]] = (
        "release", "upload", GH_TAG, "--repo", GH_REPO, "--clobber",
    )

    # ------------------------------------------------------------------
    # bootstrap.py passthrough
    # ------------------------------------------------------------------
    BOOTSTRAP_SCRIPT: ClassVar[str] = "bootstrap.py"


# =============================================================================
# Helpers
# =============================================================================


def _print(msg: str, *, color: str | None = None) -> None:
    if color:
        print(f"{color}{msg}{_Term.RESET}")
    else:
        print(msg)


def _fail(msg: str) -> None:
    """Print a red error and exit(1)."""
    _print(msg, color=_Term.RED)
    sys.exit(1)


# =============================================================================
# Version
# =============================================================================


def read_version() -> str:
    """Read and validate the version config from the project root."""
    version_file = Path(Config.ROOT_VERSION_FILE)
    if not version_file.is_file():
        _fail(
            f"Version config not found: {version_file}. "
            f"Create a file named '{Config.ROOT_VERSION_FILE}' "
            f"in the project root with a version like 'X.Y.Z'."
        )
    text = version_file.read_text(encoding="utf-8").strip()
    if not re.match(Config.VERSION_PATTERN, text):
        _fail(
            f"Invalid version in {version_file}: '{text}'. "
            f"Expected '<major>.<minor>.<patch>' (e.g. 'X.Y.Z')."
        )
    _print(f"Version (from {version_file}): {text}")
    return text


# =============================================================================
# 7-Zip discovery
# =============================================================================


def find_7z() -> str | None:
    """Locate a 7-Zip executable. Returns the full path or None."""
    for name in Config.SEVENZ_NAMES:
        exe = shutil.which(name)
        if exe:
            return exe
    if Config.IS_WINDOWS:
        for candidate in Config.SEVENZ_SEARCH_WINDOWS:
            if os.path.isfile(candidate):
                return candidate
    return None


# =============================================================================
# gh CLI (GitHub release upload)
# =============================================================================


def find_gh() -> str | None:
    """Locate the GitHub CLI. Returns its full path or None."""
    return shutil.which(Config.GH_EXE)


def gh_release_url() -> str:
    return f"https://github.com/{Config.GH_REPO}/releases/tag/{Config.GH_TAG}"


def _gh_authenticated(gh_exe: str, env: dict[str, str]) -> tuple[bool, str]:
    """(is gh able to talk to github.com, diagnostic text) for *env*.

    ``gh auth status`` exits 0 for any usable credential — an environment token,
    gh's own keyring entry, or (natively) a stored OAuth session. Missing
    optional scopes do not fail it; they are reported in the diagnostic.
    """
    result = subprocess.run(
        [gh_exe, "auth", "status"], capture_output=True, text=True, env=env,
    )
    out = ((result.stdout or "") + (result.stderr or "")).strip()
    return result.returncode == 0, out


def _git_credential_token() -> str | None:
    """Read a github.com token from the configured git credential helper.

    GitHub Desktop and Git Credential Manager keep the account's token there, so
    a machine can be able to push/upload without ``gh auth login`` ever having
    been run. Returns None when no helper answers; never raises.
    """
    try:
        result = subprocess.run(
            ["git", "credential", "fill"],
            input=f"protocol=https\nhost={Config.GH_HOST}\n\n",
            capture_output=True, text=True,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    for line in (result.stdout or "").splitlines():
        key, _, value = line.partition("=")
        if key == "password" and value.strip():
            return value.strip()
    return None


def _gh_account(gh_exe: str, env: dict[str, str]) -> str:
    """The authenticated login, for the summary ('' when unavailable)."""
    result = subprocess.run(
        [gh_exe, "api", "user", "--jq", ".login"],
        capture_output=True, text=True, env=env,
    )
    return (result.stdout or "").strip() if result.returncode == 0 else ""


def resolve_gh_auth(gh_exe: str) -> tuple[dict[str, str] | None, str]:
    """Resolve credentials for the upload.

    Returns ``(env, description)`` when an upload can authenticate, else
    ``(None, reason)``. The order is: environment token, gh's own login, then
    the git credential helper (exported to gh through GH_TOKEN for this run).
    Secrets never leave the returned environment.
    """
    env = dict(os.environ)
    for name in Config.GH_TOKEN_ENV_VARS:
        if env.get(name, "").strip():
            ok, diag = _gh_authenticated(gh_exe, env)
            if ok:
                return env, f"{name} environment variable"
            _print(f"{name} is set but gh rejects it:\n{diag}", color=_Term.RED)
            break
    else:
        ok, diag = _gh_authenticated(gh_exe, env)
        if ok:
            return env, "gh login (already authenticated)"

    token = _git_credential_token()
    if token:
        cred_env = dict(env)
        cred_env["GH_TOKEN"] = token
        ok, diag = _gh_authenticated(gh_exe, cred_env)
        if ok:
            return cred_env, "git credential helper (GH_TOKEN for this run)"
        _print(
            "The git credential helper returned a github.com token but gh "
            f"rejects it:\n{diag}",
            color=_Term.RED,
        )
        return None, "git credential token rejected by gh"

    return None, (
        "no GH_TOKEN/GITHUB_TOKEN in the environment, no gh login, and no "
        f"github.com credential in the git credential helper"
    )


def _sha256(path: str | Path) -> str:
    """Streaming sha256 of a file (release assets are a few MB)."""
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _human_size(size: int) -> str:
    return f"{size:,} bytes"


# =============================================================================
# WSL helpers
# =============================================================================


def wsl_available() -> bool:
    """True when wsl.exe exists on PATH (Windows host only)."""
    return Config.IS_WINDOWS and shutil.which(Config.WSL_EXE) is not None


def wsl_linux_path(win_path: str) -> str:
    """Convert a Windows path (C:\\dev\\kimix-base) to a WSL path (/mnt/c/dev/kimix-base)."""
    win_path = win_path.replace("/", "\\")
    match = re.match(r"^([A-Za-z]):\\(.*)$", win_path)
    if match:
        drive = match.group(1).lower()
        rest = match.group(2).replace("\\", "/")
        return f"/mnt/{drive}/{rest}"
    return win_path.replace("\\", "/")


# Preflight run inside WSL before the long build: `python3` is required by
# bootstrap.py itself; a missing `xmake` is only a warning (bootstrap.py can
# download it). This catches the classic "WSL has no python3" / "wrong distro"
# case in seconds instead of after a failed build.
_WSL_PREFLIGHT = (
    "command -v python3 >/dev/null 2>&1 || echo MISSING_PYTHON3; "
    "command -v xmake >/dev/null 2>&1 || echo MISSING_XMAKE; "
    "python3 -c 'import sys; print(\"PYTHON\", sys.version.split()[0])' "
    "2>/dev/null"
)


def wsl_preflight() -> bool:
    """Check the WSL side can host the Linux build. Returns False when it cannot."""
    # Bytes, not text: WSL relays its own diagnostic noise on stderr in the
    # Windows ANSI codepage, and decoding that as UTF-8 raises inside
    # subprocess' reader thread (which prints a traceback mid-publish).
    result = subprocess.run(
        [Config.WSL_EXE, "bash", "-lc", _WSL_PREFLIGHT],
        capture_output=True,
    )
    out = (
        (result.stdout or b"").decode("utf-8", "replace")
        + (result.stderr or b"").decode("utf-8", "replace")
    ).strip()
    if "MISSING_PYTHON3" in out or result.returncode != 0 or "PYTHON " not in out:
        _print(
            "WSL cannot run the Linux build: no working python3 in the default "
            f"distribution.\n{out}\n"
            "Install it (e.g. `wsl.exe sudo apt-get install -y python3`) or use "
            "--platform windows.",
            color=_Term.RED,
        )
        return False
    if "MISSING_XMAKE" in out:
        _print(
            "WSL has no xmake on PATH; bootstrap.py will try to download one.",
            color=_Term.YELLOW,
        )
    line = next((ln for ln in out.splitlines() if ln.startswith("PYTHON ")), "")
    _print(f"WSL preflight ok: {line}", color=_Term.GREEN)
    return True


# =============================================================================
# Build
# =============================================================================


def _bootstrap_flags(args, toolchain: str) -> list[str]:
    """Common bootstrap.py flags shared by the native build paths."""
    flags = [
        "bootstrap.py",
        "--toolchain", toolchain,
        "--jobs", str(args.jobs),
    ]
    # Ship-only build: without this bootstrap.py compiles every target (CLI +
    # every test binary), which is minutes of unrelated work and one more way
    # for a publish run to fail on code it does not publish.
    for target in Config.BUILD_TARGETS:
        flags.extend(["--target", target])
    if args.clean:
        flags.append("--clean")
    if args.verbose:
        flags.append("--verbose")
    if args.xmake:
        flags.extend(["--xmake", args.xmake])
    if args.no_download:
        flags.append("--no-download")
    return flags


def _binary_magic(path: Path) -> str:
    """Classify a built extension by its header: ``pe`` / ``elf`` / ``unknown``."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(4)
    except OSError:
        return "unknown"
    if head[:2] == b"MZ":
        return "pe"
    if head[:4] == b"\x7fELF":
        return "elf"
    return "unknown"


def _drop_foreign_extension(platform: str) -> None:
    """Remove ``bin/release`` runtime_py outputs that are foreign to *platform*.

    The ``runtime_py`` target keeps the ``.pyd`` filename on every platform
    (see src/xmake.lua) and both platforms share ``bin/release``. xmake's
    up-to-date check looks at its own build info, not at the output bytes,
    so a stale foreign-magic file survives into this platform's build and
    makes ``xmake build`` a silent no-op that still reports "Build
    succeeded!" — its ``after_link`` then re-publishes the foreign bytes as
    the Linux-side ``runtime_py.so``/``libruntime_py.so`` copies, and
    publish.py would package the wrong binary (or trip package()'s magic
    guard). Deleting every foreign-magic output first forces a real relink.

    All output names are considered in BOTH directions: the Linux
    ``after_link`` step also leaves an importable ``runtime_py.so`` copy and a
    ``libruntime_py.so`` symlink behind, which are just as foreign on the
    Windows side (and vice versa) and used to linger in ``bin/release`` after a
    mixed-platform publish run. The same applies to the kimix-core shared
    library (``kimix-core.dll`` / ``libkimix-core.so``) that runtime_py links
    against dynamically, and to the optional external-leg companion it imports
    at load time (``luisa-core.dll`` / ``libluisa-core.so``). That last one must
    be in this list for the same reason as the rest: it only exists in an
    external-leg build, so a foreign copy left behind by the other platform's
    run would be reused byte-for-byte as this platform's companion -- a stale
    binary smuggled into a "successful" release.
    """
    want = "pe" if platform == "windows" else "elf"
    for name in (
        "runtime_py.pyd", "runtime_py.so", "libruntime_py.so",
        "kimix-core.dll", "libkimix-core.so",
        "luisa-core.dll", "libluisa-core.so",
    ):
        path = Path(Config.RELEASE_DIR) / name
        if not (path.is_file() or path.is_symlink()):
            continue
        got = _binary_magic(path)
        if got != want:
            _print(
                f"Removing {path} ({got.upper()} but the {platform} target "
                f"needs {want.upper()}) so the {platform} build relinks.",
                color=_Term.YELLOW,
            )
            path.unlink(missing_ok=True)


def build_windows(args) -> int:
    """Build the windows target natively with MSVC via bootstrap.py."""
    _drop_foreign_extension("windows")
    cmd = [sys.executable, *_bootstrap_flags(args, "msvc")]
    _print(f"\nBuilding windows (MSVC): {' '.join(cmd)}", color=_Term.BOLD)
    return subprocess.run(cmd).returncode


def build_linux(args) -> int:
    """Build the linux target with GCC — natively on Linux, or via WSL from Windows."""
    # Same shared-tree hazard as the windows direction (bin/release keeps the
    # .pyd filename across platforms): drop foreign-magic outputs so xmake
    # really relinks instead of no-oping onto a stale PE copy.
    _drop_foreign_extension("linux")
    if Config.IS_LINUX:
        cmd = [sys.executable, *_bootstrap_flags(args, "gcc")]
        _print(f"\nBuilding linux (GCC): {' '.join(cmd)}", color=_Term.BOLD)
        return subprocess.run(cmd).returncode

    if not Config.IS_WINDOWS:
        _print(
            "Linux builds are only supported on a Windows host (via WSL) "
            "or a native Linux host.",
            color=_Term.RED,
        )
        return 1
    if not wsl_available():
        _print(
            f"{Config.WSL_EXE} not found on PATH — cannot build the linux "
            f"target on this machine. Use '--platform windows' to skip it.",
            color=_Term.RED,
        )
        return 1
    if not wsl_preflight():
        return 1

    # Build the inner bash script and run it inside WSL. A script file avoids
    # the fragile quoting of `wsl.exe bash -lc "<one-liner>"`.
    root = Path.cwd()
    linux_root = wsl_linux_path(str(root))
    lines = [
        "#!/usr/bin/env bash",
        "set -e",
        f"cd {shlex.quote(linux_root)}",
        "python3 " + " ".join(shlex.quote(t) for t in _bootstrap_flags(args, "gcc")),
    ]
    script_path = Path(Config.STAGING_DIR) / Config.LINUX_BUILD_SCRIPT
    script_path.parent.mkdir(parents=True, exist_ok=True)
    with open(script_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")

    cmd = [Config.WSL_EXE, "bash", "-l", wsl_linux_path(str(script_path))]
    # `-l` (login shell) sources ~/.profile so per-user PATH entries such as
    # ~/.local/bin (where a Linux xmake may live) are available; a plain
    # `bash <script>` would silently miss them and fall back to a broken
    # xmake auto-download.
    _print(f"\nBuilding linux (GCC via WSL): {' '.join(cmd)}", color=_Term.BOLD)
    return subprocess.run(cmd).returncode


def resolve_platforms(choice: str) -> list[str]:
    """Turn the --platform choice into the list of platforms to build.

    On a Windows host building "all", Linux (via WSL) is built FIRST so the
    Linux build's ELF ``runtime_py.pyd``/``.so`` artifacts do not clobber the
    Windows PE ``runtime_py.pyd`` written by the later MSVC build (both share
    ``bin/release``; the final on-disk state on a Windows host should be the
    Windows artifacts).
    """
    if choice == "all":
        platforms = []
        if Config.IS_LINUX or wsl_available():
            platforms.append("linux")
        if Config.IS_WINDOWS:
            platforms.append("windows")
        if not platforms:
            _fail(f"Unsupported host platform: {sys.platform}")
        return platforms

    if choice not in Config.PLATFORM_TOOLCHAIN:
        _fail(f"Unknown platform: '{choice}'. Use one of: all, windows, linux.")
    if sys.platform not in Config.PLATFORM_HOSTS[choice]:
        _fail(
            f"Platform '{choice}' cannot be built from host '{sys.platform}' "
            f"(supported hosts: {', '.join(Config.PLATFORM_HOSTS[choice])})."
        )
    return [choice]


def build_platform(platform: str, args) -> int:
    """Build one platform. Returns the process exit code."""
    if platform == "windows":
        return build_windows(args)
    return build_linux(args)


# =============================================================================
# Packaging
# =============================================================================


def package(platform: str, version: str) -> str:
    """Package the native artifacts for *platform* into the ZIP archive.

    Returns the absolute path of the created archive.
    """
    sevenz = find_7z()
    if not sevenz:
        _fail(
            "7-Zip not found. Install it (https://www.7-zip.org/) or put "
            "7z/7zz/7za on PATH."
        )

    required = Config.artifacts_for(platform)
    # Optional companions (external-leg luisa-core) ride along only when the
    # build left them beside the required artifacts; a standalone build packages
    # exactly `required`, as it always did.
    artifacts = Config.packaged_artifacts(platform)
    release_dir = Path(Config.RELEASE_DIR)
    missing = [
        a for a in required
        if not (release_dir / a).is_file()
    ]
    if missing:
        _fail(
            f"Missing build artifacts in {release_dir}: {', '.join(missing)}.\n"
            f"Build them first: python bootstrap.py --target "
            f"{' '.join(Config.BUILD_TARGETS)} (and re-run publish.py without "
            f"--skip-build)."
        )

    # Guard: both platforms share bin/release and the runtime_py module keeps
    # the .pyd filename on both, so a stale binary left by the other platform's
    # build can end up packaged under the wrong name. Refuse rather than ship
    # an ELF as the Windows artifact (or vice versa).
    want = "pe" if platform == "windows" else "elf"
    for artifact in artifacts:
        got = _binary_magic(release_dir / artifact)
        if got != want:
            _fail(
                f"{release_dir / artifact} is {got.upper()} but the {platform} "
                f"target needs {want.upper()}; bin/release is shared between "
                f"platforms — rebuild {platform} first."
            )

    # Stage a clean copy so the archive contains exactly the artifacts.
    staging_dir = Path(Config.STAGING_DIR) / f"{platform}-{Config.ARCH}"
    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)
    for artifact in artifacts:
        shutil.copy2(release_dir / artifact, staging_dir / artifact)

    archive_dir = Path(Config.ARCHIVE_DIR)
    archive_dir.mkdir(parents=True, exist_ok=True)
    archive_name = Config.ARCHIVE_NAME_TEMPLATE.format(
        platform=platform, arch=Config.ARCH, version=version,
    )
    # Resolve to an absolute path: 7z runs with cwd=staging_dir, so a
    # relative archive path would be resolved against the staging dir.
    archive_path = (archive_dir / archive_name).resolve()
    if archive_path.exists():
        archive_path.unlink()

    _print(f"\nPackaging {platform} artifacts into {archive_path}", color=_Term.BOLD)
    cmd = [sevenz, *Config.ZIP_ARGS, str(archive_path), *artifacts]
    _print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=str(staging_dir))
    if result.returncode != 0:
        _fail(f"7-Zip failed with exit code {result.returncode}.")
    if not archive_path.is_file():
        _fail(f"Archive was not created: {archive_path}")

    size = archive_path.stat().st_size
    _print(f"Archive created: {archive_path} ({size:,} bytes)", color=_Term.GREEN)
    return str(archive_path)


# =============================================================================
# Verification
# =============================================================================


def _archive_contains(sevenz: str, archive: str, names: tuple[str, ...]) -> bool:
    """Check that every expected file appears in the archive listing."""
    result = subprocess.run(
        [sevenz, "l", archive], capture_output=True, text=True,
    )
    listing = (result.stdout or "") + (result.stderr or "")
    for name in names:
        if name not in listing:
            _print(f"Archive is missing '{name}':\n{listing}", color=_Term.RED)
            return False
    return True


def _present_in_archive(
    sevenz: str, archive: str, names: tuple[str, ...]
) -> tuple[str, ...]:
    """The subset of *names* that the archive actually contains.

    Used for the OPTIONAL companions: they exist only in an external-leg build,
    so asking 7-Zip to extract a name an older/standalone archive never carried
    would turn a normal verify into a spurious failure. Listing first keeps the
    required-artifact behaviour exact and makes the optional ones a subset.
    """
    result = subprocess.run(
        [sevenz, "l", archive], capture_output=True, text=True,
    )
    listing = (result.stdout or "") + (result.stderr or "")
    return tuple(name for name in names if name in listing)


# Probe executed next to the extracted binary: it proves the SHIPPED file loads
# and carries the version.txt version. Written to a file (instead of `python -c`)
# so Windows backslash paths and Python quoting never reach a shell.
_PROBE_SOURCE = '''\
import sys

sys.path.insert(0, sys.path[0] or ".")
import runtime_py  # noqa: E402

print("PROBE_VERSION", runtime_py.version())
print("PROBE_CORE", runtime_py.core_version())
print("PROBE_FILE", runtime_py.__file__)
'''


def _verify_artifact(platform: str, archive: str, version: str) -> bool:
    """Extract the packaged binary and import it in a fresh interpreter.

    Windows runs the host interpreter (a PE module loads natively); Linux runs
    the interpreter inside WSL on a Windows host, or the host interpreter on a
    native Linux host. Returns False when the shipped file cannot be loaded or
    reports a different version; a platform without a usable Linux interpreter
    is reported as "not verified" but not as a failure (the archive listing and
    the magic-byte guard still apply).
    """
    required = Config.artifacts_for(platform)
    artifact = required[0]
    # Absolute paths: the probe runs with cwd=verify_dir, so relative ones would
    # be resolved twice (verify_dir/verify_dir/probe.py).
    verify_dir = (
        Path(Config.STAGING_DIR) / f"{platform}-{Config.ARCH}-{Config.VERIFY_SUBDIR}"
    ).resolve()
    if verify_dir.exists():
        shutil.rmtree(verify_dir)
    verify_dir.mkdir(parents=True, exist_ok=True)

    sevenz = find_7z()
    # Extract EVERY packaged artifact: runtime_py dynamically links kimix-core,
    # so both files must sit side by side for the import probe -- and in the
    # external leg kimix-core in turn imports the optional luisa-core companion
    # at LOAD time, so an archive that carries it must have it extracted too.
    # The optional names are taken from the archive listing: a standalone
    # archive never carries one and must not fail for its absence.
    artifacts = (
        *required,
        *_present_in_archive(sevenz, archive,
                             Config.optional_artifacts_for(platform)),
    )
    extract = subprocess.run(
        [sevenz, "e", str(Path(archive).resolve()), f"-o{verify_dir}",
         *artifacts, "-y"],
        capture_output=True, text=True,
    )
    missing = [a for a in artifacts if not (verify_dir / a).is_file()]
    if extract.returncode != 0 or missing:
        _print(
            f"Could not extract {artifact} from {archive}:\n"
            f"{(extract.stdout or '') + (extract.stderr or '')}",
            color=_Term.RED,
        )
        return False

    probe = verify_dir / Config.PROBE_NAME
    probe.write_text(_PROBE_SOURCE, encoding="utf-8", newline="\n")

    if platform == "windows":
        if not Config.IS_WINDOWS:
            _print(
                "Cannot import the Windows artifact on this host; skipping the "
                "load check.",
                color=_Term.YELLOW,
            )
            return True
        result = subprocess.run(
            [sys.executable, str(probe)], cwd=str(verify_dir),
            capture_output=True, text=True,
        )
        out = ((result.stdout or "") + (result.stderr or "")).strip()
        rc = result.returncode
    elif Config.IS_LINUX:
        # runtime_py.so's NEEDED entry is libkimix-core.so; the archive's
        # library sits next to the module in verify_dir, so point the loader
        # there (cwd is not on the default search path).
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (
            str(verify_dir) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        )
        result = subprocess.run(
            [sys.executable, str(probe)], cwd=str(verify_dir),
            capture_output=True, text=True, env=env,
        )
        out = ((result.stdout or "") + (result.stderr or "")).strip()
        rc = result.returncode
    elif wsl_available():
        inner = wsl_linux_path(str(verify_dir))
        inner_probe = wsl_linux_path(str(probe))
        # Bytes, not text: WSL relays raw diagnostic bytes on stderr.
        result = subprocess.run(
            [Config.WSL_EXE, "bash", "-lc",
             f"cd {shlex.quote(inner)}"
             f" && LD_LIBRARY_PATH={shlex.quote(inner)}"
             f" python3 {shlex.quote(inner_probe)}"],
            capture_output=True,
        )
        out = (
            (result.stdout or b"").decode("utf-8", "replace")
            + (result.stderr or b"").decode("utf-8", "replace")
        ).strip()
        rc = result.returncode
    else:
        _print(
            "No Linux interpreter available (no WSL, not a Linux host); the "
            "archive listing and magic-byte checks still passed, but the "
            "shipped .so was not imported.",
            color=_Term.YELLOW,
        )
        return True

    if rc != 0 or "PROBE_VERSION" not in out:
        _print(f"Importing the packaged {artifact} failed:\n{out}", color=_Term.RED)
        return False
    if f"PROBE_VERSION kimix-runtime {version}" not in out:
        _print(
            f"Packaged {artifact} reports the wrong version (expected {version}):\n{out}",
            color=_Term.RED,
        )
        return False
    loaded_from = next(
        (ln.split(" ", 1)[-1] for ln in out.splitlines() if ln.startswith("PROBE_FILE")),
        "",
    )
    _print(
        f"Packaged {artifact} imports OK ({version}): {loaded_from}",
        color=_Term.GREEN,
    )
    return True


def _verify_windows_pyd(version: str) -> bool:
    """Import the freshly built bin/release/runtime_py.pyd and check its version.

    Kept alongside ``_verify_artifact``: this one proves the BUILD output (before
    packaging) is loadable, which gives a clearer error when the toolchain
    produced something broken.
    """
    code = (
        "import sys\n"
        "sys.path.insert(0, r'bin/release')\n"
        "import runtime_py\n"
        "v = runtime_py.version()\n"
        "print('runtime_py loaded OK, version:', v)\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True,
    )
    out = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        _print(f"runtime_py import failed:\n{out}", color=_Term.RED)
        return False
    if version not in out:
        _print(
            f"Version mismatch: expected '{version}' inside "
            f"runtime_py.version(), got:\n{out}",
            color=_Term.RED,
        )
        return False
    _print(out.strip())
    return True


def verify(platform: str, archive: str, version: str) -> bool:
    """Verify the archive: contents, the packaged binary, and (Windows) the build."""
    sevenz = find_7z()
    ok = True
    # The packaged set, not just the required one: whatever package() put in the
    # archive (required artifacts plus an external-leg luisa-core companion when
    # the build produced one) must be listed there.
    artifacts = Config.packaged_artifacts(platform)

    path = Path(archive)
    size = path.stat().st_size if path.is_file() else 0
    _print(
        f"\nVerifying {platform} archive: {archive}\n"
        f"  size   {_human_size(size)}\n"
        f"  sha256 {_sha256(path) if path.is_file() else '<missing>'}",
        color=_Term.BOLD,
    )
    if sevenz is None or not _archive_contains(sevenz, archive, artifacts):
        ok = False

    if ok and platform == "windows":
        ok = _verify_windows_pyd(version)
    if ok:
        ok = _verify_artifact(platform, archive, version)

    if ok:
        _print(f"Verification passed for {platform}.", color=_Term.GREEN)
    else:
        _print(f"Verification FAILED for {platform}.", color=_Term.RED)
    return ok


# =============================================================================
# Upload (gh release upload)
# =============================================================================


def _remote_asset(gh_exe: str, env: dict[str, str], name: str) -> dict | None:
    """The release asset record for *name* (size/digest/state), or None."""
    query = (
        ".assets[] | select(.name == "
        + json.dumps(name)
        + ") | {name, size, digest, state}"
    )
    result = subprocess.run(
        [gh_exe, "api", f"repos/{Config.GH_REPO}/releases/tags/{Config.GH_TAG}",
         "--jq", query],
        capture_output=True, text=True, env=env,
    )
    if result.returncode != 0:
        _print(
            f"Could not read back the release assets:\n"
            f"{(result.stdout or '') + (result.stderr or '')}",
            color=_Term.RED,
        )
        return None
    out = (result.stdout or "").strip()
    if not out:
        return None
    try:
        # --jq emits one JSON object per matching asset (normally exactly one).
        return json.loads(out.splitlines()[0])
    except json.JSONDecodeError:
        return None


def _verify_remote_asset(
    platform: str, archive: str, gh_exe: str, env: dict[str, str]
) -> bool:
    """Compare the uploaded asset's size + sha256 digest with the local archive.

    Catches a truncated or partially processed upload, which `gh release upload`
    can report as success. ``digest`` is empty on older GitHub Enterprise
    servers, in which case only the size is compared.
    """
    name = Path(archive).name
    asset = _remote_asset(gh_exe, env, name)
    if asset is None:
        _print(
            f"Uploaded {name} but it does not appear on "
            f"{gh_release_url()} — treating the upload as failed.",
            color=_Term.RED,
        )
        return False
    local_size = Path(archive).stat().st_size
    local_digest = _sha256(archive)
    remote_size = int(asset.get("size") or 0)
    remote_digest = (asset.get("digest") or "").strip()
    state = asset.get("state") or ""
    if remote_size != local_size:
        _print(
            f"Uploaded {name} has size {remote_size:,} but the local archive is "
            f"{local_size:,} bytes.",
            color=_Term.RED,
        )
        return False
    if remote_digest and remote_digest != f"sha256:{local_digest}":
        _print(
            f"Uploaded {name} digest {remote_digest} does not match the local "
            f"sha256:{local_digest}.",
            color=_Term.RED,
        )
        return False
    _print(
        f"Release asset matches: {name} ({_human_size(remote_size)}, "
        f"{remote_digest or 'no digest reported'}, state={state or 'unknown'})",
        color=_Term.GREEN,
    )
    return True


def upload(
    platform: str, archive: str, gh_exe: str, env: dict[str, str]
) -> bool:
    """Upload *archive* to the release at gh_release_url() and verify it landed.

    Uses --clobber (re-publishing the same version replaces the same-name
    asset), retries transient failures, and finally compares the remote asset's
    size/digest with the local file. Returns True only when the upload is
    confirmed.
    """
    _print(
        f"\nUploading {platform} archive to {gh_release_url()}",
        color=_Term.BOLD,
    )
    cmd = [gh_exe, *Config.UPLOAD_ARGS, archive]
    _print(f"Running: {' '.join(cmd)}")
    last = ""
    for attempt in range(1, Config.UPLOAD_ATTEMPTS + 1):
        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        out = ((result.stdout or "") + (result.stderr or "")).strip()
        if result.returncode == 0:
            if out:
                _print(out)
            if _verify_remote_asset(platform, archive, gh_exe, env):
                _print(
                    f"Uploaded {Path(archive).name} to "
                    f"{Config.GH_REPO}@{Config.GH_TAG}.",
                    color=_Term.GREEN,
                )
                return True
            last = "the uploaded asset does not match the local archive"
        else:
            last = out or f"gh exited {result.returncode}"
        if attempt < Config.UPLOAD_ATTEMPTS:
            delay = Config.UPLOAD_BACKOFF_S * attempt
            _print(
                f"Upload attempt {attempt}/{Config.UPLOAD_ATTEMPTS} failed "
                f"({last}); retrying in {delay:.0f}s…",
                color=_Term.YELLOW,
            )
            time.sleep(delay)

    _print(f"gh upload failed for {platform}: {last}", color=_Term.RED)
    return False


# =============================================================================
# CLI
# =============================================================================

_EPILOG = """
Examples:
  python publish.py                          # build + package + upload all platforms
  python publish.py --platform windows       # Windows MSVC only
  python publish.py --platform linux         # Linux GCC only (native or via WSL)
  python publish.py --dry-run                # everything except the upload
  python publish.py --skip-build             # reuse bin/release, just package/verify/upload
  python publish.py --no-verify              # build + package, skip verification
  python publish.py --no-upload              # build + package, skip gh upload
  python publish.py --clean --jobs 8         # clean rebuild with 8 jobs

Archive naming: kimix_base-<platform>-<arch>-<version>.zip
  e.g. kimix_base-windows-x64-<version>.zip (written next to bin/release artifacts)

Version: read from version.txt in the project root ('<major>.<minor>.<patch>').
Build:   only the shipped target (bootstrap.py --target runtime_py).
Linux:   built natively on Linux hosts, or through WSL from a Windows host.
Upload:  verified archives go to the GitHub release
         https://github.com/Sikao-Engine/KimiX-native/releases/tag/Release
         via `gh release upload --clobber` (disable with --no-upload/--dry-run).
         Credentials come from GH_TOKEN/GITHUB_TOKEN, else an existing gh login,
         else the git credential helper for github.com.

Exit codes: 0 = all platforms built/packaged/verified/uploaded,
            1 = a build or package step failed (or bad input / gh missing),
            2 = verification failed, 3 = gh release upload failed.
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="publish.py",
        description=(
            "Build kimix-base in release mode (x64) for windows (MSVC) and/or "
            "linux (GCC, via WSL), then package bin/release/runtime_py.pyd "
            "into a ZIP archive and upload it to the GitHub release "
            f"({gh_release_url()}) with gh."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )

    build_group = parser.add_argument_group("Build options")
    build_group.add_argument(
        "--platform", "-p", choices=("all", "windows", "linux"), default="all",
        help="Platform(s) to build (default: all supported on this host).",
    )
    build_group.add_argument(
        "--arch", "-a", default=Config.ARCH, metavar="ARCH",
        help=f"Architecture to build (default: {Config.ARCH}).",
    )
    build_group.add_argument(
        "--jobs", "-j", type=int, default=Config.CPU_COUNT, metavar="N",
        help=f"Number of parallel build jobs (default: {Config.CPU_COUNT}).",
    )
    build_group.add_argument(
        "--clean", "-C", action="store_true",
        help="Clean build directories before configuring (bootstrap.py --clean).",
    )
    build_group.add_argument(
        "--verbose", "-v", action="store_true",
        help="Verbose build output.",
    )

    package_group = parser.add_argument_group("Package / verify options")
    package_group.add_argument(
        "--no-verify", action="store_true",
        help="Skip post-build verification (archive listing + artifact import).",
    )
    package_group.add_argument(
        "--skip-build", action="store_true",
        help=(
            "Reuse the artifacts already in bin/release instead of building. "
            "Turns a re-upload after a network/auth failure into seconds."
        ),
    )
    package_group.add_argument(
        "--7z", metavar="PATH", dest="sevenz",
        help="Path to the 7-Zip executable (default: auto-detect).",
    )

    upload_group = parser.add_argument_group("Upload options")
    upload_group.add_argument(
        "--no-upload", action="store_true",
        help=(
            "Skip 'gh release upload' of the packaged archives to the "
            f"{gh_release_url()} release (default: upload after success)."
        ),
    )
    upload_group.add_argument(
        "--dry-run", action="store_true",
        help=(
            "Build, package and verify everything but never upload or read the "
            "remote release (implies --no-upload; also skips the gh checks)."
        ),
    )

    xmake_group = parser.add_argument_group("bootstrap.py passthrough options")
    xmake_group.add_argument(
        "--xmake", "-x", metavar="PATH",
        help="Path to xmake executable (default: auto-detect).",
    )
    xmake_group.add_argument(
        "--no-download", action="store_true",
        help="Do not attempt to download xmake if not found.",
    )

    return parser


# =============================================================================
# Main
# =============================================================================


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    # Change to the script's directory (project root)
    script_dir = Path(__file__).resolve().parent
    os.chdir(script_dir)

    version = read_version()

    if args.sevenz:
        if not os.path.isfile(args.sevenz):
            _fail(f"--7z path does not exist: {args.sevenz}")
    elif find_7z() is None:
        _fail(
            "7-Zip not found. Install it (https://www.7-zip.org/) or pass "
            "--7z PATH."
        )

    platforms = resolve_platforms(args.platform)
    _print(
        f"Publishing {Config.MODE} / {args.arch} for: {', '.join(platforms)}"
    )
    _print(f"Build targets: {', '.join(Config.BUILD_TARGETS)}"
           + ("" if not args.skip_build else " (build skipped: --skip-build)"))

    # Fail fast on the upload prerequisites — before spending minutes on the
    # build — so a missing/unauthorized credential never wastes a compile.
    # resolve_gh_auth() also digs the token out of the git credential helper, so
    # a machine that can push but never ran `gh auth login` still publishes.
    gh_exe: str | None = None
    gh_env: dict[str, str] = dict(os.environ)
    upload_enabled = not (args.no_upload or args.dry_run)
    if upload_enabled:
        gh_exe = find_gh()
        if gh_exe is None:
            _fail(
                f"'{Config.GH_EXE}' not found on PATH but the run uploads to "
                f"{gh_release_url()}. Install the GitHub CLI, or pass "
                "--no-upload / --dry-run."
            )
        resolved, source = resolve_gh_auth(gh_exe)
        if resolved is None:
            _fail(
                f"cannot authenticate gh for {gh_release_url()}: {source}.\n"
                "Run `gh auth login`, export GH_TOKEN, or pass "
                "--no-upload / --dry-run."
            )
        gh_env = resolved
        account = _gh_account(gh_exe, gh_env)
        _print(
            f"Upload credentials: {source}"
            + (f" (account {account})" if account else ""),
            color=_Term.GREEN,
        )
    elif args.dry_run:
        _print("Dry run: packaging and verifying only, no upload.")

    results: dict[str, int] = {}
    digests: dict[str, tuple[str, int, str]] = {}
    for platform in platforms:
        if args.skip_build:
            _print(f"\nSkipping the {platform} build (--skip-build).")
            rc = 0
        else:
            rc = build_platform(platform, args)
        # Only the *first* build of the run performs the requested --clean:
        # bootstrap.py --clean wipes .xmake/, build/ AND bin/, which would
        # delete the archive already packaged for the previous platform
        # (e.g. `--platform all --clean` used to lose the linux ZIP when the
        # windows build ran second).
        args.clean = False
        if rc != 0:
            _print(f"Build FAILED for {platform} (exit {rc}).", color=_Term.RED)
            results[platform] = rc
            continue

        archive = package(platform, version)
        digests[platform] = (
            Path(archive).name,
            Path(archive).stat().st_size,
            _sha256(archive),
        )
        if not args.no_verify:
            ok = verify(platform, archive, version)
            results[platform] = 0 if ok else 2
        else:
            results[platform] = 0

        # Upload only after this platform built, packaged and verified clean.
        if results[platform] == 0 and gh_exe is not None:
            if not upload(platform, archive, gh_exe, gh_env):
                results[platform] = 3

    _print("\n" + "=" * 60)
    for platform, rc in results.items():
        status = {0: "OK", 2: "VERIFY FAILED", 3: "UPLOAD FAILED"}.get(
            rc, f"FAILED ({rc})"
        )
        color = _Term.GREEN if rc == 0 else _Term.RED
        _print(f"  {platform:<10} {status}", color=color)
        if platform in digests:
            name, size, sha = digests[platform]
            _print(f"             {name}  {_human_size(size)}  sha256:{sha}")
    _print("=" * 60)

    return 0 if all(rc == 0 for rc in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
