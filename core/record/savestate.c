// core/record/savestate.c — save/load of openpete game state to slot files.
//
// v3 file layout (little-endian throughout, no padding inside the header
// thanks to fixed-size fields):
//
//   uint32_t  magic         = 'OSv1' (0x3176534F LE)
//   uint32_t  version       = 3
//   char      ident[32]     — content identity, see ident_fill()
//   uint32_t  frame         — openpete_frame (VBL counter) at save
//   uint32_t  cpu_bytes
//   uint32_t  ram_bytes
//   uint32_t  scratch_bytes
//   uint32_t  vram_bytes    (1 MiB — VRAM_WIDTH * VRAM_HEIGHT * 2)
//   uint32_t  vbl           — openpete_vbl (guest-visible VSync counter);
//                             restored so post-load wait loops don't see a
//                             wild delta (a one-VBL timeline skew)
//   uint32_t  spu_state_bytes  — PsyX SPU voice mirror + cursors
//   uint32_t  spu_mem_bytes    — SPU sample RAM
//   uint32_t  cd_state_bytes   — PsyCross CD head-position globals
//   uint32_t  game_extra_bytes — opaque per-game host blob (game_extras)
//   uint32_t  modset_tag_bytes — enabled-modset tag string (no NUL)
//   uint32_t  arena_bytes      — low-park arena ledger
//   uint32_t  reserved[3]   = 0 (DRAWENV/DISPENV later)
//   uint8_t   cpu_regs[cpu_bytes]
//   uint8_t   ram[ram_bytes]
//   uint8_t   scratch[scratch_bytes]
//   uint8_t   vram[vram_bytes]
//   uint8_t   spu_state[spu_state_bytes]
//   uint8_t   spu_mem[spu_mem_bytes]
//   uint8_t   cd_state[cd_state_bytes]
//   uint8_t   game_extra[game_extra_bytes]
//   uint8_t   arena[arena_bytes]
//   char      modset_tag[modset_tag_bytes]
//
// Note on the libco game fiber: its host call stack is NOT snapshotted.
// On resume the fiber unwinds through whatever frames were live at load
// time, but recompiled C reads cpu->fields fresh on every access, so
// after at most 1-2 outer-loop iterations execution stabilises against
// the loaded cpu+RAM. An attempt to memcpy the fiber memory was reverted
// because libco's Windows backend uses an opaque OS-fiber handle that
// can't be serialised, and the cross-platform split wasn't worth the
// small fidelity gain.
//
// The CPU portion is exactly the prefix of CPUState that comes before its
// memory-callback function pointers — those point at host code and must
// not survive a save/load roundtrip.
//
// Identity gating (v3): ident = sha256("stateabi:<N>|cg:<codegen_stamp>|
// rom:<rom_sha256>") hex-truncated to 31 chars. This covers what actually
// varies between installations — the recomp module (boot-compiled per
// machine under the distribution model) and the ROM — and pins the state
// LAYOUT via the curated OPENPETE_STATE_ABI number (bump it whenever any
// section layout changes). Unlike the old __DATE__/__TIME__ hash, dev
// rebuilds with unchanged layout keep their slots (the section-size checks
// below still catch gross drift). Residual dev-only risk: a layout-only
// change under an unbumped ABI on a "dev-static" codegen stamp — accepted,
// same curation contract as OPENPETE_RECOMP_ABI_VERSION.

#include "openpete_state.h"
#include "openpete_discontinuity.h"
#include "openpete_fiber.h"  /* openpete_vbl — saved/restored via hdr.vbl */
#include "openpete_log.h"
#include "openpete_memcard.h" /* openpete_psx_bios_seed_ram */
#include "openpete_psxtrace.h" /* openpete_sched_active */
#include "openpete_mods.h"   /* per-mod state participation + modset tag */
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"  /* codegen_stamp / rom_sha256 identity */
#include "game_extras.h"
#include "sha256.h"

#include "PsyX/PsyX_public.h"

/* PsyX SPU state-export (PsyX_SPUAL.h is a PsyCross-internal header; the
 * thunks extern these the same way). */
