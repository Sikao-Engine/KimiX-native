// Test for src/runtime/tools/security.h (plan: commit 0582e09 "Study from
// hermes" -- security.py + background/utils.py bounded_append).
// This test covers:
// - redact_sensitive_output: all 10 chained rules with reference goldens
//   (URL userinfo, JWT, PEM, GitHub/GitLab/AWS tokens, auth headers,
//   password/secret assignments, bare bearer), leftmost/non-overlap/order
// - scrub_child_env: safe-prefix keep / secret-substring drop, order kept
// - validate_workdir: allowed set, error message with Python repr of the
//   first offending character (ASCII, non-ASCII, control chars)
// - bounded_append: head 40% / tail 60% + marker, int(cap * 0.4) semantics

#include "ut/ut.hpp"
#include <runtime/tools/security.h>

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::tools;

namespace {
kimix::string_view sv(const std::string& s) { return kimix::string_view(s); }

kimix::string redact(const char* s) { return redact_sensitive_output(sv(s)); }
} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "redact_empty_and_plain"_test = [] {
        expect((redact("") == ""));
        expect((redact("hello world") == "hello world"));
        expect((redact("no secrets here") == "no secrets here"));
    };

    "redact_url_userinfo"_test = [] {
        expect((redact("https://user:pass@example.com/path") == "https://[REDACTED]@example.com/path"));
        expect((redact("http://a:b@c/") == "http://[REDACTED]@c/"));
        expect((redact("prefix https://u:p@h suffix") == "prefix https://[REDACTED]@h suffix"));
        // no colon-password -> untouched
        expect((redact("http://user@host:8080/path") == "http://user@host:8080/path"));
    };

    "redact_jwt"_test = [] {
        expect((redact("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0."
                         "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c") == "[REDACTED]"));
        // assignment rule replaces the WHOLE match (keyword + value)
        expect((redact("token: eyJh.eyJ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c") == "[REDACTED]"));
    };

    "redact_pem"_test = [] {
        expect((redact("-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA...\n"
                         "-----END RSA PRIVATE KEY-----\n") == "[REDACTED]\n"));
        expect((redact("-----BEGIN PRIVATE KEY-----abc-----END PRIVATE KEY-----") == "[REDACTED]"));
        expect((redact("-----BEGIN OPENSSH PRIVATE KEY-----x"
                         "-----END OPENSSH PRIVATE KEY-----") == "[REDACTED]"));
        expect((redact("-----BEGIN EC PRIVATE KEY-----x-----END EC PRIVATE KEY-----") == "[REDACTED]"));
        // no END marker -> untouched
        expect((redact("no end marker -----BEGIN PRIVATE KEY-----") == "no end marker -----BEGIN PRIVATE KEY-----"));
    };

    "redact_tokens"_test = [] {
        expect((redact("ghp_abcdefghijklmnopqrstuvwxyz") == "[REDACTED]"));
        expect((redact("gho_abcdefghijklmnopqrstuvwxyz!") == "[REDACTED]!"));
        expect((redact("ghp_short") == "ghp_short"));
        expect((redact("github_pat_abcdefghijklmnopqrstuvwxyz") == "[REDACTED]"));
        expect((redact("github_pat_short") == "github_pat_short"));
        expect((redact("glpat-abcdefghijklmnopqrstuv") == "[REDACTED]"));
        expect((redact("glpat-short") == "glpat-short"));
        expect((redact("AKIAIOSFODNN7EXAMPLE") == "[REDACTED]"));
        expect((redact("AKIASHORT") == "AKIASHORT"));
    };

    "redact_auth_headers"_test = [] {
        expect((redact("Authorization: Bearer abc123") == "[REDACTED]"));
        expect((redact("authorization: abc123") == "[REDACTED]"));
        expect((redact("x-api-key: k1234567") == "[REDACTED]"));
        expect((redact("apikey=k1234567") == "[REDACTED]"));
        expect((redact("Proxy-Authorization: xyz") == "[REDACTED]"));
        expect((redact("Authorization:bearer abc") == "[REDACTED]"));
        expect((redact("Authorization: bearerX") == "[REDACTED]"));
    };

    "redact_assignments"_test = [] {
        expect((redact("password=supersecret") == "[REDACTED]"));
        expect((redact("password='secret123'") == "[REDACTED]"));
        expect((redact("password=\"secret456\"") == "[REDACTED]"));
        expect((redact("password=x") == "password=x")); // short value untouched
        expect((redact("token: abcdefghij") == "[REDACTED]"));
        expect((redact("secret = s3cr3tvalue") == "[REDACTED]"));
        expect((redact("api_key=abcdef123456") == "[REDACTED]"));
        expect((redact("api-key=abcdef123456") == "[REDACTED]"));
        expect((redact("access_key=abcdef123456") == "[REDACTED]"));
        expect((redact("PASSWORD = 'abcdef12'") == "[REDACTED]"));
        // \b before the keyword: embedded "password" is not a boundary
        expect((redact("mypassword=abcdef123") == "mypassword=abcdef123"));
        expect((redact("passwordx=abcdef123") == "passwordx=abcdef123"));
    };

    "redact_bearer_and_chain"_test = [] {
        expect((redact("Bearer abcdefghijklmnopqrstuvwxyz") == "[REDACTED]"));
        expect((redact("bearer  abcdefghijklmnopqrstuvwxyz") == "[REDACTED]"));
        expect((redact("Bearer short") == "Bearer short"));
        // chained: URL first, then auth header redacts the remainder
        expect((redact("url https://u:p@h; Authorization: Bearer abc") == "url https://[REDACTED]@h; [REDACTED]"));
        // JWT then ghp_ in the same text
        expect((redact("combined: eyJh.eyJ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c "
                         "and ghp_abcdefghijklmnopqrstuvwxyz") == "combined: eyJh.eyJ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c "
                  "and [REDACTED]"));
    };

    "scrub_child_env"_test = [] {
        kimix::vector<env_entry> in, out;
        in.push_back({"PATH", "/usr/bin"});
        in.push_back({"AWS_SECRET_ACCESS_KEY", "x"});
        in.push_back({"DATABASE_URL", "postgres://u:p@h/db"});
        in.push_back({"SSH_AUTH_SOCK", "/tmp/ssh"});
        in.push_back({"MY_TOKEN", "t"});
        in.push_back({"KIMIX_API_KEY", "k"});
        in.push_back({"USER", "u"});
        in.push_back({"AWS_ACCESS_KEY_ID", "ak"});
        in.push_back({"GIT_ASKPASS", "g"});
        scrub_child_env(in, out);
        expect((out.size() == 6u));
        expect((out[0].name == "PATH"));
        expect((out[1].name == "DATABASE_URL"));
        expect((out[2].name == "SSH_AUTH_SOCK"));
        expect((out[3].name == "KIMIX_API_KEY"));
        expect((out[4].name == "USER"));
        expect((out[5].name == "GIT_ASKPASS"));
        // empty input -> empty output
        kimix::vector<env_entry> e2, o2;
        scrub_child_env(e2, o2);
        expect(o2.empty());
        // lowercase secret substrings are found after upper-casing the name
        kimix::vector<env_entry> in3, out3;
        in3.push_back({"api_secret_key", "x"});
        in3.push_back({"LC_ALL", "C"});
        scrub_child_env(in3, out3);
        expect((out3.size() == 1u));
        expect((out3[0].name == "LC_ALL"));
    };

    "validate_workdir"_test = [] {
        expect(!validate_workdir(sv("")).has_value());
        expect(!validate_workdir(sv("C:\\Users\\me")).has_value());
        expect(!validate_workdir(sv("/tmp/x")).has_value());
        expect(!validate_workdir(sv("a b-c.d~")).has_value());
        expect(!validate_workdir(sv("C:\\Windows\\System32")).has_value());
        expect((*validate_workdir(sv("a;b")) == "Invalid workdir: character ';' is not allowed."));
        expect((*validate_workdir(sv("a\nb")) == "Invalid workdir: character '\\n' is not allowed."));
        expect((*validate_workdir(sv("a\tb")) == "Invalid workdir: character '\\t' is not allowed."));
        expect((*validate_workdir(sv("a$b")) == "Invalid workdir: character '$' is not allowed."));
        expect((*validate_workdir(sv("x!y")) == "Invalid workdir: character '!' is not allowed."));
        expect((*validate_workdir(sv("\x01x")) == "Invalid workdir: character '\\x01' is not allowed."));
        // non-ASCII: literal repr for printable chars
        const std::string caf = "caf\xC3\xA9"; // "café"
        expect((*validate_workdir(sv(caf)) == "Invalid workdir: character '\xC3\xA9' is not allowed."));
        // non-ASCII non-printable: \x / \u escapes
        const std::string nbsp = "a\xC2\xA0" "b"; // U+00A0
        expect((*validate_workdir(sv(nbsp)) == "Invalid workdir: character '\\xa0' is not allowed."));
        const std::string zwsp = "a\xE2\x80\x8B" "b"; // U+200B
        expect((*validate_workdir(sv(zwsp)) == "Invalid workdir: character '\\u200b' is not allowed."));
    };

    "bounded_append"_test = [] {
        bounded_result r = bounded_append(sv(""), sv("hello"), 100);
        expect(!r.truncated);
        expect((r.content == "hello"));
        r = bounded_append(sv("hello"), sv(""), 100);
        expect(!r.truncated);
        expect((r.content == "hello"));
        r = bounded_append(sv(""), sv(""), 0);
        expect(!r.truncated);
        expect((r.content == ""));
        // truncation: head int(20*0.4)=8, tail 12
        r = bounded_append(sv("abcdefghijklmnopqrstuvwxyz"), sv("0123456789"), 20);
        expect(r.truncated);
        expect((r.content == "abcdefgh\n[... (output truncated, keeping first 8 "
                             "and last 12 chars)]\nyz0123456789"));
        // head int(3*0.4)=1, tail 2
        r = bounded_append(sv("xxxxx"), sv("yyyyy"), 3);
        expect(r.truncated);
        expect((r.content == "x\n[... (output truncated, keeping first 1 and last "
                             "2 chars)]\nyy"));
        // cap 0: head 0, tail 0
        r = bounded_append(sv(""), sv("x"), 0);
        expect(r.truncated);
        expect((r.content == "\n[... (output truncated, keeping first 0 and last "
                             "0 chars)]\n"));
        // non-ASCII: slicing is CHARACTER-based (Python str), not byte-based
        // full = "éééééxxxxx" (10 chars); head 2 chars = "éé" (4 bytes),
        // tail 4 chars = "xxxx".
        r = bounded_append(sv("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"),
                           sv("xxxxx"), 6);
        expect(r.truncated);
        expect((r.content ==
                "\xC3\xA9\xC3\xA9\n[... (output truncated, keeping first 2 and "
                "last 4 chars)]\nxxxx"));
    };
}
