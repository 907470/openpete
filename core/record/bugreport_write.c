/* core/record/bugreport_write.c - the bug-report worker: collects the
 * press's copies and the files a reader needs, redacts and scans them,
 * hashes every member, and writes manifest.json and the zip. Runs on its
 * own thread; reads no guest or host state the press did not copy except
 * immutable facts (build id, module descriptor) and files. See
 * openpete_bugreport.h. */
#include "bugreport_internal.h"

#include "../host/sha256.h"
#include "../../third_party/tomlc99/toml.h"
#include "miniz.h"
#include "openpete_atomic_file.h"
#include "openpete_build_id.h"
#include "openpete_bugreport.h"
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_devui.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_host_trace.h"
#include "openpete_log.h"
#include "openpete_pace_log.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"
#include "openpete_state.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
#else
  #include <sys/utsname.h>
  #include <unistd.h>
#endif

/* Tails of the logs: the current run's and the previous run's. */
#define BR_LOG_TAIL (4u << 20)
#define BR_LOG_PREV_TAIL (2u << 20)
/* Findings kept for manifest.json; the count is exact beyond it. */
#define BR_FINDINGS_MAX 256

/* A growable text buffer. */
typedef struct { char* b; size_t n, cap; int oom; } wbuf_t;

/** @brief Append bytes to a buffer; an allocation failure marks it and stops it growing. */
static void wb_put(wbuf_t* w, const char* s, size_t n)
{
    if (w->oom) return;
    if (w->n + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        while (cap < w->n + n + 1) cap *= 2;
        char* g = (char*)realloc(w->b, cap);
        if (!g) { w->oom = 1; return; }
        w->b = g;
        w->cap = cap;
    }
    memcpy(w->b + w->n, s, n);
    w->n += n;
    w->b[w->n] = '\0';
}

/** @brief Append a NUL-terminated string of any length; NULL appends nothing. */
static void wb_puts(wbuf_t* w, const char* s)
{
    if (s) wb_put(w, s, strlen(s));
}

/** @brief Append formatted text to a buffer; one call's text is truncated to 4095 bytes. */
static void wb_printf(wbuf_t* w, const char* fmt, ...)
{
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) wb_put(w, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/** @brief Append a string as a JSON string literal. */
static void wb_json_str(wbuf_t* w, const char* s)
{
    wb_put(w, "\"", 1);
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; wb_put(w, e, 2); }
        else if (c < 0x20) wb_printf(w, "\\u%04x", c);
        else wb_put(w, (const char*)&c, 1);
    }
    wb_put(w, "\"", 1);
}

/** @brief Write a SHA-256 digest of a buffer as hex. */
static void sha_hex(const void* d, size_t n, char out[65])
{
    openpete_sha256_ctx c;
    uint8_t dig[32];
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, d, n);
    openpete_sha256_final(&c, dig);
    openpete_sha256_hex(dig, out);
}

/** @brief Write the SHA-256 of a file as hex, streaming it.
 * @return 0, or -1 when the file cannot be read ("" written). */
static int sha_file(const char* path, char out[65], uint64_t* size)
{
    out[0] = '\0';
    if (size) *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    static _Thread_local uint8_t buf[1 << 16];
    size_t n;
    uint64_t total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        openpete_sha256_update(&c, buf, n);
        total += n;
    }
    const int err = ferror(f);
    fclose(f);
    if (err) return -1;
    uint8_t dig[32];
    openpete_sha256_final(&c, dig);
    openpete_sha256_hex(dig, out);
    if (size) *size = total;
    return 0;
}

/** @brief Wait for another thread's answer up to a bound, then mark it absent.
 * @param a The answer's state.
 * @param t_end_ns The bound on SDL_GetTicksNS.
 * @return BR_DONE or BR_ABSENT. */
static int answer_wait(SDL_AtomicInt* a, uint64_t t_end_ns)
{
    for (;;) {
        const int v = SDL_GetAtomicInt(a);
        if (v != BR_PENDING) return v;
        if (SDL_GetTicksNS() >= t_end_ns && SDL_CompareAndSwapAtomicInt(a, BR_PENDING, BR_ABSENT))
            return BR_ABSENT;
        SDL_Delay(2);
    }
}

/** @brief Wait for the pump's device facts; withdraw the request at the bound unless the pump already holds it.
 * @param j The job.
 * @param t_end_ns The bound.
 * @return BR_DONE or BR_ABSENT. */
static int main_wait(br_job_t* j, uint64_t t_end_ns)
{
    for (;;) {
        if (SDL_GetAtomicInt(&j->main_facts) == BR_DONE) return BR_DONE;
        if (SDL_GetTicksNS() >= t_end_ns && br_main_withdraw(j)) {
            SDL_SetAtomicInt(&j->main_facts, BR_ABSENT);
            return BR_ABSENT;
        }
        SDL_Delay(2);
    }
}

/** @brief Add a staging file as a member and delete it; a missing file adds nothing.
 * @return 1 when added. */
static int add_stage_file(br_members_t* m, const br_job_t* j, const char* leaf, const char* name,
                          br_kind_t kind)
{
    char path[1400];
    openpete_paths_join(path, sizeof path, j->stage, leaf);
    uint8_t* d = NULL;
    size_t n = 0;
    if (br_read_file(path, &d, &n) != 0) return 0;
    remove(path);
    return br_members_add(m, name, d, n, kind) == 0;
}

/** @brief Write rows through a stream into a staging file, then add it as a member.
 * @return 1 when added. */
static int add_streamed(br_members_t* m, const br_job_t* j, const char* name, br_kind_t kind,
                        void (*fn)(FILE*, const br_job_t*))
{
    char path[1400], leaf[96];
    snprintf(leaf, sizeof leaf, "stream-%s", name);
    for (char* p = leaf; *p; p++) if (*p == '/') *p = '_';
    openpete_paths_join(path, sizeof path, j->stage, leaf);
    FILE* fp = fopen(path, "wb");
    if (!fp) return 0;
    fn(fp, j);
    fclose(fp);
    return add_stage_file(m, j, leaf, name, kind);
}

/** @brief Stream hashes.csv: the region-hash rows from the anchor to the press. */
static void stream_hashes(FILE* fp, const br_job_t* j)
{
    openpete_bugrec_hash_csv_rows(fp, j->hash_rows, j->hash_n, j->hash_stride);
}

/** @brief Stream input-events.csv: the raw input ring at the press. */
static void stream_input(FILE* fp, const br_job_t* j)
{
    openpete_bugrec_input_csv_entries(fp, j->input, j->input_n);
}

/** @brief Add a file's last @p tail bytes as a member, starting at a line boundary.
 * @return 1 when added. */