extern int  PsyX_CD_QueueStateSize(void);
extern void PsyX_CD_QueueSnapshot(void* dst);
extern void PsyX_CD_QueueRestore(const void* src);
extern unsigned int PsyX_SPUAL_StateSize(void);
extern int PsyX_SPUAL_SnapshotState(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreState(const void* src, unsigned int size);
extern int PsyX_SPUAL_RestoreStateEx(const void* src, unsigned int size,
                                     int quiet_al);
extern unsigned int PsyX_SPUAL_MemSize(void);
extern int PsyX_SPUAL_SnapshotMem(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreMem(const void* src, unsigned int size);

/* PsyCross CD head-position globals (LIBCD.C; plain global-scope names,
 * identical symbols under C and C++ linkage). Saved so a state taken
 * between a guest CdlSetloc and its CdRead doesn't resume reading from
 * wherever the OTHER timeline left the head. The command QUEUE
 * (g_cdComQueue) is NOT saved: it holds callback pointers and is drained
 * synchronously in this port; a queued-command-in-flight save would need
 * a quiescence audit first. */
extern int g_cdCurrentSector;
extern int g_cdSectorSize;
extern int g_cdCurrentTrack;
extern int g_cdNumFrames;
extern int g_CD_com;
extern int g_cdReadDoneFlag;
extern int g_cdXaStartLBA;
extern int g_cdXaStartVBL;

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
  #include <unistd.h>
#endif

#define OPENPETE_STATE_MAGIC    0x3176534Fu  /* 'OSv1' little-endian */
#define OPENPETE_STATE_VERSION  3u
#define OPENPETE_STATE_HASH_LEN 32

/* Curated state-layout ABI. Bump on ANY change to a section's layout
 * (header fields, SPUALVoiceState packing, cd_state_t, mods-section
 * framing, arena ledger, ...). Part of the ident hash, so a bump cleanly
 * orphans older files. 4 = arena section, MSv2 mods framing.
 * 5 = virtual XA music-head anchor joined the CD section. */
/* 6 = arena section v2: high-arena ledger + the arena's used bytes.
 * 7 = arena section v3: one ledger with allocation + stream-handle rows;
 *     the low-park ledger is gone with the low allocator.
 * 8 = SPUALVoiceState carries the voice head (consumed_dec_fp). */
#define OPENPETE_STATE_ABI 8u

/* Just the register file — the function-pointer tail of CPUState is
 * runtime-only, must not be saved or restored. */
#define CPU_REG_BYTES ((uint32_t)offsetof(CPUState, read_word))

typedef struct {
    uint32_t magic;
    uint32_t version;
    char     ident[OPENPETE_STATE_HASH_LEN];
    uint32_t frame;
    uint32_t cpu_bytes;
    uint32_t ram_bytes;
    uint32_t scratch_bytes;
    uint32_t vram_bytes;
    uint32_t vbl;
    uint32_t spu_state_bytes;
    uint32_t spu_mem_bytes;
    uint32_t cd_state_bytes;
    uint32_t game_extra_bytes;
    uint32_t modset_tag_bytes;
    uint32_t arena_bytes;      /* low-park arena ledger */
    uint32_t reserved[3];
} openpete_state_header_t;

/* CD head-position section (fixed layout, ABI-covered). */
typedef struct {
    int32_t cur_sector;
    int32_t sector_size;
    int32_t cur_track;
    int32_t num_frames;
    int32_t cd_com;
    int32_t read_done;
    int32_t xa_start_lba;   /* virtual music-head anchor (LIBCD.C) — the */
    int32_t xa_start_vbl;   /* GetlocP reply is a pure function of these */
} state_cd_t;

/* Chunked fwrite/fread. Some Windows stdio configurations short-write
 * multi-megabyte blobs in a single fwrite call (observed on the 8 MiB
 * fiber memory); chunking keeps each underlying _write under the
 * comfortable range and gives us a precise errno when a chunk fails. */
#define IO_CHUNK (64u * 1024u)

static int write_section(FILE* fp, const char* path, const char* what,
                         const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = n;
    while (left > 0) {
        size_t want = left > IO_CHUNK ? IO_CHUNK : left;
        size_t got  = fwrite(p, 1, want, fp);
        if (got != want) {
            OPL_ERROR_F("savestate",
                        "%s: short write in %s (off=%zu want=%zu got=%zu errno=%d %s)",
                        path, what, n - left, want, got, errno, strerror(errno));
            return -1;
        }
        p += got; left -= got;
    }
    return 0;
}

static int read_section(FILE* fp, const char* path, const char* what,
                        void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t left = n;
    while (left > 0) {
        size_t want = left > IO_CHUNK ? IO_CHUNK : left;
        size_t got  = fread(p, 1, want, fp);
        if (got != want) {
            OPL_ERROR_F("savestate",
                        "%s: short read in %s (off=%zu want=%zu got=%zu errno=%d %s)",
                        path, what, n - left, want, got, errno, strerror(errno));
            return -1;
        }
        p += got; left -= got;
    }
    return 0;
}

static void ident_fill(char out[OPENPETE_STATE_HASH_LEN]) {
    const openpete_recomp_module* m = openpete_recomp_module_get();
    char src[512];
    int n = snprintf(src, sizeof(src), "stateabi:%u|cg:%s|rom:%s",
                     OPENPETE_STATE_ABI,
                     (m && m->codegen_stamp) ? m->codegen_stamp : "?",
                     (m && m->rom_sha256)    ? m->rom_sha256    : "?");
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(src)) n = (int)sizeof(src);

    uint8_t digest[32];
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, src, (size_t)n);
    openpete_sha256_final(&c, digest);

    memset(out, 0, OPENPETE_STATE_HASH_LEN);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < (OPENPETE_STATE_HASH_LEN - 1) / 2; ++i) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    /* 15 bytes of digest as 30 hex chars + NUL padding to 32. */
}

