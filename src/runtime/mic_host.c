#include "runtime/mic_host.h"

#include "runtime/host_log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MIC_ALSA_STREAM_CAPTURE 1
#define MIC_ALSA_ACCESS_RW_INTERLEAVED 3
#define MIC_ALSA_FORMAT_S16_LE 2
#define MIC_CHUNK_FRAMES 320
#define MIC_MAX_READ_ERRORS 8

typedef struct mic_snd_pcm MicSndPcm;
typedef long MicSndPcmFrames;
typedef unsigned long MicSndPcmUFrames;

typedef struct mic_alsa_api {
    void *library;
    int loaded;
    int failed;
    int (*snd_pcm_open)(MicSndPcm **pcm, const char *name, int stream, int mode);
    int (*snd_pcm_set_params)(
        MicSndPcm *pcm,
        int format,
        int access,
        unsigned channels,
        unsigned rate,
        int soft_resample,
        unsigned latency
    );
    MicSndPcmFrames (*snd_pcm_readi)(
        MicSndPcm *pcm,
        void *buffer,
        MicSndPcmUFrames frames
    );
    int (*snd_pcm_prepare)(MicSndPcm *pcm);
    int (*snd_pcm_close)(MicSndPcm *pcm);
    const char *(*snd_strerror)(int errnum);
} MicAlsaApi;

typedef struct mic_recorder {
    pthread_mutex_t mutex;
    pthread_t thread;
    int active;
    int stop_requested;
    int failed;
    unsigned sample_rate;
    unsigned channels;
    unsigned max_seconds;
    uint8_t *data;
    size_t capacity;
    size_t length;
} MicRecorder;

static pthread_mutex_t mic_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;
static MicAlsaApi mic_alsa;
static MicRecorder recorder = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static const char *mic_device_name(void)
{
    const char *device = getenv("RV1106_MIC_DEVICE");

    return device != NULL && device[0] != '\0' ? device : "plughw:0,0";
}

static int load_mic_symbol(void **target, const char *name)
{
    void *symbol = dlsym(mic_alsa.library, name);

    if (symbol == NULL) return -1;
    memcpy(target, &symbol, sizeof(symbol));
    return 0;
}

static int load_mic_alsa(void)
{
    int ok;

    pthread_mutex_lock(&mic_alsa_mutex);
    if (mic_alsa.loaded) {
        pthread_mutex_unlock(&mic_alsa_mutex);
        return 0;
    }
    if (mic_alsa.failed) {
        pthread_mutex_unlock(&mic_alsa_mutex);
        return -1;
    }
    mic_alsa.library = dlopen("libasound.so.2", RTLD_LAZY);
    if (mic_alsa.library == NULL) {
        mic_alsa.library = dlopen("libasound.so", RTLD_LAZY);
    }
    if (mic_alsa.library == NULL) {
        HOST_LOG_WARNF("mic", "libasound unavailable: %s", dlerror());
        mic_alsa.failed = 1;
        pthread_mutex_unlock(&mic_alsa_mutex);
        return -1;
    }
    ok = load_mic_symbol((void **)&mic_alsa.snd_pcm_open, "snd_pcm_open") == 0 &&
         load_mic_symbol((void **)&mic_alsa.snd_pcm_set_params, "snd_pcm_set_params") == 0 &&
         load_mic_symbol((void **)&mic_alsa.snd_pcm_readi, "snd_pcm_readi") == 0 &&
         load_mic_symbol((void **)&mic_alsa.snd_pcm_prepare, "snd_pcm_prepare") == 0 &&
         load_mic_symbol((void **)&mic_alsa.snd_pcm_close, "snd_pcm_close") == 0 &&
         load_mic_symbol((void **)&mic_alsa.snd_strerror, "snd_strerror") == 0;
    if (!ok) {
        HOST_LOG_WARNF("mic", "libasound is missing required symbols: %s", dlerror());
        dlclose(mic_alsa.library);
        memset(&mic_alsa, 0, sizeof(mic_alsa));
        mic_alsa.failed = 1;
        pthread_mutex_unlock(&mic_alsa_mutex);
        return -1;
    }
    mic_alsa.loaded = 1;
    pthread_mutex_unlock(&mic_alsa_mutex);
    return 0;
}

static const char *mic_alsa_error(int code)
{
    if (mic_alsa.snd_strerror == NULL) return "ALSA error";
    return mic_alsa.snd_strerror(code);
}

