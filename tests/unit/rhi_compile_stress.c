/* rhi_compile_stress - variant-explicit compile jobs on worker threads
 * against the live mod registry, its per-tick writers and live environment
 * edits.
 *
 * Three roles run at once, as the renderer's threads do:
 *   - the display owner (main): under short registry read holds it makes
 *     compile jobs (rhi_compile_job_new) for every mod postfx pass and
 *     material slot that holds no pipeline and for a built-in pipeline in
 *     each reachable variant, writes environment variables the way the
 *     overlay's live env rows do (OPENPETE_SHADER_DIR among them, which job
 *     making reads), installs results into their slots after re-resolving
 *     the slot and comparing its generation, discards stale results as
 *     Vulkan objects, and retires swept and installed pipelines through
 *     rhi_retire_pipeline across begun frames;
 *   - N compile workers: rhi_compile_job_run on jobs from a locked queue,
 *     destroying the variant jobs themselves (rhi_compile_job_discard);
 *   - the guest thread: the per-tick writers (material_set_params,
 *     postfx_set_uniforms), material_enable, and a hot reload (remove the
 *     mod's shaders, passes and materials, register them again) every 256
 *     iterations, each call timed: its duration is the write-lock wait
 *     plus the call's own work.
 *
 * The run is made twice: once with no compiles (jobs discarded unrun) and
 * once with the workers. Reported: each writer's maximum and mean call time
 * in both, the display owner's registry read holds, the compile and install
 * counts (engagement), seam violations, and, on Linux, the getenv and fopen
 * calls made on worker threads (the harness interposes both and attributes
 * each by its caller: this executable, which links the RHI, or the Vulkan
 * driver's shared library). Exit 0 when the workers ran jobs, installed
 * results, saw no seam violation, made no getenv or fopen call from this
 * executable's code, a link and unlink made off the display owner each
 * counted one seam violation (the negative control), and no
 * read hold's CPU time or writer maximum grew beyond max(2 x, +5 ms) of the no-compile
 * run (a writer rise within the longest hold plus the unlocked control
 * section's jitter reads JITTER, not a failure);
 * 1 otherwise; 77 when no Vulkan device can be created.
 *
 * Usage: rhi_compile_stress [workers] [seconds] [shader_dir]
 * shader_dir, when given, is a directory of built .spv files the display
 * owner points OPENPETE_SHADER_DIR at on alternate cycles. Under
 * ThreadSanitizer (target rhi_compile_stress_tsan) a Linux kernel with high
 * mmap randomization needs `setarch "$(uname -m)" -R`. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* RTLD_NEXT */
#endif
#include "rhi.h"
#include "rhi_embedded_shaders.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "mod_glsl.h"

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* ---- interposed environment and file reads (Linux) ------------------- */

static _Thread_local int t_worker;   /* set on compile worker threads */
/* Calls on worker threads whose caller is in this executable (openpete
 * code, the RHI and SDL are linked in statically) and calls from a shared
 * library (the Vulkan driver inside vkCreateGraphicsPipelines). */
static atomic_uint s_worker_getenv, s_worker_fopen, s_driver_getenv, s_driver_fopen;

#if defined(__linux__)
#include <dlfcn.h>
extern char **environ;

/** @brief Return whether a code address lies in this executable rather than a shared library.
 *
 * @param addr The address.
 * @return true for the executable. */
static bool in_exe(const void *addr)
{
    static const char *exe;
    Dl_info self, at;
    if (!exe && dladdr((const void *)&in_exe, &self)) exe = self.dli_fname;
    return dladdr(addr, &at) && exe && strcmp(at.dli_fname, exe) == 0;
}

/** @brief Count one worker-thread call, as the executable's or the driver's by its caller.
 *
 * @param caller The call's return address.
 * @param own Counter for a caller in the executable.
 * @param driver Counter for a caller in a shared library. */
static void count_worker_call(const void *caller, atomic_uint *own, atomic_uint *driver)
{
    if (t_worker) atomic_fetch_add(in_exe(caller) ? own : driver, 1u);
}

/** @brief Look a variable up in environ, counting calls made on a compile worker.
 *
 * @param name Variable name.
 * @return Its value, or NULL. */
