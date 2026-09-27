#ifndef POCKETJS_RV1106_HOST_LOG_H
#define POCKETJS_RV1106_HOST_LOG_H

enum host_log_level {
    HOST_LOG_ERROR = 0,
    HOST_LOG_WARN = 1,
    HOST_LOG_INFO = 2,
    HOST_LOG_DEBUG = 3,
};

void host_log_init(void);
int host_log_enabled(enum host_log_level level);
void host_log(enum host_log_level level, const char *component, const char *format, ...);

#define HOST_LOG_ERRORF(component, ...) \
    host_log(HOST_LOG_ERROR, component, __VA_ARGS__)
#define HOST_LOG_WARNF(component, ...) \
    host_log(HOST_LOG_WARN, component, __VA_ARGS__)
#define HOST_LOG_INFOF(component, ...) \
    host_log(HOST_LOG_INFO, component, __VA_ARGS__)
#define HOST_LOG_DEBUGF(component, ...) \
    host_log(HOST_LOG_DEBUG, component, __VA_ARGS__)

#endif
