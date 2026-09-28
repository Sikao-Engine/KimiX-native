#pragma once

// DLL export/import macros for the KimixBase libraries.
//
// Four independent macro families with identical semantics:
//
// 1. KIMIX_CORE_API — core library (kimix-core)
//    - Define KIMIX_CORE_STATIC when the core library is built/linked as a
//      static library (the default in this project): KIMIX_CORE_API expands
//      to nothing. This macro must be visible to both the library and its
//      clients, so the build system propagates it as a public define.
//    - Otherwise, define KIMIX_CORE_EXPORT_DLL when building the core library
//      as a shared library; client code (without either macro) sees dllimport.
//
#if defined(KIMIX_CORE_STATIC)
    #define KIMIX_CORE_API
#elif defined(KIMIX_CORE_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_CORE_API __declspec(dllexport)
    #else
        #define KIMIX_CORE_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define KIMIX_CORE_API __declspec(dllimport)
    #else
        #define KIMIX_CORE_API
    #endif
#endif

// 2. KIMIX_LLM_API — LLM provider + built-in tool kernels (kimix-llm static lib).
//    Same three-state pattern; the target currently always builds as a static
//    library and defines KIMIX_LLM_STATIC publicly, so the macro expands to
//    nothing for both the library and its consumers.
#if defined(KIMIX_LLM_STATIC)
    #define KIMIX_LLM_API
#elif defined(KIMIX_LLM_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_LLM_API __declspec(dllexport)
    #else
        #define KIMIX_LLM_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define KIMIX_LLM_API __declspec(dllimport)
    #else
        #define KIMIX_LLM_API
    #endif
#endif

// 3. KIMIX_RUNTIME_API — runtime library (runtime_py.pyd / runtime_py.so)
//    - Same semantics as above, controlled by KIMIX_RUNTIME_STATIC /
//      KIMIX_RUNTIME_EXPORT_DLL. The runtime kernels are now built into the
//      Python extension module; the build defines KIMIX_RUNTIME_EXPORT_DLL
//      (private), while consumers (without either macro) see dllimport.

#if defined(KIMIX_RUNTIME_STATIC)
    #define KIMIX_RUNTIME_API
#elif defined(KIMIX_RUNTIME_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_RUNTIME_API __declspec(dllexport)
    #else
        #define KIMIX_RUNTIME_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define KIMIX_RUNTIME_API __declspec(dllimport)
    #else
        #define KIMIX_RUNTIME_API
    #endif
#endif

// 4. KIMIX_API_API — the C FFI shared library (kimix_api, src/api).
//    Same three-state pattern, but this family is the basement of a *C* ABI:
//    the header that uses it (src/api/ffi_common.h) has to stay valid for a
//    C99/C11 compiler as well as for C++, so only these macros — never a C++
//    attribute — may appear in front of an exported declaration.
//    - The library build defines KIMIX_API_EXPORT_DLL (see
//      src/api/xmake.lua): dllexport / default visibility.
//    - A consumer that links the import library / the .so leaves both macros
//      undefined: dllimport on Windows, nothing elsewhere.
//    - A consumer that resolves the symbols itself at run time
//      (dlopen/LoadLibrary, or a static merge of the objects) defines
//      KIMIX_API_STATIC: plain extern declarations, no linkage decoration.
#if defined(KIMIX_API_STATIC)
    #define KIMIX_API_API
#elif defined(KIMIX_API_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_API_API __declspec(dllexport)
    #else
        #define KIMIX_API_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define KIMIX_API_API __declspec(dllimport)
    #else
        #define KIMIX_API_API
    #endif
#endif