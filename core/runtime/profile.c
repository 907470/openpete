/* openpete_profile.c — see openpete_profile.h.
 *
 * Streams a compact binary `.opp` trace: an interned name table plus fixed
 * 32-byte records for presents, CPU spans, GPU zones, and census counts. The
 * binning rule is implicit in stream order: every SPAN/COUNT/GPUZ record
 * belongs to the next PRESENT record. Post-run tools reconstruct nesting from
 * (depth, t0, dur) and join GPU zones by the present_index they carry.
 *
 * All little-endian (producer and every consumer are LE x86-64 / ARM64). Writes
 * go through a large stdio buffer; one fwrite per record. Main-thread only.
 */
#include "openpete_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* ---- record type tags ------------------------------------------------------ */
enum {
    OPP_REC_NAME    = 1,  /* variable length: type,u16 id,u16 len,bytes         */
    OPP_REC_PRESENT = 2,  /* fixed 32                                           */
    OPP_REC_SPAN    = 3,  /* fixed 32                                           */
    OPP_REC_GPUZ    = 4,  /* fixed 32                                           */
    OPP_REC_COUNT   = 5,  /* fixed 32                                           */
    OPP_REC_FUNC    = 6,  /* fixed 32: per-function aggregate (Tier B)          */
};
#define OPP_REC_SIZE   32
#define OPP_MAGIC      "OPPR"
#define OPP_VERSION    1u

/* ---- state ----------------------------------------------------------------- */
static FILE*    s_fp = NULL;
static int      s_on = 0;
static uint32_t s_present_index = 0;
static uint32_t s_spans_since_present = 0;
static SDL_ThreadID s_main_tid = 0;

#define OPP_MAX_NAMES 8192
static char* s_names[OPP_MAX_NAMES];
static int   s_name_count = 0;

#define OPP_STACK_MAX 32
struct opp_span_frame { int name_id; uint64_t t0_us; };
static struct opp_span_frame s_stack[OPP_STACK_MAX];
static int  s_sp = 0;
static int  s_warned_overflow = 0;
static int  s_warned_underflow = 0;

/* ---- little-endian packers ------------------------------------------------- */
static inline void le_u16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void le_u32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline void le_u64(uint8_t* p, uint64_t v) {
    le_u32(p, (uint32_t)v); le_u32(p+4, (uint32_t)(v>>32));
}

static int on_main_thread(void) { return SDL_GetCurrentThreadID() == s_main_tid; }

/* ---- clock ----------------------------------------------------------------- */
uint64_t op_prof_now_us(void) {
    static double us_per_tick = 0.0;
    if (us_per_tick == 0.0) us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
    return (uint64_t)((double)SDL_GetPerformanceCounter() * us_per_tick);
}

int op_prof_on(void) { return s_on; }

uint32_t op_prof_next_present_index(void) { return s_present_index; }

/* ---- lifecycle ------------------------------------------------------------- */
int op_prof_open(const char* path) {
    if (!path || !*path) return 0;
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        fprintf(stderr, "[profile] fopen %s failed\n", path);
        return -1;
    }
    static char buf[256 * 1024];
    setvbuf(s_fp, buf, _IOFBF, sizeof(buf));

    uint8_t hdr[OPP_REC_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, OPP_MAGIC, 4);
    le_u32(hdr + 4, OPP_VERSION);
    le_u64(hdr + 8, 1000000u);   /* clock_hz: timestamps are already µs */
    le_u64(hdr + 16, 0u);        /* created_unix_ns: Date unavailable here */
    fwrite(hdr, 1, sizeof(hdr), s_fp);

    s_main_tid = SDL_GetCurrentThreadID();
    s_present_index = 0;
    s_spans_since_present = 0;
    s_sp = 0;
    s_name_count = 0;
    s_on = 1;
    fprintf(stderr, "[profile] tracing to %s\n", path);
    return 0;
}

void op_prof_close(void) {
    if (!s_fp) return;
    fflush(s_fp);
    fclose(s_fp);
    s_fp = NULL;
    s_on = 0;
    for (int i = 0; i < s_name_count; i++) { free(s_names[i]); s_names[i] = NULL; }
    s_name_count = 0;
}

/* ---- name interning -------------------------------------------------------- */
int op_prof_intern(const char* name) {
    if (!s_on || !name) return -1;
    for (int i = 0; i < s_name_count; i++)
        if (strcmp(s_names[i], name) == 0) return i;
    if (s_name_count >= OPP_MAX_NAMES) return -1;
    int id = s_name_count++;
    s_names[id] = strdup(name);

    size_t len = strlen(name);
    if (len > 65535) len = 65535;
    uint8_t hdr[5];
    hdr[0] = OPP_REC_NAME;
    le_u16(hdr + 1, (uint16_t)id);
    le_u16(hdr + 3, (uint16_t)len);
    fwrite(hdr, 1, sizeof(hdr), s_fp);
    fwrite(name, 1, len, s_fp);
    return id;
}

