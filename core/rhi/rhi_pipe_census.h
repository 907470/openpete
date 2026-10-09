/* rhi_pipe_census.h - pipeline-create instruments of the Vulkan RHI (private).
 *
 * Three records over every vkCreateGraphicsPipelines call of a run:
 *   - the tally: creates per phase (bring-up, precompile, live rebuild, first
 *     use, probe) split by the creation-feedback cache-hit bit, with wall and
 *     driver-reported time;
 *   - the first-use hitch metric: creates in the first-use and rebuild
 *     phases per site class, with the thread that ran them, the present a
 *     display-owner create delayed, and each class's lazy-site visits as its
 *     engagement count;
 *   - the census row: per site class, the pipeline bases created, their raw
 *     create infos over the reachable variant views, and the create infos
 *     deduped across bases on the full pipeline description, once with every
 *     specialization constant handed to a stage and once with only the
 *     constants its stages declare.
 * The records are reported at device destruction when OPENPETE_PIPE_CENSUS
 * is set (2 adds one line per create). Every entry point is safe on any
 * thread: one mutex guards the records. */
#ifndef RHI_PIPE_CENSUS_H
#define RHI_PIPE_CENSUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rhi.h"

/* Phase a create is counted in. */
typedef enum {
    RHI_PC_PHASE_BRINGUP = 0,   /* before the first frame was submitted      */
    RHI_PC_PHASE_PRECOMPILE,    /* inside rhi_pipe_phase_set(PRECOMPILE)      */
    RHI_PC_PHASE_REBUILD,       /* a live rebuild (rhi_rebuild_request)      */
    RHI_PC_PHASE_FIRST_USE,     /* after the first frame, outside the above  */
    RHI_PC_PHASE_PROBE,         /* inside rhi_pipe_phase_set(PROBE)           */
    RHI_PC_PHASES
} rhi_pc_phase;

/* Creation-feedback reading of one create. */
typedef enum {
    RHI_PC_FB_UNKNOWN = 0,      /* feedback not enabled or not marked valid  */
    RHI_PC_FB_HIT,              /* APPLICATION_PIPELINE_CACHE_HIT set        */
    RHI_PC_FB_MISS              /* valid, hit bit clear                      */
} rhi_pc_feedback;

/* One variant's create info, reduced to the fields the dedupe keys read. */
typedef struct {
    uint64_t desc_hash;     /* rhi_pc_desc_hash of the pipeline's desc       */
    uint64_t vs_hash;       /* vertex stage SPIR-V content hash              */
    uint64_t fs_hash;       /* fragment stage SPIR-V content hash            */
    uint32_t declared;      /* bit i: constant_id i declared by either stage */
    uint32_t consts[6];     /* the values handed over; 0 for an undeclared id */
    uint32_t poly_mode;     /* VkPolygonMode                                 */
    uint64_t render_pass;   /* VkRenderPass handle the pipeline targets      */
} rhi_pc_variant;

/* One create, as recorded by rhi_pc_record_create. */
typedef struct {
    rhi_pc_phase    phase;
    rhi_pipe_site   site;
    rhi_pc_feedback fb;
    bool            owner;          /* ran on the display owner                */
    uint64_t        present;        /* frame serial of the present it delayed  */
    uint64_t        wall_ns;        /* time inside vkCreateGraphicsPipelines   */
    uint64_t        driver_ns;      /* feedback duration; 0 when unknown       */
} rhi_pc_create;

/** @brief Return the site class of a pipeline from its desc and whether its stages were given as SPIR-V words.
 *
 * postfx without words is a built-in lazy postfx, with words a mod postfx;
 * a terrain pipeline with fragment words is a terrain material, without
 * them a terrain-cache pipeline; a material or mod layout is a mod
 * material; anything else is a built-in.
 * @param desc Pipeline description.
 * @param words True when a stage was given as SPIR-V words.
 * @return The site class (never RHI_PIPE_SITE_REBUILD). */
rhi_pipe_site rhi_pc_site_of(const rhi_pipeline_desc *desc, bool words);

/** @brief Return the FNV-1a 64 hash of every desc field that reaches a create info, excluding the stage names.
 *
 * Hashed field by field, so struct padding never enters the key.
 * @param desc Pipeline description.
 * @return The hash. */
