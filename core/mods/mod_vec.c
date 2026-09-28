// core/mods/mod_vec.c — the two growable tables of the mod surface and
// their exit-time census. See mod_vec.h for the contract.

#include "mod_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"

/* Census list. Tables init from the mod loader, the simulation thread and
 * the display thread, so link and unlink hold a spin lock; the exit-time
 * walk runs after those threads have stopped. */
static mod_vec_node_t* s_census_head;
static int s_census_armed;
static char s_census_lock;

/** @brief Take the census list's spin lock. */
static void census_lock(void) {
    while (__atomic_test_and_set(&s_census_lock, __ATOMIC_ACQUIRE)) { }
}

/** @brief Release the census list's spin lock. */
static void census_unlock(void) {
    __atomic_clear(&s_census_lock, __ATOMIC_RELEASE);
}

/** @brief Fill a census node and link it into the census list when named.
 *
 * An unnamed table (NULL @p name) is filled but not linked, so the list and
 * the exit report never see it.
 * @param n    Node embedded in the table.
 * @param vec  Owning table.
 * @param name Census name, or NULL.
 * @param was  Former fixed capacity.
 * @param kind MOD_VEC_FVEC, MOD_VEC_CVEC or MOD_VEC_FIXED. */
static void census_link(mod_vec_node_t* n, const void* vec, const char* name,
                        unsigned long was, int kind) {
    n->vec = vec;
    n->name = name;
    n->was = was;
    n->kind = kind;
    n->next = NULL;
    n->linked = 0;
    if (!name) return;
    census_lock();
    n->next = s_census_head;
    s_census_head = n;
    n->linked = 1;
    census_unlock();
}

/** @brief Remove a node from the census list when it is linked.
 *
 * Clears the node's link state so a second call is a no-op.
 * @param n Node embedded in the table. */
static void census_unlink(mod_vec_node_t* n) {
    mod_vec_node_t** p = &s_census_head;
    if (!n->linked) return;
    census_lock();
    while (*p) {
        if (*p == n) { *p = n->next; break; }
        p = &(*p)->next;
    }
    census_unlock();
    n->linked = 0;
    n->next = NULL;
}

/** @brief Return the exponent of the smallest power of two at least @p n.
 *
 * 0 for @p n <= 1; capped at one less than the bit width of size_t. */
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

/** @brief Replace the block index with one of twice the slots, 4 for the first.
 *
 * The old index is copied into the new one and kept on the retired list
 * rather than freed, since a reader on another thread may be inside it;
 * the list is freed with the table.
 * @return 0, or -1 on an allocation failure, which leaves the table
 *         unchanged. */
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

/** @brief Append one zeroed block of 2^block_shift elements, growing the index when full.
 *
 * Counts the growth in the table's `grew` census field.
 * @return 0, or -1 on an allocation failure. */
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

/** @brief Address of element @p i through the block index.
 *
 * Block @p i >> block_shift, offset @p i masked to the block size; the
 * block must already be allocated.
 * @param v Table.
 * @param i Element index below the allocated capacity. */
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

int mod_fvec_reserve_noted(mod_fvec* v, size_t n) {
    const char* name = v->census.name ? v->census.name : "(unnamed)";
    size_t before = v->cap;
    if (n > v->peak) v->peak = n;
    if (n <= v->cap) return 0;
    if (mod_fvec_reserve(v, n) != 0) {
        if (n <= v->failed_at) return -1;   /* this size was already reported */
        v->failed_at = n;
        OPL_ERROR_F("vec", "%s: allocation FAILED at %zu elements (%.1f MiB); "
                    "holding %zu, the rest is not stored", name, n,
                    (double)n * (double)v->elem_size / (1024.0 * 1024.0), before);
        return -1;
    }
    if (v->cap > v->census.was)
        OPL_WARN_F("vec", "%s grew %zu -> %zu elements (%.1f MiB; former fixed "
                   "capacity %lu)", name, before, v->cap,
                   (double)v->cap * (double)v->elem_size / (1024.0 * 1024.0),
                   v->census.was);
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

/** @brief Report whether the census output is enabled.
 *
 * True when OPENPETE_MOD_TABLE_CENSUS is set to a non-empty value other
 * than "0".
 *
 * @return 1 when enabled, else 0. */
static int census_enabled(void) {
    const char* e = getenv("OPENPETE_MOD_TABLE_CENSUS");
    return e && *e && strcmp(e, "0") != 0;
}

/** @brief Open the census output named by OPENPETE_MOD_TABLE_CENSUS.
 *
 * The value 1 (or an empty value) selects stderr, for an interactive run.
 * Any other value names a file, opened for writing, so a reader that
 * asserts one row per table gets the census without the rest of the run's
 * stderr interleaved; a file that cannot be opened logs a line and falls
 * back to stderr.
 * @param needs_close Receives 1 when the caller must fclose the stream.
 * @return The output stream. */
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

/** @brief Print one census line for a linked table.
 *
 * Count, peak and storage come from the table the node's kind names; grew
 * is 0 for a fixed table.
 * @param out Output stream.
 * @param n   Linked node. */
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

/* Writes the tables= line, then one row per linked table; see the header. */
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
