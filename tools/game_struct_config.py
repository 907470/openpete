"""game_struct_config.py - per-game inputs to the struct-layout probe.

One probe (tools/probe_struct_layout.py) and one generator
(tools/gen_game_structs.py) serve every game; everything that differs
between decompilations lives here, keyed by game slug:

  header_globs   patterns under the decomp's include/, in the order the
                 probe TU includes them and the header hash covers them
                 ("*.h" is flat, "**/*.h" walks subdirectories)
  psyq_stubs     the tools/psyq_types.py stub headers the decomp's
                 headers include, and whose types its structs embed
  empty_stubs    headers the decomp includes that carry no needed type
  unk_offset_re  the decomp's convention for naming an unidentified
                 member after its offset; the probe re-derives the offset
                 and refuses a mismatch
  unk_offset_bases  structs whose unk_ names are relative to an enclosing
                 struct, with that struct's offset
  unk_offset_skip  structs where the same spelling is a sequence label
                 rather than an offset, each with the member that proves
                 it; a listed struct with no mismatching member refuses
                 the run, so the list cannot outlive its reason
  comment_offsets  parse each member's trailing comment: `0xNN` is the
                 member's offset, an 8-digit address is checked against
                 the other addressed members of the same struct
  bss_globals    cross-check every struct-typed extern against the
                 `.space` its symbol reserves in asm/data
  known_slack    .bss reservations larger than the struct, with the reason
  probe_renames  {header: [function]} renamed by `#define` before that
                 header in the probe TU, where two headers declare one
                 function with different parameter types
  scalar_typedefs  non-struct typedefs (`typedef char StickState[4];`)
                 that the structs embed, emitted before the structs
  pins           layout facts read out of the game's own asm or source,
                 asserted by the probe against the probed values and by
                 the generator against the toml

A pin is {"struct", "size"|("field", "offset"), "why"}: `why` names the
file and line the value was read from.
"""

