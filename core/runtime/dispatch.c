// core/runtime/dispatch.c — psx_runtime.h glue not provided by generated C.
//
// The recompiled C calls into a small surface declared in psx_runtime.h:
// `call_by_address` (jump-register dispatch), `psx_override_dispatch`
// (per-function intercept hook — Phase 1c+), and `psx_syscall`. We
// supply them here on top of the generated dispatch table.

#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "openpete_pace_log.h"
#include "openpete_recomp_module.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Recomp-module registry -------------------------------------------
 * Core carries NO link-time reference to the recompiled code; the game
 * (static build) or the module loader (distribution D1) registers the
 * descriptor before the first dispatch. See openpete_recomp_module.h. */

static int recomp_unregistered_dispatch(CPUState* cpu, uint32_t addr) {
    (void)cpu;
    fprintf(stderr,
            "openpete: dispatch to 0x%08X before a recomp module was "
            "registered — game main must call openpete_recomp_register()\n",
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

/* Globals psxrecomp's gte.cpp expects. We don't run a frame loop in
 * Phase 1b, but the symbol must exist for link. */
uint32_t g_ps1_frame = 0;

/* The canonical-SX FIFO (openpete_canonical_sx{0,1,2}_q16) is defined in
 * core/features/widescreen.c; the recompiler-generated C reaches it via
 * the psx_runtime.h externs. */

/* Phase 1d: when the recompiled main EXE calls into an overlay we haven't
 * translated yet, stub-and-continue rather than abort. The Phase 2 manifest
 * + per-overlay translation is the proper fix; for now we want the main loop
 * to keep spinning so we can hit "≥10 iterations" / "DrawOTag fires".
 *
 * Rate-limited per-address logging: each unique miss-addr is logged on first
 * hit and every Nth subsequent hit so the log stays useful.  */
#define MISS_TABLE_SIZE 64
struct miss_entry { uint32_t addr; uint32_t hits; };
static struct miss_entry s_miss[MISS_TABLE_SIZE];
static int s_miss_overflow = 0;

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
                   "stub miss 0x%08X (#1) ra=0x%08X sp=0x%08X — likely overlay/untranslated",
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
    /* Phase 1d band-aid: when a stub call lands here from inside the main
     * loop (e.g. Spyro's TitlescreenUpdate/TitlescreenDraw, which are
     * overlay-resident and thus untranslated), the recompiled main never
     * reaches a VSync(0). Force a cooperative yield so the host fiber can
     * drive frame_limit/max-runtime checks. We use the no-tick variant so
     * the frame counter still tracks real VSync(0) calls; otherwise stubbed
     * loops would inflate it. Phase 2 (manifest + overlay translation)
     * retires this entirely. */
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
        fprintf(s_anchor_vbl_log, "%u %d\n", hit, SDL_GetAtomicInt(&openpete_vbl));
        fflush(s_anchor_vbl_log);
    }
}

/* Set while a presentation-only re-render is in flight (e.g. the
 * widescreen GamestateDraw bracket re-invokes func_8001ED5C with widened
 * cull for display). Each generated function body opens with
 * `if (psx_override_dispatch(cpu, ADDR)) return;`, so that re-invocation
 * re-enters this dispatch at the SAME anchor PC and would fire the oracle
 * anchor a second time per game frame — sampling the identical pre-draw
 * RAM and consuming two golden records for one real frame (a measurement
 * artifact, not a gameplay change). Suppress the anchor during the render
 * pass so exactly one record is consumed per game frame. */

/* Write a 528-byte 'CSv1' CPU dump matching the format produced by
 * scripts/pcsx-redux-ram-dump.lua (so a single diff helper can post-
 * process either side's dump). Layout:
 *   0    magic 0x31765343 'CSv1'
 *   4    32 GPRs (zero, at, v0..ra; MIPS register-number order)
 * 132    pc / hi / lo
 * 144    32 cop0 regs
 * 272    32 GTE data regs (cop2)
 * 400    32 GTE control regs (cop2c) */
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
    /* Bypass the host-fiber teardown — we just want a clean exit with the
     * dump on disk. The host loop's normal exit path requires yielding back
     * through SDL/PsyCross which can hang under headless GL on WSL. */
    _Exit(0);
}