static int add_tail(br_members_t* m, const char* path, size_t tail, const char* name)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (size < 0) { fclose(f); return 0; }
    const long start = (size_t)size > tail ? size - (long)tail : 0;
    fseek(f, start, SEEK_SET);
    const size_t n = (size_t)(size - start);
    uint8_t* d = (uint8_t*)malloc(n + 1);
    const size_t got = d ? fread(d, 1, n, f) : 0;
    fclose(f);
    if (!d) return 0;
    size_t skip = 0;
    if (start > 0) while (skip < got && d[skip] != '\n') skip++;
    if (skip < got && start > 0) skip++;
    memmove(d, d + skip, got - skip);
    d[got - skip] = 0;
    return br_members_add(m, name, d, got - skip, BRM_TEXT) == 0;
}

/** @brief Add a whole file as a member.
 * @return 1 when added. */
static int add_file(br_members_t* m, const char* path, const char* name, br_kind_t kind)
{
    uint8_t* d = NULL;
    size_t n = 0;
    if (br_read_file(path, &d, &n) != 0) return 0;
    return br_members_add(m, name, d, n, kind) == 0;
}

/** @brief Return the last component of a path. */
static const char* base_name(const char* p)
{
    const char* b = p;
    for (const char* c = p; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

/** @brief Copy the text after a marker up to the first of the given terminators or the line's end.
 * @param line The text searched (one line or more).
 * @param marker The marker.
 * @param ends Terminators, NULL-ended; the earliest one found ends the value.
 * @param out Receives the value.
 * @param cap Capacity of @p out.
 * @return 1 when the marker was found and the value is not empty. */
static int value_after(const char* line, const char* marker, const char* const* ends, char* out, size_t cap)
{
    const char* v = strstr(line, marker);
    if (!v) return 0;
    v += strlen(marker);
    const char* e = v + strcspn(v, "\r\n");
    for (int i = 0; ends && ends[i]; i++) {
        const char* t = br_memmem(v, (size_t)(e - v), ends[i], strlen(ends[i]));
        if (t && t < e) e = t;
    }
    const size_t n = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return n > 0;
}

/** @brief Return the start of the last line of a text that holds a needle, or NULL.
 * @param txt The text, NUL-terminated.
 * @param needle The bytes sought. */
static const char* last_line_with(const char* txt, const char* needle)
{
    const char* hit = NULL;
    for (const char* at = txt; (at = strstr(at, needle)) != NULL; at++) hit = at;
    if (!hit) return NULL;
    while (hit > txt && hit[-1] != '\n') hit--;
    return hit;
}

/** @brief Add the files a fatal exit of the previous run left, named by its log: the hang dump and the Windows minidump.
 *
 * The watchdog's line names the paths it wrote ("wrote trace=<path>
 * ram=<path>"), which OPENPETE_HANG_TRACE / OPENPETE_HANG_RAM may have
 * moved; the crash report names its minidump ("minidump: <path>"). The
 * last such line of the previous log decides; a hang line without paths
 * falls back to the default dump paths.
 * @param m The members.
 * @param prev The previous run's log tail. */
static void add_prev_fatal(br_members_t* m, const char* prev)
{
    static const char* const trace_end[] = {" ram=", NULL};
    static const char* const ram_end[] = {" - exiting", " (ram written", NULL};
    char path[1100];
    const char* hang = last_line_with(prev, "hang detected");
    if (hang) {
        if (!value_after(hang, "trace=", trace_end, path, sizeof path))
            openpete_hang_output_path("hang-trace.bin", path, sizeof path);
        add_file(m, path, "hang/prev-hang-trace.bin", BRM_BINARY);
        if (!value_after(hang, " ram=", ram_end, path, sizeof path))
            openpete_hang_output_path("hang-ram.bin", path, sizeof path);
        add_file(m, path, "hang/prev-hang-ram.bin", BRM_GUEST);
    }
    const char* dmp = last_line_with(prev, "minidump: ");
    if (dmp && value_after(dmp, "minidump: ", NULL, path, sizeof path))
        add_file(m, path, "crash/prev-openpete-crash.dmp", BRM_BINARY);
}

/** @brief Add the logs: the current log's tail (after a barrier, so the press's lines are in it), the previous run's with the fatal-exit files it names, and at a fatal exit this run's hang dump.
 * @param m The members.
 * @param j The job. */
static void add_logs(br_members_t* m, const br_job_t* j)
{
    if (j->fatal) {
        char path[1100];
        openpete_hang_output_path("hang-trace.bin", path, sizeof path);
        add_file(m, path, "hang/hang-trace.bin", BRM_BINARY);
        openpete_hang_output_path("hang-ram.bin", path, sizeof path);
        add_file(m, path, "hang/hang-ram.bin", BRM_GUEST);
    }
    if (!j->log_path[0]) return;
    (void)openpete_log_file_barrier(500);
    add_tail(m, j->log_path, BR_LOG_TAIL, "log.txt");
    char prev[1100];
    snprintf(prev, sizeof prev, "%s.1", j->log_path);
    if (!add_tail(m, prev, BR_LOG_PREV_TAIL, "log-prev.txt")) return;
    add_prev_fatal(m, (const char*)m->v[m->n - 1].data);
}

/** @brief Add the thumbnail ring as thumbs/NNN.png, oldest first, encoded as PNG.
 * @param m The members.
 * @param j The job. */
static void add_thumbs(br_members_t* m, const br_job_t* j)
{
    for (size_t i = 0; i < j->thumbs_n; i++) {
        const openpete_bugthumb_t* t = &j->thumbs[i];
        size_t len = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(t->rgba, t->w, t->h, 4, &len,
                                                              MZ_BEST_SPEED, MZ_FALSE);
        if (!png) continue;
        char name[32];
        snprintf(name, sizeof name, "thumbs/%03zu.png", i);
        br_members_add_copy(m, name, png, len, BRM_PNG);
        mz_free(png);
    }
}

/** @brief Ask the tester for a note through the overlay's prompt with the sim paused, and wait for the answer.
 *
 * Skipped at once when the press did not ask for a note or no text input
 * exists (no overlay, neither keyboard nor on-screen keyboard); skipped
 * after BR_NOTE_SHOW_MS when no frame drew the prompt, or when no keyboard
 * is attached and no on-screen keyboard showed. Otherwise waits without a
 * bound for Enter or Escape, or for the session close, which ends the
 * prompt. The sim is paused for the prompt when it was running and resumed
 * after. Sets note_state and, on Enter, note.
 * @param j The job. */
static void note_wait(br_job_t* j)
{
#define NOTE_STATE(s) snprintf(j->note_state, sizeof j->note_state, "%s", (s))
    if (!j->note_wanted) { NOTE_STATE("off"); return; }
    if (!openpete_devui_note_begin()) { NOTE_STATE("no text input"); return; }
    const int paused_here = !openpete_sim_paused();
    if (paused_here) openpete_sim_pause_request(1);
    const uint64_t t0 = SDL_GetTicksNS();
    char text[OPENPETE_DEVUI_NOTE_MAX];
    for (;;) {
        int kb = 0, osk = 0;
        const int st = openpete_devui_note_poll(text, sizeof text, &kb, &osk);
        const int late = SDL_GetTicksNS() - t0 >= (uint64_t)BR_NOTE_SHOW_MS * 1000000u;
        if (st == OPENPETE_DEVUI_NOTE_SAVED) {
            NOTE_STATE(text[0] ? "saved" : "empty");
            if (text[0]) j->note = strdup(text);
            break;
        }
        if (st == OPENPETE_DEVUI_NOTE_SKIPPED) { NOTE_STATE("skipped"); break; }
        if (st == OPENPETE_DEVUI_NOTE_IDLE) { NOTE_STATE("closed at exit"); break; }
        if (late && st == OPENPETE_DEVUI_NOTE_REQUESTED) { NOTE_STATE("not shown"); break; }
        if (late && !kb && !osk) { NOTE_STATE("no on-screen keyboard"); break; }
        SDL_Delay(5);
    }
    openpete_devui_note_end();
    if (paused_here) openpete_sim_pause_request(0);
    OPL_INFO_F("bugreport", "note: %s after %llu ms%s", j->note_state,
               (unsigned long long)((SDL_GetTicksNS() - t0) / 1000000u),
               paused_here ? " (sim paused for it)" : "");
#undef NOTE_STATE
}

/** @brief Build device.txt: the pacing environment block, the display owner's Vulkan report, the main thread's monitors and drivers, and the OS, CPU and memory facts with the allowlisted environment variables. */
static char* device_text(const br_job_t* j)
{
    wbuf_t w = {0};
    wb_puts(&w, "[pace-env]\n");
    wb_puts(&w, j->pace_env);
    wb_puts(&w, "\n");
    wb_printf(&w, "[vulkan]\n");
    const char* v = j->display_txt ? strstr(j->display_txt, "vulkan: ") : NULL;
    wb_puts(&w, v ? v : "(display report absent)");
    wb_puts(&w, "\n");
    wb_puts(&w, "[displays]\n");
    wb_puts(&w, j->main_txt ? j->main_txt : "(main-thread facts absent)\n");
    wb_puts(&w, "\n");
    wb_printf(&w, "[system]\nplatform=%s\nlogical_cpus=%d\nram_mib=%d\n", SDL_GetPlatform(),
              SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
#ifndef _WIN32
    struct utsname un;
    if (uname(&un) == 0)
        wb_printf(&w, "os=%s\nkernel=%s\nkernel_version=%s\nmachine=%s\n", un.sysname, un.release,
                  un.version, un.machine);
    FILE* f = fopen("/proc/cpuinfo", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "model name", 10)) { wb_printf(&w, "cpu=%s", strchr(line, ':') ? strchr(line, ':') + 2 : line); break; }
    if (f) fclose(f);
    f = fopen("/etc/os-release", "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "PRETTY_NAME=", 12) || !strncmp(line, "VERSION_ID=", 11)) wb_printf(&w, "os_release_%s", line);
    if (f) fclose(f);
    f = fopen("/proc/version", "r");
    if (f && fgets(line, sizeof line, f)) wb_printf(&w, "wsl=%d\n", strstr(line, "icrosoft") != NULL);
    if (f) fclose(f);
#else
    OSVERSIONINFOW vi = {.dwOSVersionInfoSize = sizeof vi};
    typedef LONG(WINAPI * rtl_fn)(OSVERSIONINFOW*);
    rtl_fn rtl = (rtl_fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl && rtl(&vi) == 0)
        wb_printf(&w, "os=Windows\nos_version=%lu.%lu.%lu\n", vi.dwMajorVersion, vi.dwMinorVersion,
                  vi.dwBuildNumber);
    const char* cpu = getenv("PROCESSOR_IDENTIFIER");
    if (cpu) wb_printf(&w, "cpu=%s\n", cpu);
#endif
    /* Allowlisted variables: the session type and compositor, not their values' paths. */
    static const char* const allow[] = {
        "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "SDL_VIDEODRIVER",
        "SDL_AUDIODRIVER", "SteamDeck", "SteamGamepadUI", "WSL_DISTRO_NAME",
    };
    for (size_t i = 0; i < sizeof allow / sizeof allow[0]; i++) {
        const char* e = getenv(allow[i]);
        if (e) wb_printf(&w, "env_%s=%s\n", allow[i], e);
    }
    static const char* const present[] = {"WAYLAND_DISPLAY", "DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY"};
    for (size_t i = 0; i < sizeof present / sizeof present[0]; i++)
        wb_printf(&w, "env_%s_set=%d\n", present[i], getenv(present[i]) != NULL);
    return w.b;
}

/** @brief Read the overlay names of the staged recomp manifest, in order, into a JSON array.
 * @param w The output.
 * @param game The game id.
 * @return 1 when a manifest was read. */
static int overlay_list(wbuf_t* w, const char* game)
{
    char exe[1024], dir[1100], path[1400];
    if (openpete_exe_path(exe, sizeof exe) != 0) { wb_printf(w, "[]"); return 0; }
    char* sl = strrchr(exe, '/');
#ifdef _WIN32
    char* bs = strrchr(exe, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
#endif
    if (sl) *sl = '\0';
    snprintf(dir, sizeof dir, "%s/recomp/manifest/%s", exe, game);
    snprintf(path, sizeof path, "%s/manifest.toml", dir);
    FILE* f = fopen(path, "r");
    if (!f) { wb_printf(w, "[]"); return 0; }
    char err[200];
    toml_table_t* t = toml_parse_file(f, err, sizeof err);
    fclose(f);
    toml_array_t* ov = t ? toml_array_in(t, "overlay") : NULL;
    wb_printf(w, "[");
    for (int i = 0; ov && i < toml_array_nelem(ov); i++) {
        toml_table_t* e = toml_table_at(ov, i);
        toml_datum_t nm = e ? toml_string_in(e, "name") : (toml_datum_t){0};
        if (i) wb_printf(w, ", ");
        wb_json_str(w, nm.ok ? nm.u.s : "");
        if (nm.ok) free(nm.u.s);
    }
    wb_printf(w, "]");
    if (t) toml_free(t);
    return 1;
}

/** @brief Add the recomp module: module.txt always, module/<file> when the module is a file.
 * @param m The members.
 * @param mod_sha Receives the module's SHA-256, "" for a static build. */
static void add_module(br_members_t* m, char mod_sha[65])
{
    const openpete_recomp_module* d = openpete_recomp_module_get();
    const char* path = openpete_recomp_module_path();
    wbuf_t w = {0};
    mod_sha[0] = '\0';
    uint64_t size = 0;
    if (path && *path && sha_file(path, mod_sha, &size) == 0) {
        char name[300];
        snprintf(name, sizeof name, "module/%s", base_name(path));
        add_file(m, path, name, BRM_MODULE);
        wb_printf(&w, "linkage=shared\npath=%s\nsize=%llu\nsha256=%s\n", path,
                  (unsigned long long)size, mod_sha);
    } else {
        wb_printf(&w, "linkage=static (linked into the executable)\n");
    }
    wb_printf(&w, "codegen_stamp=%s\nrom_stamp=%s\nabi=%u\n",
              d && d->codegen_stamp ? d->codegen_stamp : "",
              d && d->rom_sha256 ? d->rom_sha256 : "", d ? d->abi_version : 0u);
    const char* march = getenv("OPENPETE_MODULE_MARCH");
    wb_printf(&w, "module_march=%s\n", march && *march ? march : "(unset)");
    if (w.b) br_members_add(m, "module.txt", (uint8_t*)w.b, w.n, BRM_TEXT);
}

/* One mod's walk: the members it adds and the mods.txt lines. */
typedef struct {
    br_members_t* m;
    wbuf_t*       list;
    const br_mod_t* mod;
    int           copy;     /* an enabled mod: copy the files */
    size_t        files;
} mod_walk_t;

/** @brief Report whether a mod file is skipped: version control, build scratch other than the loaded object, and config.toml, which the shipped form replaces. */
static int mod_skip(const mod_walk_t* w, const char* rel, const char* full, int is_dir)
{
    const char* b = base_name(rel);
    if (is_dir) return !strcmp(b, ".git") || !strcmp(b, ".svn") || !strcmp(b, ".hg");
    if (!strcmp(rel, "config.toml")) return 1;
    if (!strncmp(rel, ".build/", 7) || !strncmp(rel, ".build\\", 7))
        return strcmp(full, w->mod->obj) != 0;
    return 0;
}

/** @brief Walk one directory of a mod, hashing every file into mods.txt and copying it when the mod is enabled.
 * @param w The walk.
 * @param dir The directory.
 * @param rel Its path from the mod's directory ("" for the top). */
static void mod_walk(mod_walk_t* w, const char* dir, const char* rel)
{
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[2048], r[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        const int is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
        if (mod_skip(w, r, full, is_dir)) continue;
        if (is_dir) { mod_walk(w, full, r); continue; }
        char sha[65];
        uint64_t size = 0;
        if (sha_file(full, sha, &size) != 0) continue;
        wb_printf(w->list, "  %s %llu %s\n", sha, (unsigned long long)size, r);
        w->files++;
        if (!w->copy) continue;
        char name[1200];
        snprintf(name, sizeof name, "mods/%s/%s", w->mod->id, r);
        add_file(w->m, full, name, strcmp(full, w->mod->obj) == 0 ? BRM_MODULE : BRM_AUTHOR);
    }
    closedir(d);
}

/** @brief Collect the dotted keys of a TOML table into a buffer, one per line, prefixed "# undeclared: " when the mod's schema lacks them. */
static void toml_keys(wbuf_t* w, toml_table_t* t, const char* prefix, const br_mod_t* mod)
{
    for (int i = 0;; i++) {
        const char* k = toml_key_in(t, i);
        if (!k) break;
        char full[512];
        snprintf(full, sizeof full, "%s%s%s", prefix, prefix[0] ? "." : "", k);
        toml_table_t* sub = toml_table_in(t, k);
        if (sub) { toml_keys(w, sub, full, mod); continue; }
        int declared = 0;
        for (int d = 0; d < mod->ndeclared && !declared; d++)
            declared = mod->declared[d] && !strcmp(mod->declared[d], full);
        if (!declared) wb_printf(w, "# %s: undeclared, value withheld\n", full);
    }
}

/** @brief Add every loaded mod: enabled ones copied whole with their loaded object and shipped config.toml, every one listed in mods.txt with per-file SHA-256.
 * @param m The members.
 * @param j The job. */
static void add_mods(br_members_t* m, const br_job_t* j)
{
    wbuf_t list = {0};
    wb_printf(&list, "# id version enabled mutative files; then per file: sha256 bytes path\n");
    for (int i = 0; i < j->nmods; i++) {
        const br_mod_t* mod = &j->mods[i];
        mod_walk_t w = {m, &list, mod, mod->enabled, 0};
        wbuf_t files = {0};
        w.list = &files;
        if (mod->dir[0]) mod_walk(&w, mod->dir, "");
        wb_printf(&list, "%s %s enabled=%d mutative=%d files=%zu\n", mod->id,
                  mod->version[0] ? mod->version : "-", mod->enabled, mod->mutative, w.files);
        wb_puts(&list, files.b);
        free(files.b);
        if (!mod->enabled) continue;
        wbuf_t cfg = {0};
        wb_put(&cfg, "", 0);
        wb_puts(&cfg, mod->cfg);
        char path[1100];
        snprintf(path, sizeof path, "%s/config.toml", mod->dir);
        FILE* f = fopen(path, "r");
        if (f) {
            char err[200];
            toml_table_t* t = toml_parse_file(f, err, sizeof err);
            fclose(f);
            if (t) { toml_keys(&cfg, t, "", mod); toml_free(t); }
            else wb_printf(&cfg, "# the user's config.toml does not parse: %s\n", err);
        }
        char name[200];
        snprintf(name, sizeof name, "mods/%s/config.toml", mod->id);
        if (cfg.b) br_members_add(m, name, (uint8_t*)cfg.b, cfg.n, BRM_TEXT);
    }
    if (list.b) br_members_add(m, "mods.txt", (uint8_t*)list.b, list.n, BRM_TEXT);
}

/** @brief Add the raw config files the layered config reads. */
static void add_config_files(br_members_t* m)
{
    static const char* const names[2] = {"config-files/exe-dir/openpete.toml",
                                         "config-files/user/openpete.toml"};
    for (int i = 0; i < 2; i++) {
        const char* p = openpete_config_file_path(i);
        if (p) add_file(m, p, names[i], BRM_TEXT);
    }
}

/** @brief Add the press snapshot, the anchor, their card images and `.modsave` bytes. */
static void add_states(br_members_t* m, const br_job_t* j)
{
    const struct { const openpete_bugrec_anchor_t* a; const char* pfx; } st[2] = {
        {&j->press, "press"}, {j->have_anchor ? &j->anchor : NULL, "anchor"}};
    for (int s = 0; s < 2; s++) {
        const openpete_bugrec_anchor_t* a = st[s].a;
        if (!a || !a->state) continue;
        char name[96];
        snprintf(name, sizeof name, "%s.state", st[s].pfx);
        br_members_add_copy(m, name, a->state, a->state_len, BRM_STATE);
        for (int p = 0; p < 2; p++) {
            const char* lead = s == 0 ? "press-" : "";
            snprintf(name, sizeof name, "%scard%d.mcd", lead, p + 1);
            if (a->card_loaded[p] && a->card[p])
                br_members_add_copy(m, name, a->card[p], OPENPETE_BUGREC_CARD_BYTES, BRM_GUEST);
            else if (s == 1 && a->card_held[p] && a->card_path[p][0])
                /* The game had not read the card by the anchor, so nothing
                 * had written it: the file is the card at the anchor
                 * unless a write landed after the game first read it. */
                add_file(m, a->card_path[p], name, BRM_GUEST);
            if (a->modsave[p]) {
                snprintf(name, sizeof name, "%scard%d.mcd.modsave", lead, p + 1);
                br_members_add_copy(m, name, a->modsave[p], a->modsave_len[p], BRM_GUEST);
            }
        }
    }
}

/** @brief Build argv.txt: the session's argv, one argument per line.
 *
 * Not a `.args` sidecar beside segment.bk2: the engine prepends a
 * sidecar's arguments to a replay's command line, and the session's own
 * movie, limit and output flags are not the replay's. */
static char* argv_text(const br_job_t* j)
{
    wbuf_t w = {0};
    wb_put(&w, "", 0);
    for (int i = 0; i < j->argc; i++) wb_printf(&w, "%s\n", j->argv[i]);
    return w.b;
}

/** @brief Append the lines of a text whose subsystem tag is one of the pacing ones. */
static void pace_lines(wbuf_t* w, const char* txt)
{
    for (const char* l = txt; l && *l;) {
        const char* e = strchr(l, '\n');
        const size_t n = e ? (size_t)(e - l + 1) : strlen(l);
        if (br_memmem(l, n, "] pace:", 7) || br_memmem(l, n, "] pacer:", 8)) wb_put(w, l, n);
        l += n;
    }
}

/* The redaction and scan pass's state. */
typedef struct {
    br_finding_t* f;
    size_t        nf, total;
    const char*   member;
    const char*   section;
    size_t        base;
} scan_t;

/** @brief Record one finding of the scan in progress. */
static void scan_hit(void* ud, size_t off, br_needle_kind_t what)
{
    scan_t* s = (scan_t*)ud;
    s->total++;
    if (s->nf >= BR_FINDINGS_MAX) return;
    br_finding_t* f = &s->f[s->nf++];
    snprintf(f->member, sizeof f->member, "%s", s->member);
    snprintf(f->section, sizeof f->section, "%s", s->section ? s->section : "");
    f->offset = s->base + off;
    f->what = what;
}

/** @brief Scan a member's bytes, by section for a state file.
 * @param R The redactor.
 * @param s The scan.
 * @param mb The member. */
static void scan_member(const br_redactor_t* R, scan_t* s, const br_member_t* mb)
{
    s->member = mb->name;
    s->section = NULL;
    s->base = 0;
    if (mb->kind == BRM_STATE) {
        openpete_state_section_t sec[16];
        const int n = openpete_state_image_sections(mb->data, mb->len, sec, 16);
        for (int i = 0; i < n; i++) {
            s->section = sec[i].name;
            s->base = sec[i].off;
            br_scan(R, mb->data + sec[i].off, sec[i].len, scan_hit, s);
        }
        if (n > 0) return;
    }
    br_scan(R, mb->data, mb->len, scan_hit, s);
}

/** @brief Rebuild a bk2 zip with its text entries redacted.
 * @param R The redactor.
 * @param mb The member, replaced in place.
 * @return 0, or -1 when the archive does not parse (left as it was). */
static int redact_bk2(br_redactor_t* R, br_member_t* mb)
{
    mz_zip_archive in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    if (!mz_zip_reader_init_mem(&in, mb->data, mb->len, 0)) return -1;
    if (!mz_zip_writer_init_heap(&out, 0, 0)) { mz_zip_reader_end(&in); return -1; }
    int ok = 1;
    const mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&in, i, &st)) { ok = 0; break; }
        size_t len = 0;
        void* d = mz_zip_reader_extract_to_heap(&in, i, &len, 0);
        if (!d) { ok = 0; break; }
        size_t rn = len;
        char* r = br_redact_text(R, (const char*)d, len, &rn);
        ok = r && mz_zip_writer_add_mem(&out, st.m_filename, r, rn, MZ_DEFAULT_LEVEL);
        free(r);
        mz_free(d);
    }
    mz_zip_reader_end(&in);
    void* zb = NULL;
    size_t zl = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&out, &zb, &zl);
    mz_zip_writer_end(&out);
    if (!ok) { free(zb); return -1; }
    free(mb->data);
    mb->data = (uint8_t*)zb;
    mb->len = zl;
    return 0;
}

