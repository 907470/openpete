/* core/runtime/native_render.c — native Vulkan renderer A/B harness.
 *
 * Phase 1c of notes/plans/openpete-native-renderer.md. See
 * openpete_native_render.h for the contract + the two-window rationale.
 *
 * 1c.1: RHI in the runtime, Vulkan window alongside PsyCross.
 * 1c.2: PsyCross hidden; its frame read back + shown in the single window.
 * 1c.3a: mirror PsyCross's VRAM into a native R16_UINT texture each present
 *   (proving real game VRAM reaches the native GPU path).
 * 1c.3b (this step): panel 0 renders the PSX DISPLAY REGION cropped to the
 *   visible viewport. The DISPENV disp rect (512x240, from PutDispEnv) and the
 *   DRAWENV clip rect (512x224, from PutDrawEnv) are captured; the panel
 *   samples the full disp width but only clip.h rows, dropping the symmetric
 *   8px overscan bands, and is fitted at the
 *   reference aspect so it lines up with the reference panel. The front-buffer
 *   origin follows the per-frame double-buffer flip via the material tpage.
 *   Layout: [native-display | reference]. 1c.3b-ii adds the diff panel +
 *   validates against the logo screen.
 *
 * Determinism: nothing here touches PSX RAM or the `.s` verdict producers.
 */

#include "openpete_native_render.h"

#include "openpete_log.h"
#include "openpete_time.h"   /* op_now — overlay's own sub-tick alpha (camera-independent) */
#include "openpete_record.h" /* native --record capture seam */
#include "rhi.h"
#include "screenshot.h"  /* openpete_write_png_rgba */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>   /* snprintf */
#include <stdlib.h>
#include <string.h>

/* PsyCross VRAM + present-FBO readback (extern "C" in PsyX_render.cpp);
 * forward-declared so this C TU need not include the C++-flavored header.
 * GR_ReadVRAM copies a rect of the 1024x512 CPU VRAM mirror tightly into dst;
 * GR_ReadPresentSceneRGBA returns the present FBO bottom-up (GL origin). */
extern int  GR_GetPresentSceneSize(int *out_w, int *out_h);
extern int  GR_ReadPresentSceneRGBA(unsigned char *pixels, int w, int h,
                                    unsigned int gl_format);
extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h);

/* GL_RGBA enum — byte order R,G,B,A, matching VK_FORMAT_R8G8B8A8_UNORM. */
#define NR_GL_RGBA 0x1908u

#define NR_VRAM_W 1024
#define NR_VRAM_H 512

/* Default window size when the caller passes none. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Per-draw PSX material — must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y;
    int32_t clut_x, clut_y;
    int32_t bpp;
} psx_material;

/* How many panels the window shows (1c.3a: VRAM + reference). */
#define NR_PANELS 2

/* Native 3D scene (Phase 2): screen-space triangles the game pushes in
 * (cyclorama/sky from gamestate). Double-buffered isn't needed — the game
 * fills it during its render pass (before openpete_native_render_frame reads
 * it). begin() clears; add_tri() appends. */
/* Native scene-triangle cap. Sized for the worst-case --no-lod load: forced-HQ
 * routes every foreground terrain cell through the 2x2 subdivide (~4x the tris)
 * and renders every moby with no distance cull, so the densest content
 * (gnastys_loot, the all-gems loot vault) peaks at ~43k tris vs ~10k for a
 * typical level at stock LOD. The old 16384 cap silently dropped everything past
 * it, and which tris dropped depended on the view → terrain that appeared/
 * disappeared with camera angle. Worst measured across all 35 levels is
 * beast_makers at ~63k (a homeworld: every enemy + gem + dragon rendered with no
 * cull, plus forced-HQ terrain); 131072 is ~2x that, covering worst-camera-angle
 * beyond the charge-path sample. The drop counter below is the permanent safety
 * net for any outlier. (Static arrays cost ~68 MB; dynamic sizing keyed on
 * --no-lod is a future optimisation — see the disable-lods plan.) */
#define MAX_SCENE_TRIS 131072
#define SCENE_VB_VERTS (MAX_SCENE_TRIS * 3 + 18)  /* +6 bg-fill, +6 fade overlay, +6 freeze backdrop */
#define OVL_MAX_VERTS 4096   /* overlay (3D text-moby glyph) vertex cap (see draw_fps_overlay) */
/* FPS-overlay supersampling: render the glyphs at OVL_SS_FACTOR× into a small
 * RGBA target covering the top-left OVL_REGION_W × OVL_REGION_H of the frame (in
 * win_h-fraction units, matching the glyph layout's win_h-relative coords), then
 * box-downsample + alpha-blend onto the frame so the corner counter is crisp at
 * any resolution. The region is generous enough for "FPS: 9999". */
#define OVL_SS_FACTOR  4
#define OVL_REGION_W   0.30f
#define OVL_REGION_H   0.05f
static openpete_scene_tri s_scene[MAX_SCENE_TRIS];      /* accumulating (frame N) */
static int                s_scene_n;
static uint32_t           s_scene_bg;  /* panel-0 background fill (rgba); 0 = none */

/* Reference frame-sync: the native scene is built from the FRESHLY-computed
 * geometry (frame N), but PsyCross's reference shows its display FBO, which lags
 * the native geometry (the OT PsyCross draws is the one Spyro built a frame or
 * two earlier — PSX double-buffer + the build-vs-draw split). Comparing the
 * native scene against the reference as-is puts them N frames apart — in A/B the
 * two panels are skewed; in diff EVERY moving object ghosts (it appears at both
 * frames in |native - ref|). To keep native and the reference on the SAME frame
 * we delay the native scene by an integer DEPTH: keep a ring of recent completed
 * frames and present the one `depth` frames back. Default depth in any
 * reference-compare mode = AB_DIFF_SCENE_DELAY (measured to align with the lagged
 * reference); vk-only has no reference, so depth 0 (live frame, runs ahead).
 * OPENPETE_NATIVE_SCENE_DELAY=<n> overrides with an explicit integer depth. */
#define SCENE_HIST 4   /* ring of completed-frame snapshots; supports delay depth 0..3 */
#define AB_DIFF_SCENE_DELAY 2   /* ab/diff default: native lagged 2 frames to match PsyCross */
static openpete_scene_tri s_scene_hist[SCENE_HIST][MAX_SCENE_TRIS]; /* completed-frame ring */
static int                s_scene_hist_n[SCENE_HIST];
static uint32_t           s_scene_hist_bg[SCENE_HIST];
static uint32_t           s_scene_hist_fade[SCENE_HIST];  /* full-screen fade overlay, per snapshot */
static uint32_t           s_scene_hist_freeze[SCENE_HIST]; /* freeze-backdrop overlay, per snapshot */
static int                s_scene_hist_hud_under[SCENE_HIST]; /* flat-HUD-under-player flag, per snapshot */
static int                s_scene_hist_head = -1;  /* ring index of the most-recent completed frame */
static int                s_scene_hist_count = 0;  /* completed frames recorded (warmup guard) */
static int                s_scene_delay = -2;   /* -2 uninit; >=0 = depth */
static int                s_scene_ref_compare = 1; /* set by init: 1 = ab/diff (lagged reference), 0 = vk-only */

static int scene_delay_depth(void) {
    if (s_scene_delay < -1) {
        const char* e = getenv("OPENPETE_NATIVE_SCENE_DELAY");
        /* Explicit env wins (integer depth); otherwise default to the measured
         * ab/diff lag so native and the lagged reference are the same frame.
         * vk-only has no reference → depth 0 (live frame). */
        if (e && *e) { int d = atoi(e); s_scene_delay = (d < 0) ? 0 : (d >= SCENE_HIST ? SCENE_HIST - 1 : d); }
        else         s_scene_delay = s_scene_ref_compare ? AB_DIFF_SCENE_DELAY : 0;
    }
    return s_scene_delay;
}

/* Current draw layer stamped onto every add_tri (OPENPETE_SCENE_WORLD/_ACTOR).
 * Reset to WORLD per frame so a non-actor feeder never inherits a stale ACTOR. */
static int s_scene_category = OPENPETE_SCENE_WORLD;

/* Carry flag stamped onto every add_tri: 1 = sub-tick-uncarryable 2D-UI (see the
 * header). Reset to 0 per frame so a feeder that never opts in emits carry=0. */
static int s_scene_carry = 0;

/* Full-screen fade overlay for the current frame (func_800190D4). 0 = none; else
 * bit31 = active, bits[25:24] = PSX ABR mode (0..3), bits[23:0] = rgb (R in 16..23,
 * G in 8..15, B in 0..7). Reset every frame; the present draws it dead last. */
static uint32_t s_scene_fade = 0;

/* Freeze-frame green overlay for the current frame (pause / inventory / flight
 * results). 0 = inactive; else bit31 = active, bits[23:0] = green tint rgb (the
 * FT4's setRGB0). Reset every frame; when active the present draws frozen_tex
 * desaturated + tinted as the backdrop instead of the world. */
static uint32_t s_scene_freeze = 0;

/* Flat-HUD-vs-player draw order for the current frame. 0 (default) = flat HUD/text
 * drawn OVER the player (on top of everything); 1 = drawn UNDER the player, set only
 * during GS_LevelTransition (the "THE ADVENTURE BEGINS"/"ENTERING <level>" flyby,
 * where Spyro flies in front of the revealed text). Reset per frame; the feeder sets
 * it from g_Gamestate. The 3D HUD icons (HUD3D) and OVERLAY/fade are always on top. */
static int s_scene_hud_under = 0;

void openpete_native_scene_begin(void) { s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_category = OPENPETE_SCENE_WORLD; s_scene_carry = 0; }

/* Frame-keyed reset: the first feeder of each game frame clears; later feeders
 * (in any order) append, so the cyclorama + world + mobys compose into one
 * scene instead of clobbering each other. See the header for the rationale.
 * On a frame change, the just-completed scene (still in s_scene) is snapshotted
 * to s_scene_disp before the clear, so the present can draw the 1-frame-delayed
 * copy that matches the reference panel. */
static unsigned s_scene_frame = 0xFFFFFFFFu;
static void alpha_census(const openpete_scene_tri* scene, int scene_n);  /* 3c instrument, defined below */
static bool scene_has_world(const openpete_scene_tri* scene, int n);     /* defined below */
static int s_scene_dropped;       /* tris refused this frame (cap hit) — capdbg */
void openpete_native_scene_begin_frame(unsigned frame) {
    if (frame != s_scene_frame) {
        if (s_scene_frame != 0xFFFFFFFFu) {  /* a completed frame is sitting in s_scene */
            /* Permanent safety net: if the scene cap is ever exceeded, terrain/
             * mobys silently vanish — warn once so the cap can be raised rather
             * than leaving a view-dependent dropout in the wild. */
            if (s_scene_dropped > 0) {
                static int s_warned = 0;
                if (!s_warned) { s_warned = 1;
                    fprintf(stderr, "native: WARNING scene cap %d exceeded (frame=%u wanted=%d, "
                            "%d tris dropped) — geometry will vanish view-dependently; raise MAX_SCENE_TRIS\n",
                            MAX_SCENE_TRIS, s_scene_frame, s_scene_n + s_scene_dropped, s_scene_dropped); }
            }
            static int s_capdbg = -1; static int s_scene_peak = 0;
            if (s_capdbg < 0) { const char* e = getenv("OPENPETE_SCENE_CAPDBG"); s_capdbg = (e && *e && *e != '0'); }
            if (s_capdbg) {
                int want = s_scene_n + s_scene_dropped;   /* true demand (raised caps → dropped=0) */
                if (want > s_scene_peak) { s_scene_peak = want;
                    fprintf(stderr, "[scenecap] frame=%u NEW PEAK want=%d (accepted=%d/%d dropped=%d)\n",
                            s_scene_frame, want, s_scene_n, MAX_SCENE_TRIS, s_scene_dropped); }
            }
            s_scene_dropped = 0;
            alpha_census(s_scene, s_scene_n);  /* census the completed scene (runs even headless) */
            int slot = (s_scene_hist_head + 1) % SCENE_HIST;
            if (s_scene_n > 0) memcpy(s_scene_hist[slot], s_scene, (size_t)s_scene_n * sizeof(s_scene[0]));
            s_scene_hist_n[slot] = s_scene_n; s_scene_hist_bg[slot] = s_scene_bg;
            s_scene_hist_fade[slot] = s_scene_fade;
            s_scene_hist_freeze[slot] = s_scene_freeze;
            s_scene_hist_hud_under[slot] = s_scene_hud_under;
            s_scene_hist_head = slot;
            if (s_scene_hist_count < SCENE_HIST) s_scene_hist_count++;
        }
        s_scene_n = 0; s_scene_bg = 0; s_scene_fade = 0; s_scene_freeze = 0; s_scene_hud_under = 0; s_scene_frame = frame;
        s_scene_category = OPENPETE_SCENE_WORLD; s_scene_carry = 0;
    }
}
void openpete_native_scene_add_tri(const openpete_scene_tri *tri) {
    if (!tri) return;
    if (s_scene_n < MAX_SCENE_TRIS) {
        s_scene[s_scene_n] = *tri;
        s_scene[s_scene_n].category = s_scene_category;  /* stamp the active layer */
        s_scene[s_scene_n].carry = s_scene_carry;        /* stamp the carry flag */
        s_scene_n++;
    } else {
        s_scene_dropped++;   /* cap hit — this tri (and the rest of the feed) is lost */
    }
}
void openpete_native_scene_set_category(int category) { s_scene_category = category; }
void openpete_native_scene_set_carry(int on) { s_scene_carry = on ? 1 : 0; }
void openpete_native_scene_set_bg(uint32_t rgba) { s_scene_bg = rgba; }

/* Gamestate draw-env clear colour (the framebuffer background the canonical
 * render auto-clears to). Captured from the PutDrawEnv thunk; the primary bg
 * source in draw_native_scene (preferred over g_Cyclorama, which is stale in
 * cyclorama-less states). rgba R,G,B,A; 0 = unset / no auto-clear → fall back to
 * the scene-fed colour. Persists across sub-tick presents (PutDrawEnv only fires
 * on canonical frames). */
static uint32_t s_drawenv_clear = 0;
void openpete_native_set_clear_color(int isbg, int r, int g, int b) {
    s_drawenv_clear = isbg
        ? (0xFF000000u | (uint32_t)(r & 0xFF) | ((uint32_t)(g & 0xFF) << 8)
                       | ((uint32_t)(b & 0xFF) << 16))
        : 0u;   /* no auto-clear → defer to the scene-fed bg / ref corner */
}
/* The game's fade for THIS tick + a "set this tick" flag. The flag (not the
 * begin_frame reset of s_scene_fade) is what tells the re-extract whether the
 * fade is actually active: the game calls func_800190D4 every tick the fade is
 * on and simply STOPS when it ends (no explicit clear). The re-extract's own
 * apply_overlay writes s_scene_fade, which would otherwise feed back into
 * commit_fade and freeze the last transition's fade on screen forever. */
static uint32_t s_game_fade = 0;
static int      s_game_fade_seen = 0;
/* Same "active only if the game set it this tick" tracking for the freeze backdrop
 * (pause/inventory/flight-results). Without it the carry fed back into itself
 * (apply_overlay re-set s_scene_freeze from the carry, capture read it back) and the
 * freeze never cleared when the menu closed → the world stayed hidden on unpause. */
static uint32_t s_game_freeze = 0;
static int      s_game_freeze_seen = 0;

void openpete_native_scene_set_fade(int mode, int r, int g, int b) {
    uint32_t v;
    if (mode < 0) { v = 0; }
    else {
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        v = (1u << 31) | ((uint32_t)(mode & 3) << 24)
          | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    s_scene_fade = v;                       /* raw (no-interp) present path */
    s_game_fade = v; s_game_fade_seen = 1;  /* the game set the fade this tick */
}
void openpete_native_scene_set_freeze_backdrop(int r, int g, int b, int fade) {
    if (r < 0) { s_scene_freeze = 0; return; }
    if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (fade < 0) fade = 0; if (fade > 127) fade = 127;   /* 7-bit fade shift in bits 24-30 */
    s_scene_freeze = (1u << 31) | ((uint32_t)fade << 24)
                   | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    s_game_freeze = s_scene_freeze; s_game_freeze_seen = 1;  /* the game set it this tick */
}
void openpete_native_scene_set_hud_under_player(int under) { s_scene_hud_under = under ? 1 : 0; }

/* --- 2D-overlay carry across sub-tick re-extracts -------------------------
 * The fade overlay (func_800190D4) and the cutscene letterbox bars
 * (func_80018F30, OVERLAY-category F4s) + menu panel (HUD_PANEL) are produced
 * by the 2D-UI feeder while the game renders. They are NOT re-derivable from
 * gamestate by the sub-tick re-extract (it has no 2D-prim chokepoint to hook),
 * and the re-extract's scene_begin() wipes them — so without carrying them they
 * vanish on every re-extracted present (which, since the canonical present now
 * also routes through the re-extract for the 1-tick lag, means they vanish
 * outright). They're screen-space and tick-constant, so capture them from the
 * live scene before the rebuild and re-apply them after. Categories that the
 * 3D feeders re-produce (HUD/HUD3D billboards, PLAYER near-OT text) are NOT
 * carried by category — they'd double. The 2D-UI HUD SPRITES/LINES (title logo,
 * "PRESS START", HUD glyph text, UI separators) live in OPENPETE_SCENE_HUD too,
 * but the 3D feeders never re-derive THEM, so they carry by an explicit per-tri
 * flag (set_carry, stamped by add_tri) that the moby near-OT billboards leave 0. */
#define OVERLAY_CARRY_MAX 1024
static openpete_scene_tri s_overlay_carry[OVERLAY_CARRY_MAX];      /* curr (latest canonical capture) */
static int                s_overlay_carry_n;
static uint32_t           s_overlay_carry_freeze;
/* The previous canonical carry set, so apply_overlay can interpolate the SCREEN
 * POSITION of an animated 2D-UI element across sub-ticks — the title-logo drop-in
 * slide moves a few px/tick; held (un-interpolated) it staircases at 30 Hz just
 * like the fade did. Rolled curr->prev by capture_overlay (canonical only); paired
 * by index when the two captures have equal count + matching sprite identity
 * (category/textured/uv), else that tri snaps to curr (a structural change — a
 * blink toggling the count, a glyph swap — is discrete anyway). */
static openpete_scene_tri s_overlay_prev[OVERLAY_CARRY_MAX];
static int                s_overlay_prev_n;
/* Wall-clock timestamps of the two most-recent CANONICAL overlay captures, so
 * apply_overlay computes the position/fade interpolation alpha from the OVERLAY's
 * OWN cadence — NOT the camera-bracket alpha, which the re-extract leaves at 0
 * whenever the camera+Spyro ring dedupes (a STATIC camera: the title screen, a
 * pause menu). The logo drop-in slides while the title camera is still, so without
 * a camera-independent alpha it staircased at the canonical tick rate. (Same trap
 * the 3D HUD-icon spin/slide hit — fixed there with per-object timestamps.) */
static op_time_t          s_overlay_curr_ns, s_overlay_prev_ns;

/* The fade ramps per sim tick, so unlike the (static) letterbox/panel tris it is
 * interpolated rather than held: s_fade_prev/curr are the two most-recent
 * CANONICAL fades (rolled by commit_fade on each canonical present), and
 * apply_overlay lerps them by the spine alpha — a 1-tick-lagged smooth ramp,
 * consistent with the rest of the scene. Without this the fade staircases at
 * 30 Hz (intermediates hold the previous tick's brightness). */
static uint32_t s_fade_prev, s_fade_curr;

void openpete_native_scene_commit_fade(void) {
    s_fade_prev = s_fade_curr;
    /* The fade is active this tick only if the game actually called set_fade
     * since the last commit; else it ended (the game stopped calling) → off. */
    s_fade_curr = s_game_fade_seen ? s_game_fade : 0;
    s_game_fade_seen = 0;
    /* Same one-tick liveness for the freeze backdrop carry: held while the game keeps
     * emitting the freeze FT4, cleared the tick it stops (menu closed → unpause), so
     * the spine's apply_overlay turns the backdrop off and the world reappears. (This
     * replaces capturing it from s_scene_freeze, which fed back into itself.) */
    uint32_t prev_freeze = s_overlay_carry_freeze;
    uint32_t new_freeze  = s_game_freeze_seen ? s_game_freeze : 0;
    /* Bridge a 1-frame freeze gap at the GS_Paused -> GS_ExitLevel handoff: the pause
     * menu stops emitting its freeze FT4 the frame it closes, but GS_ExitLevel's
     * func_8001C694 hasn't drawn its own freeze yet — so for exactly one canonical
     * tick neither sets the freeze and the carry would drop, letting the spine's
     * re-extracted (reset) Spyro + bg flash through. The game doesn't flicker because
     * its frozen framebuffer persists in VRAM. A handoff frame carries only the reset
     * Spyro (PLAYER) + bg with no real WORLD/ACTOR geometry, so when the freeze is
     * dropping but the scene has no world, hold the previous freeze. A genuine unpause
     * rebuilds terrain (WORLD) that same tick, so scene_has_world is true and the
     * freeze drops correctly — the world reappears with no lag. */
    if (new_freeze == 0 && prev_freeze != 0 && !scene_has_world(s_scene, s_scene_n)) {
        new_freeze = prev_freeze;
        /* The canonical present of this degenerate tick may show the RAW game-fiber
         * scene (the sub-tick re-extract that re-applies the carry is skipped on a
         * stuck/transition tick), which left the live s_scene_freeze at 0 — so force
         * it too, and drop the degenerate geometry (reset Spyro + bg) so only the
         * held freeze shows, exactly as on a settled freeze frame. */
        s_scene_freeze = new_freeze;
        s_scene_n = 0;
    }
    s_overlay_carry_freeze = new_freeze;
    s_game_freeze_seen = 0;
}

/* Lerp two packed fade words (bit31 active, [25:24] ABR mode, [23:0] rgb). */
static uint32_t fade_lerp(uint32_t a, uint32_t b, double t) {
    if (!(b & (1u << 31))) return 0;                         /* curr inactive → no fade */
    if (!(a & (1u << 31))) return b;                         /* prev inactive → snap on */
    if ((a & (3u << 24)) != (b & (3u << 24))) return b;      /* mode changed → snap */
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r =(int)(ar+(br-ar)*t+0.5), g=(int)(ag+(bg-ag)*t+0.5), bl=(int)(ab+(bb-ab)*t+0.5);
    return (b & 0xFF000000u) | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)bl;
}

