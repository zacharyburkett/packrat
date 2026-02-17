#include "manifest.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum pr_manifest_section {
    PR_MANIFEST_SECTION_ROOT = 0,
    PR_MANIFEST_SECTION_ATLAS,
    PR_MANIFEST_SECTION_IMAGE,
    PR_MANIFEST_SECTION_SPRITE,
    PR_MANIFEST_SECTION_SPRITE_RECTS,
    PR_MANIFEST_SECTION_ANIMATION
} pr_manifest_section_t;

typedef struct pr_manifest_diag_context {
    pr_diag_sink_fn sink;
    void *user_data;
    int error_count;
    int warning_count;
} pr_manifest_diag_context_t;

typedef struct pr_manifest_parse_state {
    pr_manifest_t *manifest;
    pr_manifest_diag_context_t *diag;
    const char *manifest_path;
    pr_manifest_section_t section;
    size_t current_image;
    size_t current_sprite;
    size_t current_rect;
    size_t current_animation;
    int parse_error_count;
} pr_manifest_parse_state_t;

static void pr_manifest_emit_diag(
    pr_manifest_diag_context_t *diag,
    pr_diag_severity_t severity,
    const char *message,
    const char *file,
    int line,
    int column,
    const char *code,
    const char *asset_id
)
{
    pr_diagnostic_t diagnostic;

    if (diag == NULL || message == NULL) {
        return;
    }

    if (severity == PR_DIAG_ERROR) {
        diag->error_count += 1;
    } else if (severity == PR_DIAG_WARNING) {
        diag->warning_count += 1;
    }

    if (diag->sink == NULL) {
        return;
    }

    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.severity = severity;
    diagnostic.message = message;
    diagnostic.file = file;
    diagnostic.line = line;
    diagnostic.column = column;
    diagnostic.code = code;
    diagnostic.asset_id = asset_id;
    diag->sink(&diagnostic, diag->user_data);
}

static int pr_manifest_reserve_array(
    void **buffer,
    size_t *capacity,
    size_t needed_count,
    size_t element_size
)
{
    size_t new_capacity;
    void *new_buffer;

    if (buffer == NULL || capacity == NULL || element_size == 0u) {
        return 0;
    }
    if (needed_count <= *capacity) {
        return 1;
    }

    new_capacity = (*capacity == 0u) ? 8u : *capacity;
    while (new_capacity < needed_count) {
        new_capacity *= 2u;
    }

    new_buffer = realloc(*buffer, new_capacity * element_size);
    if (new_buffer == NULL) {
        return 0;
    }

    *buffer = new_buffer;
    *capacity = new_capacity;
    return 1;
}

static int pr_manifest_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t length;

    if (dst == NULL || dst_size == 0u || src == NULL) {
        return 0;
    }

    length = strlen(src);
    if (length >= dst_size) {
        return 0;
    }

    memcpy(dst, src, length + 1u);
    return 1;
}

void pr_manifest_init(pr_manifest_t *manifest)
{
    if (manifest == NULL) {
        return;
    }

    memset(manifest, 0, sizeof(*manifest));
    manifest->atlas.max_page_width = 2048;
    manifest->atlas.max_page_height = 2048;
    manifest->atlas.padding = 1;
    manifest->atlas.power_of_two = 0;
    (void)pr_manifest_copy_string(
        manifest->atlas.sampling,
        sizeof(manifest->atlas.sampling),
        "pixel"
    );
}

void pr_manifest_free(pr_manifest_t *manifest)
{
    size_t i;

    if (manifest == NULL) {
        return;
    }

    if (manifest->sprites != NULL) {
        for (i = 0u; i < manifest->sprite_count; ++i) {
            free(manifest->sprites[i].rects);
            manifest->sprites[i].rects = NULL;
            manifest->sprites[i].rect_count = 0u;
            manifest->sprites[i].rect_capacity = 0u;
        }
    }

    if (manifest->animations != NULL) {
        for (i = 0u; i < manifest->animation_count; ++i) {
            free(manifest->animations[i].frames);
            manifest->animations[i].frames = NULL;
            manifest->animations[i].frame_count = 0u;
            manifest->animations[i].frame_capacity = 0u;
        }
    }

    free(manifest->images);
    free(manifest->sprites);
    free(manifest->animations);
    pr_manifest_init(manifest);
}

static char *pr_manifest_read_text_file(const char *path, size_t *out_size)
{
    FILE *file;
    char *buffer;
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

    buffer = (char *)malloc((size_t)file_size + 1u);
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

    buffer[read_size] = '\0';
    *out_size = read_size;
    return buffer;
}

static int pr_manifest_split_lines(char *text, char ***out_lines, size_t *out_line_count)
{
    char **lines;
    size_t line_capacity;
    size_t line_count;
    char *cursor;

    if (text == NULL || out_lines == NULL || out_line_count == NULL) {
        return 0;
    }

    line_capacity = 16u;
    line_count = 0u;
    lines = (char **)calloc(line_capacity, sizeof(lines[0]));
    if (lines == NULL) {
        return 0;
    }

    cursor = text;
    while (1) {
        char *newline;

        if (line_count + 1u > line_capacity) {
            char **grown;

            line_capacity *= 2u;
            grown = (char **)realloc(lines, line_capacity * sizeof(lines[0]));
            if (grown == NULL) {
                free(lines);
                return 0;
            }
            lines = grown;
        }

        lines[line_count++] = cursor;
        newline = strchr(cursor, '\n');
        if (newline == NULL) {
            break;
        }
        *newline = '\0';
        cursor = newline + 1;
    }

    *out_lines = lines;
    *out_line_count = line_count;
    return 1;
}

static void pr_manifest_strip_comment_inplace(char *line)
{
    int in_string;
    int escape_next;
    char *cursor;

    if (line == NULL) {
        return;
    }

    in_string = 0;
    escape_next = 0;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (in_string != 0) {
            if (escape_next != 0) {
                escape_next = 0;
                continue;
            }
            if (*cursor == '\\') {
                escape_next = 1;
                continue;
            }
            if (*cursor == '"') {
                in_string = 0;
            }
            continue;
        }

        if (*cursor == '"') {
            in_string = 1;
            continue;
        }
        if (*cursor == '#') {
            *cursor = '\0';
            return;
        }
    }
}

static char *pr_manifest_trim_inplace(char *value)
{
    char *start;
    char *end;

    if (value == NULL) {
        return NULL;
    }

    start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }

    if (*start == '\0') {
        return start;
    }

    end = start + strlen(start) - 1u;
    while (end > start && isspace((unsigned char)*end)) {
        *end = '\0';
        end -= 1;
    }

    return start;
}

static int pr_manifest_split_key_value_inplace(
    char *line,
    char **out_key,
    char **out_value
)
{
    int in_string;
    int escape_next;
    char *cursor;

    if (line == NULL || out_key == NULL || out_value == NULL) {
        return 0;
    }

    in_string = 0;
    escape_next = 0;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (in_string != 0) {
            if (escape_next != 0) {
                escape_next = 0;
                continue;
            }
            if (*cursor == '\\') {
                escape_next = 1;
                continue;
            }
            if (*cursor == '"') {
                in_string = 0;
            }
            continue;
        }

        if (*cursor == '"') {
            in_string = 1;
            continue;
        }
        if (*cursor == '=') {
            *cursor = '\0';
            *out_key = pr_manifest_trim_inplace(line);
            *out_value = pr_manifest_trim_inplace(cursor + 1);
            return ((*out_key)[0] != '\0') ? 1 : 0;
        }
    }

    return 0;
}

