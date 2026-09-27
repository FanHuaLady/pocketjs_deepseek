#include "device/framebuffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int framebuffer_remap(struct framebuffer *framebuffer)
{
    if (framebuffer->memory != NULL &&
        framebuffer->memory != MAP_FAILED) {
        munmap(framebuffer->memory, framebuffer->map_length);
    }
    framebuffer->map_length = framebuffer->fixed.smem_len;
    framebuffer->memory = mmap(
        NULL,
        framebuffer->map_length,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        framebuffer->fd,
        0
    );
    if (framebuffer->memory == MAP_FAILED) {
        fprintf(stderr, "mmap framebuffer failed: %s\n", strerror(errno));
        framebuffer->memory = NULL;
        framebuffer->map_length = 0;
        return -1;
    }
    return 0;
}

static int framebuffer_sync_enabled_from_env(void)
{
    const char *value = getenv("RV1106_FB_MSYNC");

    if (value == NULL || value[0] == '\0' ||
        strcmp(value, "1") == 0 ||
        strcmp(value, "true") == 0 ||
        strcmp(value, "TRUE") == 0 ||
        strcmp(value, "on") == 0 ||
        strcmp(value, "ON") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "FALSE") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0) {
        return 0;
    }
    fprintf(
        stderr,
        "invalid RV1106_FB_MSYNC=%s, using msync=on\n",
        value
    );
    return 1;
}

static uint32_t scale_channel(
    uint8_t value,
    const struct fb_bitfield *field
)
{
    uint32_t maximum;

    if (field->length == 0) {
        return 0;
    }
    maximum = (1U << field->length) - 1U;
    return ((uint32_t)value * maximum / 255U) << field->offset;
}

static uint16_t pack_rgb(
    const struct fb_var_screeninfo *variable,
    uint8_t red,
    uint8_t green,
    uint8_t blue
)
{
    uint32_t pixel = scale_channel(red, &variable->red);
    pixel |= scale_channel(green, &variable->green);
    pixel |= scale_channel(blue, &variable->blue);
    return (uint16_t)pixel;
}

static int framebuffer_validate_layout(
    const struct fb_fix_screeninfo *fixed,
    const struct fb_var_screeninfo *variable
)
{
    size_t visible_bytes;

    if (variable->xres == 0 ||
        variable->yres == 0 ||
        variable->xres_virtual < variable->xres ||
        variable->yres_virtual < variable->yres ||
        fixed->line_length < variable->xres * 2U) {
        return -1;
    }
    if (variable->xoffset > variable->xres_virtual - variable->xres ||
        variable->yoffset > variable->yres_virtual - variable->yres) {
        return -1;
    }
    if (variable->red.offset + variable->red.length > variable->bits_per_pixel ||
        variable->green.offset + variable->green.length > variable->bits_per_pixel ||
        variable->blue.offset + variable->blue.length > variable->bits_per_pixel) {
        return -1;
    }
    visible_bytes = (size_t)fixed->line_length * variable->yres;
    if (fixed->smem_len < visible_bytes) {
        return -1;
    }
    return 0;
}

