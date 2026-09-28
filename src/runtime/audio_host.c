#include "runtime/audio_host.h"

#include "runtime/host_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_MAX_STREAMS 4
#define AUDIO_ACTIVE_STREAMS 1
#define AUDIO_RING_FRAMES 16384
#define AUDIO_PERIOD_FRAMES 512
#define AUDIO_EVENT_CAPACITY 64
#define AUDIO_EVENT_LENGTH 96
#define AUDIO_ALSA_STREAM_PLAYBACK 0
#define AUDIO_ALSA_ACCESS_RW_INTERLEAVED 3
#define AUDIO_ALSA_FORMAT_S16_LE 2

typedef struct audio_snd_pcm AudioSndPcm;
typedef long AudioSndPcmFrames;
typedef unsigned long AudioSndPcmUFrames;

typedef struct audio_alsa_api {
    void *library;
    int loaded;
    int failed;
    int (*snd_pcm_open)(AudioSndPcm **pcm, const char *name, int stream, int mode);
    int (*snd_pcm_set_params)(
        AudioSndPcm *pcm,
        int format,
        int access,
        unsigned channels,
        unsigned rate,
        int soft_resample,
        unsigned latency
    );
    AudioSndPcmFrames (*snd_pcm_writei)(
        AudioSndPcm *pcm,
        const void *buffer,
        AudioSndPcmUFrames frames
    );
    int (*snd_pcm_prepare)(AudioSndPcm *pcm);
    int (*snd_pcm_drain)(AudioSndPcm *pcm);
    int (*snd_pcm_drop)(AudioSndPcm *pcm);
    int (*snd_pcm_close)(AudioSndPcm *pcm);
    int (*snd_pcm_wait)(AudioSndPcm *pcm, int timeout);
    const char *(*snd_strerror)(int errnum);
} AudioAlsaApi;

typedef struct audio_event_queue {
    pthread_mutex_t mutex;
    char events[AUDIO_EVENT_CAPACITY][AUDIO_EVENT_LENGTH];
    unsigned head;
    unsigned count;
} AudioEventQueue;

typedef struct audio_stream {
    int in_use;
    int handle;
    int sample_rate;
    int channels;
    int output_channels;
    int playing;
    int stopping;
    int end_requested;
    int underrun_reported;
    double volume;
    int16_t *ring;
    size_t read_frame;
    size_t write_frame;
    size_t used_frames;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    AudioSndPcm *pcm;
} AudioStream;

static AudioStream streams[AUDIO_MAX_STREAMS];
static pthread_mutex_t streams_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t alsa_mutex = PTHREAD_MUTEX_INITIALIZER;
static AudioEventQueue event_queue = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};
static AudioAlsaApi alsa_api;
static int next_handle = 1;

static int valid_rate(int sample_rate)
{
    return sample_rate == 44100 ||
           sample_rate == 22050 ||
           sample_rate == 11025;
}

static const char *audio_device_name(void)
{
    const char *device = getenv("RV1106_AUDIO_DEVICE");

    return device != NULL && device[0] != '\0' ? device : "hw:0,0";
}

static double audio_default_volume(void)
{
    const char *text = getenv("RV1106_AUDIO_VOLUME");
    char *end;
    double volume;

    if (text == NULL || text[0] == '\0') return 1.0;
    errno = 0;
    volume = strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !isfinite(volume)) {
        HOST_LOG_WARNF("audio", "invalid RV1106_AUDIO_VOLUME=%s, using 1.0", text);
        return 1.0;
    }
    if (volume < 0.0) return 0.0;
    if (volume > 1.0) return 1.0;
    return volume;
}

static int load_alsa_symbol(void **target, const char *name)
{
    void *symbol = dlsym(alsa_api.library, name);

    if (symbol == NULL) return -1;
    memcpy(target, &symbol, sizeof(symbol));
    return 0;
}

