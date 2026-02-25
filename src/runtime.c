#include "packrat/runtime.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PR_CHUNK_ID_STRS "STRS"
#define PR_CHUNK_ID_TXTR "TXTR"
#define PR_CHUNK_ID_SPRT "SPRT"
#define PR_CHUNK_ID_ANIM "ANIM"

#define PR_PACKAGE_HEADER_SIZE_V1 24u
#define PR_CHUNK_TABLE_ENTRY_SIZE 20u

typedef struct pr_chunk_entry {
    char id[4];
    size_t offset;
    size_t size;
    const unsigned char *payload;
} pr_chunk_entry_t;

typedef struct pr_sprite_meta {
    uint32_t first_frame;
    uint32_t frame_count;
    float pivot_x;
    float pivot_y;
} pr_sprite_meta_t;

typedef struct pr_animation_meta {
    uint32_t key_start;
    uint32_t key_count;
} pr_animation_meta_t;

typedef struct pr_atlas_page_view {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const unsigned char *pixels;
    uint32_t pixel_bytes;
} pr_atlas_page_view_t;

struct pr_package {
    unsigned char *owned_bytes;
    const unsigned char *bytes;
    size_t size;

    const char **strings;
    unsigned int string_count;

    unsigned int atlas_page_count;
    pr_atlas_page_view_t *atlas_pages;
    int has_txtr_chunk;

    pr_sprite_t *sprites;
    unsigned int sprite_count;

    pr_sprite_frame_t *sprite_frames;
    unsigned int sprite_frame_count;

    pr_animation_t *animations;
    unsigned int animation_count;

    pr_anim_frame_t *animation_frames;
    unsigned int animation_frame_count;
};

static int pr_can_read(size_t total_size, size_t offset, size_t byte_count)
{
    if (offset > total_size) {
        return 0;
    }
    return byte_count <= (total_size - offset);
}

static int pr_u64_to_size(uint64_t value, size_t *out_size)
{
    if (out_size == NULL) {
        return 0;
    }
    if (value > (uint64_t)SIZE_MAX) {
        return 0;
    }
    *out_size = (size_t)value;
    return 1;
}

static int pr_read_u16_le(
    const unsigned char *bytes,
    size_t size,
    size_t offset,
    uint16_t *out_value
)
{
    if (bytes == NULL || out_value == NULL || !pr_can_read(size, offset, 2u)) {
        return 0;
    }

    *out_value = (uint16_t)bytes[offset] |
        (uint16_t)((uint16_t)bytes[offset + 1u] << 8u);
    return 1;
}

static int pr_read_u32_le(
    const unsigned char *bytes,
    size_t size,
    size_t offset,
    uint32_t *out_value
)
{
    if (bytes == NULL || out_value == NULL || !pr_can_read(size, offset, 4u)) {
        return 0;
    }

    *out_value = (uint32_t)bytes[offset] |
        ((uint32_t)bytes[offset + 1u] << 8u) |
        ((uint32_t)bytes[offset + 2u] << 16u) |
        ((uint32_t)bytes[offset + 3u] << 24u);
    return 1;
}

static int pr_read_u64_le(
    const unsigned char *bytes,
    size_t size,
    size_t offset,
    uint64_t *out_value
)
{
    uint64_t value;
    unsigned int i;

    if (bytes == NULL || out_value == NULL || !pr_can_read(size, offset, 8u)) {
        return 0;
    }

    value = 0u;
    for (i = 0u; i < 8u; ++i) {
        value |= ((uint64_t)bytes[offset + i] << (8u * i));
    }
    *out_value = value;
    return 1;
}

static const pr_chunk_entry_t *pr_find_chunk(
    const pr_chunk_entry_t *chunks,
    uint32_t chunk_count,
    const char chunk_id[4]
)
{
    uint32_t i;

    if (chunks == NULL || chunk_id == NULL) {
        return NULL;
    }

    for (i = 0u; i < chunk_count; ++i) {
        if (memcmp(chunks[i].id, chunk_id, 4u) == 0) {
            return &chunks[i];
        }
    }
    return NULL;
}

