/*
 * pad-probe — api->pad_read per slot, checked against the synthetic stream.
 *
 * A CameraUpdate pre-hook reads every pad slot once per tick. For a slot
 * named by the `expect` config row it recomputes, from the record's own
 * `vbl`, the buttons and axes the synthetic pad stream defines for that VBL
 * and slot, and counts a mismatch when the record disagrees or when the slot
 * reads absent. The formula below is a copy of the engine's: a mod links no
 * engine symbol, so the check is a comparison of two independent
 * derivations, and the copies drifting apart is the failure the check
 * exists to catch.
 *
 * Reads are classified by the record's `vbl` against the previous tick's:
 * advanced = a freshly staged record, repeat = a re-execution on the last
 * committed record (a speculative runahead tick), backward = a re-anchor
 * (the rewind burst restages an earlier VBL). A repeat is checked against
 * the same formula, so a speculative tick proves it carried the last
 * committed VBL's bytes and not stale or invented ones.
 *
 * The mod API has no shutdown callback, so the tallies are logged every
 * `report_every` ticks; the last such line of a run carries its final
 * counts.
 *
 * With the `rumble` row set the hook also drives `pad_rumble` on
 * `rumble_slot` every tick with a distinctive actuator byte pair, and probes
 * slot 0 once, which the engine must refuse. Which device the pair reaches is
 * the claim the per-pad rumble recorders answer.
 *
 * Display-only: no guest RAM is written.
 */
#include <openpete_mod_api.h>

#include <string.h>

#define PROBE_SLOTS OPENPETE_MOD_PAD_SLOTS

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static unsigned s_expect_mask;    /* bit per slot the run must carry */
static unsigned s_report_every;

/* Rumble drive. The pair is an actuator byte pair, the encoding pad_rumble
 * takes: small motor on, large motor at a value the game's own decode never
 * produces (its three cases are {1,120}, {1,0} and {0,amount}), so a pair
 * arriving at a device is this mod's and not the game's. */
#define PAD_PROBE_SMALL 1u
#define PAD_PROBE_LARGE 0xA5u

static unsigned s_rumble_slot;    /* 0 = rumble drive off */
static unsigned s_rumble_calls, s_rumble_ok, s_rumble_refused;
static int      s_slot0_rc = 1;   /* 1 = not probed yet */

static unsigned s_ticks;
static unsigned s_reads[PROBE_SLOTS];
static unsigned s_present[PROBE_SLOTS];
static unsigned s_checks[PROBE_SLOTS];
static unsigned s_mismatch[PROBE_SLOTS];
static unsigned s_spec_checks[PROBE_SLOTS];
static unsigned s_spec_mismatch[PROBE_SLOTS];
static unsigned s_libpad_bad[PROBE_SLOTS];

static unsigned s_advanced, s_repeat, s_backward;
static unsigned s_last_vbl;
static int      s_have_vbl;

/* The engine's synthetic stream: a pure function of VBL index and slot. */
static void synth_pad_for(unsigned vbl, unsigned slot,
                          unsigned* bits, unsigned char axes[4]) {
    unsigned k = vbl * 7u + slot * 1361u;
    *bits   = (~(1u << (k % 16u))) & 0xFFFFu;
    axes[0] = (unsigned char)(k % 251u);
    axes[1] = (unsigned char)((k / 3u) % 251u);
    axes[2] = (unsigned char)((k / 7u) % 251u);
    axes[3] = (unsigned char)((k / 11u) % 251u);
}

/* Does `libpad` carry the same reading as `buttons` and `axes`? A present
 * slot is 00 73 then the low and high button bytes and RX RY LX LY; an
 * absent slot is the canonical empty port. */
static int libpad_disagrees(const openpete_mod_pad_t* p) {
    unsigned char want[8];
    if (p->present) {
        want[0] = 0x00; want[1] = 0x73;
        want[2] = (unsigned char)(p->buttons & 0xFFu);
        want[3] = (unsigned char)((p->buttons >> 8) & 0xFFu);
        want[4] = p->axes[2]; want[5] = p->axes[3];
        want[6] = p->axes[0]; want[7] = p->axes[1];
    } else {
        memset(want, 0, sizeof want);
        want[0] = 0xFF;
    }
    return memcmp(want, p->libpad, sizeof want) != 0;
}

/* 1 when the slot carries exactly the synthetic stream for its own VBL. */
static int synth_matches(const openpete_mod_pad_t* p) {
    unsigned bits;
    unsigned char axes[4];
    if (!p->present) return 0;
    synth_pad_for(p->vbl, p->slot, &bits, axes);
    return p->buttons == bits && memcmp(p->axes, axes, 4) == 0;
}

