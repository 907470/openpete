static int untouched_undoc(int a) {
    return a;
}

/** @brief Returns its argument. */
static int untouched_doc(int a) {
    return a;
}

/** @brief Sums both helpers. @param a The value. @return The sum. */
int untouched_use(int a) { return untouched_undoc(a) + untouched_doc(a); }
