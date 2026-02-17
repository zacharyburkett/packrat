#include "packrat/build.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <png.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "manifest.h"

#define PR_CHUNK_COUNT_V0 5u
#define PR_PACKAGE_VERSION_MAJOR 1u
#define PR_PACKAGE_VERSION_MINOR 0u

#define PR_CHUNK_FORMAT_STRS "STRS"
#define PR_CHUNK_FORMAT_TXTR "TXTR"
#define PR_CHUNK_FORMAT_SPRT "SPRT"
#define PR_CHUNK_FORMAT_ANIM "ANIM"
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
    uint32_t row_bytes;
    size_t pixel_bytes;
    unsigned char *pixels;
} pr_imported_image_t;

typedef struct pr_index_maps {
    uint32_t *image_id_str_idx;
    uint32_t *image_path_str_idx;
    uint32_t *sprite_id_str_idx;
    uint32_t *animation_id_str_idx;
    uint32_t *sprite_source_image_idx;
    uint32_t *animation_sprite_idx;
} pr_index_maps_t;

typedef struct pr_resolved_sprite {
    uint32_t source_image_index;
    uint32_t name_str_idx;
    uint32_t mode;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t pivot_x_milli;
    uint32_t pivot_y_milli;
} pr_resolved_sprite_t;

typedef struct pr_resolved_frame {
    uint32_t sprite_index;
    uint32_t local_frame_index;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_w;
    uint32_t source_h;
    uint32_t atlas_page;
    uint32_t atlas_x;
    uint32_t atlas_y;
    uint32_t atlas_w;
    uint32_t atlas_h;
    uint32_t u0_milli;
    uint32_t v0_milli;
    uint32_t u1_milli;
    uint32_t v1_milli;
} pr_resolved_frame_t;

typedef struct pr_pack_page {
    uint32_t max_w;
    uint32_t max_h;
    uint32_t used_w;
    uint32_t used_h;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t shelf_h;
    uint32_t final_w;
    uint32_t final_h;
} pr_pack_page_t;

typedef struct pr_resolved_animation {
    uint32_t name_str_idx;
    uint32_t sprite_index;
    uint32_t loop_mode;
    uint32_t key_start;
    uint32_t key_count;
    uint32_t total_duration_ms;
} pr_resolved_animation_t;

typedef struct pr_resolved_animation_key {
    uint32_t animation_index;
    uint32_t frame_index;
    uint32_t duration_ms;
} pr_resolved_animation_key_t;

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

static int pr_mul_size(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return 0;
    }
    if (a != 0u && b > (SIZE_MAX / a)) {
        return 0;
    }
    *out = a * b;
    return 1;
}

static int pr_decode_png_rgba_file(
    const char *path,
    uint32_t *out_width,
    uint32_t *out_height,
    uint32_t *out_row_bytes,
    unsigned char **out_pixels,
    size_t *out_pixel_bytes
)
{
    FILE *file;
    png_structp png_ptr;
    png_infop info_ptr;
    png_infop end_info_ptr;
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    int interlace_type;
    png_size_t row_bytes;
    png_bytep *rows = NULL;
    size_t rows_bytes;
    size_t pixel_bytes;
    unsigned char *pixels = NULL;
    uint32_t y;
    unsigned char signature[8];

    if (
        path == NULL ||
        out_width == NULL ||
        out_height == NULL ||
        out_row_bytes == NULL ||
        out_pixels == NULL ||
        out_pixel_bytes == NULL
    ) {
        return 0;
    }

    *out_pixels = NULL;
    *out_pixel_bytes = 0u;
    *out_width = 0u;
    *out_height = 0u;
    *out_row_bytes = 0u;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fread(signature, 1u, sizeof(signature), file) != sizeof(signature)) {
        (void)fclose(file);
        return 0;
    }
    if (png_sig_cmp((png_const_bytep)signature, 0, sizeof(signature)) != 0) {
        (void)fclose(file);
        return 0;
    }

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
        return 0;
    }
    info_ptr = png_create_info_struct(png_ptr);
    end_info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL || end_info_ptr == NULL) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(rows);
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    png_init_io(png_ptr, file);
    png_set_sig_bytes(png_ptr, (int)sizeof(signature));

    png_read_info(png_ptr, info_ptr);
    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    interlace_type = png_get_interlace_type(png_ptr, info_ptr);

    if (width == 0u || height == 0u || width > UINT32_MAX || height > UINT32_MAX) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
#if PNG_LIBPNG_VER >= 10209
        png_set_expand_gray_1_2_4_to_8(png_ptr);
#else
        png_set_gray_1_2_4_to_8(png_ptr);
#endif
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0) {
        png_set_filler(png_ptr, 0xFFu, PNG_FILLER_AFTER);
    }

    if (interlace_type != PNG_INTERLACE_NONE) {
        (void)png_set_interlace_handling(png_ptr);
    }
    png_read_update_info(png_ptr, info_ptr);

    row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    if (row_bytes == 0u || row_bytes > UINT32_MAX) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }
    if (!pr_mul_size((size_t)row_bytes, (size_t)height, &pixel_bytes)) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }

    pixels = (unsigned char *)malloc(pixel_bytes);
    if (pixels == NULL) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }

    if (!pr_mul_size((size_t)height, sizeof(rows[0]), &rows_bytes)) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }
    rows = (png_bytep *)malloc(rows_bytes);
    if (rows == NULL) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        return 0;
    }
    for (y = 0u; y < (uint32_t)height; ++y) {
        rows[y] = pixels + (size_t)y * (size_t)row_bytes;
    }

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, end_info_ptr);

    free(rows);
    (void)fclose(file);
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);

    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    *out_row_bytes = (uint32_t)row_bytes;
    *out_pixels = pixels;
    *out_pixel_bytes = pixel_bytes;
    return 1;
}

