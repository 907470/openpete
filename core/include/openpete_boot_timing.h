/* openpete_boot_timing.h - one boot timing line per run, on the pace log's
 * clock (op_now).
 *
 * Marks are recorded once each, at their first call, from any thread. The
 * device-created and first-frame instants come from the RHI's boot facts
 * (rhi_boot_stats_get), stamped on the same clock through rhi_set_clock. The
 * line is printed once, at the first tick after both the first tick and the
 * first submitted frame are known, or after OP_BOOT_LINE_MAX_TICKS ticks
 * without a frame (a run with no native renderer). Every instant is printed
 * in ms after the launcher-start mark; a mark never reached prints "-". The
 * line also carries the count of full configuration materializations
 * (openpete_config_materializations), 1 on a normal boot, and the count of
 * Vulkan devices created (the launcher's device is the renderer's, so 1 on
 * a windowed boot).
 *
 * Each mark also takes the next ordinal of one process-wide sequence at its
 * first call, and the first tick takes one too; a second line, "boot-order",
 * printed with the timing line, lists the ordinals of the seven boot steps
 * (identified, mods staged, mods compiled, mods_init, SLOT_CONTEST,
 * precompile start, first tick) and its verdict: "ordered" when all seven
 * were reached in that order, "partial" when the reached steps are in order
 * and one or more was not reached (the precompile is skipped in a
 * synchronous run unless forced), "OUT-OF-ORDER" otherwise. Ordinals rather
 * than instants decide the verdict, so two marks on one clock tick still
 * order.
 *
 * The harness knob OPENPETE_LIVE_SWITCH=<vbl>:<row>=<value>[,...] (rows
 * texture-mapping, notex, wireframe; value 0 or 1, texture-mapping 1 =
 * affine) queues the named live switch when the VBL counter reaches <vbl>;
 * each switch then rebuilds the pipelines on the present path as a settings
 * change does.
 */
#ifndef OPENPETE_BOOT_TIMING_H
#define OPENPETE_BOOT_TIMING_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OP_BOOT_LAUNCHER_START = 0,  /* process entry of the game's main          */
    OP_BOOT_MODS_INIT,           /* openpete_mods_init returned               */
    OP_BOOT_PRECOMPILE_START,    /* the pipeline precompile began             */
    OP_BOOT_PRECOMPILE_END,      /* the pipeline precompile ended             */
    OP_BOOT_IDENTIFIED,          /* the game was identified                   */
    OP_BOOT_MATERIALIZED,        /* the full configuration parse returned     */
    OP_BOOT_MODS_STAGED,         /* the game's mod-layer descriptor registered */
    OP_BOOT_MODS_COMPILED,       /* compile-on-boot (openpete_mods_prepare) returned */
    OP_BOOT_SLOT_CONTEST,        /* the game's SLOT_CONTEST unit was set      */
    OP_BOOT_MARKS
} op_boot_mark;

#define OP_BOOT_LINE_MAX_TICKS 600u

/** @brief Record a boot mark at op_now(), once: later calls for the same mark are ignored.
 *
 * Safe on any thread.
 * @param mark The mark. */
void openpete_boot_mark(op_boot_mark mark);

/** @brief Note one tick: the first call records the first-tick instant, and the boot line prints once its inputs exist.
 *
 * Called by the host loop once per VBL that ran the game fiber. */
void openpete_boot_tick(void);

/** @brief Apply the OPENPETE_LIVE_SWITCH schedule for one VBL.
 *
 * Parsed at the first call; each entry fires once, when @p vbl first
 * reaches its VBL. An unparsable entry is logged and skipped.
 * @param vbl The VBL counter. */
void openpete_live_switch_poll(unsigned vbl);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BOOT_TIMING_H */
