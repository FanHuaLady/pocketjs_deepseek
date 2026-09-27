#ifndef RV1106_PET_HOST_H
#define RV1106_PET_HOST_H

#include "quickjs.h"
#include "runtime/pet_config.h"

#include <stddef.h>
#include <stdint.h>

int pet_control_start(unsigned port);
void pet_control_stop(void);
size_t pet_control_poll(char *out, size_t capacity);
void pet_control_set_animation_config(const PetAnimationConfig *config);
void pet_control_push_command(const char *command);
int pet_control_queue_voice(uint8_t *data, size_t length, unsigned rate, unsigned channels);
int rv1106_pet_install(JSContext *ctx, JSValueConst global);

#endif