SPYRO1_PINS = [
    # include/moby.h cross-checked against asm/42CC4.s and renderer/moby.c.
    {"struct": "Vector3D", "size": 12, "why": "moby.h; renderer/moby.c"},
    {"struct": "Vector3D8", "size": 3, "why": "moby.h; renderer/moby.c"},
    {"struct": "SHORTMATRIX", "size": 20, "why": "moby.h; renderer/moby.c"},
    {"struct": "AnimationState", "size": 7, "why": "moby.h; renderer/moby.c"},
    {"struct": "Moby", "size": 0x58, "why": "asm/42CC4.s moby stride"},
    {"struct": "Moby", "field": "m_Props", "offset": 0x00, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_Position", "offset": 0x0C, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_DamageFlags", "offset": 0x18, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_RotationMatrix", "offset": 0x20, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_CollisionRegion", "offset": 0x34, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_Class", "offset": 0x36, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_AnimationState", "offset": 0x3C, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_Rotation", "offset": 0x44, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_State", "offset": 0x48, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_SectorIndex", "offset": 0x4A, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_RenderRadius", "offset": 0x50, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_WasDrawn", "offset": 0x51, "why": "renderer/moby.c"},
    {"struct": "Moby", "field": "m_UpdateDistance", "offset": 0x52, "why": "renderer/moby.c"},
    # The credits overlay's g_DB stores.
    {"struct": "DB", "size": 0x84,
     "why": "asm/nonmatchings/overlays/credits/func_credits_8007BFD0.s"},
    {"struct": "DRAWENV", "field": "r0", "offset": 0x19,
     "why": "asm/nonmatchings/overlays/credits/func_credits_8007BFD0.s"},
    {"struct": "DRAWENV", "size": 0x5C, "why": "psyq_game_hooks.h"},
    {"struct": "DISPENV", "size": 0x14, "why": "psyq_game_hooks.h"},
]

SPYRO3_PINS = [
    # Draw() picks the far draw buffer by adding the pair stride to the
    # near one and reads that buffer's OT base at +0x70.
    {"struct": "PauseData2", "field": "dat_8006fc70", "offset": 0x74,
     "why": "asm/nonmatchings/draw/Draw.s:49 (addiu $v1, $v1, 0x74)"},
    {"struct": "PauseData2", "field": "dat_8006fc6c", "offset": 0x70,
     "why": "asm/nonmatchings/draw/Draw.s:59 (lw $v1, 0x70($v1))"},
    # The moby array stride: the index is a magic-multiply divide by 0x58.
    {"struct": "Moby", "size": 0x58, "why": "src/savepoint.c:119"},
    {"struct": "Moby", "field": "position", "offset": 0x0C, "why": "include/common.h:112"},
    {"struct": "Moby", "field": "rotationMatrix", "offset": 0x20, "why": "include/common.h:116"},
    {"struct": "Moby", "field": "mobyClass", "offset": 0x36, "why": "include/common.h:118"},
    {"struct": "Moby", "field": "state", "offset": 0x48, "why": "include/common.h:128"},
    {"struct": "Moby", "field": "drawn", "offset": 0x4D, "why": "include/common.h:134"},
    {"struct": "DRAWENV", "size": 0x5C, "why": "psyq_game_hooks.h"},
    {"struct": "DISPENV", "size": 0x14, "why": "psyq_game_hooks.h"},
]

GAMES = {
    "spyro-1": {
        "header_globs": ["*.h"],
        "psyq_stubs": ["libgte.h", "libgpu.h", "libspu.h", "libcd.h"],
        "empty_stubs": ["libetc.h"],
        "unk_offset_re": r"(?:.*\.)?unk_0x([0-9a-fA-F]+)",
        "unk_offset_bases": {"SpyroPhysics": 0xC8},
        "unk_offset_skip": {},
        "comment_offsets": False,
        "bss_globals": True,
        "known_slack": {
            # PadBuffer is 0x20 in gamepad.h (u_char padding[24]); the .bss
            # reserves 0x28, annotated "0x24 + 0x4 padding": PSYQ's pad
            # buffer is 34 bytes, so the header's padding is 4 short. The
            # game only touches the first 8 bytes.
            "g_PadBuffer": 8,
            "g_PadBufferSecondController": 8,
        },
        "pins": SPYRO1_PINS,
    },
    "spyro-2": {
        # The decomp names few structs: the vector and spherical types,
        # the camera position, the savepoint, the pad state and one
        # unidentified tracer record.
        # The headers that define structs, in dependency order: camera.h
        # embeds common.h's SphericalCoordinates without including it.
        "header_globs": ["common.h", "camera.h", "pad.h", "tracers.h",
                         "savepoint.h"],
        # pad.h and savepoint.h declare func_8001B40C with different
        # parameter types; the probe TU renames the second declaration.
        "probe_renames": {"savepoint.h": ["func_8001B40C"]},
        # Non-struct typedefs the structs embed, emitted before them.
        "scalar_typedefs": ["StickState"],
        "psyq_stubs": [],
        "empty_stubs": [],
        "unk_offset_re": r"(?:.*\.)?unk_?([0-9A-Fa-f]+)",
        "unk_offset_bases": {},
        "unk_offset_skip": {
            # Structs where `unkN` numbers the unknown, not its offset.
            "Savepoint": "unk5 sits at 0x84c",
            "Pad": "unk1 sits at 0x0",
        },
        "comment_offsets": True,
        "bss_globals": True,
        "known_slack": {},
        "pins": [],
    },
    "spyro-3": {
        # include/moby/ and include/overlay/ carry struct definitions and
        # the level-id macros the overlay headers key on.
        "header_globs": ["**/*.h"],
        # The only PSYQ header the decomp's headers include; DRAWENV and
        # DISPENV are embedded in PauseData2, RECT and DR_ENV inside them.
        "psyq_stubs": ["libgpu.h"],
        "empty_stubs": [],
        "unk_offset_re": r"(?:.*\.)?unk_?([0-9A-Fa-f]+)",
        "unk_offset_bases": {},
        "unk_offset_skip": {
            # Structs where `unkN` numbers the unknown, not its offset.
            "WadHeader": "unk1 sits at 0x20",
            "Moby": "unkb sits at 0x51",
            "ProgressFlags": "unk25 sits at 0x19",
            "Sparx": "unk1 sits at 0x8",
            "Game": "unk1 sits at 0x4",
            "SpeechProps": "unk1a sits at 0x1",
            "SpeedwayRecord": "unk1 sits at 0x18",
            "SpriteAnimationData": "unk1 sits at 0x4",
            "SaveFile": "unk_8006c5d0 is an address, not an offset",
            "OvlHeader": "unk1b sits at 0x1c",
            "Pad": "unk1b sits at 0x1",
            "Savepoint": "unk5 sits at 0x84c",
            "SoundTable": "unk19 sits at 0x13",
            "Spyro": "unk3 sits at 0x1c",
        },
        "comment_offsets": True,
        "bss_globals": True,
        "known_slack": {
            # PsyqPad is the 8 bytes pad.h models; the .bss reserves 0x24,
            # annotated "7 ints after this?" - the rest of the libpad
            # buffer, which the decomp has not typed.
            "D_800718DC": 0x1C,
        },
        "pins": SPYRO3_PINS,
    },
}


def config(game: str):
    if game not in GAMES:
        raise SystemExit(f"[game-structs] ERROR: no struct config for game "
                         f"{game!r} (have {sorted(GAMES)})")
    return GAMES[game]
