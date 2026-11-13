/* moby_stage.glsl - the moby vertex stage's per-vertex projection: the
 * model-space lerp between two poses, the model-to-view rotation and
 * translation, the float perspective divide, the GTE divide-cap mirror and
 * the depths the scene stores, in fp32 with no Float64 capability.
 *
 * It computes what the CPU projection (moby.c moby_project_vertex, called by
 * the record passes and the shaded pass alike) writes per vertex, from the
 * same per-record terms rounded once to float:
 *   v    = p + (c - p) * alpha          the previous-to-current pose lerp
 *   r    = M v                          rotated offset, the env-UV input
 *   mac  = TR + r                       TR = tr + tr_lo, the double translation as two floats
 *   sx   = ofx + mac1 * H / z           z = mac3, or 1 at mac3 == 0
 *   unr  = !(mac3 > 0 && H_canon < 2 mac3)  the GTE's divide-overflow regime;
 *          inside it sx = clamp(ofx + sat16(mac1) * cap, -1024, 1023), the
 *          capped quotient and the SXY saturation (ngte_screen_gte)
 *   sat  = |mac1| or |mac2| past int16, mac3 <= 0, or H_canon >= 2 mac3
 *   tvz  = mac3 * unit                  the perspective W (unit = 2^scale byte on a
 *                                       record pass, times 32 / GPF byte on a GPF
 *                                       record; 2, or 64 / draw-scale byte, on the
 *                                       shaded pass)
 *   kvz  = mac3 * unit * depth_k
 *   vz   = kvz - tie_pull + lift, then - rho + lift2 (the sphere-shell pull)
 *   h    = (of * mac3 + mac12 * H) * unit, the homogeneous screen position
 * The record's near-floor lifts (lift, lift2) are per-record minima over
 * every vertex and arrive as instance terms. Every operation is `precise`:
 * no FMA contraction and one operation order in every pipeline that
 * compiles the stage.
 *
 * v, r and mac are carried as unevaluated float pairs (hi + lo) and rounded
 * once, so mac is the float nearest TR + M v to within a few units in its
 * own last place even where mac is small against TR and M v (a vertex near
 * the camera on a record far off-centre), where a plain float sum loses
 * everything below the float spacing of TR. The exact sum (Knuth) and the
 * exact product (Veltkamp split, Dekker) use only add, subtract and
 * multiply, which Vulkan requires correctly rounded; fma is not used,
 * because Vulkan lets an implementation evaluate it as a rounded multiply
 * and a rounded add. */

#ifndef OPENPETE_MOBY_STAGE_GLSL
#define OPENPETE_MOBY_STAGE_GLSL

/* Instance flags (openpete_moby_gstage.h OPENPETE_GSAB_F_*). */
#define MOBY_F_GPF    1u
#define MOBY_F_ANIM   2u
#define MOBY_F_LERP   4u
#define MOBY_F_MIRROR 8u
#define MOBY_F_LIFT   16u
#define MOBY_F_SHELL  32u
#define MOBY_F_ENV    64u
#define MOBY_F_SHADED 128u
#define MOBY_F_BILLBOARD 256u
#define MOBY_F_SIMPLE 512u
#define MOBY_F_SCREEN 1024u

/* Vec4s per instance record and per vertex record in the stage's storage
 * (openpete_moby_gstage.h OPENPETE_GSAB_INST_VEC4, _VERT_VEC4). */
#define MOBY_INST_VEC4 8u
#define MOBY_VERT_VEC4 2u

/* One moby record's per-record inputs. */
struct moby_inst {
    vec3  m0, m1, m2;   /* model-to-view rotation rows, 1.0 = 1.0 */
    vec3  tr;           /* the projection translation */
    vec2  of;           /* screen offsets */
    float H;            /* the display's projection distance */
    float H_canon;      /* the GTE's H: every regime test reads it */
    float div_cap;      /* the capped quotient 0x1FFFF / 65536 times the --fov scale */
    float unit;         /* projected view-Z to the perspective W: 2^scale byte, or the shaded pass's factor */
    float depth_k;      /* projected to written depth (sized records) */
    float tie_pull;     /* the one-slot tie pull, 0 when the gate is off */
    float lift;         /* the decode's near-floor lift, 0 when none */
    float rho;          /* the sphere-shell pull, 0 when none */
    float lift2;        /* the shell's near-floor lift, 0 when none */
    float alpha;        /* the pose lerp phase, previous (0) to current (1) */
    uint  flags;        /* MOBY_F_* */
    vec3  tr_lo;        /* the translation's remainder below tr's float */
};

/* One projected vertex. */
struct moby_vout {
    vec2  s;     /* screen position, the GTE pair inside the divide-overflow regime */
    float tvz;   /* perspective W, world view-Z */
    float vz;    /* written depth */
    float kvz;   /* key depth: before the tie pull and the near-floor lift */
    vec2  h;     /* homogeneous screen position (h / tvz = s outside the regime) */
    vec3  e;     /* rotated model offset M v */
    uint  unr;   /* 1 inside the divide-overflow regime */
    uint  sat;   /* 1 where the GTE would have saturated */
};

