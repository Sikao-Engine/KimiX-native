# SQLite amalgamation (vendored, third-party)

`sqlite3.c` and `sqlite3.h` are the **official SQLite amalgamation** released by
the SQLite project and downloaded from <https://sqlite.org/download.html>. They
are the single, authoritative SQLite input of this repository.

- Pinned version: **3.54.0** (also recorded in `VERSION.txt`).
- The files are public-domain third-party code. **Never hand-edit them** — the
  repository's `src/ext/` rule applies.
- The build does not consume SQLite source fragments and does not vendor the
  SQLite Fossil source tree: `src/ext/sqlite_xmake.lua` compiles `sqlite3.c`
  into the static library `kimix-sqlite3`, and every consumer (`kimix-llm`,
  `test_native_sqlite_history_index`, `test_builtin_retrieve_sqlite`,
  `test_cli_prune`, `test_sqlite`) links that target and includes the public
  `sqlite3.h`.
- The compile-time feature defines (`SQLITE_ENABLE_FTS5`,
  `SQLITE_ENABLE_MATH_FUNCTIONS`, `SQLITE_ENABLE_COLUMN_METADATA`,
  `SQLITE_THREADSAFE=1`, `SQLITE_OMIT_LOAD_EXTENSION`, plus the Windows
  `_CRT_SECURE_NO_WARNINGS` / `NOMINMAX`) live in `src/ext/sqlite_xmake.lua`.

> The pin is `VERSION.txt`, not `VERSION`: this directory is a public include dir
> of `kimix-sqlite3`, and a file named `VERSION` would shadow the C++20 standard
> header `<version>` on case-insensitive filesystems (Windows).

## Bumping SQLite

```bat
python scripts\fetch_sqlite_amalgamation.py --version X.Y.Z
xmake f -p windows -a x64 --toolchain=msvc -m release -c -y
xmake
```

The fetch script installs `sqlite3.{c,h}` from sqlite.org, validates the version
in both files plus FTS5 availability (the history-index suites need it), and
rewrites `VERSION.txt`. Then reconfigure, rebuild and run the SQLite suites
(`test_native_sqlite_history_index`, `test_builtin_retrieve_sqlite`,
`test_cli_prune`, `test_sqlite`). See
`python scripts\fetch_sqlite_amalgamation.py --help` for `--dry-run`, `--url`
(local ZIP or explicit URL) and `--sha3`.
