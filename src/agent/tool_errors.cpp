// agent/tool_errors.cpp - The typed tool-error taxonomy (see tool_errors.h).

#include "agent/tool_errors.h"

#include "agent/tool_argument_repair.h" // build_long_param_retry_msg

namespace kimix::agent {

kimix::string tool_not_found_error(kimix::string_view tool_name,
                                   const kimix::vector<kimix::string> &suggestions,
                                   kimix::string &brief) {
    // kosong/tooling/error.py:4-14.
    kimix::string message = "Tool `";
    message.append(tool_name.data(), tool_name.size());
    message += "` not found";
    if (!suggestions.empty()) {
        kimix::string hint = "did you mean ";
        for (size_t i = 0; i < suggestions.size(); ++i) {
            if (i > 0) {
                hint += ", ";
            }
            hint += "`";
            hint += suggestions[i];
            hint += "`";
        }
        hint += "?";
        message += " - ";
        message += hint;
    }
    brief = message; // the reference assigns the same composed text to both
    return message;
}

kimix::string tool_parse_error(kimix::string_view detail, kimix::string &brief) {
    kimix::string message = "Error parsing JSON arguments: ";
    message.append(detail.data(), detail.size());
    brief.assign(kBriefInvalidArguments);
    return message;
}

kimix::string tool_validate_error(kimix::string_view detail, kimix::string &brief) {
    kimix::string message = "Error validating JSON arguments: ";
    message.append(detail.data(), detail.size());
    brief.assign(kBriefInvalidArguments);
    return message;
}

kimix::string tool_validate_error_for(kimix::string_view tool_name,
                                      kimix::string_view detail,
                                      kimix::string &brief) {
    // toolset.py:1496-1509: ToolValidateError(f"Invalid arguments for tool
    // `{tool_name}`: {e}").
    kimix::string inner = "Invalid arguments for tool `";
    inner.append(tool_name.data(), tool_name.size());
    inner += "`: ";
    inner.append(detail.data(), detail.size());
    return tool_validate_error(inner, brief);
}

kimix::string tool_runtime_error(kimix::string_view detail, kimix::string &brief) {
    kimix::string message = "Error running tool: ";
    message.append(detail.data(), detail.size());
    brief.assign(kBriefToolRuntimeError);
    return message;
}

kimix::string hook_blocked_error(kimix::string_view reason, kimix::string &brief) {
    // toolset.py:1526-1534: message = result.reason or
    // "Blocked by PreToolUse hook".
    kimix::string message;
    if (reason.empty()) {
        message = "Blocked by PreToolUse hook";
    } else {
        message.assign(reason.data(), reason.size());
    }
    brief.assign(kBriefHookBlocked);
    return message;
}

kimix::string malformed_parameter_error(kimix::string_view retry_message,
                                        kimix::string &brief) {
    kimix::string message(retry_message);
    brief.assign(kBriefMalformedParameter);
    return message;
}

} // namespace kimix::agent
