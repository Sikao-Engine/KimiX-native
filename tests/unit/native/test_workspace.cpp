// Test for runtime/workspace/workspace.h
// This test covers:
// - snapshot: empty dir, nested dirs, ignore_dirs filtering, symlink skip,
//   max_file_bytes boundary
// - diff_snapshots: identical snapshots, text
// additions/deletions/modifications,
//   binary changes, multiple files combined
// - changed_files: list of changed paths

#include "ut/ut.hpp"
#include <runtime/workspace/workspace.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace fs = std::filesystem;
namespace krw = kimix::runtime::workspace;

namespace {

fs::path make_temp_dir() {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("test_workspace_" + std::to_string(now));
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path &path, kimix::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

bool create_symlink_or_skip(const fs::path &target, const fs::path &link) {
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    return !ec;
}

bool contains(const krw::snapshot_t &snap, kimix::string_view key) {
    return snap.find(kimix::string(key)) != snap.end();
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "snapshot_empty_directory"_test = [] {
        const fs::path root = make_temp_dir();
        const auto snap = krw::snapshot(root.string(), {}, 1024);
        expect(snap.empty()) << "empty directory should produce empty snapshot";
        fs::remove_all(root);
    };

    "snapshot_nested_directories"_test = [] {
        const fs::path root = make_temp_dir();
        write_file(root / "root.txt", "root content");
        write_file(root / "a" / "b" / "nested.txt", "nested content");

        const auto snap = krw::snapshot(root.string(), {}, 1024);
        expect(eq(snap.size(), size_t{2})) << "expected two files";
        expect(contains(snap, "root.txt"));
        expect(contains(snap, "a/b/nested.txt"));

        fs::remove_all(root);
    };

    "snapshot_ignore_dirs"_test = [] {
        const fs::path root = make_temp_dir();
        write_file(root / "keep" / "kept.txt", "keep");
        write_file(root / "skip" / "skipped.txt", "skip");

        kimix::vector<kimix::string> ignore = {"skip"};
        const auto snap = krw::snapshot(root.string(), ignore, 1024);
        expect(eq(snap.size(), size_t{1}))
            << "ignored directory should be pruned";
        expect(contains(snap, "keep/kept.txt"));
        expect(!contains(snap, "skip/skipped.txt"));

        fs::remove_all(root);
    };

    "snapshot_skips_symlinks"_test =
        [] {
            const fs::path root = make_temp_dir();
            write_file(root / "target.txt", "target");
            const fs::path link = root / "link.txt";

            if (!create_symlink_or_skip(root / "target.txt", link)) {
                expect(true)
                    << "symlink creation unsupported on this host; skipped";
            } else {
                const auto snap = krw::snapshot(root.string(), {}, 1024);
                expect(eq(snap.size(), size_t{1}))
                    << "symlink should be skipped";
                expect(contains(snap, "target.txt"));
                expect(!contains(snap, "link.txt"));
            }

            fs::remove_all(root);
        };

    "snapshot_max_file_bytes_boundary"_test = [] {
        const fs::path root = make_temp_dir();
        write_file(root / "exact.txt", kimix::string(10, 'a'));
        write_file(root / "too_big.txt", kimix::string(11, 'b'));

        const auto snap = krw::snapshot(root.string(), {}, 10);
        expect(eq(snap.size(), size_t{1}))
            << "only exact-sized file should be kept";
        expect(contains(snap, "exact.txt"));
        expect(!contains(snap, "too_big.txt"));

        fs::remove_all(root);
    };

    "diff_snapshots_identical"_test = [] {
        krw::snapshot_t before;
        before["a.txt"] = "same content";
        krw::snapshot_t after;
        after["a.txt"] = "same content";

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        expect(diff.empty()) << "identical snapshots should produce no diff";

        const auto files = krw::changed_files(before, after);
        expect(files.empty())
            << "identical snapshots should produce no changed files";
    };

    "diff_snapshots_text_addition"_test = [] {
        krw::snapshot_t before;
        krw::snapshot_t after;
        after["x.txt"] = "hello\n";

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("--- a/x.txt") != kimix::string_view::npos);
        expect(view.find("+++ b/x.txt") != kimix::string_view::npos);
        expect(view.find("+hello") != kimix::string_view::npos);
    };

    "diff_snapshots_empty_file"_test = [] {
        krw::snapshot_t before;
        before["empty.txt"] = "";
        krw::snapshot_t after;
        after["empty.txt"] = "";
        after["other_empty.txt"] = "";

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("--- a/empty.txt") == kimix::string_view::npos)
            << "identical empty file should not appear";
        expect(view.find("other_empty.txt") == kimix::string_view::npos)
            << "empty addition produces no diff text";
    };

    "diff_snapshots_text_deletion"_test = [] {
        krw::snapshot_t before;
        before["y.txt"] = "goodbye\n";
        krw::snapshot_t after;

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("--- a/y.txt") != kimix::string_view::npos);
        expect(view.find("+++ b/y.txt") != kimix::string_view::npos);
        expect(view.find("-goodbye") != kimix::string_view::npos);
    };

    "diff_snapshots_text_modification"_test = [] {
        krw::snapshot_t before;
        before["z.txt"] = "line a\nline b\nline c\n";
        krw::snapshot_t after;
        after["z.txt"] = "line a\nline B\nline c\n";

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("-line b") != kimix::string_view::npos);
        expect(view.find("+line B") != kimix::string_view::npos);
    };

    "diff_snapshots_binary_change"_test = [] {
        krw::snapshot_t before;
        before["b.bin"] = kimix::string("\x00\x01", 2);
        krw::snapshot_t after;
        after["b.bin"] = kimix::string("\x00\x02", 2);

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("Binary file") != kimix::string_view::npos);
        expect(view.find("--- a/b.bin") != kimix::string_view::npos);
        expect(view.find("+++ b/b.bin") != kimix::string_view::npos);
    };

    "diff_snapshots_multiple_files"_test = [] {
        krw::snapshot_t before;
        before["a.txt"] = "alpha\n";
        before["b.txt"] = "beta\n";
        krw::snapshot_t after;
        after["a.txt"] = "alpha\nupdated\n";
        after["b.txt"] = "beta\n";

        const auto diff =
            krw::diff_snapshots(before, after, krw::text_extensions_t(), 3);
        const kimix::string_view view(diff);
        expect(view.find("a.txt") != kimix::string_view::npos);
        expect(view.find("b.txt") == kimix::string_view::npos)
            << "unchanged file should not appear in diff";
    };

    "changed_files_lists_differences"_test = [] {
        krw::snapshot_t before;
        before["a.txt"] = "a";
        before["b.txt"] = "b";
        krw::snapshot_t after;
        after["a.txt"] = "a";
        after["b.txt"] = "B";
        after["c.txt"] = "c";

        const auto files = krw::changed_files(before, after);
        expect(eq(files.size(), size_t{2}));
        expect(files[0].first == "b.txt");
        expect(files[1].first == "c.txt");
    };

    "changed_files_empty_file"_test = [] {
        krw::snapshot_t before;
        before["empty.txt"] = "";
        krw::snapshot_t after;
        after["new_empty.txt"] = "";

        const auto files = krw::changed_files(before, after);
        expect(eq(files.size(), size_t{2}));
    };
}
