// core/runtime/savestate.c — save/load of openpete game state to slot files.
//
// CPUState register file + g_ram + g_scratch + PsyCross VRAM mirror +
// frame counter. Slot file layout (little-endian throughout, no padding
// inside the header thanks to fixed-size fields):
//
//   uint32_t  magic         = 'OSv1' (0x3176534F LE)
//   uint32_t  version       = 2
//   char      build_hash[32]
//   uint32_t  frame
//   uint32_t  cpu_bytes
//   uint32_t  ram_bytes
//   uint32_t  scratch_bytes
//   uint32_t  vram_bytes    (1 MiB — VRAM_WIDTH * VRAM_HEIGHT * 2)
//   uint32_t  reserved[7]   = 0  (room for DRAWENV/DISPENV/SPU offsets later)
//   uint8_t   cpu_regs[cpu_bytes]
//   uint8_t   ram[ram_bytes]
//   uint8_t   scratch[scratch_bytes]
//   uint8_t   vram[vram_bytes]
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
// Build-hash gating: any rebuild changes __DATE__/__TIME__, so a slot
// from an old binary refuses to load. Cheap insurance against silent
// layout drift between recompiles. (Ship-quality hashing — e.g. of the
// full ELF — can come later; the requirement here is "different builds
// must not load each other's slots", and __DATE__ "T" __TIME__ meets
// that as long as the user does a fresh build between sessions.)

#include "openpete_state.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "game_extras.h"

#include "PsyX/PsyX_public.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <direct.h>
  #define OPENPETE_MKDIR(path) _mkdir(path)
#else
  #include <unistd.h>
  #define OPENPETE_MKDIR(path) mkdir((path), 0755)
#endif

#define OPENPETE_STATE_MAGIC    0x3176534Fu  /* 'OSv1' little-endian */
#define OPENPETE_STATE_VERSION  2u
#define OPENPETE_STATE_HASH_LEN 32

/* Just the register file — the function-pointer tail of CPUState is
 * runtime-only, must not be saved or restored. */
#define CPU_REG_BYTES ((uint32_t)offsetof(CPUState, read_word))

typedef struct {
    uint32_t magic;
    uint32_t version;
    char     build_hash[OPENPETE_STATE_HASH_LEN];
    uint32_t frame;
    uint32_t cpu_bytes;
    uint32_t ram_bytes;
    uint32_t scratch_bytes;
    uint32_t vram_bytes;
    uint32_t reserved[7];
} openpete_state_header_t;

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

static void build_hash_fill(char out[OPENPETE_STATE_HASH_LEN]) {
    memset(out, 0, OPENPETE_STATE_HASH_LEN);
    /* __DATE__ is 11 chars, __TIME__ is 8, plus a 'T' separator = 20.
     * Fits comfortably in 32; remainder stays zero. */
    snprintf(out, OPENPETE_STATE_HASH_LEN, "%sT%s", __DATE__, __TIME__);
}

/* Resolve $XDG_STATE_HOME/openpete/states (or %LOCALAPPDATA%/openpete/states
 * on Windows; fall back to $HOME/.local/state on POSIX). Creates the
 * intermediate directories as needed. Writes the directory into `out`
 * with no trailing separator. Returns 0 on success. */
static int resolve_state_dir(char* out, size_t out_size) {
    const char* base = NULL;
    char        fallback[1024];

#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (!base || !*base) {
        const char* up = getenv("USERPROFILE");
        if (!up || !*up) {
            OPL_ERROR_F("savestate", "neither LOCALAPPDATA nor USERPROFILE set");
            return -1;
        }
        snprintf(fallback, sizeof(fallback), "%s\\AppData\\Local", up);
        base = fallback;
    }
#else
    base = getenv("XDG_STATE_HOME");
    if (!base || !*base) {
        const char* home = getenv("HOME");
        if (!home || !*home) {
            OPL_ERROR_F("savestate", "neither XDG_STATE_HOME nor HOME set");
            return -1;
        }
        snprintf(fallback, sizeof(fallback), "%s/.local/state", home);
        base = fallback;
    }
#endif

#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    char level1[2048];
    snprintf(level1, sizeof(level1), "%s%copenpete", base, sep);
    (void)OPENPETE_MKDIR(base);     /* fallback path may not exist yet */
    (void)OPENPETE_MKDIR(level1);

    int n = snprintf(out, out_size, "%s%cstates", level1, sep);
    if (n < 0 || (size_t)n >= out_size) {
        OPL_ERROR_F("savestate", "state-dir path too long");
        return -1;
    }
    (void)OPENPETE_MKDIR(out);
    return 0;
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