static pr_status_t pr_parse_chunk_table(
    const unsigned char *bytes,
    size_t size,
    pr_chunk_entry_t **out_chunks,
    uint32_t *out_chunk_count
)
{
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_size;
    uint32_t chunk_count;
    uint64_t chunk_table_offset64;
    size_t chunk_table_offset;
    size_t chunk_table_size;
    pr_chunk_entry_t *chunks;
    uint32_t i;

    if (
        bytes == NULL ||
        out_chunks == NULL ||
        out_chunk_count == NULL
    ) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_chunks = NULL;
    *out_chunk_count = 0u;

    if (size < PR_PACKAGE_HEADER_SIZE_V1) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (memcmp(bytes, "PRPK", 4u) != 0) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (
        !pr_read_u16_le(bytes, size, 4u, &version_major) ||
        !pr_read_u16_le(bytes, size, 6u, &version_minor) ||
        !pr_read_u32_le(bytes, size, 8u, &header_size) ||
        !pr_read_u32_le(bytes, size, 12u, &chunk_count) ||
        !pr_read_u64_le(bytes, size, 16u, &chunk_table_offset64)
    ) {
        return PR_STATUS_PARSE_ERROR;
    }
    (void)version_minor;

    if (version_major == 0u || header_size < PR_PACKAGE_HEADER_SIZE_V1) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (!pr_u64_to_size(chunk_table_offset64, &chunk_table_offset)) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (chunk_count == 0u) {
        return PR_STATUS_PARSE_ERROR;
    }
    if ((size_t)chunk_count > SIZE_MAX / PR_CHUNK_TABLE_ENTRY_SIZE) {
        return PR_STATUS_PARSE_ERROR;
    }
    chunk_table_size = (size_t)chunk_count * PR_CHUNK_TABLE_ENTRY_SIZE;
    if (!pr_can_read(size, chunk_table_offset, chunk_table_size)) {
        return PR_STATUS_PARSE_ERROR;
    }

    chunks = (pr_chunk_entry_t *)calloc((size_t)chunk_count, sizeof(chunks[0]));
    if (chunks == NULL) {
        return PR_STATUS_ALLOCATION_FAILED;
    }

    for (i = 0u; i < chunk_count; ++i) {
        size_t cursor;
        uint64_t payload_offset64;
        uint64_t payload_size64;

        cursor = chunk_table_offset + (size_t)i * PR_CHUNK_TABLE_ENTRY_SIZE;
        memcpy(chunks[i].id, bytes + cursor, 4u);
        if (
            !pr_read_u64_le(bytes, size, cursor + 4u, &payload_offset64) ||
            !pr_read_u64_le(bytes, size, cursor + 12u, &payload_size64)
        ) {
            free(chunks);
            return PR_STATUS_PARSE_ERROR;
        }
        if (
            !pr_u64_to_size(payload_offset64, &chunks[i].offset) ||
            !pr_u64_to_size(payload_size64, &chunks[i].size)
        ) {
            free(chunks);
            return PR_STATUS_PARSE_ERROR;
        }
        if (!pr_can_read(size, chunks[i].offset, chunks[i].size)) {
            free(chunks);
            return PR_STATUS_PARSE_ERROR;
        }
        chunks[i].payload = bytes + chunks[i].offset;
    }

    *out_chunks = chunks;
    *out_chunk_count = chunk_count;
    return PR_STATUS_OK;
}

static void pr_package_clear_parsed_data(pr_package_t *package)
{
    if (package == NULL) {
        return;
    }

    free(package->strings);
    package->strings = NULL;
    package->string_count = 0u;

    free(package->atlas_pages);
    package->atlas_pages = NULL;
    package->has_txtr_chunk = 0;

    free(package->sprites);
    package->sprites = NULL;
    package->sprite_count = 0u;

    free(package->sprite_frames);
    package->sprite_frames = NULL;
    package->sprite_frame_count = 0u;

    free(package->animations);
    package->animations = NULL;
    package->animation_count = 0u;

    free(package->animation_frames);
    package->animation_frames = NULL;
    package->animation_frame_count = 0u;

    package->atlas_page_count = 0u;
}

