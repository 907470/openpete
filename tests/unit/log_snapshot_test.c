/* log_snapshot_test - the logger's level filter, spec and format under
 * concurrent reconfiguration.
 *
 * Modes:
 *   selftest            exit 0 at once; the sanitizer runner uses it to
 *                       learn whether the sanitizer runtime starts here.
 *   semantics           single-threaded checks of the public behaviour:
 *                       rule precedence, the default level, OPENPETE_LOG
 *                       layering, spec read-back and truncation, format.
 *   race [secs] [nlog] [max_sets]
 *                       one thread loops openpete_log_set_spec and
 *                       openpete_log_set_format while nlog threads log
 *                       filtered lines and one thread reads the spec back.
 *                       Built with -fsanitize=address it fails on a rule
 *                       list freed under a reader; with -fsanitize=thread on
 *                       any unsynchronized access. A spec read back that is
 *                       neither of the two specs set is a torn read and
 *                       fails the run. max_sets bounds the writer, since
 *                       every retired configuration stays allocated.
 *
 * log.c is compiled straight in; its output goes to stderr, which the
 * semantics mode points at a temporary file to observe what was written. */

#include "openpete_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stdout, "FAIL %s:%d: ", __FILE__, __LINE__);     \
            fprintf(stdout, __VA_ARGS__);                             \
            fputc('\n', stdout);                                      \
            g_fail = 1;                                               \
        }                                                             \
    } while (0)

/* ---- semantics ---------------------------------------------------------- */

static FILE* g_sink;

/** @brief Return the size in bytes of the file stderr currently writes to. */
static long sink_size(void) {
    struct stat st;
    fflush(stderr);
    if (fstat(fileno(g_sink), &st) != 0) return -1;
    return (long)st.st_size;
}

/** @brief Log one line and report whether it reached stderr.
 *
 * @param level Severity passed to openpete_logf.
 * @param sub Subsystem passed to openpete_logf.
 * @return 1 when the line was written, 0 when the filter dropped it. */
static int emitted(openpete_log_level_t level, const char* sub) {
    long before = sink_size();
    openpete_logf(level, sub, "m");
    return sink_size() > before;
}

/** @brief Read the last line written to stderr into buf.
 *
 * @param buf Receives the line without its newline.
 * @param cap Size of buf. */
static void last_line(char* buf, size_t cap) {
    char all[8192];
    fflush(stderr);
    long n = (long)pread(fileno(g_sink), all, sizeof all - 1, 0);
    if (n < 0) n = 0;
    all[n] = '\0';
    if (n > 0 && all[n - 1] == '\n') all[--n] = '\0';
    const char* nl = strrchr(all, '\n');
    snprintf(buf, cap, "%s", nl ? nl + 1 : all);
}