/* Comma-separated slot list into a bit mask; anything else is ignored. */
static unsigned parse_slot_set(const char* s) {
    unsigned mask = 0;
    for (; *s; s++)
        if (*s >= '0' && *s < '0' + PROBE_SLOTS)
            mask |= 1u << (unsigned)(*s - '0');
    return mask;
}

static void report(void) {
    for (unsigned sl = 0; sl < PROBE_SLOTS; sl++)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "slot %u: reads=%u present=%u checks=%u mismatches=%u "
                   "spec_checks=%u spec_mismatches=%u libpad_bad=%u expected=%u",
                   sl, s_reads[sl], s_present[sl], s_checks[sl],
                   s_mismatch[sl], s_spec_checks[sl], s_spec_mismatch[sl],
                   s_libpad_bad[sl], (s_expect_mask >> sl) & 1u);
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "ticks=%u advanced=%u repeat=%u backward=%u",
               s_ticks, s_advanced, s_repeat, s_backward);
    if (s_rumble_slot)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "rumble: slot=%u calls=%u ok=%u refused=%u slot0_rc=%d",
                   s_rumble_slot, s_rumble_calls, s_rumble_ok,
                   s_rumble_refused, s_slot0_rc);
}

/* Drive the probe pair, and probe slot 0 once: the engine must refuse it,
 * and a refusal nobody asked for is not evidence. */
static void drive_rumble(void) {
    if (!s_rumble_slot) return;
    if (s_slot0_rc == 1)
        s_slot0_rc = g_api->pad_rumble(g_self, 0, PAD_PROBE_SMALL,
                                       PAD_PROBE_LARGE);
    int rc = g_api->pad_rumble(g_self, s_rumble_slot, PAD_PROBE_SMALL,
                               PAD_PROBE_LARGE);
    s_rumble_calls++;
    if (rc == 0) s_rumble_ok++; else s_rumble_refused++;
}

/* 1 when this tick re-executes the previous tick's record (speculation). */
static int classify(unsigned vbl) {
    int repeat = 0;
    if (!s_have_vbl || vbl > s_last_vbl) s_advanced++;
    else if (vbl == s_last_vbl) { s_repeat++; repeat = 1; }
    else s_backward++;
    s_last_vbl = vbl;
    s_have_vbl = 1;
    return repeat;
}

static void tally(const openpete_mod_pad_t* p, int repeat) {
    unsigned sl = p->slot;
    s_reads[sl]++;
    if (p->present) s_present[sl]++;
    if (libpad_disagrees(p)) s_libpad_bad[sl]++;
    if (!((s_expect_mask >> sl) & 1u)) return;
    int bad = !synth_matches(p);
    s_checks[sl]++;
    if (bad) s_mismatch[sl]++;
    if (repeat) {
        s_spec_checks[sl]++;
        if (bad) s_spec_mismatch[sl]++;
    }
}

static void on_tick(CPUState* cpu) {
    openpete_mod_pad_t pads[PROBE_SLOTS];
    unsigned got = 0;
    for (unsigned sl = 0; sl < PROBE_SLOTS; sl++) {
        memset(&pads[sl], 0, sizeof pads[sl]);
        pads[sl].struct_size = sizeof pads[sl];
        if (g_api->pad_read(g_self, sl, &pads[sl]) == 0) got |= 1u << sl;
    }
    int repeat = (got & 1u) ? classify(pads[0].vbl) : 0;
    for (unsigned sl = 0; sl < PROBE_SLOTS; sl++)
        if ((got >> sl) & 1u) tally(&pads[sl], repeat);

    drive_rumble();
    s_ticks++;
    if (s_report_every && s_ticks % s_report_every == 0) {
        report();
        g_api->ui_status(g_self, "ticks %u advanced %u repeat %u backward %u",
                         s_ticks, s_advanced, s_repeat, s_backward);
    }
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version < 12u) {
        api->log(self, OP_MOD_LOG_ERROR, "needs api 12 (engine has %u)",
                 api->api_version);
        return -1;
    }
    char expect[64];
    api->config_str(self, "expect", "", expect, (int)sizeof expect);
    s_expect_mask  = parse_slot_set(expect);
    s_report_every = (unsigned)api->config_int(self, "report_every", 250);
    if (api->config_bool(self, "rumble", 0))
        s_rumble_slot = (unsigned)api->config_int(self, "rumble_slot", 1);
    api->log(self, OP_MOD_LOG_INFO,
             "expect=\"%s\" mask=0x%X report_every=%u rumble_slot=%u",
             expect, s_expect_mask, s_report_every, s_rumble_slot);
    return api->override_name(self, "CameraUpdate", on_tick);
}
