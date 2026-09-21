// tests/unit/mod_vec_test.c — the mod surface's two growable tables
// (core/mods/mod_vec.c): growth past one block, address stability, the
// order-preserving remove, clear keeping storage, the no-ops on a zeroed
// table, the flat table's contiguity, and the census line format.

#include "mod_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

typedef struct { int v; char pad[12]; } elem_t;

/* Block of 4 forces three blocks at 9 elements. */
#define BLOCK 4
#define N 9

static void test_cvec_growth_and_stability(void) {
    mod_cvec v;
    void* first;
    void* second_block;
    int i;
    mod_cvec_init(&v, sizeof(elem_t), BLOCK, NULL, 0);
    for (i = 0; i < N; i++) {
        elem_t* e = (elem_t*)mod_cvec_push(&v);
        CHECK(e != NULL);
        if (!e) return;
        e->v = i;
        if (i == 0) first = e;
        if (i == BLOCK) second_block = e;
    }
    CHECK(mod_cvec_count(&v) == (size_t)N);
    CHECK(v.block_count == 3);                 /* pushed past one block */
    CHECK(mod_cvec_at(&v, 0) == first);        /* address survives growth */
    CHECK(mod_cvec_at(&v, BLOCK) == second_block);
    CHECK(((elem_t*)first)->v == 0);
    CHECK(((elem_t*)second_block)->v == BLOCK);
    for (i = 0; i < N; i++)
        CHECK(((elem_t*)mod_cvec_at(&v, (size_t)i))->v == i);
    CHECK(mod_cvec_at(&v, (size_t)N) == NULL);

    /* remove_at keeps the order of the survivors. */
    mod_cvec_remove_at(&v, 2);
    CHECK(mod_cvec_count(&v) == (size_t)N - 1);
    for (i = 0; i < N - 1; i++) {
        int want = (i < 2) ? i : i + 1;
        CHECK(((elem_t*)mod_cvec_at(&v, (size_t)i))->v == want);
    }
    mod_cvec_remove_at(&v, 100);               /* out of range: no-op */
    CHECK(mod_cvec_count(&v) == (size_t)N - 1);

    /* clear keeps the blocks; the next push reuses block 0. */
    mod_cvec_clear(&v);
    CHECK(mod_cvec_count(&v) == 0);
    CHECK(v.peak == (size_t)N);                /* the drained peak is kept */
    CHECK(v.block_count == 3);
    CHECK(v.grew == 3);
    CHECK(mod_cvec_push(&v) == first);
    CHECK(((elem_t*)first)->v == 0);           /* push zeroes the element */
    mod_cvec_free(&v);
    CHECK(v.block_count == 0 && v.count == 0 && v.blocks == NULL);
}

static void test_fvec_growth_and_contiguity(void) {
    mod_fvec v;
    unsigned char* base;
    int i;
    mod_fvec_init(&v, sizeof(elem_t), 2, NULL, 0);
    for (i = 0; i < N; i++) {
        elem_t* e = (elem_t*)mod_fvec_push(&v);
        CHECK(e != NULL);
        if (!e) return;
        e->v = i;
    }
    CHECK(mod_fvec_count(&v) == (size_t)N);
    CHECK(v.cap >= (size_t)N);
    CHECK(v.grew == 4);                        /* 0 -> 2 -> 4 -> 8 -> 16 */
    base = (unsigned char*)mod_fvec_data(&v);
    CHECK(base != NULL);
    for (i = 0; i < N; i++) {
        /* Contiguous after growth: element i sits at base + i*size. */
        CHECK((unsigned char*)mod_fvec_at(&v, (size_t)i) ==
              base + (size_t)i * sizeof(elem_t));
        CHECK(((elem_t*)(base + (size_t)i * sizeof(elem_t)))->v == i);
    }
    CHECK(mod_fvec_at(&v, (size_t)N) == NULL);

    mod_fvec_remove_at(&v, 0);
    CHECK(mod_fvec_count(&v) == (size_t)N - 1);
    for (i = 0; i < N - 1; i++)
        CHECK(((elem_t*)mod_fvec_at(&v, (size_t)i))->v == i + 1);

    /* resize grows with zeroed elements and shrinks the count. */
    CHECK(mod_fvec_resize(&v, N + 3) == 0);
    CHECK(mod_fvec_count(&v) == (size_t)N + 3);
    CHECK(((elem_t*)mod_fvec_at(&v, (size_t)N + 2))->v == 0);
    CHECK(mod_fvec_resize(&v, (size_t)N - 1) == 0);
    CHECK(mod_fvec_count(&v) == (size_t)N - 1);

    /* truncate drops the tail and keeps the survivors and the storage. */
    mod_fvec_truncate(&v, 3);
    CHECK(mod_fvec_count(&v) == 3);
    for (i = 0; i < 3; i++)
        CHECK(((elem_t*)mod_fvec_at(&v, (size_t)i))->v == i + 1);
    mod_fvec_truncate(&v, 9);                  /* above the count: no-op */
    CHECK(mod_fvec_count(&v) == 3);

    mod_fvec_clear(&v);
    CHECK(mod_fvec_count(&v) == 0);
    CHECK(v.cap >= (size_t)N);                 /* clear keeps the storage */
    CHECK((unsigned char*)mod_fvec_data(&v) == base);
    CHECK(mod_fvec_push(&v) == base);
    CHECK(((elem_t*)base)->v == 0);
    mod_fvec_free(&v);
    CHECK(v.cap == 0 && v.count == 0 && v.data == NULL);
}