static pr_status_t pr_parse_chunk_strs(
    pr_package_t *package,
    const pr_chunk_entry_t *chunk
)
{
    uint32_t version;
    uint32_t string_count;
    uint32_t blob_bytes;
    size_t offsets_bytes;
    size_t blob_offset;
    const unsigned char *blob;
    const char **string_ptrs;
    uint32_t i;

    if (package == NULL || chunk == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (
        !pr_read_u32_le(chunk->payload, chunk->size, 0u, &version) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 4u, &string_count) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 8u, &blob_bytes)
    ) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (version != 1u) {
        return PR_STATUS_PARSE_ERROR;
    }

    if ((size_t)string_count > SIZE_MAX / sizeof(uint32_t)) {
        return PR_STATUS_PARSE_ERROR;
    }
    offsets_bytes = (size_t)string_count * sizeof(uint32_t);
    blob_offset = 12u + offsets_bytes;
    if (!pr_can_read(chunk->size, 12u, offsets_bytes)) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (!pr_can_read(chunk->size, blob_offset, (size_t)blob_bytes)) {
        return PR_STATUS_PARSE_ERROR;
    }

    string_ptrs = NULL;
    if (string_count > 0u) {
        string_ptrs = (const char **)calloc(
            (size_t)string_count,
            sizeof(string_ptrs[0])
        );
        if (string_ptrs == NULL) {
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }

    blob = chunk->payload + blob_offset;
    for (i = 0u; i < string_count; ++i) {
        uint32_t str_offset;

        if (!pr_read_u32_le(chunk->payload, chunk->size, 12u + (size_t)i * 4u, &str_offset)) {
            free((void *)string_ptrs);
            return PR_STATUS_PARSE_ERROR;
        }
        if (str_offset >= blob_bytes) {
            free(string_ptrs);
            return PR_STATUS_PARSE_ERROR;
        }
        if (memchr(blob + str_offset, '\0', (size_t)blob_bytes - (size_t)str_offset) == NULL) {
            free(string_ptrs);
            return PR_STATUS_PARSE_ERROR;
        }
        string_ptrs[i] = (const char *)(blob + str_offset);
    }

    package->strings = string_ptrs;
    package->string_count = string_count;
    return PR_STATUS_OK;
}

static pr_status_t pr_parse_chunk_txtr(
    pr_package_t *package,
    const pr_chunk_entry_t *chunk
)
{
    uint32_t version;
    uint32_t page_count;
    pr_atlas_page_view_t *pages;
    unsigned char *seen_pages;
    size_t cursor;
    uint32_t i;

    if (package == NULL || chunk == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (
        !pr_read_u32_le(chunk->payload, chunk->size, 0u, &version) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 4u, &page_count)
    ) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (version != 1u) {
        return PR_STATUS_PARSE_ERROR;
    }

    pages = NULL;
    seen_pages = NULL;
    if (page_count > 0u) {
        pages = (pr_atlas_page_view_t *)calloc((size_t)page_count, sizeof(pages[0]));
        seen_pages = (unsigned char *)calloc((size_t)page_count, sizeof(seen_pages[0]));
        if (pages == NULL || seen_pages == NULL) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }

    cursor = 28u;
    if (!pr_can_read(chunk->size, 0u, cursor)) {
        free(pages);
        free(seen_pages);
        return PR_STATUS_PARSE_ERROR;
    }

    for (i = 0u; i < page_count; ++i) {
        uint32_t page_index;
        uint32_t width;
        uint32_t height;
        uint32_t pixel_blob_size;
        uint64_t expected_bytes64;
        uint32_t stride;

        if (
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 0u, &page_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 4u, &width) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 8u, &height) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 12u, &pixel_blob_size)
        ) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }

        cursor += 16u;
        if (!pr_can_read(chunk->size, cursor, (size_t)pixel_blob_size)) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }

        if (
            page_index >= page_count ||
            seen_pages[page_index] != 0u ||
            width == 0u ||
            height == 0u
        ) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }

        expected_bytes64 = (uint64_t)width * (uint64_t)height * 4u;
        if (expected_bytes64 > (uint64_t)UINT32_MAX) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }
        stride = width * 4u;
        if (pixel_blob_size != 0u && pixel_blob_size != (uint32_t)expected_bytes64) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }

        pages[page_index].width = width;
        pages[page_index].height = height;
        pages[page_index].stride = stride;
        pages[page_index].pixel_bytes = pixel_blob_size;
        pages[page_index].pixels = (pixel_blob_size > 0u) ? (chunk->payload + cursor) : NULL;
        seen_pages[page_index] = 1u;
        cursor += (size_t)pixel_blob_size;
    }

    if (cursor != chunk->size) {
        free(pages);
        free(seen_pages);
        return PR_STATUS_PARSE_ERROR;
    }
    for (i = 0u; i < page_count; ++i) {
        if (seen_pages[i] == 0u) {
            free(pages);
            free(seen_pages);
            return PR_STATUS_PARSE_ERROR;
        }
    }

    free(seen_pages);
    package->atlas_pages = pages;
    package->atlas_page_count = page_count;
    package->has_txtr_chunk = 1;
    return PR_STATUS_OK;
}

