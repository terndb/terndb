#include <stdio.h>
#include <stdint.h>
uint8_t terndb_first(uint8_t*); int32_t terndb_at(uint64_t);
int main(void){uint8_t c=65;printf("%d %d\n",terndb_first(&c),terndb_at(9));return 0;}
