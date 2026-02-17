#include "packrat/build.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "manifest.h"

#define PR_CHUNK_COUNT_V0 2u
#define PR_PACKAGE_VERSION_MAJOR 1u
#define PR_PACKAGE_VERSION_MINOR 0u

#define PR_CHUNK_FORMAT_STRS "STRS"
#define PR_CHUNK_FORMAT_INDX "INDX"

#define PR_IMAGE_FORMAT_UNKNOWN 0u
#define PR_IMAGE_FORMAT_PNG 1u

typedef struct pr_build_result_storage {
    char package_path[PR_MANIFEST_PATH_MAX];
    char debug_output_path[PR_MANIFEST_PATH_MAX];
} pr_build_result_storage_t;

typedef struct pr_byte_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} pr_byte_buffer_t;

typedef struct pr_chunk_payload {
    char id[4];
    unsigned char *bytes;
    size_t size;
} pr_chunk_payload_t;

typedef struct pr_string_table {
    char **values;
    size_t count;
    size_t capacity;
} pr_string_table_t;

typedef struct pr_imported_image {
    char resolved_path[PR_MANIFEST_PATH_MAX];
    uint32_t width;
    uint32_t height;
    uint64_t source_bytes;
    uint32_t format;
} pr_imported_image_t;

typedef struct pr_index_maps {
    uint32_t *image_id_str_idx;
    uint32_t *image_path_str_idx;
    uint32_t *sprite_id_str_idx;
    uint32_t *animation_id_str_idx;
    uint32_t *sprite_source_image_idx;
    uint32_t *animation_sprite_idx;
} pr_index_maps_t;

static pr_build_result_storage_t PR_BUILD_RESULT_STORAGE;

static void pr_emit_diag(
    pr_diag_sink_fn sink,
    void *user_data,
    pr_diag_severity_t severity,
    const char *message,
    const char *file,
    const char *code,
    const char *asset_id
)
{
    pr_diagnostic_t diag;

    if (sink == NULL || message == NULL) {
        return;
    }

    memset(&diag, 0, sizeof(diag));
    diag.severity = severity;
    diag.message = message;
    diag.file = file;
    diag.code = code;
    diag.asset_id = asset_id;
    sink(&diag, user_data);
}

static int pr_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst == NULL || dst_size == 0u || src == NULL) {
        return 0;
    }

    len = strlen(src);
    if (len >= dst_size) {
        return 0;
    }

    memcpy(dst, src, len + 1u);
    return 1;
}

static int pr_is_path_separator(char ch)
{
    return (ch == '/' || ch == '\\') ? 1 : 0;
}

static int pr_is_absolute_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (pr_is_path_separator(path[0])) {
        return 1;
    }
    if (
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':'
    ) {
        return 1;
    }
    return 0;
}

static int pr_make_directory_if_missing(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 1;
    }

#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return 1;
    }
#else
    if (mkdir(path, 0755) == 0) {
        return 1;
    }
#endif

    return (errno == EEXIST) ? 1 : 0;
}

static int pr_ensure_parent_directories(const char *file_path)
{
    char working[PR_MANIFEST_PATH_MAX];
    size_t i;
    size_t len;

    if (file_path == NULL || file_path[0] == '\0') {
        return 0;
    }

    len = strlen(file_path);
    if (len >= sizeof(working)) {
        return 0;
    }
    memcpy(working, file_path, len + 1u);

    for (i = 1u; i < len; ++i) {
        char saved;

        if (!pr_is_path_separator(working[i])) {
            continue;
        }

        saved = working[i];
        working[i] = '\0';
        if (working[0] != '\0' && !pr_make_directory_if_missing(working)) {
            return 0;
        }
        working[i] = saved;
    }

    return 1;
}

static int pr_has_prpk_extension(const char *path)
{
    size_t len;

    if (path == NULL) {
        return 0;
    }

    len = strlen(path);
    if (len < 5u) {
        return 0;
    }
    return (strcmp(path + (len - 5u), ".prpk") == 0) ? 1 : 0;
}

static int pr_manifest_directory(
    const char *manifest_path,
    char *out_dir,
    size_t out_dir_size
)
{
    const char *last_sep;
    const char *cursor;
    size_t len;

    if (
        manifest_path == NULL ||
        out_dir == NULL ||
        out_dir_size == 0u
    ) {
        return 0;
    }

    last_sep = NULL;
    for (cursor = manifest_path; *cursor != '\0'; ++cursor) {
        if (pr_is_path_separator(*cursor)) {
            last_sep = cursor;
        }
    }

    if (last_sep == NULL) {
        return pr_copy_string(out_dir, out_dir_size, ".");
    }

    len = (size_t)(last_sep - manifest_path);
    if (len == 0u) {
        if (out_dir_size < 2u) {
            return 0;
        }
        out_dir[0] = '/';
        out_dir[1] = '\0';
        return 1;
    }
    if (len >= out_dir_size) {
        return 0;
    }

    memcpy(out_dir, manifest_path, len);
    out_dir[len] = '\0';
    return 1;
}

