-- ============================================================================
-- SQLite3 (https://sqlite.org) — the vendored pre-generated amalgamation
--
-- The single SQLite input of this repository is the official amalgamation
-- committed next to this file at src/ext/sqlite_amalgamation/sqlite3.{c,h}
-- (with its VERSION.txt pin and README.md). SQLite is NOT vendored as a source
-- submodule: the build never runs lemon/tclsh/mksqlite3c, so Windows AND bare
-- Linux machines compile it as-is and bumping SQLite is just "install a newer
-- amalgamation":
--
--     python scripts/fetch_sqlite_amalgamation.py --version X.Y.Z
--
-- That script downloads the release from sqlite.org, validates it (header/
-- source version sync + FTS5 availability) and rewrites both files and VERSION.txt.
-- The feature defines stay in this file, below. This target compiles the
-- amalgamation as a plain static C library; consumers link "kimix-sqlite3".
-- ============================================================================
target("kimix-sqlite3")
    set_kind("static")
    add_rules("kimix_basic_settings") -- project-wide flags, but NO unity build
    on_load(function(target)
        local dir = path.join(os.scriptdir(), "sqlite_amalgamation")
        target:add("files", path.join(dir, "sqlite3.c"))
        target:add("includedirs", dir, {public = true})
        target:add("defines",
            "SQLITE_ENABLE_FTS5",          -- full-text search (history index)
            "SQLITE_ENABLE_MATH_FUNCTIONS",
            "SQLITE_ENABLE_COLUMN_METADATA",
            "SQLITE_THREADSAFE=1",
            "SQLITE_OMIT_LOAD_EXTENSION")  -- no dlopen/LoadLibrary dependency
        if target:is_plat("windows") then
            target:add("defines", "_CRT_SECURE_NO_WARNINGS", "NOMINMAX")
            target:add("syslinks", "advapi32", {public = true})
        else
            target:add("syslinks", "pthread", "dl", {public = true})
        end
    end)
target_end()
