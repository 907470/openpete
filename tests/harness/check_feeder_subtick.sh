#!/usr/bin/env bash
# check_feeder_subtick.sh - sub-tick interpolation of a self-linking feeder,
# measured as the screen position of the pixels it paints. Two render paths use
# it: the gem glint (check_glint_subtick.sh) and the glow ring
# (check_glow_subtick.sh), which is the form each wrapper names.
#
# Each of these routines links its own primitives into the 3D ordering table
# and the feeder reads them back out of the polybuf, so what a sub-tick present
# has to work with is the guest's own int16 screen corners, built under the
# tick's camera from the tick's pool record. A replay that re-emits them
# verbatim draws the blob at the pixels the tick drew for the whole interior of
# the tick, and the next tick moves it the whole way at once: the blob sits
# still while the world around it slides, then jumps.
#
# The defect class is a sub-tick producer that holds its tick pose. The
# measurement is where the blob is on screen, per present. The instrument fits
# a compact blob that translates; a producer whose painted extent changes with
# its own state needs its own harness, which is why check_zap_subtick.sh is a
# sibling rather than a path here.
#
# A path is named by --path and carries: what suppresses its feed for the
# leave-one-out, the manifest it reads and the alphas it judges.
#   glint   OPENPETE_GLINT=2 against =0, games/<slug>/corpus/glint-subtick-corpus.manifest;
#           the centroid judged is the traced outline centroid of the record
#           each gem names (`rec=`), the mask centroid reported only
#   glint-ext  OPENPETE_GLINT_TRACE=1, one leg per alpha, no leave-one-out, the
#           ext statistic; OPENPETE_GLINT_EXTENT=0 is the engagement leg;
#           games/<slug>/corpus/glint-ext-subtick-corpus.manifest
#   glint-rot  OPENPETE_GLINT_TRACE=1, as glint-ext, the rot statistic;
#           OPENPETE_GLINT_SPIN=0 is the engagement leg;
#           games/<slug>/corpus/glint-rot-subtick-corpus.manifest
#   glow    OPENPETE_GLOW=1  against =0, games/<slug>/corpus/glow-subtick-corpus.manifest
#   shadow  --render-paths terrain,moby,shadow,moby_shadow against terrain,moby,
#           games/<slug>/corpus/shadow-subtick-corpus.manifest
#   portal  OPENPETE_PORTAL3_DBG=1, one leg per alpha, no leave-one-out,
#           games/<slug>/corpus/portal-subtick-corpus.manifest
#   cyclorama  --render-paths cyclorama,terrain against terrain
#   flame      --render-paths terrain,moby,flame against terrain,moby
#   particles  --render-paths terrain,moby,particles against terrain,moby
#   tracer     --render-paths terrain,moby,tracer against terrain,moby;
#              OPENPETE_TRACERS_INTERP=0 is the engagement leg
#   moby-mask  --render-paths terrain,moby against terrain: the moby's painted
#              extent and colour, which check_moby_subtick.sh does not separate
#   terrain        OPENPETE_TERRAIN_VANIM=1 against =0, the interp statistic
#   terrain-tanim  OPENPETE_TERRAIN_TANIM=1 against =0, the interp statistic;
#                  both read games/<slug>/corpus/terrain-subtick-corpus.manifest
#   terrain-canim  OPENPETE_TERRAIN_CANIM=1 against =0, the interp statistic
#                  referenced at alpha 1, games/<slug>/corpus/terrain-canim-subtick-corpus.manifest
#   terrain-vis    --terrain-visibility game against frustum, the vis statistic,
#                  games/<slug>/corpus/terrain-vis-subtick-corpus.manifest
#
# The vis statistic (STAT=vis) prices the sector set a sub-tick present draws
# under the game's own visibility verdict. Its legs are the intermediate
# present at V+1, per alpha, under --terrain-visibility game and under
# frustum, which admits every sector whose sphere the present's camera sees.
# The difference of the two is the pixels of sectors one mode draws and the
# other does not; the game's occlusion groups hide only sectors the depth test
# would hide anyway, so on a row where frustum mode draws nothing the game
# hides, the legs must be equal at every alpha. This inverts the interp
# statistic's rule that alpha 0 must differ. A verdict that keeps the tick's
# frustum term drops at alpha 0 the sectors the tick's cone rejected and the
# present's camera sees. Engagement is read from the game leg's own census
# (OPENPETE_TERRAIN_BINCHECK at V+1): the `retest=` count of the sub-tick
# walk, the sectors the present-camera re-test admitted beyond the tick's
# verdict, summed over the alpha-0 leg. Pass: every alpha 0 px, engagement
# above 0; a row with 0 px everywhere and no re-tested sector is VACUOUS.
#
# The interp statistic (STAT=interp) is for a knob that suppresses a channel's
# sub-tick interpolation rather than its feed. The canonical presents are then
# identical between the legs, so the legs are intermediate presents alone and
# their difference is the pixels that interpolation moved. Read per cell: the
# count must be zero at alpha 1 (every term of a correct rebase is zero at the
# tick's own instant), above the floor at alpha 0 (the knob moved something
# this cell can see), and a channel that holds its tick state renders the same
# present in both legs at every alpha and the count collapses to zero.
#
# terrain-canim is the interp statistic referenced at alpha 1 (INTERP_REF=1).
# Its knob at 0 does not snap the channel to the newest tick: it holds the
# env-anim vertex colours at the previous tick's words at every alpha, which is
# the lerp's alpha-0 end. The legs then differ by alpha of the colour step, so
# the roles of the ends swap: alpha 0 is the identity leg (no pixel may
# differ), alpha 1 the engagement leg (the whole step, above the floor). A
# colour lerp changes how much each pixel differs rather than how many differ,
# so the cell's statistic is the summed per-pixel mean channel difference S(a):
# the step is S(1) over the alpha-1 pixel count in levels, a lerped channel
# reads S(a) = a*S(1) and a channel holding the newest tick reads S(a) = S(1).
# The residual |S(a) - a*S(1)| over the hold distance (1-a)*S(1) is the ratio
# the mask statistic classifies by: LERPED at or below 0.25, SNAP at or above
# 0.75. `own` is S(0) in levels, the alpha-0 distance from the previous tick.
# Pass: alpha 0 differs in no pixel, `own` under 10% of the step, and every
# judged interior alpha LERPED within `ctol` levels of the lerp (default 1.5).
#
# Row authoring: --row '<bk2>|<V>|<keys>' runs one row from the command line
# instead of the manifest, and OPENPETE_FEEDER_PROBE=1 with it captures the two
# canonical presents alone and prints every mask component with its centroid,
# area and mean colour, so a row's blob coordinates come from a measurement.
#
# The portal path is the homeworld portal preview of Spyro 2 and Spyro 3, and
# it takes the other statistic this script carries. What moves there is the
# aperture the preview is seen through: a screen polygon spanning most of the
# frame, not a compact blob, so the pixel mask below does not model it and the
# MASK_CAP would refuse it. Its statistic is instead the aperture polygon's
# vertices, which the feeder emits exactly under OPENPETE_PORTAL3_DBG, one
# `[portal3-ap]` line per fed preview per present, with the canonical pass and
# the sub-tick replay distinguished by `src=`. The vertices are the feeder's
# own floats, so no leave-one-out and no clustering are needed: one run per
# alpha carries the two canonical ticks and the intermediate present together.
# `--render-paths` cannot isolate this path on its own - the ordering-table
# filter drops the portal prims before the drawing-primitive count, so fast
# boot never ends and no present is reached - and the vertex list is the
# isolation instead. The engagement counter is the fed triangle count per leg.
#
# The glint-ext path takes the third statistic: the extent each glint record
# was fed at, the mean distance of its outline corners (a star quad's three
# rim corners, a cross line's two endpoints) from the record's projected
# point, which the feeder prints per record per present under
# OPENPETE_GLINT_TRACE as `[glint-ext vbl=<v> leg=canon|inter alpha=<a>]
# rec=<addr> ext=<px> n=<corners>`, in PSX screen pixels. A mask cannot price
# it: a glint paints 7-25 px, so its pixel count moves in whole pixels and its
# extent step is a fraction of one. Per named record (`rec=<addr>`,
# repeatable) the cell is the area cell's: the residual from the lerp of the
# two canonical extents over the distance a held extent stands from that
# lerp, LERPED at or below 0.25, SNAP at or above 0.75; `own` is the alpha-0
# extent's distance from canon(V-2). The bound is `rtol` (default 0.25) on the
# judged alphas; alpha 1 must print canon(V) exactly. `step` is the floor on
# the record's canonical extent step (default 0.5 px). The engagement leg is
# the alpha-0 present under OPENPETE_GLINT_EXTENT=0, where at least one named
# record must hold its tick extent (own at least 0.75 of its step).
#
# The glint-rot path takes the fourth statistic: the drawn orientation of each
# glint record's outline, `[glint-rot vbl=<v> leg=canon|inter alpha=<a>]
# rec=<addr> ori=<e> ph=<e>`, in sine-table entries (256 to the turn) modulo
# 64, since the star's spikes and the cross's arms repeat every quarter turn.
# `ori` is read off the fed corners (the argument of their summed fourth-power
# offset, over four), so it prices what the feed drew; `ph` is the phase the
# feed asked for and is reported alone. Differences are wrapped to +/-32. The
# cell, keys, identity leg and engagement leg (OPENPETE_GLINT_SPIN=0) are the
# glint-ext path's; `step` is the floor on the canonical orientation step in
# entries (default 1.0).
#
# The shadow path is the moby ground shadow, which no env knob switches off on
# its own, so its two legs are a render-path selection with and without the
# shadow path. Both legs run under a selection, so everything else on screen is
# identical between them, and the terrain and moby paths stay in because the
# shadow is a depth-aware decal tested against the world depth pre-pass.
# OPENPETE_SHADOW_PLAYER=0 on both legs drops the player's own fan, leaving the
# moby shadows as the only thing the difference can contain. The shadow is not
# a self-linking feeder replayed out of the polybuf: it is re-derived from
# gamestate every present, and what this harness measures there is whether that
# derivation reads the pose the present shows or the pose the newest tick left.
#
# Instrument: leave-one-out, not colour. Every leg is captured twice
# from one binary, the path's knob at its on value against 0 (the feed
# suppressed, the guest draw unchanged), and the pixels that differ are exactly
# the ones this feeder painted. Those pixels are clustered at a CLUSTER_R
# Chebyshev radius into components, one per blob on screen, and each
# component's centroid is its position. The component pixel count is the
# engagement counter: a leg whose mask is empty measured nothing.
#
# A sub-tick present replays the set the most recent canonical tick captured
# and shows it between the pose of the tick before (alpha 0) and the pose that
# tick drew (alpha 1). The present at VBL V+1 therefore covers the interval the
# two ticks at V-2 and V drew, so three captures per row, all from the same
# replay of one movie:
#   canon(V-2), canon(V)   the two canonical ticks, the interval's ends
#   inter(V+1, alpha)      one intermediate present over that interval
# A blob's expected position there is (1-alpha)*canon(V-2) + alpha*canon(V): it
# is on the world's own straight line between the two poses the ticks drew,
# because the camera transform the rest of the pass draws under is itself
# lerped by alpha.
#
# alpha 1 is the identity leg, where the expected position is canon(V)
# exactly: every term of a correct rebase is zero at the end of the tick's own
# interval, so the replay must reproduce the canonical present. alpha 0 is the
# leg that prices the defect: a producer that holds its tick pose reads canon(V)
# there, one whole tick step from where it belongs.
#
# Three statistics per named blob, one cell each, the census this harness
# reports:
#   centroid  the mask component's centroid, in screen pixels: translation
#   area      the component's pixel count: scale, view-Z and extent
#   colour    the mean feed-on RGB of the component's pixels, widest channel
#             of 0-255: colour, fade and opacity
# Each is compared with the lerp of the same statistic at canon(V-2) and
# canon(V). A producer that holds its tick state shows the canon(V) value,
# which sits (1-alpha) of the canonical step from that lerp, so the ratio of
# the measured residual to that distance classifies every statistic alike:
#   LERPED   ratio at or below 0.25
#   PARTIAL  between
#   SNAP     ratio at or above 0.75
#   UNMOVED  the statistic's canonical step is below its own floor, so no
#            reading of it separates a lerp from a hold
# The centroid keeps the `tol` bound, which is what decides PASS or FAIL; the
# area and colour cells assert only where the row sets `atol` or `ctol`.
#
# The colour cell of a translating additive blob reads its own colour blended
# over whatever it covers, which changes as it moves: a colour verdict is
# read with the area cell beside it, and a colour SNAP under an area SNAP is
# the extent's reading, not the colour's.
#
# Three statistics per named blob per row, all in screen pixels:
#   resid  the judged one: the distance from the shown centroid to the expected
#          (1-alpha)*canon(V-2) + alpha*canon(V), at each judged alpha
#   ident  resid at alpha 1, where the expected position is canon(V) exactly
#   own    |c(alpha 0) - canon(V-2)|, how far the alpha-0 leg stands from the
#          pose the previous tick drew: a producer that carries its own motion
#          reads near zero there, one that holds its tick pose reads the whole
#          canonical step
# The component's pixel count is printed beside every centroid. A producer
# whose painted extent changes with its own state ramps that count across the
# interval and moves the visible centroid with it, which is the residual's
# floor and the reason a row sets `tol` above the instrument's own noise.
#
# A path with a suppress knob also runs an engagement leg: the alpha-0
# present captured again with the knob at 0, where the producer must hold its
# tick pose. At least one of an `engage=1` row's blobs must read `own` of at
# least 0.75 of its canonical step there, the SNAP bound of the cells: a
# replay of the tick's own screen corners still sits under the interpolated
# camera, so the terrain and mobys that cover part of the blob move and its
# visible centroid lands short of canon(V) by the occlusion's shift. With no
# such blob the knob reached nothing this row can see, the normal legs prove
# nothing, and the row is VACUOUS. One blob
# suffices because a row may name a control blob the knob cannot move - a
# producer already at rest in the world, whose whole step is the camera's.
#
# alpha 0 is captured and reported, not judged. The replay is a screen-space
# rebase of the tick's own primitives, so at alpha 0 it shows the tick's blob -
# its size and its outline, which each routine derives from its record's own
# state - standing where the tick before drew its own, slightly different,
# blob. The two masks differ by a pixel or two and the centroid follows, which
# measures that state's ramp rather than the rebase. Reproducing the previous
# tick's blob outright needs a second copy of the routine's geometry, which
# these feeders deliberately do not carry.
#
# Input: the path's manifest, rows
#   <source> | <V> | <key>=<value> ...
# <source> is a movie under games/<slug>/bk2, or any name not ending in .bk2,
# which is the plain boot - the title screen and the attract demo, which need
# no input - replayed with no movie, the same movie-less row form the
# presentation corpus takes. A movie-less row may name `level=<name>` (booted
# with --skip-to-level) and `pose=<x>,<y>,<z>,<yaw>,<pitch>` (a --freecam-pose
# held once the level is live), which is how a row reaches a level no movie in
# the corpus visits.
# V is the canonical VBL whose primitives the intermediate present replays;
# V-2 opens the interval and the intermediate present is captured at V+1. A
# tick spans two VBLs, so V must be the first VBL of its tick: V+1 otherwise
# falls in the next tick, where the present replays that tick's set over the
# interval above V and the alpha-1 identity leg misses canon(V) by a whole tick
# step. The alpha-1 leg is what catches a row of the wrong parity.  Keys:
#   blob=<x>,<y> a blob to judge, named by its mask component's centroid in the
#                canonical capture at V (`gem=` is the same key, the glint
#                manifests' spelling; `vertex=` the portal manifests', where
#                the position is an aperture polygon vertex rather than a
#                centroid). Repeatable; at least one required.
#                A row names the blobs unoccluded over the interval: a partly
#                covered blob's visible centroid is not the interpolation of
#                its two visible centroids, so judging it would measure the
#                depth test, not the rebase.
#   tol=<px>     centroid bound, default 0.5
#   judge=<a>[,<a>...]
#                the alphas the bound applies to, default the path's; every
#                captured alpha is reported either way
#   step=<px>    floor on the blob's canonical motion over the interval. A blob
#                that does not move between the two ticks passes every alpha
#                whatever the producer does, so a row below this floor is
#                VACUOUS rather than a pass. Default 4.
#   engage=0     this row runs no code the path's suppress knob reaches, so the
#                engagement leg below is captured and reported but asserts
#                nothing. It is how a control row on a game that takes another
#                code path for the same producer is declared. Default 1.
#
# Pass criterion, per row: every named blob, at every judged alpha, within
# `tol` of its expected centroid. VACUOUS (exit 1) when a leg's mask is empty,
# when a named blob resolves to no component or to an ambiguous one in some
# leg, when a blob's canonical step is below the floor, or when the path has a
# suppress knob and the engagement leg's `own` is below that step.
#
# Verdict line per row:
#   [<path>-subtick] game=<g> bk2=<b> V=<v> blobs=<n> legs=<n> maskpx=<n>
#                    worst=<px> verdict=PASS|FAIL
#
# Usage: tests/harness/check_feeder_subtick.sh
#          --path glint|glint-ext|glint-rot|glow|shadow|portal|terrain|terrain-tanim|
#                 terrain-canim|terrain-vis|cyclorama|particles|flame|tracer|moby-mask
#          [--game SLUG] [--build-dir=DIR] [--row '<bk2>|<V>|<keys>']
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (binary, disc, bk2, python3 or PIL missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

PATHNAME=glint
ROW=
prev_arg=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --path=*)      PATHNAME="${a#--path=}";;
        --row=*)       ROW="${a#--row=}";;
    esac
    [[ "$prev_arg" == "--path" ]] && PATHNAME="$a"
    [[ "$prev_arg" == "--row" ]] && ROW="$a"
    prev_arg="$a"