/* Resolve the states directory, creating the intermediate directories as
 * needed. A sibling of memcard.c's `cards` under whichever layout
 * openpete_paths.h resolved — the game folder for a portable install, the
 * per-user state directory otherwise. Writes the directory into `out` with
 * no trailing separator. Returns 0 on success. */
static int resolve_state_dir(char* out, size_t out_size) {
    return openpete_paths_state_dir("states", out, out_size, 1);
}

static int slot_path(unsigned slot, char* out, size_t out_size) {
    if (slot >= OPENPETE_STATE_NUM_SLOTS) {
        OPL_ERROR_F("savestate", "slot %u out of range (0..%u)",
                    slot, OPENPETE_STATE_NUM_SLOTS - 1);
        return -1;
    }
    char dir[1024];
    if (resolve_state_dir(dir, sizeof(dir)) != 0) return -1;
#ifdef _WIN32
    int n = snprintf(out, out_size, "%s\\slot%u.state", dir, slot);
#else
    int n = snprintf(out, out_size, "%s/slot%u.state", dir, slot);
#endif
    if (n < 0 || (size_t)n >= out_size) {
        OPL_ERROR_F("savestate", "slot path too long");
        return -1;
    }
    return 0;
}

int openpete_state_slot_present(unsigned slot) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return 0;
    struct stat st;
    return (stat(path, &st) == 0 && (st.st_mode & S_IFREG)) ? 1 : 0;
}

