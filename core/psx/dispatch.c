// core/psx/dispatch.c - psx_runtime.h glue not provided by generated C.
//
// The recompiled C calls into a small surface declared in psx_runtime.h:
// `call_by_address` (jump-register dispatch), `psx_override_dispatch`
// (the per-function intercept hook), and `psx_syscall`, supplied here on
// top of the generated dispatch table.

#include "openpete_crash.h"
#include "openpete_fiber.h"
#include "openpete_fatal.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_pace_log.h"
#include "openpete_psxtrace.h"
#include "openpete_recomp_module.h"

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Recomp-module registry -------------------------------------------
 * Core carries NO link-time reference to the recompiled code; the game
 * (static build) or the module loader (distribution build) registers the
 * descriptor before the first dispatch. See openpete_recomp_module.h. */

/** @brief Dispatch of the sentinel module: log the address and abort.
 *
 * Reached only by a dispatch before any recomp module was registered.
 * @param cpu Unused.
 * @param addr Guest address the dispatch targeted.
 * @return Does not return. */
static int recomp_unregistered_dispatch(CPUState* cpu, uint32_t addr) {
    (void)cpu;
    fprintf(stderr,
            "openpete: dispatch to 0x%08X before a recomp module was "
            "registered - game main must call openpete_recomp_register()\n",
            addr);
    abort();
}

static const openpete_recomp_module s_recomp_sentinel = {
    OPENPETE_RECOMP_ABI_VERSION, "unregistered", "",
    recomp_unregistered_dispatch,
};
static const openpete_recomp_module* s_recomp_module = &s_recomp_sentinel;

int openpete_recomp_register(const openpete_recomp_module* m) {
    if (!m || m->abi_version != OPENPETE_RECOMP_ABI_VERSION) {
        OPL_ERROR_F("dispatch",
                    "recomp module refused: abi_version %u != %u",
                    m ? m->abi_version : 0u, OPENPETE_RECOMP_ABI_VERSION);
        return -1;
    }
    s_recomp_module = m;
    OPL_INFO_F("dispatch", "recomp module registered (codegen=%s rom=%s)",
               m->codegen_stamp ? m->codegen_stamp : "?",
               (m->rom_sha256 && m->rom_sha256[0]) ? m->rom_sha256 : "-");
    return 0;
}

const openpete_recomp_module* openpete_recomp_module_get(void) {
    return s_recomp_module;
}

/* The canonical-SX FIFO (openpete_canonical_sx{0,1,2}_q16) is defined in
 * core/renderer/widescreen.c; the recompiler-generated C reaches it via
 * the psx_runtime.h externs. */

/* A call to an address the recompiled image does not cover (an
 * untranslated overlay function, a PSYQ symbol missing from the manifest)
 * is stubbed and execution continues rather than aborting.
 *
 * Rate-limited per-address logging: each unique miss-addr is logged on first
 * hit and every power-of-two subsequent hit so the log stays useful.  */
#define MISS_TABLE_SIZE 64
struct miss_entry { uint32_t addr; uint32_t hits; };
static struct miss_entry s_miss[MISS_TABLE_SIZE];
static int s_miss_overflow = 0;

/** @brief Count a dispatch to an uncovered address and log it on a rate limit.
 *
 * Keeps a 64-entry table of missed addresses with hit counts. The first
 * hit and every hit whose count is a power of two log a warning carrying
 * the caller's ra and sp. An address that finds no free entry is not
 * counted, and the first such overflow logs one suppression warning.
 * @param addr Guest address that no translation or BIOS handler covers.
 * @param ra Return address register of the caller, for the log line.
 * @param sp Stack pointer register of the caller, for the log line. */
static void note_miss(uint32_t addr, uint32_t ra, uint32_t sp) {
    int empty = -1;
    for (int i = 0; i < MISS_TABLE_SIZE; i++) {
        if (s_miss[i].addr == addr) {
            uint32_t n = ++s_miss[i].hits;
            if ((n & (n - 1u)) == 0u) {  /* powers of two */
                OPL_WARN_F("dispatch",
                           "stub miss 0x%08X (#%u) ra=0x%08X sp=0x%08X",
                           addr, n, ra, sp);
            }
            return;
        }
        if (empty < 0 && s_miss[i].addr == 0u) empty = i;
    }
    if (empty >= 0) {
        s_miss[empty].addr = addr;
        s_miss[empty].hits = 1u;
        OPL_WARN_F("dispatch",
                   "stub miss 0x%08X (#1) ra=0x%08X sp=0x%08X - likely overlay/untranslated",
                   addr, ra, sp);
    } else if (!s_miss_overflow) {
        s_miss_overflow = 1;
        OPL_WARN_F("dispatch",
                   "miss-tracker full; further unknown-addr warnings suppressed");
    }
}

void call_by_address(CPUState* cpu, uint32_t addr) {
    if (s_recomp_module->dispatch(cpu, addr)) return;

    /* A jump to 0xA0/0xB0/0xC0 is a BIOS service call, not a missing
     * translation: the BIOS is not part of the recompiled image. */
    extern int openpete_psx_bios_call(CPUState* cpu, uint32_t addr);
    if (openpete_psx_bios_call(cpu, addr)) return;

    /* Unknown target. Could be an overlay function that lives in the EXE
     * load image but wasn't part of the recompiler input, or a renamed PSYQ
     * symbol that fell off the manifest. Stub-and-continue: zero out v0 so
     * the caller sees a consistent "function returned 0" and keep going. */
    note_miss(addr, cpu ? cpu->ra : 0u, cpu ? cpu->sp : 0u);
    if (cpu) {
        cpu->v0 = 0u;
        cpu->v1 = 0u;
    }
    /* A stubbed call inside the game's main loop (an overlay-resident
     * update/draw pair, for example) means the recompiled main never
     * reaches a VSync(0). Force a cooperative yield so the host fiber can
     * drive frame_limit/max-runtime checks. The no-tick variant keeps the
     * frame counter tracking real VSync(0) calls; stubbed loops would
     * otherwise inflate it. */
    openpete_yield_to_host_no_tick();
}

/* Halt-at-PC: when set, psx_override_dispatch checks every dispatched
 * function entry against this address; on match, dumps 2 MB g_ram to the
 * configured path and exits the process cleanly. Used as the openpete-side
 * partner of scripts/pcsx-redux-ram-dump.lua's BP-on-PC dump, so both
 * engines capture state at the same function-entry instant rather than the
 * vsync-vs-syscall phase skew you'd get otherwise. Set once before fiber
 * dispatch; never written from the fiber. */
/* Pad-read probe: guest address of the function that reads the pad buffer,
 * supplied per-game (game_get_pad_read_pc) so core carries no game address.
 * Zero = no probe, which the census reports as UNMEASURED rather than as
 * "the game never read the pad". */
uint32_t    openpete_pad_read_pc         = 0u;

