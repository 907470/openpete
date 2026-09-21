// core/mods/mod_vec.c — the two growable tables of the mod surface and
// their exit-time census. See mod_vec.h for the contract.

#include "mod_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"

/* Census list. Built during mod init on one thread; walked at exit. */
static mod_vec_node_t* s_census_head;
static int s_census_armed;

static void census_link(mod_vec_node_t* n, const void* vec, const char* name,
                        unsigned long was, int kind) {
    n->vec = vec;
    n->name = name;
    n->was = was;
    n->kind = kind;
    n->next = NULL;
    n->linked = 0;
    if (!name) return;
    n->next = s_census_head;
    s_census_head = n;
    n->linked = 1;
}

static void census_unlink(mod_vec_node_t* n) {
    mod_vec_node_t** p = &s_census_head;
    if (!n->linked) return;
    while (*p) {
        if (*p == n) { *p = n->next; break; }
        p = &(*p)->next;
    }
    n->linked = 0;
    n->next = NULL;
}

/* Smallest power of two at least `n`, with a floor of 1. */
static unsigned shift_for(size_t n) {
    unsigned s = 0;
    while (((size_t)1 << s) < n && s < (sizeof(size_t) * 8 - 1)) s++;
    return s;
}

void mod_cvec_init(mod_cvec* v, size_t elem_size, size_t block_elems,
                   const char* name, unsigned long was) {
    memset(v, 0, sizeof *v);
    v->elem_size = elem_size;
    v->block_shift = shift_for(block_elems ? block_elems : 1);
    census_link(&v->census, v, name, was, 1);
}

/* Publish a block index with room for one more block. The old array is
 * retained rather than freed: a reader on another thread may be inside
 * it. Returns 0 on success. */
static int cvec_grow_index(mod_cvec* v) {
    size_t slots = v->block_slots ? v->block_slots * 2 : 4;
    unsigned char** ix = (unsigned char**)calloc(slots, sizeof *ix);
    mod_vec_retired_t* r;
    if (!ix) return -1;
    if (v->blocks) {
        r = (mod_vec_retired_t*)malloc(sizeof *r);
        if (!r) { free(ix); return -1; }
        memcpy(ix, v->blocks, v->block_count * sizeof *ix);
        r->mem = v->blocks;
        r->next = v->retired;
        v->retired = r;
    }
    v->blocks = ix;
    v->block_slots = slots;
    return 0;
}

/* Append one block. Returns 0 on success. */
static int cvec_add_block(mod_cvec* v) {
    size_t block_elems = (size_t)1 << v->block_shift;
    unsigned char* blk;
    if (v->block_count == v->block_slots && cvec_grow_index(v) != 0) return -1;
    blk = (unsigned char*)calloc(block_elems, v->elem_size);
    if (!blk) return -1;
    v->blocks[v->block_count++] = blk;
    v->grew++;
    return 0;
}

static unsigned char* cvec_slot(const mod_cvec* v, size_t i) {
    size_t b = i >> v->block_shift;
    size_t o = i & (((size_t)1 << v->block_shift) - 1);
    return v->blocks[b] + o * v->elem_size;
}

void* mod_cvec_push(mod_cvec* v) {
    unsigned char* slot;
    if (!v->elem_size) return NULL;
    if (v->count >> v->block_shift == v->block_count && cvec_add_block(v) != 0)
        return NULL;
    slot = cvec_slot(v, v->count);
    memset(slot, 0, v->elem_size);
    v->count++;                  /* published after the element is zeroed */
    if (v->count > v->peak) v->peak = v->count;
    return slot;
}

void* mod_cvec_at(const mod_cvec* v, size_t i) {
    if (i >= v->count) return NULL;
    return cvec_slot(v, i);
}

size_t mod_cvec_count(const mod_cvec* v) { return v->count; }

void mod_cvec_remove_at(mod_cvec* v, size_t i) {
    size_t k;
    if (i >= v->count) return;
    for (k = i + 1; k < v->count; k++)
        memcpy(cvec_slot(v, k - 1), cvec_slot(v, k), v->elem_size);
    v->count--;
}

void mod_cvec_clear(mod_cvec* v) { v->count = 0; }

void mod_cvec_free(mod_cvec* v) {
    size_t b;
    for (b = 0; b < v->block_count; b++) free(v->blocks[b]);
    free(v->blocks);
    while (v->retired) {
        mod_vec_retired_t* r = v->retired;
        v->retired = r->next;
        free(r->mem);
        free(r);
    }
    census_unlink(&v->census);
    memset(v, 0, sizeof *v);
}

void mod_fvec_init(mod_fvec* v, size_t elem_size, size_t init_cap,
                   const char* name, unsigned long was) {
    memset(v, 0, sizeof *v);
    v->elem_size = elem_size;
    v->init_cap = init_cap ? init_cap : 8;
    census_link(&v->census, v, name, was, 0);
}

int mod_fvec_reserve(mod_fvec* v, size_t n) {
    size_t cap = v->cap ? v->cap : v->init_cap;
    unsigned char* d;
    if (!v->elem_size) return -1;
    if (n <= v->cap) return 0;
    while (cap < n) cap *= 2;
    d = (unsigned char*)realloc(v->data, cap * v->elem_size);
    if (!d) return -1;
    memset(d + v->cap * v->elem_size, 0, (cap - v->cap) * v->elem_size);
    v->data = d;
    v->cap = cap;
    v->grew++;
    return 0;
}