done

# Per render path: the knob that suppresses the feed, the value that turns it
# on, and the alphas whose residual the bound applies to. alpha 1 is judged on
# every path: it is the identity leg.
# LEG_ENV_ON/OFF and LEG_ARGS_ON/OFF are what the two leave-one-out legs
# differ by, plus whatever both of them need; SUPPRESS names the knob the
# engagement leg sets to 0, empty on a path that has none.
# STAT names the statistic source: `mask` is the leave-one-out mask, whose
# per-blob centroid, pixel count and mean colour are the three statistics;
# `vertex` is the feeder's own emitted polygon vertices.
# MASK_CAP is the largest mask a leg of this path may produce; above it the
# leave-one-out isolated something other than this feeder and the statistics
# would be meaningless. A path whose feed paints a surface rather than a blob
# carries a cap sized for that surface.
LEG_ENV_ON=(); LEG_ENV_OFF=(); LEG_ARGS_ON=(); LEG_ARGS_OFF=(); SUPPRESS=; STAT=mask
INTERP_REF=0
GEOM=0
MASK_CAP=20000
case "$PATHNAME" in
    # The glint's centroid cell reads the feeder's traced outline centroid
    # (GEOM=1, below) rather than the mask's, so the feed-on legs trace.
    glint)  LEG_ENV_ON=(OPENPETE_GLINT=2 OPENPETE_GLINT_TRACE=1); LEG_ENV_OFF=(OPENPETE_GLINT=0)
            GEOM=1
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    glow)   LEG_ENV_ON=(OPENPETE_GLOW=1); LEG_ENV_OFF=(OPENPETE_GLOW=0)
            ALPHAS=(0 0.25 0.5 0.75 1); JUDGE_DEF=0.25,0.5,0.75,1;;
    # The gem glint's extent, read from the feeder's own trace rather than a
    # mask: a star or cross is 7-25 px painted, so its mask's pixel count steps
    # by whole pixels and the area cell reads UNMOVED on every glint row. One
    # run per alpha, no leave-one-out, as the portal path does.
    glint-ext) LEG_ENV_ON=(OPENPETE_GLINT_TRACE=1)
            SUPPRESS=OPENPETE_GLINT_EXTENT; STAT=ext
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # The gem glint's spin, read from the same trace as its extent.
    glint-rot) LEG_ENV_ON=(OPENPETE_GLINT_TRACE=1)
            SUPPRESS=OPENPETE_GLINT_SPIN; STAT=rot
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # No suppress knob: the feeder re-derives each opening from its portal
    # record under the present's own view (p3_resolve), so the polygon is a
    # producer at rest in the world whose whole step is the camera's, and
    # OPENPETE_PORTAL3_INTERP, which lerps the captured previews, cannot move
    # it. The step floor and the alpha-0.5 residual are the engagement proof.
    portal) LEG_ENV_ON=(OPENPETE_PORTAL3_DBG=1)
            STAT=vertex
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    shadow) LEG_ENV_ON=(OPENPETE_SHADOW_PLAYER=0)
            LEG_ENV_OFF=(OPENPETE_SHADOW_PLAYER=0)
            LEG_ARGS_ON=(--render-paths terrain,moby,shadow,moby_shadow)
            LEG_ARGS_OFF=(--render-paths terrain,moby)
            SUPPRESS=OPENPETE_SHADOW_INTERP
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # The terrain paths' own animation knobs are the leave-one-out: with
    # OPENPETE_TERRAIN_VANIM=0 the sector's vertices stand at their rest
    # positions and with OPENPETE_TERRAIN_TANIM=0 its UVs stop scrolling, so
    # the difference is exactly the pixels the animated terrain painted. Both
    # legs select the terrain path alone, so nothing else on screen can enter
    # the difference, and the mask is a water or lava surface rather than a
    # blob, which is what the cap allows for.
    terrain) LEG_ENV_ON=(OPENPETE_TERRAIN_VANIM=1); LEG_ENV_OFF=(OPENPETE_TERRAIN_VANIM=0)
            LEG_ARGS_ON=(--render-paths terrain)
            LEG_ARGS_OFF=(--render-paths terrain)
            STAT=interp; MASK_CAP=320000
            ALPHAS=(0 0.25 0.5 0.75 1); JUDGE_DEF=0.5,1;;
    terrain-tanim) LEG_ENV_ON=(OPENPETE_TERRAIN_TANIM=1); LEG_ENV_OFF=(OPENPETE_TERRAIN_TANIM=0)
            LEG_ARGS_ON=(--render-paths terrain)
            LEG_ARGS_OFF=(--render-paths terrain)
            STAT=interp; MASK_CAP=320000
            ALPHAS=(0 0.25 0.5 0.75 1); JUDGE_DEF=0.5,1;;
    terrain-canim) LEG_ENV_ON=(OPENPETE_TERRAIN_CANIM=1); LEG_ENV_OFF=(OPENPETE_TERRAIN_CANIM=0)
            LEG_ARGS_ON=(--render-paths terrain)
            LEG_ARGS_OFF=(--render-paths terrain)
            STAT=interp; INTERP_REF=1; MASK_CAP=320000
            ALPHAS=(0 0.25 0.5 0.75 1); JUDGE_DEF=0,0.5;;
    # The game's visibility verdict against the frustum candidate set, every
    # path drawn in both legs: what differs is the sector set alone.
    terrain-vis) LEG_ARGS_ON=(--terrain-visibility game)
            LEG_ARGS_OFF=(--terrain-visibility frustum)
            STAT=vis; MASK_CAP=320000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0,0.5,1;;
    # The cyclorama has no suppress knob; the two legs are a render-path
    # selection with and without it, terrain in both so the horizon the
    # cyclorama is seen against is identical between them. The feed covers the
    # sky, so the cap is sized for a surface.
    cyclorama) LEG_ARGS_ON=(--render-paths cyclorama,terrain)
            LEG_ARGS_OFF=(--render-paths terrain)
            MASK_CAP=320000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # Particles, tracers and the flame are isolated by a render-path selection
    # rather than by their own feed knobs: Spyro 1 has its own particle and
    # tracer renderers, which those knobs do not reach, and the selection means
    # the same thing on all three games. Terrain and moby stay in both legs so
    # the world the effect is drawn against is identical between them.
    particles) LEG_ARGS_ON=(--render-paths terrain,moby,particles)
            LEG_ARGS_OFF=(--render-paths terrain,moby)
            MASK_CAP=60000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # The flame has no suppress knob either. Its ribbon is drawn against the
    # world it lights, so both legs keep terrain and moby.
    flame)  LEG_ARGS_ON=(--render-paths terrain,moby,flame)
            LEG_ARGS_OFF=(--render-paths terrain,moby)
            MASK_CAP=60000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    tracer) LEG_ARGS_ON=(--render-paths terrain,moby,tracer)
            LEG_ARGS_OFF=(--render-paths terrain,moby)
            SUPPRESS=OPENPETE_TRACERS_INTERP
            MASK_CAP=60000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    # Spyro 1's moby scale and colour cells, which the moby path's own harness
    # (check_moby_subtick.sh) does not separate: the mask's pixel count is the
    # scale channel and its mean colour the colour channel, both read off the
    # one moby the row names. Both legs select the moby path alone so the
    # terrain behind it cannot enter the difference.
    moby-mask) LEG_ARGS_ON=(--render-paths terrain,moby)
            LEG_ARGS_OFF=(--render-paths terrain)
            MASK_CAP=200000
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    *)      echo "feeder-subtick: FAIL (unknown --path $PATHNAME)"; exit 1;;
esac
TAG="$PATHNAME-subtick"

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# --row runs one row given on the command line instead of the manifest, which
# is how a row is authored: with OPENPETE_FEEDER_PROBE=1 the run captures the
# two canonical presents alone and prints every mask component with the
# statistics a row can name, so the blob coordinates of a new row come from a
# measurement rather than a guess.
# A path's own manifest comes first; a path that animates the same surfaces
# as its base path (the -tanim suffix) falls back to the base manifest. The
# two terrain knobs animate different surfaces, so a row that engages one
# can be vacuous under the other and the UV-scroll path keeps its own rows.
MANIFEST="$(pg_corpus_manifest "$GAME" "${PATHNAME}-subtick-corpus.manifest")"
[[ -f "$MANIFEST" ]] ||
    MANIFEST="$(pg_corpus_manifest "$GAME" "${PATHNAME%-tanim}-subtick-corpus.manifest")"
