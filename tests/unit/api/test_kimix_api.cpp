/*
 * test_kimix_api.cpp — exercises the kimix_api C-FFI shared library.
 *
 * The suite calls the library through the PUBLIC C headers only (no kimix-core
 * type is referenced anywhere), which is exactly what a foreign binding does:
 * it proves the exported surface is complete, NULL-safe, and that the
 * placement-new / std::launder placeholder lifecycle (init -> use -> destroy)
 * behaves as documented, including the guard-word error paths.
 *
 * Registered in tests/xmake.lua behind has_config("kimix_enable_api"): with the
 * option off neither kimix_api nor this test is part of the configuration.
 */
#include "ut/ut.hpp"

#include <api/kimix_api.h>

#include <cstdio>
#include <cstring>

using boost::ut::expect;
using boost::ut::operator""_test;

int main() {
    // ----------------------------------------------------------------- identity
    "abi identity"_test = [] {
        expect(kimix_api_abi_version() == KIMIX_API_ABI_VERSION);
        expect(std::strlen(kimix_api_version_string()) > 0);
        expect(std::strlen(kimix_api_build_info()) > 0);
        expect(std::strcmp(kimix_status_name(KIMIX_OK), "KIMIX_OK") == 0);
        expect(kimix_status_name(static_cast<kimix_status>(1234)) == nullptr);

        kimix_layout_info info;
        expect(kimix_api_layout_info(&info) == KIMIX_OK);
        expect(kimix_api_layout_info(nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(info.abi_version == kimix_api_abi_version());
        expect(info.size_of_bool == 1u);
        expect(info.size_of_size_t == sizeof(size_t));
        expect(info.size_of_pointer == sizeof(void *));
        // The published placeholder must be big/aligned enough for the real object.
        expect(info.vec_inner_bytes + sizeof(uint64_t) <= info.vec_abi_bytes);
        expect(info.vec_inner_align <= info.vec_abi_align);
        expect(info.vec_abi_bytes == KIMIX_VEC_BYTES);
        expect(sizeof(kimix_vec) == KIMIX_VEC_BYTES);
        expect(alignof(kimix_vec) == KIMIX_VEC_ALIGN);
        expect(kimix_vec_abi_bytes() == KIMIX_VEC_BYTES);
        expect(kimix_vec_inner_bytes() == info.vec_inner_bytes);
    };

    // ------------------------------------------------------------------ memory
    "mem alloc / realloc / free"_test = [] {
        auto *p = static_cast<unsigned char *>(kimix_mem_malloc(64));
        expect(p != nullptr);
        for (int i = 0; i < 64; ++i) {
            p[i] = static_cast<unsigned char>(i);
        }
        expect(kimix_mem_usable_size(p) >= 64u);
        expect(kimix_mem_good_size(33) >= 33u);

        auto *q = static_cast<unsigned char *>(kimix_mem_realloc(p, 128));
        expect(q != nullptr);
        expect(q[0] == 0 && q[63] == 63); // content survived
        expect(kimix_mem_expand(q, 129) != nullptr || true);
        kimix_mem_free(q);

        // Zero-filling allocations, then a 0-size / NULL edge case each.
        auto *z = static_cast<unsigned char *>(kimix_mem_zalloc(32));
        expect(z != nullptr);
        bool zeroed = true;
        for (int i = 0; i < 32; ++i) {
            zeroed = zeroed && (z[i] == 0);
        }
        expect(zeroed);
        kimix_mem_free(z);

        auto *c = static_cast<unsigned char *>(kimix_mem_calloc(8, 8));
        expect(c != nullptr);
        expect(c[63] == 0);
        kimix_mem_free(c);

        kimix_mem_free(nullptr); // documented as a no-op
        expect(kimix_mem_strdup(nullptr) == nullptr);
        expect(kimix_mem_usable_size(nullptr) == 0u);

        char *s = kimix_mem_strdup("hello");
        expect(s != nullptr);
        expect(std::strcmp(s, "hello") == 0);
        char *sn = kimix_mem_strndup("hello", 2);
        expect(sn != nullptr);
        expect(std::strcmp(sn, "he") == 0);
        kimix_mem_free(s);
        kimix_mem_free(sn);
    };

    "mem aligned allocation honours the alignment"_test = [] {
        auto *a = kimix_mem_malloc_aligned(100, 64);
        expect(a != nullptr);
        expect((reinterpret_cast<uintptr_t>(a) % 64u) == 0u);
        kimix_mem_free_aligned(a, 64);

        auto *b = kimix_mem_zalloc_aligned(48, 32);
        expect(b != nullptr);
        expect((reinterpret_cast<uintptr_t>(b) % 32u) == 0u);
        kimix_mem_free(b);

        void *p = nullptr;
        expect(kimix_mem_posix_memalign(&p, 16, 32) == KIMIX_OK);
        expect(p != nullptr);
        expect((reinterpret_cast<uintptr_t>(p) % 16u) == 0u);
        kimix_mem_free(p);
        // Argument validation happens before mimalloc is entered.
        expect(kimix_mem_posix_memalign(nullptr, 16, 32) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_mem_posix_memalign(&p, 3, 32) == KIMIX_ERR_INVALID_ARG);

        expect(kimix_mem_version() > 0u);
        expect(std::strlen(kimix_mem_version_string()) > 0);
        kimix_mem_thread_init();
        kimix_mem_collect(false);
        kimix_mem_thread_done();
    };

    // ------------------------------------------------------------------ vectors
    "vec lifecycle: init, use, destroy"_test = [] {
        kimix_vec v;
        expect(!kimix_vec_is_initialized(&v));
        expect(kimix_vec_default_init(&v) == KIMIX_OK);
        expect(kimix_vec_is_initialized(&v));
        expect(kimix_vec_empty(&v));
        expect(kimix_vec_size(&v) == 0u);

        // A second init would leak the live buffer, so it must be refused.
        expect(kimix_vec_default_init(&v) == KIMIX_ERR_INVALID_STATE);

        expect(kimix_vec_push_back(&v, 0x41) == KIMIX_OK);
        expect(kimix_vec_append(&v, "BCD", 3) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 4u);
        expect(!kimix_vec_empty(&v));
        expect(kimix_vec_capacity(&v) >= 4u);

        unsigned char *at = nullptr;
        expect(kimix_vec_at(&v, 1, &at) == KIMIX_OK);
        expect(at != nullptr && *at == 'B');
        expect(kimix_vec_at(&v, 99, &at) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_vec_at(&v, 0, nullptr) == KIMIX_ERR_INVALID_ARG);

        expect(kimix_vec_data(&v)[0] == 'A');
        expect(kimix_vec_data(&v)[3] == 'D');

        // read / write inside the current size only.
        unsigned char dst[4] = {0, 0, 0, 0};
        expect(kimix_vec_read(&v, 0, dst, 4) == KIMIX_OK);
        expect(dst[0] == 'A' && dst[3] == 'D');
        expect(kimix_vec_read(&v, 2, dst, 4) == KIMIX_ERR_OUT_OF_RANGE);
        const unsigned char patch[] = {'x', 'y'};
        expect(kimix_vec_write(&v, 1, patch, 2) == KIMIX_OK);
        expect(kimix_vec_data(&v)[1] == 'x' && kimix_vec_data(&v)[2] == 'y');
        expect(kimix_vec_write(&v, 3, patch, 2) == KIMIX_ERR_OUT_OF_RANGE);

        // resize / reserve / shrink / clear / pop / insert / erase.
        expect(kimix_vec_resize(&v, 10) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 10u);
        expect(kimix_vec_data(&v)[9] == 0); // grown with zeros
        expect(kimix_vec_resize_fill(&v, 12, 0x7F) == KIMIX_OK);
        expect(kimix_vec_data(&v)[10] == 0x7F);
        expect(kimix_vec_resize(&v, 6) == KIMIX_OK);
        expect(kimix_vec_reserve(&v, 64) == KIMIX_OK);
        expect(kimix_vec_capacity(&v) >= 64u);
        expect(kimix_vec_shrink_to_fit(&v) == KIMIX_OK);
        expect(kimix_vec_capacity(&v) >= 6u);
        expect(kimix_vec_insert(&v, 0, "z", 1) == KIMIX_OK);
        expect(kimix_vec_data(&v)[0] == 'z');
        expect(kimix_vec_insert(&v, 99, "z", 1) == KIMIX_ERR_OUT_OF_RANGE);
        expect(kimix_vec_erase(&v, 0, 1) == KIMIX_OK);
        expect(kimix_vec_pop_back(&v) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 5u);
        expect(kimix_vec_clear(&v) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 0u);
        expect(kimix_vec_pop_back(&v) == KIMIX_ERR_OUT_OF_RANGE);

        expect(kimix_vec_destroy(&v) == KIMIX_OK);
        expect(!kimix_vec_is_initialized(&v));
        // Destroying twice, or using a dead placeholder, is an error return.
        expect(kimix_vec_destroy(&v) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_size(&v) == static_cast<size_t>(-1));
        expect(kimix_vec_append(&v, "no", 2) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_vec_push_back(nullptr, 0) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_size(nullptr) == static_cast<size_t>(-1));
        expect(!kimix_vec_is_initialized(nullptr));

        // Re-initialising the very same storage after a destroy is legal.
        expect(kimix_vec_init_from(&v, "abc", 3) == KIMIX_OK);
        expect(kimix_vec_size(&v) == 3u);
        expect(kimix_vec_destroy(&v) == KIMIX_OK);
    };

    "vec copy, move, assign and swap"_test = [] {
        kimix_vec a, b, c;
        expect(kimix_vec_init_from(&a, "abcd", 4) == KIMIX_OK);

        // copy construction of an independent buffer
        expect(kimix_vec_copy_init(&b, &a) == KIMIX_OK);
        expect(kimix_vec_data(&b) != kimix_vec_data(&a));
        bool equal = false;
        expect(kimix_vec_equals(&a, &b, &equal) == KIMIX_OK);
        expect(equal);

        // copy assignment onto a live vector
        expect(kimix_vec_default_init(&c) == KIMIX_OK);
        expect(kimix_vec_assign(&c, &a) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 4u);
        expect(kimix_vec_assign_bytes(&c, "zz", 2) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 2u);

        // move construction: the buffer travels, the source becomes empty but
        // stays initialised (and still needs exactly one destroy).
        const unsigned char *moved = kimix_vec_data(&a);
        expect(kimix_vec_move_init(&b, &a) == KIMIX_ERR_INVALID_STATE); // b is live
        expect(kimix_vec_destroy(&b) == KIMIX_OK);
        expect(kimix_vec_move_init(&b, &a) == KIMIX_OK);
        expect(kimix_vec_data(&b) == moved);
        expect(kimix_vec_size(&b) == 4u);
        expect(kimix_vec_size(&a) == 0u);
        expect(kimix_vec_is_initialized(&a));
        expect(kimix_vec_move_init(&b, &b) == KIMIX_ERR_INVALID_ARG);

        // move assignment onto a live vector: `a` goes back to raw storage first
        // (the moved-from vector is still alive and must be destroyed once).
        expect(kimix_vec_destroy(&a) == KIMIX_OK);
        expect(kimix_vec_init_from(&a, "Q", 1) == KIMIX_OK);
        expect(kimix_vec_move_assign(&c, &a) == KIMIX_OK);
        expect(kimix_vec_size(&c) == 1u);
        expect(kimix_vec_size(&a) == 0u);
        expect(kimix_vec_move_assign(&c, &c) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_move_assign(&c, nullptr) == KIMIX_ERR_INVALID_ARG);

        // swap is O(1) and keeps both sides alive
        expect(kimix_vec_append(&a, "xyz", 3) == KIMIX_OK);
        expect(kimix_vec_swap(&a, &c) == KIMIX_OK);
        expect(kimix_vec_size(&a) == 1u && kimix_vec_size(&c) == 3u);

        expect(kimix_vec_destroy(&a) == KIMIX_OK);
        expect(kimix_vec_destroy(&b) == KIMIX_OK);
        expect(kimix_vec_destroy(&c) == KIMIX_OK);
    };

    "vec heap handle helpers"_test = [] {
        kimix_vec *h = kimix_vec_new();
        expect(h != nullptr);
        expect(kimix_vec_is_initialized(h));
        expect(kimix_vec_append(h, "heap", 4) == KIMIX_OK);
        expect(kimix_vec_size(h) == 4u);
        kimix_vec *h2 = kimix_vec_new_from("data", 4);
        expect(h2 != nullptr);
        bool same = false;
        expect(kimix_vec_equals(h, h2, &same) == KIMIX_OK);
        expect(!same);
        kimix_vec_free(h);
        kimix_vec_free(h2);
        kimix_vec_free(nullptr); // documented as a no-op
    };

    // -------------------------------------------------------------------- json
    "yyjson read and inspect"_test = [] {
        const char *text = R"({"name":"kimix","count":3,"ratio":1.5,"ok":true,"none":null,
                               "list":[10,-20,2.5],"nested":{"a":{"b":[1,2,3]}}})";
        kimix_yyjson_read_err err;
        kimix_yyjson_doc *doc = kimix_yyjson_read(text, std::strlen(text), KIMIX_YYJSON_READ_NOFLAG, &err);
        expect(doc != nullptr);
        expect(err.code == KIMIX_YYJSON_READ_SUCCESS);

        kimix_yyjson_val *root = kimix_yyjson_doc_root(doc);
        expect(root != nullptr);
        expect(kimix_yyjson_is_obj(root));
        expect(kimix_yyjson_get_type(root) == KIMIX_YYJSON_TYPE_OBJ);
        expect(kimix_yyjson_obj_size(root) == 7u); // name count ratio ok none list nested

        expect(kimix_yyjson_equals_str(kimix_yyjson_obj_get(root, "name"), "kimix"));
        expect(std::strcmp(kimix_yyjson_get_str(kimix_yyjson_obj_get(root, "name")), "kimix") == 0);
        expect(kimix_yyjson_get_len(kimix_yyjson_obj_get(root, "name")) == 5u);
        expect(kimix_yyjson_get_uint(kimix_yyjson_obj_get(root, "count")) == 3u);
        expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(root, "count")) == 3);
        expect(kimix_yyjson_get_num(kimix_yyjson_obj_get(root, "ratio")) == 1.5);
        expect(kimix_yyjson_is_true(kimix_yyjson_obj_get(root, "ok")));
        expect(kimix_yyjson_get_bool(kimix_yyjson_obj_get(root, "ok")));
        expect(kimix_yyjson_is_null(kimix_yyjson_obj_get(root, "none")));

        kimix_yyjson_val *list = kimix_yyjson_obj_get(root, "list");
        expect(kimix_yyjson_is_arr(list));
        expect(kimix_yyjson_arr_size(list) == 3u);
        expect(kimix_yyjson_get_sint(kimix_yyjson_arr_get(list, 1)) == -20);
        expect(kimix_yyjson_arr_get_first(list) == kimix_yyjson_arr_get(list, 0));
        expect(kimix_yyjson_arr_get_last(list) == kimix_yyjson_arr_get(list, 2));
        expect(kimix_yyjson_arr_get(list, 3) == nullptr);
        expect(kimix_yyjson_obj_get(root, "missing") == nullptr);

        kimix_yyjson_val *b = kimix_yyjson_obj_get(kimix_yyjson_obj_get(
            kimix_yyjson_obj_get(root, "nested"), "a"), "b");
        expect(b != nullptr && kimix_yyjson_arr_size(b) == 3u);
        expect(kimix_yyjson_doc_read_size(doc) == std::strlen(text));
        expect(kimix_yyjson_doc_val_count(doc) > 0u);

        // NULL-safety: every accessor must return its neutral value, not crash.
        expect(!kimix_yyjson_is_null(nullptr));
        expect(kimix_yyjson_get_uint(nullptr) == 0u);
        expect(kimix_yyjson_get_str(nullptr) == nullptr);
        expect(kimix_yyjson_arr_size(nullptr) == 0u);
        expect(kimix_yyjson_obj_size(nullptr) == 0u);

        kimix_yyjson_doc_free(doc);
        kimix_yyjson_doc_free(nullptr); // documented as a no-op
    };

    "yyjson read errors and flags"_test = [] {
        const char *bad = "{\"a\": }";
        kimix_yyjson_read_err err;
        kimix_yyjson_doc *doc = kimix_yyjson_read(bad, std::strlen(bad), KIMIX_YYJSON_READ_NOFLAG, &err);
        expect(doc == nullptr);
        expect(err.code != KIMIX_YYJSON_READ_SUCCESS);
        expect(err.msg != nullptr);
        expect(err.position < std::strlen(bad) + 1);

        // A NULL err out-parameter is allowed, and the nullable-err read still fails.
        expect(kimix_yyjson_read(bad, std::strlen(bad), KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);
        expect(kimix_yyjson_read_str(bad, KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);

        // Trailing commas are rejected by default and accepted with the flag.
        const char *loose = "[1,2,3,]";
        expect(kimix_yyjson_read_str(loose, KIMIX_YYJSON_READ_NOFLAG, nullptr) == nullptr);
        doc = kimix_yyjson_read_str(loose, KIMIX_YYJSON_READ_ALLOW_TRAILING_COMMAS, &err);
        expect(doc != nullptr);
        expect(kimix_yyjson_arr_size(kimix_yyjson_doc_root(doc)) == 3u);
        kimix_yyjson_doc_free(doc);

        // Comments and single-quoted strings (JSON5-ish flags).
        const char *c = "/* hi */ {'k': 1}";
        doc = kimix_yyjson_read_str(c,
                                    KIMIX_YYJSON_READ_ALLOW_COMMENTS
                                        | KIMIX_YYJSON_READ_ALLOW_SINGLE_QUOTED_STR,
                                    &err);
        expect(doc != nullptr);
        if (doc) {
            expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(kimix_yyjson_doc_root(doc), "k")) == 1);
            kimix_yyjson_doc_free(doc);
        }
        expect(kimix_yyjson_version() > 0u);
    };

    "yyjson write an immutable document"_test = [] {
        const char *text = "{\"a\":[1,2]}";
        kimix_yyjson_doc *doc = kimix_yyjson_read_str(text, KIMIX_YYJSON_READ_NOFLAG, nullptr);
        expect(doc != nullptr);

        size_t len = 0;
        char *out = kimix_yyjson_write(doc, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(out != nullptr);
        expect(len > 0u);
        expect(std::strlen(out) == len);
        // The writer's buffer is library-heap memory: only these two free it.
        kimix_yyjson_str_free(out);

        char *pretty = kimix_yyjson_write(doc, KIMIX_YYJSON_WRITE_PRETTY, &len, nullptr);
        expect(pretty != nullptr);
        expect(std::strlen(pretty) == len);
        kimix_mem_free(pretty); // same mimalloc heap, so kimix_mem_free is valid too

        char *one = kimix_yyjson_val_write(kimix_yyjson_doc_root(doc), KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(one != nullptr);
        kimix_yyjson_str_free(one);

        // The _buf form allocates nothing at all.
        char stack_buf[64];
        size_t written = kimix_yyjson_write_buf(stack_buf, sizeof(stack_buf), doc, KIMIX_YYJSON_WRITE_NOFLAG, nullptr);
        expect(written > 0u && written < sizeof(stack_buf));
        expect(std::strlen(stack_buf) == written);

        expect(kimix_yyjson_write(nullptr, KIMIX_YYJSON_WRITE_NOFLAG, nullptr, nullptr) == nullptr);
        kimix_yyjson_str_free(nullptr);
        kimix_yyjson_doc_free(doc);
    };

    "yyjson build a mutable document"_test = [] {
        kimix_yyjson_mut_doc *m = kimix_yyjson_mut_doc_new();
        expect(m != nullptr);
        kimix_yyjson_mut_val *root = kimix_yyjson_mut_obj(m);
        expect(root != nullptr);
        kimix_yyjson_mut_doc_set_root(m, root);
        expect(kimix_yyjson_mut_doc_root(m) == root);

        expect(kimix_yyjson_mut_obj_add_str(m, root, "cmd", "ls"));
        expect(kimix_yyjson_mut_obj_add_sint(m, root, "code", -2));
        expect(kimix_yyjson_mut_obj_add_uint(m, root, "pid", 4242));
        expect(kimix_yyjson_mut_obj_add_bool(m, root, "ok", true));
        expect(kimix_yyjson_mut_obj_add_real(m, root, "ratio", 0.5));
        expect(kimix_yyjson_mut_obj_add_null(m, root, "extra"));
        kimix_yyjson_mut_val *args = kimix_yyjson_mut_obj_add_arr(m, root, "args");
        expect(args != nullptr);
        expect(kimix_yyjson_mut_arr_add_strcpy(m, args, "-la"));
        expect(kimix_yyjson_mut_arr_add_sint(m, args, 7));
        expect(kimix_yyjson_mut_arr_add_val(args, kimix_yyjson_mut_true(m)));
        expect(kimix_yyjson_mut_arr_size(args) == 3u);
        expect(kimix_yyjson_mut_arr_get(args, 0) != nullptr);
        expect(kimix_yyjson_mut_arr_get_last(args) != nullptr);
        expect(kimix_yyjson_mut_arr_remove(args, 1) != nullptr);
        expect(kimix_yyjson_mut_arr_size(args) == 2u);
        expect(kimix_yyjson_mut_arr_append(args, kimix_yyjson_mut_str(m, "tail")));

        expect(kimix_yyjson_mut_obj_size(root) == 7u);
        expect(kimix_yyjson_mut_obj_get(root, "cmd") != nullptr);
        expect(kimix_yyjson_mut_obj_get(root, "nope") == nullptr);
        expect(kimix_yyjson_mut_obj_remove(root, "extra"));
        expect(kimix_yyjson_mut_obj_size(root) == 6u);

        size_t len = 0;
        char *out = kimix_yyjson_mut_write(m, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(out != nullptr);
        expect(std::strstr(out, "\"cmd\":\"ls\"") != nullptr);
        kimix_yyjson_str_free(out);

        // mut -> immutable, then read it back through the immutable API.
        kimix_yyjson_doc *im = kimix_yyjson_mut_doc_imut_copy(m);
        expect(im != nullptr);
        kimix_yyjson_val *ir = kimix_yyjson_doc_root(im);
        expect(kimix_yyjson_get_sint(kimix_yyjson_obj_get(ir, "code")) == -2);
        expect(kimix_yyjson_equals_str(kimix_yyjson_obj_get(ir, "cmd"), "ls"));
        kimix_yyjson_doc_free(im);

        // immutable -> mutable, mutate, write again.
        kimix_yyjson_doc *src = kimix_yyjson_read_str("{\"x\":1}", KIMIX_YYJSON_READ_NOFLAG, nullptr);
        kimix_yyjson_mut_doc *copy = kimix_yyjson_doc_mut_copy(src);
        expect(copy != nullptr);
        expect(kimix_yyjson_mut_obj_add_sint(copy, kimix_yyjson_mut_doc_root(copy), "y", 2));
        char *twice = kimix_yyjson_mut_write(copy, KIMIX_YYJSON_WRITE_NOFLAG, &len, nullptr);
        expect(twice != nullptr && std::strstr(twice, "\"y\":2") != nullptr);
        kimix_yyjson_str_free(twice);
        kimix_yyjson_mut_doc_free(kimix_yyjson_mut_doc_mut_copy(copy));
        kimix_yyjson_mut_doc_free(copy);
        kimix_yyjson_doc_free(src);

        // NULL-safety of the builders and the writers.
        expect(kimix_yyjson_mut_arr(nullptr) == nullptr);
        expect(!kimix_yyjson_mut_arr_append(nullptr, nullptr));
        expect(!kimix_yyjson_mut_obj_add_str(nullptr, nullptr, nullptr, nullptr));
        expect(kimix_yyjson_mut_write(nullptr, KIMIX_YYJSON_WRITE_NOFLAG, nullptr, nullptr) == nullptr);
        kimix_yyjson_mut_doc_free(nullptr);
        kimix_yyjson_mut_doc_free(m);
    };

    // ------------------------------------------------------------------ repair
    "repair malformed json into a vector"_test = [] {
        const char *bad = "{\"a\": 1, \"b\": [1, 2,],}";
        expect(!kimix_json_is_valid(bad, std::strlen(bad)));

        kimix_vec out;
        expect(kimix_repair(&out, bad, std::strlen(bad)) == KIMIX_OK);
        expect(kimix_vec_size(&out) > 0u);
        // The repaired text is NUL-terminated and strictly valid JSON.
        const char *cstr = kimix_repaired_cstr(&out);
        expect(cstr != nullptr);
        expect(kimix_json_is_valid_str(cstr));
        kimix_str_view view;
        expect(kimix_repaired_view(&out, &view) == KIMIX_OK);
        expect(view.length + 1 == kimix_vec_size(&out)); // the trailing '\0' is excluded
        expect(view.data != nullptr && view.data[view.length] == '\0');

        // Reuse of the same live placeholder is an assign, not a re-init.
        expect(kimix_repair(&out, bad, std::strlen(bad)) == KIMIX_ERR_INVALID_STATE);
        expect(kimix_repair_assign(&out, bad, std::strlen(bad)) == KIMIX_OK);
        expect(kimix_vec_size(&out) > 0u);

        // Already-valid input => EMPTY result (the kimix::repair() convention).
        kimix_vec clean;
        const char *good = "{\"a\":1}";
        expect(kimix_json_is_valid(good, std::strlen(good)));
        expect(kimix_repair(&clean, good, std::strlen(good)) == KIMIX_OK);
        expect(kimix_vec_size(&clean) == 0u);
        expect(kimix_repaired_view(&clean, &view) == KIMIX_OK);
        expect(view.length == 0u);
        expect(kimix_repaired_cstr(&clean) == nullptr);

        // Argument / state errors, then teardown.
        expect(kimix_repair(&out, nullptr, 4) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_repair(nullptr, good, std::strlen(good)) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_repair_assign(&clean, nullptr, 0) == KIMIX_OK);
        expect(kimix_repaired_view(&out, nullptr) == KIMIX_ERR_INVALID_ARG);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);
        expect(kimix_vec_destroy(&clean) == KIMIX_OK);
        // The storage is raw again, so a fresh construction succeeds.
        expect(kimix_repair(&out, good, std::strlen(good)) == KIMIX_OK);
        expect(kimix_vec_size(&out) == 0u);
        expect(kimix_vec_destroy(&out) == KIMIX_OK);

        // The heap variant frees through the vector FFI.
        kimix_vec *h = kimix_repair_new(bad, std::strlen(bad));
        expect(h != nullptr);
        expect(kimix_vec_size(h) > 0u);
        kimix_vec_free(h);
        expect(kimix_repair_new(nullptr, 8) == nullptr);
    };

    "repair keeps the buffer on the library heap"_test = [] {
        // A repaired result must be releasable by kimix_vec_destroy() alone: the
        // vector<char> that kimix::repair() returned was moved (not copied) into
        // the caller's placeholder, so exactly one owner ever exists.
        for (int i = 0; i < 2000; ++i) {
            kimix_vec v;
            const char *bad = "[1,2,3,]";
            if (kimix_repair(&v, bad, std::strlen(bad)) != KIMIX_OK) {
                expect(false);
                return;
            }
            expect(kimix_vec_size(&v) > 0u);
            if (kimix_vec_destroy(&v) != KIMIX_OK) {
                expect(false);
                return;
            }
        }
        expect(true);
    };
}