int openpete_state_save_file(const char* path, const CPUState* cpu,
                             unsigned frame) {
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        OPL_ERROR_F("savestate", "fopen %s for write failed: %s",
                    path, strerror(errno));
        return -1;
    }

    const char* modset_tag = openpete_mods_modset_tag();
    if (!modset_tag) modset_tag = "";

    openpete_state_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = OPENPETE_STATE_MAGIC;
    hdr.version       = OPENPETE_STATE_VERSION;
    ident_fill(hdr.ident);
    hdr.frame         = (uint32_t)frame;
    hdr.cpu_bytes     = CPU_REG_BYTES;
    hdr.ram_bytes     = OPENPETE_PSX_RAM_SIZE;
    hdr.scratch_bytes = OPENPETE_PSX_SCRATCH_SIZE;
    hdr.vram_bytes    = PsyX_VRAM_GetSize();
    /* Guest-visible VBL counter (VSync(-1) reads it; Spyro's wait loops
     * compare it against counters stored in the SAVED guest RAM). Without
     * restoring it, the first post-load wait loop sees a wild delta and
     * exits early — a one-VBL timeline skew (measured: post-load hash
     * timeline leads the post-save one by exactly one row). */
    hdr.vbl              = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    hdr.spu_state_bytes  = PsyX_SPUAL_StateSize();
    hdr.spu_mem_bytes    = PsyX_SPUAL_MemSize();
    hdr.cd_state_bytes   = (uint32_t)sizeof(state_cd_t);
    hdr.game_extra_bytes = game_state_extra_size();
    hdr.modset_tag_bytes = (uint32_t)strlen(modset_tag);
    /* Immutable-row promises are checked once per file save (and once
     * per rewind keyframe), never per speculative tick. */
    openpete_mods_arena_immutable_check("file save");
    hdr.arena_bytes      = openpete_mods_arena_state_size();

    /* Snapshot the host-side mirrors into heap buffers before writing —
     * keeps them stable across the (potentially slow) disk I/O even
     * though the game fiber is yielded right now. */
    void* vram_snap  = malloc(hdr.vram_bytes);
    void* spu_snap   = malloc(hdr.spu_state_bytes);
    void* spumem_snap= malloc(hdr.spu_mem_bytes);
    void* extra_snap = hdr.game_extra_bytes ? malloc(hdr.game_extra_bytes) : NULL;
    void* arena_snap = hdr.arena_bytes ? malloc(hdr.arena_bytes) : NULL;
    if (!vram_snap || !spu_snap || !spumem_snap ||
        (hdr.game_extra_bytes && !extra_snap) ||
        (hdr.arena_bytes && !arena_snap) ||
        PsyX_VRAM_Snapshot(vram_snap, hdr.vram_bytes) != 0 ||
        PsyX_SPUAL_SnapshotState(spu_snap, hdr.spu_state_bytes) != 0 ||
        PsyX_SPUAL_SnapshotMem(spumem_snap, hdr.spu_mem_bytes) != 0) {
        free(vram_snap); free(spu_snap); free(spumem_snap); free(extra_snap);
        free(arena_snap);
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: host-mirror snapshot failed", path);
        return -1;
    }
    if (hdr.game_extra_bytes) game_state_extra_save(extra_snap);
    if (hdr.arena_bytes)      openpete_mods_arena_state_save(arena_snap);

    state_cd_t cd;
    cd.cur_sector   = g_cdCurrentSector;
    cd.sector_size  = g_cdSectorSize;
    cd.cur_track    = g_cdCurrentTrack;
    cd.num_frames   = g_cdNumFrames;
    cd.cd_com       = g_CD_com;
    cd.read_done    = g_cdReadDoneFlag;
    cd.xa_start_lba = g_cdXaStartLBA;
    cd.xa_start_vbl = g_cdXaStartVBL;

    int rc = 0;
    if (rc == 0) rc = write_section(fp, path, "header",  &hdr, sizeof(hdr));
    if (rc == 0) rc = write_section(fp, path, "cpu",     cpu, CPU_REG_BYTES);
    if (rc == 0) rc = write_section(fp, path, "ram",     openpete_psx_get_ram(),
                                    OPENPETE_PSX_RAM_SIZE);
    if (rc == 0) rc = write_section(fp, path, "scratch", openpete_psx_get_scratch(),
                                    OPENPETE_PSX_SCRATCH_SIZE);
    if (rc == 0) rc = write_section(fp, path, "vram",    vram_snap, hdr.vram_bytes);
    if (rc == 0) rc = write_section(fp, path, "spu",     spu_snap, hdr.spu_state_bytes);
    if (rc == 0) rc = write_section(fp, path, "spumem",  spumem_snap, hdr.spu_mem_bytes);
    if (rc == 0) rc = write_section(fp, path, "cd",      &cd, sizeof(cd));
    if (rc == 0 && hdr.game_extra_bytes)
        rc = write_section(fp, path, "game-extra", extra_snap, hdr.game_extra_bytes);
    if (rc == 0 && hdr.arena_bytes)
        rc = write_section(fp, path, "arena", arena_snap, hdr.arena_bytes);
    if (rc == 0 && hdr.modset_tag_bytes)
        rc = write_section(fp, path, "modset-tag", modset_tag, hdr.modset_tag_bytes);

    free(vram_snap); free(spu_snap); free(spumem_snap); free(extra_snap);
    free(arena_snap);
    fclose(fp);
    if (rc != 0) return -1;
    OPL_INFO_F("savestate", "saved %s (frame=%u)", path, frame);
    return 0;
}

int openpete_state_save(unsigned slot, const CPUState* cpu, unsigned frame) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;
    return openpete_state_save_file(path, cpu, frame);
}

/* Under the reference's schedule (a --psxtrace-verify run with sidecars)
 * the thunk layer's position in the schedule's rows (VSync, read, seed,
 * CdSync, controller and interrupt cursors, the reference VBLANK reached)
 * is host state no state file carries; a load would replay from a RAM
 * image the schedule has moved past and desync every row after it. */
static int sched_refuses_load(const char* what) {
    if (!openpete_sched_active()) return 0;
    OPL_ERROR_F("savestate", "%s refused: the run is under the reference's "
                "schedule, whose position no state carries", what);
    return 1;
}

