// games/common/freecam_game.c — game-side body of the debug free camera (F),
// shared by every game target. See freecam_game.h.
//
// Technique (from C0mposer's Spyro-1-Practice-Rom, src/freecam.c):
//
//   1. Freeze the camera-follow and player-update functions so nothing
//      overwrites the camera each tick. psx_override_dispatch_game returning
//      1 for a function entry makes the generated body early-return
//      (core/psx/dispatch.c); freecam_should_freeze() drives that.
//
//   2. Hold the camera pose host-side and stamp it into the game's camera
//      bytes at the follow function's own tick point, in its place. A writer
//      that runs before the follow is overwritten by the stamp; only a writer
//      between the follow and the draw would need freezing.
//
//   3. The game's own per-tick matrix setup, which still runs, rebuilds the
//      view matrix from the bytes we wrote, so both the reference rasterizer
//      and the native renderer follow the free camera with no matrix math
//      duplicated here.
//
// The host pose is seeded from RAM at the first frozen follow entry after an
// engage (so the camera starts where the game left it) and after any follow
// entry that was not frozen. It is not savestated: the axes, the scripted
// sweeps and the path counter apply only on committed ticks, since a
// speculated (runahead) or replayed (rewind) tick would otherwise move the
// pose permanently where the RAM restore undoes everything else.
//
// Debug tool: OFF by default (zero writes, oracle-clean). ON is an
// intentional, opt-in gameplay-state divergence for inspecting a level.

#include "freecam_game.h"
#include "game_extras.h"        /* game_is_gameplay_live */
#include "openpete_freecam.h"
#include "openpete_fiber.h"     /* openpete_speculative / openpete_replaying */
#include "openpete_host_mouse.h" /* probe log */
#include "openpete_log.h"
#include "psx_runtime_internal.h"        /* CPUState + its read/write callbacks */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#  define M_PI    3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2  1.57079632679489661923
#endif

/* Angle units: a full turn is 0x1000 (4096). */
#define ANGLE_FULL   4096.0

#define D (&freecam_desc)

/* ---- Stored-angle codec ---------------------------------------------------
 * A 16-bit game keeps int16 angles; a 12-bit game keeps 0..0xFFF and masks
 * at every consumer. Both are held here as signed int32 wrapped to the
 * game's width so arithmetic and logging read the same either way. */
static int32_t angle_wrap(int32_t v) {
    if (D->angle_bits == 12) return ((v + 0x800) & 0xFFF) - 0x800;
    return (int32_t)(int16_t)(uint16_t)(uint32_t)v;
}
static int32_t angle_read(CPUState* cpu, uint32_t addr) {
    return angle_wrap((int32_t)(uint32_t)cpu->read_half(addr));
}
static uint16_t angle_stored(int32_t v) {
    if (D->angle_bits == 12) return (uint16_t)(v & 0xFFF);
    return (uint16_t)(int16_t)v;
}

/* ---- Host-held pose -------------------------------------------------------
 * Stored units (the game's own), so a seed/stamp round trip is exact. */
static struct {
    int32_t  x, y, z, yaw, pitch;
    int      seeded;
    unsigned gen_seen;
} s_pose;

static void pose_seed(CPUState* cpu) {
    s_pose.x     = (int32_t)cpu->read_word(D->cam_pos_x);
    s_pose.y     = (int32_t)cpu->read_word(D->cam_pos_y);
    s_pose.z     = (int32_t)cpu->read_word(D->cam_pos_z);
    s_pose.yaw   = angle_read(cpu, D->cam_yaw);
    s_pose.pitch = angle_read(cpu, D->cam_pitch);
    s_pose.seeded   = 1;
    s_pose.gen_seen = openpete_freecam_engage_generation();
}

static void pose_seed_if_needed(CPUState* cpu) {
    if (!s_pose.seeded || s_pose.gen_seen != openpete_freecam_engage_generation())
        pose_seed(cpu);
}

static void pose_stamp(CPUState* cpu) {
    s_pose.yaw   = angle_wrap(s_pose.yaw);
    s_pose.pitch = angle_wrap(s_pose.pitch);
    cpu->write_word(D->cam_pos_x, (uint32_t)s_pose.x);
    cpu->write_word(D->cam_pos_y, (uint32_t)s_pose.y);
    cpu->write_word(D->cam_pos_z, (uint32_t)s_pose.z);
    cpu->write_half(D->cam_yaw,   angle_stored(s_pose.yaw));
    cpu->write_half(D->cam_pitch, angle_stored(s_pose.pitch));
}

