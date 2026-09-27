#ifndef RV1106_PET_CONFIG_H
#define RV1106_PET_CONFIG_H

#include <stddef.h>

typedef struct {
    unsigned long idle_seconds;
    double response_multiplier;
} PetAnimationConfig;

void pet_config_defaults(PetAnimationConfig *config);
int pet_config_load(const char *path, PetAnimationConfig *config);
const char *pet_config_path(void);

#endif