static int pr_manifest_parse_string_value(
    const char *value,
    char *out_value,
    size_t out_value_size
)
{
    const char *cursor;
    size_t out_index;

    if (value == NULL || out_value == NULL || out_value_size == 0u) {
        return 0;
    }

    out_value[0] = '\0';
    cursor = value;

    if (*cursor == '"') {
        int escape_next;

        cursor += 1;
        out_index = 0u;
        escape_next = 0;
        while (*cursor != '\0') {
            char ch;

            if (escape_next != 0) {
                if (*cursor == 'n') {
                    ch = '\n';
                } else if (*cursor == 't') {
                    ch = '\t';
                } else {
                    ch = *cursor;
                }
                escape_next = 0;
            } else {
                if (*cursor == '\\') {
                    escape_next = 1;
                    cursor += 1;
                    continue;
                }
                if (*cursor == '"') {
                    cursor += 1;
                    break;
                }
                ch = *cursor;
            }

            if (out_index + 1u >= out_value_size) {
                return 0;
            }
            out_value[out_index++] = ch;
            cursor += 1;
        }

        cursor = pr_manifest_trim_inplace((char *)cursor);
        if (*cursor != '\0') {
            return 0;
        }
        out_value[out_index] = '\0';
        return 1;
    }

    if (!pr_manifest_copy_string(out_value, out_value_size, value)) {
        return 0;
    }
    return 1;
}

static int pr_manifest_parse_int_value(const char *value, int *out_value)
{
    char *end;
    long parsed;

    if (value == NULL || out_value == NULL) {
        return 0;
    }

    parsed = strtol(value, &end, 10);
    if (end == value) {
        return 0;
    }

    end = pr_manifest_trim_inplace(end);
    if (*end != '\0') {
        return 0;
    }

    *out_value = (int)parsed;
    return 1;
}

static int pr_manifest_parse_double_value(const char *value, double *out_value)
{
    char *end;
    double parsed;

    if (value == NULL || out_value == NULL) {
        return 0;
    }

    parsed = strtod(value, &end);
    if (end == value) {
        return 0;
    }

    end = pr_manifest_trim_inplace(end);
    if (*end != '\0') {
        return 0;
    }

    *out_value = parsed;
    return 1;
}

static int pr_manifest_parse_bool_value(const char *value, int *out_value)
{
    if (value == NULL || out_value == NULL) {
        return 0;
    }

    if (strcmp(value, "true") == 0) {
        *out_value = 1;
        return 1;
    }
    if (strcmp(value, "false") == 0) {
        *out_value = 0;
        return 1;
    }

    return 0;
}

static int pr_manifest_parse_section_header(
    const char *line,
    pr_manifest_section_t *out_section
)
{
    if (line == NULL || out_section == NULL) {
        return 0;
    }

    if (strcmp(line, "[atlas]") == 0) {
        *out_section = PR_MANIFEST_SECTION_ATLAS;
        return 1;
    }
    if (strcmp(line, "[[images]]") == 0) {
        *out_section = PR_MANIFEST_SECTION_IMAGE;
        return 1;
    }
    if (strcmp(line, "[[sprites]]") == 0) {
        *out_section = PR_MANIFEST_SECTION_SPRITE;
        return 1;
    }
    if (strcmp(line, "[[sprites.rects]]") == 0) {
        *out_section = PR_MANIFEST_SECTION_SPRITE_RECTS;
        return 1;
    }
    if (strcmp(line, "[[animations]]") == 0) {
        *out_section = PR_MANIFEST_SECTION_ANIMATION;
        return 1;
    }

    return 0;
}

static int pr_manifest_bracket_depth_delta(const char *text)
{
    int depth;
    int in_string;
    int escape_next;
    const char *cursor;

    if (text == NULL) {
        return 0;
    }

    depth = 0;
    in_string = 0;
    escape_next = 0;
    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (in_string != 0) {
            if (escape_next != 0) {
                escape_next = 0;
                continue;
            }
            if (*cursor == '\\') {
                escape_next = 1;
                continue;
            }
            if (*cursor == '"') {
                in_string = 0;
            }
            continue;
        }

        if (*cursor == '"') {
            in_string = 1;
            continue;
        }
        if (*cursor == '[') {
            depth += 1;
        } else if (*cursor == ']') {
            depth -= 1;
        }
    }

    return depth;
}

static int pr_manifest_append_text(
    char **buffer,
    size_t *length,
    size_t *capacity,
    const char *text
)
{
    size_t append_length;
    size_t needed;
    char *grown;

    if (buffer == NULL || length == NULL || capacity == NULL || text == NULL) {
        return 0;
    }

    append_length = strlen(text);
    needed = *length + append_length + 1u;
    if (needed > *capacity) {
        size_t new_capacity;

        new_capacity = (*capacity == 0u) ? 64u : *capacity;
        while (new_capacity < needed) {
            new_capacity *= 2u;
        }

        grown = (char *)realloc(*buffer, new_capacity);
        if (grown == NULL) {
            return 0;
        }
        *buffer = grown;
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, text, append_length);
    *length += append_length;
    (*buffer)[*length] = '\0';
    return 1;
}

static int pr_manifest_collect_array_value(
    char **lines,
    size_t line_count,
    size_t *line_index,
    char *initial_value,
    char **out_value,
    pr_manifest_diag_context_t *diag,
    const char *manifest_path
)
{
    char *combined;
    size_t length;
    size_t capacity;
    int depth;

    if (
        lines == NULL ||
        line_index == NULL ||
        initial_value == NULL ||
        out_value == NULL ||
        diag == NULL
    ) {
        return 0;
    }

    *out_value = NULL;
    combined = NULL;
    length = 0u;
    capacity = 0u;

    if (initial_value[0] != '[') {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Array value must start with '['.",
            manifest_path,
            (int)(*line_index + 1u),
            1,
            "manifest.array_missing_open",
            NULL
        );
        return 0;
    }

    if (!pr_manifest_append_text(&combined, &length, &capacity, initial_value)) {
        free(combined);
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Allocation failed while reading array value.",
            manifest_path,
            (int)(*line_index + 1u),
            1,
            "manifest.array_alloc_failed",
            NULL
        );
        return 0;
    }

    depth = pr_manifest_bracket_depth_delta(initial_value);
    while (depth > 0) {
        char *line;

        if (*line_index + 1u >= line_count) {
            free(combined);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Unterminated array value.",
                manifest_path,
                (int)(*line_index + 1u),
                1,
                "manifest.array_unterminated",
                NULL
            );
            return 0;
        }

        *line_index += 1u;
        line = lines[*line_index];
        pr_manifest_strip_comment_inplace(line);
        line = pr_manifest_trim_inplace(line);

        if (!pr_manifest_append_text(&combined, &length, &capacity, "\n")) {
            free(combined);
            return 0;
        }
        if (!pr_manifest_append_text(&combined, &length, &capacity, line)) {
            free(combined);
            return 0;
        }

        depth += pr_manifest_bracket_depth_delta(line);
    }

    *out_value = combined;
    return 1;
}

static pr_manifest_image_t *pr_manifest_push_image(pr_manifest_t *manifest)
{
    pr_manifest_image_t *image;

    if (manifest == NULL) {
        return NULL;
    }
    if (!pr_manifest_reserve_array(
            (void **)&manifest->images,
            &manifest->image_capacity,
            manifest->image_count + 1u,
            sizeof(manifest->images[0])
        )) {
        return NULL;
    }

    image = &manifest->images[manifest->image_count++];
    memset(image, 0, sizeof(*image));
    image->premultiply_alpha = 0;
    (void)pr_manifest_copy_string(image->color_space, sizeof(image->color_space), "srgb");
    return image;
}

static pr_manifest_sprite_t *pr_manifest_push_sprite(pr_manifest_t *manifest)
{
    pr_manifest_sprite_t *sprite;

    if (manifest == NULL) {
        return NULL;
    }
    if (!pr_manifest_reserve_array(
            (void **)&manifest->sprites,
            &manifest->sprite_capacity,
            manifest->sprite_count + 1u,
            sizeof(manifest->sprites[0])
        )) {
        return NULL;
    }

    sprite = &manifest->sprites[manifest->sprite_count++];
    memset(sprite, 0, sizeof(*sprite));
    sprite->mode = PR_MANIFEST_SPRITE_MODE_SINGLE;
    sprite->pivot_x = 0.5;
    sprite->pivot_y = 0.5;
    return sprite;
}

