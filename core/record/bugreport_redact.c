/* core/record/bugreport_redact.c - the bug-report redactor: path tokens,
 * user and host names in text members, and the finding scanner for the
 * members that are never rewritten. See bugreport_internal.h. */
#include "bugreport_internal.h"

#include <SDL3/SDL_stdinc.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <limits.h>
  #include <sys/utsname.h>
  #include <unistd.h>
#endif

/** @brief Report whether a byte is a path separator. */
static int is_sep(unsigned char c) { return c == '/' || c == '\\'; }

/** @brief Report whether a byte ends a path token or starts the next one. */
static int is_delim(unsigned char c)
{
    return c <= ' ' || c == '"' || c == '\'' || c == '=' || c == ',' || c == ';' ||
           c == ':' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '<' || c == '>' || c == '|' || c == '`';
}

/** @brief Report whether a byte may belong to a word: an ASCII letter or digit, `_`, `-`, or any byte of a multi-byte UTF-8 sequence. */
static int is_name_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_' ||
           c == '-' || c >= 0x80;
}

/** @brief Return the length of the placeholder `<user>` or `<host>` at @p s, or 0. */
static size_t placeholder_at(const char* s, size_t n)
{
    if (n < 6 || s[0] != '<' || s[5] != '>') return 0;
    return !memcmp(s + 1, "user", 4) || !memcmp(s + 1, "host", 4) ? 6 : 0;
}

/** @brief Compare two bytes as the redactor does: separators equal, letters folded when asked. */
static int byte_eq(unsigned char a, unsigned char b, int fold)
{
    if (is_sep(a) && is_sep(b)) return 1;
    return fold ? tolower(a) == tolower(b) : a == b;
}

/** @brief Report whether @p s of length @p n starts with @p p of length @p pn under byte_eq. */
static int starts_with(const char* s, size_t n, const char* p, size_t pn, int fold)
{
    if (pn > n) return 0;
    for (size_t i = 0; i < pn; i++)
        if (!byte_eq((unsigned char)s[i], (unsigned char)p[i], fold)) return 0;
    return 1;
}

/** @brief Add one prefix form, dropping a trailing separator, a duplicate and an empty or root form.
 * @param r The redactor.
 * @param form The prefix.
 * @param repl Its replacement. */
static void prefix_add(br_redactor_t* r, const char* form, const char* repl)
{
    if (!form || r->npre >= BR_PREFIX_MAX) return;
    size_t n = strlen(form);
    while (n > 1 && is_sep((unsigned char)form[n - 1])) n--;
    if (n < 2) return;
    for (int i = 0; i < r->npre; i++)
        if (r->pre[i].len == n && starts_with(r->pre[i].form, n, form, n, r->fold_case)) return;
    char* c = (char*)malloc(n + 1);
    if (!c) return;
    memcpy(c, form, n);
    c[n] = '\0';
    r->pre[r->npre++] = (br_prefix_t){c, n, repl};
}

/** @brief Add a form and its copy with the other separator. */
static void prefix_add_both_seps(br_redactor_t* r, const char* form, const char* repl)
{
    if (!form || !*form) return;
    prefix_add(r, form, repl);
    char alt[1024];
    snprintf(alt, sizeof alt, "%s", form);
    for (char* p = alt; *p; p++) *p = *p == '\\' ? '/' : (*p == '/' ? '\\' : *p);
    prefix_add(r, alt, repl);
}

/** @brief Add a home form, its link-resolved path and (Windows) its 8.3 short name.
 * @param r The redactor.
 * @param form The home directory as one source spells it. */
static void home_add(br_redactor_t* r, const char* form)
{
    if (!form || !*form) return;
    prefix_add_both_seps(r, form, "~");
#ifdef _WIN32
    wchar_t w[1024], s[1024];
    if (MultiByteToWideChar(CP_ACP, 0, form, -1, w, 1024)) {
        char narrow[1024];
        if (GetShortPathNameW(w, s, 1024) &&
            WideCharToMultiByte(CP_ACP, 0, s, -1, narrow, sizeof narrow, NULL, NULL))
            prefix_add_both_seps(r, narrow, "~");
        if (GetLongPathNameW(w, s, 1024) &&
            WideCharToMultiByte(CP_ACP, 0, s, -1, narrow, sizeof narrow, NULL, NULL))
            prefix_add_both_seps(r, narrow, "~");
    }
#else
    char real[PATH_MAX];
    if (realpath(form, real)) prefix_add_both_seps(r, real, "~");
#endif
}

