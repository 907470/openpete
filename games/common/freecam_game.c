// games/common/freecam_game.c - game-side body of the debug free camera (F),
// shared by every game target. See freecam_game.h.
//
// Technique (from C0mposer's Spyro-1-Practice-Rom, src/freecam.c):
//
//   1. Freeze the camera-follow and player-update functions so nothing
//      overwrites the camera each tick. The game's override_dispatch returning
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
#include "game_layout.h"        /* game_layout.cam: the pose bytes */
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
/** @brief Wrap an angle to the game's stored width as a signed value.
 *
 * @param v The angle in stored units, any range.
 * @return v wrapped to [-0x800, 0x7FF] for a 12-bit game, else to int16. */
static int32_t angle_wrap(int32_t v) {
    if (D->angle_bits == 12) return ((v + 0x800) & 0xFFF) - 0x800;
    return (int32_t)(int16_t)(uint16_t)(uint32_t)v;
}
/** @brief Read a stored angle from guest RAM as a wrapped signed value.
 *
 * @param cpu The guest CPU state.
 * @param addr The address of the 16-bit angle.
 * @return The angle after angle_wrap. */
static int32_t angle_read(CPUState* cpu, uint32_t addr) {
    return angle_wrap((int32_t)(uint32_t)cpu->read_half(addr));
}
/** @brief Encode an angle in the game's stored form.
 *
 * @param v The angle in stored units.
 * @return The low 12 bits for a 12-bit game, else the low 16 bits. */
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

/** @brief Load the host pose from the game's camera bytes.
 *
 * Reads position, yaw and pitch from game_layout.cam and records the current
 * engage generation.
 * @param cpu The guest CPU state. */
static void pose_seed(CPUState* cpu) {
    s_pose.x     = (int32_t)cpu->read_word(game_layout.cam.position + 0u);
    s_pose.y     = (int32_t)cpu->read_word(game_layout.cam.position + 4u);
    s_pose.z     = (int32_t)cpu->read_word(game_layout.cam.position + 8u);
    s_pose.yaw   = angle_read(cpu, game_layout.cam.rotation + 4u);
    s_pose.pitch = angle_read(cpu, game_layout.cam.rotation + 2u);
    s_pose.seeded   = 1;
    s_pose.gen_seen = openpete_freecam_engage_generation();
}

/** @brief Seed the host pose when it is unseeded or the freecam engaged again since the seed.
 *
 * @param cpu The guest CPU state. */
static void pose_seed_if_needed(CPUState* cpu) {
    if (!s_pose.seeded || s_pose.gen_seen != openpete_freecam_engage_generation())
        pose_seed(cpu);
}

/** @brief Write the host pose into the game's camera bytes.
 *
 * Wraps yaw and pitch to the game's angle width first, then stores position
 * as words and the two angles as halfwords under game_layout.cam.
 * @param cpu The guest CPU state. */
static void pose_stamp(CPUState* cpu) {
    s_pose.yaw   = angle_wrap(s_pose.yaw);
    s_pose.pitch = angle_wrap(s_pose.pitch);
    cpu->write_word(game_layout.cam.position + 0u, (uint32_t)s_pose.x);
    cpu->write_word(game_layout.cam.position + 4u, (uint32_t)s_pose.y);
    cpu->write_word(game_layout.cam.position + 8u, (uint32_t)s_pose.z);
    cpu->write_half(game_layout.cam.rotation + 4u, angle_stored(s_pose.yaw));
    cpu->write_half(game_layout.cam.rotation + 2u, angle_stored(s_pose.pitch));
}

/** @brief Report whether a level is running.
 *
 * Uses the game's own predicate when its descriptor has one, else
 * game_is_gameplay_live.
 * @param cpu The guest CPU state.
 * @return Non-zero while a level is live. */
static int level_live(CPUState* cpu) {
    return D->level_live ? D->level_live(cpu) : game_is_gameplay_live(cpu);
}

/** @brief Extract up to max signed decimal integers from text, skipping every non-numeric run.
 *
 * A sign counts only directly before a digit. The leniency lets a freecam log
 * line be pasted verbatim: "pos=(84992, 45582, 9948) yaw=1024 pitch=160"
 * yields all five.
 * @param s The text.
 * @param out Receives the integers.
 * @param max Capacity of @p out.
 * @return The count parsed. */
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

