// Test for the generic tool infrastructure (builtin_tools/tool.h).
// This test covers:
// - ValueElement construction: default null, tagged factories, is_* probes,
//   as_* getters, data() escape hatch, object-pointer access
// - Scalar serialize/deserialize round-trips: bool, int64 (0/-1/min/max),
//   uint64 (0/max), doubles (0.0, -0.0, 1.0, 3.14, 1e300), null, strings
//   (empty, ASCII, escapes, UTF-8, embedded NUL)
// - Nested structures: object-in-object (3+ levels), arrays of scalars,
//   arrays of objects (array of ToolParams), mixed maps
// - Empty object {} and empty array [] round-trips
// - deserialize errors: malformed JSON, non-object roots, empty span
// - try_deserialize non-throwing convenience
// - Round-trip determinism (compact writer byte stability)
// - Tool base: session() accessor, virtual operator() dispatch, virtual
//   destructor through a Tool*
// - ToolParams map helpers: contains/get/operator[]/erase
// - ToolParams fuzzy alias matching: alias_map recording (add_alias /
//   add_aliases), canonical-wins resolution, case/separator-folded matching,
//   strict get_exact/contains_exact, with_aliases() (parse-entry pattern) and
//   the fact that alias_map is a side-table that is never serialized
//   (per-tool alias acceptance lives in test_param_aliases.cpp)
#include "ut/ut.hpp"

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;