static int level_live(CPUState* cpu) {
    return D->level_live ? D->level_live(cpu) : game_is_gameplay_live(cpu);
}

/* Pull up to `max` signed integers out of arbitrary text, skipping any
 * non-numeric runs. Lenient on purpose so a whole freecam log line can be
 * pasted verbatim — "pos=(84992, 45582, 9948) yaw=1024 pitch=160" yields all
 * five. Returns the count parsed. */
static int freecam_parse_ints(const char* s, int32_t* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int is_num = (*s >= '0' && *s <= '9') ||
                     ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9');
        if (!is_num) { s++; continue; }
        char* end;
        long v = strtol(s, &end, 10);
        out[n++] = (int32_t)v;
        s = end;
    }
    return n;
}

/* Resolve a named pose through the game's resolver. A game without one
 * rejects every name. */
static int resolve_named(CPUState* cpu, const char* name, int32_t v[5]) {
    if (!D->resolve_named_pose) {
        OPL_WARN_F("freecam", "named pose '%s': this game has no named poses", name);
        return 0;
    }
    return D->resolve_named_pose(cpu, name, v);
}

/* ---- Scripted flight path (OPENPETE_FREECAM_PATH / --freecam-path) --------
 *
 * A static pose reveals a whole level in its first frame, so every first-use
 * resource it needs is created at once and nothing is created afterwards. The
 * costs that only *movement* reaches — terrain sector cache eviction and
 * refill, LOD tier crossings, moby extraction churn — are invisible to it.
 * This flies the camera instead: waypoints are linearly interpolated over
 * `ticks` canonical ticks per segment, and the path loops so a run of any
 * length stays in motion.
 *
 * Spec: "[ticks=N;] WP(;WP)+" where WP is "x,y,z[,yaw,pitch]" or a name the
 * game's resolver knows ("topdown", "sweepN"). Names resolve against the live
 * level once it is up, so a path can dive from above the level into a flight
 * across it without the caller knowing any level's bounds. */

#define FREECAM_PATH_MAX 16
#define FREECAM_PATH_DEFAULT_TICKS 120
#define FREECAM_NAME_MAX 32

static struct {
    int32_t wp[FREECAM_PATH_MAX][5];   /* x, y, z, yaw, pitch */
    char    named[FREECAM_PATH_MAX][FREECAM_NAME_MAX];  /* "" = numeric */
    int     n;
    int     ticks;      /* ticks per segment */
    int     t;          /* tick index within the current segment */
    int     seg;
    int     resolved;   /* named waypoints filled in from the live level */
    int     active;
} s_path;

/* Parse the spec into s_path. Returns 1 if a usable path (>= 2 waypoints)
 * was found. Waypoint text is bounded-copied so a malformed spec cannot run
 * off the end of the segment. */
static int freecam_path_parse(const char* spec) {
    memset(&s_path, 0, sizeof s_path);
    s_path.ticks = FREECAM_PATH_DEFAULT_TICKS;

    const char* p = spec;
    while (*p && s_path.n < FREECAM_PATH_MAX) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char seg[128];
        if (len >= sizeof seg) len = sizeof seg - 1;
        memcpy(seg, p, len);
        seg[len] = '\0';

        const char* tk = strstr(seg, "ticks=");
        if (tk) {
            int v = atoi(tk + 6);
            if (v > 0) s_path.ticks = v;
        } else {
            int32_t v[5] = {0, 0, 0, 0, 0};
            if (freecam_parse_ints(seg, v, 5) >= 3) {
                memcpy(s_path.wp[s_path.n], v, sizeof v);
                s_path.n++;
            } else {
                /* A name: the trimmed segment. */
                char* q = seg;
                while (*q == ' ') q++;
                char* e = q + strlen(q);
                while (e > q && e[-1] == ' ') *--e = '\0';
                if (*q && !D->resolve_named_pose) {
                    OPL_WARN_F("freecam", "--freecam-path: named waypoint '%s' but "
                               "this game has no named poses", q);
                    return 0;
                }
                if (*q) {
                    strncpy(s_path.named[s_path.n], q, FREECAM_NAME_MAX - 1);
                    s_path.n++;
                }
            }
        }
        if (!end) break;
        p = end + 1;
    }

    if (s_path.n < 2) {
        OPL_WARN_F("freecam",
                   "--freecam-path needs at least 2 waypoints (got %d)", s_path.n);
        return 0;
    }
    s_path.active = 1;
    OPL_INFO_F("freecam", "flight path armed: %d waypoint(s), %d tick(s) per segment",
               s_path.n, s_path.ticks);
    return 1;
}