static int pr_join_paths(
    const char *base,
    const char *tail,
    char *out_path,
    size_t out_path_size
)
{
    size_t base_len;
    size_t tail_len;
    int need_sep;
    size_t needed;

    if (
        base == NULL ||
        tail == NULL ||
        out_path == NULL ||
        out_path_size == 0u
    ) {
        return 0;
    }

    base_len = strlen(base);
    tail_len = strlen(tail);
    need_sep = (base_len > 0u && !pr_is_path_separator(base[base_len - 1u])) ? 1 : 0;

    needed = base_len + (size_t)need_sep + tail_len + 1u;
    if (needed > out_path_size) {
        return 0;
    }

    memcpy(out_path, base, base_len);
    if (need_sep != 0) {
        out_path[base_len] = '/';
        memcpy(out_path + base_len + 1u, tail, tail_len + 1u);
    } else {
        memcpy(out_path + base_len, tail, tail_len + 1u);
    }
    return 1;
}

static int pr_resolve_image_path(
    const char *manifest_path,
    const char *image_path,
    char *out_path,
    size_t out_path_size
)
{
    char manifest_dir[PR_MANIFEST_PATH_MAX];

    if (
        manifest_path == NULL ||
        image_path == NULL ||
        out_path == NULL ||
        out_path_size == 0u
    ) {
        return 0;
    }

    if (pr_is_absolute_path(image_path)) {
        return pr_copy_string(out_path, out_path_size, image_path);
    }

    if (!pr_manifest_directory(manifest_path, manifest_dir, sizeof(manifest_dir))) {
        return 0;
    }
    return pr_join_paths(manifest_dir, image_path, out_path, out_path_size);
}

static unsigned char *pr_read_binary_file(const char *path, size_t *out_size)
{
    FILE *file;
    unsigned char *buffer;
    size_t read_size;
    long file_size;

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

    file_size = ftell(file);
    if (file_size < 0) {
        (void)fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }

    buffer = (unsigned char *)malloc((size_t)file_size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1u, (size_t)file_size, file);
    (void)fclose(file);
    if (read_size != (size_t)file_size) {
        free(buffer);
        return NULL;
    }

    *out_size = read_size;
    return buffer;
}

static uint32_t pr_read_u32_be(const unsigned char *bytes)
{
    if (bytes == NULL) {
        return 0u;
    }
    return (
        ((uint32_t)bytes[0] << 24u) |
        ((uint32_t)bytes[1] << 16u) |
        ((uint32_t)bytes[2] << 8u) |
        ((uint32_t)bytes[3])
    );
}

static int pr_parse_png_dimensions(
    const unsigned char *bytes,
    size_t size,
    uint32_t *out_width,
    uint32_t *out_height
)
{
    static const unsigned char PNG_SIGNATURE[8] = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
    };
    uint32_t ihdr_len;

    if (
        bytes == NULL ||
        size < 33u ||
        out_width == NULL ||
        out_height == NULL
    ) {
        return 0;
    }

    if (memcmp(bytes, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) != 0) {
        return 0;
    }
    if (memcmp(bytes + 12u, "IHDR", 4u) != 0) {
        return 0;
    }

    ihdr_len = pr_read_u32_be(bytes + 8u);
    if (ihdr_len < 13u) {
        return 0;
    }

    *out_width = pr_read_u32_be(bytes + 16u);
    *out_height = pr_read_u32_be(bytes + 20u);
    return (*out_width > 0u && *out_height > 0u) ? 1 : 0;
}

static void pr_imported_images_free(pr_imported_image_t *images, size_t count)
{
    (void)count;
    free(images);
}

static pr_status_t pr_import_manifest_images(
    const char *manifest_path,
    const pr_manifest_t *manifest,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_imported_image_t **out_images
)
{
    pr_imported_image_t *images;
    size_t i;
    int had_io_error;

    if (
        manifest_path == NULL ||
        manifest == NULL ||
        out_images == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_images = NULL;
    if (manifest->image_count == 0u) {
        return PR_STATUS_OK;
    }

    images = (pr_imported_image_t *)calloc(
        manifest->image_count,
        sizeof(images[0])
    );
    if (images == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    had_io_error = 0;
    for (i = 0u; i < manifest->image_count; ++i) {
        const pr_manifest_image_t *image;
        unsigned char *bytes;
        size_t byte_size;

        image = &manifest->images[i];
        if (image->has_path == 0 || image->path[0] == '\0') {
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Image path is missing during import stage.",
                manifest_path,
                "build.images.path_missing",
                image->id
            );
            continue;
        }

        if (!pr_resolve_image_path(
                manifest_path,
                image->path,
                images[i].resolved_path,
                sizeof(images[i].resolved_path)
            )) {
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Failed to resolve image path.",
                manifest_path,
                "build.images.path_resolve_failed",
                image->id
            );
            continue;
        }

        bytes = pr_read_binary_file(images[i].resolved_path, &byte_size);
        if (bytes == NULL) {
            had_io_error = 1;
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Failed to read image file.",
                images[i].resolved_path,
                "build.images.read_failed",
                image->id
            );
            continue;
        }

        if (!pr_parse_png_dimensions(
                bytes,
                byte_size,
                &images[i].width,
                &images[i].height
            )) {
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Unsupported image format or invalid PNG data.",
                images[i].resolved_path,
                "build.images.format_unsupported",
                image->id
            );
            free(bytes);
            continue;
        }

        images[i].format = PR_IMAGE_FORMAT_PNG;
        images[i].source_bytes = (uint64_t)byte_size;
        free(bytes);
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (images[i].format == PR_IMAGE_FORMAT_UNKNOWN) {
            pr_imported_images_free(images, manifest->image_count);
            return (had_io_error != 0) ? PR_STATUS_IO_ERROR : PR_STATUS_VALIDATION_ERROR;
        }
    }

    *out_images = images;
    return PR_STATUS_OK;
}

