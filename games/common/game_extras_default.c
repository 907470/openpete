// games/common/game_extras_default.c — the game_extras entries whose body is
// the same for every game, parameterised by the engine-layout descriptor.
//
// A game keeps an entry in its own games/<game>/game_extras_impl.c only when
// its body differs; everything reducible to a game_layout field lives here, so
// a layout correction lands in one place instead of three.

#include "game_extras.h"
#include "game_layout.h"
#include "openpete_psx.h"

#include <stdint.h>
#include <string.h>

/* Reads game_layout.player.base + pos_off; see game_extras.h. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram || !game_layout.player.base) return -1;
    const uint32_t base = (game_layout.player.base - 0x80000000u)
                        + game_layout.player.pos_off;
    int32_t x, y, z;
    memcpy(&x, ram + base + 0, 4);
    memcpy(&y, ram + base + 4, 4);
    memcpy(&z, ram + base + 8, 4);
    *out_x = x;
    *out_y = y;
    *out_z = z;
    return 0;
}
