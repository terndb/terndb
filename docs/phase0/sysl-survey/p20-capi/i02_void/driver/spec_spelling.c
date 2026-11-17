#include <stdlib.h>
#include "i02.h"
/* the spelling c-api.md section 12 uses: void * allocator and context */
static void *my_alloc(size_t n) { return malloc(n); }
int main(void) { int ctx = 0; i02_set(my_alloc, &ctx); return 0; }
