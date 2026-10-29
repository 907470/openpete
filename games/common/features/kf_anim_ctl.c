// games/common/features/kf_anim_ctl.c - OPENPETE_VIDEO_OPEN_PANIM and
// OPENPETE_VIDEO_HOME_PANIM: keyframe-animated sectors (doors) started or
// held by animation id; OPENPETE_VIDEO_HIDE_FACES: terrain faces removed.
//
// The game moves a door by changing its descriptors' run flags by id (Spyro
// 2 func_8002365C, Spyro 3 func_80021ED8; a door opens on (id, 0xFC, 0),
// clearing the stop and reverse bits) and lets the walker
// (game_layout.kf_anim.walker) advance every running descriptor to its next
// stop keyframe. The module's hook at the walker's entry visits every
// descriptor of the registry before the walker runs and applies the rule
// the knobs bind to its id:
//
// - OPEN (OPENPETE_VIDEO_OPEN_PANIM, comma list of ids): a descriptor
//   stopped at keyframe 0 has bits 0-1 of its flags cleared, the store the
//   game's open makes, so the walker carries it to its next stop. A
//   descriptor stopped anywhere else is left alone, so the animation runs
//   once and rests instead of cycling.
// - HOME (OPENPETE_VIDEO_HOME_PANIM, comma list of ids): a descriptor
//   stopped past keyframe 0 whose keyframe 1 links back to keyframe 0 is
//   put on keyframe 1, running in reverse with one tick left, so the
//   walker's next step lands on keyframe 0, takes its stop flag and posts
//   pose 0 to the sector; a descriptor the level code restarts at keyframe
//   0 is stopped again before the walker sees it, so pose 0 holds.
//
// - HIDE_FACES (OPENPETE_VIDEO_HIDE_FACES, comma list of SEC, SEC/T,
//   SEC/T/F or SEC/T/A-B, T = h for the HP faces or l for the LP faces,
//   F one face, A-B a face range): before every walker run each listed face of the sector's terrain record is made
//   degenerate in place (every vertex index set to the first), so the
//   game's draw and the native feeders project a zero-area face. SEC alone
//   names every face of both tiers, SEC/T every face of that tier.
//
// The knobs are boot-latched and default off; with none set the hook is
// unbound. All change gameplay state (the descriptors, the sector poses
// the walker posts, the terrain face words).

#include "game_layout.h"
#include "feature_registry.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KA (game_layout.kf_anim)

enum { KFA_NONE = 0, KFA_OPEN, KFA_HOME, KFA_RULES };

/* The rule bound to each animation id (byte 0 of a descriptor). */
static uint8_t s_rule[256];
static int s_latched;

/* A descriptor's keyframe table: entries of `stride` bytes from `base`,
 * entry byte 0 the flags, the next keyframe forward at `link` and in
 * reverse at `link` + 1. Both games' walkers read pairs 0 and 1 (texture
 * swap and scroll) as 4-byte entries from +8 with the links at +1/+2, the
 * flags and tick count packed in byte 0 (Spyro 2 environment.s 0x8002264C,
 * 0x80022754; Spyro 3 0x80020EC8, 0x80020FD0), and pairs 2-6 (geometry
 * and colour) as 8-byte entries from +0xC with the tick count at +1 and the
 * links at +2/+3 (Spyro 2 0x80022890, Spyro 3 0x8002110C). */
typedef struct { uint8_t base, stride, link; } kfa_table_t;

/** @brief The keyframe table layout of registry pair `pair`.
 *
 * @param pair The pair index in the registry.
 * @return The table's base, stride and forward-link offset. */
static kfa_table_t kfa_table(uint32_t pair) {
    if (pair < 2u) return (kfa_table_t){ 0x08u, 4u, 1u };
    return (kfa_table_t){ 0x0Cu, 8u, 2u };
}

/** @brief Whether `a` lies in main RAM's KSEG0 window.
 *
 * @param a A guest address.
 * @return 1 when it does, else 0. */
static int kfa_in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

