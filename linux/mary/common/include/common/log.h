/* One log call for every daemon. Under systemd (JOURNAL_STREAM is set) each line
 * carries an sd-daemon(3) "<N>" priority prefix, so the journal files it at the
 * right level; otherwise lines read "ident: level: message". MARY_LOG_DEBUG=1
 * turns debug lines on. Never pass a secret. */
#ifndef MARY_COMMON_LOG_H
#define MARY_COMMON_LOG_H

typedef enum mc_log_level {
    MC_LOG_ERROR = 3,
    MC_LOG_WARNING = 4,
    MC_LOG_NOTICE = 5,
    MC_LOG_INFO = 6,
    MC_LOG_DEBUG = 7,
} mc_log_level;

void mc_log_init(const char *ident);
void mc_log_set_threshold(mc_log_level level);
void mc_log(mc_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
