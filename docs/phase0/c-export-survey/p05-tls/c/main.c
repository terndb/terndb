#include <pthread.h>
#include <stdio.h>
#include "terndb.h"
static void *engine(void *p) { terndb_engine_enter(); printf("engine: %d\n", terndb_on_engine()); return 0; }
int main(void) {
    pthread_t t; pthread_create(&t, 0, engine, 0); pthread_join(t, 0);
    printf("main: %d\n", terndb_on_engine());
    return 0;
}
