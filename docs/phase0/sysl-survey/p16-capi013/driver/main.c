#include <stdio.h>
#include <stdlib.h>
#include "../terndb.h"

static void *my_alloc(size_t n) { return malloc(n); }

int main(void) {
    uint8_t out[32];
    ptrdiff_t r = terndb_digest(my_alloc, (const uint8_t *)"abc", 3, out);
    terndb_Db *d = terndb_open();
    printf("digest %td len %d value %lld\n", r, TERNDB_DIGEST_LEN, (long long)terndb_value(d));
    return 0;
}