/* ---- CPU spans ------------------------------------------------------------- */
void op_prof_begin(int name_id) {
    if (!s_on || !on_main_thread()) return;
    if (s_sp >= OPP_STACK_MAX) {
        if (!s_warned_overflow) { s_warned_overflow = 1;
            fprintf(stderr, "[profile] span stack overflow (depth>%d); dropping\n", OPP_STACK_MAX); }
        return;
    }
    s_stack[s_sp].name_id = name_id;
    s_stack[s_sp].t0_us   = op_prof_now_us();
    s_sp++;
}

static void emit_span(int depth, int name_id, uint64_t t0_us, uint32_t dur_us, uint32_t aux) {
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_SPAN;
    r[1] = (uint8_t)depth;
    le_u16(r + 2, (uint16_t)name_id);
    le_u64(r + 8, t0_us);
    le_u32(r + 16, dur_us);
    le_u32(r + 20, aux);
    fwrite(r, 1, sizeof(r), s_fp);
    s_spans_since_present++;
}

void op_prof_end(void) {
    if (!s_on || !on_main_thread()) return;
    if (s_sp <= 0) {
        if (!s_warned_underflow) { s_warned_underflow = 1;
            fprintf(stderr, "[profile] unbalanced op_prof_end; ignoring\n"); }
        return;
    }
    s_sp--;
    uint64_t now = op_prof_now_us();
    uint64_t t0  = s_stack[s_sp].t0_us;
    emit_span(s_sp, s_stack[s_sp].name_id, t0, (uint32_t)(now - t0), 0);
}

void op_prof_add_span(int name_id, uint64_t dur_us) {
    if (!s_on || !on_main_thread() || name_id < 0) return;
    uint64_t now = op_prof_now_us();
    uint64_t t0  = (dur_us <= now) ? (now - dur_us) : now;
    /* depth = current stack depth so a phase span forwarded mid-scope nests
     * correctly under whatever is open (normally depth 0). */
    emit_span(s_sp, name_id, t0, (uint32_t)dur_us, 0);
}

/* ---- census counts --------------------------------------------------------- */
void op_prof_count(int name_id, uint64_t value) {
    if (!s_on || !on_main_thread() || name_id < 0) return;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_COUNT;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 4, s_present_index);   /* the present this count will land in */
    le_u64(r + 8, value);
    fwrite(r, 1, sizeof(r), s_fp);
}

/* ---- GPU zones ------------------------------------------------------------- */
void op_prof_gpu_zone(int name_id, int depth, uint32_t present_index,
                      uint32_t t0_rel_us, uint32_t dur_us) {
    if (!s_on || !on_main_thread() || name_id < 0) return;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_GPUZ;
    r[1] = (uint8_t)depth;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 4, present_index);
    le_u32(r + 8, t0_rel_us);
    le_u32(r + 12, dur_us);
    fwrite(r, 1, sizeof(r), s_fp);
}

/* ---- per-function aggregate (Tier B) --------------------------------------- */
void op_prof_func(int name_id, uint32_t addr, uint64_t calls,
                  uint64_t self_us, uint64_t total_us) {
    if (!s_on || !on_main_thread() || name_id < 0) return;
    /* Two 32-byte records: [FUNC|addr|calls|self_us] then a trailing total_us in
     * a second COUNT-shaped payload would complicate the reader, so pack self
     * and total into one record via two u64 fields (24 bytes used). */
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_FUNC;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 4, addr);
    le_u64(r + 8, self_us);
    le_u64(r + 16, total_us);
    /* calls in the last 4 bytes (clamped) + high 4 unused */
    le_u32(r + 24, (uint32_t)(calls > 0xFFFFFFFFu ? 0xFFFFFFFFu : calls));
    fwrite(r, 1, sizeof(r), s_fp);
}

/* ---- present boundary ------------------------------------------------------ */
uint32_t op_prof_present(uint32_t canon_tick, int kind) {
    if (!s_on || !on_main_thread()) return s_present_index;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_PRESENT;
    r[1] = (uint8_t)kind;
    le_u32(r + 4, s_present_index);
    le_u32(r + 8, canon_tick);
    le_u32(r + 12, s_spans_since_present);
    le_u64(r + 16, op_prof_now_us());
    fwrite(r, 1, sizeof(r), s_fp);

    uint32_t idx = s_present_index;
    s_present_index++;
    s_spans_since_present = 0;
    return idx;
}