static int load_alsa_api(void)
{
    int ok;

    pthread_mutex_lock(&alsa_mutex);
    if (alsa_api.loaded) {
        pthread_mutex_unlock(&alsa_mutex);
        return 0;
    }
    if (alsa_api.failed) {
        pthread_mutex_unlock(&alsa_mutex);
        return -1;
    }
    alsa_api.library = dlopen("libasound.so.2", RTLD_LAZY);
    if (alsa_api.library == NULL) {
        alsa_api.library = dlopen("libasound.so", RTLD_LAZY);
    }
    if (alsa_api.library == NULL) {
        HOST_LOG_WARNF("audio", "libasound unavailable: %s", dlerror());
        alsa_api.failed = 1;
        pthread_mutex_unlock(&alsa_mutex);
        return -1;
    }
    ok = load_alsa_symbol((void **)&alsa_api.snd_pcm_open, "snd_pcm_open") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_set_params, "snd_pcm_set_params") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_writei, "snd_pcm_writei") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_prepare, "snd_pcm_prepare") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_drain, "snd_pcm_drain") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_drop, "snd_pcm_drop") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_close, "snd_pcm_close") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_pcm_wait, "snd_pcm_wait") == 0 &&
         load_alsa_symbol((void **)&alsa_api.snd_strerror, "snd_strerror") == 0;
    if (!ok) {
        HOST_LOG_WARNF("audio", "libasound is missing required symbols: %s", dlerror());
        dlclose(alsa_api.library);
        memset(&alsa_api, 0, sizeof(alsa_api));
        alsa_api.failed = 1;
        pthread_mutex_unlock(&alsa_mutex);
        return -1;
    }
    alsa_api.loaded = 1;
    pthread_mutex_unlock(&alsa_mutex);
    return 0;
}

static const char *alsa_error(int code)
{
    if (alsa_api.snd_strerror == NULL) return "ALSA error";
    return alsa_api.snd_strerror(code);
}

static void queue_event_text(const char *text)
{
    unsigned slot;

    pthread_mutex_lock(&event_queue.mutex);
    if (event_queue.count == AUDIO_EVENT_CAPACITY) {
        event_queue.head = (event_queue.head + 1U) % AUDIO_EVENT_CAPACITY;
        event_queue.count--;
    }
    slot = (event_queue.head + event_queue.count) % AUDIO_EVENT_CAPACITY;
    snprintf(event_queue.events[slot], AUDIO_EVENT_LENGTH, "%s", text);
    event_queue.count++;
    pthread_mutex_unlock(&event_queue.mutex);
}

static void queue_credit(int handle, size_t free_frames)
{
    char event[AUDIO_EVENT_LENGTH];

    snprintf(
        event,
        sizeof(event),
        "{\"t\":\"credit\",\"h\":%d,\"free\":%lu}",
        handle,
        (unsigned long)free_frames
    );
    queue_event_text(event);
}

static void queue_simple_event(const char *type, int handle)
{
    char event[AUDIO_EVENT_LENGTH];

    snprintf(
        event,
        sizeof(event),
        "{\"t\":\"%s\",\"h\":%d}",
        type,
        handle
    );
    queue_event_text(event);
}

static int pop_event(char *out, size_t capacity)
{
    const char *event;

    if (out == NULL || capacity == 0) return 0;
    pthread_mutex_lock(&event_queue.mutex);
    if (event_queue.count == 0) {
        pthread_mutex_unlock(&event_queue.mutex);
        return 0;
    }
    event = event_queue.events[event_queue.head];
    snprintf(out, capacity, "%s", event);
    event_queue.head = (event_queue.head + 1U) % AUDIO_EVENT_CAPACITY;
    event_queue.count--;
    pthread_mutex_unlock(&event_queue.mutex);
    return 1;
}

static void clear_events(void)
{
    pthread_mutex_lock(&event_queue.mutex);
    event_queue.head = 0;
    event_queue.count = 0;
    pthread_mutex_unlock(&event_queue.mutex);
}

static void apply_volume(int16_t *samples, size_t sample_count, double volume)
{
    size_t i;

    if (volume >= 0.999) return;
    for (i = 0; i < sample_count; i++) {
        int value = (int)lrint((double)samples[i] * volume);

        if (value > INT16_MAX) value = INT16_MAX;
        else if (value < INT16_MIN) value = INT16_MIN;
        samples[i] = (int16_t)value;
    }
}

static int prepare_pcm(AudioSndPcm *pcm)
{
    int rc = alsa_api.snd_pcm_prepare(pcm);

    if (rc < 0) {
        HOST_LOG_WARNF("audio", "snd_pcm_prepare failed: %s", alsa_error(rc));
        return -1;
    }
    return 0;
}