if [[ -n "$ROW" ]]; then
    MANIFEST="$(mktemp)"
    trap 'rm -f "$MANIFEST"' EXIT
    printf '%s\n' "$ROW" >"$MANIFEST"
fi
pg_manifest "$MANIFEST" line
if [[ "${OPENPETE_FEEDER_PROBE:-0}" != 0 ]]; then ALPHAS=(); fi
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "$TAG: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "$TAG: SKIP (python3 or PIL missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/$TAG/$GAME"
mkdir -p "$OUT"

# One replay of a row's movie with one glint setting, capturing either the two
# canonical ticks or one intermediate present. The screenshot request is what
# makes the native renderer run and the intermediate present exist at all under
# --headless, so every leg carries one.
run_leg() {
    local tag="$1" leg="$2" alpha="$3" out="$4"; shift 4
    local log="$OUT/$tag.log"
    local -a legargs=()
    if [[ "$leg" == on ]]; then
        PG_CAPTURE_ENV=(${LEG_ENV_ON[@]+"${LEG_ENV_ON[@]}"})
        legargs=(${LEG_ARGS_ON[@]+"${LEG_ARGS_ON[@]}"})
    else
        PG_CAPTURE_ENV=(${LEG_ENV_OFF[@]+"${LEG_ENV_OFF[@]}"})
        legargs=(${LEG_ARGS_OFF[@]+"${LEG_ARGS_OFF[@]}"})
    fi
    [[ -n "$SUPPRESSED" ]] && PG_CAPTURE_ENV+=("$SUPPRESS=0")
    # The census reports from the interval's opening tick on, so a leg's log
    # holds canon(V-2), canon(V) and the present between them.
    [[ "$STAT" == vertex ]] && PG_CAPTURE_ENV+=(OPENPETE_PORTAL3_DBG_FROM="$prev")
    [[ "$STAT" == vis ]] && PG_CAPTURE_ENV+=(OPENPETE_TERRAIN_BINCHECK="$mid")
    [[ -n "$alpha" ]] && PG_CAPTURE_ENV+=(OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$out" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original \
        "${MOVIE_ARG[@]}" --screenshot-native-only \
        ${legargs[@]+"${legargs[@]}"} \
        "$@" --screenshot-output "$OUT/$tag/{n}.png" \
        --frame-limit $((V + 11))
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r bk2 vfield keys <<<"$line"
    bk2="${bk2// /}"; V="${vfield// /}"
    # A row whose first field does not end in .bk2 is the plain boot, which
    # needs no input and replays with no movie.
    if [[ "$bk2" == *.bk2 ]]; then
        MOVIE="$PG_BK2_DIR/$bk2"
        [[ -f "$MOVIE" ]] || { echo "$TAG: SKIP ($MOVIE missing)"; exit 64; }
        MOVIE_ARG=(--bk2 "$MOVIE")
    else
        MOVIE_ARG=()
        for kv in $keys; do
            case "$kv" in
                level=*) MOVIE_ARG+=(--skip-to-level "${kv#level=}");;
                pose=*)  IFS=',' read -r px py pz pyaw ppitch <<<"${kv#pose=}"
                         MOVIE_ARG+=(--freecam-pose
                                     "pos=($px, $py, $pz) yaw=$pyaw pitch=$ppitch");;
            esac
        done
    fi
    rows=$((rows + 1))
    # The capture directories carry V as well as the movie: a manifest may hold
    # two rows on one bk2, and a shared directory would leave the first row's
    # captures for the second row to clear.
    tag="${bk2%.bk2}-$V"

    if (( V < 4 )); then
        echo "$TAG: FAIL (row $bk2: V=$V has no interval below it)"; exit 1
    fi
    prev=$((V - 2)) mid=$((V + 1))

    # Every capture directory and log is cleared before the run and every
    # output asserted after it, so a stale PNG cannot be read as this run's
    # result.
    rc=0 shots=()
    SUPPRESSED=
    if [[ "$STAT" == interp || "$STAT" == vis ]]; then
        # The knob this path toggles suppresses the sub-tick interpolation of
        # one channel, not the feed: with it at 0 the canonical presents are
        # identical between the legs and only the intermediate present differs.
        # So the legs are intermediate presents alone, and what the difference
        # of the two contains is exactly the pixels this channel's
        # interpolation moved.
        for alpha in ${ALPHAS[@]+"${ALPHAS[@]}"}; do
            for lbl in on off; do
                d="$tag-i$alpha-$lbl"
                rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
                run_leg "$d" "$lbl" "$alpha" "$OUT/$d/$mid-vk.png" \
                        --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
                shots+=("$OUT/$d/$mid-vk.png")
                [[ "$STAT" == vis ]] && shots+=("$OUT/$d.log")
            done
        done
    elif [[ "$STAT" == vertex || "$STAT" == ext || "$STAT" == rot ]]; then
        # One run per alpha. The feeder's census in that run's log already
        # carries canon(V-2), canon(V) and the intermediate present, so the
        # separate canonical legs and the leave-one-out partner of the mask
        # statistic are not captured at all.
        for alpha in ${ALPHAS[@]+"${ALPHAS[@]}"}; do
            d="$tag-i$alpha"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" on "$alpha" "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png" "$OUT/$d.log")
        done
        SUPPRESSED=1
        d="$tag-s"
        rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
        run_leg "$d" on 0 "$OUT/$d/$mid-vk.png" \
                --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
        shots+=("$OUT/$d/$mid-vk.png" "$OUT/$d.log")
        SUPPRESSED=
    else
    for lbl in on off; do
        d="$tag-c-$lbl"
        rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
        run_leg "$d" "$lbl" "" "$OUT/$d/$prev-vk.png,$OUT/$d/$V-vk.png" \
                --screenshot-on-frame "$prev,$V" </dev/null || rc=$?
        shots+=("$OUT/$d/$prev-vk.png" "$OUT/$d/$V-vk.png")
        for alpha in ${ALPHAS[@]+"${ALPHAS[@]}"}; do
            d="$tag-i$alpha-$lbl"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" "$lbl" "$alpha" "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png")
        done
    done
    # The engagement leg: the alpha-0 present again with the path's own
    # interpolation suppressed, where the producer must hold its tick pose.
    if [[ -n "$SUPPRESS" && ${#ALPHAS[@]} -gt 0 ]]; then
        SUPPRESSED=1
        for lbl in on off; do
            d="$tag-s-$lbl"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" "$lbl" 0 "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png")
        done
        SUPPRESSED=
    fi
    fi
    missing=0
    for f in "${shots[@]}"; do [[ -s "$f" ]] || missing=1; done
    if (( missing || rc )); then
        echo "$TAG: FAIL (row $bk2: capture incomplete, rc=$rc; see $OUT)"
        fail=1; continue
    fi

    if [[ "$STAT" == vis ]]; then
    python3 - "$TAG" "$GAME" "$bk2" "$V" "$OUT/$tag" "$mid" "$MASK_CAP" \
             ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import re
import sys
from PIL import Image

tag, game, bk2, V, base, mid, mask_cap = sys.argv[1:8]
alphas = sys.argv[8:]
V, mid, mask_cap = int(V), int(mid), int(mask_cap)


def diffpx(a_path, b_path):
    """Pixels that differ between the game-verdict and frustum legs."""
    ia, ib = Image.open(a_path).convert("RGB"), Image.open(b_path).convert("RGB")
    if ia.size != ib.size:
        raise SystemExit("[%s] %s and %s differ in size" % (tag, a_path, b_path))
    pa, pb = ia.load(), ib.load()
    w, h = ia.size
    n, bb = 0, None
    for y in range(h):
        for x in range(w):
            if pa[x, y] != pb[x, y]:
                n += 1
                bb = (x, y, x, y) if bb is None else (min(bb[0], x), min(bb[1], y),
                                                      max(bb[2], x), max(bb[3], y))
    return n, bb


def retest(log_path):
    """The sub-tick walks' census: sectors the present-camera re-test admitted."""
    total, walks = 0, 0
    for line in open(log_path, errors="replace"):
        if "[terrain-vis]" not in line or "mode=game" not in line:
            continue
        kv = dict(re.findall(r"(\w+)=(\d+)", line))
        if kv.get("sub") != "1":
            continue
        walks += 1
        total += int(kv.get("retest", "0"))
    return total, walks


rows_out, bad = [], []
total = 0
engage = None
for a in alphas:
    n, bb = diffpx("%s-i%s-on/%d-vk.png" % (base, a, mid),
                   "%s-i%s-off/%d-vk.png" % (base, a, mid))
    if n > mask_cap:
        raise SystemExit("[%s] alpha=%s differs in %d px, above the %d cap" % (tag, a, n, mask_cap))
    rt, walks = retest("%s-i%s-on.log" % (base, a))
    if a == "0":
        engage = rt
    total += n
    rows_out.append("alpha=%s px=%d bbox=%s retest=%d subwalks=%d"
                    % (a, n, "-" if bb is None else bb, rt, walks))
    if n:
        bad.append("alpha %s: game and frustum legs differ in %d px" % (a, n))
head = ("[%s] game=%s bk2=%s V=%d present=%d legs=%d diffpx=%d engage=%s"
        % (tag, game, bk2, V, mid, len(alphas) * 2, total, "-" if engage is None else engage))
for r in rows_out:
    print("[%s]   " % tag + r)
if not bad and not engage:
    print(head + " VACUOUS")
    print("[%s]   the alpha-0 sub-tick walk re-tested no sector into the set: the row "
          "measures nothing" % tag)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    elif [[ "$STAT" == interp ]]; then
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" "$MASK_CAP" "$INTERP_REF" ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import sys
from PIL import Image

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
mask_cap = int(sys.argv[11])
ref1 = sys.argv[12] == '1'
alphas = sys.argv[13:]
V, prev, mid = int(V), int(prev), int(mid)

opts, rects = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k == 'rect':
        x0, y0, x1, y1 = (int(t) for t in v.split(','))
        rects.append((x0, y0, x1, y1))
    else:
        opts[k] = v
# The whole frame is the default cell: the render-path selection has already
# isolated the path, so every pixel that moves is this path's.
if not rects:
    rects = [None]
# The floor on the alpha-0 difference, in mask pixels. Below it the knob moved
# nothing this row can see: no animated surface stood in the cell, and the
# legs prove nothing.
px_floor = float(opts.get('step', '200'))
judged = set(opts.get('judge', judge_def).split(','))
# The bound the alpha-1 leg must reproduce the unsuppressed present within.
# Every term of the interpolation is zero at the end of the tick's own
# interval, so the two legs render the same scene and the difference is exact.
IDENT_PX = float(opts.get('itol', '0'))


def load(path):
    im = Image.open(path).convert("RGB")
    return im.load(), im.size


def diff(a_path, b_path, rect):
    """The pixels the channel's interpolation moved inside the cell, their
    centroid, and the mean absolute RGB difference over them: the leave-one-out
    of one intermediate present rendered with and without that interpolation."""
    pa, sa = load(a_path)
    pb, sb = load(b_path)
    if sa != sb:
        raise SystemExit("[%s] %s and %s differ in size" % (tag, a_path, b_path))
    w, h = sa
    x0, y0, x1, y1 = rect if rect else (0, 0, w, h)
    x0 = max(0, x0); y0 = max(0, y0); x1 = min(w, x1); y1 = min(h, y1)
    n = 0
    sx = sy = 0.0
    sd = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            ca, cb = pa[x, y], pb[x, y]
            if ca == cb:
                continue
            n += 1
            sx += x; sy += y
            sd += abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
    if n == 0:
        return 0, None, 0.0
    return n, (sx / n, sy / n), sd / (3.0 * n)


bad, vacuous, rows_out = [], [], []
cells = 0
total_px = 0
for rect in rects:
    name = "whole frame" if rect is None else "rect%s" % (rect,)
    d = {}
    for a in alphas:
        n, c, m = diff("%s-i%s-on/%d-vk.png" % (base, a, mid),
                       "%s-i%s-off/%d-vk.png" % (base, a, mid), rect)
        if n > mask_cap:
            raise SystemExit("[%s] %s alpha=%s differs in %d px, above the %d cap"
                             % (tag, name, a, n, mask_cap))
        d[a] = (n, c, m)
        total_px += n
    n0 = d.get("0", (0, None, 0.0))[0]
    n1 = d.get("1", (0, None, 0.0))[0]
    rows_out.append("cell %s" % name)
    for a in alphas:
        n, c, m = d[a]
        rows_out.append("  alpha=%s%s px=%d meanD=%.2f centroid=%s"
                        % (a, "" if a in judged else " (reported)", n, m,
                           "-" if c is None else "(%.1f,%.1f)" % c))
    if ref1:
        # Referenced at alpha 1: alpha 0 is the identity leg, alpha 1 the
        # engagement leg, and the statistic is the summed colour difference.
        if "0" in d and n0 > IDENT_PX:
            bad.append("identity: alpha 0 differs in %d px, above the %.0f bound" % (n0, IDENT_PX))
        if n1 < px_floor:
            vacuous.append("%s: alpha 1 differs in %d px, below the %.0f floor: the knob "
                           "moved nothing this cell can see" % (name, n1, px_floor))
            continue
        cells += 1
        S = dict((a, d[a][0] * d[a][2]) for a in alphas)
        step = S["1"] / n1
        own = S.get("0", 0.0) / n1
        rows_out.append("  step=%.2f levels over %d px, own=%.2f (%.1f%% of the step)"
                        % (step, n1, own, 100.0 * own / step if step else 0.0))
        ctol = float(opts.get('ctol', '1.5'))
        for a in alphas:
            av = float(a)
            if av <= 0.0 or av >= 1.0:
                continue
            dist = abs(S[a] - av * S["1"]) / n1
            hold = (1.0 - av) * S["1"] / n1
            r = dist / hold if hold else 0.0
            vd = "LERPED" if r <= 0.25 else ("SNAP" if r >= 0.75 else "PARTIAL")
            rows_out.append("  alpha=%s dist=%.2f hold=%.2f ratio=%.3f %s"
                            % (a, dist, hold, r, vd))
            if a in judged and (vd != "LERPED" or dist >= ctol):
                bad.append("alpha %s: %s, %.2f levels from the lerp (bound %.2f)"
                           % (a, vd, dist, ctol))
        if "0" in judged and own >= 0.1 * step:
            bad.append("own: alpha 0 stands %.2f levels from the previous tick, "
                       "above 10%% of the %.2f step" % (own, step))
        continue
    # The identity leg: at alpha 1 the interpolation term is zero, so the two
    # legs render the same present and no pixel may differ.
    if "1" in d and n1 > IDENT_PX:
        bad.append("identity: alpha 1 differs in %d px, above the %.0f bound" % (n1, IDENT_PX))
    # Engagement: at alpha 0 the whole tick's worth of this channel's
    # motion separates the legs. Below the floor the knob reached nothing.
    if n0 < px_floor:
        vacuous.append("%s: alpha 0 differs in %d px, below the %.0f floor: the knob "
                       "moved nothing this cell can see" % (name, n0, px_floor))
        continue
    cells += 1
    # The verdict, from the shape of the difference across the interval. A
    # linearly interpolated channel separates the legs by (1-alpha) of the
    # tick's motion, so the alpha-0.5 difference is about half the alpha-0
    # one; a channel that holds its tick state renders the same present in
    # both legs at every alpha and the difference collapses to nothing.
    for a in alphas:
        if a not in judged or a == "1":
            continue
        n, c, m = d[a]
        av = float(a)
        r = n / float(n0) if n0 else 0.0
        exp = 1.0 - av
        if r <= 0.05:
            vd = "SNAP"
        elif 0.5 * exp <= r <= 1.5 * exp:
            vd = "LERPED"
        else:
            vd = "PARTIAL"
        rows_out.append("  alpha=%s px=%d of alpha0 px=%d ratio=%.3f expected=%.3f %s"
                        % (a, n, n0, r, exp, vd))

head = ("[%s] game=%s bk2=%s V=%d cells=%d legs=%d diffpx=%d"
        % (tag, game, bk2, V, cells, len(alphas) * 2, total_px))
for r in rows_out:
    print("[%s]   " % tag + r)
if vacuous or not cells:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    elif [[ "$STAT" == ext ]]; then
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import re
import sys

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
alphas = sys.argv[11:]
V, prev, mid = int(V), int(prev), int(mid)

opts, recs = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k == 'rec':
        recs.append(v.upper())
    else:
        opts[k] = v
# The bound on the judged alphas' ratio: the residual from the lerp of the two
# canonical extents over the distance a held extent stands from it. LERPED is
# at or below 0.25, the class boundary the area cell uses.
rtol = float(opts.get('rtol', '0.25'))
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '0.5'))
engage = opts.get('engage', '1') != '0'
# The alpha-1 replay applies no scale, so it must print the canonical extent
# to the trace's last digit.
IDENT_TOL = 0.0005

rx = re.compile(r'\[glint-ext vbl=(\d+) leg=(\w+) alpha=[\d.]+\] rec=(\w+) '
                r'ext=([\d.]+) n=(\d+)')


def load(path):
    """First trace line per (vbl, leg, record) of one replay."""
    out = {}
    try:
        for line in open(path, errors='replace'):
            m = rx.search(line)
            if m:
                out.setdefault((int(m[1]), m[2], m[3].upper()), (float(m[4]), int(m[5])))
    except OSError:
        pass
    return out


legs = {a: load("%s-i%s.log" % (base, a)) for a in alphas}
supp = load("%s-s.log" % base) if suppress else {}
canon = legs.get('1', {})
vacuous, bad, out = [], [], []
worst, engaged = 0.0, 0
if not recs:
    vacuous.append("the row names no rec=")


def cls(r):
    return "LERPED" if r <= 0.25 else ("SNAP" if r >= 0.75 else "PARTIAL")


for rec in recs:
    c0 = canon.get((prev, 'canon', rec))
    c1 = canon.get((V, 'canon', rec))
    if c0 is None or c1 is None:
        vacuous.append("rec %s: no canonical trace at VBL %d or %d" % (rec, prev, V))
        continue
    step = c1[0] - c0[0]
    line = "rec %s n=%d canon%d=%.4f canon%d=%.4f step=%.4f" % (rec, c1[1], prev, c0[0], V, c1[0], step)
    if abs(step) < step_floor:
        vacuous.append("rec %s: extent step %.4f below the %.2f floor" % (rec, abs(step), step_floor))
    shown = {}
    for a in alphas:
        s = legs[a].get((mid, 'inter', rec))
        if s is None:
            vacuous.append("rec %s: no intermediate trace at VBL %d, alpha=%s" % (rec, mid, a))
            continue
        shown[a] = s[0]
        av = float(a)
        ex = c0[0] + step * av
        line += " a%s=%.4f(D=%.4f)" % (a, s[0], abs(s[0] - ex))
        if av >= 1.0:
            if abs(s[0] - c1[0]) > IDENT_TOL:
                bad.append("rec %s: alpha 1 extent %.4f is not canon(V) %.4f" % (rec, s[0], c1[0]))
            continue
        if abs(step) < step_floor:
            continue
        r = abs(s[0] - ex) / ((1.0 - av) * abs(step))
        line += " r%s=%.2f %s" % (a, r, cls(r))
        if a in judged:
            worst = max(worst, r)
            if r > rtol:
                bad.append("rec %s: alpha=%s ratio %.2f above the %.2f bound" % (rec, a, r, rtol))
    if '0' in shown:
        line += " own=%.4f" % abs(shown['0'] - c0[0])
    out.append(line)
    # The engagement leg: with the extent scale suppressed the replay
    # holds the tick's extent, which stands the whole step from canon(V-2).
    if suppress:
        s = supp.get((mid, 'inter', rec))
        if s is not None:
            own_s = abs(s[0] - c0[0])
            out.append("  %s=0 own=%.4f of step=%.4f" % (suppress, own_s, abs(step)))
            if abs(step) >= step_floor and own_s >= 0.75 * abs(step):
                engaged += 1

if suppress and engage and not engaged and not vacuous:
    vacuous.append("no record holds its tick extent under %s=0: the knob suppressed "
                   "nothing this row can see" % suppress)
head = "[%s] game=%s bk2=%s V=%d recs=%d worst=%.2f" % (tag, game, bk2, V, len(recs), worst)
for r in out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    elif [[ "$STAT" == rot ]]; then
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import re
import sys

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
alphas = sys.argv[11:]
V, prev, mid = int(V), int(prev), int(mid)

opts, recs = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k == 'rec':
        recs.append(v.upper())
    else:
        opts[k] = v
rtol = float(opts.get('rtol', '0.25'))
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '1.0'))
engage = opts.get('engage', '1') != '0'
# The alpha-1 replay applies no rotation, so it must print the canonical
# orientation to the trace's last digit.
IDENT_TOL = 0.0005

