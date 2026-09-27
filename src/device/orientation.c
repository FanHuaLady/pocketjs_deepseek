#include "device/orientation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_rotation(const char *value, enum display_rotation *rotation)
{
    char *end;
    long degrees;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }
    if (strcmp(value, "portrait") == 0) {
        *rotation = DISPLAY_ROTATION_0;
        return 0;
    }
    if (strcmp(value, "landscape") == 0) {
        *rotation = DISPLAY_ROTATION_90;
        return 0;
    }
    degrees = strtol(value, &end, 10);
    if (*end != '\0') {
        return -1;
    }
    switch (degrees) {
    case 0: *rotation = DISPLAY_ROTATION_0; return 0;
    case 90: *rotation = DISPLAY_ROTATION_90; return 0;
    case 180: *rotation = DISPLAY_ROTATION_180; return 0;
    case 270: *rotation = DISPLAY_ROTATION_270; return 0;
    default: return -1;
    }
}

int display_rotation_from_env(
    uint32_t physical_width,
    uint32_t physical_height,
    enum display_rotation *rotation
)
{
    const char *value = getenv("RV1106_DISPLAY_ROTATION");
    const char *orientation = getenv("RV1106_DISPLAY_ORIENTATION");
    enum display_rotation parsed;

    (void)physical_width;
    (void)physical_height;
    *rotation = DISPLAY_ROTATION_0;
    if (value != NULL && value[0] != '\0') {
        if (parse_rotation(value, &parsed) != 0) {
            fprintf(stderr,
                    "invalid RV1106_DISPLAY_ROTATION=%s, expected 0/90/180/270\n",
                    value);
            return -1;
        }
        *rotation = parsed;
    } else if (orientation != NULL && orientation[0] != '\0') {
        if (strcmp(orientation, "portrait") == 0) {
            *rotation = physical_width < physical_height
                ? DISPLAY_ROTATION_0
                : DISPLAY_ROTATION_90;
        } else if (strcmp(orientation, "landscape") == 0) {
            *rotation = physical_width >= physical_height
                ? DISPLAY_ROTATION_0
                : DISPLAY_ROTATION_90;
        } else {
            fprintf(stderr,
                    "invalid RV1106_DISPLAY_ORIENTATION=%s, expected portrait/landscape\n",
                    orientation);
            return -1;
        }
    }
    return 0;
}

const char *display_rotation_name(enum display_rotation rotation)
{
    switch (rotation) {
    case DISPLAY_ROTATION_0: return "0";
    case DISPLAY_ROTATION_90: return "90";
    case DISPLAY_ROTATION_180: return "180";
    case DISPLAY_ROTATION_270: return "270";
    default: return "invalid";
    }
}

void display_logical_dimensions(
    uint32_t physical_width,
    uint32_t physical_height,
    enum display_rotation rotation,
    uint32_t *logical_width,
    uint32_t *logical_height
)
{
    if (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270) {
        *logical_width = physical_height;
        *logical_height = physical_width;
    } else {
        *logical_width = physical_width;
        *logical_height = physical_height;
    }
}

void display_map_physical_to_logical(
    int physical_x,
    int physical_y,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    int *logical_x,
    int *logical_y
)
{
    switch (rotation) {
    case DISPLAY_ROTATION_90:
        *logical_x = physical_y;
        *logical_y = physical_width - 1 - physical_x;
        break;
    case DISPLAY_ROTATION_180:
        *logical_x = physical_width - 1 - physical_x;
        *logical_y = physical_height - 1 - physical_y;
        break;
    case DISPLAY_ROTATION_270:
        *logical_x = physical_height - 1 - physical_y;
        *logical_y = physical_x;
        break;
    case DISPLAY_ROTATION_0:
    default:
        *logical_x = physical_x;
        *logical_y = physical_y;
        break;
    }
}

void display_map_logical_to_physical(
    int logical_x,
    int logical_y,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    int *physical_x,
    int *physical_y
)
{
    switch (rotation) {
    case DISPLAY_ROTATION_90:
        *physical_x = physical_width - 1 - logical_y;
        *physical_y = logical_x;
        break;
    case DISPLAY_ROTATION_180:
        *physical_x = physical_width - 1 - logical_x;
        *physical_y = physical_height - 1 - logical_y;
        break;
    case DISPLAY_ROTATION_270:
        *physical_x = logical_y;
        *physical_y = physical_height - 1 - logical_x;
        break;
    case DISPLAY_ROTATION_0:
    default:
        *physical_x = logical_x;
        *physical_y = logical_y;
        break;
    }
}
