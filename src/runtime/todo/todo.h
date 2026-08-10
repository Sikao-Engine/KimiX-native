/*
 * todo.h — Todo-list merge, validation, status counts, and plain-text summary.
 *
 * Pure C++ kernel (no Python dependency). Operates on generic string-keyed
 * items; only the known keys title/status/notes/code are inspected or mutated.
 */

#pragma once

#include <core/dll_export.h>
#include <core/kimix_core.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kimix {
namespace runtime {
namespace todo {

// One todo item. Unknown fields from the original dict are intentionally not
// stored here — the Python binding only forwards the known keys.
struct TodoItem {
    string title;
    string status;   // canonical: pending, in_progress, done
    bool has_notes = false;
    string notes;
    bool has_code = false;
    string code;
};

struct MergeResult {
    vector<TodoItem> items;
    vector<string> warnings;
    std::optional<string> error;
    vector<string> regressed;
    vector<TodoItem> archived;
};

// Normalize a status string. Returns nullopt on failure.
KIMIX_RUNTIME_API std::optional<string> canonical_status(string_view status) noexcept;

// Merge/update old_items with new_items according to the requested mode.
// `fuzzy_warnings` maps a new-item title to a list of non-blocking warnings
// that are appended to the result in new-item order.
KIMIX_RUNTIME_API MergeResult merge(
    const vector<TodoItem>& old_items,
    const vector<TodoItem>& new_items,
    string_view mode,
    const unordered_map<string, vector<string>, string_hash>& fuzzy_warnings = {});

// Count canonical statuses.
KIMIX_RUNTIME_API unordered_map<string, size_t, string_hash> status_counts(const vector<TodoItem>& items);

// Plain-text summary matching kimi_cli/tools/todo/_format_todos with the
// default status filter (pending + in_progress) and an optional max_items cap.
KIMIX_RUNTIME_API string format_summary(const vector<TodoItem>& items, size_t max_items = 50);

} // namespace todo
} // namespace runtime
} // namespace kimix