/** @brief Report whether a path lies inside one of the home forms already added. */
static int under_home(const br_redactor_t* r, const char* path)
{
    const size_t n = strlen(path);
    for (int i = 0; i < r->npre; i++)
        if (r->pre[i].repl[0] == '~' && starts_with(path, n, r->pre[i].form, r->pre[i].len,
                                                     r->fold_case) &&
            (n == r->pre[i].len || is_sep((unsigned char)path[r->pre[i].len])))
            return 1;
    return 0;
}

/** @brief Add a base directory from an environment variable when it lies outside home.
 * @param r The redactor.
 * @param var The variable.
 * @param repl Its replacement, the variable's spelling. */
static void base_add(br_redactor_t* r, const char* var, const char* repl)
{
    const char* v = getenv(var);
    if (!v || !*v || under_home(r, v)) return;
    prefix_add_both_seps(r, v, repl);
#ifndef _WIN32
    char real[PATH_MAX];
    if (realpath(v, real) && !under_home(r, real)) prefix_add_both_seps(r, real, repl);
#endif
}

/** @brief Add one host name form when it is new and at least two bytes long. */
static void host_add_one(br_redactor_t* r, const char* h, size_t n)
{
    if (n < 2 || n >= sizeof r->host[0] || r->nhost >= BR_NAME_MAX) return;
    for (int i = 0; i < r->nhost; i++)
        if (strlen(r->host[i]) == n && SDL_strncasecmp(r->host[i], h, n) == 0) return;
    memcpy(r->host[r->nhost], h, n);
    r->host[r->nhost++][n] = '\0';
}

/** @brief Add a host name and, for a dotted name, its first label. */
static void host_add(br_redactor_t* r, const char* h)
{
    if (!h) return;
    host_add_one(r, h, strlen(h));
    const char* dot = strchr(h, '.');
    if (dot) host_add_one(r, h, (size_t)(dot - h));
}

/** @brief Order prefixes longest first, so a longer form wins over its own prefix. */
static int prefix_cmp(const void* a, const void* b)
{
    const size_t x = ((const br_prefix_t*)a)->len, y = ((const br_prefix_t*)b)->len;
    return x < y ? 1 : (x > y ? -1 : 0);
}

/** @brief Add a redactor's given home forms, user name and host names. */
static void identity_add(br_redactor_t* r, const br_identity_t* id)
{
    for (size_t i = 0; i < sizeof id->home / sizeof id->home[0]; i++) home_add(r, id->home[i]);
    if (id->user && *id->user) snprintf(r->user, sizeof r->user, "%s", id->user);
    for (size_t i = 0; i < sizeof id->host / sizeof id->host[0]; i++) host_add(r, id->host[i]);
}

void br_redactor_init_identity(br_redactor_t* r, const br_identity_t* id)
{
    memset(r, 0, sizeof *r);
    r->fault_after = -1;
    r->fold_case = id->fold_case;
    identity_add(r, id);
    qsort(r->pre, (size_t)r->npre, sizeof r->pre[0], prefix_cmp);
}

