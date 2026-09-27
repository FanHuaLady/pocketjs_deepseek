#include "runtime/host_globals.h"

#include "runtime/audio_host.h"
#include "runtime/pet_host.h"

int rv1106_install_globals(JSContext *ctx, JSValueConst global)
{
    return rv1106_pet_install(ctx, global) &&
           rv1106_audio_install(ctx, global);
}

void rv1106_shutdown_globals(void)
{
    rv1106_audio_shutdown();
}