void openpete_native_scene_capture_overlay(void) {
    /* Roll curr -> prev so apply_overlay can interpolate animated 2D-UI positions
     * (the logo slide-in) across sub-ticks. Runs on the CANONICAL present only, so
     * prev/curr are consecutive canonical captures (one sim tick apart). */
    if (s_overlay_carry_n > 0)
        memcpy(s_overlay_prev, s_overlay_carry, (size_t)s_overlay_carry_n * sizeof(s_overlay_carry[0]));
    s_overlay_prev_n = s_overlay_carry_n;
    s_overlay_prev_ns = s_overlay_curr_ns;   /* roll the capture timestamp too */
    s_overlay_curr_ns = op_now();
    s_overlay_carry_n = 0;
    int s_overlay_dropped = 0;
    for (int t = 0; t < s_scene_n; t++) {
        int c = s_scene[t].category, cy = s_scene[t].carry;
        /* carry==2 = a tri RE-APPLIED by apply_overlay last present. The canonical
         * game render appends to the prior sub-tick's (un-wiped) scene, so those
         * re-applied tris are still here — skip them or the carry set accumulates
         * unbounded (the 2D-UI logo cloning). Only the GAME RENDER's fresh emission
         * is captured: OVERLAY/HUD_PANEL by category, 2D-UI sprites/lines by carry==1
         * (see openpete_native_scene_set_carry). Capture runs on the CANONICAL present
         * only (run.c), so a clear here is correct (sub-ticks hold the set). */
        if (cy == 2) continue;
        if (c == OPENPETE_SCENE_OVERLAY || c == OPENPETE_SCENE_HUD_PANEL || cy == 1) {
            if (s_overlay_carry_n < OVERLAY_CARRY_MAX) s_overlay_carry[s_overlay_carry_n++] = s_scene[t];
            else s_overlay_dropped++;
        }
    }
    /* Mirror the scene-cap warn-once: a busy title/menu that overruns the carry
     * buffer silently loses 2D-UI sprites sub-tick with no signal — warn so the
     * cap can be raised rather than leaving a view-dependent dropout in the wild. */
    if (s_overlay_dropped > 0) {
        static int s_ov_warned = 0;
        if (!s_ov_warned) { s_ov_warned = 1;
            fprintf(stderr, "native: WARNING OVERLAY_CARRY_MAX %d exceeded (frame=%u, %d 2D-UI carry "
                    "tris dropped) — sprites will vanish sub-tick; raise OVERLAY_CARRY_MAX\n",
                    OVERLAY_CARRY_MAX, s_scene_frame, s_overlay_dropped);
        }
    }
    /* NOTE: the freeze backdrop carry is committed in commit_fade (the per-tick
     * overlay-signal commit), NOT here — capturing it from s_scene_freeze created a
     * feedback loop (apply_overlay re-set s_scene_freeze from the carry, so capture
     * read it straight back and the freeze never died after the menu closed → the
     * world stayed hidden on unpause). See s_game_freeze_seen. */
}

void openpete_native_scene_apply_overlay(double alpha) {
    /* The passed `alpha` is the camera-bracket alpha — 0 when the camera ring
     * dedupes (static camera). Animated 2D-UI (the logo drop-in) moves while the
     * camera is still, so derive the overlay's own alpha from its capture cadence:
     * 1-tick-lagged (now≈curr_ns ⇒ show prev, advance to curr over the next
     * capture period), exactly the camera bracket's convention but from the
     * overlay's timestamps. Falls back to the camera alpha if the timestamps
     * aren't primed yet (first frames). */
    double oalpha = alpha;
    if (s_overlay_curr_ns > s_overlay_prev_ns && s_overlay_prev_ns > 0) {
        double period = (double)(s_overlay_curr_ns - s_overlay_prev_ns);
        oalpha = (double)(op_now() - s_overlay_curr_ns) / period;
        if (oalpha < 0.0) oalpha = 0.0; else if (oalpha > 1.0) oalpha = 1.0;
    }
    /* Pair curr with prev by index only when the two captures are structurally the
     * same (equal count) — a count change (a blink toggling, a glyph added) makes
     * index pairing meaningless, so snap to curr that present. */
    int pairable = (s_overlay_prev_n == s_overlay_carry_n);
    for (int t = 0; t < s_overlay_carry_n && s_scene_n < MAX_SCENE_TRIS; t++) {
        openpete_scene_tri tri = s_overlay_carry[t];   /* curr; category already stamped */
        if (pairable) {
            const openpete_scene_tri *p = &s_overlay_prev[t];
            /* Same sprite? A rigid screen translation (the slide) keeps the texture
             * region, so category + textured + the first vertex's UV identify it; a
             * glyph swap at the same slot differs in UV and snaps to curr. */
            if (p->category == tri.category && p->textured == tri.textured &&
                p->v[0].u == tri.v[0].u && p->v[0].v == tri.v[0].v) {
                for (int k = 0; k < 3; k++) {
                    tri.v[k].sx = (float)(p->v[k].sx + (tri.v[k].sx - p->v[k].sx) * oalpha);
                    tri.v[k].sy = (float)(p->v[k].sy + (tri.v[k].sy - p->v[k].sy) * oalpha);
                }
            }
        }
        tri.carry = 2;                             /* mark re-applied so the next capture skips it */
        s_scene[s_scene_n++] = tri;
    }
    s_scene_freeze = s_overlay_carry_freeze;
    s_scene_fade   = fade_lerp(s_fade_prev, s_fade_curr, oalpha);
}

/* The scene buffer the present should draw: the depth-frames-back snapshot when
 * a reference-compare delay is set (A/B-synced with the lagged reference), else
 * the live accumulator (vk-only, runs ahead). Falls back to the live frame
 * during warmup before enough history has accumulated. */
static const openpete_scene_tri* scene_present_buf(int* out_n, uint32_t* out_bg,
                                                   uint32_t* out_fade, uint32_t* out_freeze,
                                                   int* out_hud_under) {
    int depth = scene_delay_depth();
    if (depth > 0 && depth <= s_scene_hist_count && s_scene_hist_head >= 0) {
        int slot = (s_scene_hist_head - (depth - 1) + SCENE_HIST * 4) % SCENE_HIST;
        *out_n = s_scene_hist_n[slot]; *out_bg = s_scene_hist_bg[slot];
        *out_fade = s_scene_hist_fade[slot];
        if (out_freeze) *out_freeze = s_scene_hist_freeze[slot];
        if (out_hud_under) *out_hud_under = s_scene_hist_hud_under[slot];
        return s_scene_hist[slot];
    }
    *out_n = s_scene_n; *out_bg = s_scene_bg; *out_fade = s_scene_fade;
    if (out_freeze) *out_freeze = s_scene_freeze;
    if (out_hud_under) *out_hud_under = s_scene_hud_under;
    return s_scene;
}

/* True if the presented scene holds real WORLD or ACTOR geometry — i.e. a full
 * gameplay frame worth snapshotting into frozen_tex (the pause/inventory freeze
 * backdrop). Menu/transition frames (only HUD/OVERLAY tris, or empty) return false,
 * so the snapshot keeps the last gameplay frame to freeze.
 *
 * PLAYER (Spyro) alone does NOT qualify: at the pause-start transition the world
 * feeders cut out one present before the freeze FT4 is emitted, leaving a degenerate
 * frame with only Spyro's body (W=0, Ac=0, Pl>0, freeze not yet set). Counting that
 * as a world frame snapshots a near-empty image into frozen_tex — the "residual
 * framebuffer instead of the cyclorama" under the high-FPS spine's pause menu. A real
 * gameplay frame always carries terrain (WORLD) and/or mobys (ACTOR), so gate on
 * those and the transition frame is rejected, preserving the last full world. */
static bool scene_has_world(const openpete_scene_tri* scene, int n) {
    for (int t = 0; t < n; t++) {
        int c = scene[t].category;
        if (c == OPENPETE_SCENE_WORLD || c == OPENPETE_SCENE_ACTOR)
            return true;
    }
    return false;
}

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    bool          ab_mode;      /* true = A/B compare (2 panels + reference); false = vk-only */
    int           n_panels;     /* 2 in A/B, 1 in vk-only */
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */

    /* Reference panel: PsyCross's read-back frame (RGBA8). */
    rhi_pipeline *blit_pipe;
    rhi_texture  *ref_tex;
    int           ref_w, ref_h;
    unsigned char *readback;    /* CPU readback scratch, ref_w*ref_h*4 */
    size_t        readback_cap;

    /* The native-display panel samples the PSX VRAM mirror (R16_UINT) through
     * the ported PSX sampler. */
    rhi_pipeline *vram_pipe;
    rhi_texture  *vram_tex;
    uint16_t     *vram_mirror;  /* NR_VRAM_W*NR_VRAM_H */

    /* Native 3D render (Phase 2): gouraud triangles (quad.vert/quad.frag) for
     * the cyclorama/sky scene the game pushes in. Replaces the VRAM re-display
     * in panel 0 when a scene is present. */
    rhi_pipeline *gouraud_pipe;
    rhi_pipeline *gouraud_blend_pipe; /* 3b.2: semi-transparent UNTEXTURED gouraud (alpha, ABR mode 0) */
    rhi_pipeline *gouraud_sub_pipe;   /* 3b.2: subtractive UNTEXTURED gouraud (ABR mode 2, B-F) — Spyro shadow */
    rhi_pipeline *gouraud_add_pipe;   /* 3b.3: additive UNTEXTURED gouraud (ABR mode 1, B+F) — glow rings */
    rhi_pipeline *gouraud_noz_pipe;   /* opaque UNTEXTURED gouraud, depth-test ON / write OFF — portal-sky previews */
    rhi_pipeline *scene_tex_pipe;     /* 3a.3: opaque textured scene tris (VRAM sampler) */
    rhi_pipeline *scene_tex_blend_pipe; /* 3a.3: semi-transparent (ABE) textured tris (alpha, ABR mode 0) */
    rhi_pipeline *scene_tex_add_pipe; /* 3b.1: additive (ABR mode 1, B+F) textured tris — flame */
    rhi_pipeline *scene_tex_sub_pipe; /* 3b.2: subtractive (ABR mode 2, B-F) textured tris — moby shadows */
    rhi_pipeline *gouraud_hud_pipe;   /* HUD overlay: UNTEXTURED gouraud, depth-test OFF (on top) */
    rhi_pipeline *gouraud_hud_blend_pipe; /* HUD panel: UNTEXTURED alpha gouraud, depth-test OFF (menu dark panel under text) */
    rhi_pipeline *gouraud_hud_sub_pipe; /* HUD panel: UNTEXTURED subtractive gouraud, depth-test OFF (the ABR mode-2 shaded text box, B-F) */
    rhi_pipeline *scene_tex_hud_pipe; /* HUD overlay: textured (VRAM sampler), depth-test OFF (on top) */
    rhi_buffer   *scene_vb;     /* MAX_SCENE_TRIS*3 verts, rebuilt per frame */
    float         p0_cx, p0_hx, p0_hy; /* panel-0 fitted rect (clip space, cy=0) — the 4:3 CONTENT sub-rect */
    /* Target display aspect (W/H), from --aspect (0 → default 4:3). Drives the
     * Hor+ render aspect independently of the window: the scene renders at this
     * aspect and is letterboxed/pillarboxed into the window (like PsyCross), NOT
     * stretched/auto-widened to fill it. Set at init from opts->aspect_w_over_h. */
    float         aspect_wh;
    /* Widescreen (Hor+) widen factor = render_aspect / 4:3, i.e. how much MORE
     * horizontal world the --aspect view reveals vs 4:3 (1.0 = plain 4:3). Now
     * driven by --aspect (FIXED per run), not the window. The panel-0 content
     * mapping (scene_to_clip via p0_hx) is the centred 4:3 sub-rect; the reveal
     * extends to ±p0_hx*ws_factor (the --aspect rect), which fit_cell fits into
     * the window with bars. vk-only (ab/diff stay 4:3). See update_layout. */
    float         ws_factor;

    /* Active PSX display region (DISPENV `disp` rect, VRAM px), from the
     * PutDispEnv thunk. The full disp is 512x240 double-buffered (origin flips
     * per frame). */
    int           disp_x, disp_y, disp_w, disp_h;
    /* DRAWENV `clip` rect (VRAM px), from the PutDrawEnv thunk: the actually-
     * drawn region. The visible viewport is clip.h tall (224) within the
     * 240-tall disp; the 8px bands top+bottom are garbage overscan. Used to
     * crop the native-display vertically to match the bars-free reference. */
    int           clip_x, clip_y, clip_w, clip_h;

    /* Panel geometry: one VB of NR_PANELS*6 verts, rebuilt only when a panel's
     * content SIZE changes. The disp/clip origins flip every frame (double
     * buffer) but don't change geometry — they ride the per-frame material
     * (tpage), so layout keys on sizes only. */
    rhi_buffer   *panels_vb;
    int           layout_win_w, layout_win_h, layout_ref_w, layout_ref_h;
    int           layout_disp_w, layout_clip_h;

    /* Diff mode (--renderer diff): render the native scene into native_rtt (a
     * sampleable render target at the reference resolution), then a fullscreen
     * diff pass samples native_rtt (set 0) + ref_tex (set 1) and writes
     * |native - ref| into the offscreen. diff_quad_vb is the panel-0 rect with
     * 0..1 UVs that the diff pass draws. */
    bool          diff_mode;
    bool          stats_enabled;   /* --render-stats: per-frame diff readback + stats */
    rhi_pipeline *diff_pipe;
    rhi_texture  *native_rtt;
    int           rtt_w, rtt_h;
    rhi_buffer   *diff_quad_vb;

    /* SSAA (--ssaa N): the live vk scene renders into a supersampled render
     * target (ss_factor× the window per axis), then a fragment resolve pass
     * box-averages it in linear light into the 1× offscreen. ss_factor is the
     * EFFECTIVE factor — capped to the GPU's max image dimension (and reduced on
     * an allocation failure), so a large --ssaa degrades gracefully. ssaa==1 (or
     * ss_factor==1) disables it: the scene renders straight into the offscreen. */
    int           ssaa;             /* requested factor (opts->ssaa, >=1) */
    rhi_pipeline *resolve_pipe;     /* psx_sample.vert + resolve.frag (box+gamma) */
    rhi_texture  *ss_rtt;           /* supersampled scene target (ss_w × ss_h) */
    int           ss_w, ss_h, ss_factor;

    /* Freeze-frame green overlay (pause / inventory / flight-results backdrop).
     * native_rtt is the canonical native frame each present; frozen_tex snapshots
     * the last WORLD frame's native render (copied from native_rtt on non-menu
     * frames). When the game's freeze backdrop is active, the scene draws
     * frozen_tex desaturated + green-tinted (freeze_overlay_pipe) instead of the
     * FT4 that samples PsyCross's VRAM scratch — so the green overlay shows OUR
     * render and survives the end-state where PsyCross's VRAM is gone. */
    rhi_texture  *frozen_tex;
    int           frozen_w, frozen_h;
    bool          frozen_valid;       /* a world frame has been captured */
    rhi_pipeline *freeze_overlay_pipe;/* samples frozen_tex, luma + tint (set 0) */
    rhi_buffer   *fs_quad_vb;         /* full-target (-1..1) quad, UV 0..1 (RT copies) */
    rhi_buffer   *overlay_vb;         /* FPS overlay glyph quads (NDC), rebuilt per present */

    /* FPS-overlay 4× supersample: the glyphs render into ovl_ss (RGBA,
     * OVL_SS_FACTOR× the overlay region), then ovl_resolve_pipe box-downsamples +
     * straight-alpha-blends them onto the offscreen via the ovl_quad rect — so the
     * corner counter is anti-aliased regardless of window/render res. See
     * draw_fps_overlay. */
    rhi_texture  *ovl_ss;
    int           ovl_ss_w, ovl_ss_h;
    rhi_pipeline *ovl_resolve_pipe;   /* overlay_resolve.frag, box + straight alpha, RHI_BLEND_ALPHA */
    rhi_buffer   *ovl_quad_vb;        /* composite quad at the overlay's screen rect */
    int           ovl_quad_win_w, ovl_quad_win_h;
} s_nr;

