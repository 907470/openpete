# 0004 — raw-binary input mode (`--raw-bin`, `--load-addr`, `--output-stem`)

**Files:** `recompiler/src/main_psx.cpp`

Spyro's level/titlescreen/credits overlays are raw `.text+.rodata+.data`
blobs with no PS-X EXE header, loaded at a fixed kseg0 window
(`0x8007AA38`). Previously an out-of-tree python tool
(`tools/synth_overlay_exe.py`) wrapped them in a 2 KiB header file before
invoking PSXRecomp; the openpete first-boot pipeline (distribution plan
D2) must run with no python, so the synthesis moved into the recompiler:

- `--raw-bin` — treat the input as a raw code blob; synthesize the PS-X
  EXE header in memory (magic, `pc0 = t_addr = --load-addr`, `gp = 0`,
  `t_size` = blob size) and hand it to `PS1ExeParser::parse_buffer`.
  Byte-identical to the retired python wrapper's output by construction;
  verified byte-identical generated C across all 37 Spyro overlays.
- `--load-addr=<hex>` — kseg0 load address for the blob (required with
  `--raw-bin`).
- `--output-stem=<name>` — basename for `<stem>_full.c`/`<stem>_dispatch.c`
  (default: the input filename), so output naming no longer depends on
  what the caller's input file happens to be called.