void br_redactor_init(br_redactor_t* r)
{
    memset(r, 0, sizeof *r);
    r->fault_after = -1;
    br_identity_t id = {0};
#ifdef _WIN32
    r->fold_case = 1;
    char hp[1024] = "";
    const char* hd = getenv("HOMEDRIVE");
    const char* hpath = getenv("HOMEPATH");
    if (hd && hpath) snprintf(hp, sizeof hp, "%s%s", hd, hpath);
    id.home[0] = getenv("USERPROFILE");
    id.home[1] = hp[0] ? hp : NULL;
    id.home[2] = getenv("HOME");
    id.user = getenv("USERNAME");
    id.host[0] = getenv("COMPUTERNAME");
#else
    id.home[0] = getenv("HOME");
    id.user = getenv("USER");
    if (!id.user || !*id.user) id.user = getenv("LOGNAME");
    struct utsname un;
    if (uname(&un) == 0) id.host[0] = un.nodename;
    id.host[1] = getenv("COMPUTERNAME");
#endif
    id.host[2] = getenv("HOSTNAME");
    identity_add(r, &id);
#ifdef _WIN32
    base_add(r, "LOCALAPPDATA", "%LOCALAPPDATA%");
    base_add(r, "APPDATA", "%APPDATA%");
#endif
    base_add(r, "XDG_DATA_HOME", "$XDG_DATA_HOME");
    base_add(r, "XDG_CONFIG_HOME", "$XDG_CONFIG_HOME");
    base_add(r, "XDG_STATE_HOME", "$XDG_STATE_HOME");
    base_add(r, "XDG_CACHE_HOME", "$XDG_CACHE_HOME");
    if (!r->user[0] && r->npre > 0) {
        const char* h = r->pre[0].form;
        const char* last = h;
        for (const char* p = h; *p; p++) if (is_sep((unsigned char)*p) && p[1]) last = p + 1;
        snprintf(r->user, sizeof r->user, "%s", last);
    }
    qsort(r->pre, (size_t)r->npre, sizeof r->pre[0], prefix_cmp);
}

void br_redactor_free(br_redactor_t* r)
{
    for (int i = 0; i < r->npre; i++) free(r->pre[i].form);
    r->npre = 0;
}

/* A growable output buffer. */
typedef struct { char* b; size_t n, cap; int oom; long* fault; } obuf_t;

/** @brief Append bytes to an output buffer. */
static void ob_put(obuf_t* o, const char* s, size_t n)
{
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->n + n + 1) cap *= 2;
        /* The redactor's fault_after: the allocations that still succeed, or -1. */
        char* g = o->fault && *o->fault == 0 ? NULL : (char*)realloc(o->b, cap);
        if (o->fault && *o->fault > 0) (*o->fault)--;
        if (!g) { o->oom = 1; return; }
        o->b = g;
        o->cap = cap;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = '\0';
}

/** @brief Report whether a span equals the user name or a host name in any letter case; returns the replacement or NULL. */
static const char* name_repl(const br_redactor_t* r, const char* s, size_t n)
{
    if (r->user[0] && strlen(r->user) == n && SDL_strncasecmp(s, r->user, n) == 0) return "<user>";
    for (int i = 0; i < r->nhost; i++)
        if (strlen(r->host[i]) == n && SDL_strncasecmp(s, r->host[i], n) == 0) return "<host>";
    return NULL;
}

/** @brief Return the length of the user or host name of at least BR_NAME_WORD_MIN bytes that starts at @p s, in any letter case, and is not followed by a word byte; 0 when none.
 * @param r The redactor.
 * @param s The bytes.
 * @param n Their count from @p s.
 * @param what Receives the name's kind. */
static size_t name_at(const br_redactor_t* r, const char* s, size_t n, br_needle_kind_t* what)
{
    for (int k = -1; k < r->nhost; k++) {
        const char* nm = k < 0 ? r->user : r->host[k];
        const size_t ln = strlen(nm);
        if (ln < BR_NAME_WORD_MIN || ln > n || (ln < n && is_name_byte((unsigned char)s[ln])))
            continue;
        if (SDL_strncasecmp(s, nm, ln) != 0) continue;
        *what = k < 0 ? BRF_USER : BRF_HOST;
        return ln;
    }
    return 0;
}

/** @brief Write a path token's components, replacing a component equal to the user or a host name.
 *
 * A token with no separator is a component only when @p path says it came
 * from a path (a basename); a bare word is written unchanged.
 * @param r The redactor.
 * @param o The output.
 * @param s The token.
 * @param n Its length.
 * @param path Nonzero when the token is part of a path. */
static void put_components(br_redactor_t* r, obuf_t* o, const char* s, size_t n, int path)
{
    if (!path && !memchr(s, '/', n) && !memchr(s, '\\', n)) {
        ob_put(o, s, n);
        return;
    }
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && !is_sep((unsigned char)s[j])) j++;
        const char* rep = j > i ? name_repl(r, s + i, j - i) : NULL;
        if (rep) {
            ob_put(o, rep, strlen(rep));
            r->replacements++;
        } else {
            ob_put(o, s + i, j - i);
        }
        if (j < n) ob_put(o, s + j, 1);
        i = j + 1;
    }
}

