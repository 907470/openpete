/* openpete_handoff.h — process handoff (checkpoint → exec → restore).
 * See core/mods/handoff.c for scope + contracts. */
#ifndef OPENPETE_HANDOFF_H
#define OPENPETE_HANDOFF_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/* Called once from run setup: resume_path = opts->resume_handoff (may
 * be NULL); argc/argv = the ORIGINAL process arguments (kept alive —
 * exec rebuilds from them). Also arms OPENPETE_HANDOFF_AT=<frame>. */
void openpete_handoff_init(const char* resume_path, int argc, char** argv);

/* Queue a handoff; it executes at the next quiescent point. (The mod
 * menu's "brief restart" tier.) */
void openpete_handoff_request(void);

/** @brief Run the pending handoff leg at a run-loop quiescent point.
 *
 * On the first call of a --resume-handoff process, loads the handoff file,
 * restores openpete_frame, deletes the file, and disarms the checkpoint leg
 * so a frame trigger that fired in the previous process does not fire
 * again; a failed load logs an error and the process continues from its
 * fresh state. Otherwise, when a handoff was requested or the
 * OPENPETE_HANDOFF_AT frame is reached, saves a checkpoint to
 * handoff.state in the savestate directory and execs /proc/self/exe with
 * the original argv plus --resume-handoff (POSIX only; Windows logs an
 * error). Each leg runs at most once per process; a failed save or execv
 * logs an error and the current process keeps running.
 * @param cpu Guest CPU state the checkpoint saves and the resume restores. */
void openpete_handoff_poll(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HANDOFF_H */
