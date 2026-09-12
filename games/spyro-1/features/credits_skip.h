#pragma once
/* credits_skip.h — `--skip-to-cutscene credits` / `credits_100`.
 *
 * The credits are not a level: the game enters them from Gnasty's Loot's
 * intro cutscene, from the 100% ending cutscene, or from the "CREDITS" pad
 * code on the inventory screen. This module replays the pad code's effect
 * once a level skip has landed, so the flag reaches both credit rolls. See
 * credits_skip.c for the mechanism. */

#include <stdint.h>

typedef struct CPUState CPUState;

/* Cutscene ids accepted by --skip-to-cutscene. */
#define SPYRO1_CUTSCENE_CREDITS      1 /* short roll: after defeating Gnasty */
#define SPYRO1_CUTSCENE_CREDITS_FULL 2 /* full roll: the 100% ending */

/* game_resolve_cutscene_arg: a cutscene id or name; -1 and the printed list
 * for an empty or unknown argument. */
int spyro1_cutscene_resolve(const char* arg);

/* Arm the credits for the next live gameplay tick. `full` selects the 100%
 * roll (g_CreditsSequence = 10 + the shared-model load it needs). */
void spyro1_credits_skip_arm(int full);

/* 1 while an armed roll has not started yet. The skip's present
 * fast-forward stays on across the intermediate level while this is set. */
int spyro1_credits_skip_pending(void);