/* When set, draw_native_scene maps the PSX screen to the FULL render target
 * (clip -1..1) instead of panel-0's window rect — used when rendering into
 * native_rtt for the diff pass (native_rtt IS the frame, no panel inset). */
static int s_render_full_extent = 0;
/* Horizontal divisor for the full-extent mapping. 1.0 = the 4:3 view fills the
 * target (native_rtt / diff). >1.0 = the Hor+ WIDESCREEN view fills the target:
 * the 4:3 content compresses to the centre (1/wf) and the revealed side world
 * fills out to ±1 — used to capture the freeze backdrop at the widescreen extent
 * so the pause/exit backdrop fills a widescreen window (not pillarboxed). */
static float s_render_freeze_wf = 1.0f;

/* Path-isolation mode (--render-paths, set by the game). When on, an empty
 * native scene presents the background instead of re-displaying the PSX VRAM
 * framebuffer — the VRAM is unfiltered, so falling back to it would defeat the
 * isolation. Generic core toggle; no-op until set. */
static bool s_isolation = false;
void openpete_native_set_isolation(bool on) { s_isolation = on; }

/* Diff colour-stats (--render-stats). */
#define DIFF_STATS_TOL 8           /* per-channel "match" tolerance (of 255) */
static struct {
    double   sum[3];               /* per-channel abs-diff sums over all counted px */
    uint64_t n;                    /* total counted (panel) px across frames */
    int      max[3];               /* per-channel max abs diff seen */
    uint64_t within;               /* px with max-channel diff <= TOL */
    uint64_t off;                  /* px with max-channel diff >  TOL (the differing ones) */
    unsigned frames;               /* frames sampled */
} s_diff_stats;
static unsigned char *s_stats_buf = NULL;   /* offscreen readback scratch */
static size_t         s_stats_cap = 0;

/* Read the just-presented diff offscreen back and fold its panel-rect pixels into
 * the running stats; log a throttled per-frame line. */
static void diff_stats_accumulate(void)
{
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap > s_stats_cap) {
        free(s_stats_buf);
        s_stats_buf = malloc(cap);
        s_stats_cap = s_stats_buf ? cap : 0;
    }
    if (!s_stats_buf) return;
    int w = 0, h = 0;
    if (!rhi_read_offscreen(s_nr.dev, s_stats_buf, cap, &w, &h) || w <= 0 || h <= 0) return;

    /* Panel-0 pixel rect (exclude the letterbox bars, which are a 0 diff). */
    int sx = (int)(((s_nr.p0_cx - s_nr.p0_hx) * 0.5f + 0.5f) * (float)w + 0.5f);
    int sw = (int)((s_nr.p0_hx) * (float)w + 0.5f);
    int sy = (int)((0.5f - 0.5f * s_nr.p0_hy) * (float)h + 0.5f);
    int sh = (int)((s_nr.p0_hy) * (float)h + 0.5f);
    if (sx < 0) sx = 0; if (sy < 0) sy = 0;
    if (sx + sw > w) sw = w - sx; if (sy + sh > h) sh = h - sy;
    if (sw <= 0 || sh <= 0) return;

    double fsum[3] = {0, 0, 0};
    uint64_t fn = 0, fwithin = 0, foff = 0;
    int fmax[3] = {0, 0, 0};
    for (int y = sy; y < sy + sh; y++) {
        const unsigned char* row = &s_stats_buf[(size_t)y * w * 4u];
        for (int x = sx; x < sx + sw; x++) {
            const unsigned char* px = &row[(size_t)x * 4u];
            int r = px[0], g = px[1], b = px[2];
            fsum[0] += r; fsum[1] += g; fsum[2] += b;
            if (r > fmax[0]) fmax[0] = r;
            if (g > fmax[1]) fmax[1] = g;
            if (b > fmax[2]) fmax[2] = b;
            int mc = r; if (g > mc) mc = g; if (b > mc) mc = b;
            if (mc > DIFF_STATS_TOL) foff++; else fwithin++;
            fn++;
        }
    }
    if (fn == 0) return;

    for (int c = 0; c < 3; c++) {
        s_diff_stats.sum[c] += fsum[c];
        if (fmax[c] > s_diff_stats.max[c]) s_diff_stats.max[c] = fmax[c];
    }
    s_diff_stats.n += fn; s_diff_stats.within += fwithin; s_diff_stats.off += foff;
    s_diff_stats.frames++;

    if ((s_diff_stats.frames % 60u) == 1u) {  /* throttled per-frame line */
        OPL_INFO_F("native", "diff-stats: mean=(%.1f,%.1f,%.1f) max=(%d,%d,%d) "
                   "within%d=%.1f%% diff-px=%llu/%llu",
                   fsum[0]/fn, fsum[1]/fn, fsum[2]/fn, fmax[0], fmax[1], fmax[2],
                   DIFF_STATS_TOL, 100.0 * (double)fwithin / (double)fn,
                   (unsigned long long)foff, (unsigned long long)fn);
    }
}

/* Fill a panel quad: clip-space rect centered at (cx,cy) with half-extents
 * (hx,hy); v0 is the UV-v at the TOP edge, v1 at the BOTTOM. (VRAM is uploaded
 * top-down so it uses v0<v1; the GL readback is bottom-up so the reference
 * uses v0>v1.) rgba is unused by the textured shaders. */