/** @brief Redact every member by its class and scan the ones never rewritten, collecting the findings.
 * @param R The redactor.
 * @param m The members.
 * @param s The scan. */
static void redact_all(br_redactor_t* R, br_members_t* m, scan_t* s)
{
    for (size_t i = 0; i < m->n; i++) {
        br_member_t* mb = &m->v[i];
        switch (mb->kind) {
        case BRM_TEXT: {
            size_t rn = 0;
            char* r = br_redact_text(R, (const char*)mb->data, mb->len, &rn);
            if (r) { free(mb->data); mb->data = (uint8_t*)r; mb->len = rn; }
            /* A second pass over redacted text rewrites nothing; a home
             * form still present is a redactor miss. */
            s->member = mb->name;
            s->section = "text";
            s->base = 0;
            for (int k = 0; k < R->npre; k++) {
                if (R->pre[k].repl[0] != '~') continue;
                const uint8_t* at = mb->data;
                while ((at = (const uint8_t*)br_memmem(at, mb->len - (size_t)(at - mb->data),
                                                    R->pre[k].form, R->pre[k].len)) != NULL) {
                    scan_hit(s, (size_t)(at - mb->data), BRF_HOME);
                    at += R->pre[k].len;
                }
            }
            break;
        }
        case BRM_STATE: {
            const int n = openpete_state_image_card_paths_set(mb->data, mb->len, "card1.mcd", "card2.mcd");
            if (n > 0) R->replacements += (uint64_t)n;
            scan_member(R, s, mb);
            break;
        }
        case BRM_BK2:
            (void)redact_bk2(R, mb);
            break;
        default:
            scan_member(R, s, mb);
            break;
        }
    }
}