uint32_t    openpete_halt_at_pc          = 0u;
uint32_t    openpete_halt_hit_target     = 1u;  /* dump on Nth call (1 = first) */
static uint32_t openpete_halt_hit_count  = 0u;
const char* openpete_halt_dump_path      = NULL;
const char* openpete_halt_cpu_dump_path  = NULL;
static int  openpete_halt_done           = 0;

/* PCSX-Redux oracle anchor: when openpete_psxtrace_anchor_pc is non-zero
 * and the dispatch sees that address, fire openpete_psxtrace_dispatch_hook
 * (set by run.c when --psxtrace-verify is on). Same shape as halt-at-pc
 * but the hook keeps firing across the whole run (no one-shot done flag),
 * so each game frame ticks the verifier exactly once at the same logical
 * instant the trace was captured at. */
uint32_t    openpete_psxtrace_anchor_pc       = 0u;
uint32_t    openpete_psxtrace_anchor_hit_target = 1u;
/* Hits of the anchor PC since boot, counted whether or not a verifier is
 * attached; the CD read log stamps each read with it so a read can be
 * placed against the reference's <trace>.loads sidecar. */
uint32_t    openpete_psxtrace_anchor_hits     = 0u;
void      (*openpete_psxtrace_dispatch_hook)(uint32_t addr) = NULL;

/* OPENPETE_ANCHOR_VBL_LOG=PATH: one `hit vbl` line per anchor hit, the
 * openpete side of the per-record VBL stamp the reference writes into
 * each trace record. */
static FILE* s_anchor_vbl_log = NULL;
static int   s_anchor_vbl_log_checked = 0;
/** @brief Append one `hit vbl` line to the anchor VBL log when it is enabled.
 *
 * Opens the file named by OPENPETE_ANCHOR_VBL_LOG on the first call, writes
 * a `# hit vbl` header, and then one line per call with the anchor hit
 * count and the current openpete_vbl. Each line is flushed because the halt
 * path exits without flushing stdio. Does nothing when the variable is
 * unset, empty, or the open fails.
 * @param hit Anchor hit count to record. */
static void anchor_vbl_log(uint32_t hit) {
    if (!s_anchor_vbl_log_checked) {
        s_anchor_vbl_log_checked = 1;
        const char* p = getenv("OPENPETE_ANCHOR_VBL_LOG");
        if (p && *p) {
            s_anchor_vbl_log = fopen(p, "w");
            if (s_anchor_vbl_log) fprintf(s_anchor_vbl_log, "# hit vbl\n");
        }
    }
    if (s_anchor_vbl_log) {
        /* The halt path exits without flushing stdio. */
        fprintf(s_anchor_vbl_log, "%u %d\n", hit, SDL_GetAtomicInt(&openpete_vbl_device));
        fflush(s_anchor_vbl_log);
    }
}


/** @brief Write the CPU state at a halt as a 528-byte 'CSv1' dump to the halt CPU dump path.
 *
 * The format matches scripts/pcsx-redux-ram-dump.lua, so one diff helper
 * reads either side's dump. Layout (byte offsets):
 *   0    magic 0x31765343 'CSv1'
 *   4    32 GPRs (zero, at, v0..ra; MIPS register-number order)
 * 132    pc / hi / lo
 * 144    32 cop0 regs
 * 272    32 GTE data regs (cop2)
 * 400    32 GTE control regs (cop2c)
 * The cop0 and GTE banks are written as zeros. No effect when no CPU dump
 * path is set; an open failure is logged.
 * @param cpu CPU state.
 * @param pc_at_halt Address written as pc. */
static void openpete_halt_dump_cpu(CPUState* cpu, uint32_t pc_at_halt) {
    if (!openpete_halt_cpu_dump_path) return;
    FILE* fp = fopen(openpete_halt_cpu_dump_path, "wb");
    if (!fp) {
        OPL_ERROR_F("halt-at-pc", "cpu fopen %s failed: %s",
                    openpete_halt_cpu_dump_path, strerror(errno));
        return;
    }
    uint32_t magic = 0x31765343u;
    fwrite(&magic, 1, 4, fp);
    /* 32 GPRs in MIPS register-number order. CPUState exposes named fields,
     * but we know the layout matches: zero,at,v0,v1,a0..a3,t0..t7,
     * s0..s7,t8,t9,k0,k1,gp,sp,fp,ra. Use an inline aggregate. */
    uint32_t gpr[32] = {
        0u,        cpu->at,   cpu->v0,   cpu->v1,
        cpu->a0,   cpu->a1,   cpu->a2,   cpu->a3,
        cpu->t0,   cpu->t1,   cpu->t2,   cpu->t3,
        cpu->t4,   cpu->t5,   cpu->t6,   cpu->t7,
        cpu->s0,   cpu->s1,   cpu->s2,   cpu->s3,
        cpu->s4,   cpu->s5,   cpu->s6,   cpu->s7,
        cpu->t8,   cpu->t9,   cpu->k0,   cpu->k1,
        cpu->gp,   cpu->sp,   cpu->fp,   cpu->ra,
    };
    fwrite(gpr, 4, 32, fp);
    fwrite(&pc_at_halt, 1, 4, fp);
    fwrite(&cpu->hi, 1, 4, fp);
    fwrite(&cpu->lo, 1, 4, fp);
    /* cop0 / GTE banks: openpete's CPUState doesn't directly expose 32-word
     * arrays for these (the recomp emits per-instruction state), so we
     * write zeros. The diff helper's primary use is sp-based stack masking,
     * which only needs gpr[29]. Future work: pipe through psxrecomp's
     * gte_data / gte_ctrl mirrors when those become dump-time stable. */
    static const uint32_t zero32[32] = {0};
    fwrite(zero32, 4, 32, fp);  /* cop0  */
    fwrite(zero32, 4, 32, fp);  /* gte_data */
    fwrite(zero32, 4, 32, fp);  /* gte_ctrl */
    fclose(fp);
    OPL_INFO_F("halt-at-pc",
               "cpu wrote 528 bytes to %s (pc=0x%08X gp=0x%08X sp=0x%08X ra=0x%08X)",
               openpete_halt_cpu_dump_path,
               pc_at_halt, cpu->gp, cpu->sp, cpu->ra);
}

/** @brief Dump guest RAM (and the CPU state) at a halt address and exit with code 0.
 *
 * Writes the 2 MB guest RAM to the halt dump path (`./ram.bin` when none is
 * set), then the CPU dump through openpete_halt_dump_cpu when a CPU state is
 * given, syncs the log file, and leaves through _Exit(0) to skip the host
 * fiber teardown, which can hang under headless GL. An open or short-write
 * failure is logged and the exit still happens.
 * @param cpu CPU state to dump; may be NULL to skip the CPU dump.
 * @param addr Guest address of the halt, written into the log and CPU dump. */
