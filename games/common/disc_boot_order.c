/* games/common/disc_boot_order.c - install this game's disc boot-order
 * profile in the disc cache loader.
 *
 * The table is generated from games/<game>/manifest/disc_boot_order.toml by
 * tools/disc_boot_order.py header into <build>/games/<game>/gen/
 * disc_boot_order.gen.h (cmake/openpete_game.cmake); a game without the
 * manifest compiles this TU with OPENPETE_DISC_BOOT_ORDER_NONE and its
 * loader runs in file order. Registration is an explicit call from game
 * main before openpete_run(), like the state-region table.
 */

#include "openpete_disc_cache.h"
#include <stddef.h>
#ifndef OPENPETE_DISC_BOOT_ORDER_NONE
#include "disc_boot_order.gen.h"
#endif

/** @brief Installs the game's generated boot-order profile in the disc cache loader; file order when the game has none. */
void game_disc_boot_order_register(void) {
#ifdef OPENPETE_DISC_BOOT_ORDER_NONE
    openpete_disc_cache_set_boot_order(NULL, 0);
#else
    openpete_disc_cache_set_boot_order(openpete_disc_boot_order_table,
                                       OPENPETE_DISC_BOOT_ORDER_COUNT);
#endif
}
