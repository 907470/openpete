/* mod_mat_link.h - material composition by source prefixing: a member's
 * linkability, and the combined module of a member set per contract and
 * profile.
 *
 * A member is one material fragment shader's source in one profile (stock,
 * retained terrain, portal; the decal contract has the stock profile only).
 * Preparing it preprocesses the source through glslang with the contract
 * header's include guard predefined, so the text holds only the member's
 * own declarations with its macros expanded, and decides whether it can
 * enter a combined module. Building a module prefixes every identifier the
 * member declares with `m<k>_` (k its case index), maps its uniform block
 * at set 3 binding 1 to one 256-byte array element per chain position, and
 * compiles the prefixed members under one copy of the contract header with
 * a generated `op_material` that loops over the chain header block and
 * calls each member's renamed function through a switch.
 *
 * Generated names start with `op_chain_`: the chain header block
 * `op_chain_head` (`op_chain_n` members, case ids `op_chain_ids`), the
 * chain position `op_chain_pos`, and per case k the struct
 * `op_chain_blk<k>`, the block `op_chain_ub<k>` and its array
 * `op_chain_at<k>`. A prefixed member identifier starts with `m<digit>`
 * and the contract headers declare no `op_chain_` name, so neither can
 * equal a generated name; a member's unprefixed tokens are keywords,
 * builtins, header names and fields reached after `.`.
 *
 * Depends on mod_glsl.c (glslang), mod_spv_reflect.c, mod_spv_cdata.c and
 * the SHA-256 helper; no GPU, no loader. mod_shader.c prepares members at
 * registration; the mod_material_link test builds modules from the census
 * shaders and fixtures. */
#ifndef OPENPETE_MOD_MAT_LINK_H
#define OPENPETE_MOD_MAT_LINK_H

#include "mod_spv_cdata.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contracts, equal to OPENPETE_MATERIAL_CONTRACT_SCENE and _DECAL. */
#define OPENPETE_MOD_LINK_CONTRACT_SCENE 0
#define OPENPETE_MOD_LINK_CONTRACT_DECAL 1

/* Profiles of the scene contract, in mod_shader.c's cdata order. */
#define OPENPETE_MOD_LINK_STOCK    0
#define OPENPETE_MOD_LINK_TERRAIN  1
#define OPENPETE_MOD_LINK_PORTAL   2
#define OPENPETE_MOD_LINK_PROFILES 3
/* A build profile with no members of its own: the stock members' text
 * compiled under the actor preamble (openpete_psx_material.glsl
 * OP_PROFILE_ACTOR), whose engine half adds only a statement to main(). */
#define OPENPETE_MOD_LINK_ACTOR    3

/* Chain positions a module serves, and the bytes of one member block. The
 * set 3 binding 1 range a chain draw binds is the 256-byte header plus one
 * block per position: 256 + 4 x 256 = 1280 bytes. */
#define OPENPETE_MOD_LINK_CHAIN_CAP   4
#define OPENPETE_MOD_LINK_BLOCK_BYTES 256u

/* Block members a prepared member records offsets for. */
#define OPENPETE_MOD_LINK_FIELDS_MAX 64

/* The first api pin whose fragment modules are refused when op_material
 * reaches a discard. */
#define OPENPETE_MOD_LINK_DISCARD_PIN 13

/* A member's verdict. */
enum {
    OPENPETE_MOD_LINK_OK = 0,       /* linkable                                           */
    OPENPETE_MOD_LINK_NO_SOURCE,    /* a prebuilt .spv: no source to prefix               */
    OPENPETE_MOD_LINK_PREPROCESS,   /* glslang's preprocessor refused the source          */
    OPENPETE_MOD_LINK_NO_MATERIAL,  /* the text defines no op_material                    */
    OPENPETE_MOD_LINK_HAS_MAIN,     /* the text declares main                             */
    OPENPETE_MOD_LINK_INTERFACE,    /* an interface variable, push constant or descriptor
                                     * besides the block at set 3 binding 1               */
    OPENPETE_MOD_LINK_BLOCK_SIZE,   /* the block is larger than 256 bytes                 */
    OPENPETE_MOD_LINK_COMPILE,      /* the one-member module does not compile             */
    OPENPETE_MOD_LINK_LAYOUT,       /* the module's block struct is not laid out as the
                                     * standalone block                                   */
    OPENPETE_MOD_LINK_DISCARD,      /* the one-member module's op_material reaches a
                                     * discard, so the module is refused                  */
    OPENPETE_MOD_LINK_PENDING,      /* inspected only: openpete_mod_link_prepare not run  */
    OPENPETE_MOD_LINK_VERDICTS
};

typedef struct openpete_mod_link_ctx openpete_mod_link_ctx_t;

