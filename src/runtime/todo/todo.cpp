/*
 * todo.cpp — Todo-list merge, validation, status counts, and plain-text summary.
 */

#include <runtime/todo/todo.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <iterator>
#include <set>

namespace kimix {
namespace runtime {
namespace todo {

namespace {

void trim_inplace(string& s) noexcept {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    if (start == s.size()) {
        s.clear();
        return;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    s.assign(s.begin() + static_cast<string::difference_type>(start),
             s.begin() + static_cast<string::difference_type>(end));
}

string lower_dash_to_underscore(string_view sv) noexcept {
    string out(sv.begin(), sv.end());
    trim_inplace(out);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::replace(out.begin(), out.end(), '-', '_');
    return out;
}

bool is_unfinished(const TodoItem& item) noexcept {
    return item.status != "done";
}

string format_title_list(const vector<string>& titles) {
    string out = "[";
    for (size_t i = 0; i < titles.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += '"';
        out += titles[i];
        out += '"';
    }
    out += "]";
    return out;
}

template <typename... Args>
string kformat(std::format_string<Args...> fmt, Args&&... args) {
    auto text = std::format(fmt, std::forward<Args>(args)...);
    return string(text.begin(), text.end());
}

// Validate and canonicalize a list of items in place. Returns an error message
// on failure, or std::nullopt on success.
std::optional<string> validate_items(vector<TodoItem>& items) {
    for (auto& item : items) {
        trim_inplace(item.title);
        if (item.title.empty()) {
            return kimix::string("Title cannot be empty or contain only whitespace.");
        }
        auto canon = canonical_status(item.status);
        if (!canon) {
            return kformat("Invalid status '{}'. Must be one of: pending, in_progress, done.", item.status);
        }
        item.status = std::move(*canon);
    }
    return std::nullopt;
}

vector<TodoItem> merge_by_title_update(
    const vector<TodoItem>& old_items,
    const vector<TodoItem>& new_items) {
    vector<TodoItem> merged;
    merged.reserve(old_items.size() + new_items.size());

    unordered_map<string, const TodoItem*, string_hash> new_by_title;
    for (const auto& item : new_items) {
        new_by_title[item.title] = &item;
    }

    set<string> seen;
    for (const auto& old : old_items) {
        auto it = new_by_title.find(old.title);
        if (it != new_by_title.end()) {
            const TodoItem& new_item = *it->second;
            TodoItem updated;
            updated.title = old.title;
            updated.status = new_item.status;
            if (new_item.has_notes) {
                updated.has_notes = true;
                updated.notes = new_item.notes;
            } else {
                updated.has_notes = old.has_notes;
                updated.notes = old.notes;
            }
            if (new_item.has_code) {
                updated.has_code = true;
                updated.code = new_item.code;
            } else {
                updated.has_code = old.has_code;
                updated.code = old.code;
            }
            merged.push_back(std::move(updated));
        } else {
            merged.push_back(old);
        }
        seen.insert(old.title);
    }

    for (const auto& new_item : new_items) {
        if (seen.find(new_item.title) == seen.end()) {
            merged.push_back(new_item);
            seen.insert(new_item.title);
        }
    }

    return merged;
}

} // namespace

std::optional<string> canonical_status(string_view status) noexcept {
    string normalized = lower_dash_to_underscore(status);
    if (normalized == "pending" || normalized == "in_progress" || normalized == "done") {
        return normalized;
    }
    return std::nullopt;
}

MergeResult merge(
    const vector<TodoItem>& old_items,
    const vector<TodoItem>& new_items,
    string_view mode,
    const unordered_map<string, vector<string>, string_hash>& fuzzy_warnings) {
    MergeResult result;

    // ------------------------------------------------------------------
    // 1. Validate mode.
    // ------------------------------------------------------------------
    const string mode_norm = lower_dash_to_underscore(mode);
    const bool is_append = (mode_norm == "append");
    const bool is_overwrite = (mode_norm == "overwrite");
    const bool is_force = (mode_norm == "force_overwrite");
    if (!is_append && !is_overwrite && !is_force) {
        result.error = kformat("Invalid mode '{}'. Must be one of: append, overwrite, force_overwrite.", mode);
        return result;
    }

    // ------------------------------------------------------------------
    // 2. Validate and canonicalize input items.
    // ------------------------------------------------------------------
    vector<TodoItem> old_validated = old_items;
    vector<TodoItem> new_validated = new_items;
    if (auto err = validate_items(old_validated)) {
        result.error = std::move(*err);
        return result;
    }
    if (auto err = validate_items(new_validated)) {
        result.error = std::move(*err);
        return result;
    }

    // ------------------------------------------------------------------
    // 3. Duplicate titles in the incoming list.
    // ------------------------------------------------------------------
    {
        unordered_map<string, size_t, string_hash> seen;
        vector<string> duplicates;
        for (const auto& item : new_validated) {
            auto [it, inserted] = seen.try_emplace(item.title, 1);
            if (!inserted) {
                if (++it->second == 2) {
                    duplicates.push_back(item.title);
                }
            }
        }
        if (!duplicates.empty()) {
            std::sort(duplicates.begin(), duplicates.end());
            result.error = kformat("Duplicate todo titles found: {}", format_title_list(duplicates));
            return result;
        }
    }

    // ------------------------------------------------------------------
    // 4. Branch on write mode.
    // ------------------------------------------------------------------
    if (is_force) {
        result.items = new_validated;
    } else if (is_overwrite) {
        if (std::any_of(old_validated.begin(), old_validated.end(), is_unfinished)) {
            string unfinished;
            for (const auto& item : old_validated) {
                if (is_unfinished(item)) {
                    if (!unfinished.empty()) {
                        unfinished += '\n';
                    }
                    unfinished += item.title;
                }
            }
            string err;
            kimix::format_to(
                std::back_inserter(err),
                "Error: Cannot overwrite todos while old todos are not all done. "
                "Use mode='force_overwrite' if you really want to discard unfinished work.\n"
                "Unfinished:\n{}",
                unfinished);
            result.error = std::move(err);
            return result;
        }
        result.items = new_validated;
    } else { // append
        if (new_validated.empty()) {
            if (std::any_of(old_validated.begin(), old_validated.end(), is_unfinished)) {
                string unfinished;
                for (const auto& item : old_validated) {
                    if (is_unfinished(item)) {
                        if (!unfinished.empty()) {
                            unfinished += ", ";
                        }
                        unfinished += item.title;
                    }
                }
                string err;
                kimix::format_to(
                    std::back_inserter(err),
                    "Error: Cannot clear todos while old todos are not all done. "
                    "Unfinished: {}\n"
                    "Next step: mark them done first, "
                    "or use mode='force_overwrite' to discard them intentionally.",
                    unfinished);
                result.error = std::move(err);
                return result;
            }
            result.items.clear();
        } else {
            result.items = merge_by_title_update(old_validated, new_validated);
        }
    }

    // ------------------------------------------------------------------
    // 5. Append fuzzy warnings in new-item order.
    // ------------------------------------------------------------------
    for (const auto& item : new_validated) {
        auto it = fuzzy_warnings.find(item.title);
        if (it != fuzzy_warnings.end()) {
            result.warnings.insert(result.warnings.end(), it->second.begin(), it->second.end());
        }
    }

    // ------------------------------------------------------------------
    // 6. Regression detection (clamped back to done).
    // ------------------------------------------------------------------
    if (!is_force && !old_validated.empty()) {
        unordered_map<string, string, string_hash> old_status;
        for (const auto& item : old_validated) {
            // Keep the first occurrence if duplicates somehow exist.
            old_status.try_emplace(item.title, item.status);
        }

        for (auto& item : result.items) {
            auto it = old_status.find(item.title);
            if (it != old_status.end() && it->second == "done" && item.status != "done") {
                item.status = "done";
                result.regressed.push_back(item.title);
            }
        }
    }

    // ------------------------------------------------------------------
    // 7. Archive completed todos that are no longer kept.
    // ------------------------------------------------------------------
    {
        set<string> kept_titles;
        for (const auto& item : result.items) {
            kept_titles.insert(item.title);
        }
        for (const auto& item : old_validated) {
            if (item.status == "done" && kept_titles.find(item.title) == kept_titles.end()) {
                result.archived.push_back(item);
            }
        }
    }

    // ------------------------------------------------------------------
    // 8. Enforce a single in-progress item (auto-fix).
    // ------------------------------------------------------------------
    if (!is_force) {
        bool seen_in_progress = false;
        for (auto& item : result.items) {
            if (item.status == "in_progress") {
                if (seen_in_progress) {
                    item.status = "done";
                    result.warnings.push_back(
                        kformat("Auto-fixed \"{}\": set to done (only one item may be in_progress)",
                               item.title));
                } else {
                    seen_in_progress = true;
                }
            }
        }
    }

    return result;
}

unordered_map<string, size_t, string_hash> status_counts(const vector<TodoItem>& items) {
    unordered_map<string, size_t, string_hash> counts;
    counts["pending"] = 0;
    counts["in_progress"] = 0;
    counts["done"] = 0;
    for (const auto& item : items) {
        if (item.status == "pending" || item.status == "in_progress" || item.status == "done") {
            ++counts[item.status];
        }
    }
    return counts;
}

string format_summary(const vector<TodoItem>& items, size_t max_items) {
    vector<const TodoItem*> selected;
    selected.reserve(items.size());
    for (const auto& item : items) {
        if (item.status == "pending" || item.status == "in_progress") {
            selected.push_back(&item);
        }
    }

    if (selected.empty()) {
        return {};
    }
    if (selected.size() > max_items) {
        selected.resize(max_items);
    }

    string scratch;
    bool first = true;
    for (const auto* item : selected) {
        if (!first) {
            scratch += '\n';
        }
        first = false;

        string_view display_status = item->status;
        if (item->status == "in_progress") {
            display_status = "in progress";
        }

        kimix::format_to(std::back_inserter(scratch), "- [{}] {}", display_status, item->title);

        if (item->has_code && !item->code.empty()) {
            string stripped = item->code;
            trim_inplace(stripped);
            string kind_label;
            if (!stripped.empty() && stripped[0] == '!') {
#ifdef KIMIX_PLATFORM_WINDOWS
                kind_label = "pwsh";
#else
                kind_label = "bash";
#endif
            } else {
                string lower = stripped;
                for (char& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                bool is_file = false;
                try {
                    is_file = std::filesystem::is_regular_file(stripped);
                } catch (...) {
                    is_file = false;
                }
                if ((lower.ends_with(".py") || lower.ends_with(".sh") || lower.ends_with(".ps1")) && is_file) {
                    kind_label = "file";
                } else {
                    kind_label = "inline";
                }
            }
            kimix::format_to(std::back_inserter(scratch), "  `[code: {}]`", kind_label);
        }

        if (item->status == "in_progress" && item->has_notes && !item->notes.empty()) {
            kimix::format_to(std::back_inserter(scratch), "  Notes: {}", item->notes);
        }
    }

    return scratch;
}

} // namespace todo
} // namespace runtime
} // namespace kimix
