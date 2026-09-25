/**
 * @file sdk_windows_include_test.c
 * @brief A mod translation unit including the SDK headers and <windows.h>.
 *
 * `windef.h` defines `RECT`, and the PSYQ types the SDK carries include one
 * of its own. The SDK spells every PSYQ type with a `PSX_` prefix so the two
 * definitions coexist; without the prefix this unit fails to compile with a
 * redefinition of `RECT`.
 *
 * Cross-compiled by tests/harness/check_sdk_windows_include.sh. The static
 * assertions prove both types reached the compile (a unit that included
 * neither would also compile).
 */
#include <openpete_game_structs.h>
#include <openpete_sdk_wrappers.h>

#include <windows.h>

_Static_assert(sizeof(PSX_RECT) == 8,
               "PSX_RECT is the PSYQ RECT: four 16-bit fields");
_Static_assert(sizeof(RECT) == 16,
               "RECT is windef.h's: four 32-bit fields");

int main(void) { return 0; }