/** @brief OPEN: start a descriptor stopped at keyframe 0 by clearing its stop and reverse bits.
 *
 * @param cpu The guest CPU state.
 * @param d The descriptor's guest address.
 * @param t Its keyframe table layout (unused). */
static void kfa_open(CPUState* cpu, uint32_t d, kfa_table_t t) {
    (void)t;
    const uint8_t st = cpu->read_byte(d + 1u);
    if (!(st & 2u) || cpu->read_byte(d + 2u) != 0u) return;
    cpu->write_byte(d + 1u, (uint8_t)(st & 0xFCu));
    OPL_INFO_F("kf-anim", "feed key %u: descriptor 0x%08X id %u started from keyframe 0",
               openpete_feed_key_now(), d, cpu->read_byte(d));
}

/** @brief HOME: send a stopped descriptor back to keyframe 0, or stop one restarted there.
 *
 * @param cpu The guest CPU state.
 * @param d The descriptor's guest address.
 * @param t Its keyframe table layout, for keyframe 1's reverse link. */
static void kfa_home(CPUState* cpu, uint32_t d, kfa_table_t t) {
    const uint8_t st = cpu->read_byte(d + 1u);
    const uint8_t kf = cpu->read_byte(d + 2u);
    if (kf == 0u) {
        if (!(st & 2u)) cpu->write_byte(d + 1u, (uint8_t)((st & 0xFCu) | 2u));
        return;
    }
    if (!(st & 2u) || cpu->read_byte(d + t.base + t.stride + t.link + 1u) != 0u) return;
    cpu->write_byte(d + 2u, 1u);
    cpu->write_byte(d + 3u, 1u);
    cpu->write_byte(d + 1u, 1u);
    OPL_INFO_F("kf-anim", "feed key %u: descriptor 0x%08X id %u sent to keyframe 0 from %u",
               openpete_feed_key_now(), d, cpu->read_byte(d), kf);
}

/* One OPENPETE_VIDEO_HIDE_FACES entry: faces [a, b] of sector `sec`'s HP
 * (hp = 1) or LP (hp = 0) list. */
typedef struct { uint16_t sec; uint8_t hp, a, b; } kfa_faces_t;
enum { KFA_FACES_MAX = 64 };
static kfa_faces_t s_faces[KFA_FACES_MAX];
static int s_faces_n;

/** @brief The degenerate form of one terrain face's index word.
 *
 * An HP face holds four byte vertex indices in word 0 (bits 31-24, 23-16,
 * 15-8, 7-0); an LP face holds four 7-bit indices at bits 30-24, 23-17,
 * 16-10 and 9-3 with the double-sided flag in bit 31 (the Spyro 2/3 face
 * layout, terrain face decode). Every index takes the first one's value.
 * @param w The face's word 0.
 * @param hp 1 for an HP face, 0 for an LP face.
 * @return The word with every vertex index equal to the first. */
static uint32_t kfa_degenerate(uint32_t w, int hp) {
    if (hp) { const uint32_t i0 = w >> 24; return (i0 << 24) | (i0 << 16) | (i0 << 8) | i0; }
    const uint32_t i0 = (w >> 24) & 0x7Fu;
    return (w & 0x80000007u) | (i0 << 24) | (i0 << 17) | (i0 << 10) | (i0 << 3);
}

/** @brief Make the listed faces of every OPENPETE_VIDEO_HIDE_FACES entry degenerate.
 *
 * A sector record holds its header (word 0, then words 1-5 at +4..+0x14),
 * the LP vertices from +0x1C, the LP colours and faces (8 bytes each), and
 * at +0x1C + 4 * (word 5 bits 31-24) the HP vertices, the near and far
 * colours and the HP faces (16 bytes each); word 4 holds the LP and word 5
 * the HP vertex, colour and face counts in bytes 0, 1 and 2 (the record
 * layout the terrain cache decodes). A sector index past the list or a
 * record outside main RAM is skipped; a face already degenerate is not
 * rewritten.
 * @param cpu The guest CPU state. */
