# Contributing

openpete is licensed under the MIT License (`LICENSE`). A contribution is
accepted under the same licence; no copyright assignment and no separate
contributor agreement.

## Developer Certificate of Origin

Every commit carries a `Signed-off-by:` line (`git commit -s`) with the
author's real name and an email address that reaches them. The sign-off
certifies the Developer Certificate of Origin 1.1
(<https://developercertificate.org/>): the author wrote the change or has
the right to submit it under the project's licence, and understands that
the contribution and the sign-off are public and kept with the project. A
pull request with an unsigned commit is not merged until the commit is
signed off.

## Before opening a pull request

- Build and run from `docs/src/developer/building.md`; `scripts/setup.sh`
  arms the tracked git hooks (`githooks/`), which refuse a commit that
  breaks the documentation, complexity or golden-image rules checked at
  commit time.
- Run the gates the change owes. A change to gameplay, the recompiler, the
  GTE or the state-region manifest runs `tests/harness/check_psx_oracle.sh`;
  a change to rendering runs `tests/harness/check_presentation_golden.sh`
  for every game that links the changed file; a docs-only change runs
  `mkdocs build --strict -f docs/mkdocs.yml`. The developer pages describe
  each gate and how to read its verdict.
- Every C/C++ function carries a doxygen comment opening with a one-line
  `@brief`; the doc ratchet refuses a touched function without one.
- A comment describes what the code does and the invariant it keeps, not
  who wrote it or when.
- A commit message states what changed and why; it carries no attribution
  trailer other than the sign-off.

## What is not accepted

- Game data: disc images, extracted assets, savestates or recordings that
  contain copyrighted game bytes. The repository holds no game content and
  the hooks refuse a tracked image under `tests/golden/`.
- Third-party code without its licence text: a new dependency adds its
  row to `NOTICE.md` and `docs/licences/manifest.tsv` in the same change,
  and packaging refuses a row with no text.