static int open_capture(MicSndPcm **pcm, unsigned sample_rate, unsigned channels)
{
    const char *device = mic_device_name();
    int rc;

    *pcm = NULL;
    rc = mic_alsa.snd_pcm_open(pcm, device, MIC_ALSA_STREAM_CAPTURE, 0);
    if (rc < 0) {
        HOST_LOG_WARNF("mic", "open %s failed: %s", device, mic_alsa_error(rc));
        return -1;
    }
    rc = mic_alsa.snd_pcm_set_params(
        *pcm,
        MIC_ALSA_FORMAT_S16_LE,
        MIC_ALSA_ACCESS_RW_INTERLEAVED,
        channels,
        sample_rate,
        1,
        100000
    );
    if (rc < 0) {
        HOST_LOG_WARNF(
            "mic",
            "configure %s %uch/%uHz failed: %s",
            device,
            channels,
            sample_rate,
            mic_alsa_error(rc)
        );
        mic_alsa.snd_pcm_close(*pcm);
        *pcm = NULL;
        return -1;
    }
    mic_alsa.snd_pcm_prepare(*pcm);
    return 0;
}

int rv1106_mic_capture_pcm(
    unsigned sample_rate,
    unsigned channels,
    unsigned seconds,
    Rv1106MicCapture *out
)
{
    MicSndPcm *pcm = NULL;
    uint8_t *data;
    unsigned long total_frames;
    unsigned long captured_frames = 0;
    size_t frame_bytes;
    unsigned read_errors = 0;

    if (out == NULL || sample_rate < 8000 || sample_rate > 48000 ||
        channels < 1 || channels > 2 || seconds < 1 || seconds > 15) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (load_mic_alsa() != 0) return -1;

    if (open_capture(&pcm, sample_rate, channels) != 0) {
        if (channels == 1 && open_capture(&pcm, sample_rate, 2) == 0) {
            channels = 2;
        } else {
            return -1;
        }
    }

    total_frames = (unsigned long)sample_rate * seconds;
    frame_bytes = (size_t)channels * sizeof(int16_t);
    data = calloc(total_frames, frame_bytes);
    if (data == NULL) {
        mic_alsa.snd_pcm_close(pcm);
        return -1;
    }

    while (captured_frames < total_frames) {
        unsigned long remaining = total_frames - captured_frames;
        unsigned long request = remaining < MIC_CHUNK_FRAMES ? remaining : MIC_CHUNK_FRAMES;
        MicSndPcmFrames read_frames = mic_alsa.snd_pcm_readi(
            pcm,
            data + captured_frames * frame_bytes,
            (MicSndPcmUFrames)request
        );

        if (read_frames < 0) {
            HOST_LOG_WARNF("mic", "snd_pcm_readi failed: %s", mic_alsa_error((int)read_frames));
            read_errors++;
            if (read_errors >= MIC_MAX_READ_ERRORS) {
                free(data);
                mic_alsa.snd_pcm_close(pcm);
                return -1;
            }
            mic_alsa.snd_pcm_prepare(pcm);
            continue;
        }
        if (read_frames == 0) continue;
        read_errors = 0;
        captured_frames += (unsigned long)read_frames;
    }

    mic_alsa.snd_pcm_close(pcm);
    out->data = data;
    out->length = captured_frames * frame_bytes;
    out->sample_rate = sample_rate;
    out->channels = channels;
    return 0;
}

void rv1106_mic_capture_free(Rv1106MicCapture *capture)
{
    if (capture == NULL) return;
    free(capture->data);
    memset(capture, 0, sizeof(*capture));
}

