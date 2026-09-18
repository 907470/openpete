/* Negative-control fixture for complexity_ratchet.py. `fresh` scores 40 with no row and no funding: refused. */
int fresh(int a) {
    int r = 0;
    if (a > 0) {
        if (a > 1) {
            if (a > 2) {
                if (a > 3) {
                    if (a > 4) {
                        if (a > 5) {
                            if (a > 6) {
                                if (a > 7) {
                                    r = a;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (a == 100) r += 0;
    if (a == 101) r += 1;
    if (a == 102) r += 2;
    if (a == 103) r += 3;
    return r;
}
