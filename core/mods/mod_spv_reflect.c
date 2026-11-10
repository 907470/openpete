/* mod_spv_reflect.c - read-only SPIR-V queries (see mod_spv_reflect.h). */

#include "mod_spv_reflect.h"

#include <string.h>

enum {
    OP_NAME = 5, OP_LINE = 8, OP_ENTRY_POINT = 15, OP_CAPABILITY = 17, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24, OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_PARAMETER = 55,
    OP_FUNCTION_END = 56, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59, OP_LOAD = 61,
    OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_LABEL = 248, OP_KILL = 252,
    OP_RETURN_VALUE = 254, OP_NO_LINE = 317, OP_TERMINATE_INVOCATION = 4416,
    OP_DEMOTE_TO_HELPER = 5380
};
enum { DEC_ARRAY_STRIDE = 6, DEC_MATRIX_STRIDE = 7, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34,
       DEC_OFFSET = 35 };

#define SPV_MAGIC 0x07230203u
#define SPV_DEPTH_MAX 8      /* nested struct and array levels a layout query follows */
#define SPV_CALLS_MAX 256    /* functions a call-graph walk visits */

/** @brief Return the word count of the instruction at a word index, or 0 when it is malformed.
 *
 * @param w The module words.
 * @param n The word count.
 * @param i The instruction's first word; at or past n reads as 0.
 * @return The instruction's word count, 0 for a zero count or one that runs past the module. */
static uint32_t ins_len(const uint32_t* w, size_t n, size_t i) {
    if (i >= n) return 0;
    const uint32_t wc = w[i] >> 16;
    return (wc == 0 || wc > n - i) ? 0 : wc;
}

/** @brief Return whether an opcode declares a type, constant or global variable, or starts a function: the sections that follow the debug and annotation instructions in a module's logical layout. */
static int past_annotations(uint32_t op) { return op >= 19u && op <= 60u; }

/** @brief Return whether a module starts with the SPIR-V magic and holds a header. */
static int spv_ok(const uint32_t* w, size_t n) { return w && n >= 5 && w[0] == SPV_MAGIC; }

int openpete_mod_spv_has_name(const uint32_t* w, size_t n, const char* name, size_t len) {
    if (!spv_ok(w, n)) return 0;
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && !past_annotations(w[i] & 0xFFFFu);
         i += wc)
        if ((w[i] & 0xFFFFu) == OP_NAME && wc >= 2u && (wc - 2u) * 4u >= len &&
            memcmp(&w[i + 2], name, len) == 0)
            return 1;
    return 0;
}

int openpete_mod_spv_has_capability(const uint32_t* w, size_t n, uint32_t cap) {
    if (!spv_ok(w, n)) return 0;
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0; i += wc) {
        if ((w[i] & 0xFFFFu) != OP_CAPABILITY) return 0;
        if (wc >= 2u && w[i + 1] == cap) return 1;
    }
    return 0;
}

/** @brief Find the defining instruction of a result id among type, constant and global instructions.
 *
 * @param w The module words.
 * @param n The word count.
 * @param id The result id.
 * @param op The opcode the definition must have.
 * @param res_word The result id's word position in that opcode (1 for types, 2 for constants and variables).
 * @return The instruction's word index, or 0 when not found. */
static size_t def_of(const uint32_t* w, size_t n, uint32_t id, uint32_t op, uint32_t res_word) {
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0; i += wc) {
        const uint32_t o = w[i] & 0xFFFFu;
        if (o == op && wc > res_word && w[i + res_word] == id) return i;
        if (o == OP_FUNCTION && op != OP_FUNCTION) return 0;   /* globals precede functions */
    }
    return 0;
}

/** @brief Return the opcode of the type instruction defining an id, or 0 when it is not a layout-relevant type. */
static uint32_t type_op(const uint32_t* w, size_t n, uint32_t id, size_t* at) {
    static const uint32_t k_ops[] = { OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX,
                                      OP_TYPE_ARRAY, OP_TYPE_STRUCT };
    for (size_t k = 0; k < sizeof k_ops / sizeof *k_ops; k++) {
        const size_t i = def_of(w, n, id, k_ops[k], 1);
        if (i) { *at = i; return k_ops[k]; }
    }
    return 0;
}

