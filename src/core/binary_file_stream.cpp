#include <core/binary_file_stream.h>
#include <core/stl/string.h>

#include <cstdio>
#include <utility>

namespace kimix {

// Additional non-inline helpers for BinaryFileStream.
// The core functionality is in the header; this file contains
// heavier operations that benefit from being out-of-line.

// Write operations for binary file streams (complement to read-only header)
class BinaryFileWriteStream {
public:
    BinaryFileWriteStream() noexcept : _file(nullptr) {}

    explicit BinaryFileWriteStream(const kimix::string &path) noexcept
        : _file(fopen(path.c_str(), "wb")) {
        if (!_file) {
            std::fprintf(stderr, "[kimix][warning] Failed to open file for writing: %s (%s:%d)\n",
                         path.c_str(), __FILE__, __LINE__);
        }
    }

    ~BinaryFileWriteStream() { close(); }

    BinaryFileWriteStream(const BinaryFileWriteStream &) = delete;
    BinaryFileWriteStream &operator=(const BinaryFileWriteStream &) = delete;

    BinaryFileWriteStream(BinaryFileWriteStream &&other) noexcept
        : _file(std::exchange(other._file, nullptr)) {}

    BinaryFileWriteStream &operator=(BinaryFileWriteStream &&other) noexcept {
        if (this != &other) {
            close();
            _file = std::exchange(other._file, nullptr);
        }
        return *this;
    }

    size_t write(const void *data, size_t size) noexcept {
        if (!_file) { return 0; }
        return fwrite(data, 1, size, _file);
    }

    size_t write(std::span<const kimix::byte> buffer) noexcept {
        if (!_file) { return 0; }
        return fwrite(buffer.data(), 1, buffer.size(), _file);
    }

    void close() noexcept {
        if (_file) {
            fclose(_file);
            _file = nullptr;
        }
    }

    bool is_open() const noexcept { return _file != nullptr; }
    explicit operator bool() const noexcept { return _file != nullptr; }

private:
    FILE *_file;
};

} // namespace kimix
