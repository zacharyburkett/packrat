#ifndef PACKRAT_GUI_H
#define PACKRAT_GUI_H

#include "packrat/build.h"

struct nk_context;
struct nk_image;

typedef struct pr_gui_app pr_gui_app_t;

typedef int (*pr_gui_preview_upload_rgba8_fn)(
    void *user_data,
    int width,
    int height,
    const unsigned char *pixels,
    struct nk_image *out_image
);

typedef struct pr_gui_preview_renderer {
    void *user_data;
    pr_gui_preview_upload_rgba8_fn upload_rgba8;
} pr_gui_preview_renderer_t;

pr_gui_app_t *pr_gui_app_create(void);
void pr_gui_app_destroy(pr_gui_app_t *app);

void pr_gui_app_set_image_path(pr_gui_app_t *app, const char *path);
void pr_gui_app_set_manifest_path(pr_gui_app_t *app, const char *path);
pr_status_t pr_gui_app_load_image(pr_gui_app_t *app);

/* Standalone rendering path; wraps UI in a top-level window. */
void pr_gui_app_draw(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    int window_width,
    int window_height,
    const pr_gui_preview_renderer_t *preview_renderer
);

/* Embedded rendering path; draws only UI content in the current window/group. */
void pr_gui_app_draw_embedded(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    const pr_gui_preview_renderer_t *preview_renderer
);

#endif
