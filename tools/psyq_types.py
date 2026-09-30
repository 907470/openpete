"""psyq_types.py — the PSYQ SDK types the decomp's game structs embed.

One definition serves two consumers: probe_struct_layout.py writes these
as the stub PSYQ headers for its MIPS compile of the original decomp
headers, and gen_game_structs.py emits them into openpete_game_structs.h
ahead of the game structs. Sharing the text is what makes the probe's
pins apply to the SDK header.

Layouts: RECT, DR_ENV, DRAWENV, DISPENV from games/spyro-3/spyro-3/psyq/
include/libgpu.h (a PSYQ header); MATRIX from third_party/PsyCross/
include/psx/libgte.h; SpuVolume, SpuExtAttr, SpuCommonAttr from
libspu.h; CdlLOC, CdlATV, CdlFILE from libcd.h. Every struct carries its
tag so a typedef completes the forward declarations the wrappers header
emits (typedef struct PSX_MATRIX PSX_MATRIX; ...).

The text spells the PSYQ names, which is what the MIPS probe's stub
headers must carry. The SDK-facing emitters rewrite them through
psx_prefixed(): <windows.h> defines RECT, so a mod including both it and
the SDK header would see two definitions of the name.

Types are written with the PSX width names (u_char, u_short, u_long) so
the same text compiles under the MIPS probe and, after the generator's
width rewrite, on the host.
"""

# (stub header name, C text). Order is emission order (leaf types first).
PSYQ_TYPES = [
    ("libgte.h", """\
typedef struct MATRIX {
  short m[3][3]; /**< 3x3 rotation matrix. */
  long t[3];     /**< Transfer vector. */
} MATRIX;
"""),
    ("libgpu.h", """\
typedef struct RECT {
  short x, y; /**< Offset point on VRAM. */
  short w, h; /**< Width and height. */
} RECT;

typedef struct DR_ENV {
  u_long tag;
  u_long code[15];
} DR_ENV;

typedef struct DRAWENV {
  RECT clip;         /**< Clip area. */
  short ofs[2];      /**< Drawing offset. */
  RECT tw;           /**< Texture window. */
  u_short tpage;     /**< Texture page. */
  u_char dtd;        /**< Dither flag (0:off, 1:on). */
  u_char dfe;        /**< Draw on the display area (0:off, 1:on). */
  u_char isbg;       /**< Auto-clear enable. */
  u_char r0, g0, b0; /**< Initial background colour. */
  DR_ENV dr_env;     /**< Packed drawing environment. */
} DRAWENV;

typedef struct DISPENV {
  RECT disp;         /**< Display area. */
  RECT screen;       /**< Display start point. */
  u_char isinter;    /**< Interlace 0:off, 1:on. */
  u_char isrgb24;    /**< RGB24 mode. */
  u_char pad0, pad1;
} DISPENV;
"""),
    ("libspu.h", """\
typedef struct SpuVolume {
  short left;  /**< Lch. */
  short right; /**< Rch. */
} SpuVolume;

typedef struct SpuExtAttr {
  SpuVolume volume;
  int reverb;
  int mix;
} SpuExtAttr;

typedef struct SpuCommonAttr {
  u_int mask;
  SpuVolume mvol;
  SpuVolume mvolmode;
  SpuVolume mvolx;
  SpuExtAttr cd;
  SpuExtAttr ext;
} SpuCommonAttr;
"""),
    ("libcd.h", """\
typedef struct CdlLOC {
  u_char minute; /**< Minute (BCD). */
  u_char second; /**< Second (BCD). */
  u_char sector; /**< Sector (BCD). */
  u_char track;
} CdlLOC;

typedef struct CdlATV {
  u_char val0; /**< Volume CD(L) -> SPU(L). */
  u_char val1; /**< Volume CD(L) -> SPU(R). */
  u_char val2; /**< Volume CD(R) -> SPU(L). */
  u_char val3; /**< Volume CD(R) -> SPU(R). */
} CdlATV;

typedef struct CdlFILE {
  CdlLOC pos;    /**< File location. */
  u_int size;    /**< File size. */
  char name[16]; /**< File name. */
} CdlFILE;
"""),
]

# Headers the decomp includes that carry nothing the structs need.
EMPTY_STUBS = ["libetc.h"]

SYS_TYPES_H = """\
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned int uint;
typedef unsigned short ushort;
"""

# Layout facts the probe re-derives and asserts.
DRAWENV_SIZE = 0x5C
DISPENV_SIZE = 0x14
DRAWENV_R0_OFFSET = 0x19   # the credits overlay stores r0 at g_DB + 0x19


def psyq_types(stubs=None):
    """(stub header name, C text) for the stub set a game includes, in
    emission order; every stub when `stubs` is None."""
    return [(n, t) for n, t in PSYQ_TYPES if stubs is None or n in stubs]


def psyq_names(stubs=None):
    import re
    return [m for _, text in psyq_types(stubs)
            for m in re.findall(r"^\} (\w+);", text, re.M)]


def psyq_text(stubs=None):
    return "\n".join(text for _, text in psyq_types(stubs))


# The prefix the SDK-facing spelling of every PSYQ type carries.
PSX_PREFIX = "PSX_"


def psx_prefixed(text, names=None):
    """`text` with every whole-word occurrence of a PSYQ type name under
    the PSX_ prefix. `names` restricts the rewrite to one game's stub set;
    None covers every PSYQ type."""
    import re
    names = psyq_names() if names is None else list(names)
    if not names:
        return text
    rx = re.compile(r"\b(" + "|".join(sorted(names, key=len, reverse=True))
                    + r")\b")
    return rx.sub(lambda m: PSX_PREFIX + m.group(1), text)
