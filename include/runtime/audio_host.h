#ifndef RV1106_AUDIO_HOST_H
#define RV1106_AUDIO_HOST_H

#include "quickjs.h"

int rv1106_audio_install(JSContext *ctx, JSValueConst global);
void rv1106_audio_shutdown(void);

#endif
