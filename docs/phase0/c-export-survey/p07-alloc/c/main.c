#include <stdio.h>
#include <stdlib.h>
#include "../terndb.h"
static long mine = 0, freed = 0;
static uint8_t *my_alloc(uint64_t n) { mine++; return malloc(n); }
static void my_free(uint8_t *p) { freed++; free(p); }
int main(void) {
    terndb_set_allocator(my_alloc, my_free);
    printf("work -> %lld\n", terndb_work(100));
    printf("hook allocs %llu, mine %ld, freed %ld\n", terndb_allocs(), mine, freed);
    return 0;
}
