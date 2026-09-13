#include "common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char ident[32] = "mary";
static int journal = -1;
static mc_log_level threshold = MC_LOG_INFO;

void mc_log_init(const char *name) {
    if (name) snprintf(ident, sizeof ident, "%s", name);
    journal = getenv("JOURNAL_STREAM") != NULL;
    const char *debug = getenv("MARY_LOG_DEBUG");
    if (debug && *debug && strcmp(debug, "0") != 0) threshold = MC_LOG_DEBUG;
}

void mc_log_set_threshold(mc_log_level level) { threshold = level; }

void mc_log(mc_log_level level, const char *fmt, ...) {
    if (level > threshold) return;
    if (journal < 0) journal = getenv("JOURNAL_STREAM") != NULL;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    static const char *const names[8] = { [3] = "error", [4] = "warning", [5] = "notice", [6] = "info", [7] = "debug" };
    const char *name = names[level & 7] ? names[level & 7] : "log";
    if (journal) fprintf(stderr, "<%d>%s\n", (int)level, line);
    else fprintf(stderr, "%s: %s: %s\n", ident, name, line);
}
