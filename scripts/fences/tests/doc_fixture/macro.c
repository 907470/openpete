/** @brief Defines an accessor returning 1. */
#define GEN_ACCESSOR(n) static int n(void) { return 1; }
GEN_ACCESSOR(generated)

/** @brief Uses the accessor. @return 1. */
int macro_use(void) { return generated(); }