/* Documented at the declaration in bugreport_internal.h. */
int br_disc_sha(const char* path, char out[65])
{
    out[0] = '\0';
    if (!path || !*path) return -1;
    disc_image_t* d = disc_image_open(path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_BUGREPORT);
    if (!d) return -1;
    const uint64_t total = disc_image_size(d);
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    uint8_t* buf = (uint8_t*)malloc(1u << 20);
    uint64_t seen = 0;
    int rc = buf ? 0 : -1;
    while (!rc && seen < total) {
        const size_t n = total - seen < (1u << 20) ? (size_t)(total - seen) : (1u << 20);
        if (disc_image_read_bytes(d, seen, buf, n, OPENPETE_DISC_TAG_BUGREPORT) < 0) rc = -1;
        else openpete_sha256_update(&c, buf, n);
        seen += n;
    }
    free(buf);
    disc_image_close(d);
    if (rc) return -1;
    uint8_t dig[32];
    openpete_sha256_final(&c, dig);
    openpete_sha256_hex(dig, out);
    return 0;
}

static const char* const k_needle_names[BRF_N] = {"home", "user", "host"};

/** @brief Write one anchor's counters as a JSON object.
 *
 * `seq` is the recorder's capture order (1 = the session's first anchor, 0
 * for the press capture); the replay passes the tester's launch-script flags
 * only from seq 1. */
