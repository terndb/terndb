/* The C ABI's conformance driver (docs/spec/c-api.md §15.2): built against the generated terndb.h and
 * libterndb only, with `cc -Wall -Wextra -Werror`, and run in a release and a debug build by
 * tests/capi/run.zsh. Every test runs in a child process of its own, so a test that expects the
 * library to abort (a debug build's handle checks, §2.4) is judged by how the child ended.
 *
 *   driver            runs every test, printing one line each and a summary; exits 1 on a failure
 *   driver <name>...  runs the named tests only
 */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include "terndb.h"
#include "inputs.h"

/* ---- the harness ------------------------------------------------------------------------------ */

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                 \
            exit(1);                                                                                 \
        }                                                                                            \
    } while (0)

/* A test whose library or engine cannot meet it says so and passes nothing. */
#define SKIP(why)                                                                                    \
    do {                                                                                             \
        printf("SKIP: %s\n", why);                                                                   \
        fflush(stdout);                                                                              \
        exit(77);                                                                                    \
    } while (0)

/* The code of entry 0 of `e`, or "" for none. */
static const char *code_of(terndb_error *e) { return e ? terndb_error_code(e, 0) : ""; }

/* The call `call`, which takes `&err` last, fails with code `code`; the error is freed. */
#define EXPECT_CODE(code, call)                                                                      \
    do {                                                                                             \
        terndb_error *err_ = NULL;                                                                   \
        int r_ = (call);                                                                             \
        if (r_ != -1 || strcmp(code_of(err_), code) != 0) {                                          \
            fprintf(stderr, "%s:%d: expected %s from %s, got %d %s\n", __FILE__, __LINE__, code,     \
                    #call, r_, code_of(err_));                                                       \
            exit(1);                                                                                 \
        }                                                                                            \
        terndb_error_free(err_);                                                                     \
    } while (0)

/* The call `call`, which takes `&err` last, succeeds. */
#define EXPECT_OK(call)                                                                              \
    do {                                                                                             \
        terndb_error *err_ = NULL;                                                                   \
        int r_ = (call);                                                                             \
        if (r_ < 0) {                                                                                \
            const char *m_ = "";                                                                     \
            size_t ml_ = 0;                                                                          \
            if (err_) terndb_error_message(err_, 0, &m_, &ml_);                                      \
            fprintf(stderr, "%s:%d: %s failed: %s %.*s\n", __FILE__, __LINE__, #call, code_of(err_), \
                    (int)ml_, m_);                                                                   \
            exit(1);                                                                                 \
        }                                                                                            \
    } while (0)

static void init(void) { EXPECT_OK(terndb_init(TERNDB_ABI_VERSION, NULL, NULL, NULL, &err_)); }

static terndb_config *config(const char *key, const char *value, ...) {
    terndb_config *cfg = NULL;
    EXPECT_OK(terndb_config_new(&cfg, &err_));
    va_list ap;
    va_start(ap, value);
    while (key) {
        EXPECT_OK(terndb_config_set(cfg, key, strlen(key), value, strlen(value), &err_));
        key = va_arg(ap, const char *);
        if (key) value = va_arg(ap, const char *);
    }
    va_end(ap);
    return cfg;
}

/* An anonymous memory-mode database, the one kind this build opens. */
static terndb_db *open_db(void) {
    terndb_config *cfg = config("mode", "memory", "durability", "none", NULL);
    terndb_db *db = NULL;
    EXPECT_OK(terndb_open("", 0, cfg, &db, &err_));
    terndb_config_free(cfg);
    return db;
}

static terndb_conn *session(terndb_db *db) {
    terndb_conn *c = NULL;
    EXPECT_OK(terndb_session(db, NULL, &c, &err_));
    return c;
}

static terndb_result *run(terndb_conn *c, terndb_txn *t, const uint8_t *env, size_t len) {
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, t, NULL, NULL, env, len, NULL, 0, &r, &err_));
    return r;
}

/* Runs a statement to its end, answering the number of rows it returned. */
static size_t run_all(terndb_conn *c, terndb_txn *t, const uint8_t *env, size_t len) {
    terndb_result *r = run(c, t, env, len);
    size_t rows = 0;
    terndb_batch *b = NULL;
    int k;
    terndb_error *err = NULL;
    while ((k = terndb_result_next(r, &b, &err)) == 1) {
        rows += terndb_batch_rows(b);
        terndb_batch_free(b);
    }
    if (k != 0) {
        fprintf(stderr, "run_all: %s\n", code_of(err));
        exit(1);
    }
    terndb_result_free(r);
    return rows;
}

#define RUN(c, t, name) run_all(c, t, name, name##_LEN)

/* A database with table t holding (1, ann), (2, bo), (3, cy). */
static terndb_conn *with_table(terndb_db **dbp) {
    *dbp = open_db();
    terndb_conn *c = session(*dbp);
    RUN(c, NULL, ENV_CREATE_T);
    RUN(c, NULL, ENV_INSERT_T);
    return c;
}

static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- §3 initialisation and versions ----------------------------------------------------------- */

static void test_init_order(void) {
    /* The four calls allowed before terndb_init. */
    CHECK(terndb_abi_version() == TERNDB_ABI_VERSION);
    CHECK(strlen(terndb_version()) > 0);
    CHECK(strcmp(terndb_code_name("C1008"), "not_initialized") == 0);
    (void)terndb_debug_checks();
    terndb_config *cfg = NULL;
    EXPECT_CODE("C1008", terndb_config_new(&cfg, &err_));
    terndb_db *db = NULL;
    EXPECT_CODE("C1008", terndb_open("", 0, NULL, &db, &err_));
    terndb_queue *q = NULL;
    EXPECT_CODE("C1008", terndb_queue_new(&q, &err_));
    terndb_txn_options *o = NULL;
    EXPECT_CODE("C1008", terndb_txn_options_new(&o, &err_));
    EXPECT_CODE("C1008", terndb_set_log_sink(1, NULL, NULL, &err_));
    terndb_op *op = terndb_open_async("", 0, NULL, NULL, 0);
    CHECK(op != NULL);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_CODE("C1008", terndb_op_error(op, &err_));
    CHECK(cfg == NULL && db == NULL && q == NULL && o == NULL);
}

static void test_init_version(void) {
    EXPECT_CODE("C1009", terndb_init(TERNDB_ABI_VERSION + 1, NULL, NULL, NULL, &err_));
    EXPECT_CODE("C1009", terndb_init((uint32_t)(TERNDB_ABI_MAJOR + 1) << 16, NULL, NULL, NULL, &err_));
    EXPECT_CODE("C1009", terndb_init(0, NULL, NULL, NULL, &err_));
    init();
    init(); /* the same arguments again: nothing */
    CHECK(terndb_abi_version() >> 16 == TERNDB_ABI_MAJOR);
}

static void *sys_alloc(void *ctx, size_t size, size_t align) {
    (void)ctx;
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) == 0 ? p : NULL;
}
static void sys_free(void *ctx, void *p, size_t size, size_t align) {
    (void)ctx; (void)size; (void)align;
    free(p);
}

static void test_init_twice_differently(void) {
    init();
    EXPECT_CODE("C1005", terndb_init(TERNDB_ABI_VERSION, sys_alloc, sys_free, NULL, &err_));
}

static void test_init_half_allocator(void) {
    EXPECT_CODE("C1002", terndb_init(TERNDB_ABI_VERSION, sys_alloc, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_init(TERNDB_ABI_VERSION, NULL, sys_free, NULL, &err_));
    init(); /* a refused init installs nothing */
}

static void test_code_name(void) {
    CHECK(strcmp(terndb_code_name("T1007"), "cancelled") == 0);
    CHECK(strcmp(terndb_code_name("C1001"), "invalid_handle") == 0);
    CHECK(terndb_code_name("Z9999") == NULL);
    CHECK(terndb_code_name("") == NULL);
    CHECK(terndb_code_name(NULL) == NULL);
}

/* ---- §3 the allocator ------------------------------------------------------------------------- */

#define MAX_BLOCKS 200000
struct block { void *p; size_t size, align; };
static struct block blocks[MAX_BLOCKS];
static size_t nblocks, allocs, frees, mismatched;
static pthread_mutex_t blocks_mu = PTHREAD_MUTEX_INITIALIZER;

static void *count_alloc(void *ctx, size_t size, size_t align) {
    CHECK(ctx == (void *)&blocks);
    CHECK(align != 0 && (align & (align - 1)) == 0 && align <= 16384);
    void *p = sys_alloc(NULL, size, align);
    pthread_mutex_lock(&blocks_mu);
    CHECK(nblocks < MAX_BLOCKS);
    blocks[nblocks++] = (struct block){p, size, align};
    allocs++;
    pthread_mutex_unlock(&blocks_mu);
    return p;
}

static void count_free(void *ctx, void *p, size_t size, size_t align) {
    CHECK(ctx == (void *)&blocks);
    pthread_mutex_lock(&blocks_mu);
    size_t i = 0;
    while (i < nblocks && blocks[i].p != p) i++;
    if (i == nblocks || blocks[i].size != size || blocks[i].align != align) mismatched++;
    if (i < nblocks) blocks[i] = blocks[--nblocks];
    frees++;
    pthread_mutex_unlock(&blocks_mu);
    free(p);
}

static void test_allocator_counting(void) {
    if (terndb_debug_checks()) SKIP("a debug build keeps freed handles for good (§2.4 rule 2), so their blocks are never freed");
    EXPECT_OK(terndb_init(TERNDB_ABI_VERSION, count_alloc, count_free, &blocks, &err_));
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
    terndb_result *r = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_result_next(r, &b, &err_));
    terndb_result_free(r);
    terndb_conn_close(c);
    EXPECT_OK(terndb_close(db, &err_));
    terndb_batch_free(b);
    pthread_mutex_lock(&blocks_mu);
    size_t left = nblocks, a = allocs, f = frees, bad = mismatched;
    pthread_mutex_unlock(&blocks_mu);
    if (left != 0 || bad != 0) fprintf(stderr, "allocs %zu frees %zu live %zu mismatched %zu\n", a, f, left, bad);
    CHECK(a > 0);
    CHECK(bad == 0);
    CHECK(left == 0);
    /* No traffic once everything is freed. */
    usleep(50000);
    pthread_mutex_lock(&blocks_mu);
    CHECK(allocs == a && frees == f);
    pthread_mutex_unlock(&blocks_mu);
}

static void test_allocator_null_fails_cleanly(void) {
    /* TODO: un-ignore when sysl gives an allocation failure back to the program: an allocator that
     * answers NULL stops a sysl program at the allocation (sysl packages.md "One heap"), so C2001 and
     * T1008 (c-api.md §3 rule 3) cannot be raised. */
    SKIP("sysl 0.13.6 stops the process when its allocator answers NULL; C2001 cannot be raised");
}

/* ---- §2.4 handles ----------------------------------------------------------------------------- */

static void test_leaks_every_kind(void) {
    init();
    size_t before = terndb_debug_live_handles();
    terndb_config *cfg = config("user", "terndb", NULL);
    terndb_txn_options *to = NULL;
    EXPECT_OK(terndb_txn_options_new(&to, &err_));
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, to, &t, &err_));
    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute_prepared(s, t, NULL, eo, NULL, 0, &r, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_result_next(r, &b, &err_));
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_metrics *m = NULL;
    EXPECT_OK(terndb_metrics_snapshot(db, &m, &err_));
    terndb_op *op = terndb_commit_async(t, q, 1);
    terndb_error *e = NULL;
    CHECK(terndb_config_set(cfg, "nope", 4, "x", 1, &e) == -1 && e != NULL);
    CHECK(terndb_debug_live_handles() > before + 12);

    CHECK(terndb_op_wait(op, -1) == 1);
    terndb_op_free(op);
    terndb_error_free(e);
    terndb_metrics_free(m);
    terndb_type_free(ty);
    terndb_batch_free(b);
    terndb_result_free(r);
    terndb_stmt_free(s);
    terndb_txn_free(t);
    terndb_conn_close(c);
    EXPECT_OK(terndb_close(db, &err_));
    terndb_queue_free(q);
    terndb_exec_options_free(eo);
    terndb_txn_options_free(to);
    terndb_config_free(cfg);
    CHECK(terndb_debug_live_handles() == before);
}

static void test_free_null_is_nothing(void) {
    init();
    terndb_config_free(NULL);
    terndb_conn_close(NULL);
    terndb_txn_options_free(NULL);
    terndb_exec_options_free(NULL);
    terndb_txn_free(NULL);
    terndb_stmt_free(NULL);
    terndb_result_free(NULL);
    terndb_batch_free(NULL);
    terndb_type_free(NULL);
    terndb_error_free(NULL);
    terndb_queue_free(NULL);
    terndb_op_free(NULL);
    terndb_metrics_free(NULL);
    terndb_error *e = NULL;
    CHECK(terndb_close(NULL, &e) == -1 && strcmp(code_of(e), "C1002") == 0);
    terndb_error_free(e);
}

/* Use after free, for the kinds whose calls have an error channel: C1001 in a debug build. */
static void test_use_after_free_every_kind(void) {
    if (!terndb_debug_checks()) SKIP("a release build does not check freed handles (§2.4 rule 2)");
    init();
    terndb_config *cfg = config(NULL, NULL);
    terndb_config_free(cfg);
    EXPECT_CODE("C1001", terndb_config_set(cfg, "user", 4, "x", 1, &err_));

    terndb_txn_options *to = NULL;
    EXPECT_OK(terndb_txn_options_new(&to, &err_));
    terndb_txn_options_free(to);
    EXPECT_CODE("C1001", terndb_txn_options_set_read_only(to, true, &err_));

    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    terndb_exec_options_free(eo);
    EXPECT_CODE("C1001", terndb_exec_options_set_batch_rows(eo, 5, &err_));

    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_txn_free(t);
    EXPECT_CODE("C1001", terndb_commit(t, NULL, &err_));

    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    terndb_stmt_free(s);
    terndb_result *r = NULL;
    EXPECT_CODE("C1001", terndb_execute_prepared(s, NULL, NULL, NULL, NULL, 0, &r, &err_));

    r = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_result_free(r);
    terndb_batch *b = NULL;
    EXPECT_CODE("C1001", terndb_result_next(r, &b, &err_));

    terndb_op *op = terndb_begin_async(c, NULL, &t, NULL, 0);
    CHECK(terndb_op_wait(op, -1) == 1);
    terndb_op_free(op);
    EXPECT_CODE("C1001", terndb_op_error(op, &err_));
    terndb_txn_free(t);

    terndb_conn_close(c);
    EXPECT_CODE("C1001", terndb_begin(c, NULL, &t, &err_));
    EXPECT_OK(terndb_close(db, &err_));
    terndb_conn *c2 = NULL;
    EXPECT_CODE("C1001", terndb_session(db, NULL, &c2, &err_));
    EXPECT_CODE("C1001", terndb_close(db, &err_)); /* §4.3 rule 4: never twice */
}

