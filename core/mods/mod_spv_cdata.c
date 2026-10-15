/* mod_spv_cdata.c - move large constant tables of a material fragment's
 * SPIR-V into a read-only storage buffer (see mod_spv_cdata.h). */

#include "mod_spv_cdata.h"

#include <stdlib.h>
#include <string.h>

enum {
    SPV_MAGIC = 0x07230203u,
    OP_NAME = 5, OP_ENTRY_POINT = 15,
    OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
    OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
    OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44, OP_CONSTANT_NULL = 46,
    OP_FUNCTION = 54, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59,
    OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65,
    OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72,
    OP_LABEL = 248,
    SC_UNIFORM = 2, SC_PRIVATE = 6, SC_FUNCTION = 7, SC_STORAGE_BUFFER = 12,
    DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_ARRAY_STRIDE = 6,
    DEC_NON_WRITABLE = 24, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34,
    DEC_OFFSET = 35,
};

/* One candidate table: an array variable of Function or Private storage. */
typedef struct {
    uint32_t var, sc, value, arr, elem, len_id;
    uint32_t count, comps, stride;
    size_t   store_at;      /* word offset of its OpStore; 0 = none */
    uint32_t store_blk;     /* block counter at the store */
    int      has_init, bad, dynamic;
    int      member;        /* block member, -1 = stays */
} tab_t;

/* The module and the scan state shared by the passes. */
typedef struct {
    const uint32_t* w;
    size_t   n;
    uint32_t bound, version;
    uint32_t out_bound;     /* bound of the rewritten module */
    size_t*  def;           /* [bound] word offset of a type/constant/variable */
    int32_t* var_tab;       /* [bound] table index + 1 of a candidate variable */
    int32_t* ac_tab;        /* [bound] table index + 1 of an access chain result */
    uint32_t* ptr_of;       /* [bound] new pointer type id per pointee type */
    uint8_t* drop;          /* [bound] 1 = id removed from the module */
    tab_t*   tabs;
    int      ntabs;
    size_t   types_at, func_at;
    uint32_t entry_fn;
    /* scan state */
    uint32_t cur_fn, blk, fn_labels;
    int      entry_first, entry_called;
} spv_t;

/* A growable word buffer for the output module. */
typedef struct { uint32_t* w; size_t n, cap; int oom; } wbuf_t;

/** @brief Append words to an output buffer, recording an allocation failure in it. */
static void wb_put(wbuf_t* b, const uint32_t* src, size_t k) {
    if (b->oom) return;
    if (b->n + k > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + k) cap *= 2;
        uint32_t* nw = (uint32_t*)realloc(b->w, cap * sizeof *nw);
        if (!nw) { b->oom = 1; return; }
        b->w = nw; b->cap = cap;
    }
    memcpy(b->w + b->n, src, k * sizeof *src);
    b->n += k;
}

/** @brief Append one instruction of up to six operand words to an output buffer. */
static void wb_ins(wbuf_t* b, uint32_t op, int nops, uint32_t a, uint32_t c,
                   uint32_t d, uint32_t e) {
    const uint32_t ins[5] = { op | ((uint32_t)(nops + 1) << 16), a, c, d, e };
    wb_put(b, ins, (size_t)nops + 1);
}

/** @brief Return the result id of a type, constant or variable instruction, or 0 for any other opcode. */
static uint32_t def_result(const uint32_t* ins, uint32_t op) {
    switch (op) {
    case OP_TYPE_INT: case OP_TYPE_FLOAT: case OP_TYPE_VECTOR:
    case OP_TYPE_ARRAY: case OP_TYPE_STRUCT: case OP_TYPE_POINTER:
        return ins[1];
    case OP_CONSTANT: case OP_CONSTANT_COMPOSITE: case OP_CONSTANT_NULL:
    case OP_VARIABLE:
        return ins[2];
    default:
        return 0;
    }
}

/** @brief Return 1 for an opcode of the module's preamble: capabilities through annotations, debug lines excluded.
 *
 * OpSourceContinued through OpMemberName (2-6), OpString (7), OpExtension
 * through OpCapability (10-17), the decoration group (71-75), OpModuleProcessed
 * (330), OpExecutionModeId (331), OpDecorateId (332) and the string decorations (5632, 5633). */
