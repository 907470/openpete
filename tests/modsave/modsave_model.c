/* modsave_model.c - the per-mod save-record rules as a pure state machine.
 *
 * See modsave_model.h for the domain, the state split and the effect
 * grammar. Every rule cites the design term it implements in its doxygen
 * comment; the interpretation of each point the design leaves open is
 * named Q<n> and listed in tests/modsave/model_ref.py OPEN_QUESTIONS.
 */
#include "modsave_model.h"

#include <stdio.h>
#include <string.h>

/** Calibration hook: a non-zero value seeds one rule defect (the search's
 *  --fault option); 0 in every real run. 1 swaps GAP and OK, 2 leaves the
 *  held identities unset at a write completion, 3 makes the
 *  new-playthrough reset keep the key, 4 reads mod m's status from the
 *  other mod's record. */
int msm_seed_fault = 0;

/* ── effect buffer ──────────────────────────────────────────────────────── */

typedef struct { char* p; size_t cap, len; int overflow; } fxbuf_t;

/** @brief Append one ';'-separated effect token. */
static void fx_put(fxbuf_t* fx, const char* tok) {
    const size_t n = strlen(tok) + (fx->len ? 1u : 0u);
    if (fx->len + n + 1u > fx->cap) { fx->overflow = 1; return; }
    if (fx->len) fx->p[fx->len++] = ';';
    memcpy(fx->p + fx->len, tok, strlen(tok) + 1u);
    fx->len += strlen(tok);
}

static const char* const k_status[] = { "OK", "ABSENT", "GAP", "STALE", "NEWER", "LEGACY_PAD" };
static const char* const k_qop[] = { "", "snap", "drop", "format" };

/** @brief 1 while a runahead speculation is open: host effects are deferred. */
static int spec_on(const msm_state_t* s) { return s->have_spec; }

/** @brief Log line, emitted on committed ticks only. */
static void fx_log(const msm_state_t* s, fxbuf_t* fx, const char* kind) {
    char t[48];
    if (spec_on(s)) return;
    snprintf(t, sizeof t, "L:%s", kind);
    fx_put(fx, t);
}

/** @brief Sidecar effect token S/Q<slot>.<idx>:<op>. */
static void fx_side(fxbuf_t* fx, char lead, int slot, int idx, const char* op) {
    char t[32];
    snprintf(t, sizeof t, "%c%d.%d:%s", lead, slot, idx, op);
    fx_put(fx, t);
}

/* ── small helpers ──────────────────────────────────────────────────────── */

/** @brief The next pending lineage (the first 128 bits of the SHA-256 of the consumed one). */
static msm_lin_t lin_next(msm_lin_t l) { l.n++; return l; }

/** @brief Lineage equality. */
static int lin_eq(msm_lin_t a, msm_lin_t b) { return a.root == b.root && a.n == b.n; }

/** @brief A lineage from OS entropy: a root no other lineage has. */
static msm_lin_t lin_entropy(msm_state_t* s) {
    msm_lin_t l = { ++s->next_root, 0 };
    return l;
}

/** @brief 1 when the content's legacy span is non-zero. */
static int pad_nonzero(uint8_t c) { return c == MSM_C_P; }

/** @brief Loaded, entered and enabled: the mod receives callbacks. */
static int mod_active(const msm_state_t* s, int m) {
    return (s->loaded >> m & s->entered >> m & s->enabled >> m) & 1;
}

/** @brief Loaded and entered: the mod called save_register. */
static int mod_registered(const msm_state_t* s, int m) {
    return (s->loaded >> m & s->entered >> m) & 1;
}

/** @brief The session tag: the mask of entered mods. */
static uint8_t session_tag(const msm_state_t* s) { return (uint8_t)(s->loaded & s->entered); }

/** @brief A block at process boot or after a block reset. */
static void block_boot(msm_state_t* s, msm_block_t* b) {
    memset(b, 0, sizeof *b);
    b->key = MSM_KEY_UNKNOWN;
    b->pend = lin_entropy(s);
}

/** @brief The current identity; returns 0 when the key is unknown. */
static int identity(const msm_block_t* b, msm_lin_t* lin, uint32_t* gen) {
    if (b->key == MSM_KEY_UNKNOWN) return 0;
    *lin = b->key == MSM_KEY_SET ? b->key_lin : b->pend;
    *gen = b->gen;
    return 1;
}

void msm_init(msm_state_t* s, const msm_config_t* cfg, uint8_t loaded_mask) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->loaded = s->entered = loaded_mask;
    s->enabled = 0xFF;
    for (int m = 0; m < MSM_MAX_MODS; m++) s->schema[m] = 1;
    s->armed = loaded_mask != 0;
    block_boot(s, &s->blk);
}

size_t msm_state_size(void) { return sizeof(msm_state_t); }

/* ── sidecar tables, the speculative queue and the view ────────────────── */

/** @brief Insert a captured snapshot at an index: an equal (hash, lineage) moves to the front, else it is pushed and the oldest past the cap evicted. */
static void table_put(const msm_config_t* cfg, msm_snaps_t* t, const msm_snap_t* sn,
                      fxbuf_t* fx, char lead, int slot, int idx) {
    for (int k = 0; k < t->n; k++) {
        if (t->s[k].content != sn->content || !lin_eq(t->s[k].lin, sn->lin)) continue;
        memmove(&t->s[1], &t->s[0], (size_t)k * sizeof t->s[0]);
        t->s[0] = *sn;
        if (fx) fx_side(fx, lead, slot, idx, "front");
        return;
    }
    const int keep = t->n < cfg->cap ? t->n : cfg->cap - 1;
    memmove(&t->s[1], &t->s[0], (size_t)keep * sizeof t->s[0]);
    t->s[0] = *sn;
    if (fx) fx_side(fx, lead, slot, idx, "put");
    if (t->n == cfg->cap && fx) fx_side(fx, lead, slot, idx, "evict");
    t->n = (uint8_t)(keep + 1);
}