/* Fill in any named waypoints once the level is live. */
static int freecam_path_resolve(CPUState* cpu) {
    if (s_path.resolved) return 1;
    for (int i = 0; i < s_path.n; i++) {
        int32_t v[5] = {0, 0, 0, 0, 0};
        if (!s_path.named[i][0]) continue;
        if (!resolve_named(cpu, s_path.named[i], v)) return 0;  /* not live yet */
        memcpy(s_path.wp[i], v, sizeof v);
    }
    for (int i = 0; i < s_path.n; i++)
        OPL_INFO_F("freecam", "path wp%d = (%d, %d, %d) yaw=%d pitch=%d", i,
                   s_path.wp[i][0], s_path.wp[i][1], s_path.wp[i][2],
                   s_path.wp[i][3], s_path.wp[i][4]);
    s_path.resolved = 1;
    return 1;
}

/* Shortest-arc interpolation for the game's Euler angles: going from
 * 32000 to -32000 is 1536 units forward, not 64000 units back. */
static int32_t freecam_lerp_angle(int32_t a, int32_t b, int num, int den) {
    int32_t d = angle_wrap(b - a);
    return angle_wrap(a + (d * num) / den);
}

/* Advance one tick along the path into the host pose. The counter advances
 * on committed ticks only. */
static void freecam_path_step(CPUState* cpu, int committed) {
    if (!freecam_path_resolve(cpu)) return;

    const int32_t* a = s_path.wp[s_path.seg];
    const int32_t* b = s_path.wp[(s_path.seg + 1) % s_path.n];
    const int num = s_path.t, den = s_path.ticks;

    s_pose.x     = a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * num) / den);
    s_pose.y     = a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * num) / den);
    s_pose.z     = a[2] + (int32_t)(((int64_t)(b[2] - a[2]) * num) / den);
    s_pose.yaw   = freecam_lerp_angle(a[3], b[3], num, den);
    s_pose.pitch = freecam_lerp_angle(a[4], b[4], num, den);

    if (committed && ++s_path.t >= s_path.ticks) {
        s_path.t = 0;
        s_path.seg = (s_path.seg + 1) % s_path.n;
    }
}

/* Apply an OPENPETE_FREECAM_POSE string ("x,y,z[,yaw,pitch]", any text with
 * >= 3 integers in it, or a name the game's resolver knows) to the host
 * pose. Position is required; yaw/pitch optional. */
static void freecam_apply_pose(CPUState* cpu, const char* s) {
    int32_t v[5] = {0, 0, 0, 0, 0};
    int n = freecam_parse_ints(s, v, 5);
    if (n < 3) {
        char name[FREECAM_NAME_MAX];
        const char* q = s;
        while (*q == ' ') q++;
        strncpy(name, q, sizeof name - 1);
        name[sizeof name - 1] = '\0';
        for (char* e = name + strlen(name); e > name && e[-1] == ' ';) *--e = '\0';
        if (!name[0] || !resolve_named(cpu, name, v)) {
            OPL_WARN_F("freecam",
                       "OPENPETE_FREECAM_POSE needs at least x,y,z or a named pose (got %d numbers)", n);
            return;
        }
        n = 5;
        s_pose.x = v[0]; s_pose.y = v[1]; s_pose.z = v[2];
        s_pose.yaw = angle_wrap(v[3]); s_pose.pitch = angle_wrap(v[4]);
        OPL_INFO_F("freecam", "%s pose set to (%d, %d, %d) yaw=%d pitch=%d",
                   name, v[0], v[1], v[2], v[3], v[4]);
        return;
    }
    s_pose.x = v[0]; s_pose.y = v[1]; s_pose.z = v[2];
    if (n >= 4) s_pose.yaw   = angle_wrap(v[3]);
    if (n >= 5) s_pose.pitch = angle_wrap(v[4]);
    OPL_INFO_F("freecam", "pose set to (%d, %d, %d) yaw=%d pitch=%d",
               v[0], v[1], v[2], v[3], v[4]);
}

