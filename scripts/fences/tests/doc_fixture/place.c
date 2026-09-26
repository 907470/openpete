#include "place.h"

/** Halves. @param a The value. @return Half of a. */
int placed_misplaced(int a) { return a / 2; }

/** Doubles. @param a The value. @return Twice a. */
int placed_dup(int a) { return a * 2; }

/* Body note: the multiply is exact for the fixture's range. */
int placed_ok(int a) { return a * 3; }

int placed_adjacent(int a) { return a * 4; }
