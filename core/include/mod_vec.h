// core/include/mod_vec.h — the two growable tables (mod surface, renderer scene buffers).
//
// Tables on the mod surface, and the renderer's triangle and vertex
// buffers, are sized by their content. Two containers cover every
// consumer shape; a third growth scheme is a duplicate.
//
//   mod_cvec  chunked. Elements live in fixed-size blocks linked from a
//             block index; a push never moves an element, so an address
//             handed out earlier stays valid for the life of the table.
//             Index access is a shift and a mask. Pick it when a
//             consumer holds an element address across a call, or when a
//             thread other than the writer reads the table: a realloc
//             move would turn a stale read into a use-after-free, while
//             a block never moves.
//   mod_fvec  flat, one realloc'd run. Pick it for every other table,
//             and for the consumers that need contiguity: qsort, a
//             whole-table memcpy, an open-addressed hash sized to the
//             table.
//
// Both are used from C and from C++ translation units.
//
// Threading. A vec has one writer. mod_fvec additionally has one owner
// for reads: a realloc moves the run, so a reader on another thread can
// observe a freed pointer. mod_cvec tolerates a concurrent reader that
// walks indices below the count it loaded: blocks never move, the block
// index grows by copy-then-publish, and a pushed element is zeroed
// before the count admits it, so the reader sees a zeroed element rather
// than uninitialized bytes. init, free and clear are writer-side and are
// not concurrent with a reader. Tables owned by different threads may
// init and free concurrently: the census list they link into is locked.
//
// Allocation failure is the only failure mode: push returns NULL and the
// caller reports it with the table's name and the owner.
//
// A zeroed vec is a valid empty vec for count, at, clear, remove_at and
// free; push needs init, since the element size lives in the struct. A
// static table therefore inits on its first push: `elem_size` is zero
// until init has run and nonzero afterwards, which is the test.

#ifndef OPENPETE_MOD_VEC_H
#define OPENPETE_MOD_VEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Census node, embedded in each vec. A named vec links itself into the
 * process-wide census list at init and unlinks at free, so the list
 * never holds a pointer to released storage. */
/* A superseded block index. The index array is published by
 * copy-then-swap so a concurrent reader never dereferences a freed
 * array; the old array is retained here and released with the table. */
typedef struct mod_vec_retired {
    struct mod_vec_retired* next;
    void* mem;
} mod_vec_retired_t;

#define MOD_VEC_FVEC  0
#define MOD_VEC_CVEC  1
#define MOD_VEC_FIXED 2

typedef struct mod_vec_node {
    struct mod_vec_node* next;
    const void* vec;            /* owning mod_cvec or mod_fvec */
    const char* name;           /* table name on the census line */
    unsigned long was;          /* capacity the table had as a fixed array */
    int kind;                   /* MOD_VEC_FVEC / _CVEC / _FIXED */
    int linked;                 /* node is in the census list */
} mod_vec_node_t;

/** Chunked, address-stable table. */
typedef struct mod_cvec {
    unsigned char** blocks;     /* block_count pointers, each block_elems elements */
    size_t block_count;
    size_t block_slots;         /* allocated length of `blocks` */
    size_t count;
    size_t elem_size;
    unsigned block_shift;       /* block_elems = (size_t)1 << block_shift */
    unsigned long grew;         /* block allocations since init */
    size_t peak;                /* highest count the table has held */
    mod_vec_retired_t* retired; /* superseded block indices */
    mod_vec_node_t census;
} mod_cvec;

/** Flat, contiguous table. */
typedef struct mod_fvec {
    unsigned char* data;
    size_t count;
    size_t cap;
    size_t elem_size;
    size_t init_cap;            /* capacity the first push allocates */
    unsigned long grew;         /* reallocations since init */
    size_t peak;                /* highest count the table has held */
    size_t failed_at;           /* largest request an allocation refused */
    mod_vec_node_t census;
} mod_fvec;

/**
 * @brief Initialize a chunked table.
 * @param v          Table, zeroed or previously freed.
 * @param elem_size  Element size in bytes, nonzero.
 * @param block_elems Elements per block; rounded up to a power of two.
 *                   Sized to the table's former fixed capacity, so a
 *                   stock mod set allocates one block.
 * @param name       Census name, or NULL to stay out of the census.
 * @param was        Former fixed capacity, printed on the census line.
 */
void mod_cvec_init(mod_cvec* v, size_t elem_size, size_t block_elems,
                   const char* name, unsigned long was);

/**
 * @brief Append one zeroed element.
 * @return Address of the new element, stable for the life of the table,
 *         or NULL on allocation failure or on an uninitialized table.
 */
void* mod_cvec_push(mod_cvec* v);

/**
 * @brief Address of element @p i.
 * @return NULL when @p i is at or past the count.
 */
void* mod_cvec_at(const mod_cvec* v, size_t i);

/** @brief Number of elements. */
size_t mod_cvec_count(const mod_cvec* v);

/**
 * @brief Remove element @p i, shifting the elements above it down one.
 *        The order of the remaining elements is preserved and every
 *        element above @p i changes index. Out-of-range is a no-op.
 */
void mod_cvec_remove_at(mod_cvec* v, size_t i);

/** @brief Drop every element, keeping the blocks for the next fill. */
void mod_cvec_clear(mod_cvec* v);

