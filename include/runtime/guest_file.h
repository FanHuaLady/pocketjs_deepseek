#ifndef POCKETJS_RV1106_GUEST_FILE_H
#define POCKETJS_RV1106_GUEST_FILE_H

#include <stddef.h>
#include <stdint.h>

uint8_t *guest_file_read(const char *path, size_t *length);

#endif