static int write_frames(AudioStream *stream, const int16_t *samples, size_t frames)
{
    size_t offset = 0;

    while (offset < frames) {
        AudioSndPcmFrames written = alsa_api.snd_pcm_writei(
            stream->pcm,
            samples + offset * (size_t)stream->output_channels,
            (AudioSndPcmUFrames)(frames - offset)
        );

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written == -EPIPE || written == -ESTRPIPE) {
            if (prepare_pcm(stream->pcm) != 0) return -1;
            continue;
        }
        if (written == -EAGAIN) {
            alsa_api.snd_pcm_wait(stream->pcm, 50);
            continue;
        }
        HOST_LOG_WARNF("audio", "snd_pcm_writei failed: %s", alsa_error((int)written));
        return -1;
    }
    return 0;
}

static void *audio_thread_main(void *opaque)
{
    AudioStream *stream = (AudioStream *)opaque;
    int16_t chunk[AUDIO_PERIOD_FRAMES * 2];

    for (;;) {
        size_t frames;
        int handle;
        size_t free_frames;
        size_t frame_index;

        pthread_mutex_lock(&stream->mutex);
        while (!stream->stopping &&
               (!stream->playing || stream->used_frames == 0)) {
            if (stream->playing && stream->used_frames == 0) {
                if (stream->end_requested) {
                    handle = stream->handle;
                    stream->playing = 0;
                    stream->end_requested = 0;
                    stream->underrun_reported = 0;
                    pthread_mutex_unlock(&stream->mutex);
                    alsa_api.snd_pcm_drain(stream->pcm);
                    prepare_pcm(stream->pcm);
                    queue_simple_event("ended", handle);
                    queue_credit(handle, AUDIO_RING_FRAMES);
                    pthread_mutex_lock(&stream->mutex);
                } else if (!stream->underrun_reported) {
                    handle = stream->handle;
                    stream->underrun_reported = 1;
                    pthread_mutex_unlock(&stream->mutex);
                    queue_simple_event("underrun", handle);
                    pthread_mutex_lock(&stream->mutex);
                }
            }
            if (!stream->stopping &&
                (!stream->playing || stream->used_frames == 0)) {
                pthread_cond_wait(&stream->cond, &stream->mutex);
            }
        }
        if (stream->stopping) {
            pthread_mutex_unlock(&stream->mutex);
            break;
        }

        frames = stream->used_frames;
        if (frames > AUDIO_PERIOD_FRAMES) frames = AUDIO_PERIOD_FRAMES;
        for (frame_index = 0; frame_index < frames; frame_index++) {
            size_t ring_frame = (stream->read_frame + frame_index) % AUDIO_RING_FRAMES;

            if (stream->channels == 1 && stream->output_channels == 2) {
                int16_t sample = stream->ring[ring_frame];

                chunk[frame_index * 2U] = sample;
                chunk[frame_index * 2U + 1U] = sample;
            } else {
                memcpy(
                    chunk + frame_index * (size_t)stream->output_channels,
                    stream->ring + ring_frame * (size_t)stream->channels,
                    (size_t)stream->channels * sizeof(int16_t)
                );
            }
        }
        stream->read_frame = (stream->read_frame + frames) % AUDIO_RING_FRAMES;
        stream->used_frames -= frames;
        stream->underrun_reported = 0;
        handle = stream->handle;
        free_frames = AUDIO_RING_FRAMES - stream->used_frames;
        apply_volume(
            chunk,
            frames * (size_t)stream->output_channels,
            stream->volume
        );
        pthread_mutex_unlock(&stream->mutex);

        if (write_frames(stream, chunk, frames) != 0) {
            pthread_mutex_lock(&stream->mutex);
            stream->playing = 0;
            pthread_mutex_unlock(&stream->mutex);
        }
        queue_credit(handle, free_frames);
    }
    return NULL;
}

static AudioStream *find_stream_locked(int handle)
{
    size_t i;

    for (i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (streams[i].in_use && streams[i].handle == handle) {
            return &streams[i];
        }
    }
    return NULL;
}

static int stream_count_locked(void)
{
    int count = 0;
    size_t i;

    for (i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (streams[i].in_use) count++;
    }
    return count;
}