static void openpete_halt_dump_and_exit(CPUState* cpu, uint32_t addr) {
    openpete_halt_done = 1;
    const char* path = openpete_halt_dump_path
        ? openpete_halt_dump_path : "./ram.bin";
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        OPL_ERROR_F("halt-at-pc", "fopen %s failed: %s",
                    path, strerror(errno));
    } else {
        const size_t ram_size = 2u * 1024u * 1024u;
        size_t n = fwrite(openpete_psx_get_ram(), 1, ram_size, fp);
        fclose(fp);
        if (n != ram_size) {
            OPL_ERROR_F("halt-at-pc",
                        "short write to %s: %zu of %zu bytes",
                        path, n, ram_size);
        } else {
            OPL_INFO_F("halt-at-pc",
                       "halted at PC=0x%08X, wrote %zu bytes to %s",
                       addr, ram_size, path);
        }
    }
    if (cpu) openpete_halt_dump_cpu(cpu, addr);
    fflush(stdout);
    fflush(stderr);
    openpete_log_file_sync();
    /* Bypass the host-fiber teardown - we just want a clean exit with the
     * dump on disk. The host loop's normal exit path requires yielding back
     * through SDL/PsyCross which can hang under headless GL on WSL. */
    _Exit(0);
}

/* Hang watchdog: psx_override_dispatch fires at every translated-function
 * entry, so it sees roughly the full call sequence the game fiber walks
 * each frame. The host and game fibers are cooperatively scheduled on one
 * OS thread (libco), so an infinite loop in the game fiber also blocks the
 * host fiber. Three trips end such a loop: the entry count here and the
 * loop quanta in loop_hang_quantum, both counted on the game fiber since
 * `openpete_vbl` last changed, and the stall monitor, a separate thread
 * that measures wall time with the game fiber running and `openpete_vbl`
 * unchanged. (They key off vbl rather than `openpete_frame` so fast-boot
 * iterations - which advance vbl but not frame - don't look like hangs.)
 *
 * On trip: write three rings of (addr, ra, sp) to `OPENPETE_HANG_TRACE`
 * (default hang-trace.bin in the logs state directory, beside the log
 * file) and guest RAM to `OPENPETE_HANG_RAM` (default hang-ram.bin there),
 * tell the player (openpete_fatal_message) and exit 2 (openpete_crash_terminate_now). Format: u32
 * magic 'HNG2', then
 * for each ring a u32 count, a u32 head index, and count*{u32 addr, u32 ra,
 * u32 sp}: the entry ring for the tripping interval, the entry ring as it
 * stood at the previous vbl boundary, and the recent ring.
 * Disabled when `OPENPETE_HANG_THRESHOLD=0`. */
#define HANG_RING 4096u
struct hang_entry { uint32_t addr; uint32_t ra; uint32_t sp; };
static struct hang_entry s_hang_ring[HANG_RING];
static uint32_t s_hang_head = 0u;
/* Snapshot of the ring at the last frame-boundary tick. Tight loops fill the
 * primary ring with their own entries, hiding what was happening *before* the
 * loop began - the snapshot preserves it. */
static struct hang_entry s_hang_ring_prev[HANG_RING];
static uint32_t s_hang_head_prev = 0u;
/* The two rings above hold the FIRST HANG_RING dispatches of an interval, so a
 * loop that starts later than that leaves no trace in either. This one is
 * written on every dispatch and keeps only the most recent entries, which is
 * the loop itself. */
#define HANG_RECENT 512u
static struct hang_entry s_hang_recent[HANG_RECENT];
static uint32_t s_hang_recent_head = 0u;
static uint32_t s_hang_calls_since_tick = 0u;
static uint32_t s_hang_last_vbl = 0u;
static int s_hang_threshold_inited = 0;
static uint32_t s_hang_threshold = 50000000u;  /* env-overridable */
/* HANG_ARMED, HANG_STALL_REQUESTED (the stall monitor asks the game fiber to
 * dump at its next function entry or loop poll) or HANG_TRIPPED (one caller
 * owns the dump). Moves to HANG_TRIPPED only by compare-and-swap, so the
 * game fiber and the monitor thread never write the dump files together. */
enum { HANG_ARMED = 0, HANG_STALL_REQUESTED = 1, HANG_TRIPPED = 2 };
static SDL_AtomicInt s_hang_state = {HANG_ARMED};
/* Loop quanta counted at s_hang_quanta_vbl (loop_hang_quantum). */
static uint32_t s_hang_quanta = 0u, s_hang_quanta_max = 0u;
static int32_t  s_hang_quanta_vbl = -1;

/** @brief Take ownership of the hang dump.
 * @return 1 when the caller moved the state to HANG_TRIPPED, 0 when another caller owns it. */
static int hang_claim(void) {
    for (;;) {
        const int st = SDL_GetAtomicInt(&s_hang_state);
        if (st == HANG_TRIPPED) return 0;
        if (SDL_CompareAndSwapAtomicInt(&s_hang_state, st, HANG_TRIPPED)) return 1;
    }
}

/** @brief Write the default path of a hang-watchdog output file, `leaf` in the logs state directory, into out.
 *
 * The working directory of a launched game is arbitrary (the install
 * directory, the user's home, a system directory), so the dumps go where
 * the log file goes. Falls back to the working directory only when no
 * state directory resolves.
 * @param leaf File name.
 * @param out Receives the path.
 * @param cap Capacity of out. */
static void hang_output_path(const char* leaf, char* out, size_t cap) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    char dir[1024];
    if (openpete_paths_state_dir("logs", dir, sizeof dir, 1) == 0)
        snprintf(out, cap, "%s%c%s", dir, sep, leaf);
    else
        snprintf(out, cap, "%s", leaf);
}

/** @brief Trip the hang watchdog: write the dispatch rings and guest RAM, tell the player, and exit with code 2.
 *
 * The caller owns the dump (hang_claim). Called on the game fiber for every
 * trip but one: the stall monitor's fallback, which runs on the monitor
 * thread when the game fiber reaches no function entry and no loop poll
 * within STALL_HANDOFF_MS of the request. No entry or poll means the rings
 * are not being written; guest RAM may still be, so that image can be torn.
 * @param trip_addr The guest function whose entry crossed the threshold, or
 *     the header of the call-free loop whose quanta crossed theirs, or the
 *     entry or header that took a stall request; 0 for the fallback.
 * @param quanta For an entry or quanta trip, 0 or the loop quanta counted
 *     since `openpete_vbl` last changed.
 * @param stall_ms 0 for an entry or quanta trip; else the wall milliseconds
 *     the game fiber ran with `openpete_vbl` unchanged.
 * @param off_thread Nonzero when called on the stall monitor thread. */
