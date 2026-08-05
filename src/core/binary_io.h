#pragma once

#include "stl/string.h"
#include "stl/vector.h"
#include "basic_traits.h"

#include <span>
#include <cstddef>

namespace kimix {

// ---------------------------------------------------------------------------
// BinaryIO — abstract binary I/O interface
// Used for shader source/cache serialization.
// ---------------------------------------------------------------------------

class BinaryIO {
public:
    virtual ~BinaryIO() = default;

    // Read shader source code from a given name.
    // Returns true if the source was read successfully.
    virtual bool read_shader_source(string_view name, string& source) = 0;

    // Write shader source code for a given name.
    // Returns true if the source was written successfully.
    virtual bool write_shader_source(string_view name, string_view source) = 0;

    // Read cached shader binary data.
    // Returns true if the cache was read successfully.
    virtual bool read_shader_cache(string_view name, vector<byte>& data) = 0;

    // Write cached shader binary data.
    // Returns true if the cache was written successfully.
    virtual bool write_shader_cache(string_view name, std::span<const byte> data) = 0;
};

// ---------------------------------------------------------------------------
// DefaultBinaryIO — file-based binary I/O
// ---------------------------------------------------------------------------

class DefaultBinaryIO : public BinaryIO {
public:
    explicit DefaultBinaryIO(string cache_directory = ".cache")
        : _cache_dir(std::move(cache_directory)) {}

    bool read_shader_source(string_view name, string& source) override;
    bool write_shader_source(string_view name, string_view source) override;
    bool read_shader_cache(string_view name, vector<byte>& data) override;
    bool write_shader_cache(string_view name, std::span<const byte> data) override;

private:
    string _cache_dir;
};

} // namespace kimix
