#include <stdio.h>
#include "terndb.h"

int main(void) {
    int32_t a[4] = {1, 2, 3, 4};
    const char *key = "hello";
    uint8_t out = 0;
    printf("%zu %d %.2f %d %u\n", terndb_count(10, -3), terndb_ok(false), terndb_mix(1.5f, 2.25),
           terndb_sum4(&a), terndb_first((const uint8_t *)key));
    terndb_put((const uint8_t *)key, 5, &out);
    printf("%c\n", out);
    return 0;
}