rx = re.compile(r'\[glint-rot vbl=(\d+) leg=(\w+) alpha=[\d.]+\] rec=(\w+) '
                r'ori=([\d.]+) ph=([\d.]+) ext=([\d.]+) n=(\d+)')


def load(path):
    """First trace line per (vbl, leg, record) of one replay."""
    out = {}
    try:
        for line in open(path, errors='replace'):
            m = rx.search(line)
            if m:
                out.setdefault((int(m[1]), m[2], m[3].upper()),
                               (float(m[4]), float(m[5]), float(m[6]), int(m[7])))
    except OSError:
        pass
    return out


def wrap(d, period):
    """d on the shortest arc of a circle of `period` entries."""
    d = (d + period / 2.0) % period - period / 2.0
    return d


legs = {a: load("%s-i%s.log" % (base, a)) for a in alphas}
supp = load("%s-s.log" % base) if suppress else {}
canon = legs.get('1', {})
vacuous, bad, out = [], [], []
worst, engaged = 0.0, 0
if not recs:
    vacuous.append("the row names no rec=")


def cls(r):
    return "LERPED" if r <= 0.25 else ("SNAP" if r >= 0.75 else "PARTIAL")


for rec in recs:
    c0 = canon.get((prev, 'canon', rec))
    c1 = canon.get((V, 'canon', rec))
    if c0 is None or c1 is None:
        vacuous.append("rec %s: no canonical trace at VBL %d or %d" % (rec, prev, V))
        continue
    step = wrap(c1[0] - c0[0], 64.0)
    pstep = wrap(c1[1] - c0[1], 256.0)
    line = ("rec %s n=%d ext=%.2f canon%d=%.4f canon%d=%.4f step=%.4f ph%d=%.2f ph%d=%.2f"
            % (rec, c1[3], c1[2], prev, c0[0], V, c1[0], step, prev, c0[1], V, c1[1]))
    if abs(step) < step_floor:
        vacuous.append("rec %s: orientation step %.4f below the %.2f floor"
                       % (rec, abs(step), step_floor))
    shown = {}
    for a in alphas:
        s = legs[a].get((mid, 'inter', rec))
        if s is None:
            vacuous.append("rec %s: no intermediate trace at VBL %d, alpha=%s" % (rec, mid, a))
            continue
        shown[a] = s[0]
        av = float(a)
        dev = abs(wrap(s[0] - (c0[0] + step * av), 64.0))
        pdev = abs(wrap(s[1] - (c0[1] + pstep * av), 256.0))
        line += " a%s=%.4f(D=%.4f ph=%.2f pD=%.2f)" % (a, s[0], dev, s[1], pdev)
        if av >= 1.0:
            if abs(wrap(s[0] - c1[0], 64.0)) > IDENT_TOL:
                bad.append("rec %s: alpha 1 orientation %.4f is not canon(V) %.4f"
                           % (rec, s[0], c1[0]))
            continue
        if abs(step) < step_floor:
            continue
        r = dev / ((1.0 - av) * abs(step))
        line += " r%s=%.2f %s" % (a, r, cls(r))
        if a in judged:
            worst = max(worst, r)
            if r > rtol:
                bad.append("rec %s: alpha=%s ratio %.2f above the %.2f bound" % (rec, a, r, rtol))
    if '0' in shown:
        line += " own=%.4f" % abs(wrap(shown['0'] - c0[0], 64.0))
    out.append(line)
    # The engagement leg: with the spin suppressed the replay holds
    # the tick's orientation, which stands the whole step from canon(V-2).
    if suppress:
        s = supp.get((mid, 'inter', rec))
        if s is not None:
            own_s = abs(wrap(s[0] - c0[0], 64.0))
            out.append("  %s=0 own=%.4f of step=%.4f" % (suppress, own_s, abs(step)))
            if abs(step) >= step_floor and own_s >= 0.75 * abs(step):
                engaged += 1