/** @brief Release the blocks, unlink the census node and zero the table. */
void mod_cvec_free(mod_cvec* v);

/**
 * @brief Initialize a flat table.
 * @param v          Table, zeroed or previously freed.
 * @param elem_size  Element size in bytes, nonzero.
 * @param init_cap   Capacity the first push allocates; sized to the
 *                   table's former fixed capacity.
 * @param name       Census name, or NULL to stay out of the census.
 * @param was        Former fixed capacity, printed on the census line.
 */
void mod_fvec_init(mod_fvec* v, size_t elem_size, size_t init_cap,
                   const char* name, unsigned long was);

/**
 * @brief Append one zeroed element.
 * @return Address of the new element, invalidated by the next growth, or
 *         NULL on allocation failure or on an uninitialized table.
 */
void* mod_fvec_push(mod_fvec* v);

/**
 * @brief Address of element @p i.
 * @return NULL when @p i is at or past the count.
 */
void* mod_fvec_at(const mod_fvec* v, size_t i);

/** @brief Number of elements. */
size_t mod_fvec_count(const mod_fvec* v);

/**
 * @brief Base address of the contiguous run, for qsort, memcpy and hash
 *        consumers.
 * @return NULL while nothing is allocated.
 */
void* mod_fvec_data(const mod_fvec* v);

/**
 * @brief Grow the capacity to at least @p n elements without changing
 *        the count.
 * @return 0 on success, -1 on allocation failure.
 */
int mod_fvec_reserve(mod_fvec* v, size_t n);

/**
 * @brief mod_fvec_reserve for a run whose caller keeps its own element
 *        count (a buffer refilled per use and indexed through
 *        mod_fvec_data). @p n raises the peak. A growth to a capacity above
 *        the table's former fixed capacity (`was`), and each allocation
 *        failure at a request size larger than any refused before, is
 *        logged under the census name.
 * @return 0 on success, -1 on allocation failure; the run and its
 *         capacity are then unchanged, and the caller fills what fits.
 */
int mod_fvec_reserve_noted(mod_fvec* v, size_t n);

/**
 * @brief Remove element @p i, shifting the elements above it down one.
 *        The order of the remaining elements is preserved and every
 *        element above @p i changes index. Out-of-range is a no-op.
 */
void mod_fvec_remove_at(mod_fvec* v, size_t i);

/**
 * @brief Set the count to @p n, zeroing any elements this adds.
 *        For a slot table whose size is a function of another table.
 * @return 0 on success, -1 on allocation failure (the count is unchanged).
 */
int mod_fvec_resize(mod_fvec* v, size_t n);

/**
 * @brief Drop every element above index @p n, keeping the capacity.
 *        For the compact-in-place idiom: the caller copies the survivors
 *        down to [0, n) and calls this to drop the tail. A count at or
 *        below @p n is left alone.
 */
void mod_fvec_truncate(mod_fvec* v, size_t n);

/** @brief Drop every element, keeping the capacity for the next fill. */
void mod_fvec_clear(mod_fvec* v);

/** @brief Release the run, unlink the census node and zero the table. */
void mod_fvec_free(mod_fvec* v);

/**
 * @brief A fixed table held at a real bound, reported beside the growable
 *        ones.
 *
 * Some tables cannot grow past a bound the format fixes (an index carried
 * in a tag's spare bits, an ABI field). Their storage is still sized to
 * that bound rather than to a guess, and the census shows how much of it a
 * run uses, so the bound can be checked against real content the same way
 * a growable table's high-water is.
 */
typedef struct mod_fixed {
    size_t count;
    size_t peak;
    size_t storage;             /* elements the storage holds */
    mod_vec_node_t census;
} mod_fixed;

/**
 * @brief Register a fixed table in the census.
 * @param t      The table.
 * @param name   Census name, or NULL to stay out of the census.
 * @param was    Capacity the table had before it was sized to the bound.
 * @param storage  Elements the storage holds.
 */
void mod_fixed_init(mod_fixed* t, const char* name, unsigned long was,
                    size_t storage);
/** @brief Publish the table's live element count, tracking its high-water. */
void mod_fixed_set(mod_fixed* t, size_t count);
/**
 * @brief Publish a count and storage for a table that is not one object.
 *
 * For a row table that every mod owns a copy of: the census node cannot
 * live in the copies, because a copy duplicates the node while the list
 * still points at the original. One process-global counter stands for
 * the whole population instead, so `peak` is the largest any one mod
 * reached and `count` is the most recent.
 */
void mod_fixed_note(mod_fixed* t, size_t count, size_t storage);

/**
 * @brief Arm the exit-time census report.
 *
 * Called once from the mod loader's init. The report runs at exit and
 * prints nothing unless OPENPETE_MOD_TABLE_CENSUS is set to a value
 * other than 0.
 */
void mod_vec_census_arm(void);

/**
 * @brief Print the census: one summary line naming the number of tables,
 *        then one line per named table,
 *        "mod-table: {name} count={n} peak={p} was={former capacity}
 *        storage={blocks for a cvec, capacity for an fvec} grew={k}".
 *
 * peak against was is what shows a table having carried more than its
 * former fixed capacity: count is the live size, which a table drained
 * every tick reports as 0 whatever it held in between. grew counts
 * allocations and is 1 on a table that only ever allocated its first
 * block.
 */
void mod_vec_census_report(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_VEC_H */
