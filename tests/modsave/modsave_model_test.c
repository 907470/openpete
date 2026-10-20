/* modsave_model_test.c - the save-record design's worked cases on the pure C rules.
 *
 * Each case is an event list with the effects every event must produce,
 * written from the design's delivery rule, status precedence and capture
 * rules (the harness cases of the save-record plan), not from the module.
 * Two mods (0 and 1), one card slot with two save indices, cap 4; every
 * case boots with both mods loaded and mounts the card first. Effect
 * grammar: modsave_model.h. Exit 0 when every case matches.
 */
#include "modsave_model.h"

#include <stdio.h>
#include <string.h>

typedef struct { msm_event_t ev; const char* fx; } step_t;

#define T_         { MSM_EV_T, 0, 0, 0, 0 }
#define RESET_     { MSM_EV_RESET, 0, 0, 0, 0 }
#define LOAD(i)    { MSM_EV_LOAD, 0, i, 0, 0 }
#define WRITE(i,c) { MSM_EV_WRITE, 0, i, c, 0 }
#define PLAY(m,v)  { MSM_EV_PLAY, m, v, 0, 0 }
#define TOGGLE(m)  { MSM_EV_TOGGLE, m, 0, 0, 0 }
#define SCHEMA(m)  { MSM_EV_SCHEMA, m, 0, 0, 0 }
#define EDIT(i,c)  { MSM_EV_EDIT, 0, i, c, 0 }
#define RESTART_   { MSM_EV_RESTART, 3, 3, 0, 0 }
#define MOUNT_     { MSM_EV_MOUNT, 0, 0, 0, 0 }
#define SS_TAKE_   { MSM_EV_SS_TAKE, 0, 0, 0, 0 }
#define SS_LOAD_   { MSM_EV_SS_LOAD, 0, 0, 0, 0 }
#define SS_OLD_    { MSM_EV_SS_LOAD_OLD, 0, 0, 0, 0 }
#define END_       { MSM_EV_COUNT, 0, 0, 0, 0 }
#define X MSM_C_X
#define Y MSM_C_Y
#define P MSM_C_P

typedef struct { const char* name; step_t steps[16]; } case_t;

