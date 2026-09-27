#include "device/framebuffer.h"
#include "device/touchscreen.h"
#include "pocket_runtime.h"
#include "runtime/guest_file.h"
#include "runtime/host_log.h"
#include "runtime/pet_config.h"
#include "runtime/pet_host.h"
#include "runtime/pocket_extension.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TARGET_FRAME_NS 16666667LL
#define FPS_SAMPLE_NS 1000000000LL
#define HOST_STATS_OPCODE_FPS 1

static volatile sig_atomic_t stop_requested = 0;

struct phase_stats {
    unsigned long long samples;
    unsigned long long total_ns;
    unsigned long long max_ns;
};

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    sigemptyset(&action.sa_mask);
    action.sa_handler = request_stop;
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

static int parse_frame_limit(const char *text, unsigned long *limit)
{
    char *end;
    unsigned long value;

    value = strtoul(text, &end, 10);
    if (text[0] == '\0' || *end != '\0') {
        return -1;
    }
    *limit = value;
    return 0;
}

static long long monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void sleep_until_ns(long long deadline_ns)
{
    while (!stop_requested) {
        long long now_ns = monotonic_ns();
        long long remaining_ns;
        struct timespec remaining;

        if (now_ns < 0 || now_ns >= deadline_ns) {
            break;
        }
        remaining_ns = deadline_ns - now_ns;
        remaining.tv_sec = remaining_ns / 1000000000LL;
        remaining.tv_nsec = remaining_ns % 1000000000LL;
        if (nanosleep(&remaining, NULL) == 0) {
            break;
        }
        if (errno != EINTR) {
            break;
        }
    }
}

static int profile_enabled(void)
{
    const char *value = getenv("RV1106_PROFILE");

    return value != NULL &&
           value[0] != '\0' &&
           value[0] != '0' &&
           value[0] != 'f' &&
           value[0] != 'F';
}

static void record_phase(
    struct phase_stats *stats,
    long long elapsed_ns
)
{
    unsigned long long duration;

    if (elapsed_ns < 0) {
        return;
    }
    duration = (unsigned long long)elapsed_ns;
    stats->samples++;
    stats->total_ns += duration;
    if (duration > stats->max_ns) {
        stats->max_ns = duration;
    }
}

static double phase_average_ms(const struct phase_stats *stats)
{
    if (stats->samples == 0) {
        return 0.0;
    }
    return (double)stats->total_ns /
           (double)stats->samples /
           1000000.0;
}

static double phase_max_ms(const struct phase_stats *stats)
{
    return (double)stats->max_ns / 1000000.0;
}

static void print_phase_stats(
    const char *name,
    const struct phase_stats *stats
)
{
    printf(
        "profile %s avg_ms=%.3f max_ms=%.3f samples=%llu\n",
        name,
        phase_average_ms(stats),
        phase_max_ms(stats),
        stats->samples
    );
}