char *getenv(const char *name)
{
    count_worker_call(__builtin_return_address(0), &s_worker_getenv, &s_driver_getenv);
    const size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/** @brief getenv under the secure name, counted the same way.
 *
 * @param name Variable name.
 * @return Its value, or NULL. */
char *secure_getenv(const char *name)
{
    count_worker_call(__builtin_return_address(0), &s_worker_getenv, &s_driver_getenv);
    const int was = t_worker;
    t_worker = 0;
    char *v = getenv(name);
    t_worker = was;
    return v;
}

/** @brief Open a file through the next fopen, counting calls made on a compile worker.
 *
 * @param path File path.
 * @param mode fopen mode.
 * @return The stream, or NULL. */
FILE *fopen(const char *path, const char *mode)
{
    typedef FILE *(*fopen_fn)(const char *, const char *);
    static fopen_fn real;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    count_worker_call(__builtin_return_address(0), &s_worker_fopen, &s_driver_fopen);
    return real ? real(path, mode) : NULL;
}
#endif

/* ---- stubs for the mod loader and renderer symbols mod_shader.c calls -- */

OPENPETE_THREAD_LOCAL int openpete_mods_in_present;
static char s_mod_dir[512];
static const char *const k_mod_id = "stress";

/** @brief Return the stress mod's id.
 *
 * @param self Unused.
 * @return "stress". */
const char *openpete_mods_id_of(openpete_mod_t *self) { (void)self; return k_mod_id; }
/** @brief Return the stress mod's directory.
 *
 * @param self Unused.
 * @return The directory. */
const char *openpete_mods_dir_of(openpete_mod_t *self) { (void)self; return s_mod_dir; }
/** @brief Return the staged SDK directory.
 *
 * @return "". */
const char *openpete_mods_sdk_dir(void) { return ""; }
/** @brief Report every mod enabled.
 *
 * @param id Unused.
 * @return 1. */
int openpete_mods_is_enabled(const char *id) { (void)id; return 1; }
/** @brief Return a mod's load index.
 *
 * @param self Unused.
 * @return 0. */
int openpete_mods_load_index_of(openpete_mod_t *self) { (void)self; return 0; }
/** @brief Return the toggle epoch.
 *
 * @return 0. */
unsigned openpete_mods_toggle_epoch(void) { return 0; }
/** @brief Accept the material tag-split arming.
 *
 * @param on Unused. */
void openpete_native_set_tag_split(int on) { (void)on; }

/** @brief Map a shader path's extension to its stage.
 *
 * @param path Path.
 * @return OPENPETE_MOD_GLSL_STAGE_FRAG for .frag, VERT for .vert, else -1. */
int openpete_mod_glsl_stage_from_path(const char *path)
{
    const size_t n = strlen(path);
    if (n > 5 && strcmp(path + n - 5, ".frag") == 0) return OPENPETE_MOD_GLSL_STAGE_FRAG;
    if (n > 5 && strcmp(path + n - 5, ".vert") == 0) return OPENPETE_MOD_GLSL_STAGE_VERT;
    return -1;
}

/** @brief Return a fixed source text for any path, so the registry takes the compile route.
 *
 * @param path Unused.
 * @param len_out Receives the length.
 * @return A malloc'd copy of the text. */
char *openpete_mod_glsl_read_file(const char *path, size_t *len_out)
{
    (void)path;
    static const char k_src[] = "// stress\n";
    char *s = malloc(sizeof k_src);
    if (s) memcpy(s, k_src, sizeof k_src);
    if (len_out) *len_out = sizeof k_src - 1;
    return s;
}

/** @brief Fill an include context with empty directories.
 *
 * @param inc The context.
 * @param shader_path Unused.
 * @param sdk_dir Unused. */
void openpete_mod_glsl_include_init(openpete_mod_glsl_include_t *inc, const char *shader_path,
                                    const char *sdk_dir)
{
    (void)shader_path; (void)sdk_dir;
    memset(inc, 0, sizeof *inc);
}

/** @brief Report no terrain profile.
 *
 * @param src Unused.
 * @param stage Unused.
 * @return 0. */
int openpete_mod_glsl_has_terrain_profile(const char *src, int stage)
{
    (void)src; (void)stage;
    return 0;
}

static const uint32_t *s_frag_words;   /* the postfx fragment every mod shader compiles to */
static size_t          s_frag_n;

/** @brief Compile any source to a copy of the raster-quantize postfx fragment.
 *
 * @param src Unused.
 * @param stage Unused.
 * @param inc Unused.
 * @param preamble Unused.
 * @param log_path Unused.
 * @param out_words Receives a malloc'd copy.
 * @return The word count. */
size_t openpete_mod_glsl_compile(const char *src, int stage, const openpete_mod_glsl_include_t *inc,
                                 const char *preamble, const char *log_path, uint32_t **out_words)
{
    (void)src; (void)stage; (void)inc; (void)preamble; (void)log_path;
    *out_words = malloc(s_frag_n * 4);
    if (!*out_words) return 0;
    memcpy(*out_words, s_frag_words, s_frag_n * 4);
    return s_frag_n;
}

/* ---- the job queue ----------------------------------------------------- */

typedef enum { JOB_BUILTIN, JOB_VARIANT, JOB_POSTFX, JOB_MATERIAL } job_kind;

typedef struct qjob {
    struct qjob     *next;
    rhi_compile_job *job;
    job_kind         kind;
    int              index;   /* postfx pass or material index */
} qjob;

static struct {
    SDL_Mutex     *mx;
    SDL_Condition *cv;
    qjob          *todo, *done;
    bool           stop;
} s_q;

/** @brief Push a job onto a list under the queue lock and wake a worker.
 *
 * @param list The list head.
 * @param j The job. */
static void q_push(qjob **list, qjob *j)
{
    SDL_LockMutex(s_q.mx);
    j->next = *list;
    *list = j;
    SDL_SignalCondition(s_q.cv);
    SDL_UnlockMutex(s_q.mx);
}

/* Counts of one run. */
typedef struct {
    atomic_uint ran, ran_ok, worker_discards;
    unsigned enqueued, installed, stale, slot_taken, builtin_retired, swept_retired,
             unrun_discards, reloads;
    uint64_t max_ns[4], sum_ns[4];
    unsigned calls[4];
    uint64_t ctl_max_ns;              /* the guest's unlocked control section */
    uint64_t hold_max_ns, hold_sum_ns; /* the display owner's registry read holds, wall */
    uint64_t hold_cpu_max_ns;          /* the same holds in the thread's CPU time */
    unsigned holds;
} run_stats;

static rhi_device *s_dev;
static run_stats   s_st;
static atomic_bool s_guest_stop;

/** @brief Run jobs from the queue until stopped: variant jobs are destroyed here, the rest go to the done list.
 *
 * @param arg Unused.
 * @return 0. */
static int SDLCALL worker_main(void *arg)
{
    (void)arg;
    t_worker = 1;
    for (;;) {
        SDL_LockMutex(s_q.mx);
        while (!s_q.todo && !s_q.stop) SDL_WaitCondition(s_q.cv, s_q.mx);
        qjob *j = s_q.todo;
        if (j) s_q.todo = j->next;
        SDL_UnlockMutex(s_q.mx);
        if (!j) return 0;
        const int r = rhi_compile_job_run(s_dev, j->job, 0);
        atomic_fetch_add(&s_st.ran, 1u);
        if (r == 0) atomic_fetch_add(&s_st.ran_ok, 1u);
        if (j->kind == JOB_VARIANT) {
            rhi_compile_job_discard(s_dev, j->job);
            atomic_fetch_add(&s_st.worker_discards, 1u);
            free(j);
            continue;
        }
        q_push(&s_q.done, j);
    }
}

/* ---- the guest thread -------------------------------------------------- */

static openpete_mod_t *const k_self = (openpete_mod_t *)&k_mod_id;
enum { W_PARAMS, W_UNIFORMS, W_ENABLE, W_RELOAD };
static const char *const k_writer[4] = { "material_set_params", "postfx_set_uniforms",
                                         "material_enable", "hot_reload" };

/** @brief Register the stress mod's shader, postfx pass (enabled) and material.
 *
 * @param fx Receives the pass handle.
 * @param mat Receives the material handle. */
static void mod_register(int *fx, int *mat)
{
    const int sh = openpete_mods_shader_register(k_self, "shaders/stress.frag");
    *fx = openpete_mods_postfx_register(k_self, sh, 0, 0);
    openpete_mods_postfx_enable(k_self, *fx, 1);
    openpete_mod_material_selector_t sel;
    memset(&sel, 0, sizeof sel);
    sel.struct_size = sizeof sel;
    sel.channel = -1; sel.ident = -1; sel.moby_class = -1; sel.texture = -1; sel.level = -1;
    *mat = openpete_mods_material_register(k_self, &sel, -1, sh, NULL);
}

/** @brief Time one writer call into the run's stats.
 *
 * @param w Writer index.
 * @param t0 SDL_GetTicksNS before the call. */
static void writer_time(int w, Uint64 t0)
{
    const uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > s_st.max_ns[w]) s_st.max_ns[w] = dt;
    s_st.sum_ns[w] += dt;
    s_st.calls[w]++;
}

