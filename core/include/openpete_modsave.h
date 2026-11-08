/* openpete_modsave.h - per-mod save records: the game save descriptor and
 * the engine surface the card, savestate, runahead and dispatch layers call.
 *
 * A mod registers one record schema (openpete_mod_api::save_register). At
 * each non-empty save the game writes through openpete_memcard_write, the
 * engine asks every registered mod for its record and stores a snapshot in
 * `<card image>.modsave` keyed by save index, bound to the save by the
 * SHA-256 of its record as the card holds it. When the game loads a save
 * or starts a new playthrough, each mod whose held identity differs from
 * the live key's receives a read at the next entry of the game's update
 * function. The card image is never modified.
 *
 * Every game-specific fact is in openpete_save_desc_t; a game without one
 * has no save records (save_register returns -1).
 */
#ifndef OPENPETE_MODSAVE_H
#define OPENPETE_MODSAVE_H

#include <stddef.h>
#include <stdint.h>

#include "openpete_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/** Longest legacy span a descriptor may name, in bytes. */
#define OPENPETE_MODSAVE_LEGACY_MAX 256u

/** @brief The game save descriptor: every game-specific term of the save-record rules.
 *
 * A guest address of 0 names no function. Addresses are main-executable
 * addresses unless resident_overlay is set, in which case each *_ovl
 * member names the overlay id the address belongs to (0 = the executable).
 */
typedef struct openpete_save_desc {
    /** Card file names whose writes are captured; NULL-terminated. */
    const char* const* file_names;
    /** Records: record_count records of record_size bytes at
     *  record_base + index * record_size in the file. */
    uint32_t record_base, record_size, record_count;
    /** Hashed span within a record. */
    uint32_t hash_ofs, hash_len;
    /** Legacy span within a record (LEGACY_PAD); legacy_len 0 = none. */
    uint32_t legacy_ofs, legacy_len;
    /** 1 when the record is empty by the game's own predicate. */
    int (*is_empty)(const uint8_t* rec);
    /** 1 when the record passes the game's checksum verdict. */
    int (*checksum_ok)(const uint8_t* rec);
    /** 0: a write's issue and completion are one call (openpete_memcard_write). */
    int async_write;
    /** The game's record writer (Spyro 1: SaveCreate); with load_fn, the
     *  save functions the duplicate-override warning watches. 0 = none. */
    uint32_t save_create_fn;
    /** Load binding: the function whose first entry receives the record. */
    uint32_t load_fn;
    /** Guest address of the record at the load binding's entry. */
    uint32_t (*load_record)(struct CPUState* cpu);
    /** The game's chosen card slot and save index; 1 when it names one. */
    int (*chosen)(struct CPUState* cpu, int* slot, int* index);
    /** New-playthrough resets: entry of any listed function. */
    const uint32_t* reset_fns;
    size_t          reset_count;
    /** Delivery function: reads are delivered at its first entry. */
    uint32_t delivery_fn;
    /** The resident overlay id, or NULL when every address is in the
     *  executable. */
    uint32_t (*resident_overlay)(void);
    uint32_t load_ovl, delivery_ovl;
    const uint32_t* reset_ovls;  /* reset_count entries, or NULL */
} openpete_save_desc_t;

/** Nonzero while the save-record observer runs: any mod loaded at mods
 *  init, or a card with a `.modsave` mounted. One test on the dispatch path. */
extern int openpete_modsave_armed;

/** @brief Dispatch arm (behind openpete_modsave_armed): the reset, load-binding and delivery observers at a function's first entry.
 *
 * Runs above the mods arm, before any override; ignores the re-entry a
 * mod's base() makes (openpete_mods_bypass_pending).
 * @param cpu Guest CPU state.
 * @param addr The entered function's address. */
void openpete_modsave_dispatch(struct CPUState* cpu, uint32_t addr);

/** @brief The save_register api entry: record a mod's schema and callbacks.
 *
 * A second call from the same mod replaces its registration.
 * @param self The calling mod.
 * @param schema Its record format version, 1 or more.
 * @param write Its serializer.
 * @param read Its deliverer.
 * @return 0, or -1 for a NULL callback, schema 0 or a game with no descriptor. */
int openpete_modsave_register(openpete_mod_t* self, uint32_t schema,
                              openpete_mod_save_write_fn write,
                              openpete_mod_save_read_fn read);

/** @brief Arm, size the key's block and report duplicate save overrides; called once at the end of openpete_mods_init. */
void openpete_modsave_on_mods_init(void);

/** @brief Re-run the duplicate-override check after a deferred first entry. */
void openpete_modsave_on_entered(void);

/** @brief Drop a mod's save registration (hot reload, failed entry); its row in the key's block stays.
 * @param mod_id The mod's id. */
void openpete_modsave_remove_owner(const char* mod_id);

/* ── card layer (core/host/memcard.c) ── */

/** @brief A card image was loaded or provisioned: read its `.modsave`, arming when one exists.
 *
 * An unparseable sidecar is logged, renamed aside and treated as absent.
 * Under --adopt-saves the newest snapshot of every non-empty save takes the
 * running modset tag.
 * @param slot Card slot, 0 or 1.
 * @param image_path The image's resolved path.
 * @param adopt Nonzero under --adopt-saves. */
void openpete_modsave_card_mounted(int slot, const char* image_path, int adopt);

