/* rhi_pipe_census.c - pipeline-create tally, first-use hitch metric and
 * census row (contract in rhi_pipe_census.h). */
#include "rhi_pipe_census.h"

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_LOG(...) fprintf(stderr, "[rhi/vk] " __VA_ARGS__)
#define PC_EVENTS   24   /* hitch events listed individually per run */

static const char *const k_phase_name[RHI_PC_PHASES] = {
    "bringup", "precompile", "rebuild", "first_use", "probe", "launcher"
};
static const char *const k_kind_name[RHI_PC_KINDS] = {
    "pipeline", "lib_preraster", "lib_fragment", "lib_output", "link", "link_lto"
};
static const char *const k_site_name[RHI_PIPE_SITES] = {
    "builtin", "terrain_cache", "builtin_lazy", "mod_postfx",
    "mod_material", "terrain_material", "rebuild"
};

/* Open-addressed set of nonzero 64-bit keys (0 marks an empty slot); the
 * header's rhi_pc_keyset has the same layout. */
typedef rhi_pc_keyset pc_set;

typedef struct {
    unsigned creates, hit, miss, unknown;
    uint64_t wall_ns, driver_ns;
} pc_tally;

typedef struct {
    unsigned creates, miss, unknown, hit, owner, worker;
    uint64_t sum_ns, max_ns;
} pc_hitch;

static SDL_Mutex  *s_mx;
static atomic_int  s_mx_init;     /* 0 none, 1 creating, 2 ready; release publishes s_mx */
static SDL_AtomicInt s_visits[RHI_PIPE_SITES];
static pc_tally    s_tally[RHI_PC_PHASES];
static unsigned    s_known[RHI_PC_PHASES];   /* rhi_pc_known_add */
static pc_tally    s_kind[RHI_PIPE_SITES][RHI_PC_KINDS];
static pc_hitch    s_hitch[RHI_PIPE_SITES];
static rhi_pc_create s_events[PC_EVENTS];
static unsigned    s_event_n;
static unsigned    s_bases[RHI_PIPE_SITES], s_raw[RHI_PIPE_SITES];
static pc_set      s_full_site[RHI_PIPE_SITES], s_decl_site[RHI_PIPE_SITES];
static pc_set      s_full_all, s_decl_all;

/** @brief Take the records' mutex, creating it on first use. */
static void pc_lock(void)
{
    if (atomic_load_explicit(&s_mx_init, memory_order_acquire) != 2) {
        int none = 0;
        if (atomic_compare_exchange_strong(&s_mx_init, &none, 1)) {
            s_mx = SDL_CreateMutex();
            atomic_store_explicit(&s_mx_init, 2, memory_order_release);
        }
        while (atomic_load_explicit(&s_mx_init, memory_order_acquire) != 2) SDL_Delay(0);
    }
    SDL_LockMutex(s_mx);
}

/** @brief Release the records' mutex. */
static void pc_unlock(void) { SDL_UnlockMutex(s_mx); }

/** @brief Insert a key into a set, growing it at half load.
 *
 * @param s The set.
 * @param k The key; 0 is stored as 1.
 * @return 1 when the key was new, 0 when present or on allocation failure. */
static int pc_set_insert(pc_set *s, uint64_t k)
{
    if (!k) k = 1;
    if (2 * (s->n + 1) > s->cap) {
        const size_t ncap = s->cap ? 2 * s->cap : 256;
        uint64_t *ns = calloc(ncap, sizeof *ns);
        if (!ns) return 0;
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->slot[i]) continue;
            size_t j = (size_t)s->slot[i] & (ncap - 1);
            while (ns[j]) j = (j + 1) & (ncap - 1);
            ns[j] = s->slot[i];
        }
        free(s->slot);
        s->slot = ns;
        s->cap = ncap;
    }
    size_t j = (size_t)k & (s->cap - 1);
    while (s->slot[j]) {
        if (s->slot[j] == k) return 0;
        j = (j + 1) & (s->cap - 1);
    }
    s->slot[j] = k;
    s->n++;
    return 1;
}

/** @brief Fold one 64-bit value into an FNV-1a 64 hash, byte by byte.
 *
 * @param h Running hash.
 * @param v Value.
 * @return The updated hash. */
static uint64_t pc_mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 0x100000001b3ull; }
    return h;
}

