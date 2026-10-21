#pragma once
/* level_skip.h - the --skip-to-level driver every game shares: the phase
 * machine that induces the title screen's demo-timeout transition toward a
 * target, holds the demo flag clear through the level, and releases the
 * windowed present fast-forward on the playable edge or a fail-safe
 * deadline.
 *
 * A game supplies a level_skip_game_t: the three guest-state steps only it
 * knows (the induction, the pick test, the demo-flag hold), its playable
 * edge, and the release watches of its other skips (--skip-to-cutscene,
 * --skip-to-area), consulted in order. Disarmed, nothing here touches guest
 * memory. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* A target watched by the release block in place of the level skip's own
 * edge: armed while watching() holds, released when released() does. */
typedef struct {
    int  (*watching)(void);
    int  (*released)(CPUState* cpu, char* eng, size_t cap);
    void (*finish)(void);
} level_skip_watch_t;

typedef struct level_skip_game {
    /* Armed: make the title's induction stores once its window is open.
     * Returns 1 when made, 0 while the window is not open, -1 to refuse the
     * target (the skip disarms, its target clears and the fast-forward is
     * dropped). `log_frame` is the frame value the induction line prints. */
    int  (*induce)(CPUState* cpu, int target, uint32_t log_frame);
    /* Seeded: the loader has taken the target. NULL passes on the next
     * call, with no log line. */
    int  (*picked)(CPUState* cpu, int target);
    /* Picked: hold the demo flag clear for one tick. Returns 0 when the
     * hold ends (the skip disarms). */
    int  (*hold)(CPUState* cpu);
    /* Logged when the hold ends, with " at frame=%u" appended; NULL: none. */
    const char* hold_end_log;
    /* The level skip's own playable edge, tested in the picked phase. */
    int  (*playable)(CPUState* cpu);
    /* The other skips' release watches, the first watching one deciding. */
    const level_skip_watch_t* watches;
    size_t                    n_watches;
    /* A game.state word and value whose entry edge re-sets the fail-safe
     * deadline (a cutscene chain outlasts one deadline); word 0: none. */
    uint32_t rearm_word, rearm_value;
    /* Log lines print the tick's frame_counter rather than the feed key. */
    unsigned char log_frame_counter;
    /* level_skip_target keeps the target after the skip disarms. */
    unsigned char target_persists;

    /* --skip-to-cutscene's ride, for level_skip_arm_cutscene: the level a
     * cutscene starts from (0 the title, -1 none named), whether it rides a
     * --skip-to-level there (NULL: every row does), the arm, and the level
     * a cutscene with no named source rides (0: such an id is unknown). */
    int  (*cutscene_source)(int id);
    int  (*cutscene_rides)(int id);
    void (*cutscene_arm)(int id);
    int  cutscene_default_level;
} level_skip_game_t;

/** @brief Arm the skip to a level number, or disarm it with 0 or less.
 *
 * Arming also arms the present fast-forward in a windowed run, with the line
 * "fast-forwarding presents until level N loads".
 * @param g The game.
 * @param number The level number; 0 or less disarms. */
void level_skip_set(const level_skip_game_t* g, int number);

/** @brief Return the armed target.
 *
 * @param g The game.
 * @return The target; while disarmed 0, or the last target when the game's
 *         target_persists is set. */
int level_skip_target(const level_skip_game_t* g);

/** @brief Report whether the loader took the target and the demo-flag hold runs.
 *
 * @return 1 in the picked phase, else 0. */
int level_skip_picked(void);

/** @brief Hold presents in a windowed run until the skip's release.
 *
 * Sets openpete_skip_present_until_level and zeroes the fail-safe deadline,
 * which the next release check sets from the current counter. Does nothing
 * headless.
 * @param until Text naming the awaited edge for the log line; NULL logs
 *        nothing. */
void level_skip_ffwd_arm(const char* until);

/** @brief Advance the skip one tick, then run the release check.
 *
 * Armed, it calls the game's induce; seeded, its picked test; picked, its
 * hold, which ends the skip when it reports 0. The release check runs while
 * a present fast-forward or a watch is armed: on the first watching watch's
 * release, or with none watching on the picked phase's playable edge, it
 * logs "level playable"; past openpete_skip_failsafe_vbls() from the first
 * check it logs the fail-safe line. Either clears the fast-forward and
 * finishes every watching watch.
 * @param g The game.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter, for the deadline. */
void level_skip_on_tick(const level_skip_game_t* g, CPUState* cpu,
                        uint32_t frame_counter);

/** @brief Arm a --skip-to-cutscene target, riding a --skip-to-level to its source level.
 *
 * Refuses, with one stderr line, a cutscene whose named source differs from
 * an armed --skip-to-level. A title-source cutscene arms the present
 * fast-forward silently; any other rides a --skip-to-level to its source,
 * or to cutscene_default_level when it names none, unless one is armed or
 * the row rides none.
 * @param g The game.
 * @param id The cutscene id; 0 or less does nothing. */
void level_skip_arm_cutscene(const level_skip_game_t* g, int id);
