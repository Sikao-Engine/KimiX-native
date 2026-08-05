#!/usr/bin/env python3
# xmake: https://github.com/xmake-io/xmake
"""
bootstrap.py — Cross-platform C++ project bootstrap with xmake.

Detects toolchains, checks for or installs xmake, configures, builds,
and optionally runs tests. Works on Windows, Linux, and macOS.

Usage examples:
  python bootstrap.py                        # auto-detect, release build
  python bootstrap.py --debug                # debug build
  python bootstrap.py --toolchain clang-cl   # use a specific toolchain
  python bootstrap.py --test                 # build + run tests
  python bootstrap.py --clean --debug        # clean rebuild in debug mode
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
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
    IS_MACOS: ClassVar[bool] = sys.platform == "darwin"
    CPU_COUNT: ClassVar[int] = os.cpu_count() or 4

    # ------------------------------------------------------------------
    # Default toolchain per platform
    # ------------------------------------------------------------------
    DEFAULT_TOOLCHAIN: ClassVar[dict[str, str]] = {
        "win32": "msvc",
        "linux": "gcc",
        "darwin": "llvm",
    }

    # Toolchain names mapped to xmake `--toolchain=` values
    TOOLCHAIN_XMAKE_NAME: ClassVar[dict[str, str]] = {
        "msvc": "msvc",
        "clang-cl": "clang-cl",
        "llvm": "llvm",
        "clang": "clang",
        "gcc": "gcc",
        "g++": "gcc",
    }

    # Aliases for toolchain normalisation
    TOOLCHAIN_ALIASES: ClassVar[dict[str, str]] = {
        "clang": "llvm",
        "clang++": "llvm",
        "clang-cl": "clang-cl",
    }

    # ------------------------------------------------------------------
    # MSVC / vswhere
    # ------------------------------------------------------------------
    VSWHERE_EXE: ClassVar[str] = "vswhere.exe"
    VSWHERE_REL_PATH: ClassVar[tuple[str, ...]] = (
        "Microsoft Visual Studio", "Installer", "vswhere.exe",
    )
    VSWHERE_FALLBACK_PROGRAM_FILES: ClassVar[str] = r"C:\Program Files (x86)"
    VSWHERE_FIND_PATTERN: ClassVar[str] = "**/Auxiliary/Build/vcvars64.bat"
    VSWHERE_VERSION_PART: ClassVar[str] = "Microsoft Visual Studio"

    # vcvars environment activation
    VCVARS_ENV_DUMP_SCRIPT: ClassVar[str] = (
        "import os, json; "
        "print('[[ENV]]' + json.dumps(dict(os.environ)))"
    )
    VCVARS_ENV_MARKER: ClassVar[str] = "[[ENV]]"

    # ------------------------------------------------------------------
    # Compiler detection
    # ------------------------------------------------------------------
    GCC_NAMES: ClassVar[tuple[str, ...]] = ("gcc", "g++")
    GCC_VERSION_RANGE: ClassVar[range] = range(7, 20)
    LLVM_CLANG_NAMES: ClassVar[tuple[str, ...]] = ("clang", "clang-cl")

    # LLVM search paths per platform
    LLVM_SEARCH_DIRS_WINDOWS: ClassVar[tuple[str, ...]] = (
        r"C:\Program Files\LLVM\bin",
    )
    LLVM_MSVC_REL_PATH: ClassVar[tuple[str, ...]] = ("Llvm", "x64", "bin")
    LLVM_SEARCH_DIRS_LINUX: ClassVar[tuple[str, ...]] = (
        "/usr/bin", "/usr/local/bin",
    )
    LLVM_SEARCH_DIRS_MACOS: ClassVar[tuple[str, ...]] = (
        "/usr/local/opt/llvm/bin",
        "/opt/homebrew/opt/llvm/bin",
        "/usr/bin",
    )

    # ------------------------------------------------------------------
    # XMake
    # ------------------------------------------------------------------
    XMAKE_MIN_VERSION: ClassVar[str] = "3.0.6"
    XMAKE_FALLBACK_TAG: ClassVar[str] = "v3.0.9"
    XMAKE_DEPS_DIR: ClassVar[str] = ".deps"
    XMAKE_EXTRACT_SUBDIR: ClassVar[str] = "xmake"

    # Download URLs
    XMAKE_VERSION_SCRIPT_URL: ClassVar[str] = (
        "https://fastly.jsdelivr.net/gh/xmake-io/xmake@master/scripts/get.ps1"
    )
    XMAKE_DOWNLOAD_URL: ClassVar[str] = (
        "https://github.com/xmake-io/xmake/releases/download/{tag}/{file}"
    )
    XMAKE_WINDOWS_ARCHIVE: ClassVar[str] = "xmake-master.win64.zip"
    XMAKE_UNIX_ARCHIVE: ClassVar[str] = "xmake-master.tar.gz"

    # Post-extract binary paths (relative to extract dir)
    XMAKE_WINDOWS_BIN_REL: ClassVar[tuple[str, ...]] = ("xmake", "xmake.exe")
    XMAKE_UNIX_BIN_REL: ClassVar[tuple[str, ...]] = ("bin", "xmake")

    # Build-from-source commands (Unix only)
    XMAKE_BUILD_CONFIGURE: ClassVar[list[str]] = ["sh", "./configure"]
    XMAKE_BUILD_MAKE: ClassVar[list[str]] = ["make"]
    XMAKE_BUILD_INSTALL: ClassVar[list[str]] = ["make", "install", "PREFIX=."]

    # Install instructions displayed when xmake not found
    XMAKE_INSTALL_HELP_WINDOWS: ClassVar[str] = (
        "  Windows: irm https://xmake.io/psget.text | iex"
    )
    XMAKE_INSTALL_HELP_LINUX: ClassVar[str] = (
        "  Linux:   curl -fsSL https://xmake.io/shget.text | bash"
    )

    # Version-tag extraction from the PowerShell helper script
    XMAKE_VERSION_TAG_PREFIX: ClassVar[str] = '$LastRelease = "'
    XMAKE_VERSION_TAG_SUFFIX: ClassVar[str] = '"'

    # ------------------------------------------------------------------
    # Build
    # ------------------------------------------------------------------
    BUILD_MODE_DEBUG: ClassVar[str] = "debug"
    BUILD_MODE_RELEASE: ClassVar[str] = "release"

    # Directories removed on --clean
    CLEAN_DIRS: ClassVar[tuple[str, ...]] = (".xmake", "build", "bin")

    # xmake configure platform/arch flags
    XMAKE_PLATFORM_FLAGS: ClassVar[dict[str, tuple[str, ...]]] = {
        "linux": ("-p", "linux", "-a", "x86_64"),
        "darwin": ("-p", "macosx", "-a", "arm64"),
        # Windows uses auto-detection, no extra flags needed
    }

    # ------------------------------------------------------------------
    # Test
    # ------------------------------------------------------------------
    TEST_TARGET: ClassVar[str] = "kimix-test"
    TEST_RUN_ARGS: ClassVar[tuple[str, ...]] = ("run", "kimix-test")

    # ------------------------------------------------------------------
    # xmake config flags that are always passed
    # ------------------------------------------------------------------
    XMAKE_CONFIG_BASE_FLAGS: ClassVar[tuple[str, ...]] = ("-c", "-y")


# =============================================================================
# Helpers
# =============================================================================


def _print(msg: str, *, color: str | None = None) -> None:
    if color:
        print(f"{color}{msg}{_Term.RESET}")
    else:
        print(msg)


def _which(name: str) -> str | None:
    """Find an executable on PATH. Returns the full path or None."""
    if Config.IS_WINDOWS:
        result = shutil.which(name)
        if not result:
            for ext in (".exe", ".bat", ".cmd"):
                result = shutil.which(name + ext)
                if result:
                    break
        return result
    return shutil.which(name)


def _run_capture(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Run a command; capture output."""
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def _compiler_version(exe: str, flag: str = "--version") -> str | None:
    """Get the first line of version output from a compiler."""
    try:
        result = _run_capture([exe, flag])
        out = (result.stdout or "") + (result.stderr or "")
        return out.splitlines()[0].strip()
    except Exception:
        return None


