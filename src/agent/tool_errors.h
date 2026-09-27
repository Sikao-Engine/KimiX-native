// agent/tool_errors.h - The typed tool-error taxonomy (F10 / audit G19).
//
// Port of kosong/tooling/error.py (44 lines, verbatim strings) plus the two
// dispatch-side errors toolset.py builds inline:
//
//   ToolNotFoundError  message/brief "Tool `{name}` not found[ - did you mean
//                      `a`, `b`?]"                         (error.py:4-14)
//   ToolParseError     "Error parsing JSON arguments: {detail}" /
//                      "Invalid arguments"                  (error.py:17-24)
//   ToolValidateError  "Error validating JSON arguments: {detail}" /
//                      "Invalid arguments"                  (error.py:27-34);
//                      the coercion branch composes
//                      "Invalid arguments for tool `{name}`: {e}"
//                      (toolset.py:1496-1509)
//   ToolRuntimeError   "Error running tool: {detail}" /
//                      "Tool runtime error"                 (error.py:37-44)
//   hook block         ToolError(message=result.reason or
//                      "Blocked by PreToolUse hook", brief="Hook blocked")
//                      (toolset.py:1526-1534)
//   malformed param    brief "Malformed parameter"
//                      (toolset.py:1555-1572)
//
// Every builder returns the model-facing message; `brief` (the short UI
// string) goes through the out-parameter when requested. The caller keeps the
// <system>ERROR: ...</system> envelope intact - only the text inside changes.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <core/kimix_core.h>

namespace kimix::agent {

// Shared briefs (error.py verbatim).
inline constexpr kimix::string_view kBriefInvalidArguments = "Invalid arguments";
inline constexpr kimix::string_view kBriefToolRuntimeError = "Tool runtime error";
inline constexpr kimix::string_view kBriefHookBlocked = "Hook blocked";
inline constexpr kimix::string_view kBriefMalformedParameter =
    "Malformed parameter";

// ToolNotFoundError (error.py:4-14). With suggestions the message and brief
// gain " - did you mean `a`, `b`, `c`?".
kimix::string tool_not_found_error(kimix::string_view tool_name,
                                   const kimix::vector<kimix::string> &suggestions,
                                   kimix::string &brief);

// ToolParseError (error.py:17-24): "Error parsing JSON arguments: {detail}".
kimix::string tool_parse_error(kimix::string_view detail, kimix::string &brief);

// ToolValidateError (error.py:27-34 + toolset.py:1496-1509): the detail is
// the coercion failure, already composed as
// "Invalid arguments for tool `{name}`: {e}" by the caller-facing overload
// below.
kimix::string tool_validate_error(kimix::string_view detail, kimix::string &brief);
// The composed form: "Error validating JSON arguments: Invalid arguments for
// tool `{name}`: {e}".
kimix::string tool_validate_error_for(kimix::string_view tool_name,
                                      kimix::string_view detail,
                                      kimix::string &brief);

// ToolRuntimeError (error.py:37-44): "Error running tool: {detail}".
kimix::string tool_runtime_error(kimix::string_view detail, kimix::string &brief);

// The PreToolUse hook block (toolset.py:1526-1534): message =
// `reason` or "Blocked by PreToolUse hook", brief "Hook blocked".
kimix::string hook_blocked_error(kimix::string_view reason, kimix::string &brief);

// The long-param retry error (toolset.py:1555-1572): the message is
// build_long_param_retry_msg(saved, "Parameters appear to be in the wrong
// format. The raw content has been saved to temp files."), brief "Malformed
// parameter".
kimix::string malformed_parameter_error(kimix::string_view retry_message,
                                        kimix::string &brief);

} // namespace kimix::agent