uint64_t rhi_pc_bytes_hash(const void *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = p;
    for (size_t i = 0; b && i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

uint64_t rhi_pc_desc_hash(const rhi_pipeline_desc *d)
{
    const uint64_t f[] = {
        (uint64_t)d->blend, d->depth_test, d->depth_write_off, d->depth_overwrite,
        d->depth_bias, (uint64_t)d->depth_class, (uint64_t)d->stencil,
        d->color_write_off, (uint64_t)(int64_t)d->texel_stp_select,
        d->uses_vram, d->uses_vram2, d->uses_vram3, d->uses_vram4, d->uses_vram5,
        d->set1_vram_depth, d->uniform_set2, d->uniform_set5, d->uniform_set7,
        d->pair_set56, d->pair_set7, d->ssbo_set7,
        d->push_const_size, d->postfx, d->mod_pipeline, d->material_pipeline,
        d->terrain_pipeline, d->terrain_anim, d->terrain_near, d->terrain_cells,
        d->wire_scene,
    };
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) h = pc_mix(h, f[i]);
    return h;
}

/** @brief Return word i of a SPIR-V blob.
 *
 * @param b Blob bytes.
 * @param i Word index.
 * @return The word. */
static uint32_t pc_word(const uint8_t *b, size_t i)
{
    uint32_t w;
    memcpy(&w, b + 4 * i, sizeof w);
    return w;
}

uint32_t rhi_pc_spec_ids(const void *spv, size_t bytes)
{
    enum { OP_DECORATE = 71, DEC_SPEC_ID = 1 };
    const uint8_t *b = spv;
    const size_t n = b ? bytes / 4 : 0;
    uint32_t ids = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t w0 = pc_word(b, i), len = w0 >> 16;
        if (len == 0 || i + len > n) break;
        if ((w0 & 0xFFFFu) == OP_DECORATE && len >= 4 && pc_word(b, i + 2) == DEC_SPEC_ID &&
            pc_word(b, i + 3) < 32)
            ids |= 1u << pc_word(b, i + 3);
        i += len;
    }
    return ids;
}

rhi_pipe_site rhi_pc_site_of(const rhi_pipeline_desc *d, bool words)
{
    if (d->postfx) return words ? RHI_PIPE_SITE_MOD_POSTFX : RHI_PIPE_SITE_BUILTIN_LAZY;
    if (d->terrain_pipeline) return words ? RHI_PIPE_SITE_TERRAIN_MATERIAL : RHI_PIPE_SITE_TERRAIN_CACHE;
    if (d->material_pipeline || d->mod_pipeline) return RHI_PIPE_SITE_MOD_MATERIAL;
    return RHI_PIPE_SITE_BUILTIN;
}

/** @brief Return a variant's dedupe key.
 *
 * @param v The variant.
 * @param declared_only True to hash only the constants its stages declare.
 * @return The key. */
static uint64_t pc_key(const rhi_pc_variant *v, bool declared_only)
{
    uint64_t h = pc_mix(pc_mix(pc_mix(0xcbf29ce484222325ull, v->desc_hash), v->vs_hash), v->fs_hash);
    for (int i = 0; i < 6; i++)
        h = pc_mix(h, (!declared_only || (v->declared >> i) & 1u) ? (uint64_t)v->consts[i] + 1u : 0u);
    return pc_mix(pc_mix(h, v->poly_mode), v->render_pass);
}

uint64_t rhi_pc_variant_key(const rhi_pc_variant *v)
{
    const uint64_t k = pc_key(v, true);
    return k ? k : 1u;
}

int rhi_pc_keyset_insert(rhi_pc_keyset *s, uint64_t k)
{
    return pc_set_insert((pc_set *)s, k);
}

int rhi_pc_keyset_has(const rhi_pc_keyset *s, uint64_t k)
{
    if (!k) k = 1;
    if (!s->cap) return 0;
    size_t j = (size_t)k & (s->cap - 1);
    while (s->slot[j]) {
        if (s->slot[j] == k) return 1;
        j = (j + 1) & (s->cap - 1);
    }
    return 0;
}

void rhi_pc_keyset_free(rhi_pc_keyset *s)
{
    free(s->slot);
    s->slot = NULL;
    s->cap = s->n = 0;
}

void rhi_pc_add_base(rhi_pipe_site site, const rhi_pc_variant *v, int nv)
{
    if ((unsigned)site >= RHI_PIPE_SITES || nv <= 0 || nv > 8) return;
    pc_set local = {0};
    pc_lock();
    s_bases[site]++;
    for (int i = 0; i < nv; i++) {
        const uint64_t full = pc_key(&v[i], false), decl = pc_key(&v[i], true);
        s_raw[site] += (unsigned)pc_set_insert(&local, full);
        pc_set_insert(&s_full_site[site], full);
        pc_set_insert(&s_decl_site[site], decl);
        pc_set_insert(&s_full_all, full);
        pc_set_insert(&s_decl_all, decl);
    }
    pc_unlock();
    free(local.slot);
}