/** @brief Run the per-tick writers, the enable toggle and a hot reload every 256 iterations until stopped.
 *
 * Swept material slots are never reused, so reloads stop short of the
 * registry's material table bound.
 *
 * @param arg Unused.
 * @return 0. */
static int SDLCALL guest_main(void *arg)
{
    (void)arg;
    int fx = -1, mat = -1;
    mod_register(&fx, &mat);
    unsigned char block[16] = {0};
    for (unsigned it = 0; !atomic_load(&s_guest_stop); it++) {
        block[0] = (unsigned char)it;
        /* Control: the same copy with no lock, so its maximum is the
         * scheduling jitter the writers' times carry too. */
        Uint64 t0 = SDL_GetTicksNS();
        static volatile unsigned char s_sink[16];
        memcpy((void *)s_sink, block, sizeof block);
        const uint64_t ctl = SDL_GetTicksNS() - t0;
        if (ctl > s_st.ctl_max_ns) s_st.ctl_max_ns = ctl;
        t0 = SDL_GetTicksNS();
        openpete_mods_material_set_params(k_self, mat, block, sizeof block);
        writer_time(W_PARAMS, t0);
        t0 = SDL_GetTicksNS();
        openpete_mods_postfx_set_uniforms(k_self, fx, block, sizeof block);
        writer_time(W_UNIFORMS, t0);
        t0 = SDL_GetTicksNS();
        openpete_mods_material_enable(k_self, mat, (int)(it & 1u));
        writer_time(W_ENABLE, t0);
        if ((it & 255u) == 255u && openpete_mods_material_enum_count() < 240) {
            t0 = SDL_GetTicksNS();
            openpete_mods_postfx_remove_owner(k_mod_id);
            openpete_mods_material_remove_owner(k_mod_id);
            openpete_mods_shader_remove_owner(k_mod_id);
            mod_register(&fx, &mat);
            writer_time(W_RELOAD, t0);
            s_st.reloads++;
        }
        SDL_DelayNS(200000);   /* 0.2 ms: well above the tick rate's write frequency */
    }
    return 0;
}

