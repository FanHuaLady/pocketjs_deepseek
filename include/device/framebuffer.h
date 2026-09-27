#ifndef POCKETJS_RV1106_FRAMEBUFFER_H
#define POCKETJS_RV1106_FRAMEBUFFER_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#include "device/orientation.h"

struct framebuffer {
    int fd;
    size_t map_length;
    uint8_t *memory;
    struct fb_fix_screeninfo fixed;
    struct fb_var_screeninfo variable;
    struct fb_var_screeninfo original_variable;
    int has_original_variable;
    int sync_enabled;
    enum display_rotation rotation;
    uint32_t logical_width;
    uint32_t logical_height;
};

struct framebuffer_present_stats {
    long long conversion_ns;
    long long sync_ns;
    unsigned long long pixels;
};

int framebuffer_open(const char *path, struct framebuffer *framebuffer);
void framebuffer_close(struct framebuffer *framebuffer);
uint32_t framebuffer_buffer_count(const struct framebuffer *framebuffer);
int framebuffer_try_enable_double_buffer(struct framebuffer *framebuffer);
int framebuffer_fill_rgb(
    struct framebuffer *framebuffer,
    uint32_t buffer_index,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);
int framebuffer_pan(struct framebuffer *framebuffer, uint32_t buffer_index);
int framebuffer_sync(struct framebuffer *framebuffer);
int framebuffer_present_bgra(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride
);
int framebuffer_present_bgra_damage(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const int *bounds
);
int framebuffer_present_bgra_damage_timed(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const int *bounds,
    struct framebuffer_present_stats *stats
);

#endif