int framebuffer_open(const char *path, struct framebuffer *framebuffer)
{
    enum display_rotation rotation;

    framebuffer->sync_enabled = framebuffer_sync_enabled_from_env();
    framebuffer->rotation = DISPLAY_ROTATION_0;
    framebuffer->logical_width = 0;
    framebuffer->logical_height = 0;
    framebuffer->fd = open(path, O_RDWR);
    if (framebuffer->fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(
        framebuffer->fd,
        FBIOGET_FSCREENINFO,
        &framebuffer->fixed
    ) < 0 ||
        ioctl(
            framebuffer->fd,
            FBIOGET_VSCREENINFO,
            &framebuffer->variable
        ) < 0) {
        fprintf(stderr, "cannot query %s: %s\n", path, strerror(errno));
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }
    if (framebuffer->variable.bits_per_pixel != 16 ||
        framebuffer_validate_layout(
            &framebuffer->fixed,
            &framebuffer->variable
        ) != 0) {
        fprintf(stderr,
                "unsupported or invalid framebuffer layout: "
                "%ux%u virtual=%ux%u bpp=%u stride=%u smem_len=%u\n",
                framebuffer->variable.xres,
                framebuffer->variable.yres,
                framebuffer->variable.xres_virtual,
                framebuffer->variable.yres_virtual,
                framebuffer->variable.bits_per_pixel,
                framebuffer->fixed.line_length,
                framebuffer->fixed.smem_len);
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }
    if (display_rotation_from_env(
            framebuffer->variable.xres,
            framebuffer->variable.yres,
            &rotation
        ) != 0) {
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }
    framebuffer->rotation = rotation;
    display_logical_dimensions(
        framebuffer->variable.xres,
        framebuffer->variable.yres,
        rotation,
        &framebuffer->logical_width,
        &framebuffer->logical_height
    );
    framebuffer->original_variable = framebuffer->variable;
    framebuffer->has_original_variable = 1;
    if (framebuffer_remap(framebuffer) != 0) {
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return -1;
    }
    printf(
        "framebuffer opened: %s %ux%u %ubpp stride=%u\n",
        path,
        framebuffer->variable.xres,
        framebuffer->variable.yres,
        framebuffer->variable.bits_per_pixel,
        framebuffer->fixed.line_length
    );
    printf(
        "display transform: rotation=%s logical=%ux%u\n",
        display_rotation_name(framebuffer->rotation),
        framebuffer->logical_width,
        framebuffer->logical_height
    );
    printf(
        "framebuffer sync: msync=%s\n",
        framebuffer->sync_enabled ? "on" : "off"
    );
    return 0;
}

void framebuffer_close(struct framebuffer *framebuffer)
{
    if (framebuffer->fd >= 0 && framebuffer->has_original_variable) {
        struct fb_var_screeninfo restore = framebuffer->original_variable;

        restore.activate = FB_ACTIVATE_NOW;
        ioctl(framebuffer->fd, FBIOPUT_VSCREENINFO, &restore);
    }
    if (framebuffer->memory != NULL &&
        framebuffer->memory != MAP_FAILED) {
        munmap(framebuffer->memory, framebuffer->map_length);
    }
    if (framebuffer->fd >= 0) {
        close(framebuffer->fd);
    }
    framebuffer->memory = NULL;
    framebuffer->fd = -1;
    framebuffer->map_length = 0;
    framebuffer->has_original_variable = 0;
}

uint32_t framebuffer_buffer_count(const struct framebuffer *framebuffer)
{
    size_t visible_bytes;
    size_t memory_buffers;
    uint32_t virtual_buffers;
    uint32_t buffers;

    if (framebuffer->variable.yres == 0 ||
        framebuffer->fixed.line_length == 0) {
        return 0;
    }
    visible_bytes = (size_t)framebuffer->fixed.line_length *
                    framebuffer->variable.yres;
    if (visible_bytes == 0) {
        return 0;
    }
    memory_buffers = framebuffer->map_length / visible_bytes;
    virtual_buffers = framebuffer->variable.yres_virtual /
                      framebuffer->variable.yres;
    buffers = memory_buffers < virtual_buffers
        ? (uint32_t)memory_buffers
        : virtual_buffers;
    return buffers == 0 ? 1 : buffers;
}

