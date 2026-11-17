#include <stdio.h>
#include <stdlib.h>
#include "ha.h"

static long calls, frees, bytes;
static uint8_t *my_alloc(uint64_t n) { calls++; bytes += (long)n; return malloc(n); }
static void my_free(uint8_t *p) { frees++; free(p); }

int main(void) {
    ha_set(my_alloc, my_free);
    int64_t r = ha_work(1000);
    printf("work=%lld host allocs=%ld frees=%ld bytes=%ld\n", (long long)r, calls, frees, bytes);
    return 0;
}