/** @brief Check every public behaviour of the level filter, the spec read-back and the format. */
static int run_semantics(void) {
    g_sink = tmpfile();
    if (!g_sink || dup2(fileno(g_sink), STDERR_FILENO) < 0) {
        fprintf(stdout, "FAIL: cannot redirect stderr\n");
        return 1;
    }
    openpete_log_pin_stderr_tty(0);
    unsetenv("OPENPETE_LOG");

    /* Before any init: default info, plain, empty spec. */
    CHECK(!emitted(OPL_DEBUG, "a"), "pre-init debug dropped");
    CHECK(emitted(OPL_INFO, "a"), "pre-init info written");
    CHECK(strcmp(openpete_log_get_spec(), "") == 0, "pre-init spec empty");
    CHECK(openpete_log_get_format() == OPENPETE_LOG_PLAIN, "pre-init plain");

    /* The most recently added of equal names wins. */
    openpete_log_init(OPENPETE_LOG_PLAIN, "a:debug,a:error,b:trace");
    CHECK(!emitted(OPL_WARN, "a"), "a:error after a:debug drops warn");
    CHECK(emitted(OPL_ERROR, "a"), "a:error writes error");
    CHECK(emitted(OPL_TRACE, "b"), "b:trace writes trace");
    CHECK(!emitted(OPL_DEBUG, "c"), "unnamed subsystem takes default info");
    CHECK(emitted(OPL_INFO, "c"), "unnamed subsystem writes info");
    CHECK(!emitted(OPL_DEBUG, NULL), "NULL subsystem filters as the default");
    CHECK(strcmp(openpete_log_get_spec(), "a:debug,a:error,b:trace") == 0,
          "spec read back");

    /* Exact match only, "*" sets the default, malformed segments skipped. */
    openpete_log_set_spec("ab:trace,*:warn,nocolon,c:bogus");
    CHECK(!emitted(OPL_INFO, "a"), "prefix of a rule name does not match");
    CHECK(emitted(OPL_WARN, "a"), "default warn writes warn");
    CHECK(emitted(OPL_TRACE, "ab"), "exact name matches");
    CHECK(!emitted(OPL_INFO, "c"), "unknown level segment skipped");
    CHECK(!emitted(OPL_TRACE, "b"), "set_spec clears earlier rules");

    /* The empty name is a rule NULL subsystems match. */
    openpete_log_set_spec(":trace");
    CHECK(emitted(OPL_TRACE, NULL), "rule for the empty name matches NULL");

    /* NULL and empty clear to plain *:info. */
    openpete_log_set_spec(NULL);
    CHECK(strcmp(openpete_log_get_spec(), "") == 0, "NULL spec reads back empty");
    CHECK(!emitted(OPL_DEBUG, "a"), "NULL spec: default info");
    CHECK(emitted(OPL_INFO, "a"), "NULL spec: info written");

    /* set_spec does not layer on OPENPETE_LOG; init does, spec last. */
    setenv("OPENPETE_LOG", "e:error,f:trace", 1);
    openpete_log_set_spec("g:debug");
    CHECK(emitted(OPL_INFO, "e"), "set_spec ignores the environment");
    openpete_log_init(OPENPETE_LOG_PLAIN, "e:debug");
    CHECK(emitted(OPL_DEBUG, "e"), "init spec overrides the environment");
    CHECK(emitted(OPL_TRACE, "f"), "init keeps environment rules");
    CHECK(!emitted(OPL_DEBUG, "g"), "init clears set_spec rules");
    CHECK(strcmp(openpete_log_get_spec(), "e:debug") == 0, "init reads back spec");
    openpete_log_init(OPENPETE_LOG_PLAIN, NULL);
    CHECK(strcmp(openpete_log_get_spec(), "e:error,f:trace") == 0,
          "init with NULL spec reads back the environment");
    CHECK(!emitted(OPL_WARN, "e"), "environment rule applies");
    unsetenv("OPENPETE_LOG");

    /* Spec read-back truncates at 127 bytes. */
    char longspec[200];
    memset(longspec, 'x', sizeof longspec - 1);
    longspec[sizeof longspec - 1] = '\0';
    openpete_log_set_spec(longspec);
    CHECK(strlen(openpete_log_get_spec()) == 127, "spec read-back truncated to 127");

    /* A pointer from get_spec stays valid and unchanged after a later set. */
    openpete_log_set_spec("h:debug");
    const char* held = openpete_log_get_spec();
    openpete_log_set_spec("i:debug");
    CHECK(strcmp(held, "h:debug") == 0, "held spec pointer unchanged");
    CHECK(strcmp(openpete_log_get_spec(), "i:debug") == 0, "new spec read back");

    /* Format switches survive a spec change and vice versa. */
    openpete_log_set_format(OPENPETE_LOG_JSON);
    openpete_log_set_spec("j:trace");
    CHECK(openpete_log_get_format() == OPENPETE_LOG_JSON, "spec keeps format");
    CHECK(emitted(OPL_TRACE, "j"), "format keeps spec");
    char line[512];
    last_line(line, sizeof line);
    CHECK(strncmp(line, "{\"t\":", 5) == 0 &&
          strstr(line, "\"level\":\"trace\",\"subsystem\":\"j\",\"msg\":\"m\"}"),
          "json line: %s", line);
    openpete_log_set_format(OPENPETE_LOG_PLAIN);
    CHECK(emitted(OPL_INFO, "k"), "plain info written");
    last_line(line, sizeof line);
    CHECK(strstr(line, " [info ] k: m") != NULL, "plain line: %s", line);

    /* init sets the format and keeps the timestamp origin. */
    openpete_log_init(OPENPETE_LOG_JSON, NULL);
    CHECK(openpete_log_get_format() == OPENPETE_LOG_JSON, "init sets format");
    emitted(OPL_INFO, "t");
    last_line(line, sizeof line);
    double t = atof(line + 5);
    CHECK(t >= 0.0 && t < 60.0, "timestamp relative to the first init: %s", line);

    fprintf(stdout, "semantics: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}

/* ---- race --------------------------------------------------------------- */

static const char k_spec_a[] =
    "a:debug,b:trace,c:warn,d:error,e:debug,f:trace,g:info,h:debug";
static const char k_spec_b[] = "x:debug,y:trace";

static atomic_int g_stop;
static atomic_long g_logs, g_sets, g_torn;

/** @brief Log filtered lines until stopped; each call walks the whole rule list and writes nothing. */
static void* logger(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        openpete_logf(OPL_TRACE, "zzz-not-in-spec", "x");
        atomic_fetch_add(&g_logs, 1);
    }
    return NULL;
}

/** @brief Read the spec back until stopped, counting reads that match neither spec set. */
static void* spec_reader(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", openpete_log_get_spec());
        if (strcmp(buf, k_spec_a) != 0 && strcmp(buf, k_spec_b) != 0)
            atomic_fetch_add(&g_torn, 1);
        (void)openpete_log_get_format();
    }
    return NULL;
}