/** @brief Apply one host op to a table; @p fx NULL applies it silently (lookup view). */
static void table_apply(const msm_config_t* cfg, msm_table_t* t, const msm_qop_t* op, fxbuf_t* fx) {
    if (op->kind == MSM_Q_FORMAT) {
        for (int i = 0; i < MSM_MAX_IDX; i++) t->idx[i].n = 0;
        if (fx) fx_side(fx, 'S', op->slot, 0, "format");
        return;
    }
    if (op->kind == MSM_Q_DROP) {
        t->idx[op->idx].n = 0;
        if (fx) fx_side(fx, 'S', op->slot, op->idx, "drop");
        return;
    }
    table_put(cfg, &t->idx[op->idx], &op->snap, fx, 'S', op->slot, op->idx);
}

/** @brief The snapshots lookups see at (slot, idx): the sidecar in memory with the speculative queue applied. */
static msm_snaps_t view(const msm_state_t* s, int slot, int idx) {
    msm_table_t t;
    if (s->sc_mem[slot].state == MSM_SC_PRESENT) t = s->sc_mem[slot];
    else memset(&t, 0, sizeof t);
    for (int q = 0; q < s->nq; q++)
        if (s->queue[q].slot == slot) table_apply(&s->cfg, &t, &s->queue[q], NULL);
    return t.idx[idx];
}

/** @brief The first .modsave of a card: one snapshot per non-empty index except @p skip, fresh lineage, no mod records, the card's .modset tag (Q4). */
static void seed_card(msm_state_t* s, int slot, int skip, fxbuf_t* fx) {
    msm_table_t* t = &s->sc_mem[slot];
    memset(t, 0, sizeof *t);
    t->state = MSM_SC_PRESENT;
    for (int i = 0; i < s->cfg.n_idx; i++) {
        const uint8_t c = s->card[slot][i];
        if (i == skip || c == MSM_C_E || c == MSM_C_T) continue;
        msm_snap_t sn;
        memset(&sn, 0, sizeof sn);
        sn.content = c;
        sn.tag = s->modset[slot] ? (uint8_t)(s->modset[slot] - 1) : 0;
        sn.lin = lin_entropy(s);
        t->idx[i].s[0] = sn;
        t->idx[i].n = 1;
        fx_side(fx, 'S', slot, i, "seed");
    }
}

/** @brief Write the sidecar file, or consume an injected fault (memory kept, retried at the next write). */
static void sidecar_write(msm_state_t* s, int slot, fxbuf_t* fx) {
    if (s->sc_fault) {
        s->sc_fault = 0;
        fx_log(s, fx, "sidecar-write-failed");
        return;
    }
    s->sc_disk[slot] = s->sc_mem[slot];
}

/** @brief Commit one host op to the sidecar: seed the card's first .modsave, apply, write the file. */
static void commit_op(msm_state_t* s, const msm_qop_t* op, fxbuf_t* fx) {
    if (s->sc_mem[op->slot].state != MSM_SC_PRESENT)
        seed_card(s, op->slot, op->kind == MSM_Q_FORMAT ? -1 : op->idx, fx);
    table_apply(&s->cfg, &s->sc_mem[op->slot], op, fx);
    sidecar_write(s, op->slot, fx);
}

/** @brief A host op: committed now, or queued on a speculative tick. */
static void host_op(msm_state_t* s, const msm_qop_t* op, fxbuf_t* fx) {
    if (!spec_on(s)) { commit_op(s, op, fx); return; }
    if (s->nq < MSM_MAX_QUEUE) s->queue[s->nq++] = *op;
    fx_side(fx, 'Q', op->slot, op->kind == MSM_Q_FORMAT ? 0 : op->idx, k_qop[op->kind]);
}

/** @brief The card flush after a write: committed ticks write the file and clear the rewind ring (Q10); led ticks mark the slot dirty. */
static void flush_card(msm_state_t* s, int slot) {
    if (spec_on(s)) { s->dirty[slot] = 1; return; }
    memcpy(s->disk[slot], s->card[slot], sizeof s->disk[slot]);
    s->have_rw = 0;
}

/* ── status and delivery ────────────────────────────────────────────────── */

/** @brief The status source: 2 = the snapshot matching the key's (hash, lineage), 1 = the lineage's newest at the index, 0 = none. */
static int status_source(const msm_state_t* s, const msm_block_t* b, msm_snap_t* out) {
    const msm_snaps_t v = view(s, b->key_slot, b->key_idx);
    for (int k = 0; k < v.n; k++)
        if (v.s[k].content == b->key_content && lin_eq(v.s[k].lin, b->key_lin)) { *out = v.s[k]; return 2; }
    for (int k = 0; k < v.n; k++)
        if (lin_eq(v.s[k].lin, b->key_lin)) { *out = v.s[k]; return 1; }
    return 0;
}

typedef struct { int status, schema, payload; } read_t;  /* payload -1 = no data, 'P' = legacy span */