static void json_anchor(wbuf_t* w, const openpete_bugrec_anchor_t* a)
{
    wb_printf(w, "{\"seq\": %llu, \"frame\": %u, \"vbl\": %d, \"feed_key\": %u, \"log_idx\": %d, \"consume\": %u, "
              "\"consume_write\": %u, \"level\": %d, \"park_ra\": \"0x%08X\", \"park_sp\": \"0x%08X\", "
              "\"capture_us\": %llu}", (unsigned long long)a->seq, a->frame, a->vbl, a->feed_key, a->log_idx, a->consume,
              a->consume_write, a->level, a->site.ra, a->site.sp, (unsigned long long)a->capture_us);
}

/** @brief Write the manifest's identity fields: schema, build, artifact, module, disc, game and argv.
 * @param w The output.
 * @param j The job.
 * @param mod_sha The module's SHA-256, "" for a static build. */
static void json_identity(wbuf_t* w, const br_job_t* j, const char* mod_sha)
{
    char exe[1024], exe_sha[65] = "", ident[80] = "", dsha[65] = "";
    if (openpete_exe_path(exe, sizeof exe) == 0) (void)sha_file(exe, exe_sha, NULL);
    const openpete_bugrec_anchor_t* id_src = j->press.state ? &j->press : &j->anchor;
    if (id_src->state)
        (void)openpete_state_image_ident(id_src->state, id_src->state_len, ident, sizeof ident);
    (void)br_disc_sha(j->disc_path, dsha);
    const openpete_recomp_module* d = openpete_recomp_module_get();
    const char* march = getenv("OPENPETE_MODULE_MARCH");
    wb_printf(w, "{\n  \"schema\": %d,\n  \"trigger\": ", OPENPETE_BUGREPORT_SCHEMA);
    wb_json_str(w, j->fatal ? j->fatal : "press");
    if (j->exit_thread) {
        wb_printf(w, ",\n  \"exit_thread\": ");
        wb_json_str(w, j->exit_thread);
    }
    wb_printf(w, ",\n  \"build_version\": ");
    wb_json_str(w, openpete_build_version());
    wb_printf(w, ",\n  \"build_tagged\": %d,\n  \"build_commit\": ", openpete_build_tagged());
    wb_json_str(w, openpete_build_commit());
    wb_printf(w, ",\n  \"exe_sha256\": ");
    wb_json_str(w, exe_sha);
    wb_printf(w, ",\n  \"codegen_stamp\": ");
    wb_json_str(w, d && d->codegen_stamp ? d->codegen_stamp : "");
    wb_printf(w, ",\n  \"rom_stamp\": ");
    wb_json_str(w, d && d->rom_sha256 ? d->rom_sha256 : "");
    wb_printf(w, ",\n  \"overlays\": ");
    const int have_ovl = overlay_list(w, j->game);
    wb_printf(w, ",\n  \"overlays_source\": \"%s\"", have_ovl ? "recomp/manifest" : "none (no staged recomp manifest)");
    wb_printf(w, ",\n  \"module_sha256\": ");
    wb_json_str(w, mod_sha);
    wb_printf(w, ",\n  \"module_march\": ");
    wb_json_str(w, march && *march ? march : "");
    wb_printf(w, ",\n  \"state_ident\": ");
    wb_json_str(w, ident);
    wb_printf(w, ",\n  \"game\": ");
    wb_json_str(w, j->game);
    wb_printf(w, ",\n  \"disc\": ");
    wb_json_str(w, base_name(j->disc_path));
    wb_printf(w, ",\n  \"disc_sha256\": ");
    wb_json_str(w, dsha);
    wb_printf(w, ",\n  \"argv\": [");
    for (int i = 0; i < j->argc; i++) {
        if (i) wb_printf(w, ", ");
        wb_json_str(w, j->argv[i]);
    }
    wb_printf(w, "]");
}