static void pr_imported_images_free(pr_imported_image_t *images, size_t count)
{
    size_t i;

    if (images == NULL) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        free(images[i].pixels);
        images[i].pixels = NULL;
        images[i].pixel_bytes = 0u;
    }
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

        free(bytes);
        bytes = NULL;

        if (!pr_decode_png_rgba_file(
                images[i].resolved_path,
                &images[i].width,
                &images[i].height,
                &images[i].row_bytes,
                &images[i].pixels,
                &images[i].pixel_bytes
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
            continue;
        }

        images[i].format = PR_IMAGE_FORMAT_PNG;
        images[i].source_bytes = (uint64_t)byte_size;
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

static int pr_reserve_array(
    void **buffer,
    size_t *capacity,
    size_t needed_count,
    size_t element_size
)
{
    size_t new_capacity;
    void *grown;

    if (
        buffer == NULL ||
        capacity == NULL ||
        element_size == 0u
    ) {
        return 0;
    }
    if (needed_count <= *capacity) {
        return 1;
    }

    new_capacity = (*capacity == 0u) ? 16u : *capacity;
    while (new_capacity < needed_count) {
        new_capacity *= 2u;
    }

    grown = realloc(*buffer, new_capacity * element_size);
    if (grown == NULL) {
        return 0;
    }

    *buffer = grown;
    *capacity = new_capacity;
    return 1;
}

static uint32_t pr_round_up_pow2_u32(uint32_t value)
{
    uint32_t v;

    if (value <= 1u) {
        return 1u;
    }

    v = value - 1u;
    v |= v >> 1u;
    v |= v >> 2u;
    v |= v >> 4u;
    v |= v >> 8u;
    v |= v >> 16u;
    return v + 1u;
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

static int pr_append_resolved_frame(
    pr_resolved_frame_t **frames,
    size_t *frame_count,
    size_t *frame_capacity,
    const pr_resolved_frame_t *frame
)
{
    if (
        frames == NULL ||
        frame_count == NULL ||
        frame_capacity == NULL ||
        frame == NULL
    ) {
        return 0;
    }

    if (!pr_reserve_array(
            (void **)frames,
            frame_capacity,
            *frame_count + 1u,
            sizeof((*frames)[0])
        )) {
        return 0;
    }

    (*frames)[*frame_count] = *frame;
    *frame_count += 1u;
    return 1;
}

static pr_status_t pr_resolve_sprite_frames(
    const pr_manifest_t *manifest,
    const pr_imported_image_t *images,
    const pr_index_maps_t *maps,
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_resolved_sprite_t **out_sprites,
    size_t *out_sprite_count,
    pr_resolved_frame_t **out_frames,
    size_t *out_frame_count
)
{
    pr_resolved_sprite_t *sprites;
    pr_resolved_frame_t *frames;
    size_t frame_count;
    size_t frame_capacity;
    size_t sprite_index;

    if (
        manifest == NULL ||
        (manifest->sprite_count > 0u && images == NULL) ||
        maps == NULL ||
        out_sprites == NULL ||
        out_sprite_count == NULL ||
        out_frames == NULL ||
        out_frame_count == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_sprites = NULL;
    *out_sprite_count = 0u;
    *out_frames = NULL;
    *out_frame_count = 0u;

    if (manifest->sprite_count == 0u) {
        return PR_STATUS_OK;
    }

    sprites = (pr_resolved_sprite_t *)calloc(
        manifest->sprite_count,
        sizeof(sprites[0])
    );
    if (sprites == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    frames = NULL;
    frame_count = 0u;
    frame_capacity = 0u;

    for (sprite_index = 0u; sprite_index < manifest->sprite_count; ++sprite_index) {
        const pr_manifest_sprite_t *sprite;
        const pr_imported_image_t *image;
        pr_resolved_sprite_t *resolved;
        uint32_t source_image_index;
        uint32_t local_frame_index;

        sprite = &manifest->sprites[sprite_index];
        source_image_index = maps->sprite_source_image_idx[sprite_index];
        if (source_image_index >= manifest->image_count) {
            free(frames);
            free(sprites);
            return PR_STATUS_INTERNAL_ERROR;
        }
        image = &images[source_image_index];
        resolved = &sprites[sprite_index];
        resolved->source_image_index = source_image_index;
        resolved->name_str_idx = maps->sprite_id_str_idx[sprite_index];
        resolved->mode = (uint32_t)sprite->mode;
        resolved->first_frame = (uint32_t)frame_count;
        resolved->frame_count = 0u;
        resolved->pivot_x_milli = pr_pivot_to_milli(sprite->pivot_x);
        resolved->pivot_y_milli = pr_pivot_to_milli(sprite->pivot_y);
        local_frame_index = 0u;

        if (sprite->mode == PR_MANIFEST_SPRITE_MODE_SINGLE) {
            pr_resolved_frame_t frame;
            uint32_t x;
            uint32_t y;
            uint32_t w;
            uint32_t h;

            x = (sprite->has_x != 0 && sprite->x > 0) ? (uint32_t)sprite->x : 0u;
            y = (sprite->has_y != 0 && sprite->y > 0) ? (uint32_t)sprite->y : 0u;
            w = (sprite->has_w != 0 && sprite->w > 0) ? (uint32_t)sprite->w : image->width;
            h = (sprite->has_h != 0 && sprite->h > 0) ? (uint32_t)sprite->h : image->height;

            if (x + w > image->width || y + h > image->height) {
                free(frames);
                free(sprites);
                pr_emit_diag(
                    diag_sink,
                    diag_user_data,
                    PR_DIAG_ERROR,
                    "Single sprite source rectangle exceeds source image bounds.",
                    image->resolved_path,
                    "build.sprite.single_rect_oob",
                    sprite->id
                );
                return PR_STATUS_VALIDATION_ERROR;
            }

            memset(&frame, 0, sizeof(frame));
            frame.sprite_index = (uint32_t)sprite_index;
            frame.local_frame_index = local_frame_index++;
            frame.source_x = x;
            frame.source_y = y;
            frame.source_w = w;
            frame.source_h = h;
            frame.atlas_w = w;
            frame.atlas_h = h;
            if (!pr_append_resolved_frame(&frames, &frame_count, &frame_capacity, &frame)) {
                free(frames);
                free(sprites);
                return PR_STATUS_ALLOCATION_FAILED;
            }
        } else if (sprite->mode == PR_MANIFEST_SPRITE_MODE_RECTS) {
            size_t rect_index;

            for (rect_index = 0u; rect_index < sprite->rect_count; ++rect_index) {
                const pr_manifest_sprite_rect_t *rect;
                pr_resolved_frame_t frame;
                uint32_t x;
                uint32_t y;
                uint32_t w;
                uint32_t h;

                rect = &sprite->rects[rect_index];
                x = (uint32_t)rect->x;
                y = (uint32_t)rect->y;
                w = (uint32_t)rect->w;
                h = (uint32_t)rect->h;

                if (x + w > image->width || y + h > image->height) {
                    free(frames);
                    free(sprites);
                    pr_emit_diag(
                        diag_sink,
                        diag_user_data,
                        PR_DIAG_ERROR,
                        "Rect sprite source rectangle exceeds source image bounds.",
                        image->resolved_path,
                        "build.sprite.rect_oob",
                        sprite->id
                    );
                    return PR_STATUS_VALIDATION_ERROR;
                }

                memset(&frame, 0, sizeof(frame));
                frame.sprite_index = (uint32_t)sprite_index;
                frame.local_frame_index = local_frame_index++;
                frame.source_x = x;
                frame.source_y = y;
                frame.source_w = w;
                frame.source_h = h;
                frame.atlas_w = w;
                frame.atlas_h = h;
                if (!pr_append_resolved_frame(&frames, &frame_count, &frame_capacity, &frame)) {
                    free(frames);
                    free(sprites);
                    return PR_STATUS_ALLOCATION_FAILED;
                }
            }
        } else if (sprite->mode == PR_MANIFEST_SPRITE_MODE_GRID) {
            uint32_t margin_x;
            uint32_t margin_y;
            uint32_t spacing_x;
            uint32_t spacing_y;
            uint32_t cell_w;
            uint32_t cell_h;
            uint32_t cols;
            uint32_t rows;
            uint32_t total_cells;
            uint32_t frame_start;
            uint32_t frame_count_target;
            uint32_t i;

            margin_x = (sprite->has_margin_x != 0 && sprite->margin_x > 0) ?
                (uint32_t)sprite->margin_x : 0u;
            margin_y = (sprite->has_margin_y != 0 && sprite->margin_y > 0) ?
                (uint32_t)sprite->margin_y : 0u;
            spacing_x = (sprite->has_spacing_x != 0 && sprite->spacing_x > 0) ?
                (uint32_t)sprite->spacing_x : 0u;
            spacing_y = (sprite->has_spacing_y != 0 && sprite->spacing_y > 0) ?
                (uint32_t)sprite->spacing_y : 0u;
            cell_w = (uint32_t)sprite->cell_w;
            cell_h = (uint32_t)sprite->cell_h;

            if (image->width < margin_x + cell_w || image->height < margin_y + cell_h) {
                free(frames);
                free(sprites);
                pr_emit_diag(
                    diag_sink,
                    diag_user_data,
                    PR_DIAG_ERROR,
                    "Grid sprite has no valid cells in source image.",
                    image->resolved_path,
                    "build.sprite.grid_no_cells",
                    sprite->id
                );
                return PR_STATUS_VALIDATION_ERROR;
            }

            cols = 1u + (image->width - margin_x - cell_w) / (cell_w + spacing_x);
            rows = 1u + (image->height - margin_y - cell_h) / (cell_h + spacing_y);
            total_cells = cols * rows;
            frame_start = (sprite->has_frame_start != 0 && sprite->frame_start > 0) ?
                (uint32_t)sprite->frame_start : 0u;
            if (frame_start >= total_cells) {
                free(frames);
                free(sprites);
                pr_emit_diag(
                    diag_sink,
                    diag_user_data,
                    PR_DIAG_ERROR,
                    "Grid sprite frame_start exceeds available cell count.",
                    image->resolved_path,
                    "build.sprite.grid_frame_start_oob",
                    sprite->id
                );
                return PR_STATUS_VALIDATION_ERROR;
            }

            if (sprite->has_frame_count != 0) {
                frame_count_target = (uint32_t)sprite->frame_count;
            } else {
                frame_count_target = total_cells - frame_start;
            }

            if (frame_start + frame_count_target > total_cells) {
                free(frames);
                free(sprites);
                pr_emit_diag(
                    diag_sink,
                    diag_user_data,
                    PR_DIAG_ERROR,
                    "Grid sprite frame range exceeds available cell count.",
                    image->resolved_path,
                    "build.sprite.grid_frame_count_oob",
                    sprite->id
                );
                return PR_STATUS_VALIDATION_ERROR;
            }

            for (i = 0u; i < frame_count_target; ++i) {
                uint32_t cell_index;
                uint32_t row;
                uint32_t col;
                pr_resolved_frame_t frame;

                cell_index = frame_start + i;
                row = cell_index / cols;
                col = cell_index % cols;

                memset(&frame, 0, sizeof(frame));
                frame.sprite_index = (uint32_t)sprite_index;
                frame.local_frame_index = local_frame_index++;
                frame.source_x = margin_x + col * (cell_w + spacing_x);
                frame.source_y = margin_y + row * (cell_h + spacing_y);
                frame.source_w = cell_w;
                frame.source_h = cell_h;
                frame.atlas_w = cell_w;
                frame.atlas_h = cell_h;
                if (!pr_append_resolved_frame(&frames, &frame_count, &frame_capacity, &frame)) {
                    free(frames);
                    free(sprites);
                    return PR_STATUS_ALLOCATION_FAILED;
                }
            }
        } else {
            free(frames);
            free(sprites);
            return PR_STATUS_VALIDATION_ERROR;
        }

        resolved->frame_count = (uint32_t)(frame_count - (size_t)resolved->first_frame);
        if (resolved->frame_count == 0u) {
            free(frames);
            free(sprites);
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Sprite resolved to zero frames.",
                manifest_path,
                "build.sprite.zero_frames",
                sprite->id
            );
            return PR_STATUS_VALIDATION_ERROR;
        }
    }

    *out_sprites = sprites;
    *out_sprite_count = manifest->sprite_count;
    *out_frames = frames;
    *out_frame_count = frame_count;
    return PR_STATUS_OK;
}

static int pr_place_frame_in_page(
    pr_pack_page_t *page,
    uint32_t padded_w,
    uint32_t padded_h,
    uint32_t padding,
    uint32_t *out_x,
    uint32_t *out_y
)
{
    uint32_t place_x;
    uint32_t place_y;

    if (
        page == NULL ||
        out_x == NULL ||
        out_y == NULL ||
        padded_w == 0u ||
        padded_h == 0u
    ) {
        return 0;
    }
    if (padded_w > page->max_w || padded_h > page->max_h) {
        return 0;
    }

    if (page->cursor_x + padded_w > page->max_w) {
        if (page->cursor_y + page->shelf_h + padded_h > page->max_h) {
            return 0;
        }
        page->cursor_y += page->shelf_h;
        page->cursor_x = 0u;
        page->shelf_h = 0u;
    }

    if (page->cursor_y + padded_h > page->max_h) {
        return 0;
    }

    place_x = page->cursor_x;
    place_y = page->cursor_y;
    page->cursor_x += padded_w;
    if (padded_h > page->shelf_h) {
        page->shelf_h = padded_h;
    }

    if (place_x + padded_w > page->used_w) {
        page->used_w = place_x + padded_w;
    }
    if (place_y + padded_h > page->used_h) {
        page->used_h = place_y + padded_h;
    }

    *out_x = place_x + padding;
    *out_y = place_y + padding;
    return 1;
}

typedef struct pr_pack_item {
    uint32_t frame_index;
    uint32_t padded_w;
    uint32_t padded_h;
    uint64_t area;
    uint32_t sprite_index;
    uint32_t local_frame_index;
} pr_pack_item_t;

static int pr_pack_item_compare(const void *lhs, const void *rhs)
{
    const pr_pack_item_t *a;
    const pr_pack_item_t *b;

    a = (const pr_pack_item_t *)lhs;
    b = (const pr_pack_item_t *)rhs;

    if (a->area != b->area) {
        return (a->area < b->area) ? 1 : -1;
    }
    if (a->padded_h != b->padded_h) {
        return (a->padded_h < b->padded_h) ? 1 : -1;
    }
    if (a->padded_w != b->padded_w) {
        return (a->padded_w < b->padded_w) ? 1 : -1;
    }
    if (a->sprite_index != b->sprite_index) {
        return (a->sprite_index > b->sprite_index) ? 1 : -1;
    }
    if (a->local_frame_index != b->local_frame_index) {
        return (a->local_frame_index > b->local_frame_index) ? 1 : -1;
    }
    return 0;
}

static pr_status_t pr_pack_resolved_frames(
    const pr_manifest_t *manifest,
    pr_resolved_frame_t *frames,
    size_t frame_count,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_pack_page_t **out_pages,
    size_t *out_page_count
)
{
    pr_pack_page_t *pages;
    size_t page_count;
    size_t page_capacity;
    pr_pack_item_t *items;
    size_t i;
    uint32_t padding;

    if (
        manifest == NULL ||
        (frame_count > 0u && frames == NULL) ||
        out_pages == NULL ||
        out_page_count == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_pages = NULL;
    *out_page_count = 0u;
    if (frame_count == 0u) {
        return PR_STATUS_OK;
    }

    padding = (manifest->atlas.padding > 0) ? (uint32_t)manifest->atlas.padding : 0u;
    pages = NULL;
    page_count = 0u;
    page_capacity = 0u;
    items = (pr_pack_item_t *)calloc(frame_count, sizeof(items[0]));
    if (items == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    for (i = 0u; i < frame_count; ++i) {
        uint32_t padded_w;
        uint32_t padded_h;

        padded_w = frames[i].source_w + padding * 2u;
        padded_h = frames[i].source_h + padding * 2u;
        if (
            padded_w > (uint32_t)manifest->atlas.max_page_width ||
            padded_h > (uint32_t)manifest->atlas.max_page_height
        ) {
            free(items);
            pr_emit_diag(
                diag_sink,
                diag_user_data,
                PR_DIAG_ERROR,
                "Frame is too large for atlas page constraints.",
                NULL,
                "build.atlas.frame_too_large",
                NULL
            );
            return PR_STATUS_VALIDATION_ERROR;
        }
        items[i].frame_index = (uint32_t)i;
        items[i].padded_w = padded_w;
        items[i].padded_h = padded_h;
        items[i].area = (uint64_t)padded_w * (uint64_t)padded_h;
        items[i].sprite_index = frames[i].sprite_index;
        items[i].local_frame_index = frames[i].local_frame_index;
    }

    qsort(items, frame_count, sizeof(items[0]), pr_pack_item_compare);

    for (i = 0u; i < frame_count; ++i) {
        size_t page_index;
        int placed;

        placed = 0;
        for (page_index = 0u; page_index < page_count; ++page_index) {
            uint32_t atlas_x;
            uint32_t atlas_y;

            if (!pr_place_frame_in_page(
                    &pages[page_index],
                    items[i].padded_w,
                    items[i].padded_h,
                    padding,
                    &atlas_x,
                    &atlas_y
                )) {
                continue;
            }

            frames[items[i].frame_index].atlas_page = (uint32_t)page_index;
            frames[items[i].frame_index].atlas_x = atlas_x;
            frames[items[i].frame_index].atlas_y = atlas_y;
            placed = 1;
            break;
        }

        if (placed != 0) {
            continue;
        }

        if (!pr_reserve_array(
                (void **)&pages,
                &page_capacity,
                page_count + 1u,
                sizeof(pages[0])
            )) {
            free(items);
            free(pages);
            return PR_STATUS_ALLOCATION_FAILED;
        }

        memset(&pages[page_count], 0, sizeof(pages[0]));
        pages[page_count].max_w = (uint32_t)manifest->atlas.max_page_width;
        pages[page_count].max_h = (uint32_t)manifest->atlas.max_page_height;

        {
            uint32_t atlas_x;
            uint32_t atlas_y;

            if (!pr_place_frame_in_page(
                    &pages[page_count],
                    items[i].padded_w,
                    items[i].padded_h,
                    padding,
                    &atlas_x,
                    &atlas_y
                )) {
                free(items);
                free(pages);
                return PR_STATUS_VALIDATION_ERROR;
            }

            frames[items[i].frame_index].atlas_page = (uint32_t)page_count;
            frames[items[i].frame_index].atlas_x = atlas_x;
            frames[items[i].frame_index].atlas_y = atlas_y;
        }
        page_count += 1u;
    }

    for (i = 0u; i < page_count; ++i) {
        uint32_t final_w;
        uint32_t final_h;

        final_w = (pages[i].used_w > 0u) ? pages[i].used_w : 1u;
        final_h = (pages[i].used_h > 0u) ? pages[i].used_h : 1u;
        if (manifest->atlas.power_of_two != 0) {
            final_w = pr_round_up_pow2_u32(final_w);
            final_h = pr_round_up_pow2_u32(final_h);
            if (final_w > pages[i].max_w) {
                final_w = pages[i].max_w;
            }
            if (final_h > pages[i].max_h) {
                final_h = pages[i].max_h;
            }
        }
        pages[i].final_w = final_w;
        pages[i].final_h = final_h;
    }

    for (i = 0u; i < frame_count; ++i) {
        pr_pack_page_t *page;

        page = &pages[frames[i].atlas_page];
        frames[i].atlas_w = frames[i].source_w;
        frames[i].atlas_h = frames[i].source_h;
        frames[i].u0_milli = (uint32_t)(((uint64_t)frames[i].atlas_x * 1000000u) / page->final_w);
        frames[i].v0_milli = (uint32_t)(((uint64_t)frames[i].atlas_y * 1000000u) / page->final_h);
        frames[i].u1_milli = (uint32_t)(((uint64_t)(frames[i].atlas_x + frames[i].atlas_w) * 1000000u) / page->final_w);
        frames[i].v1_milli = (uint32_t)(((uint64_t)(frames[i].atlas_y + frames[i].atlas_h) * 1000000u) / page->final_h);
    }

    free(items);
    *out_pages = pages;
    *out_page_count = page_count;
    return PR_STATUS_OK;
}

static pr_status_t pr_resolve_animations(
    const pr_manifest_t *manifest,
    const pr_index_maps_t *maps,
    const pr_resolved_sprite_t *sprites,
    size_t sprite_count,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_resolved_animation_t **out_animations,
    size_t *out_animation_count,
    pr_resolved_animation_key_t **out_keys,
    size_t *out_key_count
)
{
    pr_resolved_animation_t *animations;
    pr_resolved_animation_key_t *keys;
    size_t key_count;
    size_t key_capacity;
    size_t animation_index;

    if (
        manifest == NULL ||
        maps == NULL ||
        out_animations == NULL ||
        out_animation_count == NULL ||
        out_keys == NULL ||
        out_key_count == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (
        manifest->sprite_count != sprite_count ||
        (manifest->animation_count > 0u && sprites == NULL)
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_animations = NULL;
    *out_animation_count = 0u;
    *out_keys = NULL;
    *out_key_count = 0u;

    if (manifest->animation_count == 0u) {
        return PR_STATUS_OK;
    }

    animations = (pr_resolved_animation_t *)calloc(
        manifest->animation_count,
        sizeof(animations[0])
    );
    if (animations == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    keys = NULL;
    key_count = 0u;
    key_capacity = 0u;

    for (animation_index = 0u; animation_index < manifest->animation_count; ++animation_index) {
        const pr_manifest_animation_t *animation;
        const pr_resolved_sprite_t *sprite;
        pr_resolved_animation_t *resolved;
        size_t frame_index;
        uint32_t total_duration;

        animation = &manifest->animations[animation_index];
        resolved = &animations[animation_index];
        resolved->name_str_idx = maps->animation_id_str_idx[animation_index];
        resolved->sprite_index = maps->animation_sprite_idx[animation_index];
        resolved->loop_mode = (uint32_t)animation->loop_mode;
        resolved->key_start = (uint32_t)key_count;
        resolved->key_count = 0u;
        resolved->total_duration_ms = 0u;

        if (resolved->sprite_index >= manifest->sprite_count) {
            free(keys);
            free(animations);
            return PR_STATUS_INTERNAL_ERROR;
        }
        sprite = &sprites[resolved->sprite_index];
        total_duration = 0u;

        for (frame_index = 0u; frame_index < animation->frame_count; ++frame_index) {
            const pr_manifest_animation_frame_t *frame;
            pr_resolved_animation_key_t key;

            frame = &animation->frames[frame_index];
            if ((uint32_t)frame->index >= sprite->frame_count) {
                free(keys);
                free(animations);
                pr_emit_diag(
                    diag_sink,
                    diag_user_data,
                    PR_DIAG_ERROR,
                    "Animation frame index exceeds resolved sprite frame count.",
                    NULL,
                    "build.animation.frame_index_oob",
                    animation->id
                );
                return PR_STATUS_VALIDATION_ERROR;
            }

            if (!pr_reserve_array(
                    (void **)&keys,
                    &key_capacity,
                    key_count + 1u,
                    sizeof(keys[0])
                )) {
                free(keys);
                free(animations);
                return PR_STATUS_ALLOCATION_FAILED;
            }

            key.animation_index = (uint32_t)animation_index;
            key.frame_index = (uint32_t)frame->index;
            key.duration_ms = (frame->ms > 0) ? (uint32_t)frame->ms : 0u;
            keys[key_count] = key;
            key_count += 1u;
            resolved->key_count += 1u;
            total_duration += key.duration_ms;
        }

        resolved->total_duration_ms = total_duration;
    }

    *out_animations = animations;
    *out_animation_count = manifest->animation_count;
    *out_keys = keys;
    *out_key_count = key_count;
    return PR_STATUS_OK;
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

static uint32_t pr_atlas_sampling_code(const char *sampling)
{
    if (sampling != NULL && strcmp(sampling, "linear") == 0) {
        return 1u;
    }
    return 0u;
}

static int pr_build_chunk_txtr(
    const pr_manifest_t *manifest,
    const pr_pack_page_t *pages,
    size_t page_count,
    const pr_imported_image_t *images,
    const pr_resolved_sprite_t *sprites,
    size_t sprite_count,
    const pr_resolved_frame_t *frames,
    size_t frame_count,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    unsigned char **page_pixels;
    size_t *page_pixel_bytes;
    size_t i;
    size_t j;

    if (
        manifest == NULL ||
        (page_count > 0u && pages == NULL) ||
        (manifest->image_count > 0u && images == NULL) ||
        sprite_count != manifest->sprite_count ||
        (sprite_count > 0u && sprites == NULL) ||
        (frame_count > 0u && frames == NULL) ||
        chunk == NULL
    ) {
        return 0;
    }

    page_pixels = NULL;
    page_pixel_bytes = NULL;
    if (page_count > 0u) {
        page_pixels = (unsigned char **)calloc(page_count, sizeof(page_pixels[0]));
        page_pixel_bytes = (size_t *)calloc(page_count, sizeof(page_pixel_bytes[0]));
        if (page_pixels == NULL || page_pixel_bytes == NULL) {
            free(page_pixels);
            free(page_pixel_bytes);
            return 0;
        }
    }

    for (i = 0u; i < page_count; ++i) {
        size_t pixel_count;
        size_t pixel_bytes;

        if (pages[i].final_w == 0u || pages[i].final_h == 0u) {
            goto fail;
        }
        if (
            !pr_mul_size((size_t)pages[i].final_w, (size_t)pages[i].final_h, &pixel_count) ||
            !pr_mul_size(pixel_count, 4u, &pixel_bytes)
        ) {
            goto fail;
        }
        if (pixel_bytes > 0xFFFFFFFFu) {
            goto fail;
        }

        page_pixels[i] = (unsigned char *)calloc(1u, pixel_bytes);
        if (page_pixels[i] == NULL) {
            goto fail;
        }
        page_pixel_bytes[i] = pixel_bytes;
    }

    for (i = 0u; i < frame_count; ++i) {
        uint32_t page_index;
        uint32_t sprite_index;
        uint32_t image_index;
        const pr_imported_image_t *source_image;
        unsigned char *page_buffer;
        size_t page_stride;
        size_t src_row_bytes;
        uint32_t row;

        page_index = frames[i].atlas_page;
        sprite_index = frames[i].sprite_index;

        if (page_index >= page_count || sprite_index >= sprite_count) {
            goto fail;
        }

        image_index = sprites[sprite_index].source_image_index;
        if (image_index >= manifest->image_count) {
            goto fail;
        }
        source_image = &images[image_index];
        if (source_image->pixels == NULL) {
            goto fail;
        }

        if (
            frames[i].source_x + frames[i].source_w > source_image->width ||
            frames[i].source_y + frames[i].source_h > source_image->height ||
            frames[i].atlas_x + frames[i].atlas_w > pages[page_index].final_w ||
            frames[i].atlas_y + frames[i].atlas_h > pages[page_index].final_h ||
            frames[i].atlas_w != frames[i].source_w ||
            frames[i].atlas_h != frames[i].source_h
        ) {
            goto fail;
        }

        page_buffer = page_pixels[page_index];
        page_stride = (size_t)pages[page_index].final_w * 4u;
        src_row_bytes = (size_t)frames[i].source_w * 4u;

        for (row = 0u; row < frames[i].source_h; ++row) {
            const unsigned char *src_row;
            unsigned char *dst_row;
            size_t src_offset;
            size_t dst_offset;

            src_offset = (size_t)(frames[i].source_y + row) * (size_t)source_image->row_bytes +
                (size_t)frames[i].source_x * 4u;
            dst_offset = (size_t)(frames[i].atlas_y + row) * page_stride +
                (size_t)frames[i].atlas_x * 4u;

            if (
                src_offset + src_row_bytes > source_image->pixel_bytes ||
                dst_offset + src_row_bytes > page_pixel_bytes[page_index]
            ) {
                goto fail;
            }

            src_row = source_image->pixels + src_offset;
            dst_row = page_buffer + dst_offset;
            memcpy(dst_row, src_row, src_row_bytes);
        }
    }

    pr_byte_buffer_init(&buffer);
    if (
        !pr_byte_buffer_append_u32_le(&buffer, 1u) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)page_count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->atlas.max_page_width) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->atlas.max_page_height) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)manifest->atlas.padding) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)(manifest->atlas.power_of_two != 0)) ||
        !pr_byte_buffer_append_u32_le(&buffer, pr_atlas_sampling_code(manifest->atlas.sampling))
    ) {
        pr_byte_buffer_free(&buffer);
        return 0;
    }

    for (i = 0u; i < page_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)i) ||
            !pr_byte_buffer_append_u32_le(&buffer, pages[i].final_w) ||
            !pr_byte_buffer_append_u32_le(&buffer, pages[i].final_h) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)page_pixel_bytes[i]) ||
            !pr_byte_buffer_append(&buffer, page_pixels[i], page_pixel_bytes[i])
        ) {
            pr_byte_buffer_free(&buffer);
            goto fail;
        }
    }

    memcpy(chunk->id, PR_CHUNK_FORMAT_TXTR, 4u);
    chunk->bytes = buffer.data;
    chunk->size = buffer.size;

    for (j = 0u; j < page_count; ++j) {
        free(page_pixels[j]);
    }
    free(page_pixels);
    free(page_pixel_bytes);
    return 1;