static pr_status_t pr_parse_chunk_sprt(
    pr_package_t *package,
    const pr_chunk_entry_t *chunk
)
{
    uint32_t version;
    uint32_t sprite_count;
    uint32_t frame_count;
    size_t sprite_records_bytes;
    size_t frame_records_bytes;
    size_t cursor;
    pr_sprite_meta_t *sprite_meta;
    unsigned char *frame_seen;
    uint32_t max_page_plus_one;
    uint32_t i;

    if (package == NULL || chunk == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (
        !pr_read_u32_le(chunk->payload, chunk->size, 0u, &version) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 4u, &sprite_count) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 8u, &frame_count)
    ) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (version != 1u) {
        return PR_STATUS_PARSE_ERROR;
    }
    if ((size_t)sprite_count > SIZE_MAX / 28u || (size_t)frame_count > SIZE_MAX / 60u) {
        return PR_STATUS_PARSE_ERROR;
    }

    sprite_records_bytes = (size_t)sprite_count * 28u;
    frame_records_bytes = (size_t)frame_count * 60u;
    cursor = 12u;
    if (!pr_can_read(chunk->size, cursor, sprite_records_bytes + frame_records_bytes)) {
        return PR_STATUS_PARSE_ERROR;
    }

    sprite_meta = NULL;
    frame_seen = NULL;
    if (sprite_count > 0u) {
        package->sprites = (pr_sprite_t *)calloc((size_t)sprite_count, sizeof(package->sprites[0]));
        sprite_meta = (pr_sprite_meta_t *)calloc((size_t)sprite_count, sizeof(sprite_meta[0]));
        if (package->sprites == NULL || sprite_meta == NULL) {
            free(sprite_meta);
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }
    if (frame_count > 0u) {
        package->sprite_frames = (pr_sprite_frame_t *)calloc(
            (size_t)frame_count,
            sizeof(package->sprite_frames[0])
        );
        frame_seen = (unsigned char *)calloc((size_t)frame_count, sizeof(frame_seen[0]));
        if (package->sprite_frames == NULL || frame_seen == NULL) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }

    package->sprite_count = sprite_count;
    package->sprite_frame_count = frame_count;

    for (i = 0u; i < sprite_count; ++i) {
        uint32_t name_str_idx;
        uint32_t source_image_index;
        uint32_t mode;
        uint32_t first_frame;
        uint32_t local_frame_count;
        uint32_t pivot_x_milli;
        uint32_t pivot_y_milli;

        if (
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 0u, &name_str_idx) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 4u, &source_image_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 8u, &mode) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 12u, &first_frame) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 16u, &local_frame_count) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 20u, &pivot_x_milli) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 24u, &pivot_y_milli)
        ) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        (void)source_image_index;
        (void)mode;

        if (name_str_idx >= package->string_count) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        if (first_frame > frame_count || local_frame_count > (frame_count - first_frame)) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        package->sprites[i].id = package->strings[name_str_idx];
        package->sprites[i].frame_count = local_frame_count;
        package->sprites[i].frames = (
            local_frame_count > 0u
        ) ? &package->sprite_frames[first_frame] : NULL;

        sprite_meta[i].first_frame = first_frame;
        sprite_meta[i].frame_count = local_frame_count;
        sprite_meta[i].pivot_x = (float)pivot_x_milli / 1000.0f;
        sprite_meta[i].pivot_y = (float)pivot_y_milli / 1000.0f;

        cursor += 28u;
    }

    max_page_plus_one = package->atlas_page_count;
    for (i = 0u; i < frame_count; ++i) {
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
        uint32_t target;
        const pr_sprite_meta_t *meta;
        pr_sprite_frame_t *frame;

        if (
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 0u, &sprite_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 4u, &local_frame_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 8u, &source_x) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 12u, &source_y) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 16u, &source_w) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 20u, &source_h) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 24u, &atlas_page) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 28u, &atlas_x) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 32u, &atlas_y) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 36u, &atlas_w) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 40u, &atlas_h) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 44u, &u0_milli) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 48u, &v0_milli) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 52u, &u1_milli) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 56u, &v1_milli)
        ) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        (void)source_x;
        (void)source_y;
        (void)source_w;
        (void)source_h;

        if (sprite_index >= sprite_count) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        meta = &sprite_meta[sprite_index];
        if (local_frame_index >= meta->frame_count) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        target = meta->first_frame + local_frame_index;
        if (target >= frame_count || frame_seen[target] != 0u) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        frame = &package->sprite_frames[target];
        frame->atlas_page = atlas_page;
        frame->x = atlas_x;
        frame->y = atlas_y;
        frame->w = atlas_w;
        frame->h = atlas_h;
        frame->u0 = (float)u0_milli / 1000000.0f;
        frame->v0 = (float)v0_milli / 1000000.0f;
        frame->u1 = (float)u1_milli / 1000000.0f;
        frame->v1 = (float)v1_milli / 1000000.0f;
        frame->pivot_x = meta->pivot_x;
        frame->pivot_y = meta->pivot_y;
        frame_seen[target] = 1u;

        if (package->has_txtr_chunk != 0) {
            if (atlas_page >= package->atlas_page_count) {
                free(frame_seen);
                free(sprite_meta);
                return PR_STATUS_PARSE_ERROR;
            }
        } else if (atlas_page < UINT32_MAX) {
            uint32_t next_page;

            next_page = atlas_page + 1u;
            if (next_page > max_page_plus_one) {
                max_page_plus_one = next_page;
            }
        }

        cursor += 60u;
    }

    for (i = 0u; i < frame_count; ++i) {
        if (frame_seen[i] == 0u) {
            free(frame_seen);
            free(sprite_meta);
            return PR_STATUS_PARSE_ERROR;
        }
    }

    if (package->has_txtr_chunk == 0 && max_page_plus_one > package->atlas_page_count) {
        package->atlas_page_count = max_page_plus_one;
    }

    free(frame_seen);
    free(sprite_meta);

    if (cursor != chunk->size) {
        return PR_STATUS_PARSE_ERROR;
    }
    return PR_STATUS_OK;
}