static void build_panel(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                        float u0, float v0, float u1, float v1)
{
    const rhi_vertex tl = { cx - hx, cy + hy, 0.0f, u0, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex tr = { cx + hx, cy + hy, 0.0f, u1, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex br = { cx + hx, cy - hy, 0.0f, u1, v1, 0xFFFFFFFFu, 0 };
    const rhi_vertex bl = { cx - hx, cy - hy, 0.0f, u0, v1, 0xFFFFFFFFu, 0 };
    out[0] = tl; out[1] = tr; out[2] = br;
    out[3] = tl; out[4] = br; out[5] = bl;
}

/* Aspect-fit content (content_w x content_h px) inside a clip-space cell
 * centered at (cx,cy) with clip half-extents (chx,chy), returning the fitted
 * half-extents (letterbox/pillarbox; the clear fills the bars). */
static void fit_cell(float cx, float cy, float chx, float chy,
                     int content_w, int content_h, int win_w, int win_h,
                     float *out_hx, float *out_hy)
{
    (void)cx; (void)cy;
    float cell_w = chx * (float)win_w;  /* cell size in px (full clip = win px) */
    float cell_h = chy * (float)win_h;
    float s = cell_w / (float)content_w;
    float sy = cell_h / (float)content_h;
    if (sy < s) s = sy;
    *out_hx = ((float)content_w * s) / (float)win_w;
    *out_hy = ((float)content_h * s) / (float)win_h;
}

int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h)
{
    if (!opts || opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        return 0; /* PsyCross owns the display — native window inert */

    if (opts->headless) {
        OPL_WARN_F("native", "--renderer vk|ab|diff ignored under --headless (no display)");
        return 0;
    }

    /* A/B compare mode shows the PsyCross reference in a second panel and delays
     * the native scene by one frame to align with that lagged reference. vk and
     * diff are single full-window panels with no reference panel and no scene
     * delay. (diff's live colour-diff render + diff screenshot land in a later
     * step; for now diff mirrors vk so the flag is usable.) */
    bool ab_mode = (opts->renderer == OPENPETE_RENDERER_AB);
    bool diff_mode = (opts->renderer == OPENPETE_RENDERER_DIFF);
    s_nr.ab_mode  = ab_mode;
    s_nr.diff_mode = diff_mode;
    /* Render aspect from --aspect (W/H); 0 → default 4:3. Drives the Hor+ amount
     * + letterbox/pillarbox (vk-only; ab/diff stay 4:3 vs the reference). */
    s_nr.aspect_wh = (opts->aspect_w_over_h > 0.0f) ? opts->aspect_w_over_h : 0.0f;
    s_nr.stats_enabled = diff_mode && opts->render_stats;  /* per-frame diff readback */
    s_nr.n_panels = ab_mode ? 2 : 1;   /* vk/diff are single full-window panels */
    /* Delay the native scene one frame whenever we compare against the lagged
     * PsyCross reference — A/B (panel sync) AND diff (else moving objects ghost
     * across native frame N vs reference frame N-1). vk-only has no reference. */
    s_scene_ref_compare = (ab_mode || diff_mode) ? 1 : 0;

    /* SDL_INIT_VIDEO is already up (PsyX_Initialise). Create a SECOND window
     * for the Vulkan surface: a single SDL window can't carry both PsyCross's
     * GL context and our Vulkan surface, so the native renderer owns its own.
     * PsyCross's GL window is started hidden (see run.c), so this Vulkan
     * window is the single visible one. Match the game's resolved window size
     * so it isn't surprisingly larger than the PsyCross-only window. */
    int w = (win_w > 0) ? win_w : NR_DEFAULT_W;
    int h = (win_h > 0) ? win_h : NR_DEFAULT_H;

    /* A/B shows two panels side-by-side, so widen the window 2x by default —
     * each panel then gets the game's full width instead of being squeezed into
     * half. vk/diff are single full-window panels and keep the resolved size.
     * An explicit --window is treated as the intended TOTAL window size and is
     * left alone (the user picked it deliberately). */
    if (ab_mode && opts->window_w <= 0)
        w *= 2;

    /* vk (the default display) wears the game's own name; the dev compare modes
     * keep a descriptive label so the A/B / diff windows are self-identifying. */
    const char *title = ab_mode   ? "openpete — native renderer (A/B)"
                      : diff_mode ? "openpete — native renderer (diff)"
                      : (opts->display_name && opts->display_name[0])
                                  ? opts->display_name
                                  : "Spyro the Dragon";
    SDL_Window *win = SDL_CreateWindow(title, w, h,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-native");
    if (!dev) {
        OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
        SDL_DestroyWindow(win);
        return 1;
    }
    /* On an unrecoverable device-loss (acquire/present), flag a clean shutdown
     * rather than spin failed frames forever (W0.6 RHI robustness). */
    extern void openpete_request_shutdown(void);
    rhi_set_device_lost_handler(openpete_request_shutdown);
    /* Live-window supersampling (--ssaa N, default 1=no-op): the ss_rtt + resolve
     * pass are set up lazily (ensure_ss_rtt) once the window size is known. */
    s_nr.ssaa = (opts->ssaa > 0) ? opts->ssaa : 1;

    rhi_pipeline_desc blit_desc = {
        .vert_spv = "psx_sample.vert.spv", /* passthrough pos + UV */
        .frag_spv = "blit.frag.spv",       /* sampler2D at set 0 */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    rhi_pipeline_desc vram_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "psx_sample.frag.spv", /* usampler2D + material push const */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Gouraud pipeline for the native 3D scene: vertex-coloured, untextured
     * (quad.vert/quad.frag from 1b.1) — exactly the cyclorama dome's shading. */
    rhi_pipeline_desc gouraud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,  /* 3a.3: sort actors near→far */
        .uses_vram = false, .push_const_size = 0,
    };
    /* Semi-transparent UNTEXTURED gouraud pipeline (3b.2): the Spyro silhouette
     * shadow (func_80059A48) is a POLY_G3 strip with ABE set (ABR mode 0,
     * (B+F)/2) — untextured, so it can't go through the textured-blend pipe.
     * RHI_BLEND_ALPHA = src*a + dst*(1-a); the feeder sets vertex alpha = 128
     * (~0.5) so the result is (B+F)/2, reproducing mode 0. depth_write_off: the
     * shadow tests against opaque geometry (a wall/ground occludes it) but
     * never occludes another transparent layer. Same quad.vert/quad.frag as the
     * opaque gouraud pipe — quad.frag emits the full vertex rgba incl. alpha. */
    rhi_pipeline_desc gouraud_blend_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Subtractive UNTEXTURED gouraud pipeline (3b.2, the Spyro silhouette
     * shadow): POLY_G3 with ABR mode 2 (dst = B - F). RHI_BLEND_SUBTRACT
     * reverse-subtracts the vertex colour from the framebuffer, darkening the
     * ground (the shadow grey is subtracted; black vertices leave it
     * unchanged). depth_write_off like the other transparent passes. */
    rhi_pipeline_desc gouraud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Additive UNTEXTURED gouraud pipeline (3b.3, the glow rings): POLY_G3 with
     * ABR mode 1 (dst = B + F). RHI_BLEND_ADD = src ONE + dst ONE. The glow is a
     * gouraud fan, bright centre colour fading to black ring vertices, so the
     * additive accumulation peaks at the centre and vanishes at the edge — the
     * classic halo. depth_write_off like the other transparent passes. */
    rhi_pipeline_desc gouraud_add_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Opaque UNTEXTURED gouraud, depth-test ON but depth-WRITE OFF: the portal-sky
     * previews (portal_native.c, OPENPETE_SCENE_PORTAL_SKY). They are a far
     * background seen through the arch window — the arch stone's depth (written by
     * the WORLD group, drawn first) must occlude them, but they must never write
     * depth, so the actors/player drawn afterward are never per-pixel cut by the
     * portal sky as Spyro flies into/through the portal. Same quad.vert/quad.frag
     * as the opaque pipe; only the depth-write differs. */
    rhi_pipeline_desc gouraud_noz_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .depth_write_off = true,
        .uses_vram = false, .push_const_size = 0,
    };
    /* Textured scene pipeline (3a.3): samples the PSX VRAM mirror via the ported
     * decoder + per-draw material, modulates by the vertex colour (tex*col/128).
     * Reuses psx_sample.vert (clip-space passthrough + uv + colour). */
    rhi_pipeline_desc scene_tex_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Semi-transparent (ABE) textured pipeline: alpha blend, reproducing PSX ABR
     * mode 0 ((B+F)/2) with per-texel STP gating via psx_scene.frag's STP alpha.
     * depth_write_off: transparent tris Z-TEST against opaque but do NOT write
     * depth, so they never occlude each other (correct ordering still needs the
     * back-to-front sort, 3c; this is the prerequisite half). */
    rhi_pipeline_desc scene_tex_blend_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Additive (ABR mode 1, dst = B + F) textured pipeline (3b.1): the first
     * additive consumer is Spyro's flame (func_80058D64 / r_flame.s), a textured
     * gouraud quad ribbon whose tpage selects ABR=1. RHI_BLEND_ADD = src ONE +
     * dst ONE; depth_write_off so the glow tests against opaque geometry (a wall
     * occludes the flame) but additive layers never occlude each other. The
     * grey-ramp vertex colour modulates the flame sprite (tex*col/128) and the
     * result accumulates into the framebuffer — bright core, never darkens. */
    rhi_pipeline_desc scene_tex_add_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_ADD, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Subtractive (ABR mode 2, dst = B - F) textured pipeline (3b.2): moby
     * shadows (func_80059F8C), a textured FT3 fan whose tpage selects ABR=2.
     * RHI_BLEND_SUBTRACT reverse-subtracts the modulated shadow sprite from the
     * framebuffer (tex*col/128 darkens the ground), depth_write_off so the
     * shadow tests against opaque geometry but never occludes another layer. */
    rhi_pipeline_desc scene_tex_sub_desc = {
        .vert_spv = "psx_scene.vert.spv",   /* perspective-correct UV (near-camera) */
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = true, .depth_write_off = true,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* HUD overlay pipelines (depth_test = false): HUD/UI mobys are a 2D overlay
     * drawn LAST, ignoring the depth buffer so they sit on top of all world/actor
     * geometry regardless of their world-space view-Z (the PSX draws HUD in a
     * separate near-OT pass). Painter's order = submission order. One untextured
     * (gouraud) + one textured (VRAM sampler), mirroring the opaque pair. */
    rhi_pipeline_desc gouraud_hud_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    /* HUD panel pipeline: untextured alpha gouraud, depth-test OFF — the dim
     * semi-transparent dark panel behind the pause/inventory menu text (a 0x2A F4,
     * ABR mode 0 = (B+F)/2). Like gouraud_hud_pipe but alpha-blended; the vertex
     * alpha (128) gives (B+F)/2. Drawn at the start of the HUD layer so the text
     * reads on top. quad.frag emits the full vertex rgba incl. alpha. */
    rhi_pipeline_desc gouraud_hud_blend_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    /* HUD panel SUBTRACTIVE pipeline: untextured subtractive gouraud, depth-test OFF.
     * The in-game "shaded box" behind HUD text (func_8001860C) and the pause/inventory
     * menu panel (func_8001A40C) both emit their 0x2A POLY_F4 under
     * SetDrawMode(getTPage(0, 2, ..)) = ABR mode 2 (B-F), which DARKENS the framebuffer
     * by the box colour. (B+F)/2 alpha would instead wash it toward grey (a too-bright
     * box) — the bug this fixes. RHI_BLEND_SUBTRACT reverse-subtracts the vertex colour
     * (alpha ignored), exactly the PSX. The box's real ABR is read per-prim from its
     * DR_MODE in ui_sprite_native, so the HUD-panel draw picks alpha vs subtractive. */
    rhi_pipeline_desc gouraud_hud_sub_desc = {
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false,
        .uses_vram = false, .push_const_size = 0,
    };
    rhi_pipeline_desc scene_tex_hud_desc = {
        .vert_spv = "psx_scene.vert.spv",
        .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    /* Diff pipeline (--renderer diff): samples two textures — native render at
     * set 0, reference at set 1 — and outputs |native - ref| (diff.frag). No
     * depth, no blend. Built unconditionally so --screenshot-on-frame can dump
     * a -diff.png regardless of the live --renderer mode. */
    rhi_pipeline_desc diff_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "diff.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .uses_vram2 = true, .push_const_size = 0,
    };
    /* Freeze-frame overlay pipeline (pause / inventory / flight-results backdrop):
     * samples frozen_tex (set 0) — the last world frame's native render — and
     * outputs it desaturated + green-tinted (freeze_overlay.frag; tint = vertex
     * colour). No depth (it's the menu backdrop), no blend (opaque, like the PSX
     * grayscale-freeze FT4 it replaces). */
    rhi_pipeline_desc freeze_overlay_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "freeze_overlay.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    /* SSAA resolve pipeline (--ssaa N): samples the supersampled scene RT (set 0)
     * and box-averages the N×N block in linear light into the 1× offscreen
     * (resolve.frag). No depth, no blend; the ssaa factor rides a push constant. */
    rhi_pipeline_desc resolve_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "resolve.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(int),
    };
    /* FPS-overlay supersample resolve: like resolve_desc but alpha-aware
     * (overlay_resolve.frag) and alpha-blended, to composite the downsampled
     * glyphs over the scene. Optional — draw_fps_overlay falls back to a direct
     * 1× draw if this (or ovl_quad_vb) didn't come up. */
    rhi_pipeline_desc ovl_resolve_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "overlay_resolve.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(int),
    };
    rhi_pipeline *blit_pipe    = rhi_create_pipeline(dev, &blit_desc);
    rhi_pipeline *vram_pipe    = rhi_create_pipeline(dev, &vram_desc);
    rhi_pipeline *gouraud_pipe = rhi_create_pipeline(dev, &gouraud_desc);
    rhi_pipeline *gouraud_blend_pipe = rhi_create_pipeline(dev, &gouraud_blend_desc);
    rhi_pipeline *gouraud_sub_pipe = rhi_create_pipeline(dev, &gouraud_sub_desc);
    rhi_pipeline *gouraud_add_pipe = rhi_create_pipeline(dev, &gouraud_add_desc);
    rhi_pipeline *gouraud_noz_pipe = rhi_create_pipeline(dev, &gouraud_noz_desc);
    rhi_pipeline *scene_tex_pipe = rhi_create_pipeline(dev, &scene_tex_desc);
    rhi_pipeline *scene_tex_blend_pipe = rhi_create_pipeline(dev, &scene_tex_blend_desc);
    rhi_pipeline *scene_tex_add_pipe = rhi_create_pipeline(dev, &scene_tex_add_desc);
    rhi_pipeline *scene_tex_sub_pipe = rhi_create_pipeline(dev, &scene_tex_sub_desc);
    rhi_pipeline *gouraud_hud_pipe = rhi_create_pipeline(dev, &gouraud_hud_desc);
    rhi_pipeline *gouraud_hud_blend_pipe = rhi_create_pipeline(dev, &gouraud_hud_blend_desc);
    rhi_pipeline *gouraud_hud_sub_pipe = rhi_create_pipeline(dev, &gouraud_hud_sub_desc);
    rhi_pipeline *scene_tex_hud_pipe = rhi_create_pipeline(dev, &scene_tex_hud_desc);
    rhi_pipeline *diff_pipe = rhi_create_pipeline(dev, &diff_desc);
    rhi_pipeline *freeze_overlay_pipe = rhi_create_pipeline(dev, &freeze_overlay_desc);
    rhi_pipeline *resolve_pipe = rhi_create_pipeline(dev, &resolve_desc);
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    rhi_buffer   *scene_vb  = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * SCENE_VB_VERTS);
    rhi_buffer   *diff_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *fs_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_buffer   *overlay_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * OVL_MAX_VERTS);
    /* Optional overlay-supersample resources (graceful fallback if NULL). */
    rhi_pipeline *ovl_resolve_pipe = rhi_create_pipeline(dev, &ovl_resolve_desc);
    rhi_buffer   *ovl_quad_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_H);
    uint16_t     *mirror    = calloc((size_t)NR_VRAM_W * NR_VRAM_H, sizeof(uint16_t));
    if (!blit_pipe || !vram_pipe || !gouraud_pipe || !gouraud_blend_pipe || !gouraud_sub_pipe || !gouraud_add_pipe || !scene_tex_pipe || !scene_tex_blend_pipe || !scene_tex_add_pipe || !scene_tex_sub_pipe || !gouraud_hud_pipe || !gouraud_hud_blend_pipe || !gouraud_hud_sub_pipe || !scene_tex_hud_pipe || !panels_vb || !scene_vb || !vram_tex || !mirror ||
        !diff_pipe || !diff_quad_vb || !freeze_overlay_pipe || !resolve_pipe || !fs_quad_vb || !overlay_vb) {
        OPL_ERROR_F("native", "RHI setup failed; continuing without native renderer");
        free(mirror);
        if (resolve_pipe) rhi_destroy_pipeline(dev, resolve_pipe);
        if (gouraud_hud_sub_pipe) rhi_destroy_pipeline(dev, gouraud_hud_sub_pipe);
        if (gouraud_hud_blend_pipe) rhi_destroy_pipeline(dev, gouraud_hud_blend_pipe);
        if (freeze_overlay_pipe) rhi_destroy_pipeline(dev, freeze_overlay_pipe);
        if (overlay_vb)    rhi_destroy_buffer(dev, overlay_vb);
        if (fs_quad_vb)    rhi_destroy_buffer(dev, fs_quad_vb);
        if (vram_tex)      rhi_destroy_texture(dev, vram_tex);
        if (diff_quad_vb)  rhi_destroy_buffer(dev, diff_quad_vb);
        if (scene_vb)      rhi_destroy_buffer(dev, scene_vb);
        if (panels_vb)     rhi_destroy_buffer(dev, panels_vb);
        if (diff_pipe)     rhi_destroy_pipeline(dev, diff_pipe);
        if (scene_tex_hud_pipe) rhi_destroy_pipeline(dev, scene_tex_hud_pipe);
        if (gouraud_hud_pipe)   rhi_destroy_pipeline(dev, gouraud_hud_pipe);
        if (scene_tex_sub_pipe) rhi_destroy_pipeline(dev, scene_tex_sub_pipe);
        if (scene_tex_add_pipe) rhi_destroy_pipeline(dev, scene_tex_add_pipe);
        if (scene_tex_blend_pipe) rhi_destroy_pipeline(dev, scene_tex_blend_pipe);
        if (scene_tex_pipe) rhi_destroy_pipeline(dev, scene_tex_pipe);
        if (gouraud_noz_pipe) rhi_destroy_pipeline(dev, gouraud_noz_pipe);
        if (gouraud_add_pipe) rhi_destroy_pipeline(dev, gouraud_add_pipe);
        if (gouraud_sub_pipe) rhi_destroy_pipeline(dev, gouraud_sub_pipe);
        if (gouraud_blend_pipe) rhi_destroy_pipeline(dev, gouraud_blend_pipe);
        if (gouraud_pipe)  rhi_destroy_pipeline(dev, gouraud_pipe);
        if (vram_pipe)     rhi_destroy_pipeline(dev, vram_pipe);
        if (blit_pipe)     rhi_destroy_pipeline(dev, blit_pipe);
        rhi_destroy(dev);
        SDL_DestroyWindow(win);
        return 1;
    }

    s_nr.enabled     = true;
    s_nr.window      = win;
    s_nr.dev         = dev;
    s_nr.win_w       = w;
    s_nr.win_h       = h;
    s_nr.blit_pipe    = blit_pipe;
    s_nr.vram_pipe    = vram_pipe;
    s_nr.gouraud_pipe = gouraud_pipe;
    s_nr.gouraud_blend_pipe = gouraud_blend_pipe;
    s_nr.gouraud_sub_pipe = gouraud_sub_pipe;
    s_nr.gouraud_add_pipe = gouraud_add_pipe;
    s_nr.gouraud_noz_pipe = gouraud_noz_pipe;
    s_nr.scene_tex_pipe = scene_tex_pipe;
    s_nr.scene_tex_blend_pipe = scene_tex_blend_pipe;
    s_nr.scene_tex_add_pipe = scene_tex_add_pipe;
    s_nr.scene_tex_sub_pipe = scene_tex_sub_pipe;
    s_nr.gouraud_hud_pipe = gouraud_hud_pipe;
    s_nr.gouraud_hud_blend_pipe = gouraud_hud_blend_pipe;
    s_nr.gouraud_hud_sub_pipe = gouraud_hud_sub_pipe;
    s_nr.scene_tex_hud_pipe = scene_tex_hud_pipe;
    s_nr.panels_vb    = panels_vb;
    s_nr.scene_vb     = scene_vb;
    s_nr.diff_pipe    = diff_pipe;
    s_nr.diff_quad_vb = diff_quad_vb;
    s_nr.freeze_overlay_pipe = freeze_overlay_pipe;
    s_nr.resolve_pipe = resolve_pipe;
    s_nr.fs_quad_vb   = fs_quad_vb;
    s_nr.overlay_vb   = overlay_vb;
    s_nr.ovl_resolve_pipe = ovl_resolve_pipe;   /* may be NULL → 1× fallback */
    s_nr.ovl_quad_vb  = ovl_quad_vb;            /* may be NULL → 1× fallback */
    s_nr.vram_tex     = vram_tex;
    s_nr.vram_mirror  = mirror;

    /* Static full-target quad (clip -1..1, UV 0..1 top-down) for render-target
     * copies — currently the native_rtt -> frozen_tex freeze-snapshot blit. */
    {
        rhi_vertex fsq[6];
        build_panel(fsq, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(fs_quad_vb, fsq, sizeof(fsq));
    }
    OPL_INFO_F("native", "Vulkan %s window up (%dx%d, device='%s'); PsyCross hidden",
               ab_mode ? "A/B" : diff_mode ? "diff" : "vk",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

/* Toggle the native window between windowed and borderless-desktop fullscreen.
 * Bound to Alt+Enter and double-click in run.c (moved out of PsyCross, which
 * only ever drove its now-hidden GL window). SDL3 decides the fullscreen *kind*
 * by the window's fullscreen mode: NULL = borderless desktop (no video-mode
 * switch), which is what we want — so force it NULL before entering. The present
 * path re-reads the window size every frame (SDL_GetWindowSizeInPixels) and the
 * RHI recreates the swapchain on resize, so layout + swapchain follow with no
 * extra work here. No-op until the native window is up. */
void openpete_native_toggle_fullscreen(void)
{
    if (!s_nr.enabled || !s_nr.window) return;
    bool fs = (SDL_GetWindowFlags(s_nr.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!fs)
        SDL_SetWindowFullscreenMode(s_nr.window, NULL);  /* borderless desktop */
    SDL_SetWindowFullscreen(s_nr.window, !fs);
    OPL_INFO_F("native", "fullscreen %s", !fs ? "on (borderless desktop)" : "off");
}

void openpete_native_set_display_rect(int x, int y, int w, int h)
{
    /* Log size changes only; the disp origin flips every frame (double buffer). */
    if (s_nr.enabled && (w != s_nr.disp_w || h != s_nr.disp_h))
        OPL_INFO_F("native", "disp %dx%d @ (%d,%d) [VRAM px]", w, h, x, y);
    s_nr.disp_x = x; s_nr.disp_y = y; s_nr.disp_w = w; s_nr.disp_h = h;
}

void openpete_native_set_draw_clip(int x, int y, int w, int h)
{
    /* Log on size change; the clip origin flips per frame like disp. The
     * overscan band (per side) is derived from the sizes, not clip.y. */
    if (s_nr.enabled && (w != s_nr.clip_w || h != s_nr.clip_h))
        OPL_INFO_F("native", "draw clip %dx%d @ (%d,%d) [VRAM px] (overscan band=%d/side)",
                   w, h, x, y, s_nr.disp_h > 0 ? (s_nr.disp_h - h) / 2 : 0);
    s_nr.clip_x = x; s_nr.clip_y = y; s_nr.clip_w = w; s_nr.clip_h = h;
}

/* Mirror PsyCross's whole VRAM (1024x512) into the native R16_UINT texture.
 * Full snapshot for now; 1c's dirty-rect sub-rect path (driven by hooking the
 * image ops) is a later efficiency step. Always available (reads PsyCross's
 * CPU mirror), even before the first scene. */
static void sync_vram_texture(void)
{
    GR_ReadVRAM(s_nr.vram_mirror, 0, 0, NR_VRAM_W, NR_VRAM_H);
    rhi_update_vram(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror);
}

/* Pull PsyCross's finished frame from its present FBO into s_nr.ref_tex.
 * Returns true if a reference frame is ready to draw this present. */
static bool sync_reference_texture(void)
{
    int pw = 0, ph = 0;
    if (!GR_GetPresentSceneSize(&pw, &ph) || pw <= 0 || ph <= 0)
        return false; /* PsyCross hasn't produced a scene yet */

    if (pw != s_nr.ref_w || ph != s_nr.ref_h) {
        if (s_nr.ref_tex) {
            rhi_wait_idle(s_nr.dev);
            rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
            s_nr.ref_tex = NULL;
        }
        s_nr.ref_tex = rhi_create_rgba8(s_nr.dev, (uint32_t)pw, (uint32_t)ph);
        s_nr.ref_w = pw;
        s_nr.ref_h = ph;

        size_t need = (size_t)pw * (size_t)ph * 4u;
        if (need > s_nr.readback_cap) {
            free(s_nr.readback);
            s_nr.readback = malloc(need);
            s_nr.readback_cap = s_nr.readback ? need : 0;
        }
        OPL_INFO_F("native", "reference FBO size %dx%d", pw, ph);
    }

    if (!s_nr.ref_tex || !s_nr.readback)
        return false;
    if (!GR_ReadPresentSceneRGBA(s_nr.readback, pw, ph, NR_GL_RGBA))
        return false;

    rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
    return true;
}

/* (Re)create native_rtt (the canonical native frame each present) AND frozen_tex
 * (the freeze-backdrop snapshot) at the reference resolution. native_rtt is the
 * frame the diff pass compares and the live vk/ab panel blits from; frozen_tex
 * holds the last world frame for the pause/inventory/flight green overlay. No-op
 * until the reference size is known; recreates both on size change. Must run
 * OUTSIDE a frame (creates GPU resources / waits idle). `want` gates allocation:
 * always true once the native path is live (the canonical-frame model needs the
 * RTTs in every mode); the screenshot-all path also passes true. */
static void ensure_native_rtt(bool want)
{
    if (!want || s_nr.ref_w <= 0 || s_nr.ref_h <= 0) return;
    if (s_nr.native_rtt && s_nr.rtt_w == s_nr.ref_w && s_nr.rtt_h == s_nr.ref_h) return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.native_rtt) { rhi_destroy_texture(s_nr.dev, s_nr.native_rtt); s_nr.native_rtt = NULL; }
    if (s_nr.frozen_tex) { rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex); s_nr.frozen_tex = NULL; }
    s_nr.frozen_valid = false;
    s_nr.native_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)s_nr.ref_w, (uint32_t)s_nr.ref_h);
    s_nr.frozen_tex = rhi_create_render_target(s_nr.dev, (uint32_t)s_nr.ref_w, (uint32_t)s_nr.ref_h);
    s_nr.rtt_w = s_nr.ref_w; s_nr.rtt_h = s_nr.ref_h;
    s_nr.frozen_w = s_nr.ref_w; s_nr.frozen_h = s_nr.ref_h;
    if (s_nr.native_rtt)
        OPL_INFO_F("native", "native render target %dx%d (+ freeze snapshot)", s_nr.ref_w, s_nr.ref_h);
}

/* (Re)create ss_rtt — the SSAA supersampled scene target — at the EFFECTIVE
 * factor × the window. The requested --ssaa factor is capped to the GPU's max
 * 2D image dimension and stepped down on an allocation failure (to 1 = SSAA
 * off). No-op when --ssaa is 1, the window size is unknown, or it already
 * matches. Runs OUTSIDE a frame (creates GPU resources / waits idle). */
static void ensure_ss_rtt(void)
{
    int req = (s_nr.ssaa > 1) ? s_nr.ssaa : 1;
    int win_w = s_nr.win_w, win_h = s_nr.win_h;
    if (req <= 1 || win_w <= 0 || win_h <= 0) {              /* SSAA off → drop any RT */
        if (s_nr.ss_rtt) {
            rhi_wait_idle(s_nr.dev);
            rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt); s_nr.ss_rtt = NULL;
            s_nr.ss_w = s_nr.ss_h = 0; s_nr.ss_factor = 1;
        }
        return;
    }
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    int eff = req;
    while (eff > 1 && ((uint32_t)(win_w * eff) > maxdim || (uint32_t)(win_h * eff) > maxdim)) eff--;
    int want_w = win_w * eff, want_h = win_h * eff;
    if (s_nr.ss_rtt && s_nr.ss_w == want_w && s_nr.ss_h == want_h) return;  /* unchanged */
    rhi_wait_idle(s_nr.dev);
    if (s_nr.ss_rtt) { rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt); s_nr.ss_rtt = NULL; }
    while (eff > 1) {  /* allocation fallback: step down until it fits VRAM */
        s_nr.ss_rtt = rhi_create_render_target(s_nr.dev, (uint32_t)(win_w * eff), (uint32_t)(win_h * eff));
        if (s_nr.ss_rtt) break;
        eff--;
    }
    if (s_nr.ss_rtt) {
        rhi_texture_extent(s_nr.ss_rtt, &s_nr.ss_w, &s_nr.ss_h);
        s_nr.ss_factor = eff;
        OPL_INFO_F("native", "SSAA %dx%s -> scene RT %dx%d (window %dx%d, GPU max dim %u)",
                   req, (eff != req) ? " (capped)" : "", s_nr.ss_w, s_nr.ss_h, win_w, win_h, maxdim);
    } else {
        s_nr.ss_w = s_nr.ss_h = 0; s_nr.ss_factor = 1;   /* couldn't allocate → SSAA off */
    }
}

/* Rebuild the panel row (VRAM | reference), each aspect-fit into its cell, so
 * resizing never stretches. Only runs when the window or reference size
 * changes (the VRAM panel is a fixed 1024x512). */
static void update_layout(void)
{
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return;
    if (s_nr.win_w == s_nr.layout_win_w && s_nr.win_h == s_nr.layout_win_h &&
        s_nr.ref_w == s_nr.layout_ref_w && s_nr.ref_h == s_nr.layout_ref_h &&
        s_nr.disp_w == s_nr.layout_disp_w && s_nr.clip_h == s_nr.layout_clip_h)
        return;

    /* n_panels cells across the window; cell i centered at cx_i, half-width
     * 1/n_panels in clip space, full height. vk-only mode has 1 (full-window)
     * panel; A/B has 2 (native | reference). */
    const int   np  = s_nr.n_panels > 0 ? s_nr.n_panels : 2;
    const float chx = 1.0f / (float)np;
    rhi_vertex verts[NR_PANELS * 6] = {0};

    /* Panel 0 — native render of the PSX display region, sampled from the VRAM
     * texture and cropped to the visible viewport: full disp width, but only
     * clip.h rows (the 8px overscan bands top+bottom are dropped). Fitted at
     * the REFERENCE aspect so it lines up 1:1 with the reference panel (the
     * 512-wide framebuffer is non-square; both panels show it at the same
     * display shape). Falls back to the whole VRAM until DISPENV/DRAWENV are
     * set. Top-down (v0=0 top .. v1=H bottom); UVs in texel units relative to
     * the per-frame source tpage. */
    {
        int have_clip = (s_nr.disp_w > 0 && s_nr.clip_h > 0);
        int uv_w  = have_clip ? s_nr.disp_w : NR_VRAM_W;  /* sampled region size */
        int uv_h  = have_clip ? s_nr.clip_h : NR_VRAM_H;
        int fit_w = have_clip ? s_nr.ref_w  : NR_VRAM_W;  /* 4:3 display shape */
        int fit_h = have_clip ? s_nr.ref_h  : NR_VRAM_H;

        /* Hor+ widen factor from --aspect (FIXED per run): how much wider the
         * --aspect view is than 4:3. vk-only; ab/diff stay 4:3 vs the reference.
         * Clamp ≥1 — Hor+ only ADDS world; a narrower --aspect just renders 4:3
         * and the window letterboxes. */
        float wf = 1.0f;
        if (!s_scene_ref_compare && s_nr.aspect_wh > 0.0f) {
            wf = s_nr.aspect_wh / (4.0f / 3.0f);
            if (wf < 1.0f) wf = 1.0f;
        }
        s_nr.ws_factor = wf;

        /* Fit the FULL --aspect display rect (4:3 content widened by wf) into the
         * window → letterbox/pillarbox bars where the window ≠ --aspect. The 4:3
         * CONTENT sub-rect (what scene_to_clip maps the canonical 512-wide screen
         * onto, unstretched) is 1/wf of that width and centred; the Hor+ reveal
         * fills the rest of the --aspect rect. (Replaces the old window-driven
         * auto-Hor+ that filled the whole window — now --aspect controls the render
         * shape and the window only adds bars, matching PsyCross.) */
        float cx = -1.0f + chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, (int)((float)fit_w * wf + 0.5f), fit_h,
                 s_nr.win_w, s_nr.win_h, &hx, &hy);
        s_nr.p0_cx = cx; s_nr.p0_hx = hx / wf; s_nr.p0_hy = hy; /* 4:3 sub-rect for the scene map */
        /* VRAM fallback panel (loading screens / OT-empty): the canonical 4:3
         * framebuffer, shown at the 4:3 sub-rect (unstretched, pillarboxed within
         * the --aspect area). */
        build_panel(&verts[0], cx, 0.0f, s_nr.p0_hx, hy, 0.0f, 0.0f, (float)uv_w, (float)uv_h);
    }
    /* Panel 1 — reference (RGBA8, bottom-up: v0=1 top .. v1=0 bottom), UVs
     * normalized (blit uses texture()). Only in A/B mode (np >= 2). */
    if (np >= 2) {
        float cx = -1.0f + 3.0f * chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, s_nr.ref_w, s_nr.ref_h, s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[6], cx, 0.0f, hx, hy, 0.0f, 1.0f, 1.0f, 0.0f);
    }

    /* The VB may still be read by an in-flight frame; layout changes are rare
     * (startup / resize), so fully sync before rewriting it. */
    rhi_wait_idle(s_nr.dev);
    rhi_update_buffer(s_nr.panels_vb, verts, sizeof(verts));
    /* Diff mode: the diff pass draws one quad over panel 0's rect, sampling
     * native_rtt (set 0) + ref_tex (set 1) with 0..1 UVs (top-down; diff.frag
     * V-flips the GL-bottom-up reference). */
    if (s_nr.diff_quad_vb) {
        rhi_vertex dq[6] = {0};
        build_panel(dq, s_nr.p0_cx, 0.0f, s_nr.p0_hx, s_nr.p0_hy, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(s_nr.diff_quad_vb, dq, sizeof(dq));
    }
    s_nr.layout_win_w = s_nr.win_w; s_nr.layout_win_h = s_nr.win_h;
    s_nr.layout_ref_w = s_nr.ref_w; s_nr.layout_ref_h = s_nr.ref_h;
    s_nr.layout_disp_w = s_nr.disp_w; s_nr.layout_clip_h = s_nr.clip_h;
}

