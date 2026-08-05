#include <core/binary_io.h>
#include <core/binary_file_stream.h>
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
        std::fprintf(stderr, "[kimix] Failed to open shader source file: %s\n",
                     file_path.string().c_str());
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
        std::fprintf(stderr, "[kimix][warning] Failed to create cache directory '%s': %s (%s:%d)\n",
                     _cache_dir.c_str(), ec.message().c_str(), __FILE__, __LINE__);
        return false;
    }

    kimix::filesystem::path file_path = dir_path / name;
    FILE *file = fopen(file_path.string().c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[kimix][warning] Failed to open shader source file for writing: %s (%s:%d)\n",
                     file_path.string().c_str(), __FILE__, __LINE__);
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
        std::fprintf(stderr, "[kimix] Failed to open shader cache file: %s\n",
                     file_path.string().c_str());
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
        std::fprintf(stderr, "[kimix][warning] Failed to create cache directory '%s': %s (%s:%d)\n",
                     _cache_dir.c_str(), ec.message().c_str(), __FILE__, __LINE__);
        return false;
    }

    kimix::filesystem::path file_path = dir_path / name;
    FILE *file = fopen(file_path.string().c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[kimix][warning] Failed to open shader cache file for writing: %s (%s:%d)\n",
                     file_path.string().c_str(), __FILE__, __LINE__);
        return false;
    }

    size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

} // namespace kimix