static int is_preamble_op(uint32_t op) {
    return (op >= 2 && op <= 7) || (op >= 10 && op <= 17) ||
           (op >= 71 && op <= 75) || (op >= 330 && op <= 332) ||
           op == 5632 || op == 5633;
}

/** @brief Index the module: definition offsets, the end of the annotations, the first function and the entry function.
 *
 * @return 0, or -1 when an instruction's word count is 0 or runs past the
 *         module, a result id is not below the bound, or no function or
 *         entry point exists. */
static int spv_index(spv_t* s) {
    s->types_at = 0;
    s->func_at = 0;
    for (size_t i = 5; i < s->n;) {
        const uint32_t wc = s->w[i] >> 16, op = s->w[i] & 0xFFFFu;
        if (wc == 0 || wc > s->n - i) return -1;
        if (!s->types_at && !is_preamble_op(op)) s->types_at = i;
        if (op == OP_ENTRY_POINT && !s->entry_fn && wc >= 3) s->entry_fn = s->w[i + 2];
        if (op == OP_FUNCTION && !s->func_at) s->func_at = i;
        const uint32_t id = wc >= 3 ? def_result(&s->w[i], op) : 0;
        if (id >= s->bound) return -1;
        if (id) s->def[id] = i;
        i += wc;
    }
    return (s->types_at && s->func_at && s->entry_fn) ? 0 : -1;
}

/** @brief Return the defining instruction of an id, or NULL when the id is not a recorded definition of the given opcode. */
static const uint32_t* def_of(const spv_t* s, uint32_t id, uint32_t op) {
    if (id == 0 || id >= s->bound || !s->def[id]) return NULL;
    const uint32_t* ins = &s->w[s->def[id]];
    return (ins[0] & 0xFFFFu) == op ? ins : NULL;
}

/** @brief Return 1 when a type id is a 32-bit OpTypeInt or OpTypeFloat. */
static int is_word_scalar(const spv_t* s, uint32_t ty) {
    const uint32_t* t = def_of(s, ty, OP_TYPE_INT);
    if (!t) t = def_of(s, ty, OP_TYPE_FLOAT);
    return t && t[2] == 32;
}

/** @brief Classify a candidate variable's array type, filling the element, count, component count and std430 stride.
 *
 * @return 1 when the pointee is an array of a 32-bit scalar or a 2-4
 *         component vector of one, with a positive OpConstant length. */
static int tab_type(const spv_t* s, uint32_t ptr_ty, tab_t* t) {
    const uint32_t* p = def_of(s, ptr_ty, OP_TYPE_POINTER);
    const uint32_t* a = p ? def_of(s, p[3], OP_TYPE_ARRAY) : NULL;
    const uint32_t* len = a ? def_of(s, a[3], OP_CONSTANT) : NULL;
    if (!len || (len[0] >> 16) != 4 || len[3] == 0 || len[3] > 0x7FFFFFFFu) return 0;
    t->arr = p[3];
    t->elem = a[2];
    t->len_id = a[3];
    t->count = len[3];
    const uint32_t* v = def_of(s, a[2], OP_TYPE_VECTOR);
    if (v) {
        if (!is_word_scalar(s, v[2]) || v[3] < 2 || v[3] > 4) return 0;
        t->comps = v[3];
    } else {
        if (!is_word_scalar(s, a[2])) return 0;
        t->comps = 1;
    }
    t->stride = t->comps == 1 ? 4u : t->comps == 2 ? 8u : 16u;
    return 1;
}

/** @brief Return 1 when an id is an OpConstantComposite or OpConstantNull of exactly the given array type. */
static int is_table_value(const spv_t* s, uint32_t id, uint32_t arr) {
    const uint32_t* c = def_of(s, id, OP_CONSTANT_COMPOSITE);
    if (!c) c = def_of(s, id, OP_CONSTANT_NULL);
    return c && c[1] == arr;
}

/** @brief Record every Function or Private array variable of a qualifying type as a candidate table.
 *
 * A variable with an initializer qualifies only when the initializer is a
 * constant of its own array type.
 * @return 0, or -1 on an allocation failure. */
