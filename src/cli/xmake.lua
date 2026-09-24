-- Kimix CLI (src/cli)
--
-- The native command-line front end for the KimiX agent.  It is the C++ port of
-- kimi-agent's Python CLI layer:
--   * src/kimix/cli_impl/   (args / REPL core / slash commands / init)
--   * src/kimix/ui/         (terminal printing + wire-message streaming)
--   * src/kimix/utils/      (config deserialization + session store)
-- See src/cli/PLAN.md for the module map, the frozen interfaces and every
-- documented deviation from the Python reference.
--
-- Dependency graph:  kimix_cli (binary) -> kimix-cli (static) -> kimix-llm
-- `kimix-llm` is the existing static library that already contains the agent
-- (src/agent), the unified LLM facade (src/llm) and the built-in tools
-- (src/builtin_tools), so depending on it is exactly the requested
-- "agent + llm + builtin_tools" dependency set.

-- The CLI implementation library.  main.cpp is excluded: a static library must
-- not carry a second main().
target("kimix-cli")
    set_kind("static")
    add_files("*.cpp")
    remove_files("main.cpp")
    add_headerfiles("*.h")
    -- Public include dir: `src/` so sources are included as "cli/xxx.h" and the
    -- inherited "llm/..." / "builtin_tools/..." / "agent/..." includes keep
    -- working from the library and from the test target.
    add_includedirs("..", { public = true })
    add_deps("kimix-llm") -- == agent + llm + builtin_tools (see the note above)
    -- kimix-core is consumed as the static copy (same as the other executables
    -- in this project); kimix-llm only re-exports core symbols through the pyd.
    add_defines("KIMIX_CORE_STATIC")
    _config_project({ batch_size = 8, project_kind = "static" })
target_end()

-- The CLI executable.  Run with:
--   bin/debug/kimix_cli.exe --provider C:/dev/ds_flash.json \
--       --agent-file <agent_worker.json> [--dry-run] [-p "prompt"]
target("kimix_cli")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kimix-cli")
    add_includedirs("..", { public = true })
    add_defines("KIMIX_CORE_STATIC")
    _config_project({ batch_size = 8 })
    -- `xmake run kimix_cli -- --help` should execute the binary directly.
    add_rules("kimix_run_target")
target_end()
