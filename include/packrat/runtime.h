#ifndef PACKRAT_RUNTIME_H
#define PACKRAT_RUNTIME_H

#include <stddef.h>

#include "packrat/build.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pr_package pr_package_t;

typedef enum pr_loop_mode {
    PR_LOOP_ONCE = 0,
    PR_LOOP_LOOP,
    PR_LOOP_PING_PONG
} pr_loop_mode_t;

typedef struct pr_sprite_frame {
    unsigned int atlas_page;
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    float u0;
    float v0;
    float u1;
    float v1;
    float pivot_x;
    float pivot_y;
} pr_sprite_frame_t;

typedef struct pr_sprite {
    const char *id;
    unsigned int frame_count;
    const pr_sprite_frame_t *frames;
} pr_sprite_t;

typedef struct pr_anim_frame {
    unsigned int sprite_frame_index;
    unsigned int duration_ms;
} pr_anim_frame_t;

typedef struct pr_animation {
    const char *id;
    const pr_sprite_t *sprite;
    pr_loop_mode_t loop_mode;
    unsigned int frame_count;
    const pr_anim_frame_t *frames;
} pr_animation_t;

pr_status_t pr_package_open_file(const char *path, pr_package_t **out_package);
pr_status_t pr_package_open_memory(
    const void *data,
    size_t size,
    pr_package_t **out_package
);
void pr_package_close(pr_package_t *package);

const pr_sprite_t *pr_package_find_sprite(
    const pr_package_t *package,
    const char *sprite_id
);

const pr_animation_t *pr_package_find_animation(
    const pr_package_t *package,
    const char *animation_id
);

unsigned int pr_package_atlas_page_count(const pr_package_t *package);

unsigned int pr_package_sprite_count(const pr_package_t *package);
const pr_sprite_t *pr_package_sprite_at(
    const pr_package_t *package,
    unsigned int index
);

unsigned int pr_package_animation_count(const pr_package_t *package);
const pr_animation_t *pr_package_animation_at(
    const pr_package_t *package,
    unsigned int index
);

#ifdef __cplusplus
}
#endif

#endif