namespace {

// Recursive semantic equality for two ValueElement trees.
bool veq(const ValueElement &a, const ValueElement &b) {
    if (a.is_null() || b.is_null()) {
        return a.is_null() && b.is_null();
    }
    if (a.is_bool() || b.is_bool()) {
        return a.is_bool() && b.is_bool() && a.as_bool() == b.as_bool();
    }
    if (a.is_int() || b.is_int() || a.is_uint() || b.is_uint()) {
        const bool a_num = a.is_int() || a.is_uint();
        const bool b_num = b.is_int() || b.is_uint();
        if (!a_num || !b_num) {
            return false; // int/uint vs real mismatch (1 vs 1.0)
        }
        // Compare numerically: JSON has no unsigned literal, so values that
        // fit in int64_t normalize to the int alternative on parse.
        if (a.is_int() && b.is_int()) {
            return a.as_int() == b.as_int();
        }
        if (a.is_uint() && b.is_uint()) {
            return a.as_uint() == b.as_uint();
        }
        if (a.is_int() && b.is_uint()) {
            return a.as_int() >= 0 &&
                   static_cast<uint64_t>(a.as_int()) == b.as_uint();
        }
        if (a.is_uint() && b.is_int()) {
            return b.as_int() >= 0 &&
                   a.as_uint() == static_cast<uint64_t>(b.as_int());
        }
        return false;
    }
    if (a.is_real() || b.is_real()) {
        return a.is_real() && b.is_real() && a.as_real() == b.as_real();
    }
    if (a.is_string() || b.is_string()) {
        return a.is_string() && b.is_string() &&
               a.as_string() == b.as_string();
    }
    if (a.is_array() || b.is_array()) {
        if (!a.is_array() || !b.is_array()) {
            return false;
        }
        const ValueElement::Array &aa = a.as_array();
        const ValueElement::Array &bb = b.as_array();
        if (aa.size() != bb.size()) {
            return false;
        }
        for (size_t i = 0; i < aa.size(); ++i) {
            if (!veq(aa[i], bb[i])) {
                return false;
            }
        }
        return true;
    }
    const ToolParams *pa = a.as_object();
    const ToolParams *pb = b.as_object();
    if (pa == nullptr || pb == nullptr) {
        return false;
    }
    if (pa->values.size() != pb->values.size()) {
        return false;
    }
    for (const auto &[k, v] : pa->values) {
        auto it = pb->values.find(k);
        if (it == pb->values.end() || !veq(v, it->second)) {
            return false;
        }
    }
    return true;
}

// Semantic equality for two ToolParams objects.
bool veq_objs(const ToolParams &a, const ToolParams &b) {
    if (a.values.size() != b.values.size()) {
        return false;
    }
    for (const auto &[k, v] : a.values) {
        auto it = b.values.find(k);
        if (it == b.values.end() || !veq(v, it->second)) {
            return false;
        }
    }
    return true;
}

// Serialize a ToolParams into a kimix::string (no NUL terminator).
kimix::string to_json(const ToolParams &p) {
    kimix::vector<char> buf;
    p.serialize(buf);
    return kimix::string(buf.data(), buf.size());
}

// Round-trip: serialize `p`, parse into a fresh ToolParams, return it.
ToolParams round_trip(const ToolParams &p) {
    kimix::vector<char> buf;
    p.serialize(buf);
    ToolParams q;
    q.deserialize(kimix::span<char const>(buf.data(), buf.size()));
    return q;
}

// ── JSON golden helpers (see the encoding notes in tool_types_goldens.inc) ──

// JSON string escaping, byte for byte the same as jesc() in
// scripts/gen_tool_types_goldens.py.
std::string tt_json_str(kimix::string_view s) {
    static const char *k_hex = "0123456789abcdef";
    std::string out;
    for (char ch : s) {
        const unsigned char b = static_cast<unsigned char>(ch);
        if (b == '"') {
            out += "\\\"";
        } else if (b == '\\') {
            out += "\\\\";
        } else if (b < 0x20) {
            out += "\\u00";
            out.push_back(k_hex[(b >> 4) & 0x0F]);
            out.push_back(k_hex[b & 0x0F]);
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string tt_hex64(uint64_t v) {
    static const char *k_hex = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; i--) {
        s[static_cast<size_t>(i)] = k_hex[v & 0x0F];
        v >>= 4;
    }
    return s;
}

// Canonical rendering of one value - the C++ half of json_canon() in the
// generator: integers as "i:<dec>", reals as their IEEE-754 bit pattern,
// strings JSON-escaped, object keys SORTED (the native map is unordered).
std::string tt_canon(const ValueElement &e);

std::string tt_canon_object_entries(const ToolParams &p) {
    std::vector<std::pair<std::string, const ValueElement *>> items;
    items.reserve(p.values.size());
    for (const auto &[k, v] : p.values) {
        items.emplace_back(k, &v);
    }
    std::sort(items.begin(), items.end(),
              [](const std::pair<std::string, const ValueElement *> &a,
                 const std::pair<std::string, const ValueElement *> &b) {
                  return a.first < b.first;
              });
    std::string out;
    for (size_t i = 0; i < items.size(); i++) {
        if (i != 0u) {
            out += ",";
        }
        out += "k:" + tt_json_str(items[i].first) + "=" + tt_canon(*items[i].second);
    }
    return out;
}

std::string tt_canon(const ValueElement &e) {
    if (e.is_null()) {
        return "null";
    }
    if (e.is_bool()) {
        return e.as_bool() ? "true" : "false";
    }
    if (e.is_int()) {
        return "i:" + std::to_string(e.as_int());
    }
    if (e.is_uint()) {
        return "i:" + std::to_string(e.as_uint());
    }
    if (e.is_real()) {
        const double d = e.as_real();
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return "f:" + tt_hex64(bits);
    }
    if (e.is_string()) {
        return "s:" + tt_json_str(e.as_string());
    }
    if (e.is_array()) {
        std::string out = "[";
        const ValueElement::Array &arr = e.as_array();
        for (size_t i = 0; i < arr.size(); i++) {
            if (i != 0u) {
                out += ",";
            }
            out += tt_canon(arr[i]);
        }
        return out + "]";
    }
    const ToolParams *inner = e.as_object();
    return "{" + (inner != nullptr ? tt_canon_object_entries(*inner) : std::string()) + "}";
}

std::string tt_canon_object(const ToolParams &p) {
    return "{" + tt_canon_object_entries(p) + "}";
}

std::string tt_str(kimix::string_view sv) { return std::string(sv.data(), sv.size()); }

// ── Probe tool classes for the ToolRegistry tests ───────────────────────────
// Registered at static-init through the public macro, exactly like a real tool.
struct ProbeToolA : Tool {
    using Tool::Tool;
    void operator()(ToolParams const *parameters) override { last_parameters = parameters; }
    ToolParams const *last_parameters = nullptr;
};

struct ProbeToolB : ProbeToolA {
    using ProbeToolA::ProbeToolA;
};

// Second registration of the SAME registry name: the registry must replace the
// entry in place (position of the first registration, last content wins).
struct ProbeToolSequel : ProbeToolA {
    using ProbeToolA::ProbeToolA;
};

KIMIX_REGISTER_TOOL_NAMED(ProbeToolA, "TTProbeAlpha", "probe alpha (first)",
                          R"JSON({"type":"object"})JSON");
KIMIX_REGISTER_TOOL_NAMED(ProbeToolB, "TTProbeBeta", "probe beta",
                          R"JSON({"type":"object"})JSON");
KIMIX_REGISTER_TOOL_NAMED(ProbeToolSequel, "TTProbeAlpha", "probe alpha (second)",
                          R"JSON({"type":"object"})JSON");

// The ToolParams half of the golden vectors (the utf8/line-stream half belongs
// to test_tool_types.cpp).
#define KIMIX_TT_GOLDEN_NO_LINE_VECTORS 1
#include "tool_types_goldens.inc"

// Deserialize one golden text into `p` (false + message on failure).
bool tt_parse(kimix::string_view text, ToolParams &p, kimix::string &err) {
    return p.try_deserialize(kimix::span<char const>(text.data(), text.size()), err);
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "value_element_construction"_test = [] {
        ValueElement n;
        expect(n.is_null()) << "default is JSON null";
        expect(!n.is_bool() && !n.is_int() && !n.is_uint() && !n.is_real() &&
               !n.is_string() && !n.is_array() && !n.is_object());

        ValueElement b = ValueElement::make_bool(true);
        expect(b.is_bool());
        expect(eq(b.as_bool(), true));
        expect(std::holds_alternative<bool>(b.data()));

        ValueElement i = ValueElement::make_int(-42);
        expect(i.is_int());
        expect(eq(i.as_int(), int64_t(-42)));
        expect(!i.is_uint() && !i.is_real());

        ValueElement u = ValueElement::make_uint(7u);
        expect(u.is_uint());
        expect(eq(u.as_uint(), uint64_t(7)));

        ValueElement d = ValueElement::make_real(2.5);
        expect(d.is_real());
        expect(eq(d.as_real(), 2.5));

        ValueElement s = ValueElement::make_string(kimix::string("hi"));
        expect(s.is_string());
        expect(eq(s.as_string(), kimix::string("hi")));

        ValueElement::Array arr = {ValueElement::make_int(1),
                                   ValueElement::make_string(kimix::string("x"))};
        ValueElement a = ValueElement::make_array(std::move(arr));
        expect(a.is_array());
        expect(eq(a.as_array().size(), size_t(2)));
        expect(eq(a.as_array()[0].as_int(), int64_t(1)));

        kimix::shared_ptr<ToolParams> obj(new ToolParams());
        obj->values["k"] = ValueElement::make_int(9);
        ValueElement o = ValueElement::make_object(obj);
        expect(o.is_object());
        expect(o.as_object() != nullptr);
        expect(eq(o.as_object()->values.at(kimix::string("k")).as_int(),
                  int64_t(9)));
        const ValueElement &co = o;
        expect(co.as_object() != nullptr);
        expect(eq(co.as_object()->values.size(), size_t(1)));

        // as_object is null for non-object alternatives.
        expect(i.as_object() == nullptr);
        expect(a.as_object() == nullptr);
    };

    "scalar_round_trip"_test = [] {
        ToolParams p;
        p.values["bool_true"] = ValueElement::make_bool(true);
        p.values["bool_false"] = ValueElement::make_bool(false);
        p.values["int_zero"] = ValueElement::make_int(0);
        p.values["int_neg"] = ValueElement::make_int(-1);
        p.values["int_min"] = ValueElement::make_int(std::numeric_limits<int64_t>::min());
        p.values["int_max"] = ValueElement::make_int(std::numeric_limits<int64_t>::max());
        p.values["uint_zero"] = ValueElement::make_uint(0);
        p.values["uint_max"] = ValueElement::make_uint(std::numeric_limits<uint64_t>::max());
        p.values["real_zero"] = ValueElement::make_real(0.0);
        p.values["real_neg_zero"] = ValueElement::make_real(-0.0);
        p.values["real_one"] = ValueElement::make_real(1.0);
        p.values["real_pi"] = ValueElement::make_real(3.14);
        p.values["real_big"] = ValueElement::make_real(1e300);
        p.values["null"] = ValueElement::make_null();

        ToolParams q = round_trip(p);
        expect(veq_objs(p, q));

        // Distinct real sign must survive the round trip.
        expect(std::signbit(q.get("real_neg_zero")->as_real()))
            << "-0.0 keeps its sign bit";
        // Number discrimination: INT64_MAX stays int, UINT64_MAX stays uint,
        // small uints normalize to int (JSON has no unsigned literal), and
        // 1.0 stays real while 1 stays int.
        expect(q.get("int_max")->is_int());
        expect(q.get("uint_max")->is_uint());
        expect(q.get("uint_zero")->is_int()) << "0 parses as a signed int";
        expect(q.get("real_one")->is_real());
        expect(q.get("real_one")->as_real() == 1.0);
        expect(veq(p.values.at(kimix::string("real_one")),
                   q.values.at(kimix::string("real_one"))));
    };

    "string_round_trip"_test = [] {
        ToolParams p;
        p.values["empty"] = ValueElement::make_string(kimix::string());
        p.values["ascii"] =
            ValueElement::make_string(kimix::string("hello world"));
        p.values["escapes"] = ValueElement::make_string(
            kimix::string("quote \" backslash \\ tab \t newline \n"));
        p.values["utf8"] = ValueElement::make_string(kimix::string("你好"));
        p.values["emoji"] =
            ValueElement::make_string(kimix::string("\xF0\x9F\x98\x80"));
        p.values["nul"] = ValueElement::make_string(kimix::string("a\0b", 3));

        kimix::vector<char> buf;
        p.serialize(buf);
        const kimix::string json(buf.data(), buf.size());
        // Embedded NUL must be escaped, not emitted raw.
        expect(json.find('\0') == kimix::string::npos);
        expect(json.find("\\u0000") != kimix::string::npos);

        ToolParams q = round_trip(p);
        expect(veq_objs(p, q));
        expect(eq(q.get("nul")->as_string(), kimix::string("a\0b", 3)));
        expect(eq(q.get("utf8")->as_string(), kimix::string("你好")));
    };

    "nested_structures"_test = [] {
        ToolParams p;

        // Object inside object, 3+ levels.
        auto level2 = std::make_shared<ToolParams>();
        level2->values["deep"] = ValueElement::make_int(42);
        level2->values["deep_str"] =
            ValueElement::make_string(kimix::string("bottom"));
        auto level1 = std::make_shared<ToolParams>();
        level1->values["child"] = ValueElement::make_object(level2);
        p.values["root"] = ValueElement::make_object(level1);

        // Array of scalars.
        ValueElement::Array scalars = {
            ValueElement::make_int(1), ValueElement::make_real(2.5),
            ValueElement::make_string(kimix::string("three")),
            ValueElement::make_bool(true), ValueElement::make_null()};
        p.values["scalars"] = ValueElement::make_array(std::move(scalars));

        // Array of objects ("array of ToolParams").
        ValueElement::Array items;
        auto o1 = std::make_shared<ToolParams>();
        o1->values["id"] = ValueElement::make_int(1);
        auto o2 = std::make_shared<ToolParams>();
        o2->values["id"] = ValueElement::make_int(2);
        o2->values["name"] = ValueElement::make_string(kimix::string("two"));
        items.push_back(ValueElement::make_object(o1));
        items.push_back(ValueElement::make_object(o2));
        p.values["items"] = ValueElement::make_array(std::move(items));

        // Array of arrays inside an object (mixed map).
        ValueElement::Array row1 = {ValueElement::make_int(1),
                                    ValueElement::make_int(2)};
        ValueElement::Array row2 = {ValueElement::make_int(3),
                                    ValueElement::make_int(4)};
        ValueElement::Array matrix;
        matrix.push_back(ValueElement::make_array(std::move(row1)));
        matrix.push_back(ValueElement::make_array(std::move(row2)));
        p.values["matrix"] = ValueElement::make_array(std::move(matrix));

        ToolParams q = round_trip(p);
        expect(veq_objs(p, q));

        // Structural spot checks on the parsed copy.
        const ToolParams *root_obj = q.get("root")->as_object();
        expect(root_obj != nullptr);
        const ToolParams *child_obj = root_obj->get("child")->as_object();
        expect(child_obj != nullptr);
        expect(eq(child_obj->get("deep")->as_int(), int64_t(42)));

        const ValueElement::Array &items_arr = q.get("items")->as_array();
        expect(eq(items_arr.size(), size_t(2)));
        expect(eq(items_arr[1].as_object()->get("id")->as_int(), int64_t(2)));
        expect(eq(items_arr[1].as_object()->get("name")->as_string(),
                  kimix::string("two")));
    };

    "empty_object_array_round_trip"_test = [] {
        ToolParams p;
        expect(eq(to_json(p), kimix::string("{}"))) << "empty object -> {}";
        ToolParams q = round_trip(p);
        expect(q.values.empty());

        ToolParams p2;
        p2.values["a"] = ValueElement::make_array(ValueElement::Array{});
        expect(eq(to_json(p2), kimix::string("{\"a\":[]}")))
            << "empty array serializes";
        ToolParams q2 = round_trip(p2);
        expect(eq(q2.get("a")->as_array().size(), size_t(0)));
    };

    "deserialize_errors"_test = [] {
        // deserialize() never throws (kimix is built with
        // kimix_enable_exception=false): malformed input is reported with
        // `false` plus a descriptive message instead of std::runtime_error.
        auto parse_fails = [](const char *text, kimix::string &error) {
            ToolParams p;
            error.clear();
            const bool ok = p.deserialize(
                kimix::span<char const>(text, std::char_traits<char>::length(text)),
                &error);
            return !ok;
        };

        kimix::string err;
        // Malformed JSON.
        expect(parse_fails("{", err));
        expect(parse_fails("{\"a\":}", err));
        expect(parse_fails("{\"a\":1} trailing", err));

        // Non-object roots.
        expect(parse_fails("[1,2]", err));
        expect(parse_fails("\"str\"", err));
        expect(parse_fails("42", err));
        expect(parse_fails("null", err));
        expect(!err.empty()) << "a descriptive message is reported";

        // Empty span.
        expect(parse_fails("", err));
        expect(!err.empty()) << "empty span reports an error";

        // Valid object parses without failing.
        ToolParams p;
        const kimix::string good = "{\"ok\":true}";
        expect(p.deserialize(
            kimix::span<char const>(good.data(), good.size()), &err));
        expect(err.empty()) << "error cleared on success";
        expect(!p.values.empty());
    };

    "try_deserialize"_test = [] {
        ToolParams p;
        kimix::string err;
        const kimix::string good = "{\"a\":1}";
        expect(p.try_deserialize(
            kimix::span<char const>(good.data(), good.size()), err));
        expect(err.empty()) << "error cleared on success";
        expect(eq(p.get("a")->as_int(), int64_t(1)));

        const kimix::string bad = "[1,2]";
        expect(!p.try_deserialize(
            kimix::span<char const>(bad.data(), bad.size()), err));
        expect(!err.empty());
        expect(err.find("root must be a JSON object") != kimix::string::npos)
            << "descriptive non-object message";

        expect(!p.try_deserialize(kimix::span<char const>(), err));
        expect(!err.empty()) << "empty span reports an error";
    };

    "round_trip_determinism"_test = [] {
        // Byte-exact single-key golden.
        ToolParams p;
        ValueElement::Array arr = {ValueElement::make_int(1),
                                   ValueElement::make_int(2),
                                   ValueElement::make_int(3)};
        p.values["k"] = ValueElement::make_array(std::move(arr));
        expect(eq(to_json(p), kimix::string("{\"k\":[1,2,3]}")));

        // Complex document: serialize -> deserialize -> serialize yields the
        // same compact bytes (deterministic writer + preserved key order).
        ToolParams c;
        c.values["a"] = ValueElement::make_int(1);
        ValueElement::Array mix = {
            ValueElement::make_bool(true), ValueElement::make_null(),
            ValueElement::make_object([] {
                auto inner = std::make_shared<ToolParams>();
                inner->values["c"] = ValueElement::make_string(kimix::string("x"));
                inner->values["d"] = ValueElement::make_int(2);
                return inner;
            }())};
        c.values["b"] = ValueElement::make_array(std::move(mix));

        ToolParams q = round_trip(c);
        expect(veq_objs(c, q));
        expect(eq(to_json(c), to_json(q))) << "compact writer determinism";
    };

    "tool_base"_test = [] {
        Session s;
        bool dtor_ran = false;

        struct dummy_tool : Tool {
            using Tool::Tool;
            void operator()(ToolParams const *parameters) override {
                seen = parameters;
            }
            ~dummy_tool() override {
                if (flag != nullptr) {
                    *flag = true;
                }
            }
            ToolParams const *seen = nullptr;
            bool *flag = nullptr;
        };

        dummy_tool t(&s);
        expect(t.session() == &s) << "session() returns the constructor arg";

        ToolParams params;
        params.values["x"] = ValueElement::make_int(1);
        Tool *base = &t;
        base->operator()(&params);
        expect(t.seen == &params) << "virtual dispatch through Tool*";

        // Deleting through the base pointer runs the derived destructor.
        Tool *heap = new dummy_tool(&s);
        static_cast<dummy_tool *>(heap)->flag = &dtor_ran;
        dtor_ran = false;
        delete heap;
        expect(dtor_ran) << "virtual destructor runs through Tool*";

        // Null parameters are allowed by the base contract.
        base->operator()(nullptr);
        expect(t.seen == nullptr);
    };

    "map_helpers"_test = [] {
        ToolParams p;
        p.values["alpha"] = ValueElement::make_int(1);
        expect(p.contains("alpha"));
        expect(!p.contains("beta"));

        ValueElement *a = p.get("alpha");
        expect(a != nullptr);
        expect(eq(a->as_int(), int64_t(1)));
        expect(p.get("beta") == nullptr);

        p["gamma"] = ValueElement::make_string(kimix::string("g"));
        expect(p.contains("gamma"));
        expect(eq(p.get("gamma")->as_string(), kimix::string("g")));

        const ToolParams &cp = p;
        expect(cp.contains("alpha"));
        expect(cp.get("alpha") != nullptr);
        expect(eq(cp.get("alpha")->as_int(), int64_t(1)));
        expect(cp.get("nope") == nullptr);

        p.values.erase(kimix::string("gamma"));
        expect(!p.contains("gamma"));
        expect(p.get("gamma") == nullptr);

        // operator[] inserts a null ValueElement for a missing key.
        ValueElement &fresh = p["fresh"];
        expect(fresh.is_null());
        expect(p.contains("fresh"));
    };

    // ── Fuzzy alias matching (ToolParams::alias_map + param_alias) ───────────
    // A tool's arguments are produced by an LLM, so a wrong-but-reasonable name
    // ("command" for `cmd`) must resolve too; the canonical name always wins and
    // an undeclared name is still rejected. See the tool.h header comment.
    "alias_map_records_declarations"_test = [] {
        ToolParams p;
        p.values["cmd"] = ValueElement::make_string(kimix::string("ls"));
        p.add_alias("cmd", "command cmdline");
        expect(eq(p.alias_map.size(), size_t(1)));
        expect(eq(p.alias_map.at(kimix::string("cmd")),
                  kimix::string("command cmdline")));
        // The canonical key is returned exactly as sent - no alias involved.
        expect(eq(p.get("cmd")->as_string(), kimix::string("ls")));
    };

    "alias_used_only_when_canonical_is_absent"_test = [] {
        ToolParams p;
        p.values["command"] = ValueElement::make_string(kimix::string("ls"));
        p.add_alias("cmd", "command");
        const ToolParams &cp = p;
        expect(cp.get("cmd") != nullptr) << "declared alias resolves";
        expect(eq(cp.get("cmd")->as_string(), kimix::string("ls")));
        expect(cp.contains("cmd"));
        // get_exact()/contains_exact() never consult the alias table.
        expect(cp.get_exact("cmd") == nullptr);
        expect(!cp.contains_exact("cmd"));
        expect(cp.contains_exact("command"));
        expect(cp.get("nope") == nullptr) << "undeclared name is not matched";
    };

    "alias_canonical_wins_over_alias"_test = [] {
        ToolParams p;
        p.values["cmd"] = ValueElement::make_string(kimix::string("canonical"));
        p.values["command"] = ValueElement::make_string(kimix::string("aliased"));
        p.add_alias("cmd", "command");
        expect(eq(p.get("cmd")->as_string(), kimix::string("canonical")));
    };

    "alias_matching_folds_case_and_separators"_test = [] {
        ToolParams p;
        p.values["Command-Line"] = ValueElement::make_string(kimix::string("folded"));
        p.add_alias("cmd", "command_line");
        expect(p.get("cmd") != nullptr);
        expect(eq(p.get("cmd")->as_string(), kimix::string("folded")));
    };

    "alias_exact_name_beats_folded_match"_test = [] {
        ToolParams p;
        p.values["command-line"] = ValueElement::make_string(kimix::string("folded"));
        p.values["command_line"] = ValueElement::make_string(kimix::string("exact"));
        p.add_alias("cmd", "command_line");
        expect(eq(p.get("cmd")->as_string(), kimix::string("exact")));
    };

    "alias_table_merges_and_is_idempotent"_test = [] {
        ToolParams p;
        p.add_alias("cmd", "command");
        p.add_alias("cmd", "command cmdline"); // dup skipped, new name appended
        p.add_alias("cmd", "COMMAND"); // folded duplicate skipped
        expect(eq(p.alias_map.at(kimix::string("cmd")),
                  kimix::string("command cmdline")));
        expect(eq(p.alias_map.size(), size_t(1)));
    };

    "alias_json_null_is_not_a_match"_test = [] {
        ToolParams p;
        p.values["command"] = ValueElement::make_null();
        p.add_alias("cmd", "command");
        expect(p.get("cmd") == nullptr);
    };

    "alias_with_aliases_copies_and_installs"_test = [] {
        static const param_alias decls[] = {
            {"cmd", "command"},
            {"timeout", "timeout_seconds"},
        };
        ToolParams src;
        src.values["command"] = ValueElement::make_string(kimix::string("ls"));
        src.values["timeout_seconds"] = ValueElement::make_int(5);
        const ToolParams resolved = ToolParams::with_aliases(&src, decls);
        expect(eq(resolved.alias_map.size(), size_t(2)));
        expect(eq(resolved.get("cmd")->as_string(), kimix::string("ls")));
        expect(eq(resolved.get("timeout")->as_int(), int64_t(5)));
        // The source object is untouched - that is what makes the parse-entry
        // pattern safe for a `const ToolParams *` parameter.
        expect(src.alias_map.empty());
        expect(src.get("cmd") == nullptr);
        // A null input yields an empty object (still nothing to resolve).
        const ToolParams empty = ToolParams::with_aliases(nullptr, decls);
        expect(empty.values.empty());
        expect(empty.get("cmd") == nullptr);
    };

    "alias_map_is_never_serialized"_test = [] {
        ToolParams p;
        p.values["cmd"] = ValueElement::make_string(kimix::string("ls"));
        p.add_alias("cmd", "command");
        kimix::vector<char> out;
        kimix::string error;
        expect(p.serialize(out, &error)) << "serialize succeeds";
        const kimix::string_view text(out.data(), out.size());
        expect(text == kimix::string_view("{\"cmd\":\"ls\"}"))
            << "alias_map is a side-table, not part of the payload";
    };

    // ── Resolution order / JSON null / get_exact (README contract) ───────────
    "alias_resolution_order_and_nulls"_test = [] {
        // (1) canonical key present and non-null -> always wins.
        // (2) canonical key present but JSON null -> the declared alias is used.
        ToolParams p;
        p.values["cmd"] = ValueElement::make_null();
        p.values["command"] = ValueElement::make_string(kimix::string("ls"));
        p.add_alias("cmd", "command cmdline");
        expect(p.get("cmd") != nullptr);
        expect(eq(p.get("cmd")->as_string(), kimix::string("ls")))
            << "a null canonical falls through to the alias";
        // ... but get_exact still reports the null that was actually sent.
        expect(p.get_exact("cmd") != nullptr);
        expect(p.get_exact("cmd")->is_null());
        expect(p.contains_exact("cmd"));

        // (3) canonical null and no alias present: the null element itself is
        //     returned (contains() is true) - documented behaviour.
        ToolParams q;
        q.values["cmd"] = ValueElement::make_null();
        q.add_alias("cmd", "command");
        expect(q.get("cmd") != nullptr);
        expect(q.get("cmd")->is_null());
        expect(q.contains("cmd"));
        expect(!q.contains("command"))
            << "the alias table is keyed by the canonical name: asking for "
               "\"command\" is a plain absent lookup";
        expect(q.get("command") == nullptr);
        expect(!q.contains_exact("command"));

        // (4) the FIRST declared alias that is present wins (declaration order).
        ToolParams r;
        r.values["cmdline"] = ValueElement::make_string(kimix::string("second"));
        r.values["command"] = ValueElement::make_string(kimix::string("first"));
        r.add_alias("cmd", "command cmdline");
        expect(eq(r.get("cmd")->as_string(), kimix::string("first")));

        // (5) the folded pass is deterministic: among several keys that only
        //     match modulo case/'_'/'-' the lexicographically smallest wins.
        ToolParams f;
        f.values["Command_Line"] = ValueElement::make_string(kimix::string("b"));
        f.values["COMMAND-LINE"] = ValueElement::make_string(kimix::string("a"));
        f.add_alias("cmd", "commandline");
        expect(eq(f.get("cmd")->as_string(), kimix::string("a")))
            << "hash order must not decide the result";

        // (6) every documented separator splits the alternates list.
        ToolParams s2;
        s2.values["CommandLine"] = ValueElement::make_string(kimix::string("v"));
        s2.add_alias("cmd", "command,cmdline|command_line;shell_command\tshell");
        expect(s2.get("cmd") != nullptr) << "folded match across separators";
        expect(eq(s2.get("cmd")->as_string(), kimix::string("v")));

        // (7) empty canonical / empty alternates are no-ops.
        ToolParams e;
        e.add_alias("", "command");
        e.add_alias("cmd", "");
        expect(e.alias_map.empty());

        // (8) with_aliases() inherits an already installed table.
        ToolParams src;
        src.values["command"] = ValueElement::make_string(kimix::string("ls"));
        src.add_alias("timeout", "timeout_seconds");
        static const param_alias decls[] = {{"cmd", "command"}};
        const ToolParams resolved = ToolParams::with_aliases(&src, decls);
        expect(eq(resolved.alias_map.size(), size_t(2))) << "source table inherited";
        expect(eq(resolved.get("cmd")->as_string(), kimix::string("ls")));
    };

    // ── JSON fidelity goldens (CPython json module) ──────────────────────────
    // See scripts/gen_tool_types_goldens.py: every vector is classified by the
    // generator itself, and the canonical rendering (i:/f:/s: tags, sorted
    // object keys, IEEE-754 bit patterns) is shared by both sides.
    "json_parity_golden"_test = [] {
        size_t failures = 0;
        for (const tt_json_golden &g : k_tt_json_golden) {
            ToolParams p;
            kimix::string err;
            const bool ok = tt_parse(g.text, p, err);
            const std::string canon = tt_canon_object(p);
            bool bad = !ok || canon != tt_str(g.canon);
            // The value must also survive serialize -> parse unchanged.
            kimix::vector<char> buf;
            kimix::string serr;
            ToolParams q;
            if (!bad) {
                const bool rt =
                    p.serialize(buf, &serr) &&
                    q.try_deserialize(kimix::span<char const>(buf.data(), buf.size()), serr);
                bad = !rt || tt_canon_object(q) != canon;
            }
            if (bad) {
                failures++;
                if (failures <= 8) {
                    expect(false) << "json " << tt_str(g.text) << " -> " << canon << " want "
                                  << tt_str(g.canon) << " (err=" << tt_str(err) << ")";
                }
            }
        }
        expect(eq(failures, size_t(0)))
            << "CPython json.loads parity over "
            << sizeof(k_tt_json_golden) / sizeof(k_tt_json_golden[0]) << " vectors";
        expect(sizeof(k_tt_json_golden) / sizeof(k_tt_json_golden[0]) >= 60u)
            << "the generated corpus must stay large";
    };

    "json_both_reject_golden"_test = [] {
        for (const tt_json_reject_golden &g : k_tt_json_reject_golden) {
            ToolParams p;
            kimix::string err;
            expect(!tt_parse(g.text, p, err))
                << "must reject (CPython raises ValueError): " << tt_str(g.text);
            expect(!err.empty()) << "a message is reported";
        }
        expect(sizeof(k_tt_json_reject_golden) / sizeof(k_tt_json_reject_golden[0]) >= 38u);
    };

    // Documented divergences: CPython's json module accepts these, the native
    // value model cannot hold them (non-object root; inf/nan literal; lone
    // surrogate escape).  Asserted so a future change is noticed, not silently
    // accepted - a caller that needs these must stay on the Python mirror.
    "json_reference_values_native_rejects"_test = [] {
        size_t n = 0;
        for (const tt_json_reference_golden &g : k_tt_json_nonobject_golden) {
            ToolParams p;
            kimix::string err;
            expect(!tt_parse(g.text, p, err))
                << "non-object root is rejected by contract (CPython: " << tt_str(g.ref) << ")";
            n++;
        }
        for (const tt_json_reference_golden &g : k_tt_json_unrepresentable_golden) {
            ToolParams p;
            kimix::string err;
            expect(!tt_parse(g.text, p, err))
                << "unrepresentable value (CPython: " << tt_str(g.ref) << ")";
            n++;
        }
        expect(n > 0u) << "the divergence arrays are not empty";
        expect(sizeof(k_tt_json_nonobject_golden) / sizeof(k_tt_json_nonobject_golden[0]) >= 6u);
        expect(sizeof(k_tt_json_unrepresentable_golden) /
                   sizeof(k_tt_json_unrepresentable_golden[0]) >=
               15u);
    };

    // yyjson's documented number policy reads an integer outside
    // [INT64_MIN, UINT64_MAX] as a DOUBLE, so those payloads silently lose
    // exactness where CPython keeps an arbitrary-precision int.
    "json_lossy_integer_golden"_test = [] {
        for (const tt_json_lossy_golden &g : k_tt_json_lossy_number_golden) {
            ToolParams p;
            kimix::string err;
            const bool ok = tt_parse(g.text, p, err);
            const std::string canon = tt_canon_object(p);
            expect(ok) << "native reader accepts it as a double: " << tt_str(g.text)
                       << " (err=" << tt_str(err) << ")";
            expect(canon.find("f:") != std::string::npos)
                << "the out-of-range integer became a real: " << canon;
            expect(canon != tt_str(g.canon))
                << "CPython keeps it exact (" << tt_str(g.ref) << "), the native reader does not";
            expect(sizeof(k_tt_json_lossy_number_golden) /
                       sizeof(k_tt_json_lossy_number_golden[0]) >=
                   6u);
        }
    };

    // ── Serialization fidelity (the properties the JSON goldens cannot pin) ──
    "json_serialize_fidelity"_test = [] {
        // Exact integer text at both ends of the representable range.
        ToolParams p;
        p.values["i_min"] = ValueElement::make_int(std::numeric_limits<int64_t>::min());
        p.values["i_max"] = ValueElement::make_int(std::numeric_limits<int64_t>::max());
        p.values["u_max"] = ValueElement::make_uint(std::numeric_limits<uint64_t>::max());
        kimix::vector<char> buf;
        expect(p.serialize(buf));
        const std::string text(buf.data(), buf.size());
        expect(text.find("-9223372036854775808") != std::string::npos);
        expect(text.find("9223372036854775807") != std::string::npos);
        expect(text.find("18446744073709551615") != std::string::npos)
            << "uint64 max keeps its unsigned form";
        ToolParams q = round_trip(p);
        expect(q.get("u_max")->is_uint()) << "uint64 max stays unsigned";
        expect(eq(q.get("u_max")->as_uint(), std::numeric_limits<uint64_t>::max()));
        expect(q.get("i_min")->is_int());
        expect(eq(q.get("i_min")->as_int(), std::numeric_limits<int64_t>::min()));

        // Doubles: the writer emits enough digits for a bit-exact round trip.
        const double k_doubles[] = {3.14,        0.1,     1e300,  5e-324,
                                    1.0 / 3.0,   -0.0,    1e-300, 1.7976931348623157e308,
                                    123456.789};
        for (double d : k_doubles) {
            ToolParams one;
            one.values["d"] = ValueElement::make_real(d);
            ToolParams back = round_trip(one);
            const ValueElement *got = back.get("d");
            expect(got != nullptr && got->is_real()) << "real survives";
            double g = got->as_real();
            expect(std::memcmp(&g, &d, sizeof(double)) == 0)
                << "bit-exact double round trip for " << d;
        }

        // Deep nesting (64 levels) survives both directions.
        auto deep = std::make_shared<ToolParams>();
        for (int i = 0; i < 64; i++) {
            auto outer = std::make_shared<ToolParams>();
            outer->values["a"] = ValueElement::make_object(deep);
            deep = outer;
        }
        ToolParams dn;
        dn.values["root"] = ValueElement::make_object(deep);
        ToolParams dn2 = round_trip(dn);
        const ValueElement *cur = dn2.get("root");
        int depth = 0;
        while (cur != nullptr && cur->is_object()) {
            cur = cur->as_object()->get("a");
            depth++;
        }
        expect(eq(depth, 65)) << "64 nested objects + the leaf";

        // A key carrying an embedded NUL must not be truncated by the writer
        // (yyjson_mut_obj_add_val() takes a NUL-terminated key).
        ToolParams nul_key;
        const kimix::string key("a\0b", 3);
        nul_key.values[key] = ValueElement::make_int(1);
        kimix::vector<char> nbuf;
        expect(nul_key.serialize(nbuf));
        const kimix::string njson(nbuf.data(), nbuf.size());
        expect(njson.find("a\\u0000b") != kimix::string::npos)
            << "the key keeps its embedded NUL (escaped): " << std::string(njson.data(), njson.size());
        ToolParams nq;
        expect(nq.deserialize(kimix::span<char const>(nbuf.data(), nbuf.size())));
        expect(eq(nq.values.size(), size_t(1)));
        expect(nq.get_exact(key) != nullptr) << "the key round-trips whole";
        expect(eq(nq.get_exact(key)->as_int(), int64_t(1)));

        // An empty key is legal JSON and must survive too.
        ToolParams ek;
        ek.values[""] = ValueElement::make_int(7);
        ToolParams ek2 = round_trip(ek);
        expect(ek2.get_exact("") != nullptr);
        expect(eq(ek2.get_exact("")->as_int(), int64_t(7)));
    };

    // ── ToolRegistry (registration, lookup, factory) ─────────────────────────
    "tool_registry_registration_and_lookup"_test = [] {
        auto &reg = ToolRegistry::instance();

        const ToolMeta *alpha = reg.find("TTProbeAlpha");
        expect(alpha != nullptr) << "registered through KIMIX_REGISTER_TOOL_NAMED";
        expect(eq(alpha->description, kimix::string("probe alpha (second)")))
            << "a duplicate name replaces the earlier entry (last wins)";
        expect(eq(alpha->parameters_json, kimix::string("{\"type\":\"object\"}")))
            << "the schema string is stored verbatim";

        // The replacement keeps the slot of the FIRST registration, so the
        // relative order of all() is the insertion order of first appearance.
        kimix::vector<ToolMeta> all = reg.all();
        size_t ia = SIZE_MAX;
        size_t ib = SIZE_MAX;
        for (size_t i = 0; i < all.size(); i++) {
            if (all[i].name == "TTProbeAlpha") {
                ia = i;
            }
            if (all[i].name == "TTProbeBeta") {
                ib = i;
            }
        }
        expect(ia != SIZE_MAX && ib != SIZE_MAX);
        expect(ia < ib) << "the replaced entry keeps its original position";
        expect(eq(reg.size(), all.size())) << "size() matches all()";

        // Exact lookup is case-sensitive, find_ci is not (ASCII).
        expect(reg.find("TTProbeAlpha") != nullptr);
        expect(reg.find("ttprobealpha") == nullptr) << "find() is exact";
        expect(reg.find_ci("ttprobealpha") != nullptr);
        expect(eq(reg.find_ci("TTPROBEBETA")->name, kimix::string("TTProbeBeta")));
        expect(reg.find("TTProbeAlphaX") == nullptr);
        expect(reg.find_ci("TTProbeAlphaX") == nullptr);
        expect(reg.find_ci("TTProbeAlph") == nullptr) << "same length is required";
        expect(reg.find("") == nullptr);
        expect(reg.find_ci("") == nullptr);
    };

    "tool_registry_create_and_null_session"_test = [] {
        auto &reg = ToolRegistry::instance();
        Session s;
        s.work_dir = "C:/work";

        auto a = reg.create("TTProbeAlpha", &s);
        expect(a != nullptr) << "exact name";
        expect(a->session() == &s);

        auto b = reg.create("ttprobebeta", &s);
        expect(b != nullptr) << "case-insensitive create";

        // The null-session factory path: a tool constructed for a session-less
        // caller must exist and report a null session (tools that need OS
        // access check it before touching it).
        auto c = reg.create("TTProbeAlpha", nullptr);
        expect(c != nullptr);
        expect(c->session() == nullptr);

        expect(reg.create("TTProbeNope", &s) == nullptr);
        expect(reg.create("", &s) == nullptr);

        // A fresh instance per call, and it is the registered class.
        auto d = reg.create("TTProbeAlpha", &s);
        expect(d != nullptr && a != nullptr && d.get() != a.get());
        ToolParams params;
        params.values["x"] = ValueElement::make_int(1);
        (*a)(&params);
        expect(static_cast<ProbeToolA *>(a.get())->last_parameters == &params)
              << "the factory built the registered class";
      };

      // ── agent_*.json coverage (acceptance criterion) ─────────────────────────
      // The KimiX agent role definitions (C:/dev/kimi-agent/src/kimix/agent_*.json:
      // boss, planner, readonly, subagent, worker) name the tools each role may
      // call.  Their union is the required tool surface of this library: every
      // entry must resolve to a registered C++ class with a description, a JSON
      // schema and a working factory.  This is the check for "implement all the
      // tools defined in agent_*.json, with the same behaviour as the Python
      // implementation" - the behaviour half is covered by the per-tool suites
      // (test_builtin_<tool>) and by python/tests/test_parity_*.py.
      "registry_covers_every_agent_json_tool"_test = [] {
          struct agent_tool_entry {
              const char *json_id; // "<python module>:<attr>" as written in the JSON
              const char *registry_name; // ToolRegistry key (C++ class name)
          };
          // Union of agent_boss.json / agent_planner.json / agent_readonly.json /
          // agent_subagent.json / agent_worker.json - 25 distinct tools.
          static const agent_tool_entry k_agent_tools[] = {
              {"kimi_cli.tools.file:read", "Read"},
              {"kimi_cli.tools.file:read_image", "ReadImage"},
              {"kimi_cli.tools.file:glob", "Glob"},
              {"kimi_cli.tools.file:grep", "Grep"},
              {"kimi_cli.tools.file:edit", "Edit"},
              {"kimi_cli.tools.file:write", "Write"},
              {"kimix.tools.web.fetch_url:fetch_url", "FetchUrl"},
              {"kimi_cli.tools.web:web_search", "WebSearch"},
              {"kimix.tools.note:WritePlan", "WritePlan"},
              {"kimix.tools.note:ReadPlan", "ReadPlan"},
              {"kimix.tools.note:EditPlan", "EditPlan"},
              {"kimix.tools.agent:subagent", "Subagent"},
              {"kimix.tools.agent:send_message", "SendMessage"},
              {"kimix.tools.agent:list_agents", "ListAgents"},
              {"kimix.tools.agent:interrupt_agent", "InterruptAgent"},
              {"kimix.tools.swarm:workflow", "Workflow"},
              {"kimi_cli.tools.todo:todo_write", "TodoWrite"},
              {"kimi_cli.tools.todo:todo_update", "TodoUpdate"},
              {"kimi_cli.tools.memory:retrieve", "Retrieve"},
              {"kimix.tools.context:compact", "Compact"},
              {"kimix.tools.file.bash:bash", "Bash"},
              {"kimix.tools.file.bash:pwsh", "Pwsh"},
              {"kimix.tools.file.run:Run", "Run"},
              {"kimix.tools.py:python", "Python"},
              {"kimix.tools.background:job_output", "JobOutput"},
          };
          const size_t expected = sizeof(k_agent_tools) / sizeof(k_agent_tools[0]);
          expect(eq(expected, size_t(25))) << "the agent JSON union has 25 tools";

          auto &reg = ToolRegistry::instance();
          Session s;
          s.work_dir = ".";
          size_t resolved = 0;
          for (const agent_tool_entry &e : k_agent_tools) {
              const ToolMeta *m = reg.find_ci(e.registry_name);
              expect(m != nullptr)
                  << "missing tool class for " << e.json_id << " (" << e.registry_name << ")";
              if (m == nullptr) {
                  continue;
              }
              ++resolved;
              expect(eq(m->name, kimix::string(e.registry_name)))
                  << "registry key is the class name";
              expect(!m->description.empty())
                  << e.registry_name << " has an LLM-facing description";
              expect(!m->parameters_json.empty())
                  << e.registry_name << " has a parameter schema";
              expect(m->parameters_json[0] == '{')
                  << e.registry_name << " publishes a JSON object schema";
              kimix::unique_ptr<Tool> instance = reg.create(e.registry_name, &s);
              expect(instance != nullptr) << e.registry_name << " is constructible";
              if (instance != nullptr) {
                  expect(instance->session() == &s);
              }
          }
          expect(eq(resolved, expected)) << "every agent_*.json tool resolved";

          // No strays: the registry must contain exactly the agent-facing tools
          // plus the two probe fixtures registered by this test file.
          for (const ToolMeta &m : reg.all()) {
              bool known = (m.name == "TTProbeAlpha" || m.name == "TTProbeBeta");
              for (const agent_tool_entry &e : k_agent_tools) {
                  if (m.name == e.registry_name) {
                      known = true;
                  }
              }
              expect(known) << "unexpected registry entry: " << m.name;
          }
      };

}