static void pr_byte_buffer_init(pr_byte_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    memset(buffer, 0, sizeof(*buffer));
}

static void pr_byte_buffer_free(pr_byte_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

static int pr_byte_buffer_reserve(pr_byte_buffer_t *buffer, size_t needed)
{
    unsigned char *grown;
    size_t new_capacity;

    if (buffer == NULL) {
        return 0;
    }
    if (needed <= buffer->capacity) {
        return 1;
    }

    new_capacity = (buffer->capacity == 0u) ? 64u : buffer->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2u;
    }

    grown = (unsigned char *)realloc(buffer->data, new_capacity);
    if (grown == NULL) {
        return 0;
    }

    buffer->data = grown;
    buffer->capacity = new_capacity;
    return 1;
}

static int pr_byte_buffer_append(
    pr_byte_buffer_t *buffer,
    const void *bytes,
    size_t byte_count
)
{
    if (buffer == NULL || bytes == NULL) {
        return 0;
    }
    if (!pr_byte_buffer_reserve(buffer, buffer->size + byte_count)) {
        return 0;
    }

    memcpy(buffer->data + buffer->size, bytes, byte_count);
    buffer->size += byte_count;
    return 1;
}

static int pr_byte_buffer_append_u16_le(pr_byte_buffer_t *buffer, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    return pr_byte_buffer_append(buffer, bytes, sizeof(bytes));
}

static int pr_byte_buffer_append_u32_le(pr_byte_buffer_t *buffer, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    bytes[2] = (unsigned char)((value >> 16u) & 0xFFu);
    bytes[3] = (unsigned char)((value >> 24u) & 0xFFu);
    return pr_byte_buffer_append(buffer, bytes, sizeof(bytes));
}