/** @brief Report whether a token starts an absolute path: `/x`, `\\x`, `X:\` or `X:/`. */
static int abs_path_at(const char* s, size_t n)
{
    if (n >= 2 && is_sep((unsigned char)s[0]) && !is_delim((unsigned char)s[1])) return 1;
    return n >= 3 && isalpha((unsigned char)s[0]) && s[1] == ':' && is_sep((unsigned char)s[2]);
}

/** @brief Return the length of the token at @p s: up to the next delimiter, a drive colon included. */
static size_t token_len(const char* s, size_t n)
{
    size_t j = (n >= 2 && isalpha((unsigned char)s[0]) && s[1] == ':') ? 2 : 0;
    while (j < n && !is_delim((unsigned char)s[j])) j++;
    return j;
}

/** @brief Redact one token: a home or base prefix, an absolute path outside them, or a relative path's components.
 * @return The bytes consumed. */
static size_t redact_token(br_redactor_t* r, obuf_t* o, const char* s, size_t n)
{
    for (int k = 0; k < r->npre; k++) {
        const br_prefix_t* p = &r->pre[k];
        if (!starts_with(s, n, p->form, p->len, r->fold_case)) continue;
        if (p->len < n && !is_sep((unsigned char)s[p->len]) && !is_delim((unsigned char)s[p->len]))
            continue;
        ob_put(o, p->repl, strlen(p->repl));
        r->replacements++;
        const size_t rest = token_len(s + p->len, n - p->len);
        put_components(r, o, s + p->len, rest, 1);
        return p->len + rest;
    }
    const size_t t = token_len(s, n);
    if (abs_path_at(s, t)) {
        size_t end = t;
        while (end > 0 && is_sep((unsigned char)s[end - 1])) end--;
        size_t b = end;
        while (b > 0 && !is_sep((unsigned char)s[b - 1])) b--;
        if (b > 0) r->replacements++;
        put_components(r, o, s + b, end - b, 1);
        return t;
    }
    put_components(r, o, s, t, 0);
    return t ? t : 1;
}

/** @brief Replace a user or host name that is the whole value after the `key=` whose `=` is at @p in[i].
 * @return The bytes consumed from the `=`, or 0 when the value is no name. */
static size_t redact_key_value(br_redactor_t* r, obuf_t* o, const char* in, size_t i, size_t n)
{
    if (i == 0 || !is_name_byte((unsigned char)in[i - 1])) return 0;
    size_t j = i + 1;
    while (j < n && !is_delim((unsigned char)in[j]) && !is_sep((unsigned char)in[j])) j++;
    if (j == i + 1 || (j < n && !is_delim((unsigned char)in[j]))) return 0;
    const char* rep = name_repl(r, in + i + 1, j - i - 1);
    if (!rep) return 0;
    ob_put(o, "=", 1);
    ob_put(o, rep, strlen(rep));
    r->replacements++;
    return j - i;
}

/** @brief First pass: rewrite each path token (home and base prefixes, absolute paths, name components) and each name that is a whole `key=` value.
 * @param r The redactor.
 * @param in The text.
 * @param n Its length.
 * @param o The output. */
static void redact_paths(br_redactor_t* r, const char* in, size_t n, obuf_t* o)
{
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)in[i];
        if (!is_delim(c)) {
            i += redact_token(r, o, in + i, n - i);
            continue;
        }
        const size_t kv = c == '=' ? redact_key_value(r, o, in, i, n) : 0;
        if (!kv) ob_put(o, in + i, 1);
        i += kv ? kv : 1;
    }
}

/** @brief Second pass: replace every whole-word user or host name of BR_NAME_WORD_MIN bytes or more, wherever it stands, copying the placeholders as they are.
 * @param r The redactor.
 * @param in The first pass's output.
 * @param n Its length.
 * @param o The output. */
