// Minimal test with yyjson + mimalloc using Boost.UT
// To identify if Boost.UT causes the hang
#include <cstdio>

#include "ut/ut.hpp"
#include "yyjson.h"

extern "C" size_t mi_malloc_size(const void *p);

using namespace boost::ut;
using namespace boost::ut::literals;

int main() {
    std::printf("before test\n");
    
    "basic"_test = [] {
        std::printf("in test body\n");
        const char* json = "{\"a\":1}";
        yyjson_doc* doc = yyjson_read(json, 7, 0);
        expect(doc != nullptr);
        if (doc) {
            size_t sz = mi_malloc_size(doc);
            std::printf("mimalloc tracking size: %zu\n", sz);
            yyjson_doc_free(doc);
        }
    };
    
    std::printf("after test registration\n");
    return 0;
}