static int find_candidates(spv_t* s) {
    int cap = 0;
    for (size_t i = 5; i < s->n; i += s->w[i] >> 16) {
        const uint32_t* ins = &s->w[i];
        const uint32_t wc = ins[0] >> 16;
        if ((ins[0] & 0xFFFFu) != OP_VARIABLE || wc < 4) continue;
        if (ins[3] != SC_FUNCTION && ins[3] != SC_PRIVATE) continue;
        tab_t t;
        memset(&t, 0, sizeof t);
        t.var = ins[2];
        t.sc = ins[3];
        t.member = -1;
        if (!tab_type(s, ins[1], &t)) continue;
        if (wc >= 5) {
            if (!is_table_value(s, ins[4], t.arr)) continue;
            t.has_init = 1;
            t.value = ins[4];
        }
        if (s->ntabs == cap) {
            cap = cap ? cap * 2 : 8;
            tab_t* nt = (tab_t*)realloc(s->tabs, (size_t)cap * sizeof *nt);
            if (!nt) return -1;
            s->tabs = nt;
        }
        s->tabs[s->ntabs] = t;
        s->var_tab[t.var] = ++s->ntabs;
    }
    return 0;
}

/** @brief Disqualify the table behind any candidate variable or access-chain result among an instruction's words.
 *
 * Words [from, wc) are tested as ids without decoding the operand kinds, so
 * a literal equal to such an id also disqualifies; that costs a missed move,
 * never a wrong rewrite. The pointer operand of an OpLoad (word 3) is the
 * one use an access-chain result may have. */
static void scan_generic(spv_t* s, const uint32_t* ins, uint32_t from) {
    const uint32_t wc = ins[0] >> 16, op = ins[0] & 0xFFFFu;
    for (uint32_t k = from; k < wc; k++) {
        const uint32_t x = ins[k];
        if (x >= s->bound) continue;
        if (s->var_tab[x]) s->tabs[s->var_tab[x] - 1].bad = 1;
        if (s->ac_tab[x] && !(op == OP_LOAD && k == 3))
            s->tabs[s->ac_tab[x] - 1].bad = 1;
    }
}

/** @brief Scan an OpStore: the one store of a constant that fills a candidate table, or a disqualifying use.
 *
 * A Private table's store counts only in the entry function's first block
 * before any call; a Function table's records its block for the access
 * chains to match. */
static void scan_store(spv_t* s, const uint32_t* ins, size_t at) {
    if ((ins[0] >> 16) < 3) return;
    const uint32_t ptr = ins[1];
    scan_generic(s, ins, 2);
    if (ptr >= s->bound) return;
    if (s->ac_tab[ptr]) { s->tabs[s->ac_tab[ptr] - 1].bad = 1; return; }
    if (!s->var_tab[ptr]) return;
    tab_t* t = &s->tabs[s->var_tab[ptr] - 1];
    const int placed = t->sc == SC_FUNCTION ||
                       (s->cur_fn == s->entry_fn && s->entry_first && !s->entry_called);
    if (t->has_init || t->store_at || !placed || !is_table_value(s, ins[2], t->arr)) {
        t->bad = 1;
        return;
    }
    t->store_at = at;
    t->store_blk = s->blk;
    t->value = ins[2];
}

/** @brief Scan an access chain: a read of a candidate table, which must follow its fill, or a disqualifying use.
 *
 * A chain needs at least one index; a first index that is not an
 * OpConstant marks the table run-time indexed. Its result is tracked so
 * scan_generic can confirm that only OpLoad uses it. */
static void scan_access_chain(spv_t* s, const uint32_t* ins) {
    const uint32_t wc = ins[0] >> 16;
    if (wc < 4) return;
    const uint32_t base = ins[3];
    if (base >= s->bound || !s->var_tab[base]) { scan_generic(s, ins, 3); return; }
    const int ti = s->var_tab[base] - 1;
    tab_t* t = &s->tabs[ti];
    const int filled = t->has_init ||
                       (t->store_at && (t->sc == SC_PRIVATE || s->blk == t->store_blk));
    if (wc < 5 || !filled) t->bad = 1;
    if (wc >= 5 && !def_of(s, ins[4], OP_CONSTANT)) t->dynamic = 1;
    scan_generic(s, ins, 4);
    if (ins[2] < s->bound) s->ac_tab[ins[2]] = ti + 1;
}

/** @brief Track the function, block and entry-block state the store and access-chain rules read. */
static void scan_track(spv_t* s, const uint32_t* ins, uint32_t op) {
    if (op == OP_FUNCTION) {
        s->cur_fn = ins[2];
        s->fn_labels = 0;
    } else if (op == OP_LABEL) {
        s->blk++;
        s->entry_first = s->cur_fn == s->entry_fn && s->fn_labels == 0;
        s->fn_labels++;
    } else if (op == OP_FUNCTION_CALL && s->entry_first) {
        s->entry_called = 1;
    }
}