int framebuffer_try_enable_double_buffer(struct framebuffer *framebuffer)
{
    struct fb_var_screeninfo requested;
    size_t required_bytes;

    if (framebuffer->fd < 0) {
        return -1;
    }
    if (framebuffer_buffer_count(framebuffer) >= 2) {
        return 0;
    }

    requested = framebuffer->variable;
    requested.xoffset = 0;
    requested.yoffset = 0;
    requested.yres_virtual = framebuffer->variable.yres * 2U;
    requested.activate = FB_ACTIVATE_NOW;
    if (ioctl(framebuffer->fd, FBIOPUT_VSCREENINFO, &requested) < 0) {
        fprintf(
            stderr,
            "FBIOPUT_VSCREENINFO double buffer failed: %s\n",
            strerror(errno)
        );
        return -1;
    }
    if (ioctl(framebuffer->fd, FBIOGET_FSCREENINFO, &framebuffer->fixed) < 0 ||
        ioctl(framebuffer->fd, FBIOGET_VSCREENINFO, &framebuffer->variable) < 0) {
        fprintf(stderr, "cannot re-query framebuffer: %s\n", strerror(errno));
        return -1;
    }

    required_bytes = (size_t)framebuffer->fixed.line_length *
                     framebuffer->variable.yres * 2U;
    if (framebuffer->variable.yres_virtual < framebuffer->variable.yres * 2U ||
        framebuffer->fixed.smem_len < required_bytes) {
        fprintf(
            stderr,
            "framebuffer double buffer unavailable: yres_virtual=%u smem_len=%u required=%zu\n",
            framebuffer->variable.yres_virtual,
            framebuffer->fixed.smem_len,
            required_bytes
        );
        return -1;
    }

    if (framebuffer_remap(framebuffer) != 0) {
        return -1;
    }
    printf(
        "framebuffer double buffer enabled: yres_virtual=%u buffers=%u\n",
        framebuffer->variable.yres_virtual,
        framebuffer_buffer_count(framebuffer)
    );
    return 0;
}

int framebuffer_fill_rgb(
    struct framebuffer *framebuffer,
    uint32_t buffer_index,
    uint8_t red,
    uint8_t green,
    uint8_t blue
)
{
    uint16_t output = pack_rgb(&framebuffer->variable, red, green, blue);
    uint32_t buffer_count = framebuffer_buffer_count(framebuffer);
    uint32_t y;

    if (buffer_count == 0 || buffer_index >= buffer_count) {
        return -1;
    }
    for (y = 0; y < framebuffer->variable.yres; y++) {
        uint32_t x;
        size_t row_offset = (size_t)(buffer_index * framebuffer->variable.yres + y) *
                            framebuffer->fixed.line_length;
        uint8_t *row = framebuffer->memory + row_offset;

        for (x = 0; x < framebuffer->variable.xres; x++) {
            size_t offset = (size_t)(x + framebuffer->variable.xoffset) * 2U;
            if (row_offset + offset + sizeof(output) > framebuffer->map_length) {
                return -1;
            }
            memcpy(row + offset, &output, sizeof(output));
        }
    }
    return 0;
}

int framebuffer_pan(struct framebuffer *framebuffer, uint32_t buffer_index)
{
    struct fb_var_screeninfo requested = framebuffer->variable;
    uint32_t buffer_count = framebuffer_buffer_count(framebuffer);

    if (buffer_count == 0 || buffer_index >= buffer_count) {
        return -1;
    }
    requested.xoffset = 0;
    requested.yoffset = buffer_index * framebuffer->variable.yres;
    requested.activate = FB_ACTIVATE_VBL;
    if (ioctl(framebuffer->fd, FBIOPAN_DISPLAY, &requested) < 0) {
        requested.activate = FB_ACTIVATE_NOW;
        if (ioctl(framebuffer->fd, FBIOPAN_DISPLAY, &requested) < 0) {
            fprintf(stderr, "FBIOPAN_DISPLAY failed: %s\n", strerror(errno));
            return -1;
        }
    }
    framebuffer->variable.yoffset = requested.yoffset;
    return 0;
}

