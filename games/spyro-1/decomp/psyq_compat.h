/* games/spyro-1/decomp/psyq_compat.h - PSYQ/PsyCross type compat shim.
 *
 * Bridges the renaming of PSYQ types in PsyCross. Spyro's matching-decomp
 * source uses original PSYQ identifiers (RECT, ...); PsyCross's libgpu.h
 * uses RECT16 for the same struct (PsyX renamed it because Win32 has its
 * own RECT). This shim is force-included BEFORE every compile of a .c file
 * under spyro-1/src/, so the source can stay identical to the decomp tree.
 *
 * Add aliases here as new TUs surface. Each alias should be a one-liner
 * with a comment naming the upstream PsyCross symbol it points at.
 *
 * Strategy:
 *   - Pre-include <libgpu.h>/<libgte.h> from PsyCross so their typedefs
 *     are already in scope.
 *   - Then `#define RECT RECT16` (and similar) so subsequent uses of the
 *     PSYQ name resolve to PsyCross's struct.
 */

#ifndef SPYRO_DECOMP_PSYQ_COMPAT_H
#define SPYRO_DECOMP_PSYQ_COMPAT_H

#include <libgte.h>
#include <libgpu.h>
#include <libcd.h>

/* PsyCross renamed PSYQ's RECT to RECT16 to dodge the Win32 RECT clash. */
#define RECT RECT16

/* Native cd.c (post-rewriter) registers its read-completion callback
 * (CDReadDone) via the PSYQ name CdReadCallback. We route reads through
 * psyq_CdRead in psyq_thunks.cpp (XA pump, overlay-magic detection), so
 * native-side registration must land in psyq_thunks.cpp's host-cb slot
 * - not PsyCross's internal register, which our flow doesn't consult.
 * openpete_CdReadCallback (psyq_thunks.cpp) does that bridge. */
typedef void (*openpete_native_cd_read_cb_t)(unsigned char intr,
                                             unsigned char* result);
void openpete_CdReadCallback(openpete_native_cd_read_cb_t cb);
#define CdReadCallback openpete_CdReadCallback

#endif
