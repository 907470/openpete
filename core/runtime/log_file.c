// core/runtime/log_file.c — mirror stderr into a rotating log file.
//
// A GUI launch has no terminal attached: a double-clicked AppImage, a
// .desktop entry, a file manager's "Open with". Everything the runtime
// reports goes to stderr and is lost there, so a user who hits a problem
// has nothing to attach to a report.
//
// The sink is an fd-level tee rather than a hook inside openpete_logf()
// on purpose: when the game fails to start at all, the messages that
// explain why are the ones we do NOT emit ourselves — the Vulkan loader
// finding no driver, SDL failing to open a display, the dynamic linker's
// own complaints. A logger-level tee would capture none of them.
//
// stderr is dup2'd onto a pipe; a pump thread copies every byte to the
// real stderr and to the file. Terminal and piped output are therefore
// unchanged, which matters because every gate script captures stderr.

#include "openpete_log.h"
#include "openpete_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#  include <io.h>
#  include <sys/stat.h>
#  define op_pipe(fds)      _pipe((fds), 65536, _O_BINARY)
#  define op_dup(fd)        _dup(fd)
#  define op_dup2(a, b)     _dup2((a), (b))
#  define op_read(f, b, n)  _read((f), (b), (unsigned)(n))
#  define op_write(f, b, n) _write((f), (b), (unsigned)(n))
#  define op_close(fd)      _close(fd)
#  define OP_OPEN_MODE      (_S_IREAD | _S_IWRITE)
#  define OP_PATH_SEP       '\\'
#else
#  include <sys/file.h>
#  include <unistd.h>
#  define op_pipe(fds)      pipe(fds)
#  define op_dup(fd)        dup(fd)
#  define op_dup2(a, b)     dup2((a), (b))
#  define op_read(f, b, n)  read((f), (b), (n))
#  define op_write(f, b, n) write((f), (b), (n))
#  define op_close(fd)      close(fd)
#  define OP_OPEN_MODE      0644
#  define OP_PATH_SEP       '/'
#  ifndef O_BINARY
#    define O_BINARY 0
#  endif
#endif

/* Enough for a very long session at info level, small enough that a
 * runaway trace-level subsystem cannot fill the user's disk. */
#define LOG_CAP_BYTES (16u * 1024u * 1024u)

/* Matches paths.c's own buffer size; the paths API takes a caller
 * buffer rather than exporting the constant. */
#define LOG_PATH_MAX 1024

static int         s_log_fd      = -1;
static int         s_real_stderr = -1;
static int         s_pipe_r      = -1;
static SDL_Thread* s_pump        = NULL;
static char        s_path[LOG_PATH_MAX];
static size_t      s_written     = 0;
static int         s_truncated   = 0;
#if !defined(_WIN32)
static int         s_lock_fd     = -1;
#endif

static void write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        long w = (long)op_write(fd, buf, n);
        if (w > 0) { buf += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return;  /* the sink is gone; dropping output beats spinning */
    }
}

/* Strip ANSI escapes on the way to the file. The terminal leg keeps its
 * colour (the logger emits it whenever the real stderr is a tty), but a
 * file full of \033[33m is worse than useless in a bug report. Only CSI
 * sequences occur here; the state carries across reads because a
 * sequence can straddle a pipe buffer boundary. */
static int s_esc = 0;  /* 0 outside, 1 saw ESC, 2 inside CSI */

static size_t strip_ansi(const char* in, size_t n, char* out) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (s_esc == 0) {
            if (c == '\033') { s_esc = 1; continue; }
            out[w++] = c;
        } else if (s_esc == 1) {
            s_esc = (c == '[') ? 2 : 0;
        } else if (c >= '@' && c <= '~') {
            s_esc = 0;  /* final byte of the sequence */
        }
    }
    return w;
}

