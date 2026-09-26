/** @brief Adds one. */
int ext_notag(int a) { return a + 1; }

/** @brief Adds one. */
static int static_brief(int a) { return a + 1; }

/** @brief Adds two. @param a The value. @return The sum. */
int ext_tagged(int a) { return static_brief(static_brief(a)); }