if suppress and engage and not engaged and not vacuous:
    vacuous.append("no record holds its tick orientation under %s=0: the knob "
                   "suppressed nothing this row can see" % suppress)
head = "[%s] game=%s bk2=%s V=%d recs=%d worst=%.2f" % (tag, game, bk2, V, len(recs), worst)
for r in out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    elif [[ "$STAT" == vertex ]]; then
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import re
import sys

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
alphas = sys.argv[11:]
V, prev, mid = int(V), int(prev), int(mid)

opts, named = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k in ('vertex', 'blob', 'gem'):
        x, _, y = v.partition(',')
        named.append((float(x), float(y)))
    else:
        opts[k] = v
tol = float(opts.get('tol', '0.5'))
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '4'))
engage = opts.get('engage', '1') != '0'

# Two aperture vertices this close are one corner: the clip emits a corner
# twice where an edge meets the rect exactly there, and the pair would
# otherwise read as an ambiguous match.
DEDUPE_R = 0.05
# A named corner must resolve to one vertex: the runner-up must sit at least
# this many pixels further from the target than the winner.
AMBIG_MARGIN = 6.0
# The radius a named corner is looked for in.
FIND_R = 12.0
# The bound the alpha-1 replay must reproduce the canonical present within,
# vertex for vertex: the two are the same float expression evaluated at 1, so
# the only slack is the census's own two printed decimals.
IDENT_TOL = 0.011