static pr_manifest_sprite_rect_t *pr_manifest_push_sprite_rect(pr_manifest_sprite_t *sprite)
{
    pr_manifest_sprite_rect_t *rect;

    if (sprite == NULL) {
        return NULL;
    }
    if (!pr_manifest_reserve_array(
            (void **)&sprite->rects,
            &sprite->rect_capacity,
            sprite->rect_count + 1u,
            sizeof(sprite->rects[0])
        )) {
        return NULL;
    }

    rect = &sprite->rects[sprite->rect_count++];
    memset(rect, 0, sizeof(*rect));
    return rect;
}

static pr_manifest_animation_t *pr_manifest_push_animation(pr_manifest_t *manifest)
{
    pr_manifest_animation_t *animation;

    if (manifest == NULL) {
        return NULL;
    }
    if (!pr_manifest_reserve_array(
            (void **)&manifest->animations,
            &manifest->animation_capacity,
            manifest->animation_count + 1u,
            sizeof(manifest->animations[0])
        )) {
        return NULL;
    }

    animation = &manifest->animations[manifest->animation_count++];
    memset(animation, 0, sizeof(*animation));
    animation->loop_mode = PR_LOOP_LOOP;
    return animation;
}

static pr_manifest_animation_frame_t *pr_manifest_push_animation_frame(
    pr_manifest_animation_t *animation
)
{
    pr_manifest_animation_frame_t *frame;

    if (animation == NULL) {
        return NULL;
    }
    if (!pr_manifest_reserve_array(
            (void **)&animation->frames,
            &animation->frame_capacity,
            animation->frame_count + 1u,
            sizeof(animation->frames[0])
        )) {
        return NULL;
    }

    frame = &animation->frames[animation->frame_count++];
    memset(frame, 0, sizeof(*frame));
    return frame;
}

static int pr_manifest_parse_animation_frames_value(
    const char *value,
    pr_manifest_animation_t *animation,
    pr_manifest_diag_context_t *diag,
    const char *manifest_path,
    int line_number
)
{
    char *work;
    char *cursor;
    char *end;
    int any_frame;

    if (value == NULL || animation == NULL || diag == NULL) {
        return 0;
    }

    work = (char *)malloc(strlen(value) + 1u);
    if (work == NULL) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Allocation failed while parsing animation frames.",
            manifest_path,
            line_number,
            1,
            "manifest.frames_alloc_failed",
            animation->id
        );
        return 0;
    }
    memcpy(work, value, strlen(value) + 1u);
    cursor = pr_manifest_trim_inplace(work);

    end = cursor + strlen(cursor);
    if (end == cursor || cursor[0] != '[' || end[-1] != ']') {
        free(work);
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Animation frames must be an array of inline tables.",
            manifest_path,
            line_number,
            1,
            "manifest.frames_not_array",
            animation->id
        );
        return 0;
    }

    cursor += 1;
    end -= 1;
    *end = '\0';

    any_frame = 0;
    while (1) {
        char *object_start;
        char *object_end;
        int brace_depth;
        int in_string;
        int escape_next;
        char *object_text;
        pr_manifest_animation_frame_t *frame;
        char *pair_cursor;
        int has_index;
        int has_ms;

        while (*cursor != '\0' && (isspace((unsigned char)*cursor) || *cursor == ',')) {
            cursor += 1;
        }
        if (*cursor == '\0') {
            break;
        }

        if (*cursor != '{') {
            free(work);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Each animation frame entry must be an inline table.",
                manifest_path,
                line_number,
                1,
                "manifest.frames_inline_table_expected",
                animation->id
            );
            return 0;
        }

        object_start = cursor;
        object_end = cursor;
        brace_depth = 0;
        in_string = 0;
        escape_next = 0;
        while (*object_end != '\0') {
            if (in_string != 0) {
                if (escape_next != 0) {
                    escape_next = 0;
                    object_end += 1;
                    continue;
                }
                if (*object_end == '\\') {
                    escape_next = 1;
                    object_end += 1;
                    continue;
                }
                if (*object_end == '"') {
                    in_string = 0;
                }
                object_end += 1;
                continue;
            }

            if (*object_end == '"') {
                in_string = 1;
                object_end += 1;
                continue;
            }
            if (*object_end == '{') {
                brace_depth += 1;
            } else if (*object_end == '}') {
                brace_depth -= 1;
                if (brace_depth == 0) {
                    object_end += 1;
                    break;
                }
            }
            object_end += 1;
        }

        if (brace_depth != 0) {
            free(work);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Unterminated inline frame table.",
                manifest_path,
                line_number,
                1,
                "manifest.frames_unterminated_table",
                animation->id
            );
            return 0;
        }

        object_text = (char *)malloc((size_t)(object_end - object_start) + 1u);
        if (object_text == NULL) {
            free(work);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Allocation failed while parsing frame object.",
                manifest_path,
                line_number,
                1,
                "manifest.frames_object_alloc_failed",
                animation->id
            );
            return 0;
        }
        memcpy(object_text, object_start, (size_t)(object_end - object_start));
        object_text[object_end - object_start] = '\0';

        {
            char *inner;
            char *inner_end;

            inner = pr_manifest_trim_inplace(object_text);
            inner_end = inner + strlen(inner);
            if (inner_end <= inner + 1u || inner[0] != '{' || inner_end[-1] != '}') {
                free(object_text);
                free(work);
                return 0;
            }
            inner_end[-1] = '\0';
            pair_cursor = inner + 1u;
        }

        frame = pr_manifest_push_animation_frame(animation);
        if (frame == NULL) {
            free(object_text);
            free(work);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Allocation failed while storing animation frame.",
                manifest_path,
                line_number,
                1,
                "manifest.frames_store_alloc_failed",
                animation->id
            );
            return 0;
        }
        frame->line = line_number;
        has_index = 0;
        has_ms = 0;

        while (*pair_cursor != '\0') {
            char *pair_start;
            char *pair_end;
            int quote_depth;
            int quote_escape;
            char *key;
            char *pair_value;
            char *pair_text;

            while (*pair_cursor != '\0' && (isspace((unsigned char)*pair_cursor) || *pair_cursor == ',')) {
                pair_cursor += 1;
            }
            if (*pair_cursor == '\0') {
                break;
            }

            pair_start = pair_cursor;
            pair_end = pair_cursor;
            quote_depth = 0;
            quote_escape = 0;
            while (*pair_end != '\0') {
                if (quote_depth != 0) {
                    if (quote_escape != 0) {
                        quote_escape = 0;
                        pair_end += 1;
                        continue;
                    }
                    if (*pair_end == '\\') {
                        quote_escape = 1;
                        pair_end += 1;
                        continue;
                    }
                    if (*pair_end == '"') {
                        quote_depth = 0;
                    }
                    pair_end += 1;
                    continue;
                }

                if (*pair_end == '"') {
                    quote_depth = 1;
                    pair_end += 1;
                    continue;
                }
                if (*pair_end == ',') {
                    break;
                }
                pair_end += 1;
            }

            pair_text = (char *)malloc((size_t)(pair_end - pair_start) + 1u);
            if (pair_text == NULL) {
                free(object_text);
                free(work);
                return 0;
            }
            memcpy(pair_text, pair_start, (size_t)(pair_end - pair_start));
            pair_text[pair_end - pair_start] = '\0';
            key = pr_manifest_trim_inplace(pair_text);

            if (!pr_manifest_split_key_value_inplace(key, &key, &pair_value)) {
                free(pair_text);
                free(object_text);
                free(work);
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "Invalid key/value pair in animation frame.",
                    manifest_path,
                    line_number,
                    1,
                    "manifest.frames_invalid_pair",
                    animation->id
                );
                return 0;
            }

            if (strcmp(key, "index") == 0) {
                int parsed_index;

                if (!pr_manifest_parse_int_value(pair_value, &parsed_index)) {
                    free(pair_text);
                    free(object_text);
                    free(work);
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "Animation frame index must be an integer.",
                        manifest_path,
                        line_number,
                        1,
                        "manifest.frames_index_invalid",
                        animation->id
                    );
                    return 0;
                }
                frame->index = parsed_index;
                frame->has_index = 1;
                has_index = 1;
            } else if (strcmp(key, "ms") == 0) {
                int parsed_ms;

                if (!pr_manifest_parse_int_value(pair_value, &parsed_ms)) {
                    free(pair_text);
                    free(object_text);
                    free(work);
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "Animation frame ms must be an integer.",
                        manifest_path,
                        line_number,
                        1,
                        "manifest.frames_ms_invalid",
                        animation->id
                    );
                    return 0;
                }
                frame->ms = parsed_ms;
                frame->has_ms = 1;
                has_ms = 1;
            } else {
                char message[128];

                (void)snprintf(
                    message,
                    sizeof(message),
                    "Unknown animation frame field: %s",
                    key
                );
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    message,
                    manifest_path,
                    line_number,
                    1,
                    "manifest.frames_unknown_field",
                    animation->id
                );
            }

            free(pair_text);
            pair_cursor = (*pair_end == ',') ? (pair_end + 1) : pair_end;
        }

        if (has_index == 0 || has_ms == 0) {
            free(object_text);
            free(work);
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "Animation frame entries require index and ms.",
                manifest_path,
                line_number,
                1,
                "manifest.frames_missing_fields",
                animation->id
            );
            return 0;
        }

        any_frame = 1;
        free(object_text);
        cursor = object_end;
    }

    if (any_frame == 0) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Animation frames array cannot be empty.",
            manifest_path,
            line_number,
            1,
            "manifest.frames_empty",
            animation->id
        );
        free(work);
        return 0;
    }

    animation->has_frames = 1;
    free(work);
    return 1;
}