def _running_as_root() -> bool:
    """True when running as root on Unix (geteuid == 0)."""
    geteuid = getattr(os, "geteuid", None)
    return bool(geteuid and geteuid() == 0)


def _xmake_env(base: dict[str, str] | None = None) -> dict[str, str]:
    """Environment for xmake invocations.

    xmake refuses to run as root unless XMAKE_ROOT=y is set (or --root is
    passed). Allow it explicitly when we are root (e.g. in containers/WSL).
    """
    env = dict(os.environ if base is None else base)
    if _running_as_root():
        env["XMAKE_ROOT"] = "y"
    return env


# =============================================================================
# Toolchain detection
# =============================================================================


def _find_vswhere() -> str | None:
    """Locate vswhere.exe. Returns path or None."""
    vswhere = _which(Config.VSWHERE_EXE)
    if vswhere:
        return vswhere
    prog_files = os.environ.get(
        "ProgramFiles(x86)", Config.VSWHERE_FALLBACK_PROGRAM_FILES,
    )
    candidate = Path(prog_files).joinpath(*Config.VSWHERE_REL_PATH)
    return str(candidate) if candidate.exists() else None


def find_msvc() -> dict[str, str]:
    """Find MSVC installations via vswhere; return {version: vcvars_path}."""
    vswhere = _find_vswhere()
    if not vswhere:
        return {}

    try:
        result = _run_capture([
            vswhere, "-format", "json", "-utf8", "-nologo",
            "-sort", "-products", "*", "-find", Config.VSWHERE_FIND_PATTERN,
        ])
        vcvars_paths = json.loads(result.stdout)
    except Exception:
        return {}

    msvc: dict[str, str] = {}
    for raw_path in vcvars_paths:
        p = Path(raw_path.replace("\\", "/"))
        try:
            idx = p.parts.index(Config.VSWHERE_VERSION_PART)
            version_dir = p.parts[idx + 1]
            if not version_dir.isdigit():
                continue
            msvc[version_dir] = str(p)
        except (ValueError, IndexError):
            continue
    return msvc


