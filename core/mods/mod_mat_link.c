/* mod_mat_link.c - material composition by source prefixing (see
 * mod_mat_link.h).
 *
 * The front end is a tokeniser over glslang's preprocessed text, not a
 * parser: a prefix applied to every declaration and every use of a
 * member-declared identifier alike needs no scope analysis. A token is
 * left unprefixed when it is a keyword or builtin, a `gl_` name, a name the
 * contract header declares at global scope (or a macro it defines), the
 * token after `.`, a qualifier name inside `layout(...)` or an attribute
 * name inside `[[...]]`, or a member name inside a struct body (each is reached only after `.`). Members of the
 * material block at set 3 binding 1 are global names and are rewritten to
 * the block's array element at the chain position. */

#include "mod_mat_link.h"
#include "mod_glsl.h"
#include "mod_spv_reflect.h"
#include "../host/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* growing string                                                      */

typedef struct { char* s; size_t len, cap; int oom; } sbuf_t;

/** @brief Append bytes to a growing string; an allocation failure marks it and drops the bytes. */
static void sb_put(sbuf_t* b, const char* s, size_t len) {
    if (b->oom) return;
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + len + 1 > cap) cap *= 2;
        char* p = (char*)realloc(b->s, cap);
        if (!p) { b->oom = 1; return; }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, len);
    b->len += len;
    b->s[b->len] = '\0';
}
/** @brief Append a NUL-terminated string to a growing string. */
static void sb_str(sbuf_t* b, const char* s) { sb_put(b, s, strlen(s)); }
/** @brief Append printf-formatted text of at most 255 bytes to a growing string. */
static void sb_fmt(sbuf_t* b, const char* fmt, int a, int c) {
    char tmp[256];
    const int k = snprintf(tmp, sizeof tmp, fmt, a, c);
    if (k > 0) sb_put(b, tmp, (size_t)k < sizeof tmp ? (size_t)k : sizeof tmp - 1);
}
/** @brief Hand a growing string's buffer to the caller, or free it and return NULL after an allocation failure. */
static char* sb_take(sbuf_t* b) {
    if (b->oom) { free(b->s); return NULL; }
    if (!b->s) return calloc(1, 1);
    return b->s;
}

/* ------------------------------------------------------------------ */
/* string set                                                          */

typedef struct { char** slot; uint32_t cap, n; } strset_t;

/** @brief Return the FNV-1a hash of a byte string. */
static uint32_t str_hash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

/** @brief Return the slot a string occupies in a set, or the empty slot it would take. */
static char** set_slot(const strset_t* set, const char* s, size_t len) {
    uint32_t i = str_hash(s, len) & (set->cap - 1u);
    while (set->slot[i] && !(strlen(set->slot[i]) == len && memcmp(set->slot[i], s, len) == 0))
        i = (i + 1u) & (set->cap - 1u);
    return &set->slot[i];
}

/** @brief Return whether a set holds a string. */
static int set_has(const strset_t* set, const char* s, size_t len) {
    return set->cap && *set_slot(set, s, len) != NULL;
}

/** @brief Double a set's table, keeping its strings.
 *
 * @return 0, or -1 on allocation failure. */
static int set_grow(strset_t* set) {
    strset_t big = { (char**)calloc(set->cap ? set->cap * 2u : 64u, sizeof(char*)),
                     set->cap ? set->cap * 2u : 64u, set->n };
    if (!big.slot) return -1;
    for (uint32_t i = 0; i < set->cap; i++)
        if (set->slot[i]) *set_slot(&big, set->slot[i], strlen(set->slot[i])) = set->slot[i];
    free(set->slot);
    *set = big;
    return 0;
}

/** @brief Add a string to a set; a string already present is kept once.
 *
 * @return 0, or -1 on allocation failure. */
static int set_add(strset_t* set, const char* s, size_t len) {
    if ((set->n + 1u) * 2u > set->cap && set_grow(set) != 0) return -1;
    char** at = set_slot(set, s, len);
    if (*at) return 0;
    char* copy = (char*)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, s, len);
    copy[len] = '\0';
    *at = copy;
    set->n++;
    return 0;
}

/** @brief Add every space-separated word of a string to a set. */
static void set_add_words(strset_t* set, const char* words) {
    for (const char* p = words; *p;) {
        while (*p == ' ' || *p == '\n') p++;
        const char* q = p;
        while (*q && *q != ' ' && *q != '\n') q++;
        if (q > p) set_add(set, p, (size_t)(q - p));
        p = q;
    }
}

/** @brief Free a set's strings and table. */
static void set_free(strset_t* set) {
    for (uint32_t i = 0; i < set->cap; i++) free(set->slot[i]);
    free(set->slot);
    memset(set, 0, sizeof *set);
}

/* ------------------------------------------------------------------ */
/* builtin names                                                       */

/* GLSL 4.60 keywords and reserved words. */
static const char k_keywords[] =
    "attribute const uniform varying buffer shared coherent volatile restrict readonly "
    "writeonly atomic_uint layout centroid flat smooth noperspective patch sample invariant "
    "precise break continue do for while switch case default if else subroutine in out "
    "inout int void bool true false float double discard return lowp mediump highp "
    "precision struct uint demote terminateInvocation nonuniformEXT common partition active "
    "asm class union enum typedef template this resource goto inline noinline public static "
    "extern external interface long short half fixed unsigned superp input output hvec2 "
    "hvec3 hvec4 fvec2 fvec3 fvec4 filter sizeof cast namespace using sampler samplerShadow "
    "subpassInput isubpassInput usubpassInput subpassInputMS isubpassInputMS usubpassInputMS "
    "float16_t float32_t float64_t int8_t uint8_t int16_t uint16_t int32_t uint32_t int64_t "
    "uint64_t";