/* ---- the display owner --------------------------------------------------- */

/** @brief Return the calling thread's CPU time, ns; wall time where no thread clock exists.
 *
 * @return The time. */
static uint64_t thread_cpu_ns(void)
{
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return SDL_GetTicksNS();
#endif
}

/* Postfx desc the precompile builds for a pass: fullscreen vertex stage,
 * the pass's fragment words. */
static const rhi_pipeline_desc k_postfx_desc = {
    .vert_spv = "postfx_fs.vert.spv", .blend = RHI_BLEND_NONE, .postfx = true,
};
/* A depth-tested scene pipeline whose stages declare AFFINE_UV and NOTEX
 * and that the wireframe view selects: every view is a distinct create. */
static const rhi_pipeline_desc k_scene_desc = {
    .vert_spv = "quad.vert.spv", .frag_spv = "quad_key.frag.spv",
    .blend = RHI_BLEND_NONE, .depth_test = true,
    .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true,
};

/* Slots with a job in flight, by kind and index. */
static unsigned char s_pending[2][1024];

/** @brief Enqueue a job for every live pass and material slot that holds no pipeline and has none in flight.
 *
 * Runs under the registry read lock, as the compose's enqueue does.
 * @param compile false discards each job unrun (the no-compile run). */
static void enqueue_mod_slots(bool compile)
{
    const rhi_pipe_variant var = rhi_pipe_variant_current();
    const uint32_t epoch = rhi_pipe_variant_epoch();
    for (int i = 0; i < openpete_mods_postfx_enum_count() && i < 1024; i++) {
        openpete_postfx_pass_t p;
        if (openpete_mods_postfx_enum_at(i, &p) != 0 || *p.pipeline_slot || s_pending[0][i]) continue;
        rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, p.frag_spv,
                                                   p.frag_words, var, p.generation, epoch);
        if (!job) continue;
        s_st.enqueued++;
        if (!compile) { rhi_compile_job_discard(s_dev, job); s_st.unrun_discards++; continue; }
        qjob *q = calloc(1, sizeof *q);
        *q = (qjob){ .job = job, .kind = JOB_POSTFX, .index = i };
        s_pending[0][i] = 1;
        q_push(&s_q.todo, q);
    }
    for (int i = 0; i < openpete_mods_material_enum_count() && i < 1024; i++) {
        openpete_mod_material_gpu_t g;
        int contract = 0, terrain = 0;
        if (openpete_mods_material_enum_at(i, &contract, &terrain, &g) != 0 ||
            *g.pipeline_slot[0] || s_pending[1][i])
            continue;
        rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, g.frag_spv,
                                                   g.frag_words, var, g.generation, epoch);
        if (!job) continue;
        s_st.enqueued++;
        if (!compile) { rhi_compile_job_discard(s_dev, job); s_st.unrun_discards++; continue; }
        qjob *q = calloc(1, sizeof *q);
        *q = (qjob){ .job = job, .kind = JOB_MATERIAL, .index = i };
        s_pending[1][i] = 1;
        q_push(&s_q.todo, q);
    }
}

