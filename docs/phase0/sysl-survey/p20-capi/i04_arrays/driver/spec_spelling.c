#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "i04.h"
/* the spellings c-api.md section 12 uses: size_t, uint8_t digest[32], uint8_t out[16] */
static bool on_size(size_t n) { return n > 0; }
int main(void) {
    uint8_t digest[32], out[16];
    return (int)i04_set(on_size, digest, out, 1.0f, 2.0);
}