/** @brief Write the manifest's thumbnail fields: state, count, period, span, the newest one's age at the press, and per thumbnail its member, present clock, feed key and size.
 * @param w The output.
 * @param j The job. */
static void json_thumbs(wbuf_t* w, const br_job_t* j)
{
    const size_t n = j->thumbs_n;
    const char* state = !openpete_bugthumbs_enabled() ? "off" : n ? "held" : "none held";
    wb_printf(w, ",\n  \"thumbs\": {\"state\": \"%s\", \"count\": %zu, \"period_ms\": %lld", state, n,
              (long long)(OPENPETE_BUGTHUMB_PERIOD_NS / 1000000));
    if (n) {
        const openpete_bugthumb_t* a = &j->thumbs[0];
        const openpete_bugthumb_t* b = &j->thumbs[n - 1];
        wb_printf(w, ", \"span_ms\": %.1f, \"newest_age_ms\": %.1f",
                  (double)(b->present_ns - a->present_ns) / 1e6,
                  (double)(j->press_present_ns - b->present_ns) / 1e6);
    }
    wb_printf(w, ", \"list\": [");
    for (size_t i = 0; i < n; i++)
        wb_printf(w, "%s\n    {\"name\": \"thumbs/%03zu.png\", \"present_ns\": %lld, \"feed_key\": %u, "
                  "\"w\": %d, \"h\": %d}", i ? "," : "", i, (long long)j->thumbs[i].present_ns,
                  j->thumbs[i].feed_key, j->thumbs[i].w, j->thumbs[i].h);
    wb_printf(w, "]}");
}

/** @brief Write the manifest's sub-tick ring fields: presents and entries held, presents and entries the ring dropped, and the entries offered outside a present.
 * @param w The output.
 * @param j The job. */
static void json_subtick(wbuf_t* w, const br_job_t* j)
{
    uint32_t hv[7] = {0};
    uint64_t base = 0, outside = 0;
    if (j->subtick && j->subtick_len >= 48u) {
        memcpy(hv, j->subtick + 8, sizeof hv);
        memcpy(&base, j->subtick + 32, 8);
        memcpy(&outside, j->subtick + 40, 8);
    }
    wb_printf(w, ",\n  \"subtick\": {\"member\": %s, \"version\": %u, \"presents\": %u, \"entries\": %u, "
              "\"presents_dropped\": %u, \"entries_dropped\": %llu, \"entries_outside\": %llu}",
              j->subtick ? "\"subtick.bin\"" : "null", hv[0], hv[3], hv[4], hv[5],
              (unsigned long long)base, (unsigned long long)outside);
}