int rhi_pc_level(void)
{
    static SDL_AtomicInt lvl = { -1 };   /* read from compile jobs on any thread */
    int v = SDL_GetAtomicInt(&lvl);
    if (v < 0) {
        const char *e = getenv("OPENPETE_PIPE_CENSUS");
        v = (e && *e) ? atoi(e) : 0;
        if (v < 0) v = 0;
        SDL_SetAtomicInt(&lvl, v);
    }
    return v;
}

/** @brief Count one create of the first-use or rebuild phase in its class's hitch row and the event list.
 *
 * Caller holds the mutex.
 * @param c The create. */
static void pc_hitch_add(const rhi_pc_create *c)
{
    const rhi_pipe_site site = c->phase == RHI_PC_PHASE_REBUILD ? RHI_PIPE_SITE_REBUILD : c->site;
    pc_hitch *h = &s_hitch[site];
    h->creates++;
    h->hit += c->fb == RHI_PC_FB_HIT;
    if (c->fb == RHI_PC_FB_HIT) return;
    h->miss += c->fb == RHI_PC_FB_MISS;
    h->unknown += c->fb == RHI_PC_FB_UNKNOWN;
    h->owner += c->owner;
    h->worker += !c->owner;
    h->sum_ns += c->wall_ns;
    if (c->wall_ns > h->max_ns) h->max_ns = c->wall_ns;
    if (s_event_n < PC_EVENTS) s_events[s_event_n] = *c;
    s_event_n++;
}

/** @brief Add one create to a tally row.
 *
 * Caller holds the mutex.
 * @param t The row.
 * @param c The create. */
static void pc_tally_add(pc_tally *t, const rhi_pc_create *c)
{
    t->creates++;
    t->hit += c->fb == RHI_PC_FB_HIT;
    t->miss += c->fb == RHI_PC_FB_MISS;
    t->unknown += c->fb == RHI_PC_FB_UNKNOWN;
    t->wall_ns += c->wall_ns;
    t->driver_ns += c->driver_ns;
}

void rhi_pc_record_create(const rhi_pc_create *c)
{
    if ((unsigned)c->phase >= RHI_PC_PHASES || (unsigned)c->site >= RHI_PIPE_SITES ||
        (unsigned)c->kind >= RHI_PC_KINDS) return;
    static const char *const k_fb[3] = { "unknown", "hit", "miss" };
    pc_lock();
    pc_tally_add(&s_tally[c->phase], c);
    pc_tally_add(&s_kind[c->site][c->kind], c);
    if (c->phase == RHI_PC_PHASE_FIRST_USE || c->phase == RHI_PC_PHASE_REBUILD) pc_hitch_add(c);
    pc_unlock();
    if (rhi_pc_level() >= 2)
        PC_LOG("pipe-create: phase=%s site=%s kind=%s cache=%s wall_ms=%.3f driver_ms=%.3f "
               "thread=%s present=%llu\n", k_phase_name[c->phase], k_site_name[c->site],
               k_kind_name[c->kind], k_fb[c->fb], (double)c->wall_ns / 1e6,
               (double)c->driver_ns / 1e6, c->owner ? "owner" : "worker",
               (unsigned long long)c->present);
}

void rhi_pc_known_add(rhi_pc_phase phase)
{
    if ((unsigned)phase >= RHI_PC_PHASES) return;
    pc_lock();
    s_known[phase]++;
    pc_unlock();
}

void rhi_pipe_site_visit(rhi_pipe_site site, int count)
{
    if ((unsigned)site < RHI_PIPE_SITES && count > 0) SDL_AddAtomicInt(&s_visits[site], count);
}

void rhi_pc_rebuild_visit(void) { rhi_pipe_site_visit(RHI_PIPE_SITE_REBUILD, 1); }

unsigned rhi_pc_bringup(uint64_t *out_ns)
{
    pc_lock();
    const unsigned n = s_tally[RHI_PC_PHASE_BRINGUP].creates;
    *out_ns = s_tally[RHI_PC_PHASE_BRINGUP].wall_ns;
    pc_unlock();
    return n;
}

void rhi_pc_tally_get(rhi_pc_phase phase, unsigned *hit, unsigned *miss, unsigned *unknown)
{
    *hit = *miss = *unknown = 0;
    if ((unsigned)phase >= RHI_PC_PHASES) return;
    pc_lock();
    *hit = s_tally[phase].hit;
    *miss = s_tally[phase].miss;
    *unknown = s_tally[phase].unknown;
    pc_unlock();
}

