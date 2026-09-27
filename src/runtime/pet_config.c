#include "runtime/pet_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PET_CONFIG_MAX_BYTES 8192

void pet_config_defaults(PetAnimationConfig *config)
{
    if (config == NULL) return;
    config->idle_seconds = 60;
    config->response_multiplier = 3.0;
}

static int read_config(const char *path, char *buffer, size_t capacity)
{
    FILE *file;
    size_t length;

    file = fopen(path, "rb");
    if (file == NULL) return -1;
    length = fread(buffer, 1, capacity - 1, file);
    if (ferror(file) != 0) {
        fclose(file);
        return -1;
    }
    buffer[length] = '\0';
    fclose(file);
    return 0;
}

static int find_number(
    const char *json,
    const char *key,
    double *value
)
{
    char pattern[64];
    const char *cursor;
    char *end;

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return -1;
    }
    cursor = strstr(json, pattern);
    if (cursor == NULL) return 1;
    cursor += strlen(pattern);
    cursor = strchr(cursor, ':');
    if (cursor == NULL) return -1;
    errno = 0;
    *value = strtod(cursor + 1, &end);
    if (end == cursor + 1 || errno == ERANGE) return -1;
    return 0;
}

int pet_config_load(const char *path, PetAnimationConfig *config)
{
    char json[PET_CONFIG_MAX_BYTES];
    double value;
    int result;

    if (config == NULL) return -1;
    pet_config_defaults(config);
    if (path == NULL || read_config(path, json, sizeof(json)) != 0) return -1;

    result = find_number(json, "idleSeconds", &value);
    if (result == 0 && value >= 1.0 && value <= 86400.0) {
        config->idle_seconds = (unsigned long)value;
    }
    result = find_number(json, "responseMultiplier", &value);
    if (result == 0 && value >= 0.1 && value <= 30.0) {
        config->response_multiplier = value;
    }
    return 0;
}

const char *pet_config_path(void)
{
    const char *path = getenv("RV1106_PET_CONFIG");
    return path != NULL && path[0] != '\0' ? path : "pet_config.json";
}