/** @brief Run the reconfiguration race and report its counters.
 *
 * @param secs Wall-clock bound of the writer loop.
 * @param nlog Number of logging threads, 1 to 16.
 * @param max_sets Bound on the number of set_spec calls.
 * @return 0 when no torn spec read was seen. */
static int run_race(int secs, int nlog, long max_sets) {
    if (nlog < 1) nlog = 1;
    if (nlog > 16) nlog = 16;
    /* Every line logged here is filtered, so stderr carries only the
     * sanitizer's report. */
    openpete_log_init(OPENPETE_LOG_PLAIN, k_spec_a);
    pthread_t th[16], rs;
    for (int i = 0; i < nlog; i++) pthread_create(&th[i], NULL, logger, NULL);
    pthread_create(&rs, NULL, spec_reader, NULL);
    time_t end = time(NULL) + secs;
    long k = 0;
    while (time(NULL) < end && k < max_sets) {
        openpete_log_set_spec((k & 1) ? k_spec_b : k_spec_a);
        if ((k & 255) == 0)
            openpete_log_set_format((k & 256) ? OPENPETE_LOG_JSON : OPENPETE_LOG_PLAIN);
        k++;
        atomic_fetch_add(&g_sets, 1);
    }
    atomic_store(&g_stop, 1);
    for (int i = 0; i < nlog; i++) pthread_join(th[i], NULL);
    pthread_join(rs, NULL);
    long torn = atomic_load(&g_torn);
    fprintf(stdout, "race: sets=%ld logs=%ld torn_spec_reads=%ld: %s\n",
            atomic_load(&g_sets), atomic_load(&g_logs), torn,
            torn ? "FAIL" : "PASS");
    return torn ? 1 : 0;
}

/** @brief Dispatch on the mode argument; see the file comment. */
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "semantics";
    if (strcmp(mode, "selftest") == 0) return 0;
    if (strcmp(mode, "semantics") == 0) return run_semantics();
    if (strcmp(mode, "race") == 0) {
        int secs = argc > 2 ? atoi(argv[2]) : 2;
        int nlog = argc > 3 ? atoi(argv[3]) : 3;
        long max_sets = argc > 4 ? atol(argv[4]) : 200000;
        return run_race(secs, nlog, max_sets);
    }
    fprintf(stderr, "usage: %s selftest|semantics|race [secs] [nlog] [max_sets]\n", argv[0]);
    return 2;
}