/** @brief Scan every use of every candidate table, marking each table bad, dynamic, or filled by its one store.
 *
 * Debug names, decorations, entry-point interfaces and a candidate's own
 * declaration are not uses. */
static void scan_uses(spv_t* s) {
    for (size_t i = 5; i < s->n; i += s->w[i] >> 16) {
        const uint32_t* ins = &s->w[i];
        const uint32_t op = ins[0] & 0xFFFFu;
        scan_track(s, ins, op);
        switch (op) {
        case OP_NAME: case OP_DECORATE: case OP_MEMBER_DECORATE: case OP_ENTRY_POINT:
            break;
        case OP_VARIABLE:
            if ((ins[0] >> 16) >= 3 && ins[2] < s->bound && s->var_tab[ins[2]]) break;
            scan_generic(s, ins, 1);
            break;
        case OP_STORE:
            scan_store(s, ins, i);
            break;
        case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN:
            scan_access_chain(s, ins);
            break;
        default:
            scan_generic(s, ins, 1);
            break;
        }
    }
}

/** @brief Write one element's std430 words (zero padded to the stride), or check it is writable when dst is NULL.
 *
 * @return 1 when every component is an OpConstant or OpConstantNull. */
static int elem_words(const spv_t* s, const tab_t* t, uint32_t id, uint32_t* dst) {
    uint32_t v[4] = { 0, 0, 0, 0 };
    if (def_of(s, id, OP_CONSTANT_NULL)) {
        /* zeros */
    } else if (t->comps == 1) {
        const uint32_t* c = def_of(s, id, OP_CONSTANT);
        if (!c || (c[0] >> 16) != 4) return 0;
        v[0] = c[3];
    } else {
        const uint32_t* c = def_of(s, id, OP_CONSTANT_COMPOSITE);
        if (!c || (c[0] >> 16) != 3 + t->comps) return 0;
        for (uint32_t k = 0; k < t->comps; k++) {
            const uint32_t* e = def_of(s, c[3 + k], OP_CONSTANT);
            if (e && (e[0] >> 16) == 4) v[k] = e[3];
            else if (!def_of(s, c[3 + k], OP_CONSTANT_NULL)) return 0;
        }
    }
    if (dst) memcpy(dst, v, t->stride);
    return 1;
}

/** @brief Write a table's std430 bytes, or check that every element resolves when dst is NULL.
 *
 * An OpConstantNull value is all zeros. @return 1 on success. */
static int tab_fill(const spv_t* s, const tab_t* t, uint8_t* dst) {
    const uint32_t* c = def_of(s, t->value, OP_CONSTANT_COMPOSITE);
    if (!c) {
        if (dst) memset(dst, 0, (size_t)t->count * t->stride);
        return def_of(s, t->value, OP_CONSTANT_NULL) != NULL;
    }
    if ((c[0] >> 16) != 3 + t->count) return 0;
    for (uint32_t e = 0; e < t->count; e++) {
        uint32_t words[4];
        if (!elem_words(s, t, c[3 + e], dst ? words : NULL)) return 0;
        if (dst) memcpy(dst + (size_t)e * t->stride, words, t->stride);
    }
    return 1;
}

/* The moved tables' block: one member per distinct (value, array type). */
typedef struct {
    int      members;
    int      first_tab[64];   /* a table holding each member's data */
    uint32_t offset[64];
    size_t   bytes;
} block_t;

/** @brief Choose the tables that move and give each a block member, sharing one member per constant.
 *
 * @return The number of members (at most 64; tables past that stay). */
static int assign_members(spv_t* s, block_t* b) {
    b->members = 0;
    b->bytes = 0;
    for (int i = 0; i < s->ntabs; i++) {
        tab_t* t = &s->tabs[i];
        const size_t size = (size_t)t->count * t->stride;
        if (t->bad || !t->dynamic || !t->value || size < OPENPETE_MOD_SPV_CDATA_MIN_BYTES ||
            !tab_fill(s, t, NULL))
            continue;
        int m = 0;
        while (m < b->members && (s->tabs[b->first_tab[m]].value != t->value ||
                                  s->tabs[b->first_tab[m]].arr != t->arr))
            m++;
        if (m == b->members) {
            if (b->members == 64) continue;
            b->first_tab[m] = i;
            b->offset[m] = (uint32_t)((b->bytes + 15u) & ~(size_t)15u);
            b->bytes = b->offset[m] + size;
            b->members++;
        }
        t->member = m;
        s->drop[t->var] = 1;
    }
    return b->members;
}

