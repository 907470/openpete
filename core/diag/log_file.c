// core/diag/log_file.c - mirror stderr into a rotating log file.
//
// A GUI launch has no terminal attached: a double-clicked AppImage, a
// .desktop entry, a file manager's "Open with". Everything the runtime
// reports goes to stderr and is lost there, so a user who hits a problem
// has nothing to attach to a report.
//
// The sink is an fd-level tee rather than a hook inside openpete_logf()
// on purpose: when the game fails to start at all, the messages that
// explain why are the ones we do NOT emit ourselves - the Vulkan loader
// finding no driver, SDL failing to open a display, the dynamic linker's
// own complaints. A logger-level tee would capture none of them.
//
// stderr is dup2'd onto a pipe; a pump thread copies every byte to the
// real stderr and to the file. Terminal and piped output are therefore
// unchanged, which matters because every gate script captures stderr.
//
// The pump is a native thread, not an SDL one: the close runs from atexit,
// after the renderer teardown has called SDL_Quit, and SDL_Quit invalidates
// every SDL_Thread handle, so SDL_WaitThread would return without joining
// and the process would exit with the tail of stderr still in the pipe.

#include "openpete_log.h"
#include "openpete_crash.h"
#include "openpete_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <sys/stat.h>
#  include <windows.h>
#  include <process.h>
#  define op_pipe(fds)      _pipe((fds), 65536, _O_BINARY)
#  define op_dup(fd)        _dup(fd)
#  define op_dup2(a, b)     _dup2((a), (b))
#  define op_read(f, b, n)  _read((f), (b), (unsigned)(n))
#  define op_write(f, b, n) _write((f), (b), (unsigned)(n))
#  define op_close(fd)      _close(fd)
#  define op_sleep_ms(ms)   Sleep(ms)
#  define OP_OPEN_MODE      (_S_IREAD | _S_IWRITE)
#  define OP_PATH_SEP       '\\'
#  define OP_NULL_DEVICE    "NUL"
/* Without a console the C runtime parks a negative sentinel, not a
 * handle, in the standard slots. */
#  define op_fd_is_open(fd) (_get_osfhandle(fd) > 0)
#else
#  include <pthread.h>
#  include <signal.h>
#  include <sys/file.h>
#  include <unistd.h>
#  define op_pipe(fds)      pipe(fds)
#  define op_dup(fd)        dup(fd)
#  define op_dup2(a, b)     dup2((a), (b))
#  define op_read(f, b, n)  read((f), (b), (n))
#  define op_write(f, b, n) write((f), (b), (n))
#  define op_close(fd)      close(fd)
#  define op_sleep_ms(ms)   usleep((ms) * 1000u)
#  define OP_OPEN_MODE      0644
#  define OP_PATH_SEP       '/'
#  define OP_NULL_DEVICE    "/dev/null"
#  define op_fd_is_open(fd) (fcntl((fd), F_GETFD) >= 0)
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
static int         s_pump_live   = 0;
static char        s_path[LOG_PATH_MAX];
static size_t      s_written     = 0;
static int         s_truncated   = 0;
/* Descriptor behind the stderr FILE object. 2 whenever the process had a
 * stderr at startup. A console-less Windows process leaves the object
 * detached (_fileno() < 0, every fprintf fails with EBADF); it is rebound
 * to a fresh descriptor, which then mirrors fd 2. */
static int         s_stderr_fd   = 2;
#if defined(_WIN32)
/* Whatever STD_ERROR_HANDLE named before the swap; NULL under a GUI-subsystem
 * launch with no console, restored verbatim on close. */
static HANDLE      s_win_orig_err = NULL;
#else
static int         s_lock_fd     = -1;
#endif

/** @brief Write all n bytes of buf to fd, retrying short writes and EINTR; other errors drop the rest silently. */
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

/** @brief Copy in[0..n) to out without ANSI CSI escape sequences and return the bytes written.
 *
 * The escape state persists in s_esc across calls, so a sequence split over
 * two reads is removed whole. out holds at least n bytes.
 * @param in Input bytes.
 * @param n Number of input bytes.
 * @param out Receives the filtered bytes. */
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

/** @brief Pump thread body: copy the stderr pipe to the real stderr and the log file until EOF.
 *
 * Every read goes to the real stderr unchanged. The copy written to the log
 * file has ANSI escapes stripped and is cut off once LOG_CAP_BYTES have been
 * written: a one-line truncation note is appended and the file receives
 * nothing more, while the real stderr keeps receiving everything. Returns
 * on EOF (every writer closed) or a read error other than EINTR. */
