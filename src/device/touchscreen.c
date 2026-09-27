#include "device/touchscreen.h"
#include "runtime/host_log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TOUCHSCREEN_SCAN_LIMIT 32

static int read_abs_range(
    int fd,
    unsigned int code,
    int *minimum,
    int *maximum
)
{
    struct input_absinfo info;

    if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) {
        return -1;
    }
    *minimum = info.minimum;
    *maximum = info.maximum;
    return 0;
}

static int has_abs_code(int fd, unsigned int code)
{
    struct input_absinfo info;

    return ioctl(fd, EVIOCGABS(code), &info) == 0;
}

static int parse_bool_env(const char *name)
{
    const char *value = getenv(name);

    return value != NULL &&
           value[0] != '\0' &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0;
}

static void override_range_from_env(
    const char *minimum_name,
    const char *maximum_name,
    int *minimum,
    int *maximum
)
{
    const char *minimum_value = getenv(minimum_name);
    const char *maximum_value = getenv(maximum_name);
    char *end;
    long parsed_minimum;
    long parsed_maximum;

    if (minimum_value == NULL || maximum_value == NULL) {
        return;
    }
    parsed_minimum = strtol(minimum_value, &end, 10);
    if (minimum_value[0] == '\0' || *end != '\0') {
        return;
    }
    parsed_maximum = strtol(maximum_value, &end, 10);
    if (maximum_value[0] == '\0' || *end != '\0' ||
        parsed_maximum <= parsed_minimum) {
        return;
    }
    *minimum = (int)parsed_minimum;
    *maximum = (int)parsed_maximum;
}

