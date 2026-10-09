/**
 * @file game_structs_check.c
 * @brief Compiles the generated typed game structs so their pins run.
 *
 * openpete_game_structs.h ends in one _Static_assert per struct size and
 * per member offset, taken from manifest/struct_layout.toml - the MIPS
 * o32 layout probed from the decompilation's own headers. Compiling the
 * header proves the host view of every struct has the guest layout, so a
 * header change that moves a member fails the build instead of silently
 * moving an offsetof.
 */
#include "openpete_game_structs.h"

/** Marks the layout pins as compiled; a translation unit needs one
 *  external declaration. */
const int openpete_game_structs_checked = 1;
