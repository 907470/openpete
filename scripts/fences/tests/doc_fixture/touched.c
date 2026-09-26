static int touched_undoc(int a) {
    return a;
}

/** Returns its argument. */
static int touched_doc(int a) {
    return a;
}

int touched_use(int a);
int touched_use(int a) { return touched_undoc(a) + touched_doc(a); }