int openpete_state_save(unsigned slot, const CPUState* cpu, unsigned frame) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        OPL_ERROR_F("savestate", "fopen %s for write failed: %s",
                    path, strerror(errno));
        return -1;
    }

    openpete_state_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = OPENPETE_STATE_MAGIC;
    hdr.version       = OPENPETE_STATE_VERSION;
    build_hash_fill(hdr.build_hash);
    hdr.frame         = (uint32_t)frame;
    hdr.cpu_bytes     = CPU_REG_BYTES;
    hdr.ram_bytes     = OPENPETE_PSX_RAM_SIZE;
    hdr.scratch_bytes = OPENPETE_PSX_SCRATCH_SIZE;
    hdr.vram_bytes    = PsyX_VRAM_GetSize();

    /* Snapshot VRAM into a heap buffer before writing — keeps the host-
     * side `vram` array stable across the (potentially slow) disk I/O
     * even though the game fiber is yielded right now. */
    void* vram_snap = malloc(hdr.vram_bytes);
    if (!vram_snap || PsyX_VRAM_Snapshot(vram_snap, hdr.vram_bytes) != 0) {
        free(vram_snap);
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: VRAM snapshot failed", path);
        return -1;
    }

    int rc = 0;
    if (rc == 0) rc = write_section(fp, path, "header",  &hdr, sizeof(hdr));
    if (rc == 0) rc = write_section(fp, path, "cpu",     cpu, CPU_REG_BYTES);
    if (rc == 0) rc = write_section(fp, path, "ram",     openpete_psx_get_ram(),
                                    OPENPETE_PSX_RAM_SIZE);
    if (rc == 0) rc = write_section(fp, path, "scratch", openpete_psx_get_scratch(),
                                    OPENPETE_PSX_SCRATCH_SIZE);
    if (rc == 0) rc = write_section(fp, path, "vram",    vram_snap, hdr.vram_bytes);

    free(vram_snap);
    fclose(fp);
    if (rc != 0) return -1;
    OPL_INFO_F("savestate", "saved slot %u to %s (frame=%u)", slot, path, frame);
    return 0;
}

int openpete_state_load(unsigned slot, CPUState* cpu, unsigned* out_frame) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;

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

    char want_hash[OPENPETE_STATE_HASH_LEN];
    build_hash_fill(want_hash);
    if (memcmp(hdr.build_hash, want_hash, OPENPETE_STATE_HASH_LEN) != 0) {
        fclose(fp);
        char hh[OPENPETE_STATE_HASH_LEN + 1];
        char wh[OPENPETE_STATE_HASH_LEN + 1];
        memcpy(hh, hdr.build_hash, OPENPETE_STATE_HASH_LEN); hh[OPENPETE_STATE_HASH_LEN] = '\0';
        memcpy(wh, want_hash,      OPENPETE_STATE_HASH_LEN); wh[OPENPETE_STATE_HASH_LEN] = '\0';
        OPL_ERROR_F("savestate", "%s: build_hash mismatch (slot=%s, binary=%s)",
                    path, hh, wh);
        return -1;
    }

    if (hdr.cpu_bytes != CPU_REG_BYTES ||
        hdr.ram_bytes != OPENPETE_PSX_RAM_SIZE ||
        hdr.scratch_bytes != OPENPETE_PSX_SCRATCH_SIZE ||
        hdr.vram_bytes != PsyX_VRAM_GetSize()) {
        fclose(fp);
        OPL_ERROR_F("savestate",
                    "%s: section sizes mismatch (cpu=%u/%u ram=%u/%u "
                    "scratch=%u/%u vram=%u/%u)",
                    path,
                    hdr.cpu_bytes, CPU_REG_BYTES,
                    hdr.ram_bytes, OPENPETE_PSX_RAM_SIZE,
                    hdr.scratch_bytes, OPENPETE_PSX_SCRATCH_SIZE,
                    hdr.vram_bytes, PsyX_VRAM_GetSize());
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
    uint8_t* ram_buf  = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
    uint8_t* vram_buf = (uint8_t*)malloc(hdr.vram_bytes);
    uint8_t  scratch_buf[OPENPETE_PSX_SCRATCH_SIZE];

    if (!ram_buf || !vram_buf) {
        free(ram_buf); free(vram_buf);
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: out of memory", path);
        return -1;
    }

    int rc = 0;
    if (rc == 0) rc = read_section(fp, path, "cpu",     cpu_buf,     CPU_REG_BYTES);
    if (rc == 0) rc = read_section(fp, path, "ram",     ram_buf,     OPENPETE_PSX_RAM_SIZE);
    if (rc == 0) rc = read_section(fp, path, "scratch", scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    if (rc == 0) rc = read_section(fp, path, "vram",    vram_buf,    hdr.vram_bytes);
    fclose(fp);

    if (rc != 0) {
        free(ram_buf); free(vram_buf);
        return -1;
    }

    /* Commit. Only the register file is overwritten on cpu — the
     * function-pointer tail (read_word/write_word/...) keeps its host
     * bindings from openpete_psx_runtime_init. */
    memcpy(cpu,                        cpu_buf,     CPU_REG_BYTES);
    memcpy(openpete_psx_get_ram(),     ram_buf,     OPENPETE_PSX_RAM_SIZE);
    memcpy(openpete_psx_get_scratch(), scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    (void)PsyX_VRAM_Restore(vram_buf, hdr.vram_bytes);
    free(ram_buf); free(vram_buf);

    /* Per-game post-load resync — restores any host-side state that
     * mirrors RAM (e.g. Spyro's active-overlay dispatcher pointer). */
    game_on_state_loaded(cpu);

    if (out_frame) *out_frame = hdr.frame;
    OPL_INFO_F("savestate", "loaded slot %u from %s (frame=%u)",
               slot, path, hdr.frame);
    return 0;
}