static int open_configured_pcm(AudioStream *stream, const char *device, int output_channels)
{
    int rc = alsa_api.snd_pcm_open(
        &stream->pcm,
        device,
        AUDIO_ALSA_STREAM_PLAYBACK,
        0
    );

    if (rc < 0) {
        HOST_LOG_WARNF("audio", "open %s failed: %s", device, alsa_error(rc));
        stream->pcm = NULL;
        return -1;
    }
    rc = alsa_api.snd_pcm_set_params(
        stream->pcm,
        AUDIO_ALSA_FORMAT_S16_LE,
        AUDIO_ALSA_ACCESS_RW_INTERLEAVED,
        (unsigned)output_channels,
        (unsigned)stream->sample_rate,
        1,
        100000
    );
    if (rc == 0) {
        stream->output_channels = output_channels;
        return 0;
    }
    HOST_LOG_WARNF(
        "audio",
        "configure %s %dch failed: %s",
        device,
        output_channels,
        alsa_error(rc)
    );
    alsa_api.snd_pcm_close(stream->pcm);
    stream->pcm = NULL;
    return -1;
}

static void reset_stream(AudioStream *stream)
{
    memset(stream, 0, sizeof(*stream));
    stream->volume = audio_default_volume();
}

static int create_stream(int sample_rate, int channels)
{
    AudioStream *stream = NULL;
    const char *device = audio_device_name();
    size_t i;
    int rc;

    if (!valid_rate(sample_rate) || channels < 1 || channels > 2) return -1;
    if (load_alsa_api() != 0) return -1;

    pthread_mutex_lock(&streams_mutex);
    if (stream_count_locked() >= AUDIO_ACTIVE_STREAMS) {
        pthread_mutex_unlock(&streams_mutex);
        return -1;
    }
    for (i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (!streams[i].in_use) {
            stream = &streams[i];
            break;
        }
    }
    if (stream == NULL) {
        pthread_mutex_unlock(&streams_mutex);
        return -1;
    }
    reset_stream(stream);
    stream->in_use = 1;
    stream->handle = next_handle++;
    if (next_handle <= 0) next_handle = 1;
    pthread_mutex_unlock(&streams_mutex);

    stream->sample_rate = sample_rate;
    stream->channels = channels;
    stream->output_channels = channels;
    stream->ring = calloc(
        AUDIO_RING_FRAMES * (size_t)channels,
        sizeof(int16_t)
    );
    if (stream->ring == NULL) goto fail;
    if (pthread_mutex_init(&stream->mutex, NULL) != 0) goto fail;
    if (pthread_cond_init(&stream->cond, NULL) != 0) goto fail_mutex;

    rc = open_configured_pcm(stream, device, channels);
    if (rc != 0 && channels == 1) {
        HOST_LOG_WARNF("audio", "%s", "falling back to stereo output for mono stream");
        rc = open_configured_pcm(stream, device, 2);
    }
    if (rc != 0) goto fail_cond;
    if (pthread_create(&stream->thread, NULL, audio_thread_main, stream) != 0) {
        goto fail_pcm;
    }
    queue_credit(stream->handle, AUDIO_RING_FRAMES);
    return stream->handle;

fail_pcm:
    if (stream->pcm != NULL) {
        alsa_api.snd_pcm_close(stream->pcm);
        stream->pcm = NULL;
    }
fail_cond:
    pthread_cond_destroy(&stream->cond);
fail_mutex:
    pthread_mutex_destroy(&stream->mutex);
fail:
    free(stream->ring);
    pthread_mutex_lock(&streams_mutex);
    reset_stream(stream);
    pthread_mutex_unlock(&streams_mutex);
    return -1;
}

static void destroy_stream(AudioStream *stream)
{
    if (stream == NULL || !stream->in_use) return;
    pthread_mutex_lock(&stream->mutex);
    stream->stopping = 1;
    pthread_cond_signal(&stream->cond);
    pthread_mutex_unlock(&stream->mutex);
    pthread_join(stream->thread, NULL);
    if (stream->pcm != NULL) {
        alsa_api.snd_pcm_drop(stream->pcm);
        alsa_api.snd_pcm_close(stream->pcm);
    }
    pthread_cond_destroy(&stream->cond);
    pthread_mutex_destroy(&stream->mutex);
    free(stream->ring);
    pthread_mutex_lock(&streams_mutex);
    reset_stream(stream);
    pthread_mutex_unlock(&streams_mutex);
}