AP = re.compile(r'\[portal3-ap f=(\d+) src=(\w+) a=([0-9.]+) slot=(\d+) arg=(\d+) n=(\d+) '
                r'd7=(-?\d+) dw=(-?[0-9.]+) ir0=(-?\d+) far=([0-9A-F]+)(?: rec=[0-9A-F]+ native=\d+)?\](.*)')
FEED = re.compile(r'\[portal3-feed f=(\d+) src=(\w+) a=([0-9.]+) portals=(\d+) '
                  r'tris=(\d+) emitted=(\d+)\]')
XY = re.compile(r'\(([-0-9.]+),([-0-9.]+)\)')


def read(path):
    """The aperture corners and the fed triangle count of every present in one
    run's census, keyed (src, VBL). Only the base record of each portal carries
    the aperture; its duplicate corners are folded."""
    verts, tris = {}, {}
    try:
        fh = open(path, errors='replace')
    except IOError:
        return None, None
    with fh:
        for ln in fh:
            ln = ln.strip()
            m = FEED.match(ln)
            if m:
                tris[(m.group(2), int(m.group(1)))] = int(m.group(5))
                continue
            m = AP.match(ln)
            if not m or m.group(5) != '1':
                continue
            pts = verts.setdefault((m.group(2), int(m.group(1))), [])
            for x, y in XY.findall(m.group(11)):
                q = (float(x), float(y))
                if all(abs(q[0] - r[0]) > DEDUPE_R or abs(q[1] - r[1]) > DEDUPE_R
                       for r in pts):
                    pts.append(q)
    return verts, tris


def find(pts, tx, ty, radius):
    """The one corner near (tx, ty), or None with the reason."""
    d = sorted((((p[0] - tx) ** 2 + (p[1] - ty) ** 2) ** 0.5, p) for p in pts)
    if not d or d[0][0] > radius:
        return None, "no vertex within %.1f px of (%.1f,%.1f)" % (radius, tx, ty)
    if len(d) > 1 and d[1][0] - d[0][0] < AMBIG_MARGIN:
        return None, ("two vertices within %.1f px of each other at (%.1f,%.1f)"
                      % (AMBIG_MARGIN, tx, ty))
    return d[0][1], ""


