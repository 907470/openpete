## Change

What changed and why.

## Checks run

Rows of the table under Checks in `CONTRIBUTING.md` this change ran, with
each verdict (PASS, SKIP and its reason, or the failure). Name the game for
the oracle and the presentation golden.

- [ ] `ctest --test-dir build/linux-release -LE slow`:
- [ ] `ctest --test-dir build/linux-release -L determinism`:
- [ ] `tests/harness/check_mods.sh`:
- [ ] `tests/harness/check_presentation_golden.sh --game <game>`:
- [ ] `tests/harness/check_psx_oracle.sh --game <game>`:
- [ ] `scripts/build-docs.sh` (a diff under `docs/`):

## Sign-off

- [ ] Every commit carries a `Signed-off-by:` line (`git commit -s`;
      Developer Certificate of Origin 1.1, see `CONTRIBUTING.md`).