fail:
    for (j = 0u; j < page_count; ++j) {
        free(page_pixels[j]);
    }
    free(page_pixels);
    free(page_pixel_bytes);
    return 0;
}

static int pr_build_chunk_sprt(
    const pr_resolved_sprite_t *sprites,
    size_t sprite_count,
    const pr_resolved_frame_t *frames,
    size_t frame_count,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    size_t i;

    if (
        (sprite_count > 0u && sprites == NULL) ||
        (frame_count > 0u && frames == NULL) ||
        chunk == NULL
    ) {
        return 0;
    }

    pr_byte_buffer_init(&buffer);
    if (
        !pr_byte_buffer_append_u32_le(&buffer, 1u) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)sprite_count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)frame_count)
    ) {
        pr_byte_buffer_free(&buffer);
        return 0;
    }

    for (i = 0u; i < sprite_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].name_str_idx) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].source_image_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].mode) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].first_frame) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].frame_count) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].pivot_x_milli) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].pivot_y_milli)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    for (i = 0u; i < frame_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].sprite_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].local_frame_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].source_x) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].source_y) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].source_w) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].source_h) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].atlas_page) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].atlas_x) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].atlas_y) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].atlas_w) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].atlas_h) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].u0_milli) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].v0_milli) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].u1_milli) ||
            !pr_byte_buffer_append_u32_le(&buffer, frames[i].v1_milli)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    memcpy(chunk->id, PR_CHUNK_FORMAT_SPRT, 4u);
    chunk->bytes = buffer.data;
    chunk->size = buffer.size;
    return 1;
}

