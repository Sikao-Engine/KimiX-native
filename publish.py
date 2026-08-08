#!/usr/bin/env python3
"""
publish.py — Build and package release archives for kimix-base.

Builds the project in release mode for x64 with:
  Windows: MSVC via xmake (native host build)
  Linux:   GCC  via xmake (native on Linux, or through WSL when run on Windows)

Then packages the result (``runtime_py.pyd`` on Windows / ``runtime_py.so`` on
Linux, from ``bin/release``) into a ZIP archive named
``kimix_base-<platform>-<arch>-<version>.zip``.

The archive is a plain ZIP (built with 7-Zip's `-tzip` / Deflate), NOT a 7z:
the previous `.7z` release used the BCJ2 + LZMA2 filters that `py7zr` cannot
decompress, so the release format was switched to standard ZIP.

The version string is read from `version.txt` in the project root — the single
config file for the version. Nothing in the repo hard-codes it: xmake generates
the C++ version header from it at build time, and the Python shim / tests read
it directly. `publish.py` refuses to run if it is missing or not in `X.Y.Z`
form.

Usage examples:
  python publish.py                          # build + package all supported platforms
  python publish.py --platform windows       # Windows MSVC only
  python publish.py --platform linux         # Linux GCC only (native or via WSL)
  python publish.py --platform all --no-verify
  python publish.py --clean --jobs 8
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
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

    # Artifacts packaged into the archive (relative to RELEASE_DIR).
    # Windows ships the CPython extension under its native .pyd name; Linux
    # must use the .so suffix (CPython on Linux only imports *.so modules).
    ARTIFACTS_WINDOWS: ClassVar[tuple[str, ...]] = ("runtime_py.pyd",)
    ARTIFACTS_LINUX: ClassVar[tuple[str, ...]] = ("runtime_py.so",)

    @staticmethod
    def artifacts_for(platform: str) -> tuple[str, ...]:
        """Artifact names to package for *platform* (relative to RELEASE_DIR)."""
        if platform == "windows":
            return Config.ARTIFACTS_WINDOWS
        return Config.ARTIFACTS_LINUX

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
    if args.clean:
        flags.append("--clean")
    if args.verbose:
        flags.append("--verbose")
    if args.xmake:
        flags.extend(["--xmake", args.xmake])
    if args.no_download:
        flags.append("--no-download")
    return flags


def build_windows(args) -> int:
    """Build the windows target natively with MSVC via bootstrap.py."""
    cmd = [sys.executable, *_bootstrap_flags(args, "msvc")]
    _print(f"\nBuilding windows (MSVC): {' '.join(cmd)}", color=_Term.BOLD)
    return subprocess.run(cmd).returncode


def build_linux(args) -> int:
    """Build the linux target with GCC — natively on Linux, or via WSL from Windows."""
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
    """Turn the --platform choice into the list of platforms to build."""
    if choice == "all":
        platforms = ["windows"] if Config.IS_WINDOWS else []
        if Config.IS_LINUX or wsl_available():
            platforms.append("linux")
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

    artifacts = Config.artifacts_for(platform)
    release_dir = Path(Config.RELEASE_DIR)
    missing = [
        a for a in artifacts
        if not (release_dir / a).is_file()
    ]
    if missing:
        _fail(
            f"Missing build artifacts in {release_dir}: {', '.join(missing)}. "
            f"Run the build first (publish.py builds automatically)."
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


def _verify_windows_pyd(version: str) -> bool:
    """Import runtime_py.pyd from bin/release and check the reported version."""
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
    """Verify the archive contents (and the pyd import on Windows)."""
    sevenz = find_7z()
    ok = True
    artifacts = Config.artifacts_for(platform)

    _print(f"\nVerifying {platform} archive: {archive}", color=_Term.BOLD)
    if sevenz is None or not _archive_contains(sevenz, archive, artifacts):
        ok = False

    if ok and platform == "windows":
        ok = _verify_windows_pyd(version)

    if ok:
        _print(f"Verification passed for {platform}.", color=_Term.GREEN)
    else:
        _print(f"Verification FAILED for {platform}.", color=_Term.RED)
    return ok


# =============================================================================
# CLI
# =============================================================================

_EPILOG = """
Examples:
  python publish.py                          # build + package all platforms
  python publish.py --platform windows       # Windows MSVC only
  python publish.py --platform linux         # Linux GCC only (native or via WSL)
  python publish.py --no-verify              # build + package, skip verification
  python publish.py --clean --jobs 8         # clean rebuild with 8 jobs

Archive naming: kimix_base-<platform>-<arch>-<version>.zip
  e.g. kimix_base-windows-x64-<version>.zip (written next to bin/release artifacts)

Version: read from version.txt in the project root ('<major>.<minor>.<patch>').
Linux:   built natively on Linux hosts, or through WSL from a Windows host.
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="publish.py",
        description=(
            "Build kimix-base in release mode (x64) for windows (MSVC) and/or "
            "linux (GCC, via WSL), then package bin/release/runtime_py.pyd "
            "into a ZIP archive."
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
        help="Skip post-build verification (archive listing + pyd import).",
    )
    package_group.add_argument(
        "--7z", metavar="PATH", dest="sevenz",
        help="Path to the 7-Zip executable (default: auto-detect).",
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

    results: dict[str, int] = {}
    for platform in platforms:
        rc = build_platform(platform, args)
        if rc != 0:
            _print(f"Build FAILED for {platform} (exit {rc}).", color=_Term.RED)
            results[platform] = rc
            continue

        archive = package(platform, version)
        if not args.no_verify:
            ok = verify(platform, archive, version)
            results[platform] = 0 if ok else 2
        else:
            results[platform] = 0

    _print("\n" + "=" * 60)
    for platform, rc in results.items():
        status = "OK" if rc == 0 else f"FAILED ({rc})"
        color = _Term.GREEN if rc == 0 else _Term.RED
        _print(f"  {platform:<10} {status}", color=color)
    _print("=" * 60)

    return 0 if all(rc == 0 for rc in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