/* Hang watchdog: psx_override_dispatch fires at every translated-function
 * entry, so it sees roughly the full call sequence the game fiber walks
 * each frame. Because the host/game fibers are cooperatively scheduled in
 * one OS thread (libco), an infinite loop in the game fiber blocks the
 * host fiber too — no host-side timer can fire. Watching `openpete_vbl`
 * here lets the game fiber notice it hasn't called VSync(0) in a long time
 * and dump where it's been before we lose the trace. (We key off vbl rather
 * than `openpete_frame` so fast-boot iterations — which advance vbl but not
 * frame — don't look like hangs.)
 *
 * On trip: write three rings of (addr, ra, sp) to `OPENPETE_HANG_TRACE`
 * (default ./hang-trace.bin) and `_Exit(2)`. Format: u32 magic 'HANG2', then
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
 * loop began — the snapshot preserves it. */
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

static void openpete_hang_dump_and_exit(uint32_t trip_addr) {
    s_hang_tripped = 1;
    const char* path = getenv("OPENPETE_HANG_TRACE");
    if (!path || !*path) path = "./hang-trace.bin";
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
    if (!rampath || !*rampath) rampath = "./hang-ram.bin";
    FILE* rp = fopen(rampath, "wb");
    if (rp) {
        const size_t ram_size = 2u * 1024u * 1024u;
        fwrite(openpete_psx_get_ram(), 1, ram_size, rp);
        fclose(rp);
    }
    OPL_ERROR_F("watchdog",
                "hang detected: %u dispatch entries since openpete_vbl=%u "
                "(trip addr=0x%08X), wrote trace=%s ram=%s — exiting",
                s_hang_calls_since_tick, s_hang_last_vbl, trip_addr,
                path, rampath);
    fflush(stdout); fflush(stderr);
    /* stderr is a pipe drained by the log pump thread; _Exit would take the
     * watchdog's own diagnosis down with the process before it is copied. */
    openpete_log_file_sync();
    _Exit(2);
}

/* Per-function intercept gate emitted at the top of every generated body.
 * Phase 1b: never intercepts. Phase 1c+ wires per-game quirks here. */
__attribute__((weak)) int psx_override_dispatch_game(CPUState* cpu, uint32_t addr);
int psx_override_dispatch(CPUState* cpu, uint32_t addr) {
    if (!s_hang_threshold_inited) {
        const char* env = getenv("OPENPETE_HANG_THRESHOLD");
        if (env && *env) s_hang_threshold = (uint32_t)strtoul(env, NULL, 0);
        s_hang_threshold_inited = 1;
    }
    if (s_hang_threshold && !s_hang_tripped) {
        uint32_t vbl_now = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
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
    /* Pad-read probe: the guest function that consumes the pad buffer. Sited
     * here rather than in the game's native-dispatch switch because every
     * translated function entry passes through this call in BOTH build modes,
     * so the probe reads the same moment whether the tick ran the recompiled
     * function or its decompiled twin. */
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
        anchor_vbl_log(openpete_psxtrace_anchor_hits);
        if (openpete_psxtrace_dispatch_hook &&
            openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target) {
            openpete_psxtrace_dispatch_hook(addr);
        }
    }
    if (psx_override_dispatch_game) return psx_override_dispatch_game(cpu, addr);
    (void)cpu; (void)addr;
    return 0;
}

void psx_syscall(CPUState* cpu, uint32_t code) {
    OPL_WARN_F("dispatch", "syscall code=0x%X (ra=0x%08X) — ignored",
               code, cpu ? cpu->ra : 0u);
}

/* MMIO trace toggle — referenced by generated C indirectly via the runtime
 * header. No-op in the slim runner; real MMIO tracing belongs in PsyCross. */
void psx_mmio_trace_enable(int enable) { (void)enable; }

void diag_track_gp1(uint32_t cmd) { (void)cmd; }

/* runtime_init/load aliases that match psxrecomp's psx_runtime.h spelling. */
void psx_runtime_init(CPUState* cpu) { openpete_psx_runtime_init(cpu); }
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size) {
    openpete_psx_load(addr, data, size);
}
uint8_t* psx_get_ram(void)     { return openpete_psx_get_ram(); }
uint8_t* psx_get_scratch(void) { return openpete_psx_get_scratch(); }

void psx_install_crash_handler(void) {
    openpete_psx_install_crash_handler();
}
