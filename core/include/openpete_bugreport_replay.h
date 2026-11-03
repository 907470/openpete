/* core/include/openpete_bugreport_replay.h - --bug-report-replay: replay a
 * bug-report bundle from its anchor in a fresh headless session and compare
 * it with the tester's run.
 *
 * Before the game is chosen, main hands the command line to
 * openpete_bugreport_replay_prepare. It reads the bundle (a zip or an
 * extracted directory), verifies every member against manifest.json,
 * copies the members into a scratch directory, and rebuilds the command
 * line: the tester's argv with its output, route and path flags dropped
 * and `~` remapped into the scratch directory, then the replay's own flags
 * (headless, the scratch copies of the anchor's cards, the bundle's mods
 * as the mods root, the anchor as the --resume-handoff file, the segment as
 * the --bk2 movie at the anchor's consume), then the developer's flags. The
 * OPENPETE_* environment becomes env.txt's, without the harness triggers,
 * and the bundle's config files become the two config tiers.
 *
 * At run setup openpete_bugreport_replay_admit admits the bundle on the
 * game, the ROM stamp (the disc SHA-256 when either side carries none) and
 * the state identity recomputed from the manifest's stamps, and labels a
 * codegen-stamp, build-version or disc difference cross-module, cross-build
 * or cross-disc. The anchor loads at its park site on an empty CD queue
 * with the movie held until the load commits; every tick after it compares
 * the region-hash row with hashes.csv; at the press frame the state is
 * captured and compared with press.state section by section. A bundle
 * written at a fatal exit (manifest press null: the hang watchdog, the
 * CdSync spin guard) holds no press.state: it replays to hashes.csv's
 * last row, compares hash rows only and names the trigger on the verdict
 * line (`sections=skipped fatal=<trigger>`). The
 * session ends with exit 0 (IDENTICAL), 2 (DIVERGED or VACUOUS) or 64
 * (refused). Nothing here stores into PSX RAM; the anchor load is the
 * savestate loader's.
 */
#ifndef OPENPETE_BUGREPORT_REPLAY_H
#define OPENPETE_BUGREPORT_REPLAY_H

#include "openpete_thread_roles.h"

#include <stdint.h>

typedef struct CPUState CPUState;
struct openpete_options;

#ifdef __cplusplus
extern "C" {
#endif

/* Exit status of a refused replay. */
#define OPENPETE_BUGREPORT_REPLAY_REFUSED 64

/** @brief Prepare a --bug-report-replay session: read, verify and copy the bundle, set the environment and the config tiers, and rebuild the command line.
 *
 * Returns at once when argv holds no --bug-report-replay. The rebuilt
 * argv is heap-allocated for the process lifetime.
 * @param argc The argument count; replaced by the rebuilt one.
 * @param argv The arguments; replaced by the rebuilt ones.
 * @return 0 to continue (no replay, or prepared), else the exit status
 *         (OPENPETE_BUGREPORT_REPLAY_REFUSED, with the reason printed). */
int openpete_bugreport_replay_prepare(int* argc, char*** argv);

/** @brief Admit the prepared bundle against the running game, module and disc, and arm the replay.
 *
 * Called at run setup after the cards and the handoff are configured.
 * @param opts The parsed options.
 * @return 0 when admitted or when no replay runs, else OPENPETE_BUGREPORT_REPLAY_REFUSED. */
int openpete_bugreport_replay_admit(const struct openpete_options* opts);

/** @brief Follow the anchor load and take the end-state compare at the press frame.
 *
 * Called at every quiescent final yield after the handoff poll. Releases
 * the movie on the iteration the anchor load commits; at the first final
 * yield at or past the press frame captures the state, compares it with
 * press.state and requests the shutdown. A fatal bundle's end frame is
 * its last hash row, where the shutdown is requested with no capture.
 * @param cpu The guest register file. */
void openpete_bugreport_replay_step(const CPUState* cpu) OP_REQUIRES(sim);

/** @brief Compare one tick's region-hash row with the bundle's row of the same frame.
 *
 * @param frame openpete_frame after the tick.
 * @param vbl openpete_vbl after the tick.
 * @param total The total hash.
 * @param per The per-region hashes (openpete_state_region_count entries). */
void openpete_bugreport_replay_tick(unsigned frame, int vbl, uint64_t total, const uint64_t* per)
    OP_REQUIRES(sim);

/** @brief Print the verdict line and turn the session's exit status into the replay's.
 *
 * @param rc The session's exit status.
 * @return @p rc when no replay ran, else 0, 2 or OPENPETE_BUGREPORT_REPLAY_REFUSED. */
int openpete_bugreport_replay_exit(int rc);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGREPORT_REPLAY_H */
