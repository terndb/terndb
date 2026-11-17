#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt.h"

static int32_t cb(int32_t n) { return rt_panic(n); }

static void *other(void *arg) {
    (void)arg;
    printf("other thread: is_engine=%d id=%llu\n", rt_is_engine(), (unsigned long long)rt_thread_id());
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "direct") == 0) { printf("calling rt_panic(5)\n"); fflush(stdout); return rt_panic(5); }
    if (argc > 1 && strcmp(argv[1], "nested") == 0) { printf("calling rt_call_cb\n"); fflush(stdout); return rt_call_cb(cb, 5); }
    rt_mark_engine();
    printf("main thread: is_engine=%d id=%llu\n", rt_is_engine(), (unsigned long long)rt_thread_id());
    pthread_t t; pthread_create(&t, NULL, other, NULL); pthread_join(t, NULL);
    return 0;
}
