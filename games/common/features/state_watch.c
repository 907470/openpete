// games/common/features/state_watch.c - OPENPETE_STATE_WATCH; see
// state_watch.h.

#include "state_watch.h"

#include "openpete_fiber.h"          /* openpete_speculative, openpete_feed_key_now */
#include "psx_runtime_internal.h"    /* CPUState + its read callbacks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WATCH_MAX  16
#define WATCH_NAME 24

typedef struct {
    char     name[WATCH_NAME];
    uint32_t addr;
    char     size;          /* 'b', 'h' or 'w' */
} watch_t;

static watch_t s_watch[WATCH_MAX];
static int     s_count = -1;   /* -1 until the knob is parsed */

/** @brief Parse one NAME=ADDR[:SIZE] entry into w.
 *
 * @param text The entry, NUL-terminated.
 * @param w The entry parsed.
 * @return 1 on success, 0 on a malformed entry. */
static int watch_parse_one(const char* text, watch_t* w) {
    const char* eq = strchr(text, '=');
    if (!eq || eq == text || (size_t)(eq - text) >= WATCH_NAME) return 0;
    memcpy(w->name, text, (size_t)(eq - text));
    w->name[eq - text] = '\0';
    char* end = NULL;
    unsigned long a = strtoul(eq + 1, &end, 16);
    if (end == eq + 1) return 0;
    w->size = 'w';
    if (*end == ':') {
        w->size = end[1];
        if ((w->size != 'b' && w->size != 'h' && w->size != 'w') || end[2] != '\0') return 0;
    } else if (*end != '\0') {
        return 0;
    }
    w->addr = (uint32_t)a;
    return 1;
}

/** @brief Parse OPENPETE_STATE_WATCH into s_watch once; malformed entries are reported and skipped. */
static void watch_parse(void) {
    const char* env = getenv("OPENPETE_STATE_WATCH");
    s_count = 0;
    if (!env || !*env) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", env);
    for (char* tok = strtok(buf, ","); tok && s_count < WATCH_MAX; tok = strtok(NULL, ",")) {
        if (watch_parse_one(tok, &s_watch[s_count])) s_count++;
        else fprintf(stderr, "[state] OPENPETE_STATE_WATCH: skipped malformed entry '%s'\n", tok);
    }
}

/** @brief Read one watched address at its size, sign-extended.
 *
 * @param cpu Guest CPU state.
 * @param w The watched address.
 * @return The value, sign-extended to 32 bits. */
static int32_t watch_read(CPUState* cpu, const watch_t* w) {
    switch (w->size) {
    case 'b': return (int32_t)(int8_t)cpu->read_byte(w->addr);
    case 'h': return (int32_t)(int16_t)cpu->read_half(w->addr);
    default:  return (int32_t)cpu->read_word(w->addr);
    }
}

void state_watch_on_tick(CPUState* cpu, uint32_t frame_counter) {
    static int32_t s_prev[WATCH_MAX];
    static int s_have_prev = 0;
    if (s_count < 0) watch_parse();
    if (s_count == 0 || openpete_speculative) return;
    int32_t cur[WATCH_MAX];
    int changed = !s_have_prev;
    for (int i = 0; i < s_count; i++) {
        cur[i] = watch_read(cpu, &s_watch[i]);
        if (cur[i] != s_prev[i]) changed = 1;
    }
    if (!changed) return;
    memcpy(s_prev, cur, sizeof(int32_t) * (size_t)s_count);
    s_have_prev = 1;
    fprintf(stderr, "[state] vbl=%u frame=%u", openpete_feed_key_now(), (unsigned)frame_counter);
    for (int i = 0; i < s_count; i++) fprintf(stderr, " %s=%d", s_watch[i].name, (int)cur[i]);
    fputc('\n', stderr);
}