static pr_status_t pr_parse_chunk_anim(
    pr_package_t *package,
    const pr_chunk_entry_t *chunk
)
{
    uint32_t version;
    uint32_t animation_count;
    uint32_t key_count;
    size_t animation_records_bytes;
    size_t key_records_bytes;
    size_t cursor;
    pr_animation_meta_t *animation_meta;
    uint32_t i;

    if (package == NULL || chunk == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (
        !pr_read_u32_le(chunk->payload, chunk->size, 0u, &version) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 4u, &animation_count) ||
        !pr_read_u32_le(chunk->payload, chunk->size, 8u, &key_count)
    ) {
        return PR_STATUS_PARSE_ERROR;
    }
    if (version != 1u) {
        return PR_STATUS_PARSE_ERROR;
    }
    if ((size_t)animation_count > SIZE_MAX / 24u || (size_t)key_count > SIZE_MAX / 12u) {
        return PR_STATUS_PARSE_ERROR;
    }

    animation_records_bytes = (size_t)animation_count * 24u;
    key_records_bytes = (size_t)key_count * 12u;
    cursor = 12u;
    if (!pr_can_read(chunk->size, cursor, animation_records_bytes + key_records_bytes)) {
        return PR_STATUS_PARSE_ERROR;
    }

    animation_meta = NULL;
    if (animation_count > 0u) {
        package->animations = (pr_animation_t *)calloc(
            (size_t)animation_count,
            sizeof(package->animations[0])
        );
        animation_meta = (pr_animation_meta_t *)calloc(
            (size_t)animation_count,
            sizeof(animation_meta[0])
        );
        if (package->animations == NULL || animation_meta == NULL) {
            free(animation_meta);
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }
    if (key_count > 0u) {
        package->animation_frames = (pr_anim_frame_t *)calloc(
            (size_t)key_count,
            sizeof(package->animation_frames[0])
        );
        if (package->animation_frames == NULL) {
            free(animation_meta);
            return PR_STATUS_ALLOCATION_FAILED;
        }
    }

    package->animation_count = animation_count;
    package->animation_frame_count = key_count;

    for (i = 0u; i < animation_count; ++i) {
        uint32_t name_str_idx;
        uint32_t sprite_index;
        uint32_t loop_mode;
        uint32_t key_start;
        uint32_t local_key_count;
        uint32_t total_duration_ms;

        if (
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 0u, &name_str_idx) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 4u, &sprite_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 8u, &loop_mode) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 12u, &key_start) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 16u, &local_key_count) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 20u, &total_duration_ms)
        ) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        (void)total_duration_ms;

        if (name_str_idx >= package->string_count || sprite_index >= package->sprite_count) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        if (loop_mode > (uint32_t)PR_LOOP_PING_PONG) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        if (key_start > key_count || local_key_count > (key_count - key_start)) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        package->animations[i].id = package->strings[name_str_idx];
        package->animations[i].sprite = &package->sprites[sprite_index];
        package->animations[i].loop_mode = (pr_loop_mode_t)loop_mode;
        package->animations[i].frame_count = local_key_count;
        package->animations[i].frames = (
            local_key_count > 0u
        ) ? &package->animation_frames[key_start] : NULL;

        animation_meta[i].key_start = key_start;
        animation_meta[i].key_count = local_key_count;

        cursor += 24u;
    }

    for (i = 0u; i < key_count; ++i) {
        uint32_t animation_index;
        uint32_t frame_index;
        uint32_t duration_ms;
        const pr_animation_meta_t *meta;
        const pr_animation_t *animation;

        if (
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 0u, &animation_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 4u, &frame_index) ||
            !pr_read_u32_le(chunk->payload, chunk->size, cursor + 8u, &duration_ms)
        ) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        if (animation_index >= animation_count) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        meta = &animation_meta[animation_index];
        animation = &package->animations[animation_index];

        if (i < meta->key_start || i >= meta->key_start + meta->key_count) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }
        if (frame_index >= animation->sprite->frame_count) {
            free(animation_meta);
            return PR_STATUS_PARSE_ERROR;
        }

        package->animation_frames[i].sprite_frame_index = frame_index;
        package->animation_frames[i].duration_ms = duration_ms;

        cursor += 12u;
    }

    free(animation_meta);

    if (cursor != chunk->size) {
        return PR_STATUS_PARSE_ERROR;
    }
    return PR_STATUS_OK;
}

