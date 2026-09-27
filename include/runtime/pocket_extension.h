#ifndef RV1106_POCKET_EXTENSION_H
#define RV1106_POCKET_EXTENSION_H

#include "quickjs.h"

typedef int (*PocketRuntimeGlobalInstaller)(JSContext *ctx, JSValueConst global);

void pocket_runtime_set_global_installer(PocketRuntimeGlobalInstaller installer);

#endif
