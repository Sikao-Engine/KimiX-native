// test_python_code_session.cpp - H8: the persistent /code exec context.
// Covers the request/response protocol kernels (make_request /
// parse_response / the "@CODE_RESULT@" framing) and the live session: state
// persists across exec_file calls (the exec_ctx analogue), __file__ and
// sys.argv are set per run, a failing script leaves the session usable, and
// a missing interpreter degrades to a clean start failure (the caller falls
// back to the one-shot spawn).
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.
// The live tests skip cleanly when no python interpreter is installed.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/python_code_session.h"
#include "builtin_tools/python_tool.h"

#include <cstdio>

namespace {

using namespace boost::ut;

bool python_available() {
    return !kimix::builtin_tools::python::Python::detect_python_exe().empty();
}

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

bool write_text(const kimix::string &path, const kimix::string &text) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

} // namespace

int main() {
    using namespace boost::ut;
    using kimix::builtin_tools::python::CodeExecSession;
    using kimix::builtin_tools::python::exec_result;

    "protocol_request_carries_code_path_and_argv"_test = [] {
        const kimix::string request = CodeExecSession::make_request(
            "print('hi')", "C:/tmp/script.py", {"a", "b c"}, 7);
        expect(request.find("\"op\":\"exec\"") != kimix::string::npos)
            << request;
        expect(request.find("\"code\":\"print('hi')\"") != kimix::string::npos)
            << request;
        expect(request.find("\"path\":\"C:/tmp/script.py\"") !=
               kimix::string::npos)
            << request;
        expect(request.find("\"argv\":[\"a\",\"b c\"]") != kimix::string::npos)
            << request;
        expect(request.find("\"id\":7") != kimix::string::npos) << request;
    };

    "protocol_response_parses_every_field"_test = [] {
        exec_result out;
        expect(CodeExecSession::parse_response(
            R"(@CODE_RESULT@{"ok":true,"output":"out text","stderr":"e"})",
            out));
        expect(out.ok);
        expect(eq(out.output, kimix::string("out text")));
        expect(eq(out.stderr_text, kimix::string("e")));
        expect(out.error.empty());
        expect(out.traceback.empty());

        expect(CodeExecSession::parse_response(
            R"(@CODE_RESULT@{"ok":false,"error":"ZeroDivisionError: division by zero","traceback":"Traceback (most recent call last):"})",
            out));
        expect(!out.ok);
        expect(out.error.find("ZeroDivisionError") != kimix::string::npos);
        expect(out.traceback.find("Traceback") != kimix::string::npos);

        // Framing without the marker / broken JSON fails.
        expect(!CodeExecSession::parse_response("garbage", out));
        expect(!CodeExecSession::parse_response("@CODE_RESULT@{", out));
    };

    "driver_source_keeps_the_namespace_alive"_test = [] {
        // The embedded driver must exec() into a module-level dict (the
        // exec_ctx analogue), set __file__/sys.argv per request and answer on
        // the "@CODE_RESULT@" channel.
        const kimix::string_view source = CodeExecSession::driver_source();
        expect(!source.empty());
        expect(kimix::string(source).find("_CTX = {}") != kimix::string::npos);
        expect(kimix::string(source).find("exec(compile(code, path, \"exec\"), _CTX)") !=
               kimix::string::npos);
        expect(kimix::string(source).find("_CTX[\"__file__\"] = path") !=
               kimix::string::npos);
        expect(kimix::string(source).find("sys.argv = [path] + list(argv)") !=
               kimix::string::npos);
        expect(kimix::string(source).find("@CODE_RESULT@") !=
               kimix::string::npos);
        // The session survives a failing script (the reference's exec_ctx
        // keeps working after an exception).
        expect(kimix::string(source).find("except BaseException as exc:") !=
               kimix::string::npos);
    };

    "session_start_fails_cleanly_without_an_interpreter"_test = [] {
          if (python_available()) {
              std::printf("skipping: python is installed\n");
              return;
          }
        CodeExecSession session;
        kimix::string error;
        expect(!session.start("<no such work dir>", error));
        expect(!error.empty());
        expect(!session.alive());
    };

    "live_session_persists_state_across_calls"_test = [] {
          if (!python_available()) {
              std::printf("skipping: no python interpreter\n");
              return;
          }
        const kimix::string ws = tmp_workspace("kimix_test_code_session_ws");
        CodeExecSession session;
        kimix::string error;
        expect(session.start(ws, error)) << error;

        // Script 1: define state and write it into the workspace.
        const kimix::string first_script =
            kimix::to_string(kimix::filesystem::path(ws) / "one.py");
        expect(write_text(first_script,
                          "COUNTER = 41\n"
                          "print('defined', COUNTER)\n"));
        exec_result out;
        expect(session.exec_file(first_script, {}, out, error)) << error;
        expect(out.ok);
        expect(out.output.find("defined 41") != kimix::string::npos) << out.output;

        // Script 2: the previous execution namespace is still alive.
        const kimix::string second_script =
            kimix::to_string(kimix::filesystem::path(ws) / "two.py");
        expect(write_text(second_script,
                          "COUNTER += 1\n"
                          "print('argv', sys.argv[1:])\n"
                          "import sys\n"));
        // (sys is imported after the use on purpose? no - import first.)
        expect(write_text(second_script,
                          "import sys\n"
                          "COUNTER += 1\n"
                          "print('argv', sys.argv[1:])\n"));
        out = exec_result{};
        expect(session.exec_file(second_script, {"alpha", "beta"}, out, error))
            << error;
        expect(out.ok) << out.error << out.traceback;
        // State from one.py survived (the exec_ctx analogue).
        expect(out.output.find("argv ['alpha', 'beta']") != kimix::string::npos)
            << out.output;

        // __file__ and sys.argv mirror the reference's per-call setup.
        const kimix::string third_script =
            kimix::to_string(kimix::filesystem::path(ws) / "three.py");
        expect(write_text(third_script,
                          "print('file', __file__.replace(chr(92), '/'))\n"
                          "print('argv', sys.argv)\n"));
        out = exec_result{};
        expect(session.exec_file(third_script, {"x"}, out, error)) << error;
        expect(out.ok) << out.error;
        expect(out.output.find("file ") != kimix::string::npos) << out.output;
        expect(out.output.find("three.py") != kimix::string::npos) << out.output;
          expect(out.output.find("argv ['") != kimix::string::npos) << out.output;
          // sys.argv[0] is the script path as passed (host separators), the
          // extra args follow: "..., 'three.py', 'x']".
          expect(out.output.find("three.py', 'x']") != kimix::string::npos)
              << out.output;

        // A failing script reports the error + traceback and the session
        // STAYS usable (commands.py catches the exception and continues).
        const kimix::string bad_script =
            kimix::to_string(kimix::filesystem::path(ws) / "bad.py");
        expect(write_text(bad_script, "raise ValueError('no can do')\n"));
        out = exec_result{};
        expect(session.exec_file(bad_script, {}, out, error)) << error;
        expect(!out.ok);
        expect(out.error.find("no can do") != kimix::string::npos) << out.error;
        expect(out.traceback.find("ValueError") != kimix::string::npos)
            << out.traceback;
        const kimix::string after_script =
            kimix::to_string(kimix::filesystem::path(ws) / "after.py");
        expect(write_text(after_script, "print('still alive', COUNTER >= 41)\n"));
        out = exec_result{};
        expect(session.exec_file(after_script, {}, out, error)) << error;
        expect(out.ok) << out.error;
        expect(out.output.find("still alive True") != kimix::string::npos)
            << out.output;

        session.shutdown();
        expect(!session.alive());
    };

    return 0;
}
