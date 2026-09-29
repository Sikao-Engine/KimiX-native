// test_tool_registry_schema.cpp - F12 (audit G26): the light-weight
// JSON-Schema meta-validation run at registration, plus F14 (audit G14):
// register_external_tool - the runtime registration of a host-answered tool
// from a wire schema (name-conflict policy, empty-description default, the
// schema refusal and the host answering layer as a test double).
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/tool_registry.h"
#include "builtin_tools/tool_schema_validate.h"
#include "agent/soul.h"

#include <cstdio>

namespace {

using namespace boost::ut;

class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_ws() {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) /
        "kimix_test_tool_registry_schema_ws";
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

} // namespace

int main() {
    using namespace boost::ut;
    using kimix::builtin_tools::ToolMeta;

    // ── The structural validator (kosong/tooling/__init__.py:33-79) ────────

    "validator_accepts_the_builtin_registry_schemas"_test = [] {
        // Every registration the process made (the whole built-in set went
        // through KIMIX_REGISTER_TOOL_* -> register_tool_validated) must have
        // produced NO diagnostic: a broken schema literal now fails at load.
        const auto &diagnostics =
            kimix::builtin_tools::ToolRegistry::instance()
                .registration_diagnostics();
        if (!diagnostics.empty()) {
            for (const kimix::string &d : diagnostics) {
                std::fprintf(stderr, "diagnostic: %s\n", d.c_str());
            }
        }
        expect(diagnostics.empty());
        // And the schemas themselves validate one by one.
        for (const ToolMeta &meta :
             kimix::builtin_tools::ToolRegistry::instance().all()) {
            kimix::string error;
            expect(kimix::builtin_tools::schema_validate::
                       validate_parameters_schema(meta.parameters_json, error))
                << meta.name << ": " << error;
        }
    };

    "write_and_edit_schemas_expose_allow_conflicts"_test = [] {
        // F-new-4: write/edit refused conflict-marker content while telling
        // the caller to "pass allow_conflicts=true", but the parameter was
        // not declared in either schema - the dispatch layer dropped it and
        // the refusal repeated, making the guardrail unactionable. Both
        // tools already honor the flag; the schemas must expose it.
        bool saw_write = false;
        bool saw_edit = false;
        for (const ToolMeta &meta :
             kimix::builtin_tools::ToolRegistry::instance().all()) {
            if (meta.name == "write") {
                saw_write = true;
                expect(meta.parameters_json.find("allow_conflicts") !=
                       kimix::string::npos)
                    << meta.name;
            }
            if (meta.name == "edit") {
                saw_edit = true;
                expect(meta.parameters_json.find("allow_conflicts") !=
                       kimix::string::npos)
                    << meta.name;
            }
        }
        expect(saw_write);
        expect(saw_edit);
    };

    "validator_checks_types_properties_required_enum"_test = [] {        kimix::string error;
        using kimix::builtin_tools::schema_validate::validate_parameters_schema;
        // Well-formed schemas pass.
        expect(validate_parameters_schema("{}", error));
        expect(validate_parameters_schema(
            R"({"type":"object","properties":{"a":{"type":"string"}},"required":["a"]})",
            error));
        expect(validate_parameters_schema(
            R"({"type":"object","properties":{"a":{"type":["string","null"]}}})",
            error));
        expect(validate_parameters_schema(
            R"({"type":"object","properties":{"a":{"type":"object","properties":{"b":{"type":"integer"}},"required":["b"]}}})",
            error));
        expect(validate_parameters_schema(
            R"({"type":"object","properties":{"a":{"items":{"type":"string"}}}})",
            error));
        expect(validate_parameters_schema(
            R"({"type":"object","properties":{"e":{"enum":["a","b"]}}})", error));
        // Unknown keywords are allowed (JSON Schema is an open world).
        expect(validate_parameters_schema(
            R"({"type":"object","x-custom":{"weird":true}})", error));
        // Boolean schemas are valid.
        expect(validate_parameters_schema("true", error));
        // A non-object document is refused.
        expect(!validate_parameters_schema("[]", error));
        expect(!validate_parameters_schema("42", error));
        expect(!validate_parameters_schema("not json", error));
        // "type" must name a simple type.
        expect(!validate_parameters_schema(R"({"type":"strin"})", error));
        expect(!validate_parameters_schema(R"({"type":{"a":1}})", error));
        expect(!validate_parameters_schema(R"({"type":["string","nope"]})",
                                           error));
        // "properties" values must be schemas (recursion).
        expect(!validate_parameters_schema(
            R"({"type":"object","properties":{"a":{"type":"bogus"}}})", error));
        // "required" must be an array of non-empty strings.
        expect(!validate_parameters_schema(
            R"({"type":"object","required":["a",42]})", error));
        expect(!validate_parameters_schema(
            R"({"type":"object","required":"a"})", error));
        // "enum" must be a non-empty array.
        expect(!validate_parameters_schema(R"({"enum":[]})", error));
        expect(!validate_parameters_schema(R"({"enum":"x"})", error));
        // "items" recurses.
        expect(!validate_parameters_schema(
            R"({"items":{"type":"nope"}})", error));
    };

    // ── register_tool_validated (the KIMIX_REGISTER_TOOL_* path) ───────────

    "register_tool_validated_refuses_an_invalid_schema"_test = [] {
        auto &registry = kimix::builtin_tools::ToolRegistry::instance();
        const size_t before = registry.size();
        ToolMeta bad;
        bad.name = "schema_bad_probe";
        bad.description = "probe";
        bad.parameters_json = R"({"type":"object","required":"oops"})";
        kimix::string error;
        expect(!registry.register_tool_validated(std::move(bad), error));
        expect(error.find("invalid parameters schema for tool "
                          "'schema_bad_probe'") != kimix::string::npos)
            << error;
        // The tool was never registered and the diagnostic is recorded.
        expect(registry.find("schema_bad_probe") == nullptr);
        expect(registry.size() == before);
        const auto &diagnostics = registry.registration_diagnostics();
        expect(!diagnostics.empty());
        expect(diagnostics.back().find("schema_bad_probe") !=
               kimix::string::npos);
        // A valid schema registers (last-wins replacement like register_tool).
        ToolMeta good;
        good.name = "schema_good_probe";
        good.description = "probe";
        good.parameters_json =
            R"({"type":"object","properties":{"path":{"type":"string"}}})";
        expect(registry.register_tool_validated(std::move(good), error));
        expect(registry.find("schema_good_probe") != nullptr);
        expect(registry.unregister_tool("schema_good_probe"));
        expect(registry.find("schema_good_probe") == nullptr);
        expect(!registry.unregister_tool("schema_good_probe"));
    };

    // ── register_external_tool (toolset.py:1766-1785 + WireExternalTool) ───

    "register_external_tool_conflicts_replacement_and_defaults"_test = [] {
        auto &registry = kimix::builtin_tools::ToolRegistry::instance();
        // A non-external tool owns the name: the reference's exact error.
        kimix::string error;
        kimix::builtin_tools::ExternalToolCall call =
            [](kimix::string_view, kimix::string &result_json,
               kimix::string &) {
                result_json = R"({"status":"ok","output":"host says hi"})";
                return true;
            };
        expect(!registry.register_external_tool(
            "read", "host read", "{}", call, error));
        expect(eq(error, kimix::string("tool name conflicts with existing tool")));
        // A valid registration works; an empty description gets the
        // WireExternalTool default.
        expect(registry.register_external_tool("wire_probe", "", "{}", call,
                                               error));
        const ToolMeta *meta = registry.find("wire_probe");
        expect(meta != nullptr);
        if (meta != nullptr) {
            expect(meta->external);
            expect(eq(meta->description,
                      kimix::string("No description provided.")));
        }
        // Re-registering the same EXTERNAL name replaces the entry.
        expect(registry.register_external_tool("wire_probe", "second", "{}",
                                               call, error));
        meta = registry.find("wire_probe");
        expect(meta != nullptr);
        if (meta != nullptr) {
            expect(eq(meta->description, kimix::string("second")));
        }
        // An invalid schema refuses with the validator's error (the
        // reference's Tool model validator raising through str(e)).
        expect(!registry.register_external_tool("wire_probe_bad", "x",
                                                R"({"enum":[]})", call, error));
        expect(error.find("enum") != kimix::string::npos) << error;
        expect(registry.unregister_tool("wire_probe"));
    };

    "soul_dispatches_an_external_tool_through_the_host_double"_test = [] {
        kimix::agent::AgentSession session(tmp_ws());
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        auto &registry = kimix::builtin_tools::ToolRegistry::instance();
        kimix::builtin_tools::ExternalToolCall call =
            [](kimix::string_view arguments, kimix::string &result_json,
               kimix::string &) {
                // The host sees the repaired arguments and answers with a
                // tool-result payload.
                result_json = kimix::string(R"({"status":"ok","output":"echo: )");
                result_json += arguments;
                result_json += R"("})";
                return true;
            };
        kimix::string error;
        expect(registry.register_external_tool("wire_probe2", "probe", "{}",
                                               call, error));
        // Offered to the LLM (external tools are enabled by registration).
        bool offered = false;
        for (const kimix::llm::Tool &d : soul.tool_definitions()) {
            if (d.name == "wire_probe2") {
                offered = true;
            }
        }
        expect(offered);
        // A dispatched call carries the host's payload (with the wire id).
        kimix::string err;
        const kimix::string out = soul.execute_tool_call(
            "wire_probe2", R"({"q":"v"})", err, "call-ext-1");
        expect(out.find("echo:") != kimix::string::npos) << out;
        expect(out.find("host says hi") == kimix::string::npos) << out;

        // Without a wire id the call is refused (the reference's missing
        // tool-call context).
        err.clear();
        const kimix::string no_ctx =
            soul.execute_tool_call("wire_probe2", "{}", err);
        expect(eq(no_ctx, kimix::string("<system>ERROR: External tool calls "
                                        "must be invoked from a tool call "
                                        "context.</system>")))
            << no_ctx;

        // Without the host answering layer: no wire.
        expect(registry.register_external_tool("wire_probe3", "probe", "{}",
                                               kimix::builtin_tools::ExternalToolCall{},
                                               error));
        err.clear();
        const kimix::string no_wire =
            soul.execute_tool_call("wire_probe3", "{}", err, "call-ext-2");
        expect(eq(no_wire, kimix::string("<system>ERROR: Wire is not available "
                                         "for external tool calls.</system>")))
            << no_wire;

        // A failing host answer becomes the External tool error.
        kimix::builtin_tools::ExternalToolCall failing =
            [](kimix::string_view, kimix::string &, kimix::string &err2) {
                err2 = "host crashed";
                return false;
            };
        expect(registry.register_external_tool("wire_probe4", "probe", "{}",
                                               failing, error));
        err.clear();
        const kimix::string failed =
            soul.execute_tool_call("wire_probe4", "{}", err, "call-ext-3");
        expect(failed.find("External tool call failed: host crashed") !=
               kimix::string::npos)
            << failed;

        expect(registry.unregister_tool("wire_probe2"));
        expect(registry.unregister_tool("wire_probe3"));
        expect(registry.unregister_tool("wire_probe4"));
    };

    return 0;
}
