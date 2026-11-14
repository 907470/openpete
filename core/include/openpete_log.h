// core/include/openpete_log.h - structured logger.
//
// All runtime logs go through this. Format is selectable at boot:
//   plain -> "1.234 [info ] subsys: message ..."
//   json  -> {"t":1.234,"level":"info","subsystem":"subsys","msg":"...","key":"value",...}
//
// Per-subsystem level filters via OPENPETE_LOG env var or --log-level.
// Spec: "subsystem:level,subsystem:level,...,*:default-level". The level
// follows the last colon; a rule for "mod" also covers every "mod:<id>"
// subsystem, and a rule naming one "mod:<id>" exactly wins over it.
// Levels: trace < debug < info < warn < error.

#ifndef OPENPETE_LOG_H
#define OPENPETE_LOG_H

#include "openpete.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE: dispatch_chain.c (recomp-module source kit) hardcodes OPL_INFO's
 * value (2) via a local prototype to stay header-self-contained - keep
 * these values stable or update it. */
typedef enum {
    OPL_TRACE = 0,
    OPL_DEBUG = 1,
    OPL_INFO  = 2,
    OPL_WARN  = 3,
    OPL_ERROR = 4,
} openpete_log_level_t;

/** @brief Set the log format and rebuild the per-subsystem level rules from OPENPETE_LOG and a spec string.
 *
 * Clears every rule and resets the default level to OPL_INFO, then parses
 * OPENPETE_LOG and after it level_spec, so a rule in level_spec overrides
 * the environment's. A spec is a comma-separated list of
 * `<subsystem>:<level>` pairs (trace, debug, info, warn, error); the
 * subsystem `*` sets the default, any other name matches a subsystem by
 * exact string. A pair with an unknown level is ignored. The timestamp
 * origin is taken on the first call only, so the second call from
 * openpete_run() after the one in main() keeps the log's timeline. The
 * spec string read back by openpete_log_get_spec is level_spec when
 * non-NULL, else the environment value.
 * @param fmt Output format for subsequent log lines.
 * @param level_spec Level rules from the command line or config, or NULL. */
void openpete_log_init(openpete_log_format_t fmt, const char* level_spec);

/** @brief Switch the log output format; the next line is written in it.
 *
 * The knob registry's live log-format row calls it.
 * @param fmt OPENPETE_LOG_PLAIN or OPENPETE_LOG_JSON. */
void openpete_log_set_format(openpete_log_format_t fmt);
/** @brief Return the current log output format.
 * @return The format last set by openpete_log_init or openpete_log_set_format. */
openpete_log_format_t openpete_log_get_format(void);
/** @brief Replace every level rule with the rules of spec.
 *
 * Unlike openpete_log_init it does not layer on OPENPETE_LOG: the rules are
 * cleared, the default level reset to OPL_INFO, and only spec parsed, so a
 * NULL or empty spec leaves plain `*:info`. The knob registry's live
 * log-level row calls it.
 * @param spec Level rules in the openpete_log_init syntax, or NULL. */
void openpete_log_set_spec(const char* spec);
/** @brief Return the last applied level spec.
 *
 * Safe on any thread while another reconfigures the logger. The string is
 * never modified or freed, so a held pointer keeps the spec it was read as.
 * @return The spec string, "" when none was applied; truncated to 127 bytes. */
const char* openpete_log_get_spec(void);

/** @brief Mirror everything written to stderr into a log file through a pipe and a pump thread.
 *
 * A launch with no terminal (a double-clicked AppImage, a .desktop entry)
 * then still leaves its output on disk. fd 2 is replaced by a pipe; the
 * pump thread copies each read to the original stderr and, with ANSI
 * escape sequences stripped, to the log file until LOG_CAP_BYTES. Missing
 * standard descriptors are first bound to the null device, so the log file
 * cannot land on fd 2. The previous run's file is renamed to ".1" only once
 * the pipe and the duplicated stderr are up, so an early failure leaves it
 * intact. On POSIX a non-blocking flock on "<path>.lock" keeps a second
 * concurrent instance off the file; that instance runs without a sink. On
 * Windows the Win32 standard error handle is pointed at the pipe as well.
 * openpete_log_file_close is registered with atexit.
 * @param spec The resolved --log-file value: NULL falls back to
 *             $OPENPETE_LOG_FILE and then to <state>/logs/openpete.log;
 *             "off", "0" or "" disable the sink.
 * @return 0 when a file sink is active (also when one already was), -1
 *         when the sink is disabled or setup failed. */
int openpete_log_file_open(const char* spec);
/** @brief Stop the file sink: restore fd 2, let the pump drain the pipe to EOF, join it and close the descriptors.
 *
 * No-op when no sink is active. Registered with atexit by
 * openpete_log_file_open; the handoff to a rebuilt process also calls it
 * directly. The wait for EOF assumes no child process still holds an
 * inherited fd 2. A later openpete_log_file_open may start a new sink. */
void openpete_log_file_close(void);
/** @brief Return the path of the active log file.
 *
 * @return The path, "" when no sink is active. The string is static and
 *         changes on the next open. */
