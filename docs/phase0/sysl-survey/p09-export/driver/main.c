#include <stdio.h>
#include "libdbx.a.h"

/* the header spells sysl's *u8 as uint8_t *, not void *, so the callback and the context are cast */
static int32_t sum_until(uint8_t *ctx, int64_t v) {
    int64_t *acc = (int64_t *)ctx;
    *acc += v;
    return *acc >= 6;
}

int main(void) {
    dbx_db *db = dbx_open();
    for (int i = 1; i <= 5; i++) dbx_put(db, i);
    int64_t acc = 0;
    int seen = dbx_scan(db, sum_until, (uint8_t *)&acc);
    printf("seen %d acc %lld\n", seen, (long long)acc);
    dbx_close(db);
    return 0;
}