/* Map a PSX screen-space point (x in [0,disp_w], y in [0,disp_h]) into panel
 * 0's fitted clip-space rect, cropping the symmetric overscan band so the
 * native scene lines up with the overscan-cropped reference. */
static void scene_to_clip(float sx, float sy, float *cx, float *cy)
{
    float disp_w = s_nr.disp_w > 0 ? (float)s_nr.disp_w : 512.0f;
    float disp_h = s_nr.disp_h > 0 ? (float)s_nr.disp_h : 240.0f;
    float clip_h = s_nr.clip_h > 0 ? (float)s_nr.clip_h : 224.0f;
    float band   = (disp_h - clip_h) * 0.5f;
    float nx = sx / disp_w;             /* 0..1 across the frame width */
    float ny = (sy - band) / clip_h;    /* 0..1 down the visible band  */
    if (s_render_full_extent) {
        /* The render target IS the frame: map to the full target, clip -1..1.
         * Divide x by s_render_freeze_wf so a widescreen freeze capture (wf>1)
         * compresses the 4:3 content to the centre and the Hor+ sides fill ±1
         * (wf==1 → the plain 4:3 full-extent for native_rtt / diff). */
        *cx = (-1.0f + nx * 2.0f) / s_render_freeze_wf;
        *cy =  1.0f - ny * 2.0f;        /* screen y down -> clip y up */
    } else {
        *cx = (s_nr.p0_cx - s_nr.p0_hx) + nx * (2.0f * s_nr.p0_hx);
        *cy = (0.0f + s_nr.p0_hy)       - ny * (2.0f * s_nr.p0_hy);
    }
}

/* Widescreen (Hor+) factor for the feeders' horizontal trivial rejects. 1.0
 * unless vk-only with a wider-than-4:3 window (set in update_layout). */
float openpete_native_widescreen_factor(void)
{
    return s_nr.ws_factor > 1.0f ? s_nr.ws_factor : 1.0f;
}
/* Visible PSX screen-X range under the active widen factor: [0,512] at 4:3,
 * widened symmetrically about the screen centre (256). */
void openpete_native_widescreen_x_bounds(float *xlo, float *xhi)
{
    float F = openpete_native_widescreen_factor();
    if (xlo) *xlo = 256.0f - 256.0f * F;
    if (xhi) *xhi = 256.0f + 256.0f * F;
}

/* The clip-space rect the full-frame cover quads (background fill / fade /
 * freeze backdrop) span. The --aspect RENDER rect (4:3 sub-rect widened by the
 * Hor+ factor) so the revealed side strips get a background behind the sky dome /
 * world — but NOT the letterbox/pillarbox bars beyond it (they stay black). The
 * whole target in full_extent (native_rtt) mode. */
static void cover_rect(int full_extent, float *l, float *r, float *t, float *b)
{
    float fhx = s_nr.p0_hx * s_nr.ws_factor;   /* --aspect rect half-width (= p0_hx at 4:3) */
    *l = full_extent ? -1.0f : s_nr.p0_cx - fhx;
    *r = full_extent ?  1.0f : s_nr.p0_cx + fhx;
    *t = full_extent ?  1.0f : s_nr.p0_hy;
    *b = full_extent ? -1.0f : -s_nr.p0_hy;
}

/* The scene background colour = what PsyCross actually clears the framebuffer to,
 * sampled from a top-left corner of the reference readback. This matches PsyCross
 * in EVERY state (levels, titlescreen, menus) and mode — unlike a gamestate
 * source like g_Cyclorama.m_BackgroundColor, which is the level sky colour but is
 * stale/wrong outside a level (e.g. the titlescreen, where the frame clears to a
 * different colour). Returns 0 (no bg) until a reference frame exists; the caller
 * then falls back to the scene-fed colour. rgba byte order R,G,B,A.
 *
 * ONLY a valid source in the A/B / diff HARNESS, where pixel-matching PsyCross is
 * the point. In vk-only (the shipping path) the reference readback is still
 * populated (sync_reference_texture runs ungated), but its top-left corner is
 * frequently CONTENT, not the clear — the cyclorama dome, the HUD (gem/life
 * counters), or overscan — and reusing PsyCross's per-frame output violates the
 * native renderer's Core Principle (derive from gamestate). So return 0 outside
 * compare mode, forcing the gamestate `scene_bg` (g_Cyclorama.m_BackgroundColor)
 * to be used instead. (Non-level vk states still want a gamestate draw-env clear
 * source — tracked as a follow-up, see the native-renderer plan.) */
static uint32_t bg_from_ref_corner(void)
{
    if (!s_scene_ref_compare) return 0;   /* vk-only: use gamestate scene_bg, not PsyCross */
    if (!s_nr.readback || s_nr.ref_w < 6 || s_nr.ref_h < 6) return 0;
    /* GL readback is bottom-up, so the TOP rows are at the end; inset a few px. */
    const unsigned char* c =
        &s_nr.readback[(((size_t)(s_nr.ref_h - 4) * (size_t)s_nr.ref_w) + 4) * 4u];
    return 0xFF000000u | (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16);
}

/* Build + draw the native 3D scene (cyclorama + actors). full_extent=false
 * draws into panel 0's window rect (the normal vk/ab path); full_extent=true
 * draws into the whole render target (used when rendering into native_rtt for
 * the diff pass, where the target IS the frame). Returns true if anything was
 * drawn. */
/* The flat 2D HUD layer (depth-test off): the menu dark panel (HUD_PANEL, alpha)
 * first, then the textured HUD (2D backdrops), then the untextured HUD text/glyphs.
 * Textured-before-untextured matches the PSX HUD-OT submission order (a 2D backdrop
 * is linked before the text/icon glyphs — e.g. the flight results green backdrop
 * UNDER the "CRASHED" text). Drawn either UNDER the player (gi==2, GS_LevelTransition
 * flyby so Spyro flies in front of the revealed text) or, by default, OVER the player
 * (after the whole scene) — see hud_under in draw_native_scene. The 3D HUD icons
 * (HUD3D) are a separate depth-isolated pass drawn after this either way. */
/* Textured-tri material batching (see the bucket build in render_scene). The
 * scene index of each textured block-tri, in VB order, so the draw side can read
 * a tri's material and coalesce adjacent same-material runs into one draw. */
static int s_txscene[MAX_SCENE_TRIS];
static const openpete_scene_tri* s_txsort_scene;   /* qsort context (set per call) */
static int tex_mat_cmp(const void* pa, const void* pb) {
    const openpete_scene_tri* a = &s_txsort_scene[*(const int*)pa];
    const openpete_scene_tri* b = &s_txsort_scene[*(const int*)pb];
    if (a->tpage_y != b->tpage_y) return a->tpage_y < b->tpage_y ? -1 : 1;
    if (a->tpage_x != b->tpage_x) return a->tpage_x < b->tpage_x ? -1 : 1;
    if (a->clut_y  != b->clut_y ) return a->clut_y  < b->clut_y  ? -1 : 1;
    if (a->clut_x  != b->clut_x ) return a->clut_x  < b->clut_x  ? -1 : 1;
    if (a->bpp     != b->bpp    ) return a->bpp     < b->bpp     ? -1 : 1;
    return 0;
}
/* Draw a bucket's textured vert range [first_vert, first_vert+vert_count),
 * coalescing adjacent same-material tris into one rhi_draw (one push-constant per
 * run, not per tri). Caller binds the pipeline + vram + vertex buffer. */
static void draw_tex_runs(int first_vert, int vert_count, int tx_start,
                          const openpete_scene_tri* scene) {
    int base = (first_vert - tx_start) / 3, ntri = vert_count / 3, i = 0;
    while (i < ntri) {
        const openpete_scene_tri* s0 = &scene[s_txscene[base + i]];
        const psx_material m = { s0->tpage_x, s0->tpage_y, s0->clut_x, s0->clut_y, s0->bpp };
        rhi_push_constants(s_nr.dev, &m, sizeof m);
        int run = 1;
        while (i + run < ntri) {
            const openpete_scene_tri* s1 = &scene[s_txscene[base + i + run]];
            if (s1->tpage_x != s0->tpage_x || s1->tpage_y != s0->tpage_y ||
                s1->clut_x != s0->clut_x || s1->clut_y != s0->clut_y || s1->bpp != s0->bpp) break;
            run++;
        }
        rhi_draw(s_nr.dev, (uint32_t)(run * 3), (uint32_t)(first_vert + i * 3));
        i += run;
    }
}

static void draw_hud_flat_layer(const openpete_scene_tri* scene, int scene_n,
                                const int* ut_first, const int* ut_count, int tx_start,
                                int hud_tex_first, int hud_tex_count)
{
    if (ut_count[8] > 0) {                                          /* HUD_PANEL (semi-transparent 2D box) */
        /* Each HUD-panel box carries its real PSX ABR mode in its blend field (read
         * from the box's DR_MODE in ui_sprite_native): mode 2 (B-F) -> subtractive
         * (blend 3), mode 0 ((B+F)/2) -> alpha (blend 1). Draw each box with the pipe
         * matching its blend. The slot-8 VB range is in scene[] submission order, so
         * walking scene[] for HUD_PANEL tris tracks the vertex offset in lockstep. */
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        int voff = ut_first[8], vend = ut_first[8] + ut_count[8];
        for (int t = 0; t < scene_n && voff + 3 <= vend; t++) {
            if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL) continue;
            rhi_pipeline *pipe = (scene[t].blend == 3) ? s_nr.gouraud_hud_sub_pipe
                                                       : s_nr.gouraud_hud_blend_pipe;
            if (pipe) { rhi_bind_pipeline(s_nr.dev, pipe); rhi_draw(s_nr.dev, 3, (uint32_t)voff); }
            voff += 3;
        }
    }
    if (hud_tex_count > 0 && s_nr.vram_tex && s_nr.scene_tex_hud_pipe) {  /* textured HUD (2D backdrops) */
        rhi_bind_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        draw_tex_runs(hud_tex_first, hud_tex_count, tx_start, scene);  /* bucket 6, submission order + run-coalesce */
    }
    if (s_nr.gouraud_hud_pipe && ut_count[6] > 0) {                 /* untextured HUD text/glyphs */
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[6], (uint32_t)ut_first[6]);
    }
}

/* 3c falsify-first instrument: per-present census of ALPHA-class (blend==1, ABR
 * mode 0 (B+F)/2) tris — the ONLY order-dependent class (additive/subtractive are
 * commutative, so a back-to-front sort is a provable no-op for them). Reports the
 * alpha count, its NDC-depth spread, and the number of overlapping pairs emitted
 * OUT of back-to-front order (i emitted before j, screen bboxes overlap, i nearer
 * than j -> i should have been drawn last). inversions>0 on any frame means the
 * sort would visibly change that frame; inversions==0 across a corpus means the
 * sort is unobservable there. Env-gated (OPENPETE_ALPHA_CENSUS), inert otherwise.
 * Display-only diagnostic. */
static void alpha_census(const openpete_scene_tri* scene, int scene_n)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_ALPHA_CENSUS"); s_on = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_on) return;

    /* Blend histogram + running max per class. Sampling-proof: print whenever ANY
     * class sets a new running high (so the FIRST add>0 / sub>0 / alpha>0 always
     * shows), plus the first few frames (fire-confirm) and every alpha frame. This
     * settles "does mode-0 alpha ever occur" regardless of sample spacing. */
    static int s_shown = 0;
    static int mx[4] = {0,0,0,0};
    int hb[4] = {0,0,0,0};
    for (int t = 0; t < scene_n; t++) { int b = scene[t].blend; if (b >= 0 && b <= 3) hb[b]++; }
    int newmax = 0;
    for (int b = 0; b < 4; b++) { if (hb[b] > mx[b]) { mx[b] = hb[b]; newmax = 1; } }
    if (scene_n > 0 && (s_shown < 4 || newmax || hb[1] > 0)) {
        if (s_shown < 4) s_shown++;
        fprintf(stderr, "BLEND f=%u n=%d  o=%d alpha=%d add=%d sub=%d | max[o/a/ad/s]=%d/%d/%d/%d\n",
                s_scene_frame, scene_n, hb[0], hb[1], hb[2], hb[3], mx[0], mx[1], mx[2], mx[3]);
    }

    /* Collect alpha tris: centroid depth + screen bbox, in EMIT order. */
    enum { CAP = 4096 };
    static float cz[CAP]; static float bx0[CAP], by0[CAP], bx1[CAP], by1[CAP]; static unsigned char tex[CAP];
    int m = 0;
    for (int t = 0; t < scene_n && m < CAP; t++) {
        if (scene[t].blend != 1) continue;
        float x0 = scene[t].v[0].sx, x1 = x0, y0 = scene[t].v[0].sy, y1 = y0, zs = scene[t].v[0].z;
        for (int k = 1; k < 3; k++) {
            float x = scene[t].v[k].sx, y = scene[t].v[k].sy, z = scene[t].v[k].z;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            zs += z;
        }
        cz[m] = zs / 3.0f; bx0[m] = x0; by0[m] = y0; bx1[m] = x1; by1[m] = y1;
        tex[m] = (unsigned char)(scene[t].textured ? 1 : 0);
        m++;
    }
    if (m == 0) return;

    float zmin = cz[0], zmax = cz[0]; int ntex = 0;
    for (int i = 0; i < m; i++) { if (cz[i] < zmin) zmin = cz[i]; if (cz[i] > zmax) zmax = cz[i]; ntex += tex[i]; }

    /* Out-of-order overlapping pairs. i<j = emit order; inversion if bboxes overlap
     * and i is NEARER (smaller z) than j (i drawn first but should be drawn last). */
    int overlaps = 0, inversions = 0;
    for (int i = 0; i < m; i++) {
        for (int j = i + 1; j < m; j++) {
            if (bx0[i] > bx1[j] || bx0[j] > bx1[i] || by0[i] > by1[j] || by0[j] > by1[i]) continue;
            overlaps++;
            if (cz[i] < cz[j]) inversions++;   /* nearer tri emitted first -> wrong for back-to-front */
        }
    }
    fprintf(stderr, "ALPHA-CENSUS f=%u alpha=%d (tex=%d) zspan=[%.3f,%.3f] overlaps=%d inversions=%d\n",
            s_scene_frame, m, ntex, (double)zmin, (double)zmax, overlaps, inversions);
}

