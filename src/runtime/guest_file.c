#include "runtime/guest_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint8_t *guest_file_read(const char *path, size_t *length)
{
    FILE *file;
    long size;
    uint8_t *data;
    size_t read_length;

    if (path == NULL || length == NULL) {
        fprintf(stderr, "guest file path and length are required\n");
        return NULL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cannot open guest file %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    if ((uintmax_t)size > SIZE_MAX - 1U) {
        fprintf(stderr, "guest file %s is too large\n", path);
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)size + 1U);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    read_length = fread(data, 1, (size_t)size, file);
    if (read_length != (size_t)size || ferror(file)) {
        fprintf(stderr, "cannot read guest file %s\n", path);
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    data[read_length] = 0;
    *length = read_length;
    return data;
}