/* A scripted pose (OPENPETE_FREECAM_POSE / --freecam-pose) waiting to be
 * stamped. The engage hook runs before the guest fiber, so a pose written
 * there lands one frame point too early: the camera position jumps while the
 * matrices the guest builds this frame are still the follow camera's, and the
 * frame is snapshot in that half-applied state. Held here instead and stamped
 * from freecam_drive, so position and matrices settle within one frame. */
static const char* s_pending_pose = NULL;

/* Drive the host pose from input and stamp it. Runs IN PLACE OF the game's
 * camera follow (see freecam_should_freeze), i.e. at the exact tick point the
 * follow logic normally runs, so the camera we write is what the downstream
 * matrix setup and world renderer consume that same tick. */
static void freecam_drive(CPUState* cpu) {
    const int committed = !openpete_speculative && !openpete_replaying;

    pose_seed_if_needed(cpu);

    /* Stamp a queued scripted pose here, at the follow logic's own tick
     * point, before the axes are read: the same tick's matrix setup then
     * consumes it, so no frame renders the new position under the old
     * orientation. */
    if (s_pending_pose) {
        const char* pose = s_pending_pose;
        s_pending_pose = NULL;
        freecam_apply_pose(cpu, pose);
    }
    /* A scripted path owns the camera outright — the interactive axes would
     * fight it, and a headless run has none to read anyway. */
    if (s_path.active) {
        freecam_path_step(cpu, committed);
        pose_stamp(cpu);
        if (D->draw_all) D->draw_all(cpu);
        return;
    }

    openpete_freecam_axes ax;
    openpete_freecam_get_axes(&ax);

    const int si    = openpete_freecam_speed_index();  /* pre-clamped */
    const int speed = D->move_speed[si];
    const int turn  = D->turn_speed[si];

    /* --- Look: modify the logical yaw + pitch. Turning right lowers yaw,
     * looking up lowers pitch ("positive pitch = look down"); the descriptor
     * maps the logical angles onto the game's stored ones. --- */
    int32_t yaw   = (s_pose.yaw - D->yaw_offset) * D->yaw_sign;
    int32_t pitch = s_pose.pitch * D->pitch_sign;
    if (committed) {
        yaw   -= (int32_t)lround((double)ax.look_yaw   * turn);
        pitch -= (int32_t)lround((double)ax.look_pitch * turn);
    }

    /* Mouse look. The host loop accumulates the look axes' mouse counts into
     * running totals; this consumer keeps the total at its last rebase and
     * the angle already applied, and on each COMMITTED tick applies
     * round(k * sensitivity * (total - base)) - applied. Speculated
     * (runahead) and replayed (rewind) ticks apply nothing: they are
     * re-executed or erased, and the committed tick that follows sees the
     * whole motion. The pose after N ticks is exact between rebases; a rebase
     * happens on engage (generation counter), on a sensitivity change, and,
     * for pitch, on a clamped apply so reversing the mouse moves at once. */
    static long long mb_x, mb_y, ma_x, ma_y;
    static unsigned  gen_seen = ~0u;
    static float     sens_seen = -1.0f;
    long long mdx = 0, mdy = 0, tx = 0, ty = 0;
    if (committed) {
        openpete_freecam_look_counts(&tx, &ty);
        const unsigned gen  = openpete_freecam_engage_generation();
        const float    sens = openpete_freecam_mouse_sensitivity();
        if (gen != gen_seen || sens != sens_seen) {
            mb_x = tx; mb_y = ty; ma_x = 0; ma_y = 0;
            gen_seen = gen; sens_seen = sens;
        }
        const long long target_x = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(tx - mb_x));
        const long long target_y = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(ty - mb_y));
        mdx = target_x - ma_x; mdy = target_y - ma_y;
        ma_x = target_x; ma_y = target_y;
        /* Mouse right turns right (lowers yaw); the default `mouse:-y`
         * spelling makes mouse up a positive count, which looks up (lowers
         * pitch). */
        yaw   -= (int32_t)mdx;
        pitch -= (int32_t)mdy;
    }
    /* OPENPETE_FREECAM_SPIN=<yaw-units-per-tick>: scripted continuous yaw
     * sweep (4096 = full turn), on top of input — lets one run profile every
     * view direction. */
    {
        static int s_have_spin = -1; static int s_spin;
        if (s_have_spin < 0) {
            const char* e = getenv("OPENPETE_FREECAM_SPIN");
            s_spin = e ? atoi(e) : 0;
            s_have_spin = 1;
        }
        if (committed) yaw += s_spin;
    }
    if (pitch > D->pitch_limit || pitch < -D->pitch_limit) {
        pitch = pitch > 0 ? D->pitch_limit : -D->pitch_limit;
        if (committed) { mb_y = ty; ma_y = 0; }   /* rebase pitch on clamp */
    }
    if (mdx || mdy)
        openpete_host_mouse_probe_log("freecam", "mouse apply dx=%lld dy=%lld -> yaw=%d pitch=%d",
                                      mdx, mdy, (int)angle_wrap(yaw), (int)angle_wrap(pitch));
    s_pose.yaw   = angle_wrap(yaw * D->yaw_sign + D->yaw_offset);
    s_pose.pitch = angle_wrap(pitch * D->pitch_sign);

    /* --- Move: modify the int32 position. Forward/right basis is the
     * practice ROM's: forward from (yaw, pitch), right = yaw − 90° on the
     * horizontal plane, and Space/Shift + forward-pitch move along world Z.
     * Freecam is already non-canonical, so host float trig is fine here (no
     * need to route through the game's fixed-point sin/cos). --- */
    int32_t px = s_pose.x;
    int32_t py = s_pose.y;
    int32_t pz = s_pose.z;

    const double ry = (double)yaw   * (2.0 * M_PI / ANGLE_FULL);
    const double rp = (double)pitch * (2.0 * M_PI / ANGLE_FULL);
    const double cp = cos(rp);
    const double fwdx =  cos(ry) * cp;
    const double fwdy =  sin(ry) * cp;
    const double fwdz = -sin(rp);
    const double rgtx =  cos(ry - M_PI_2);
    const double rgty =  sin(ry - M_PI_2);

    if (committed) {
        const double mvx = (ax.move_fwd * fwdx + ax.move_strafe * rgtx) * speed;
        const double mvy = (ax.move_fwd * fwdy + ax.move_strafe * rgty) * speed;
        const double mvz = (ax.move_fwd * fwdz) * speed + (double)ax.move_up * speed;

        px += (int32_t)lround(mvx);
        py += (int32_t)lround(mvy);
        pz += (int32_t)lround(mvz);
    }

    /* OPENPETE_FREECAM_DRIFT="dx,dy,dz": scripted per-tick camera drift
     * (world units), applied on top of input. Deterministic boundary-crossing
     * repro for cull/handoff work — headless runs can sweep the camera across
     * an admission edge without a human at the stick. */
    {
        static int32_t s_drift[3]; static int s_have_drift = -1;
        if (s_have_drift < 0) {
            const char* e = getenv("OPENPETE_FREECAM_DRIFT");
            s_have_drift = 0;
            if (e && *e) {
                char* end;
                s_drift[0] = (int32_t)strtol(e, &end, 10);
                if (*end) s_drift[1] = (int32_t)strtol(end + 1, &end, 10);
                if (*end) s_drift[2] = (int32_t)strtol(end + 1, &end, 10);
                s_have_drift = 1;
            }
        }
        if (s_have_drift && committed) { px += s_drift[0]; py += s_drift[1]; pz += s_drift[2]; }
    }

    s_pose.x = px; s_pose.y = py; s_pose.z = pz;
    pose_stamp(cpu);

    /* Log the live camera while engaged, but only when it changed since the
     * last log (no console spam while parked). */
    {
        int32_t cyaw = s_pose.yaw, cpitch = s_pose.pitch;
        static int      s_have = 0;
        static int32_t  l_x, l_y, l_z, l_yaw, l_pitch;
        if (!s_have || px != l_x || py != l_y || pz != l_z ||
            cyaw != l_yaw || cpitch != l_pitch) {
            s_have = 1;
            l_x = px; l_y = py; l_z = pz; l_yaw = cyaw; l_pitch = cpitch;
            OPL_INFO_F("freecam", "pos=(%d, %d, %d) yaw=%d pitch=%d",
                       px, py, pz, (int)cyaw, (int)cpitch);
        }
    }

    /* Draw all sectors in view, not just the camera's region (no pop-in). */
    if (D->draw_all) D->draw_all(cpu);
}

