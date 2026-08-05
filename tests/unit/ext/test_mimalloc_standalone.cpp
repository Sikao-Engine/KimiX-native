// Minimal test to check if mimalloc works when compiled inline alongside yyjson
#include <cstdio>

#define YYJSON_CUSTOM_ALC yyjson_mimalloc_alc

#include "yyjson.h"
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

const yyjson_alc yyjson_mimalloc_alc = {
    mimalloc_malloc,
    mimalloc_realloc,
    mimalloc_free,
    NULL
};

int main() {
    std::printf("test start\n");
    
    // Test mimalloc directly
    void* p = mi_malloc(64);
    std::printf("mi_malloc(64) = %p\n", p);
    if (p) {
        mi_free(p);
        std::printf("mi_free ok\n");
    }
    
    // Test yyjson with mimalloc allocator
    const char* json = "{\"hello\":\"world\"}";
    yyjson_doc* doc = yyjson_read(json, 18, 0);
    if (doc) {
        std::printf("yyjson parsed OK\n");
        yyjson_doc_free(doc);
    } else {
        std::printf("yyjson parse FAILED\n");
    }
    
    // Test yyjson mutable doc with mimalloc
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
    if (mdoc) {
        std::printf("yyjson mut_doc OK\n");
        yyjson_mut_doc_free(mdoc);
    } else {
        std::printf("yyjson mut_doc FAILED\n");
    }
    
    std::printf("test end\n");
    return 0;
}