static bool draw_native_scene(bool full_extent)
{
    int scene_n; uint32_t scene_bg, scene_fade, scene_freeze; int hud_under = 0;
    const openpete_scene_tri* scene = scene_present_buf(&scene_n, &scene_bg, &scene_fade, &scene_freeze, &hud_under);

    /* Background colour = the GAMESTATE draw-env clear (s_drawenv_clear, captured
     * from PutDrawEnv) — the colour the canonical render actually auto-clears the
     * framebuffer to, correct in EVERY state. Preferred over scene_bg
     * (g_Cyclorama.m_BackgroundColor), which equals the clear only on frames that
     * draw the cyclorama and goes stale in cyclorama-less states (e.g. the black
     * level-name flyby: clear is black, g_Cyclorama still holds the old sky). Both
     * are gamestate (derive-from-gamestate principle holds); the draw-env clear is
     * just the more robust source. scene_bg is the fallback (no auto-clear), then
     * the PsyCross readback corner (ab/diff harness / isolated --render-paths). */
    uint32_t bg = s_drawenv_clear ? s_drawenv_clear : scene_bg;
    if (bg == 0) bg = bg_from_ref_corner();

    /* Empty scene: normally return false so the caller re-displays the PSX VRAM
     * framebuffer (load screens, transitions, and other direct-present frames).
     * But in ISOLATION (--render-paths) the VRAM is the UNFILTERED frame — falling
     * back to it would show every path and defeat the isolation. So in isolation
     * present just the background (= PsyCross's filtered clear) instead. */
    if (scene_n <= 0 && (!s_isolation || bg == 0))
        return false;

    s_render_full_extent = full_extent ? 1 : 0;

    static rhi_vertex buf[SCENE_VB_VERTS];
    int n = 0;
    /* Background fill (untextured, far plane), drawn first; depth-tested actors
     * sit in front of it. Covers the panel rect (window) or the full target. */
    if (bg != 0) {
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const rhi_vertex tl = { l, t, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex tr = { r, t, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex br = { r, b, 1.0f, 0, 0, bg, 0 };
        const rhi_vertex bl = { l, b, 1.0f, 0, 0, bg, 0 };
        buf[0] = tl; buf[1] = tr; buf[2] = br;
        buf[3] = tl; buf[4] = br; buf[5] = bl;
        n = 6;
    }
    /* The scene draws in 6 LAYERED groups, in this order, so a ground-decal
     * shadow lands OVER the terrain it falls on but UNDER the actors standing on
     * it (the PSX terrain -> shadow -> Spyro draw order). A single near-bias on
     * the shadow can't do both (it would also paint over Spyro's feet); the layer
     * order does. Each group's UNTEXTURED tris are packed into a contiguous VB
     * range (one batched draw); its TEXTURED tris are drawn per-tri (material
     * push) by filtering the appended textured block. gcat <0 = any (only OPAQUE
     * splits WORLD vs ACTOR vs PLAYER; transparent classes route by blend alone).
     *   0 WORLD-opaque  (blend 0, cat WORLD)   terrain / sky / props
     *   1 SUBTRACT      (blend 3)              shadows (depth_write_off)
     *   2 ACTOR-opaque  (blend 0, cat ACTOR)   mobys — cover the shadows
     *   --- HUD/overlay (depth-test off) drawn here: OVER mobys, UNDER the player ---
     *   3 PLAYER-opaque (blend 0, cat PLAYER)  Spyro — covers the near-OT HUD text
     *   4 ALPHA         (blend 1)              ABR0 semi-transparent
     *   5 ADDITIVE      (blend 2)              ABR1 flame / glows (last)
     * The HUD/overlay sits BETWEEN regular mobys and the player so the PSX's
     * near-OT-slot UI (HUD icons, "DEMO MODE", level-intro / "RESCUED <dragon>"
     * text) draws OVER mobys (e.g. the rescued dragon Nestor) yet UNDER the player
     * Spyro (who flies in front of the intro text) — exactly the PSX OT order
     * (text at the near slot, over other mobys; the player rendered on top). */
    struct scene_group { int blend, gcat; rhi_pipeline *utex, *tex; };
    const struct scene_group groups[6] = {
        { 0, OPENPETE_SCENE_WORLD,  s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 3, -1,                    s_nr.gouraud_sub_pipe,   s_nr.scene_tex_sub_pipe   },
        { 0, OPENPETE_SCENE_ACTOR,  s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 0, OPENPETE_SCENE_PLAYER, s_nr.gouraud_pipe,       s_nr.scene_tex_pipe       },
        { 1, -1,                    s_nr.gouraud_blend_pipe, s_nr.scene_tex_blend_pipe },
        { 2, -1,                    s_nr.gouraud_add_pipe,   s_nr.scene_tex_add_pipe   },
    };
    /* ut group 6 = HUD untextured overlay (after ACTOR); 7 = OVERLAY (dead last,
     * letterbox / HUD-OT 2D flat quads, over everything); 8 = HUD_PANEL (alpha,
     * under the HUD text); 9 = HUD3D (in-game 3D icon mobys, DEPTH-TESTED on top
     * after a depth clear). Groups 6/7/8 are depth-test off; 9 is depth-on. */
    int ut_first[10], ut_count[10];
    for (int gi = 0; gi < 6; gi++) {
        ut_first[gi] = n;
        int gb = groups[gi].blend, gc = groups[gi].gcat;
        for (int t = 0; t < scene_n; t++) {
            if (scene[t].textured || n + 3 > SCENE_VB_VERTS) continue;
            if (scene[t].category == OPENPETE_SCENE_HUD ||
                scene[t].category == OPENPETE_SCENE_HUD3D ||
                scene[t].category == OPENPETE_SCENE_HUD_PANEL) continue;  /* -> HUD passes */
            int b = scene[t].blend; if (b < 0 || b > 3) b = 0;   /* untextured class */
            if (b != gb) continue;
            if (gb == 0 && gc >= 0 && scene[t].category != gc) continue;
            for (int k = 0; k < 3; k++) {
                float cx, cy;
                scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
                rhi_vertex *o = &buf[n + k];
                o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
                o->rgba = scene[t].v[k].rgba; o->tex = 0;
            }
            n += 3;
        }
        ut_count[gi] = n - ut_first[gi];
    }
    /* HUD untextured: every HUD-category untextured tri (any blend), painter's
     * order = submission order; the overlay pipe ignores depth so they sit on top. */
    ut_first[6] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[6] = n - ut_first[6];
    /* OVERLAY untextured: HUD-OT 2D flat quads (letterbox bars). Drawn dead last,
     * over everything (incl. the player). Untextured opaque; painter's order. */
    ut_first[7] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_OVERLAY || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[7] = n - ut_first[7];
    /* HUD_PANEL untextured: the semi-transparent menu dark panel, drawn alpha-blended
     * at the START of the HUD layer (before the HUD text/icons) so the text reads on
     * top. Submission order; depth-test off. */
    ut_first[8] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD_PANEL || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[8] = n - ut_first[8];
    /* HUD3D untextured: the in-game 3D HUD icon mobys (gem/dragon/Spyro-head/key/
     * digits). DEPTH-TESTED (drawn with the opaque gouraud_pipe after a mid-pass
     * depth clear) so their own faces Z-sort — depth-off paints them inside-out.
     * Per-vertex z is the real view-Z depth (openpete_native_depth_from_viewz),
     * same as actors. */
    ut_first[9] = n;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_HUD3D || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    ut_count[9] = n - ut_first[9];
    /* PORTAL_SKY untextured: the destination cyclorama previews inside homeworld
     * portal arches. Collected as their own range so they can be drawn in a
     * dedicated depth-write-OFF pass AFTER the WORLD group but BEFORE actors/player
     * (see the draw site). Submission order = the feeder's far→near painter sort. */
    int portal_first = n, portal_count = 0;
    for (int t = 0; t < scene_n; t++) {
        if (scene[t].textured || scene[t].category != OPENPETE_SCENE_PORTAL_SKY || n + 3 > SCENE_VB_VERTS) continue;
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(scene[t].v[k].sx, scene[t].v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[n + k];
            o->x = cx; o->y = cy; o->z = scene[t].v[k].z; o->u = 0.0f; o->v = 0.0f;
            o->rgba = scene[t].v[k].rgba; o->tex = 0;
        }
        n += 3;
    }
    portal_count = n - portal_first;
    /* Textured tris, bucketed by draw group then (for opaque groups) material-
     * sorted, so the draw loop coalesces same-material runs into ONE vkCmdDraw.
     * Previously each textured triangle was its own vkCmdDraw + vkCmdPushConstants
     * (~3800 draws/present in a busy scene, ×~480/s = the high-FPS bottleneck);
     * material-sorting collapses that ~36× (terrain/mobys share few tpages).
     *   buckets 0..5 = the 6 layered groups (SAME partition as the draw loop)
     *   bucket  6    = HUD 2D textured backdrops (draw_hud_flat_layer)
     * HUD3D-textured and unmatched textured tris are not drawn by anyone (prior
     * behaviour), so they're dropped from the block. Opaque buckets (0,2,3) are
     * material-sorted — depth resolves visibility regardless of draw order, so it
     * is exact. Transparent buckets (1,4,5) and HUD (6) keep submission order
     * (additive/subtractive commute; ABR0 alpha is rare/single-layer; the flat-HUD
     * backdrops want painter's order) and only coalesce already-adjacent runs. */
    int tx_start = n;
    int txg_first[7], txg_count[7], txtri = 0;
    for (int b = 0; b < 7; b++) {
        txg_first[b] = tx_start + txtri * 3;
        int run_start = txtri;
        for (int t = 0; t < scene_n; t++) {
            if (!scene[t].textured) continue;
            if (tx_start + (txtri + 1) * 3 > SCENE_VB_VERTS) break;
            int cat = scene[t].category;
            int belongs;
            if (b == 6) belongs = (cat == OPENPETE_SCENE_HUD);
            else {
                int bl = scene[t].blend; if (bl < 0 || bl > 3) bl = 0;
                belongs = cat != OPENPETE_SCENE_HUD && cat != OPENPETE_SCENE_HUD3D &&
                          bl == groups[b].blend &&
                          !(groups[b].blend == 0 && groups[b].gcat >= 0 && cat != groups[b].gcat);
            }
            if (belongs) s_txscene[txtri++] = t;
        }
        if (b == 0 || b == 2 || b == 3) {   /* opaque: material-sort (depth-correct) */
            s_txsort_scene = scene;
            qsort(&s_txscene[run_start], (size_t)(txtri - run_start), sizeof(int), tex_mat_cmp);
        }
        txg_count[b] = (tx_start + txtri * 3) - txg_first[b];
    }
    for (int i = 0; i < txtri; i++) {
        const openpete_scene_tri *st = &scene[s_txscene[i]];
        for (int k = 0; k < 3; k++) {
            float cx, cy;
            scene_to_clip(st->v[k].sx, st->v[k].sy, &cx, &cy);
            rhi_vertex *o = &buf[tx_start + i * 3 + k];
            o->x = cx; o->y = cy; o->z = st->v[k].z;
            o->u = st->v[k].u; o->v = st->v[k].v;
            o->rgba = st->v[k].rgba; o->tex = 0;
        }
    }
    n = tx_start + txtri * 3;

    /* Full-screen fade overlay (func_800190D4): a single full-panel quad at the
     * near plane (z=0 passes LESS_OR_EQUAL against everything), drawn dead last so
     * it tints the WHOLE composited frame — world, actors, HUD and 2D UI — exactly
     * as the PSX's semi-transparent full-screen POLY_F4 does. The vertex colour is
     * the fade colour; the blend mode (set below) reproduces the PSX ABR. */
    int fade_first = -1, fade_mode = -1;
    if ((scene_fade & (1u << 31)) && n + 6 <= SCENE_VB_VERTS) {
        fade_mode  = (int)((scene_fade >> 24) & 3u);
        int fr = (int)((scene_fade >> 16) & 0xFFu);
        int fg = (int)((scene_fade >> 8) & 0xFFu);
        int fb = (int)(scene_fade & 0xFFu);
        /* mode 0 = (B+F)/2 -> alpha 128; add/subtract ignore alpha. */
        uint32_t fa = (fade_mode == 0) ? 0x80u : 0xFFu;
        uint32_t frgba = (uint32_t)fr | ((uint32_t)fg << 8) | ((uint32_t)fb << 16) | (fa << 24);
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const rhi_vertex tl = { l, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex tr = { r, t, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex br = { r, b, 0.0f, 0, 0, frgba, 0 };
        const rhi_vertex bl = { l, b, 0.0f, 0, 0, frgba, 0 };
        fade_first = n;
        buf[n+0] = tl; buf[n+1] = tr; buf[n+2] = br;
        buf[n+3] = tl; buf[n+4] = br; buf[n+5] = bl;
        n += 6;
    }

    /* Freeze-frame green overlay backdrop (pause / inventory / flight results):
     * a full-panel quad sampling frozen_tex (the last world frame's native render),
     * desaturated + green-tinted by freeze_overlay_pipe. The tint rides the vertex
     * colour (the game's setRGB0 0x4C,0x80,0x40). UV 0..1 (top-down, like the RT).
     * Drawn FIRST (the backdrop) so the menu UI composites on top. Only when a
     * world frame has been captured; else fall through (clear/bg shows). */
    int freeze_first = -1;
    if ((scene_freeze & (1u << 31)) && s_nr.frozen_valid && s_nr.freeze_overlay_pipe
        && n + 6 <= SCENE_VB_VERTS) {
        uint32_t trgb = scene_freeze & 0xFFFFFFu;     /* rgb in R<<16|G<<8|B */
        uint32_t tr8 = (trgb >> 16) & 0xFFu, tg8 = (trgb >> 8) & 0xFFu, tb8 = trgb & 0xFFu;
        /* Fade-to-black shift (bits 24-30, 0..127) rides the vertex alpha as 0..1; the
         * freeze_overlay shader subtracts it from the desaturated luma (GS_ExitLevel
         * case 0). 0 for the static pause/inventory backdrop -> alpha 0 -> no shift. */
        uint32_t fade7 = (scene_freeze >> 24) & 0x7Fu;
        uint32_t ta8 = (fade7 * 255u + 63u) / 127u;    /* 0..127 -> 0..255 (rounded) */
        uint32_t trgba = tr8 | (tg8 << 8) | (tb8 << 16) | (ta8 << 24);  /* byte order R,G,B,A */
        /* Fill the render rect (cover_rect = the --aspect display rect, fit into the
         * window). frozen_tex is captured at the MATCHING aspect — 4:3 in the default
         * /A-B path, or the Hor+ widescreen view in vk-widescreen (see compose_variant)
         * — so this fills with no stretch and no pillarbox. cover_rect is the --aspect
         * rect fit into the window, so a non-matching window letter/pillarboxes (bars,
         * not stretch); it recomputes each present, tracking live resizes. */
        float l, r, t, b;
        cover_rect(full_extent, &l, &r, &t, &b);
        const rhi_vertex tl = { l, t, 1.0f, 0.0f, 0.0f, trgba, 0 };
        const rhi_vertex tr = { r, t, 1.0f, 1.0f, 0.0f, trgba, 0 };
        const rhi_vertex br = { r, b, 1.0f, 1.0f, 1.0f, trgba, 0 };
        const rhi_vertex bl = { l, b, 1.0f, 0.0f, 1.0f, trgba, 0 };
        freeze_first = n;
        buf[n+0] = tl; buf[n+1] = tr; buf[n+2] = br;
        buf[n+3] = tl; buf[n+4] = br; buf[n+5] = bl;
        n += 6;
    }

    rhi_update_buffer(s_nr.scene_vb, buf, sizeof(rhi_vertex) * (size_t)(n > 0 ? n : 1));

    /* Confine the scene to panel 0's pixel rect so triangles that extend off
     * the PSX screen (legitimately, after near-plane culling) are clipped to
     * the panel instead of streaking across the window. In full_extent mode the
     * RT pass already scissors to the whole target (the frame fills it). */
    if (!full_extent) {
        /* Scissor to the --aspect render rect: the 4:3 content sub-rect (p0_hx)
         * widened by the Hor+ factor (ws_factor). The revealed side world fills
         * out to this rect; everything beyond is the letterbox/pillarbox bars
         * (left clear → black/bg), so the render respects --aspect instead of
         * stretching to the window. ws_factor==1 → exactly the 4:3 rect. */
        float fhx = s_nr.p0_hx * s_nr.ws_factor;
        int sx = (int)(((s_nr.p0_cx - fhx) * 0.5f + 0.5f) * (float)s_nr.win_w + 0.5f);
        int sw = (int)((fhx) * (float)s_nr.win_w + 0.5f);
        int sy = (int)((0.5f - 0.5f * s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        int sh = (int)((s_nr.p0_hy) * (float)s_nr.win_h + 0.5f);
        rhi_set_scissor(s_nr.dev, sx, sy, sw, sh);
    }

    /* Background fill: the panel-rect far-plane quad built at buf[0..6) above.
     * Drawn FIRST so all scene geometry composites over it; fills the cyclorama
     * background colour where the sky dome / terrain don't cover the panel (the
     * PSX draw-env clear). The off-display native_rtt receives this colour from
     * its render-pass CLEAR, but the LIVE window offscreen is cleared to BLACK
     * (so letterbox borders stay black) and then composes the scene over it — so
     * without this draw the live window shows BLACK sky wherever the dome has a
     * gap, even though screenshots (which read native_rtt) look correct. z=1.0
     * (far) + depth-write: the dome (also z=1) overdraws it via LESS_OR_EQUAL,
     * nearer geometry over that. */
    if (bg != 0 && s_nr.gouraud_pipe) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, 6, 0);
    }

    /* Freeze backdrop FIRST (behind world/UI): the frozen native frame sampled
     * by freeze_overlay_pipe (set 0 = frozen_tex), desaturated + green-tinted. On
     * a freeze frame the world groups are empty (the game stops rendering the
     * world), so this fills the screen and the menu UI composites over it. */
    if (freeze_first >= 0 && s_nr.frozen_tex) {
        rhi_bind_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.frozen_tex);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, 6, (uint32_t)freeze_first);
    }

    /* Draw the 6 layered groups in order (WORLD-opaque, SUBTRACT shadows,
     * ACTOR-opaque, PLAYER-opaque, ALPHA, ADDITIVE). For each: the batched
     * untextured range, then its textured tris as material-coalesced runs
     * (draw_tex_runs over the pre-bucketed/sorted block — one draw per material
     * run, not per triangle). Opaque groups write depth; the transparent groups
     * (1/4/5) are depth_write_off (test vs opaque, never occlude each other).
     *
     * The HUD/overlay layer (depth-test OFF) is drawn AFTER the ACTOR group (mobys,
     * gi==2) and BEFORE the PLAYER group (Spyro, gi==3): it ignores world depth so
     * it sits on top of the terrain AND regular mobys (the "DEMO MODE" banner /
     * level-intro & "RESCUED <dragon>" text / HUD icons the .s forces to the near OT
     * slot, over other mobys like the rescued dragon), but writes no depth, so the
     * player Spyro drawn next still covers it (Spyro flies in front of the intro
     * text). Matches the PSX moby OT: near-slot UI over other mobys, player on top. */
    for (int gi = 0; gi < 6; gi++) {
        /* Freeze backdrop active (pause / inventory / flight-results): the world is
         * frozen INTO the backdrop (drawn above), and the canonical game render emits
         * no world/actor/player/shadow/transparent geometry while a menu is up — only
         * the HUD menu (text mobys, panel, 3D icons) on top. The high-FPS spine, by
         * contrast, blindly re-extracts the still-in-RAM gameplay actors every present;
         * drawing them here would paint full-colour Spyro/mobys over the green-tinted
         * backdrop (a ghost double-image). Skip the gameplay groups; the HUD layers
         * below still draw, so the animated menu stays smooth at the present rate. */
        if (freeze_first >= 0) continue;
        const struct scene_group *g = &groups[gi];
        if (g->utex && ut_count[gi] > 0) {
            rhi_bind_pipeline(s_nr.dev, g->utex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)ut_count[gi], (uint32_t)ut_first[gi]);
        }
        if (txg_count[gi] > 0 && s_nr.vram_tex && g->tex) {
            rhi_bind_pipeline(s_nr.dev, g->tex);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            draw_tex_runs(txg_first[gi], txg_count[gi], tx_start, scene);
        }
        /* Portal-sky previews: drawn right after the WORLD group (gi==0) so they
         * depth-test against the arch stone / terrain just written (the arch frame
         * occludes them into the opening shape), but via the depth-WRITE-OFF pipe so
         * the SUBTRACT/ACTOR/PLAYER groups drawn next are never per-pixel cut by the
         * sky — Spyro flying into/through the portal always sorts on top, matching
         * the PSX OT (portal sky = one bucket, the whole player model in front). */
        if (gi == 0 && s_nr.gouraud_noz_pipe && portal_count > 0) {
            rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_noz_pipe);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, (uint32_t)portal_count, (uint32_t)portal_first);
        }
        /* GS_LevelTransition flyby ONLY (hud_under): draw the flat HUD layer here —
         * after the ACTOR group (gi==2) but BEFORE the PLAYER group (gi==3) — so the
         * fly-in Spyro draws in front of the revealed "THE ADVENTURE BEGINS" text.
         * In every other state the flat HUD draws OVER the player (after the loop). */
        if (gi == 2 && hud_under)
            draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);
    }

    /* Default: the flat 2D HUD/text draws OVER the whole scene incl. the player
     * (Spyro behind the HUD/text). Only the GS_LevelTransition flyby puts it under
     * the player (handled at gi==2 above). */
    if (!hud_under)
        draw_hud_flat_layer(scene, scene_n, ut_first, ut_count, tx_start, txg_first[6], txg_count[6]);

    /* HUD3D layer — the in-game 3D HUD icon mobys (gem/dragon/Spyro-head/key/
     * digits). They are real 3D models, so depth-off (the flat HUD layer) paints
     * their own faces inside-out. Draw them LAST, on top of the whole scene, but
     * with the depth buffer CLEARED first (rhi_clear_depth) so they Z-sort among
     * their own faces (depth-test ON via the opaque gouraud_pipe) while never being
     * occluded by world/actor/player geometry. Their vertex positions already match
     * the .s to <1px; this only fixes their internal depth ordering. */
    if (s_nr.gouraud_pipe && ut_count[9] > 0) {
        rhi_clear_depth(s_nr.dev);
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[9], (uint32_t)ut_first[9]);
    }

    /* OVERLAY layer (HUD-OT 2D flat quads — the cutscene letterbox bars): drawn
     * after every scene group, over everything incl. the player, depth-test off
     * (gouraud_hud_pipe). Before the fade, matching the PSX HUD-OT order (the fade
     * F4 is linked after the border, so it draws on top of the bars). */
    if (s_nr.gouraud_hud_pipe && ut_count[7] > 0) {
        rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
        rhi_draw(s_nr.dev, (uint32_t)ut_count[7], (uint32_t)ut_first[7]);
    }

    /* Draw the full-screen fade overlay last (over every layer above), in the PSX
     * ABR blend: mode 2 (B-F) subtract -> fade to black; mode 1 (B+F) add -> fade to
     * white; mode 0 ((B+F)/2) -> alpha. These pipes are depth_write_off + the quad
     * is at z=0, so it tests-passes against the whole frame and writes no depth. */
    if (fade_first >= 0) {
        rhi_pipeline* fp = (fade_mode == 2) ? s_nr.gouraud_sub_pipe
                         : (fade_mode == 0) ? s_nr.gouraud_blend_pipe
                                            : s_nr.gouraud_add_pipe;  /* 1 (and 3) -> additive */
        if (fp) {
            rhi_bind_pipeline(s_nr.dev, fp);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.scene_vb);
            rhi_draw(s_nr.dev, 6, (uint32_t)fade_first);
        }
    }

    if (!full_extent) rhi_reset_scissor(s_nr.dev);
    s_render_full_extent = 0;
    return true;
}

