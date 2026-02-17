#include "packrat/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pr_package {
    unsigned char *owned_bytes;
    const unsigned char *bytes;
    size_t size;
};

static unsigned char *pr_read_binary_file(const char *path, size_t *out_size)
{
    FILE *file;
    unsigned char *buffer;
    size_t read_size;
    long size;

    if (path == NULL || out_size == NULL) {
        return NULL;
    }

    *out_size = 0u;
    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        (void)fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }

    buffer = (unsigned char *)malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1u, (size_t)size, file);
    (void)fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    *out_size = read_size;
    return buffer;
}

pr_status_t pr_package_open_file(const char *path, pr_package_t **out_package)
{
    pr_package_t *package;
    size_t size;
    unsigned char *bytes;

    if (path == NULL || out_package == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_package = NULL;
    bytes = pr_read_binary_file(path, &size);
    if (bytes == NULL) {
        return PR_STATUS_IO_ERROR;
    }

    package = (pr_package_t *)calloc(1u, sizeof(*package));
    if (package == NULL) {
        free(bytes);
        return PR_STATUS_ALLOCATION_FAILED;
    }

    package->owned_bytes = bytes;
    package->bytes = bytes;
    package->size = size;
    *out_package = package;
    return PR_STATUS_OK;
}

pr_status_t pr_package_open_memory(
    const void *data,
    size_t size,
    pr_package_t **out_package
)
{
    pr_package_t *package;
    unsigned char *copy;

    if (data == NULL || size == 0u || out_package == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_package = NULL;
    package = (pr_package_t *)calloc(1u, sizeof(*package));
    if (package == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    copy = (unsigned char *)malloc(size);
    if (copy == NULL) {
        free(package);
        return PR_STATUS_ALLOCATION_FAILED;
    }

    memcpy(copy, data, size);
    package->owned_bytes = copy;
    package->bytes = copy;
    package->size = size;

    *out_package = package;
    return PR_STATUS_OK;
}

void pr_package_close(pr_package_t *package)
{
    if (package == NULL) {
        return;
    }

    free(package->owned_bytes);
    package->owned_bytes = NULL;
    package->bytes = NULL;
    package->size = 0u;
    free(package);
}

const pr_sprite_t *pr_package_find_sprite(
    const pr_package_t *package,
    const char *sprite_id
)
{
    (void)package;
    (void)sprite_id;
    return NULL;
}

const pr_animation_t *pr_package_find_animation(
    const pr_package_t *package,
    const char *animation_id
)
{
    (void)package;
    (void)animation_id;
    return NULL;
}