def _activate_msvc(vcvars: str) -> dict[str, str]:
    """Run vcvars64.bat and capture the environment diff."""
    try:
        result = subprocess.run(
            [vcvars, "&&", sys.executable, "-c", Config.VCVARS_ENV_DUMP_SCRIPT],
            capture_output=True, text=True, shell=True,
        )
        marker = Config.VCVARS_ENV_MARKER
        idx = result.stdout.find(marker)
        if idx >= 0:
            return json.loads(result.stdout[idx + len(marker):])
    except Exception:
        pass
    return {}


def find_gcc() -> dict[str, str]:
    """Find GCC installations; return {version_or_default: compiler_path}."""
    gcc_map: dict[str, str] = {}
    for exe in Config.GCC_NAMES:
        path = _which(exe)
        if path:
            ver = _compiler_version(path)
            if ver:
                gcc_map["default"] = path
                break
    for major in Config.GCC_VERSION_RANGE:
        for exe in (f"gcc-{major}", f"g++-{major}"):
            path = _which(exe)
            if path:
                gcc_map[str(major)] = path
                break
    return gcc_map


def find_llvm() -> dict[str, str]:
    """Find LLVM/Clang installations; return {clang_path: version_string}."""
    llvm_map: dict[str, str] = {}

    candidate_dirs: list[str] = []
    if Config.IS_WINDOWS:
        candidate_dirs.extend(Config.LLVM_SEARCH_DIRS_WINDOWS)
        msvc = find_msvc()
        for vcvars in msvc.values():
            llvm_dir = Path(vcvars).parent.parent.joinpath(*Config.LLVM_MSVC_REL_PATH)
            if llvm_dir.exists():
                candidate_dirs.append(str(llvm_dir))
    elif Config.IS_LINUX:
        candidate_dirs.extend(Config.LLVM_SEARCH_DIRS_LINUX)
    elif Config.IS_MACOS:
        candidate_dirs.extend(Config.LLVM_SEARCH_DIRS_MACOS)

    for d in candidate_dirs:
        for name in Config.LLVM_CLANG_NAMES:
            path = d / Path(name)
            if Config.IS_WINDOWS:
                path = path.with_suffix(".exe")
            if path.is_file():
                ver = _compiler_version(str(path))
                if ver:
                    llvm_map[str(path)] = ver
    return llvm_map


