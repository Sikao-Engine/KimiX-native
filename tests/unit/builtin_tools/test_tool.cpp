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
#include "ut/ut.hpp"

#include "builtin_tools/tool.h"

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
        auto parse = [](const char *text) {
            ToolParams p;
            p.deserialize(kimix::span<char const>(
                text, std::char_traits<char>::length(text)));
            return p;
        };

        // Malformed JSON.
        expect(throws<std::runtime_error>([&] { parse("{"); }));
        expect(throws<std::runtime_error>([&] { parse("{\"a\":}"); }));
        expect(throws<std::runtime_error>([&] { parse("{\"a\":1} trailing"); }));

        // Non-object roots.
        expect(throws<std::runtime_error>([&] { parse("[1,2]"); }));
        expect(throws<std::runtime_error>([&] { parse("\"str\""); }));
        expect(throws<std::runtime_error>([&] { parse("42"); }));
        expect(throws<std::runtime_error>([&] { parse("null"); }));

        // Empty span.
        expect(throws<std::runtime_error>(
            [] { ToolParams p; p.deserialize(kimix::span<char const>()); }));

        // Valid object parses without throwing.
        expect(!parse("{\"ok\":true}").values.empty());
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
}