/* GLSL 4.60 builtin functions and the subgroup and demote extension functions. */
static const char k_builtin_fns[] =
    "radians degrees sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh pow exp log "
    "exp2 log2 sqrt inversesqrt abs sign floor trunc round roundEven ceil fract mod modf min "
    "max clamp mix step smoothstep isnan isinf floatBitsToInt floatBitsToUint intBitsToFloat "
    "uintBitsToFloat fma frexp ldexp packUnorm2x16 packSnorm2x16 packUnorm4x8 packSnorm4x8 "
    "unpackUnorm2x16 unpackSnorm2x16 unpackUnorm4x8 unpackSnorm4x8 packHalf2x16 "
    "unpackHalf2x16 packDouble2x32 unpackDouble2x32 length distance dot cross normalize "
    "faceforward reflect refract ftransform matrixCompMult outerProduct transpose determinant "
    "inverse lessThan lessThanEqual greaterThan greaterThanEqual equal notEqual any all not "
    "uaddCarry usubBorrow umulExtended imulExtended bitfieldExtract bitfieldInsert "
    "bitfieldReverse bitCount findLSB findMSB textureSize textureQueryLod textureQueryLevels "
    "textureSamples texture textureProj textureLod textureOffset texelFetch texelFetchOffset "
    "textureProjOffset textureLodOffset textureProjLod textureProjLodOffset textureGrad "
    "textureGradOffset textureProjGrad textureProjGradOffset textureGather "
    "textureGatherOffset textureGatherOffsets texture1D texture1DProj texture1DLod "
    "texture1DProjLod texture2D texture2DProj texture2DLod texture2DProjLod texture3D "
    "texture3DProj texture3DLod texture3DProjLod textureCube textureCubeLod shadow1D shadow2D "
    "shadow1DProj shadow2DProj shadow1DLod shadow2DLod shadow1DProjLod shadow2DProjLod "
    "atomicCounterIncrement atomicCounterDecrement atomicCounter atomicCounterAdd "
    "atomicCounterSubtract atomicCounterMin atomicCounterMax atomicCounterAnd "
    "atomicCounterOr atomicCounterXor atomicCounterExchange atomicCounterCompSwap atomicAdd "
    "atomicMin atomicMax atomicAnd atomicOr atomicXor atomicExchange atomicCompSwap "
    "imageSize imageSamples imageLoad imageStore imageAtomicAdd imageAtomicMin "
    "imageAtomicMax imageAtomicAnd imageAtomicOr imageAtomicXor imageAtomicExchange "
    "imageAtomicCompSwap EmitStreamVertex EndStreamPrimitive EmitVertex EndPrimitive dFdx "
    "dFdy dFdxFine dFdyFine dFdxCoarse dFdyCoarse fwidth fwidthFine fwidthCoarse "
    "interpolateAtCentroid interpolateAtSample interpolateAtOffset noise1 noise2 noise3 "
    "noise4 barrier memoryBarrier memoryBarrierAtomicCounter memoryBarrierBuffer "
    "memoryBarrierShared memoryBarrierImage groupMemoryBarrier subpassLoad anyInvocation "
    "allInvocations allInvocationsEqual helperInvocationEXT subgroupBarrier "
    "subgroupMemoryBarrier subgroupMemoryBarrierBuffer subgroupMemoryBarrierShared "
    "subgroupMemoryBarrierImage subgroupElect subgroupAll subgroupAny subgroupAllEqual "
    "subgroupBroadcast subgroupBroadcastFirst subgroupBallot subgroupInverseBallot "
    "subgroupBallotBitExtract subgroupBallotBitCount subgroupBallotInclusiveBitCount "
    "subgroupBallotExclusiveBitCount subgroupBallotFindLSB subgroupBallotFindMSB "
    "subgroupShuffle subgroupShuffleXor subgroupShuffleUp subgroupShuffleDown subgroupAdd "
    "subgroupMul subgroupMin subgroupMax subgroupAnd subgroupOr subgroupXor "
    "subgroupInclusiveAdd subgroupInclusiveMul subgroupInclusiveMin subgroupInclusiveMax "
    "subgroupInclusiveAnd subgroupInclusiveOr subgroupInclusiveXor subgroupExclusiveAdd "
    "subgroupExclusiveMul subgroupExclusiveMin subgroupExclusiveMax subgroupExclusiveAnd "
    "subgroupExclusiveOr subgroupExclusiveXor subgroupClusteredAdd subgroupClusteredMul "
    "subgroupClusteredMin subgroupClusteredMax subgroupClusteredAnd subgroupClusteredOr "
    "subgroupClusteredXor subgroupQuadBroadcast subgroupQuadSwapHorizontal "
    "subgroupQuadSwapVertical subgroupQuadSwapDiagonal";

/** @brief Add the vector, matrix, sampler, image and texture type names to a set.
 *
 * Vectors: [bdiu]?vec2-4 and the explicit-width f16/f32/f64/i8-i64/u8-u64
 * vectors; matrices mat2-4, matCxR, the d and f16-f64 forms; opaque types
 * the i/u forms of sampler, image and texture per dimensionality, the
 * sampler shadow forms. */
static void add_type_names(strset_t* set) {
    static const char* const vec_pre[] = { "", "b", "d", "i", "u", "f16", "f32", "f64", "i8",
                                           "u8", "i16", "u16", "i32", "u32", "i64", "u64" };
    static const char* const mat_pre[] = { "", "d", "f16", "f32", "f64" };
    static const char* const opq[] = { "sampler", "image", "texture" };
    static const char* const dims[] = { "1D", "2D", "3D", "Cube", "2DRect", "1DArray",
                                        "2DArray", "CubeArray", "Buffer", "2DMS", "2DMSArray",
                                        "1DShadow", "2DShadow", "CubeShadow", "2DRectShadow",
                                        "1DArrayShadow", "2DArrayShadow", "CubeArrayShadow" };
    char nm[64];
    for (size_t p = 0; p < sizeof vec_pre / sizeof *vec_pre; p++)
        for (int c = 2; c <= 4; c++) {
            snprintf(nm, sizeof nm, "%svec%d", vec_pre[p], c);
            set_add(set, nm, strlen(nm));
        }
    for (size_t p = 0; p < sizeof mat_pre / sizeof *mat_pre; p++)
        for (int c = 2; c <= 4; c++) {
            snprintf(nm, sizeof nm, "%smat%d", mat_pre[p], c);
            set_add(set, nm, strlen(nm));
            for (int r = 2; r <= 4; r++) {
                snprintf(nm, sizeof nm, "%smat%dx%d", mat_pre[p], c, r);
                set_add(set, nm, strlen(nm));
            }
        }
    static const char* const opq_pre[] = { "", "i", "u" };
    for (size_t o = 0; o < 3; o++)
        for (size_t d = 0; d < sizeof dims / sizeof *dims; d++)
            for (size_t q = 0; q < 3; q++) {
                snprintf(nm, sizeof nm, "%s%s%s", opq_pre[q], opq[o], dims[d]);
                set_add(set, nm, strlen(nm));
            }
}

/* ------------------------------------------------------------------ */
/* tokens                                                              */

enum { TK_WS, TK_ID, TK_NUM, TK_PUNCT, TK_DIR };

typedef struct { const char* s; uint32_t len; int kind; } tok_t;
typedef struct { tok_t* v; int n, cap; } toks_t;

/** @brief Return whether a character can start an identifier. */
static int id_start(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
/** @brief Return whether a character can continue an identifier. */
static int id_char(char c) { return id_start(c) || (c >= '0' && c <= '9'); }

/** @brief Return the length of the number token at a position.
 *
 * Digits, letters, `_` and `.`, and a sign right after an exponent letter
 * of a decimal number. */
static uint32_t num_len(const char* p) {
    const int hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    uint32_t k = 0;
    for (;;) {
        const char c = p[k];
        if (id_char(c) || c == '.') { k++; continue; }
        if ((c == '+' || c == '-') && k && !hex && (p[k - 1] == 'e' || p[k - 1] == 'E')) { k++; continue; }
        return k;
    }
}

/** @brief Return the length of the punctuator at a position: a three-, two- or one-character operator. */
static uint32_t punct_len(const char* p) {
    static const char* const k_ops3[] = { "<<=", ">>=" };
    static const char* const k_ops2[] = { "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
                                          "||", "^^", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=" };
    for (size_t i = 0; i < 2; i++)
        if (strncmp(p, k_ops3[i], 3) == 0) return 3;
    for (size_t i = 0; i < sizeof k_ops2 / sizeof *k_ops2; i++)
        if (strncmp(p, k_ops2[i], 2) == 0) return 2;
    return 1;
}

/** @brief Return the kind and length of the token at a position.
 *
 * A `#` that starts a line (after blanks) begins a directive running to
 * the end of the line.
 * @param p The position.
 * @param line_start Whether only blanks precede it on its line.
 * @param len Receives the length. */
static int tok_at(const char* p, int line_start, uint32_t* len) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        uint32_t k = 0;
        while (p[k] == ' ' || p[k] == '\t' || p[k] == '\n' || p[k] == '\r') k++;
        *len = k;
        return TK_WS;
    }
    if (*p == '#' && line_start) {
        uint32_t k = 0;
        while (p[k] && p[k] != '\n') k++;
        *len = k;
        return TK_DIR;
    }
    if (id_start(*p)) {
        uint32_t k = 0;
        while (id_char(p[k])) k++;
        *len = k;
        return TK_ID;
    }
    if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        *len = num_len(p);
        return TK_NUM;
    }
    *len = punct_len(p);
    return TK_PUNCT;
}