int openpete_state_load_file(const char* path, CPUState* cpu,
                             unsigned* out_frame) {
    if (sched_refuses_load(path)) return -1;
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        OPL_ERROR_F("savestate", "fopen %s for read failed: %s",
                    path, strerror(errno));
        return -1;
    }

    openpete_state_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: short read on header", path);
        return -1;
    }

    if (hdr.magic != OPENPETE_STATE_MAGIC) {
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: bad magic 0x%08X (expected 0x%08X)",
                    path, hdr.magic, OPENPETE_STATE_MAGIC);
        return -1;
    }
    if (hdr.version != OPENPETE_STATE_VERSION) {
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: version %u not supported (expected %u)",
                    path, hdr.version, OPENPETE_STATE_VERSION);
        return -1;
    }

    char want_ident[OPENPETE_STATE_HASH_LEN];
    ident_fill(want_ident);
    if (memcmp(hdr.ident, want_ident, OPENPETE_STATE_HASH_LEN) != 0) {
        fclose(fp);
        char hh[OPENPETE_STATE_HASH_LEN + 1];
        char wh[OPENPETE_STATE_HASH_LEN + 1];
        memcpy(hh, hdr.ident,  OPENPETE_STATE_HASH_LEN); hh[OPENPETE_STATE_HASH_LEN] = '\0';
        memcpy(wh, want_ident, OPENPETE_STATE_HASH_LEN); wh[OPENPETE_STATE_HASH_LEN] = '\0';
        OPL_ERROR_F("savestate",
                    "%s: identity mismatch (file=%s, this build=%s) — "
                    "state was made against a different ROM / recomp module "
                    "/ state ABI",
                    path, hh, wh);
        return -1;
    }

    if (hdr.cpu_bytes != CPU_REG_BYTES ||
        hdr.ram_bytes != OPENPETE_PSX_RAM_SIZE ||
        hdr.scratch_bytes != OPENPETE_PSX_SCRATCH_SIZE ||
        hdr.vram_bytes != PsyX_VRAM_GetSize() ||
        hdr.spu_state_bytes != PsyX_SPUAL_StateSize() ||
        hdr.spu_mem_bytes != PsyX_SPUAL_MemSize() ||
        hdr.cd_state_bytes != (uint32_t)sizeof(state_cd_t)) {
        fclose(fp);
        OPL_ERROR_F("savestate",
                    "%s: section sizes mismatch (cpu=%u/%u ram=%u/%u "
                    "scratch=%u/%u vram=%u/%u spu=%u/%u spumem=%u/%u cd=%u/%u)",
                    path,
                    hdr.cpu_bytes, CPU_REG_BYTES,
                    hdr.ram_bytes, OPENPETE_PSX_RAM_SIZE,
                    hdr.scratch_bytes, OPENPETE_PSX_SCRATCH_SIZE,
                    hdr.vram_bytes, PsyX_VRAM_GetSize(),
                    hdr.spu_state_bytes, PsyX_SPUAL_StateSize(),
                    hdr.spu_mem_bytes, PsyX_SPUAL_MemSize(),
                    hdr.cd_state_bytes, (uint32_t)sizeof(state_cd_t));
        return -1;
    }

    /* Read everything into temp buffers first so a short read doesn't
     * leave RAM half-overwritten and the game fiber wedged. The fiber
     * buffer in particular MUST be atomic — a partial fiber-memory write
     * corrupts saved sp + register slots and the next co_switch will
     * crash. */
    /* Read into temp buffers first so a short read doesn't leave any
     * region half-overwritten and the game fiber wedged. */
    uint8_t  cpu_buf[CPU_REG_BYTES];
    uint8_t* ram_buf    = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
    uint8_t* vram_buf   = (uint8_t*)malloc(hdr.vram_bytes);
    uint8_t* spu_buf    = (uint8_t*)malloc(hdr.spu_state_bytes);
    uint8_t* spumem_buf = (uint8_t*)malloc(hdr.spu_mem_bytes);
    uint8_t* extra_buf  = hdr.game_extra_bytes ? (uint8_t*)malloc(hdr.game_extra_bytes) : NULL;
    uint8_t* arena_buf  = hdr.arena_bytes ? (uint8_t*)malloc(hdr.arena_bytes) : NULL;
    char*    tag_buf    = (char*)malloc((size_t)hdr.modset_tag_bytes + 1);
    uint8_t  scratch_buf[OPENPETE_PSX_SCRATCH_SIZE];
    state_cd_t cd;

    if (!ram_buf || !vram_buf || !spu_buf || !spumem_buf || !tag_buf ||
        (hdr.game_extra_bytes && !extra_buf) ||
        (hdr.arena_bytes && !arena_buf)) {
        free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf);
        free(extra_buf); free(arena_buf); free(tag_buf);
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: out of memory", path);
        return -1;
    }

    int rc = 0;
    if (rc == 0) rc = read_section(fp, path, "cpu",     cpu_buf,     CPU_REG_BYTES);
    if (rc == 0) rc = read_section(fp, path, "ram",     ram_buf,     OPENPETE_PSX_RAM_SIZE);
    if (rc == 0) rc = read_section(fp, path, "scratch", scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    if (rc == 0) rc = read_section(fp, path, "vram",    vram_buf,    hdr.vram_bytes);
    if (rc == 0) rc = read_section(fp, path, "spu",     spu_buf,     hdr.spu_state_bytes);
    if (rc == 0) rc = read_section(fp, path, "spumem",  spumem_buf,  hdr.spu_mem_bytes);
    if (rc == 0) rc = read_section(fp, path, "cd",      &cd,         sizeof(cd));
    if (rc == 0 && hdr.game_extra_bytes)
        rc = read_section(fp, path, "game-extra", extra_buf, hdr.game_extra_bytes);
    if (rc == 0 && hdr.arena_bytes)
        rc = read_section(fp, path, "arena", arena_buf, hdr.arena_bytes);
    if (rc == 0 && hdr.modset_tag_bytes)
        rc = read_section(fp, path, "modset-tag", tag_buf, hdr.modset_tag_bytes);
    tag_buf[hdr.modset_tag_bytes] = '\0';
    fclose(fp);

    /* PRE-COMMIT verdict: the arena ledger must pass BEFORE any guest
     * byte is touched — a refused load leaves the timeline untouched. */
    if (rc == 0 && openpete_mods_arena_state_verdict(arena_buf,
                                                     hdr.arena_bytes) != 0)
        rc = -1;

    if (rc != 0) {
        free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf);
        free(extra_buf); free(arena_buf); free(tag_buf);
        OPL_ERROR_F("savestate", "%s: load refused — running state untouched",
                    path);
        return -1;
    }

    /* Modset check, pre-commit: guest RAM and the arena carry mod-parked
     * payloads and pointers into them, so a file saved under a DIFFERENT
     * mutative modset restores a world the running mods did not build.
     * Refuse rather than proceed; the ledger verdict above covers the
     * layout, this covers the set. */
    {
        const char* now_tag = openpete_mods_modset_tag();
        if (!now_tag) now_tag = "";
        if (strcmp(now_tag, tag_buf) != 0) {
            OPL_ERROR_F("savestate",
                        "%s: MODSET MISMATCH — state saved under [%s], "
                        "running [%s]; load refused — running state "
                        "untouched. Enable the same mods (and versions) "
                        "the state was saved with.",
                        path, tag_buf, now_tag);
            free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf);
            free(extra_buf); free(arena_buf); free(tag_buf);
            return -1;
        }
    }

    /* Commit. Only the register file is overwritten on cpu — the
     * function-pointer tail (read_word/write_word/...) keeps its host
     * bindings from openpete_psx_runtime_init. */
    memcpy(cpu,                        cpu_buf,     CPU_REG_BYTES);
    memcpy(openpete_psx_get_ram(),     ram_buf,     OPENPETE_PSX_RAM_SIZE);
    /* The kernel's fixed low-RAM words are constant over a run and absent
     * from a state written before the boot seeded them; re-seeding is
     * idempotent on a state that carries them. */
    openpete_psx_bios_seed_ram(openpete_psx_get_ram());
    memcpy(openpete_psx_get_scratch(), scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    (void)PsyX_VRAM_Restore(vram_buf, hdr.vram_bytes);
    (void)PsyX_SPUAL_RestoreMem(spumem_buf, hdr.spu_mem_bytes);
    (void)PsyX_SPUAL_RestoreState(spu_buf, hdr.spu_state_bytes);
    g_cdCurrentSector = cd.cur_sector;
    g_cdSectorSize    = cd.sector_size;
    g_cdCurrentTrack  = cd.cur_track;
    g_cdNumFrames     = cd.num_frames;
    g_CD_com          = cd.cd_com;
    g_cdReadDoneFlag  = cd.read_done;
    g_cdXaStartLBA    = cd.xa_start_lba;
    g_cdXaStartVBL    = cd.xa_start_vbl;
    free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf); free(tag_buf);

    /* Re-anchor the guest-visible VBL counter to the saved value (see the
     * save-side comment): without this the first post-load wait loop sees
     * a wild delta and the timeline leads by one VBL. */
    SDL_SetAtomicInt(&openpete_vbl, (int)hdr.vbl);

    /* Per-game post-load resync — restores any host-side state that
     * mirrors RAM (e.g. Spyro's active-overlay dispatcher pointer). */
    game_on_state_loaded(cpu);

    /* Game-side extra host state (e.g. Spyro's WAD-overlay append cursor).
     * The impl validates its own version/size and ignores foreign blobs. */
    if (hdr.game_extra_bytes)
        game_state_extra_load(extra_buf, hdr.game_extra_bytes);
    free(extra_buf);

    /* Arena apply (verdict passed pre-commit): restore the arena bytes
     * and re-anchor both cursors before anything downstream — a mod's
     * next tick hook may re-park against the restored layout. */
    openpete_mods_arena_state_apply(arena_buf, hdr.arena_bytes);
    free(arena_buf);

    /* Broadcast the discontinuity LAST, once engine + game + mod state is
     * fully committed: host caches derived from guest RAM (interp prev/curr
     * tables, texture-pack registry, bk2 recording, ...) resync here so
     * nothing carries a pre-load snapshot across the jump. */
    openpete_guest_discontinuity_notify();

    if (out_frame) *out_frame = hdr.frame;
    OPL_INFO_F("savestate", "loaded %s (frame=%u)", path, hdr.frame);
    return 0;
}