/* Unpack an instance record from its MOBY_INST_VEC4 words. */
moby_inst moby_inst_unpack(vec4 q0, vec4 q1, vec4 q2, vec4 q3, vec4 q4, vec4 q5, vec4 q6,
                           vec4 q7)
{
    moby_inst I;
    I.m0 = q0.xyz; I.m1 = q1.xyz; I.m2 = q2.xyz;
    I.tr = vec3(q0.w, q1.w, q2.w);
    I.of = q3.xy; I.H = q3.z; I.H_canon = q3.w;
    I.div_cap = q4.x; I.unit = q4.y; I.depth_k = q4.z;
    I.tie_pull = q5.x; I.lift = q5.y; I.rho = q5.z; I.lift2 = q5.w;
    I.alpha = q6.x; I.flags = floatBitsToUint(q6.y);
    I.tr_lo = vec3(q6.z, q6.w, q7.x);
    return I;
}

/* a + b as s + e exactly, s the rounded sum (Knuth's two-sum). */
vec2 moby_two_sum(float a, float b)
{
    precise float s = a + b;
    precise float bb = s - a;
    precise float e = (a - (s - bb)) + (b - bb);
    return vec2(s, e);
}

/* a as hi + lo exactly, each half of a's significand (Veltkamp, 2^12 + 1). */
vec2 moby_split(float a)
{
    precise float c = 4097.0 * a;
    precise float hi = c - (c - a);
    precise float lo = a - hi;
    return vec2(hi, lo);
}

/* a b as p + e exactly, p the rounded product (Dekker). */
vec2 moby_two_prod(float a, float b)
{
    precise float p = a * b;
    vec2 A = moby_split(a), B = moby_split(b);
    precise float e = ((A.x * B.x - p) + A.x * B.y + A.y * B.x) + A.y * B.y;
    return vec2(p, e);
}

/* The row m times the pair vh + vl, as a pair: each product of the high
 * halves exact, accumulated by exact sums; the low half's products and the
 * accumulated errors summed in float. */
vec2 moby_dot2(vec3 m, vec3 vh, vec3 vl)
{
    vec2 a = moby_two_prod(m.x, vh.x);
    vec2 b = moby_two_prod(m.y, vh.y);
    vec2 c = moby_two_prod(m.z, vh.z);
    vec2 s1 = moby_two_sum(a.x, b.x);
    vec2 s2 = moby_two_sum(s1.x, c.x);
    precise float lo = (((a.y + b.y) + c.y) + (s1.y + s2.y)) +
                       ((m.x * vl.x + m.y * vl.y) + m.z * vl.z);
    return vec2(s2.x, lo);
}

/* t + r as a float, t = th + tl and r = r.x + r.y pairs. */
float moby_add2(float th, float tl, vec2 r)
{
    vec2 s = moby_two_sum(th, r.x);
    precise float lo = (s.y + tl) + r.y;
    precise float f = s.x + lo;
    return f;
}

/* Project one vertex: c is the current pose's model-space vertex, p the
 * previous pose's (equal to c without a lerp), dtr an offset added to the
 * translation (zero outside the readback's control). */
moby_vout moby_stage(moby_inst I, vec3 c, vec3 p, vec3 dtr)
{
    moby_vout o;
    /* v = p + (c - p) alpha as vh + vl. */
    vec3 vh, vl;
    for (int k = 0; k < 3; k++) {
        vec2 d = moby_two_sum(c[k], -p[k]);
        vec2 q = moby_two_prod(d.x, I.alpha);
        vec2 w = moby_two_sum(p[k], q.x);
        precise float l = w.y + (q.y + d.y * I.alpha);
        vh[k] = w.x;
        vl[k] = l;
    }
    vec2 R1 = moby_dot2(I.m0, vh, vl);
    vec2 R2 = moby_dot2(I.m1, vh, vl);
    vec2 R3 = moby_dot2(I.m2, vh, vl);
    precise float r1 = R1.x + R1.y;
    precise float r2 = R2.x + R2.y;
    precise float r3 = R3.x + R3.y;
    precise vec3 tr = I.tr + dtr;
    precise float mac1 = moby_add2(tr.x, I.tr_lo.x, R1);
    precise float mac2 = moby_add2(tr.y, I.tr_lo.y, R2);
    precise float mac3 = moby_add2(tr.z, I.tr_lo.z, R3);
    precise float z = mac3 != 0.0 ? mac3 : 1.0;
    precise float fsx = I.of.x + mac1 * I.H / z;
    precise float fsy = I.of.y + mac2 * I.H / z;
    bool unr = !(mac3 > 0.0 && I.H_canon < 2.0 * mac3);
    bool ir = mac1 > 32767.0 || mac1 < -32768.0 || mac2 > 32767.0 || mac2 < -32768.0 ||
              mac3 <= 0.0;
    precise float gsx = clamp(I.of.x + clamp(mac1, -32768.0, 32767.0) * I.div_cap, -1024.0, 1023.0);
    precise float gsy = clamp(I.of.y + clamp(mac2, -32768.0, 32767.0) * I.div_cap, -1024.0, 1023.0);
    o.s = unr ? vec2(gsx, gsy) : vec2(fsx, fsy);
    o.unr = unr ? 1u : 0u;
    o.sat = (ir || I.H_canon >= 2.0 * mac3) ? 1u : 0u;
    precise float tvz = mac3 * I.unit;
    precise float kvz = mac3 * I.unit * I.depth_k;
    precise float vz = ((kvz - I.tie_pull) + I.lift - I.rho) + I.lift2;
    o.tvz = tvz;
    o.kvz = kvz - I.rho;
    o.vz = vz;
    precise float hx = (I.of.x * mac3 + mac1 * I.H) * I.unit;
    precise float hy = (I.of.y * mac3 + mac2 * I.H) * I.unit;
    o.h = vec2(hx, hy);
    o.e = vec3(r1, r2, r3);
    return o;
}

#endif
