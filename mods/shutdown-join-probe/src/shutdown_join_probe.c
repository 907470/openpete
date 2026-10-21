/* shutdown-join-probe - a mod thread stopped and joined by the shutdown
 * hook (see mod.toml).
 *
 * The thread registers a 1x1 image under PROBE_ID every 2 ms. Each
 * registration keeps its superseded image for the session and logs a
 * line, so the image is the smallest the entry accepts and the period
 * the longest that still lands a call inside the engine's teardown after
 * the exit pass (about 30 ms) in the control leg. The source reads no guest RAM
 * and names no game symbol, so it builds for every game. */
#include <openpete_mod_api.h>

#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define PROBE_ID 0x7E02u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static atomic_int  s_stop;
static atomic_uint s_calls;     /* texture_register calls the thread made */
static atomic_uint s_refused;   /* calls that returned non-zero */
#ifdef _WIN32
static HANDLE    s_thread;
#else
static pthread_t s_thread;
#endif

/** @brief Sleep about two milliseconds. */
static void nap_2ms(void) {
#ifdef _WIN32
    Sleep(2);
#else
    struct timespec ts = { 0, 2000000L };
    nanosleep(&ts, NULL);
#endif
}

/** @brief Thread body: register PROBE_ID until the stop flag is set. @param arg Unused. */
static void worker_main(void* arg) {
    static const unsigned char px[4] = { 0, 255, 255, 255 };
    (void)arg;
    while (!atomic_load(&s_stop)) {
        if (g_api->texture_register(g_self, (uint16_t)PROBE_ID, px, 1, 1) != 0)
            atomic_fetch_add(&s_refused, 1u);
        atomic_fetch_add(&s_calls, 1u);
        nap_2ms();
    }
}

#ifdef _WIN32
/** @brief Win32 thread trampoline into worker_main. @param a Unused. @return 0. */
static DWORD WINAPI thread_entry(LPVOID a) { worker_main(a); return 0; }
#else
/** @brief pthread trampoline into worker_main. @param a Unused. @return NULL. */
static void* thread_entry(void* a) { worker_main(a); return NULL; }
#endif

/** @brief Start the worker thread joinable. @return 0, or -1. */
static int start_thread(void) {
#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, thread_entry, NULL, 0, NULL);
    return s_thread ? 0 : -1;
#else
    return pthread_create(&s_thread, NULL, thread_entry, NULL) == 0 ? 0 : -1;
#endif
}

/** @brief Shutdown hook: stop the worker, join it, and log what it did. */
static void on_shutdown(void) {
    atomic_store(&s_stop, 1);
#ifdef _WIN32
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
#else
    pthread_join(s_thread, NULL);
#endif
    g_api->log(g_self, OP_MOD_LOG_INFO, "shutdown-join-probe: worker joined "
               "after %u registrations (%u failed)",
               atomic_load(&s_calls), atomic_load(&s_refused));
}

/** @brief Start the worker and, unless `hook = false`, register the shutdown hook.
 * @param api Engine api. @param self This mod.
 * @return 0, or 1 when the api is older than 13 or the thread cannot start. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version < 13u) return 1;
    const int hook = api->config_bool(self, "hook", 1);
    if (start_thread() != 0) {
        api->log(self, OP_MOD_LOG_ERROR, "shutdown-join-probe: thread start failed");
        return 1;
    }
    if (hook && api->register_shutdown_hook(self, on_shutdown) != 0) {
        api->log(self, OP_MOD_LOG_ERROR, "shutdown-join-probe: hook refused");
        return 1;
    }
    api->log(self, OP_MOD_LOG_INFO, "shutdown-join-probe: worker started, "
             "shutdown hook %s", hook ? "registered" : "not registered");
    return 0;
}
