/* games/common/renderer/moby_patch_list.h - the walk of a keyframe's
 * faceblock patch list, as each game's copy loop runs it.
 *
 * A list is a run of entries, each a header word, (cnt << 24) | the byte
 * offset into the high-poly faceblock with cnt in bits 24-30, followed by the
 * replacement words; the entry whose header word is <= 0 (signed) ends the
 * list, the only end the copy loops test. Spyro 1's loop tests the count
 * before each copy (r_moby.s:542-566), so an entry with count 0 copies
 * nothing and consumes no list word. Spyro 2's and Spyro 3's run the copy
 * before testing the count (S2 mobydraw.s 0x80044648-0x80044668, S3
 * mobydraw.s 0x80031268-0x80031288), so an entry with count 0 copies one
 * word and consumes one list word: the entry copies max(cnt, 1) words.
 *
 * Header-only, over a word reader and writer, so the walk is unit-tested on a
 * host array (tests/unit/moby_patch_list_test.c). */
#ifndef OPENPETE_MOBY_PATCH_LIST_H
#define OPENPETE_MOBY_PATCH_LIST_H

#include <stdint.h>

/* One word the apply replaced: its address and its original value. */
typedef struct { uint32_t addr, val; } moby_patch_save_t;

/* The word reader and writer the walk runs through, with their context. */
typedef struct {
    void* ctx;
    uint32_t (*read_word)(void* ctx, uint32_t addr);
    void (*write_word)(void* ctx, uint32_t addr, uint32_t val);
} moby_patch_mem_t;

/** @brief The words one entry copies, from its header word, as the game's copy loop runs it.
 *
 * @param head The entry's header word.
 * @param body_first Nonzero where the copy loop runs its body before testing
 *        the count (Spyro 2 and 3): a count of 0 copies one word.
 * @return The count in bits 24-30, or 1 for a count of 0 under body_first. */
static inline int moby_patch_entry_words(uint32_t head, int body_first) {
    const int cnt = (int)((head >> 24) & 0x7Fu);
    return (cnt == 0 && body_first) ? 1 : cnt;
}

/** @brief The byte span of guest memory a patch list writes, walked read-only.
 *
 * Stops where moby_patch_list_apply stops: after the terminating entry, or at
 * the entry that would take the words past max_words or the entries past
 * max_entries, which the apply rolls back.
 * @param m The word reader.
 * @param fb The high-poly faceblock the offsets address.
 * @param list The list's first header word.
 * @param body_first As moby_patch_entry_words.
 * @param max_words The most words a list may copy.
 * @param max_entries The most entries a list may hold.
 * @param lo Lowered to the span's first byte when an entry starts below it.
 * @param hi Raised to the span's end when an entry ends above it. */
static inline void moby_patch_list_span(const moby_patch_mem_t* m, uint32_t fb, uint32_t list,
                                        int body_first, int max_words, int max_entries,
                                        uint32_t* lo, uint32_t* hi) {
    uint32_t at = list;
    int n = 0, entries = 0;
    for (;;) {
        const uint32_t head = m->read_word(m->ctx, at);
        at += 4u;
        const uint32_t dst = fb + (head & 0xFFFFu);
        const int words = moby_patch_entry_words(head, body_first);
        if (n + words > max_words || ++entries > max_entries) break;
        n += words;
        at += 4u * (uint32_t)words;
        const uint32_t end = dst + 4u * (uint32_t)words;
        if (dst < *lo) *lo = dst;
        if (end > *hi) *hi = end;
        if ((int32_t)head <= 0) break;
    }
}

/** @brief Apply a patch list in place, saving each replaced word in forward order.
 *
 * Every entry applies; a list whose words exceed max_words or whose entries
 * exceed max_entries is rolled back (each saved word written back) and
 * applies none.
 * @param m The word reader and writer; the patched words are written.
 * @param fb The high-poly faceblock the offsets address.
 * @param list The list's first header word.
 * @param body_first As moby_patch_entry_words.
 * @param sv Receives (address, original) per replaced word, max_words entries.
 * @param max_words The most words a list may copy.
 * @param max_entries The most entries a list may hold.
 * @return The replaced word count, or -1 for a rolled-back list. */
static inline int moby_patch_list_apply(const moby_patch_mem_t* m, uint32_t fb, uint32_t list,
                                        int body_first, moby_patch_save_t* sv,
                                        int max_words, int max_entries) {
    uint32_t at = list;
    int n = 0, entries = 0;
    for (;;) {
        const uint32_t head = m->read_word(m->ctx, at);
        at += 4u;
        uint32_t dst = fb + (head & 0xFFFFu);
        const int words = moby_patch_entry_words(head, body_first);
        if (n + words > max_words || ++entries > max_entries) {
            for (int k = 0; k < n; k++) m->write_word(m->ctx, sv[k].addr, sv[k].val);
            return -1;
        }
        for (int q = 0; q < words; q++) {
            sv[n].addr = dst;
            sv[n].val = m->read_word(m->ctx, dst);
            m->write_word(m->ctx, dst, m->read_word(m->ctx, at));
            at += 4u; dst += 4u; n++;
        }
        if ((int32_t)head <= 0) break;
    }
    return n;
}

#endif /* OPENPETE_MOBY_PATCH_LIST_H */
