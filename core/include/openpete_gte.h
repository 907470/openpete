// core/include/openpete_gte.h - the GTE core entry and its instrument hooks.
//
// gte_execute (declared in psx_runtime_internal.h, defined in
// core/psx/gte.c) runs one cop2 command on a CPUState. The core it calls,
// openpete_gte_run, takes the two register arrays alone so a harness can
// drive it without a CPU. The instruments (core/psx/gte_instruments.cpp)
// are the env-gated diagnostics around each command: the side-by-side
// trace (OPENPETE_GTE_TRACE), the command census (OPENPETE_GTE_CENSUS),
// the render probe counters PsyCross prints (OPENPETE_RENDER_PROBE), a
// one-VBL caller histogram (OPENPETE_GTE_PCHIST), the screen-box caller
// log (OPENPETE_RTPS_BOXLOG) and the screen-offset census
// (OPENPETE_OFX_CENSUS).
#ifndef OPENPETE_GTE_H
#define OPENPETE_GTE_H

#include <stdint.h>

#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Run one GTE command on a register file and write the file back in its hardware read form.
 *
 * The function code is cmd bits 0-5; sf is bit 19, lm bit 10, and MVMVA's
 * mx/v/cv fields bits 17-18, 15-16 and 13-14. A code with no documented
 * command changes nothing, FLAG included. Writes the canonical-SX FIFO on
 * every RTPS and RTPT vertex.
 * @param data The 32 data registers (cop2r0-31), updated in place.
 * @param ctrl The 32 control registers (cop2r32-63), updated in place.
 * @param cmd The cop2 command field (the instruction's low 25 bits, or the whole word).
 * @return 1 when a documented command ran, 0 for an undocumented code. */
int openpete_gte_run(uint32_t* data, uint32_t* ctrl, uint32_t cmd);

/* Instrument switch read by gte_execute: -1 until the first command reads
 * the instrument environment, then 1 when any instrument is armed, else 0. */
extern int openpete_gte_instruments_on;

/** @brief Feed one executed command to the armed instruments; the first call reads their environment and may disarm them all.
 *
 * @param cpu The CPU state, holding the post-command register file.
 * @param cmd The command field gte_execute received.
 * @param ran 1 when the command was documented and ran, 0 otherwise.
 * @param pre_data Data registers before the command.
 * @param pre_ctrl Control registers before the command. */
void openpete_gte_instruments_op(CPUState* cpu, uint32_t cmd, int ran,
                                 const uint32_t pre_data[32], const uint32_t pre_ctrl[32]);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GTE_H */