static void openpete_hang_dump_and_exit(uint32_t trip_addr, uint32_t quanta,
                                        uint32_t stall_ms, int off_thread) {
    char trace_default[1100], ram_default[1100];
    hang_output_path("hang-trace.bin", trace_default, sizeof trace_default);
    hang_output_path("hang-ram.bin", ram_default, sizeof ram_default);
    const char* path = getenv("OPENPETE_HANG_TRACE");
    if (!path || !*path) path = trace_default;
    FILE* fp = fopen(path, "wb");
    if (fp) {
        uint32_t magic = 0x32474E48u;  /* 'HNG2' */
        uint32_t count = HANG_RING;
        uint32_t recent = HANG_RECENT;
        fwrite(&magic, 4, 1, fp);
        fwrite(&count, 4, 1, fp);
        fwrite(&s_hang_head, 4, 1, fp);
        fwrite(s_hang_ring, sizeof(s_hang_ring[0]), HANG_RING, fp);
        /* Pre-loop snapshot: ring as it stood at the last frame boundary. */
        fwrite(&count, 4, 1, fp);
        fwrite(&s_hang_head_prev, 4, 1, fp);
        fwrite(s_hang_ring_prev, sizeof(s_hang_ring_prev[0]), HANG_RING, fp);
        fwrite(&recent, 4, 1, fp);
        fwrite(&s_hang_recent_head, 4, 1, fp);
        fwrite(s_hang_recent, sizeof(s_hang_recent[0]), HANG_RECENT, fp);
        fclose(fp);
    }
    /* Also dump 2 MB RAM so post-mortem can inspect any address. */
    const char* rampath = getenv("OPENPETE_HANG_RAM");
    if (!rampath || !*rampath) rampath = ram_default;
    FILE* rp = fopen(rampath, "wb");
    if (rp) {
        const size_t ram_size = 2u * 1024u * 1024u;
        fwrite(openpete_psx_get_ram(), 1, ram_size, rp);
        fclose(rp);
    }
    if (stall_ms)
        OPL_ERROR_F("watchdog",
                    "hang detected: game fiber ran %u.%03u s wall with openpete_vbl=%d unchanged "
                    "(%u dispatch entries, %u loop quanta), %s at 0x%08X, wrote trace=%s ram=%s%s - exiting",
                    stall_ms / 1000u, stall_ms % 1000u, SDL_GetAtomicInt(&openpete_vbl),
                    s_hang_calls_since_tick,
                    s_hang_quanta_vbl == SDL_GetAtomicInt(&openpete_vbl) ? s_hang_quanta : 0u,
                    off_thread ? "dumped off-thread, no entry or poll took the request" : "taken",
                    trip_addr, path, rampath,
                    off_thread ? " (ram written while the guest ran: may be torn)" : "");
    else if (quanta)
        OPL_ERROR_F("watchdog",
                    "hang detected: %u loop quanta at header 0x%08X since openpete_vbl=%d "
                    "(%u dispatch entries), wrote trace=%s ram=%s - exiting",
                    quanta, trip_addr, SDL_GetAtomicInt(&openpete_vbl),
                    s_hang_calls_since_tick, path, rampath);
    else
        OPL_ERROR_F("watchdog",
                    "hang detected: %u dispatch entries since openpete_vbl=%u "
                    "(trip addr=0x%08X), wrote trace=%s ram=%s - exiting",
                    s_hang_calls_since_tick, s_hang_last_vbl, trip_addr,
                    path, rampath);
    char msg[2600];
    snprintf(msg, sizeof msg, "The game stopped responding and was closed. "
             "Diagnostic files were written to %s and %s.", path, rampath);
    openpete_fatal_message("OpenPete stopped responding", msg);
    fflush(stdout); fflush(stderr);
    /* stderr is a pipe drained by the log pump thread; exiting first would take the
     * watchdog's own diagnosis down with the process before it is copied. */
    openpete_log_file_sync();
    /* Other threads may be mid-wait; leave without running any destructor. */
    openpete_crash_terminate_now(2);
}

/* Per-function intercept gate emitted at the top of every generated body.
 * The registered game's override_dispatch runs its per-game arms; NULL
 * until registration and for a game without one. */
static int (*s_override_game)(CPUState* cpu, uint32_t addr);

void psx_set_override_dispatch_game(int (*fn)(CPUState* cpu, uint32_t addr)) {
    s_override_game = fn;
}

/* The oracle's clock replay counts the guest's own entries of each
 * function. Two kinds of gate hit are not the guest's: every hit inside a
 * host-initiated guest call (interrupt handlers, BIOS services, host
 * features), which the depth counts, and the one hit of a hook that runs
 * the body of the very call the guest made (the gate already counted it),
 * which s_gate_redispatch names while the game's override for that
 * address runs. */
static int      s_gate_depth = 0;
static uint32_t s_gate_redispatch = 0;
static uint64_t s_redispatch_skips = 0;   /* re-dispatches that skipped the entry observers */

void openpete_psxtrace_gate_host_enter(void) { s_gate_depth++; }
void openpete_psxtrace_gate_host_leave(void) { s_gate_depth--; }

/** @brief Pass one gate hit to the clock replay unless it is a host re-dispatch or inside a host-initiated call.
 *
 * A hit at the address the running override was entered for is that
 * override's re-dispatch of the guest's own call: it clears the mark and
 * is not counted. The replay's VBLANK delivery runs in
 * psyq_sched_clock_gate.
 * @param cpu Guest CPU state.
 * @param addr Guest address of the function entered. */
static void gate_clock(CPUState* cpu, uint32_t addr) {
    if (addr == s_gate_redispatch) { s_gate_redispatch = 0; return; }
    if (s_gate_depth || !openpete_sched_clock_replay()) return;
    extern void psyq_sched_clock_gate(CPUState* cpu, uint32_t addr) __attribute__((weak));
    if (psyq_sched_clock_gate) psyq_sched_clock_gate(cpu, addr);
}

/** @brief Run the game's override arms for addr with addr marked as the address a re-dispatch re-enters.
 *
 * The previous mark is restored afterwards, so the marks nest with the
 * overrides.
 * @param cpu Guest CPU state.
 * @param addr Guest address of the function entered.
 * @return The game's verdict: nonzero when the original must not run; 0 when the game has no override. */
static int gate_override(CPUState* cpu, uint32_t addr) {
    int (*const game)(CPUState*, uint32_t) = s_override_game;
    if (!game) return 0;
    const uint32_t outer = s_gate_redispatch;
    s_gate_redispatch = addr;
    const int handled = game(cpu, addr);
    s_gate_redispatch = outer;
    return handled;
}
/* The game's loop anchor and its entries: one per game tick. */
static uint32_t s_loop_anchor_pc = 0u;
static uint32_t s_loop_anchor_ticks = 0u;
static uint64_t s_loop_insns_anchor = 0u;   /* loop instructions since the loop anchor's last entry */

uint64_t openpete_loop_insns_since_anchor(void) { return s_loop_insns_anchor; }

void openpete_loop_anchor_set(uint32_t pc) { s_loop_anchor_pc = pc; }

/* The game's pad-ring consumer, whose entries openpete_consume counts. */
static uint32_t s_consume_pc = 0u;

void openpete_consume_pc_set(uint32_t pc) { s_consume_pc = pc; }

/** @brief Count a function entry against the per-tick anchors: the loop anchor and the pad-ring consumer.
 *
 * @param addr The entered function's guest address. */
static inline void count_entry_anchors(uint32_t addr) {
    if (addr == s_loop_anchor_pc) { s_loop_anchor_ticks++; s_loop_insns_anchor = 0u; }
    if (addr == s_consume_pc) openpete_feed_consume_enter();
}