/* Move the player to wherever the free camera is floating. Camera and player
 * share one world space, so the position copies straight across.
 *
 * The previous position is written to match: the physics derives this tick's
 * movement from it, so leaving it at the pre-teleport spot would present the
 * whole jump as one tick of travel — a sweep across the level that collides
 * with whatever lies between the two points.
 *
 * Momentum is cancelled through the game's own routine (reset_momentum)
 * rather than by zeroing fields here: whatever the player carried when the
 * camera was engaged belongs to the place he left, not the one he lands in.
 * Everything else — state, animation, health, facing — is deliberately
 * untouched: the player carries on falling / gliding / standing as he was,
 * from the new spot. The player update resumes next tick, once the camera is
 * disengaged, and re-derives the floor and wall queries from the new
 * position. */
static void freecam_teleport_player_to_camera(CPUState* cpu) {
    pose_seed_if_needed(cpu);
    const int32_t x = s_pose.x, y = s_pose.y, z = s_pose.z;

    cpu->write_word(D->player_pos + 0, (uint32_t)x);
    cpu->write_word(D->player_pos + 4, (uint32_t)y);
    cpu->write_word(D->player_pos + 8, (uint32_t)z);

    if (D->player_prev_pos) {
        cpu->write_word(D->player_prev_pos + 0, (uint32_t)x);
        cpu->write_word(D->player_prev_pos + 4, (uint32_t)y);
        cpu->write_word(D->player_prev_pos + 8, (uint32_t)z);
    }

    if (D->reset_momentum) D->reset_momentum(cpu);
    else OPL_INFO_F("freecam", "teleport keeps momentum: this game has no reset routine");

    OPL_INFO_F("freecam", "teleported player to (%d, %d, %d)", x, y, z);
}