int openpete_state_load(unsigned slot, CPUState* cpu,
                        unsigned* out_frame) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;
    return openpete_state_load_file(path, cpu, out_frame);
}

int openpete_state_resolve_dir(char* out, unsigned long out_size) {
    return resolve_state_dir(out, (size_t)out_size);
}

/* ── In-memory snapshot (see openpete_state.h for the contract) ──────── */

struct openpete_state_mem {
    int        valid;
    unsigned   frame;
    uint32_t   vbl;
    uint8_t    cpu_buf[CPU_REG_BYTES];
    uint8_t    scratch_buf[OPENPETE_PSX_SCRATCH_SIZE];
    uint8_t*   ram;        /* 2 MiB */
    uint8_t*   spu;        /* PsyX_SPUAL_StateSize() */
    uint32_t   spu_bytes;
    state_cd_t cd;
    /* CD transport (command queue + drain-mode flags) and the game's
     * extra host-state blob. A speculative/rewound tick can run
     * load-transition code — CD commands enqueued, the WAD cursor moved,
     * the overlay dispatcher flipped — and restoring guest RAM without
     * these leaves the host machinery on the abandoned timeline: phantom
     * queued commands detonate the next CdSync drain, and a flipped
     * dispatcher routes overlay calls into the wrong level's functions.
     * cdq holds host pointers → in-memory snapshots only, never disk. */
    uint8_t*   cdq;        /* PsyX_CD_QueueStateSize() */
    uint32_t   cdq_bytes;
    uint8_t*   gx;         /* game_state_extra_size() (may be 0) */
    uint32_t   gx_bytes;
    /* VRAM: a speculative tick that crosses into a transition uploads
     * fade/loading graphics (LoadImage) into VRAM, and the committed
     * entry then reads VRAM back into gameplay RAM (the framebuffer-
     * capture StoreImage the crossfade uses) — so VRAM is gameplay-
     * READABLE state at transitions and must roll back with guest RAM.
     * Restored via the row-delta path: a tick whose speculation never
     * touched VRAM costs one compare, no GPU traffic. */
    uint8_t*   vram;       /* PsyX_VRAM_GetSize() */
    uint32_t   vram_bytes;
    /* Mod arena: the used prefix [0, watermark). The watermark only
     * grows, so the buffer is realloc'd grow-only at save time (the
     * ARENA itself is never realloc'd — only this snapshot copy). */
    uint8_t*   arena;
    uint32_t   arena_cap;
    uint32_t   arena_used;
};