static int pr_byte_buffer_append_u64_le(pr_byte_buffer_t *buffer, uint64_t value)
{
    unsigned char bytes[8];
    size_t i;

    for (i = 0u; i < 8u; ++i) {
        bytes[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    }
    return pr_byte_buffer_append(buffer, bytes, sizeof(bytes));
}

static void pr_string_table_init(pr_string_table_t *table)
{
    if (table == NULL) {
        return;
    }
    memset(table, 0, sizeof(*table));
}

static void pr_string_table_free(pr_string_table_t *table)
{
    size_t i;

    if (table == NULL) {
        return;
    }

    for (i = 0u; i < table->count; ++i) {
        free(table->values[i]);
    }
    free(table->values);
    table->values = NULL;
    table->count = 0u;
    table->capacity = 0u;
}

static int pr_string_table_add(
    pr_string_table_t *table,
    const char *value,
    uint32_t *out_index
)
{
    size_t i;
    char *copied;

    if (table == NULL || value == NULL || out_index == NULL) {
        return 0;
    }

    for (i = 0u; i < table->count; ++i) {
        if (strcmp(table->values[i], value) == 0) {
            *out_index = (uint32_t)i;
            return 1;
        }
    }

    if (table->count + 1u > table->capacity) {
        size_t new_capacity;
        char **grown;

        new_capacity = (table->capacity == 0u) ? 16u : table->capacity * 2u;
        grown = (char **)realloc(table->values, new_capacity * sizeof(table->values[0]));
        if (grown == NULL) {
            return 0;
        }
        table->values = grown;
        table->capacity = new_capacity;
    }

    copied = (char *)malloc(strlen(value) + 1u);
    if (copied == NULL) {
        return 0;
    }
    memcpy(copied, value, strlen(value) + 1u);

    table->values[table->count] = copied;
    *out_index = (uint32_t)table->count;
    table->count += 1u;
    return 1;
}

static long pr_find_image_index(const pr_manifest_t *manifest, const char *image_id)
{
    size_t i;

    if (manifest == NULL || image_id == NULL) {
        return -1;
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (manifest->images[i].has_id == 0) {
            continue;
        }
        if (strcmp(manifest->images[i].id, image_id) == 0) {
            return (long)i;
        }
    }

    return -1;
}

static long pr_find_sprite_index(const pr_manifest_t *manifest, const char *sprite_id)
{
    size_t i;

    if (manifest == NULL || sprite_id == NULL) {
        return -1;
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        if (manifest->sprites[i].has_id == 0) {
            continue;
        }
        if (strcmp(manifest->sprites[i].id, sprite_id) == 0) {
            return (long)i;
        }
    }

    return -1;
}

static uint32_t pr_sprite_frame_count_hint(const pr_manifest_sprite_t *sprite)
{
    if (sprite == NULL) {
        return 0u;
    }

    switch (sprite->mode) {
    case PR_MANIFEST_SPRITE_MODE_SINGLE:
        return 1u;
    case PR_MANIFEST_SPRITE_MODE_RECTS:
        return (uint32_t)sprite->rect_count;
    case PR_MANIFEST_SPRITE_MODE_GRID:
        if (sprite->has_frame_count != 0 && sprite->frame_count > 0) {
            return (uint32_t)sprite->frame_count;
        }
        return 0u;
    default:
        return 0u;
    }
}

static uint32_t pr_animation_total_duration_ms(const pr_manifest_animation_t *animation)
{
    size_t i;
    uint64_t total;

    if (animation == NULL) {
        return 0u;
    }

    total = 0u;
    for (i = 0u; i < animation->frame_count; ++i) {
        if (animation->frames[i].ms > 0) {
            total += (uint64_t)animation->frames[i].ms;
        }
    }
    if (total > 0xFFFFFFFFu) {
        total = 0xFFFFFFFFu;
    }
    return (uint32_t)total;
}

static uint32_t pr_pivot_to_milli(double pivot)
{
    long value;

    if (pivot < 0.0) {
        pivot = 0.0;
    } else if (pivot > 1.0) {
        pivot = 1.0;
    }

    value = (long)(pivot * 1000.0 + 0.5);
    if (value < 0L) {
        value = 0L;
    }
    if (value > 1000L) {
        value = 1000L;
    }
    return (uint32_t)value;
}

static int pr_allocate_index_maps(
    const pr_manifest_t *manifest,
    pr_index_maps_t *maps
)
{
    if (manifest == NULL || maps == NULL) {
        return 0;
    }

    memset(maps, 0, sizeof(*maps));
    if (manifest->image_count > 0u) {
        maps->image_id_str_idx = (uint32_t *)calloc(manifest->image_count, sizeof(uint32_t));
        maps->image_path_str_idx = (uint32_t *)calloc(manifest->image_count, sizeof(uint32_t));
        if (maps->image_id_str_idx == NULL || maps->image_path_str_idx == NULL) {
            return 0;
        }
    }
    if (manifest->sprite_count > 0u) {
        maps->sprite_id_str_idx = (uint32_t *)calloc(manifest->sprite_count, sizeof(uint32_t));
        maps->sprite_source_image_idx = (uint32_t *)calloc(manifest->sprite_count, sizeof(uint32_t));
        if (maps->sprite_id_str_idx == NULL || maps->sprite_source_image_idx == NULL) {
            return 0;
        }
    }
    if (manifest->animation_count > 0u) {
        maps->animation_id_str_idx = (uint32_t *)calloc(manifest->animation_count, sizeof(uint32_t));
        maps->animation_sprite_idx = (uint32_t *)calloc(manifest->animation_count, sizeof(uint32_t));
        if (maps->animation_id_str_idx == NULL || maps->animation_sprite_idx == NULL) {
            return 0;
        }
    }

    return 1;
}

static void pr_free_index_maps(pr_index_maps_t *maps)
{
    if (maps == NULL) {
        return;
    }

    free(maps->image_id_str_idx);
    free(maps->image_path_str_idx);
    free(maps->sprite_id_str_idx);
    free(maps->animation_id_str_idx);
    free(maps->sprite_source_image_idx);
    free(maps->animation_sprite_idx);
    memset(maps, 0, sizeof(*maps));
}

static int pr_build_string_table_and_maps(
    const pr_manifest_t *manifest,
    const pr_imported_image_t *imported_images,
    pr_string_table_t *table,
    pr_index_maps_t *maps,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    size_t i;
    uint32_t ignored_index;

    if (
        manifest == NULL ||
        table == NULL ||
        maps == NULL ||
        (manifest->image_count > 0u && imported_images == NULL)
    ) {
        return 0;
    }

    if (!pr_string_table_add(table, manifest->package_name, &ignored_index)) {
        return 0;
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (!pr_string_table_add(table, manifest->images[i].id, &maps->image_id_str_idx[i])) {
            return 0;
        }
        if (!pr_string_table_add(
                table,
                imported_images[i].resolved_path,
                &maps->image_path_str_idx[i]
            )) {
            return 0;
        }
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        long source_image_index;

        if (!pr_string_table_add(table, manifest->sprites[i].id, &maps->sprite_id_str_idx[i])) {
            return 0;
        }
        source_image_index = pr_find_image_index(manifest, manifest->sprites[i].source);
        if (source_image_index < 0) {
            maps->sprite_source_image_idx[i] = 0xFFFFFFFFu;
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Sprite source image id was not found during index mapping.",
                NULL,
                "build.index.sprite_source_missing",
                manifest->sprites[i].id
            );
            return 0;
        }
        maps->sprite_source_image_idx[i] = (uint32_t)source_image_index;
    }

    for (i = 0u; i < manifest->animation_count; ++i) {
        long sprite_index;

        if (!pr_string_table_add(
                table,
                manifest->animations[i].id,
                &maps->animation_id_str_idx[i]
            )) {
            return 0;
        }
        sprite_index = pr_find_sprite_index(manifest, manifest->animations[i].sprite);
        if (sprite_index < 0) {
            maps->animation_sprite_idx[i] = 0xFFFFFFFFu;
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Animation sprite id was not found during index mapping.",
                NULL,
                "build.index.animation_sprite_missing",
                manifest->animations[i].id
            );
            return 0;
        }
        maps->animation_sprite_idx[i] = (uint32_t)sprite_index;
    }

    return 1;
}