static void redact_names(br_redactor_t* r, const char* in, size_t n, obuf_t* o)
{
    for (size_t k = 0; k < n;) {
        const char* s = in + k;
        br_needle_kind_t what = BRF_USER;
        size_t len = placeholder_at(s, n - k);
        if (len) {
            ob_put(o, s, len);
        } else if ((k == 0 || !is_name_byte((unsigned char)s[-1])) &&
                   (len = name_at(r, s, n - k, &what)) != 0) {
            ob_put(o, what == BRF_USER ? "<user>" : "<host>", 6);
            r->replacements++;
        } else {
            ob_put(o, s, 1);
            len = 1;
        }
        k += len;
    }
}

char* br_redact_text(br_redactor_t* r, const char* in, size_t n, size_t* out_n)
{
    obuf_t paths = {.fault = &r->fault_after}, names = {.fault = &r->fault_after};
    ob_put(&paths, "", 0);
    redact_paths(r, in, n, &paths);
    if (!paths.oom) {
        ob_put(&names, "", 0);
        redact_names(r, paths.b, paths.n, &names);
    }
    free(paths.b);
    if (paths.oom || names.oom) { free(names.b); return NULL; }
    *out_n = names.n;
    return names.b;
}

/** @brief Return the length of the home form that starts at @p s, or 0.
 * @param r The redactor.
 * @param s The bytes.
 * @param n Their count from @p s. */
static size_t home_at(const br_redactor_t* r, const char* s, size_t n)
{
    for (int k = 0; k < r->npre; k++)
        if (r->pre[k].repl[0] == '~' && starts_with(s, n, r->pre[k].form, r->pre[k].len, r->fold_case))
            return r->pre[k].len;
    return 0;
}

size_t br_scan(const br_redactor_t* r, const uint8_t* d, size_t n,
               void (*hit)(void* ud, size_t off, br_needle_kind_t what), void* ud)
{
    size_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        const char* s = (const char*)d + i;
        br_needle_kind_t what = BRF_HOME;
        const size_t ph = placeholder_at(s, n - i);
        if (ph) { i += ph - 1; continue; }
        size_t len = home_at(r, s, n - i);
        if (!len && (i == 0 || !is_name_byte(d[i - 1]))) len = name_at(r, s, n - i, &what);
        if (!len) continue;
        if (hit) hit(ud, i, what);
        hits++;
        i += len - 1;
    }
    return hits;
}

int br_members_add(br_members_t* m, const char* name, uint8_t* data, size_t len, br_kind_t kind)
{
    if (m->n == m->cap) {
        const size_t cap = m->cap ? m->cap * 2 : 64;
        br_member_t* g = (br_member_t*)realloc(m->v, cap * sizeof *g);
        if (!g) { free(data); return -1; }
        m->v = g;
        m->cap = cap;
    }
    char* nm = strdup(name);
    if (!nm) { free(data); return -1; }
    m->v[m->n++] = (br_member_t){nm, data, len, kind};
    return 0;
}

int br_members_add_copy(br_members_t* m, const char* name, const void* data, size_t len,
                        br_kind_t kind)
{
    uint8_t* c = (uint8_t*)malloc(len ? len : 1);
    if (!c) return -1;
    if (len) memcpy(c, data, len);
    return br_members_add(m, name, c, len, kind);
}

void br_members_free(br_members_t* m)
{
    for (size_t i = 0; i < m->n; i++) { free(m->v[i].name); free(m->v[i].data); }
    free(m->v);
    memset(m, 0, sizeof *m);
}

int br_read_file(const char* path, uint8_t** out, size_t* len)
{
    *out = NULL;
    *len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, n = 0;
    uint8_t* b = (uint8_t*)malloc(cap + 1);
    while (b) {
        const size_t got = fread(b + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        cap *= 2;
        uint8_t* g = (uint8_t*)realloc(b, cap + 1);
        if (!g) { free(b); b = NULL; break; }
        b = g;
    }
    const int err = ferror(f);
    fclose(f);
    if (!b || err) { free(b); return -1; }
    b[n] = 0;
    *out = b;
    *len = n;
    return 0;
}

const void* br_memmem(const void* hay, size_t n, const void* needle, size_t k)
{
    if (!k) return hay;
    const unsigned char* h = (const unsigned char*)hay;
    const unsigned char first = *(const unsigned char*)needle;
    for (size_t i = 0; i + k <= n; i++)
        if (h[i] == first && !memcmp(h + i, needle, k)) return h + i;
    return NULL;
}
