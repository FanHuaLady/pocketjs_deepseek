#ifndef RV1106_PET_HOST_H
#define RV1106_PET_HOST_H

#include "quickjs.h"
#include "runtime/pet_config.h"

#include <stddef.h>

int pet_control_start(unsigned port);
void pet_control_stop(void);
size_t pet_control_poll(char *out, size_t capacity);
void pet_control_set_animation_config(const PetAnimationConfig *config);
int rv1106_pet_install(JSContext *ctx, JSValueConst global);

#endif
