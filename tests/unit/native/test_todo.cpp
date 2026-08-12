// Test for runtime/todo/todo.h.
// This test covers:
// - append with new, updated, and identical items
// - overwrite blocked by unfinished items
// - force_overwrite clears the list
// - empty old/new lists
// - duplicate new titles
// - invalid status strings
// - regression detection (done -> pending/in_progress clamping)
// - fuzzy-warning forwarding
// - auto-fix of multiple in_progress items
// - status_counts and format_summary

#include "ut/ut.hpp"

#include <runtime/todo/todo.h>

#include <algorithm>
#include <cstddef>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;

namespace todo = kimix::runtime::todo;

namespace {

todo::TodoItem make_item(const char* title, const char* status, const char* notes = nullptr,
                         const char* code = nullptr) {
    todo::TodoItem item;
    item.title = title;
    item.status = status;
    if (notes != nullptr) {
        item.has_notes = true;
        item.notes = notes;
    }
    if (code != nullptr) {
        item.has_code = true;
        item.code = code;
    }
    return item;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "append_updates_existing_and_appends_new"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "pending"),
            make_item("B", "done"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("A", "in_progress"),
            make_item("C", "pending"),
        };

        auto result = todo::merge(old_items, new_items, "append");

        expect(!result.error.has_value()) << "append should succeed";
        expect(eq(result.items.size(), 3u));
        expect(eq(result.items[0].title, string("A")));
        expect(eq(result.items[0].status, string("in_progress")));
        expect(eq(result.items[1].title, string("B")));
        expect(eq(result.items[1].status, string("done")));
        expect(eq(result.items[2].title, string("C")));
        expect(eq(result.items[2].status, string("pending")));
        expect(result.warnings.empty()) << "no warnings expected";
        expect(result.regressed.empty()) << "no regressions expected";
        expect(result.archived.empty()) << "no archived items expected";
    };

    "append_preserves_old_notes_and_code_when_new_omits_them"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "pending", "old notes", "old code"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("A", "in_progress"),
        };

        auto result = todo::merge(old_items, new_items, "append");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 1u));
        expect(eq(result.items[0].title, string("A")));
        expect(eq(result.items[0].status, string("in_progress")));
        expect(eq(result.items[0].notes, string("old notes")));
        expect(eq(result.items[0].code, string("old code")));
    };

    "append_identical_items_leaves_list_unchanged"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "pending"),
            make_item("B", "done"),
        };
        auto result = todo::merge(old_items, old_items, "append");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 2u));
        expect(eq(result.items[0].status, string("pending")));
        expect(eq(result.items[1].status, string("done")));
    };

    "append_empty_new_clears_done_list"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
            make_item("B", "done"),
        };
        vector<todo::TodoItem> new_items;
        auto result = todo::merge(old_items, new_items, "append");

        expect(!result.error.has_value());
        expect(result.items.empty());
        expect(eq(result.archived.size(), 2u)) << "all done old items should be archived on clear";
    };

    "append_empty_new_blocked_by_unfinished"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
            make_item("B", "pending"),
        };
        vector<todo::TodoItem> new_items;
        auto result = todo::merge(old_items, new_items, "append");

        expect(result.error.has_value()) << "clear with unfinished items should fail";
        expect(result.error->find("Cannot clear todos") != string::npos);
    };

    "overwrite_blocked_by_unfinished_items"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "pending"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("B", "done"),
        };
        auto result = todo::merge(old_items, new_items, "overwrite");

        expect(result.error.has_value()) << "overwrite with unfinished old items should fail";
        expect(result.error->find("Cannot overwrite todos") != string::npos);
    };

    "overwrite_when_all_old_done_succeeds"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
            make_item("B", "done"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("C", "pending"),
        };
        auto result = todo::merge(old_items, new_items, "overwrite");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 1u));
        expect(eq(result.items[0].title, string("C")));
        expect(eq(result.archived.size(), 2u)) << "both old done items should be archived";
    };

    "force_overwrite_replaces_list_and_archives_done"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
            make_item("B", "pending"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("C", "in_progress"),
        };
        auto result = todo::merge(old_items, new_items, "force_overwrite");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 1u));
        expect(eq(result.items[0].title, string("C")));
        expect(eq(result.archived.size(), 1u));
        expect(eq(result.archived[0].title, string("A")));
        expect(result.regressed.empty());
    };

    "empty_old_list_appends_all_new_items"_test = [] {
        vector<todo::TodoItem> old_items;
        vector<todo::TodoItem> new_items = {
            make_item("A", "pending"),
            make_item("B", "pending"),
        };
        auto result = todo::merge(old_items, new_items, "append");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 2u));
    };

    "duplicate_new_titles_return_error"_test = [] {
        vector<todo::TodoItem> new_items = {
            make_item("A", "pending"),
            make_item("A", "done"),
        };
        vector<todo::TodoItem> old_items;
        auto result = todo::merge(old_items, new_items, "append");

        expect(result.error.has_value());
        expect(result.error->find("Duplicate todo titles found") != string::npos);
    };

    "invalid_status_returns_error"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("A", "unknown_status"),
        };
        auto result = todo::merge({}, items, "append");

        expect(result.error.has_value());
        expect(result.error->find("Invalid status") != string::npos);
    };

    "status_is_case_and_dash_insensitive"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("A", "In-Progress"),
        };
        auto canon = todo::canonical_status("In-Progress");
        expect(canon.has_value());
        expect(eq(*canon, string("in_progress")));

        auto result = todo::merge({}, items, "append");
        expect(!result.error.has_value());
        expect(eq(result.items[0].status, string("in_progress")));
    };

    "regression_done_to_pending_is_clamped"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("A", "pending"),
        };
        auto result = todo::merge(old_items, new_items, "append");

        expect(!result.error.has_value()) << "regressions are reported, not thrown";
        expect(eq(result.items.size(), 1u));
        expect(eq(result.items[0].status, string("done"))) << "regressed item clamped to done";
        expect(eq(result.regressed.size(), 1u));
        expect(eq(result.regressed[0], string("A")));
    };

    "regression_done_to_in_progress_is_clamped"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "done"),
            make_item("B", "done"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("A", "in_progress"),
            make_item("B", "done"),
        };
        auto result = todo::merge(old_items, new_items, "append");

        expect(eq(result.items[0].status, string("done")));
        expect(eq(result.regressed.size(), 1u));
        expect(eq(result.regressed[0], string("A")));
    };

    "fuzzy_warnings_are_forwarded_in_order"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("A", "pending"),
        };
        vector<todo::TodoItem> new_items = {
            make_item("B", "pending"),
            make_item("C", "pending"),
        };
        unordered_map<string, vector<string>, string_hash> fuzzy;
        fuzzy["B"] = {"B looks like A"};
        fuzzy["C"] = {"C warning 1", "C warning 2"};

        auto result = todo::merge(old_items, new_items, "append", fuzzy);

        expect(!result.error.has_value());
        expect(eq(result.warnings.size(), 3u));
        expect(eq(result.warnings[0], string("B looks like A")));
        expect(eq(result.warnings[1], string("C warning 1")));
        expect(eq(result.warnings[2], string("C warning 2")));
    };

    "auto_fix_marks_extra_in_progress_done"_test = [] {
        vector<todo::TodoItem> new_items = {
            make_item("A", "in_progress"),
            make_item("B", "in_progress"),
            make_item("C", "pending"),
        };
        auto result = todo::merge({}, new_items, "append");

        expect(!result.error.has_value());
        expect(eq(result.items.size(), 3u));
        expect(eq(result.items[0].status, string("in_progress")));
        expect(eq(result.items[1].status, string("done")));
        expect(eq(result.items[2].status, string("pending")));
        expect(eq(result.warnings.size(), 1u));
        expect(result.warnings[0].find("Auto-fixed \"B\"") != string::npos);
    };

    "status_counts_returns_expected_totals"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("A", "pending"),
            make_item("B", "in_progress"),
            make_item("C", "done"),
            make_item("D", "done"),
        };
        auto counts = todo::status_counts(items);
        expect(eq(counts["pending"], 1u));
        expect(eq(counts["in_progress"], 1u));
        expect(eq(counts["done"], 2u));
    };

    "format_summary_renders_active_items"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("A", "pending", nullptr, "!pytest -x"),
            make_item("B", "in_progress", "some notes"),
            make_item("C", "done"),
        };
        auto summary = todo::format_summary(items);

        expect(summary.find("- [pending] A") != string::npos);
        expect(summary.find("`[code: bash]`") != string::npos || summary.find("`[code: pwsh]`") != string::npos);
        expect(summary.find("- [in progress] B") != string::npos);
        expect(summary.find("Notes: some notes") != string::npos);
        expect(summary.find("C") == string::npos) << "done items are filtered by default";
    };

    "format_summary_respects_max_items"_test = [] {
        vector<todo::TodoItem> items;
        for (int i = 0; i < 10; ++i) {
            items.push_back(make_item(std::to_string(i).c_str(), "pending"));
        }
        auto summary = todo::format_summary(items, 3);
        expect(eq(std::count(summary.begin(), summary.end(), '\n'), 2))
            << "three items produce two newlines";
    };

    "children_are_validated_recursively"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("P", "pending"),
        };
        items[0].children = {make_item("C", "bogus")};
        auto result = todo::merge({}, items, "append");
        expect(result.error.has_value());
        expect(result.error->find("Invalid status") != string::npos);
    };

    "children_are_preserved_on_same_title_update"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("P", "pending"),
        };
        old_items[0].children = {make_item("C", "pending")};
        // same-title update without children keeps the old subtree
        vector<todo::TodoItem> new_items = {make_item("P", "done")};
        auto result = todo::merge(old_items, new_items, "append");
        expect(!result.error.has_value());
        expect(eq(result.items.size(), 1u));
        expect(eq(result.items[0].status, string("done")));
        expect(eq(result.items[0].children.size(), 1u)) << "old children preserved";
        expect(eq(result.items[0].children[0].title, string("C")));
    };

    "children_are_replaced_when_new_carries_them"_test = [] {
        vector<todo::TodoItem> old_items = {
            make_item("P", "pending"),
        };
        old_items[0].children = {make_item("C", "pending")};
        vector<todo::TodoItem> new_items = {make_item("P", "pending")};
        new_items[0].children = {make_item("C2", "pending")};
        auto result = todo::merge(old_items, new_items, "append");
        expect(!result.error.has_value());
        expect(eq(result.items[0].children.size(), 1u));
        expect(eq(result.items[0].children[0].title, string("C2"))) << "children replaced";
    };

    "force_overwrite_keeps_incoming_children"_test = [] {
        vector<todo::TodoItem> new_items = {
            make_item("P", "pending"),
        };
        new_items[0].children = {make_item("C", "pending")};
        auto result = todo::merge({}, new_items, "force_overwrite");
        expect(!result.error.has_value());
        expect(eq(result.items[0].children.size(), 1u));
        expect(eq(result.items[0].children[0].title, string("C")));
    };

    "status_counts_ignores_children"_test = [] {
        vector<todo::TodoItem> items = {
            make_item("P", "pending"),
        };
        items[0].children = {make_item("C", "in_progress")};
        auto counts = todo::status_counts(items);
        expect(eq(counts["pending"], 1u)) << "children are not counted (top-level only)";
        expect(eq(counts["in_progress"], 0u));
    };
}