const char* openpete_log_file_path(void);  /* "" when inactive */

/** @brief Pin whether stderr is a terminal before the log-file mirror swaps fd 2 for a pipe.
 *
 * The plain-text format colours each line by level only when stderr is a
 * terminal; without the pin the test after the swap sees the pipe and drops
 * colour. openpete_log_file_open calls it with isatty of the duplicated
 * original stderr.
 * @param is_tty Nonzero when the original stderr is a terminal. */
void openpete_log_pin_stderr_tty(int is_tty);

/** @brief Flush stderr and wait 50 ms for the pump thread to copy it into the log file.
 *
 * No-op when no file sink is active. Called immediately before the
 * process terminates without the normal shutdown (the halt-at-PC and
 * hang-watchdog exits, the forced exit after a quit grace timeout), where
 * text still in the stderr pipe would be lost with the process. The crash
 * handler does not call it; it waits on openpete_log_file_barrier and
 * writes its report through
 * openpete_log_file_crash_fds. The wait is a fixed sleep, not a drain
 * handshake. Not async-signal-safe. */
void openpete_log_file_sync(void);

/** @brief Wait until everything the calling thread wrote to stderr before the call is in the real stderr and the log file.
 *
 * Returns at once when no file sink is active. Takes a ticket and writes
 * one NUL byte into the stderr pipe; the pump drops NULs from both sinks
 * and counts them after writing each chunk, and the call returns once the
 * count reaches the ticket. Text the caller wrote first precedes that NUL
 * in the pipe, so it outlives a fault or a kill right after the call.
 * Waits by yielding the CPU, never by a timed sleep, and never queries the
 * pipe. Async-signal-safe: the crash handlers call it before their report.
 * @param timeout_ms Longest wait.
 * @return 1 when the text is in both sinks (or no sink is active), 0 on a
 *         timeout or a failed write. */
int openpete_log_file_barrier(unsigned timeout_ms);

/** @brief Name the descriptors a crash report is written to directly, bypassing the stderr pipe.
 *
 * With the file sink active these are the original stderr and the log
 * file; otherwise fd 2 alone. Reads two statics, so the crash handler may
 * call it from a signal.
 * @param fds Receives up to two descriptors.
 * @return The number written. */
int openpete_log_file_crash_fds(int fds[2]);

/** @brief Format and write one log line to stderr when level passes the subsystem's filter.
 *
 * The level is compared against the rule whose subsystem matches exactly,
 * else the default level. The message is formatted with vsnprintf into a
 * 1024-byte buffer (longer messages are truncated) and written as
 * `<seconds> [<level>] <subsystem>: <message>` in the plain format, coloured
 * by level when stderr is a terminal, or as one JSON object
 * `{"t","level","subsystem","msg"}` in the JSON format. The timestamp is
 * monotonic seconds since the first openpete_log_init. stderr is flushed
 * after every line.
 * @param level Severity of the message.
 * @param subsystem Subsystem name the filter and the line carry; NULL is
 *        filtered as the empty name (the default level unless a rule names
 *        the empty subsystem) and prints "-" (plain) or "" (JSON).
 * @param fmt printf-style format of the message. */
void openpete_logf(openpete_log_level_t level, const char* subsystem,
                   const char* fmt, ...);

/** @brief Start capturing the calling thread's log lines into a buffer instead of stderr.
 *
 * While the capture is on, every openpete_logf line of level info or above
 * (whatever the subsystem filter) and every openpete_log_rawf text from this
 * thread is appended to the buffer in the plain format and not written to
 * stderr or the log file. Other threads are unaffected. A capture already
 * on is discarded. */
void openpete_log_capture_begin(void);

/** @brief Stop the calling thread's capture and return its text.
 * @param len Receives the text's length; may be NULL.
 * @return The NUL-terminated text, owned by the caller (free()); NULL when
 *         allocation failed before the first line. */
char* openpete_log_capture_end(size_t* len);

/** @brief Write preformatted text to stderr, or to the calling thread's capture while one is on.
 *
 * The census lines harnesses grep on stderr in their own format use it, so
 * a capture also collects them.
 * @param fmt printf-style format; the text is truncated to 1023 bytes. */
void openpete_log_rawf(const char* fmt, ...);

/* Convenience macros - subsystem becomes part of the log line. */
#define OPL_LOG(level, sub, ...) openpete_logf((level), (sub), __VA_ARGS__)
#define OPL_TRACE_F(sub, ...) OPL_LOG(OPL_TRACE, sub, __VA_ARGS__)
#define OPL_DEBUG_F(sub, ...) OPL_LOG(OPL_DEBUG, sub, __VA_ARGS__)
#define OPL_INFO_F(sub, ...)  OPL_LOG(OPL_INFO,  sub, __VA_ARGS__)
#define OPL_WARN_F(sub, ...)  OPL_LOG(OPL_WARN,  sub, __VA_ARGS__)
#define OPL_ERROR_F(sub, ...) OPL_LOG(OPL_ERROR, sub, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_LOG_H */