def detect_toolchains() -> dict[str, list[str]]:
    """Auto-detect available toolchains. Returns {toolchain_name: [version, ...]}."""
    result: dict[str, list[str]] = {}

    if Config.IS_WINDOWS:
        msvc = find_msvc()
        if msvc:
            result["msvc"] = sorted(msvc.keys())
        llvm = find_llvm()
        if llvm:
            if any("clang-cl" in k for k in llvm):
                result["clang-cl"] = ["detected"]
            if any("clang" in k.lower() and "clang-cl" not in k.lower() for k in llvm):
                result["llvm"] = ["detected"]
    elif Config.IS_LINUX:
        gcc = find_gcc()
        if gcc:
            result["gcc"] = sorted(gcc.keys())
        llvm = find_llvm()
        if llvm:
            result["llvm"] = ["detected"]
    elif Config.IS_MACOS:
        llvm = find_llvm()
        if llvm:
            result["llvm"] = ["detected"]
        gcc = find_gcc()
        if gcc:
            result["gcc"] = sorted(gcc.keys())

    return result


def default_toolchain() -> str:
    """Return the default toolchain for the current platform."""
    return Config.DEFAULT_TOOLCHAIN.get(sys.platform, "gcc")


# =============================================================================
# XMake installation
# =============================================================================


def _parse_version(text: str) -> tuple[int, ...] | None:
    """Extract a numeric version tuple (e.g. (3, 0, 9)) from text."""
    match = re.search(r"v?(\d+(?:\.\d+)+)", text)
    if not match:
        return None
    try:
        return tuple(int(p) for p in match.group(1).split("."))
    except ValueError:
        return None


def _xmake_bin_in_deps() -> str | None:
    """Check if xmake exists in the local DEPS_DIR."""
    deps = Path(Config.XMAKE_DEPS_DIR)
    if not deps.is_dir():
        return None
    name = "xmake.exe" if Config.IS_WINDOWS else "xmake"
    candidates = sorted(deps.glob(f"**/{name}"))
    return str(candidates[0]) if candidates else None


def check_xmake(xmake_exe: str = "xmake") -> str | None:
    """Check if xmake is installed and new enough. Returns path or None."""
    exe = _which(xmake_exe) or _xmake_bin_in_deps()
    if not exe:
        return None
    try:
        result = _run_capture([exe, "--version"], env=_xmake_env())
        if result.returncode != 0:
            return None
        version = _parse_version(result.stdout or "")
        minimum = _parse_version(Config.XMAKE_MIN_VERSION)
        if version and minimum and version < minimum:
            _print(
                f"xmake {exe} is too old "
                f"({'.'.join(map(str, version))} < {Config.XMAKE_MIN_VERSION}).",
                color=_Term.YELLOW,
            )
            return None
        return exe
    except Exception:
        pass
    return None


def _fetch_xmake_tag() -> str:
    """Fetch the latest xmake release tag from the install script."""
    try:
        with urllib.request.urlopen(Config.XMAKE_VERSION_SCRIPT_URL) as resp:
            script = resp.read().decode("utf-8")
        prefix = Config.XMAKE_VERSION_TAG_PREFIX
        suffix = Config.XMAKE_VERSION_TAG_SUFFIX
        return script.split(prefix)[1].split(suffix)[0]
    except Exception:
        _print(
            f"Failed to detect latest xmake version. "
            f"Trying {Config.XMAKE_FALLBACK_TAG}.",
            color=_Term.RED,
        )
        return Config.XMAKE_FALLBACK_TAG