/* A freed handle's address is never handed out again in a debug build (§2.4 rule 2). */
static void test_freed_address_not_reused(void) {
    if (!terndb_debug_checks()) SKIP("a release build gives freed memory back");
    init();
    enum { N = 2000 };
    static void *seen[N];
    for (int i = 0; i < N; i++) {
        terndb_config *c = NULL;
        EXPECT_OK(terndb_config_new(&c, &err_));
        seen[i] = c;
        for (int j = 0; j < i; j++) CHECK(seen[j] != (void *)c);
        terndb_config_free(c);
    }
}

static void test_wrong_kind_every_kind(void) {
    if (!terndb_debug_checks()) SKIP("a release build does not check a handle's kind (§2.4 rule 2)");
    init();
    terndb_config *cfg = config(NULL, NULL);
    terndb_txn_options *to = NULL;
    EXPECT_OK(terndb_txn_options_new(&to, &err_));
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_conn *c2 = NULL;
    EXPECT_CODE("C1001", terndb_session((terndb_db *)cfg, NULL, &c2, &err_));
    EXPECT_CODE("C1001", terndb_config_set((terndb_config *)db, "user", 4, "x", 1, &err_));
    EXPECT_CODE("C1001", terndb_begin((terndb_conn *)db, NULL, &t, &err_));
    EXPECT_CODE("C1001", terndb_commit((terndb_txn *)c, NULL, &err_));
    EXPECT_CODE("C1001", terndb_txn_options_set_read_only((terndb_txn_options *)q, true, &err_));
    EXPECT_CODE("C1001", terndb_exec_options_set_batch_rows((terndb_exec_options *)to, 1, &err_));
    EXPECT_CODE("C1001", terndb_open("", 0, (terndb_config *)to, &db, &err_));
    terndb_result *r = NULL;
    EXPECT_CODE("C1001", terndb_execute(c, (terndb_txn *)cfg, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, &r, &err_));
    EXPECT_CODE("C1001", terndb_execute(c, NULL, NULL, (terndb_exec_options *)to, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0,
                                        &r, &err_));
    terndb_metrics *m = NULL;
    EXPECT_CODE("C1001", terndb_metrics_snapshot((terndb_db *)c, &m, &err_));
    terndb_batch *b = NULL;
    EXPECT_CODE("C1001", terndb_result_next((terndb_result *)t, &b, &err_));
    EXPECT_CODE("C1001", terndb_op_error((terndb_op *)q, &err_));
    EXPECT_CODE("C1001", terndb_batch_decode((terndb_type *)cfg, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
}

/* A function with no error channel aborts a debug build on a freed or wrong-kind handle: each of
 * these runs in a child and must end by a signal, naming the code on stderr. */
static void abort_batch_after_free(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
    terndb_batch_free(b);
    (void)terndb_batch_rows(b);
}
static void abort_error_freed_twice(void) {
    init();
    terndb_error *e = NULL;
    terndb_config *cfg = NULL;
    CHECK(terndb_config_set(cfg, "user", 4, "x", 1, &e) == -1);
    terndb_error_free(e);
    terndb_error_free(e);
}
static void abort_error_accessor_after_free(void) {
    init();
    terndb_error *e = NULL;
    CHECK(terndb_config_set(NULL, "user", 4, "x", 1, &e) == -1);
    terndb_error_free(e);
    (void)terndb_error_entry_count(e);
}
static void abort_type_free_borrowed(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_type_free((terndb_type *)terndb_type_element(ty));
}
static void abort_type_freed_twice(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_type_free(ty);
    terndb_type_free(ty);
}
static void abort_queue_fd_after_free(void) {
    init();
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    terndb_queue_free(q);
    (void)terndb_queue_fd(q);
}
static void abort_metrics_after_free(void) {
    init();
    terndb_db *db = open_db();
    terndb_metrics *m = NULL;
    EXPECT_OK(terndb_metrics_snapshot(db, &m, &err_));
    terndb_metrics_free(m);
    (void)terndb_metrics_count(m);
}
static void abort_op_poll_after_free(void) {
    init();
    terndb_op *op = terndb_open_async("", 0, NULL, NULL, 0);
    terndb_op_wait(op, -1);
    terndb_op_free(op);
    (void)terndb_op_poll(op);
}
static void abort_column_wrong_kind(void) {
    init();
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    (void)terndb_column_len((const terndb_column *)q);
}
static void abort_getter_wrong_representation(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
    const terndb_column *x = terndb_column_field(terndb_batch_root(b), 0);
    (void)terndb_column_get_i64(x, 0); /* x is int32 */
}
static void abort_getter_index_out_of_range(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
    const terndb_column *x = terndb_column_field(terndb_batch_root(b), 0);
    (void)terndb_column_get_i32(x, 3);
}
static void abort_batch_column_out_of_range(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
    (void)terndb_batch_column(b, terndb_batch_column_count(b));
}
static void abort_error_entry_out_of_range(void) {
    init();
    terndb_error *e = NULL;
    CHECK(terndb_config_set(NULL, "user", 4, "x", 1, &e) == -1);
    (void)terndb_error_code(e, 1);
}
static void abort_take_wrong_kind(void) {
    init();
    terndb_op *op = terndb_open_async("", 0, NULL, NULL, 0);
    terndb_op_wait(op, -1);
    (void)terndb_op_take_result(op);
}
static void abort_null_getter(void) {
    init();
    (void)terndb_batch_rows(NULL);
}

/* ---- §2.3 threads ----------------------------------------------------------------------------- */

struct hammer { terndb_conn *c; int ok, busy, other; };

static void *hammer_main(void *arg) {
    struct hammer *h = arg;
    for (int i = 0; i < 300; i++) {
        terndb_error *e = NULL;
        int32_t st = 0;
        int r = terndb_transaction_status(h->c, 1, &st, NULL, &e);
        if (r == 0) h->ok++;
        else if (strcmp(code_of(e), "C1004") == 0) h->busy++;
        else h->other++;
        terndb_error_free(e);
    }
    return NULL;
}

static void test_conn_concurrent_use(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    struct hammer a = {c, 0, 0, 0}, b = {c, 0, 0, 0};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, hammer_main, &a);
    pthread_create(&tb, NULL, hammer_main, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    /* Every call either ran or was refused with C1004; some were refused, and all ran alone. */
    CHECK(a.other == 0 && b.other == 0);
    CHECK(a.ok + a.busy == 300 && b.ok + b.busy == 300);
    CHECK(a.ok > 0 && b.ok > 0);
    CHECK(a.busy + b.busy > 0);
    /* The connection still serves afterwards. */
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
}

static void test_conn_moves_between_threads(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    struct hammer a = {c, 0, 0, 0};
    pthread_t ta;
    pthread_create(&ta, NULL, hammer_main, &a);
    pthread_join(ta, NULL);
    CHECK(a.ok == 300 && a.busy == 0);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
}

struct sink_state {
    terndb_conn *c;
    terndb_queue *q;
    terndb_op *op;
    pthread_t thread;
    int calls, threads_differ, wrong_thread, blocked, records, saw_open, bad_level;
};
static struct sink_state sink;

static void sink_fn(void *ctx, const terndb_log_record *rec) {
    struct sink_state *s = ctx;
    if (s->calls == 0) s->thread = pthread_self();
    else if (!pthread_equal(s->thread, pthread_self())) s->threads_differ++;
    s->calls++;
    s->records++;
    int32_t level = terndb_log_record_level(rec);
    if (level < TERNDB_LOG_ERROR || level > TERNDB_LOG_TRACE) s->bad_level++;
    const char *m;
    size_t ml;
    terndb_log_record_message(rec, &m, &ml);
    if (ml == 15 && memcmp(m, "database opened", 15) == 0) s->saw_open++;
    if (s->c) {
        int64_t t0 = now_ms();
        terndb_txn *t = NULL;
        terndb_error *e = NULL;
        if (terndb_begin(s->c, NULL, &t, &e) == -1 && strcmp(code_of(e), "C1003") == 0) s->wrong_thread++;
        terndb_error_free(e);
        if (!terndb_debug_checks()) {
            if (s->op && terndb_op_wait(s->op, -1) == -1) s->wrong_thread++;
            uint64_t tags[4];
            if (s->q && terndb_queue_wait(s->q, tags, 4, -1) == 0) s->wrong_thread++;
        }
        if (now_ms() - t0 > 1000) s->blocked++;
    }
}

static void test_sink_reentry(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    memset(&sink, 0, sizeof sink);
    sink.c = c;
    EXPECT_OK(terndb_queue_new(&sink.q, &err_));
    sink.op = terndb_begin_async(c, NULL, &(terndb_txn *){NULL}, NULL, 0);
    EXPECT_OK(terndb_set_log_sink(TERNDB_LOG_TRACE, sink_fn, &sink, &err_));
    terndb_db *db2 = open_db(); /* emits "database opened" */
    terndb_conn *c2 = session(db2);
    terndb_conn_close(c2);
    for (int i = 0; i < 200 && sink.calls < 2; i++) usleep(10000);
    EXPECT_OK(terndb_set_log_sink(0, NULL, NULL, &err_));
    CHECK(sink.calls >= 1);
    CHECK(sink.saw_open >= 1);
    CHECK(sink.threads_differ == 0);
    CHECK(!pthread_equal(sink.thread, pthread_self()));
    CHECK(sink.wrong_thread == sink.calls * (terndb_debug_checks() ? 1 : 3));
    CHECK(sink.blocked == 0);
    CHECK(sink.bad_level == 0);
}

static void test_sink_levels_and_removal(void) {
    init();
    memset(&sink, 0, sizeof sink);
    EXPECT_CODE("C1002", terndb_set_log_sink(6, sink_fn, &sink, &err_));
    EXPECT_CODE("C1002", terndb_set_log_sink(-1, sink_fn, &sink, &err_));
    /* Level 0 delivers nothing; "database opened" is INFO, above WARN. */
    EXPECT_OK(terndb_set_log_sink(0, sink_fn, &sink, &err_));
    terndb_db *db = open_db();
    EXPECT_OK(terndb_set_log_sink(TERNDB_LOG_WARN, sink_fn, &sink, &err_));
    terndb_db *db2 = open_db();
    usleep(100000);
    CHECK(sink.calls == 0);
    EXPECT_OK(terndb_set_log_sink(TERNDB_LOG_INFO, sink_fn, &sink, &err_));
    terndb_db *db3 = open_db();
    for (int i = 0; i < 200 && sink.calls < 1; i++) usleep(10000);
    CHECK(sink.calls == 1);
    /* Once removed, the old sink is not called again. */
    EXPECT_OK(terndb_set_log_sink(0, NULL, NULL, &err_));
    int before = sink.calls;
    terndb_db *db4 = open_db();
    usleep(100000);
    CHECK(sink.calls == before);
    (void)db; (void)db2; (void)db3; (void)db4;
}

static void field_of(const terndb_log_record *r, const char *key, char *out, size_t cap) {
    out[0] = 0;
    for (size_t k = 0; k < terndb_log_record_field_count(r); k++) {
        const char *kp, *vp;
        size_t kl, vl;
        terndb_log_record_field(r, k, &kp, &kl, &vp, &vl);
        if (kl == strlen(key) && memcmp(kp, key, kl) == 0 && vl < cap) {
            memcpy(out, vp, vl);
            out[vl] = 0;
        }
    }
}

static int fields_ok;
static void sink_fields(void *ctx, const terndb_log_record *r) {
    (void)ctx;
    char db[32];
    field_of(r, "database", db, sizeof db);
    const char *t;
    size_t tl;
    terndb_log_record_target(r, &t, &tl);
    if (strlen(db) > 0 && tl > 0 && terndb_log_record_time(r) > 1700000000000000LL) fields_ok++;
}

static void test_log_record_fields(void) {
    init();
    EXPECT_OK(terndb_set_log_sink(TERNDB_LOG_TRACE, sink_fields, NULL, &err_));
    terndb_db *db = open_db();
    terndb_conn *c = session(db);
    terndb_conn_close(c);
    for (int i = 0; i < 200 && fields_ok < 3; i++) usleep(10000);
    EXPECT_OK(terndb_set_log_sink(0, NULL, NULL, &err_));
    CHECK(fields_ok >= 3); /* opened, session opened, session closed */
}

/* ---- §1 rule 6 inputs are copied -------------------------------------------------------------- */

static void test_inputs_scribbled(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    uint8_t *env = malloc(ENV_SELECT_T_LEN);
    memcpy(env, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_op *op = terndb_execute_async(c, NULL, NULL, NULL, env, ENV_SELECT_T_LEN, NULL, 0, NULL, 0);
    memset(env, 0xA5, ENV_SELECT_T_LEN);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_result *r = terndb_op_take_result(op);
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    CHECK(terndb_batch_rows(b) == 3);
    /* A synchronous call, and a configuration's text. */
    memcpy(env, ENV_COUNT_T, ENV_COUNT_T_LEN);
    terndb_result *r2 = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, NULL, env, ENV_COUNT_T_LEN, NULL, 0, &r2, &err_));
    memset(env, 0, ENV_COUNT_T_LEN);
    terndb_batch *b2 = NULL;
    CHECK(terndb_result_next(r2, &b2, &(terndb_error *){NULL}) == 1);
    CHECK(terndb_column_get_i64(terndb_column_field(terndb_batch_root(b2), 0), 0) == 3);
    char key[16] = "user", value[16] = "terndb";
    terndb_config *cfg = NULL;
    EXPECT_OK(terndb_config_new(&cfg, &err_));
    EXPECT_OK(terndb_config_set(cfg, key, 4, value, 6, &err_));
    memset(key, 'z', sizeof key);
    memset(value, 'z', sizeof value);
    terndb_conn *c2 = NULL;
    EXPECT_OK(terndb_session(db, cfg, &c2, &err_));
    free(env);
}

/* ---- §2.2 rule 3 a batch outlives everything -------------------------------------------------- */

static void test_batch_outlives_db(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_result *r = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    const uint8_t *p;
    size_t n;
    terndb_batch_bytes(b, &p, &n);
    uint8_t *copy = malloc(n);
    memcpy(copy, p, n);
    terndb_result_free(r);
    terndb_conn_close(c);
    EXPECT_OK(terndb_close(db, &err_));
    const uint8_t *p2;
    size_t n2;
    terndb_batch_bytes(b, &p2, &n2);
    CHECK(p2 == p && n2 == n && memcmp(p2, copy, n) == 0);
    const terndb_column *name = terndb_column_field_by_name(terndb_batch_root(b), "name", 4);
    size_t len;
    const uint8_t *s = terndb_column_bytes(name, 1, &len);
    CHECK(len == 2 && memcmp(s, "bo", 2) == 0);
    terndb_batch_free(b);
    free(copy);
}

static void test_borrowed_pointers_stable(void) {
    init();
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN, &b, &err_));
    terndb_type_free(ty); /* §12.1 rule 16: the batch holds its own copy */
    const terndb_column *root = terndb_batch_root(b);
    const terndb_column *name = terndb_column_field_by_name(root, "name", 4);
    size_t len;
    const uint8_t *s = terndb_column_bytes(name, 0, &len);
    const uint8_t *tb;
    size_t tl;
    terndb_type_bytes(terndb_batch_type(b), &tb, &tl);
    CHECK(tl == GOLD_V15_TYPE_LEN && memcmp(tb, GOLD_V15_TYPE, tl) == 0);
    for (int i = 0; i < 100; i++) {
        terndb_type *t2 = NULL;
        EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &t2, &err_));
        terndb_batch *b2 = NULL;
        EXPECT_OK(terndb_batch_decode(t2, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN, &b2, &err_));
        terndb_batch_free(b2);
        terndb_type_free(t2);
    }
    size_t len2;
    CHECK(terndb_column_bytes(name, 0, &len2) == s && len2 == len && memcmp(s, "ann", 3) == 0);
    CHECK(memcmp(tb, GOLD_V15_TYPE, tl) == 0);
    terndb_batch_free(b);
}

