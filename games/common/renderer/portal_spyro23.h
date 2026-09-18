/* games/common/renderer/portal_spyro23.c's interface: the homeworld portal
 * preview feeder shared by the two games whose portal pass and preview leaf
 * are the same routines at different addresses.
 *
 * The leaf is instruction-identical across the two: 525 words each, of which
 * 49 differ and every one of those is an address inside a lui/addiu pair or a
 * jump target [CITE: games/spyro-2/spyro-2/asm/cyclorama.s:382-951;
 * games/spyro-3/spyro-3/asm/cyclorama.s:378-945]. The pass is the same routine
 * minus Spyro 3's opening animation: Spyro 3 reads the portal record's counter
 * byte +0x43, advances it by 0x10 per tick to a ceiling of 0x100 and then to 0
 * [CITE: games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:40-51],
 * and while it is nonzero scales each projected pentagon vertex, taken
 * relative to the pentagon's centroid, by counter / 256
 * [CITE: games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:303-334];
 * Spyro 2's pass never touches +0x43
 * [CITE: games/spyro-2/spyro-2/asm/nonmatchings/drawutil/func_8004C66C.s:33,65,141].
 * The animation only moves the numbers the pass writes into the parameter
 * block, which this feeder reads, so the difference needs no code here.
 *
 * The parameter block's field offsets are identical in both games; the two
 * disassemblies differ only in which register holds each clip-rect bound when
 * it is stored [CITE: games/spyro-2/spyro-2/asm/nonmatchings/drawutil/func_8004C66C.s:363-369;
 * games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:509-517].
 * So each game supplies only the block address and the two hook addresses. */
#ifndef OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H
#define OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"     /* ngte_view: the caller's projection view */
#include "renderer/terrain_game.h"   /* openpete_portal_skyplane */

/* What the two games do not share. Each game's renderer/portal_game.c defines
 * the instance; a game that leaves `pass` or `leaf` zero registers no hook and
 * never enters the feeder.
 *
 *   block  the pass's parameter block for the leaf: +0x00 y-min, +0x04 y-max,
 *          +0x08 x-min, +0x0C x-max, +0x10 preview mesh pointer, +0x14
 *          depth >> 7, +0x18/+0x1A/+0x1C far colour halfwords at GTE scale,
 *          +0x1E IR0, +0x20 edge count, +0x24 the 8-byte edge entries
 *   pass   the portal pass: gates each record, projects its pentagon, fills
 *          the block and calls the leaf
 *   leaf   the preview leaf: draws one pre-projected mesh under the block */
typedef struct portal_game_s23_consts {
    uint32_t block;
    uint32_t pass;
    uint32_t leaf;
} portal_game_s23_consts_t;

extern const portal_game_s23_consts_t portal_game_s23;

/* Replay the tick's captured previews on a sub-tick present; `alpha` lerps
 * the rect and pentagon edges from the previous tick when `interp_on`. */
void portal_s23_reextract(CPUState* cpu, double alpha, int interp_on);

/* True while a portal preview fed within the last couple of presents. */
int portal_s23_sky_recent(void);

/* Fill out[] with the opening plane of every portal the pass previewed on the
 * current or immediately-previous tick, one entry per portal slot. Returns the
 * count (0 when no preview is active). */
int portal_s23_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max);

#endif /* OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H */
