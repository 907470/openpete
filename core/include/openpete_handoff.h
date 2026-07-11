/* openpete_handoff.h — process handoff (checkpoint → exec → restore).
 * See core/runtime/handoff.c for scope + contracts. */
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

/* Run-loop quiescent point: performs the pending resume (first call of
 * a --resume-handoff process) or the pending checkpoint+exec. */
void openpete_handoff_poll(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HANDOFF_H */
