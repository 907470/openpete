/* boot_timing.c - boot timing line and the live-switch harness schedule
 * (contract in openpete_boot_timing.h). */
#include "openpete_boot_timing.h"

#include "openpete_config.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"
#include "rhi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int64_t s_mark[OP_BOOT_MARKS];
static _Atomic int     s_seq[OP_BOOT_MARKS];   /* ordinal of each mark, 0 unreached */
static _Atomic int     s_seq_next;             /* last ordinal handed out           */
static int      s_first_tick_seq;
static int64_t  s_first_tick_ns;
static unsigned s_ticks;
static int      s_line_done;

void openpete_boot_mark(op_boot_mark mark)
{
    if ((unsigned)mark >= OP_BOOT_MARKS) return;
    int64_t zero = 0;
    const int64_t now = op_now();
    if (atomic_compare_exchange_strong(&s_mark[mark], &zero, now ? now : 1))
        atomic_store(&s_seq[mark], atomic_fetch_add(&s_seq_next, 1) + 1);
}

/* The boot steps the order line checks, in their required order. */
#define BOOT_ORDER_STEPS 7

/** @brief Print the boot-order line: each step's ordinal ("-" when unreached) and the order verdict. */
static void boot_order_print(void)
{
    static const char *const k_name[BOOT_ORDER_STEPS] = {
        "identified", "mods_staged", "mods_compiled", "mods_init",
        "slot_contest", "precompile", "first_tick"
    };
    const int seq[BOOT_ORDER_STEPS] = {
        atomic_load(&s_seq[OP_BOOT_IDENTIFIED]),   atomic_load(&s_seq[OP_BOOT_MODS_STAGED]),
        atomic_load(&s_seq[OP_BOOT_MODS_COMPILED]), atomic_load(&s_seq[OP_BOOT_MODS_INIT]),
        atomic_load(&s_seq[OP_BOOT_SLOT_CONTEST]), atomic_load(&s_seq[OP_BOOT_PRECOMPILE_START]),
        s_first_tick_seq
    };
    char text[256];
    size_t len = 0;
    int last = 0, reached = 0, ordered = 1;
    for (int i = 0; i < BOOT_ORDER_STEPS; i++) {
        char v[16];
        if (seq[i]) snprintf(v, sizeof v, "%d", seq[i]);
        else snprintf(v, sizeof v, "-");
        len += (size_t)snprintf(text + len, sizeof text - len, "%s%s=%s", i ? " " : "", k_name[i], v);
        if (len >= sizeof text) len = sizeof text - 1;
        if (!seq[i]) continue;
        reached++;
        if (seq[i] < last) ordered = 0;
        last = seq[i];
    }
    const char *verdict = !ordered ? "OUT-OF-ORDER"
                        : reached == BOOT_ORDER_STEPS ? "ordered" : "partial";
    OPL_INFO_F("boot", "boot-order: %s; steps_reached=%d/%d verdict=%s", text, reached,
               BOOT_ORDER_STEPS, verdict);
}

/** @brief Format an instant as ms after the launcher-start mark, or "-" when it was never reached.
 *
 * @param buf Destination.
 * @param n Its size.
 * @param t0 The launcher-start instant.
 * @param t The instant, 0 when unreached.
 * @return buf. */
static const char *boot_ms(char *buf, size_t n, int64_t t0, int64_t t)
{
    if (!t) snprintf(buf, n, "-");
    else snprintf(buf, n, "%.1f", (double)(t - t0) / 1e6);
    return buf;
}