/** @brief Resolve a named pose through the game's resolver.
 *
 * A game without a resolver rejects every name with a WARN.
 * @param cpu The guest CPU state.
 * @param name The pose name ("topdown", "sweepN").
 * @param v Receives {x, y, z, yaw, pitch} in stored units.
 * @return Non-zero on success; 0 for an unknown name, no live level, or no
 *         resolver. */
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
 * costs that only *movement* reaches - terrain sector cache eviction and
 * refill, LOD tier crossings, moby extraction churn - are invisible to it.
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

/** @brief Parse a flight-path spec into s_path and arm it.
 *
 * Segments split on ';'. A segment containing "ticks=" sets the ticks per
 * segment when positive; one with three or more integers is a numeric
 * waypoint, missing yaw and pitch reading 0; any other non-blank segment is
 * a named waypoint, resolved later. Each segment is copied into a 128-byte
 * buffer, truncating longer ones, and at most FREECAM_PATH_MAX waypoints are
 * kept. A named waypoint on a game without named poses refuses the spec.
 * @param spec The spec text.
 * @return 1 when two or more waypoints were parsed and the path is active,
 *         else 0. */
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

/** @brief Fill in the path's named waypoints from the live level, once.
 *
 * Logs every waypoint after the first success.
 * @param cpu The guest CPU state.
 * @return 1 once resolved; 0 while a name cannot resolve yet. */
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

/** @brief Interpolate between two stored angles along the shorter arc.
 *
 * For a 16-bit game, 32000 to -32000 is 1536 units forward, not 64000 back.
 * The fraction num / den truncates toward zero.
 * @param a The start angle.
 * @param b The end angle.
 * @param num Numerator of the fraction.
 * @param den Denominator of the fraction; non-zero.
 * @return The wrapped angle. */
static int32_t freecam_lerp_angle(int32_t a, int32_t b, int num, int den) {
    int32_t d = angle_wrap(b - a);
    return angle_wrap(a + (d * num) / den);
}

/** @brief Write the path's pose for the current tick into the host pose, then advance the counter.
 *
 * Interpolates linearly between the current segment's two waypoints, angles
 * along the shorter arc; the last segment loops to the first waypoint. Does
 * nothing while a named waypoint cannot resolve.
 * @param cpu The guest CPU state.
 * @param committed Non-zero on a committed tick, the only kind that advances
 *        the counter. */
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

/** @brief Set the host pose from an OPENPETE_FREECAM_POSE string.
 *
 * Text with three or more integers sets x, y, z and, when present, yaw and
 * pitch, keeping the current angles otherwise. Other text is trimmed and
 * resolved as a named pose, which sets all five. An unresolvable string is
 * logged and changes nothing.
 * @param cpu The guest CPU state.
 * @param s The pose text. */
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

/** @brief Move the host pose for one tick and stamp it into the camera bytes.
 *
 * Runs in place of the game's camera follow, at its tick point, so the same
 * tick's matrix setup and renderer consume the stamped camera. Seeds the pose
 * from RAM after an engage, applies a queued scripted pose, and, when a path
 * is active, takes the path's pose instead of input. Otherwise applies the
 * look axes, the mouse look, OPENPETE_FREECAM_SPIN and the pitch clamp, then
 * the move axes and OPENPETE_FREECAM_DRIFT. Input, mouse, spin and drift
 * apply on committed ticks only; the mouse look applies the difference
 * between the scaled count total since its last rebase and the angle already
 * applied, rebasing on engage, on a sensitivity change, and for pitch on a
 * clamp. A changed pose is logged. Calls the game's draw_all hook when set.
 * @param cpu The guest CPU state. */
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
    /* A scripted path owns the camera outright - the interactive axes would
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
     * sweep (4096 = full turn), on top of input - lets one run profile every
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
     * repro for cull/handoff work - headless runs can sweep the camera across
     * an admission edge from a script. */
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

