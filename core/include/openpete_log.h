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
