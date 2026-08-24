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

void openpete_log_init(openpete_log_format_t fmt, const char* level_spec);

/* Live re-configuration. set_spec replaces ALL level rules with the given
 * spec (NULL/empty = plain *:info); get_spec returns the last applied spec
 * ("" when none). Format switches take effect on the next line. */
void openpete_log_set_format(openpete_log_format_t fmt);
openpete_log_format_t openpete_log_get_format(void);
void openpete_log_set_spec(const char* spec);
const char* openpete_log_get_spec(void);

/* Mirror everything written to stderr into a log file, so a launch with
 * no terminal (a double-clicked AppImage, a .desktop entry) still leaves
 * something to attach to a bug report. `spec` is the resolved --log-file
 * value: NULL falls back to $OPENPETE_LOG_FILE and then to
 * <state>/logs/openpete.log; "off", "0" or "" disable the sink. Returns 0
 * when a file sink is active. One previous run is kept as ".1"; a second
 * concurrent instance runs without a sink rather than interleave. */
int openpete_log_file_open(const char* spec);
void openpete_log_file_close(void);
const char* openpete_log_file_path(void);  /* "" when inactive */

/* Pin the answer to "is stderr a terminal?" before the mirror swaps fd 2
 * for a pipe. Colour selection would otherwise see the pipe. */
void openpete_log_pin_stderr_tty(int is_tty);

/* Flush and let the pump thread drain. For the crash path, where the
 * process is about to die and the dump would otherwise be lost in the
 * pipe. Not async-signal-safe. */
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
