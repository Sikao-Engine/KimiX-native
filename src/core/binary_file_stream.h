/*
 * binary_file_stream.h — kimix::BinaryFileStream for reading binary files.
 *
 * kimix::BinaryFileStream:
 *   Opens a file by path (const kimix::string&) or from an existing FILE*.
 *   Default-constructed streams are not open.
 *
 *   Methods:
 *     length()       — total file size in bytes
 *     position()     — current read position (0 at start)
 *     read(span)     — read into a std::span<byte>, returns bytes read
 *     read(ptr, sz)  — read into a raw buffer, returns bytes read
 *     read_all()     — read entire file into kimix::vector<byte>
 *     set_pos(pos)   — seek to a position
 *     close()        — close the file, resetting length to 0
 *     is_open()      — true if a file is open
 *
 *   Move semantics: moving transfers ownership; the moved-from stream
 *   becomes closed.
 *
 *   Opening a non-existent file leaves the stream in a closed state
 *   (is_open() == false).
 *
 * Example:
 *   kimix::BinaryFileStream bfs("data.bin");
 *   if (!bfs) { /* handle error *\/ }
 *   auto data = bfs.read_all();
 *   bfs.close();
 */
#pragma once

#include "stl/string.h"
#include "stl/vector.h"
#include "stl/memory.h"
#include "basic_traits.h"

#include <cstdio>
#include <cstddef>
#include <span>

namespace kimix {

// ---------------------------------------------------------------------------
// BinaryFileStream — reads binary files
// ---------------------------------------------------------------------------

class BinaryFileStream {
public:
    BinaryFileStream() noexcept : _file(nullptr), _length(0) {}

    explicit BinaryFileStream(FILE* file) noexcept : _file(file) {
        if (_file) {
            fseek(_file, 0, SEEK_END);
            _length = static_cast<size_t>(ftell(_file));
            fseek(_file, 0, SEEK_SET);
        }
    }

    explicit BinaryFileStream(const string& path) noexcept
        : BinaryFileStream(fopen(path.c_str(), "rb")) {}

    BinaryFileStream(const BinaryFileStream&) = delete;
    BinaryFileStream& operator=(const BinaryFileStream&) = delete;

    BinaryFileStream(BinaryFileStream&& other) noexcept
        : _file(std::exchange(other._file, nullptr))
        , _length(std::exchange(other._length, 0)) {}

    BinaryFileStream& operator=(BinaryFileStream&& other) noexcept {
        if (this != &other) {
            close();
            _file = std::exchange(other._file, nullptr);
            _length = std::exchange(other._length, 0);
        }
        return *this;
    }

    ~BinaryFileStream() { close(); }

    // Read data into a span
    size_t read(std::span<byte> buffer) noexcept {
        if (!_file) { return 0; }
        return fread(buffer.data(), 1, buffer.size(), _file);
    }

    // Read data into a pre-allocated buffer
    size_t read(void* buffer, size_t size) noexcept {
        if (!_file) { return 0; }
        return fread(buffer, 1, size, _file);
    }

    // Read entire file into a vector of bytes
    vector<byte> read_all() {
        if (!_file) { return {}; }
        vector<byte> data(_length);
        fseek(_file, 0, SEEK_SET);
        size_t read_bytes = fread(data.data(), 1, _length, _file);
        data.resize(read_bytes);
        return data;
    }

    void close() noexcept {
        if (_file) {
            fclose(_file);
            _file = nullptr;
            _length = 0;
        }
    }

    void set_pos(long pos) noexcept {
        if (_file) { fseek(_file, pos, SEEK_SET); }
    }

    long position() const noexcept {
        if (!_file) { return 0; }
        return ftell(_file);
    }

    size_t length() const noexcept { return _length; }

    bool is_open() const noexcept { return _file != nullptr; }
    explicit operator bool() const noexcept { return _file != nullptr; }

private:
    FILE* _file;
    size_t _length;
};

} // namespace kimix