static int clamp(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int map_coordinate(
    int value,
    int raw_minimum,
    int raw_maximum,
    int output_size
)
{
    long long numerator;
    int bounded;

    if (output_size <= 1 || raw_maximum <= raw_minimum) {
        return 0;
    }
    bounded = clamp(value, raw_minimum, raw_maximum);
    numerator = (long long)(bounded - raw_minimum) * (output_size - 1);
    return (int)(numerator / (raw_maximum - raw_minimum));
}

static void reset_slots(struct touchscreen *touchscreen)
{
    int slot;

    touchscreen->active_slot = 0;
    for (slot = 0; slot < TOUCHSCREEN_MAX_SLOTS; slot++) {
        touchscreen->slot_tracking_ids[slot] = -1;
        touchscreen->slot_raw_x[slot] = 0;
        touchscreen->slot_raw_y[slot] = 0;
        touchscreen->slot_has_x[slot] = 0;
        touchscreen->slot_has_y[slot] = 0;
    }
}

static void reset_contact_state(struct touchscreen *touchscreen)
{
    reset_slots(touchscreen);
    touchscreen->tracking_id = -1;
    touchscreen->button_down = 0;
    touchscreen->has_x = 0;
    touchscreen->has_y = 0;
    touchscreen->down = 0;
    touchscreen->resync_pending = 0;
}

static int configure_touchscreen_ranges(struct touchscreen *touchscreen)
{
    int mt_min_x;
    int mt_max_x;
    int mt_min_y;
    int mt_max_y;

    if (read_abs_range(
        touchscreen->fd,
        ABS_MT_POSITION_X,
        &mt_min_x,
        &mt_max_x
    ) == 0 &&
        read_abs_range(
            touchscreen->fd,
            ABS_MT_POSITION_Y,
            &mt_min_y,
            &mt_max_y
        ) == 0 &&
        has_abs_code(touchscreen->fd, ABS_MT_TRACKING_ID)) {
        touchscreen->raw_min_x = mt_min_x;
        touchscreen->raw_max_x = mt_max_x;
        touchscreen->raw_min_y = mt_min_y;
        touchscreen->raw_max_y = mt_max_y;
        touchscreen->uses_multitouch = 1;
        touchscreen->uses_slots = has_abs_code(touchscreen->fd, ABS_MT_SLOT);
    } else if (
        read_abs_range(
            touchscreen->fd,
            ABS_X,
            &touchscreen->raw_min_x,
            &touchscreen->raw_max_x
        ) != 0 ||
        read_abs_range(
            touchscreen->fd,
            ABS_Y,
            &touchscreen->raw_min_y,
            &touchscreen->raw_max_y
        ) != 0) {
        return -1;
    }

    override_range_from_env(
        "RV1106_TOUCH_MIN_X",
        "RV1106_TOUCH_MAX_X",
        &touchscreen->raw_min_x,
        &touchscreen->raw_max_x
    );
    override_range_from_env(
        "RV1106_TOUCH_MIN_Y",
        "RV1106_TOUCH_MAX_Y",
        &touchscreen->raw_min_y,
        &touchscreen->raw_max_y
    );
    return 0;
}

static int touchscreen_open_path(
    const char *path,
    int width,
    int height,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    struct touchscreen *touchscreen,
    int quiet
)
{
    memset(touchscreen, 0, sizeof(*touchscreen));
    reset_contact_state(touchscreen);
    touchscreen->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (touchscreen->fd < 0) {
        if (!quiet) {
            fprintf(stderr, "cannot open touchscreen %s: %s\n", path, strerror(errno));
        }
        return -1;
    }

    touchscreen->width = width;
    touchscreen->height = height;
    touchscreen->physical_width = physical_width;
    touchscreen->physical_height = physical_height;
    touchscreen->rotation = rotation;
    touchscreen->tracking_id = -1;
    touchscreen->flip_x = parse_bool_env("RV1106_TOUCH_FLIP_X");
    touchscreen->flip_y = parse_bool_env("RV1106_TOUCH_FLIP_Y");
    touchscreen->swap_xy = parse_bool_env("RV1106_TOUCH_SWAP_XY");

    if (configure_touchscreen_ranges(touchscreen) != 0) {
        if (!quiet) {
            fprintf(stderr, "cannot query touchscreen coordinate ranges\n");
        }
        close(touchscreen->fd);
        touchscreen->fd = -1;
        return -1;
    }

    printf(
        "touchscreen opened: %s protocol=%s raw_x=[%d,%d] raw_y=[%d,%d] logical=%dx%d\n",
        path,
        touchscreen->uses_multitouch
            ? (touchscreen->uses_slots ? "multitouch-slots" : "multitouch")
            : "single-touch",
        touchscreen->raw_min_x,
        touchscreen->raw_max_x,
        touchscreen->raw_min_y,
        touchscreen->raw_max_y,
        width,
        height
    );
    printf(
        "touchscreen transform: swap_xy=%d flip_x=%d flip_y=%d\n",
        touchscreen->swap_xy,
        touchscreen->flip_x,
        touchscreen->flip_y
    );
    printf(
        "touchscreen display rotation: %s\n",
        display_rotation_name(rotation)
    );
    return 0;
}

int touchscreen_open(
    const char *path,
    int width,
    int height,
    int physical_width,
    int physical_height,
    enum display_rotation rotation,
    struct touchscreen *touchscreen
)
{
    unsigned int index;
    char candidate[32];

    if (path != NULL && path[0] != '\0') {
        return touchscreen_open_path(
            path,
            width,
            height,
            physical_width,
            physical_height,
            rotation,
            touchscreen,
            0
        );
    }

    for (index = 0; index < TOUCHSCREEN_SCAN_LIMIT; index++) {
        snprintf(candidate, sizeof(candidate), "/dev/input/event%u", index);
        if (touchscreen_open_path(
                candidate,
                width,
                height,
                physical_width,
                physical_height,
                rotation,
                touchscreen,
                1
            ) == 0) {
            return 0;
        }
    }
    fprintf(stderr, "cannot find touchscreen under /dev/input/event*\n");
    return -1;
}

void touchscreen_close(struct touchscreen *touchscreen)
{
    if (touchscreen->fd >= 0) {
        close(touchscreen->fd);
    }
    touchscreen->fd = -1;
    touchscreen->down = 0;
}

static void update_logical_position(struct touchscreen *touchscreen)
{
    int x;
    int y;

    if (touchscreen->swap_xy) {
        x = map_coordinate(
            touchscreen->raw_y,
            touchscreen->raw_min_y,
            touchscreen->raw_max_y,
            touchscreen->physical_width
        );
        y = map_coordinate(
            touchscreen->raw_x,
            touchscreen->raw_min_x,
            touchscreen->raw_max_x,
            touchscreen->physical_height
        );
    } else {
        x = map_coordinate(
            touchscreen->raw_x,
            touchscreen->raw_min_x,
            touchscreen->raw_max_x,
            touchscreen->physical_width
        );
        y = map_coordinate(
            touchscreen->raw_y,
            touchscreen->raw_min_y,
            touchscreen->raw_max_y,
            touchscreen->physical_height
        );
    }
    if (touchscreen->flip_x) {
        x = touchscreen->physical_width - 1 - x;
    }
    if (touchscreen->flip_y) {
        y = touchscreen->physical_height - 1 - y;
    }
    touchscreen->x = clamp(x, 0, touchscreen->physical_width - 1);
    touchscreen->y = clamp(y, 0, touchscreen->physical_height - 1);

    display_map_physical_to_logical(
        touchscreen->x,
        touchscreen->y,
        touchscreen->physical_width,
        touchscreen->physical_height,
        touchscreen->rotation,
        &touchscreen->x,
        &touchscreen->y
    );
    touchscreen->x = clamp(touchscreen->x, 0, touchscreen->width - 1);
    touchscreen->y = clamp(touchscreen->y, 0, touchscreen->height - 1);
}

static void sync_active_multitouch_slot(struct touchscreen *touchscreen)
{
    int slot;

    touchscreen->down = 0;
    for (slot = 0; slot < TOUCHSCREEN_MAX_SLOTS; slot++) {
        if (touchscreen->slot_tracking_ids[slot] >= 0 &&
            touchscreen->slot_has_x[slot] &&
            touchscreen->slot_has_y[slot]) {
            touchscreen->raw_x = touchscreen->slot_raw_x[slot];
            touchscreen->raw_y = touchscreen->slot_raw_y[slot];
            touchscreen->has_x = 1;
            touchscreen->has_y = 1;
            touchscreen->down = 1;
            update_logical_position(touchscreen);
            return;
        }
    }
}

void touchscreen_poll(struct touchscreen *touchscreen)
{
    struct input_event event;
    ssize_t size;

    if (touchscreen->fd < 0) return;

    for (;;) {
        size = read(touchscreen->fd, &event, sizeof(event));
        if (size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            touchscreen->disconnected = 1;
            reset_contact_state(touchscreen);
            HOST_LOG_WARNF("touch", "read failed: %s", strerror(errno));
            break;
        }
        if (size == 0) {
            touchscreen->disconnected = 1;
            reset_contact_state(touchscreen);
            HOST_LOG_WARNF("touch", "%s", "touchscreen returned EOF");
            break;
        }
        if (size != (ssize_t)sizeof(event)) {
            HOST_LOG_WARNF("touch", "short evdev read: %zd bytes", size);
            break;
        }

        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            touchscreen->sync_dropped++;
            touchscreen->resync_pending = 1;
            reset_slots(touchscreen);
            touchscreen->tracking_id = -1;
            touchscreen->button_down = 0;
            touchscreen->has_x = 0;
            touchscreen->has_y = 0;
            touchscreen->down = 0;
            HOST_LOG_WARNF(
                "touch",
                "evdev SYN_DROPPED count=%lu; discarding until SYN_REPORT",
                touchscreen->sync_dropped
            );
            continue;
        }
        if (touchscreen->resync_pending) {
            if (event.type == EV_SYN && event.code == SYN_REPORT) {
                touchscreen->resync_pending = 0;
            }
            continue;
        }

        if (event.type == EV_ABS) {
            switch (event.code) {
            case ABS_MT_SLOT:
                if (event.value >= 0 && event.value < TOUCHSCREEN_MAX_SLOTS) {
                    touchscreen->active_slot = event.value;
                }
                break;
            case ABS_MT_TRACKING_ID:
                touchscreen->tracking_id = event.value;
                touchscreen->slot_tracking_ids[touchscreen->active_slot] = event.value;
                break;
            case ABS_MT_POSITION_X:
                touchscreen->slot_raw_x[touchscreen->active_slot] = event.value;
                touchscreen->slot_has_x[touchscreen->active_slot] = 1;
                break;
            case ABS_X:
                touchscreen->raw_x = event.value;
                touchscreen->has_x = 1;
                break;
            case ABS_MT_POSITION_Y:
                touchscreen->slot_raw_y[touchscreen->active_slot] = event.value;
                touchscreen->slot_has_y[touchscreen->active_slot] = 1;
                break;
            case ABS_Y:
                touchscreen->raw_y = event.value;
                touchscreen->has_y = 1;
                break;
            default:
                break;
            }
        } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
            touchscreen->button_down = event.value != 0;
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            if (touchscreen->uses_multitouch) {
                sync_active_multitouch_slot(touchscreen);
            } else {
                touchscreen->down = touchscreen->button_down;
                if (touchscreen->down &&
                    touchscreen->has_x &&
                    touchscreen->has_y) {
                    update_logical_position(touchscreen);
                }
            }
        }
    }
}

int touchscreen_is_down(const struct touchscreen *touchscreen)
{
    return touchscreen->down;
}

int touchscreen_x(const struct touchscreen *touchscreen)
{
    return touchscreen->x;
}

int touchscreen_y(const struct touchscreen *touchscreen)
{
    return touchscreen->y;
}