/** @brief Split a text into tokens; whitespace and directives are tokens, so the tokens concatenate to the text.
 *
 * @return 0, or -1 on allocation failure. */
static int tokenise(const char* text, toks_t* out) {
    memset(out, 0, sizeof *out);
    int line_start = 1;
    for (const char* p = text; *p;) {
        if (out->n == out->cap) {
            const int cap = out->cap ? out->cap * 2 : 1024;
            tok_t* v = (tok_t*)realloc(out->v, (size_t)cap * sizeof *v);
            if (!v) return -1;
            out->v = v;
            out->cap = cap;
        }
        uint32_t len = 0;
        const int kind = tok_at(p, line_start, &len);
        out->v[out->n++] = (tok_t){ p, len, kind };
        if (kind == TK_WS) line_start = line_start || memchr(p, '\n', len) != NULL;
        else line_start = 0;
        p += len;
    }
    return 0;
}

/** @brief Return whether a token is a given punctuator or identifier. */
static int tok_is(const tok_t* t, const char* s) {
    return t->len == strlen(s) && memcmp(t->s, s, t->len) == 0;
}

/** @brief Return the index of the next significant token (not whitespace or directive) at or after i, or n. */
static int next_sig(const toks_t* tk, int i) {
    while (i < tk->n && (tk->v[i].kind == TK_WS || tk->v[i].kind == TK_DIR)) i++;
    return i;
}

/** @brief Return the index of the bracket closing the one at i, or n when it is unbalanced.
 *
 * @param open The opening punctuator, "(" or "{".
 * @param close Its closing punctuator. */
static int match_close(const toks_t* tk, int i, const char* open, const char* close) {
    int depth = 0;
    for (; i < tk->n; i++) {
        if (tk->v[i].kind != TK_PUNCT) continue;
        if (tok_is(&tk->v[i], open)) depth++;
        else if (tok_is(&tk->v[i], close) && --depth == 0) return i;
    }
    return tk->n;
}

/* ------------------------------------------------------------------ */
/* contract header names                                               */

struct openpete_mod_link_ctx {
    char     sdk_shader_dir[OPENPETE_MOD_GLSL_PATH_MAX];
    openpete_mod_glsl_include_t inc;   /* resolves in <sdk>/shaders */
    strset_t builtin;
    strset_t header[2][OPENPETE_MOD_LINK_PROFILES];
    int      header_state[2][OPENPETE_MOD_LINK_PROFILES];   /* 0 unread, 1 ready, -1 failed */
};

static const char* const k_contract_header[2] = { "openpete_psx_material.glsl",
                                                  "openpete_psx_decal.glsl" };
static const char* const k_contract_guard[2] = { "#define OPENPETE_PSX_MATERIAL_GLSL 1\n",
                                                 "#define OPENPETE_PSX_DECAL_GLSL 1\n" };

const char* openpete_mod_link_profile_preamble(int contract, int profile) {
    if (contract != OPENPETE_MOD_LINK_CONTRACT_SCENE) return NULL;
    if (profile == OPENPETE_MOD_LINK_TERRAIN) return OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE;
    if (profile == OPENPETE_MOD_LINK_PORTAL) return OPENPETE_MOD_GLSL_PORTAL_PREAMBLE;
    return NULL;
}

openpete_mod_link_ctx_t* openpete_mod_link_ctx_new(const char* sdk_dir) {
    openpete_mod_link_ctx_t* c = (openpete_mod_link_ctx_t*)calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->sdk_shader_dir, sizeof c->sdk_shader_dir, "%s/shaders", sdk_dir);
    char probe[OPENPETE_MOD_GLSL_PATH_MAX + 16];
    snprintf(probe, sizeof probe, "%s/link.frag", c->sdk_shader_dir);
    openpete_mod_glsl_include_init(&c->inc, probe, sdk_dir);
    set_add_words(&c->builtin, k_keywords);
    set_add_words(&c->builtin, k_builtin_fns);
    add_type_names(&c->builtin);
    return c;
}

void openpete_mod_link_ctx_free(openpete_mod_link_ctx_t* ctx) {
    if (!ctx) return;
    set_free(&ctx->builtin);
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < OPENPETE_MOD_LINK_PROFILES; p++) set_free(&ctx->header[c][p]);
    free(ctx);
}

/** @brief Add the identifiers declared in an anonymous interface block body to a set.
 *
 * @param open The body's `{` token.
 * @param close Its `}` token. */
static void add_block_members(const toks_t* tk, int open, int close, strset_t* set) {
    const int after = next_sig(tk, close + 1);
    if (after >= tk->n || !tok_is(&tk->v[after], ";")) return;   /* a named instance */
    for (int i = open + 1; i < close; i++)
        if (tk->v[i].kind == TK_ID) set_add(set, tk->v[i].s, tk->v[i].len);
}

/** @brief Return whether a token is a storage qualifier that declares an interface variable or block at global scope. */
static int is_storage(const tok_t* t) {
    static const char* const k_storage[] = { "in", "out", "inout", "uniform", "buffer",
                                             "shared", "attribute", "varying" };
    for (size_t i = 0; i < sizeof k_storage / sizeof *k_storage; i++)
        if (tok_is(t, k_storage[i])) return 1;
    return 0;
}

/** @brief Add every identifier a text declares at global scope to a set.
 *
 * Global scope is brace and parenthesis depth 0; an anonymous interface
 * block's members count as global. Initializer and parameter identifiers
 * inside parentheses do not count; names at depth 0 that are not
 * declarations (types, operands of a global initializer) are included,
 * which only widens the set of names left unprefixed by builtins and
 * other globals. */
static void collect_globals(const toks_t* tk, strset_t* set) {
    int brace = 0, paren = 0;
    for (int i = 0; i < tk->n; i++) {
        const tok_t* t = &tk->v[i];
        if (t->kind == TK_ID && brace == 0 && paren == 0) {
            set_add(set, t->s, t->len);
            const int nm = next_sig(tk, i + 1), op = nm < tk->n ? next_sig(tk, nm + 1) : tk->n;
            if (is_storage(t) && op < tk->n && tok_is(&tk->v[op], "{")) {
                const int close = match_close(tk, op, "{", "}");
                add_block_members(tk, op, close, set);
            }
        }
        if (t->kind != TK_PUNCT) continue;
        if (tok_is(t, "{")) brace++;
        else if (tok_is(t, "}")) brace--;
        else if (tok_is(t, "(")) paren++;
        else if (tok_is(t, ")")) paren--;
    }
}

