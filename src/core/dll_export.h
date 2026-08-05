#pragma once

// DLL export/import macros for the KimixBase libraries.
//
// Two independent macro families with identical semantics:
//
// 1. KIMIX_CORE_API — core library (kimix-core)
//    - Define KIMIX_CORE_STATIC when the core library is built/linked as a
//      static library (the default in this project): KIMIX_CORE_API expands
//      to nothing. This macro must be visible to both the library and its
//      clients, so the build system propagates it as a public define.
//    - Otherwise, define KIMIX_CORE_EXPORT_DLL when building the core library
//      as a shared library; client code (without either macro) sees dllimport.
//
// 2. KIMIX_RUNTIME_API — runtime library (runtime.dll / libruntime)
//    - Same semantics as above, controlled by KIMIX_RUNTIME_STATIC /
//      KIMIX_RUNTIME_EXPORT_DLL. The runtime library is built as a shared
//      library in this project: the build defines KIMIX_RUNTIME_EXPORT_DLL
//      (private), while consumers (without either macro) see dllimport.

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