/** @brief Mark each moved constant composite for removal unless an instruction that stays still references it.
 *
 * The moved variables' declarations and stores, and debug names and
 * decorations, are not references. */
static void mark_dead_values(spv_t* s, const block_t* b) {
    for (int m = 0; m < b->members; m++) s->drop[s->tabs[b->first_tab[m]].value] = 2;
    for (size_t i = 5; i < s->n; i += s->w[i] >> 16) {
        const uint32_t* ins = &s->w[i];
        const uint32_t op = ins[0] & 0xFFFFu, wc = ins[0] >> 16;
        if (op == OP_NAME || op == OP_DECORATE) continue;
        if (op == OP_VARIABLE && s->drop[ins[2]] == 1) continue;
        if (op == OP_STORE && ins[1] < s->bound && s->drop[ins[1]] == 1) continue;
        const uint32_t self = wc >= 3 ? def_result(ins, op) : 0;
        for (uint32_t k = 1; k < wc; k++)
            if (ins[k] < s->bound && ins[k] != self && s->drop[ins[k]] == 2)
                s->drop[ins[k]] = 3;   /* still referenced: keep */
    }
    for (int m = 0; m < b->members; m++) {
        uint8_t* d = &s->drop[s->tabs[b->first_tab[m]].value];
        *d = *d == 2 ? 1 : 0;
    }
}

/* New ids of the block and its types. */
typedef struct {
    uint32_t arr[64], kidx[64];
    uint32_t block, ptr_block, var, int_ty;
    uint32_t* ptr_types;      /* pointee types needing a new pointer, in order */
    int      nptr;
} newids_t;

/** @brief Return 1 when an instruction is an access chain into a moved table. */
static int is_moved_chain(const spv_t* s, const uint32_t* ins) {
    const uint32_t op = ins[0] & 0xFFFFu;
    return (op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) &&
           (ins[0] >> 16) >= 5 && ins[3] < s->bound && s->var_tab[ins[3]] &&
           s->tabs[s->var_tab[ins[3]] - 1].member >= 0;
}

/** @brief Allocate the new ids and collect each pointee type a moved access chain needs a block-storage pointer for.
 *
 * @return 0, or -1 when an access chain's result type is not a pointer or
 *         an allocation fails. */
static int alloc_ids(spv_t* s, const block_t* b, newids_t* id) {
    uint32_t next = s->bound;
    const tab_t* t0 = &s->tabs[b->first_tab[0]];
    id->int_ty = s->w[s->def[t0->len_id] + 1];
    for (int m = 0; m < b->members; m++) { id->arr[m] = next++; id->kidx[m] = next++; }
    id->block = next++;
    id->ptr_block = next++;
    id->var = next++;
    id->nptr = 0;
    id->ptr_types = NULL;
    for (size_t i = 5; i < s->n; i += s->w[i] >> 16) {
        const uint32_t* ins = &s->w[i];
        if (!is_moved_chain(s, ins)) continue;
        const uint32_t* p = def_of(s, ins[1], OP_TYPE_POINTER);
        if (!p) return -1;
        if (s->ptr_of[p[3]]) continue;
        uint32_t* nt = (uint32_t*)realloc(id->ptr_types, (size_t)(id->nptr + 1) * sizeof *nt);
        if (!nt) return -1;
        id->ptr_types = nt;
        id->ptr_types[id->nptr++] = p[3];
        s->ptr_of[p[3]] = next++;
    }
    s->out_bound = next;
    return 0;
}