static pr_status_t pr_parse_loaded_package(pr_package_t *package)
{
    pr_chunk_entry_t *chunks;
    uint32_t chunk_count;
    const pr_chunk_entry_t *strs_chunk;
    const pr_chunk_entry_t *txtr_chunk;
    const pr_chunk_entry_t *sprt_chunk;
    const pr_chunk_entry_t *anim_chunk;
    pr_status_t status;

    if (package == NULL || package->bytes == NULL || package->size == 0u) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    chunks = NULL;
    chunk_count = 0u;
    status = pr_parse_chunk_table(package->bytes, package->size, &chunks, &chunk_count);
    if (status != PR_STATUS_OK) {
        return status;
    }

    strs_chunk = pr_find_chunk(chunks, chunk_count, PR_CHUNK_ID_STRS);
    txtr_chunk = pr_find_chunk(chunks, chunk_count, PR_CHUNK_ID_TXTR);
    sprt_chunk = pr_find_chunk(chunks, chunk_count, PR_CHUNK_ID_SPRT);
    anim_chunk = pr_find_chunk(chunks, chunk_count, PR_CHUNK_ID_ANIM);

    if (strs_chunk == NULL || sprt_chunk == NULL || anim_chunk == NULL) {
        free(chunks);
        return PR_STATUS_PARSE_ERROR;
    }

    status = pr_parse_chunk_strs(package, strs_chunk);
    if (status != PR_STATUS_OK) {
        free(chunks);
        pr_package_clear_parsed_data(package);
        return status;
    }

    if (txtr_chunk != NULL) {
        status = pr_parse_chunk_txtr(package, txtr_chunk);
        if (status != PR_STATUS_OK) {
            free(chunks);
            pr_package_clear_parsed_data(package);
            return status;
        }
    }

    status = pr_parse_chunk_sprt(package, sprt_chunk);
    if (status != PR_STATUS_OK) {
        free(chunks);
        pr_package_clear_parsed_data(package);
        return status;
    }

    status = pr_parse_chunk_anim(package, anim_chunk);
    if (status != PR_STATUS_OK) {
        free(chunks);
        pr_package_clear_parsed_data(package);
        return status;
    }

    free(chunks);
    return PR_STATUS_OK;
}

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
    if (size <= 0L) {
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
    pr_status_t status;

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

    status = pr_parse_loaded_package(package);
    if (status != PR_STATUS_OK) {
        pr_package_close(package);
        return status;
    }

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
    pr_status_t status;

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

    status = pr_parse_loaded_package(package);
    if (status != PR_STATUS_OK) {
        pr_package_close(package);
        return status;
    }

    *out_package = package;
    return PR_STATUS_OK;
}