/** @brief Write the manifest's records field: the pace record types this build writes and the coarse ring's member, version, rows and row span.
 *
 * A reader takes a type missing from the list as not recorded by the
 * writing build, which tells an old bundle from a run that produced none.
 * @param w The output.
 * @param j The job. */
static void json_records(wbuf_t* w, const br_job_t* j)
{
    uint32_t span = 0;
    if (j->pace_coarse && j->pace_coarse_len >= 24u) memcpy(&span, (const uint8_t*)j->pace_coarse + 20, 4);
    wb_printf(w, ",\n  \"records\": {\"pace\": {\"member\": %s, \"version\": %u, \"types\": [",
              j->pace ? "\"pace.oppl\"" : "null", OP_PACE_VERSION);
    for (int t = OP_PACE_VBL; t <= OP_PACE_GPU_FRAME; t++) wb_printf(w, "%s%d", t > OP_PACE_VBL ? ", " : "", t);
    wb_printf(w, "], \"gpu_frames\": %d}, \"pace_coarse\": {\"member\": %s, \"version\": %u, "
              "\"rows\": %zu, \"span_s\": %u}}", openpete_pace_gpu_records_wanted(),
              j->pace_coarse ? "\"pace-coarse.bin\"" : "null", OP_PACE_COARSE_VERSION,
              j->pace_coarse_rows, span);
}

/** @brief Write the manifest's session fields: anchor, press, window, taint, mods, answers and the member list.
 * @param w The output.
 * @param j The job.
 * @param m The members (redacted).
 * @param s The scan's findings. */
static void json_session(wbuf_t* w, const br_job_t* j, const br_members_t* m, const scan_t* s)
{
    wb_printf(w, ",\n  \"anchor\": ");
    if (j->have_anchor) json_anchor(w, &j->anchor); else wb_printf(w, "null");
    wb_printf(w, ",\n  \"press\": ");
    if (j->press.state) json_anchor(w, &j->press); else wb_printf(w, "null");
    wb_printf(w, ",\n  \"window_ticks\": %u,\n  \"segment_rows\": %zu,\n  \"hash_rows\": %zu",
              j->have_anchor && j->press.state ? j->press.frame - j->anchor.frame : 0u,
              j->segment_rows, j->hash_n);
    json_thumbs(w, j);
    json_subtick(w, j);
    json_records(w, j);
    wb_printf(w, ",\n  \"note\": {\"state\": ");
    wb_json_str(w, j->note_state[0] ? j->note_state : "off");
    wb_printf(w, ", \"chars\": %zu}", j->note ? strlen(j->note) : (size_t)0);
    wb_printf(w, ",\n  \"taint\": {");
    for (int k = 0; k < BUGREC_TAINT_N; k++)
        wb_printf(w, "%s\"%s\": %llu", k ? ", " : "", openpete_bugrec_taint_name((openpete_bugrec_taint_t)k),
                  (unsigned long long)j->taint[k]);
    wb_printf(w, "},\n  \"mods\": [");
    for (int i = 0, n = 0; i < j->nmods; i++) {
        if (!j->mods[i].enabled) continue;
        wb_printf(w, "%s{\"id\": ", n++ ? ", " : "");
        wb_json_str(w, j->mods[i].id);
        wb_printf(w, ", \"version\": ");
        wb_json_str(w, j->mods[i].version);
        wb_printf(w, ", \"mutative\": %d}", j->mods[i].mutative);
    }
    wb_printf(w, "],\n  \"answers\": {\"screen\": \"%s\", \"display\": \"%s\", \"main\": \"%s\"}",
              SDL_GetAtomicInt((SDL_AtomicInt*)&j->shot) == BR_DONE ? "done" : "absent",
              SDL_GetAtomicInt((SDL_AtomicInt*)&j->display) == BR_DONE ? "done" : "absent",
              SDL_GetAtomicInt((SDL_AtomicInt*)&j->main_facts) == BR_DONE ? "done" : "absent");
    wb_printf(w, ",\n  \"press_us\": %llu,\n  \"display_us\": %llu", (unsigned long long)j->press_us,
              (unsigned long long)j->display_us);
    wb_printf(w, ",\n  \"redaction\": {\"replacements\": %llu, \"findings\": %zu, \"list\": [",
              (unsigned long long)j->replacements, s->total);
    for (size_t i = 0; i < s->nf; i++) {
        wb_printf(w, "%s\n    {\"member\": ", i ? "," : "");
        wb_json_str(w, s->f[i].member);
        wb_printf(w, ", \"section\": ");
        wb_json_str(w, s->f[i].section);
        wb_printf(w, ", \"offset\": %llu, \"what\": \"%s\"}", (unsigned long long)s->f[i].offset,
                  k_needle_names[s->f[i].what]);
    }
    wb_printf(w, "]},\n  \"members\": [");
    for (size_t i = 0; i < m->n; i++) {
        char sha[65];
        sha_hex(m->v[i].data, m->v[i].len, sha);
        wb_printf(w, "%s\n    {\"name\": ", i ? "," : "");
        wb_json_str(w, m->v[i].name);
        wb_printf(w, ", \"size\": %zu, \"sha256\": \"%s\"}", m->v[i].len, sha);
    }
    wb_printf(w, "\n  ]\n}\n");
}

/** @brief Zip the members and manifest.json and write the archive atomically.
 * @return 0, or -1 (logged). */
static int write_zip(const br_job_t* j, const br_members_t* m, const wbuf_t* manifest,
                     uint64_t* bytes)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return -1;
    int ok = mz_zip_writer_add_mem(&z, "manifest.json", manifest->b, manifest->n, MZ_DEFAULT_LEVEL);
    for (size_t i = 0; i < m->n && ok; i++) {
        const br_kind_t k = m->v[i].kind;
        const mz_uint level = (k == BRM_PNG || k == BRM_BK2) ? MZ_NO_COMPRESSION : MZ_BEST_SPEED;
        ok = mz_zip_writer_add_mem(&z, m->v[i].name, m->v[i].data, m->v[i].len, level);
        if (!ok)
            OPL_ERROR_F("bugreport", "zip: adding %s failed: %s", m->v[i].name,
                        mz_zip_get_error_string(mz_zip_get_last_error(&z)));
    }
    void* zb = NULL;
    size_t zl = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &zb, &zl);
    mz_zip_writer_end(&z);
    if (!ok) { free(zb); return -1; }
    const int rc = op_atomic_write_file(j->zip_path, zb, zl);
    free(zb);
    *bytes = zl;
    return rc;
}

/** @brief Add the press's in-memory copies and the staged reports as members.
 * @param m The members.
 * @param j The job. */