/** @brief Read a decoration literal of an id (OpDecorate) or of a struct member (OpMemberDecorate).
 *
 * @param w The module words.
 * @param n The word count.
 * @param id The decorated id.
 * @param member The member index, or -1 for OpDecorate.
 * @param dec The decoration.
 * @param out Receives the first literal.
 * @return 1 when found, 0 otherwise. */
static int decoration(const uint32_t* w, size_t n, uint32_t id, int member, uint32_t dec,
                      uint32_t* out) {
    const uint32_t op = member < 0 ? OP_DECORATE : OP_MEMBER_DECORATE;
    const uint32_t at = member < 0 ? 2u : 3u;   /* the decoration word */
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && !past_annotations(w[i] & 0xFFFFu);
         i += wc) {
        if ((w[i] & 0xFFFFu) != op || wc <= at + 1u || w[i + 1] != id) continue;
        if (member >= 0 && w[i + 2] != (uint32_t)member) continue;
        if (w[i + at] == dec) { *out = w[i + at + 1u]; return 1; }
    }
    return 0;
}

static int struct_size(const uint32_t* w, size_t n, size_t at, int depth, uint32_t* offs, int max,
                       uint32_t* size);

/** @brief Compute the std140 size of one struct member from its type and the member's decorations.
 *
 * A matrix occupies its column count times its MatrixStride; an array its
 * length times its ArrayStride; a nested struct its own computed size.
 * @return 0, or -1 when a size cannot be read. */
static int member_size(const uint32_t* w, size_t n, uint32_t st, int m, uint32_t ty, int depth,
                       uint32_t* out) {
    size_t at = 0;
    uint32_t v = 0;
    switch (type_op(w, n, ty, &at)) {
    case OP_TYPE_INT: case OP_TYPE_FLOAT: *out = w[at + 2] / 8u; return 0;
    case OP_TYPE_VECTOR: {
        uint32_t c = 0;
        if (member_size(w, n, st, m, w[at + 2], depth, &c) != 0) return -1;
        *out = c * w[at + 3];
        return 0;
    }
    case OP_TYPE_MATRIX:
        if (!decoration(w, n, st, m, DEC_MATRIX_STRIDE, &v)) return -1;
        *out = v * w[at + 3];
        return 0;
    case OP_TYPE_ARRAY: {
        const size_t len = def_of(w, n, w[at + 3], OP_CONSTANT, 2);
        if (!len || !decoration(w, n, ty, -1, DEC_ARRAY_STRIDE, &v)) return -1;
        *out = v * w[len + 3];
        return 0;
    }
    case OP_TYPE_STRUCT:
        return depth < SPV_DEPTH_MAX ? (struct_size(w, n, at, depth + 1, NULL, 0, out) < 0 ? -1 : 0)
                                     : -1;
    default: return -1;
    }
}

/** @brief Compute a struct type's member offsets and size: the largest offset plus member size.
 *
 * @param at The OpTypeStruct instruction's word index.
 * @return The member count, or -1 when an offset or a size cannot be read. */
static int struct_size(const uint32_t* w, size_t n, size_t at, int depth, uint32_t* offs, int max,
                       uint32_t* size) {
    const uint32_t st = w[at + 1];
    const int members = (int)ins_len(w, n, at) - 2;
    uint32_t end = 0;
    for (int m = 0; m < members; m++) {
        uint32_t off = 0, sz = 0;
        if (!decoration(w, n, st, m, DEC_OFFSET, &off) ||
            member_size(w, n, st, m, w[at + 2 + m], depth, &sz) != 0)
            return -1;
        if (offs && m < max) offs[m] = off;
        if (off + sz > end) end = off + sz;
    }
    if (size) *size = end;
    return members;
}