static void pr_manifest_mark_parse_error(pr_manifest_parse_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->parse_error_count += 1;
}

static void pr_manifest_parse_root_assignment(
    pr_manifest_parse_state_t *state,
    const char *key,
    const char *value,
    int line_number
)
{
    pr_manifest_t *manifest;

    manifest = state->manifest;
    if (strcmp(key, "schema_version") == 0) {
        int parsed;

        if (!pr_manifest_parse_int_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "schema_version must be an integer.",
                state->manifest_path,
                line_number,
                1,
                "manifest.schema_version_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        manifest->schema_version = parsed;
        manifest->has_schema_version = 1;
        return;
    }
    if (strcmp(key, "package_name") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "package_name must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.package_name_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(
            manifest->package_name,
            sizeof(manifest->package_name),
            parsed
        );
        manifest->has_package_name = 1;
        return;
    }
    if (strcmp(key, "output") == 0) {
        char parsed[PR_MANIFEST_PATH_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "output must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.output_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(
            manifest->output,
            sizeof(manifest->output),
            parsed
        );
        manifest->has_output = 1;
        return;
    }
    if (strcmp(key, "debug_output") == 0) {
        char parsed[PR_MANIFEST_PATH_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "debug_output must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.debug_output_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(
            manifest->debug_output,
            sizeof(manifest->debug_output),
            parsed
        );
        manifest->has_debug_output = 1;
        return;
    }
    if (strcmp(key, "pretty_debug_json") == 0) {
        int parsed_bool;

        if (!pr_manifest_parse_bool_value(value, &parsed_bool)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "pretty_debug_json must be true or false.",
                state->manifest_path,
                line_number,
                1,
                "manifest.pretty_debug_json_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        manifest->pretty_debug_json = parsed_bool;
        manifest->has_pretty_debug_json = 1;
        return;
    }

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown top-level key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.unknown_root_key",
            NULL
        );
        pr_manifest_mark_parse_error(state);
    }
}