/* ---- §8 golden vectors and the walk ------------------------------------------------------------ */

static void check_golden(terndb_conn *c, const uint8_t *env, size_t env_len, const uint8_t *gold, size_t gold_len,
                         const uint8_t *type, size_t type_len) {
    terndb_result *r = run(c, NULL, env, env_len);
    const uint8_t *tb;
    size_t tl;
    terndb_result_type_bytes(r, &tb, &tl);
    CHECK(tl == type_len && memcmp(tb, type, tl) == 0);
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    const uint8_t *p;
    size_t n;
    terndb_batch_bytes(b, &p, &n);
    if (n != gold_len || memcmp(p, gold, n) != 0) {
        fprintf(stderr, "batch differs from the golden vector: %zu octets, golden %zu\n", n, gold_len);
        for (size_t i = 0; i < n; i++) fprintf(stderr, "%02X%s", p[i], (i + 1) % 16 ? " " : "\n");
        fprintf(stderr, "\n");
        exit(1);
    }
    CHECK(((uintptr_t)p & 7) == 0);
    terndb_batch *end = NULL;
    CHECK(terndb_result_next(r, &end, &(terndb_error *){NULL}) == 0);
    terndb_batch_free(b);
    terndb_result_free(r);
}

static void test_golden_vectors_embedded(void) {
    init();
    terndb_db *db = open_db();
    terndb_conn *c = session(db);
    check_golden(c, ENV_V15, ENV_V15_LEN, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN, GOLD_V15_TYPE, GOLD_V15_TYPE_LEN);
    check_golden(c, ENV_V16, ENV_V16_LEN, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, GOLD_V16_TYPE, GOLD_V16_TYPE_LEN);
    check_golden(c, ENV_V16_NO_NULL, ENV_V16_NO_NULL_LEN, GOLD_V16_NO_NULL_BATCH, GOLD_V16_NO_NULL_BATCH_LEN, GOLD_V16_TYPE,
                 GOLD_V16_TYPE_LEN);
}

static void test_golden_vectors_network(void) { SKIP("this build has no network mode (terndb_connect is C1007)"); }

static terndb_batch *decode(const uint8_t *type, size_t tlen, const uint8_t *bytes, size_t len) {
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(type, tlen, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_OK(terndb_batch_decode(ty, bytes, len, &b, &err_));
    terndb_type_free(ty);
    return b;
}

static int text_is(const terndb_column *c, size_t i, const char *want) {
    size_t len;
    const uint8_t *p = terndb_column_bytes(c, i, &len);
    return len == strlen(want) && memcmp(p, want, len) == 0;
}

/* §6.6 walked from C as §8.3's informative note walks it. */
static void test_decode_vectors(void) {
    init();
    terndb_batch *b = decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN);
    CHECK(terndb_batch_rows(b) == 2);
    CHECK(terndb_batch_tuple(b) == 0);
    const terndb_column *root = terndb_batch_root(b);
    CHECK(root == terndb_batch_column(b, 0));
    CHECK(terndb_column_kind(root) == TERNDB_COL_RECORD);
    CHECK(terndb_column_field_count(root) == 5);
    const terndb_column *id = terndb_column_field_by_name(root, "id", 2);
    CHECK(id == terndb_column_field(root, 0));
    CHECK(terndb_column_kind(id) == TERNDB_COL_FIXED);
    CHECK(terndb_column_get_i64(id, 0) == 1 && terndb_column_get_i64(id, 1) == 2);
    size_t w;
    const uint8_t *raw = terndb_column_fixed(id, &w);
    CHECK(w == 8 && ((const int64_t *)(const void *)raw)[1] == 2);
    const terndb_column *name = terndb_column_field_by_name(root, "name", 4);
    CHECK(terndb_column_kind(name) == TERNDB_COL_VARBIN);
    CHECK(text_is(name, 0, "ann") && text_is(name, 1, "bo"));
    CHECK(terndb_column_get_i64(terndb_column_field_by_name(root, "post_count", 10), 0) == 2);
    CHECK(terndb_column_field_by_name(root, "nope", 4) == NULL);
    CHECK(terndb_column_field_by_name(root, "na", 2) == NULL);

    const terndb_column *status = terndb_column_field_by_name(root, "status", 6);
    CHECK(terndb_column_kind(status) == TERNDB_COL_UNION);
    CHECK(terndb_column_variant_count(status) == 2);
    CHECK(terndb_column_union_tag(status, 0) == 0 && terndb_column_union_tag(status, 1) == 1);
    const terndb_column *banned = terndb_column_variant(status, 1);
    size_t at = terndb_column_union_offset(status, 1);
    CHECK(text_is(terndb_column_field_by_name(banned, "reason", 6), at, "spam"));

    const terndb_column *posts = terndb_column_field_by_name(root, "posts", 5);
    CHECK(terndb_column_kind(posts) == TERNDB_COL_RELATION);
    size_t s, e;
    terndb_column_range(posts, 0, &s, &e);
    CHECK(s == 0 && e == 2);
    terndb_column_range(posts, 1, &s, &e);
    CHECK(s == 2 && e == 2);
    const terndb_column *p = terndb_column_children(posts);
    CHECK(text_is(terndb_column_field_by_name(p, "title", 5), 1, "yo"));
    const terndb_column *comments = terndb_column_field_by_name(p, "comments", 8);
    terndb_column_range(comments, 0, &s, &e);
    CHECK(s == 0 && e == 1);
    terndb_column_range(comments, 1, &s, &e);
    CHECK(s == 1 && e == 1);
    const terndb_column *cm = terndb_column_children(comments);
    CHECK(terndb_column_get_i64(terndb_column_field(cm, 0), 0) == 100);
    CHECK(text_is(terndb_column_field(cm, 1), 0, "ok"));

    /* Types reached from the batch. */
    const terndb_type *rt = terndb_batch_type(b);
    CHECK(terndb_type_kind(rt) == TERNDB_TYPE_LIST && !terndb_type_nullable(rt));
    const terndb_type *et = terndb_type_element(rt);
    CHECK(terndb_type_kind(et) == TERNDB_TYPE_RECORD && terndb_type_member_count(et) == 5);
    const char *mn;
    size_t ml;
    terndb_type_member_name(et, 3, &mn, &ml);
    CHECK(ml == 6 && memcmp(mn, "status", 6) == 0);
    CHECK(terndb_type_kind(terndb_type_member_type(et, 3)) == TERNDB_TYPE_UNION);
    CHECK(terndb_type_kind(terndb_column_type(id)) == TERNDB_TYPE_INT64);
    /* root; id, name, post_count; status, active, banned, reason; posts, post, id, title, comments,
     * comment, id, body. */
    CHECK(terndb_batch_column_count(b) == 16);
    terndb_batch_free(b);

    /* §6.7: a nullable column. */
    b = decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN);
    const terndb_column *x = terndb_column_field(terndb_batch_root(b), 0);
    CHECK(terndb_column_len(x) == 3 && terndb_column_null_count(x) == 1);
    CHECK(!terndb_column_is_null(x, 0) && terndb_column_is_null(x, 1) && !terndb_column_is_null(x, 2));
    CHECK(terndb_column_get_i32(x, 0) == 7 && terndb_column_get_i32(x, 1) == 0 && terndb_column_get_i32(x, 2) == 9);
    CHECK(terndb_type_nullable(terndb_column_type(x)) && terndb_type_kind(terndb_column_type(x)) == TERNDB_TYPE_INT32);
    CHECK(terndb_column_buffer_count(x) == 2);
    const uint8_t *vb;
    size_t vl;
    terndb_column_buffer(x, 0, &vb, &vl);
    CHECK(vl == 1 && vb[0] == 0x05);
    terndb_batch_free(b);
    b = decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, GOLD_V16_NO_NULL_BATCH, GOLD_V16_NO_NULL_BATCH_LEN);
    x = terndb_column_field(terndb_batch_root(b), 0);
    CHECK(terndb_column_null_count(x) == 0 && !terndb_column_is_null(x, 1));
    terndb_column_buffer(x, 0, &vb, &vl);
    CHECK(vl == 0);
    terndb_batch_free(b);
}

static void expect_bad_batch(const uint8_t *bytes, size_t len) {
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    terndb_error *e = NULL;
    CHECK(terndb_batch_decode(ty, bytes, len, &b, &e) == -1);
    CHECK(strcmp(code_of(e), "W1008") == 0);
    CHECK(b == NULL);
    terndb_error_free(e);
    terndb_type_free(ty);
}

static void test_decode_corrupt(void) {
    init();
    uint8_t buf[512];
    CHECK(GOLD_V15_BATCH_LEN <= sizeof buf);
    /* Too short, and a trailing octet. */
    expect_bad_batch(GOLD_V15_BATCH, GOLD_V15_BATCH_LEN - 8);
    memcpy(buf, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN);
    buf[GOLD_V15_BATCH_LEN] = 0;
    expect_bad_batch(buf, GOLD_V15_BATCH_LEN + 1);
    /* A non-zero padding octet: the last octet of the payload is padding (§6.4). */
    memcpy(buf, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN);
    buf[GOLD_V15_BATCH_LEN - 1] ^= 0xFF;
    expect_bad_batch(buf, GOLD_V15_BATCH_LEN);
    /* A wrong node count. */
    memcpy(buf, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN);
    buf[1] ^= 0x01;
    expect_bad_batch(buf, GOLD_V15_BATCH_LEN);
    /* Empty. */
    expect_bad_batch(GOLD_V15_BATCH, 0);
    /* A non-relation result type is C1002 (§12.1 rule 16). */
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    EXPECT_CODE("C1002", terndb_batch_decode(terndb_type_element(ty), GOLD_V15_BATCH, GOLD_V15_BATCH_LEN, &b, &err_));
    terndb_type_free(ty);
}

static void test_type_decode_errors(void) {
    init();
    terndb_type *ty = NULL;
    terndb_error *e = NULL;
    CHECK(terndb_type_decode(GOLD_V15_TYPE, 0, &ty, &e) == -1 && code_of(e)[0] == 'Q');
    terndb_error_free(e);
    e = NULL;
    CHECK(terndb_type_decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN - 1, &ty, &e) == -1 && code_of(e)[0] == 'Q');
    terndb_error_free(e);
    /* A named type reference (kind 32) is Q1002 (§12.1 rule 20). */
    const uint8_t named[] = {0x20, 0x04, 0x01, 'a', 0x01, 'b'};
    EXPECT_CODE("Q1002", terndb_type_decode(named, sizeof named, &ty, &err_));
    CHECK(ty == NULL);
}

/* Each node's buffers read raw (§8.3 rule 3) agree with the getters: two decoders of one batch. */
static void test_raw_buffers_agree(void) {
    init();
    terndb_batch *b = decode(GOLD_V15_TYPE, GOLD_V15_TYPE_LEN, GOLD_V15_BATCH, GOLD_V15_BATCH_LEN);
    const terndb_column *root = terndb_batch_root(b);
    const terndb_column *name = terndb_column_field(root, 1);
    const uint8_t *off, *data;
    size_t offl, datal;
    CHECK(terndb_column_buffer_count(name) == 2);
    terndb_column_buffer(name, 0, &off, &offl);
    terndb_column_buffer(name, 1, &data, &datal);
    for (size_t i = 0; i < terndb_column_len(name); i++) {
        uint32_t a, z;
        memcpy(&a, off + 4 * i, 4);
        memcpy(&z, off + 4 * i + 4, 4);
        size_t len;
        const uint8_t *p = terndb_column_bytes(name, i, &len);
        CHECK(p == data + a && len == z - a);
    }
    const uint8_t *pb, *bb;
    size_t pl, bl;
    terndb_batch_bytes(b, &bb, &bl);
    terndb_column_buffer(terndb_column_field(root, 0), 0, &pb, &pl);
    CHECK(pb >= bb && pb + pl <= bb + bl && pl == 16);
    terndb_batch_free(b);
}