/** @brief The Records status precedence for mod @p m: LEGACY_PAD, ABSENT, NEWER, STALE, GAP, OK. */
static read_t status_for(const msm_state_t* s, const msm_block_t* b, int m) {
    read_t r = { MSM_ABSENT, 0, -1 };
    msm_snap_t src;
    const int kind = b->key == MSM_KEY_SET ? status_source(s, b, &src) : 0;
    const msm_rec_t* rec = kind ? &src.rec[msm_seed_fault == 4 ? 1 - m : m] : NULL;
    if (!rec || !rec->present) {
        if (b->legacy) { r.status = MSM_LEGACY_PAD; r.payload = 'P'; }
        return r;
    }
    r.schema = rec->schema;
    if (rec->schema > s->schema[m]) { r.status = MSM_NEWER; return r; }
    r.payload = rec->payload;
    r.status = kind == 1 ? MSM_STALE : (rec->gap ? MSM_GAP : MSM_OK);
    if (msm_seed_fault == 1 && (r.status == MSM_GAP || r.status == MSM_OK))
        r.status = r.status == MSM_GAP ? MSM_OK : MSM_GAP;
    return r;
}

/** @brief The fixture's read callback: the arena value a read leaves (mod 0 imports its own legacy magic as 2). */
static uint8_t mod_reacts(int m, const read_t* r) {
    if (r->status == MSM_OK || r->status == MSM_GAP || r->status == MSM_STALE) return (uint8_t)r->payload;
    if (r->status == MSM_LEGACY_PAD && m == 0) return 2;
    return 0;
}

/** @brief Pick the resolved candidate of a pending load: the only one, or the game's choice among several; -1 to drop. */
static int load_pick(const msm_block_t* b) {
    const uint8_t c = b->pload_cands;
    if (c && !(c & (c - 1))) {
        int bit = 0;
        while (!(c >> bit & 1)) bit++;
        return bit;
    }
    if (b->pload_chosen == 0xFF) return -1;
    const int bit = (b->pload_chosen >> 4) * MSM_MAX_IDX + (b->pload_chosen & 15);
    return c >> bit & 1 ? bit : -1;
}

/** @brief Resolve the pending load at the delivery arm: the key names the save, the lineage of the matching snapshot, else the index's newest, else fresh. */
static void resolve_load(msm_state_t* s, fxbuf_t* fx) {
    msm_block_t* b = &s->blk;
    const int bit = load_pick(b);
    b->pload = 0;
    if (bit < 0) { fx_log(s, fx, "multimatch-dropped"); return; }
    const int slot = bit / MSM_MAX_IDX, idx = bit % MSM_MAX_IDX;
    const msm_snaps_t v = view(s, slot, idx);
    int match = -1;
    for (int k = 0; k < v.n && match < 0; k++) if (v.s[k].content == b->pload_content) match = k;
    msm_lin_t lin;
    if (match >= 0) lin = v.s[match].lin;
    else if (v.n) lin = v.s[0].lin;
    else { lin = b->pend; b->pend = lin_next(b->pend); }
    const int tk = match >= 0 ? match : (v.n ? 0 : -1);
    if (tk >= 0 && v.s[tk].tag != session_tag(s)) fx_log(s, fx, "mismatch");
    b->key = MSM_KEY_SET;
    b->key_slot = (uint8_t)slot; b->key_idx = (uint8_t)idx;
    b->key_content = b->pload_content; b->key_lin = lin;
    b->gen++;
    b->legacy = b->pload_legacy;
}

/** @brief The delivery arm (first entry of the delivery function): resolve a pending load, then read every active mod whose held identity differs. */
static void deliver(msm_state_t* s, fxbuf_t* fx) {
    if (!s->armed) return;
    if (s->blk.pload) resolve_load(s, fx);
    msm_block_t* b = &s->blk;
    msm_lin_t lin; uint32_t gen;
    if (!identity(b, &lin, &gen)) return;
    for (int m = 0; m < s->cfg.n_mods; m++) {
        if (!mod_active(s, m)) continue;
        if (b->held[m] && lin_eq(b->held_lin[m], lin) && b->held_gen[m] == gen) continue;
        const read_t r = status_for(s, b, m);
        char t[40], pay[8];
        if (r.payload < 0) snprintf(pay, sizeof pay, "-");
        else if (r.payload == 'P') snprintf(pay, sizeof pay, "P");
        else snprintf(pay, sizeof pay, "%d", r.payload);
        snprintf(t, sizeof t, "R%d:%s:%d:%s", m, k_status[r.status], r.schema, pay);
        fx_put(fx, t);
        b->arena[m] = mod_reacts(m, &r);
        b->held[m] = 1; b->held_lin[m] = lin; b->held_gen[m] = gen;
    }
}

/* ── write issue and completion ────────────────────────────────────────── */

/** @brief Carry-forward source: the snapshot whose (hash, lineage) equal the key's, else the lineage's newest (logged); 0 = none. */
static int carry_source(const msm_state_t* s, fxbuf_t* fx, msm_snap_t* out) {
    const msm_block_t* b = &s->blk;
    const msm_snaps_t v = view(s, b->key_slot, b->key_idx);
    for (int k = 0; k < v.n; k++)
        if (v.s[k].content == b->key_content && lin_eq(v.s[k].lin, b->key_lin)) { *out = v.s[k]; return 1; }
    for (int k = 0; k < v.n; k++)
        if (lin_eq(v.s[k].lin, b->key_lin)) { fx_log(s, fx, "carry-from-newest"); *out = v.s[k]; return 1; }
    return 0;
}

/** @brief One mod's record in a new entry: carried with gap when inactive, kept on a NEWER refusal or a -1 write, else the write's payload. */
static msm_rec_t entry_rec(msm_state_t* s, int m, const msm_rec_t* prev, uint8_t fails, fxbuf_t* fx) {
    msm_rec_t r = prev ? *prev : (msm_rec_t){ 0, 0, 0, 0 };
    if (!mod_active(s, m)) {
        if (r.present) r.gap = 1;
        return r;
    }
    if (r.present && r.schema > s->schema[m]) { fx_log(s, fx, "refuse-newer"); return r; }
    char t[8];
    snprintf(t, sizeof t, "W%d", m);
    fx_put(fx, t);
    if (fails >> m & 1) return r;
    msm_rec_t w = { 1, s->schema[m], 0, s->blk.arena[m] };
    return w;
}