/** @brief Emit the block's decorations: array strides, member offsets, NonWritable, the block kind, set and binding. */
static void emit_decorations(const spv_t* s, const block_t* b, const newids_t* id,
                             wbuf_t* o) {
    for (int m = 0; m < b->members; m++) {
        const tab_t* t = &s->tabs[b->first_tab[m]];
        wb_ins(o, OP_DECORATE, 3, id->arr[m], DEC_ARRAY_STRIDE, t->stride, 0);
        wb_ins(o, OP_MEMBER_DECORATE, 4, id->block, (uint32_t)m, DEC_OFFSET, b->offset[m]);
        wb_ins(o, OP_MEMBER_DECORATE, 3, id->block, (uint32_t)m, DEC_NON_WRITABLE, 0);
    }
    wb_ins(o, OP_DECORATE, 2, id->block,
           s->version >= 0x10300u ? DEC_BLOCK : DEC_BUFFER_BLOCK, 0, 0);
    wb_ins(o, OP_DECORATE, 3, id->var, DEC_DESCRIPTOR_SET, OPENPETE_MOD_SPV_CDATA_SET, 0);
    wb_ins(o, OP_DECORATE, 3, id->var, DEC_BINDING, OPENPETE_MOD_SPV_CDATA_BINDING, 0);
}

/** @brief Emit the block's types, member-index constants and variable, ahead of the first function. */
static void emit_globals(const spv_t* s, const block_t* b, const newids_t* id,
                         wbuf_t* o) {
    const uint32_t sc = s->version >= 0x10300u ? SC_STORAGE_BUFFER : SC_UNIFORM;
    for (int m = 0; m < b->members; m++) {
        const tab_t* t = &s->tabs[b->first_tab[m]];
        wb_ins(o, OP_TYPE_ARRAY, 3, id->arr[m], t->elem, t->len_id, 0);
    }
    const uint32_t st[2] = { OP_TYPE_STRUCT | ((uint32_t)(b->members + 2) << 16), id->block };
    wb_put(o, st, 2);
    wb_put(o, id->arr, (size_t)b->members);
    wb_ins(o, OP_TYPE_POINTER, 3, id->ptr_block, sc, id->block, 0);
    for (int k = 0; k < id->nptr; k++)
        wb_ins(o, OP_TYPE_POINTER, 3, s->ptr_of[id->ptr_types[k]], sc, id->ptr_types[k], 0);
    for (int m = 0; m < b->members; m++)
        wb_ins(o, OP_CONSTANT, 3, id->int_ty, id->kidx[m], (uint32_t)m, 0);
    wb_ins(o, OP_VARIABLE, 3, id->ptr_block, id->var, sc, 0);
}

/** @brief Emit an access chain into a moved table rebased onto the block: block-storage pointer type, the variable, the member index, then the original indices. */
static void emit_chain(const spv_t* s, const uint32_t* ins, const newids_t* id,
                       wbuf_t* o) {
    const uint32_t wc = ins[0] >> 16;
    const tab_t* t = &s->tabs[s->var_tab[ins[3]] - 1];
    const uint32_t pointee = s->w[s->def[ins[1]] + 3];
    const uint32_t head[5] = { (ins[0] & 0xFFFFu) | ((wc + 1) << 16), s->ptr_of[pointee],
                               ins[2], id->var, id->kidx[t->member] };
    wb_put(o, head, 5);
    wb_put(o, ins + 4, wc - 4);
}

/** @brief Emit an OpEntryPoint without the moved variables, adding the block variable from SPIR-V 1.4, where the interface lists every global. */
static void emit_entry_point(const spv_t* s, const uint32_t* ins, const newids_t* id,
                             wbuf_t* o) {
    const uint32_t wc = ins[0] >> 16;
    uint32_t k = 3;                         /* the name string ends at a word with a 0 top byte */
    while (k < wc && (ins[k] >> 24) != 0) k++;
    k++;
    const size_t at = o->n;
    wb_put(o, ins, k < wc ? k : wc);
    for (; k < wc; k++)
        if (ins[k] >= s->bound || !s->drop[ins[k]]) wb_put(o, &ins[k], 1);
    if (s->version >= 0x10400u) wb_put(o, &id->var, 1);
    if (!o->oom) o->w[at] = (o->w[at] & 0xFFFFu) | ((uint32_t)(o->n - at) << 16);
}

/** @brief Return 1 when an instruction is removed: a moved variable, its store, a dead constant, or a name or decoration of a removed id. */
static int is_dropped(const spv_t* s, const uint32_t* ins) {
    const uint32_t op = ins[0] & 0xFFFFu;
    if ((ins[0] >> 16) < 2) return 0;
    switch (op) {
    case OP_NAME: case OP_DECORATE: case OP_STORE:
        return ins[1] < s->bound && s->drop[ins[1]] == 1;
    case OP_VARIABLE: case OP_CONSTANT_COMPOSITE:
        return (ins[0] >> 16) >= 3 && ins[2] < s->bound && s->drop[ins[2]] == 1;
    default:
        return 0;
    }
}