/** @brief Capture a synchronous file write after its flush.
 *
 * @param slot Card slot.
 * @param name The card file name.
 * @param file The file's first byte in the slot's card image.
 * @param file_len Bytes from @p file to the end of the image.
 * @param ofs Write offset in the file.
 * @param bytes Write length.
 * @param flushed 1 when the flush returned 0 (written, or suppressed on a
 *        speculative tick), 0 when it failed. */
void openpete_modsave_card_write(int slot, const char* name, const uint8_t* file,
                                 size_t file_len, int ofs, int bytes, int flushed);

/** @brief A frame write reached the slot; logged once as an outside edit when it lands in the save file's records with no write of the port pending.
 * @param slot Card slot.
 * @param frame Frame number. */
void openpete_modsave_card_frame_write(int slot, int frame);

/* ── BIOS file layer (core/psx/psx_bios.c), a descriptor with async_write ── */

/** @brief The issue of a bu file write: one pending entry for the port holding, per covered record, its index and, when non-empty, the lineage, the carried records and each enabled mod's write.
 *
 * Runs when the BIOS accepts the write (one write in flight per port). A
 * write covering no record leaves an entry with no record, so its
 * completion is not an outside edit. The entry travels with the key's
 * block through savestates, rewind and runahead.
 * @param port Card port, 0 or 1.
 * @param name The file name from the card directory.
 * @param src The bytes written, host view of the guest buffer.
 * @param ofs Write offset in the file.
 * @param bytes Write length. */
void openpete_modsave_bu_write_issue(int port, const char* name, const uint8_t* src, int ofs, int bytes);

/** @brief The end of a bu file write: on success each covered non-empty record must pass the checksum verdict in the card image, then its snapshot is stored and, when the generation is unchanged since issue, the key set; a failure or a failed verdict keeps nothing.
 *
 * A successful completion with no entry (a savestate taken before the
 * issue) is logged as an outside edit.
 * @param port Card port, 0 or 1.
 * @param name The file name, or NULL when unknown.
 * @param ok 1 for the operation-7 completion, 0 for a failure. */
void openpete_modsave_bu_write_done(int port, const char* name, int ok);

/** @brief The slot's card was formatted: drop every sidecar entry of the card.
 *
 * Queued on a speculative tick like every other sidecar change.
 * @param slot Card slot, 0 or 1. */
void openpete_modsave_card_format(int slot);

/** @brief Runahead speculation ended with its led ticks rolled back: discard the speculative queue. */
void openpete_modsave_spec_discard(void);

/** @brief Runahead speculation became the timeline: commit the speculative queue in order.
 *
 * A failed card flush at adoption drops the queue and logs it, since the
 * led ticks already reported success to the game.
 * @param flushed 1 when every marked card slot was flushed, 0 when one failed. */
void openpete_modsave_spec_commit(int flushed);

/* ── savestate trailer (core/runtime/game_runtime.c, core/record/savestate.c) ── */

/** @brief Return the byte size of the key's block trailer appended to the game-extra section.
 *
 * The block part is fixed per process once openpete_modsave_on_mods_init
 * sized the per-mod rows. On a game whose writes complete asynchronously
 * the pending write entries follow it (bytes, length, magic), so the size
 * grows while a write is in flight; snapshot buffers grow with it.
 * @return The trailer's size: the block, one row per parsed mod, length and magic, then any pending entries. */
uint32_t openpete_modsave_block_size(void);

/** @brief Write the key's block trailer: the block, its length, and the magic.
 *
 * @param dst Receives openpete_modsave_block_size() bytes. */
void openpete_modsave_block_save(void* dst);

/** @brief Restore the key's block from a game-extra section's tail.
 *
 * @param src The whole section.
 * @param size Its length.
 * @return The length of the game's own payload before the trailer; @p size
 *         when no trailer is present, after resetting the key to unknown. */
uint32_t openpete_modsave_block_load(const void* src, uint32_t size);

/** @brief Split a game-extra section into the game's own payload and a normalised copy of the save-record trailer.
 *
 * The copy holds the block header, the rows whose held identity is not
 * empty sorted by mod id (the header's row count rewritten to theirs), and
 * the pending-entry trailer as saved, so two sections from builds with
 * different installed mods compare equal when no mod holds an identity.
 * Without a block trailer the copy is the bytes after the payload, verbatim.
 * Reads only @p src; the module's state is untouched.
 * @param src The whole section.
 * @param size Its length.
 * @param out Receives the malloc'd copy, or NULL on allocation failure; free with free().
 * @param out_len Receives the copy's length.
 * @return The length of the game's own payload, as openpete_modsave_block_load returns it. */
uint32_t openpete_modsave_block_normalise(const void* src, uint32_t size, uint8_t** out, uint32_t* out_len);

/** @brief A savestate with no game-extra section was loaded: key unknown, held identities empty, generation kept. */
void openpete_modsave_block_absent(void);

/** @brief Serialise a slot's `.modsave` record set as the bytes the sidecar file holds, without writing it.
 * @param slot Card slot, 0 or 1.
 * @param out Receives the malloc'd bytes, or NULL when the slot's card has
 *        no record set; free with free().
 * @param len Receives their length, 0 for none.
 * @return 1 when bytes were produced, 0 when there is no record set, -1
 *         when allocation failed. */
int openpete_modsave_sidecar_bytes(int slot, uint8_t** out, size_t* len);

/** @brief After a savestate file load, rebind the key's card slot to the port holding the card the state held at that slot.
 * @param state_paths The state's card section: the image path each port held, "" for none. */
void openpete_modsave_rebind(const char* const state_paths[2]);

#ifdef __cplusplus
}
#endif

#endif