/** @brief Write issue: decide the lineage (the key's when it names this save, else the pending lineage, consumed at completion: Q1), the carry source and refusals, call each write. */
static void issue(msm_state_t* s, int slot, int idx, uint8_t content, uint8_t fails, fxbuf_t* fx) {
    msm_block_t* b = &s->blk;
    msm_entry_t* e = &b->entry[slot];
    memset(e, 0, sizeof *e);
    e->used = 1; e->captured = 1;
    e->slot = (uint8_t)slot; e->idx = (uint8_t)idx; e->content = content; e->gen = b->gen;
    if (content == MSM_C_E) return;
    const int same = b->key == MSM_KEY_SET && b->key_slot == slot && b->key_idx == idx;
    msm_snap_t src;
    int have = 0;
    if (same) {
        e->lin = b->key_lin;
        have = carry_source(s, fx, &src);
    } else {
        if (b->key == MSM_KEY_UNKNOWN) fx_log(s, fx, "write-key-unknown");
        e->lin = b->pend;   /* consumed at completion (Q1) */
        e->fresh = 1;
    }
    for (int m = 0; m < s->cfg.n_mods; m++)
        e->rec[m] = entry_rec(s, m, have ? &src.rec[m] : NULL, fails, fx);
    e->tag = session_tag(s);
}

/** @brief The key, legacy copy and held identities after a successful completion whose generation is unchanged since issue. */
static void complete_key(msm_state_t* s, const msm_entry_t* e) {
    msm_block_t* b = &s->blk;
    if (b->gen != e->gen) return;
    b->key = MSM_KEY_SET;
    b->key_slot = e->slot; b->key_idx = e->idx; b->key_content = e->content; b->key_lin = e->lin;
    b->legacy = (uint8_t)pad_nonzero(e->content);
    if (e->fresh) b->pend = lin_next(e->lin);
    if (msm_seed_fault == 2) return;
    for (int m = 0; m < s->cfg.n_mods; m++) {
        if (!mod_active(s, m)) continue;
        b->held[m] = 1; b->held_lin[m] = e->lin; b->held_gen[m] = b->gen;
    }
}

/** @brief Write completion: 0 ok (card written, key and snapshot), 1 fail (nothing), 2 torn (card holds a torn record, no snapshot). */
static void complete(msm_state_t* s, int slot, int outcome, fxbuf_t* fx) {
    msm_entry_t e = s->blk.entry[slot];
    memset(&s->blk.entry[slot], 0, sizeof e);
    if (!e.used || outcome == 1) return;
    s->card[slot][e.idx] = outcome == 2 ? (uint8_t)MSM_C_T : e.content;
    flush_card(s, slot);
    if (outcome == 2 || !e.captured) return;
    msm_qop_t op;
    memset(&op, 0, sizeof op);
    op.slot = (uint8_t)slot; op.idx = e.idx;
    if (!e.lin.root) {
        op.kind = MSM_Q_DROP;
        host_op(s, &op, fx);
        return;
    }
    complete_key(s, &e);
    op.kind = MSM_Q_SNAP;
    op.snap.content = e.content; op.snap.tag = e.tag; op.snap.lin = e.lin;
    memcpy(op.snap.rec, e.rec, sizeof op.snap.rec);
    host_op(s, &op, fx);
}

/** @brief An issue while unarmed: an uncaptured entry that only writes the card at completion. */
static void issue_plain(msm_state_t* s, int slot, int idx, uint8_t content) {
    msm_entry_t* e = &s->blk.entry[slot];
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->slot = (uint8_t)slot; e->idx = (uint8_t)idx; e->content = content; e->gen = s->blk.gen;
}

/* ── tick events ───────────────────────────────────────────────────────── */

/** @brief The load binding's first entry: record the candidate set (every mounted image's equal record) and the legacy copy. */
static void bind(msm_state_t* s, int slot, int idx, uint8_t chosen) {
    const uint8_t c = s->card[slot][idx];
    if (!s->armed || c == MSM_C_T) return;
    msm_block_t* b = &s->blk;
    b->pload_cands = 0;
    for (int k = 0; k < s->cfg.n_slots; k++)
        for (int i = 0; i < s->cfg.n_idx; i++)
            if (s->mounted[k] && s->card[k][i] == c) b->pload_cands |= (uint8_t)(1u << (k * MSM_MAX_IDX + i));
    b->pload = 1;
    b->pload_legacy = (uint8_t)pad_nonzero(c);
    b->pload_content = c;
    b->pload_chosen = chosen;
}

/** @brief A synchronous game write (issue and completion in one call). */
static void ev_write(msm_state_t* s, const msm_event_t* ev, fxbuf_t* fx) {
    if (!s->armed) {
        s->card[ev->a][ev->b] = ev->c;
        flush_card(s, ev->a);
        return;
    }
    issue(s, ev->a, ev->b, ev->c, ev->d, fx);
    complete(s, ev->a, 0, fx);
}

/** @brief A write whose card flush fails: s_card keeps the record, nothing is captured (Q8). */
static void ev_write_fail(msm_state_t* s, const msm_event_t* ev, fxbuf_t* fx) {
    if (s->armed) {
        issue(s, ev->a, ev->b, ev->c, 0, fx);
        complete(s, ev->a, 1, fx);
    }
    s->card[ev->a][ev->b] = ev->c;
    fx_log(s, fx, "flush-failed");
}