static void pr_manifest_parse_atlas_assignment(
    pr_manifest_parse_state_t *state,
    const char *key,
    const char *value,
    int line_number
)
{
    pr_manifest_atlas_t *atlas;

    atlas = &state->manifest->atlas;
    if (strcmp(key, "max_page_width") == 0) {
        int parsed;

        if (!pr_manifest_parse_int_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "atlas.max_page_width must be an integer.",
                state->manifest_path,
                line_number,
                1,
                "manifest.atlas.max_page_width_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        atlas->max_page_width = parsed;
        atlas->has_max_page_width = 1;
        return;
    }
    if (strcmp(key, "max_page_height") == 0) {
        int parsed;

        if (!pr_manifest_parse_int_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "atlas.max_page_height must be an integer.",
                state->manifest_path,
                line_number,
                1,
                "manifest.atlas.max_page_height_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        atlas->max_page_height = parsed;
        atlas->has_max_page_height = 1;
        return;
    }
    if (strcmp(key, "padding") == 0) {
        int parsed;

        if (!pr_manifest_parse_int_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "atlas.padding must be an integer.",
                state->manifest_path,
                line_number,
                1,
                "manifest.atlas.padding_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        atlas->padding = parsed;
        atlas->has_padding = 1;
        return;
    }
    if (strcmp(key, "power_of_two") == 0) {
        int parsed_bool;

        if (!pr_manifest_parse_bool_value(value, &parsed_bool)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "atlas.power_of_two must be true or false.",
                state->manifest_path,
                line_number,
                1,
                "manifest.atlas.power_of_two_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        atlas->power_of_two = parsed_bool;
        atlas->has_power_of_two = 1;
        return;
    }
    if (strcmp(key, "sampling") == 0) {
        char parsed[PR_MANIFEST_SMALL_TEXT_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "atlas.sampling must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.atlas.sampling_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(atlas->sampling, sizeof(atlas->sampling), parsed);
        atlas->has_sampling = 1;
        return;
    }

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown atlas key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.atlas.unknown_key",
            NULL
        );
        pr_manifest_mark_parse_error(state);
    }
}

static void pr_manifest_parse_image_assignment(
    pr_manifest_parse_state_t *state,
    const char *key,
    const char *value,
    int line_number
)
{
    pr_manifest_image_t *image;

    if (state->current_image >= state->manifest->image_count) {
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            "Image assignment without active [[images]] block.",
            state->manifest_path,
            line_number,
            1,
            "manifest.images.no_active_block",
            NULL
        );
        pr_manifest_mark_parse_error(state);
        return;
    }

    image = &state->manifest->images[state->current_image];
    if (strcmp(key, "id") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "images.id must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.images.id_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(image->id, sizeof(image->id), parsed);
        image->has_id = 1;
        return;
    }
    if (strcmp(key, "path") == 0) {
        char parsed[PR_MANIFEST_PATH_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "images.path must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.images.path_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(image->path, sizeof(image->path), parsed);
        image->has_path = 1;
        return;
    }
    if (strcmp(key, "premultiply_alpha") == 0) {
        int parsed_bool;

        if (!pr_manifest_parse_bool_value(value, &parsed_bool)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "images.premultiply_alpha must be true or false.",
                state->manifest_path,
                line_number,
                1,
                "manifest.images.premultiply_alpha_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        image->premultiply_alpha = parsed_bool;
        image->has_premultiply_alpha = 1;
        return;
    }
    if (strcmp(key, "color_space") == 0) {
        char parsed[PR_MANIFEST_SMALL_TEXT_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "images.color_space must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.images.color_space_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(image->color_space, sizeof(image->color_space), parsed);
        image->has_color_space = 1;
        return;
    }

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown images key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.images.unknown_key",
            NULL
        );
        pr_manifest_mark_parse_error(state);
    }
}

static void pr_manifest_parse_sprite_assignment(
    pr_manifest_parse_state_t *state,
    const char *key,
    const char *value,
    int line_number
)
{
    pr_manifest_sprite_t *sprite;

    if (state->current_sprite >= state->manifest->sprite_count) {
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            "Sprite assignment without active [[sprites]] block.",
            state->manifest_path,
            line_number,
            1,
            "manifest.sprites.no_active_block",
            NULL
        );
        pr_manifest_mark_parse_error(state);
        return;
    }

    sprite = &state->manifest->sprites[state->current_sprite];
    if (strcmp(key, "id") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.id must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.id_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(sprite->id, sizeof(sprite->id), parsed);
        sprite->has_id = 1;
        return;
    }
    if (strcmp(key, "source") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.source must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.source_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(sprite->source, sizeof(sprite->source), parsed);
        sprite->has_source = 1;
        return;
    }
    if (strcmp(key, "mode") == 0) {
        char parsed[PR_MANIFEST_SMALL_TEXT_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.mode must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.mode_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        if (strcmp(parsed, "single") == 0) {
            sprite->mode = PR_MANIFEST_SPRITE_MODE_SINGLE;
        } else if (strcmp(parsed, "grid") == 0) {
            sprite->mode = PR_MANIFEST_SPRITE_MODE_GRID;
        } else if (strcmp(parsed, "rects") == 0) {
            sprite->mode = PR_MANIFEST_SPRITE_MODE_RECTS;
        } else {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.mode must be one of single, grid, rects.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.mode_unknown",
                sprite->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        sprite->has_mode = 1;
        return;
    }
    if (strcmp(key, "pivot_x") == 0) {
        double parsed;

        if (!pr_manifest_parse_double_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.pivot_x must be a number.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.pivot_x_invalid",
                sprite->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        sprite->pivot_x = parsed;
        sprite->has_pivot_x = 1;
        return;
    }
    if (strcmp(key, "pivot_y") == 0) {
        double parsed;

        if (!pr_manifest_parse_double_value(value, &parsed)) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.pivot_y must be a number.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.pivot_y_invalid",
                sprite->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        sprite->pivot_y = parsed;
        sprite->has_pivot_y = 1;
        return;
    }

#define PR_PARSE_SPRITE_INT_FIELD(field_name, field_code) \
    if (strcmp(key, #field_name) == 0) { \
        int parsed; \
        if (!pr_manifest_parse_int_value(value, &parsed)) { \
            pr_manifest_emit_diag( \
                state->diag, \
                PR_DIAG_ERROR, \
                "sprites." #field_name " must be an integer.", \
                state->manifest_path, \
                line_number, \
                1, \
                field_code, \
                sprite->id \
            ); \
            pr_manifest_mark_parse_error(state); \
            return; \
        } \
        sprite->field_name = parsed; \
        sprite->has_##field_name = 1; \
        return; \
    }

    PR_PARSE_SPRITE_INT_FIELD(x, "manifest.sprites.x_invalid");
    PR_PARSE_SPRITE_INT_FIELD(y, "manifest.sprites.y_invalid");
    PR_PARSE_SPRITE_INT_FIELD(w, "manifest.sprites.w_invalid");
    PR_PARSE_SPRITE_INT_FIELD(h, "manifest.sprites.h_invalid");
    PR_PARSE_SPRITE_INT_FIELD(cell_w, "manifest.sprites.cell_w_invalid");
    PR_PARSE_SPRITE_INT_FIELD(cell_h, "manifest.sprites.cell_h_invalid");
    PR_PARSE_SPRITE_INT_FIELD(frame_start, "manifest.sprites.frame_start_invalid");
    PR_PARSE_SPRITE_INT_FIELD(frame_count, "manifest.sprites.frame_count_invalid");
    PR_PARSE_SPRITE_INT_FIELD(margin_x, "manifest.sprites.margin_x_invalid");
    PR_PARSE_SPRITE_INT_FIELD(margin_y, "manifest.sprites.margin_y_invalid");
    PR_PARSE_SPRITE_INT_FIELD(spacing_x, "manifest.sprites.spacing_x_invalid");
    PR_PARSE_SPRITE_INT_FIELD(spacing_y, "manifest.sprites.spacing_y_invalid");

#undef PR_PARSE_SPRITE_INT_FIELD

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown sprites key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.sprites.unknown_key",
            sprite->id
        );
        pr_manifest_mark_parse_error(state);
    }
}

static void pr_manifest_parse_sprite_rect_assignment(
    pr_manifest_parse_state_t *state,
    const char *key,
    const char *value,
    int line_number
)
{
    pr_manifest_sprite_t *sprite;
    pr_manifest_sprite_rect_t *rect;

    if (
        state->current_sprite >= state->manifest->sprite_count ||
        state->current_rect == (size_t)-1
    ) {
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            "sprites.rects assignment without active [[sprites.rects]] block.",
            state->manifest_path,
            line_number,
            1,
            "manifest.sprites.rects.no_active_block",
            NULL
        );
        pr_manifest_mark_parse_error(state);
        return;
    }

    sprite = &state->manifest->sprites[state->current_sprite];
    if (state->current_rect >= sprite->rect_count) {
        pr_manifest_mark_parse_error(state);
        return;
    }
    rect = &sprite->rects[state->current_rect];

#define PR_PARSE_RECT_INT_FIELD(field_name, field_code) \
    if (strcmp(key, #field_name) == 0) { \
        int parsed; \
        if (!pr_manifest_parse_int_value(value, &parsed)) { \
            pr_manifest_emit_diag( \
                state->diag, \
                PR_DIAG_ERROR, \
                "sprites.rects." #field_name " must be an integer.", \
                state->manifest_path, \
                line_number, \
                1, \
                field_code, \
                sprite->id \
            ); \
            pr_manifest_mark_parse_error(state); \
            return; \
        } \
        rect->field_name = parsed; \
        rect->has_##field_name = 1; \
        return; \
    }

    PR_PARSE_RECT_INT_FIELD(x, "manifest.sprites.rects.x_invalid");
    PR_PARSE_RECT_INT_FIELD(y, "manifest.sprites.rects.y_invalid");
    PR_PARSE_RECT_INT_FIELD(w, "manifest.sprites.rects.w_invalid");
    PR_PARSE_RECT_INT_FIELD(h, "manifest.sprites.rects.h_invalid");

#undef PR_PARSE_RECT_INT_FIELD

    if (strcmp(key, "label") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "sprites.rects.label must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.sprites.rects.label_invalid",
                sprite->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(rect->label, sizeof(rect->label), parsed);
        rect->has_label = 1;
        return;
    }

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown sprites.rects key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.sprites.rects.unknown_key",
            sprite->id
        );
        pr_manifest_mark_parse_error(state);
    }
}

static void pr_manifest_parse_animation_assignment(
    pr_manifest_parse_state_t *state,
    char **lines,
    size_t line_count,
    size_t *line_index,
    const char *key,
    char *value,
    int line_number
)
{
    pr_manifest_animation_t *animation;

    if (state->current_animation >= state->manifest->animation_count) {
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            "Animation assignment without active [[animations]] block.",
            state->manifest_path,
            line_number,
            1,
            "manifest.animations.no_active_block",
            NULL
        );
        pr_manifest_mark_parse_error(state);
        return;
    }

    animation = &state->manifest->animations[state->current_animation];
    if (strcmp(key, "id") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "animations.id must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.animations.id_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(animation->id, sizeof(animation->id), parsed);
        animation->has_id = 1;
        return;
    }
    if (strcmp(key, "sprite") == 0) {
        char parsed[PR_MANIFEST_ID_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "animations.sprite must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.animations.sprite_invalid",
                NULL
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        (void)pr_manifest_copy_string(animation->sprite, sizeof(animation->sprite), parsed);
        animation->has_sprite = 1;
        return;
    }
    if (strcmp(key, "loop") == 0) {
        char parsed[PR_MANIFEST_SMALL_TEXT_MAX];

        if (!pr_manifest_parse_string_value(value, parsed, sizeof(parsed))) {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "animations.loop must be a string.",
                state->manifest_path,
                line_number,
                1,
                "manifest.animations.loop_invalid",
                animation->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }

        if (strcmp(parsed, "once") == 0) {
            animation->loop_mode = PR_LOOP_ONCE;
        } else if (strcmp(parsed, "loop") == 0) {
            animation->loop_mode = PR_LOOP_LOOP;
        } else if (strcmp(parsed, "ping_pong") == 0) {
            animation->loop_mode = PR_LOOP_PING_PONG;
        } else {
            pr_manifest_emit_diag(
                state->diag,
                PR_DIAG_ERROR,
                "animations.loop must be one of once, loop, ping_pong.",
                state->manifest_path,
                line_number,
                1,
                "manifest.animations.loop_unknown",
                animation->id
            );
            pr_manifest_mark_parse_error(state);
            return;
        }
        animation->has_loop_mode = 1;
        return;
    }
    if (strcmp(key, "frames") == 0) {
        char *array_text;

        array_text = NULL;
        if (!pr_manifest_collect_array_value(
                lines,
                line_count,
                line_index,
                value,
                &array_text,
                state->diag,
                state->manifest_path
            )) {
            pr_manifest_mark_parse_error(state);
            free(array_text);
            return;
        }

        animation->frame_count = 0u;
        if (!pr_manifest_parse_animation_frames_value(
                array_text,
                animation,
                state->diag,
                state->manifest_path,
                line_number
            )) {
            pr_manifest_mark_parse_error(state);
            free(array_text);
            return;
        }

        free(array_text);
        return;
    }

    {
        char message[128];

        (void)snprintf(message, sizeof(message), "Unknown animations key: %s", key);
        pr_manifest_emit_diag(
            state->diag,
            PR_DIAG_ERROR,
            message,
            state->manifest_path,
            line_number,
            1,
            "manifest.animations.unknown_key",
            animation->id
        );
        pr_manifest_mark_parse_error(state);
    }
}

static int pr_manifest_parse_text(
    const char *manifest_path,
    char *text,
    pr_manifest_diag_context_t *diag,
    pr_manifest_t *manifest
)
{
    pr_manifest_parse_state_t state;
    char **lines;
    size_t line_count;
    size_t i;

    if (manifest_path == NULL || text == NULL || diag == NULL || manifest == NULL) {
        return 0;
    }

    if (!pr_manifest_split_lines(text, &lines, &line_count)) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Failed to split manifest into lines.",
            manifest_path,
            1,
            1,
            "manifest.split_lines_failed",
            NULL
        );
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.manifest = manifest;
    state.diag = diag;
    state.manifest_path = manifest_path;
    state.section = PR_MANIFEST_SECTION_ROOT;
    state.current_image = (size_t)-1;
    state.current_sprite = (size_t)-1;
    state.current_rect = (size_t)-1;
    state.current_animation = (size_t)-1;

    for (i = 0u; i < line_count; ++i) {
        char *line;
        int line_number;
        pr_manifest_section_t parsed_section;

        line = lines[i];
        line_number = (int)(i + 1u);
        pr_manifest_strip_comment_inplace(line);
        line = pr_manifest_trim_inplace(line);
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == '[') {
            if (!pr_manifest_parse_section_header(line, &parsed_section)) {
                pr_manifest_emit_diag(
                    state.diag,
                    PR_DIAG_ERROR,
                    "Unknown or unsupported section header.",
                    state.manifest_path,
                    line_number,
                    1,
                    "manifest.section_unknown",
                    NULL
                );
                pr_manifest_mark_parse_error(&state);
                continue;
            }

            state.section = parsed_section;
            state.current_rect = (size_t)-1;
            if (parsed_section == PR_MANIFEST_SECTION_IMAGE) {
                pr_manifest_image_t *image;

                image = pr_manifest_push_image(state.manifest);
                if (image == NULL) {
                    pr_manifest_emit_diag(
                        state.diag,
                        PR_DIAG_ERROR,
                        "Allocation failed for [[images]] entry.",
                        state.manifest_path,
                        line_number,
                        1,
                        "manifest.images.alloc_failed",
                        NULL
                    );
                    pr_manifest_mark_parse_error(&state);
                    continue;
                }
                image->line = line_number;
                state.current_image = state.manifest->image_count - 1u;
            } else if (parsed_section == PR_MANIFEST_SECTION_SPRITE) {
                pr_manifest_sprite_t *sprite;

                sprite = pr_manifest_push_sprite(state.manifest);
                if (sprite == NULL) {
                    pr_manifest_emit_diag(
                        state.diag,
                        PR_DIAG_ERROR,
                        "Allocation failed for [[sprites]] entry.",
                        state.manifest_path,
                        line_number,
                        1,
                        "manifest.sprites.alloc_failed",
                        NULL
                    );
                    pr_manifest_mark_parse_error(&state);
                    continue;
                }
                sprite->line = line_number;
                state.current_sprite = state.manifest->sprite_count - 1u;
            } else if (parsed_section == PR_MANIFEST_SECTION_SPRITE_RECTS) {
                pr_manifest_sprite_rect_t *rect;
                pr_manifest_sprite_t *sprite;

                if (state.current_sprite >= state.manifest->sprite_count) {
                    pr_manifest_emit_diag(
                        state.diag,
                        PR_DIAG_ERROR,
                        "[[sprites.rects]] requires an active [[sprites]] entry.",
                        state.manifest_path,
                        line_number,
                        1,
                        "manifest.sprites.rects.no_parent",
                        NULL
                    );
                    pr_manifest_mark_parse_error(&state);
                    continue;
                }
                sprite = &state.manifest->sprites[state.current_sprite];
                rect = pr_manifest_push_sprite_rect(sprite);
                if (rect == NULL) {
                    pr_manifest_emit_diag(
                        state.diag,
                        PR_DIAG_ERROR,
                        "Allocation failed for [[sprites.rects]] entry.",
                        state.manifest_path,
                        line_number,
                        1,
                        "manifest.sprites.rects.alloc_failed",
                        sprite->id
                    );
                    pr_manifest_mark_parse_error(&state);
                    continue;
                }
                rect->line = line_number;
                state.current_rect = sprite->rect_count - 1u;
            } else if (parsed_section == PR_MANIFEST_SECTION_ANIMATION) {
                pr_manifest_animation_t *animation;

                animation = pr_manifest_push_animation(state.manifest);
                if (animation == NULL) {
                    pr_manifest_emit_diag(
                        state.diag,
                        PR_DIAG_ERROR,
                        "Allocation failed for [[animations]] entry.",
                        state.manifest_path,
                        line_number,
                        1,
                        "manifest.animations.alloc_failed",
                        NULL
                    );
                    pr_manifest_mark_parse_error(&state);
                    continue;
                }
                animation->line = line_number;
                state.current_animation = state.manifest->animation_count - 1u;
            }

            continue;
        }

        {
            char *key;
            char *value;

            key = NULL;
            value = NULL;
            if (!pr_manifest_split_key_value_inplace(line, &key, &value)) {
                pr_manifest_emit_diag(
                    state.diag,
                    PR_DIAG_ERROR,
                    "Invalid key/value assignment.",
                    state.manifest_path,
                    line_number,
                    1,
                    "manifest.invalid_assignment",
                    NULL
                );
                pr_manifest_mark_parse_error(&state);
                continue;
            }

            switch (state.section) {
            case PR_MANIFEST_SECTION_ROOT:
                pr_manifest_parse_root_assignment(&state, key, value, line_number);
                break;
            case PR_MANIFEST_SECTION_ATLAS:
                pr_manifest_parse_atlas_assignment(&state, key, value, line_number);
                break;
            case PR_MANIFEST_SECTION_IMAGE:
                pr_manifest_parse_image_assignment(&state, key, value, line_number);
                break;
            case PR_MANIFEST_SECTION_SPRITE:
                pr_manifest_parse_sprite_assignment(&state, key, value, line_number);
                break;
            case PR_MANIFEST_SECTION_SPRITE_RECTS:
                pr_manifest_parse_sprite_rect_assignment(&state, key, value, line_number);
                break;
            case PR_MANIFEST_SECTION_ANIMATION:
                pr_manifest_parse_animation_assignment(
                    &state,
                    lines,
                    line_count,
                    &i,
                    key,
                    value,
                    line_number
                );
                break;
            default:
                pr_manifest_mark_parse_error(&state);
                break;
            }
        }
    }

    free(lines);
    return (state.parse_error_count == 0) ? 1 : 0;
}

