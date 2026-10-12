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

/** @brief Run the pending handoff leg at a run-loop quiescent point.
 *
 * On the first call of a --resume-handoff process at which the game fiber is
 * parked at the handoff file's park site (waiting up to 1800 calls, then
 * continuing fresh), loads the handoff file, restores openpete_frame, deletes the file, and disarms the checkpoint leg
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
