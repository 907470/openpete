/* portal.h — the Spyro 3 portal preview feeder's export to the orchestrator. */
#pragma once
struct CPUState;
/* Replay the tick's captured previews on a sub-tick present; `alpha` lerps
 * the rect and pentagon edges from the previous tick when `interp_on`. */
void portal3_reextract(struct CPUState* cpu, double alpha, int interp_on);
