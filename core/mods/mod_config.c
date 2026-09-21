// core/mods/mod_config.c — per-mod user configuration (config.toml).
//
// Grown out of the spyro3 mod merge: one mod carrying several features
// (gems, chests, per-feature SFX/collision) needs a USER-editable
// on/off surface that isn't the manifest (mod.toml describes what the
// mod IS; config.toml describes what the user WANTS from it) and isn't
// env vars (they don't cross the WSL→Windows interop boundary — the
// .flame flag-file hack this replaces).
//
// Model: mods/<id>/config.toml, optional, parsed by the loader right
// before the mod's entry runs (boot AND hot reload — so a config edit
// in dev mode re-enters the mod with fresh values; the loader watches
// the file's content hash alongside the sources). Keys are addressed
// with dotted paths ("chests.collision" = key `collision` in table
// [chests]); values may be bool, integer, float, or string. Mods read
// them through api->config_* with a per-key default, so an absent file
// or key is always well-defined and a shipped config.toml documents
// the defaults instead of defining them.
//
// A parse error never disables the mod: it logs, and every getter
// returns its default (same posture as a missing file).

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/tomlc99/toml.h"
#include "mod_vec.h"

/* Slots the first block holds, and keys a slot allocates up front. Both
 * tables grow past these. */
#define CFG_SLOTS_BLOCK 64
#define CFG_KEYS_INIT   128
#define CFG_KEY_LEN  96
#define CFG_STR_LEN  128

typedef struct {
    char    key[CFG_KEY_LEN];
    char    type;           /* 'b' bool, 'i' int, 'd' double, 's' string */
    int64_t i;
    double  d;
    char    s[CFG_STR_LEN];
} cfg_entry_t;

typedef struct {
    const openpete_mod_t* mod;   /* slot owner (identity key) */
    mod_fvec e;                  /* cfg_entry_t */
} cfg_slot_t;

/* Chunked: a slot is keyed by its owner's address and its entry list
 * lives inside it, so a slot must not move once handed out. */
static mod_cvec s_slots;   /* cfg_slot_t */

static int slot_keys(const cfg_slot_t* s) {
    return (int)mod_fvec_count(&s->e);
}

static cfg_slot_t* slot_of(const openpete_mod_t* mod, int create) {
    for (size_t i = 0; i < mod_cvec_count(&s_slots); i++) {
        cfg_slot_t* c = (cfg_slot_t*)mod_cvec_at(&s_slots, i);
        if (c->mod == mod) return c;
    }
    if (!create) return NULL;
    if (!s_slots.elem_size)
        mod_cvec_init(&s_slots, sizeof(cfg_slot_t), CFG_SLOTS_BLOCK,
                      "cfg_slots", CFG_SLOTS_BLOCK);
    cfg_slot_t* s = (cfg_slot_t*)mod_cvec_push(&s_slots);
    if (!s) return NULL;
    mod_fvec_init(&s->e, sizeof(cfg_entry_t), CFG_KEYS_INIT, NULL, 0);
    s->mod = mod;
    return s;
}

/* Flatten one table into slot entries, prefixing nested tables with
 * "name." — arrays are logged and skipped. */
static void flatten(cfg_slot_t* s, const toml_table_t* tab,
                    const char* prefix, const char* log_tag) {
    for (int i = 0;; i++) {
        const char* k = toml_key_in(tab, i);
        if (!k) break;

        char full[CFG_KEY_LEN];
        if (prefix[0])
            snprintf(full, sizeof full, "%s.%s", prefix, k);
        else
            snprintf(full, sizeof full, "%s", k);

        toml_table_t* sub = toml_table_in(tab, k);
        if (sub) { flatten(s, sub, full, log_tag); continue; }
        if (toml_array_in(tab, k)) {
            OPL_WARN_F(log_tag, "config.toml: \"%s\" is an array — "
                       "unsupported, ignored", full);
            continue;
        }
        /* Built in a local and pushed only once the value typed: an
         * unsupported type leaves the list untouched. */
        cfg_entry_t tmp;
        cfg_entry_t* e = &tmp;
        memset(e, 0, sizeof *e);
        snprintf(e->key, sizeof e->key, "%s", full);

        toml_datum_t v;
        if ((v = toml_bool_in(tab, k)).ok)        { e->type = 'b'; e->i = v.u.b ? 1 : 0; e->d = (double)e->i; }
        else if ((v = toml_int_in(tab, k)).ok)    { e->type = 'i'; e->i = v.u.i; e->d = (double)v.u.i; }
        else if ((v = toml_double_in(tab, k)).ok) { e->type = 'd'; e->d = v.u.d; e->i = (int64_t)v.u.d; }
        else if ((v = toml_string_in(tab, k)).ok) {
            e->type = 's';
            snprintf(e->s, sizeof e->s, "%s", v.u.s);
            free(v.u.s);
        } else {
            OPL_WARN_F(log_tag, "config.toml: \"%s\" has an unsupported "
                       "value type — ignored", full);
            continue;
        }
        {
            cfg_entry_t* slot = (cfg_entry_t*)mod_fvec_push(&s->e);
            if (!slot) {
                OPL_ERROR_F(log_tag, "config.toml: key table allocation "
                            "failed — \"%s\" and the rest ignored", full);
                return;
            }
            *slot = tmp;
        }
    }
}