void* mod_fvec_push(mod_fvec* v) {
    unsigned char* slot;
    if (mod_fvec_reserve(v, v->count + 1) != 0) return NULL;
    slot = v->data + v->count * v->elem_size;
    memset(slot, 0, v->elem_size);
    v->count++;
    if (v->count > v->peak) v->peak = v->count;
    return slot;
}

void* mod_fvec_at(const mod_fvec* v, size_t i) {
    if (i >= v->count) return NULL;
    return v->data + i * v->elem_size;
}

size_t mod_fvec_count(const mod_fvec* v) { return v->count; }

void* mod_fvec_data(const mod_fvec* v) { return v->data; }

void mod_fvec_remove_at(mod_fvec* v, size_t i) {
    if (i >= v->count) return;
    memmove(v->data + i * v->elem_size, v->data + (i + 1) * v->elem_size,
            (v->count - i - 1) * v->elem_size);
    v->count--;
}

int mod_fvec_resize(mod_fvec* v, size_t n) {
    if (n > v->count) {
        if (mod_fvec_reserve(v, n) != 0) return -1;
        memset(v->data + v->count * v->elem_size, 0,
               (n - v->count) * v->elem_size);
    }
    v->count = n;
    if (v->count > v->peak) v->peak = v->count;
    return 0;
}

void mod_fvec_truncate(mod_fvec* v, size_t n) {
    if (n < v->count) v->count = n;
}

void mod_fvec_clear(mod_fvec* v) { v->count = 0; }

void mod_fvec_free(mod_fvec* v) {
    free(v->data);
    census_unlink(&v->census);
    memset(v, 0, sizeof *v);
}

static int census_enabled(void) {
    const char* e = getenv("OPENPETE_MOD_TABLE_CENSUS");
    return e && *e && strcmp(e, "0") != 0;
}

/* Where the report goes. A bare on-value writes it to stderr, which is
 * what an interactive run wants. Any other value names a file and the
 * report is written there instead.
 *
 * The file form exists because stderr is not a plain descriptor here:
 * the log mirror dup2's it onto a pipe and a pump thread copies the
 * bytes out. The report is a burst at exit, so its tail races the pump
 * and is lost intermittently — a handful of rows on one run, none on
 * another. A reader that asserts one row per table needs the whole
 * report, and a file is not on the pipe. */
static FILE* census_open(int* needs_close) {
    const char* e = getenv("OPENPETE_MOD_TABLE_CENSUS");
    *needs_close = 0;
    if (!e || !*e || strcmp(e, "1") == 0) return stderr;
    FILE* f = fopen(e, "wb");
    if (!f) {
        fprintf(stderr, "mod-table: cannot write %s\n", e);
        return stderr;
    }
    *needs_close = 1;
    return f;
}

static void census_row(FILE* out, const mod_vec_node_t* n) {
    size_t count, peak, storage;
    unsigned long grew;
    if (n->kind == MOD_VEC_FIXED) {
        const mod_fixed* t = (const mod_fixed*)n->vec;
        count = t->count; peak = t->peak; storage = t->storage; grew = 0;
    } else if (n->kind == MOD_VEC_CVEC) {
        const mod_cvec* v = (const mod_cvec*)n->vec;
        count = v->count; peak = v->peak; storage = v->block_count;
        grew = v->grew;
    } else {
        const mod_fvec* v = (const mod_fvec*)n->vec;
        count = v->count; peak = v->peak; storage = v->cap; grew = v->grew;
    }
    fprintf(out, "mod-table: %s count=%zu peak=%zu was=%lu "
            "storage=%zu grew=%lu\n",
            n->name, count, peak, n->was, storage, grew);
}

void mod_fixed_init(mod_fixed* t, const char* name, unsigned long was,
                    size_t storage) {
    if (!t) return;
    t->count = 0;
    t->peak = 0;
    t->storage = storage;
    census_link(&t->census, t, name, was, MOD_VEC_FIXED);
}

void mod_fixed_note(mod_fixed* t, size_t count, size_t storage) {
    if (!t) return;
    mod_fixed_set(t, count);
    if (storage > t->storage) t->storage = storage;
}

void mod_fixed_set(mod_fixed* t, size_t count) {
    if (!t) return;
    t->count = count;
    if (count > t->peak) t->peak = count;
}

/* The `tables=` header states how many rows follow, so a reader can tell
 * a short report from a complete one without knowing which tables to
 * expect. */
void mod_vec_census_report(void) {
    const mod_vec_node_t* n;
    size_t tables = 0;
    int needs_close = 0;
    FILE* out;
    if (!census_enabled()) return;
    out = census_open(&needs_close);
    for (n = s_census_head; n; n = n->next) tables++;
    fprintf(out, "mod-table: tables=%zu\n", tables);
    for (n = s_census_head; n; n = n->next) census_row(out, n);
    fflush(out);
    if (needs_close) fclose(out);
}

void mod_vec_census_arm(void) {
    if (s_census_armed) return;
    s_census_armed = 1;
    atexit(mod_vec_census_report);
}