uint32_t openpete_loop_anchor_ticks(void) { return s_loop_anchor_ticks; }

/* Function entries since openpete_vbl last changed, for the spin tick. */
static uint32_t s_spin_calls_since_vbl = 0u;
static uint32_t s_spin_last_vbl = 0u;

/** @brief Pass one VBL to a guest spin that has entered 2048 functions without one.
 *
 * Counts function entries since `openpete_vbl` last changed; every 2048th
 * calls psyq_sched_spin_tick, which under the schedule charges one VBL
 * while a disc read's completion is deferred. The entries are counted
 * whether or not the hang watchdog is armed.
 * @param cpu Guest CPU state.
 * @param vbl_now `openpete_vbl` at this entry.
 * @param addr Guest address of the function entered. */
static void spin_tick(CPUState* cpu, uint32_t vbl_now, uint32_t addr) {
    if (vbl_now != s_spin_last_vbl) {
        s_spin_last_vbl = vbl_now;
        s_spin_calls_since_vbl = 0u;
        return;
    }
    /* A spin that polls a deferred disc read without waiting on VSync
     * spent VBLs on the reference; every so many entries without a VBL,
     * the schedule passes one here (psyq_thunks.cpp). */
    if ((++s_spin_calls_since_vbl & 2047u) != 2047u) return;
    extern void psyq_sched_spin_tick(CPUState* cpu, uint32_t addr) __attribute__((weak));
    openpete_psxtrace_gate_host_enter();
    if (psyq_sched_spin_tick) psyq_sched_spin_tick(cpu, addr);
    openpete_psxtrace_gate_host_leave();
}

/* Stall monitor: a host thread that samples, every STALL_SAMPLE_MS of wall
 * time, `openpete_vbl` and openpete_game_running. It counts the wall time
 * the game fiber runs with `openpete_vbl` unchanged and trips at
 * OPENPETE_HANG_STALL_SECONDS. This catches the loops the game-fiber trips
 * miss or reach late: a loop with neither calls nor polled back-edges
 * (a storing loop), and a loop that polls on few of its back-edges. A
 * parked game fiber (the host loop runs: a host pause, the devui, a window
 * drag, the pacer's sleep) adds no time and keeps the count; only a change
 * of `openpete_vbl` resets it. One sample adds at most 4 * STALL_SAMPLE_MS,
 * so a process stopped by a debugger or a system suspend adds one capped
 * sample, not the time it stood still.
 *
 * On a trip the monitor sets HANG_STALL_REQUESTED; the game fiber takes it at
 * its next function entry or loop poll (stall_take) and dumps there with the
 * rings it writes. When none comes within STALL_HANDOFF_MS the game fiber is
 * in a loop with neither, which writes no ring, and the monitor dumps itself.
 * The monitor reads only atomics and stores into no guest RAM. */
#define STALL_SAMPLE_MS     25u
#define STALL_HANDOFF_MS    2000u
#define STALL_SECONDS_DEFAULT 30u
static uint32_t      s_stall_ms_limit = STALL_SECONDS_DEFAULT * 1000u;
static SDL_AtomicInt s_stall_ms;           /* the current stretch, ms */
static SDL_AtomicInt s_stall_max_ms;       /* the longest stretch this run, ms */
static SDL_AtomicInt s_stall_max_vbl;      /* openpete_vbl during it */
static SDL_AtomicInt s_stall_samples;      /* samples taken, the engagement count */

/** @brief Take the stall monitor's dump request on the game fiber and dump at this entry or loop poll.
 * @param addr The function entered or the loop header polled. */
static void stall_take(uint32_t addr) {
    if (!SDL_CompareAndSwapAtomicInt(&s_hang_state, HANG_STALL_REQUESTED, HANG_TRIPPED)) return;
    openpete_hang_dump_and_exit(addr, 0u, (uint32_t)SDL_GetAtomicInt(&s_stall_ms), 0);
}

/** @brief Body of the stall monitor thread: sample the game fiber's progress until a trip exits the process.
 * @param arg Unused.
 * @return Never returns while the process runs. */
static int SDLCALL stall_monitor_main(void* arg) {
    (void)arg;
    int32_t  last_vbl = SDL_GetAtomicInt(&openpete_vbl);
    uint32_t flat_ms = 0u, request_ms = 0u;
    uint64_t prev = SDL_GetTicksNS();  /* clock-guard: wall */
    for (;;) {
        SDL_Delay(STALL_SAMPLE_MS);
        const uint64_t now = SDL_GetTicksNS();  /* clock-guard: wall */
        uint64_t dt = (now - prev) / 1000000u;
        prev = now;
        if (dt > 4u * STALL_SAMPLE_MS) dt = 4u * STALL_SAMPLE_MS;
        const int32_t vbl = SDL_GetAtomicInt(&openpete_vbl);
        SDL_AddAtomicInt(&s_stall_samples, 1);
        if (vbl != last_vbl) {
            last_vbl = vbl;
            flat_ms = 0u;
            request_ms = 0u;
            SDL_CompareAndSwapAtomicInt(&s_hang_state, HANG_STALL_REQUESTED, HANG_ARMED);
        } else if (SDL_GetAtomicInt(&openpete_game_running)) {
            flat_ms += (uint32_t)dt;
        }
        SDL_SetAtomicInt(&s_stall_ms, (int)flat_ms);
        if ((int)flat_ms > SDL_GetAtomicInt(&s_stall_max_ms)) {
            SDL_SetAtomicInt(&s_stall_max_ms, (int)flat_ms);
            SDL_SetAtomicInt(&s_stall_max_vbl, vbl);
        }
        if (!s_stall_ms_limit || flat_ms < s_stall_ms_limit) continue;
        if (!request_ms) {
            if (SDL_CompareAndSwapAtomicInt(&s_hang_state, HANG_ARMED, HANG_STALL_REQUESTED))
                request_ms = flat_ms;
            continue;
        }
        if (flat_ms - request_ms < STALL_HANDOFF_MS) continue;
        if (SDL_CompareAndSwapAtomicInt(&s_hang_state, HANG_STALL_REQUESTED, HANG_TRIPPED))
            openpete_hang_dump_and_exit(0u, 0u, flat_ms, 1);
    }
    return 0;
}

/** @brief Read OPENPETE_HANG_STALL_SECONDS and start the stall monitor thread; 0 seconds still measures but never trips. */
static void stall_monitor_start(void) {
    const char* env = getenv("OPENPETE_HANG_STALL_SECONDS");
    if (env && *env) s_stall_ms_limit = (uint32_t)strtoul(env, NULL, 0) * 1000u;
    SDL_Thread* th = SDL_CreateThread(stall_monitor_main, "openpete-stall", NULL);
    if (!th) {
        OPL_WARN_F("watchdog", "stall monitor thread not started: %s", SDL_GetError());
        return;
    }
    SDL_DetachThread(th);
}