static int SDLCALL pump_thread(void* unused) {
    (void)unused;
    char buf[4096];
    char clean[4096];
    for (;;) {
        long n = (long)op_read(s_pipe_r, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;  /* 0 = every writer closed; the process is exiting */

        write_all(s_real_stderr, buf, (size_t)n);

        if (s_log_fd < 0 || s_truncated) continue;
        size_t m = strip_ansi(buf, (size_t)n, clean);
        if (m == 0) continue;
        if (s_written + m > LOG_CAP_BYTES) {
            static const char note[] = "\n[log truncated: size cap reached]\n";
            write_all(s_log_fd, note, sizeof note - 1);
            s_truncated = 1;
        } else {
            write_all(s_log_fd, clean, m);
            s_written += m;
        }
    }
    return 0;
}

static int resolve_path(const char* spec, char* out, size_t cap) {
    if (spec) {
        if (strlen(spec) >= cap) return -1;
        snprintf(out, cap, "%s", spec);
        return 0;
    }
    char dir[LOG_PATH_MAX];
    if (openpete_paths_state_dir("logs", dir, sizeof dir, 1) != 0) return -1;
    if (snprintf(out, cap, "%s%copenpete.log", dir, OP_PATH_SEP) >= (int)cap)
        return -1;
    return 0;
}

/* One writer per file. A second concurrent instance — a gate running
 * headless copies in parallel, or a user launching twice — would
 * interleave into the same file and race the rotation, so it runs
 * without a file sink instead. */
static int take_lock(const char* path) {
#if defined(_WIN32)
    (void)path;
    return 0;
#else
    char lock[LOG_PATH_MAX + 8];
    if (snprintf(lock, sizeof lock, "%s.lock", path) >= (int)sizeof lock)
        return -1;
    s_lock_fd = open(lock, O_CREAT | O_WRONLY | O_CLOEXEC, OP_OPEN_MODE);
    if (s_lock_fd < 0) return -1;
    if (flock(s_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(s_lock_fd);
        s_lock_fd = -1;
        return -1;
    }
    return 0;
#endif
}

static void drop_lock(void) {
#if !defined(_WIN32)
    if (s_lock_fd >= 0) { close(s_lock_fd); s_lock_fd = -1; }
#endif
}

int openpete_log_file_open(const char* spec) {
    if (s_pump) return 0;  /* already open */

    if (!spec) spec = getenv("OPENPETE_LOG_FILE");
    if (spec && (!*spec || strcmp(spec, "off") == 0 || strcmp(spec, "0") == 0))
        return -1;

    if (resolve_path(spec, s_path, sizeof s_path) != 0) return -1;
    if (take_lock(s_path) != 0) return -1;

    /* Keep one generation. A crash that a relaunch papers over is the
     * common report ("it worked the second time"), and the run that
     * failed is the one worth reading. */
    char prev[LOG_PATH_MAX + 4];
    if (snprintf(prev, sizeof prev, "%s.1", s_path) < (int)sizeof prev) {
        remove(prev);
        rename(s_path, prev);
    }

    s_log_fd = open(s_path, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY,
                    OP_OPEN_MODE);
    if (s_log_fd < 0) { drop_lock(); return -1; }

    int fds[2];
    if (op_pipe(fds) != 0) goto fail_file;
    s_pipe_r = fds[0];

    s_real_stderr = op_dup(2);
    if (s_real_stderr < 0) { op_close(fds[1]); goto fail_pipe; }

    /* Settle the colour question against the real stderr, before fd 2
     * becomes a pipe and the logger concludes it is not a terminal. */
#if defined(_WIN32)
    openpete_log_pin_stderr_tty(_isatty(s_real_stderr));
#else
    openpete_log_pin_stderr_tty(isatty(s_real_stderr));
#endif

    setvbuf(stderr, NULL, _IONBF, 0);
    if (op_dup2(fds[1], 2) < 0) { op_close(fds[1]); goto fail_dup; }
    /* fd 2 is now the only writer, so restoring it in close() is what
     * gives the pump its EOF. */
    op_close(fds[1]);

    s_pump = SDL_CreateThread(pump_thread, "openpete-log", NULL);
    if (!s_pump) { op_dup2(s_real_stderr, 2); goto fail_dup; }

    atexit(openpete_log_file_close);
    return 0;

fail_dup:
    op_close(s_real_stderr);
    s_real_stderr = -1;
fail_pipe:
    op_close(s_pipe_r);
    s_pipe_r = -1;
fail_file:
    op_close(s_log_fd);
    s_log_fd = -1;
    drop_lock();
    return -1;
}

void openpete_log_file_close(void) {
    if (!s_pump) return;

    fflush(stderr);
    op_dup2(s_real_stderr, 2);  /* last pipe writer goes away -> pump EOF */
    /* The EOF depends on no live child still holding an inherited fd 2.
     * Every process this runtime spawns (the first-boot compiler and
     * linker, the mod compiler) is waited on before shutdown, and their
     * output landing in the log is exactly why fd 2 is inherited. A
     * future daemonized child would have to close it. */
    SDL_WaitThread(s_pump, NULL);
    s_pump = NULL;

    op_close(s_pipe_r);      s_pipe_r      = -1;
    op_close(s_real_stderr); s_real_stderr = -1;
    op_close(s_log_fd);      s_log_fd      = -1;
    drop_lock();
}

const char* openpete_log_file_path(void) {
    return s_pump ? s_path : "";
}

void openpete_log_file_sync(void) {
    if (!s_pump) return;
    fflush(stderr);
    /* Give the pump a moment to drain what the caller just wrote. Called
     * from the crash handler, which is already past the point of being
     * signal-safe (it uses fprintf and backtrace); without this the
     * crash dump can die in the pipe with the process. */
    SDL_Delay(50);
}