/** @brief Place the player at a world position, optionally setting the facing and re-seating the follow camera.
 *
 * The previous position is written to match: the physics derives the tick's
 * movement from it, and an old previous position would present the jump as
 * one tick of travel that collides with everything between the two points.
 * The facing is written as the pair the game keeps, the 12-bit physics angle
 * and the 8-bit copy (angle >> 4) the player update re-derives from it each
 * tick; the byte alone lasts one tick, the angle alone leaves one tick drawn
 * at the old facing. Momentum is cancelled through the game's reset_momentum
 * routine. State, animation and health are untouched, and the next player
 * update re-derives the floor and wall queries from the new position. A
 * missing descriptor hook or address skips its step with a log line.
 * @param cpu The guest CPU state.
 * @param x World x.
 * @param y World y.
 * @param z World z.
 * @param yaw Facing, 4096 per turn, masked to 12 bits.
 * @param have_yaw Non-zero writes the facing.
 * @param seat_camera Non-zero calls the game's place_camera routine. */
static void freecam_teleport_player(CPUState* cpu, int32_t x, int32_t y, int32_t z,
                                    int32_t yaw, int have_yaw, int seat_camera) {
    cpu->write_word(D->player_pos + 0, (uint32_t)x);
    cpu->write_word(D->player_pos + 4, (uint32_t)y);
    cpu->write_word(D->player_pos + 8, (uint32_t)z);

    if (D->player_prev_pos) {
        cpu->write_word(D->player_prev_pos + 0, (uint32_t)x);
        cpu->write_word(D->player_prev_pos + 4, (uint32_t)y);
        cpu->write_word(D->player_prev_pos + 8, (uint32_t)z);
    }

    if (have_yaw) {
        if (D->player_facing) {
            const uint32_t a = (uint32_t)yaw & 0xFFFu;
            cpu->write_word(D->player_facing, a);
            if (D->player_facing_byte) cpu->write_byte(D->player_facing_byte, (uint8_t)(a >> 4));
        } else {
            OPL_WARN_F("freecam", "facing ignored: this game has no facing field located");
        }
    }

    if (D->reset_momentum) D->reset_momentum(cpu);
    else OPL_INFO_F("freecam", "teleport keeps momentum: this game has no reset routine");

    if (seat_camera) {
        if (D->place_camera) D->place_camera(cpu);
        else OPL_INFO_F("freecam", "camera left to converge: this game has no placement camera routine");
    }
}

/** @brief Move the player to the free camera's position.
 *
 * Camera and player share one world space, so the position copies across.
 * The facing is kept and the follow camera is not re-seated, so the follow
 * converges from the view that was being inspected.
 * @param cpu The guest CPU state. */
static void freecam_teleport_player_to_camera(CPUState* cpu) {
    pose_seed_if_needed(cpu);
    freecam_teleport_player(cpu, s_pose.x, s_pose.y, s_pose.z, 0, 0, 0);
    OPL_INFO_F("freecam", "teleported player to (%d, %d, %d)", s_pose.x, s_pose.y, s_pose.z);
}

/** @brief Report whether the player object can be moved.
 *
 * Uses the game's own predicate when its descriptor has one, else level_live.
 * @param cpu The guest CPU state.
 * @return Non-zero when a placement can apply. */
static int player_placeable(CPUState* cpu) {
    return D->player_placeable ? D->player_placeable(cpu) : level_live(cpu);
}

/* 1 while a scripted placement is still waiting for its tick. The freecam's
 * scripted engage waits on it: the freecam engages at level_live, before the
 * fly-in ends, and freezes the player update and the follow, so a placement
 * gated on the ordinary follow would never fire under it. */
static int s_player_pose_pending = -1;   /* -1: env not read yet */

/** @brief Report whether OPENPETE_PLAYER_POSE is set and not yet applied.
 *
 * Reads the variable on the first call and caches the result until
 * freecam_player_pose_maybe_apply clears it.
 * @return 1 while a placement is waiting, else 0. */
static int player_pose_pending(void) {
    if (s_player_pose_pending < 0) {
        const char* pose = getenv("OPENPETE_PLAYER_POSE");
        s_player_pose_pending = (pose && *pose) ? 1 : 0;
    }
    return s_player_pose_pending;
}