void openpete_mods_config_load(openpete_mod_t* mod, const char* mod_dir,
                               const char* log_tag) {
    cfg_slot_t* s = slot_of(mod, 1);
    if (!s) {
        OPL_ERROR_F("mods", "config slot allocation failed");
        return;
    }
    mod_fvec_clear(&s->e);   /* reload replaces (hot reload re-enters here) */

    char path[1200];
    snprintf(path, sizeof path, "%s/config.toml", mod_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return;   /* no config file: every getter returns defaults */

    char errbuf[256];
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof errbuf);
    fclose(fp);
    if (!root) {
        OPL_ERROR_F(log_tag, "config.toml parse error: %s — all keys "
                    "fall back to defaults", errbuf);
        return;
    }
    flatten(s, root, "", log_tag);
    toml_free(root);
    OPL_INFO_F(log_tag, "config.toml: %d key(s) loaded", slot_keys(s));
}

static const cfg_entry_t* find(openpete_mod_t* self, const char* key) {
    if (!self || !key) return NULL;
    const cfg_slot_t* s = slot_of(self, 0);
    if (!s) return NULL;
    for (int i = 0; i < slot_keys(s); i++) {
        cfg_entry_t* e = (cfg_entry_t*)mod_fvec_at(&s->e, (size_t)i);
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

int openpete_mods_config_bool(openpete_mod_t* self, const char* key,
                              int def) {
    const cfg_entry_t* e = find(self, key);
    if (!e) return def;
    if (e->type == 'b' || e->type == 'i') return e->i != 0;
    OPL_WARN_F("mods", "config key \"%s\" is not a bool — default used",
               key);
    return def;
}

int64_t openpete_mods_config_int(openpete_mod_t* self, const char* key,
                                 int64_t def) {
    const cfg_entry_t* e = find(self, key);
    if (!e) return def;
    if (e->type == 'i' || e->type == 'b' || e->type == 'd') return e->i;
    OPL_WARN_F("mods", "config key \"%s\" is not a number — default used",
               key);
    return def;
}

double openpete_mods_config_float(openpete_mod_t* self, const char* key,
                                  double def) {
    const cfg_entry_t* e = find(self, key);
    if (!e) return def;
    if (e->type == 'd' || e->type == 'i' || e->type == 'b') return e->d;
    OPL_WARN_F("mods", "config key \"%s\" is not a number — default used",
               key);
    return def;
}

int openpete_mods_config_str(openpete_mod_t* self, const char* key,
                             const char* def, char* buf, int cap) {
    if (!buf || cap <= 0) return 0;
    const cfg_entry_t* e = find(self, key);
    if (e && e->type == 's') {
        snprintf(buf, (size_t)cap, "%s", e->s);
        return 1;
    }
    if (e)
        OPL_WARN_F("mods", "config key \"%s\" is not a string — default "
                   "used", key);
    snprintf(buf, (size_t)cap, "%s", def ? def : "");
    return 0;
}

/* ── Comment-preserving writer (the Mods panel's Apply path) ─────────
 * config.toml is the USER's file — hand-written comments document what
 * each key means, so a rewrite that drops them is unacceptable. This
 * writer edits the file as text: for each edit it finds the dotted
 * path's section header and the `leaf = value` line inside it, and
 * replaces only the value span, keeping indentation and any trailing
 * comment. Missing keys append at the end of their section; missing
 * sections append at EOF. Scalars only — exactly the shapes the
 * [[config]] schema can declare. */

/* Headroom one edit can add to the text: a section header, the key and
 * its value, each bounded by the schema's own field sizes. */
#define CFGW_EDIT_GROWTH 1024

static void cfgw_format_value(char* out, size_t cap,
                              const openpete_mods_cfg_edit_t* e) {
    switch (e->type) {
    case 'b': snprintf(out, cap, "%s", e->ival ? "true" : "false"); break;
    case 'i': snprintf(out, cap, "%lld", (long long)e->ival); break;
    case 'd':
        snprintf(out, cap, "%g", e->fval);
        /* TOML requires a float to carry '.' or an exponent — "%g" of
         * 2.0 prints "2", which would silently retype the key. */
        if (!strpbrk(out, ".eE") && strlen(out) + 2 < cap)
            strcat(out, ".0");
        break;
    case 's': {
        size_t o = 0;
        if (o < cap - 1) out[o++] = '"';
        for (const char* c = e->sval ? e->sval : ""; *c && o < cap - 3; c++) {
            if (*c == '"' || *c == '\\') out[o++] = '\\';
            out[o++] = *c;
        }
        out[o++] = '"';
        out[o] = '\0';
        break;
    }
    default: out[0] = '\0'; break;
    }
}

/* Line starts of `text` are walked repeatedly; all spans are byte
 * offsets into the buffer. Returns 0 on success. */
static int cfgw_apply_one(char* text, size_t* len, size_t cap,
                          const openpete_mods_cfg_edit_t* e) {
    char section[96] = "", leaf[96];
    const char* dot = strrchr(e->key, '.');
    if (dot) {
        size_t sl = (size_t)(dot - e->key);
        if (sl >= sizeof section) return -1;
        memcpy(section, e->key, sl);
        section[sl] = '\0';
        snprintf(leaf, sizeof leaf, "%s", dot + 1);
    } else {
        snprintf(leaf, sizeof leaf, "%s", e->key);
    }

    char value[192];
    cfgw_format_value(value, sizeof value, e);

    /* Locate the section's span: [sec_start, sec_end). Root section is
     * everything before the first header. */
    size_t sec_start = 0, sec_end = *len;
    int in_target = section[0] == '\0'; /* root keys start in-section */
    size_t pos = 0;
    int found_section = in_target;
    while (pos < *len) {
        size_t eol = pos;
        while (eol < *len && text[eol] != '\n') eol++;
        const char* line = text + pos;
        size_t llen = eol - pos;
        /* A header line: optional ws, '[', name, ']' */
        size_t w = 0;
        while (w < llen && (line[w] == ' ' || line[w] == '\t')) w++;
        if (w < llen && line[w] == '[' && (w + 1 >= llen || line[w+1] != '[')) {
            char name[96] = "";
            size_t n = 0, q = w + 1;
            while (q < llen && line[q] != ']' && n < sizeof name - 1)
                name[n++] = line[q++];
            name[n] = '\0';
            if (in_target) { sec_end = pos; break; } /* target section ended */
            if (section[0] && strcmp(name, section) == 0) {
                in_target = 1;
                found_section = 1;
                sec_start = eol < *len ? eol + 1 : eol;
                sec_end = *len;
            }
        }
        pos = eol < *len ? eol + 1 : eol;
    }

    if (found_section) {
        /* Find `leaf =` inside [sec_start, sec_end). */
        size_t p2 = sec_start;
        while (p2 < sec_end) {
            size_t eol = p2;
            while (eol < sec_end && text[eol] != '\n') eol++;
            char* line = text + p2;
            size_t llen = eol - p2;
            size_t w = 0;
            while (w < llen && (line[w] == ' ' || line[w] == '\t')) w++;
            size_t kl = strlen(leaf);
            if (w + kl <= llen && memcmp(line + w, leaf, kl) == 0) {
                size_t a = w + kl;
                while (a < llen && (line[a] == ' ' || line[a] == '\t')) a++;
                if (a < llen && line[a] == '=') {
                    a++;
                    while (a < llen && (line[a] == ' ' || line[a] == '\t'))
                        a++;
                    /* Value span: quoted string or bare token, up to a
                     * trailing comment. */
                    size_t b = a;
                    if (b < llen && line[b] == '"') {
                        b++;
                        while (b < llen &&
                               !(line[b] == '"' && line[b-1] != '\\'))
                            b++;
                        if (b < llen) b++;
                    } else {
                        while (b < llen && line[b] != '#') b++;
                        while (b > a && (line[b-1] == ' ' ||
                                         line[b-1] == '\t'))
                            b--;
                    }
                    size_t vlen = strlen(value);
                    size_t old_span = b - a;
                    if (*len - old_span + vlen >= cap) return -1;
                    memmove(text + p2 + a + vlen, text + p2 + b,
                            *len - (p2 + b) + 1);
                    memcpy(text + p2 + a, value, vlen);
                    *len = *len - old_span + vlen;
                    return 0;
                }
            }
            p2 = eol < sec_end ? eol + 1 : eol;
        }
        /* Key missing: insert at the section's end (before the next
         * header), after trimming trailing blank lines of the span. */
        size_t ins = sec_end;
        while (ins > sec_start &&
               (text[ins-1] == '\n' || text[ins-1] == ' ' ||
                text[ins-1] == '\t')) {
            if (text[ins-1] == '\n' && ins >= 2 && text[ins-2] != '\n')
                break;
            ins--;
        }
        char add[320];
        int alen = snprintf(add, sizeof add, "%s = %s\n", leaf, value);
        if (alen < 0 || *len + (size_t)alen >= cap) return -1;
        memmove(text + ins + alen, text + ins, *len - ins + 1);
        memcpy(text + ins, add, (size_t)alen);
        *len += (size_t)alen;
        return 0;
    }

    /* Section missing: append it. */
    char add[448];
    int alen = snprintf(add, sizeof add, "%s%s[%s]\n%s = %s\n",
                        *len && text[*len-1] != '\n' ? "\n" : "",
                        *len ? "\n" : "", section, leaf, value);
    if (alen < 0 || *len + (size_t)alen >= cap) return -1;
    memcpy(text + *len, add, (size_t)alen + 1);
    *len += (size_t)alen;
    return 0;
}

int openpete_mods_config_write(const char* mod_dir, const char* log_tag,
                               int n, const openpete_mods_cfg_edit_t* e) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config.toml", mod_dir);

    /* The buffer is the file plus what the edits can append, so the
     * editable size is the file's own size. */
    mod_fvec buf;
    size_t cap = 0, len = 0;
    char* text;
    FILE* fp = fopen(path, "rb");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long end = ftell(fp);
            if (end > 0) cap = (size_t)end;
        }
        rewind(fp);
    }
    cap += (size_t)n * CFGW_EDIT_GROWTH + 1;
    mod_fvec_init(&buf, 1, cap, NULL, 0);
    if (mod_fvec_resize(&buf, cap) != 0) {
        if (fp) fclose(fp);
        mod_fvec_free(&buf);
        OPL_ERROR_F(log_tag, "config.toml: edit buffer allocation failed "
                    "— file left unchanged");
        return -1;
    }
    text = (char*)mod_fvec_data(&buf);
    if (fp) {
        len = fread(text, 1, cap - 1, fp);
        fclose(fp);
    }
    text[len] = '\0';

    for (int i = 0; i < n; i++) {
        if (cfgw_apply_one(text, &len, cap, &e[i]) != 0) {
            mod_fvec_free(&buf);
            OPL_ERROR_F(log_tag, "config.toml: failed to write \"%s\" — "
                        "file left unchanged", e[i].key);
            return -1;
        }
    }

    char tmp[1220];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        mod_fvec_free(&buf);
        OPL_ERROR_F(log_tag, "config.toml: cannot open %s for writing", tmp);
        return -1;
    }
    size_t wrote = fwrite(text, 1, len, out);
    fclose(out);
    mod_fvec_free(&buf);
    if (wrote != len) {
        remove(tmp);
        OPL_ERROR_F(log_tag, "config.toml: short write — original kept");
        return -1;
    }
#ifdef _WIN32
    remove(path); /* rename() refuses to clobber on Windows */
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        OPL_ERROR_F(log_tag, "config.toml: rename failed — original kept");
        return -1;
    }
    OPL_INFO_F(log_tag, "config.toml: %d key(s) written", n);
    return 0;
}