int openpete_mod_spv_block_layout(const uint32_t* w, size_t n, uint32_t set, uint32_t binding,
                                  uint32_t* offs, int max, uint32_t* size) {
    if (!spv_ok(w, n)) return -1;
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && (w[i] & 0xFFFFu) != OP_FUNCTION;
         i += wc) {
        uint32_t s = 0, b = 0;
        if ((w[i] & 0xFFFFu) != OP_VARIABLE || wc < 4) continue;
        if (!decoration(w, n, w[i + 2], -1, DEC_DESCRIPTOR_SET, &s) || s != set ||
            !decoration(w, n, w[i + 2], -1, DEC_BINDING, &b) || b != binding)
            continue;
        const size_t ptr = def_of(w, n, w[i + 1], OP_TYPE_POINTER, 1);
        const size_t st = ptr ? def_of(w, n, w[ptr + 3], OP_TYPE_STRUCT, 1) : 0;
        return st ? struct_size(w, n, st, 0, offs, max, size) : -1;
    }
    return -1;
}

/** @brief Find the result id an exact OpName names.
 *
 * @return The id, or 0 when no OpName carries the name. */
static uint32_t id_named(const uint32_t* w, size_t n, const char* name) {
    const size_t len = strlen(name) + 1;   /* the NUL makes the match exact */
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && !past_annotations(w[i] & 0xFFFFu);
         i += wc)
        if ((w[i] & 0xFFFFu) == OP_NAME && wc >= 3u && (wc - 2u) * 4u >= len &&
            memcmp(&w[i + 2], name, len) == 0)
            return w[i + 1];
    return 0;
}

int openpete_mod_spv_struct_layout(const uint32_t* w, size_t n, const char* name,
                                   uint32_t* offs, int max, uint32_t* size) {
    if (!spv_ok(w, n)) return -1;
    const uint32_t id = id_named(w, n, name);
    const size_t st = id ? def_of(w, n, id, OP_TYPE_STRUCT, 1) : 0;
    return st ? struct_size(w, n, st, 0, offs, max, size) : -1;
}

int openpete_mod_spv_member_array(const uint32_t* w, size_t n, const char* name, uint32_t member,
                                  uint32_t* offset, uint32_t* stride) {
    if (!spv_ok(w, n)) return -1;
    const uint32_t id = id_named(w, n, name);
    const size_t st = id ? def_of(w, n, id, OP_TYPE_STRUCT, 1) : 0;
    if (!st || member + 2u >= ins_len(w, n, st)) return -1;
    const uint32_t ty = w[st + 2 + member];
    if (!def_of(w, n, ty, OP_TYPE_ARRAY, 1)) return -1;
    return decoration(w, n, id, (int)member, DEC_OFFSET, offset) &&
           decoration(w, n, ty, -1, DEC_ARRAY_STRIDE, stride) ? 0 : -1;
}

/** @brief Find the result id of the function whose OpName starts with a prefix.
 *
 * @return The id, or 0 when none. */
static uint32_t fn_named(const uint32_t* w, size_t n, const char* prefix) {
    const size_t len = strlen(prefix);
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && !past_annotations(w[i] & 0xFFFFu);
         i += wc) {
        if ((w[i] & 0xFFFFu) != OP_NAME || wc < 3u || (wc - 2u) * 4u < len ||
            memcmp(&w[i + 2], prefix, len) != 0)
            continue;
        if (def_of(w, n, w[i + 1], OP_FUNCTION, 2)) return w[i + 1];
    }
    return 0;
}

/** @brief Return whether an id is the target of an OpEntryPoint. */
static int is_entry(const uint32_t* w, size_t n, uint32_t id) {
    for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0 && !past_annotations(w[i] & 0xFFFFu);
         i += wc)
        if ((w[i] & 0xFFFFu) == OP_ENTRY_POINT && wc >= 3u && w[i + 2] == id) return 1;
    return 0;
}

/** @brief Return whether an opcode ends the invocation. */
static int op_kills(uint32_t op) {
    return op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER;
}

