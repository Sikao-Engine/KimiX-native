#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""clangd_symbols.py — export a C++ symbol/using-directive index via clangd.

`detect_unity_build.py` is a fast pure-source lexer, but a lexer cannot see
resolved namespaces, overload signatures, or transitive `using namespace`
effects.  This script asks the real compiler front end (clangd) for the
document-symbol tree of each translation unit and turns it into a flat index:

    {
      "namespaces": ["kimix", "kimix::graph_db", ...],
      "symbols": [
        {"file": "src/graph_db/graph.cpp", "kind": 12, "name": "guid_key",
         "qualified": "kimix::graph_db::src_graph_db_graph::guid_key",
         "namespace": "kimix::graph_db::src_graph_db_graph", "container": "",
         "line": 1193, "signature": "(const kimix::Guid &)",
         "detail": "const kimix::Guid &(const kimix::Guid &) noexcept"},
        ...
      ],
      "using_directives": [
        {"file": "src/graph_db/graph.cpp", "line": 1358,
         "scope": "kimix::graph_db", "target": "src_graph_db_graph",
         "resolved": "kimix::graph_db::src_graph_db_graph"},
        ...
      ]
    }

The index is what `detect_unity_build.py --clangd` compares: two functions
with the same unqualified name and the same parameter list, in two namespaces
that become visible at one scope through `using namespace` directives, are an
ambiguous unqualified call once the files share a unity batch.

Usage:
    python scripts/clangd_symbols.py --out build/symbols.json
    python scripts/clangd_symbols.py --root . src/graph_db/graph.cpp
    python scripts/clangd_symbols.py --all --out build/symbols.json

Flags:
    --root DIR                  project root (default: parent of scripts/)
    --clangd PATH               clangd executable (default: clangd)
    --compile-commands-dir DIR  directory holding compile_commands.json
    --out FILE                  write the JSON index (default: stdout)
    --all                       export every first-party .cpp/.h, not just
                                the files given on the command line
    --quiet                     print only the summary line
    --jobs N      clangd processes to run in parallel (default 4)