static void test_random_nested_walk(void) {
    SKIP("no randomised nested-result generator yet; the walk is checked on V15/V16 and against raw buffers");
}

/* ---- §6 the completion queue and operations --------------------------------------------------- */

static void test_queue_exactly_once(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    enum { N = 100 };
    terndb_op *ops[N];
    for (int i = 0; i < N; i++)
        ops[i] = terndb_execute_async(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, q, 1000 + i);
    int seen[N] = {0};
    int got = 0;
    while (got < N) {
        uint64_t tags[16];
        size_t n = terndb_queue_wait(q, tags, 16, 5000);
        CHECK(n > 0);
        for (size_t k = 0; k < n; k++) {
            CHECK(tags[k] >= 1000 && tags[k] < 1000 + N);
            seen[tags[k] - 1000]++;
            got++;
        }
    }
    usleep(50000);
    uint64_t extra[4];
    CHECK(terndb_queue_drain(q, extra, 4) == 0);
    for (int i = 0; i < N; i++) {
        CHECK(seen[i] == 1);
        CHECK(terndb_op_poll(ops[i]) == 1);
        terndb_result *r = terndb_op_take_result(ops[i]);
        CHECK(r != NULL);
        CHECK(terndb_op_take_result(ops[i]) == NULL); /* §6.2 rule 8: once */
        terndb_result_free(r);
        terndb_op_free(ops[i]);
    }
}

static int readable(int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

static void test_queue_fd_level(void) {
    init();
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    int fd = terndb_queue_fd(q);
    CHECK(fd >= 0);
    CHECK(!readable(fd));
    terndb_op *a = terndb_open_async("", 0, NULL, q, 1); /* fails: disk mode is the default */
    terndb_op *b = terndb_open_async("", 0, NULL, q, 2);
    CHECK(terndb_op_wait(a, -1) == 1 && terndb_op_wait(b, -1) == 1);
    CHECK(readable(fd));
    CHECK(readable(fd)); /* level-triggered: still readable */
    uint64_t tags[4];
    CHECK(terndb_queue_drain(q, tags, 1) == 1);
    CHECK(readable(fd)); /* one left */
    CHECK(terndb_queue_drain(q, tags, 4) == 1);
    CHECK(!readable(fd));
    CHECK(terndb_queue_drain(q, tags, 4) == 0);
    CHECK(terndb_queue_wait(q, tags, 4, 20) == 0); /* timeout */
    CHECK(terndb_queue_drain(q, NULL, 4) == 0);
    CHECK(terndb_queue_drain(q, tags, 0) == 0);
    terndb_op_free(a);
    terndb_op_free(b);
    terndb_queue_free(q);
}

static void test_queue_full(void) {
    /* The queue has no bound: completions that are never drained are all kept, each once. */
    init();
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    enum { N = 10000 };
    for (int i = 0; i < N; i++) {
        terndb_op *op = terndb_connect_async("bad", 3, NULL, q, (uint64_t)i);
        CHECK(terndb_op_poll(op) == 1);
        terndb_op_free(op);
    }
    static uint64_t tags[N + 1];
    CHECK(terndb_queue_drain(q, tags, N + 1) == N);
    for (int i = 0; i < N; i++) CHECK(tags[i] == (uint64_t)i);
    CHECK(!readable(terndb_queue_fd(q)));
    terndb_queue_free(q);
}

static void test_op_free_pending(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    RUN(c, NULL, ENV_CREATE_BIG);
    RUN(c, NULL, ENV_INSERT_BIG_A);
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    /* A pending execute freed: its tag never appears, and the connection goes on. */
    terndb_op *op = terndb_execute_async(c, NULL, NULL, NULL, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, q, 7);
    terndb_op_free(op);
    terndb_op *ok = terndb_execute_async(c, NULL, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, q, 8);
    uint64_t tags[4];
    size_t n = 0;
    int64_t t0 = now_ms();
    while (now_ms() - t0 < 3000) {
        n += terndb_queue_wait(q, tags + n, 4 - n, 100);
        if (n > 0 && tags[n - 1] == 8) break;
    }
    for (size_t i = 0; i < n; i++) CHECK(tags[i] != 7);
    CHECK(n >= 1 && tags[n - 1] == 8);
    terndb_op_free(ok);
    /* A queue freed with an operation pending: permitted, and the completion is discarded. */
    terndb_queue *q2 = NULL;
    EXPECT_OK(terndb_queue_new(&q2, &err_));
    terndb_op *late = terndb_execute_async(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, q2, 9);
    terndb_queue_free(q2);
    CHECK(terndb_op_wait(late, -1) == 1);
    EXPECT_OK(terndb_op_error(late, &err_));
    terndb_op_free(late);
    /* A pending begin freed: the transaction handle stays valid to free (§6.2 rule 11). */
    terndb_txn *t = NULL;
    terndb_op *b = terndb_begin_async(c, NULL, &t, NULL, 0);
    terndb_op_free(b);
    CHECK(t != NULL);
    terndb_txn_free(t);
    terndb_queue_free(q);
}

static void test_op_error_pending(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    RUN(c, NULL, ENV_CREATE_BIG);
    RUN(c, NULL, ENV_INSERT_BIG_A);
    terndb_result *r = run(c, NULL, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN);
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    terndb_batch_free(b);
    /* A next on an unpulled-window result may still be pending; poll, then error is C1005 or done. */
    terndb_op *op = terndb_commit_async(NULL, NULL, 0);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    CHECK(terndb_op_commit_seq(op, &(uint64_t){0}) == -1);
    terndb_op_free(op);
    /* An operation still pending: its error is C1005. */
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_op *slow = terndb_result_next_async(r, NULL, 0);
    terndb_error *e = NULL;
    int rc = terndb_op_error(slow, &e);
    CHECK(rc == 0 || (rc == -1 && strcmp(code_of(e), "C1005") == 0));
    terndb_error_free(e);
    terndb_op_free(slow);
    terndb_result_free(r);
    terndb_txn_free(t);
}

static void test_pipeline_txn(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    terndb_txn *t = NULL;
    terndb_op *ops[4];
    ops[0] = terndb_begin_async(c, NULL, &t, q, 0);
    CHECK(t != NULL);
    CHECK(terndb_txn_state(t) == TERNDB_TXN_PENDING || terndb_txn_state(t) == TERNDB_TXN_OPEN);
    ops[1] = terndb_execute_async(c, t, NULL, NULL, ENV_INSERT_4, ENV_INSERT_4_LEN, NULL, 0, q, 1);
    ops[2] = terndb_execute_async(c, t, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, q, 2);
    ops[3] = terndb_commit_async(t, q, 3);
    uint64_t order[4];
    size_t n = 0;
    while (n < 4) {
        size_t k = terndb_queue_wait(q, order + n, 4 - n, 5000);
        CHECK(k > 0);
        n += k;
    }
    for (int i = 0; i < 4; i++) {
        CHECK(order[i] == (uint64_t)i); /* one transaction: submission order (§6.2 rule 3) */
        EXPECT_OK(terndb_op_error(ops[i], &err_));
    }
    terndb_result *r = terndb_op_take_result(ops[2]);
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    CHECK(terndb_column_get_i64(terndb_column_field(terndb_batch_root(b), 0), 0) == 4);
    uint64_t seq = 0;
    CHECK(terndb_op_commit_seq(ops[3], &seq) == 1 && seq > 0);
    CHECK(terndb_txn_state(t) == TERNDB_TXN_CLOSED);
    CHECK(terndb_txn_id(t) != 0);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 4);
}

static void test_pipeline_begin_fails(void) {
    init();
    terndb_db *db = open_db();
    terndb_config *cfg = config("max_open_txns", "1", NULL);
    terndb_conn *c = NULL;
    EXPECT_OK(terndb_session(db, cfg, &c, &err_));
    RUN(c, NULL, ENV_CREATE_T);
    terndb_txn *t1 = NULL, *t2 = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t1, &err_));
    terndb_op *b = terndb_begin_async(c, NULL, &t2, NULL, 0);
    terndb_op *x = terndb_execute_async(c, t2, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, NULL, 0);
    terndb_op *k = terndb_commit_async(t2, NULL, 0);
    CHECK(terndb_op_wait(b, -1) == 1 && terndb_op_wait(x, -1) == 1 && terndb_op_wait(k, -1) == 1);
    EXPECT_CODE("W2016", terndb_op_error(b, &err_));
    /* Every request queued on it fails with the begin's error (§5.3 rule 2). */
    EXPECT_CODE("W2016", terndb_op_error(x, &err_));
    EXPECT_CODE("W2016", terndb_op_error(k, &err_));
    CHECK(terndb_txn_state(t2) == TERNDB_TXN_CLOSED);
    CHECK(terndb_txn_isolation(t2) == -1 && terndb_txn_id(t2) == 0);
}

static void test_cancel_query(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    RUN(c, NULL, ENV_CREATE_BIG);
    RUN(c, NULL, ENV_INSERT_BIG_A);
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(eo, 4096, &err_));
    EXPECT_OK(terndb_exec_options_set_batch_rows(eo, 64, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, &r, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    terndb_batch_free(b);
    terndb_result_cancel(r);
    terndb_error *e = NULL;
    int k;
    while ((k = terndb_result_next(r, &b, &e)) == 1) terndb_batch_free(b);
    CHECK(k == -1);
    CHECK(strcmp(code_of(e), "T1007") == 0);
    terndb_error_free(e);
    /* After the end, the same answer again (§8.1 rule 1). */
    e = NULL;
    CHECK(terndb_result_next(r, &b, &e) == -1 && strcmp(code_of(e), "T1007") == 0);
    terndb_error_free(e);
    terndb_result_cancel(r); /* after the end: nothing */
    EXPECT_CODE("C1005", terndb_result_counts(r, &(const uint64_t *){NULL}, &(size_t){0}, &err_));
    terndb_result_free(r);
    /* Cancelled through its operation, before it produced anything. */
    terndb_op *op = terndb_execute_async(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, NULL, 0);
    terndb_op_cancel(op);
    CHECK(terndb_op_wait(op, -1) == 1);
    terndb_error *oe = NULL;
    if (terndb_op_error(op, &oe) == 0) {
        r = terndb_op_take_result(op);
        while ((k = terndb_result_next(r, &b, &e)) == 1) terndb_batch_free(b);
        CHECK(k == -1 && strcmp(code_of(e), "T1007") == 0);
        terndb_error_free(e);
        terndb_result_free(r);
    } else {
        CHECK(strcmp(code_of(oe), "T1007") == 0);
        terndb_error_free(oe);
    }
    terndb_op_free(op);
    /* Cancelling a completed operation changes nothing. */
    op = terndb_execute_async(c, NULL, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, NULL, 0);
    CHECK(terndb_op_wait(op, -1) == 1);
    terndb_op_cancel(op);
    EXPECT_OK(terndb_op_error(op, &err_));
    r = terndb_op_take_result(op);
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    CHECK(terndb_column_get_i64(terndb_column_field(terndb_batch_root(b), 0), 0) == 3);
}

static void test_cancel_commit_ignored(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    RUN(c, t, ENV_INSERT_4);
    terndb_op *op = terndb_commit_async(t, NULL, 0);
    terndb_op_cancel(op);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_OK(terndb_op_error(op, &err_));
    CHECK(terndb_op_commit_seq(op, &(uint64_t){0}) == 1);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 4);
}

static void test_prefetch_bound(void) {
    SKIP("the octets an embedded request holds unpulled are not observable through the ABI");
}

static void test_implicit_commit_fails(void) {
    SKIP("no statement makes an embedded implicit commit fail: commit_unknown needs a WAL device (§5.3 rule 4)");
}

static void test_cursor_past_end(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_result *r = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    EXPECT_CODE("C1005", terndb_result_counts(r, &(const uint64_t *){NULL}, &(size_t){0}, &err_));
    EXPECT_CODE("C1005", terndb_result_commit(r, &(uint64_t){0}, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    terndb_batch_free(b);
    b = (terndb_batch *)(uintptr_t)1;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 0);
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 0);
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 0);
    const uint64_t *counts;
    size_t n;
    EXPECT_OK(terndb_result_counts(r, &counts, &n, &err_));
    CHECK(n == 1 && counts[0] == 3);
    uint64_t seq = 0;
    CHECK(terndb_result_commit(r, &seq, &(terndb_error *){NULL}) == 0); /* a read writes nothing */
    terndb_op *op = terndb_result_next_async(r, NULL, 0);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_OK(terndb_op_error(op, &err_));
    CHECK(terndb_op_take_batch(op) == NULL);
    terndb_op_free(op);
    terndb_result_free(r);
    /* A write's counts and commit number. */
    r = run(c, NULL, ENV_INSERT_4, ENV_INSERT_4_LEN);
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 0);
    EXPECT_OK(terndb_result_counts(r, &counts, &n, &err_));
    CHECK(n == 1 && counts[0] == 1);
    CHECK(terndb_result_commit(r, &seq, &(terndb_error *){NULL}) == 1 && seq > 0);
    CHECK(terndb_result_type(r) == NULL);
    const uint8_t *tb;
    size_t tl = 99;
    terndb_result_type_bytes(r, &tb, &tl);
    CHECK(tl == 0);
    terndb_result_free(r);
}

/* ---- §6.2 rule 1 sync and async are one ------------------------------------------------------- */

static terndb_op *await(terndb_op *op) {
    CHECK(op != NULL);
    CHECK(terndb_op_wait(op, -1) == 1);
    return op;
}

