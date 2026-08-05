#pragma once

#include "dll_export.h"
#include "platform.h"
#include "stl/string.h"
#include "stl/vector.h"
#include "stl/functional.h"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace kimix {

// ---------------------------------------------------------------------------
// DynamicModule — loads DLL/SO at runtime
// ---------------------------------------------------------------------------

class DynamicModule {
public:
    DynamicModule() = default;

    ~DynamicModule() {
        unload();
    }

    DynamicModule(const DynamicModule&) = delete;
    DynamicModule& operator=(const DynamicModule&) = delete;

    DynamicModule(DynamicModule&& other) noexcept
        : _handle(std::exchange(other._handle, nullptr)) {}

    DynamicModule& operator=(DynamicModule&& other) noexcept {
        if (this != &other) {
            unload();
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }

    // Load a module by name (uses platform search paths)
    bool load(string_view name) noexcept {
        unload();
        _handle = dynamic_module_load(name.data());
        return _handle != nullptr;
    }

    // Load a module from a specific folder + name
    bool load(string_view folder, string_view name) noexcept {
        unload();
        string full_path(folder);
        full_path += env_separator();
        full_path += name;
        _handle = dynamic_module_load(full_path.c_str());
        return _handle != nullptr;
    }

    void unload() noexcept {
        if (_handle) {
            dynamic_module_destroy(_handle);
            _handle = nullptr;
        }
    }

    // Get a function pointer from the module
    template <typename Signature>
    function<Signature> function(string_view name) const {
        if (!_handle) { return nullptr; }
        void* sym = dynamic_module_find_symbol(_handle, name.data());
        if (!sym) { return nullptr; }
        return reinterpret_cast<Signature*>(sym);
    }

    bool is_loaded() const noexcept { return _handle != nullptr; }
    explicit operator bool() const noexcept { return _handle != nullptr; }

    // ---------------------------------------------------------------------------
    // Module search paths (static)
    // ---------------------------------------------------------------------------

    static void add_search_path(string_view path) {
        search_paths().emplace_back(path);
    }

    static void remove_search_path(string_view path) {
        auto& paths = search_paths();
        paths.erase(
            std::remove_if(paths.begin(), paths.end(),
                [&](const string& p) { return p == path; }),
            paths.end()
        );
    }

    static void reset_search_paths() {
        search_paths().clear();
    }

    static const vector<string>& get_search_paths() {
        return search_paths();
    }

private:
    static vector<string>& search_paths() {
        static vector<string> paths;
        return paths;
    }

    void* _handle = nullptr;
};

} // namespace kimix