/** @brief Enqueue the built-in scene pipeline in one view and its five other views as variant jobs.
 *
 * @param cycle Cycle number; selects the installed view.
 * @param compile false discards each job unrun. */
static void enqueue_builtin(unsigned cycle, bool compile)
{
    for (int v = 0; v < 6; v++) {
        const rhi_pipe_variant var = { .affine = (v & 1) != 0, .notex = v == 2 || v == 3,
                                       .wireframe = v >= 4, .truescope = true };
        rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_scene_desc, NULL, 0, NULL, 0, var, 0,
                                                   rhi_pipe_variant_epoch());
        if (!job) continue;
        s_st.enqueued++;
        if (!compile) { rhi_compile_job_discard(s_dev, job); s_st.unrun_discards++; continue; }
        qjob *q = calloc(1, sizeof *q);
        *q = (qjob){ .job = job, .kind = (unsigned)v == cycle % 6u ? JOB_BUILTIN : JOB_VARIANT };
        q_push(&s_q.todo, q);
    }
}

/** @brief Install or discard every finished job on the display owner.
 *
 * A mod slot's result installs only when its slot, re-resolved by index,
 * still holds no pipeline and its generation equals the job's; otherwise it
 * is destroyed as Vulkan objects. A built-in result is installed, then
 * retired at once. Runs under the registry read lock. */
static void drain_done(void)
{
    SDL_LockMutex(s_q.mx);
    qjob *j = s_q.done;
    s_q.done = NULL;
    SDL_UnlockMutex(s_q.mx);
    while (j) {
        qjob *next = j->next;
        if (j->kind == JOB_BUILTIN) {
            rhi_pipeline *p = rhi_compile_job_install(s_dev, j->job);
            if (p) { s_st.installed++; rhi_retire_pipeline(s_dev, p); s_st.builtin_retired++; }
        } else {
            void **slot = NULL;
            unsigned gen = 0;
            openpete_postfx_pass_t pp;
            openpete_mod_material_gpu_t g;
            int contract = 0, terrain = 0;
            if (j->kind == JOB_POSTFX && openpete_mods_postfx_enum_at(j->index, &pp) == 0) {
                slot = pp.pipeline_slot; gen = pp.generation;
            } else if (j->kind == JOB_MATERIAL &&
                       openpete_mods_material_enum_at(j->index, &contract, &terrain, &g) == 0) {
                slot = g.pipeline_slot[0]; gen = g.generation;
            }
            s_pending[j->kind == JOB_MATERIAL][j->index] = 0;
            if (!slot || gen != rhi_compile_job_generation(j->job)) {
                rhi_compile_job_discard(s_dev, j->job);
                s_st.stale++;
            } else if (*slot) {
                rhi_compile_job_discard(s_dev, j->job);
                s_st.slot_taken++;
            } else {
                rhi_pipeline *p = rhi_compile_job_install(s_dev, j->job);
                if (p) { *slot = p; s_st.installed++; }
            }
        }
        free(j);
        j = next;
    }
}

