/** @brief Adds one. The sum wraps. */
static int two_sentences(int a) { return a + 1; }

/** @brief Adds one to the value
 * the caller passes. */
static int two_lines(int a) { return a + 1; }

/** @brief Adds one to `a.b`, e.g. a counter.
 *
 * The detail follows a blank comment line. It may hold any number of
 * sentences. */
static int brief_then_detail(int a) { return a + 1; }

/** @brief Adds one */
static int unterminated(int a) { return a + 1; }

int longbrief_use(int a);
/** @brief Uses every function above.
 * @param a The value.
 * @return The sum. */
int longbrief_use(int a) {
    return two_sentences(a) + two_lines(a) + brief_then_detail(a)
        + unterminated(a);
}
