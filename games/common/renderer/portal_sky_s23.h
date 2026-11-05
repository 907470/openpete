/* games/common/renderer/portal_sky_s23.h - the destination sky behind a
 * Spyro 2/3 homeworld portal's picture, read from the destination level's
 * WAD entry.
 *
 * A portal record's picture (the mesh header at the record's +0x3C pointer
 * plus 0x3C) is the destination level's cyclorama pre-projected into the 4:3
 * frame under one fixed camera per portal and cut to the region the 4:3 frame
 * shows. Every picture colour is an entry of the destination sky's colour
 * tables, and its vertices are the projections of sky vertices truncated to
 * integers. This module reads the destination's sky block host-side, recovers
 * the camera that projects it onto the picture, and returns the whole sky
 * projected under that camera, so the feeder can draw it beneath the picture
 * wherever the picture stops short of the displayed window. */
#ifndef OPENPETE_COMMON_RENDERER_PORTAL_SKY_S23_H
#define OPENPETE_COMMON_RENDERER_PORTAL_SKY_S23_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* One extension triangle in the picture's authored screen space, with the
 * destination sky's colours before any fade (0x00BBGGRR). */
typedef struct portal_sky_tri {
    float    x[3], y[3];
    uint32_t rgb[3];
} portal_sky_tri;

/* One portal's extension: the strip continuation (the first nstrip
 * triangles) then the destination sky, and the sky's background colour. */
typedef struct portal_sky_ext {
    const portal_sky_tri* tri;
    int                   n, nstrip;
    uint32_t              bg;   /* 0x00BBGGRR, 0 with no triangles */
} portal_sky_ext;

/** @brief Finds the sky block's payload in a level's data sub-file by the loader's block walk.
 *
 * Steps over portal_game_s23.level_wad_hops size-prefixed blocks, then over
 * the block whose size word sits at +0x0C, and takes the next block's
 * payload: three colour bytes and a pad, the sector count, and one offset
 * per sector relative to the payload. Every size is checked against the
 * sub-file. Reads the installed portal_game_s23 and the buffer only.
 * @param b The sub-file (sub-file 1 of the level's WAD entry).
 * @param n Its length.
 * @param len Receives the payload's length.
 * @return The payload's offset, or 0 when the chain leaves the sub-file. */
uint32_t portal_sky_s23_walk(const uint8_t* b, uint32_t n, uint32_t* len);

/** @brief The destination sky of one portal record, projected under the camera its picture was authored with.
 *
 * The destination is the level number at the record's +0x42; its sky block is
 * read from WAD entry level_wad_base + 2 * slot, sub-file 1, through the
 * loader's block chain, once per level per session. The camera is recovered
 * from the picture and the sky alone and cached per record, picture contents
 * and level, so a later call for the same portal returns the cached list. A
 * corner within 1.5 pixels of a picture vertex takes that vertex's position,
 * so the extension's faces meet the picture's exactly. A build that
 * portal_sky_s23_prefetch queued and the background thread has not finished
 * is waited for, and built on the calling thread when the background thread
 * has not started it, so the list never depends on the thread's progress.
 * Host reads only: no PSX RAM is written and the disc reads go through their
 * own handle.
 *
 * Ahead of the sky's triangles the list holds the continuation of the strip
 * a picture may carry below the sky's lowest ring (faces in colours the sky
 * does not carry): one quad per picture row and side whose outermost face is
 * such a face, in the picture's colour there, from far past the frame to the
 * frame's edge.
 * @param cpu Reads the record and its picture mesh.
 * @param rec The portal record.
 * @param out Receives the triangles (valid until a later call evicts the
 *        entry; the cache holds 32 portals), the strip's count, and the
 *        registered sky's background colour (0x00BBGGRR), the sky block
 *        header's three colour bytes, which the sky relocation copies to the
 *        frame's clear colour.
 * @return The triangle count; 0 when the destination's sky cannot be read
 *         or no camera registers it to the picture. */
int portal_sky_s23_extension(CPUState* cpu, uint32_t rec, portal_sky_ext* out) OP_REQUIRES(sim);

/** @brief Queues the extension of one portal record for the background build thread.
 *
 * Does nothing when an entry for the record, its picture pointer and its
 * destination exists, or its picture is not in RAM. Otherwise snapshots the
 * picture, reads the destination's sky (a disc read on the calling thread)
 * and queues the registration and build, which then run off the calling
 * thread. Host reads only.
 * @param cpu Reads the record and its picture mesh.
 * @param rec The portal record.
 * @return 1 when a build was queued, else 0. */
int portal_sky_s23_prefetch(CPUState* cpu, uint32_t rec) OP_REQUIRES(sim);

#endif /* OPENPETE_COMMON_RENDERER_PORTAL_SKY_S23_H */