/* ---- native FPS overlay (vk live window) --------------------------------
 * The legacy GL FPS counter (fps_overlay.cpp) stamps PsyCross's hidden window —
 * invisible in vk, and in pure-vk PsyCross never even presents. This is its
 * native equivalent: the FPS counter drawn with Spyro's OWN 3D HUD text mobys
 * (the gold extruded glyph models), built game-side by text_moby_native and
 * pushed via openpete_native_set_overlay_glyphs. Pure display: drawn only on the
 * LIVE vk present (not screenshots, ab, or diff, so the comparison images stay
 * clean). Toggle at runtime (F11) or start off with --show-fps 0. */
static int s_fps_overlay  = 1;   /* enabled (default on) */
static int s_overlay_live = 0;   /* 1 only during the live present's compose_variant */
static uint64_t s_fps_win_start = 0;  /* rolling 0.5s FPS window */
static int      s_fps_frames    = 0;
static int      s_fps_value     = 0;

void openpete_native_set_fps_overlay(int on) { s_fps_overlay = on ? 1 : 0; }
int  openpete_native_toggle_fps_overlay(void) { s_fps_overlay = !s_fps_overlay; return s_fps_overlay; }
int  openpete_native_fps_overlay_enabled(void) { return s_fps_overlay; }
int  openpete_native_fps_value(void) { return s_fps_value; }

/* Overlay geometry built from Spyro's own 3D HUD text mobys (see
 * openpete_native_render.h). The game-side emitter (text_moby_native) pushes a
 * triangle list each frame; draw_fps_overlay draws it on the live present, or
 * nothing when empty (the game side caches the glyph models, so this is empty
 * only before the font has ever loaded — there is no bitmap fallback). Currently
 * the FPS counter; reusable for other text overlays. */
static openpete_hud_vert s_overlay_glyph_verts[OVL_MAX_VERTS];
static int               s_overlay_glyph_n = 0;
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts) {
    if (nverts < 0) nverts = 0;
    if (nverts > OVL_MAX_VERTS) nverts = OVL_MAX_VERTS;
    if (nverts > 0 && verts) memcpy(s_overlay_glyph_verts, verts, (size_t)nverts * sizeof(verts[0]));
    s_overlay_glyph_n = nverts;
}

/* Draw the overlay built from Spyro's own 3D HUD text mobys, fed each frame by
 * the game side (text_moby_native -> openpete_native_set_overlay_glyphs). Verts
 * arrive in window-height-fraction overlay space (origin top-left, +y down) with
 * a depth in [0,1); map to NDC (x via win_h so glyphs stay square) and draw
 * depth-tested through the opaque gouraud pipe after a local depth clear, so the
 * glyphs z-sort among their own faces yet sit on top of the whole frame — the
 * same isolation trick as the in-game HUD3D pass. Returns true if it drew. */
static bool draw_overlay_glyphs_3d(float ax, float ay) {
    if (s_overlay_glyph_n < 3 || !s_nr.gouraud_pipe || !s_nr.overlay_vb) return false;
    static rhi_vertex buf[OVL_MAX_VERTS];
    int n = 0;
    for (int i = 0; i < s_overlay_glyph_n && n < OVL_MAX_VERTS; i++) {
        const openpete_hud_vert *v = &s_overlay_glyph_verts[i];
        /* Glyph coords are win_h-relative; ax/ay map them into the target's NDC.
         * Offscreen 1×: ax=2·win_h/win_w, ay=2. Overlay SS: ax=2/OVL_REGION_W,
         * ay=2/OVL_REGION_H — both keep glyphs square (ax/ay carry the same
         * win_h scale through the target's pixel dims). */
        buf[n].x = v->x * ax - 1.0f;
        buf[n].y = 1.0f - v->y * ay;
        buf[n].z = v->z; buf[n].u = 0.0f; buf[n].v = 0.0f;
        buf[n].rgba = v->rgba; buf[n].tex = 0;
        n++;
    }
    n -= n % 3;
    if (n <= 0) return false;
    rhi_update_buffer(s_nr.overlay_vb, buf, (size_t)n * sizeof(rhi_vertex));
    rhi_reset_scissor(s_nr.dev);
    rhi_clear_depth(s_nr.dev);
    rhi_bind_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    rhi_bind_vertex_buffer(s_nr.dev, s_nr.overlay_vb);
    rhi_draw(s_nr.dev, (uint32_t)n, 0);
    return true;
}

/* True when the FPS overlay should render this frame: glyphs built, enabled, and
 * not suppressed by the record gating (kept out of recordings unless
 * OPENPETE_FPS_IN_RECORD lets the live look be captured for verification). */
static bool fps_overlay_should_draw(void) {
    if (!s_fps_overlay || s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_overlay_glyph_n < 3) return false;
    if (openpete_record_active()) {
        static int s_in_rec = -1;
        if (s_in_rec < 0) { const char *e = getenv("OPENPETE_FPS_IN_RECORD");
                            s_in_rec = (e && *e && *e != '0') ? 1 : 0; }
        if (!s_in_rec) return false;
    }
    return true;
}

/* True when the 4× supersample path is usable (all resources up). */
static bool ovl_ss_ready(void) {
    return s_nr.ovl_resolve_pipe && s_nr.ovl_quad_vb && s_nr.ovl_ss;
}

/* (Re)create ovl_ss at OVL_SS_FACTOR× the overlay region and rebuild the
 * composite quad on a window resize. Creates GPU resources / waits idle — call
 * OUTSIDE a frame (alongside ensure_ss_rtt). No-op if the SS pipeline/quad
 * failed at init (draw_fps_overlay then falls back to the 1× path). */
static void ensure_ovl_ss(void) {
    if (!s_nr.ovl_resolve_pipe || !s_nr.ovl_quad_vb || s_nr.win_w <= 0 || s_nr.win_h <= 0) return;
    int rw = (int)(OVL_REGION_W * (float)s_nr.win_h + 0.5f);
    int rh = (int)(OVL_REGION_H * (float)s_nr.win_h + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int want_w = rw * OVL_SS_FACTOR, want_h = rh * OVL_SS_FACTOR;
    uint32_t maxdim = rhi_max_image_dimension(s_nr.dev);
    if ((uint32_t)want_w > maxdim) want_w = (int)maxdim;
    if ((uint32_t)want_h > maxdim) want_h = (int)maxdim;
    if (!s_nr.ovl_ss || s_nr.ovl_ss_w != want_w || s_nr.ovl_ss_h != want_h) {
        rhi_wait_idle(s_nr.dev);
        if (s_nr.ovl_ss) { rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss); s_nr.ovl_ss = NULL; }
        s_nr.ovl_ss = rhi_create_render_target(s_nr.dev, (uint32_t)want_w, (uint32_t)want_h);
        s_nr.ovl_ss_w = want_w; s_nr.ovl_ss_h = want_h;
        if (s_nr.ovl_ss)
            OPL_INFO_F("native", "FPS overlay SS target %dx%d (%d×)", want_w, want_h, OVL_SS_FACTOR);
    }
    /* Composite quad — the overlay's screen rect (top-left) in offscreen NDC.
     * Rect width is win_h-relative (glyph x uses win_h), so its NDC width scales
     * by win_h/win_w; UV 0..1 maps the whole SS target (v0 top, top-down). */
    if (s_nr.ovl_quad_win_w != s_nr.win_w || s_nr.ovl_quad_win_h != s_nr.win_h) {
        float rw_ndc = OVL_REGION_W * (float)s_nr.win_h / (float)s_nr.win_w * 2.0f;
        float rh_ndc = OVL_REGION_H * 2.0f;
        rhi_vertex q[6];
        build_panel(q, -1.0f + rw_ndc * 0.5f, 1.0f - rh_ndc * 0.5f,
                    rw_ndc * 0.5f, rh_ndc * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
        rhi_update_buffer(s_nr.ovl_quad_vb, q, sizeof(q));
        s_nr.ovl_quad_win_w = s_nr.win_w; s_nr.ovl_quad_win_h = s_nr.win_h;
    }
}

/* Pre-pass: render the FPS glyphs at OVL_SS_FACTOR× into ovl_ss (cleared
 * transparent). Its own render-target pass, so call it BEFORE the offscreen pass
 * opens (no nested render passes). No-op unless the SS path is ready. */
static void prepare_fps_overlay_ss(void) {
    if (!ovl_ss_ready() || !fps_overlay_should_draw()) return;
    rhi_begin_render_target(s_nr.dev, s_nr.ovl_ss, 0.0f, 0.0f, 0.0f, 0.0f);  /* transparent */
    draw_overlay_glyphs_3d(2.0f / OVL_REGION_W, 2.0f / OVL_REGION_H);
    rhi_end_render_target(s_nr.dev, s_nr.ovl_ss);
}

/* Composite the FPS overlay into the currently-open offscreen pass. SS path:
 * box-downsample + straight-alpha-blend the ovl_ss the pre-pass built. Fallback:
 * draw the glyphs directly at 1× (the original crisp-but-aliased path). */
static void draw_fps_overlay(void) {
    if (!fps_overlay_should_draw()) return;
    if (ovl_ss_ready()) {
        int factor = OVL_SS_FACTOR;
        rhi_reset_scissor(s_nr.dev);
        rhi_bind_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.ovl_ss);
        rhi_push_constants(s_nr.dev, &factor, sizeof(factor));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.ovl_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
    } else {
        draw_overlay_glyphs_3d(2.0f * (float)s_nr.win_h / (float)s_nr.win_w, 2.0f);
    }
}

/* Capture the freeze backdrop from the CLEAN windowed offscreen we just ended
 * (correctly projected/pillarboxed at every window size, and — under --ssaa —
 * already box-resolved back to 1× window resolution). Hardware-blit the
 * offscreen's 4:3 content region (cover_rect = the p0 sub-rect widened by
 * ws_factor, in window pixels) into frozen_tex; the pause/inventory/exit freeze
 * present then samples this clean snapshot. World frames only, so the snapshot
 * is the gameplay frame to freeze, not the menu drawn over it. Must be called
 * after rhi_end_offscreen (off_image in TRANSFER_SRC). */
static void capture_freeze_from_offscreen(bool world_frame)
{
    if (!(world_frame && s_nr.frozen_tex && s_nr.win_w > 0 && s_nr.win_h > 0)) return;
    float fhx = s_nr.p0_hx * s_nr.ws_factor;
    int sx = (int)(((s_nr.p0_cx - fhx) * 0.5f + 0.5f) * (float)s_nr.win_w + 0.5f);
    int sw = (int)(fhx * (float)s_nr.win_w + 0.5f);
    int sy = (int)(((1.0f - s_nr.p0_hy) * 0.5f) * (float)s_nr.win_h + 0.5f);
    int sh = (int)(s_nr.p0_hy * (float)s_nr.win_h + 0.5f);
    if (sw > 0 && sh > 0) {
        rhi_snapshot_offscreen_to_rt(s_nr.dev, s_nr.frozen_tex, sx, sy, sw, sh);
        s_nr.frozen_valid = true;
    }
}

/* Renderer variants — the live mode picks one; --screenshot-on-frame dumps all
 * three (see openpete_native_render_screenshot_all). */
enum { NRV_VK = 0, NRV_AB = 1, NRV_DIFF = 2 };

/* Compose one variant into the open frame's offscreen, ready for the caller to
 * rhi_blit_to_swapchain + rhi_end_frame. Assumes rhi_begin_frame succeeded, the
 * layout matches the variant's panel count (1 for vk/diff, 2 for ab), and — for
 * NRV_DIFF — native_rtt is allocated. Returns true iff the diff pass actually
 * ran (scene non-empty + reference present), so the caller can fold per-frame
 * --render-stats. Shared by the live render path and the screenshot-all dump. */
static bool compose_variant(int variant, bool have_ref)
{
    int sn; uint32_t sbg, sfade, sfreeze;
    const openpete_scene_tri* scene = scene_present_buf(&sn, &sbg, &sfade, &sfreeze, NULL);
    uint32_t bg = sbg; if (bg == 0) bg = bg_from_ref_corner();  /* gamestate primary (== PSX clear); corner fallback */
    bool want_scene = (sn > 0) || (s_isolation && bg != 0);

    /* native_rtt is the OFF-DISPLAY reference-resolution render: the diff pass
     * compares it, and the freeze-backdrop snapshot copies it. The LIVE vk/ab panel
     * does NOT blit from it — it draws the scene DIRECTLY into the window-sized
     * offscreen below, so the native display keeps its FULL window resolution
     * (rendering into the ref-sized native_rtt and upscaling would soften textures
     * and drop fine UI). So native_rtt is rendered only when actually consumed:
     * every frame in diff mode, and on WORLD frames in vk/ab to refresh the freeze
     * snapshot (skip menu/freeze frames so the snapshot is the gameplay frame to
     * freeze, never the menu drawn over it). */
    bool world_frame = want_scene && !(sfreeze & (1u << 31)) && scene_has_world(scene, sn);
    bool rtt_drew = false;
    float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;
    /* native_rtt is the off-display, reference-resolution render consumed by the
     * diff pass (every frame) and the ab freeze-backdrop snapshot (world frames).
     * The vk freeze backdrop is NOT captured from here: this full-extent render
     * skews window-dependently — it squished the pause/inventory freeze at
     * non-native window sizes — so vk snapshots the freeze from the clean,
     * correctly-projected windowed offscreen instead (rhi_snapshot_offscreen_to_rt
     * after the offscreen pass below). */
    if (s_nr.native_rtt && want_scene &&
        (variant == NRV_DIFF || (variant == NRV_AB && world_frame))) {
        rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
        rtt_drew = draw_native_scene(true);
        rhi_end_render_target(s_nr.dev, s_nr.native_rtt);

        if (variant == NRV_AB && rtt_drew && world_frame && s_nr.frozen_tex) {
            rhi_begin_render_target(s_nr.dev, s_nr.frozen_tex, 0.0f, 0.0f, 0.0f, 1.0f);
            rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.native_rtt);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
            rhi_end_render_target(s_nr.dev, s_nr.frozen_tex);
            s_nr.frozen_valid = true;
        }
    }

    /* Diff: |native_rtt - reference| into the offscreen (panel-0 rect). */
    if (variant == NRV_DIFF && rtt_drew && have_ref) {
        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        rhi_bind_pipeline(s_nr.dev, s_nr.diff_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.native_rtt);  /* set 0 = native */
        rhi_bind_vram2(s_nr.dev, s_nr.ref_tex);    /* set 1 = reference */
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.diff_quad_vb);
        rhi_draw(s_nr.dev, 6, 0);
        rhi_end_offscreen(s_nr.dev);
        return true;
    }

    /* SSAA (--ssaa>1, vk mode): render the scene into the supersampled ss_rtt,
     * then a fragment resolve pass box-averages it (in linear light) into the 1×
     * offscreen — a true box filter at every factor and gamma-correct, unlike the
     * present blit's single bilinear tap. Scoped to vk (ab/diff stay reference-
     * resolution for the 1:1 PsyCross comparison). */
    if (variant == NRV_VK && s_nr.ss_rtt && s_nr.ss_factor > 1 && s_nr.layout_ref_w > 0) {
        rhi_begin_render_target(s_nr.dev, s_nr.ss_rtt, 0.0f, 0.0f, 0.0f, 1.0f);
        bool drew = draw_native_scene(false);   /* scene at ss_factor× (scissor auto-scaled) */
        rhi_end_render_target(s_nr.dev, s_nr.ss_rtt);

        rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
        if (drew) {
            int n = s_nr.ss_factor;
            rhi_bind_pipeline(s_nr.dev, s_nr.resolve_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ss_rtt);
            rhi_push_constants(s_nr.dev, &n, sizeof(n));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_quad_vb);
            rhi_draw(s_nr.dev, 6, 0);
        } else {
            /* Empty scene → PSX VRAM framebuffer fallback (1× direct, no SSAA). */
            int tx = 0, ty = 0;
            if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
                int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
                tx = s_nr.disp_x; ty = s_nr.disp_y + dy;
            }
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            rhi_bind_pipeline(s_nr.dev, s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }
        if (s_overlay_live) draw_fps_overlay();   /* on the resolved 1× offscreen, crisp */
        rhi_end_offscreen(s_nr.dev);
        /* The freeze snapshot lives in the fall-through path below, which this SSAA
         * branch skips — so capture it here too, from the box-resolved 1× offscreen
         * (window resolution, matching the non-SSAA snapshot). Without this the
         * pause/inventory freeze had no captured frame under --ssaa and showed the
         * clear/cyclorama colour instead of the frozen world. */
        capture_freeze_from_offscreen(world_frame);
        return false;
    }

    /* vk / ab (and the diff fallback on an empty scene): draw the scene DIRECTLY
     * into the window-sized offscreen at full resolution (the original display path,
     * unchanged by the freeze work). The freeze overlay — drawn inside
     * draw_native_scene sampling frozen_tex — composes here too on menu frames. An
     * empty scene falls back to re-displaying the PSX VRAM framebuffer (loading
     * screens / transitions / pre-scene bootstrap). */
    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
    if (s_nr.layout_ref_w > 0) { /* layout built (needs a known ref size) */
        /* tpage = the CURRENT front-buffer origin (flips per frame via the double
         * buffer) + the top overscan band so the sampled rows are the visible clip. */
        int tx = 0, ty = 0;
        if (s_nr.disp_w > 0 && s_nr.clip_h > 0 && s_nr.clip_h <= s_nr.disp_h) {
            int dy = (s_nr.disp_h - s_nr.clip_h) / 2;
            tx = s_nr.disp_x;
            ty = s_nr.disp_y + dy;
        }
        if (!draw_native_scene(false)) {
            const psx_material nd_mat = { tx, ty, 0, 0, 16 };
            rhi_bind_pipeline(s_nr.dev, s_nr.vram_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
            rhi_push_constants(s_nr.dev, &nd_mat, sizeof(nd_mat));
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 0);
        }

        /* Panel 1 — reference (A/B compare only; vk/diff are single panels). */
        if (variant == NRV_AB && have_ref) {
            rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ref_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 6);
        }
    }

    /* FPS overlay — vk live present only (kept off ab/diff so comparison images
     * stay clean, and off screenshots/dumps where s_overlay_live is 0). */
    if (variant == NRV_VK && s_overlay_live) draw_fps_overlay();

    rhi_end_offscreen(s_nr.dev);

    if (variant == NRV_VK) capture_freeze_from_offscreen(world_frame);
    return false;
}