def download_xmake() -> str | None:
    """Download and extract xmake to DEPS_DIR. Returns path to xmake binary or None."""
    _print("Downloading xmake...", color=_Term.YELLOW)

    tag = _fetch_xmake_tag()
    os.makedirs(Config.XMAKE_DEPS_DIR, exist_ok=True)

    if Config.IS_WINDOWS:
        archive = Config.XMAKE_WINDOWS_ARCHIVE
    else:
        archive = Config.XMAKE_UNIX_ARCHIVE

    url = Config.XMAKE_DOWNLOAD_URL.format(tag=tag, file=archive)
    zip_path = os.path.join(Config.XMAKE_DEPS_DIR, archive)

    _print(f"  Fetching {url}")
    try:
        urllib.request.urlretrieve(url, zip_path)
    except Exception as e:
        _print(f"Failed to download xmake: {e}", color=_Term.RED)
        return None

    extract_dir = os.path.join(Config.XMAKE_DEPS_DIR, Config.XMAKE_EXTRACT_SUBDIR)
    if os.path.exists(extract_dir):
        shutil.rmtree(extract_dir)
    os.makedirs(extract_dir, exist_ok=True)

    _print(f"  Extracting to {extract_dir}")

    if Config.IS_WINDOWS:
        with zipfile.ZipFile(zip_path, "r") as zf:
            zf.extractall(extract_dir)
        xmake_exe = str(Path(extract_dir).joinpath(*Config.XMAKE_WINDOWS_BIN_REL))
    else:
        import tarfile
        with tarfile.open(zip_path, "r:gz") as tf:
            tf.extractall(extract_dir)
        dirs = [
            d for d in os.listdir(extract_dir)
            if os.path.isdir(os.path.join(extract_dir, d))
        ]
        if not dirs:
            _print("Failed to find extracted xmake directory.", color=_Term.RED)
            return None
        src_dir = os.path.join(extract_dir, dirs[0])
        build_env = _xmake_env()
        subprocess.run(
            Config.XMAKE_BUILD_CONFIGURE, cwd=src_dir, check=True, env=build_env,
        )
        subprocess.run(
            Config.XMAKE_BUILD_MAKE + [f"-j{Config.CPU_COUNT}"],
            cwd=src_dir, check=True, env=build_env,
        )
        subprocess.run(
            Config.XMAKE_BUILD_INSTALL, cwd=src_dir, check=True, env=build_env,
        )
        xmake_exe = str(Path(src_dir).joinpath(*Config.XMAKE_UNIX_BIN_REL))

    if not os.path.exists(xmake_exe):
        _print(f"xmake binary not found at {xmake_exe}", color=_Term.RED)
        return None

    _print(f"  xmake installed: {xmake_exe}", color=_Term.GREEN)
    return xmake_exe


def ensure_xmake(args) -> str:
    """Ensure xmake is available. Returns path to xmake binary. Exits on failure."""
    xmake_exe = args.xmake or "xmake"
    exe = check_xmake(xmake_exe)
    if exe:
        _print(f"xmake found: {exe}", color=_Term.GREEN)
        return exe

    _print("xmake not found.", color=_Term.YELLOW)
    if args.no_download:
        _print(
            "Automatic download disabled (--no-download). "
            "Please install xmake manually.",
            color=_Term.RED,
        )
        _print(Config.XMAKE_INSTALL_HELP_WINDOWS, color=_Term.RED)
        _print(Config.XMAKE_INSTALL_HELP_LINUX, color=_Term.RED)
        sys.exit(1)

    exe = download_xmake()
    if not exe:
        _print("Failed to download xmake. Please install manually.", color=_Term.RED)
        sys.exit(1)
    return exe


# =============================================================================
# Build workflow
# =============================================================================


def _latest_msvc_env() -> dict[str, str]:
    """Return the environment diff for the latest MSVC installation."""
    msvc = find_msvc()
    if not msvc:
        return {}
    versions = sorted(msvc.keys())
    vcvars = msvc[versions[-1]]
    _print(f"  Activating MSVC {versions[-1]} environment: {vcvars}")
    env = _activate_msvc(vcvars)
    if not env:
        return {}
    return {
        k: v for k, v in env.items()
        if k not in os.environ or os.environ[k] != v
    }


def prepare_environment(toolchain: str) -> dict[str, str]:
    """Prepare the environment for the given toolchain."""
    if not Config.IS_WINDOWS:
        return {}

    # clang-cl and llvm on Windows still need the MSVC linker environment
    need_msvc = toolchain in ("msvc", "clang-cl", "llvm")
    if need_msvc:
        return _latest_msvc_env()

    return {}


