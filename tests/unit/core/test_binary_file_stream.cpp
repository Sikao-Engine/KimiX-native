// Test for binary_file_stream.h (kimix::BinaryFileStream).
// This test covers:
// - Construct from path
// - length() and position()
// - read() reads correct data
// - set_pos() seeks correctly
// - close()

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

// Helper: create a temporary file with known content
static kimix::string create_temp_file() {
    kimix::string path = "test_binary_stream_temp.bin";
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        const char data[] = "Hello World! Binary file stream test.";
        fwrite(data, 1, sizeof(data), f);
        fclose(f);
    }
    return path;
}

static void remove_temp_file(const kimix::string& path) {
    std::remove(path.c_str());
}

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "bfs_construct_from_path"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);
        expect(bfs.is_open()) << "should be open after construction from path";
        bfs.close();
        remove_temp_file(path);
    };

    "bfs_length"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);
        expect(bfs.length() > 0_u) << "file should have non-zero length";
        expect(eq(bfs.length(), sizeof("Hello World! Binary file stream test.")));
        bfs.close();
        remove_temp_file(path);
    };

    "bfs_position"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);
        expect(eq(bfs.position(), 0_l)) << "initial position should be 0";
        bfs.close();
        remove_temp_file(path);
    };

    "bfs_read_correct_data"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);

        char buffer[64] = {};
        size_t read = bfs.read(buffer, sizeof(buffer));
        expect(read > 0_u) << "should read some bytes";
        expect(std::strcmp(buffer, "Hello World! Binary file stream test.") == 0)
            << "read data should match written data";
        bfs.close();
        remove_temp_file(path);
    };

    "bfs_read_partial"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);

        char buffer[6] = {};
        size_t read = bfs.read(buffer, 5);
        expect(eq(read, 5_u));
        expect(std::strcmp(buffer, "Hello") == 0);

        bfs.close();
        remove_temp_file(path);
    };

    "bfs_set_pos_seek"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);

        bfs.set_pos(6);
        expect(eq(bfs.position(), 6_l)) << "position should be 6 after set_pos(6)";

        char buffer[7] = {};
        bfs.read(buffer, 6);
        expect(std::strcmp(buffer, "World!") == 0) << "should read from position 6";

        bfs.close();
        remove_temp_file(path);
    };

    "bfs_read_all"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);

        auto data = bfs.read_all();
        expect(eq(data.size(), sizeof("Hello World! Binary file stream test.")));
        expect(std::memcmp(data.data(), "Hello World! Binary file stream test.", data.size()) == 0);

        bfs.close();
        remove_temp_file(path);
    };

    "bfs_close"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs(path);
        expect(bfs.is_open());

        bfs.close();
        expect(!bfs.is_open()) << "should not be open after close";
        expect(eq(bfs.length(), 0_u)) << "length should be 0 after close";
        remove_temp_file(path);
    };

    "bfs_default_construction"_test = [] {
        kimix::BinaryFileStream bfs;
        expect(!bfs.is_open());
        expect(eq(bfs.length(), 0_u));
    };

    "bfs_move_constructor"_test = [] {
        auto path = create_temp_file();
        kimix::BinaryFileStream bfs1(path);
        size_t len = bfs1.length();

        kimix::BinaryFileStream bfs2(std::move(bfs1));
        expect(bfs2.is_open());
        expect(eq(bfs2.length(), len));
        expect(!bfs1.is_open()) << "moved-from stream should not be open";

        bfs2.close();
        remove_temp_file(path);
    };

    "bfs_non_existent_file"_test = [] {
        kimix::BinaryFileStream bfs("__nonexistent_file_test__.bin");
        expect(!bfs.is_open()) << "non-existent file should not open";
    };

}