static JSValue js_audio_create_stream(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t sample_rate;
    int32_t channels;

    (void)this_value;
    if (argc < 2 ||
        JS_ToInt32(ctx, &sample_rate, argv[0]) < 0 ||
        JS_ToInt32(ctx, &channels, argv[1]) < 0) {
        return JS_NewInt32(ctx, -1);
    }
    return JS_NewInt32(ctx, create_stream(sample_rate, channels));
}

static JSValue js_audio_destroy_stream(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    AudioStream *stream = NULL;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    destroy_stream(stream);
    return JS_UNDEFINED;
}

static int js_read_pcm_buffer(
    JSContext *ctx,
    JSValueConst value,
    const uint8_t **bytes,
    size_t *length
)
{
    uint8_t *direct = JS_GetArrayBuffer(ctx, length, value);

    if (!JS_HasException(ctx)) {
        *bytes = direct;
        return 1;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t offset = 0;
        size_t byte_length = 0;
        size_t bytes_per_element = 0;
        size_t buffer_length = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(
            ctx,
            value,
            &offset,
            &byte_length,
            &bytes_per_element
        );
        uint8_t *base;

        (void)bytes_per_element;
        if (JS_IsException(buffer)) return 0;
        base = JS_GetArrayBuffer(ctx, &buffer_length, buffer);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, buffer);
            return 0;
        }
        if (offset > buffer_length || byte_length > buffer_length - offset) {
            JS_FreeValue(ctx, buffer);
            JS_ThrowRangeError(ctx, "typed array outside backing buffer");
            return 0;
        }
        *bytes = base == NULL ? NULL : base + offset;
        *length = byte_length;
        JS_FreeValue(ctx, buffer);
    }
    return 1;
}

static JSValue js_audio_write_pcm(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    const uint8_t *bytes;
    const int16_t *samples;
    size_t byte_length;
    AudioStream *stream;
    size_t input_frames;
    size_t accepted_frames;
    size_t first_frames;
    size_t sample_offset;

    (void)this_value;
    if (argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) < 0) {
        return JS_NewInt32(ctx, 0);
    }
    if (!js_read_pcm_buffer(ctx, argv[1], &bytes, &byte_length)) {
        return JS_EXCEPTION;
    }
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL || bytes == NULL) return JS_NewInt32(ctx, 0);

    input_frames = byte_length / ((size_t)stream->channels * sizeof(int16_t));
    samples = (const int16_t *)bytes;
    pthread_mutex_lock(&stream->mutex);
    accepted_frames = AUDIO_RING_FRAMES - stream->used_frames;
    if (accepted_frames > input_frames) accepted_frames = input_frames;
    first_frames = AUDIO_RING_FRAMES - stream->write_frame;
    if (first_frames > accepted_frames) first_frames = accepted_frames;
    sample_offset = first_frames * (size_t)stream->channels;
    memcpy(
        stream->ring + stream->write_frame * (size_t)stream->channels,
        samples,
        sample_offset * sizeof(int16_t)
    );
    if (first_frames < accepted_frames) {
        memcpy(
            stream->ring,
            samples + sample_offset,
            (accepted_frames - first_frames) *
                (size_t)stream->channels *
                sizeof(int16_t)
        );
    }
    stream->write_frame = (stream->write_frame + accepted_frames) % AUDIO_RING_FRAMES;
    stream->used_frames += accepted_frames;
    pthread_cond_signal(&stream->cond);
    pthread_mutex_unlock(&stream->mutex);

    return JS_NewInt32(ctx, (int32_t)accepted_frames);
}

static JSValue js_audio_play(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    AudioStream *stream;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL) return JS_UNDEFINED;
    pthread_mutex_lock(&stream->mutex);
    stream->playing = 1;
    stream->underrun_reported = 0;
    pthread_cond_signal(&stream->cond);
    pthread_mutex_unlock(&stream->mutex);
    return JS_UNDEFINED;
}