static long pr_manifest_find_image_index(const pr_manifest_t *manifest, const char *id)
{
    size_t i;

    if (manifest == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (manifest->images[i].has_id == 0) {
            continue;
        }
        if (strcmp(manifest->images[i].id, id) == 0) {
            return (long)i;
        }
    }

    return -1;
}

static long pr_manifest_find_sprite_index(const pr_manifest_t *manifest, const char *id)
{
    size_t i;

    if (manifest == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        if (manifest->sprites[i].has_id == 0) {
            continue;
        }
        if (strcmp(manifest->sprites[i].id, id) == 0) {
            return (long)i;
        }
    }

    return -1;
}

static void pr_manifest_validate_duplicates(
    const pr_manifest_t *manifest,
    pr_manifest_diag_context_t *diag,
    const char *manifest_path
)
{
    size_t i;
    size_t j;

    if (manifest == NULL || diag == NULL) {
        return;
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        if (manifest->images[i].has_id == 0) {
            continue;
        }
        for (j = i + 1u; j < manifest->image_count; ++j) {
            if (manifest->images[j].has_id == 0) {
                continue;
            }
            if (strcmp(manifest->images[i].id, manifest->images[j].id) == 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "Duplicate image id.",
                    manifest_path,
                    manifest->images[j].line,
                    1,
                    "manifest.images.duplicate_id",
                    manifest->images[j].id
                );
            }
        }
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        if (manifest->sprites[i].has_id == 0) {
            continue;
        }
        for (j = i + 1u; j < manifest->sprite_count; ++j) {
            if (manifest->sprites[j].has_id == 0) {
                continue;
            }
            if (strcmp(manifest->sprites[i].id, manifest->sprites[j].id) == 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "Duplicate sprite id.",
                    manifest_path,
                    manifest->sprites[j].line,
                    1,
                    "manifest.sprites.duplicate_id",
                    manifest->sprites[j].id
                );
            }
        }
    }

    for (i = 0u; i < manifest->animation_count; ++i) {
        if (manifest->animations[i].has_id == 0) {
            continue;
        }
        for (j = i + 1u; j < manifest->animation_count; ++j) {
            if (manifest->animations[j].has_id == 0) {
                continue;
            }
            if (strcmp(manifest->animations[i].id, manifest->animations[j].id) == 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "Duplicate animation id.",
                    manifest_path,
                    manifest->animations[j].line,
                    1,
                    "manifest.animations.duplicate_id",
                    manifest->animations[j].id
                );
            }
        }
    }
}