Exit codes: 0 = index written, 1 = clangd/parse failure for every file,
2 = bad input / no compile_commands.json.
"""
import concurrent.futures
import argparse
import io
import json
import os
import subprocess
import sys

CPP_EXTS = (".cpp", ".cc", ".cxx")
HEADER_EXTS = (".h", ".hpp", ".hh", ".hxx")
SKIP_PREFIXES = ("src/ext/", "src/core/lmdb/", "src/core/detail/", "tests/ut/")


def is_first_party(rel):
    rel = rel.replace("\\", "/")
    return not rel.startswith(SKIP_PREFIXES)


def find_compile_commands(root):
    for d in (os.path.join(root, ".vscode"), os.path.join(root, "build")):
        if os.path.isfile(os.path.join(d, "compile_commands.json")):
            return d
    return None


# ---------------------------------------------------------------------------
# Minimal clangd LSP client (stdlib only; check_cpp_syntax.py uses orjson)
# ---------------------------------------------------------------------------

class ClangdClient:
    def __init__(self, clangd_path="clangd", compile_commands_dir=None,
                 verbose=False):
        self.clangd_path = clangd_path
        self.compile_commands_dir = compile_commands_dir
        self.verbose = verbose
        self.proc = None
        self._id = 0

    def start(self):
        cmd = [self.clangd_path]
        if self.compile_commands_dir:
            cmd.append("--compile-commands-dir=" + self.compile_commands_dir)
        cmd += ["--background-index=0", "--clang-tidy=false", "--log=error"]
        if self.verbose:
            print("[clangd] " + " ".join(cmd), file=sys.stderr)
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)

    def stop(self):
        if not self.proc:
            return
        try:
            self._request("shutdown", {})
            self._notify("exit", {})
            self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        finally:
            self.proc = None

    def _send(self, message):
        body = json.dumps(message).encode("utf-8")
        self.proc.stdin.write(
            ("Content-Length: %d\r\n\r\n" % len(body)).encode("utf-8") + body)
        self.proc.stdin.flush()

    def _read(self):
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            ch = self.proc.stdout.read(1)
            if not ch:
                return None
            header += ch
        length = 0
        for line in header.decode("ascii", "replace").split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        if length <= 0:
            return None
        return json.loads(self.proc.stdout.read(length))

    def _request(self, method, params):
        self._id += 1
        rid = self._id
        self._send({"jsonrpc": "2.0", "id": rid, "method": method,
                    "params": params})
        while True:
            msg = self._read()
            if msg is None:
                raise RuntimeError("clangd exited while waiting for %s" %
                                   method)
            if msg.get("id") == rid:
                if "error" in msg:
                    raise RuntimeError("clangd %s: %s" %
                                       (method, msg["error"]))
                return msg.get("result")

    def _notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def initialize(self, root_uri):
        self._request("initialize", {
            "processId": os.getpid(),
            "rootUri": root_uri,
            "capabilities": {
                "textDocument": {
                    "documentSymbol": {
                        "hierarchicalDocumentSymbolSupport": True,
                    },
                },
            },
            "workspaceFolders": [{"uri": root_uri, "name": "root"}],
        })
        self._notify("initialized", {})

    def open(self, path, text):
        self._notify("textDocument/didOpen", {
            "textDocument": {
                "uri": path_uri(path),
                "languageId": "cpp",
                "version": 1,
                "text": text,
            },
        })

    def close(self, path):
        self._notify("textDocument/didClose", {
            "textDocument": {"uri": path_uri(path)},
        })

    def document_symbols(self, path):
        return self._request("textDocument/documentSymbol", {
            "textDocument": {"uri": path_uri(path)},
        })


def path_uri(path):
    from pathlib import Path
    return Path(path).resolve().as_uri()


# ---------------------------------------------------------------------------
# Symbol tree -> flat index
# ---------------------------------------------------------------------------

NAMESPACE = 3
CLASS = 5
METHOD = 6
FIELD = 8
CONSTRUCTOR = 9
ENUM = 10
INTERFACE = 11
FUNCTION = 12
VARIABLE = 13
STRUCT = 23
OPERATOR = 25
CONTAINER_KINDS = (NAMESPACE, CLASS, ENUM, INTERFACE, STRUCT)


def signature_params(detail):
    """Extract the parameter list from clangd's `detail` string.

    `detail` looks like `const kimix::Guid &(const kimix::Guid &) noexcept`
    or `(size_t) noexcept` (constructor) or `template bool (const T &)`.
    Group by parameters only: two functions with the same name and parameters
    are ambiguous unqualified calls even if their return types differ.
    """
    if not detail:
        return ""
    start = detail.find("(")
    if start < 0:
        return detail
    depth = 0
    for i in range(start, len(detail)):
        ch = detail[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return detail[start:i + 1]
    return detail[start:]


def join_scope(*parts):
    return "::".join(p for p in parts if p)


def walk_tree(nodes, namespace, container, relpath, out, local=False):
    for node in nodes or []:
        kind = node.get("kind")
        name = (node.get("name") or "").strip()
        detail = node.get("detail") or ""
        sel = node.get("selectionRange") or node.get("range") or {}
        line = sel.get("start", {}).get("line", -1) + 1

        if not name:
            continue

        if kind == NAMESPACE:
            out["namespaces"].add(name if not namespace
                                  else namespace + "::" + name)
            walk_tree(node.get("children"), join_scope(namespace, name),
                      "", relpath, out, local)
            continue

        if kind in CONTAINER_KINDS:
            # A class/struct/enum is a container for members, but not a
            # namespace-scope name for unqualified lookup.
            out["namespaces"].add(namespace)
            walk_tree(node.get("children"), namespace,
                      join_scope(container, name), relpath, out, local)
            continue

        if name.startswith("using namespace"):
            if local:
                continue  # function-local: cannot affect a batch
            target = name[len("using namespace"):].strip()
            out["using_directives"].append({
                "file": relpath,
                "line": line,
                "scope": namespace,
                "target": target,
            })
            continue

        # Out-of-line member definitions show up as `Class::method` at
        # namespace scope; split the class out of the name.
        symbol_name = name
        symbol_container = container
        if "::" in name and kind in (FUNCTION, METHOD, CONSTRUCTOR, OPERATOR):
            parts = name.split("::")
            symbol_name = parts[-1]
            symbol_container = join_scope(container, *parts[:-1])
        qualified = join_scope(namespace, symbol_container, symbol_name)
        out["namespaces"].add(namespace)
        out["symbols"].append({
            "file": relpath,
            "kind": kind,
            "name": symbol_name,
            "qualified": qualified,
            "namespace": namespace,
            "container": symbol_container,
            "line": line,
            "signature": signature_params(detail),
            "detail": detail,
        })
        if node.get("children"):
            walk_tree(node["children"], namespace,
                      join_scope(symbol_container, symbol_name), relpath, out,
                      local or kind in (METHOD, CONSTRUCTOR, FUNCTION, OPERATOR))


def resolve_target(scope, raw, namespaces):
    raw = raw.strip()
    if not raw:
        return raw
    if raw.startswith("::"):
        candidates = [raw[2:]]
    else:
        candidates = []
        parts = scope.split("::") if scope else []
        for i in range(len(parts), -1, -1):
            prefix = "::".join(parts[:i])
            candidates.append(prefix + "::" + raw if prefix else raw)
    for cand in candidates:
        if cand in namespaces:
            return cand
    # Namespace aliases / headers parsed with fallback flags: fall back to a
    # unique suffix match, otherwise keep the spelling.
    matches = [ns for ns in namespaces
               if ns == raw or ns.endswith("::" + raw)]
    if len(matches) == 1:
        return matches[0]
    return raw


def _export_chunk(root, files, clangd_path, compile_commands_dir, verbose):
    """Export one slice of the files through one clangd process."""
    from pathlib import Path
    out = {"files": [], "namespaces": set(), "symbols": [],
           "using_directives": []}
    client = ClangdClient(clangd_path, compile_commands_dir, verbose)
    client.start()
    try:
        client.initialize(Path(compile_commands_dir or root).as_uri())
        for rel in files:
            path = os.path.join(root, rel)
            if not os.path.isfile(path):
                continue
            try:
                with io.open(path, "r", encoding="utf-8",
                             errors="replace", newline="") as f:
                    text = f.read()
                client.open(path, text)
                tree = client.document_symbols(path)
                client.close(path)
            except Exception as ex:
                if verbose:
                    print("[clangd] skip %s: %s" % (rel, ex),
                          file=sys.stderr)
                continue
            if not isinstance(tree, list):
                continue
            out["files"].append(rel)
            walk_tree(tree, "", "", rel, out)
    finally:
        client.stop()
    return out


def export_symbols(root, files, clangd_path="clangd", compile_commands_dir=None,
                   verbose=False, jobs=4):
    """Return the symbol index for `files` (paths relative to root).

    Files are spread over `jobs` clangd processes: one clangd parses one file
    at a time, and a unity-build lint can involve a few hundred files.
    """
    root = os.path.abspath(root)
    if compile_commands_dir is None:
        compile_commands_dir = find_compile_commands(root)
    files = sorted(set(f.replace("\\", "/") for f in files))
    jobs = max(1, min(int(jobs or 1), len(files) or 1))
    chunks = [files[i::jobs] for i in range(jobs)]
    partials = []
    if jobs == 1:
        partials.append(_export_chunk(root, chunks[0], clangd_path,
                                      compile_commands_dir, verbose))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(_export_chunk, root, chunk, clangd_path,
                                   compile_commands_dir, verbose)
                       for chunk in chunks if chunk]
            for future in futures:
                partials.append(future.result())

    out = {"root": root, "files": [], "namespaces": set(), "symbols": [],
           "using_directives": []}
    for part in partials:
        out["files"].extend(part["files"])
        out["namespaces"].update(part["namespaces"])
        out["symbols"].extend(part["symbols"])
        out["using_directives"].extend(part["using_directives"])

    namespaces = out["namespaces"]
    for directive in out["using_directives"]:
        directive["resolved"] = resolve_target(
            directive["scope"], directive["target"], namespaces)
    out["namespaces"] = sorted(namespaces)
    out["files"].sort()
    out["symbols"].sort(key=lambda s: (s["file"], s["line"], s["qualified"]))
    out["using_directives"].sort(key=lambda d: (d["file"], d["line"]))
    return out



def collect_files(root, all_files=False, given=None):
    root = os.path.abspath(root)
    if given:
        rels = []
        for path in given:
            abs_path = path if os.path.isabs(path) else os.path.join(root, path)
            rel = os.path.relpath(abs_path, root).replace("\\", "/")
            rels.append(rel)
        return sorted(set(rels))
    if not all_files:
        return []
    rels = []
    for base in ("src", "tests"):
        base_dir = os.path.join(root, base)
        for dirpath, dirnames, filenames in os.walk(base_dir):
            dirnames[:] = [d for d in dirnames
                           if not (base == "src" and d == "ext")]
            for fn in sorted(filenames):
                if fn.lower().endswith(CPP_EXTS + HEADER_EXTS):
                    rel = os.path.relpath(os.path.join(dirpath, fn), root)
                    rel = rel.replace(os.sep, "/")
                    if is_first_party(rel):
                        rels.append(rel)
    return sorted(rels)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Export a clangd C++ symbol index.")
    ap.add_argument("--root", default=None,
                    help="project root (default: parent of scripts/)")
    ap.add_argument("--clangd", default="clangd", help="clangd executable")
    ap.add_argument("--compile-commands-dir", default=None)
    ap.add_argument("--out", default=None, help="write JSON here (default stdout)")
    ap.add_argument("--all", action="store_true",
                    help="export every first-party source/header")
    ap.add_argument("--jobs", type=int, default=4,
                    help="clangd processes to run in parallel (default 4)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("files", nargs="*", help="files to export")
    args = ap.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(root)
    if not os.path.isdir(os.path.join(root, "src")):
        print("error: --root %s does not look like the repo root" % root,
              file=sys.stderr)
        return 2
    files = collect_files(root, args.all, args.files)
    if not files:
        print("error: no files to export (pass files or --all)", file=sys.stderr)
        return 2
    index = export_symbols(root, files, args.clangd,
                           args.compile_commands_dir, not args.quiet,
                           args.jobs)
    if not index["files"]:
        print("error: clangd exported no files", file=sys.stderr)
        return 1
    text = json.dumps(index, indent=2, sort_keys=True) + "\n"
    if args.out:
        out_dir = os.path.dirname(os.path.abspath(args.out))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with io.open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if not args.quiet:
        print("== %d file(s), %d symbol(s), %d using-directive(s) ==" %
              (len(index["files"]), len(index["symbols"]),
               len(index["using_directives"])), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
