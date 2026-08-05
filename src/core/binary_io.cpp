#include <core/binary_io.h>
#include <core/binary_file_stream.h>
#include <core/logging.h>
#include <core/stl/filesystem.h>
#include <core/stl/format.h>

#include <cstdio>
#include <cstring>

namespace kimix {

// ---------------------------------------------------------------------------
// DefaultBinaryIO implementation
// ---------------------------------------------------------------------------

bool DefaultBinaryIO::read_shader_source(string_view name, string &source) {
    kimix::filesystem::path file_path = kimix::filesystem::path(_cache_dir) / name;
    BinaryFileStream stream(file_path.string().c_str());
    if (!stream) {
        KIMIX_VERBOSE("Failed to open shader source file: {}", file_path.string());
        return false;
    }
    auto data = stream.read_all();
    source.assign(reinterpret_cast<char *>(data.data()), data.size());
    return true;
}

bool DefaultBinaryIO::write_shader_source(string_view name, string_view source) {
    kimix::filesystem::path dir_path(_cache_dir);
    std::error_code ec;
    kimix::filesystem::create_directories(dir_path, ec);
    if (ec) {
        KIMIX_WARNING_WITH_LOCATION(
            "Failed to create cache directory '{}': {}",
            _cache_dir, ec.message());
        return false;
    }

    kimix::filesystem::path file_path = dir_path / name;
    FILE *file = fopen(file_path.string().c_str(), "wb");
    if (!file) {
        KIMIX_WARNING_WITH_LOCATION(
            "Failed to open shader source file for writing: {}",
            file_path.string());
        return false;
    }

    size_t written = fwrite(source.data(), 1, source.size(), file);
    fclose(file);
    return written == source.size();
}

bool DefaultBinaryIO::read_shader_cache(string_view name, vector<byte> &data) {
    kimix::filesystem::path file_path = kimix::filesystem::path(_cache_dir) / name;
    BinaryFileStream stream(file_path.string().c_str());
    if (!stream) {
        KIMIX_VERBOSE("Failed to open shader cache file: {}", file_path.string());
        return false;
    }
    data = stream.read_all();
    return true;
}

bool DefaultBinaryIO::write_shader_cache(string_view name, std::span<const byte> data) {
    kimix::filesystem::path dir_path(_cache_dir);
    std::error_code ec;
    kimix::filesystem::create_directories(dir_path, ec);
    if (ec) {
        KIMIX_WARNING_WITH_LOCATION(
            "Failed to create cache directory '{}': {}",
            _cache_dir, ec.message());
        return false;
    }

    kimix::filesystem::path file_path = dir_path / name;
    FILE *file = fopen(file_path.string().c_str(), "wb");
    if (!file) {
        KIMIX_WARNING_WITH_LOCATION(
            "Failed to open shader cache file for writing: {}",
            file_path.string());
        return false;
    }

    size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

} // namespace kimix