/** @brief Retire the pipelines the registry's sweeps retired.
 *
 * Runs under the registry read lock, as the compose's drain does. */
static void drain_swept(void)
{
    void *dead[64];
    int n;
    while ((n = openpete_mods_postfx_take_retired(dead, 64)) > 0)
        for (int i = 0; i < n; i++) {
            rhi_retire_pipeline(s_dev, (rhi_pipeline *)dead[i]);
            s_st.swept_retired++;
        }
}

/** @brief Write environment variables as the overlay's live env rows do, from the display owner.
 *
 * @param cycle Cycle number.
 * @param shader_dir Directory for OPENPETE_SHADER_DIR on odd cycles, or NULL. */
static void env_edit(unsigned cycle, const char *shader_dir)
{
    char v[32];
    snprintf(v, sizeof v, "%u", cycle);
    SDL_setenv_unsafe("OPENPETE_STRESS_ROW", v, 1);
    if (cycle & 1u) SDL_unsetenv_unsafe("OPENPETE_STRESS_ALT");
    else SDL_setenv_unsafe("OPENPETE_STRESS_ALT", v, 1);
    if (!shader_dir) return;
    if (cycle & 1u) SDL_setenv_unsafe("OPENPETE_SHADER_DIR", shader_dir, 1);
    else SDL_unsetenv_unsafe("OPENPETE_SHADER_DIR");
}

/** @brief Run one timed leg: guest thread, workers when compiling, and the display owner's cycle.
 *
 * @param workers Worker count; 0 discards every job unrun.
 * @param seconds Duration.
 * @param shader_dir See env_edit. */
static void run_leg(int workers, int seconds, const char *shader_dir)
{
    memset(&s_st, 0, sizeof s_st);
    memset(s_pending, 0, sizeof s_pending);
    atomic_store(&s_guest_stop, false);
    s_q.stop = false;
    SDL_Thread *guest = SDL_CreateThread(guest_main, "stress-guest", NULL);
    SDL_Thread *w[64];
    const int nw = workers < 64 ? workers : 64;
    for (int i = 0; i < nw; i++) w[i] = SDL_CreateThread(worker_main, "stress-worker", NULL);
    const Uint64 end = SDL_GetTicksNS() + (Uint64)seconds * 1000000000ull;
    for (unsigned cycle = 0; SDL_GetTicksNS() < end; cycle++) {
        const bool frame = rhi_begin_frame(s_dev);
        openpete_mods_registry_read_lock();
        const Uint64 h0 = SDL_GetTicksNS();
        const uint64_t c0 = thread_cpu_ns();
        drain_swept();
        drain_done();
        enqueue_mod_slots(nw > 0);
        if ((cycle & 7u) == 0) enqueue_builtin(cycle, nw > 0);
        const uint64_t hold = SDL_GetTicksNS() - h0;
        const uint64_t hold_cpu = thread_cpu_ns() - c0;
        openpete_mods_registry_read_unlock();
        if (hold_cpu > s_st.hold_cpu_max_ns) s_st.hold_cpu_max_ns = hold_cpu;
        if (hold > s_st.hold_max_ns) s_st.hold_max_ns = hold;
        s_st.hold_sum_ns += hold;
        s_st.holds++;
        if (frame) rhi_end_frame(s_dev);
        env_edit(cycle, shader_dir);
        SDL_DelayNS(1000000);
    }
    atomic_store(&s_guest_stop, true);
    SDL_WaitThread(guest, NULL);
    SDL_LockMutex(s_q.mx);
    s_q.stop = true;
    SDL_BroadcastCondition(s_q.cv);
    SDL_UnlockMutex(s_q.mx);
    for (int i = 0; i < nw; i++) SDL_WaitThread(w[i], NULL);
    for (qjob *j = s_q.todo; j;) { qjob *n = j->next; rhi_compile_job_discard(s_dev, j->job); free(j); j = n; }
    s_q.todo = NULL;
    openpete_mods_registry_read_lock();
    drain_done();
    drain_swept();
    openpete_mods_registry_read_unlock();
}

