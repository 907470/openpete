/* games/spyro-1/decomp/psyq_compat.h — Phase 3/4 compat shim.
 *
 * Bridges the renaming of PSYQ types in PsyCross. Spyro's matching-decomp
 * source uses original PSYQ identifiers (RECT, …); PsyCross's libgpu.h
 * uses RECT16 for the same struct (PsyX renamed it because Win32 has its
 * own RECT). This shim is force-included BEFORE every spyro-1/src/*.c
 * compile so the source can stay identical to the decomp tree.
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

/* PsyCross renamed PSYQ's RECT to RECT16 to dodge the Win32 RECT clash. */
#define RECT RECT16

#endif
