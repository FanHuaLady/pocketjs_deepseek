#include "runtime/host_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum host_log_level minimum_level = HOST_LOG_INFO;
static int initialized;

static const char *level_name(enum host_log_level level)
{
    switch (level) {
    case HOST_LOG_ERROR: return "ERROR";
    case HOST_LOG_WARN: return "WARN";
    case HOST_LOG_INFO: return "INFO";
    case HOST_LOG_DEBUG: return "DEBUG";
    default: return "UNKNOWN";
    }
}

void host_log_init(void)
{
    const char *value = getenv("RV1106_LOG_LEVEL");

    if (value != NULL) {
        if (strcmp(value, "error") == 0) {
            minimum_level = HOST_LOG_ERROR;
        } else if (strcmp(value, "warn") == 0) {
            minimum_level = HOST_LOG_WARN;
        } else if (strcmp(value, "info") == 0) {
            minimum_level = HOST_LOG_INFO;
        } else if (strcmp(value, "debug") == 0) {
            minimum_level = HOST_LOG_DEBUG;
        } else {
            fprintf(stderr,
                    "[WARN][host] invalid RV1106_LOG_LEVEL=%s, using info\n",
                    value);
        }
    }
    initialized = 1;
}

int host_log_enabled(enum host_log_level level)
{
    if (!initialized) {
        host_log_init();
    }
    return level <= minimum_level;
}

void host_log(enum host_log_level level, const char *component, const char *format, ...)
{
    va_list arguments;

    if (!host_log_enabled(level)) {
        return;
    }
    fprintf(stderr, "[%s][%s] ", level_name(level), component);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}
