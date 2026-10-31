#include <core/binary_io.h>
#include <core/binary_file_stream.h>
#include <core/platform.h>
#include <core/stl/filesystem.h>
#include <core/stl/format.h>

#include <cstdio>
#include <cstring>

namespace kimix {

// Build cache_dir/name as a path without ever throwing: the narrow path
// constructor converts via the ANSI code page and would terminate the
// process (no exceptions in this build) on unrepresentable bytes, so route
// through kimix::platform::path_from_narrow() (core/platform.h) and treat an
// undecodable name as an empty path, which the callers below already treat
// as "no such file".
//
// The platform helpers are INLINE ON PURPOSE: standalone kimix-core is a
// static archive whose objects are linked into several modules (kimix-llm,
// kimix-cli, runtime_py, kimix_api, the test binaries), and each module can
// carry its own mimalloc instance. An owning path/string built by a core-exported
// factory would hand the caller a block its module cannot free safely (the
// crash luisa-compute fixed in commit d80d45d). The inline helpers allocate
// and free inside whichever module calls them; only the allocation-free
// conversion primitives cross the module boundary.
static kimix::filesystem::path make_cache_path(const string &dir, string_view name) {
    kimix::filesystem::path base = kimix::platform::path_from_narrow(dir);
    if (base.empty() && !dir.empty()) { return {}; }
    kimix::filesystem::path leaf = kimix::platform::path_from_narrow(name);
    if (leaf.empty() && !name.empty()) { return {}; }
    return base / leaf;
}

// ---------------------------------------------------------------------------
// DefaultBinaryIO implementation
// ---------------------------------------------------------------------------

bool DefaultBinaryIO::read_shader_source(string_view name, string &source) {
    auto file_path = make_cache_path(_cache_dir, name);
    if (file_path.empty()) { return false; }
    const auto path_str = kimix::platform::path_to_narrow(file_path);
    BinaryFileStream stream(path_str.c_str());
    if (!stream) {
        std::fprintf(stderr, "[kimix] Failed to open shader source file: %s\n",
                     path_str.c_str());
        return false;
    }
    auto data = stream.read_all();
    source.assign(reinterpret_cast<char *>(data.data()), data.size());
    return true;
}

bool DefaultBinaryIO::write_shader_source(string_view name, string_view source) {
    auto dir_path = kimix::platform::path_from_narrow(_cache_dir);
    if (dir_path.empty() && !_cache_dir.empty()) { return false; }
    std::error_code ec;
    kimix::filesystem::create_directories(dir_path, ec);
    if (ec) {
        std::fprintf(stderr, "[kimix][warning] Failed to create cache directory '%s': %s (%s:%d)\n",
                     _cache_dir.c_str(), ec.message().c_str(), __FILE__, __LINE__);
        return false;
    }

    auto file_path = make_cache_path(_cache_dir, name);
    if (file_path.empty()) { return false; }
    const auto path_str = kimix::platform::path_to_narrow(file_path);
    FILE *file = fopen(path_str.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[kimix][warning] Failed to open shader source file for writing: %s (%s:%d)\n",
                     path_str.c_str(), __FILE__, __LINE__);
        return false;
    }

    size_t written = fwrite(source.data(), 1, source.size(), file);
    fclose(file);
    return written == source.size();
}

bool DefaultBinaryIO::read_shader_cache(string_view name, vector<byte> &data) {
    auto file_path = make_cache_path(_cache_dir, name);
    if (file_path.empty()) { return false; }
    const auto path_str = kimix::platform::path_to_narrow(file_path);
    BinaryFileStream stream(path_str.c_str());
    if (!stream) {
        std::fprintf(stderr, "[kimix] Failed to open shader cache file: %s\n",
                     path_str.c_str());
        return false;
    }
    data = stream.read_all();
    return true;
}

bool DefaultBinaryIO::write_shader_cache(string_view name, std::span<const byte> data) {
    auto dir_path = kimix::platform::path_from_narrow(_cache_dir);
    if (dir_path.empty() && !_cache_dir.empty()) { return false; }
    std::error_code ec;
    kimix::filesystem::create_directories(dir_path, ec);
    if (ec) {
        std::fprintf(stderr, "[kimix][warning] Failed to create cache directory '%s': %s (%s:%d)\n",
                     _cache_dir.c_str(), ec.message().c_str(), __FILE__, __LINE__);
        return false;
    }

    auto file_path = make_cache_path(_cache_dir, name);
    if (file_path.empty()) { return false; }
    const auto path_str = kimix::platform::path_to_narrow(file_path);
    FILE *file = fopen(path_str.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[kimix][warning] Failed to open shader cache file for writing: %s (%s:%d)\n",
                     path_str.c_str(), __FILE__, __LINE__);
        return false;
    }

    size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

} // namespace kimix
