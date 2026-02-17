#ifndef PACKRAT_MANIFEST_H
#define PACKRAT_MANIFEST_H

#include <stddef.h>

#include "packrat/build.h"
#include "packrat/runtime.h"

#define PR_MANIFEST_ID_MAX 128u
#define PR_MANIFEST_PATH_MAX 1024u
#define PR_MANIFEST_SMALL_TEXT_MAX 32u

typedef enum pr_manifest_sprite_mode {
    PR_MANIFEST_SPRITE_MODE_SINGLE = 0,
    PR_MANIFEST_SPRITE_MODE_GRID,
    PR_MANIFEST_SPRITE_MODE_RECTS
} pr_manifest_sprite_mode_t;

typedef struct pr_manifest_sprite_rect {
    int x;
    int y;
    int w;
    int h;
    int has_x;
    int has_y;
    int has_w;
    int has_h;
    char label[PR_MANIFEST_ID_MAX];
    int has_label;
    int line;
} pr_manifest_sprite_rect_t;

typedef struct pr_manifest_image {
    char id[PR_MANIFEST_ID_MAX];
    char path[PR_MANIFEST_PATH_MAX];
    int has_id;
    int has_path;
    int premultiply_alpha;
    int has_premultiply_alpha;
    char color_space[PR_MANIFEST_SMALL_TEXT_MAX];
    int has_color_space;
    int line;
} pr_manifest_image_t;

typedef struct pr_manifest_sprite {
    char id[PR_MANIFEST_ID_MAX];
    char source[PR_MANIFEST_ID_MAX];
    int has_id;
    int has_source;
    pr_manifest_sprite_mode_t mode;
    int has_mode;
    double pivot_x;
    double pivot_y;
    int has_pivot_x;
    int has_pivot_y;
    int x;
    int y;
    int w;
    int h;
    int has_x;
    int has_y;
    int has_w;
    int has_h;
    int cell_w;
    int cell_h;
    int frame_start;
    int frame_count;
    int margin_x;
    int margin_y;
    int spacing_x;
    int spacing_y;
    int has_cell_w;
    int has_cell_h;
    int has_frame_start;
    int has_frame_count;
    int has_margin_x;
    int has_margin_y;
    int has_spacing_x;
    int has_spacing_y;
    pr_manifest_sprite_rect_t *rects;
    size_t rect_count;
    size_t rect_capacity;
    int line;
} pr_manifest_sprite_t;

typedef struct pr_manifest_animation_frame {
    int index;
    int ms;
    int has_index;
    int has_ms;
    int line;
} pr_manifest_animation_frame_t;

typedef struct pr_manifest_animation {
    char id[PR_MANIFEST_ID_MAX];
    char sprite[PR_MANIFEST_ID_MAX];
    int has_id;
    int has_sprite;
    pr_loop_mode_t loop_mode;
    int has_loop_mode;
    pr_manifest_animation_frame_t *frames;
    size_t frame_count;
    size_t frame_capacity;
    int has_frames;
    int line;
} pr_manifest_animation_t;

typedef struct pr_manifest_atlas {
    int max_page_width;
    int max_page_height;
    int padding;
    int power_of_two;
    char sampling[PR_MANIFEST_SMALL_TEXT_MAX];
    int has_max_page_width;
    int has_max_page_height;
    int has_padding;
    int has_power_of_two;
    int has_sampling;
} pr_manifest_atlas_t;

typedef struct pr_manifest {
    int schema_version;
    int has_schema_version;
    char package_name[PR_MANIFEST_ID_MAX];
    int has_package_name;
    char output[PR_MANIFEST_PATH_MAX];
    int has_output;
    char debug_output[PR_MANIFEST_PATH_MAX];
    int has_debug_output;
    int pretty_debug_json;
    int has_pretty_debug_json;
    pr_manifest_atlas_t atlas;
    pr_manifest_image_t *images;
    size_t image_count;
    size_t image_capacity;
    pr_manifest_sprite_t *sprites;
    size_t sprite_count;
    size_t sprite_capacity;
    pr_manifest_animation_t *animations;
    size_t animation_count;
    size_t animation_capacity;
} pr_manifest_t;

void pr_manifest_init(pr_manifest_t *manifest);
void pr_manifest_free(pr_manifest_t *manifest);

pr_status_t pr_manifest_load_and_validate(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_manifest_t *out_manifest,
    int *out_error_count,
    int *out_warning_count
);

#endif
