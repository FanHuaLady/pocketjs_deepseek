#ifndef POCKETJS_RV1106_MIC_HOST_H
#define POCKETJS_RV1106_MIC_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct rv1106_mic_capture {
    uint8_t *data;
    size_t length;
    unsigned sample_rate;
    unsigned channels;
} Rv1106MicCapture;

int rv1106_mic_capture_pcm(
    unsigned sample_rate,
    unsigned channels,
    unsigned seconds,
    Rv1106MicCapture *out
);

void rv1106_mic_capture_free(Rv1106MicCapture *capture);

int rv1106_mic_record_start(unsigned sample_rate, unsigned channels, unsigned max_seconds);
int rv1106_mic_record_stop(Rv1106MicCapture *out);
int rv1106_mic_recording(void);

#endif
