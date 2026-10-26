// games/common/features/demo_egg.h - a title-screen demo launcher turned
// into a sound.
//
// Spyro 2 and Spyro 3 each carry a demo of another game on the disc (Crash
// Team Racing, Crash Bash) and a pad combination on the title screen that
// launches it: the title update calls a short run of shutdown functions
// and then a launcher that LoadExecs the demo's EXE. openpete cannot host
// a second EXE, so each of those calls is skipped when it comes from the
// launch branch, and a sample out of the demo's own sound data, decoded
// from the disc at boot, plays through the host voice path
// (openpete_mod_sound_play_pcm) instead. The branch is recognised by the
// resident overlay and the return address each of its jal instructions
// stores, so the same functions called from anywhere else run normally.
// The hooks write no guest memory.

#ifndef OPENPETE_DEMO_EGG_H
#define OPENPETE_DEMO_EGG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct disc_image;

/** @brief One call the launch branch makes: the callee and the return address its jal stores. */
typedef struct {
    uint32_t callee;   /**< Entry vaddr of the called function. */
    uint32_t ra;       /**< The jal's return address, inside the title overlay. */
} demo_egg_site_t;

/** @brief A sample located in a disc file: its ADPCM bytes and its playback rate. */
typedef struct {
    uint8_t* adpcm;    /**< malloc'd SPU-ADPCM body, freed by the engine. */
    uint32_t len;      /**< Byte length of adpcm. */
    uint32_t rate_hz;  /**< Playback rate in Hz. */
} demo_egg_sample_t;

/** @brief Locates a sample through its disc file's own tables. */
typedef int (*demo_egg_locate_fn)(struct disc_image* img, demo_egg_sample_t* out);

/** @brief One game's launch branch, its sample, and the engine's state for it. */
typedef struct {
    const char*            name;     /**< Log prefix and the sound's cache key. */
    uint32_t               overlay;  /**< Id of the title overlay that holds the branch. */
    const demo_egg_site_t* sites;    /**< The branch's calls. */
    uint32_t               n_sites;  /**< Entries in sites. */
    demo_egg_locate_fn     locate;   /**< The game's sample locator. */
    /* Engine state. */
    int16_t*               pcm;      /**< Decoded sample, NULL until decoded. */
    uint32_t               frames;   /**< Samples in pcm. */
    uint32_t               rate_hz;  /**< Playback rate of pcm. */
    int                    loaded;   /**< 1 once decode has been attempted. */
    unsigned               last_yield; /**< openpete_frame of the last branch call. */
    int                    seen;     /**< 1 once a branch call has been seen. */
    unsigned               plays;    /**< Presses that played the sample. */
} demo_egg_t;

/** @brief Opens the disc, runs the egg's locator and decodes the sample, once per process.
 *
 * Logs the decoded sample count, rate and duration at info level, or why
 * nothing was decoded (no demo on the disc, a table that does not hold the
 * expected sample). Later calls do nothing.
 * @param egg The game's egg.
 * @param disc_path The --disc path, or NULL. */
void demo_egg_load(demo_egg_t* egg, const char* disc_path);

/** @brief Skips a call of the launch branch and plays the sample on the first call of a press.
 *
 * A call is the branch's when the title overlay is resident and the
 * call's return address and callee name one of the egg's sites. Branch
 * calls whose openpete_frame lies within a few yields of the previous one
 * belong to the same held press; the first call of a press plays the
 * sample, except on a speculative tick.
 * @param egg The game's egg.
 * @param cpu The guest CPU state.
 * @param addr The entered function's vaddr.
 * @return 1 to skip the guest function, 0 to run it. */
int demo_egg_on_call(demo_egg_t* egg, struct CPUState* cpu, uint32_t addr);

/** @brief Reads a little-endian u16 from a byte buffer.
 *
 * @param p Two bytes.
 * @return The value. */
uint16_t demo_egg_le16(const uint8_t* p);

/** @brief Reads a little-endian u32 from a byte buffer.
 *
 * @param p Four bytes.
 * @return The value. */
uint32_t demo_egg_le32(const uint8_t* p);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DEMO_EGG_H */