static int pr_manifest_sprite_frame_count_hint(
    const pr_manifest_sprite_t *sprite,
    int *out_count,
    int *out_exact
)
{
    if (sprite == NULL || out_count == NULL || out_exact == NULL) {
        return 0;
    }

    switch (sprite->mode) {
    case PR_MANIFEST_SPRITE_MODE_SINGLE:
        *out_count = 1;
        *out_exact = 1;
        return 1;
    case PR_MANIFEST_SPRITE_MODE_RECTS:
        *out_count = (int)sprite->rect_count;
        *out_exact = 1;
        return 1;
    case PR_MANIFEST_SPRITE_MODE_GRID:
        if (sprite->has_frame_count != 0) {
            *out_count = sprite->frame_count;
            *out_exact = 1;
        } else {
            *out_count = 0;
            *out_exact = 0;
        }
        return 1;
    default:
        *out_count = 0;
        *out_exact = 0;
        return 0;
    }
}

static void pr_manifest_validate_semantics(
    const pr_manifest_t *manifest,
    pr_manifest_diag_context_t *diag,
    const char *manifest_path
)
{
    size_t i;

    if (manifest == NULL || diag == NULL || manifest_path == NULL) {
        return;
    }

    if (manifest->has_schema_version == 0) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Missing required key: schema_version.",
            manifest_path,
            1,
            1,
            "manifest.missing_schema_version",
            NULL
        );
    } else if (manifest->schema_version != 1) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Unsupported schema_version. Expected 1.",
            manifest_path,
            1,
            1,
            "manifest.unsupported_schema_version",
            NULL
        );
    }

    if (manifest->has_package_name == 0 || manifest->package_name[0] == '\0') {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Missing required key: package_name.",
            manifest_path,
            1,
            1,
            "manifest.missing_package_name",
            NULL
        );
    }
    if (manifest->has_output == 0 || manifest->output[0] == '\0') {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "Missing required key: output.",
            manifest_path,
            1,
            1,
            "manifest.missing_output",
            NULL
        );
    }

    if (manifest->atlas.max_page_width <= 0) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "atlas.max_page_width must be > 0.",
            manifest_path,
            1,
            1,
            "manifest.atlas.max_page_width_range",
            NULL
        );
    }
    if (manifest->atlas.max_page_height <= 0) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "atlas.max_page_height must be > 0.",
            manifest_path,
            1,
            1,
            "manifest.atlas.max_page_height_range",
            NULL
        );
    }
    if (manifest->atlas.padding < 0) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "atlas.padding must be >= 0.",
            manifest_path,
            1,
            1,
            "manifest.atlas.padding_range",
            NULL
        );
    }
    if (
        strcmp(manifest->atlas.sampling, "pixel") != 0 &&
        strcmp(manifest->atlas.sampling, "linear") != 0
    ) {
        pr_manifest_emit_diag(
            diag,
            PR_DIAG_ERROR,
            "atlas.sampling must be pixel or linear.",
            manifest_path,
            1,
            1,
            "manifest.atlas.sampling_unknown",
            NULL
        );
    }

    for (i = 0u; i < manifest->image_count; ++i) {
        const pr_manifest_image_t *image;

        image = &manifest->images[i];
        if (image->has_id == 0 || image->id[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "images entry is missing id.",
                manifest_path,
                image->line,
                1,
                "manifest.images.missing_id",
                NULL
            );
        }
        if (image->has_path == 0 || image->path[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "images entry is missing path.",
                manifest_path,
                image->line,
                1,
                "manifest.images.missing_path",
                image->id
            );
        }
        if (
            strcmp(image->color_space, "srgb") != 0 &&
            strcmp(image->color_space, "linear") != 0
        ) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "images.color_space must be srgb or linear.",
                manifest_path,
                image->line,
                1,
                "manifest.images.color_space_unknown",
                image->id
            );
        }
    }

    for (i = 0u; i < manifest->sprite_count; ++i) {
        const pr_manifest_sprite_t *sprite;

        sprite = &manifest->sprites[i];
        if (sprite->has_id == 0 || sprite->id[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "sprites entry is missing id.",
                manifest_path,
                sprite->line,
                1,
                "manifest.sprites.missing_id",
                NULL
            );
        }
        if (sprite->has_source == 0 || sprite->source[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "sprites entry is missing source.",
                manifest_path,
                sprite->line,
                1,
                "manifest.sprites.missing_source",
                sprite->id
            );
        } else if (pr_manifest_find_image_index(manifest, sprite->source) < 0) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "sprites.source references unknown image id.",
                manifest_path,
                sprite->line,
                1,
                "manifest.sprites.source_unknown",
                sprite->id
            );
        }

        if (sprite->pivot_x < 0.0 || sprite->pivot_x > 1.0) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "sprites.pivot_x must be between 0 and 1.",
                manifest_path,
                sprite->line,
                1,
                "manifest.sprites.pivot_x_range",
                sprite->id
            );
        }
        if (sprite->pivot_y < 0.0 || sprite->pivot_y > 1.0) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "sprites.pivot_y must be between 0 and 1.",
                manifest_path,
                sprite->line,
                1,
                "manifest.sprites.pivot_y_range",
                sprite->id
            );
        }

        if (sprite->mode == PR_MANIFEST_SPRITE_MODE_GRID) {
            if (sprite->has_cell_w == 0 || sprite->cell_w <= 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "grid sprites require cell_w > 0.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.grid.cell_w",
                    sprite->id
                );
            }
            if (sprite->has_cell_h == 0 || sprite->cell_h <= 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "grid sprites require cell_h > 0.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.grid.cell_h",
                    sprite->id
                );
            }
            if (sprite->has_frame_start != 0 && sprite->frame_start < 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "grid sprites frame_start must be >= 0.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.grid.frame_start",
                    sprite->id
                );
            }
            if (sprite->has_frame_count != 0 && sprite->frame_count <= 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "grid sprites frame_count must be > 0 when provided.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.grid.frame_count",
                    sprite->id
                );
            }
        } else if (sprite->mode == PR_MANIFEST_SPRITE_MODE_RECTS) {
            size_t rect_index;

            if (sprite->rect_count == 0u) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "rects sprites require at least one [[sprites.rects]] entry.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.rects.empty",
                    sprite->id
                );
            }

            for (rect_index = 0u; rect_index < sprite->rect_count; ++rect_index) {
                const pr_manifest_sprite_rect_t *rect;

                rect = &sprite->rects[rect_index];
                if (rect->has_x == 0 || rect->has_y == 0 || rect->has_w == 0 || rect->has_h == 0) {
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "sprites.rects entries require x, y, w, h.",
                        manifest_path,
                        rect->line,
                        1,
                        "manifest.sprites.rects.missing_fields",
                        sprite->id
                    );
                    continue;
                }
                if (rect->x < 0 || rect->y < 0 || rect->w <= 0 || rect->h <= 0) {
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "sprites.rects values must satisfy x>=0, y>=0, w>0, h>0.",
                        manifest_path,
                        rect->line,
                        1,
                        "manifest.sprites.rects.range",
                        sprite->id
                    );
                }
            }
        } else if (sprite->mode == PR_MANIFEST_SPRITE_MODE_SINGLE) {
            if (sprite->has_w != 0 && sprite->w <= 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "single sprite w must be > 0 when provided.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.single.w_range",
                    sprite->id
                );
            }
            if (sprite->has_h != 0 && sprite->h <= 0) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "single sprite h must be > 0 when provided.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.single.h_range",
                    sprite->id
                );
            }
            if ((sprite->has_x != 0 && sprite->x < 0) || (sprite->has_y != 0 && sprite->y < 0)) {
                pr_manifest_emit_diag(
                    diag,
                    PR_DIAG_ERROR,
                    "single sprite x/y must be >= 0 when provided.",
                    manifest_path,
                    sprite->line,
                    1,
                    "manifest.sprites.single.xy_range",
                    sprite->id
                );
            }
        }
    }

    for (i = 0u; i < manifest->animation_count; ++i) {
        const pr_manifest_animation_t *animation;
        long sprite_index;

        animation = &manifest->animations[i];
        if (animation->has_id == 0 || animation->id[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "animations entry is missing id.",
                manifest_path,
                animation->line,
                1,
                "manifest.animations.missing_id",
                NULL
            );
        }
        if (animation->has_sprite == 0 || animation->sprite[0] == '\0') {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "animations entry is missing sprite reference.",
                manifest_path,
                animation->line,
                1,
                "manifest.animations.missing_sprite",
                animation->id
            );
            continue;
        }

        sprite_index = pr_manifest_find_sprite_index(manifest, animation->sprite);
        if (sprite_index < 0) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "animations.sprite references unknown sprite id.",
                manifest_path,
                animation->line,
                1,
                "manifest.animations.sprite_unknown",
                animation->id
            );
        }

        if (animation->has_frames == 0 || animation->frame_count == 0u) {
            pr_manifest_emit_diag(
                diag,
                PR_DIAG_ERROR,
                "animations.frames is required and cannot be empty.",
                manifest_path,
                animation->line,
                1,
                "manifest.animations.frames_missing",
                animation->id
            );
            continue;
        }

        {
            size_t frame_index;
            int frame_count_hint;
            int frame_count_exact;
            int warned_unknown_bound;

            warned_unknown_bound = 0;
            frame_count_hint = 0;
            frame_count_exact = 0;
            if (sprite_index >= 0) {
                (void)pr_manifest_sprite_frame_count_hint(
                    &manifest->sprites[(size_t)sprite_index],
                    &frame_count_hint,
                    &frame_count_exact
                );
            }

            for (frame_index = 0u; frame_index < animation->frame_count; ++frame_index) {
                const pr_manifest_animation_frame_t *frame;

                frame = &animation->frames[frame_index];
                if (frame->has_index == 0 || frame->index < 0) {
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "animation frame index must be >= 0.",
                        manifest_path,
                        frame->line,
                        1,
                        "manifest.animations.frame_index_range",
                        animation->id
                    );
                }
                if (frame->has_ms == 0 || frame->ms <= 0) {
                    pr_manifest_emit_diag(
                        diag,
                        PR_DIAG_ERROR,
                        "animation frame ms must be > 0.",
                        manifest_path,
                        frame->line,
                        1,
                        "manifest.animations.frame_ms_range",
                        animation->id
                    );
                }

                if (sprite_index >= 0 && frame->has_index != 0) {
                    if (frame_count_exact != 0) {
                        if (frame->index >= frame_count_hint) {
                            pr_manifest_emit_diag(
                                diag,
                                PR_DIAG_ERROR,
                                "animation frame index exceeds sprite frame count.",
                                manifest_path,
                                frame->line,
                                1,
                                "manifest.animations.frame_index_oob",
                                animation->id
                            );
                        }
                    } else if (warned_unknown_bound == 0) {
                        pr_manifest_emit_diag(
                            diag,
                            PR_DIAG_WARNING,
                            "Cannot fully validate animation frame bounds for sprite without exact frame_count.",
                            manifest_path,
                            animation->line,
                            1,
                            "manifest.animations.frame_index_unbounded",
                            animation->id
                        );
                        warned_unknown_bound = 1;
                    }
                }
            }
        }
    }

    pr_manifest_validate_duplicates(manifest, diag, manifest_path);
}

