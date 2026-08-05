// Test for src/runtime/json/json_store.h (plan 016).
// This test covers:
// - JsonStore load / deep-merge update / get (orjson OPT_INDENT_2 bytes)
// - keys() document order
// - save_atomic: file bytes + atomic tmp+rename (no leftover .tmp)
// - clear / invalid-input fallbacks
// - scan_notifications: ordering, ties keep input order, malformed lines

#include "ut/ut.hpp"
#include <runtime/json/json_store.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::json;

namespace {

kimix::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    kimix::string out;
    if (!in) {
        return out;
    }
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    out.assign(data.data(), data.size());
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "json_store_load_get_pretty"_test = [] {
        JsonStore store;
        store.load("{\"a\": 1, \"b\": [1, 2, 3], \"c\": {\"x\": \"y\"}}");
        kimix::string out;
        store.get(out);
        expect(eq(out, kimix::string(
            "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2,\n    3\n  ],\n"
            "  \"c\": {\n    \"x\": \"y\"\n  }\n}")));
        const auto keys = store.keys();
        expect(eq(keys.size(), size_t{3}));
        expect(eq(keys[0], kimix::string("a")));
        expect(eq(keys[1], kimix::string("b")));
        expect(eq(keys[2], kimix::string("c")));
    };

    "json_store_deep_merge"_test = [] {
        JsonStore store;
        store.load("{\"a\": 1, \"b\": {\"x\": 1, \"y\": 2}}");
        store.update("{\"a\": 2, \"b\": {\"y\": 3, \"z\": 4}, \"c\": 5}");
        kimix::string out;
        store.get(out);
        // Existing keys keep position; new keys append; nested objects merge.
        expect(eq(out, kimix::string(
            "{\n  \"a\": 2,\n  \"b\": {\n    \"x\": 1,\n    \"y\": 3,\n"
            "    \"z\": 4\n  },\n  \"c\": 5\n}")));
    };

    "json_store_empty_and_invalid"_test = [] {
        JsonStore store;
        kimix::string out;
        store.get(out); // empty store -> {}
        expect(eq(out, kimix::string("{}")));
        store.load("not json");
        store.get(out); // invalid -> {}
        expect(eq(out, kimix::string("{}")));
        store.load("{\"k\": 1}");
        store.update("not json"); // invalid update -> no change
        store.get(out);
        expect(out.find("\"k\": 1") != kimix::string::npos);
        store.clear();
        store.get(out);
        expect(eq(out, kimix::string("{}")));
        expect(store.loaded()); // clear keeps an empty document
    };

    "json_store_save_atomic"_test = [] {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "kimix_json_store_test";
        std::filesystem::create_directories(dir);
        const std::filesystem::path path = dir / "task.json";
        std::filesystem::remove(path);
        JsonStore store;
        store.load("{\"spec\": {\"id\": \"t1\"}, \"runtime\": {\"status\": \"running\"}}");
        kimix::string blob;
        expect(store.save_atomic(path.string(), blob));
        expect(eq(read_file(path), blob));
        expect(!std::filesystem::exists(path.string() + ".tmp"));
        // Round-trip: re-load the file bytes.
        JsonStore again;
        again.load(blob);
        kimix::string out2;
        again.get(out2);
        expect(eq(out2, blob));
        std::filesystem::remove_all(dir);
    };

    "json_store_number_formatting"_test = [] {
        JsonStore store;
        store.load("{\"i\": 42, \"f\": 1.5, \"big\": 1e20, \"neg\": -3, \"zero\": 0.0}");
        kimix::string out;
        store.get(out);
        expect(out.find("\"i\": 42") != kimix::string::npos);
        expect(out.find("\"f\": 1.5") != kimix::string::npos);
        expect(out.find("\"big\": 1e+20") != kimix::string::npos);
        expect(out.find("\"neg\": -3") != kimix::string::npos);
        expect(out.find("\"zero\": 0.0") != kimix::string::npos);
    };

    "scan_notifications_order_and_ties"_test = [] {
        kimix::vector<notification_row> rows;
        const char* jsonl =
            "{\"event\": {\"id\": \"a\", \"created_at\": 1.0}}\n"
            "{\"event\": {\"id\": \"b\", \"created_at\": 3.0}, \"delivery\": {\"sinks\": {}}}\n"
            "{\"event\": {\"id\": \"c\", \"created_at\": 2.0}}\n"
            "garbage line\n"
            "{\"event\": {\"id\": \"d\", \"created_at\": 3.0}}\n";
        scan_notifications(jsonl, 0, rows);
        expect(eq(rows.size(), size_t{4}));
        // 3.0 entries keep input order (stable sort); then 2.0, then 1.0.
        expect(eq(rows[0].id, kimix::string("b")));
        expect(eq(rows[1].id, kimix::string("d")));
        expect(eq(rows[2].id, kimix::string("c")));
        expect(eq(rows[3].id, kimix::string("a")));
        expect(!rows[0].delivery_json.empty());
        expect(rows[3].delivery_json.empty());
    };

    "scan_notifications_empty_and_missing_fields"_test = [] {
        kimix::vector<notification_row> rows;
        scan_notifications("", 0, rows);
        expect(rows.empty());
        scan_notifications("{\"event\": {\"id\": \"x\"}}\n{\"event\": {\"id\": \"y\", \"created_at\": 5}}\n",
                           0, rows);
        expect(eq(rows.size(), size_t{2}));
        expect(eq(rows[0].id, kimix::string("y"))); // created_at 5 > 0
        expect(rows[0].created_at > 4.9);
    };

    return 0;
}
