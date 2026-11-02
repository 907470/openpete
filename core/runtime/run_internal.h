// core/runtime/run_internal.h - run.c symbols the host-loop modules
// (host_state_ops.c) call.
#pragma once

#include "openpete_input_log.h"
#include "psxrt/psx_runtime.h"

#include <stdint.h>

/** @brief Read OPENPETE_SYNTH_PADS once: the synthetic pad-stream mode for slots 1-3.
 *
 * The synthetic stream lets the N-port input log be exercised without host
 * devices filling slots 1-3. Its bytes are a pure function of the VBL index
 * and the slot (synth_pad_for), so the append path checks that the staged
 * ports survived the tick and the rewind burst checks that the ports it
 * restaged from the log match what that VBL carried. Generate-and-verify
 * stages the stream over the live devices. Verify-only leaves the staged
 * slots as movie replay filled them and only checks them: replaying a
 * recording of the stream with the generator on would overwrite the bytes
 * read back from disk with the bytes they are compared against.
 * @return 0 off (unset, empty or starting with '0'), 2 verify-only (a value
 *         starting with 'v' or 'V'), else 1 generate-and-verify. */
int synth_pads_enabled(void);

/** @brief Test an input record against the synthetic stream over the slots the current mode fills.
 *
 * Generate-and-verify checks slots 1-3; verify-only checks slot 1 alone,
 * the one player-2 slot a two-player movie replay carries.
 * @param rec The input record.
 * @param frame The VBL index the stream is generated for.
 * @return 1 when every checked slot matches, else 0. */
int synth_pads_match(const openpete_input_rec_t* rec, unsigned frame);

extern unsigned long long s_synth_rewind_checks;
extern unsigned long long s_synth_rewind_mismatches;

/* Game fiber state. Static rather than capture so the libco trampoline (which
 * takes no args) can find it. */
struct run_game {
    CPUState cpu;
    uint32_t entry;
    uint32_t gp;
    int      done;
    int      dispatch_ok;
};
extern struct run_game g_game;