static int pr_build_chunk_strs(
    const pr_string_table_t *table,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    size_t i;
    size_t strings_blob_bytes;
    uint32_t running_offset;

    if (table == NULL || chunk == NULL) {
        return 0;
    }

    strings_blob_bytes = 0u;
    for (i = 0u; i < table->count; ++i) {
        strings_blob_bytes += strlen(table->values[i]) + 1u;
    }
    if (strings_blob_bytes > 0xFFFFFFFFu) {
        return 0;
    }

    pr_byte_buffer_init(&buffer);
    if (
        !pr_byte_buffer_append_u32_le(&buffer, 1u) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)table->count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)strings_blob_bytes)
    ) {
        pr_byte_buffer_free(&buffer);
        return 0;
    }

    running_offset = 0u;
    for (i = 0u; i < table->count; ++i) {
        if (!pr_byte_buffer_append_u32_le(&buffer, running_offset)) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
        running_offset += (uint32_t)(strlen(table->values[i]) + 1u);
    }

    for (i = 0u; i < table->count; ++i) {
        size_t len;

        len = strlen(table->values[i]) + 1u;
        if (!pr_byte_buffer_append(&buffer, table->values[i], len)) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    memcpy(chunk->id, PR_CHUNK_FORMAT_STRS, 4u);
    chunk->bytes = buffer.data;
    chunk->size = buffer.size;
    return 1;
}

static int pr_build_chunk_indx(
    const pr_manifest_t *manifest,
    const pr_imported_image_t *images,
    const pr_index_maps_t *maps,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    size_t i;

    if (
        manifest == NULL ||
        images == NULL ||
        maps == NULL ||
        chunk == NULL
    ) {
        return 0;
    }

    pr_byte_buffer_init(&buffer);
    if (
        !pr_byte_buffer_append_u32_le(&buffer, 1u) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->image_count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->sprite_count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->animation_count)
    ) {
        pr_byte_buffer_free(&buffer);
        return 0;
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, maps->image_id_str_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, maps->image_path_str_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, images[i].width) ||
            !pr_byte_buffer_append_u32_le(&buffer, images[i].height) ||
            !pr_byte_buffer_append_u32_le(&buffer, images[i].format)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        const pr_manifest_sprite_t *sprite;

        sprite = &manifest->sprites[i];
        if (
            !pr_byte_buffer_append_u32_le(&buffer, maps->sprite_id_str_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, maps->sprite_source_image_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)sprite->mode) ||
            !pr_byte_buffer_append_u32_le(&buffer, pr_sprite_frame_count_hint(sprite)) ||
            !pr_byte_buffer_append_u32_le(&buffer, pr_pivot_to_milli(sprite->pivot_x)) ||
            !pr_byte_buffer_append_u32_le(&buffer, pr_pivot_to_milli(sprite->pivot_y))
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    for (i = 0u; i < manifest->animation_count; ++i) {
        const pr_manifest_animation_t *animation;

        animation = &manifest->animations[i];
        if (
            !pr_byte_buffer_append_u32_le(&buffer, maps->animation_id_str_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, maps->animation_sprite_idx[i]) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)animation->loop_mode) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)animation->frame_count) ||
            !pr_byte_buffer_append_u32_le(&buffer, pr_animation_total_duration_ms(animation))
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    memcpy(chunk->id, PR_CHUNK_FORMAT_INDX, 4u);
    chunk->bytes = buffer.data;
    chunk->size = buffer.size;
    return 1;
}

static void pr_chunk_payload_free(pr_chunk_payload_t *chunk)
{
    if (chunk == NULL) {
        return;
    }
    free(chunk->bytes);
    chunk->bytes = NULL;
    chunk->size = 0u;
}

