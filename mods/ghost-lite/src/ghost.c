/* ghost-lite — rung 16: the actor render service + engine services.
 * See mod.toml. Pattern to copy: a tick hook CAPTURES (sim state, ring +
 * data-dir file); the present hook only reads mod state + ctx, evaluates
 * its own position curve at ctx->alpha, and submits draw_actor commands
 * — the engine never interpolates mod visuals, and the ghost never
 * touches guest RAM. */

#include <openpete_mod_api.h>
#include <spyro1_actor_pose.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING  1024 /* ~34 s of poses at 29.913 Hz */
#define DELAY 90   /* trail ghost runs ~3 s behind the player */
#define LEAD  45   /* prior-run PACE ghost runs ~1.5 s ahead — on identical
                    * inputs (the attract demo) a lockstep replay overlaps
                    * the live player pixel-for-pixel and is fully occluded;
                    * leading keeps the race ghost readable and in frame
                    * (the camera looks forward) */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

typedef struct { unsigned seq; spyro1_actor_pose_t pose; } rec_t;

static rec_t    g_ring[RING];
static unsigned g_seq;     /* successful captures this run              */
static FILE*    g_rec_fp;  /* this run's recording (data-dir service)   */

static spyro1_actor_pose_t* g_prior;   /* prior-run recording (race ghost) */
static unsigned             g_prior_n;

/* Overlay-tunable replay parameters. Host-side only: the widgets write
 * them, the present hook reads them — no guest state involved, so they
 * are live without disturbing the sim. */
static int g_show_trail  = 1;
static int g_show_pace   = 1;
static int g_delay       = DELAY;
static int g_lead        = LEAD;
static int g_translucent = 1;

/* Present-hook draw submissions, for the overlay status line. */
static unsigned g_trail_draws;
static unsigned g_pace_draws;

static void on_camera_update(CPUState* cpu) {
    g_api->base(cpu);
    spyro1_actor_pose_t p;
    if (g_api->capture_actor_pose(SPYRO1_ACTOR_KIND_PLAYER, &p,
                                  (int)sizeof p) != (int)sizeof p)
        return; /* player not rendered yet (boot / cutscene) */
    if (g_seq == 0)
        g_api->log(g_self, OP_MOD_LOG_INFO, "ghost-lite: recording");
    g_ring[g_seq % RING].seq  = g_seq;
    g_ring[g_seq % RING].pose = p;
    if (g_rec_fp) { fwrite(&p, sizeof p, 1, g_rec_fp); fflush(g_rec_fp); }
    g_seq++;
    if (g_seq == DELAY + 2)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "ghost-lite: ghost armed (trailing %d ticks)", DELAY);
}

/* Lerp only `pos` between two consecutive records (everything else in the
 * blob is opaque stream state — take the earlier record's). The tint's
 * alpha byte is forced to 0xFF when translucency is off; PSX blending
 * averages, so any A < 0xFF is the same translucent look. Returns 0 when
 * the command was queued. */
static int draw_lerped(const spyro1_actor_pose_t* a,
                       const spyro1_actor_pose_t* b, float frac,
                       uint32_t tint) {
    spyro1_actor_pose_t p = *a;
    if (b && b->model == a->model)
        for (int i = 0; i < 3; i++)
            p.pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * frac;
    if (!g_translucent) tint |= 0xFF000000u;
    return g_api->draw_actor(SPYRO1_ACTOR_KIND_PLAYER, &p, (int)sizeof p,
                             tint);
}

static void on_present(const openpete_present_ctx_t* ctx) {
    /* Trail ghost: index runs on the mod's own capture sequence,
     * evaluated at alpha — sub-tick smooth by the mod's hand. */
    if (g_show_trail && g_seq >= (unsigned)g_delay + 2) {
        double t = (double)(g_seq - 1) + (double)ctx->alpha - (double)g_delay;
        if (t >= 0.0) {
            unsigned i0 = (unsigned)t;
            float frac = (float)(t - (double)i0);
            if (i0 + 1 < g_seq && g_seq - i0 < RING - 1) {
                const rec_t* r0 = &g_ring[i0 % RING];
                const rec_t* r1 = &g_ring[(i0 + 1) % RING];
                if (r0->seq == i0 && r1->seq == i0 + 1 &&
                    draw_lerped(&r0->pose, &r1->pose, frac, 0x80A0C8FFu) == 0)
                    g_trail_draws++;
            }
        }
    }
    /* Prior-run PACE ghost: the previous session's recording, replayed
     * LEAD ticks ahead of our capture sequence (see the LEAD comment). */
    if (g_show_pace && g_prior_n >= 2 && g_seq > 0) {
        double t = (double)(g_seq - 1) + (double)ctx->alpha + (double)g_lead;
        unsigned i0 = (unsigned)t;
        if (i0 + 1 < g_prior_n &&
            draw_lerped(&g_prior[i0], &g_prior[i0 + 1],
                        (float)(t - (double)i0), 0x80FFD060u) == 0)
            g_pace_draws++;
    }
}

/* Overlay section: replay status plus the two ghosts' timing offsets,
 * which the present hook above reads on its next present. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("captured %u poses (ring holds %d)", g_seq, RING);
    if (g_prior_n)
        ui->text("prior run: %u poses", g_prior_n);
    else
        ui->text_disabled("prior run: none — this run becomes the pace "
                          "ghost next launch");
    ui->text("drawn: trail %u, pace %u", g_trail_draws, g_pace_draws);

    ui->separator();
    ui->checkbox("Trail ghost (blue)", &g_show_trail);
    ui->slider_int("Trail delay", &g_delay, 10, 600);
    ui->tooltip("Ticks the trail ghost replays behind the player. The ring "
                "holds ~34 s of poses; longer delays fall out of it.");
    ui->checkbox("Pace ghost (gold)", &g_show_pace);
    ui->slider_int("Pace lead", &g_lead, 0, 300);
    ui->tooltip("Ticks the prior run replays ahead of the live capture. At "
                "0 an identical run overlaps the player and is occluded.");

    ui->separator();
    ui->checkbox("Translucent", &g_translucent);
    ui->tooltip("Off draws both ghosts opaque.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;

    /* Data-dir service: load the previous run's recording, then start
     * this run's (truncating — the newest run is the race ghost). */
    const char* dd = api->data_dir(self);
    if (dd) {
        char path[1024];
        snprintf(path, sizeof path, "%s/ghost.s1pose", dd);
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned n = (unsigned)(sz / (long)sizeof(spyro1_actor_pose_t));
            if (n > 0) {
                g_prior = (spyro1_actor_pose_t*)malloc(
                    (size_t)n * sizeof(spyro1_actor_pose_t));
                if (g_prior &&
                    fread(g_prior, sizeof(spyro1_actor_pose_t), n, f) == n) {
                    g_prior_n = n;
                    api->log(self, OP_MOD_LOG_INFO,
                             "ghost-lite: loaded %u recorded poses", n);
                } else {
                    free(g_prior); g_prior = NULL;
                }
            }
            fclose(f);
        }
        g_rec_fp = fopen(path, "wb");
        if (g_rec_fp)
            api->log(self, OP_MOD_LOG_INFO, "ghost-lite: recording to %s",
                     path);
    }

    api->register_ui(self, ui_draw, NULL);

    if (api->register_present_hook(self, on_present) != 0) return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
