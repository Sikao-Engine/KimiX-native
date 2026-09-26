// test_tool_taxonomy.cpp - Unit tests for the tool taxonomy port
// (src/agent/tool_taxonomy.h), a byte-faithful port of
// kimi_cli/soul/tool_taxonomy.py: exact EDIT_TOOLS / SHELL_TOOLS /
// VERIFICATION_TOOL_HINTS / PATH_PARAM_KEYS / COMMAND_PARAM_KEYS contents and
// the is_edit_tool / is_shell_tool / is_verification_tool_hint / path_params_of
// predicates.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <agent/tool_taxonomy.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main() {
    using kimix::agent::is_edit_tool;
    using kimix::agent::is_shell_tool;
    using kimix::agent::is_verification_tool_hint;
    using kimix::agent::path_params_of;

    "edit_tools_match_reference_set"_test = [] {
        expect(is_edit_tool("write"));
        expect(is_edit_tool("edit"));
        expect(is_edit_tool("HashEdit"));
        expect(is_edit_tool("WritePlan"));
        expect(is_edit_tool("EditPlan"));
        expect(is_edit_tool("Write"));
        expect(is_edit_tool("Edit"));
        expect(is_edit_tool("Replace"));
        expect(is_edit_tool("StrReplace"));
        expect(!is_edit_tool("read"));
        expect(!is_edit_tool("bash"));
        expect(!is_edit_tool("todo_write"));
        expect(!is_edit_tool("WRITE")); // no case folding in the reference set
    };

    "shell_tools_match_reference_set"_test = [] {
        expect(is_shell_tool("bash"));
        expect(is_shell_tool("pwsh"));
        expect(is_shell_tool("Run"));
        expect(!is_shell_tool("run")); // "Run" only, like the reference
        expect(!is_shell_tool("python"));
        expect(!is_shell_tool("edit"));
    };

    "verification_hints_are_todo_write_plus_shells"_test = [] {
        expect(is_verification_tool_hint("todo_write"));
        expect(is_verification_tool_hint("bash"));
        expect(is_verification_tool_hint("pwsh"));
        expect(is_verification_tool_hint("Run"));
        expect(!is_verification_tool_hint("write"));
        expect(!is_verification_tool_hint("edit"));
        expect(!is_verification_tool_hint("read"));
    };

    "path_params_of_extracts_first_path_key"_test = [] {
        // Exact PATH_PARAM_KEYS order: path, file_path, filename.
        const auto p1 = path_params_of(R"({"path":"src/a.cpp","other":1})");
        expect(p1.has_value());
        expect(*p1 == "src/a.cpp");
        const auto p2 = path_params_of(R"({"file_path":"b.txt"})");
        expect(p2.has_value());
        expect(*p2 == "b.txt");
        const auto p3 = path_params_of(R"({"filename":"c.md"})");
        expect(p3.has_value());
        expect(*p3 == "c.md");
        // First matching key wins.
        const auto p4 = path_params_of(R"({"path":"a","filename":"b"})");
        expect(p4.has_value());
        expect(*p4 == "a");
    };

    "path_params_of_absent_or_invalid"_test = [] {
        expect(!path_params_of(R"({"command":"ls"})").has_value());
        expect(!path_params_of("").has_value());
        expect(!path_params_of("not json").has_value());
        expect(!path_params_of(R"([1,2,3])").has_value()); // not an object
        expect(!path_params_of(R"({"path":42})").has_value()); // not a string
    };

    return 0;
}
