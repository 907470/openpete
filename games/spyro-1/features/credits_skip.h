#pragma once
/* credits_skip.h — `--skip-to-level credits` / `credits_100`.
 *
 * The credits are not a level: the game enters them from Gnasty's Loot's
 * intro cutscene, from the 100% ending cutscene, or from the "CREDITS" pad
 * code on the inventory screen. This module replays the pad code's effect
 * once the skip has landed in a level, so the same flag reaches both credit
 * rolls. See credits_skip.c for the mechanism. */

#include <stdint.h>

typedef struct CPUState CPUState;

/* Pseudo level ids accepted by --skip-to-level. Above every real id (10-65)
 * and the custom-level slot. */
#define SPYRO1_LEVEL_CREDITS      70 /* short roll: after defeating Gnasty */
#define SPYRO1_LEVEL_CREDITS_FULL 71 /* full roll: the 100% ending */

/* Arm the credits for the next live gameplay tick. `full` selects the 100%
 * roll (g_CreditsSequence = 10 + the shared-model load it needs). */
void spyro1_credits_skip_arm(int full);

/* 1 while an armed roll has not started yet. The skip's present
 * fast-forward stays on across the intermediate level while this is set. */
int spyro1_credits_skip_pending(void);