static void same_error(int sync_rc, terndb_error *se, terndb_op *op) {
    terndb_error *ae = NULL;
    int arc = terndb_op_error(op, &ae);
    CHECK(sync_rc == -1 && arc == -1);
    CHECK(strcmp(code_of(se), code_of(ae)) == 0);
    const char *sp, *ap;
    size_t sl, al;
    terndb_error_path(se, 0, &sp, &sl);
    terndb_error_path(ae, 0, &ap, &al);
    CHECK(sl == al && memcmp(sp, ap, sl) == 0);
    CHECK(terndb_error_txn_state(se) == terndb_error_txn_state(ae));
    CHECK(terndb_error_retryable(se) == terndb_error_retryable(ae));
    terndb_error_free(se);
    terndb_error_free(ae);
    terndb_op_free(op);
}

static void test_sync_async_equivalence(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_error *e = NULL;

    /* open: a refused one, both ways. */
    terndb_db *d2 = NULL;
    same_error(terndb_open("", 0, NULL, &d2, &e), e, await(terndb_open_async("", 0, NULL, NULL, 0)));
    /* open: a successful one. */
    terndb_config *mem = config("mode", "memory", "durability", "none", NULL);
    terndb_op *op = await(terndb_open_async("", 0, mem, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_db *d3 = terndb_op_take_db(op);
    CHECK(d3 != NULL && terndb_op_take_db(op) == NULL);
    terndb_op_free(op);
    EXPECT_OK(terndb_close(d3, &err_));
    /* close, async. */
    terndb_db *d4 = NULL;
    EXPECT_OK(terndb_open("", 0, mem, &d4, &err_));
    op = await(terndb_close_async(d4, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_op_free(op);
    /* prepare: a failing tree, both ways. */
    terndb_stmt *s = NULL;
    e = NULL;
    same_error(terndb_prepare(c, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, &s, &e), e,
               await(terndb_prepare_async(c, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, NULL, 0)));
    /* prepare: one that succeeds gives the same shape. */
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    op = await(terndb_prepare_async(c, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0));
    terndb_stmt *s2 = terndb_op_take_stmt(op);
    CHECK(s2 != NULL && memcmp(terndb_stmt_shape(s), terndb_stmt_shape(s2), 32) == 0);
    CHECK(terndb_stmt_catalog_version(s) == terndb_stmt_catalog_version(s2));
    terndb_op_free(op);
    /* execute: a failing tree, and a constraint failure, both ways. */
    terndb_result *r = NULL;
    e = NULL;
    same_error(terndb_execute(c, NULL, NULL, NULL, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, NULL, 0, &r, &e), e,
               await(terndb_execute_async(c, NULL, NULL, NULL, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, NULL, 0, NULL, 0)));
    /* begin, commit, rollback, savepoints and status, both ways. */
    terndb_txn *t1 = NULL, *t2 = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t1, &err_));
    op = await(terndb_begin_async(c, NULL, &t2, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_op_free(op);
    CHECK(terndb_txn_isolation(t1) == terndb_txn_isolation(t2));
    EXPECT_OK(terndb_savepoint(t1, "a", 1, &err_));
    op = await(terndb_savepoint_async(t2, "a", 1, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_op_free(op);
    EXPECT_OK(terndb_rollback_to_savepoint(t1, "a", 1, &err_));
    op = await(terndb_rollback_to_savepoint_async(t2, "a", 1, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_op_free(op);
    EXPECT_OK(terndb_release_savepoint(t1, "a", 1, &err_));
    op = await(terndb_release_savepoint_async(t2, "a", 1, NULL, 0));
    EXPECT_OK(terndb_op_error(op, &err_));
    terndb_op_free(op);
    uint64_t txid = terndb_txn_id(t1);
    CHECK(terndb_commit(t1, NULL, &(terndb_error *){NULL}) == 0); /* an empty write set: 0 */
    op = await(terndb_commit_async(t2, NULL, 0));
    CHECK(terndb_op_commit_seq(op, &(uint64_t){0}) == 0);
    terndb_op_free(op);
    /* a commit of a closed transaction: W2001 both ways. */
    e = NULL;
    same_error(terndb_commit(t1, NULL, &e), e, await(terndb_commit_async(t2, NULL, 0)));
    e = NULL;
    same_error(terndb_rollback(t1, &e), e, await(terndb_rollback_async(t2, NULL, 0)));
    /* An unknown savepoint, both ways, in two fresh transactions. */
    terndb_txn *t3 = NULL, *t4 = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t3, &err_));
    EXPECT_OK(terndb_begin(c, NULL, &t4, &err_));
    e = NULL;
    same_error(terndb_rollback_to_savepoint(t3, "zz", 2, &e), e, await(terndb_rollback_to_savepoint_async(t4, "zz", 2, NULL, 0)));
    e = NULL;
    same_error(terndb_release_savepoint(t3, "zz", 2, &e), e, await(terndb_release_savepoint_async(t4, "zz", 2, NULL, 0)));
    CHECK(terndb_txn_state(t3) == terndb_txn_state(t4));
    terndb_txn_free(t3);
    terndb_txn_free(t4);
    int32_t st = -5;
    EXPECT_OK(terndb_transaction_status(c, txid, &st, NULL, &err_));
    op = await(terndb_transaction_status_async(c, txid, NULL, 0));
    int32_t ast = -6;
    CHECK(terndb_op_txn_status(op, &ast, NULL) >= 0);
    CHECK(st == ast);
    terndb_op_free(op);
    /* connect: refused alike. */
    terndb_conn *nc = NULL;
    e = NULL;
    same_error(terndb_connect("terndb://h/db", 13, NULL, &nc, &e), e, await(terndb_connect_async("terndb://h/db", 13, NULL, NULL, 0)));
    /* snapshot, clone, promote: refused alike. */
    e = NULL;
    same_error(terndb_snapshot(db, "", 0, &e), e, await(terndb_snapshot_async(db, "", 0, NULL, 0)));
    e = NULL;
    same_error(terndb_clone(db, "", 0, NULL, &d2, &e), e, await(terndb_clone_async(db, "", 0, NULL, NULL, 0)));
    e = NULL;
    same_error(terndb_promote(db, "d", 1, NULL, &d2, &e), e, await(terndb_promote_async(db, "d", 1, NULL, NULL, 0)));
    /* next: the same batch octets both ways. */
    terndb_result *ra = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_result *rb = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_batch *ba = NULL;
    CHECK(terndb_result_next(ra, &ba, &(terndb_error *){NULL}) == 1);
    op = await(terndb_result_next_async(rb, NULL, 0));
    terndb_batch *bb = terndb_op_take_batch(op);
    CHECK(bb != NULL);
    const uint8_t *pa, *pb;
    size_t la, lb;
    terndb_batch_bytes(ba, &pa, &la);
    terndb_batch_bytes(bb, &pb, &lb);
    CHECK(la == lb && memcmp(pa, pb, la) == 0);
    terndb_op_free(op);
    (void)s2;
}

/* ---- §10 errors ------------------------------------------------------------------------------- */

static void test_error_accessors(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    /* A validation error: a Q code with a path. */
    terndb_result *r = NULL;
    terndb_error *e = NULL;
    CHECK(terndb_execute(c, NULL, NULL, NULL, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, NULL, 0, &r, &e) == -1);
    CHECK(r == NULL);
    CHECK(terndb_error_entry_count(e) >= 1);
    const char *code = terndb_error_code(e, 0);
    CHECK(strlen(code) == 5 && code[0] == 'Q');
    CHECK(strcmp(terndb_error_name(e, 0), terndb_code_name(code)) == 0);
    const char *m;
    size_t ml;
    terndb_error_message(e, 0, &m, &ml);
    CHECK(ml > 0);
    const char *p;
    size_t pl;
    terndb_error_path(e, 0, &p, &pl);
    CHECK(pl > 0 && p[0] == '/');
    CHECK(!terndb_error_retryable(e));
    CHECK(terndb_error_txn_state(e) == TERNDB_TXN_STATE_UNAFFECTED);
    uint64_t v = 99;
    CHECK(!terndb_error_tuple(e, &v) && v == 99);
    (void)terndb_error_span(e, 0, &v, &v);
    for (size_t k = 0; k < terndb_error_detail_count(e, 0); k++) {
        const char *dk, *dv;
        size_t dkl, dvl;
        terndb_error_detail(e, 0, k, &dk, &dkl, &dv, &dvl);
        CHECK(dkl > 0);
    }
    terndb_error_free(e);
    /* A C-layer error: one entry, empty path, unaffected, never retryable (§10.2 rule 3), with
     * details.key. */
    terndb_config *cfg = NULL;
    EXPECT_OK(terndb_config_new(&cfg, &err_));
    e = NULL;
    CHECK(terndb_config_set(cfg, "colour", 6, "x", 1, &e) == -1);
    CHECK(terndb_error_entry_count(e) == 1);
    CHECK(strcmp(terndb_error_code(e, 0), "C1002") == 0);
    CHECK(strcmp(terndb_error_name(e, 0), "invalid_argument") == 0);
    terndb_error_path(e, 0, &p, &pl);
    CHECK(pl == 0);
    CHECK(!terndb_error_retryable(e) && terndb_error_txn_state(e) == TERNDB_TXN_STATE_UNAFFECTED);
    CHECK(!terndb_error_txid(e, &v) && !terndb_error_span(e, 0, &v, &v));
    CHECK(terndb_error_detail_count(e, 0) == 1);
    const char *dk, *dv;
    size_t dkl, dvl;
    terndb_error_detail(e, 0, 0, &dk, &dkl, &dv, &dvl);
    CHECK(dkl == 3 && memcmp(dk, "key", 3) == 0 && dvl == 6 && memcmp(dv, "colour", 6) == 0);
    terndb_error_free(e);
    /* A constraint failure inside a transaction: it moves the transaction's state. */
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    e = NULL;
    terndb_result *rr = NULL;
    int rc = terndb_execute(c, t, NULL, NULL, ENV_INSERT_DUP, ENV_INSERT_DUP_LEN, NULL, 0, &rr, &e);
    if (rc == 0) {
        terndb_batch *b = NULL;
        rc = terndb_result_next(rr, &b, &e);
        terndb_result_free(rr);
    }
    CHECK(rc == -1 && e != NULL);
    CHECK(terndb_error_txid(e, &v) && v == terndb_txn_id(t));
    int32_t ts = terndb_error_txn_state(e);
    CHECK(ts == TERNDB_TXN_STATE_ABORTED || ts == TERNDB_TXN_STATE_CLOSED);
    CHECK(terndb_txn_state(t) == (ts == TERNDB_TXN_STATE_ABORTED ? TERNDB_TXN_ABORTED : TERNDB_TXN_CLOSED));
    terndb_error_free(e);
    if (ts == TERNDB_TXN_STATE_ABORTED) {
        /* §5.3 rule 6: later requests other than rollback are W2003. */
        EXPECT_CODE("W2003", terndb_execute(c, t, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, &rr, &err_));
        EXPECT_OK(terndb_rollback(t, &err_));
    }
    CHECK(terndb_txn_state(t) == TERNDB_TXN_CLOSED);
    EXPECT_CODE("W2001", terndb_execute(c, t, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, &rr, &err_));
}

static void test_errors_match_network(void) { SKIP("this build has no network mode to compare with"); }

/* err = NULL is accepted everywhere; success never writes *err. */
static void test_error_null_out(void) {
    init();
    terndb_config *cfg = NULL;
    CHECK(terndb_config_new(&cfg, NULL) == 0);
    CHECK(terndb_config_set(cfg, "nope", 4, "x", 1, NULL) == -1);
    CHECK(terndb_config_set(NULL, "user", 4, "x", 1, NULL) == -1);
    terndb_error *sentinel = (terndb_error *)(uintptr_t)0x1234;
    terndb_error *e = sentinel;
    CHECK(terndb_config_set(cfg, "user", 4, "x", 1, &e) == 0 && e == sentinel);
    terndb_db *db = NULL;
    CHECK(terndb_open("", 0, NULL, &db, NULL) == -1 && db == NULL);
    terndb_config *mem = config("mode", "memory", "durability", "none", NULL);
    e = sentinel;
    CHECK(terndb_open("", 0, mem, &db, &e) == 0 && e == sentinel);
    terndb_conn *c = NULL;
    e = sentinel;
    CHECK(terndb_session(db, NULL, &c, &e) == 0 && e == sentinel);
    terndb_result *r = NULL;
    CHECK(terndb_execute(c, NULL, NULL, NULL, ENV_BAD_COLUMN, ENV_BAD_COLUMN_LEN, NULL, 0, &r, NULL) == -1);
    e = sentinel;
    CHECK(terndb_execute(c, NULL, NULL, NULL, ENV_CREATE_T, ENV_CREATE_T_LEN, NULL, 0, &r, &e) == 0 && e == sentinel);
    terndb_batch *b = NULL;
    e = sentinel;
    CHECK(terndb_result_next(r, &b, &e) == 0 && e == sentinel);
    terndb_txn *t = NULL;
    CHECK(terndb_begin(NULL, NULL, &t, NULL) == -1);
    CHECK(terndb_commit(NULL, NULL, NULL) == -1);
    CHECK(terndb_snapshot(db, "", 0, NULL) == -1);
    CHECK(terndb_close(NULL, NULL) == -1);
    CHECK(terndb_set_log_sink(9, NULL, NULL, NULL) == -1);
    e = sentinel;
    CHECK(terndb_set_log_sink(0, NULL, NULL, &e) == 0 && e == sentinel);
}

/* ---- NULL for every pointer parameter of every fallible function (§1 rule 7) -------------------- */

static void test_null_arguments(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_config *cfg = config(NULL, NULL);
    terndb_txn_options *to = NULL;
    EXPECT_OK(terndb_txn_options_new(&to, &err_));
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    terndb_result *r = run(c, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN);
    terndb_type *ty = NULL;
    EXPECT_OK(terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, &ty, &err_));
    terndb_batch *b = NULL;
    terndb_conn *oc = NULL;
    terndb_db *od = NULL;
    terndb_stmt *os = NULL;
    terndb_result *orr = NULL;
    terndb_metrics *m = NULL;
    terndb_queue *q = NULL;
    uint8_t digest[32] = {0};

    EXPECT_CODE("C1002", terndb_init(TERNDB_ABI_VERSION, sys_alloc, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_config_new(NULL, &err_));
    EXPECT_CODE("C1002", terndb_config_set(NULL, "user", 4, "x", 1, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, NULL, 4, "x", 1, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "user", 4, NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_open(NULL, 3, NULL, &od, &err_));
    EXPECT_CODE("C1002", terndb_open("", 0, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_close(NULL, &err_));
    EXPECT_CODE("C1002", terndb_snapshot(NULL, "", 0, &err_));
    EXPECT_CODE("C1002", terndb_snapshot(db, NULL, 2, &err_));
    EXPECT_CODE("C1002", terndb_clone(NULL, "", 0, NULL, &od, &err_));
    EXPECT_CODE("C1002", terndb_clone(db, "", 0, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_promote(NULL, "d", 1, NULL, &od, &err_));
    EXPECT_CODE("C1002", terndb_promote(db, "d", 1, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_session(NULL, NULL, &oc, &err_));
    EXPECT_CODE("C1002", terndb_session(db, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_connect(NULL, 0, NULL, &oc, &err_));
    EXPECT_CODE("C1002", terndb_connect("terndb://h/d", 12, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_new(NULL, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_isolation(NULL, 0, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_synchronous(NULL, 0, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_read_only(NULL, true, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_after(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_lock_timeout_ms(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_replica_wait_timeout_ms(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_max_lag_ms(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_clear(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_new(NULL, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_prefetch_bytes(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_batch_rows(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_batch_bytes(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_statement_timeout_ms(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_clear(NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_begin(NULL, NULL, &t, &err_));
    EXPECT_CODE("C1002", terndb_begin(c, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_commit(NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_rollback(NULL, &err_));
    EXPECT_CODE("C1002", terndb_savepoint(NULL, "a", 1, &err_));
    EXPECT_CODE("C1002", terndb_savepoint(t, NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_rollback_to_savepoint(NULL, "a", 1, &err_));
    EXPECT_CODE("C1002", terndb_rollback_to_savepoint(t, NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_release_savepoint(NULL, "a", 1, &err_));
    EXPECT_CODE("C1002", terndb_release_savepoint(t, NULL, 1, &err_));
    EXPECT_CODE("C1002", terndb_transaction_status(NULL, 1, &(int32_t){0}, NULL, &err_));
    EXPECT_CODE("C1002", terndb_transaction_status(c, 1, NULL, NULL, &err_));
    EXPECT_CODE("C1002", terndb_prepare(NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, &os, &err_));
    EXPECT_CODE("C1002", terndb_prepare(c, NULL, 4, &os, &err_));
    EXPECT_CODE("C1002", terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, &err_));
    EXPECT_CODE("C1002", terndb_execute(NULL, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute(c, NULL, NULL, NULL, NULL, 4, NULL, 0, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 3, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, NULL, &err_));
    EXPECT_CODE("C1002", terndb_execute_prepared(NULL, NULL, NULL, NULL, NULL, 0, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute_prepared(s, NULL, NULL, NULL, NULL, 2, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute_prepared(s, NULL, NULL, NULL, NULL, 0, NULL, &err_));
    EXPECT_CODE("C1002", terndb_execute_digest(NULL, NULL, NULL, NULL, digest, NULL, 0, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute_digest(c, NULL, NULL, NULL, NULL, NULL, 0, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute_digest(c, NULL, NULL, NULL, digest, NULL, 1, &orr, &err_));
    EXPECT_CODE("C1002", terndb_execute_digest(c, NULL, NULL, NULL, digest, NULL, 0, NULL, &err_));
    EXPECT_CODE("C1002", terndb_result_next(NULL, &b, &err_));
    EXPECT_CODE("C1002", terndb_result_next(r, NULL, &err_));
    EXPECT_CODE("C1002", terndb_result_counts(NULL, &(const uint64_t *){NULL}, &(size_t){0}, &err_));
    EXPECT_CODE("C1002", terndb_result_commit(NULL, &(uint64_t){0}, &err_));
    EXPECT_CODE("C1002", terndb_batch_decode(NULL, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, &b, &err_));
    EXPECT_CODE("C1002", terndb_batch_decode(ty, NULL, 4, &b, &err_));
    EXPECT_CODE("C1002", terndb_batch_decode(ty, GOLD_V16_BATCH, GOLD_V16_BATCH_LEN, NULL, &err_));
    EXPECT_CODE("C1002", terndb_type_decode(NULL, 3, &ty, &err_));
    EXPECT_CODE("C1002", terndb_type_decode(GOLD_V16_TYPE, GOLD_V16_TYPE_LEN, NULL, &err_));
    EXPECT_CODE("C1002", terndb_queue_new(NULL, &err_));
    EXPECT_CODE("C1002", terndb_op_error(NULL, &err_));
    EXPECT_CODE("C1002", terndb_metrics_snapshot(NULL, &m, &err_));
    EXPECT_CODE("C1002", terndb_metrics_snapshot(db, NULL, &err_));
    /* The async forms deliver an argument error through their operation (§6.2 rule 2). */
    terndb_op *op = await(terndb_execute_async(c, NULL, NULL, NULL, NULL, 3, NULL, 0, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_begin_async(NULL, NULL, &t, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_begin_async(c, NULL, NULL, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_commit_async(NULL, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_prepare_async(NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_result_next_async(NULL, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_close_async(NULL, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_execute_digest_async(c, NULL, NULL, NULL, NULL, NULL, 0, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    op = await(terndb_savepoint_async(t, NULL, 2, NULL, 0));
    EXPECT_CODE("C1002", terndb_op_error(op, &err_));
    terndb_op_free(op);
    /* Optional pointers: NULL options, NULL commit_seq, NULL params with length 0. */
    EXPECT_OK(terndb_execute(c, NULL, NULL, NULL, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, &orr, &err_));
    terndb_result_free(orr);
    EXPECT_OK(terndb_commit(t, NULL, &err_));
    (void)q;
}

/* ---- zero-length trees and parameters --------------------------------------------------------- */

static void test_zero_length_tree(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_result *r = NULL;
    terndb_error *e = NULL;
    CHECK(terndb_execute(c, NULL, NULL, NULL, NULL, 0, NULL, 0, &r, &e) == -1);
    CHECK(code_of(e)[0] == 'Q' && r == NULL);
    terndb_error_free(e);
    e = NULL;
    CHECK(terndb_execute(c, NULL, NULL, NULL, ENV_SELECT_T, 0, NULL, 0, &r, &e) == -1 && code_of(e)[0] == 'Q');
    terndb_error_free(e);
    terndb_stmt *s = NULL;
    e = NULL;
    CHECK(terndb_prepare(c, NULL, 0, &s, &e) == -1 && code_of(e)[0] == 'Q');
    terndb_error_free(e);
    /* A truncated tree. */
    e = NULL;
    CHECK(terndb_execute(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN - 1, NULL, 0, &r, &e) == -1);
    CHECK(code_of(e)[0] == 'Q');
    terndb_error_free(e);
    /* The connection serves on. */
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
}

static void test_ddl_through_execute(void) {
    init();
    terndb_db *db = open_db();
    terndb_conn *c = session(db);
    uint64_t v0 = terndb_conn_catalog_version(c);
    /* DDL with a NULL parameter block, and with the explicit empty block 01 00 (§0 rule 4). */
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, NULL, ENV_CREATE_T, ENV_CREATE_T_LEN, (const uint8_t[]){1, 0}, 2, &r, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 0);
    terndb_result_free(r);
    CHECK(RUN(c, NULL, ENV_INSERT_T) == 0);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
    CHECK(terndb_conn_catalog_version(c) >= v0);
    CHECK(terndb_conn_is_embedded(c));
}

static void test_params(void) {
    init();
    terndb_db *db = open_db();
    terndb_conn *c = session(db);
    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_PARAM, ENV_PARAM_LEN, &s, &err_));
    CHECK(terndb_stmt_param_count(s) == 1);
    CHECK(terndb_type_kind(terndb_stmt_param_type(s, 0)) == TERNDB_TYPE_INT64);
    CHECK(terndb_type_kind(terndb_type_element(terndb_stmt_result_type(s))) == TERNDB_TYPE_RECORD);
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute_prepared(s, NULL, NULL, NULL, PARAMS_42, PARAMS_42_LEN, &r, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    CHECK(terndb_column_get_i64(terndb_column_field(terndb_batch_root(b), 0), 0) == 42);
    terndb_batch_free(b);
    terndb_result_free(r);
    /* The same by digest, from the plan cache. */
    EXPECT_OK(terndb_execute_digest(c, NULL, NULL, NULL, terndb_stmt_shape(s), PARAMS_42, PARAMS_42_LEN, &r, &err_));
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    CHECK(memcmp(terndb_result_shape(r), terndb_stmt_shape(s), 32) == 0);
    terndb_batch_free(b);
    terndb_result_free(r);
    /* An unknown digest runs nothing: W2004. */
    uint8_t zero[32] = {0};
    EXPECT_CODE("W2004", terndb_execute_digest(c, NULL, NULL, NULL, zero, NULL, 0, &r, &err_));
    /* A parameter block that does not decode by the declared types. */
    terndb_error *e = NULL;
    CHECK(terndb_execute_prepared(s, NULL, NULL, NULL, (const uint8_t[]){1, 0}, 2, &r, &e) == -1);
    CHECK(code_of(e)[0] == 'Q');
    terndb_error_free(e);
    /* implicit options beside a transaction: W2011 (§7 rule 2). */
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_txn_options *to = NULL;
    EXPECT_OK(terndb_txn_options_new(&to, &err_));
    EXPECT_CODE("W2011", terndb_execute_prepared(s, t, to, NULL, PARAMS_42, PARAMS_42_LEN, &r, &err_));
    /* A transaction of another connection: C1002. */
    terndb_conn *c2 = session(db);
    EXPECT_CODE("C1002", terndb_execute(c2, t, NULL, NULL, ENV_PARAM, ENV_PARAM_LEN, PARAMS_42, PARAMS_42_LEN, &r, &err_));
    terndb_stmt_free(s);
}

/* ---- §5 options and configuration ------------------------------------------------------------- */

static void test_options(void) {
    init();
    terndb_txn_options *o = NULL;
    EXPECT_OK(terndb_txn_options_new(&o, &err_));
    EXPECT_OK(terndb_txn_options_set_isolation(o, TERNDB_ISOLATION_SNAPSHOT, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_isolation(o, 3, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_isolation(o, -1, &err_));
    EXPECT_OK(terndb_txn_options_set_synchronous(o, TERNDB_SYNC_REMOTE_APPLY, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_synchronous(o, 4, &err_));
    EXPECT_OK(terndb_txn_options_set_lock_timeout_ms(o, INT64_MAX, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_lock_timeout_ms(o, (uint64_t)INT64_MAX + 1, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_replica_wait_timeout_ms(o, UINT64_MAX, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_set_max_lag_ms(o, UINT64_MAX, &err_));
    for (int32_t f = TERNDB_TXN_OPT_ISOLATION; f <= TERNDB_TXN_OPT_MAX_LAG; f++) EXPECT_OK(terndb_txn_options_clear(o, f, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_clear(o, 0, &err_));
    EXPECT_CODE("C1002", terndb_txn_options_clear(o, 8, &err_));
    terndb_exec_options *x = NULL;
    EXPECT_OK(terndb_exec_options_new(&x, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(x, SIZE_MAX, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_prefetch_bytes(x, 0, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_batch_rows(x, 0, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_batch_bytes(x, 0, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_set_statement_timeout_ms(x, UINT64_MAX, &err_));
    for (int32_t f = TERNDB_EXEC_OPT_PREFETCH_BYTES; f <= TERNDB_EXEC_OPT_STATEMENT_TIMEOUT; f++)
        EXPECT_OK(terndb_exec_options_clear(x, f, &err_));
    EXPECT_CODE("C1002", terndb_exec_options_clear(x, 5, &err_));
    /* Options in use: a snapshot transaction reports its effective level; an unlimited window
     * reads everything. */
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    EXPECT_OK(terndb_txn_options_set_isolation(o, TERNDB_ISOLATION_SNAPSHOT, &err_));
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, o, &t, &err_));
    CHECK(terndb_txn_state(t) == TERNDB_TXN_OPEN);
    CHECK(terndb_txn_isolation(t) >= TERNDB_ISOLATION_SERIALIZABLE && terndb_txn_isolation(t) <= TERNDB_ISOLATION_READ_COMMITTED);
    CHECK(terndb_txn_snapshot(t) > 0 || terndb_txn_id(t) > 0);
    EXPECT_OK(terndb_rollback(t, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(x, SIZE_MAX, &err_));
    EXPECT_OK(terndb_exec_options_set_batch_rows(x, 1, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, x, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, &r, &err_));
    size_t batches = 0;
    terndb_batch *b = NULL;
    while (terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1) {
        CHECK(terndb_batch_rows(b) == 1);
        batches++;
        terndb_batch_free(b);
    }
    CHECK(batches == 3);
    /* A read-only transaction refuses a write: T4001. */
    EXPECT_OK(terndb_txn_options_set_read_only(o, true, &err_));
    EXPECT_OK(terndb_begin(c, o, &t, &err_));
    terndb_error *e = NULL;
    int rc = terndb_execute(c, t, NULL, NULL, ENV_INSERT_4, ENV_INSERT_4_LEN, NULL, 0, &r, &e);
    if (rc == 0) rc = terndb_result_next(r, &b, &e);
    CHECK(rc == -1 && strcmp(code_of(e), "T4001") == 0);
}

static void test_config_strict(void) {
    init();
    terndb_config *cfg = NULL;
    EXPECT_OK(terndb_config_new(&cfg, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "modes", 5, "memory", 6, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "", 0, "memory", 6, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "mode", 4, "ram", 3, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "mode", 4, "", 0, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "threads", 7, "0", 1, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "threads", 7, "-1", 2, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "threads", 7, "1x", 2, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "memory_limit", 12, "99999999999999999999", 20, &err_));
    EXPECT_CODE("C1002", terndb_config_set(cfg, "read_only", 9, "yes", 3, &err_));
    EXPECT_OK(terndb_config_set(cfg, "threads", 7, "1", 1, &err_));
    EXPECT_OK(terndb_config_set(cfg, "threads", 7, "2", 1, &err_)); /* set twice: the last */
    EXPECT_OK(terndb_config_set(cfg, "memory_limit", 12, "0", 1, &err_));
    /* A key that does not apply to the call. */
    terndb_config *o = config("mode", "memory", "durability", "none", "user", "terndb", NULL);
    terndb_db *db = NULL;
    EXPECT_CODE("C1002", terndb_open("", 0, o, &db, &err_));
    terndb_db *mem = open_db();
    terndb_conn *c = NULL;
    terndb_config *s1 = config("mode", "memory", NULL);
    EXPECT_CODE("C1002", terndb_session(mem, s1, &c, &err_));
    terndb_config *s2 = config("password", "x", NULL);
    EXPECT_CODE("C1002", terndb_session(mem, s2, &c, &err_));
    terndb_config *s3 = config("tls_ca_file", "/x", NULL);
    EXPECT_CODE("C1002", terndb_session(mem, s3, &c, &err_));
    /* durability with disk mode is C1002 (§4.2 rule 4). */
    terndb_config *d = config("mode", "disk", "durability", "log", NULL);
    EXPECT_CODE("C1002", terndb_open("dir", 3, d, &db, &err_));
    /* Session keys that apply. */
    terndb_config *ok = config("isolation", "snapshot", "synchronous", "on", "lock_timeout_ms", "100",
                               "statement_timeout_ms", "60000", "user", "terndb", "application", "driver",
                               "max_in_flight", "8", "max_open_txns", "2", NULL);
    EXPECT_OK(terndb_session(mem, ok, &c, &err_));
    CHECK(RUN(c, NULL, ENV_CREATE_T) == 0);
    terndb_conn_close(c);
    /* An unknown role: W3001 (§5.1 rule 1). */
    terndb_config *who = config("user", "nobody", NULL);
    EXPECT_CODE("W3001", terndb_session(mem, who, &c, &err_));
    (void)db;
}

static void test_max_in_flight(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *base = with_table(&db);
    RUN(base, NULL, ENV_CREATE_BIG);
    RUN(base, NULL, ENV_INSERT_BIG_A);
    terndb_config *cfg = config("max_in_flight", "2", NULL);
    terndb_conn *c = NULL;
    EXPECT_OK(terndb_session(db, cfg, &c, &err_));
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(eo, 1024, &err_));
    terndb_result *r1 = NULL, *r2 = NULL, *r3 = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, &r1, &err_));
    EXPECT_OK(terndb_execute(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, &r2, &err_));
    EXPECT_CODE("W2008", terndb_execute(c, NULL, NULL, eo, ENV_COUNT_T, ENV_COUNT_T_LEN, NULL, 0, &r3, &err_));
    terndb_result_free(r1);
    terndb_result_free(r2);
}

/* ---- §4 databases ------------------------------------------------------------------------------ */

static void test_open_modes(void) {
    init();
    terndb_db *db = NULL;
    terndb_error *e = NULL;
    /* Disk mode, the default, is not in this build: C1007 with details.reason. */
    CHECK(terndb_open("dir", 3, NULL, &db, &e) == -1 && strcmp(code_of(e), "C1007") == 0);
    const char *k, *v;
    size_t kl, vl;
    CHECK(terndb_error_detail_count(e, 0) == 1);
    terndb_error_detail(e, 0, 0, &k, &kl, &v, &vl);
    CHECK(kl == 6 && memcmp(k, "reason", 6) == 0);
    terndb_error_free(e);
    EXPECT_CODE("C1007", terndb_open("", 0, NULL, &db, &err_));
    terndb_config *disk = config("mode", "disk", NULL);
    EXPECT_CODE("C1007", terndb_open("dir", 3, disk, &db, &err_));
    /* Memory mode needs durability none: log is the default. */
    terndb_config *m = config("mode", "memory", NULL);
    EXPECT_CODE("C1007", terndb_open("", 0, m, &db, &err_));
    terndb_config *snap = config("mode", "memory", "durability", "snapshot", NULL);
    EXPECT_CODE("C1007", terndb_open("dir", 3, snap, &db, &err_));
    /* durability none in a directory: C1007; anonymous: opens. */
    terndb_config *none = config("mode", "memory", "durability", "none", "create", "always", "read_only", "false",
                                 "threads", "2", "memory_limit", "0", "snapshot_interval_ms", "1000",
                                 "max_snapshot_age_ms", "0", NULL);
    EXPECT_CODE("C1007", terndb_open("dir", 3, none, &db, &err_));
    EXPECT_OK(terndb_open("", 0, none, &db, &err_));
    CHECK(terndb_db_is_memory(db));
    /* Two anonymous databases are independent. */
    terndb_db *db2 = NULL;
    EXPECT_OK(terndb_open("", 0, none, &db2, &err_));
    terndb_conn *a = session(db), *b = session(db2);
    RUN(a, NULL, ENV_CREATE_T);
    RUN(a, NULL, ENV_INSERT_T);
    RUN(b, NULL, ENV_CREATE_T);
    CHECK(RUN(a, NULL, ENV_SELECT_T) == 3 && RUN(b, NULL, ENV_SELECT_T) == 0);
}

static void test_read_only_db(void) {
    init();
    terndb_config *ro = config("mode", "memory", "durability", "none", "read_only", "true", NULL);
    terndb_db *db = NULL;
    EXPECT_OK(terndb_open("", 0, ro, &db, &err_));
    terndb_conn *c = session(db);
    terndb_result *r = NULL;
    terndb_error *e = NULL;
    int rc = terndb_execute(c, NULL, NULL, NULL, ENV_CREATE_T, ENV_CREATE_T_LEN, NULL, 0, &r, &e);
    if (rc == 0) rc = terndb_result_next(r, &(terndb_batch *){NULL}, &e);
    CHECK(rc == -1 && strcmp(code_of(e), "T4001") == 0);
}

static void test_disk_only_calls(void) {
    init();
    terndb_db *db = open_db();
    terndb_db *out = NULL;
    EXPECT_CODE("C1007", terndb_snapshot(db, "", 0, &err_));
    EXPECT_CODE("C1007", terndb_snapshot(db, "snapdir", 7, &err_));
    EXPECT_CODE("C1007", terndb_clone(db, "", 0, NULL, &out, &err_));
    EXPECT_CODE("C1007", terndb_promote(db, "promoted", 8, NULL, &out, &err_));
    terndb_config *bad = config("user", "x", NULL);
    EXPECT_CODE("C1002", terndb_clone(db, "", 0, bad, &out, &err_));
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    terndb_op *ops[3] = {terndb_snapshot_async(db, "", 0, q, 1), terndb_clone_async(db, "", 0, NULL, q, 2),
                         terndb_promote_async(db, "p", 1, NULL, q, 3)};
    for (int i = 0; i < 3; i++) {
        CHECK(terndb_op_wait(ops[i], -1) == 1);
        EXPECT_CODE("C1007", terndb_op_error(ops[i], &err_));
        CHECK(terndb_op_take_db(ops[i]) == NULL || i == 0);
    }
    uint64_t tags[4];
    CHECK(terndb_queue_drain(q, tags, 4) == 3);
    CHECK(out == NULL);
}

static void test_promote_roundtrip(void) { SKIP("disk storage is not in this build (terndb_promote is C1007, test_disk_only_calls)"); }
static void test_clone_isolation(void) { SKIP("clone is not in this build (C1007, test_disk_only_calls)"); }
static void test_clone_constant_time(void) { SKIP("clone is not in this build (C1007, test_disk_only_calls)"); }
static void test_lock_in_process(void) { SKIP("disk storage, and so CONTROL's lock, is not in this build"); }
static void test_lock_child_process(void) { SKIP("disk storage, and so CONTROL's lock, is not in this build"); }
static void test_transaction_status_after_crash(void) { SKIP("an embedded crash leaves nothing to reopen without disk storage"); }

static void test_connect(void) {
    init();
    terndb_conn *c = NULL;
    EXPECT_CODE("C1007", terndb_connect("terndb://localhost/db", 21, NULL, &c, &err_));
    EXPECT_CODE("C1007", terndb_connect("terndb://h:7365/db", 18, NULL, &c, &err_));
    EXPECT_CODE("C1007", terndb_connect("terndb+unix:///tmp/s?database=x", 31, NULL, &c, &err_));
    EXPECT_CODE("C1002", terndb_connect("http://h/db", 11, NULL, &c, &err_));
    EXPECT_CODE("C1002", terndb_connect("terndb://", 9, NULL, &c, &err_));
    EXPECT_CODE("C1002", terndb_connect("terndb://h", 10, NULL, &c, &err_));
    EXPECT_CODE("C1002", terndb_connect("terndb+unix:///tmp/s", 20, NULL, &c, &err_));
    EXPECT_CODE("C1002", terndb_connect("", 0, NULL, &c, &err_));
    terndb_config *cfg = config("mode", "memory", NULL);
    EXPECT_CODE("C1002", terndb_connect("terndb://h/db", 13, cfg, &c, &err_));
    CHECK(c == NULL);
}

/* ---- §2.2 rule 4 closing with dependants alive ------------------------------------------------- */

static void test_close_with_open_sessions(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    RUN(c, NULL, ENV_CREATE_BIG);
    RUN(c, NULL, ENV_INSERT_BIG_A);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    RUN(c, t, ENV_INSERT_4);
    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(eo, 2048, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, &r, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    EXPECT_OK(terndb_close(db, &err_));
    /* The running request ends cancelled by the shutdown; every other call is C1006. */
    terndb_error *e = NULL;
    int k;
    terndb_batch *b2 = NULL;
    while ((k = terndb_result_next(r, &b2, &e)) == 1) terndb_batch_free(b2);
    CHECK(k == -1 && strcmp(code_of(e), "T1007") == 0);
    terndb_error_free(e);
    EXPECT_CODE("C1006", terndb_commit(t, NULL, &err_));
    EXPECT_CODE("C1006", terndb_execute(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, &r, &err_));
    EXPECT_CODE("C1006", terndb_execute_prepared(s, NULL, NULL, NULL, NULL, 0, &r, &err_));
    EXPECT_CODE("C1006", terndb_begin(c, NULL, &t, &err_));
    terndb_op *op = await(terndb_execute_async(c, NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, NULL, 0));
    EXPECT_CODE("C1006", terndb_op_error(op, &err_));
    terndb_op_free(op);
    /* The batch is still readable; the dependants free cleanly. */
    CHECK(terndb_batch_rows(b) > 0);
    terndb_batch_free(b);
    terndb_result_free(r);
    terndb_stmt_free(s);
    terndb_txn_free(t);
    terndb_conn_close(c);
}

static void test_conn_close_with_cursors(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    RUN(c, NULL, ENV_CREATE_BIG);
    RUN(c, NULL, ENV_INSERT_BIG_A);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    RUN(c, t, ENV_INSERT_4);
    terndb_exec_options *eo = NULL;
    EXPECT_OK(terndb_exec_options_new(&eo, &err_));
    EXPECT_OK(terndb_exec_options_set_prefetch_bytes(eo, 2048, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute(c, NULL, NULL, eo, ENV_CROSS_BIG, ENV_CROSS_BIG_LEN, NULL, 0, &r, &err_));
    terndb_conn_close(c);
    terndb_error *e = NULL;
    int k;
    terndb_batch *b = NULL;
    while ((k = terndb_result_next(r, &b, &e)) == 1) terndb_batch_free(b);
    CHECK(k == -1 && strcmp(code_of(e), "T1007") == 0);
    terndb_error_free(e);
    EXPECT_CODE("C1006", terndb_commit(t, NULL, &err_));
    CHECK(terndb_txn_state(t) != TERNDB_TXN_OPEN || 1);
    terndb_result_free(r);
    terndb_txn_free(t);
    /* Its transaction was rolled back: another session does not see row 4. */
    terndb_conn *c2 = session(db);
    CHECK(RUN(c2, NULL, ENV_SELECT_T) == 3);
}

static void test_txn_free_rolls_back(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    RUN(c, t, ENV_INSERT_4);
    terndb_txn_free(t);
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 3);
    /* A transaction with its commit in flight is not rolled back by its free. */
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    RUN(c, t, ENV_INSERT_4);
    terndb_op *op = terndb_commit_async(t, NULL, 0);
    terndb_txn_free(t);
    CHECK(terndb_op_wait(op, -1) == 1);
    EXPECT_OK(terndb_op_error(op, &err_));
    CHECK(RUN(c, NULL, ENV_SELECT_T) == 4);
}

/* Every order of freeing db, conn, txn, stmt, result and batch is clean (property 22). */
static void free_in_order(const int *order) {
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_stmt *s = NULL;
    EXPECT_OK(terndb_prepare(c, ENV_SELECT_T, ENV_SELECT_T_LEN, &s, &err_));
    terndb_result *r = NULL;
    EXPECT_OK(terndb_execute_prepared(s, t, NULL, NULL, NULL, 0, &r, &err_));
    terndb_batch *b = NULL;
    CHECK(terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1);
    for (int i = 0; i < 6; i++) {
        switch (order[i]) {
        case 0: EXPECT_OK(terndb_close(db, &err_)); break;
        case 1: terndb_conn_close(c); break;
        case 2: terndb_txn_free(t); break;
        case 3: terndb_stmt_free(s); break;
        case 4: terndb_result_free(r); break;
        case 5: terndb_batch_free(b); break;
        }
    }
}

static int next_perm(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return 0;
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    int x = a[i]; a[i] = a[j]; a[j] = x;
    for (int l = i + 1, r = n - 1; l < r; l++, r--) { x = a[l]; a[l] = a[r]; a[r] = x; }
    return 1;
}

static void test_close_order_permutations(void) {
    init();
    size_t before = terndb_debug_live_handles();
    int order[6] = {0, 1, 2, 3, 4, 5};
    int n = 0;
    do {
        free_in_order(order);
        n++;
    } while (next_perm(order, 6));
    CHECK(n == 720);
    CHECK(terndb_debug_live_handles() == before);
}

/* ---- §11.2 metrics ------------------------------------------------------------------------------ */

static int64_t metric(terndb_metrics *m, const char *name, int32_t *kind) {
    for (size_t i = 0; i < terndb_metrics_count(m); i++) {
        const char *p;
        size_t l;
        terndb_metrics_name(m, i, &p, &l);
        if (l == strlen(name) && memcmp(p, name, l) == 0) {
            if (kind) *kind = terndb_metrics_kind(m, i);
            return terndb_metrics_value(m, i);
        }
    }
    fprintf(stderr, "no metric %s\n", name);
    exit(1);
}

static void test_metrics(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *c = with_table(&db);
    terndb_metrics *m = NULL;
    EXPECT_OK(terndb_metrics_snapshot(db, &m, &err_));
    const char *required[] = {"terndb.commits", "terndb.aborts", "terndb.sessions", "terndb.txns.open", "terndb.wal.bytes",
                              "terndb.wal.fsync_us", "terndb.buffer.hits", "terndb.buffer.misses", "terndb.undo.bytes",
                              "terndb.snapshot.oldest_age_ms", "terndb.memory.bytes"};
    for (size_t i = 0; i < sizeof required / sizeof *required; i++) (void)metric(m, required[i], NULL);
    int32_t kind = 0;
    int64_t commits = metric(m, "terndb.commits", &kind);
    CHECK(kind == TERNDB_METRIC_COUNTER && commits >= 2);
    CHECK(metric(m, "terndb.sessions", &kind) == 1 && kind == TERNDB_METRIC_GAUGE);
    CHECK(metric(m, "terndb.memory.bytes", NULL) > 0);
    (void)metric(m, "terndb.wal.fsync_us", &kind);
    CHECK(kind == TERNDB_METRIC_HISTOGRAM);
    for (size_t i = 0; i < terndb_metrics_count(m); i++) {
        if (terndb_metrics_kind(m, i) != TERNDB_METRIC_HISTOGRAM) continue;
        uint64_t last = 0;
        for (size_t k = 0; k < terndb_metrics_bucket_count(m, i); k++) {
            uint64_t ub, n;
            terndb_metrics_bucket(m, i, k, &ub, &n);
            CHECK(ub > last);
            last = ub;
        }
        CHECK(terndb_metrics_bucket_count(m, i) > 0);
    }
    terndb_metrics_free(m);
    /* An abort is counted with its code as a label. */
    terndb_txn *t = NULL;
    EXPECT_OK(terndb_begin(c, NULL, &t, &err_));
    terndb_result *r = NULL;
    terndb_error *e = NULL;
    int rc = terndb_execute(c, t, NULL, NULL, ENV_INSERT_DUP, ENV_INSERT_DUP_LEN, NULL, 0, &r, &e);
    if (rc == 0) rc = terndb_result_next(r, &(terndb_batch *){NULL}, &e);
    CHECK(rc == -1);
    char code[8];
    snprintf(code, sizeof code, "%s", code_of(e));
    terndb_error_free(e);
    EXPECT_OK(terndb_metrics_snapshot(db, &m, &err_));
    int found = 0;
    for (size_t i = 0; i < terndb_metrics_count(m); i++) {
        if (terndb_metrics_label_count(m, i) != 1) continue;
        const char *k, *v;
        size_t kl, vl;
        terndb_metrics_label(m, i, 0, &k, &kl, &v, &vl);
        if (kl == 4 && memcmp(k, "code", 4) == 0 && vl == strlen(code) && memcmp(v, code, vl) == 0 &&
            terndb_metrics_value(m, i) == 1)
            found = 1;
    }
    CHECK(found);
    CHECK(metric(m, "terndb.txns.open", NULL) <= 1);
    terndb_metrics_free(m);
    /* Once its one session closes, the session gauge falls back to 0 (the engine thread counts it). */
    terndb_conn_close(c);
    int64_t sessions = -1;
    for (int i = 0; i < 200 && sessions != 0; i++) {
        EXPECT_OK(terndb_metrics_snapshot(db, &m, &err_));
        sessions = metric(m, "terndb.sessions", NULL);
        terndb_metrics_free(m);
        if (sessions != 0) usleep(10000);
    }
    CHECK(sessions == 0);
}

/* ---- the event loop: 1,000 queries through one descriptor --------------------------------------- */

static void test_event_loop_integration(void) {
    init();
    terndb_db *db = NULL;
    terndb_conn *base = with_table(&db);
    (void)base;
    enum { CONNS = 4, PER = 250, N = CONNS * PER };
    terndb_conn *conns[CONNS];
    for (int i = 0; i < CONNS; i++) conns[i] = session(db);
    terndb_queue *q = NULL;
    EXPECT_OK(terndb_queue_new(&q, &err_));
    static terndb_op *ops[N];
    for (int i = 0; i < N; i++)
        ops[i] = terndb_execute_async(conns[i % CONNS], NULL, NULL, NULL, ENV_SELECT_T, ENV_SELECT_T_LEN, NULL, 0, q, (uint64_t)i);
    int kq = kqueue();
    CHECK(kq >= 0);
    struct kevent ev;
    EV_SET(&ev, terndb_queue_fd(q), EVFILT_READ, EV_ADD, 0, 0, NULL);
    CHECK(kevent(kq, &ev, 1, NULL, 0, NULL) == 0);
    static int done[N];
    int completed = 0;
    size_t rows = 0;
    while (completed < N) {
        struct kevent got;
        struct timespec ts = {5, 0};
        int n = kevent(kq, NULL, 0, &got, 1, &ts);
        CHECK(n == 1);
        uint64_t tags[64];
        size_t k;
        while ((k = terndb_queue_drain(q, tags, 64)) > 0) {
            for (size_t j = 0; j < k; j++) {
                CHECK(tags[j] < N && done[tags[j]] == 0);
                done[tags[j]] = 1;
                completed++;
                terndb_op *op = ops[tags[j]];
                EXPECT_OK(terndb_op_error(op, &err_));
                terndb_result *r = terndb_op_take_result(op);
                terndb_batch *b = NULL;
                while (terndb_result_next(r, &b, &(terndb_error *){NULL}) == 1) {
                    rows += terndb_batch_rows(b);
                    terndb_batch_free(b);
                }
                terndb_result_free(r);
                terndb_op_free(op);
            }
        }
    }
    CHECK(rows == (size_t)N * 3);
    close(kq);
}

/* ---- the table ---------------------------------------------------------------------------------- */

struct test {
    const char *name;
    void (*fn)(void);
    const char *abort_code; /* expected to abort a debug build, naming this code */
};

static const struct test tests[] = {
    {"init_order", test_init_order, NULL},
    {"init_version", test_init_version, NULL},
    {"init_twice_differently", test_init_twice_differently, NULL},
    {"init_half_allocator", test_init_half_allocator, NULL},
    {"code_name", test_code_name, NULL},
    {"allocator_counting", test_allocator_counting, NULL},
    {"allocator_null_fails_cleanly", test_allocator_null_fails_cleanly, NULL},
    {"leaks_every_kind", test_leaks_every_kind, NULL},
    {"free_null_is_nothing", test_free_null_is_nothing, NULL},
    {"use_after_free_every_kind", test_use_after_free_every_kind, NULL},
    {"freed_address_not_reused", test_freed_address_not_reused, NULL},
    {"wrong_kind_every_kind", test_wrong_kind_every_kind, NULL},
    {"abort_batch_after_free", abort_batch_after_free, "C1001"},
    {"abort_error_freed_twice", abort_error_freed_twice, "C1001"},
    {"abort_error_accessor_after_free", abort_error_accessor_after_free, "C1001"},
    {"abort_type_free_borrowed", abort_type_free_borrowed, "C1001"},
    {"abort_type_freed_twice", abort_type_freed_twice, "C1001"},
    {"abort_queue_fd_after_free", abort_queue_fd_after_free, "C1001"},
    {"abort_metrics_after_free", abort_metrics_after_free, "C1001"},
    {"abort_op_poll_after_free", abort_op_poll_after_free, "C1001"},
    {"abort_column_wrong_kind", abort_column_wrong_kind, "C1001"},
    {"abort_getter_wrong_representation", abort_getter_wrong_representation, "C1002"},
    {"abort_getter_index_out_of_range", abort_getter_index_out_of_range, "C1002"},
    {"abort_batch_column_out_of_range", abort_batch_column_out_of_range, "C1002"},
    {"abort_error_entry_out_of_range", abort_error_entry_out_of_range, "C1002"},
    {"abort_take_wrong_kind", abort_take_wrong_kind, "C1002"},
    {"abort_null_getter", abort_null_getter, "C1002"},
    {"conn_concurrent_use", test_conn_concurrent_use, NULL},
    {"conn_moves_between_threads", test_conn_moves_between_threads, NULL},
    {"sink_reentry", test_sink_reentry, NULL},
    {"sink_levels_and_removal", test_sink_levels_and_removal, NULL},
    {"log_record_fields", test_log_record_fields, NULL},
    {"inputs_scribbled", test_inputs_scribbled, NULL},
    {"batch_outlives_db", test_batch_outlives_db, NULL},
    {"borrowed_pointers_stable", test_borrowed_pointers_stable, NULL},
    {"golden_vectors_embedded", test_golden_vectors_embedded, NULL},
    {"golden_vectors_network", test_golden_vectors_network, NULL},
    {"decode_vectors", test_decode_vectors, NULL},
    {"decode_corrupt", test_decode_corrupt, NULL},
    {"type_decode_errors", test_type_decode_errors, NULL},
    {"raw_buffers_agree", test_raw_buffers_agree, NULL},
    {"random_nested_walk", test_random_nested_walk, NULL},
    {"queue_exactly_once", test_queue_exactly_once, NULL},
    {"queue_fd_level", test_queue_fd_level, NULL},
    {"queue_full", test_queue_full, NULL},
    {"op_free_pending", test_op_free_pending, NULL},
    {"op_error_pending", test_op_error_pending, NULL},
    {"pipeline_txn", test_pipeline_txn, NULL},
    {"pipeline_begin_fails", test_pipeline_begin_fails, NULL},
    {"cancel_query", test_cancel_query, NULL},
    {"cancel_commit_ignored", test_cancel_commit_ignored, NULL},
    {"prefetch_bound", test_prefetch_bound, NULL},
    {"implicit_commit_fails", test_implicit_commit_fails, NULL},
    {"cursor_past_end", test_cursor_past_end, NULL},
    {"sync_async_equivalence", test_sync_async_equivalence, NULL},
    {"error_accessors", test_error_accessors, NULL},
    {"errors_match_network", test_errors_match_network, NULL},
    {"error_null_out", test_error_null_out, NULL},
    {"null_arguments", test_null_arguments, NULL},
    {"zero_length_tree", test_zero_length_tree, NULL},
    {"ddl_through_execute", test_ddl_through_execute, NULL},
    {"params", test_params, NULL},
    {"options", test_options, NULL},
    {"config_strict", test_config_strict, NULL},
    {"max_in_flight", test_max_in_flight, NULL},
    {"open_modes", test_open_modes, NULL},
    {"read_only_db", test_read_only_db, NULL},
    {"disk_only_calls", test_disk_only_calls, NULL},
    {"promote_roundtrip", test_promote_roundtrip, NULL},
    {"clone_isolation", test_clone_isolation, NULL},
    {"clone_constant_time", test_clone_constant_time, NULL},
    {"lock_in_process", test_lock_in_process, NULL},
    {"lock_child_process", test_lock_child_process, NULL},
    {"transaction_status_after_crash", test_transaction_status_after_crash, NULL},
    {"connect", test_connect, NULL},
    {"close_with_open_sessions", test_close_with_open_sessions, NULL},
    {"conn_close_with_cursors", test_conn_close_with_cursors, NULL},
    {"txn_free_rolls_back", test_txn_free_rolls_back, NULL},
    {"close_order_permutations", test_close_order_permutations, NULL},
    {"metrics", test_metrics, NULL},
    {"event_loop_integration", test_event_loop_integration, NULL},
};

/* Runs one test in a child; answers 0 passed, 1 failed, 2 skipped. */
static int run_one(const struct test *t, int debug) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 2);
        alarm(120);
        t->fn();
        if (t->abort_code && debug) {
            fprintf(stderr, "did not abort\n");
            _exit(1);
        }
        _exit(0);
    }
    close(pipefd[1]);
    char err[8192];
    size_t n = 0;
    ssize_t k;
    while ((k = read(pipefd[0], err + n, sizeof err - 1 - n)) > 0) n += (size_t)k;
    err[n] = 0;
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    int result;
    if (t->abort_code && debug)
        result = (WIFSIGNALED(status) && strstr(err, t->abort_code) != NULL) ? 0 : 1;
    else if (t->abort_code && !debug)
        result = 2; /* a release build's behaviour is undefined here (§1 rule 7) */
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
        result = 2;
    else
        result = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    printf("%s %s\n", result == 0 ? "PASS" : result == 2 ? "SKIP" : "FAIL", t->name);
    if (result == 1) {
        if (WIFSIGNALED(status)) printf("  killed by signal %d\n", WTERMSIG(status));
        printf("%s", err);
    }
    return result;
}

static int debug_build(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(terndb_debug_checks() ? 1 : 0);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 1;
}

int main(int argc, char **argv) {
    int debug = debug_build();
    size_t count = sizeof tests / sizeof *tests;
    int passed = 0, failed = 0, skipped = 0;
    for (size_t i = 0; i < count; i++) {
        int chosen = argc == 1;
        for (int a = 1; a < argc; a++)
            if (strcmp(argv[a], tests[i].name) == 0) chosen = 1;
        if (!chosen) continue;
        switch (run_one(&tests[i], debug)) {
        case 0: passed++; break;
        case 1: failed++; break;
        default: skipped++; break;
        }
    }
    printf("%s build: %d passed, %d failed, %d skipped\n", debug ? "debug" : "release", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}
