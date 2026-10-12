// core/psx/dispatch.c - psx_runtime.h glue not provided by generated C.
//
// The recompiled C calls into a small surface declared in psx_runtime.h:
// `call_by_address` (jump-register dispatch), `psx_override_dispatch`
// (the per-function intercept hook), and `psx_syscall`, supplied here on
// top of the generated dispatch table.

#include "openpete_fiber.h"
#include "openpete_fatal.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_pace_log.h"
#include "openpete_psxtrace.h"
#include "openpete_recomp_module.h"

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
 * each frame. Because the host/game fibers are cooperatively scheduled in
 * one OS thread (libco), an infinite loop in the game fiber blocks the
 * host fiber too - no host-side timer can fire. Watching `openpete_vbl`
 * here lets the game fiber notice it hasn't called VSync(0) in a long time
 * and dump where it's been before we lose the trace. (We key off vbl rather
 * than `openpete_frame` so fast-boot iterations - which advance vbl but not
 * frame - don't look like hangs.)
 *
 * On trip: write three rings of (addr, ra, sp) to `OPENPETE_HANG_TRACE`
 * (default hang-trace.bin in the logs state directory, beside the log
 * file) and guest RAM to `OPENPETE_HANG_RAM` (default hang-ram.bin there),
 * tell the player (openpete_fatal_message) and `_Exit(2)`. Format: u32
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
static int s_hang_tripped = 0;

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
 * @param trip_addr The guest function whose entry crossed the threshold. */
static void openpete_hang_dump_and_exit(uint32_t trip_addr) {
    s_hang_tripped = 1;
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
    /* stderr is a pipe drained by the log pump thread; _Exit would take the
     * watchdog's own diagnosis down with the process before it is copied. */
    openpete_log_file_sync();
    _Exit(2);
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

uint32_t openpete_loop_anchor_ticks(void) { return s_loop_anchor_ticks; }

/* Function entries since openpete_vbl last changed, for the spin tick. */
static uint32_t s_spin_calls_since_vbl = 0u;
static uint32_t s_spin_last_vbl = 0u;

/** @brief Pass one VBL to a guest spin that has entered 2048 functions without one.
 *
 * Counts function entries since `openpete_vbl` last changed; every 2048th
 * calls psyq_sched_spin_tick, whose per-game predicate decides whether the
 * spin is a disc wait a VBL is owed to. The entries are counted whether or
 * not the hang watchdog is armed.
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
     * the schedule or the drive model passes one here (psyq_thunks.cpp). */
    if ((++s_spin_calls_since_vbl & 2047u) != 2047u) return;
    extern void psyq_sched_spin_tick(CPUState* cpu, uint32_t addr) __attribute__((weak));
    openpete_psxtrace_gate_host_enter();
    if (psyq_sched_spin_tick) psyq_sched_spin_tick(cpu, addr);
    openpete_psxtrace_gate_host_leave();
}

int psx_override_dispatch(CPUState* cpu, uint32_t addr) {
    if (!s_hang_threshold_inited) {
        const char* env = getenv("OPENPETE_HANG_THRESHOLD");
        if (env && *env) s_hang_threshold = (uint32_t)strtoul(env, NULL, 0);
        s_hang_threshold_inited = 1;
    }
    const uint32_t vbl_now = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    if (s_hang_threshold && !s_hang_tripped) {
        if (vbl_now != s_hang_last_vbl) {
            /* Vbl boundary: snapshot the ring before the next vbl interval's
             * dispatches overwrite it. If a hang trips during the next
             * interval, the snapshot still shows what got us here. */
            memcpy(s_hang_ring_prev, s_hang_ring, sizeof(s_hang_ring));
            s_hang_head_prev = s_hang_head;
            s_hang_last_vbl = vbl_now;
            s_hang_calls_since_tick = 0u;
        } else if (++s_hang_calls_since_tick > s_hang_threshold) {
            openpete_hang_dump_and_exit(addr);
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
    spin_tick(cpu, vbl_now, addr);
    if (addr == s_loop_anchor_pc) { s_loop_anchor_ticks++; s_loop_insns_anchor = 0u; }
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

/* Quanta per loop header, the first LOOP_SITES distinct headers. */
#define LOOP_SITES 64u
static uint32_t s_loop_site_hdr[LOOP_SITES], s_loop_site_trips[LOOP_SITES];
static int32_t  s_loop_site_first_vbl[LOOP_SITES];
static uint32_t s_loop_sites = 0u;

/** @brief Count one quantum against its loop header in the per-site census.
 * @param header The loop's header address. */
static void loop_site_note(uint32_t header) {
    for (uint32_t i = 0; i < s_loop_sites; i++)
        if (s_loop_site_hdr[i] == header) { s_loop_site_trips[i]++; return; }
    if (s_loop_sites == LOOP_SITES) return;
    s_loop_site_hdr[s_loop_sites] = header;
    s_loop_site_trips[s_loop_sites] = 1u;
    s_loop_site_first_vbl[s_loop_sites] = SDL_GetAtomicInt(&openpete_vbl);
    s_loop_sites++;
}

void openpete_loop_site_census_log(void) {
    for (uint32_t i = 0; i < s_loop_sites; i++)
        OPL_INFO_F("delivery", "loop site 0x%08X: %u quanta, first at vbl %d",
                   s_loop_site_hdr[i], s_loop_site_trips[i], s_loop_site_first_vbl[i]);
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
    *enabled = s_loop_delivery != 0;
}

void psx_loop_poll(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t pure) {
    s_loop_polls++;
    s_loop_insns_anchor += insns;
    s_loop_acc += (uint64_t)insns * LOOP_SCALE;
    if (s_loop_acc > s_loop_acc_max) s_loop_acc_max = s_loop_acc;
    if (s_loop_acc < LOOP_QUANTUM) return;
    /* One VBL of instructions has run since the last VBL. */
    s_loop_trips++;
    s_loop_last_header = header;
    loop_site_note(header);
    s_loop_acc -= LOOP_QUANTUM;
    if (++s_loop_quanta > s_loop_quanta_max) s_loop_quanta_max = s_loop_quanta;
    if (s_loop_delivery < 0) {
        const char* env = getenv("OPENPETE_LOOP_DELIVERY");
        s_loop_delivery = (env && *env) ? (atoi(env) != 0) : 1;
    }
    if (!s_loop_delivery) return;
    extern void psyq_deliver_loop_vbl(CPUState* cpu, uint32_t header, uint32_t pure) __attribute__((weak));
    if (psyq_deliver_loop_vbl) psyq_deliver_loop_vbl(cpu, header, pure);
}
