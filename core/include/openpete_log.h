// core/include/openpete_log.h — structured logger.
//
// All runtime logs go through this. Format is selectable at boot:
//   plain → "1.234 [info ] subsys: message ..."
//   json  → {"t":1.234,"level":"info","subsystem":"subsys","msg":"...","key":"value",...}
//
// Per-subsystem level filters via OPENPETE_LOG env var or --log-level.
// Spec: "subsystem:level,subsystem:level,...,*:default-level".
// Levels: trace < debug < info < warn < error.

#ifndef OPENPETE_LOG_H
#define OPENPETE_LOG_H

#include "openpete.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE: dispatch_chain.c (recomp-module source kit) hardcodes OPL_INFO's
 * value (2) via a local prototype to stay header-self-contained — keep
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

/* Live re-configuration. set_spec replaces ALL level rules with the given
 * spec (NULL/empty = plain *:info); get_spec returns the last applied spec
 * ("" when none). Format switches take effect on the next line. */
void openpete_log_set_format(openpete_log_format_t fmt);
openpete_log_format_t openpete_log_get_format(void);
void openpete_log_set_spec(const char* spec);
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
void openpete_log_file_close(void);
const char* openpete_log_file_path(void);  /* "" when inactive */

/* Pin the answer to "is stderr a terminal?" before the mirror swaps fd 2
 * for a pipe. Colour selection would otherwise see the pipe. */
void openpete_log_pin_stderr_tty(int is_tty);

/** @brief Flush stderr and wait 50 ms for the pump thread to copy it into the log file.
 *
 * No-op when no file sink is active. Called immediately before the
 * process terminates without the normal shutdown (the crash handler, the
 * halt-at-PC and hang-watchdog exits, the forced exit after a quit grace
 * timeout), where text still in the stderr pipe would be lost with the
 * process. The wait is a fixed sleep, not a drain handshake. Not
 * async-signal-safe. */
void openpete_log_file_sync(void);

/* Log a message. `extra` is an optional NUL-terminated key=value sequence,
 * one pair per line, used for structured fields. May be NULL. */
void openpete_logf(openpete_log_level_t level, const char* subsystem,
                   const char* fmt, ...);

/* Convenience macros — subsystem becomes part of the log line. */
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