/* A call-graph walk: the functions queued, in visit order. */
typedef struct { uint32_t id[SPV_CALLS_MAX]; int n; } fn_set_t;

/** @brief Queue a function id unless it is already queued; a full set drops it. */
static void fn_push(fn_set_t* s, uint32_t id) {
    for (int k = 0; k < s->n; k++)
        if (s->id[k] == id) return;
    if (s->n < SPV_CALLS_MAX) s->id[s->n++] = id;
}

/** @brief Scan one function body: queue each callee and report whether it ends the invocation.
 *
 * @param at The OpFunction instruction's word index.
 * @return 1 when the body holds an OpKill, OpTerminateInvocation or OpDemoteToHelperInvocation. */
static int fn_scan(const uint32_t* w, size_t n, size_t at, fn_set_t* calls) {
    size_t i = at;
    for (uint32_t wc; (wc = ins_len(w, n, i)) != 0; i += wc) {
        const uint32_t op = w[i] & 0xFFFFu;
        if (op == OP_FUNCTION_END) return 0;
        if (op_kills(op)) return 1;
        if (op == OP_FUNCTION_CALL && wc >= 4u) fn_push(calls, w[i + 3]);
    }
    return 0;
}

int openpete_mod_spv_fn_kills(const uint32_t* w, size_t n, const char* fn_prefix) {
    if (!spv_ok(w, n)) return -1;
    fn_set_t calls = { { 0 }, 0 };
    const uint32_t root = fn_named(w, n, fn_prefix);
    if (root) {
        fn_push(&calls, root);
    } else {
        for (size_t i = 5, wc; (wc = ins_len(w, n, i)) != 0; i += wc)
            if ((w[i] & 0xFFFFu) == OP_FUNCTION && wc >= 5u && !is_entry(w, n, w[i + 2]))
                fn_push(&calls, w[i + 2]);
    }
    for (int k = 0; k < calls.n; k++) {
        const size_t at = def_of(w, n, calls.id[k], OP_FUNCTION, 2);
        if (at && fn_scan(w, n, at, &calls)) return 1;
    }
    return 0;
}

/* The identity pattern's progress through one function body. */
typedef struct { uint32_t param, loaded; int params, labels, other, returns_param; } ident_t;

/** @brief Fold one instruction of a function body into the identity pattern.
 *
 * @param w The module words.
 * @param i The instruction's word index.
 * @param wc Its word count.
 * @param s The pattern state. */
static void ident_step(const uint32_t* w, size_t i, uint32_t wc, ident_t* s) {
    const uint32_t op = w[i] & 0xFFFFu;
    if (op == OP_LINE || op == OP_NO_LINE) return;
    if (op == OP_FUNCTION_PARAMETER && wc >= 3u) { s->param = w[i + 2]; s->params++; return; }
    if (op == OP_LABEL) { s->labels++; return; }
    if (op == OP_LOAD && wc >= 4u && w[i + 3] == s->param && !s->loaded) {
        s->loaded = w[i + 2];
        return;
    }
    if (op == OP_RETURN_VALUE && wc >= 2u &&
        ((s->loaded && w[i + 1] == s->loaded) || w[i + 1] == s->param)) {
        s->returns_param++;
        return;
    }
    s->other++;
}

int openpete_mod_spv_fn_identity(const uint32_t* w, size_t n, const char* fn_prefix) {
    if (!spv_ok(w, n)) return 0;
    const uint32_t id = fn_named(w, n, fn_prefix);
    const size_t at = id ? def_of(w, n, id, OP_FUNCTION, 2) : 0;
    if (!at) return 0;
    ident_t s = { 0, 0, 0, 0, 0, 0 };
    size_t i = at + ins_len(w, n, at);
    for (uint32_t wc; (wc = ins_len(w, n, i)) != 0 && (w[i] & 0xFFFFu) != OP_FUNCTION_END; i += wc)
        ident_step(w, i, wc, &s);
    return s.params == 1 && s.labels == 1 && s.other == 0 && s.returns_param == 1;
}
