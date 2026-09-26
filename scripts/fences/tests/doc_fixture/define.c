/** @brief Returns the constant. */
#define BLOCK_VALUE 1
static int blocked(void) { return BLOCK_VALUE; }

/** @brief Uses the constant. @return 1. */
int define_use(void) { return blocked(); }