/** @brief File create: one write of empty records over every index (Q9). */
static void ev_file_create(msm_state_t* s, int slot, fxbuf_t* fx) {
    for (int i = 0; i < s->cfg.n_idx; i++) {
        msm_event_t w = { MSM_EV_WRITE, (uint8_t)slot, (uint8_t)i, MSM_C_E, 0 };
        ev_write(s, &w, fx);
    }
}

/** @brief Format: every record empty on the card, every sidecar entry of the card dropped. */
static void ev_format(msm_state_t* s, int slot, fxbuf_t* fx) {
    memset(s->card[slot], MSM_C_E, sizeof s->card[slot]);
    flush_card(s, slot);
    if (!s->armed) return;
    msm_qop_t op;
    memset(&op, 0, sizeof op);
    op.kind = MSM_Q_FORMAT; op.slot = (uint8_t)slot;
    host_op(s, &op, fx);
}

/** @brief A new-playthrough reset: key cleared, generation advanced, legacy copy emptied. */
static void ev_reset(msm_state_t* s) {
    if (!s->armed) return;
    if (msm_seed_fault != 3) s->blk.key = MSM_KEY_CLEARED;
    s->blk.gen++;
    s->blk.legacy = 0;
}

/** @brief Dispatch a tick event's game action (after the delivery arm). */
static void tick_action(msm_state_t* s, const msm_event_t* ev, fxbuf_t* fx) {
    switch (ev->kind) {
    case MSM_EV_LOAD:        bind(s, ev->a, ev->b, (uint8_t)(ev->a << 4 | ev->b)); break;
    case MSM_EV_LOAD_STALE:  bind(s, ev->a, ev->b, 0xFF); break;
    case MSM_EV_RESET:       ev_reset(s); break;
    case MSM_EV_WRITE:       ev_write(s, ev, fx); break;
    case MSM_EV_WRITE_FAIL:  ev_write_fail(s, ev, fx); break;
    case MSM_EV_ISSUE:
        if (s->armed) issue(s, ev->a, ev->b, ev->c, ev->d, fx);
        else issue_plain(s, ev->a, ev->b, ev->c);
        break;
    case MSM_EV_COMPLETE:    complete(s, ev->a, ev->b, fx); break;
    case MSM_EV_FILE_CREATE: ev_file_create(s, ev->a, fx); break;
    case MSM_EV_FORMAT:      ev_format(s, ev->a, fx); break;
    case MSM_EV_PLAY:        s->blk.arena[ev->a] = ev->b; break;
    default: break;
    }
}

/* ── host events ───────────────────────────────────────────────────────── */

/** @brief A new process: the mod list, a boot block, cards reloaded from disk, no sidecar read yet. */
static void ev_restart(msm_state_t* s, uint8_t loaded, uint8_t entered) {
    s->loaded = loaded;
    s->entered = (uint8_t)(entered & loaded);
    s->enabled = 0xFF;
    block_boot(s, &s->blk);
    s->armed = loaded != 0;
    memset(s->mounted, 0, sizeof s->mounted);
    memcpy(s->card, s->disk, sizeof s->card);
    memset(s->dirty, 0, sizeof s->dirty);
    memset(s->sc_mem, 0, sizeof s->sc_mem);
    s->sc_fault = 0;
    s->nq = 0;
    s->have_rw = 0;
}

/** @brief First card access: read its sidecar (an unparseable one is set aside), arm when one exists. */
static void ev_mount(msm_state_t* s, int slot, fxbuf_t* fx) {
    s->mounted[slot] = 1;
    if (s->sc_disk[slot].state == MSM_SC_CORRUPT) {
        fx_log(s, fx, "sidecar-unparseable");
        memset(&s->sc_disk[slot], 0, sizeof s->sc_disk[slot]);
    }
    s->sc_mem[slot] = s->sc_disk[slot];
    if (s->sc_mem[slot].state == MSM_SC_PRESENT) s->armed = 1;
}

/** @brief First card access under --adopt-saves: the newest snapshot of every non-empty index takes the session tag. */
static void ev_mount_adopt(msm_state_t* s, int slot, fxbuf_t* fx) {
    ev_mount(s, slot, fx);
    msm_table_t* t = &s->sc_mem[slot];
    if (t->state != MSM_SC_PRESENT) return;
    int changed = 0;
    for (int i = 0; i < s->cfg.n_idx; i++) {
        const uint8_t c = s->card[slot][i];
        if (!t->idx[i].n || c == MSM_C_E || c == MSM_C_T || t->idx[i].s[0].tag == session_tag(s)) continue;
        t->idx[i].s[0].tag = session_tag(s);
        changed = 1;
    }
    if (!changed) return;
    fx_side(fx, 'S', slot, 0, "adopt");
    sidecar_write(s, slot, fx);
}

/** @brief 1 when a sidecar holds a snapshot at @p idx with record content @p c. */
static int table_holds(const msm_table_t* t, int idx, uint8_t c) {
    if (t->state != MSM_SC_PRESENT) return 0;
    for (int k = 0; k < t->idx[idx].n; k++) if (t->idx[idx].s[k].content == c) return 1;
    return 0;
}

/** @brief Port-rebind lookup after a savestate load: the key's slot becomes the port whose sidecar holds its hash at its index, else the key becomes unknown (Q5). */
static void rebind(msm_state_t* s, fxbuf_t* fx) {
    msm_block_t* b = &s->blk;
    if (!s->armed || b->key != MSM_KEY_SET) return;
    for (int k = 0; k < s->cfg.n_slots; k++) {
        const int c = (b->key_slot + k) % s->cfg.n_slots;
        const msm_table_t* t = s->mounted[c] ? &s->sc_mem[c] : &s->sc_disk[c];
        if (table_holds(t, b->key_idx, b->key_content)) { b->key_slot = (uint8_t)c; return; }
    }
    b->key = MSM_KEY_UNKNOWN;
    fx_log(s, fx, "rebind-unknown");
}

