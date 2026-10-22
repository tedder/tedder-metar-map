/*
 * Syslog-style log lines (RFC 3164-style tag): local0.<severity>: message
 * Use for console and optional persistent log on FAT.
 */
#ifndef TMM_LOG_H
#define TMM_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

void tmm_log(const char *severity, FILE *err_file, bool persist, const char *fmt, ...);

/* Convenience — map common levels */
#define TMM_L_EMERG(ff, p, ...)   tmm_log("emerg", ff, p, __VA_ARGS__)
#define TMM_L_ALERT(ff, p, ...)   tmm_log("alert", ff, p, __VA_ARGS__)
#define TMM_L_CRIT(ff, p, ...)    tmm_log("crit", ff, p, __VA_ARGS__)
#define TMM_L_ERR(ff, p, ...)     tmm_log("err", ff, p, __VA_ARGS__)
#define TMM_L_WARNING(ff, p, ...) tmm_log("warning", ff, p, __VA_ARGS__)
#define TMM_L_NOTICE(ff, p, ...)  tmm_log("notice", ff, p, __VA_ARGS__)
#define TMM_L_INFO(ff, p, ...)    tmm_log("info", ff, p, __VA_ARGS__)
#define TMM_L_DEBUG(ff, p, ...)   tmm_log("debug", ff, p, __VA_ARGS__)

#endif
