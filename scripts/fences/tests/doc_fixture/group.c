/** @name Accessors
 * @{ */
static int grouped(int a) { return a; }
/** @} */

/** @brief Returns a. */
static int briefed(int a) { return grouped(a); }

/** @brief Uses both. @param a The value. @return a. */
int group_use(int a) { return briefed(a); }