def seg_dist(c, a, b):
    """Distance from a corner to the segment between its two canonical
    positions - the line its own motion lies on."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / n
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return ((c[0] - (a[0] + dx * t)) ** 2 + (c[1] - (a[1] + dy * t)) ** 2) ** 0.5


legs = {}
vacuous, bad, rows_out = [], [], []
tris_total = 0
for name, path in ([(a, "%s-i%s.log" % (base, a)) for a in alphas]
                   + ([("suppressed", "%s-s.log" % base)] if suppress else [])):
    verts, tris = read(path)
    if verts is None:
        vacuous.append("leg %s: no log at %s" % (name, path))
        continue
    legs[name] = verts
    for key in (("canon", prev), ("canon", V), ("sub", mid)):
        if key not in verts:
            vacuous.append("leg %s: no %s present at VBL %d in %s"
                           % (name, key[0], key[1], path))
        elif tris.get(key, 0) == 0:
            vacuous.append("leg %s: the %s present at VBL %d fed no portal triangle"
                           % (name, key[0], key[1]))
        else:
            tris_total += tris[key]

if not named:
    vacuous.append("the row names no vertex")
if vacuous:
    print("[%s] game=%s bk2=%s V=%d verts=%d legs=%d tris=%d VACUOUS"
          % (tag, game, bk2, V, len(named), len(legs), tris_total))
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)

# The identity leg, whole-polygon: at alpha 1 the replay evaluates the same
# expression at the tick's own instant, so it must reproduce the canonical
# present corner for corner, not merely within the residual bound.
if "1" in legs:
    c = legs["1"][("canon", V)]
    r = legs["1"][("sub", mid)]
    if len(c) != len(r):
        bad.append("identity: alpha 1 replays %d corners, canon%d has %d"
                   % (len(r), V, len(c)))
    else:
        worst_id = max([max(abs(a[0] - b[0]), abs(a[1] - b[1]))
                        for a, b in zip(c, r)] or [0.0])
        rows_out.append("identity alpha=1 corners=%d worst=%.3f" % (len(c), worst_id))
        if worst_id > IDENT_TOL:
            bad.append("identity: alpha 1 differs from canon%d by %.3f px"
                       % (V, worst_id))

worst = 0.0
engaged = 0
for gx, gy in named:
    # The row names the corner where the replayed tick drew it; its partner one
    # tick back opens the interval. Both are read from the alpha-1 leg's own
    # census, and every other leg's canonical presents are the same run.
    ref = legs.get("1") or legs[alphas[0]]
    c1, why = find(ref[("canon", V)], gx, gy, FIND_R)
    if c1 is None:
        vacuous.append("vertex (%.1f,%.1f) at canon%d: %s" % (gx, gy, V, why))
        continue
    c0, why = find(ref[("canon", prev)], c1[0], c1[1], 3 * FIND_R)
    if c0 is None:
        vacuous.append("vertex (%.1f,%.1f) at canon%d: %s" % (gx, gy, prev, why))
        continue
    step = ((c1[0] - c0[0]) ** 2 + (c1[1] - c0[1]) ** 2) ** 0.5
    rows_out.append("vertex (%.1f,%.1f) canon%d=(%.2f,%.2f) canon%d=(%.2f,%.2f) step=%.2f"
                    % (gx, gy, prev, c0[0], c0[1], V, c1[0], c1[1], step))
    if step < step_floor:
        vacuous.append("vertex (%.1f,%.1f) steps %.2f px over the interval, below the %.2f floor"
                       % (gx, gy, step, step_floor))
        continue
    shown0 = None
    for a in alphas:
        av = float(a)
        ex = (c0[0] + (c1[0] - c0[0]) * av, c0[1] + (c1[1] - c0[1]) * av)
        # The corner is identified by the interval it must lie on, not by the
        # expected point: a replay that holds its tick state sits at the
        # interval's closing end, which is on the same segment, so one rule
        # names the same corner before and after a fix.
        cand = sorted((seg_dist(p, c0, c1), p) for p in legs[a][("sub", mid)])
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("vertex (%.1f,%.1f) at alpha=%s: no corner on the interval"
                           % (gx, gy, a))
            continue
        if len(cand) > 1 and cand[1][0] - cand[0][0] < AMBIG_MARGIN:
            vacuous.append("vertex (%.1f,%.1f) at alpha=%s: two corners on the interval"
                           % (gx, gy, a))
            continue
        ci = cand[0][1]
        if av == 0.0:
            shown0 = ci
        d = ((ci[0] - ex[0]) ** 2 + (ci[1] - ex[1]) ** 2) ** 0.5
        if a in judged:
            worst = max(worst, d)
        rows_out.append("  alpha=%s%s shown=(%.2f,%.2f) expected=(%.2f,%.2f) D=%.2f"
                        % (a, "" if a in judged else " (reported)",
                           ci[0], ci[1], ex[0], ex[1], d))
        if a in judged and d > tol:
            bad.append("vertex (%.1f,%.1f) alpha=%s D=%.2f above the %.2f bound"
                       % (gx, gy, a, d, tol))

    # own: how far the alpha-0 leg stands from the corner the previous tick
    # drew. A replay that carries the interval reads near zero there, one that
    # holds its tick state reads the whole step.
    if shown0 is not None:
        own = ((shown0[0] - c0[0]) ** 2 + (shown0[1] - c0[1]) ** 2) ** 0.5
        rows_out.append("  own=%.2f of step=%.2f" % (own, step))
    # The engagement leg: with the replay's interpolation suppressed
    # the aperture must hold its tick state, the whole step from the previous
    # tick's. A smaller reading means the knob reached nothing.
    if suppress:
        cand = sorted((seg_dist(p, c0, c1), p) for p in legs["suppressed"][("sub", mid)])
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("vertex (%.1f,%.1f) under %s=0: no corner on the interval"
                           % (gx, gy, suppress))
        else:
            cs = cand[0][1]
            own_s = ((cs[0] - c0[0]) ** 2 + (cs[1] - c0[1]) ** 2) ** 0.5
            rows_out.append("  %s=0 own=%.2f of step=%.2f" % (suppress, own_s, step))
            if own_s >= step - IDENT_TOL:
                engaged += 1

if suppress and engage and not engaged:
    vacuous.append("no corner holds its tick state under %s=0: the knob suppressed "
                   "nothing this row can see" % suppress)
head = ("[%s] game=%s bk2=%s V=%d verts=%d legs=%d tris=%d worst=%.2f"
        % (tag, game, bk2, V, len(named), len(legs), tris_total, worst))
for r in rows_out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    else
    FEEDER_GEOM="$GEOM" \
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" "$MASK_CAP" ${ALPHAS[@]+"${ALPHAS[@]}"} <<'PYEOF' || fail=1
import os
import sys
from PIL import Image

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
mask_cap = int(sys.argv[11])
alphas = sys.argv[12:]
V, prev, mid = int(V), int(prev), int(mid)
probe = os.environ.get("OPENPETE_FEEDER_PROBE", "") not in ("", "0")

opts, gems, grecs = {}, [], []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k in ('gem', 'blob'):
        x, _, y = v.partition(',')
        gems.append((float(x), float(y)))
    elif k == 'rec':
        grecs.append(v.upper())
    else:
        opts[k] = v
tol = float(opts.get('tol', '0.5'))
# The geometric centroid (the glint path). A rotating star's mask centroid
# traces an arc between the two ticks' rasters, and the midpoint of that arc's
# chord is not its expected value, so the translation is judged on the
# centroid of the outline corners the feed emitted, which the feeder prints
# per record per present (`[glint-ext] ... c=(x,y)`, PSX screen pixels). Each
# gem names its record with a `rec=` key, in order; `tol` and `step` apply to
# that centroid in PSX pixels, and the mask centroid is reported only.
geom = os.environ.get("FEEDER_GEOM", "0") not in ("", "0")
if geom and len(grecs) != len(gems):
    raise SystemExit("[%s] the glint path needs one rec= per gem (%d gems, %d recs)"
                     % (tag, len(gems), len(grecs)))


def geo_load(path):
    """(vbl, leg, rec) -> the traced outline corner centroid of one replay."""
    import re
    rx = re.compile(r'\[glint-ext vbl=(\d+) leg=(\w+) alpha=[\d.]+\] rec=(\w+) '
                    r'ext=[\d.]+ n=\d+ at=\([-\d.]+,[-\d.]+\) c=\(([-\d.]+),([-\d.]+)\)')
    out = {}
    try:
        for line in open(path, errors='replace'):
            m = rx.search(line)
            if m:
                out.setdefault((int(m[1]), m[2], m[3].upper()), (float(m[4]), float(m[5])))
    except OSError:
        pass
    return out
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '4'))
engage = opts.get('engage', '1') != '0'
# The area and colour statistics are judged only where the row sets a bound;
# without one they are measured and classified but assert nothing, so a row
# written for the centroid alone keeps its verdict.
atol = float(opts['atol']) if 'atol' in opts else None
ctol = float(opts['ctol']) if 'ctol' in opts else None
# Per-statistic floors on the canonical step. A statistic that barely moves
# over the interval cannot separate a lerp from a hold, so it is classified
# UNMOVED rather than LERPED. Area is in mask pixels, colour in 0-255 levels
# of the widest channel.
area_floor = float(opts.get('astep', '40'))
col_floor = float(opts.get('cstep', '6'))

# Two components of one mask are the same blob when their pixels come within
# this Chebyshev distance: a four-point star's arms and a glow ring's fan are
# each contiguous, and the nearest two blobs of any corpus row stand far
# further apart than this.
CLUSTER_R = int(opts.get('cluster', '6'))
# A named blob must resolve to one component: the runner-up must sit at least
# this many pixels further from the target than the winner.
AMBIG_MARGIN = 6.0
# The radius a named blob's component is looked for in, around the position the
# row names and around each tracked position after it.
FIND_R = float(opts.get('findr', '12'))


def mask(a_path, b_path):
    """The pixels this feed painted and the colour it painted them: the
    leave-one-out difference of the feed-on and feed-off captures of one
    present, each pixel carrying its feed-on RGB."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        raise SystemExit("[%s] %s and %s differ in size" % (tag, a_path, b_path))
    pa, pb = a.load(), b.load()
    w, h = a.size
    px = {}
    for y in range(h):
        for x in range(w):
            ca = pa[x, y]
            if ca != pb[x, y]:
                px[(x, y)] = ca
    return px


def components(px):
    """Cluster mask pixels at CLUSTER_R, returning (cx, cy, count, r, g, b) per
    blob, where r,g,b is the mean feed-on colour of the blob's pixels. The
    flood fill walks the Chebyshev neighbourhood of each pixel rather than the
    remaining set, so a mask of a whole terrain surface clusters in time linear
    in its pixel count."""
    seen = set(px)
    out = []
    span = range(-CLUSTER_R, CLUSTER_R + 1)
    while seen:
        start = seen.pop()
        stack = [start]
        comp = [start]
        while stack:
            x, y = stack.pop()
            for dy in span:
                for dx in span:
                    q = (x + dx, y + dy)
                    if q in seen:
                        seen.discard(q)
                        stack.append(q)
                        comp.append(q)
        n = len(comp)
        sr = sg = sb = 0
        for q in comp:
            c = px[q]
            sr += c[0]; sg += c[1]; sb += c[2]
        out.append((sum(p[0] for p in comp) / n, sum(p[1] for p in comp) / n, n,
                    sr / n, sg / n, sb / n))
    return sorted(out, key=lambda c: (-c[2], c[0]))


