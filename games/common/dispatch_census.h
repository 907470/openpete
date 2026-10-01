/* games/common/dispatch_census.h — host half of the overlay dispatch census.
 *
 * overlay_dispatch.c is compiled into the recomp module, whose first-boot
 * build sees no libc headers; it classifies each dispatch into the overlay
 * window and hands the row to these exe-side functions, which own the file. */
#ifndef OPENPETE_DISPATCH_CENSUS_H
#define OPENPETE_DISPATCH_CENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opens the file named by OPENPETE_DISPATCH_CENSUS for writing.
 *
 * An unset or empty variable, or a failed open, leaves the census off.
 * @return Nonzero when the census file is open. */
int  dispatch_census_open(void);
/** @brief Appends one `<overlay> <addr> <ra> <class> <resolved|missed>` row.
 *
 * A no-op when the census file is not open.
 * @param overlay Name of the resident overlay, or "none".
 * @param addr The dispatch target.
 * @param ra The link register captured before the callee ran.
 * @param cls The call class: jalr, jal, host or jr.
 * @param resolved Non-zero when a dispatcher ran the target. */
void dispatch_census_append(const char* overlay, uint32_t addr, uint32_t ra,
                            const char* cls, int resolved);

#ifdef __cplusplus
}
#endif
#endif