static void kfa_hide_faces(CPUState* cpu) {
    const uint32_t list = cpu->read_word(game_layout.env.sector_ptr);
    const uint32_t n = cpu->read_word(game_layout.env.sector_count);
    if (!kfa_in_ram(list) || n > 4096u) return;
    for (int i = 0; i < s_faces_n; i++) {
        const kfa_faces_t* h = &s_faces[i];
        if (h->sec >= n) continue;
        const uint32_t sec = cpu->read_word(list + 4u * h->sec);
        if (!kfa_in_ram(sec)) continue;
        const uint32_t g = cpu->read_word(sec + (h->hp ? 0x14u : 0x10u));
        const uint32_t nf = (g >> 16) & 0xFFu;
        const uint32_t faces = h->hp
            ? sec + 0x1Cu + ((g >> 24) & 0xFFu) * 4u + ((g & 0xFFu) + ((g >> 8) & 0xFFu) * 2u) * 4u
            : sec + 0x1Cu + ((g & 0xFFu) + ((g >> 8) & 0xFFu)) * 4u;
        const uint32_t stride = h->hp ? 16u : 8u;
        for (uint32_t f = h->a; f <= h->b && f < nf; f++) {
            const uint32_t at = faces + f * stride, w = cpu->read_word(at);
            const uint32_t d = kfa_degenerate(w, h->hp);
            if (d != w) cpu->write_word(at, d);
        }
    }
}

typedef void (*kfa_rule_fn)(CPUState* cpu, uint32_t d, kfa_table_t t);
static const kfa_rule_fn k_rules[KFA_RULES] = { NULL, kfa_open, kfa_home };

/** @brief Apply the bound rule to every descriptor of one registry pair.
 *
 * A pair whose count or array pointer is out of range (the registry
 * before a level has filled it) is skipped, as is a descriptor pointer
 * outside main RAM.
 * @param cpu The guest CPU state.
 * @param pair The pair index. */
static void kfa_walk_pair(CPUState* cpu, uint32_t pair) {
    const uint32_t at = KA.registry + 8u * pair;
    const uint32_t cnt = cpu->read_word(at), arr = cpu->read_word(at + 4u);
    if (cnt > 1024u || !kfa_in_ram(arr)) return;
    const kfa_table_t t = kfa_table(pair);
    for (uint32_t q = 0; q < cnt; q++) {
        const uint32_t d = cpu->read_word(arr + 4u * q);
        if (!kfa_in_ram(d)) continue;
        const uint8_t rule = s_rule[cpu->read_byte(d)];
        if (rule != KFA_NONE) k_rules[rule](cpu, d, t);
    }
}

/** @brief Apply the bound rules over the whole registry and hide the listed faces, then let the walker run.
 *
 * @param cpu The guest CPU state.
 * @param addr The walker's entry (unused).
 * @return 0: the walker's body runs. */
static int kfa_on_walker(CPUState* cpu, uint32_t addr) {
    (void)addr;
    for (uint32_t p = 0; p < KA.pairs; p++) kfa_walk_pair(cpu, p);
    if (s_faces_n) kfa_hide_faces(cpu);
    return 0;
}

/** @brief Bind `rule` to every id the comma list in environment variable `var` names.
 *
 * Parses strtoul base-0 tokens up to the first that is not a number. An id
 * above 255 cannot match a descriptor's id byte and is dropped with a
 * warning; an id the other knob already bound keeps its first rule, with a
 * warning.
 * @param var The variable.
 * @param rule KFA_OPEN or KFA_HOME.
 * @return The number of ids bound. */
static int kfa_bind_list(const char* var, uint8_t rule) {
    int n = 0;
    const char* e = getenv(var);
    while (e && *e) {
        char* end = NULL;
        const unsigned long v = strtoul(e, &end, 0);
        if (end == e) break;
        e = (*end == ',') ? end + 1 : end;
        if (v > 0xFFu || s_rule[v] != KFA_NONE) {
            OPL_WARN_F("kf-anim", "%s: id %lu %s; dropped", var, v,
                       v > 0xFFu ? "is not a byte" : "is already bound by the other knob");
            continue;
        }
        s_rule[v] = rule;
        n++;
    }
    return n;
}

