#!/usr/bin/env bash
# gen_sqlite_amalgamation.sh — regenerate the SQLite amalgamation
# (src/ext/sqlite_amalgamation/sqlite3.c + sqlite3.h) from the pinned
# sqlite submodule at src/ext/sqlite.
#
# Mirrors the generation pipeline of sqlite's own main.mk (lemon parser,
# opcode tables, pragma table, keyword hash, FTS5) so that the committed
# amalgamation can be compiled on machines WITHOUT tclsh/gcc (e.g. a bare
# Linux CI image). Run this whenever the src/ext/sqlite submodule is bumped.
#
# Requirements: bash, a C compiler (cc/gcc/cl), tclsh.
# Run from anywhere; paths are anchored at the repo root.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOP="$REPO_ROOT/src/ext/sqlite"
OUT="$REPO_ROOT/src/ext/sqlite_amalgamation"
B="$(mktemp -d)"
trap 'rm -rf "$B"' EXIT

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || CC=gcc
command -v tclsh >/dev/null 2>&1 || { echo "error: tclsh not found" >&2; exit 1; }
[ -f "$TOP/tool/mksqlite3c.tcl" ] || { echo "error: $TOP missing (submodule not cloned?)" >&2; exit 1; }

# MinGW gcc does not accept MSYS-style /d/... paths; convert for the compiler.
TOP_CC="$TOP"
if command -v cygpath >/dev/null 2>&1; then
    TOP_CC="$(cygpath -m "$TOP")"
    B_CC="$(cygpath -m "$B")"
    TOP_T="$TOP_CC"
else
    B_CC="$B"
    TOP_T="$TOP"
fi

echo "== building lemon / mkkeywordhash in $B"
"$CC" -O2 "$TOP_CC/tool/lemon.c" -o "$B_CC/lemon"
"$CC" -O2 "$TOP_CC/tool/mkkeywordhash.c" -o "$B_CC/mkkeywordhash"
"$CC" -O2 "$TOP_CC/tool/mksourceid.c" -o "$B_CC/mksourceid"
cp "$TOP/manifest" "$TOP/manifest.uuid" "$B/"

cd "$B"
# lemon needs its parser template lempar.c in the working directory.
cp "$TOP/tool/lempar.c" .

echo "== generating parse.c/parse.h"
cp "$TOP/src/parse.y" .
./lemon -S parse.y

echo "== generating fts5parse.c/fts5parse.h"
cp "$TOP/ext/fts5/fts5parse.y" .
./lemon -S fts5parse.y

echo "== generating opcodes"
cat parse.h "$TOP/src/vdbe.c" | tclsh "$TOP_T/tool/mkopcodeh.tcl" > opcodes.h
tclsh "$TOP_T/tool/mkopcodec.tcl" opcodes.h > opcodes.c

echo "== generating pragma.h / keywordhash.h / sqlite3.h"
tclsh "$TOP_T/tool/mkpragmatab.tcl"
./mkkeywordhash > keywordhash.h
tclsh "$TOP_T/tool/mksqlite3h.tcl" "$TOP_T" -o sqlite3.h
tclsh "$TOP_T/tool/mkctimec.tcl"

echo "== generating fts5.c"
tclsh "$TOP_T/ext/fts5/tool/mkfts5c.tcl"
cp "$TOP/ext/fts5/fts5.h" .

echo "== assembling tsrc"
mkdir tsrc
# Use main.mk's own SRC lists (SRC = plus SRC += blocks; it mixes $TOP-relative
# sources with build-dir generated files). Entries without $(TOP) resolve
# against the build dir.
sed -n '/^SRC =/,/^TESTSRC/p' "$TOP/main.mk" | grep -oE '\$\(TOP\)/[^ 	]+|keywordhash\.h|opcodes\.[ch]|parse\.[ch]|sqlite_cfg\.h|shell\.c|sqlite3\.h' > src_list.txt
for f in $(cat src_list.txt); do
    case "$f" in
        '$(TOP)'/*) src_file="$TOP/${f#'$(TOP)/'}" ;;
        *)          src_file="$f" ;;
    esac
    if [ -f "$src_file" ]; then
        cp "$src_file" tsrc/
    else
        echo "note: skipping missing SRC entry: $f"
    fi
done
# SRC's "Generated source code files" block (second SRC += in main.mk) lives in
# the build dir, not under $(TOP); copy the ones this script generated.
for f in keywordhash.h opcodes.c opcodes.h parse.c parse.h sqlite3.h ctime.c pragma.h fts5.c fts5.h; do
    [ -f "$f" ] && cp "$f" tsrc/ || echo "note: generated file missing: $f"
done
rm -f tsrc/sqlite.h.in tsrc/parse.y
tclsh "$TOP_T/tool/vdbe-compress.tcl" < tsrc/vdbe.c > vdbe.new
mv -f vdbe.new tsrc/vdbe.c

echo "== building amalgamation"
tclsh "$TOP_T/tool/mksqlite3c.tcl"

mkdir -p "$OUT"
cp sqlite3.c sqlite3.h "$OUT"/
echo "OK: wrote $OUT/sqlite3.c ($(< sqlite3.c wc -c) bytes) and $OUT/sqlite3.h ($(< sqlite3.h wc -c) bytes)"