static void test_zeroed(void) {
    mod_cvec c;
    mod_fvec f;
    memset(&c, 0, sizeof c);
    memset(&f, 0, sizeof f);
    CHECK(mod_cvec_count(&c) == 0);
    CHECK(mod_cvec_at(&c, 0) == NULL);
    CHECK(mod_cvec_push(&c) == NULL);          /* no element size */
    mod_cvec_remove_at(&c, 0);
    mod_cvec_clear(&c);
    mod_cvec_free(&c);
    mod_cvec_free(&c);                         /* free of a zeroed table */
    CHECK(mod_fvec_count(&f) == 0);
    CHECK(mod_fvec_at(&f, 0) == NULL);
    CHECK(mod_fvec_push(&f) == NULL);
    mod_fvec_remove_at(&f, 0);
    mod_fvec_truncate(&f, 0);
    mod_fvec_clear(&f);
    mod_fvec_free(&f);
    mod_fvec_free(&f);
}

/* The census report writes through the logger to stderr. Redirect fd 2
 * to a file, run the report, and read the lines back. */
static void test_census(void) {
    const char* path = "mod_vec_test_census.txt";
    mod_cvec c;
    mod_fvec f;
    char buf[4096];
    FILE* fp;
    size_t n;
    int i;

    mod_cvec_init(&c, sizeof(elem_t), BLOCK, "chunked_table", 4);
    mod_fvec_init(&f, sizeof(elem_t), 2, "flat_table", 2);
    for (i = 0; i < N; i++) { (void)mod_cvec_push(&c); (void)mod_fvec_push(&f); }

    remove(path);
    fflush(stderr);
    if (!freopen(path, "w", stderr)) { printf("FAIL: freopen\n"); s_fail++; return; }
    /* putenv, not setenv: the MinGW CRT has no setenv. An empty
     * value reads as disabled. */
    putenv((char*)"OPENPETE_MOD_TABLE_CENSUS=1");
    mod_vec_census_report();
    putenv((char*)"OPENPETE_MOD_TABLE_CENSUS=");
    mod_vec_census_report();                   /* silent without the env var */
    fflush(stderr);

    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (!fp) return;
    n = fread(buf, 1, sizeof buf - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(path);
    printf("census output:\n%s", buf);
    CHECK(strstr(buf, "mod-table: tables=2") != NULL);
    CHECK(strstr(buf,
        "mod-table: chunked_table count=9 peak=9 was=4 storage=3 grew=3") != NULL);
    CHECK(strstr(buf,
        "mod-table: flat_table count=9 peak=9 was=2 storage=16 grew=4") != NULL);
    /* One report, not two: the second call saw no env var. */
    {
        const char* p = buf;
        int reports = 0;
        while ((p = strstr(p, "mod-table: tables=")) != NULL) { reports++; p++; }
        CHECK(reports == 1);
    }

    /* A freed table leaves the census list. */
    mod_cvec_free(&c);
    mod_fvec_free(&f);
}

int main(void) {
    openpete_log_init(OPENPETE_LOG_PLAIN, "*:info");
    test_cvec_growth_and_stability();
    test_fvec_growth_and_contiguity();
    test_zeroed();
    test_census();
    if (s_fail) { printf("mod_vec_test: %d failure(s)\n", s_fail); return 1; }
    printf("mod_vec_test: PASS\n");
    return 0;
}