uint64_t rhi_pc_desc_hash(const rhi_pipeline_desc *desc);

/** @brief Return the FNV-1a 64 hash of n bytes.
 *
 * @param p The bytes, or NULL.
 * @param n Their count.
 * @return The hash; the FNV offset basis for NULL or zero bytes. */
uint64_t rhi_pc_bytes_hash(const void *p, size_t n);

/** @brief Return the constant_id set a SPIR-V module declares through OpDecorate SpecId.
 *
 * Walks the instruction stream after the 5-word header, reading each word
 * with memcpy so an unaligned blob is accepted; a malformed instruction
 * length ends the walk.
 * @param spv Module bytes, or NULL.
 * @param bytes Their count.
 * @return Bit i set when constant_id i (below 32) is declared. */
uint32_t rhi_pc_spec_ids(const void *spv, size_t bytes);

/** @brief Return the dedupe key of a variant: the full pipeline description with only the constants its stages declare.
 *
 * The one key of the precompile set, the census row's dedup_full column and
 * the warm-boot key manifest.
 * @param v The variant.
 * @return The key, never 0. */
uint64_t rhi_pc_variant_key(const rhi_pc_variant *v);

/* Open-addressed set of nonzero 64-bit keys (0 marks an empty slot);
 * zero-initialised it is empty. Not locked: one owner per set. */
typedef struct {
    uint64_t *slot;
    size_t    cap;   /* power of two, or 0 before the first insert */
    size_t    n;
} rhi_pc_keyset;

/** @brief Insert a key into a set, growing it at half load.
 *
 * @param s The set.
 * @param k The key; 0 is stored as 1.
 * @return 1 when the key was new, 0 when present or on allocation failure. */
int rhi_pc_keyset_insert(rhi_pc_keyset *s, uint64_t k);

/** @brief Return whether a set holds a key.
 *
 * @param s The set.
 * @param k The key; 0 reads as 1.
 * @return 1 when present. */
int rhi_pc_keyset_has(const rhi_pc_keyset *s, uint64_t k);

/** @brief Free a set's storage and leave it empty.
 *
 * @param s The set. */
void rhi_pc_keyset_free(rhi_pc_keyset *s);

/** @brief Add one pipeline base and its variant create infos to the census row.
 *
 * The base's raw count is the number of distinct full keys among its
 * variants; the deduped counts are the keys not yet seen in the run, per
 * class and over every class.
 * @param site The base's site class.
 * @param v Its variants.
 * @param nv Their count (at most 8). */
void rhi_pc_add_base(rhi_pipe_site site, const rhi_pc_variant *v, int nv);

/** @brief Count one pipeline create in the tally and, in the first-use and rebuild phases, in the hitch metric.
 *
 * At OPENPETE_PIPE_CENSUS=2 the create is also logged on one line.
 * @param c The create. */
void rhi_pc_record_create(const rhi_pc_create *c);

/** @brief Count one rebuild call as a visit of the rebuild class. */
void rhi_pc_rebuild_visit(void);

/** @brief Return whether OPENPETE_PIPE_CENSUS is set, read once.
 *
 * @return 0 off, 1 the end-of-run report, 2 the report plus one line per create. */
int rhi_pc_level(void);

/** @brief Print the tally, the hitch metric with each class's engagement verdict, and the census row.
 *
 * Prints nothing unless rhi_pc_level is nonzero. */
void rhi_pc_report(void);

/** @brief Return the bring-up phase's create count and wall time.
 *
 * @param out_ns Receives the summed wall time in ns.
 * @return The number of creates counted in the bring-up phase. */
unsigned rhi_pc_bringup(uint64_t *out_ns);

/** @brief Read one phase's creation-feedback counts.
 *
 * @param phase The phase.
 * @param hit Receives its cache hits.
 * @param miss Receives its cache misses.
 * @param unknown Receives its creates with no valid feedback. */
void rhi_pc_tally_get(rhi_pc_phase phase, unsigned *hit, unsigned *miss, unsigned *unknown);

#endif /* RHI_PIPE_CENSUS_H */
