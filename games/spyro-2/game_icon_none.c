/* game_icon_none.c — this game ships no window icon yet.
 *
 * The host reads openpete_game_icon_w and skips the icon when it is zero. The
 * symbols are defined rather than left undefined because a PE/COFF link cannot
 * leave an undefined weak reference unresolved: the host's declarations are
 * weak, which is enough on ELF and not on Windows. Replacing this file with a
 * generated icon TU is the only change a game needs to gain one.
 */
#include <stdint.h>

const uint8_t openpete_game_icon_rgba[1] = { 0 };
const int openpete_game_icon_w = 0;
const int openpete_game_icon_h = 0;