int main(int argc, char **argv)
{
    static const uint8_t empty_pack[1] = {0};
    struct framebuffer framebuffer = {
        .fd = -1,
        .map_length = 0,
        .memory = NULL,
    };
    struct touchscreen touchscreen = {
        .fd = -1,
    };
    PocketRuntimeInput input = {
        .buttons = 0,
        .touch_down = 0,
        .touch_x = 0,
        .touch_y = 0,
        .touch_hit = 0,
    };
    const uint8_t *rendered;
    uint8_t *script;
    uint8_t *pack = (uint8_t *)empty_pack;
    size_t script_length;
    size_t pack_length = 0;
    unsigned long frame_limit = 0;
    unsigned long frames_presented = 0;
    unsigned long dropped_frames = 0;
    unsigned long sample_frames = 0;
    unsigned long damage_frames = 0;
    unsigned long full_present_frames = 0;
    unsigned long empty_damage_frames = 0;
    long long next_frame_ns;
    long long sample_start_ns;
    long long profile_boot_start_ns = -1;
    long long profile_boot_end_ns = -1;
    long long profile_run_start_ns = -1;
    long long profile_run_end_ns = -1;
    struct phase_stats profile_touch = {0};
    struct phase_stats profile_tick = {0};
    struct phase_stats profile_render = {0};
    struct phase_stats profile_present = {0};
    struct phase_stats profile_conversion = {0};
    struct phase_stats profile_sync = {0};
    struct phase_stats profile_frame = {0};
    unsigned long long profile_present_pixels = 0;
    int profiling;
    int previous_touch_down = 0;
    int touch_hit = 0;
    int first_render = 1;
    int stats_bridge_enabled = 0;
    int pet_control_started = 0;
    PetAnimationConfig pet_animation_config;
    int exit_code = 0;
    int ok;

    host_log_init();
    profiling = profile_enabled();

    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s guest.js [guest.pak] [frames]\n", argv[0]);
        return 2;
    }
    if (argc == 4 && parse_frame_limit(argv[3], &frame_limit) != 0) {
        fprintf(stderr, "frames must be a non-negative integer\n");
        return 2;
    }
    if (install_signal_handlers() != 0) {
        return 1;
    }
    pet_config_defaults(&pet_animation_config);
    if (pet_config_load(pet_config_path(), &pet_animation_config) != 0) {
        HOST_LOG_WARNF("pet", "config not found, using defaults: %s", pet_config_path());
    } else {
        printf(
            "pet config: idle=%lus response_multiplier=%.2f\n",
            pet_animation_config.idle_seconds,
            pet_animation_config.response_multiplier
        );
    }
    pet_control_set_animation_config(&pet_animation_config);

    script = guest_file_read(argv[1], &script_length);
    if (script == NULL) {
        return 1;
    }
    if (argc >= 3) {
        pack = guest_file_read(argv[2], &pack_length);
        if (pack == NULL) {
            free(script);
            return 1;
        }
    }
    if (framebuffer_open("/dev/fb0", &framebuffer) != 0) {
        free(script);
        if (argc >= 3) free(pack);
        return 1;
    }
    if (touchscreen_open(
        getenv("RV1106_TOUCH_DEVICE"),
        (int)framebuffer.logical_width,
        (int)framebuffer.logical_height,
        (int)framebuffer.variable.xres,
        (int)framebuffer.variable.yres,
        framebuffer.rotation,
        &touchscreen
    ) != 0) {
        HOST_LOG_WARNF("main", "%s", "touchscreen input disabled");
    }

    {
        const char *port_text = getenv("RV1106_PET_PORT");
        unsigned long port = port_text == NULL ? 17890UL : strtoul(port_text, NULL, 10);
        if (port > 0 && port <= 65535 && pet_control_start((unsigned)port) == 0) {
            pet_control_started = 1;
            printf("pet control: http://0.0.0.0:%lu/command\n", port);
        } else {
            HOST_LOG_WARNF("pet", "%s", "pet control server disabled");
        }
    }

    pocket_runtime_set_global_installer(rv1106_pet_install);
    if (profiling) {
        profile_boot_start_ns = monotonic_ns();
    }
    ok = pocket_runtime_boot(
        (const char *)script,
        script_length,
        pack,
        pack_length,
        (int)framebuffer.logical_width,
        (int)framebuffer.logical_height
    );
    if (profiling) {
        profile_boot_end_ns = monotonic_ns();
    }
    free(script);
    if (!ok) {
        HOST_LOG_ERRORF(
            "runtime",
            "PocketJS boot failed: %s",
            pocket_runtime_error()
        );
        exit_code = 1;
        goto cleanup_devices;
    }
    stats_bridge_enabled = pocket_runtime_harness_bind("__rv1106HostStats");
    if (!stats_bridge_enabled) {
        HOST_LOG_WARNF(
            "runtime",
            "PocketJS stats bridge disabled: %s",
            pocket_runtime_error()
        );
    }

    next_frame_ns = monotonic_ns();
    if (next_frame_ns < 0) {
        perror("clock_gettime");
        exit_code = 1;
        goto cleanup_runtime;
    }
    sample_start_ns = next_frame_ns;
    if (profiling) {
        profile_run_start_ns = next_frame_ns;
    }

    while (!stop_requested &&
           (frame_limit == 0 || frames_presented < frame_limit)) {
        PocketRuntimeInput frame_input = input;
        long long frame_start_ns = -1;
        long long phase_start_ns = -1;
        long long now_ns;

        if (profiling) {
            frame_start_ns = monotonic_ns();
            phase_start_ns = frame_start_ns;
        }
        touchscreen_poll(&touchscreen);
        if (profiling) {
            long long phase_end_ns = monotonic_ns();

            record_phase(&profile_touch, phase_end_ns - phase_start_ns);
            phase_start_ns = phase_end_ns;
        }
        frame_input.touch_down = touchscreen_is_down(&touchscreen);
        frame_input.touch_x = touchscreen_x(&touchscreen);
        frame_input.touch_y = touchscreen_y(&touchscreen);
        if (frame_input.touch_down && !previous_touch_down) {
            touch_hit = pocket_runtime_hit_test_bounds(
                (float)frame_input.touch_x,
                (float)frame_input.touch_y
            );
            frame_input.touch_hit = touch_hit;
        } else if (frame_input.touch_down) {
            frame_input.touch_hit = touch_hit;
        } else if (!frame_input.touch_down) {
            touch_hit = 0;
            frame_input.touch_hit = 0;
        }
        previous_touch_down = frame_input.touch_down;

        ok = pocket_runtime_tick(&frame_input);
        if (profiling) {
            long long phase_end_ns = monotonic_ns();

            record_phase(&profile_tick, phase_end_ns - phase_start_ns);
            phase_start_ns = phase_end_ns;
        }
        if (!ok) {
            HOST_LOG_ERRORF(
                "runtime",
                "PocketJS tick failed: %s",
                pocket_runtime_error()
            );
            exit_code = 1;
            goto cleanup_runtime;
        }

        rendered = pocket_runtime_render();
        if (profiling) {
            long long phase_end_ns = monotonic_ns();

            record_phase(&profile_render, phase_end_ns - phase_start_ns);
            phase_start_ns = phase_end_ns;
        }
        if (rendered == NULL) {
            HOST_LOG_ERRORF(
                "runtime",
                "PocketJS render failed: %s",
                pocket_runtime_error()
            );
            exit_code = 1;
            goto cleanup_runtime;
        }
        {
            int damage_bounds[4];
            int has_damage = pocket_runtime_damage_bounds(damage_bounds);
            const int *present_bounds = NULL;

            if (!first_render && !has_damage) {
                empty_damage_frames++;
            } else {
                if (first_render || !has_damage) {
                    full_present_frames++;
                } else {
                    damage_frames++;
                    present_bounds = damage_bounds;
                }
            }
            if (first_render || has_damage) {
                struct framebuffer_present_stats present_stats = {0};
                long long present_start_ns = profiling
                    ? monotonic_ns()
                    : -1;

                if (framebuffer_present_bgra_damage_timed(
                    &framebuffer,
                    rendered,
                    pocket_runtime_width(),
                    pocket_runtime_height(),
                    pocket_runtime_stride(),
                    present_bounds,
                    profiling ? &present_stats : NULL
                ) != 0) {
                    exit_code = 1;
                    goto cleanup_runtime;
                }
                if (profiling) {
                    long long present_end_ns = monotonic_ns();

                    record_phase(
                        &profile_present,
                        present_end_ns - present_start_ns
                    );
                    record_phase(
                        &profile_conversion,
                        present_stats.conversion_ns
                    );
                    record_phase(&profile_sync, present_stats.sync_ns);
                    profile_present_pixels += present_stats.pixels;
                }
            }
            first_render = 0;
        }

        frames_presented++;
        sample_frames++;
        now_ns = monotonic_ns();
        if (now_ns < 0) {
            perror("clock_gettime");
            exit_code = 1;
            goto cleanup_runtime;
        }
        if (now_ns - sample_start_ns >= FPS_SAMPLE_NS) {
            long long elapsed_ns = now_ns - sample_start_ns;
            int fps = (int)((sample_frames * 1000000000ULL +
                             (unsigned long long)elapsed_ns / 2ULL) /
                            (unsigned long long)elapsed_ns);

            if (stats_bridge_enabled &&
                !pocket_runtime_harness_call(
                    HOST_STATS_OPCODE_FPS,
                    fps,
                    NULL
                )) {
                HOST_LOG_ERRORF(
                    "runtime",
                    "PocketJS stats update failed: %s",
                    pocket_runtime_error()
                );
                exit_code = 1;
                goto cleanup_runtime;
            }
            sample_frames = 0;
            sample_start_ns = now_ns;
        }
        if (profiling) {
            long long frame_end_ns = monotonic_ns();

            record_phase(
                &profile_frame,
                frame_end_ns - frame_start_ns
            );
        }

        next_frame_ns += TARGET_FRAME_NS;
        if (now_ns > next_frame_ns) {
            unsigned long missed = (unsigned long)(
                (now_ns - next_frame_ns) / TARGET_FRAME_NS
            ) + 1UL;

            dropped_frames += missed;
            next_frame_ns = now_ns + TARGET_FRAME_NS;
        }
        if (!stop_requested &&
            (frame_limit == 0 || frames_presented < frame_limit)) {
            sleep_until_ns(next_frame_ns);
        }
    }
    if (profiling) {
        profile_run_end_ns = monotonic_ns();
    }

    printf(
        "PocketJS frames presented: %lu, dropped=%lu, %ux%u stride=%u "
        "damage_pixels=%lu damage_frames=%lu full_present=%lu empty_damage=%lu\n",
        frames_presented,
        dropped_frames,
        pocket_runtime_width(),
        pocket_runtime_height(),
        pocket_runtime_stride(),
        pocket_runtime_damage_pixels(),
        damage_frames,
        full_present_frames,
        empty_damage_frames
    );
    if (profiling) {
        double run_ms = profile_run_start_ns >= 0 &&
                        profile_run_end_ns >= profile_run_start_ns
            ? (double)(profile_run_end_ns - profile_run_start_ns) / 1000000.0
            : 0.0;
        double effective_fps = run_ms > 0.0
            ? (double)frames_presented * 1000.0 / run_ms
            : 0.0;
        double boot_ms = profile_boot_start_ns >= 0 &&
                         profile_boot_end_ns >= profile_boot_start_ns
            ? (double)(profile_boot_end_ns - profile_boot_start_ns) / 1000000.0
            : 0.0;

        printf(
            "profile boot_ms=%.3f run_ms=%.3f effective_fps=%.2f\n",
            boot_ms,
            run_ms,
            effective_fps
        );
        print_phase_stats("frame", &profile_frame);
        print_phase_stats("touch", &profile_touch);
        print_phase_stats("tick", &profile_tick);
        print_phase_stats("render", &profile_render);
        print_phase_stats("present", &profile_present);
        print_phase_stats("conversion", &profile_conversion);
        print_phase_stats("sync", &profile_sync);
        printf(
            "profile present_pixels=%llu avg_pixels=%.1f\n",
            profile_present_pixels,
            profile_present.samples == 0
                ? 0.0
                : (double)profile_present_pixels /
                  (double)profile_present.samples
        );
    }

cleanup_runtime:
    pocket_runtime_shutdown();

cleanup_devices:
    if (pet_control_started) pet_control_stop();
    if (argc >= 3) free(pack);
    framebuffer_close(&framebuffer);
    touchscreen_close(&touchscreen);
    return exit_code;
}
