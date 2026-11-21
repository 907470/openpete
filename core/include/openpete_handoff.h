#include "openpete_thread_roles.h"   /* OP_REQUIRES */
/* openpete_handoff.h - process handoff (checkpoint -> exec -> restore).
 * See core/mods/handoff.c for scope + contracts. */
#ifndef OPENPETE_HANDOFF_H
#define OPENPETE_HANDOFF_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/** @brief Record the resume file and the process arguments, and arm the OPENPETE_HANDOFF_AT trigger.
 *
 * Called once from run setup. A non-empty OPENPETE_HANDOFF_AT (parsed with
 * base 0) arms the checkpoint leg at that openpete_frame value.
 * @param resume_path the --resume-handoff file to restore at the first
 *        openpete_handoff_poll, or NULL.
 * @param argc the original process argument count.
 * @param argv the original process arguments; kept by pointer, since the
 *        exec rebuilds its argv from them. */
void openpete_handoff_init(const char* resume_path, int argc, char** argv);

/** @brief Request a checkpoint-and-exec handoff at the next openpete_handoff_poll.
 *
 * Has no effect in a process that already ran a handoff leg: one resumed
 * with --resume-handoff, or one whose checkpoint leg already ran. No
 * caller in the tree. */
void openpete_handoff_request(void);

/** @brief Set how long a --resume-handoff load waits and whether it also waits for an empty CD command queue.
 *
 * Called before the first openpete_handoff_poll. The default waits 1800
 * polls for the park site and ignores the queue.
 * @param site_wait Polls the load waits in all before it gives up; 0 restores the default.
 * @param cd_empty Nonzero to load only at a poll where the PsyCross CD
 *        command queue holds no pending read. */
void openpete_handoff_resume_policy(unsigned site_wait, int cd_empty);

/** @brief Report the --resume-handoff load's outcome.
 *
 * @param site_waits Receives the polls that found another park site; may be NULL.
 * @param cd_waits Receives the polls that found CD reads queued; may be NULL.
 * @return 0 while pending or with no resume file, 1 once loaded, -1 when it failed. */
int openpete_handoff_resume_result(unsigned* site_waits, unsigned* cd_waits);

/** @brief Run the pending handoff leg at a run-loop quiescent point.
 *
 * On the first call of a --resume-handoff process at which the game fiber is
 * parked at the handoff file's park site (waiting up to the resume
 * policy's bound, 1800 calls by default, then continuing fresh; under the
 * empty-queue policy also for an empty CD command queue), loads the handoff file, restores openpete_frame, deletes the file, and disarms the checkpoint leg
 * so a frame trigger that fired in the previous process does not fire
 * again; a failed load logs an error and the process continues from its
 * fresh state. Otherwise, when a handoff was requested or the
 * OPENPETE_HANDOFF_AT frame is reached, saves a checkpoint to
 * handoff.state in the savestate directory and execs /proc/self/exe with
 * the original argv plus --resume-handoff (POSIX only; Windows logs an
 * error). Each leg runs at most once per process; a failed save or execv
 * logs an error and the current process keeps running.
 * @param cpu Guest CPU state the checkpoint saves and the resume restores. */
void openpete_handoff_poll(CPUState* cpu) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HANDOFF_H */