pr_status_t pr_manifest_load_and_validate(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_manifest_t *out_manifest,
    int *out_error_count,
    int *out_warning_count
)
{
    pr_manifest_diag_context_t diag;
    pr_manifest_t manifest;
    char *text;
    size_t text_size;
    pr_status_t status;

    if (out_error_count != NULL) {
        *out_error_count = 0;
    }
    if (out_warning_count != NULL) {
        *out_warning_count = 0;
    }

    if (manifest_path == NULL || manifest_path[0] == '\0' || out_manifest == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    memset(&diag, 0, sizeof(diag));
    diag.sink = diag_sink;
    diag.user_data = diag_user_data;

    pr_manifest_init(&manifest);
    text = pr_manifest_read_text_file(manifest_path, &text_size);
    if (text == NULL) {
        pr_manifest_emit_diag(
            &diag,
            PR_DIAG_ERROR,
            "Failed to read manifest file.",
            manifest_path,
            1,
            1,
            "manifest.read_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }
    if (text_size == 0u) {
        free(text);
        pr_manifest_emit_diag(
            &diag,
            PR_DIAG_ERROR,
            "Manifest file is empty.",
            manifest_path,
            1,
            1,
            "manifest.empty",
            NULL
        );
        return PR_STATUS_VALIDATION_ERROR;
    }

    if (!pr_manifest_parse_text(manifest_path, text, &diag, &manifest)) {
        free(text);
        if (out_error_count != NULL) {
            *out_error_count = diag.error_count;
        }
        if (out_warning_count != NULL) {
            *out_warning_count = diag.warning_count;
        }
        pr_manifest_free(&manifest);
        return PR_STATUS_PARSE_ERROR;
    }
    free(text);

    pr_manifest_validate_semantics(&manifest, &diag, manifest_path);
    if (diag.error_count > 0) {
        status = PR_STATUS_VALIDATION_ERROR;
        pr_manifest_free(&manifest);
    } else {
        *out_manifest = manifest;
        status = PR_STATUS_OK;
    }

    if (out_error_count != NULL) {
        *out_error_count = diag.error_count;
    }
    if (out_warning_count != NULL) {
        *out_warning_count = diag.warning_count;
    }

    return status;
}

