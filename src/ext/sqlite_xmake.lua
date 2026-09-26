-- ============================================================================
-- SQLite3 (https://github.com/sqlite/sqlite, submodule at src/ext/sqlite)
--
-- The upstream submodule only ships fragmentary parser/generator sources that
-- must go through lemon + tclsh (parse.y -> parse.c, opcode tables, pragma
-- table, keyword hash, FTS5) before they can be compiled at all. So the build
-- consumes the PRE-GENERATED amalgamation committed next to this file at
-- src/ext/sqlite_amalgamation/sqlite3.{c,h}. Regenerate it after bumping the
-- submodule (any machine with bash + a C compiler + tclsh, e.g. Git Bash on
-- Windows):
--
--     bash scripts/gen_sqlite_amalgamation.sh
--
-- That script mirrors sqlite's own main.mk generation pipeline. Keeping the
-- amalgamation checked in lets Windows AND bare Linux machines build without
-- tclsh/lemon. This target compiles the amalgamation as a plain static C
-- library; consumers link "kimix-sqlite3".
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