/* Game-registered sub-tick re-extract callback (high-FPS spine). Invoked by
 * the host loop on each intermediate present; rebuilds s_scene against an
 * interpolated gamestate view. NULL → no intermediates (present stays at sim
 * cadence). See openpete_native_render.h. */
static openpete_native_reextract_fn s_reextract_cb = NULL;
void openpete_native_register_reextract(openpete_native_reextract_fn fn) { s_reextract_cb = fn; }
openpete_native_reextract_fn openpete_native_get_reextract(void) { return s_reextract_cb; }

static openpete_native_commit_fn s_commit_cb = NULL;
void openpete_native_register_commit(openpete_native_commit_fn fn) { s_commit_cb = fn; }
openpete_native_commit_fn openpete_native_get_commit(void) { return s_commit_cb; }

/* --record async capture: persistent host buffer the readback ring swizzles
 * into before handing off to the encoder thread. Process-lifetime; freed in
 * shutdown. Shared by the per-present cycle and the shutdown drain. */
static unsigned char *s_recbuf = NULL;
static size_t         s_reccap = 0;
static unsigned char *recbuf_ensure(size_t need)
{
    if (need == 0) return NULL;
    if (need > s_reccap) {
        unsigned char *nb = realloc(s_recbuf, need);
        if (!nb) return NULL;
        s_recbuf = nb; s_reccap = need;
    }
    return s_recbuf;
}

/* Queue this present for async readback (tagged with its wall-clock PTS) and, if
 * a frame captured a couple presents ago is ready, hand it to the encoder. Rides
 * the frame's own fence — NO GPU stall, so the recording runs at real speed
 * (unlike the screenshot path's blocking rhi_read_offscreen). Called between the
 * blit and end_frame, while the frame slot is still current. */
static void record_cycle(uint64_t pts_us)
{
    unsigned char *buf = recbuf_ensure((size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u);
    if (!buf) return;
    int w = 0, h = 0; uint64_t tag = 0;
    if (rhi_cycle_readback(s_nr.dev, pts_us, buf, s_reccap, &w, &h, &tag) && w > 0 && h > 0)
        openpete_record_submit_frame(buf, w, h, /*top_down=*/1, tag);
}

void openpete_native_render_frame(uint64_t record_pts_us)
{
    if (!s_nr.enabled)
        return;

    /* Rolling 0.5s FPS window for the overlay. Counts actual presents (canonical
     * + sub-tick intermediates), so it reflects the true on-screen present rate. */
    {
        uint64_t now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
        if (s_fps_win_start == 0) s_fps_win_start = now;
        s_fps_frames++;
        uint64_t el = now - s_fps_win_start;
        if (freq > 0 && el >= freq / 2u) {
            s_fps_value = (int)((double)s_fps_frames * (double)freq / (double)el + 0.5);
            s_fps_win_start = now; s_fps_frames = 0;
        }
    }

    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross input). */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        s_nr.win_w = pw;
        s_nr.win_h = ph;
        rhi_notify_resize(s_nr.dev);
    }

    /* Mirror VRAM + read the reference back BEFORE opening our frame: these do
     * their own one-shot GPU submits, kept clear of our render pass. Called
     * UNCONDITIONALLY: sync_reference_texture self-no-ops when PsyCross hasn't
     * produced a scene (pure vk — the OT isn't rasterized — so GR_GetPresentSceneSize
     * returns 0 → early false), so it's free there; whenever PsyCross IS rendering
     * (ab/diff/screenshot) it must run, both to upload the reference AND because its
     * GL readback orders the GPU work the screenshot's rhi_read_offscreen then reads
     * (gating it off for vk blanked the captured native-intermediate frames). NRV_VK
     * never samples have_ref, so the value itself only matters for ab/diff. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();

    /* Reference resolution (the display shape for the window aspect-fit + the
     * native_rtt/freeze size) normally comes from PsyCross's present FBO via
     * sync_reference_texture. In pure vk PsyCross no longer rasterizes the OT, so
     * that returns nothing and ref_w/ref_h would stay 0 → update_layout bails →
     * compose draws nothing → black. Derive it from GAMESTATE instead: the
     * bars-free PSX display is 4:3 corrected for the vertical overscan crop
     * (aspect = 4/3 * disp_h/clip_h), at a ~2x-internal reference so the
     * off-display RT stays crisp. Only the aspect matters to the layout (fit_cell
     * cancels the absolute scale); the size feeds native_rtt. Gated vk-only —
     * ab/diff genuinely need the PsyCross reference, so they wait for it. */
    if (!have_ref && !s_scene_ref_compare) {
        int dh = s_nr.disp_h > 0 ? s_nr.disp_h : 240;
        int ch = s_nr.clip_h > 0 ? s_nr.clip_h : 224;
        int rh = ch * 2;
        int rw = (int)((double)rh * (4.0 / 3.0) * (double)dh / (double)ch + 0.5);
        if (rw > 0 && rh > 0) { s_nr.ref_w = rw; s_nr.ref_h = rh; }
    }
    update_layout();
    ensure_native_rtt(true);   /* native_rtt is the canonical frame in every mode */
    ensure_ss_rtt();           /* SSAA supersampled scene target (--ssaa>1, vk mode) */
    ensure_ovl_ss();           /* FPS-overlay supersample target + composite quad */

    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */

    int variant = s_nr.diff_mode ? NRV_DIFF : s_nr.ab_mode ? NRV_AB : NRV_VK;
    s_overlay_live = 1;                       /* this is the live present → allow the FPS overlay */
    /* Render the FPS glyphs at OVL_SS_FACTOR× into ovl_ss now — its own render
     * pass, so it must precede compose_variant's offscreen pass. compose's
     * draw_fps_overlay then composites the downsampled result. */
    prepare_fps_overlay_ss();
    bool did_diff = compose_variant(variant, have_ref);
    s_overlay_live = 0;                       /* screenshot/dump composes must not draw it */

    rhi_blit_to_swapchain(s_nr.dev);

    /* --record: capture the on-screen native frame via the async ring (no-op
     * unless recording is active). MUST run before end_frame, while this frame
     * slot is still current — the copy is appended to the frame's command
     * buffer. */
    if (openpete_record_active())
        record_cycle(record_pts_us);

    rhi_end_frame(s_nr.dev);

    if (did_diff && s_nr.stats_enabled) diff_stats_accumulate();  /* reads back the diff */
}

/* Insert a suffix before the extension: ("shot-42.png","-vk") -> "shot-42-vk.png". */
static void path_with_suffix(const char *base, const char *suffix,
                             char *out, size_t cap)
{
    const char *dot = strrchr(base, '.');
    if (dot) snprintf(out, cap, "%.*s%s%s", (int)(dot - base), base, suffix, dot);
    else     snprintf(out, cap, "%s%s", base, suffix);
}

int openpete_native_render_screenshot_all(const char *base)
{
    if (!s_nr.enabled || !base) return 1;

    /* vk-only widescreen: the RTT path below renders the 4:3 CONTENT shape (the
     * RTT is ref-sized), which doesn't show the Hor+ widening. The just-presented
     * LIVE offscreen, however, already holds the full widescreen frame at window
     * size — grab it now, before this function opens a new frame, and use it for
     * the -vk image so the dump matches what the window shows. Only in vk-only
     * (ab/diff stay 4:3 to compare against the reference). */
    unsigned char *live = NULL; int live_w = 0, live_h = 0;
    if (!s_scene_ref_compare && s_nr.ws_factor > 1.0001f &&
        s_nr.win_w > 0 && s_nr.win_h > 0) {
        size_t lcap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
        live = malloc(lcap);
        if (live && !rhi_read_offscreen(s_nr.dev, live, lcap, &live_w, &live_h)) {
            free(live); live = NULL;
        }
    }

    /* Re-sync VRAM + reference so the dump matches the just-presented frame. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    ensure_native_rtt(true);
    if (!have_ref || !s_nr.native_rtt || !s_nr.readback ||
        s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return 1;

    /* Compose every variant at the REFERENCE resolution rather than the window
     * size, so the images are the true content size with no window letterbox:
     *   -vk   = native scene           (W x H)
     *   -diff = |native - reference|   (W x H)
     *   -ab   = native | reference     (2W x H)
     * The native scene is rendered full-extent into the ref-sized RTT and read
     * straight back; the reference comes from the PsyCross readback. */
    const int W = s_nr.ref_w, H = s_nr.ref_h;
    const size_t frame_px = (size_t)W * (size_t)H;

    /* Background = the gamestate draw-env clear (matching the live native render);
     * scene_bg then corner as fallbacks. See draw_native_scene for the rationale. */
    int scene_n; uint32_t scene_bg, scene_fade;
    (void)scene_present_buf(&scene_n, &scene_bg, &scene_fade, NULL, NULL);
    uint32_t bg = s_drawenv_clear ? s_drawenv_clear : scene_bg;
    if (bg == 0) bg = bg_from_ref_corner();
    const float br  = (float)((bg >>  0) & 0xFF) / 255.0f;
    const float bgc = (float)((bg >>  8) & 0xFF) / 255.0f;
    const float bb  = (float)((bg >> 16) & 0xFF) / 255.0f;

    /* Render the native scene into the ref-sized RTT (full extent, no panels).
     * The offscreen + present are kept valid with a plain bg clear so the window
     * isn't disturbed mid-dump; the RTT (not the window) is the data source. */
    if (!rhi_begin_frame(s_nr.dev)) return 1;
    rhi_begin_render_target(s_nr.dev, s_nr.native_rtt, br, bgc, bb, 1.0f);
    draw_native_scene(true);
    rhi_end_render_target(s_nr.dev, s_nr.native_rtt);
    rhi_begin_offscreen(s_nr.dev, br, bgc, bb, 1.0f);
    rhi_end_offscreen(s_nr.dev);
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);

    unsigned char *nat = malloc(frame_px * 4);          /* native, top-down    */
    unsigned char *ref = malloc(frame_px * 4);          /* reference, top-down  */
    unsigned char *ab  = malloc(frame_px * 4 * 2);      /* native | reference   */
    unsigned char *dif = malloc(frame_px * 4);          /* |native - reference| */
    if (!nat || !ref || !ab || !dif) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }

    int nw = 0, nh = 0;
    if (!rhi_read_render_target(s_nr.dev, s_nr.native_rtt, nat, frame_px * 4, &nw, &nh)) {
        free(nat); free(ref); free(ab); free(dif);
        return 1;
    }
    /* The PsyCross readback is GL bottom-up; flip to top-down to match the RTT. */
    for (int y = 0; y < H; y++)
        memcpy(&ref[(size_t)y * W * 4],
               &s_nr.readback[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
    /* Empty native scene (load/transition frames): mirror the reference so -vk
     * shows the framebuffer and -diff reads ~zero instead of a flat clear. */
    if (scene_n <= 0) memcpy(nat, ref, frame_px * 4);

    /* -ab: native on the left, reference on the right (2W wide). */
    for (int y = 0; y < H; y++) {
        memcpy(&ab[(size_t)y * (2 * W) * 4],         &nat[(size_t)y * W * 4], (size_t)W * 4);
        memcpy(&ab[((size_t)y * (2 * W) + W) * 4],   &ref[(size_t)y * W * 4], (size_t)W * 4);
    }
    /* -diff: per-channel absolute difference, opaque. */
    for (size_t i = 0; i < frame_px; i++) {
        for (int c = 0; c < 3; c++) {
            int d = (int)nat[i * 4 + c] - (int)ref[i * 4 + c];
            dif[i * 4 + c] = (unsigned char)(d < 0 ? -d : d);
        }
        dif[i * 4 + 3] = 0xFF;
    }

    int rc = 0;
    char path[1100];
    path_with_suffix(base, "-vk", path, sizeof(path));
    if (live) {   /* vk-only widescreen: the live offscreen (window-sized, Hor+) */
        if (openpete_write_png_rgba(path, live, live_w, live_h) == 0)
            OPL_INFO_F("native", "vk screenshot %dx%d (widescreen) -> %s", live_w, live_h, path);
        else rc = 1;
    } else if (openpete_write_png_rgba(path, nat, W, H) == 0)
        OPL_INFO_F("native", "vk screenshot %dx%d -> %s", W, H, path);
    else rc = 1;
    path_with_suffix(base, "-ab", path, sizeof(path));
    if (openpete_write_png_rgba(path, ab, 2 * W, H) == 0)
        OPL_INFO_F("native", "ab screenshot %dx%d -> %s", 2 * W, H, path);
    else rc = 1;
    path_with_suffix(base, "-diff", path, sizeof(path));
    if (openpete_write_png_rgba(path, dif, W, H) == 0)
        OPL_INFO_F("native", "diff screenshot %dx%d -> %s", W, H, path);
    else rc = 1;

    free(nat); free(ref); free(ab); free(dif); free(live);
    return rc;
}

int openpete_native_render_screenshot(const char *path)
{
    if (!s_nr.enabled || !path) return 1;
    size_t cap = (size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u;
    if (cap == 0) return 1;
    unsigned char *buf = malloc(cap);
    if (!buf) return 1;
    int w = 0, h = 0, rc = 1;
    if (rhi_read_offscreen(s_nr.dev, buf, cap, &w, &h))
        rc = openpete_write_png_rgba(path, buf, w, h);
    free(buf);
    if (rc == 0)
        OPL_INFO_F("native", "A/B screenshot %dx%d -> %s", w, h, path);
    return rc;
}

/* --record shutdown drain: the async ring still holds the last few presents
 * (queued but not yet cycled out). Flush them to the encoder, in PTS order,
 * before openpete_record_shutdown() finalizes the file — otherwise the tail of
 * the recording is lost. Blocks once (the device is torn down right after). */
void openpete_native_render_record_flush(void)
{
    if (!s_nr.enabled || !openpete_record_active())
        return;
    unsigned char *buf = recbuf_ensure((size_t)s_nr.win_w * (size_t)s_nr.win_h * 4u);
    if (!buf) return;
    int w = 0, h = 0; uint64_t tag = 0;
    while (rhi_drain_readback(s_nr.dev, buf, s_reccap, &w, &h, &tag) && w > 0 && h > 0)
        openpete_record_submit_frame(buf, w, h, /*top_down=*/1, tag);
}

void openpete_native_render_shutdown(void)
{
    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.ref_tex)      rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    if (s_nr.native_rtt)   rhi_destroy_texture(s_nr.dev, s_nr.native_rtt);
    if (s_nr.frozen_tex)   rhi_destroy_texture(s_nr.dev, s_nr.frozen_tex);
    if (s_nr.ss_rtt)       rhi_destroy_texture(s_nr.dev, s_nr.ss_rtt);
    if (s_nr.ovl_ss)       rhi_destroy_texture(s_nr.dev, s_nr.ovl_ss);
    if (s_nr.ovl_quad_vb)  rhi_destroy_buffer(s_nr.dev, s_nr.ovl_quad_vb);
    if (s_nr.ovl_resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.ovl_resolve_pipe);
    if (s_nr.vram_tex)     rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.diff_quad_vb) rhi_destroy_buffer(s_nr.dev, s_nr.diff_quad_vb);
    if (s_nr.fs_quad_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.fs_quad_vb);
    if (s_nr.overlay_vb)   rhi_destroy_buffer(s_nr.dev, s_nr.overlay_vb);
    if (s_nr.scene_vb)     rhi_destroy_buffer(s_nr.dev, s_nr.scene_vb);
    if (s_nr.panels_vb)    rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    if (s_nr.freeze_overlay_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.freeze_overlay_pipe);
    if (s_nr.resolve_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.resolve_pipe);
    if (s_nr.diff_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.diff_pipe);
    if (s_nr.scene_tex_hud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_hud_pipe);
    if (s_nr.gouraud_hud_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_sub_pipe);
    if (s_nr.gouraud_hud_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_blend_pipe);
    if (s_nr.gouraud_hud_pipe)   rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_hud_pipe);
    if (s_nr.scene_tex_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_sub_pipe);
    if (s_nr.scene_tex_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_add_pipe);
    if (s_nr.scene_tex_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_blend_pipe);
    if (s_nr.scene_tex_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.scene_tex_pipe);
    if (s_nr.gouraud_noz_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_noz_pipe);
    if (s_nr.gouraud_add_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_add_pipe);
    if (s_nr.gouraud_sub_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_sub_pipe);
    if (s_nr.gouraud_blend_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_blend_pipe);
    if (s_nr.gouraud_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.gouraud_pipe);
    if (s_nr.vram_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.vram_pipe);
    if (s_nr.blit_pipe)    rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);
    free(s_nr.vram_mirror);
    free(s_recbuf); s_recbuf = NULL; s_reccap = 0;   /* --record async ring host buffer */

    /* --render-stats aggregate over the whole run. */
    if (s_diff_stats.frames > 0 && s_diff_stats.n > 0) {
        double inv = 1.0 / (double)s_diff_stats.n;
        OPL_INFO_F("native", "diff-stats AGGREGATE over %u frames: mean=(%.2f,%.2f,%.2f) "
                   "max=(%d,%d,%d) within%d=%.2f%% diff-px=%.1f%% (avg %llu/frame)",
                   s_diff_stats.frames,
                   s_diff_stats.sum[0]*inv, s_diff_stats.sum[1]*inv, s_diff_stats.sum[2]*inv,
                   s_diff_stats.max[0], s_diff_stats.max[1], s_diff_stats.max[2],
                   DIFF_STATS_TOL, 100.0 * (double)s_diff_stats.within * inv,
                   100.0 * (double)s_diff_stats.off * inv,
                   (unsigned long long)(s_diff_stats.off / s_diff_stats.frames));
    }
    free(s_stats_buf);
    s_stats_buf = NULL; s_stats_cap = 0;
    memset(&s_diff_stats, 0, sizeof(s_diff_stats));

    memset(&s_nr, 0, sizeof(s_nr));
    OPL_INFO_F("native", "Vulkan native renderer window shut down");
}
