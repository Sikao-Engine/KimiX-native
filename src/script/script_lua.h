/*
 * script_lua.h -- INTERNAL: the one place the vendored Lua C API is included.
 *
 * The vendored lua.h / lauxlib.h / lualib.h carry no `extern "C"` block
 * (luaconf.h only gives LUAMOD_API the `extern "C"` spelling for the luaopen_*
 * functions), so a C++ consumer has to wrap the includes itself -- otherwise
 * the declarations are C++-mangled and never match the static archive's C names.
 *
 * Only translation units of src/script include this header; nothing else in
 * src/ sees a Lua header (see the source lint, scripts/check_lua_sandbox.py).
 */
#pragma once
#ifndef KIMIX_SCRIPT_LUA_H
#define KIMIX_SCRIPT_LUA_H

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#endif /* KIMIX_SCRIPT_LUA_H */
