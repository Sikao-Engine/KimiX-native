// Minimal test to check if mimalloc works when compiled inline
#include <cstdio>
#include <mimalloc.h>

int main() {
    std::printf("mimalloc test start\n");
    void* p = mi_malloc(64);
    std::printf("allocated: %p\n", p);
    if (p) {
        mi_free(p);
        std::printf("freed\n");
    }
    std::printf("done\n");
    return 0;
}