/* One prepared member. */
typedef struct openpete_mod_link_member {
    int      verdict;     /* OPENPETE_MOD_LINK_*                                    */
    char     reason[256]; /* what decided a verdict other than OK                   */
    int      contract;    /* OPENPETE_MOD_LINK_CONTRACT_*                           */
    int      profile;     /* OPENPETE_MOD_LINK_STOCK / _TERRAIN / _PORTAL           */
    int      identity;    /* the standalone op_material returns its parameter      */
    int      kills;       /* the standalone op_material reaches a discard           */
    int      block_fields;  /* members of the standalone block; -1 = no block      */
    uint32_t block_bytes;   /* the standalone block's size, not rounded             */
    uint32_t block_offs[OPENPETE_MOD_LINK_FIELDS_MAX];
    char*    text;        /* preprocessed text without #version, #extension, #line
                           * and blank lines (malloc'd; NULL when not preprocessed) */
    char*    exts;        /* its #extension lines but the include directive's        */
    uint8_t  sha[32];     /* SHA-256 of text: the member's sort key                  */
} openpete_mod_link_member_t;

/* A combined module. */
typedef struct {
    char*     text;       /* the generated GLSL (malloc'd)                          */
    uint32_t* spv;        /* the compiled module, tables moved (malloc'd)           */
    size_t    words;
    openpete_mod_spv_cdata_t cdata;   /* the moved constant tables                  */
    int       cases;      /* distinct members: the switch's case ids 0..cases-1     */
    int       kills;      /* refused: the dispatcher reaches a discard              */
    char      log[512];   /* the failure, when the build failed                     */
} openpete_mod_link_module_t;

/* Compiles one generated module text under a profile preamble; returns the
 * word count (0 on failure) and the malloc'd words in *out. NULL in the
 * calls below compiles with openpete_mod_glsl_compile uncached. */
typedef size_t (*openpete_mod_link_compile_fn)(void* user, const char* text,
                                               const char* preamble, const char* log_path,
                                               uint32_t** out);

/** @brief Create a link context: the builtin names and, per contract and profile on first use, the contract header's names.
 *
 * @param sdk_dir The staged SDK root; the headers are `<sdk_dir>/shaders/`.
 * @return The context, or NULL on allocation failure. */
openpete_mod_link_ctx_t* openpete_mod_link_ctx_new(const char* sdk_dir);

/** @brief Free a link context.
 *
 * @param ctx The context; NULL does nothing. */
void openpete_mod_link_ctx_free(openpete_mod_link_ctx_t* ctx);

/** @brief Return the preamble that compiles a contract's profile, the text mod_shader.c injects after `#version`.
 *
 * @param contract OPENPETE_MOD_LINK_CONTRACT_*.
 * @param profile OPENPETE_MOD_LINK_STOCK, _TERRAIN, _PORTAL or _ACTOR.
 * @return The preamble, or NULL for the stock profile. */
const char* openpete_mod_link_profile_preamble(int contract, int profile);

/** @brief Return a verdict's name, such as "ok" or "interface".
 *
 * @param verdict An OPENPETE_MOD_LINK_* verdict.
 * @return The name, or "?" out of range. */
const char* openpete_mod_link_verdict_name(int verdict);

/** @brief Inspect one member's standalone module: identity, discard and the block layout, with verdict OPENPETE_MOD_LINK_PENDING.
 *
 * Reads SPIR-V only, so registration can run it on every material
 * fragment; openpete_mod_link_prepare decides the verdict later.
 * @param contract OPENPETE_MOD_LINK_CONTRACT_*.
 * @param profile OPENPETE_MOD_LINK_STOCK, _TERRAIN or _PORTAL.
 * @param spv The member's standalone module in this profile.
 * @param words Its word count.
 * @param out Receives the member. */
void openpete_mod_link_inspect(int contract, int profile, const uint32_t* spv, size_t words,
                               openpete_mod_link_member_t* out);

/** @brief Decide an inspected member's verdict: preprocess, the source checks and the one-member compile.
 *
 * Linkable (OPENPETE_MOD_LINK_OK) when the text defines op_material and
 * declares no main; declares no interface variable, push constant or
 * descriptor besides one uniform block at set 3 binding 1; that block is
 * at most 256 bytes; and the member's prefixed text compiles as a
 * one-member module whose block struct has the standalone block's member
 * offsets, a 256-byte array stride at offset 256, and whose op_material
 * reaches no discard. The checks run in that order and the first failure
 * decides the verdict and its reason. A text kept from an earlier call is
 * freed first.
 * @param ctx The link context.
 * @param src The member's NUL-terminated source; NULL gives no-source.
 * @param src_path Its path; local includes resolve beside it.
 * @param compile The one-member compile, or NULL.
 * @param user Passed to @p compile.
 * @param log_path The file glslang failures are written to.
 * @param m The member, inspected by openpete_mod_link_inspect.
 * @return The verdict. */
int openpete_mod_link_prepare(openpete_mod_link_ctx_t* ctx, const char* src, const char* src_path,
                              openpete_mod_link_compile_fn compile, void* user,
                              const char* log_path, openpete_mod_link_member_t* m);