/** @brief Print one tally line per phase.
 *
 * Caller holds the mutex. */
static void pc_report_tally(void)
{
    for (int p = 0; p < RHI_PC_PHASES; p++) {
        const pc_tally *t = &s_tally[p];
        PC_LOG("pipe-tally: phase=%s creates=%u hit=%u miss=%u unknown=%u known=%u wall_ms=%.1f "
               "driver_ms=%.1f\n", k_phase_name[p], t->creates, t->hit, t->miss, t->unknown,
               s_known[p], (double)t->wall_ns / 1e6, (double)t->driver_ns / 1e6);
    }
}

/** @brief Print one line per site class and create kind that counted a create, every phase together.
 *
 * Caller holds the mutex. */
static void pc_report_kinds(void)
{
    for (int s = 0; s < RHI_PIPE_SITES; s++)
        for (int k = 0; k < RHI_PC_KINDS; k++) {
            const pc_tally *t = &s_kind[s][k];
            if (!t->creates) continue;
            PC_LOG("pipe-kind: class=%s kind=%s creates=%u hit=%u miss=%u unknown=%u "
                   "wall_ms=%.1f driver_ms=%.1f\n", k_site_name[s], k_kind_name[k], t->creates,
                   t->hit, t->miss, t->unknown, (double)t->wall_ns / 1e6,
                   (double)t->driver_ns / 1e6);
        }
}

/** @brief Print one hitch line per site class with its engagement verdict, then the first events.
 *
 * Caller holds the mutex. The lazy classes and the rebuild class read VACUOUS at zero visits; the
 * eager classes (builtin, terrain_cache) have no lazy site and read n/a. */
static void pc_report_hitch(void)
{
    for (int s = 0; s < RHI_PIPE_SITES; s++) {
        const pc_hitch *h = &s_hitch[s];
        const int visits = SDL_GetAtomicInt(&s_visits[s]);
        const bool eager = s == RHI_PIPE_SITE_BUILTIN || s == RHI_PIPE_SITE_TERRAIN_CACHE;
        PC_LOG("pipe-hitch: class=%s visits=%d creates=%u hit=%u miss=%u unknown=%u "
               "sum_ms=%.1f max_ms=%.1f owner=%u worker=%u engagement=%s\n",
               k_site_name[s], visits, h->creates, h->hit, h->miss, h->unknown,
               (double)h->sum_ns / 1e6, (double)h->max_ns / 1e6, h->owner, h->worker,
               eager ? "n/a" : visits > 0 ? "engaged" : "VACUOUS");
    }
    const unsigned shown = s_event_n < PC_EVENTS ? s_event_n : PC_EVENTS;
    for (unsigned i = 0; i < shown; i++) {
        const rhi_pc_create *c = &s_events[i];
        PC_LOG("pipe-hitch-event: %u class=%s phase=%s cache=%s wall_ms=%.2f thread=%s present=%llu\n",
               i, k_site_name[c->phase == RHI_PC_PHASE_REBUILD ? RHI_PIPE_SITE_REBUILD : c->site],
               k_phase_name[c->phase], c->fb == RHI_PC_FB_MISS ? "miss" : "unknown",
               (double)c->wall_ns / 1e6, c->owner ? "owner" : "worker",
               (unsigned long long)c->present);
    }
    if (s_event_n > shown) PC_LOG("pipe-hitch-event: %u more not listed\n", s_event_n - shown);
}

/** @brief Print one census line per site class and the run total.
 *
 * Caller holds the mutex. */
static void pc_report_census(void)
{
    unsigned bases = 0, raw = 0;
    for (int s = 0; s < RHI_PIPE_SITE_REBUILD; s++) {
        bases += s_bases[s];
        raw += s_raw[s];
        PC_LOG("pipe-census: class=%s bases=%u raw=%u dedup_full=%zu dedup_declared=%zu\n",
               k_site_name[s], s_bases[s], s_raw[s], s_full_site[s].n, s_decl_site[s].n);
    }
    PC_LOG("pipe-census: class=all bases=%u raw=%u dedup_full=%zu dedup_declared=%zu\n",
           bases, raw, s_full_all.n, s_decl_all.n);
}

void rhi_pc_report(void)
{
    if (!rhi_pc_level()) return;
    pc_lock();
    pc_report_tally();
    pc_report_kinds();
    pc_report_hitch();
    pc_report_census();
    pc_unlock();
}