/** @brief Savestate load of a blob older than the block: key unknown, held empty, pending lineage re-minted, generation kept, arena from the state. */
static void ev_ss_load_old(msm_state_t* s) {
    const uint32_t gen = s->blk.gen;
    msm_block_t b;
    block_boot(s, &b);
    b.gen = gen;
    memcpy(b.arena, s->ss.arena, sizeof b.arena);
    s->blk = b;
    s->have_rw = 0;
}

/** @brief Restore the dirty slots' card images from disk and discard the speculative queue. */
static void restore_dirty(msm_state_t* s) {
    for (int c = 0; c < s->cfg.n_slots; c++)
        if (s->dirty[c]) memcpy(s->card[c], s->disk[c], sizeof s->card[c]);
    memset(s->dirty, 0, sizeof s->dirty);
    s->nq = 0;
}

/** @brief Runahead adoption: flush the dirty slots and commit the queue in order. */
static void ev_ra_adopt(msm_state_t* s, fxbuf_t* fx) {
    int wrote = 0;
    s->have_spec = 0;
    for (int c = 0; c < s->cfg.n_slots; c++) {
        if (!s->dirty[c]) continue;
        memcpy(s->disk[c], s->card[c], sizeof s->disk[c]);
        wrote = 1;
    }
    memset(s->dirty, 0, sizeof s->dirty);
    msm_qop_t q[MSM_MAX_QUEUE];
    const int n = s->nq;
    memcpy(q, s->queue, sizeof q);
    s->nq = 0;
    for (int i = 0; i < n; i++) commit_op(s, &q[i], fx);
    if (wrote) s->have_rw = 0;
}

/** @brief Runahead adoption whose card flush fails: the queue is dropped and logged (Q7). */
static void ev_ra_adopt_fail(msm_state_t* s, fxbuf_t* fx) {
    s->have_spec = 0;
    memset(s->dirty, 0, sizeof s->dirty);
    if (s->nq) fx_log(s, fx, "adoption-flush-failed");
    s->nq = 0;
}

/** @brief Runahead, savestate, rewind and fault-injection events. */
static void timeline_event(msm_state_t* s, const msm_event_t* ev, fxbuf_t* fx) {
    switch (ev->kind) {
    case MSM_EV_SS_TAKE:     s->ss = s->blk; s->have_ss = 1; break;
    case MSM_EV_SS_LOAD:     s->blk = s->ss; s->have_rw = 0; rebind(s, fx); break;
    case MSM_EV_SS_LOAD_OLD: ev_ss_load_old(s); break;
    case MSM_EV_RA_BEGIN:    s->spec = s->blk; s->have_spec = 1; break;
    case MSM_EV_RA_ROLLBACK: s->blk = s->spec; s->have_spec = 0; restore_dirty(s); break;
    case MSM_EV_RA_ADOPT:    ev_ra_adopt(s, fx); break;
    case MSM_EV_RA_ADOPT_FAIL: ev_ra_adopt_fail(s, fx); break;
    case MSM_EV_RA_PARTIAL:  s->have_spec = 0; restore_dirty(s); break;
    case MSM_EV_RW_MARK:     s->rw = s->blk; s->have_rw = 1; break;
    case MSM_EV_REWIND:      s->blk = s->rw; break;
    case MSM_EV_HANDOFF:
        s->ss = s->blk; s->have_ss = 1;
        ev_restart(s, ev->a, ev->b);
        s->blk = s->ss; s->have_rw = 0; rebind(s, fx);
        break;
    default: break;
    }
}

/** @brief Dispatch a host event (between ticks; no delivery arm). */
static void host_event(msm_state_t* s, const msm_event_t* ev, fxbuf_t* fx) {
    switch (ev->kind) {
    case MSM_EV_TOGGLE:      s->enabled ^= (uint8_t)(1u << ev->a); break;
    case MSM_EV_ENTER:       s->entered |= (uint8_t)(1u << ev->a); break;
    case MSM_EV_SCHEMA:      s->schema[ev->a] = (uint8_t)(3 - s->schema[ev->a]); break;
    case MSM_EV_RESTART:     ev_restart(s, ev->a, ev->b); break;
    case MSM_EV_MOUNT:       ev_mount(s, ev->a, fx); break;
    case MSM_EV_MOUNT_ADOPT: ev_mount_adopt(s, ev->a, fx); break;
    case MSM_EV_EDIT:        s->disk[ev->a][ev->b] = ev->c; break;
    case MSM_EV_SC_CORRUPT:  s->sc_disk[ev->a].state = MSM_SC_CORRUPT; break;
    case MSM_EV_SC_FAULT:    s->sc_fault = 1; break;
    case MSM_EV_MODSET:      s->modset[ev->a] = ev->b; break;
    default:                 timeline_event(s, ev, fx); break;
    }
}

/* ── preconditions ─────────────────────────────────────────────────────── */

/** @brief Preconditions of the tick events. */
static int tick_enabled(const msm_state_t* s, const msm_event_t* ev) {
    switch (ev->kind) {
    case MSM_EV_LOAD: case MSM_EV_LOAD_STALE:
        return s->mounted[ev->a] && s->card[ev->a][ev->b] != MSM_C_E;
    case MSM_EV_WRITE: case MSM_EV_WRITE_FAIL: case MSM_EV_ISSUE:
        return s->mounted[ev->a] && !s->blk.entry[ev->a].used;
    case MSM_EV_COMPLETE:    return s->blk.entry[ev->a].used;
    case MSM_EV_FILE_CREATE: case MSM_EV_FORMAT:
        return s->mounted[ev->a] && !s->blk.entry[ev->a].used;
    case MSM_EV_PLAY:        return mod_active(s, ev->a) && s->blk.arena[ev->a] != ev->b;
    default:                 return 1;
    }
}