void pr_package_close(pr_package_t *package)
{
    if (package == NULL) {
        return;
    }

    pr_package_clear_parsed_data(package);

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
    unsigned int i;

    if (package == NULL || sprite_id == NULL) {
        return NULL;
    }

    for (i = 0u; i < package->sprite_count; ++i) {
        if (package->sprites[i].id != NULL && strcmp(package->sprites[i].id, sprite_id) == 0) {
            return &package->sprites[i];
        }
    }
    return NULL;
}

const pr_animation_t *pr_package_find_animation(
    const pr_package_t *package,
    const char *animation_id
)
{
    unsigned int i;

    if (package == NULL || animation_id == NULL) {
        return NULL;
    }

    for (i = 0u; i < package->animation_count; ++i) {
        if (
            package->animations[i].id != NULL &&
            strcmp(package->animations[i].id, animation_id) == 0
        ) {
            return &package->animations[i];
        }
    }
    return NULL;
}

pr_status_t pr_package_resolve_sprite_binding(
    const pr_package_t *package,
    const char *sprite_id,
    const char *animation_id,
    const pr_sprite_t **out_sprite,
    const pr_animation_t **out_animation
)
{
    const pr_sprite_t *sprite;
    const pr_animation_t *animation;

    if (package == NULL || out_sprite == NULL || out_animation == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    *out_sprite = NULL;
    *out_animation = NULL;

    sprite = NULL;
    animation = NULL;
    if (animation_id != NULL && animation_id[0] != '\0') {
        animation = pr_package_find_animation(package, animation_id);
        if (animation == NULL) {
            return PR_STATUS_VALIDATION_ERROR;
        }
        if (animation->sprite == NULL) {
            return PR_STATUS_VALIDATION_ERROR;
        }
        sprite = animation->sprite;
    }

    if (sprite_id != NULL && sprite_id[0] != '\0') {
        const pr_sprite_t *explicit_sprite;

        explicit_sprite = pr_package_find_sprite(package, sprite_id);
        if (explicit_sprite == NULL) {
            return PR_STATUS_VALIDATION_ERROR;
        }
        if (sprite != NULL && sprite != explicit_sprite) {
            return PR_STATUS_VALIDATION_ERROR;
        }
        sprite = explicit_sprite;
    }

    if (sprite == NULL) {
        return PR_STATUS_VALIDATION_ERROR;
    }

    *out_sprite = sprite;
    *out_animation = animation;
    return PR_STATUS_OK;
}

unsigned int pr_package_atlas_page_count(const pr_package_t *package)
{
    if (package == NULL) {
        return 0u;
    }
    return package->atlas_page_count;
}

const unsigned char *pr_package_atlas_page_pixels(
    const pr_package_t *package,
    unsigned int index,
    unsigned int *out_width,
    unsigned int *out_height,
    unsigned int *out_stride
)
{
    const pr_atlas_page_view_t *page;

    if (
        package == NULL ||
        index >= package->atlas_page_count ||
        package->atlas_pages == NULL
    ) {
        return NULL;
    }

    page = &package->atlas_pages[index];
    if (out_width != NULL) {
        *out_width = page->width;
    }
    if (out_height != NULL) {
        *out_height = page->height;
    }
    if (out_stride != NULL) {
        *out_stride = page->stride;
    }
    return page->pixels;
}

unsigned int pr_package_sprite_count(const pr_package_t *package)
{
    if (package == NULL) {
        return 0u;
    }
    return package->sprite_count;
}

const pr_sprite_t *pr_package_sprite_at(
    const pr_package_t *package,
    unsigned int index
)
{
    if (package == NULL || index >= package->sprite_count) {
        return NULL;
    }
    return &package->sprites[index];
}

unsigned int pr_package_animation_count(const pr_package_t *package)
{
    if (package == NULL) {
        return 0u;
    }
    return package->animation_count;
}

const pr_animation_t *pr_package_animation_at(
    const pr_package_t *package,
    unsigned int index
)
{
    if (package == NULL || index >= package->animation_count) {
        return NULL;
    }
    return &package->animations[index];
}
