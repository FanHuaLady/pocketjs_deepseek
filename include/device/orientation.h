#ifndef POCKETJS_RV1106_ORIENTATION_H
#define POCKETJS_RV1106_ORIENTATION_H

#include <stdint.h>

enum display_rotation {
    DISPLAY_ROTATION_0 = 0,
    DISPLAY_ROTATION_90 = 90,
    DISPLAY_ROTATION_180 = 180,
    DISPLAY_ROTATION_270 = 270,
};

int display_rotation_from_env(
    uint32_t physical_width,
    uint32_t physical_height,
    enum display_rotation *rotation
);
const char *display_rotation_name(enum display_rotation rotation);
void display_logical_dimensions(
    uint32_t physical_width,
    uint32_t physical_height,
    enum display_rotation rotation,
    uint32_t *logical_width,
    uint32_t *logical_height
);
void display_map_physical_to_logical(
    int physical_x,
    int physical_y,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    int *logical_x,
    int *logical_y
);
void display_map_logical_to_physical(
    int logical_x,
    int logical_y,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    int *physical_x,
    int *physical_y
);

#endif
