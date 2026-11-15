#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../terndb.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *waker(void *p) { usleep(100000); printf("wake -> %d\n", terndb_wake()); return 0; }
int main(void) {
    printf("open -> %d\n", terndb_open());
    pthread_t t; pthread_create(&t, 0, waker, 0);
    double a = now();
    long long n = terndb_run_once(5000);
    printf("run_once -> %lld after %.0f ms\n", n, (now() - a) * 1000);
    pthread_join(t, 0);
    return 0;
}