/** @brief Print one leg's counts and writer times.
 *
 * @param name Leg name. */
static void report(const char *name)
{
    printf("leg=%s enqueued=%u ran=%u ran_ok=%u installed=%u stale_discarded=%u slot_taken=%u "
           "worker_discarded=%u unrun_discarded=%u builtin_retired=%u swept_retired=%u reloads=%u\n",
           name, s_st.enqueued, atomic_load(&s_st.ran), atomic_load(&s_st.ran_ok), s_st.installed,
           s_st.stale, s_st.slot_taken, atomic_load(&s_st.worker_discards), s_st.unrun_discards,
           s_st.builtin_retired, s_st.swept_retired, s_st.reloads);
    for (int w = 0; w < 4; w++)
        printf("leg=%s writer=%s calls=%u max_ms=%.3f mean_ms=%.4f\n", name, k_writer[w],
               s_st.calls[w], (double)s_st.max_ns[w] / 1e6,
               s_st.calls[w] ? (double)s_st.sum_ns[w] / 1e6 / s_st.calls[w] : 0.0);
    printf("leg=%s read_holds=%u hold_max_ms=%.3f hold_cpu_max_ms=%.3f hold_mean_ms=%.4f "
           "control_max_ms=%.3f\n", name, s_st.holds, (double)s_st.hold_max_ns / 1e6,
           (double)s_st.hold_cpu_max_ns / 1e6,
           s_st.holds ? (double)s_st.hold_sum_ns / 1e6 / s_st.holds : 0.0,
           (double)s_st.ctl_max_ns / 1e6);
}

/** @brief Return the growth bound of a no-compile maximum: twice it or 5 ms more, whichever is larger.
 *
 * @param base The no-compile maximum, ns.
 * @return The bound, ns. */
static uint64_t growth_bound(uint64_t base)
{
    return base * 2 > base + 5000000u ? base * 2 : base + 5000000u;
}

/** @brief Copy the embedded raster-quantize fragment's SPIR-V as the words every stress mod shader compiles to.
 *
 * @return true when the blob was found. */
static bool load_frag_words(void)
{
    const void *b = NULL;
    size_t n = 0;
    for (unsigned i = 0; i < g_rhi_embedded_shader_count; i++)
        if (strcmp(g_rhi_embedded_shaders[i].name, "raster_quantize.frag.spv") == 0) {
            b = g_rhi_embedded_shaders[i].data;
            n = g_rhi_embedded_shaders[i].size;
        }
    if (!b || n < 4 || (n & 3u)) return false;
    uint32_t *w = malloc(n);
    if (!w) return false;
    memcpy(w, b, n);
    s_frag_words = w;
    s_frag_n = n / 4;
    return true;
}

/** @brief Install and retire one pipeline off the display owner: the seam check's negative control.
 *
 * @param arg A compiled job.
 * @return 0. */
static int SDLCALL seam_control_main(void *arg)
{
    rhi_pipeline *p = rhi_compile_job_install(s_dev, (rhi_compile_job *)arg);
    if (p) rhi_retire_pipeline(s_dev, p);
    return 0;
}

/** @brief Run the no-compile leg and the worker leg and judge the worker leg.
 *
 * @param argc Argument count.
 * @param argv workers, seconds, shader_dir (all optional).
 * @return 0 pass, 1 fail, 77 no Vulkan device. */