static void add_press_members(br_members_t* m, br_job_t* j)
{
    add_states(m, j);
    if (j->segment) br_members_add_copy(m, "segment.bk2", j->segment, j->segment_len, BRM_BK2);
    char* a = argv_text(j);
    if (a) br_members_add(m, "argv.txt", (uint8_t*)a, strlen(a), BRM_TEXT);
    add_streamed(m, j, "hashes.csv", BRM_BINARY, stream_hashes);
    add_streamed(m, j, "input-events.csv", BRM_TEXT, stream_input);
    if (j->trace) br_members_add_copy(m, "host-trace.bin", j->trace, j->trace_n * HT_REC_BYTES, BRM_BINARY);
    if (j->pace) br_members_add_copy(m, "pace.oppl", j->pace, j->pace_len, BRM_BINARY);
    if (j->pace_coarse)
        br_members_add_copy(m, "pace-coarse.bin", j->pace_coarse, j->pace_coarse_len, BRM_BINARY);
    if (j->hang) br_members_add_copy(m, "hang/rings.bin", j->hang, j->hang_len, BRM_BINARY);
    if (j->subtick) br_members_add_copy(m, "subtick.bin", j->subtick, j->subtick_len, BRM_BINARY);
    wbuf_t rep = {0};
    wb_puts(&rep, "# sim thread, at the press\n");
    wb_puts(&rep, j->reports);
    wb_puts(&rep, "# display owner, at the screenshot\n");
    wb_puts(&rep, j->display_txt ? j->display_txt : "(absent)\n");
    if (rep.b) br_members_add(m, "reports.txt", (uint8_t*)rep.b, rep.n, BRM_TEXT);
    wbuf_t pc = {0};
    wb_put(&pc, "", 0);
    pace_lines(&pc, j->reports);
    pace_lines(&pc, j->display_txt);
    if (pc.b) br_members_add(m, "pace-census.txt", (uint8_t*)pc.b, pc.n, BRM_TEXT);
    if (j->summary) br_members_add_copy(m, "summary.txt", j->summary, strlen(j->summary), BRM_TEXT);
    if (j->env) br_members_add_copy(m, "env.txt", j->env, strlen(j->env), BRM_TEXT);
    add_stage_file(m, j, "config.txt", "config.txt", BRM_TEXT);
    add_stage_file(m, j, "keymap.txt", "keymap.txt", BRM_TEXT);
    if (SDL_GetAtomicInt(&j->main_facts) == BR_DONE)
        add_stage_file(m, j, "devices.txt", "devices.txt", BRM_TEXT);
    if (SDL_GetAtomicInt(&j->shot) == BR_DONE)
        add_stage_file(m, j, "screen.png", "screen.png", BRM_PNG);
    add_thumbs(m, j);
    if (j->note) {
        wbuf_t nt = {0};
        wb_printf(&nt, "%s\n", j->note);
        if (nt.b) br_members_add(m, "notes.txt", (uint8_t*)nt.b, nt.n, BRM_TEXT);
    }
    char* dev = device_text(j);
    if (dev) br_members_add(m, "device.txt", (uint8_t*)dev, strlen(dev), BRM_TEXT);
}

/** @brief Remove the staging directory and whatever a late answer left in it. */
static void stage_remove(const br_job_t* j)
{
    DIR* d = opendir(j->stage);
    struct dirent* e;
    while (d && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1500];
        snprintf(p, sizeof p, "%s/%s", j->stage, e->d_name);
        remove(p);
    }
    if (d) closedir(d);
#ifdef _WIN32
    _rmdir(j->stage);
#else
    rmdir(j->stage);
#endif
}

void br_job_write(br_job_t* j)
{
    const uint64_t t0 = SDL_GetTicksNS();
    const int main_ok = main_wait(j, t0 + (uint64_t)BR_MAIN_WAIT_MS * 1000000u);
    const int shot = answer_wait(&j->shot, t0 + (uint64_t)BR_SHOT_WAIT_MS * 1000000u);
    const int disp = answer_wait(&j->display, t0 + (uint64_t)BR_SHOT_WAIT_MS * 1000000u);
    /* After the screenshot, so the prompt is not in screen.png. */
    if (!j->fatal) note_wait(j);
    SDL_SetAtomicInt(&j->noted, 1);
    br_redactor_t R;
    br_redactor_init(&R);
    br_members_t m = {0};
    add_press_members(&m, j);
    add_config_files(&m);
    add_logs(&m, j);
    char mod_sha[65];
    add_module(&m, mod_sha);
    add_mods(&m, j);
    scan_t s = {0};
    s.f = (br_finding_t*)calloc(BR_FINDINGS_MAX, sizeof *s.f);
    redact_all(&R, &m, &s);
    j->replacements = R.replacements;
    j->findings = s.total;
    wbuf_t man = {0};
    json_identity(&man, j, mod_sha);
    json_session(&man, j, &m, &s);
    /* The manifest is a text member too (argv, the disc's name). */
    size_t rn = 0;
    char* red = man.oom ? NULL : br_redact_text(&R, man.b, man.n, &rn);
    if (red) { free(man.b); man.b = red; man.n = rn; man.cap = rn + 1; }
    j->replacements = R.replacements;
    uint64_t bytes = 0;
    j->rc = man.oom ? -1 : write_zip(j, &m, &man, &bytes);
    j->members = m.n + 1;
    j->zip_bytes = bytes;
    for (size_t i = 0; i < s.nf && i < 8; i++)
        OPL_WARN_F("bugreport", "finding: %s %s offset %llu: %s", s.f[i].member, s.f[i].section,
                   (unsigned long long)s.f[i].offset, k_needle_names[s.f[i].what]);
    OPL_INFO_F("bugreport", "answers: screen %s, display %s, devices %s",
               shot == BR_DONE ? "done" : "absent", disp == BR_DONE ? "done" : "absent",
               main_ok == BR_DONE ? "done" : "absent");
    free(man.b);
    free(s.f);
    br_members_free(&m);
    br_redactor_free(&R);
    stage_remove(j);
    j->write_ms = (SDL_GetTicksNS() - t0) / 1000000u;
}

void br_job_free(br_job_t* j)
{
    if (!j) return;
    openpete_bugrec_anchor_release(&j->press);
    openpete_bugrec_anchor_release(&j->anchor);
    free(j->segment);
    free(j->hash_rows);
    free(j->trace);
    free(j->pace);
    free(j->pace_coarse);
    free(j->input);
    free(j->hang);
    free(j->subtick);
    openpete_bugthumbs_free(j->thumbs, j->thumbs_n);
    free(j->note);
    free(j->reports);
    free(j->summary);
    free(j->env);
    free(j->display_txt);
    free(j->main_txt);
    for (int i = 0; i < j->argc; i++) free(j->argv[i]);
    free(j->argv);
    for (int i = 0; i < j->nmods; i++) {
        free(j->mods[i].cfg);
        for (int d = 0; d < j->mods[i].ndeclared; d++) free(j->mods[i].declared[d]);
        free(j->mods[i].declared);
    }
    free(j->mods);
    free(j);
}