int framebuffer_sync(struct framebuffer *framebuffer)
{
    if (!framebuffer->sync_enabled) {
        return 0;
    }
    if (msync(framebuffer->memory, framebuffer->map_length, MS_SYNC) < 0) {
        fprintf(stderr, "msync framebuffer failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static long long framebuffer_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int framebuffer_present_bgra_rect(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const int *bounds,
    struct framebuffer_present_stats *stats
)
{
    uint32_t copy_width = width < framebuffer->logical_width
        ? width
        : framebuffer->logical_width;
    uint32_t copy_height = height < framebuffer->logical_height
        ? height
        : framebuffer->logical_height;
    int x0 = 0;
    int y0 = 0;
    int x1 = (int)copy_width;
    int y1 = (int)copy_height;
    long long conversion_start_ns = -1;
    long long conversion_end_ns = -1;
    long long sync_start_ns = -1;
    long long sync_end_ns = -1;
    int y;

    if (stats != NULL) {
        stats->conversion_ns = 0;
        stats->sync_ns = 0;
        stats->pixels = 0;
        conversion_start_ns = framebuffer_monotonic_ns();
    }
    if (framebuffer->memory == NULL ||
        bgra == NULL ||
        width == 0 ||
        height == 0 ||
        stride < width * 4U) {
        return -1;
    }
    if (bounds != NULL) {
        x0 = bounds[0];
        y0 = bounds[1];
        x1 = bounds[2];
        y1 = bounds[3];
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)copy_width) x1 = (int)copy_width;
    if (y1 > (int)copy_height) y1 = (int)copy_height;
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }

    for (y = y0; y < y1; y++) {
        int x;
        const uint8_t *source = bgra + (size_t)y * stride;

        for (x = x0; x < x1; x++) {
            const uint8_t *pixel = source + (size_t)x * 4U;
            int physical_x;
            int physical_y;
            size_t row_offset;
            size_t offset;
            uint8_t *row;
            uint16_t output = pack_rgb(
                &framebuffer->variable,
                pixel[2],
                pixel[1],
                pixel[0]
            );

            display_map_logical_to_physical(
                x,
                y,
                (int)framebuffer->variable.xres,
                (int)framebuffer->variable.yres,
                framebuffer->rotation,
                &physical_x,
                &physical_y
            );
            row_offset = (size_t)(physical_y + framebuffer->variable.yoffset) *
                         framebuffer->fixed.line_length;
            offset = (size_t)(physical_x + framebuffer->variable.xoffset) * 2U;
            if (row_offset + offset + sizeof(output) > framebuffer->map_length) {
                return -1;
            }
            row = framebuffer->memory + row_offset;
            memcpy(row + offset, &output, sizeof(output));
            if (stats != NULL) {
                stats->pixels++;
            }
        }
    }

    if (stats != NULL) {
        conversion_end_ns = framebuffer_monotonic_ns();
        if (conversion_start_ns >= 0 && conversion_end_ns >= conversion_start_ns) {
            stats->conversion_ns = conversion_end_ns - conversion_start_ns;
        }
        sync_start_ns = framebuffer_monotonic_ns();
    }
    {
        int result = framebuffer_sync(framebuffer);

        if (stats != NULL) {
            sync_end_ns = framebuffer_monotonic_ns();
            if (sync_start_ns >= 0 && sync_end_ns >= sync_start_ns) {
                stats->sync_ns = sync_end_ns - sync_start_ns;
            }
        }
        return result;
    }
}

int framebuffer_present_bgra(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride
)
{
    return framebuffer_present_bgra_rect(
        framebuffer,
        bgra,
        width,
        height,
        stride,
        NULL,
        NULL
    );
}

int framebuffer_present_bgra_damage(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const int *bounds
)
{
    return framebuffer_present_bgra_damage_timed(
        framebuffer,
        bgra,
        width,
        height,
        stride,
        bounds,
        NULL
    );
}

int framebuffer_present_bgra_damage_timed(
    struct framebuffer *framebuffer,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const int *bounds,
    struct framebuffer_present_stats *stats
)
{
    return framebuffer_present_bgra_rect(
        framebuffer,
        bgra,
        width,
        height,
        stride,
        bounds,
        stats
    );
}