static JSValue js_audio_pause(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    AudioStream *stream;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL) return JS_UNDEFINED;
    pthread_mutex_lock(&stream->mutex);
    stream->playing = 0;
    stream->underrun_reported = 0;
    pthread_mutex_unlock(&stream->mutex);
    return JS_UNDEFINED;
}

static JSValue js_audio_stop(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    AudioStream *stream;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL) return JS_UNDEFINED;
    pthread_mutex_lock(&stream->mutex);
    stream->playing = 0;
    stream->end_requested = 0;
    stream->underrun_reported = 0;
    stream->read_frame = 0;
    stream->write_frame = 0;
    stream->used_frames = 0;
    pthread_mutex_unlock(&stream->mutex);
    alsa_api.snd_pcm_drop(stream->pcm);
    prepare_pcm(stream->pcm);
    queue_credit(handle, AUDIO_RING_FRAMES);
    return JS_UNDEFINED;
}

static JSValue js_audio_set_volume(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    double volume;
    AudioStream *stream;

    (void)this_value;
    if (argc < 2 ||
        JS_ToInt32(ctx, &handle, argv[0]) < 0 ||
        JS_ToFloat64(ctx, &volume, argv[1]) < 0) {
        return JS_UNDEFINED;
    }
    if (!isfinite(volume)) volume = 1.0;
    if (volume < 0.0) volume = 0.0;
    else if (volume > 1.0) volume = 1.0;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL) return JS_UNDEFINED;
    pthread_mutex_lock(&stream->mutex);
    stream->volume = volume;
    pthread_mutex_unlock(&stream->mutex);
    return JS_UNDEFINED;
}

static JSValue js_audio_end_stream(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    int32_t handle;
    AudioStream *stream;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&streams_mutex);
    stream = find_stream_locked(handle);
    pthread_mutex_unlock(&streams_mutex);
    if (stream == NULL) return JS_UNDEFINED;
    pthread_mutex_lock(&stream->mutex);
    stream->end_requested = 1;
    pthread_cond_signal(&stream->cond);
    pthread_mutex_unlock(&stream->mutex);
    return JS_UNDEFINED;
}

static JSValue js_audio_poll(
    JSContext *ctx,
    JSValueConst this_value,
    int argc,
    JSValueConst *argv
)
{
    char event[AUDIO_EVENT_LENGTH];

    (void)this_value;
    (void)argc;
    (void)argv;
    if (!pop_event(event, sizeof(event))) return JS_UNDEFINED;
    return JS_NewString(ctx, event);
}

static int set_function(JSContext *ctx, JSValueConst object, const char *name, JSCFunction *function, int argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);

    if (JS_IsException(value)) return 0;
    if (JS_SetPropertyStr(ctx, object, name, value) < 0) {
        JS_FreeValue(ctx, value);
        return 0;
    }
    return 1;
}

int rv1106_audio_install(JSContext *ctx, JSValueConst global)
{
    JSValue audio = JS_NewObject(ctx);

    if (JS_IsException(audio)) return 0;
    if (!set_function(ctx, audio, "createStream", js_audio_create_stream, 2) ||
        !set_function(ctx, audio, "destroyStream", js_audio_destroy_stream, 1) ||
        !set_function(ctx, audio, "writePcm", js_audio_write_pcm, 2) ||
        !set_function(ctx, audio, "play", js_audio_play, 1) ||
        !set_function(ctx, audio, "pause", js_audio_pause, 1) ||
        !set_function(ctx, audio, "stop", js_audio_stop, 1) ||
        !set_function(ctx, audio, "setVolume", js_audio_set_volume, 2) ||
        !set_function(ctx, audio, "endStream", js_audio_end_stream, 1) ||
        !set_function(ctx, audio, "poll", js_audio_poll, 0)) {
        JS_FreeValue(ctx, audio);
        return 0;
    }
    return JS_SetPropertyStr(ctx, global, "audio", audio) >= 0;
}

void rv1106_audio_shutdown(void)
{
    size_t i;

    for (i = 0; i < AUDIO_MAX_STREAMS; i++) {
        AudioStream *stream = NULL;

        pthread_mutex_lock(&streams_mutex);
        if (streams[i].in_use) stream = &streams[i];
        pthread_mutex_unlock(&streams_mutex);
        destroy_stream(stream);
    }
    clear_events();
}