void freecam_player_pose_maybe_apply(CPUState* cpu) {
    if (!player_pose_pending()) return;
    if (!player_placeable(cpu)) return;
    s_player_pose_pending = 0;
    const char* pose = getenv("OPENPETE_PLAYER_POSE");
    int32_t v[5] = {0, 0, 0, 0, 0};
    const int n = freecam_parse_ints(pose, v, 5);
    if (n < 3 || n > 4) {
        OPL_WARN_F("freecam", "OPENPETE_PLAYER_POSE needs x,y,z or x,y,z,yaw (got %d numbers); not applied", n);
        return;
    }
    freecam_teleport_player(cpu, v[0], v[1], v[2], v[3], n == 4, 1);
    if (n == 4) OPL_INFO_F("freecam", "player placed at (%d, %d, %d) yaw=%d", v[0], v[1], v[2], v[3] & 0xFFF);
    else        OPL_INFO_F("freecam", "player placed at (%d, %d, %d)", v[0], v[1], v[2]);
}

int game_get_player_pose(CPUState* cpu, int32_t* out_x, int32_t* out_y, int32_t* out_z,
                         int32_t* out_yaw, int32_t* out_level) {
    if (!level_live(cpu)) return -1;
    if (!player_placeable(cpu)) return -2;
    *out_x = (int32_t)cpu->read_word(D->player_pos + 0);
    *out_y = (int32_t)cpu->read_word(D->player_pos + 4);
    *out_z = (int32_t)cpu->read_word(D->player_pos + 8);
    *out_yaw   = D->player_facing ? (int32_t)(cpu->read_word(D->player_facing) & 0xFFFu) : -1;
    *out_level = D->level_id ? (int32_t)cpu->read_word(D->level_id) : -1;
    return 0;
}

/** @brief Read OPENPETE_FREECAM_AT_VBL, the VBL the scripted engage waits for.
 *
 * A timed engage passes a replaying bk2's refusal; the movie desyncs from
 * that VBL on, since the camera follow is frozen, so it serves captures of a
 * state the movie reaches, taken at or just after the VBL.
 * @return The VBL, or -1 when unset, to engage as soon as the level is
 *         live. */
static int freecam_engage_vbl(void) {
    const char* at = getenv("OPENPETE_FREECAM_AT_VBL");
    return (at && *at) ? atoi(at) : -1;
}

/** @brief Engage the free camera for a scripted pose or path.
 *
 * With @p through_bk2 and a bk2 active, the bk2 flag is cleared around the
 * toggle so the movie's refusal does not apply, then restored.
 * @param through_bk2 True bypasses the bk2 refusal.
 * @return False when the engage was refused, else true. */
static bool freecam_engage_scripted(bool through_bk2) {
    const bool bk2 = through_bk2 && openpete_bk2_active();
    if (bk2) openpete_bk2_set_active(false);
    const bool engaged = openpete_freecam_toggle();
    if (bk2) openpete_bk2_set_active(true);
    return engaged;
}

/* Scripted engage from the environment; see freecam_game.h. */
void freecam_maybe_autoengage(CPUState* cpu) {
    static int s_done = 0;
    if (s_done) return;
    if (player_pose_pending()) return;   /* the placement fires first */
    const char* pose = getenv("OPENPETE_FREECAM_POSE");
    const char* path = getenv("OPENPETE_FREECAM_PATH");
    const char* eng  = getenv("OPENPETE_FREECAM");
    int want = (pose && *pose) || (path && *path) ||
               (eng && *eng && *eng != '0');
    if (!want || openpete_freecam_active()) { s_done = 1; return; }
    const int at_vbl = freecam_engage_vbl();
    if (at_vbl >= 0 && openpete_frame < (unsigned)at_vbl) return;
    if (level_live(cpu)) {
        s_done = 1;
        if (!freecam_engage_scripted(at_vbl >= 0)) return;   /* refused (bk2 replay/record) */
        /* A path supersedes a single pose: it stamps the camera every tick,
         * so an initial jump would only be overwritten on the next one. */
        if (path && *path) freecam_path_parse(path);
        else if (pose && *pose) s_pending_pose = pose;
    }
}

/** @brief Report whether a function entry is on the descriptor's extra freeze list.
 *
 * @param addr The function's guest address.
 * @return 1 when the zero-terminated extra_freeze list holds the address,
 *         else 0. */
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
         * halves atomic - the same tick that moves the player is the one
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
