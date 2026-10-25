// games/common/features/kf_anim_ctl.c - OPENPETE_VIDEO_OPEN_PANIM and
// OPENPETE_VIDEO_HOME_PANIM: keyframe-animated sectors (doors) started or
// held by animation id.
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
// Both knobs are boot-latched and default off; with neither set the hook is
// unbound. Both change gameplay state (the descriptors, and the sector
// poses the walker posts).

#include "game_layout.h"
#include "feature_registry.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"

#include <stdint.h>
#include <stdlib.h>

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

/** @brief Apply the bound rules over the whole registry, then let the walker run.
 *
 * @param cpu The guest CPU state.
 * @param addr The walker's entry (unused).
 * @return 0: the walker's body runs. */
static int kfa_on_walker(CPUState* cpu, uint32_t addr) {
    (void)addr;
    for (uint32_t p = 0; p < KA.pairs; p++) kfa_walk_pair(cpu, p);
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

static feature_dispatch_entry_t kfa_dispatch[] = {
    { 0u, kfa_on_walker, 0u },
    { 0u, NULL, 0u },
};

/** @brief Latch both id lists and bind the walker hook while either names an id.
 *
 * A game whose layout names no walker leaves the hook unbound and warns
 * when either knob is set.
 * @param opts Unused. */
static void kfa_apply_options(const openpete_options_t* opts) {
    (void)opts;
    if (s_latched) return;
    s_latched = 1;
    const int n_open = kfa_bind_list("OPENPETE_VIDEO_OPEN_PANIM", KFA_OPEN);
    const int n_home = kfa_bind_list("OPENPETE_VIDEO_HOME_PANIM", KFA_HOME);
    if (n_open + n_home == 0) return;
    if (KA.walker == 0u || KA.registry == 0u) {
        OPL_WARN_F("kf-anim", "OPENPETE_VIDEO_OPEN_PANIM/HOME_PANIM are set but this game names no keyframe walker; ignored");
        return;
    }
    kfa_dispatch[0].addr = KA.walker;
    OPL_INFO_F("kf-anim", "%d id(s) started, %d held at keyframe 0, before the walker at 0x%08X",
               n_open, n_home, KA.walker);
}

const feature_module_t kf_anim_ctl_module = {
    .name           = "kf_anim_ctl",
    .apply_options  = kfa_apply_options,
    .dispatch_table = kfa_dispatch,
};