def seg_dist(c, a, b):
    """Distance from a component's centroid to the segment between the two
    canonical positions - the line the blob's own motion lies on."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / n
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return ((c[0] - (a[0] + dx * t)) ** 2 + (c[1] - (a[1] + dy * t)) ** 2) ** 0.5


def find(comps, tx, ty, radius):
    """The one component near (tx, ty), or None with the reason."""
    d = sorted(((((c[0] - tx) ** 2 + (c[1] - ty) ** 2) ** 0.5), c) for c in comps)
    if not d or d[0][0] > radius:
        return None, "no component within %.1f px of (%.1f,%.1f)" % (radius, tx, ty)
    if len(d) > 1 and d[1][0] - d[0][0] < AMBIG_MARGIN:
        return None, ("two components within %.1f px of each other at (%.1f,%.1f)"
                      % (AMBIG_MARGIN, tx, ty))
    return d[0][1], ""


def st_centroid(c):
    return (c[0], c[1])


def st_area(c):
    return (float(c[2]),)


def st_colour(c):
    return (c[3], c[4], c[5])


def dist(u, w):
    """The residual of a statistic: Euclidean for the two-vector centroid, the
    widest channel for the three-vector colour, the magnitude for area. Each
    is the one a bound on that statistic is naturally written in."""
    if len(u) == 2:
        return ((u[0] - w[0]) ** 2 + (u[1] - w[1]) ** 2) ** 0.5
    return max(abs(a - b) for a, b in zip(u, w))


def lerp(u, w, a):
    return tuple(p + (q - p) * a for p, q in zip(u, w))


def fmt(u):
    return "(" + ",".join("%.1f" % v for v in u) + ")"


# A statistic's residual against its own hold: a producer that holds its tick
# state shows the canonical V value, which sits (1-alpha) of the step from the
# expected lerp. The ratio of the measured residual to that distance is what
# separates the three verdicts, and it is the same number for every statistic
# and every channel, so one pair of thresholds classifies them all.
LERP_R = 0.25
SNAP_R = 0.75


def verdict(resid, hold):
    if hold <= 0.0:
        return "UNMOVED"
    r = resid / hold
    if r <= LERP_R:
        return "LERPED"
    if r >= SNAP_R:
        return "SNAP"
    return "PARTIAL"


STATS = (("centroid", st_centroid, step_floor, None if geom else tol),
         ("area", st_area, area_floor, atol),
         ("colour", st_colour, col_floor, ctol))

legs = {}
maskpx = 0
vacuous, bad, notes = [], [], []
for name, on, off, vbl in (
        [("canon%d" % prev, "%s-c-on/%d-vk.png" % (base, prev),
          "%s-c-off/%d-vk.png" % (base, prev), prev),
         ("canon%d" % V, "%s-c-on/%d-vk.png" % (base, V),
          "%s-c-off/%d-vk.png" % (base, V), V)] +
        [("inter%s" % a, "%s-i%s-on/%d-vk.png" % (base, a, mid),
          "%s-i%s-off/%d-vk.png" % (base, a, mid), mid) for a in alphas] +
        ([("suppressed", "%s-s-on/%d-vk.png" % (base, mid),
           "%s-s-off/%d-vk.png" % (base, mid), mid)] if suppress and alphas else [])):
    px = mask(on, off)
    if len(px) > mask_cap:
        raise SystemExit("[%s] leg %s masks %d px, above the %d cap: the "
                         "leave-one-out is not isolating this feeder"
                         % (tag, name, len(px), mask_cap))
    legs[name] = components(px)
    maskpx += len(px)
    if not px:
        vacuous.append("leg %s masks no pixel: the feed painted nothing" % name)

if probe:
    # Row authoring: the components of the two canonical presents, largest
    # first, with every statistic a row can name. No blob is judged.
    for name in ("canon%d" % prev, "canon%d" % V):
        print("[%s] probe %s components=%d" % (tag, name, len(legs.get(name, []))))
        for c in legs.get(name, [])[:20]:
            print("[%s]   blob=%.1f,%.1f area=%d colour=(%.1f,%.1f,%.1f)"
                  % (tag, c[0], c[1], c[2], c[3], c[4], c[5]))
    print("[%s] probe game=%s bk2=%s V=%d maskpx=%d" % (tag, game, bk2, V, maskpx))
    sys.exit(0)

worst = 0.0
engaged = 0
engaged_stats = 0
rows_out = []
geo_legs = {}
if geom:
    geo_legs["canon"] = geo_load("%s-c-on.log" % base)
    for a in alphas:
        geo_legs[a] = geo_load("%s-i%s-on.log" % (base, a))


def judge_geom(rec, gx, gy):
    """The geometric centroid cell of one gem: its record's traced outline
    centroid at every captured alpha against the lerp of the two canonical
    ones, judged against `tol` on the judged alphas."""
    global worst, engaged_stats
    g0 = geo_legs["canon"].get((prev, 'canon', rec))
    g1 = geo_legs["canon"].get((V, 'canon', rec))
    if g0 is None or g1 is None:
        vacuous.append("gem (%.1f,%.1f) rec %s: no traced centroid at canon%d or canon%d"
                       % (gx, gy, rec, prev, V))
        return
    gstep = dist(g0, g1)
    parts = []
    for a in alphas:
        gs = geo_legs[a].get((mid, 'inter', rec))
        if gs is None:
            vacuous.append("gem (%.1f,%.1f) rec %s: no traced centroid at inter alpha=%s"
                           % (gx, gy, rec, a))
            continue
        r = dist(gs, lerp(g0, g1, float(a)))
        parts.append("a%s=%.3f" % (a, r))
        if a not in judged:
            continue
        if a == "1" or gstep >= step_floor:
            worst = max(worst, r)
            if r > tol:
                bad.append("gem (%.1f,%.1f) rec %s geometric centroid alpha=%s D=%.3f "
                           "above the %.2f bound" % (gx, gy, rec, a, r, tol))
    if gstep < step_floor:
        notes.append("gem (%.1f,%.1f) rec %s: geometric step %.2f PSX px below the %.2f "
                     "floor: reported, not judged" % (gx, gy, rec, gstep, step_floor))
    else:
        engaged_stats += 1
    rows_out.append("  cell (%.3f,%.3f)->(%.3f,%.3f) geometric rec=%s step=%.2f %s (PSX px)"
                    % (g0[0], g0[1], g1[0], g1[1], rec, gstep, " ".join(parts)))
if not gems:
    vacuous.append("the row names no blob")
for gi, (gx, gy) in enumerate(gems):
    # The row names the blob where the replayed tick drew it; its partner one
    # tick back opens the interval.
    c1, why = find(legs["canon%d" % V], gx, gy, FIND_R)
    if c1 is None:
        vacuous.append("blob (%.1f,%.1f) at canon%d: %s" % (gx, gy, V, why))
        continue
    c0, why = find(legs["canon%d" % prev], c1[0], c1[1], 3 * FIND_R)
    if c0 is None:
        vacuous.append("blob (%.1f,%.1f) at canon%d: %s" % (gx, gy, prev, why))
        continue
    step = ((c1[0] - c0[0]) ** 2 + (c1[1] - c0[1]) ** 2) ** 0.5
    rows_out.append("blob (%.1f,%.1f) canon%d=(%.1f,%.1f) %dpx rgb=(%.1f,%.1f,%.1f) "
                    "canon%d=(%.1f,%.1f) %dpx rgb=(%.1f,%.1f,%.1f) step=%.2f"
                    % (gx, gy, prev, c0[0], c0[1], c0[2], c0[3], c0[4], c0[5],
                       V, c1[0], c1[1], c1[2], c1[3], c1[4], c1[5], step))
    # A blob whose centroid barely moves over the interval passes any centroid
    # bound whatever the producer does, so the centroid is reported and not
    # judged there. Its area and its colour are separate channels and are
    # measured either way; the row is VACUOUS only when no statistic of any of
    # its blobs cleared its own floor.
    judge_centroid = step >= step_floor
    if geom:
        judge_centroid = False
        judge_geom(grecs[gi], gx, gy)
    elif not judge_centroid:
        notes.append("blob (%.1f,%.1f) steps %.2f px over the interval, below the %.2f "
                     "centroid floor: its centroid is reported, not judged"
                     % (gx, gy, step, step_floor))
    shown0 = None
    shown = {}
    for a in alphas:
        av = float(a)
        ex = (c0[0] + (c1[0] - c0[0]) * av, c0[1] + (c1[1] - c0[1]) * av)
        # The component is identified by the interval it must lie on, not by
        # the expected point: a producer that holds its tick pose sits at the
        # interval's closing end, which is on the same segment, so one rule
        # names the same star before and after the fix.
        cand = sorted(((seg_dist(c, c0, c1), c) for c in legs["inter%s" % a]))
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("blob (%.1f,%.1f) at inter alpha=%s: no component on the interval"
                           % (gx, gy, a))
            continue
        if len(cand) > 1 and cand[1][0] - cand[0][0] < AMBIG_MARGIN:
            vacuous.append("blob (%.1f,%.1f) at inter alpha=%s: two components on the interval"
                           % (gx, gy, a))
            continue
        ci = cand[0][1]
        shown[a] = ci
        if float(a) == 0.0:
            shown0 = ci
        d = ((ci[0] - ex[0]) ** 2 + (ci[1] - ex[1]) ** 2) ** 0.5
        if a in judged and judge_centroid:
            worst = max(worst, d)
        rows_out.append("  alpha=%s%s shown=(%.1f,%.1f) %dpx rgb=(%.1f,%.1f,%.1f) "
                        "expected=(%.1f,%.1f) D=%.2f"
                        % (a, "" if (a in judged and not geom) else " (reported)",
                           ci[0], ci[1], ci[2], ci[3], ci[4], ci[5], ex[0], ex[1], d))
        if a in judged and judge_centroid and d > tol:
            bad.append("blob (%.1f,%.1f) alpha=%s D=%.2f above the %.2f bound"
                       % (gx, gy, a, d, tol))

    # The census line per statistic: the canonical step, the residual at every
    # captured alpha against the lerp of the same statistic, the alpha-1
    # identity, the alpha-0 own, and the verdict.
    for sname, sfn, floor, sbound in STATS:
        s0, s1 = sfn(c0), sfn(c1)
        sstep = dist(s0, s1)
        parts = []
        vd = "UNMOVED"
        for a in alphas:
            if a not in shown:
                continue
            av = float(a)
            sx = lerp(s0, s1, av)
            sr = dist(sfn(shown[a]), sx)
            parts.append("a%s=%.2f" % (a, sr))
            if a in judged and a != "1":
                vd = verdict(sr, (1.0 - av) * sstep)
            if sbound is not None and a in judged and sr > sbound:
                bad.append("blob (%.1f,%.1f) %s alpha=%s resid=%.2f above the %.2f bound"
                           % (gx, gy, sname, a, sr, sbound))
        ident = dist(sfn(shown["1"]), s1) if "1" in shown else float('nan')
        own = dist(sfn(shown["0"]), s0) if "0" in shown else float('nan')
        if sstep < floor:
            vd = "UNMOVED(step=%.2f<%.2f)" % (sstep, floor)
        else:
            engaged_stats += 1
        rows_out.append("  cell %s %s step=%.2f %s ident=%.2f own=%.2f %s"
                        % (fmt(s0) + "->" + fmt(s1), sname, sstep,
                           " ".join(parts), ident, own, vd))

    # own: how far the alpha-0 leg stands from the pose the previous tick drew.
    # A producer that carries its own motion reads near zero, one that holds
    # its tick pose reads the whole step.
    if shown0 is not None:
        own = ((shown0[0] - c0[0]) ** 2 + (shown0[1] - c0[1]) ** 2) ** 0.5
        rows_out.append("  own=%.2f of step=%.2f  area canon%d=%dpx canon%d=%dpx alpha0=%dpx"
                        % (own, step, prev, c0[2], V, c1[2], shown0[2]))
    # The engagement leg: with the path's interpolation suppressed the
    # producer must hold its tick pose, which is the whole step away from the
    # previous tick's less the occlusion's shift; the SNAP bound separates it
    # from a knob that reached nothing.
    if suppress:
        cand = sorted(((seg_dist(c, c0, c1), c) for c in legs["suppressed"]))
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("blob (%.1f,%.1f) under %s=0: no component on the interval"
                           % (gx, gy, suppress))
        else:
            cs = cand[0][1]
            own_s = ((cs[0] - c0[0]) ** 2 + (cs[1] - c0[1]) ** 2) ** 0.5
            rows_out.append("  %s=0 own=%.2f of step=%.2f %dpx" % (suppress, own_s, step, cs[2]))
            if own_s >= 0.75 * step:
                engaged += 1

if suppress and engage and not engaged:
    vacuous.append("no blob holds its tick pose under %s=0: the knob suppressed "
                   "nothing this row can see" % suppress)
if gems and not engaged_stats:
    vacuous.append("no statistic of any named blob moved over the interval: the row "
                   "measures nothing")
for n in notes:
    rows_out.append(n)
head = ("[%s] game=%s bk2=%s V=%d blobs=%d legs=%d maskpx=%d worst=%.2f"
        % (tag, game, bk2, V, len(gems), len(legs), maskpx, worst))
for r in rows_out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    fi
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "$TAG: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