/** @brief Record a member that has no source (a prebuilt .spv): unlinkable on every profile.
 *
 * Identity and discard are still read from the module.
 * @param contract OPENPETE_MOD_LINK_CONTRACT_*.
 * @param profile The profile.
 * @param spv The prebuilt module.
 * @param words Its word count.
 * @param out Receives the member. */
void openpete_mod_link_prebuilt(int contract, int profile, const uint32_t* spv, size_t words,
                                openpete_mod_link_member_t* out);

/** @brief Build the combined module of a member set.
 *
 * Members are ordered by the SHA-256 of their text, and members with equal
 * text share one case, so a set gives byte-identical text and SPIR-V in any
 * input order. The text: `#version 450`, the include directive, the
 * members' other `#extension` lines (sorted, distinct), the contract
 * header, the chain header block and `op_chain_pos`, each case's prefixed
 * text, and the dispatcher. It compiles under the profile preamble; a
 * module whose op_material reaches a discard is refused; the constant-table
 * move then runs on it (openpete_mod_spv_cdata_extract) unless
 * OPENPETE_MOD_SPV_CDATA is 0.
 * @param ctx The link context.
 * @param members The members, each linkable or in preparation, all of one contract and profile
 *        (the stock profile for OPENPETE_MOD_LINK_ACTOR).
 * @param n Their count, at least 1.
 * @param contract OPENPETE_MOD_LINK_CONTRACT_*.
 * @param profile The profile; OPENPETE_MOD_LINK_ACTOR on the scene contract only.
 * @param compile The compile, or NULL.
 * @param user Passed to @p compile.
 * @param log_path The file a glslang failure is written to.
 * @param case_of Receives each input member's case id; may be NULL.
 * @param out Receives the module; free with openpete_mod_link_module_free.
 * @return 0, or -1 when the text cannot be generated, the compile fails or
 *         the module is refused (out->log says which). */
int openpete_mod_link_build(openpete_mod_link_ctx_t* ctx,
                            const openpete_mod_link_member_t* const* members, int n,
                            int contract, int profile, openpete_mod_link_compile_fn compile,
                            void* user, const char* log_path, int* case_of,
                            openpete_mod_link_module_t* out);

/** @brief Generate a combined module's text and case ids, the first half of openpete_mod_link_build; no glslang compile runs.
 *
 * @param ctx The link context.
 * @param members The members, as openpete_mod_link_build takes them.
 * @param n Their count, at least 1.
 * @param contract OPENPETE_MOD_LINK_CONTRACT_*.
 * @param profile The profile; OPENPETE_MOD_LINK_ACTOR on the scene contract only.
 * @param case_of Receives each input member's case id; may be NULL.
 * @param out Receives the text and the case count; free with openpete_mod_link_module_free.
 * @return 0, or -1 when the inputs are bad or the text cannot be generated (out->log says which). */
int openpete_mod_link_build_text(openpete_mod_link_ctx_t* ctx,
                                 const openpete_mod_link_member_t* const* members, int n,
                                 int contract, int profile, int* case_of,
                                 openpete_mod_link_module_t* out);

/** @brief Compile a module whose text openpete_mod_link_build_text generated, the second half of openpete_mod_link_build: the compile, the Float64 and discard refusals and the constant-table move.
 *
 * Reads no member, so it may run on another thread than the text's
 * generation while the members change, given a non-NULL @p compile.
 * @param ctx The link context; read only when @p compile is NULL.
 * @param contract The contract the text was generated for.
 * @param profile The profile the text was generated for.
 * @param compile The compile, or NULL for an uncached glslang compile through @p ctx.
 * @param user Passed to @p compile.
 * @param log_path The file a glslang failure is written to.
 * @param out The module holding the text; receives the SPIR-V and the tables.
 * @return 0, or -1 when the compile fails or the module is refused (out->log says which). */
int openpete_mod_link_build_compile(openpete_mod_link_ctx_t* ctx, int contract, int profile,
                                    openpete_mod_link_compile_fn compile, void* user,
                                    const char* log_path, openpete_mod_link_module_t* out);

/** @brief Return whether a fragment module is refused for a mod's api pin: pinned at 13 or later and op_material reaching a discard.
 *
 * @param api_pin The owner's manifest api pin.
 * @param spv The module.
 * @param words Its word count.
 * @return 1 when refused, 0 otherwise. */
int openpete_mod_link_discard_refused(int api_pin, const uint32_t* spv, size_t words);

/** @brief Free a prepared member's text; the struct itself is the caller's.
 *
 * @param m The member; NULL does nothing. */
void openpete_mod_link_member_free(openpete_mod_link_member_t* m);

/** @brief Free a module's text, SPIR-V and table block; the struct itself is the caller's.
 *
 * @param m The module; NULL does nothing. */
void openpete_mod_link_module_free(openpete_mod_link_module_t* m);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_MAT_LINK_H */
