#ifndef OPENPETE_NATIVE_STP_H
#define OPENPETE_NATIVE_STP_H

/* Texel STP selection of a textured scene triangle: which texels of its
 * texture the fragment stage keeps (psx_scene.frag STP_SELECT). The GPU
 * applies a semi-transparent primitive's ABR equation only to texels whose
 * bit 15 (STP) is set and draws its STP-clear texels opaque. ABR 0 reproduces
 * that through the texel alpha; the ABR 1-3 equations read no alpha, so a 2D
 * feeder emits such a primitive as two triangle sets: its STP-clear texels
 * through the opaque pipe, then its STP-set texels through the ABR's pipe. */
enum {
    OPENPETE_STP_ALL = 0,      /* every non-zero texel (the default) */
    OPENPETE_STP_CLEAR_ONLY,   /* texels with bit 15 clear */
    OPENPETE_STP_SET_ONLY,     /* texels with bit 15 set */
    OPENPETE_STP_SELS
};

/* Sticky, like set_prim: every tri added after the call carries `sel` until
 * the next call. A feeder that sets it resets it to OPENPETE_STP_ALL after its
 * triangles. */
void openpete_native_scene_set_stp_select(int sel);

#endif