def _toolchain_base(toolchain: str) -> str:
    """Normalise a toolchain string to its base name.

    Multi-word names (e.g. 'clang-cl') are looked up verbatim first;
    only then is a version suffix ('gcc-13' -> 'gcc') stripped.
    """
    tc = toolchain.lower()
    if tc in Config.TOOLCHAIN_ALIASES or tc in Config.TOOLCHAIN_XMAKE_NAME:
        return Config.TOOLCHAIN_ALIASES.get(tc, tc)
    head = tc.split("-")[0]
    return Config.TOOLCHAIN_ALIASES.get(head, head)


def resolve_toolchain(toolchain: str | None, available: dict[str, list[str]]) -> str:
    """Resolve the toolchain string. Validates it's available; exits on error."""
    tc = toolchain.lower() if toolchain else default_toolchain()
    base = _toolchain_base(tc)

    if base not in available:
        _print(
            f"Toolchain '{tc}' not found. Available: {list(available.keys())}",
            color=_Term.RED,
        )
        sys.exit(1)

    return tc


def _xmake_toolchain_name(toolchain: str) -> str:
    """Map a user-facing toolchain string to xmake's --toolchain= name."""
    tc = toolchain.lower()
    if tc in Config.TOOLCHAIN_XMAKE_NAME:
        return Config.TOOLCHAIN_XMAKE_NAME[tc]
    head = tc.split("-")[0]
    return Config.TOOLCHAIN_XMAKE_NAME.get(head, head)


def _xmake_config_flags(mode: str, xmake_tc: str) -> list[str]:
    """Build the argument list for `xmake f ...`."""
    flags = [
        "f", "-m", mode,
        f"--toolchain={xmake_tc}",
        *Config.XMAKE_CONFIG_BASE_FLAGS,
    ]
    extra = Config.XMAKE_PLATFORM_FLAGS.get(sys.platform, ())
    flags.extend(extra)
    return flags


def config_and_build(args) -> int:
    """Configure and build the project. Returns exit code."""
    xmake = ensure_xmake(args)

    # Detect available toolchains
    available = detect_toolchains()
    _print(f"Platform: {sys.platform} | Available toolchains: {list(available.keys())}")

    # Resolve toolchain
    toolchain = resolve_toolchain(args.toolchain, available)
    _print(f"Toolchain: {toolchain}")

    # Prepare environment
    extra_env = prepare_environment(toolchain)
    env = _xmake_env()
    env.update(extra_env)

    # Determine build mode
    mode = Config.BUILD_MODE_DEBUG if args.debug else Config.BUILD_MODE_RELEASE
    _print(f"Build mode: {mode}")

    # Clean if requested
    if args.clean:
        _print("Cleaning...", color=_Term.YELLOW)
        for d in Config.CLEAN_DIRS:
            if os.path.exists(d):
                shutil.rmtree(d)
                _print(f"  Removed {d}")

    # Configure
    config_cmd = [xmake] + _xmake_config_flags(mode, _xmake_toolchain_name(toolchain))
    _print(f"\nConfiguring: {' '.join(config_cmd)}")
    result = subprocess.run(config_cmd, env=env)
    if result.returncode != 0:
        _print("Configuration failed.", color=_Term.RED)
        return result.returncode

    # Build
    build_cmd = [xmake, "build", "-j", str(args.jobs)]
    if args.verbose:
        build_cmd.append("-v")

    _print(f"\nBuilding: {' '.join(build_cmd)}")
    result = subprocess.run(build_cmd, env=env)
    if result.returncode != 0:
        _print("Build failed.", color=_Term.RED)
        return result.returncode

    _print("\nBuild succeeded!", color=_Term.GREEN)
    return 0


def run_tests(args) -> int:
    """Run the project tests via xmake run. Returns exit code."""
    xmake = ensure_xmake(args)

    _print(f"\nRunning tests: {Config.TEST_TARGET}")
    result = subprocess.run([xmake, *Config.TEST_RUN_ARGS], env=_xmake_env())
    if result.returncode != 0:
        _print("Tests failed!", color=_Term.RED)
        return result.returncode

    _print("All tests passed!", color=_Term.GREEN)
    return 0


