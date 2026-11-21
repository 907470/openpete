/* games/common/renderer/sector_pvs.c - see sector_pvs.h. */
#include "renderer/sector_pvs.h"

#include <string.h>

/** @brief Walks one PVS node's sector bytes: the first strictly increasing run hidden, the rest visible, 0xFF bytes skipped.
 * @param b The tree bytes.
 * @param v The first sector byte's offset.
 * @param end The node's end offset.
 * @param vis The visibility table. */
static void spvs_node(const uint8_t* b, uint32_t v, uint32_t end, uint8_t vis[256]) {
    if (v < end) {
        uint32_t a = b[v];
        while (v != end && a != 0xFFu) {
            vis[a] = 0;
            const uint32_t prev = a;
            if (v + 1u >= end) { v++; break; }
            a = b[v + 1u];
            v++;
            if ((int32_t)prev - (int32_t)a >= 0) break;
        }
    }
    for (; v < end; v++)
        if (b[v] != 0xFFu) vis[b[v]] = 0xFF;
}

/** @brief Steps the descent's node coordinates one level down and returns the octant the position falls in.
 * @param p The position shifted right 11, x y z.
 * @param c The node coordinates, doubled and advanced into the octant.
 * @param depth The depth being left, 7 at the root.
 * @return The octant, x bit 4, y bit 2, z bit 1. */
static uint32_t spvs_octant(const uint32_t p[3], uint32_t c[3], int depth) {
    uint32_t oct = 0;
    for (int a = 0; a < 3; a++) {
        c[a] <<= 1;
        if ((int64_t)p[a] - (int64_t)((((uint64_t)c[a] + 1u) << depth) >> 1) >= 0) { c[a]++; oct |= 4u >> a; }
    }
    return oct;
}

/** @brief Returns the offset of a node's child in an octant: past the node's children offset, then past each lower present sibling's length byte.
 * @param b The tree bytes.
 * @param len Their length.
 * @param t The node's offset.
 * @param mask The node's child mask.
 * @param oct The octant, present in the mask.
 * @return The child's offset; past len when the bytes end. */
static uint32_t spvs_child(const uint8_t* b, uint32_t len, uint32_t t, uint32_t mask, uint32_t oct) {
    t += (uint32_t)b[t + 2u] | ((uint32_t)b[t + 3u] << 8);
    for (uint32_t i = 0; i < oct && t < len; i++)
        if (mask & (1u << i)) t += b[t];
    return t;
}

int sector_pvs_lookup(const uint8_t* b, uint32_t len, uint32_t t, const int32_t pos[3],
                      uint8_t vis[256], int* open) {
    memset(vis, 0xFF, 256);
    if (open) *open = 1;
    if (len < 4u || t > len - 4u
        || ((uint32_t)b[t] | ((uint32_t)b[t + 1u] << 8) | ((uint32_t)b[t + 2u] << 16)
            | ((uint32_t)b[t + 3u] << 24)) == 0xFFFFFFFFu)
        return 256;
    memset(vis, 0, 256);
    const uint32_t p[3] = { (uint32_t)pos[0] >> 11, (uint32_t)pos[1] >> 11, (uint32_t)pos[2] >> 11 };
    uint32_t c[3] = { 0, 0, 0 };
    for (int depth = 7; ; depth--) {
        if (t > len - 4u || b[t] == 0u || t + b[t] > len) { memset(vis, 0xFF, 256); return 256; }
        const uint32_t mask = b[t + 1u], end = t + b[t];
        spvs_node(b, t + (depth ? (mask ? 4u : 2u) : 1u), end, vis);
        if (depth == 0 || mask == 0) break;
        const uint32_t oct = spvs_octant(p, c, depth);
        if (!(mask & (1u << oct))) { memset(vis, 0xFF, 256); return 256; }
        t = spvs_child(b, len, t, mask, oct);
    }
    if (open) *open = 0;
    int n = 0;
    for (int i = 0; i < 256; i++) n += vis[i] == 0xFF;
    return n;
}

void sector_pvs_ring(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3],
                     uint8_t nb[SECTOR_PVS_RING][256], uint8_t open[SECTOR_PVS_RING]) {
    for (int k = 0; k < SECTOR_PVS_RING; k++) {
        const int d[3] = { k / 9 - 1, (k / 3) % 3 - 1, k % 3 - 1 };
        int32_t q[3];
        int out = 0;
        for (int a = 0; a < 3; a++) {
            q[a] = p[a] + d[a] * SECTOR_PVS_CELL;
            out |= q[a] < 0 || q[a] >= 128 * SECTOR_PVS_CELL;
        }
        int op = 1;
        if (!out) sector_pvs_lookup(b, len, tree, q, nb[k], &op);
        open[k] = (uint8_t)op;
        if (op) memset(nb[k], 0, 256);
    }
}

void sector_pvs_pairs(const uint8_t nb[SECTOR_PVS_RING][256], const uint8_t open[SECTOR_PVS_RING],
                      int open_agrees, uint32_t n, uint8_t out[256]) {
    if (n > 256u) n = 256u;
    for (int k = 0; k < SECTOR_PVS_CENTRE; k++) {
        const int o = SECTOR_PVS_RING - 1 - k;
        const int ao = open_agrees && open[k], bo = open_agrees && open[o];
        for (uint32_t i = 0; i < n; i++)
            if ((nb[k][i] && (nb[o][i] || bo)) || (nb[o][i] && ao)) out[i] = 0xFF;
    }
}