/* Optional scripted / headless engage. Set OPENPETE_FREECAM=1 to engage the
 * free camera, or OPENPETE_FREECAM_POSE="x,y,z,yaw,pitch" to engage AND jump to
 * a specific pose (for reproducing a test view — the coords come straight from
 * the per-tick freecam log). Fires once the level is live — deliberately NOT
 * from boot: engaging during a level load / demo transition would freeze the
 * player update while the level is still setting up and leave it un-rendered.
 * Interactive use (F / stick-click) is independent of this. */
void freecam_maybe_autoengage(CPUState* cpu) {
    static int s_done = 0;
    if (s_done) return;
    const char* pose = getenv("OPENPETE_FREECAM_POSE");
    const char* path = getenv("OPENPETE_FREECAM_PATH");
    const char* eng  = getenv("OPENPETE_FREECAM");
    int want = (pose && *pose) || (path && *path) ||
               (eng && *eng && *eng != '0');
    if (!want || openpete_freecam_active()) { s_done = 1; return; }
    if (level_live(cpu)) {
        s_done = 1;
        if (!openpete_freecam_toggle()) return;   /* refused (bk2 replay/record) */
        /* A path supersedes a single pose: it stamps the camera every tick,
         * so an initial jump would only be overwritten on the next one. */
        if (path && *path) freecam_path_parse(path);
        else if (pose && *pose) s_pending_pose = pose;
    }
}

static int in_extra_freeze(uint32_t addr) {
    if (!D->extra_freeze) return 0;
    for (const uint32_t* p = D->extra_freeze; *p; ++p)
        if (*p == addr) return 1;
    return 0;
}

int freecam_should_freeze(CPUState* cpu, uint32_t addr) {
    if (!openpete_freecam_active()) return 0;

    /* The player's own update: suppress it entirely so the scene holds
     * still. */
    if (addr == D->player_update || in_extra_freeze(addr))
        return !D->freeze_only_when_live || level_live(cpu);

    /* The camera follow: replace it with our drive (same fiber, same tick
     * point), then suppress the original so it can't overwrite the camera. */
    if (addr == D->cam_follow) {
        if (D->freeze_only_when_live && !level_live(cpu)) {
            /* The follow runs and moves the camera; the host pose is re-read
             * at the next frozen entry. */
            s_pose.seeded = 0;
            return 0;
        }
        /* A pending T: move the player up to the camera and hand the camera
         * back. Disengaging here rather than on the host loop keeps the two
         * halves atomic — the same tick that moves the player is the one
         * that stops freezing the player update. Returning 0 then lets the
         * real follow run immediately, so it re-derives the follow camera
         * from the player's new position on this very tick and there's no
         * frame of stale free-camera view before it snaps back. */
        if (openpete_freecam_take_teleport_request()) {
            freecam_teleport_player_to_camera(cpu);
            openpete_freecam_toggle();
            s_pose.seeded = 0;
            return 0;
        }
        freecam_drive(cpu);
        return 1;
    }
    return 0;
}