# =============================================================================
# CLI
# =============================================================================

_EPILOG = """
Examples:
  python bootstrap.py                        # auto-detect, release build
  python bootstrap.py --debug                # debug build with auto toolchain
  python bootstrap.py --toolchain gcc        # use GCC explicitly
  python bootstrap.py --toolchain clang-cl   # use Clang-CL on Windows
  python bootstrap.py --test                 # build + run tests
  python bootstrap.py --clean --debug --test # clean debug build + test

Supported toolchains:
  Windows:  msvc (default), clang-cl, llvm
  Linux:    gcc (default), llvm
  macOS:    llvm (default), gcc
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bootstrap.py",
        description="Cross-platform C++ project bootstrap with xmake.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )

    build_group = parser.add_argument_group("Build options")
    build_group.add_argument(
        "--debug", "-d", action="store_true",
        help="Build in debug mode (default: release).",
    )
    build_group.add_argument(
        "--toolchain", "-t", metavar="TC",
        help="Toolchain to use (e.g. msvc, clang-cl, llvm, gcc). "
             "Auto-detected if omitted.",
    )
    build_group.add_argument(
        "--jobs", "-j", type=int, default=Config.CPU_COUNT, metavar="N",
        help=f"Number of parallel build jobs (default: {Config.CPU_COUNT}).",
    )
    build_group.add_argument(
        "--clean", "-C", action="store_true",
        help="Clean build directories before configuring.",
    )
    build_group.add_argument(
        "--verbose", "-v", action="store_true",
        help="Verbose build output.",
    )

    test_group = parser.add_argument_group("Test options")
    test_group.add_argument(
        "--test", "-T", action="store_true",
        help="Run tests after building.",
    )

    xmake_group = parser.add_argument_group("xmake options")
    xmake_group.add_argument(
        "--xmake", "-x", metavar="PATH",
        help="Path to xmake executable (default: auto-detect).",
    )
    xmake_group.add_argument(
        "--no-download", action="store_true",
        help="Do not attempt to download xmake if not found.",
    )

    env_group = parser.add_argument_group("Environment options")
    env_group.add_argument(
        "--list-toolchains", action="store_true",
        help="List detected toolchains and exit.",
    )
    env_group.add_argument(
        "--list-env", action="store_true",
        help="Show detected platform/environment info and exit.",
    )

    return parser


# =============================================================================
# Info commands
# =============================================================================


def _cmd_list_toolchains() -> int:
    tc = detect_toolchains()
    _print(f"Platform: {sys.platform} ({platform.machine()})")
    _print(f"Default toolchain: {default_toolchain()}")
    _print("Available toolchains:")
    for name, versions in tc.items():
        _print(f"  {name}: {', '.join(versions)}")
    return 0


def _cmd_list_env() -> int:
    _print(f"Platform:      {sys.platform} ({platform.machine()})")
    _print(f"Python:        {sys.version}")
    _print(f"CPU count:     {Config.CPU_COUNT}")
    _print(f"Default TC:    {default_toolchain()}")
    tc = detect_toolchains()
    _print("Toolchains:")
    for name, versions in tc.items():
        _print(f"  {name}: {', '.join(versions)}")
    xmake = check_xmake()
    _print(f"xmake:         {xmake or 'not found'}")
    if Config.IS_WINDOWS:
        msvc = find_msvc()
        _print(f"MSVC:          {sorted(msvc.keys()) if msvc else 'not found'}")
    if Config.IS_LINUX or Config.IS_MACOS:
        gcc = find_gcc()
        _print(f"GCC:           {sorted(gcc.keys()) if gcc else 'not found'}")
    llvm = find_llvm()
    _print(f"LLVM:          {len(llvm)} installation(s)" if llvm else "LLVM:          not found")
    return 0


# =============================================================================
# Main
# =============================================================================


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    # Change to the script's directory
    script_dir = Path(__file__).resolve().parent
    os.chdir(script_dir)

    if args.list_toolchains:
        return _cmd_list_toolchains()
    if args.list_env:
        return _cmd_list_env()

    # Build
    rc = config_and_build(args)
    if rc != 0:
        return rc

    # Tests
    if args.test:
        return run_tests(args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