/** @brief Preconditions of the host events. */
static int host_enabled(const msm_state_t* s, const msm_event_t* ev) {
    const int quiet = !s->have_spec;
    switch (ev->kind) {
    case MSM_EV_TOGGLE: case MSM_EV_SCHEMA: return mod_registered(s, ev->a);
    case MSM_EV_ENTER:  return (s->loaded >> ev->a & 1) && !(s->entered >> ev->a & 1);
    case MSM_EV_MOUNT: case MSM_EV_MOUNT_ADOPT: return !s->mounted[ev->a];
    case MSM_EV_SS_LOAD: case MSM_EV_SS_LOAD_OLD: return quiet && s->have_ss;
    case MSM_EV_RA_ROLLBACK: case MSM_EV_RA_ADOPT: case MSM_EV_RA_ADOPT_FAIL:
    case MSM_EV_RA_PARTIAL: return s->have_spec;
    case MSM_EV_REWIND: return quiet && s->have_rw;
    case MSM_EV_EDIT:   return quiet && s->disk[ev->a][ev->b] != ev->c;
    case MSM_EV_SC_CORRUPT: return quiet && s->sc_disk[ev->a].state == MSM_SC_PRESENT;
    case MSM_EV_SC_FAULT: return !s->sc_fault;
    case MSM_EV_MODSET: return quiet && s->modset[ev->a] != ev->b;
    default: return quiet; /* restart, handoff, savestate take, runahead begin, rewind mark */
    }
}

int msm_is_tick(uint8_t kind) { return kind <= MSM_EV_PLAY; }

int msm_enabled(const msm_state_t* s, const msm_event_t* ev) {
    if (ev->kind >= MSM_EV_COUNT) return 0;
    return msm_is_tick(ev->kind) ? tick_enabled(s, ev) : host_enabled(s, ev);
}

int msm_apply(msm_state_t* s, const msm_event_t* ev, char* out, size_t cap) {
    fxbuf_t fx = { out, cap, 0, 0 };
    if (cap) out[0] = '\0';
    if (msm_is_tick(ev->kind)) {
        deliver(s, &fx);
        tick_action(s, ev, &fx);
    } else {
        host_event(s, ev, &fx);
    }
    return fx.overflow ? -1 : 0;
}

const char* msm_event_name(int kind) {
    static const char* const names[MSM_EV_COUNT] = {
        "T", "LOAD", "LOAD_STALE", "RESET", "WRITE", "WRITE_FAIL", "ISSUE", "COMPLETE",
        "FILE_CREATE", "FORMAT", "PLAY", "TOGGLE", "ENTER", "SCHEMA", "RESTART", "MOUNT",
        "MOUNT_ADOPT", "SS_TAKE", "SS_LOAD", "SS_LOAD_OLD", "RA_BEGIN", "RA_ROLLBACK",
        "RA_ADOPT", "RA_ADOPT_FAIL", "RA_PARTIAL", "RW_MARK", "REWIND", "EDIT",
        "SC_CORRUPT", "SC_FAULT", "HANDOFF", "MODSET",
    };
    return kind >= 0 && kind < MSM_EV_COUNT ? names[kind] : NULL;
}

/* ── canonical form ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t root[64];
    uint32_t low[64];
    int      n;
    uint8_t* out;
    size_t   cap, len;
} canon_t;

/** @brief Record a lineage's root (first occurrence order) and its root's lowest counter. */
static void cn_see(canon_t* c, msm_lin_t l) {
    if (!l.root) return;
    for (int i = 0; i < c->n; i++) {
        if (c->root[i] != l.root) continue;
        if (l.n < c->low[i]) c->low[i] = l.n;
        return;
    }
    if (c->n < 64) { c->root[c->n] = l.root; c->low[c->n] = l.n; c->n++; }
}

/** @brief Append bytes to the canonical form. */
static void cn_put(canon_t* c, const void* p, size_t n) {
    if (c->len + n > c->cap) { c->len = c->cap + 1u; return; }
    memcpy(c->out + c->len, p, n);
    c->len += n;
}

/** @brief Append one byte. */
static void cn_u8(canon_t* c, unsigned v) { const uint8_t b = (uint8_t)v; cn_put(c, &b, 1); }

/** @brief Append a lineage renamed: (first-occurrence rank, counter minus the root's lowest). */
static void cn_lin(canon_t* c, msm_lin_t l) {
    uint32_t v[2] = { 0, 0 };
    for (int i = 0; l.root && i < c->n; i++)
        if (c->root[i] == l.root) { v[0] = (uint32_t)i + 1u; v[1] = l.n - c->low[i]; }
    cn_put(c, v, sizeof v);
}

/** @brief Visit every lineage a block holds. */
static void cn_see_block(canon_t* c, const msm_block_t* b) {
    if (b->key == MSM_KEY_SET) cn_see(c, b->key_lin);
    cn_see(c, b->pend);
    for (int m = 0; m < MSM_MAX_MODS; m++) if (b->held[m]) cn_see(c, b->held_lin[m]);
    for (int k = 0; k < MSM_MAX_SLOTS; k++) if (b->entry[k].used) cn_see(c, b->entry[k].lin);
}

/** @brief Visit every lineage a sidecar table holds. */
static void cn_see_table(canon_t* c, const msm_table_t* t) {
    if (t->state != MSM_SC_PRESENT) return;
    for (int i = 0; i < MSM_MAX_IDX; i++)
        for (int k = 0; k < t->idx[i].n; k++) cn_see(c, t->idx[i].s[k].lin);
}