static int pr_build_chunk_anim(
    const pr_resolved_animation_t *animations,
    size_t animation_count,
    const pr_resolved_animation_key_t *keys,
    size_t key_count,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    size_t i;

    if (
        (animation_count > 0u && animations == NULL) ||
        (key_count > 0u && keys == NULL) ||
        chunk == NULL
    ) {
        return 0;
    }

    pr_byte_buffer_init(&buffer);
    if (
        !pr_byte_buffer_append_u32_le(&buffer, 1u) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)animation_count) ||
        !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)key_count)
    ) {
        pr_byte_buffer_free(&buffer);
        return 0;
    }

    for (i = 0u; i < animation_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].name_str_idx) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].sprite_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].loop_mode) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].key_start) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].key_count) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].total_duration_ms)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    for (i = 0u; i < key_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, keys[i].animation_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, keys[i].frame_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, keys[i].duration_ms)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    memcpy(chunk->id, PR_CHUNK_FORMAT_ANIM, 4u);
    chunk->bytes = buffer.data;
    chunk->size = buffer.size;
    return 1;
}

static int pr_build_chunk_indx(
    const pr_manifest_t *manifest,
    const pr_imported_image_t *images,
    const pr_index_maps_t *maps,
    const pr_resolved_sprite_t *sprites,
    size_t sprite_count,
    const pr_resolved_animation_t *animations,
    size_t animation_count,
    pr_chunk_payload_t *chunk
)
{
    pr_byte_buffer_t buffer;
    size_t i;

    if (
        manifest == NULL ||
        (manifest->image_count > 0u && images == NULL) ||
        maps == NULL ||
        sprite_count != manifest->sprite_count ||
        animation_count != manifest->animation_count ||
        (sprite_count > 0u && sprites == NULL) ||
        (animation_count > 0u && animations == NULL) ||
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

    for (i = 0u; i < sprite_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].name_str_idx) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)i) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].source_image_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].first_frame) ||
            !pr_byte_buffer_append_u32_le(&buffer, sprites[i].frame_count)
        ) {
            pr_byte_buffer_free(&buffer);
            return 0;
        }
    }

    for (i = 0u; i < animation_count; ++i) {
        if (
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].name_str_idx) ||
            !pr_byte_buffer_append_u32_le(&buffer, (uint32_t)i) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].sprite_index) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].key_start) ||
            !pr_byte_buffer_append_u32_le(&buffer, animations[i].key_count)
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
    pr_resolved_sprite_t *resolved_sprites;
    size_t resolved_sprite_count;
    pr_resolved_frame_t *resolved_frames;
    size_t resolved_frame_count;
    pr_pack_page_t *atlas_pages;
    size_t atlas_page_count;
    pr_resolved_animation_t *resolved_animations;
    size_t resolved_animation_count;
    pr_resolved_animation_key_t *resolved_animation_keys;
    size_t resolved_animation_key_count;
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
    resolved_sprites = NULL;
    resolved_sprite_count = 0u;
    resolved_frames = NULL;
    resolved_frame_count = 0u;
    atlas_pages = NULL;
    atlas_page_count = 0u;
    resolved_animations = NULL;
    resolved_animation_count = 0u;
    resolved_animation_keys = NULL;
    resolved_animation_key_count = 0u;
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
        goto cleanup;
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
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Invalid or too-long package output path.",
            options->manifest_path,
            "build.output_invalid",
            NULL
        );
        status = PR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
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
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Debug output path exceeds max path length.",
            options->manifest_path,
            "build.debug_output_path_too_long",
            NULL
        );
        status = PR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }

    status = pr_import_manifest_images(
        options->manifest_path,
        &manifest,
        diag_sink,
        diag_user_data,
        &images
    );
    if (status != PR_STATUS_OK) {
        goto cleanup;
    }

    if (options->strict_mode != 0 && warning_count > 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Strict mode failed: warnings were emitted.",
            options->manifest_path,
            "build.strict_warnings",
            NULL
        );
        status = PR_STATUS_VALIDATION_ERROR;
        goto cleanup;
    }

    if (!pr_allocate_index_maps(&manifest, &maps)) {
        status = PR_STATUS_ALLOCATION_FAILED;
        goto cleanup;
    }

    if (!pr_build_string_table_and_maps(
            &manifest,
            images,
            &strings,
            &maps,
            diag_sink,
            diag_user_data
        )) {
        status = PR_STATUS_INTERNAL_ERROR;
        goto cleanup;
    }

    status = pr_resolve_sprite_frames(
        &manifest,
        images,
        &maps,
        options->manifest_path,
        diag_sink,
        diag_user_data,
        &resolved_sprites,
        &resolved_sprite_count,
        &resolved_frames,
        &resolved_frame_count
    );
    if (status != PR_STATUS_OK) {
        goto cleanup;
    }

    status = pr_pack_resolved_frames(
        &manifest,
        resolved_frames,
        resolved_frame_count,
        diag_sink,
        diag_user_data,
        &atlas_pages,
        &atlas_page_count
    );
    if (status != PR_STATUS_OK) {
        goto cleanup;
    }

    status = pr_resolve_animations(
        &manifest,
        &maps,
        resolved_sprites,
        resolved_sprite_count,
        diag_sink,
        diag_user_data,
        &resolved_animations,
        &resolved_animation_count,
        &resolved_animation_keys,
        &resolved_animation_key_count
    );
    if (status != PR_STATUS_OK) {
        goto cleanup;
    }

    if (
        !pr_build_chunk_strs(&strings, &chunks[0]) ||
        !pr_build_chunk_txtr(
            &manifest,
            atlas_pages,
            atlas_page_count,
            images,
            resolved_sprites,
            resolved_sprite_count,
            resolved_frames,
            resolved_frame_count,
            &chunks[1]
        ) ||
        !pr_build_chunk_sprt(
            resolved_sprites,
            resolved_sprite_count,
            resolved_frames,
            resolved_frame_count,
            &chunks[2]
        ) ||
        !pr_build_chunk_anim(
            resolved_animations,
            resolved_animation_count,
            resolved_animation_keys,
            resolved_animation_key_count,
            &chunks[3]
        ) ||
        !pr_build_chunk_indx(
            &manifest,
            images,
            &maps,
            resolved_sprites,
            resolved_sprite_count,
            resolved_animations,
            resolved_animation_count,
            &chunks[4]
        )
    ) {
        status = PR_STATUS_ALLOCATION_FAILED;
        goto cleanup;
    }

    status = pr_write_package_with_chunks(
        PR_BUILD_RESULT_STORAGE.package_path,
        chunks,
        PR_CHUNK_COUNT_V0,
        diag_sink,
        diag_user_data
    );
    if (status != PR_STATUS_OK) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to write package output.",
            PR_BUILD_RESULT_STORAGE.package_path,
            "build.output_write_failed",
            NULL
        );
        goto cleanup;
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
            goto cleanup;
        }
    }

    out_result->package_path = PR_BUILD_RESULT_STORAGE.package_path;
    out_result->debug_output_path = (
        PR_BUILD_RESULT_STORAGE.debug_output_path[0] != '\0'
    ) ? PR_BUILD_RESULT_STORAGE.debug_output_path : NULL;
    out_result->atlas_page_count = (unsigned int)atlas_page_count;
    out_result->sprite_count = (unsigned int)manifest.sprite_count;
    out_result->animation_count = (unsigned int)manifest.animation_count;

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_NOTE,
        "Wrote .prpk package with STRS/TXTR/SPRT/ANIM/INDX chunks.",
        PR_BUILD_RESULT_STORAGE.package_path,
        "build.package_written",
        NULL
    );

    status = PR_STATUS_OK;

cleanup:
    for (i = 0; i < (int)PR_CHUNK_COUNT_V0; ++i) {
        pr_chunk_payload_free(&chunks[i]);
    }
    free(resolved_animation_keys);
    free(resolved_animations);
    free(atlas_pages);
    free(resolved_frames);
    free(resolved_sprites);
    pr_free_index_maps(&maps);
    pr_string_table_free(&strings);
    pr_imported_images_free(images, manifest.image_count);
    pr_manifest_free(&manifest);
    return status;
}
