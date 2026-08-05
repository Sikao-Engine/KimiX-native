// Test for yyjson (JSON library) using mimalloc as memory allocator.
// This test covers:
// - Parsing JSON strings with mimalloc-backed allocator
// - Writing JSON with mimalloc-backed allocator
// - Error handling with invalid JSON
// - Direct mi_malloc / mi_free usage (allocate and deallocate only)
// Allocator is passed at runtime via yyjson_alc (not compile-time YYJSON_CUSTOM_ALC).

#include "ut/ut.hpp"

#include "yyjson.h"

// mimalloc-backed yyjson allocator
#include <mimalloc.h>

static void *mimalloc_malloc(void *ctx, size_t size) {
    (void)ctx;
    return mi_malloc(size);
}

static void *mimalloc_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return mi_realloc(ptr, size);
}

static void mimalloc_free(void *ctx, void *ptr) {
    (void)ctx;
    mi_free(ptr);
}

static const yyjson_alc yyjson_mimalloc_alc = {
    mimalloc_malloc,
    mimalloc_realloc,
    mimalloc_free,
    NULL
};

#include <cstring>
#include <string>
#include <cmath>
#include <cstdlib>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "yyjson_read"_test = [] {
        const char *json = R"({
            "name": "KimixBase",
            "version": 1,
            "active": true,
            "pi": 3.14,
            "tags": ["test", "json"],
            "counts": [1, 2, 3]
        })";

        yyjson_doc *doc = yyjson_read_opts((char*)json, strlen(json), 0, &yyjson_mimalloc_alc, nullptr);
        expect(doc != nullptr) << "should parse valid JSON";
        if (!doc) return;

        yyjson_val *root = yyjson_doc_get_root(doc);
        expect(root != nullptr);
        expect(eq(yyjson_get_type(root), YYJSON_TYPE_OBJ));

        yyjson_val *name = yyjson_obj_get(root, "name");
        expect(name != nullptr);
        expect(eq(yyjson_get_type(name), YYJSON_TYPE_STR));
        expect(eq(strcmp(yyjson_get_str(name), "KimixBase"), 0));
        expect(eq(yyjson_get_len(name), 9u));

        yyjson_val *ver = yyjson_obj_get(root, "version");
        expect(ver != nullptr);
        expect(eq(yyjson_get_int(ver), 1));

        yyjson_val *active = yyjson_obj_get(root, "active");
        expect(active != nullptr);
        expect(yyjson_is_true(active));
        expect(!yyjson_is_false(active));

        yyjson_val *pi = yyjson_obj_get(root, "pi");
        expect(pi != nullptr);
        expect(lt(std::abs(yyjson_get_real(pi) - 3.14), 1e-10));

        yyjson_val *tags = yyjson_obj_get(root, "tags");
        expect(tags != nullptr);
        expect(eq(yyjson_get_type(tags), YYJSON_TYPE_ARR));
        yyjson_val *tag0 = yyjson_arr_get_first(tags);
        expect(tag0 != nullptr);
        expect(eq(strcmp(yyjson_get_str(tag0), "test"), 0));
        yyjson_val *tag1 = yyjson_arr_get(tags, 1);
        expect(tag1 != nullptr);
        expect(eq(strcmp(yyjson_get_str(tag1), "json"), 0));

        yyjson_val *counts = yyjson_obj_get(root, "counts");
        expect(counts != nullptr);
        expect(eq(yyjson_arr_size(counts), 3u));
        expect(eq(yyjson_get_int(yyjson_arr_get(counts, 0)), 1));
        expect(eq(yyjson_get_int(yyjson_arr_get(counts, 1)), 2));
        expect(eq(yyjson_get_int(yyjson_arr_get(counts, 2)), 3));

        yyjson_doc_free(doc);
    };

    "yyjson_write"_test = [] {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(&yyjson_mimalloc_alc);
        expect(doc != nullptr);

        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_str(doc, root, "hello", "world");
        yyjson_mut_obj_add_int(doc, root, "count", 42);
        yyjson_mut_obj_add_bool(doc, root, "flag", true);

        char *json = yyjson_mut_write(doc, 0, nullptr);
        expect(json != nullptr);

        yyjson_doc *verify = yyjson_read_opts(json, strlen(json), 0, &yyjson_mimalloc_alc, nullptr);
        expect(verify != nullptr);
        yyjson_val *vroot = yyjson_doc_get_root(verify);
        expect(eq(strcmp(yyjson_get_str(yyjson_obj_get(vroot, "hello")), "world"), 0));
        expect(eq(yyjson_get_int(yyjson_obj_get(vroot, "count")), 42));
        expect(yyjson_is_true(yyjson_obj_get(vroot, "flag")));

        free(json);
        yyjson_doc_free(verify);
        yyjson_mut_doc_free(doc);
    };

    "yyjson_mimalloc"_test = [] {
        // Directly allocate and deallocate via mi_malloc / mi_free — the only two APIs we use.
        void *p = mi_malloc(128);
        expect(p != nullptr) << "mi_malloc(128) should succeed";
        if (p != nullptr) {
            // Write a pattern to verify the memory is usable
            memset(p, 0xAB, 128);
            char *buf = (char*)p;
            bool ok = true;
            for (int i = 0; i < 128; i++) {
                if (buf[i] != (char)0xAB) { ok = false; break; }
            }
            expect(ok) << "mimalloc memory should be writable and readable";
            mi_free(p);
        }

        // Allocate via yyjson (which uses mi_malloc internally), then free via yyjson (which uses mi_free)
        const char *text = R"({"hello": "world"})";
        yyjson_doc *doc = yyjson_read_opts((char*)text, strlen(text), 0, &yyjson_mimalloc_alc, nullptr);
        expect(doc != nullptr) << "yyjson should parse JSON using mimalloc allocator";
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            expect(root != nullptr);
            yyjson_val *v = yyjson_obj_get(root, "hello");
            expect(v != nullptr);
            expect(eq(strcmp(yyjson_get_str(v), "world"), 0));
            yyjson_doc_free(doc);
        }

        // Allocate via yyjson mut doc (which uses mi_malloc), then free via yyjson mut doc (which uses mi_free)
        yyjson_mut_doc *mdoc = yyjson_mut_doc_new(&yyjson_mimalloc_alc);
        expect(mdoc != nullptr) << "yyjson mut doc should be created using mimalloc allocator";
        if (mdoc) {
            yyjson_mut_val *r = yyjson_mut_obj(mdoc);
            yyjson_mut_doc_set_root(mdoc, r);
            yyjson_mut_obj_add_str(mdoc, r, "key", "value");
            yyjson_mut_doc_free(mdoc);
        }
    };

    "yyjson_invalid"_test = [] {
        const char *bad = "{invalid json here}";
        yyjson_doc *doc = yyjson_read_opts((char*)bad, strlen(bad), 0, &yyjson_mimalloc_alc, nullptr);
        expect(doc == nullptr) << "invalid JSON should return null";

        const char *trailing = R"({"a":1} trailing garbage)";
        yyjson_read_err err;
        doc = yyjson_read_opts((char*)trailing, strlen(trailing), YYJSON_READ_NOFLAG, &yyjson_mimalloc_alc, &err);
        expect(doc == nullptr) << "trailing garbage should fail";
        expect(neq(err.code, YYJSON_READ_SUCCESS));
    };

    "yyjson_null"_test = [] {
        const char *json = R"({"data": null})";
        yyjson_doc *doc = yyjson_read_opts((char*)json, strlen(json), 0, &yyjson_mimalloc_alc, nullptr);
        expect(doc != nullptr);

        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *data = yyjson_obj_get(root, "data");
        expect(data != nullptr);
        expect(yyjson_is_null(data));

        yyjson_doc_free(doc);
    };
}