static void *record_thread_main(void *opaque)
{
    MicRecorder *state = (MicRecorder *)opaque;
    MicSndPcm *pcm = NULL;
    unsigned channels;
    size_t frame_bytes;
    size_t max_frames;
    size_t captured_frames = 0;
    unsigned read_errors = 0;

    if (load_mic_alsa() != 0) goto fail;
    channels = state->channels;
    if (open_capture(&pcm, state->sample_rate, channels) != 0) {
        if (channels == 1 && open_capture(&pcm, state->sample_rate, 2) == 0) {
            channels = 2;
        } else {
            goto fail;
        }
    }

    pthread_mutex_lock(&state->mutex);
    state->channels = channels;
    pthread_mutex_unlock(&state->mutex);

    frame_bytes = (size_t)channels * sizeof(int16_t);
    max_frames = state->capacity / frame_bytes;

    while (captured_frames < max_frames) {
        MicSndPcmFrames read_frames;
        size_t remaining_frames;
        size_t request_frames;
        int stop;

        pthread_mutex_lock(&state->mutex);
        stop = state->stop_requested;
        pthread_mutex_unlock(&state->mutex);
        if (stop) break;

        remaining_frames = max_frames - captured_frames;
        request_frames = remaining_frames < MIC_CHUNK_FRAMES
            ? remaining_frames
            : MIC_CHUNK_FRAMES;
        read_frames = mic_alsa.snd_pcm_readi(
            pcm,
            state->data + captured_frames * frame_bytes,
            (MicSndPcmUFrames)request_frames
        );
        if (read_frames < 0) {
            HOST_LOG_WARNF("mic", "snd_pcm_readi failed: %s", mic_alsa_error((int)read_frames));
            read_errors++;
            if (read_errors >= MIC_MAX_READ_ERRORS) goto fail_with_pcm;
            mic_alsa.snd_pcm_prepare(pcm);
            continue;
        }
        if (read_frames == 0) continue;
        read_errors = 0;
        captured_frames += (size_t)read_frames;
        pthread_mutex_lock(&state->mutex);
        state->length = captured_frames * frame_bytes;
        pthread_mutex_unlock(&state->mutex);
    }

    mic_alsa.snd_pcm_close(pcm);
    pthread_mutex_lock(&state->mutex);
    state->active = 0;
    pthread_mutex_unlock(&state->mutex);
    return NULL;

fail_with_pcm:
    if (pcm != NULL) mic_alsa.snd_pcm_close(pcm);
fail:
    pthread_mutex_lock(&state->mutex);
    state->failed = 1;
    state->active = 0;
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

int rv1106_mic_record_start(unsigned sample_rate, unsigned channels, unsigned max_seconds)
{
    size_t capacity;

    if (sample_rate < 8000 || sample_rate > 48000 ||
        channels < 1 || channels > 2 || max_seconds < 1 || max_seconds > 30) {
        return -1;
    }

    pthread_mutex_lock(&recorder.mutex);
    if (recorder.active) {
        pthread_mutex_unlock(&recorder.mutex);
        return -1;
    }
    free(recorder.data);
    capacity = (size_t)sample_rate * max_seconds * channels * sizeof(int16_t);
    recorder.data = calloc(1, capacity);
    if (recorder.data == NULL) {
        pthread_mutex_unlock(&recorder.mutex);
        return -1;
    }
    recorder.capacity = capacity;
    recorder.sample_rate = sample_rate;
    recorder.channels = channels;
    recorder.max_seconds = max_seconds;
    recorder.active = 1;
    recorder.stop_requested = 0;
    recorder.failed = 0;
    recorder.length = 0;
    recorder.thread = 0;
    if (pthread_create(&recorder.thread, NULL, record_thread_main, &recorder) != 0) {
        free(recorder.data);
        recorder.data = NULL;
        recorder.active = 0;
        pthread_mutex_unlock(&recorder.mutex);
        return -1;
    }
    pthread_mutex_unlock(&recorder.mutex);
    return 0;
}

int rv1106_mic_record_stop(Rv1106MicCapture *out)
{
    uint8_t *data;
    size_t length;
    unsigned sample_rate;
    unsigned channels;
    int failed;
    int should_join;
    pthread_t thread;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&recorder.mutex);
    should_join = recorder.active || recorder.data != NULL;
    if (!should_join) {
        pthread_mutex_unlock(&recorder.mutex);
        return -1;
    }
    recorder.stop_requested = 1;
    thread = recorder.thread;
    pthread_mutex_unlock(&recorder.mutex);

    if (thread) pthread_join(thread, NULL);

    pthread_mutex_lock(&recorder.mutex);
    data = recorder.data;
    length = recorder.length;
    sample_rate = recorder.sample_rate;
    channels = recorder.channels;
    failed = recorder.failed || length < (size_t)(sample_rate / 5U) * channels * sizeof(int16_t);
    recorder.data = NULL;
    recorder.capacity = 0;
    recorder.length = 0;
    recorder.active = 0;
    recorder.stop_requested = 0;
    recorder.thread = 0;
    pthread_mutex_unlock(&recorder.mutex);

    if (failed || data == NULL || length == 0) {
        free(data);
        return -1;
    }
    out->data = data;
    out->length = length;
    out->sample_rate = sample_rate;
    out->channels = channels;
    return 0;
}

int rv1106_mic_recording(void)
{
    int active;

    pthread_mutex_lock(&recorder.mutex);
    active = recorder.active;
    pthread_mutex_unlock(&recorder.mutex);
    return active;
}