/** @brief Print the boot timing line from the marks and the RHI's boot facts. */
static void boot_line_print(void)
{
    rhi_boot_stats rb;
    rhi_boot_stats_get(&rb);
    int64_t t0 = atomic_load(&s_mark[OP_BOOT_LAUNCHER_START]);
    if (!t0) t0 = atomic_load(&s_mark[OP_BOOT_MODS_INIT]);
    char a[32], b[32], c[32], d[32], e[32], f[32], g[32], h[32];
    const char *mesa = getenv("MESA_SHADER_CACHE_DISABLE");
    OPL_INFO_F("boot",
               "timing (ms after launcher start, op_now clock): identified=%s materialized=%s "
               "mods_init=%s device_created=%s "
               "precompile_start=%s precompile_end=%s first_frame=%s first_tick=%s; "
               "materializations=%d devices_created=%u; "
               "bringup_pipes=%u bringup_pipe_ms=%.1f; cache_file_bytes=%llu cache_offered_bytes=%llu "
               "cache_load_ms=%.2f app_cold=%d mesa_cache_disabled=%s",
               boot_ms(g, sizeof g, t0, atomic_load(&s_mark[OP_BOOT_IDENTIFIED])),
               boot_ms(h, sizeof h, t0, atomic_load(&s_mark[OP_BOOT_MATERIALIZED])),
               boot_ms(a, sizeof a, t0, atomic_load(&s_mark[OP_BOOT_MODS_INIT])),
               boot_ms(b, sizeof b, t0, rb.device_created_ns),
               boot_ms(c, sizeof c, t0, atomic_load(&s_mark[OP_BOOT_PRECOMPILE_START])),
               boot_ms(d, sizeof d, t0, atomic_load(&s_mark[OP_BOOT_PRECOMPILE_END])),
               boot_ms(e, sizeof e, t0, rb.first_frame_ns),
               boot_ms(f, sizeof f, t0, s_first_tick_ns),
               openpete_config_materializations(), rb.devices_created,
               rb.bringup_creates, (double)rb.bringup_ns / 1e6,
               (unsigned long long)rb.cache_file_bytes, (unsigned long long)rb.cache_offered_bytes,
               (double)rb.cache_load_ns / 1e6, rb.cache_cold, (mesa && *mesa) ? mesa : "unset");
}

void openpete_boot_tick(void)
{
    if (s_line_done) return;
    if (!s_first_tick_ns) {
        s_first_tick_ns = op_now();
        s_first_tick_seq = atomic_fetch_add(&s_seq_next, 1) + 1;
    }
    rhi_boot_stats rb;
    rhi_boot_stats_get(&rb);
    if (!rb.first_frame_ns && ++s_ticks < OP_BOOT_LINE_MAX_TICKS) return;
    s_line_done = 1;
    boot_line_print();
    boot_order_print();
}

/* One OPENPETE_LIVE_SWITCH entry. */
typedef struct {
    unsigned vbl;
    int      row;     /* 0 texture-mapping, 1 notex, 2 wireframe */
    int      value;
    int      fired;
} live_switch;

#define LIVE_SWITCH_MAX 16
static live_switch s_ls[LIVE_SWITCH_MAX];
static int         s_ls_n = -1;   /* -1 until parsed */

/** @brief Parse one "<vbl>:<row>=<value>" entry.
 *
 * @param tok The entry text.
 * @param out Receives the entry.
 * @return 1 on success, 0 when malformed. */
static int live_switch_parse_one(const char *tok, live_switch *out)
{
    static const char *const k_rows[3] = { "texture-mapping", "notex", "wireframe" };
    char row[32];
    unsigned vbl = 0;
    int value = 0;
    if (sscanf(tok, "%u:%31[a-z-]=%d", &vbl, row, &value) != 3) return 0;
    for (int r = 0; r < 3; r++) {
        if (strcmp(row, k_rows[r]) != 0) continue;
        *out = (live_switch){ vbl, r, value != 0, 0 };
        return 1;
    }
    return 0;
}

/** @brief Parse OPENPETE_LIVE_SWITCH into the schedule, logging each entry. */
static void live_switch_parse(void)
{
    s_ls_n = 0;
    const char *e = getenv("OPENPETE_LIVE_SWITCH");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ","); tok && s_ls_n < LIVE_SWITCH_MAX; tok = strtok(NULL, ",")) {
        if (live_switch_parse_one(tok, &s_ls[s_ls_n])) s_ls_n++;
        else OPL_WARN_F("live-switch", "unparsable entry '%s' skipped", tok);
    }
    OPL_INFO_F("live-switch", "%d switch(es) scheduled", s_ls_n);
}

void openpete_live_switch_poll(unsigned vbl)
{
    if (s_ls_n < 0) live_switch_parse();
    for (int i = 0; i < s_ls_n; i++) {
        live_switch *ls = &s_ls[i];
        if (ls->fired || vbl < ls->vbl) continue;
        ls->fired = 1;
        OPL_INFO_F("live-switch", "vbl %u: row %d -> %d", vbl, ls->row, ls->value);
        if (ls->row == 0) openpete_native_set_texture_mapping(ls->value);
        else if (ls->row == 1) openpete_native_set_notex(ls->value);
        else openpete_native_set_wireframe(ls->value);
    }
}