/** @brief Write the rewritten module: the header with the new bound, the block's decorations at the end of the annotations, its globals before the first function, and every kept instruction with moved access chains rebased. */
static void emit_module(const spv_t* s, const block_t* b, const newids_t* id,
                        wbuf_t* o) {
    const uint32_t hdr[5] = { s->w[0], s->w[1], s->w[2], s->out_bound, s->w[4] };
    wb_put(o, hdr, 5);
    for (size_t i = 5; i < s->n; i += s->w[i] >> 16) {
        const uint32_t* ins = &s->w[i];
        if (i == s->types_at) emit_decorations(s, b, id, o);
        if (i == s->func_at) emit_globals(s, b, id, o);
        if (is_dropped(s, ins)) continue;
        if (is_moved_chain(s, ins)) emit_chain(s, ins, id, o);
        else if ((ins[0] & 0xFFFFu) == OP_ENTRY_POINT) emit_entry_point(s, ins, id, o);
        else wb_put(o, ins, ins[0] >> 16);
    }
}

/** @brief Allocate the per-id tables of a scan. @return 0, or -1 on an allocation failure. */
static int spv_alloc(spv_t* s) {
    s->def = (size_t*)calloc(s->bound, sizeof *s->def);
    s->var_tab = (int32_t*)calloc(s->bound, sizeof *s->var_tab);
    s->ac_tab = (int32_t*)calloc(s->bound, sizeof *s->ac_tab);
    s->ptr_of = (uint32_t*)calloc(s->bound, sizeof *s->ptr_of);
    s->drop = (uint8_t*)calloc(s->bound, 1);
    return (s->def && s->var_tab && s->ac_tab && s->ptr_of && s->drop) ? 0 : -1;
}

/** @brief Release the per-id tables and candidate list of a scan. */
static void spv_free(spv_t* s) {
    free(s->def); free(s->var_tab); free(s->ac_tab); free(s->ptr_of); free(s->drop);
    free(s->tabs);
}

/** @brief Build the block bytes and the rewritten module once the members are chosen.
 *
 * @return 0, or -1 on an allocation failure or an unexpected access-chain type. */
static int rewrite(spv_t* s, const block_t* b, uint32_t** out, size_t* out_n,
                   openpete_mod_spv_cdata_t* data) {
    newids_t id;
    memset(&id, 0, sizeof id);
    const size_t bytes = (b->bytes + 15u) & ~(size_t)15u;
    uint8_t* blob = (uint8_t*)calloc(1, bytes);
    wbuf_t o = { NULL, 0, 0, 0 };
    if (!blob || alloc_ids(s, b, &id) != 0) { free(blob); free(id.ptr_types); return -1; }
    for (int m = 0; m < b->members; m++)
        (void)tab_fill(s, &s->tabs[b->first_tab[m]], blob + b->offset[m]);
    emit_module(s, b, &id, &o);
    free(id.ptr_types);
    if (o.oom) { free(o.w); free(blob); return -1; }
    int moved = 0;
    for (int i = 0; i < s->ntabs; i++) moved += s->tabs[i].member >= 0;
    *out = o.w;
    *out_n = o.n;
    data->bytes = blob;
    data->len = bytes;
    data->tables = moved;
    return moved;
}

int openpete_mod_spv_cdata_extract(const uint32_t* in, size_t n,
                                   uint32_t** out, size_t* out_n,
                                   openpete_mod_spv_cdata_t* data) {
    memset(data, 0, sizeof *data);
    if (!in || n < 5 || in[0] != SPV_MAGIC || in[3] == 0 || in[3] > (1u << 24)) return -1;
    spv_t s;
    memset(&s, 0, sizeof s);
    s.w = in;
    s.n = n;
    s.bound = in[3];
    s.version = in[1] & 0x00FFFF00u;
    int rc = -1;
    if (spv_alloc(&s) != 0 || spv_index(&s) != 0 || find_candidates(&s) != 0) goto done;
    rc = 0;
    if (s.ntabs == 0) goto done;
    scan_uses(&s);
    block_t b;
    if (assign_members(&s, &b) == 0) goto done;
    mark_dead_values(&s, &b);
    rc = rewrite(&s, &b, out, out_n, data);
done:
    spv_free(&s);
    return rc;
}
