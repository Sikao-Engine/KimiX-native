/*
 * workspace.h — Workspace snapshot, diff, and changed-files kernels for swarm
 * copy-mode selection.
 *
 * Kernels in this file are pure C++ (no Python dependency) and are compiled
 * into the runtime shared library. They mirror the copy-mode behavior of
 * src/kimix/tools/swarm/best_of_n.py.
 */

#pragma once

#include <core/kimix_core.h>

#include <optional>
#include <utility>

namespace kimix {
namespace runtime {
namespace workspace {

// Hash functor for kimix::string keys. We intentionally do not use
// kimix::hash64 here so that consumers of the workspace API do not pull in
// kimix-core objects that duplicate symbols exported by the runtime DLL.
struct string_hash {
    size_t operator()(const kimix::string &s) const noexcept {
        // FNV-1a 64-bit reduced to size_t.
        size_t h = 1469598103934665603ull;
        for (unsigned char c : s) {
            h ^= static_cast<size_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }
};

// Snapshot: relative POSIX path -> raw file contents.
using snapshot_t =
    kimix::unordered_map<kimix::string, kimix::string, string_hash>;

// Optional set of extensions (e.g. ".py") that are always treated as text.
using text_extensions_t =
    kimix::optional<kimix::unordered_set<kimix::string, string_hash>>;

// Walk `root` recursively and return a snapshot of regular files.
// Symlinks are skipped, directories whose name appears in `ignore_dirs` are
// pruned, and files larger than `max_file_bytes` are omitted.
KIMIX_RUNTIME_API snapshot_t snapshot(
    kimix::string_view root, const kimix::vector<kimix::string> &ignore_dirs,
    size_t max_file_bytes);

// Return a combined unified diff of all changed files in `before` and `after`.
// Text files are diffed with the diff engine; binary files emit a minimal
// binary marker hunk. Files are text when their extension is in
// `text_extensions` (if provided) or when their bytes contain no null byte.
KIMIX_RUNTIME_API kimix::string
diff_snapshots(const snapshot_t &before, const snapshot_t &after,
               const text_extensions_t &text_extensions, size_t context_lines);

// Return copy-mode operations as (source_relative_path, dest_relative_path)
// for every relative path whose content differs between the two snapshots.
KIMIX_RUNTIME_API kimix::vector<std::pair<kimix::string, kimix::string>>
changed_files(const snapshot_t &before, const snapshot_t &after);

} // namespace workspace
} // namespace runtime
} // namespace kimix