static int pr_write_u16_le(FILE *file, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static int pr_write_u32_le(FILE *file, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    bytes[2] = (unsigned char)((value >> 16u) & 0xFFu);
    bytes[3] = (unsigned char)((value >> 24u) & 0xFFu);
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static int pr_write_u64_le(FILE *file, uint64_t value)
{
    unsigned char bytes[8];
    size_t i;

    for (i = 0u; i < 8u; ++i) {
        bytes[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    }
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static pr_status_t pr_write_package_with_chunks(
    const char *output_path,
    const pr_chunk_payload_t *chunks,
    size_t chunk_count,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    FILE *file;
    uint32_t header_size;
    uint64_t chunk_table_offset;
    uint64_t payload_offset;
    size_t i;

    if (
        output_path == NULL ||
        output_path[0] == '\0' ||
        chunks == NULL ||
        chunk_count == 0u
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (!pr_ensure_parent_directories(output_path)) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to create output directory path.",
            output_path,
            "build.output_dir_create_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    file = fopen(output_path, "wb");
    if (file == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to open output package file.",
            output_path,
            "build.output_open_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    header_size = 24u;
    chunk_table_offset = (uint64_t)header_size;
    payload_offset = chunk_table_offset + (uint64_t)chunk_count * 20u;

    if (
        fwrite("PRPK", 1u, 4u, file) != 4u ||
        !pr_write_u16_le(file, PR_PACKAGE_VERSION_MAJOR) ||
        !pr_write_u16_le(file, PR_PACKAGE_VERSION_MINOR) ||
        !pr_write_u32_le(file, header_size) ||
        !pr_write_u32_le(file, (uint32_t)chunk_count) ||
        !pr_write_u64_le(file, chunk_table_offset)
    ) {
        (void)fclose(file);
        return PR_STATUS_IO_ERROR;
    }

    for (i = 0u; i < chunk_count; ++i) {
        if (
            fwrite(chunks[i].id, 1u, 4u, file) != 4u ||
            !pr_write_u64_le(file, payload_offset) ||
            !pr_write_u64_le(file, (uint64_t)chunks[i].size)
        ) {
            (void)fclose(file);
            return PR_STATUS_IO_ERROR;
        }
        payload_offset += (uint64_t)chunks[i].size;
    }

    for (i = 0u; i < chunk_count; ++i) {
        if (
            chunks[i].size > 0u &&
            fwrite(chunks[i].bytes, 1u, chunks[i].size, file) != chunks[i].size
        ) {
            (void)fclose(file);
            return PR_STATUS_IO_ERROR;
        }
    }

    if (fclose(file) != 0) {
        return PR_STATUS_IO_ERROR;
    }
    return PR_STATUS_OK;
}

static void pr_write_json_escaped(FILE *file, const char *text)
{
    const char *cursor;

    if (file == NULL || text == NULL) {
        return;
    }

    for (cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
            (void)fputs("\\\\", file);
            break;
        case '"':
            (void)fputs("\\\"", file);
            break;
        case '\n':
            (void)fputs("\\n", file);
            break;
        case '\r':
            (void)fputs("\\r", file);
            break;
        case '\t':
            (void)fputs("\\t", file);
            break;
        default:
            (void)fputc(*cursor, file);
            break;
        }
    }
}

static const char *pr_image_format_name(uint32_t format)
{
    switch (format) {
    case PR_IMAGE_FORMAT_PNG:
        return "png";
    default:
        return "unknown";
    }
}

static pr_status_t pr_write_debug_json(
    const char *debug_path,
    const pr_manifest_t *manifest,
    const pr_imported_image_t *images,
    const char *resolved_output_path,
    int pretty_json,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    FILE *file;
    size_t i;

    if (
        debug_path == NULL ||
        debug_path[0] == '\0' ||
        manifest == NULL ||
        resolved_output_path == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (!pr_ensure_parent_directories(debug_path)) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to create debug output directory path.",
            debug_path,
            "build.debug_dir_create_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    file = fopen(debug_path, "wb");
    if (file == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to open debug output file.",
            debug_path,
            "build.debug_open_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    if (pretty_json != 0) {
        (void)fputs("{\n", file);
        (void)fprintf(file, "  \"schema_version\": %d,\n", manifest->schema_version);
        (void)fputs("  \"package_name\": \"", file);
        pr_write_json_escaped(file, manifest->package_name);
        (void)fputs("\",\n", file);
        (void)fputs("  \"output\": \"", file);
        pr_write_json_escaped(file, resolved_output_path);
        (void)fputs("\",\n", file);
        (void)fputs("  \"counts\": {\n", file);
        (void)fprintf(file, "    \"images\": %u,\n", (unsigned int)manifest->image_count);
        (void)fprintf(file, "    \"sprites\": %u,\n", (unsigned int)manifest->sprite_count);
        (void)fprintf(file, "    \"animations\": %u\n", (unsigned int)manifest->animation_count);
        (void)fputs("  },\n", file);
        (void)fputs("  \"images\": [\n", file);

        for (i = 0u; i < manifest->image_count; ++i) {
            (void)fputs("    {\n", file);
            (void)fputs("      \"id\": \"", file);
            pr_write_json_escaped(file, manifest->images[i].id);
            (void)fputs("\",\n", file);
            (void)fputs("      \"resolved_path\": \"", file);
            pr_write_json_escaped(file, images[i].resolved_path);
            (void)fputs("\",\n", file);
            (void)fprintf(file, "      \"width\": %u,\n", images[i].width);
            (void)fprintf(file, "      \"height\": %u,\n", images[i].height);
            (void)fprintf(file, "      \"bytes\": %llu,\n", (unsigned long long)images[i].source_bytes);
            (void)fputs("      \"format\": \"", file);
            pr_write_json_escaped(file, pr_image_format_name(images[i].format));
            (void)fputs("\"\n", file);
            (void)fputs((i + 1u < manifest->image_count) ? "    },\n" : "    }\n", file);
        }
        (void)fputs("  ]\n", file);
        (void)fputs("}\n", file);
    } else {
        (void)fputs("{\"schema_version\":", file);
        (void)fprintf(file, "%d", manifest->schema_version);
        (void)fputs(",\"package_name\":\"", file);
        pr_write_json_escaped(file, manifest->package_name);
        (void)fputs("\",\"output\":\"", file);
        pr_write_json_escaped(file, resolved_output_path);
        (void)fputs("\",\"counts\":{\"images\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->image_count);
        (void)fputs(",\"sprites\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->sprite_count);
        (void)fputs(",\"animations\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->animation_count);
        (void)fputs("},\"images\":[", file);
        for (i = 0u; i < manifest->image_count; ++i) {
            (void)fputs("{\"id\":\"", file);
            pr_write_json_escaped(file, manifest->images[i].id);
            (void)fputs("\",\"resolved_path\":\"", file);
            pr_write_json_escaped(file, images[i].resolved_path);
            (void)fprintf(
                file,
                "\",\"width\":%u,\"height\":%u,\"bytes\":%llu,\"format\":\"%s\"}",
                images[i].width,
                images[i].height,
                (unsigned long long)images[i].source_bytes,
                pr_image_format_name(images[i].format)
            );
            if (i + 1u < manifest->image_count) {
                (void)fputs(",", file);
            }
        }
        (void)fputs("]}\n", file);
    }

    if (fclose(file) != 0) {
        return PR_STATUS_IO_ERROR;
    }
    return PR_STATUS_OK;
}

pr_status_t pr_validate_manifest_file(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    pr_manifest_t manifest;
    pr_status_t status;
    int error_count;
    int warning_count;

    pr_manifest_init(&manifest);
    status = pr_manifest_load_and_validate(
        manifest_path,
        diag_sink,
        diag_user_data,
        &manifest,
        &error_count,
        &warning_count
    );
    if (status != PR_STATUS_OK) {
        return status;
    }

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_NOTE,
        "Manifest validated successfully.",
        manifest_path,
        "manifest.valid",
        NULL
    );

    pr_manifest_free(&manifest);
    return PR_STATUS_OK;
}

pr_status_t pr_build_package(
    const pr_build_options_t *options,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_build_result_t *out_result
)
{
    pr_manifest_t manifest;
    pr_imported_image_t *images;
    pr_string_table_t strings;
    pr_index_maps_t maps;
    pr_chunk_payload_t chunks[PR_CHUNK_COUNT_V0];
    pr_status_t status;
    const char *output_path;
    const char *debug_output_path;
    int validation_errors;
    int validation_warnings;
    int warning_count;
    int i;

    if (
        options == NULL ||
        options->manifest_path == NULL ||
        out_result == NULL
    ) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Build options and output result are required.",
            NULL,
            "build.invalid_arguments",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    memset(out_result, 0, sizeof(*out_result));
    memset(&PR_BUILD_RESULT_STORAGE, 0, sizeof(PR_BUILD_RESULT_STORAGE));
    pr_manifest_init(&manifest);
    images = NULL;
    pr_string_table_init(&strings);
    memset(&maps, 0, sizeof(maps));
    memset(chunks, 0, sizeof(chunks));

    status = pr_manifest_load_and_validate(
        options->manifest_path,
        diag_sink,
        diag_user_data,
        &manifest,
        &validation_errors,
        &validation_warnings
    );
    if (status != PR_STATUS_OK) {
        return status;
    }
    (void)validation_errors;
    warning_count = validation_warnings;

    output_path = (
        options->output_override != NULL &&
        options->output_override[0] != '\0'
    ) ? options->output_override : manifest.output;
    if (
        output_path == NULL ||
        output_path[0] == '\0' ||
        !pr_copy_string(
            PR_BUILD_RESULT_STORAGE.package_path,
            sizeof(PR_BUILD_RESULT_STORAGE.package_path),
            output_path
        )
    ) {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Invalid or too-long package output path.",
            options->manifest_path,
            "build.output_invalid",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (!pr_has_prpk_extension(PR_BUILD_RESULT_STORAGE.package_path)) {
        warning_count += 1;
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_WARNING,
            "Resolved output path does not use .prpk extension.",
            PR_BUILD_RESULT_STORAGE.package_path,
            "build.output_extension",
            NULL
        );
    }

    debug_output_path = (
        options->debug_output_override != NULL &&
        options->debug_output_override[0] != '\0'
    ) ? options->debug_output_override :
        ((manifest.has_debug_output != 0) ? manifest.debug_output : NULL);
    if (
        debug_output_path != NULL &&
        debug_output_path[0] != '\0' &&
        !pr_copy_string(
            PR_BUILD_RESULT_STORAGE.debug_output_path,
            sizeof(PR_BUILD_RESULT_STORAGE.debug_output_path),
            debug_output_path
        )
    ) {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Debug output path exceeds max path length.",
            options->manifest_path,
            "build.debug_output_path_too_long",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    status = pr_import_manifest_images(
        options->manifest_path,
        &manifest,
        diag_sink,
        diag_user_data,
        &images
    );
    if (status != PR_STATUS_OK) {
        pr_manifest_free(&manifest);
        return status;
    }

    if (options->strict_mode != 0 && warning_count > 0) {
        pr_imported_images_free(images, manifest.image_count);
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Strict mode failed: warnings were emitted.",
            options->manifest_path,
            "build.strict_warnings",
            NULL
        );
        return PR_STATUS_VALIDATION_ERROR;
    }

    if (!pr_allocate_index_maps(&manifest, &maps)) {
        pr_imported_images_free(images, manifest.image_count);
        pr_manifest_free(&manifest);
        return PR_STATUS_ALLOCATION_FAILED;
    }

    if (!pr_build_string_table_and_maps(
            &manifest,
            images,
            &strings,
            &maps,
            diag_sink,
            diag_user_data
        )) {
        pr_free_index_maps(&maps);
        pr_string_table_free(&strings);
        pr_imported_images_free(images, manifest.image_count);
        pr_manifest_free(&manifest);
        return PR_STATUS_INTERNAL_ERROR;
    }

    if (
        !pr_build_chunk_strs(&strings, &chunks[0]) ||
        !pr_build_chunk_indx(&manifest, images, &maps, &chunks[1])
    ) {
        for (i = 0; i < (int)PR_CHUNK_COUNT_V0; ++i) {
            pr_chunk_payload_free(&chunks[i]);
        }
        pr_free_index_maps(&maps);
        pr_string_table_free(&strings);
        pr_imported_images_free(images, manifest.image_count);
        pr_manifest_free(&manifest);
        return PR_STATUS_ALLOCATION_FAILED;
    }

    status = pr_write_package_with_chunks(
        PR_BUILD_RESULT_STORAGE.package_path,
        chunks,
        PR_CHUNK_COUNT_V0,
        diag_sink,
        diag_user_data
    );
    for (i = 0; i < (int)PR_CHUNK_COUNT_V0; ++i) {
        pr_chunk_payload_free(&chunks[i]);
    }
    if (status != PR_STATUS_OK) {
        pr_free_index_maps(&maps);
        pr_string_table_free(&strings);
        pr_imported_images_free(images, manifest.image_count);
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to write package output.",
            PR_BUILD_RESULT_STORAGE.package_path,
            "build.output_write_failed",
            NULL
        );
        return status;
    }

    if (PR_BUILD_RESULT_STORAGE.debug_output_path[0] != '\0') {
        status = pr_write_debug_json(
            PR_BUILD_RESULT_STORAGE.debug_output_path,
            &manifest,
            images,
            PR_BUILD_RESULT_STORAGE.package_path,
            (options->pretty_debug_json != 0 || manifest.pretty_debug_json != 0) ? 1 : 0,
            diag_sink,
            diag_user_data
        );
        if (status != PR_STATUS_OK) {
            pr_free_index_maps(&maps);
            pr_string_table_free(&strings);
            pr_imported_images_free(images, manifest.image_count);
            pr_manifest_free(&manifest);
            return status;
        }
    }

    out_result->package_path = PR_BUILD_RESULT_STORAGE.package_path;
    out_result->debug_output_path = (
        PR_BUILD_RESULT_STORAGE.debug_output_path[0] != '\0'
    ) ? PR_BUILD_RESULT_STORAGE.debug_output_path : NULL;
    out_result->atlas_page_count = 0u;
    out_result->sprite_count = (unsigned int)manifest.sprite_count;
    out_result->animation_count = (unsigned int)manifest.animation_count;

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_NOTE,
        "Wrote .prpk package with STRS and INDX chunks.",
        PR_BUILD_RESULT_STORAGE.package_path,
        "build.package_written",
        NULL
    );

    pr_free_index_maps(&maps);
    pr_string_table_free(&strings);
    pr_imported_images_free(images, manifest.image_count);
    pr_manifest_free(&manifest);
    return PR_STATUS_OK;
}