static void pump_loop(void) {
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
}

/* Thread shim: start the pump, and join it. */
#if defined(_WIN32)
static HANDLE s_pump;

/** @brief Body of the log pump thread (Windows): copy the stderr pipe to the real stderr and the log file until EOF. */
static unsigned __stdcall pump_entry(void* unused) {
    (void)unused;
    openpete_crash_thread_attach();
    pump_loop();
    return 0;
}

/** @brief Start the pump thread on _beginthreadex (Windows).
 *
 * @return 0 on success, -1 when the thread cannot be created. */
static int pump_start(void) {
    uintptr_t h = _beginthreadex(NULL, 0, pump_entry, NULL, 0, NULL);
    if (h == 0) return -1;
    s_pump = (HANDLE)h;
    return 0;
}

/** @brief Wait for the pump thread to exit and release its handle (Windows). */
static void pump_join(void) {
    WaitForSingleObject(s_pump, INFINITE);
    CloseHandle(s_pump);
    s_pump = NULL;
}
#else
static pthread_t s_pump;

/** @brief Body of the log pump thread (POSIX): block asynchronous signals, then copy the stderr pipe to the real stderr and the log file until EOF. */
static void* pump_entry(void* unused) {
    (void)unused;
    openpete_crash_thread_attach();
    /* Asynchronous signals go to the game's threads, whose handlers expect
     * them there; the pump only copies bytes. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);  sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT); sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGALRM); sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    pump_loop();
    return NULL;
}

/** @brief Start the pump thread on pthread_create (POSIX).
 *
 * @return 0 on success, -1 when the thread cannot be created. */
static int pump_start(void) {
    return pthread_create(&s_pump, NULL, pump_entry, NULL) == 0 ? 0 : -1;
}

/** @brief Wait for the pump thread to exit (POSIX). */
static void pump_join(void) {
    pthread_join(s_pump, NULL);
}
#endif

/** @brief Choose the log file path: spec when given, else <state>/logs/openpete.log.
 *
 * The state directory is made when it does not exist.
 * @param spec Explicit path, or NULL.
 * @param out Receives the path.
 * @param cap Size of out.
 * @return 0 on success, -1 when the path does not fit in cap or the state
 *         directory cannot be resolved. */
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

/** @brief Take a non-blocking exclusive flock on "<path>.lock" so one process writes the log file.
 *
 * A second concurrent instance (parallel headless gate runs, a second
 * launch) would interleave into the same file and race the rotation; it
 * fails here and runs without a file sink. The lock descriptor is kept in
 * s_lock_fd until drop_lock. On Windows no lock is taken and the call
 * always succeeds.
 * @return 0 when the lock is held (always on Windows), -1 when the lock
 *         path is too long, cannot be opened, or another process holds it. */
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

/** @brief Close the log lock descriptor, which releases the flock; nothing on Windows. */
static void drop_lock(void) {
#if !defined(_WIN32)
    if (s_lock_fd >= 0) { close(s_lock_fd); s_lock_fd = -1; }
#endif
}

/** @brief Open the null device on each of fds 0 to 2 that is not open.
 *
 * A GUI-subsystem Windows exe launched by a double click has no standard
 * handles, and a process whose parent closed fd 2 has none either. Every
 * later open() in openpete_log_file_open would then land on the lowest free
 * descriptor: the log file or the lock becomes fd 2 and the pipe swap
 * closes it. With the null device parked there, fd 2 is a writable
 * non-terminal stderr, the tee works unchanged, and the terminal leg
 * discards into the null device. On Windows a stderr FILE left detached
 * (_fileno() < 0) is also reopened on the null device and its descriptor
 * stored in s_stderr_fd. Stops at the first failed open. */
static void reserve_std_fds(void) {
    for (int fd = 0; fd <= 2; fd++) {
        if (op_fd_is_open(fd)) continue;
        int n = open(OP_NULL_DEVICE, O_RDWR | O_BINARY);
        if (n < 0) return;
        if (n != fd) { op_dup2(n, fd); op_close(n); }
    }
#if defined(_WIN32)
    if (_fileno(stderr) < 0 && freopen(OP_NULL_DEVICE, "w", stderr))
        s_stderr_fd = _fileno(stderr);
#endif
}

/** @brief Point fd 2, and the rebound stderr descriptor when distinct, back at the original stderr.
 *
 * Closing the last pipe write end this way gives the pump its EOF. */
static void restore_fd2(void) {
    op_dup2(s_real_stderr, 2);
    if (s_stderr_fd != 2) op_dup2(s_real_stderr, s_stderr_fd);
}

int openpete_log_file_open(const char* spec) {
    if (s_pump_live) return 0;  /* already open */

    if (!spec) spec = getenv("OPENPETE_LOG_FILE");
    if (spec && (!*spec || strcmp(spec, "off") == 0 || strcmp(spec, "0") == 0))
        return -1;

    if (resolve_path(spec, s_path, sizeof s_path) != 0) return -1;
    reserve_std_fds();
    if (take_lock(s_path) != 0) return -1;

    int fds[2];
    if (op_pipe(fds) != 0) { drop_lock(); return -1; }
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

    /* Rotate and open only once the tee is certain to go up: a failure
     * above must leave the previous run's log intact, not replaced by an
     * empty file. One generation is kept because a crash that a relaunch
     * papers over is the common report ("it worked the second time"), and
     * the run that failed is the one worth reading. */
    char prev[LOG_PATH_MAX + 4];
    if (snprintf(prev, sizeof prev, "%s.1", s_path) < (int)sizeof prev) {
        remove(prev);
        rename(s_path, prev);
    }
    s_log_fd = open(s_path, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY,
                    OP_OPEN_MODE);
    if (s_log_fd < 0) { op_close(fds[1]); goto fail_dup; }

    setvbuf(stderr, NULL, _IONBF, 0);
    if (op_dup2(fds[1], 2) < 0) { op_close(fds[1]); goto fail_file; }
    /* fd 2 (and the rebound stderr descriptor, when distinct) are now the
     * only writers, so restoring them in close() is what gives the pump
     * its EOF. */
    op_close(fds[1]);
    if (s_stderr_fd != 2) op_dup2(2, s_stderr_fd);

#if defined(_WIN32)
    /* _dup2 rewires the C runtime only. Code that writes through the
     * Win32 standard handle (the Vulkan loader, SDL, a child compiler
     * inheriting its parent's handles) needs STD_ERROR_HANDLE pointed at
     * the pipe as well. */
    s_win_orig_err = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
#endif

    if (pump_start() != 0) {
#if defined(_WIN32)
        SetStdHandle(STD_ERROR_HANDLE, s_win_orig_err);
#endif
        restore_fd2();
        goto fail_file;
    }
    s_pump_live = 1;

    atexit(openpete_log_file_close);
    return 0;

fail_file:
    op_close(s_log_fd);
    s_log_fd = -1;
fail_dup:
    op_close(s_real_stderr);
    s_real_stderr = -1;
fail_pipe:
    op_close(s_pipe_r);
    s_pipe_r = -1;
    drop_lock();
    return -1;
}

void openpete_log_file_close(void) {
    if (!s_pump_live) return;

    fflush(stderr);
#if defined(_WIN32)
    SetStdHandle(STD_ERROR_HANDLE, s_win_orig_err);
#endif
    restore_fd2();  /* last pipe writer goes away -> pump EOF */
    /* The EOF depends on no live child still holding an inherited fd 2.
     * Every process this runtime spawns (the first-boot compiler and
     * linker, the mod compiler) is waited on before shutdown, and their
     * output landing in the log is exactly why fd 2 is inherited. A
     * future daemonized child would have to close it. */
    pump_join();
    s_pump_live = 0;

    op_close(s_pipe_r);      s_pipe_r      = -1;
    op_close(s_real_stderr); s_real_stderr = -1;
    op_close(s_log_fd);      s_log_fd      = -1;
    drop_lock();
}

const char* openpete_log_file_path(void) {
    return s_pump_live ? s_path : "";
}

void openpete_log_file_sync(void) {
    if (!s_pump_live) return;
    fflush(stderr);
    /* Give the pump a moment to drain what the caller just wrote. Called
     * from the exits that skip the normal shutdown (halt-at-PC, the hang
     * watchdog, the forced exit after a quit timeout); without this the
     * text still in the pipe dies with the process. */
    op_sleep_ms(50);
}

int openpete_log_file_crash_fds(int fds[2]) {
    if (!s_pump_live || s_real_stderr < 0 || s_log_fd < 0) {
        fds[0] = 2;
        return 1;
    }
    fds[0] = s_real_stderr;
    fds[1] = s_log_fd;
    return 2;
}
