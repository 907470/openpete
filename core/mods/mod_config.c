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

#define CFG_SLOTS    64   /* >= loader MODS_MAX */
#define CFG_KEYS_MAX 128
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
    cfg_entry_t e[CFG_KEYS_MAX];
    int n;
} cfg_slot_t;

static cfg_slot_t s_slots[CFG_SLOTS];
static int        s_slot_count;

static cfg_slot_t* slot_of(const openpete_mod_t* mod, int create) {
    for (int i = 0; i < s_slot_count; i++)
        if (s_slots[i].mod == mod) return &s_slots[i];
    if (!create || s_slot_count >= CFG_SLOTS) return NULL;
    cfg_slot_t* s = &s_slots[s_slot_count++];
    memset(s, 0, sizeof *s);
    s->mod = mod;
    return s;
}

/* Flatten one table into slot entries, prefixing nested tables with
 * "name." — arrays and >CFG_KEYS_MAX overflow are logged and skipped. */
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
        if (s->n >= CFG_KEYS_MAX) {
            OPL_WARN_F(log_tag, "config.toml: more than %d keys — "
                       "\"%s\" and the rest ignored", CFG_KEYS_MAX, full);
            return;
        }

        cfg_entry_t* e = &s->e[s->n];
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
        s->n++;
    }
}

void openpete_mods_config_load(openpete_mod_t* mod, const char* mod_dir,
                               const char* log_tag) {
    cfg_slot_t* s = slot_of(mod, 1);
    if (!s) {
        OPL_ERROR_F("mods", "config slots exhausted (%d)", CFG_SLOTS);
        return;
    }
    s->n = 0;   /* reload replaces (hot reload re-enters here) */

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
    OPL_INFO_F(log_tag, "config.toml: %d key(s) loaded", s->n);
}

static const cfg_entry_t* find(openpete_mod_t* self, const char* key) {
    if (!self || !key) return NULL;
    const cfg_slot_t* s = slot_of(self, 0);
    if (!s) return NULL;
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->e[i].key, key) == 0) return &s->e[i];
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