/** @brief Read OPENPETE_HANG_THRESHOLD on the first call and start the stall monitor; 0 disarms every watchdog trip. */
static void hang_threshold_init(void) {
    if (s_hang_threshold_inited) return;
    const char* env = getenv("OPENPETE_HANG_THRESHOLD");
    if (env && *env) s_hang_threshold = (uint32_t)strtoul(env, NULL, 0);
    s_hang_threshold_inited = 1;
    if (s_hang_threshold) stall_monitor_start();
}

/** @brief Note a function entry for the hang watchdog: take a stall request, count the entry, and record it in the rings.
 *
 * Counts entries since `openpete_vbl` last changed and trips the watchdog
 * past OPENPETE_HANG_THRESHOLD of them; at each change it snapshots the
 * entry ring. Does nothing while the watchdog is disarmed or has tripped.
 * @param cpu Guest CPU state; may be NULL.
 * @param addr The entered function's guest address.
 * @param vbl_now `openpete_vbl` at this entry. */
static inline void hang_note_entry(CPUState* cpu, uint32_t addr, uint32_t vbl_now) {
    const int hang_state = SDL_GetAtomicInt(&s_hang_state);
    if (!s_hang_threshold || hang_state == HANG_TRIPPED) return;
    if (hang_state == HANG_STALL_REQUESTED) stall_take(addr);
    if (vbl_now != s_hang_last_vbl) {
        /* Vbl boundary: snapshot the ring before the next vbl interval's
         * dispatches overwrite it. If a hang trips during the next
         * interval, the snapshot still shows what got us here. */
        memcpy(s_hang_ring_prev, s_hang_ring, sizeof(s_hang_ring));
        s_hang_head_prev = s_hang_head;
        s_hang_last_vbl = vbl_now;
        s_hang_calls_since_tick = 0u;
    } else if (++s_hang_calls_since_tick > s_hang_threshold && hang_claim()) {
        openpete_hang_dump_and_exit(addr, 0u, 0u, 0);
    }
    /* Only fill the ring for the first HANG_RING dispatches of each
     * frame. A tight loop produces 2-3 distinct addrs and trivially
     * overwrites useful state; capping per-frame writes preserves the
     * path *into* the loop. */
    if (s_hang_calls_since_tick < HANG_RING) {
        s_hang_ring[s_hang_head].addr = addr;
        s_hang_ring[s_hang_head].ra   = cpu ? cpu->ra : 0u;
        s_hang_ring[s_hang_head].sp   = cpu ? cpu->sp : 0u;
        s_hang_head = (s_hang_head + 1u) % HANG_RING;
    }
    s_hang_recent[s_hang_recent_head].addr = addr;
    s_hang_recent[s_hang_recent_head].ra   = cpu ? cpu->ra : 0u;
    s_hang_recent[s_hang_recent_head].sp   = cpu ? cpu->sp : 0u;
    s_hang_recent_head = (s_hang_recent_head + 1u) % HANG_RECENT;
}

int psx_override_dispatch(CPUState* cpu, uint32_t addr) {
    hang_threshold_init();
    const uint32_t vbl_now = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    /* A body the running override re-enters for the guest's own call (a mod
     * link's base(), a hook that runs the original) is not a guest entry:
     * the observers below already saw this call at its first entry. */
    const int redispatch = (addr == s_gate_redispatch);
    hang_note_entry(cpu, addr, vbl_now);
    if (redispatch) {
        s_redispatch_skips++;
        gate_clock(cpu, addr);
        return gate_override(cpu, addr);
    }
    spin_tick(cpu, vbl_now, addr);
    count_entry_anchors(addr);
    /* Pad-read probe: the guest function that consumes the pad buffer. Sited
     * here because every translated function entry passes through this call,
     * ahead of every game and mod override arm. */
    if (openpete_pad_read_pc != 0u && addr == openpete_pad_read_pc)
        openpete_pace_pad_read();

    if (openpete_halt_at_pc != 0u && addr == openpete_halt_at_pc &&
        !openpete_halt_done) {
        openpete_halt_hit_count++;
        if (openpete_halt_hit_count >= openpete_halt_hit_target) {
            openpete_halt_dump_and_exit(cpu, addr);
            /* unreachable */
        }
    }
    if (openpete_psxtrace_anchor_pc != 0u &&
        addr == openpete_psxtrace_anchor_pc) {
        openpete_psxtrace_anchor_hits++;
        if (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target) {
            /* The reference's VBLANK at this record, charged before the
             * hash (psxtrace_schedule.c). */
            extern void psyq_sched_anchor(CPUState* cpu, uint32_t record) __attribute__((weak));
            if (psyq_sched_anchor)
                psyq_sched_anchor(cpu, openpete_psxtrace_anchor_hits - openpete_psxtrace_anchor_hit_target);
        }
        anchor_vbl_log(openpete_psxtrace_anchor_hits);
        if (openpete_psxtrace_dispatch_hook &&
            openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target) {
            openpete_psxtrace_dispatch_hook(addr);
        }
    }
    gate_clock(cpu, addr);
    return gate_override(cpu, addr);
}

/* --- Critical sections ---------------------------------------------------
 * The kernel masks interrupts before it jumps to the executable, so the
 * flag starts set. EnterCriticalSection (syscall a0 = 1) sets it and returns
 * whether interrupts were enabled; ExitCriticalSection (a0 = 2) and the
 * kernel calls whose bodies end in leaveCriticalSection clear it. A VBLANK
 * that falls due while it is set is latched by the delivery side and taken
 * when it clears (openpete_crit_leave). */
static int      s_crit = 1;
static uint32_t s_crit_first_clear_ra = 0u;     /* return address of the first clearing call */
static int32_t  s_crit_first_clear_vbl = -1;    /* openpete_vbl there, -1 before it */
static uint32_t s_crit_enters = 0u, s_crit_exits = 0u, s_crit_kernel_leaves = 0u;

int openpete_crit_masked(void) { return s_crit; }

void openpete_crit_set(int masked) { s_crit = masked ? 1 : 0; }

/** @brief Clear the critical-section flag and take a VBLANK latched while it was set.
 *
 * Records the first clearing call's return address for the probe. The
 * latched VBLANK runs through the delivery side's flush.
 * @param cpu Guest CPU state at the leaving call. */
static void crit_clear(CPUState* cpu) {
    if (!s_crit) return;
    s_crit = 0;
    if (s_crit_first_clear_vbl < 0) {
        s_crit_first_clear_ra = cpu ? cpu->ra : 0u;
        s_crit_first_clear_vbl = SDL_GetAtomicInt(&openpete_vbl);
        OPL_INFO_F("crit", "interrupts first enabled at ra=0x%08X vbl=%d",
                   s_crit_first_clear_ra, s_crit_first_clear_vbl);
    }
    extern void psyq_crit_flush(CPUState* cpu) __attribute__((weak));
    if (psyq_crit_flush && cpu) psyq_crit_flush(cpu);
}

void openpete_crit_leave(CPUState* cpu) {
    s_crit_kernel_leaves++;
    crit_clear(cpu);
}