/** @brief Append one OPENPETE_VIDEO_HIDE_FACES entry, SEC, SEC/T, SEC/T/F or SEC/T/A-B, parsed from `e`.
 *
 * SEC alone appends two entries (every HP and every LP face); a range end
 * above 255 is clamped. A malformed entry is dropped with a warning.
 * @param e The entry's text, up to the next comma or the end.
 * @param len Its length. */
static void kfa_faces_add(const char* e, size_t len) {
    char buf[48];
    if (len == 0u || len >= sizeof buf) return;
    memcpy(buf, e, len);
    buf[len] = '\0';
    unsigned sec = 0, a = 0, b = 255;
    char t = 0;
    int ok = sscanf(buf, "%u/%c/%u-%u", &sec, &t, &a, &b);
    if (ok == 2) { a = 0; b = 255; }
    if (ok == 3) b = a;
    if (ok < 1 || sec > 0xFFFFu || (ok >= 2 && t != 'h' && t != 'l') || a > b) {
        OPL_WARN_F("kf-anim", "OPENPETE_VIDEO_HIDE_FACES: entry '%s' is not SEC, SEC/h|l, SEC/h|l/F or SEC/h|l/A-B; dropped", buf);
        return;
    }
    if (b > 255u) b = 255u;
    for (int hp = 1; hp >= 0; hp--) {
        if (ok >= 2 && (t == 'h') != hp) continue;
        if (s_faces_n >= KFA_FACES_MAX) {
            OPL_WARN_F("kf-anim", "OPENPETE_VIDEO_HIDE_FACES: more than %d entries; '%s' dropped", KFA_FACES_MAX, buf);
            return;
        }
        s_faces[s_faces_n++] = (kfa_faces_t){ (uint16_t)sec, (uint8_t)hp, (uint8_t)a, (uint8_t)b };
    }
}

/** @brief Latch the OPENPETE_VIDEO_HIDE_FACES entries.
 *
 * @return The number of entries latched. */
static int kfa_bind_faces(void) {
    const char* e = getenv("OPENPETE_VIDEO_HIDE_FACES");
    while (e && *e) {
        const char* c = strchr(e, ',');
        const size_t len = c ? (size_t)(c - e) : strlen(e);
        kfa_faces_add(e, len);
        e += len + (c ? 1u : 0u);
    }
    return s_faces_n;
}

static feature_dispatch_entry_t kfa_dispatch[] = {
    { 0u, kfa_on_walker, 0u },
    { 0u, NULL, 0u },
};

/** @brief Latch the id lists and the face list and bind the walker hook while any is set.
 *
 * A game whose layout names no walker leaves the hook unbound and warns
 * when any knob is set.
 * @param opts Unused. */
static void kfa_apply_options(const openpete_options_t* opts) {
    (void)opts;
    if (s_latched) return;
    s_latched = 1;
    const int n_open = kfa_bind_list("OPENPETE_VIDEO_OPEN_PANIM", KFA_OPEN);
    const int n_home = kfa_bind_list("OPENPETE_VIDEO_HOME_PANIM", KFA_HOME);
    const int n_faces = kfa_bind_faces();
    if (n_open + n_home + n_faces == 0) return;
    if (KA.walker == 0u || KA.registry == 0u || game_layout.env.sector_ptr == 0u) {
        OPL_WARN_F("kf-anim", "OPENPETE_VIDEO_OPEN_PANIM/HOME_PANIM/HIDE_FACES are set but this game names no keyframe walker; ignored");
        return;
    }
    kfa_dispatch[0].addr = KA.walker;
    OPL_INFO_F("kf-anim", "%d id(s) started, %d held at keyframe 0, %d face range(s) hidden, before the walker at 0x%08X",
               n_open, n_home, n_faces, KA.walker);
}

const feature_module_t kf_anim_ctl_module = {
    .name           = "kf_anim_ctl",
    .apply_options  = kfa_apply_options,
    .dispatch_table = kfa_dispatch,
};