/** @brief Add the macro names a header file and the files it includes define to a set.
 *
 * Reads the raw files: every `#define NAME` line, and every
 * `#include "name"` resolved in the SDK shader directory, to a depth of 8.
 * Lines of an inactive conditional branch count too. */
static void collect_macros(const char* dir, const char* name, int depth, strset_t* set) {
    char path[OPENPETE_MOD_GLSL_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char* raw = depth < 8 ? openpete_mod_glsl_read_file(path, NULL) : NULL;
    if (!raw) return;
    for (char* line = raw; line && *line;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        char inc[128];
        if (strncmp(p, "#define", 7) == 0) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            size_t k = 0;
            while (id_char(p[k])) k++;
            if (k) set_add(set, p, k);
        } else if (sscanf(p, "#include \"%127[^\"]\"", inc) == 1) {
            collect_macros(dir, inc, depth + 1, set);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(raw);
}

/** @brief Return the names a contract header declares in a profile, reading them on first use.
 *
 * The header is preprocessed under the profile preamble alone; its global
 * names (collect_globals) and the macro names of its include closure
 * (collect_macros) form the set.
 * @return The set, or NULL when the header cannot be preprocessed. */
static const strset_t* header_names(openpete_mod_link_ctx_t* ctx, int contract, int profile) {
    int* state = &ctx->header_state[contract][profile];
    strset_t* set = &ctx->header[contract][profile];
    if (*state) return *state > 0 ? set : NULL;
    char src[256];
    snprintf(src, sizeof src,
             "#version 450\n#extension GL_GOOGLE_include_directive : require\n#include \"%s\"\n",
             k_contract_header[contract]);
    char* pre = openpete_mod_glsl_preprocess(src, OPENPETE_MOD_GLSL_STAGE_FRAG, &ctx->inc,
                                             openpete_mod_link_profile_preamble(contract, profile),
                                             "/dev/null", NULL);
    toks_t tk = { NULL, 0, 0 };
    *state = -1;
    if (pre && tokenise(pre, &tk) == 0) {
        collect_globals(&tk, set);
        collect_macros(ctx->sdk_shader_dir, k_contract_header[contract], 0, set);
        *state = 1;
    }
    free(tk.v);
    free(pre);
    return *state > 0 ? set : NULL;
}

/* ------------------------------------------------------------------ */
/* the member's text                                                   */

/** @brief Return whether a line starts, after blanks, with a directive word. */
static int line_is(const char* line, size_t len, const char* word) {
    while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
    const size_t k = strlen(word);
    return len >= k && memcmp(line, word, k) == 0;
}

/** @brief Return whether a line holds only blanks. */
static int line_blank(const char* line, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return 0;
    return 1;
}

/** @brief Return whether a byte range contains a NUL-terminated needle. */
static int has_bytes(const char* hay, size_t len, const char* needle) {
    const size_t k = strlen(needle);
    for (size_t i = 0; i + k <= len; i++)
        if (memcmp(hay + i, needle, k) == 0) return 1;
    return 0;
}

/** @brief Split preprocessed text into the member text and its `#extension` lines.
 *
 * `#version`, `#line` and blank lines are dropped, the include directive's
 * `#extension` line too (every module carries it); every other line keeps
 * its place. */
static void split_member(const char* pre, sbuf_t* body, sbuf_t* exts) {
    for (const char* line = pre; *line;) {
        const char* nl = strchr(line, '\n');
        const size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (line_is(line, len, "#extension")) {
            if (!has_bytes(line, len, "GL_GOOGLE_include_directive")) {
                sb_put(exts, line, len);
                sb_str(exts, "\n");
            }
        } else if (!line_is(line, len, "#version") && !line_is(line, len, "#line") &&
                   !line_blank(line, len)) {
            sb_put(body, line, len);
            sb_str(body, "\n");
        }
        line += len + (nl ? 1 : 0);
    }
}

/* ------------------------------------------------------------------ */
/* global scan                                                         */

#define SCAN_FIELDS_MAX OPENPETE_MOD_LINK_FIELDS_MAX

/* What a member declares at global scope. */
typedef struct {
    int has_material;      /* op_material is defined                          */
    int has_main;          /* main is declared                                */
    int blk_start, blk_end;   /* the material block's tokens, `;` included; -1 none */
    int blk_open, blk_close;  /* its body braces                              */
    int inst;              /* its instance-name token, -1 anonymous           */
    strset_t fields;       /* its members' names                              */
    char iface[200];       /* the first foreign interface, "" none            */
} scan_t;

/** @brief Read `set = N` and `binding = N` from the layout qualifiers between two tokens.
 *
 * @param set Receives the set, -1 when absent or not a literal.
 * @param binding Receives the binding, likewise. */
static void layout_set_binding(const toks_t* tk, int from, int to, long* set, long* binding) {
    *set = *binding = -1;
    for (int i = from; i < to; i++) {
        const tok_t* t = &tk->v[i];
        if (t->kind != TK_ID || !(tok_is(t, "set") || tok_is(t, "binding"))) continue;
        const int eq = next_sig(tk, i + 1), val = eq < to ? next_sig(tk, eq + 1) : to;
        if (val >= to || !tok_is(&tk->v[eq], "=") || tk->v[val].kind != TK_NUM) continue;
        const long v = strtol(tk->v[val].s, NULL, 0);
        if (tok_is(t, "set")) *set = v; else *binding = v;
    }
}

/** @brief Return the index of the `;` ending a global declaration that starts at or after i: the first at brace depth 0. */
static int decl_end(const toks_t* tk, int i) {
    int brace = 0;
    for (; i < tk->n; i++) {
        if (tk->v[i].kind != TK_PUNCT) continue;
        if (tok_is(&tk->v[i], "{")) brace++;
        else if (tok_is(&tk->v[i], "}")) brace--;
        else if (brace == 0 && tok_is(&tk->v[i], ";")) return i;
    }
    return tk->n - 1;
}

/** @brief Return the first token's text of a declaration, at most 120 bytes, for a reason line. */
static void decl_text(const toks_t* tk, int from, int to, char* out, size_t cap) {
    size_t k = 0;
    for (int i = from; i <= to && i < tk->n && k + 1 < cap && k < 120; i++) {
        const tok_t* t = &tk->v[i];
        const size_t take = t->kind == TK_WS ? 1 : t->len;
        if (k + take + 1 >= cap) break;
        if (t->kind == TK_WS) out[k++] = ' ';
        else { memcpy(out + k, t->s, take); k += take; }
    }
    out[k] = '\0';
}

/** @brief Record the names of the material block's members.
 *
 * Per member declaration: qualifier keywords and `layout(...)` groups are
 * skipped, the first other identifier is the type, and the first
 * identifier of each comma-separated declarator is a member name. */
static void block_fields(const toks_t* tk, scan_t* sc) {
    int want = 0;   /* 0 type, 1 declarator name, 2 inside a declarator */
    int depth = 0;
    for (int i = sc->blk_open + 1; i < sc->blk_close; i++) {
        const tok_t* t = &tk->v[i];
        if (t->kind == TK_PUNCT) {
            if (tok_is(t, "(") || tok_is(t, "[")) depth++;
            else if (tok_is(t, ")") || tok_is(t, "]")) depth--;
            else if (depth == 0 && tok_is(t, ";")) want = 0;
            else if (depth == 0 && tok_is(t, ",")) want = 1;
            continue;
        }
        if (t->kind != TK_ID || depth) continue;
        if (want == 0) {
            if (!(tok_is(t, "layout") || tok_is(t, "highp") || tok_is(t, "mediump") ||
                  tok_is(t, "lowp") || tok_is(t, "precise") || tok_is(t, "invariant")))
                want = 1;
        } else if (want == 1) {
            set_add(&sc->fields, t->s, t->len);
            want = 2;
        }
    }
}

/** @brief Classify a global declaration that carries a storage qualifier: the material block, or a foreign interface.
 *
 * @param start The declaration's first token.
 * @param q The storage qualifier token.
 * @return The index of the declaration's ending `;`. */
static int scan_interface(const toks_t* tk, int start, int q, scan_t* sc) {
    const int end = decl_end(tk, q);
    const int nm = next_sig(tk, q + 1), open = nm < tk->n ? next_sig(tk, nm + 1) : tk->n;
    long set = -1, binding = -1;
    layout_set_binding(tk, start, q, &set, &binding);
    const int block = tok_is(&tk->v[q], "uniform") && open < tk->n && tok_is(&tk->v[open], "{");
    if (block && set == 3 && binding == 1 && sc->blk_start < 0 && !sc->iface[0]) {
        sc->blk_start = start;
        sc->blk_end = end;
        sc->blk_open = open;
        sc->blk_close = match_close(tk, open, "{", "}");
        const int inst = next_sig(tk, sc->blk_close + 1);
        sc->inst = inst < end && tk->v[inst].kind == TK_ID ? inst : -1;
        block_fields(tk, sc);
        return end;
    }
    if (!sc->iface[0]) {
        char what[160];
        decl_text(tk, next_sig(tk, start), open < end ? open : end, what, sizeof what);
        snprintf(sc->iface, sizeof sc->iface, "%s`%s`",
                 block && set == 3 && binding == 1 ? "a second block at set 3 binding 1: " : "",
                 what);
    }
    return end;
}

/** @brief Note a function name at global scope: an op_material definition or a main declaration. */
static void scan_function(const toks_t* tk, int i, scan_t* sc) {
    const int open = next_sig(tk, i + 1);
    if (open >= tk->n || !tok_is(&tk->v[open], "(")) return;
    if (tok_is(&tk->v[i], "main")) { sc->has_main = 1; return; }
    const int body = next_sig(tk, match_close(tk, open, "(", ")") + 1);
    if (body < tk->n && tok_is(&tk->v[body], "{")) sc->has_material = 1;
}

/** @brief Scan a member's tokens at global scope: op_material, main, the material block and foreign interfaces. */
static void scan_member(const toks_t* tk, scan_t* sc) {
    memset(sc, 0, sizeof *sc);
    sc->blk_start = sc->blk_end = sc->blk_open = sc->blk_close = sc->inst = -1;
    int brace = 0, paren = 0, start = 0;
    for (int i = 0; i < tk->n; i++) {
        const tok_t* t = &tk->v[i];
        if (t->kind == TK_ID && brace == 0 && paren == 0) {
            if (is_storage(t)) { i = scan_interface(tk, start, i, sc); start = i + 1; continue; }
            if (tok_is(t, "op_material") || tok_is(t, "main")) scan_function(tk, i, sc);
        }
        if (t->kind != TK_PUNCT) continue;
        if (tok_is(t, "{")) brace++;
        else if (tok_is(t, "(")) paren++;
        else if (tok_is(t, ")")) paren--;
        else if (tok_is(t, "}") && --brace == 0) start = i + 1;
        else if (tok_is(t, ";") && brace == 0) start = i + 1;
    }
}

/* ------------------------------------------------------------------ */
/* prefixing                                                           */

#define WALK_STRUCTS_MAX 16

/* The prefix walk over one member's tokens. */
typedef struct {
    const toks_t*   tk;
    const scan_t*   sc;
    const strset_t* builtin;
    const strset_t* header;
    sbuf_t*         out;
    int  k;                 /* the case index: the m<k>_ prefix and the block names */
    int  brace, paren, bracket;
    int  layout_paren;      /* paren depth of an open layout( ... ), 0 none   */
    int  attr_bracket;      /* bracket depth of an open [[ ... ]] attribute, 0 none */
    int  structs[WALK_STRUCTS_MAX], nstructs;   /* brace depths of open struct bodies */
    int  struct_pending;    /* `struct` seen, its `{` not yet                 */
    int  decl_start;        /* in a struct body: the next identifier starts a member declaration */
    const tok_t* prev;      /* the previous significant token                 */
} walk_t;

/** @brief Return whether the walk is directly inside a struct body (not inside its array-size brackets). */
static int in_struct(const walk_t* w) {
    return w->nstructs && w->structs[w->nstructs - 1] == w->brace && w->bracket == 0;
}

/** @brief Emit an identifier under the general rule: kept when a keyword, builtin, `gl_` name or contract header name, prefixed otherwise; op_material is always prefixed. */
static void emit_general(walk_t* w, const tok_t* t) {
    const int keep = !tok_is(t, "op_material") &&
                     (set_has(w->builtin, t->s, t->len) || set_has(w->header, t->s, t->len) ||
                      (t->len > 3 && memcmp(t->s, "gl_", 3) == 0));
    if (!keep) sb_fmt(w->out, "m%d_", w->k, 0);
    sb_put(w->out, t->s, t->len);
}

/** @brief Emit an identifier inside a struct body: a member's type takes the general rule, a member name is kept. */
static void emit_struct_ident(walk_t* w, const tok_t* t) {
    if (!w->decl_start) { sb_put(w->out, t->s, t->len); return; }
    if (tok_is(t, "highp") || tok_is(t, "mediump") || tok_is(t, "lowp") || tok_is(t, "precise")) {
        sb_put(w->out, t->s, t->len);
        return;
    }
    w->decl_start = 0;
    emit_general(w, t);
}

/** @brief Emit one identifier token of a member. */
static void emit_ident(walk_t* w, const tok_t* t) {
    const scan_t* sc = w->sc;
    if (w->prev && tok_is(w->prev, ".")) { sb_put(w->out, t->s, t->len); return; }
    const int qualifier_pos = w->prev && (tok_is(w->prev, "(") || tok_is(w->prev, ",") ||
                                          tok_is(w->prev, "["));
    if (qualifier_pos && ((w->layout_paren && w->paren == w->layout_paren) ||
                          (w->attr_bracket && w->bracket == w->attr_bracket))) {
        sb_put(w->out, t->s, t->len);   /* a layout qualifier or attribute name */
        return;
    }
    if (tok_is(t, "struct")) { w->struct_pending = 1; sb_put(w->out, t->s, t->len); return; }
    if (in_struct(w)) { emit_struct_ident(w, t); return; }
    if (sc->blk_start >= 0 && sc->inst < 0 && set_has(&sc->fields, t->s, t->len)) {
        sb_fmt(w->out, "op_chain_at%d[op_chain_pos].", w->k, 0);
        sb_put(w->out, t->s, t->len);
        return;
    }
    if (sc->inst >= 0 && t->len == w->tk->v[sc->inst].len &&
        memcmp(t->s, w->tk->v[sc->inst].s, t->len) == 0) {
        sb_fmt(w->out, "op_chain_at%d[op_chain_pos]", w->k, 0);
        return;
    }
    emit_general(w, t);
}

/** @brief Emit one punctuator and track brace, parenthesis, bracket, layout and struct-body depth. */
static void emit_punct(walk_t* w, const tok_t* t) {
    if (tok_is(t, "{")) {
        w->brace++;
        if (w->struct_pending && w->nstructs < WALK_STRUCTS_MAX) {
            w->structs[w->nstructs++] = w->brace;
            w->decl_start = 1;
        }
        w->struct_pending = 0;
    } else if (tok_is(t, "}")) {
        if (w->nstructs && w->structs[w->nstructs - 1] == w->brace) w->nstructs--;
        w->brace--;
    } else if (tok_is(t, "(")) {
        w->paren++;
        if (w->prev && tok_is(w->prev, "layout")) w->layout_paren = w->paren;
    } else if (tok_is(t, ")")) {
        if (w->paren == w->layout_paren) w->layout_paren = 0;
        w->paren--;
    } else if (tok_is(t, "[")) {
        w->bracket++;
        if (w->prev && tok_is(w->prev, "[") && !w->attr_bracket) w->attr_bracket = w->bracket;
    } else if (tok_is(t, "]")) {
        if (w->bracket == w->attr_bracket) w->attr_bracket = 0;
        w->bracket--;
    } else if (tok_is(t, ";") && in_struct(w)) {
        w->decl_start = 1;
    }
    sb_put(w->out, t->s, t->len);
}

/** @brief Emit one token of a member through the prefix rules. */
static void emit_tok(walk_t* w, const tok_t* t) {
    if (t->kind == TK_ID) emit_ident(w, t);
    else if (t->kind == TK_PUNCT) emit_punct(w, t);
    else sb_put(w->out, t->s, t->len);
    if (t->kind != TK_WS && t->kind != TK_DIR) w->prev = t;
}

/** @brief Return the name of the padding member: `op_chain_pad`, lengthened with `_` until no block member has it. */
static void pad_name(const scan_t* sc, char* out, size_t cap) {
    snprintf(out, cap, "op_chain_pad");
    while (set_has(&sc->fields, out, strlen(out)) && strlen(out) + 2 < cap) strcat(out, "_");
}

/** @brief Emit the material block as the case's struct, padded to 256 bytes, and the block holding one struct per chain position.
 *
 * @param block_bytes The standalone block's size. */
static void emit_block(walk_t* w, uint32_t block_bytes) {
    const scan_t* sc = w->sc;
    sb_fmt(w->out, "struct op_chain_blk%d ", w->k, 0);
    w->struct_pending = 1;
    for (int i = sc->blk_open; i < sc->blk_close; i++) emit_tok(w, &w->tk->v[i]);
    const uint32_t used = (block_bytes + 15u) & ~15u;
    if (used < OPENPETE_MOD_LINK_BLOCK_BYTES) {
        char pad[32];
        pad_name(sc, pad, sizeof pad);
        sb_str(w->out, "\n    vec4 ");
        sb_str(w->out, pad);
        sb_fmt(w->out, "[%d];\n", (int)((OPENPETE_MOD_LINK_BLOCK_BYTES - used) / 16u), 0);
    }
    emit_tok(w, &w->tk->v[sc->blk_close]);
    sb_fmt(w->out, ";\nlayout(set = 3, binding = 1, std140) uniform op_chain_ub%d {\n"
                   "    layout(offset = 256) op_chain_blk%d", w->k, w->k);
    sb_fmt(w->out, " op_chain_at%d[%d];\n};", w->k, OPENPETE_MOD_LINK_CHAIN_CAP);
}

/** @brief Emit one member's prefixed text: its tokens through the prefix rules, its material block replaced.
 *
 * @return 0, or -1 when the text does not tokenise. */
static int emit_member(openpete_mod_link_ctx_t* ctx, const openpete_mod_link_member_t* m, int k,
                       const strset_t* header, sbuf_t* out) {
    toks_t tk;
    if (tokenise(m->text, &tk) != 0) { free(tk.v); return -1; }
    scan_t sc;
    scan_member(&tk, &sc);
    walk_t w;
    memset(&w, 0, sizeof w);
    w.tk = &tk;
    w.sc = &sc;
    w.builtin = &ctx->builtin;
    w.header = header;
    w.out = out;
    w.k = k;
    for (int i = 0; i < tk.n; i++) {
        if (i == sc.blk_start) {
            emit_block(&w, m->block_bytes);
            i = sc.blk_end;
            continue;
        }
        emit_tok(&w, &tk.v[i]);
    }
    set_free(&sc.fields);
    free(tk.v);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the combined module                                                 */

/* One member in module order. */
typedef struct { const openpete_mod_link_member_t* m; int input; } order_t;

/** @brief Order members by the SHA-256 of their text, then by input position. */
static int order_cmp(const void* a, const void* b) {
    const order_t* x = (const order_t*)a;
    const order_t* y = (const order_t*)b;
    const int c = memcmp(x->m->sha, y->m->sha, sizeof x->m->sha);
    return c ? c : x->input - y->input;
}

/** @brief Order C strings by strcmp, for qsort over an array of pointers. */
static int str_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/** @brief Emit the distinct `#extension` lines of a member set, sorted. */
static void emit_extensions(const order_t* ord, int n, sbuf_t* out) {
    const char* lines[256];
    int count = 0;
    char* copies[64] = { 0 };
    for (int i = 0; i < n && i < 64; i++) {
        copies[i] = ord[i].m->exts ? strdup(ord[i].m->exts) : NULL;
        for (char* p = copies[i]; p && *p && count < 256;) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (*p) lines[count++] = p;
            p = nl ? nl + 1 : p + strlen(p);
        }
    }
    qsort(lines, (size_t)count, sizeof *lines, str_cmp);
    for (int i = 0; i < count; i++) {
        if (i && strcmp(lines[i], lines[i - 1]) == 0) continue;
        sb_str(out, lines[i]);
        sb_str(out, "\n");
    }
    for (int i = 0; i < n && i < 64; i++) free(copies[i]);
}

/** @brief Emit the generated op_material: a loop over the chain header's positions calling each case's function through a switch.
 *
 * Position 0 receives the engine's base and each later position the
 * previous position's output; a case id the module lacks passes the colour
 * through. */
static void emit_dispatcher(int cases, sbuf_t* out) {
    sb_str(out, "vec4 op_material(vec4 base)\n{\n"
                "    vec4 op_chain_c = base;\n");
    sb_fmt(out, "    uint op_chain_k = min(op_chain_n, %du);\n", OPENPETE_MOD_LINK_CHAIN_CAP, 0);
    sb_str(out, "    for (uint op_chain_i = 0u; op_chain_i < op_chain_k; op_chain_i++) {\n"
                "        op_chain_pos = op_chain_i;\n"
                "        switch (op_chain_ids[op_chain_i]) {\n");
    for (int k = 0; k < cases; k++)
        sb_fmt(out, "        case %du: op_chain_c = m%d_op_material(op_chain_c); break;\n", k, k);
    sb_str(out, "        default: break;\n"
                "        }\n"
                "    }\n"
                "    return op_chain_c;\n"
                "}\n");
}

/** @brief Generate a combined module's text from members in module order.
 *
 * @param case_of Receives each sorted member's case id.
 * @return The case count, or -1 when a member's text does not tokenise or an allocation fails. */
static int module_text(openpete_mod_link_ctx_t* ctx, const order_t* ord, int n, int contract,
                       const strset_t* header, int* case_of, sbuf_t* out) {
    sb_str(out, "#version 450\n#extension GL_GOOGLE_include_directive : require\n");
    emit_extensions(ord, n, out);
    sb_str(out, "#include \"");
    sb_str(out, k_contract_header[contract]);
    sb_str(out, "\"\n"
                "layout(set = 3, binding = 1, std140) uniform op_chain_head {\n"
                "    uint  op_chain_n;\n"
                "    uvec4 op_chain_ids;\n"
                "};\n"
                "uint op_chain_pos;\n");
    int cases = 0;
    for (int i = 0; i < n; i++) {
        if (i && memcmp(ord[i].m->sha, ord[i - 1].m->sha, sizeof ord[i].m->sha) == 0) {
            case_of[i] = case_of[i - 1];
            continue;
        }
        case_of[i] = cases;
        if (emit_member(ctx, ord[i].m, cases, header, out) != 0) return -1;
        cases++;
    }
    emit_dispatcher(cases, out);
    return out->oom ? -1 : cases;
}

/** @brief Copy the first `ERROR:` line of a glslang log, or its first line, into a buffer. */
static void log_head(const char* log_path, char* out, size_t cap) {
    char* txt = log_path ? openpete_mod_glsl_read_file(log_path, NULL) : NULL;
    out[0] = '\0';
    if (!txt) return;
    const char* line = strstr(txt, "ERROR:");
    if (!line) line = txt;
    size_t k = 0;
    while (line[k] && line[k] != '\n' && k + 1 < cap) k++;
    memcpy(out, line, k);
    out[k] = '\0';
    free(txt);
}

/** @brief Compile a module text under the profile preamble through the caller's compile, or uncached through glslang. */
static size_t module_compile(openpete_mod_link_ctx_t* ctx, const char* text, const char* preamble,
                             openpete_mod_link_compile_fn compile, void* user,
                             const char* log_path, uint32_t** words) {
    if (compile) return compile(user, text, preamble, log_path, words);
    return openpete_mod_glsl_compile(text, OPENPETE_MOD_GLSL_STAGE_FRAG, &ctx->inc, preamble,
                                     log_path, words);
}

/** @brief Run the constant-table move on a combined module unless OPENPETE_MOD_SPV_CDATA is 0; a module the rewrite rejects keeps its tables. */
static void module_move_tables(openpete_mod_link_module_t* out) {
    static const char k_material_fn[] = "op_material(";
    const char* off = getenv("OPENPETE_MOD_SPV_CDATA");
    if ((off && strcmp(off, "0") == 0) ||
        !openpete_mod_spv_has_name(out->spv, out->words, k_material_fn, sizeof k_material_fn - 1))
        return;
    uint32_t* moved = NULL;
    size_t moved_n = 0;
    if (openpete_mod_spv_cdata_extract(out->spv, out->words, &moved, &moved_n, &out->cdata) <= 0)
        return;
    free(out->spv);
    out->spv = moved;
    out->words = moved_n;
}

/** @brief Check a build's inputs: a member count of 1 to 64, members of the requested contract and profile, each with text. */
static int build_inputs_ok(const openpete_mod_link_member_t* const* members, int n, int contract,
                           int profile) {
    if (n < 1 || n > 64 || contract < 0 || contract > 1 || profile < 0 ||
        profile >= OPENPETE_MOD_LINK_PROFILES)
        return 0;
    for (int i = 0; i < n; i++)
        if (!members[i] || !members[i]->text || members[i]->contract != contract ||
            members[i]->profile != profile)
            return 0;
    return 1;
}

int openpete_mod_link_build(openpete_mod_link_ctx_t* ctx,
                            const openpete_mod_link_member_t* const* members, int n,
                            int contract, int profile, openpete_mod_link_compile_fn compile,
                            void* user, const char* log_path, int* case_of,
                            openpete_mod_link_module_t* out) {
    memset(out, 0, sizeof *out);
    const strset_t* header = build_inputs_ok(members, n, contract, profile)
        ? header_names(ctx, contract, profile) : NULL;
    if (!header) { snprintf(out->log, sizeof out->log, "bad inputs or unreadable contract header"); return -1; }
    order_t ord[64];
    int sorted_case[64];
    for (int i = 0; i < n; i++) ord[i] = (order_t){ members[i], i };
    qsort(ord, (size_t)n, sizeof *ord, order_cmp);
    sbuf_t text = { NULL, 0, 0, 0 };
    out->cases = module_text(ctx, ord, n, contract, header, sorted_case, &text);
    out->text = sb_take(&text);
    if (out->cases < 0 || !out->text) { snprintf(out->log, sizeof out->log, "text generation failed"); return -1; }
    for (int i = 0; i < n; i++)
        if (case_of) case_of[ord[i].input] = sorted_case[i];
    out->words = module_compile(ctx, out->text, openpete_mod_link_profile_preamble(contract, profile),
                                compile, user, log_path, &out->spv);
    if (out->words == 0) { out->spv = NULL; log_head(log_path, out->log, sizeof out->log); return -1; }
    if (openpete_mod_spv_fn_kills(out->spv, out->words, "op_material(") == 1) {
        out->kills = 1;
        snprintf(out->log, sizeof out->log, "refused: op_material reaches a discard");
        return -1;
    }
    module_move_tables(out);
    return 0;
}

void openpete_mod_link_module_free(openpete_mod_link_module_t* m) {
    if (!m) return;
    free(m->text);
    free(m->spv);
    free(m->cdata.bytes);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ */
/* members                                                             */

static const char* const k_verdict_name[OPENPETE_MOD_LINK_VERDICTS] = {
    "ok", "no-source", "preprocess", "no-material", "has-main", "interface", "block-size",
    "compile", "layout", "discard", "pending"
};

const char* openpete_mod_link_verdict_name(int verdict) {
    return verdict >= 0 && verdict < OPENPETE_MOD_LINK_VERDICTS ? k_verdict_name[verdict] : "?";
}

/** @brief Read what the standalone module says of a member: identity, discard and the block layout. */
static void member_from_spv(openpete_mod_link_member_t* m, int contract, int profile,
                            const uint32_t* spv, size_t words) {
    memset(m, 0, sizeof *m);
    m->contract = contract;
    m->profile = profile;
    m->identity = openpete_mod_spv_fn_identity(spv, words, "op_material(");
    m->kills = openpete_mod_spv_fn_kills(spv, words, "op_material(") == 1;
    m->block_fields = openpete_mod_spv_block_layout(spv, words, 3, 1, m->block_offs,
                                                    OPENPETE_MOD_LINK_FIELDS_MAX, &m->block_bytes);
}

/** @brief Set a member's verdict and reason; returns the verdict. */
static int verdict(openpete_mod_link_member_t* m, int v, const char* reason) {
    m->verdict = v;
    snprintf(m->reason, sizeof m->reason, "%.250s", reason ? reason : "");
    return v;
}

void openpete_mod_link_prebuilt(int contract, int profile, const uint32_t* spv, size_t words,
                                openpete_mod_link_member_t* out) {
    member_from_spv(out, contract, profile, spv, words);
    verdict(out, OPENPETE_MOD_LINK_NO_SOURCE, "no source: a prebuilt .spv is not recompiled");
}

/** @brief Preprocess a member and keep its text, its extension lines and the text's SHA-256.
 *
 * @return 0, or -1 when glslang's preprocessor refuses the source. */
static int member_text(openpete_mod_link_ctx_t* ctx, const char* src, const char* src_path,
                       const char* log_path, openpete_mod_link_member_t* m) {
    char preamble[256];
    const char* prof = openpete_mod_link_profile_preamble(m->contract, m->profile);
    snprintf(preamble, sizeof preamble, "%s%s", k_contract_guard[m->contract], prof ? prof : "");
    openpete_mod_glsl_include_t inc;
    openpete_mod_glsl_include_init(&inc, src_path, "");
    snprintf(inc.sdk_shader_dir, sizeof inc.sdk_shader_dir, "%s", ctx->sdk_shader_dir);
    char* pre = openpete_mod_glsl_preprocess(src, OPENPETE_MOD_GLSL_STAGE_FRAG, &inc, preamble,
                                             log_path, NULL);
    if (!pre) return -1;
    sbuf_t body = { NULL, 0, 0, 0 }, exts = { NULL, 0, 0, 0 };
    split_member(pre, &body, &exts);
    free(pre);
    m->text = sb_take(&body);
    m->exts = sb_take(&exts);
    if (!m->text) return -1;
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    openpete_sha256_update(&hc, m->text, strlen(m->text));
    openpete_sha256_final(&hc, m->sha);
    return 0;
}

/** @brief Run the source checks on a member's text: op_material, main, foreign interfaces and the block size.
 *
 * @return The verdict, OPENPETE_MOD_LINK_OK when every check passes. */
static int member_source_checks(openpete_mod_link_member_t* m) {
    toks_t tk;
    if (tokenise(m->text, &tk) != 0) { free(tk.v); return verdict(m, OPENPETE_MOD_LINK_PREPROCESS, "tokenise failed"); }
    scan_t sc;
    scan_member(&tk, &sc);
    char why[256];
    int v = OPENPETE_MOD_LINK_OK;
    if (!sc.has_material) {
        v = verdict(m, OPENPETE_MOD_LINK_NO_MATERIAL, "defines no op_material");
    } else if (sc.has_main) {
        v = verdict(m, OPENPETE_MOD_LINK_HAS_MAIN, "declares main");
    } else if (sc.iface[0]) {
        snprintf(why, sizeof why, "declares %s", sc.iface);
        v = verdict(m, OPENPETE_MOD_LINK_INTERFACE, why);
    } else if (sc.blk_start >= 0 && m->block_fields < 0) {
        v = verdict(m, OPENPETE_MOD_LINK_LAYOUT,
                    "the standalone module holds no block at set 3 binding 1");
    } else if (m->block_bytes > OPENPETE_MOD_LINK_BLOCK_BYTES) {
        snprintf(why, sizeof why, "block at set 3 binding 1 is %u B, over %u B",
                 m->block_bytes, OPENPETE_MOD_LINK_BLOCK_BYTES);
        v = verdict(m, OPENPETE_MOD_LINK_BLOCK_SIZE, why);
    }
    if (sc.blk_start < 0) m->block_fields = -1;
    set_free(&sc.fields);
    free(tk.v);
    return v;
}

/** @brief Check a one-member module's block struct against the standalone block: member offsets, a 256-byte stride at offset 256.
 *
 * @return OPENPETE_MOD_LINK_OK or OPENPETE_MOD_LINK_LAYOUT. */
static int member_layout_check(openpete_mod_link_member_t* m, const openpete_mod_link_module_t* mod) {
    if (m->block_fields < 0) return OPENPETE_MOD_LINK_OK;
    uint32_t offs[OPENPETE_MOD_LINK_FIELDS_MAX + 1], size = 0, at = 0, stride = 0;
    const int nf = openpete_mod_spv_struct_layout(mod->spv, mod->words, "op_chain_blk0", offs,
                                                  OPENPETE_MOD_LINK_FIELDS_MAX + 1, &size);
    if (nf < m->block_fields ||
        openpete_mod_spv_member_array(mod->spv, mod->words, "op_chain_ub0", 0, &at, &stride) != 0 ||
        at != OPENPETE_MOD_LINK_BLOCK_BYTES || stride != OPENPETE_MOD_LINK_BLOCK_BYTES)
        return verdict(m, OPENPETE_MOD_LINK_LAYOUT, "block struct not at offset 256 with stride 256");
    const int checked = m->block_fields < OPENPETE_MOD_LINK_FIELDS_MAX ? m->block_fields
                                                                       : OPENPETE_MOD_LINK_FIELDS_MAX;
    for (int f = 0; f < checked; f++)
        if (offs[f] != m->block_offs[f])
            return verdict(m, OPENPETE_MOD_LINK_LAYOUT, "block struct member offsets differ from the block");
    return OPENPETE_MOD_LINK_OK;
}

void openpete_mod_link_inspect(int contract, int profile, const uint32_t* spv, size_t words,
                               openpete_mod_link_member_t* out) {
    member_from_spv(out, contract, profile, spv, words);
    verdict(out, OPENPETE_MOD_LINK_PENDING, "source checks and one-member compile not run");
}

int openpete_mod_link_prepare(openpete_mod_link_ctx_t* ctx, const char* src, const char* src_path,
                              openpete_mod_link_compile_fn compile, void* user,
                              const char* log_path, openpete_mod_link_member_t* m) {
    openpete_mod_link_member_free(m);
    if (!src) return verdict(m, OPENPETE_MOD_LINK_NO_SOURCE, "no source");
    if (member_text(ctx, src, src_path, log_path, m) != 0) {
        char why[256];
        log_head(log_path, why, sizeof why);
        return verdict(m, OPENPETE_MOD_LINK_PREPROCESS, why);
    }
    verdict(m, OPENPETE_MOD_LINK_OK, NULL);
    if (member_source_checks(m) != OPENPETE_MOD_LINK_OK) return m->verdict;
    const openpete_mod_link_member_t* one[1] = { m };
    openpete_mod_link_module_t mod;
    if (openpete_mod_link_build(ctx, one, 1, m->contract, m->profile, compile, user, log_path,
                                NULL, &mod) != 0) {
        verdict(m, mod.kills ? OPENPETE_MOD_LINK_DISCARD : OPENPETE_MOD_LINK_COMPILE, mod.log);
    } else {
        member_layout_check(m, &mod);
    }
    openpete_mod_link_module_free(&mod);
    return m->verdict;
}

int openpete_mod_link_discard_refused(int api_pin, const uint32_t* spv, size_t words) {
    return api_pin >= OPENPETE_MOD_LINK_DISCARD_PIN &&
           openpete_mod_spv_fn_kills(spv, words, "op_material(") == 1;
}

void openpete_mod_link_member_free(openpete_mod_link_member_t* m) {
    if (!m) return;
    free(m->text);
    free(m->exts);
    m->text = m->exts = NULL;
}
