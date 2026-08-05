#include "../core/kimix_core.h"
#include <cstdio>
#include <cstdlib>

int main() {
    printf("Kimix Test Suite\n");
    printf("Version: %s\n\n", kimix::version_string);

    int failures = 0;

    // Test add
    {
        int result = kimix::add(2, 3);
        if (result == 5) {
            printf("[PASS] add(2, 3) == 5\n");
        } else {
            printf("[FAIL] add(2, 3) == %d, expected 5\n", result);
            failures++;
        }
    }

    // Test multiply
    {
        int result = kimix::multiply(4, 5);
        if (result == 20) {
            printf("[PASS] multiply(4, 5) == 20\n");
        } else {
            printf("[FAIL] multiply(4, 5) == %d, expected 20\n", result);
            failures++;
        }
    }

    // Test negative numbers
    {
        int result = kimix::add(-3, 7);
        if (result == 4) {
            printf("[PASS] add(-3, 7) == 4\n");
        } else {
            printf("[FAIL] add(-3, 7) == %d, expected 4\n", result);
            failures++;
        }
    }

    printf("\n%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