/** @brief Append a snapshot. */
static void cn_snap(canon_t* c, const msm_snap_t* sn) {
    cn_u8(c, sn->content); cn_u8(c, sn->tag); cn_lin(c, sn->lin);
    for (int m = 0; m < MSM_MAX_MODS; m++) {
        const msm_rec_t* r = &sn->rec[m];
        cn_u8(c, r->present);
        if (r->present) { cn_u8(c, r->schema); cn_u8(c, r->gap); cn_u8(c, r->payload); }
    }
}

/** @brief Append a sidecar table. */
static void cn_table(canon_t* c, const msm_table_t* t) {
    cn_u8(c, t->state);
    if (t->state != MSM_SC_PRESENT) return;
    for (int i = 0; i < MSM_MAX_IDX; i++) {
        cn_u8(c, t->idx[i].n);
        for (int k = 0; k < t->idx[i].n; k++) cn_snap(c, &t->idx[i].s[k]);
    }
}

/** @brief Append a pending write entry; its generation as equality with the block's. */
static void cn_entry(canon_t* c, const msm_entry_t* e, uint32_t gen) {
    cn_u8(c, e->used);
    if (!e->used) return;
    cn_u8(c, e->captured); cn_u8(c, e->fresh); cn_u8(c, e->idx); cn_u8(c, e->content); cn_u8(c, e->tag);
    cn_u8(c, e->gen == gen);
    cn_lin(c, e->lin);
    for (int m = 0; m < MSM_MAX_MODS; m++) {
        cn_u8(c, e->rec[m].present);
        if (e->rec[m].present) { cn_u8(c, e->rec[m].schema); cn_u8(c, e->rec[m].gap); cn_u8(c, e->rec[m].payload); }
    }
}

/** @brief Append a block; held and entry generations as equality with the block's own. */
static void cn_block(canon_t* c, const msm_block_t* b) {
    cn_u8(c, b->key);
    if (b->key == MSM_KEY_SET) {
        cn_u8(c, b->key_slot); cn_u8(c, b->key_idx); cn_u8(c, b->key_content); cn_lin(c, b->key_lin);
    }
    cn_lin(c, b->pend);
    cn_u8(c, b->legacy);
    for (int m = 0; m < MSM_MAX_MODS; m++) {
        cn_u8(c, b->held[m]);
        if (b->held[m]) { cn_lin(c, b->held_lin[m]); cn_u8(c, b->held_gen[m] == b->gen); }
    }
    cn_u8(c, b->pload);
    if (b->pload) { cn_u8(c, b->pload_cands); cn_u8(c, b->pload_legacy); cn_u8(c, b->pload_content); cn_u8(c, b->pload_chosen); }
    for (int k = 0; k < MSM_MAX_SLOTS; k++) cn_entry(c, &b->entry[k], b->gen);
    cn_put(c, b->arena, sizeof b->arena);
}

/** @brief Visit every lineage in the state, in the canonical walk order. */
static void cn_see_all(canon_t* c, const msm_state_t* s) {
    cn_see_block(c, &s->blk);
    for (int k = 0; k < MSM_MAX_SLOTS; k++) cn_see_table(c, &s->sc_mem[k]);
    for (int k = 0; k < MSM_MAX_SLOTS; k++) cn_see_table(c, &s->sc_disk[k]);
    for (int q = 0; q < s->nq; q++) if (s->queue[q].kind == MSM_Q_SNAP) cn_see(c, s->queue[q].snap.lin);
    if (s->have_spec) cn_see_block(c, &s->spec);
    if (s->have_ss) cn_see_block(c, &s->ss);
    if (s->have_rw) cn_see_block(c, &s->rw);
}

/** @brief Append the host fields that hold no lineage. */
static void cn_host(canon_t* c, const msm_state_t* s) {
    cn_u8(c, s->loaded); cn_u8(c, s->entered & s->loaded);
    cn_u8(c, s->enabled & s->loaded & s->entered);
    cn_put(c, s->schema, sizeof s->schema);
    cn_u8(c, s->armed);
    cn_put(c, s->mounted, sizeof s->mounted);
    cn_put(c, s->card, sizeof s->card);
    cn_put(c, s->disk, sizeof s->disk);
    cn_put(c, s->dirty, sizeof s->dirty);
    cn_u8(c, s->sc_fault);
    cn_put(c, s->modset, sizeof s->modset);
}

size_t msm_canon(const msm_state_t* s, uint8_t* out, size_t cap) {
    canon_t c;
    memset(&c, 0, sizeof c);
    c.out = out; c.cap = cap;
    cn_see_all(&c, s);
    cn_block(&c, &s->blk);
    cn_host(&c, s);
    for (int k = 0; k < MSM_MAX_SLOTS; k++) cn_table(&c, &s->sc_mem[k]);
    for (int k = 0; k < MSM_MAX_SLOTS; k++) cn_table(&c, &s->sc_disk[k]);
    cn_u8(&c, s->nq);
    for (int q = 0; q < s->nq; q++) {
        cn_u8(&c, s->queue[q].kind); cn_u8(&c, s->queue[q].slot); cn_u8(&c, s->queue[q].idx);
        if (s->queue[q].kind == MSM_Q_SNAP) cn_snap(&c, &s->queue[q].snap);
    }
    cn_u8(&c, s->have_spec); if (s->have_spec) cn_block(&c, &s->spec);
    cn_u8(&c, s->have_ss);   if (s->have_ss) cn_block(&c, &s->ss);
    cn_u8(&c, s->have_rw);   if (s->have_rw) cn_block(&c, &s->rw);
    return c.len > cap ? 0 : c.len;
}
