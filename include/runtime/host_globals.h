#ifndef RV1106_HOST_GLOBALS_H
#define RV1106_HOST_GLOBALS_H

#include "quickjs.h"

int rv1106_install_globals(JSContext *ctx, JSValueConst global);
void rv1106_shutdown_globals(void);

#endif