void openpete_crit_report(uint32_t* first_clear_ra, int32_t* first_clear_vbl,
                          uint32_t* enters, uint32_t* exits, uint32_t* kernel_leaves) {
    *first_clear_ra = s_crit_first_clear_ra;
    *first_clear_vbl = s_crit_first_clear_vbl;
    *enters = s_crit_enters;
    *exits = s_crit_exits;
    *kernel_leaves = s_crit_kernel_leaves;
}

void psx_syscall(CPUState* cpu, uint32_t code) {
    if (cpu && cpu->a0 == 1u) {
        /* EnterCriticalSection: 1 when interrupts were enabled. */
        s_crit_enters++;
        cpu->v0 = s_crit ? 0u : 1u;
        s_crit = 1;
        return;
    }
    if (cpu && cpu->a0 == 2u) {
        s_crit_exits++;
        crit_clear(cpu);
        return;
    }
    OPL_WARN_F("dispatch", "syscall code=0x%X a0=%u (ra=0x%08X) - ignored",
               code, cpu ? cpu->a0 : 0u, cpu ? cpu->ra : 0u);
}

/* --- Polling-loop delivery -----------------------------------------------
 * The accumulator holds the instructions the classified loops have run
 * since the last VBL, scaled by 2250000 so that one VBL of instructions at
 * one instruction per cycle, 33868800 x 37609 / 2250000 = 566120.755, is
 * the whole number LOOP_QUANTUM. */
#define LOOP_SCALE   2250000ull
#define LOOP_QUANTUM (33868800ull * 37609ull)
static uint64_t s_loop_acc = 0u;
static uint64_t s_loop_acc_max = 0u;        /* per-run maximum before a VBL */
static uint32_t s_loop_quanta = 0u;         /* quanta since the last VBL */
static uint32_t s_loop_quanta_max = 0u;
static uint64_t s_loop_polls = 0u, s_loop_trips = 0u;
static uint32_t s_loop_last_header = 0u;
static int      s_loop_delivery = -1;       /* OPENPETE_LOOP_DELIVERY, read once */

/** @brief Return whether loop delivery is on, reading OPENPETE_LOOP_DELIVERY on the first call (unset or empty means on). */
static int loop_delivery_enabled(void) {
    if (s_loop_delivery < 0) {
        const char* env = getenv("OPENPETE_LOOP_DELIVERY");
        s_loop_delivery = (env && *env) ? (atoi(env) != 0) : 1;
    }
    return s_loop_delivery;
}

/* Per-site census over every classified loop header that polls, in first
 * poll order (open addressing on the header). A row's polls count its taken
 * back-edges; its runs count polls whose previous poll was at another header
 * (an entry that iterated at least once; each outer iteration of a nested
 * loop is one run of the outer header); its quanta count the quanta reached
 * there. A site with no row never took its back-edge: it was not entered, or
 * its loop closed on its first test. */
#define LOOP_SITES     1024u   /* > the largest game's emission sites (Spyro 3: 372) */
#define LOOP_SITE_HASH 2048u
struct loop_site {
    uint32_t hdr;
    uint32_t runs, quanta;
    uint64_t polls;
    int32_t  first_poll_vbl, first_quantum_vbl;
};
static struct loop_site s_loop_site[LOOP_SITES];
static uint16_t s_loop_site_slot[LOOP_SITE_HASH];   /* index + 1, 0 empty */
static uint32_t s_loop_sites = 0u, s_loop_sites_dropped = 0u;
static uint32_t s_loop_site_last = UINT32_MAX;     /* row of the previous poll's header */

/** @brief Find or add the census row of a loop header.
 * @param header The loop's header address.
 * @return The row index, or UINT32_MAX when the table is full. */
static uint32_t loop_site_row(uint32_t header) {
    uint32_t h = (header >> 2) * 2654435761u;
    for (uint32_t probe = 0; probe < LOOP_SITE_HASH; probe++) {
        const uint32_t slot = (h + probe) & (LOOP_SITE_HASH - 1u);
        const uint32_t idx = s_loop_site_slot[slot];
        if (idx && s_loop_site[idx - 1u].hdr == header) return idx - 1u;
        if (idx) continue;
        if (s_loop_sites == LOOP_SITES) break;
        struct loop_site* r = &s_loop_site[s_loop_sites];
        memset(r, 0, sizeof *r);
        r->hdr = header;
        r->first_poll_vbl = SDL_GetAtomicInt(&openpete_vbl);
        r->first_quantum_vbl = -1;
        s_loop_site_slot[slot] = (uint16_t)(++s_loop_sites);
        return s_loop_sites - 1u;
    }
    s_loop_sites_dropped++;
    return UINT32_MAX;
}

/** @brief Count one back-edge poll against its loop header: the poll, a run when the previous poll was at another header, and the quantum it reached.
 * @param header The loop's header address.
 * @param quantum Nonzero when this poll reached a quantum. */
static void loop_site_note(uint32_t header, int quantum) {
    uint32_t i = s_loop_site_last;
    const int same = (i != UINT32_MAX && s_loop_site[i].hdr == header);
    if (!same) i = loop_site_row(header);
    s_loop_site_last = i;
    if (i == UINT32_MAX) return;
    struct loop_site* r = &s_loop_site[i];
    r->polls++;
    if (!same) r->runs++;
    if (!quantum) return;
    if (!r->quanta) r->first_quantum_vbl = SDL_GetAtomicInt(&openpete_vbl);
    r->quanta++;
}

/* Hang watchdog over the call-free loop class: quanta reached since
 * openpete_vbl last changed. A call-free loop enters no function, so the
 * entry count in psx_override_dispatch never moves while one spins; a VBL
 * delivered and held (the critical-section flag set) leaves openpete_vbl
 * where it is, so a spin the held VBLs cannot end still counts here. A
 * quantum is LOOP_QUANTUM / LOOP_SCALE (about 566,121) instructions summed
 * over polled back-edges only: a loop body with stores does not poll, and a
 * loop that polls on few of its back-edges reaches a quantum only after many
 * times that many instructions, so the guest and wall time to
 * HANG_LOOP_QUANTA quanta have no fixed bound (the stall monitor bounds the
 * wall time). Armed with the entry watchdog (OPENPETE_HANG_THRESHOLD=0
 * disarms every trip). */
#define HANG_LOOP_QUANTA 6000u

/** @brief Count a quantum against the hang watchdog and trip it at HANG_LOOP_QUANTA quanta with `openpete_vbl` unchanged.
 * @param header The loop's header address, the trip address. */
static void loop_hang_quantum(uint32_t header) {
    const int32_t vbl = SDL_GetAtomicInt(&openpete_vbl);
    if (vbl != s_hang_quanta_vbl) { s_hang_quanta_vbl = vbl; s_hang_quanta = 0u; }
    if (++s_hang_quanta > s_hang_quanta_max) s_hang_quanta_max = s_hang_quanta;
    hang_threshold_init();
    if (s_hang_threshold && s_hang_quanta >= HANG_LOOP_QUANTA && hang_claim())
        openpete_hang_dump_and_exit(header, s_hang_quanta, 0u, 0);
}