static const case_t k_cases[] = {
    { "load save 1, New Game: ABSENT", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { END_, NULL } } },
    { "load 1, disable, load 2, enable: reads save 2", {
        { PLAY(0, 1), "" },
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { PLAY(0, 2), "" },
        { WRITE(1, Y), "W0;W1;S0.1:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:1;R1:OK:1:0" },
        { TOGGLE(0), "" },
        { LOAD(1), "" },
        { T_, "R1:OK:1:0" },
        { TOGGLE(0), "" },
        { T_, "R0:OK:1:2" },
        { END_, NULL } } },
    { "New Game with a mod disabled, enable: ABSENT", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { TOGGLE(0), "" },
        { RESET_, "" },
        { T_, "R1:ABSENT:0:-" },
        { TOGGLE(0), "" },
        { T_, "R0:ABSENT:0:-" },
        { END_, NULL } } },
    { "Quit Game, reload the same save: read again", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { END_, NULL } } },
    { "New Game twice with no save: ABSENT twice (intro's second clear)", {
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { END_, NULL } } },
    { "New Game, save, disable, save, enable: no read", {
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { WRITE(0, X), "W0;W1;S0.0:put" },
        { TOGGLE(0), "" },
        { WRITE(0, Y), "W1;S0.0:put" },
        { TOGGLE(0), "" },
        { T_, "" },
        { END_, NULL } } },
    { "savestate from save 1 loaded while on save 2: no read", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { SS_TAKE_, "" },
        { WRITE(1, Y), "W0;W1;S0.1:put" },
        { LOAD(1), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { SS_LOAD_, "" },
        { T_, "" },
        { END_, NULL } } },
    { "load save 1, write to index 2: no read", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { WRITE(1, Y), "W0;W1;S0.1:put" },
        { T_, "" },
        { END_, NULL } } },
    { "savestate blob older than the block: key unknown, no read", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:OK:1:0" },
        { SS_TAKE_, "" },
        { SS_OLD_, "" },
        { T_, "" },
        { WRITE(0, Y), "L:write-key-unknown;W0;W1;S0.0:put" },
        { END_, NULL } } },
    { "mod toggled off across a save, back on: GAP", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { TOGGLE(1), "" },
        { PLAY(0, 1), "" },
        { WRITE(0, Y), "W0;S0.0:put" },
        { TOGGLE(1), "" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:1;R1:GAP:1:0" },
        { END_, NULL } } },
    { "downgrade: NEWER, then the save refuses the write", {
        { SCHEMA(1), "" },
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { SCHEMA(1), "" },
        { LOAD(0), "" },
        { T_, "R0:OK:1:0;R1:NEWER:2:-" },
        { WRITE(0, X), "W0;L:refuse-newer;S0.0:front" },
        { END_, NULL } } },
    { "record edited outside openpete: STALE", {
        { WRITE(0, X), "L:write-key-unknown;W0;W1;S0.0:put" },
        { EDIT(0, Y), "" },
        { RESTART_, "" },
        { MOUNT_, "" },
        { LOAD(0), "" },
        { T_, "R0:STALE:1:0;R1:STALE:1:0" },
        { END_, NULL } } },
    { "a record with a legacy pad and no snapshot: LEGACY_PAD", {
        { EDIT(0, P), "" },
        { RESTART_, "" },
        { MOUNT_, "" },
        { LOAD(0), "" },
        { T_, "R0:LEGACY_PAD:0:P;R1:LEGACY_PAD:0:P" },
        { END_, NULL } } },
    { "snapshot eviction across two lineages", {
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { WRITE(0, X), "W0;W1;S0.0:put" },
        { WRITE(0, Y), "W0;W1;S0.0:put" },
        { WRITE(0, P), "W0;W1;S0.0:put" },
        { RESET_, "" },
        { T_, "R0:ABSENT:0:-;R1:ABSENT:0:-" },
        { WRITE(0, X), "W0;W1;S0.0:put" },
        { WRITE(0, Y), "W0;W1;S0.0:put;S0.0:evict" },
        { END_, NULL } } },
};

/** @brief Run one case; prints each mismatch and returns the mismatch count. */
static int run_case(const case_t* c) {
    msm_state_t s;
    const msm_config_t cfg = { 2, 1, 2, 4 };
    char fx[1024];
    int bad = 0;
    msm_init(&s, &cfg, 3);
    const msm_event_t mount = { MSM_EV_MOUNT, 0, 0, 0, 0 };
    msm_apply(&s, &mount, fx, sizeof fx);
    for (int i = 0; c->steps[i].fx; i++) {
        const msm_event_t* ev = &c->steps[i].ev;
        if (!msm_enabled(&s, ev)) {
            printf("  FAIL %s: step %d (%s) not enabled\n", c->name, i, msm_event_name(ev->kind));
            return bad + 1;
        }
        msm_apply(&s, ev, fx, sizeof fx);
        if (strcmp(fx, c->steps[i].fx) != 0) {
            printf("  FAIL %s: step %d (%s) effects \"%s\", want \"%s\"\n", c->name, i,
                   msm_event_name(ev->kind), fx, c->steps[i].fx);
            bad++;
        }
    }
    return bad;
}

/**
 * @brief Run every worked case.
 * @return 0 when every effect matched, 1 otherwise.
 */
int main(void) {
    int bad = 0, steps = 0;
    const int n = (int)(sizeof k_cases / sizeof k_cases[0]);
    for (int i = 0; i < n; i++) {
        const int b = run_case(&k_cases[i]);
        for (int k = 0; k_cases[i].steps[k].fx; k++) steps++;
        printf("%s %s\n", b ? "FAIL" : "PASS", k_cases[i].name);
        bad += b;
    }
    printf("modsave_model_test: %d cases, %d steps, %d mismatches\n", n, steps, bad);
    return bad ? 1 : 0;
}