int main(int argc, char **argv)
{
    const int workers = argc > 1 ? atoi(argv[1]) : 4;
    const int seconds = argc > 2 ? atoi(argv[2]) : 5;
    const char *shader_dir = argc > 3 ? argv[3] : NULL;
    SDL_setenv_unsafe("MESA_SHADER_CACHE_DISABLE", "true", 1);   /* no driver file IO on workers */
    snprintf(s_mod_dir, sizeof s_mod_dir, "%s", "rhi_compile_stress.d");
    SDL_CreateDirectory(s_mod_dir);
    s_q.mx = SDL_CreateMutex();
    s_q.cv = SDL_CreateCondition();
#if defined(__linux__)
    (void)in_exe((const void *)&main);   /* resolve the executable's name before any thread */
#endif
    /* The registry creates its lock on first use, unsynchronized: take it
     * once here, as mod init does on the boot thread, before any thread. */
    openpete_mods_registry_read_lock();
    openpete_mods_registry_read_unlock();
    s_dev = rhi_create_headless("rhi_compile_stress", 64, 64);
    if (!s_dev) { fprintf(stderr, "rhi_compile_stress: no Vulkan device - SKIP\n"); return 77; }
    if (!load_frag_words()) { fprintf(stderr, "rhi_compile_stress: no raster_quantize.frag.spv\n"); return 1; }

    run_leg(0, seconds, shader_dir);
    report("no_compile");
    uint64_t base_max[4];
    for (int w = 0; w < 4; w++) base_max[w] = s_st.max_ns[w];
    const uint64_t base_hold = s_st.hold_cpu_max_ns;
    atomic_store(&s_worker_getenv, 0u);
    atomic_store(&s_worker_fopen, 0u);
    run_leg(workers, seconds, shader_dir);
    report("compile");

    /* Workers take no registry lock, so a writer waits only on the display
     * owner's read holds. The holds' CPU time (wall time minus preemption)
     * is judged against the no-compile run; a writer's maximum is judged
     * likewise, and a rise the compile leg's longest wall hold plus its
     * scheduling jitter (the control) accounts for is reported, not failed:
     * CPU contention from the workers stretches a hold, the hold's work
     * does not grow. */
    int fail = 0;
    if (!atomic_load(&s_st.ran_ok) || !s_st.installed) {
        printf("verdict: VACUOUS (no job compiled or installed)\n");
        fail = 1;
    }
    if (s_st.hold_cpu_max_ns > growth_bound(base_hold)) {
        printf("verdict: GROWTH read_hold cpu max_ms %.3f -> %.3f\n", (double)base_hold / 1e6,
               (double)s_st.hold_cpu_max_ns / 1e6);
        fail = 1;
    }
    for (int w = 0; w < 4; w++) {
        if (s_st.max_ns[w] <= growth_bound(base_max[w])) continue;
        const bool explained = s_st.max_ns[w] <= s_st.hold_max_ns + s_st.ctl_max_ns + 1000000u;
        printf("verdict: %s writer=%s max_ms %.3f -> %.3f (compile leg hold max %.3f + control max %.3f)\n",
               explained ? "JITTER" : "GROWTH", k_writer[w], (double)base_max[w] / 1e6,
               (double)s_st.max_ns[w] / 1e6, (double)s_st.hold_max_ns / 1e6,
               (double)s_st.ctl_max_ns / 1e6);
        if (!explained) fail = 1;
    }
    const unsigned seams = rhi_seam_violations();
    const unsigned genv = atomic_load(&s_worker_getenv), gfo = atomic_load(&s_worker_fopen);
    printf("driver_getenv=%u driver_fopen=%u (worker-thread calls from a shared library)\n",
           atomic_load(&s_driver_getenv), atomic_load(&s_driver_fopen));
    printf("workers=%d seconds=%d seam_violations=%u worker_getenv=%u worker_fopen=%u%s\n",
           workers, seconds, seams, genv, gfo,
#if defined(__linux__)
           ""
#else
           " (not interposed on this platform)"
#endif
    );
    if (seams || genv || gfo) fail = 1;

    /* Negative control: a registry link and unlink from a thread that does
     * not own the display must each count one seam violation. */
    rhi_compile_job *cj = rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, s_frag_words,
                                              s_frag_n, rhi_pipe_variant_current(), 0, 0);
    if (cj && rhi_compile_job_run(s_dev, cj, 0) == 0) {
        SDL_WaitThread(SDL_CreateThread(seam_control_main, "stress-seam", cj), NULL);
    } else {
        rhi_compile_job_discard(s_dev, cj);
    }
    const unsigned seam_hits = rhi_seam_violations() - seams;
    printf("seam_control: off-owner link+unlink counted %u violation(s) (want 2): %s\n", seam_hits,
           seam_hits == 2 ? "engaged" : "NOT ENGAGED");
    if (seam_hits != 2) fail = 1;

    openpete_mods_postfx_retire_all();
    openpete_mods_material_retire_all();
    rhi_wait_idle(s_dev);
    openpete_mods_registry_read_lock();
    drain_swept();
    openpete_mods_registry_read_unlock();
    rhi_destroy(s_dev);
    printf("verdict: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