/* Idle check of loops classified pure (OPENPETE_LOOP_PURE_CHECK=1): a store-free
 * poll loop rereads the same memory into the same registers, so between two
 * consecutive back-edge polls of one header inside one VBL every general
 * register, hi and lo hold the same values unless the polled memory changed,
 * which exits the loop. An iteration that changes a register is counted
 * against the header as a misclassification candidate. */
#define PURE_REGS 33u /* at..ra, hi, lo */
static int      s_pure_check = -1;
static uint32_t s_pure_hdr[LOOP_SITES], s_pure_iters[LOOP_SITES], s_pure_changed[LOOP_SITES];
static int32_t  s_pure_first_reg[LOOP_SITES];
static uint32_t s_pure_sites = 0u;
static uint32_t s_pure_last_hdr = 0u;
static int32_t  s_pure_last_vbl = -1;
static uint64_t s_pure_last_poll = 0u;
static uint32_t s_pure_last_regs[PURE_REGS];

/** @brief Copy the general registers (at..ra), hi and lo into a snapshot.
 * @param cpu Guest CPU state.
 * @param out PURE_REGS words. */
static void pure_snapshot(const CPUState* cpu, uint32_t* out) {
    memcpy(out, &cpu->at, 31u * sizeof(uint32_t));
    out[31] = cpu->hi;
    out[32] = cpu->lo;
}

/** @brief Compare a pure site's registers with its previous back-edge poll and count an iteration that changed one.
 * @param cpu Guest CPU state at the back edge.
 * @param header The loop's header address. */
static void loop_pure_check(const CPUState* cpu, uint32_t header) {
    if (s_pure_check < 0) {
        const char* env = getenv("OPENPETE_LOOP_PURE_CHECK");
        s_pure_check = (env && *env && *env != '0');
    }
    if (!s_pure_check) return;
    uint32_t regs[PURE_REGS];
    pure_snapshot(cpu, regs);
    int32_t vbl = SDL_GetAtomicInt(&openpete_vbl);
    int consecutive = header == s_pure_last_hdr && vbl == s_pure_last_vbl &&
                      s_loop_polls == s_pure_last_poll + 1u;
    if (consecutive) {
        uint32_t i = 0;
        while (i < s_pure_sites && s_pure_hdr[i] != header) i++;
        if (i == s_pure_sites && s_pure_sites < LOOP_SITES) {
            s_pure_hdr[i] = header;
            s_pure_first_reg[i] = -1;
            s_pure_sites++;
        }
        if (i < s_pure_sites) {
            s_pure_iters[i]++;
            for (uint32_t r = 0; r < PURE_REGS; r++) {
                if (regs[r] == s_pure_last_regs[r]) continue;
                s_pure_changed[i]++;
                if (s_pure_first_reg[i] < 0) s_pure_first_reg[i] = (int32_t)r + 1;
                break;
            }
        }
    }
    s_pure_last_hdr = header;
    s_pure_last_vbl = vbl;
    s_pure_last_poll = s_loop_polls;
    memcpy(s_pure_last_regs, regs, sizeof regs);
}

void openpete_loop_site_census_log(void) {
    OPL_INFO_F("delivery", "loop sites: %u polled, %u dropped (table full); watchdog: max %u quanta at one vbl, "
               "trip at %u; re-dispatches past the entry observers: %llu",
               s_loop_sites, s_loop_sites_dropped, s_hang_quanta_max, HANG_LOOP_QUANTA,
               (unsigned long long)s_redispatch_skips);
    OPL_INFO_F("watchdog", "stall monitor: %d samples, longest game-fiber run with openpete_vbl unchanged %d ms "
               "at openpete_vbl=%d; trip at %u ms%s",
               SDL_GetAtomicInt(&s_stall_samples), SDL_GetAtomicInt(&s_stall_max_ms), SDL_GetAtomicInt(&s_stall_max_vbl), s_stall_ms_limit,
               s_hang_threshold ? "" : " (watchdog disarmed, monitor not started)");
    for (uint32_t i = 0; i < s_loop_sites; i++) {
        const struct loop_site* r = &s_loop_site[i];
        OPL_INFO_F("delivery", "loop site 0x%08X: %llu polls, %u runs, %u quanta, first poll at vbl %d, "
                   "first quantum at vbl %d",
                   r->hdr, (unsigned long long)r->polls, r->runs, r->quanta, r->first_poll_vbl,
                   r->first_quantum_vbl);
    }
    if (s_pure_check <= 0) return;
    /* Register numbering: 1..31 = at..ra, 32 = hi, 33 = lo. */
    uint32_t flagged = 0u;
    for (uint32_t i = 0; i < s_pure_sites; i++) {
        if (s_pure_changed[i]) flagged++;
        OPL_INFO_F("delivery", "pure check 0x%08X: %u iterations compared, %u changed a register%s",
                   s_pure_hdr[i], s_pure_iters[i], s_pure_changed[i],
                   s_pure_changed[i] ? " (misclassified candidate)" : "");
        if (s_pure_changed[i])
            OPL_INFO_F("delivery", "pure check 0x%08X: first changed register %d",
                       s_pure_hdr[i], s_pure_first_reg[i]);
    }
    OPL_INFO_F("delivery", "pure check: %u site(s) compared, %u flagged", s_pure_sites, flagged);
}

void openpete_loop_vbl(void) {
    s_loop_acc = 0u;
    s_loop_quanta = 0u;
}

void openpete_loop_report(uint64_t* polls, uint64_t* trips, uint64_t* acc_max_insns,
                          uint32_t* quanta_max, uint32_t* last_header, int* enabled) {
    *polls = s_loop_polls;
    *trips = s_loop_trips;
    *acc_max_insns = s_loop_acc_max / LOOP_SCALE;
    *quanta_max = s_loop_quanta_max;
    *last_header = s_loop_last_header;
    *enabled = loop_delivery_enabled();
}

void psx_loop_poll(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t pure) {
    s_loop_polls++;
    if (SDL_GetAtomicInt(&s_hang_state) == HANG_STALL_REQUESTED) stall_take(header);
    if (pure == 1u) loop_pure_check(cpu, header);
    s_loop_insns_anchor += insns;
    s_loop_acc += (uint64_t)insns * LOOP_SCALE;
    if (s_loop_acc > s_loop_acc_max) s_loop_acc_max = s_loop_acc;
    const int quantum = s_loop_acc >= LOOP_QUANTUM;
    loop_site_note(header, quantum);
    if (!quantum) return;
    /* One VBL of instructions has run since the last VBL. */
    s_loop_trips++;
    s_loop_last_header = header;
    s_loop_acc -= LOOP_QUANTUM;
    if (++s_loop_quanta > s_loop_quanta_max) s_loop_quanta_max = s_loop_quanta;
    loop_hang_quantum(header);
    if (!loop_delivery_enabled()) return;
    extern void psyq_deliver_loop_vbl(CPUState* cpu, uint32_t header, uint32_t pure) __attribute__((weak));
    if (psyq_deliver_loop_vbl) psyq_deliver_loop_vbl(cpu, header, pure);
}