openpete_state_mem_t* openpete_state_mem_new(void) {
    openpete_state_mem_t* m =
        (openpete_state_mem_t*)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->spu_bytes  = PsyX_SPUAL_StateSize();
    m->cdq_bytes  = (uint32_t)PsyX_CD_QueueStateSize();
    m->gx_bytes   = game_state_extra_size();
    m->vram_bytes = PsyX_VRAM_GetSize();
    m->ram  = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
    m->spu  = (uint8_t*)malloc(m->spu_bytes);
    m->cdq  = (uint8_t*)malloc(m->cdq_bytes);
    m->gx   = m->gx_bytes ? (uint8_t*)malloc(m->gx_bytes) : NULL;
    m->vram = (uint8_t*)malloc(m->vram_bytes);
    if (!m->ram || !m->spu || !m->cdq || (m->gx_bytes && !m->gx) || !m->vram) {
        openpete_state_mem_free(m);
        return NULL;
    }
    return m;
}

void openpete_state_mem_free(openpete_state_mem_t* m) {
    if (!m) return;
    free(m->ram);
    free(m->spu);
    free(m->cdq);
    free(m->gx);
    free(m->vram);
    free(m->arena);
    free(m);
}

int openpete_state_mem_save(openpete_state_mem_t* m, const CPUState* cpu,
                            unsigned frame) {
    if (!m) return -1;
    m->frame = frame;
    m->vbl   = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    memcpy(m->cpu_buf,     cpu,                        CPU_REG_BYTES);
    memcpy(m->ram,         openpete_psx_get_ram(),     OPENPETE_PSX_RAM_SIZE);
    memcpy(m->scratch_buf, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
    if (PsyX_SPUAL_SnapshotState(m->spu, m->spu_bytes) != 0) return -1;
    m->cd.cur_sector   = g_cdCurrentSector;
    m->cd.sector_size  = g_cdSectorSize;
    m->cd.cur_track    = g_cdCurrentTrack;
    m->cd.num_frames   = g_cdNumFrames;
    m->cd.cd_com       = g_CD_com;
    m->cd.read_done    = g_cdReadDoneFlag;
    m->cd.xa_start_lba = g_cdXaStartLBA;
    m->cd.xa_start_vbl = g_cdXaStartVBL;
    PsyX_CD_QueueSnapshot(m->cdq);
    if (m->gx_bytes) game_state_extra_save(m->gx);
    if (PsyX_VRAM_Snapshot(m->vram, m->vram_bytes) != 0) return -1;
    {
        uint32_t used = openpete_mods_arena_used();
        if (used > m->arena_cap) {
            uint8_t* grown = (uint8_t*)realloc(m->arena, used);
            if (!grown) return -1;
            m->arena = grown;
            m->arena_cap = used;
        }
        if (used) memcpy(m->arena, openpete_mods_arena_bytes(), used);
        m->arena_used = used;
    }
    m->valid = 1;
    return 0;
}

int openpete_state_mem_load(const openpete_state_mem_t* m, CPUState* cpu,
                            unsigned* out_frame, int quiet) {
    if (!m || !m->valid) return -1;
    if (sched_refuses_load("in-memory state load")) return -1;
    memcpy(cpu,                        m->cpu_buf,     CPU_REG_BYTES);
    memcpy(openpete_psx_get_ram(),     m->ram,         OPENPETE_PSX_RAM_SIZE);
    memcpy(openpete_psx_get_scratch(), m->scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    if (PsyX_SPUAL_RestoreStateEx(m->spu, m->spu_bytes, quiet ? 1 : 0) != 0)
        return -1;
    g_cdCurrentSector = m->cd.cur_sector;
    g_cdSectorSize    = m->cd.sector_size;
    g_cdCurrentTrack  = m->cd.cur_track;
    g_cdNumFrames     = m->cd.num_frames;
    g_CD_com          = m->cd.cd_com;
    g_cdReadDoneFlag  = m->cd.read_done;
    g_cdXaStartLBA    = m->cd.xa_start_lba;
    g_cdXaStartVBL    = m->cd.xa_start_vbl;
    PsyX_CD_QueueRestore(m->cdq);
    /* Runs on the QUIET path too (runahead's per-tick rollback): a
     * speculative tick that crossed into a load moved the WAD cursor or
     * flipped the overlay dispatcher — host state the committed timeline
     * must never inherit from an abandoned branch. Cheap and change-only
     * (the dispatcher setter no-ops when the id already matches). */
    if (m->gx_bytes) game_state_extra_load(m->gx, m->gx_bytes);
    (void)PsyX_VRAM_RestoreDelta(m->vram, m->vram_bytes);
    openpete_mods_arena_mem_restore(m->arena, m->arena_used);
    SDL_SetAtomicInt(&openpete_vbl, (int)m->vbl);
    if (!quiet) {
        game_on_state_loaded(cpu);
        openpete_guest_discontinuity_notify();
    }
    if (out_frame) *out_frame = m->frame;
    return 0;
}
