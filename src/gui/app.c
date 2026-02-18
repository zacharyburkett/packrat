#include "packrat/gui.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#include "fission/nuklear.h"

#define PR_GUI_TEXT_PATH_MAX 1024
#define PR_GUI_TEXT_ID_MAX 128
#define PR_GUI_STATUS_TEXT_MAX 256
#define PR_GUI_MAX_FRAMES 2048

typedef struct pr_gui_frame {
    int x;
    int y;
    int w;
    int h;
    int ms;
} pr_gui_frame_t;

typedef struct pr_gui_diag_state {
    char first_error[PR_GUI_STATUS_TEXT_MAX];
    int error_count;
    int warning_count;
} pr_gui_diag_state_t;

struct pr_gui_app {
    char image_path[PR_GUI_TEXT_PATH_MAX];
    char manifest_path[PR_GUI_TEXT_PATH_MAX];
    char package_name[PR_GUI_TEXT_ID_MAX];
    char output_path[PR_GUI_TEXT_PATH_MAX];
    char image_id[PR_GUI_TEXT_ID_MAX];
    char sprite_id[PR_GUI_TEXT_ID_MAX];
    char animation_id[PR_GUI_TEXT_ID_MAX];

    int loop_mode_index;
    int default_frame_ms;

    unsigned char *image_pixels;
    size_t image_pixel_bytes;
    int image_width;
    int image_height;
    int image_loaded;
    int image_texture_dirty;
    struct nk_image image_nk;
    int image_nk_valid;

    pr_gui_frame_t frames[PR_GUI_MAX_FRAMES];
    int frame_count;
    int selected_frame;

    int drag_active;
    int drag_start_x;
    int drag_start_y;
    int drag_current_x;
    int drag_current_y;

    struct nk_rect image_draw_rect;
    int image_draw_rect_valid;

    char status_text[PR_GUI_STATUS_TEXT_MAX];
    int status_is_error;
};

static const char *PR_GUI_LOOP_MODES[] = {
    "once",
    "loop",
    "ping_pong"
};

static int pr_gui_copy_text(char *dst, size_t dst_size, const char *src)
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

static void pr_gui_set_status(pr_gui_app_t *app, int is_error, const char *format, ...)
{
    va_list args;

    if (app == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    (void)vsnprintf(app->status_text, sizeof(app->status_text), format, args);
    va_end(args);

    app->status_is_error = (is_error != 0) ? 1 : 0;
}

static void pr_gui_diag_sink(const pr_diagnostic_t *diag, void *user_data)
{
    pr_gui_diag_state_t *state;

    if (diag == NULL || user_data == NULL) {
        return;
    }

    state = (pr_gui_diag_state_t *)user_data;
    if (diag->severity == PR_DIAG_ERROR) {
        state->error_count += 1;
        if (state->first_error[0] == '\0' && diag->message != NULL) {
            if (diag->file != NULL && diag->file[0] != '\0') {
                (void)snprintf(
                    state->first_error,
                    sizeof(state->first_error),
                    "%s (%s)",
                    diag->message,
                    diag->file
                );
            } else {
                (void)snprintf(state->first_error, sizeof(state->first_error), "%s", diag->message);
            }
        }
    } else if (diag->severity == PR_DIAG_WARNING) {
        state->warning_count += 1;
    }
}

static float pr_gui_clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int pr_gui_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
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

static int pr_gui_decode_png_rgba_file(
    const char *path,
    int *out_width,
    int *out_height,
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
    png_bytep *rows;
    size_t rows_bytes;
    size_t pixel_bytes;
    unsigned char *pixels;
    unsigned char signature[8];
    png_uint_32 y;

    if (
        path == NULL ||
        out_width == NULL ||
        out_height == NULL ||
        out_pixels == NULL ||
        out_pixel_bytes == NULL
    ) {
        return 0;
    }

    *out_width = 0;
    *out_height = 0;
    *out_pixels = NULL;
    *out_pixel_bytes = 0u;

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
        (void)fclose(file);
        return 0;
    }

    info_ptr = png_create_info_struct(png_ptr);
    end_info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL || end_info_ptr == NULL) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    rows = NULL;
    pixels = NULL;
    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
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

    if (width == 0u || height == 0u || width > INT_MAX || height > INT_MAX) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
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
    if (row_bytes == 0u) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    if (!pr_mul_size((size_t)row_bytes, (size_t)height, &pixel_bytes)) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    pixels = (unsigned char *)malloc(pixel_bytes);
    if (pixels == NULL) {
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    if (!pr_mul_size((size_t)height, sizeof(rows[0]), &rows_bytes)) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    rows = (png_bytep *)malloc(rows_bytes);
    if (rows == NULL) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
        (void)fclose(file);
        return 0;
    }

    for (y = 0u; y < height; ++y) {
        rows[y] = pixels + (size_t)y * (size_t)row_bytes;
    }

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, end_info_ptr);

    free(rows);
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info_ptr);
    (void)fclose(file);

    *out_width = (int)width;
    *out_height = (int)height;
    *out_pixels = pixels;
    *out_pixel_bytes = pixel_bytes;
    return 1;
}

static void pr_gui_release_image(pr_gui_app_t *app)
{
    if (app == NULL) {
        return;
    }

    free(app->image_pixels);
    app->image_pixels = NULL;
    app->image_pixel_bytes = 0u;
    app->image_width = 0;
    app->image_height = 0;
    app->image_loaded = 0;
    app->image_texture_dirty = 0;
    app->image_nk_valid = 0;
    app->image_draw_rect_valid = 0;
}

static void pr_gui_frame_clamp_to_image(pr_gui_frame_t *frame, int image_width, int image_height)
{
    if (frame == NULL || image_width <= 0 || image_height <= 0) {
        return;
    }

    frame->x = pr_gui_clamp_int(frame->x, 0, image_width - 1);
    frame->y = pr_gui_clamp_int(frame->y, 0, image_height - 1);
    frame->w = pr_gui_clamp_int(frame->w, 1, image_width - frame->x);
    frame->h = pr_gui_clamp_int(frame->h, 1, image_height - frame->y);
    frame->ms = pr_gui_clamp_int(frame->ms, 1, 60000);
}

static int pr_gui_add_frame(pr_gui_app_t *app, int x, int y, int w, int h, int ms)
{
    pr_gui_frame_t *frame;

    if (app == NULL || app->image_loaded == 0) {
        return 0;
    }
    if (app->frame_count >= PR_GUI_MAX_FRAMES) {
        pr_gui_set_status(app, 1, "Frame limit reached (%d).", PR_GUI_MAX_FRAMES);
        return 0;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > app->image_width) {
        w = app->image_width - x;
    }
    if (y + h > app->image_height) {
        h = app->image_height - y;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    frame = &app->frames[app->frame_count];
    frame->x = x;
    frame->y = y;
    frame->w = w;
    frame->h = h;
    frame->ms = (ms > 0) ? ms : app->default_frame_ms;
    pr_gui_frame_clamp_to_image(frame, app->image_width, app->image_height);

    app->selected_frame = app->frame_count;
    app->frame_count += 1;
    return 1;
}

static void pr_gui_remove_selected_frame(pr_gui_app_t *app)
{
    int i;

    if (app == NULL) {
        return;
    }
    if (app->selected_frame < 0 || app->selected_frame >= app->frame_count) {
        return;
    }

    for (i = app->selected_frame; i + 1 < app->frame_count; ++i) {
        app->frames[i] = app->frames[i + 1];
    }

    app->frame_count -= 1;
    if (app->frame_count <= 0) {
        app->frame_count = 0;
        app->selected_frame = -1;
    } else if (app->selected_frame >= app->frame_count) {
        app->selected_frame = app->frame_count - 1;
    }
}

static void pr_gui_clear_frames(pr_gui_app_t *app)
{
    if (app == NULL) {
        return;
    }

    app->frame_count = 0;
    app->selected_frame = -1;
}

static int pr_gui_map_screen_to_image_edge(
    const struct nk_rect *draw_rect,
    int image_width,
    int image_height,
    float screen_x,
    float screen_y,
    int *out_x,
    int *out_y
)
{
    float u;
    float v;
    int x;
    int y;

    if (
        draw_rect == NULL ||
        image_width <= 0 ||
        image_height <= 0 ||
        out_x == NULL ||
        out_y == NULL ||
        draw_rect->w <= 0.0f ||
        draw_rect->h <= 0.0f
    ) {
        return 0;
    }

    u = (screen_x - draw_rect->x) / draw_rect->w;
    v = (screen_y - draw_rect->y) / draw_rect->h;

    if (u < 0.0f) {
        u = 0.0f;
    }
    if (u > 1.0f) {
        u = 1.0f;
    }
    if (v < 0.0f) {
        v = 0.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }

    x = (int)(u * (float)image_width + 0.5f);
    y = (int)(v * (float)image_height + 0.5f);

    x = pr_gui_clamp_int(x, 0, image_width);
    y = pr_gui_clamp_int(y, 0, image_height);

    *out_x = x;
    *out_y = y;
    return 1;
}

static struct nk_rect pr_gui_fit_rect_with_aspect(
    const struct nk_rect *bounds,
    int source_width,
    int source_height
)
{
    struct nk_rect out;
    float source_aspect;
    float bounds_aspect;

    out = nk_rect(0.0f, 0.0f, 0.0f, 0.0f);
    if (
        bounds == NULL ||
        source_width <= 0 ||
        source_height <= 0 ||
        bounds->w <= 0.0f ||
        bounds->h <= 0.0f
    ) {
        return out;
    }

    source_aspect = (float)source_width / (float)source_height;
    bounds_aspect = bounds->w / bounds->h;

    if (source_aspect >= bounds_aspect) {
        out.w = bounds->w;
        out.h = bounds->w / source_aspect;
        out.x = bounds->x;
        out.y = bounds->y + (bounds->h - out.h) * 0.5f;
    } else {
        out.h = bounds->h;
        out.w = bounds->h * source_aspect;
        out.x = bounds->x + (bounds->w - out.w) * 0.5f;
        out.y = bounds->y;
    }

    return out;
}

static struct nk_rect pr_gui_frame_to_draw_rect(
    const pr_gui_frame_t *frame,
    const struct nk_rect *image_draw_rect,
    int image_width,
    int image_height
)
{
    struct nk_rect draw;

    draw = nk_rect(0.0f, 0.0f, 0.0f, 0.0f);
    if (
        frame == NULL ||
        image_draw_rect == NULL ||
        image_width <= 0 ||
        image_height <= 0 ||
        frame->w <= 0 ||
        frame->h <= 0
    ) {
        return draw;
    }

    draw.x = image_draw_rect->x + ((float)frame->x / (float)image_width) * image_draw_rect->w;
    draw.y = image_draw_rect->y + ((float)frame->y / (float)image_height) * image_draw_rect->h;
    draw.w = ((float)frame->w / (float)image_width) * image_draw_rect->w;
    draw.h = ((float)frame->h / (float)image_height) * image_draw_rect->h;

    if (draw.w < 1.0f) {
        draw.w = 1.0f;
    }
    if (draw.h < 1.0f) {
        draw.h = 1.0f;
    }
    return draw;
}

static void pr_gui_write_toml_escaped_string(FILE *file, const char *text)
{
    const unsigned char *cursor;

    if (file == NULL) {
        return;
    }

    (void)fputc('"', file);
    if (text != NULL) {
        cursor = (const unsigned char *)text;
        while (*cursor != '\0') {
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
                if (*cursor < 0x20u) {
                    (void)fprintf(file, "\\u%04x", (unsigned int)*cursor);
                } else {
                    (void)fputc((int)*cursor, file);
                }
                break;
            }
            cursor += 1;
        }
    }
    (void)fputc('"', file);
}

static pr_status_t pr_gui_save_manifest(pr_gui_app_t *app)
{
    FILE *file;
    int i;

    if (app == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (app->manifest_path[0] == '\0') {
        pr_gui_set_status(app, 1, "Manifest path is required.");
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (app->image_path[0] == '\0') {
        pr_gui_set_status(app, 1, "Image path is required.");
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (app->frame_count <= 0) {
        pr_gui_set_status(app, 1, "At least one frame is required.");
        return PR_STATUS_VALIDATION_ERROR;
    }
    if (app->image_loaded != 0) {
        for (i = 0; i < app->frame_count; ++i) {
            pr_gui_frame_clamp_to_image(&app->frames[i], app->image_width, app->image_height);
        }
    }

    file = fopen(app->manifest_path, "wb");
    if (file == NULL) {
        pr_gui_set_status(app, 1, "Could not write manifest: %s", strerror(errno));
        return PR_STATUS_IO_ERROR;
    }

    (void)fputs("schema_version = 1\n", file);
    (void)fputs("package_name = ", file);
    pr_gui_write_toml_escaped_string(file, app->package_name);
    (void)fputs("\n", file);

    (void)fputs("output = ", file);
    pr_gui_write_toml_escaped_string(file, app->output_path);
    (void)fputs("\n\n", file);

    (void)fputs("[[images]]\n", file);
    (void)fputs("id = ", file);
    pr_gui_write_toml_escaped_string(file, app->image_id);
    (void)fputs("\n", file);
    (void)fputs("path = ", file);
    pr_gui_write_toml_escaped_string(file, app->image_path);
    (void)fputs("\n\n", file);

    (void)fputs("[[sprites]]\n", file);
    (void)fputs("id = ", file);
    pr_gui_write_toml_escaped_string(file, app->sprite_id);
    (void)fputs("\n", file);
    (void)fputs("source = ", file);
    pr_gui_write_toml_escaped_string(file, app->image_id);
    (void)fputs("\n", file);
    (void)fputs("mode = \"rects\"\n\n", file);

    for (i = 0; i < app->frame_count; ++i) {
        const pr_gui_frame_t *frame;

        frame = &app->frames[i];
        (void)fputs("[[sprites.rects]]\n", file);
        (void)fprintf(file, "x = %d\n", frame->x);
        (void)fprintf(file, "y = %d\n", frame->y);
        (void)fprintf(file, "w = %d\n", frame->w);
        (void)fprintf(file, "h = %d\n", frame->h);
        (void)fprintf(file, "label = \"frame_%03d\"\n\n", i);
    }

    (void)fputs("[[animations]]\n", file);
    (void)fputs("id = ", file);
    pr_gui_write_toml_escaped_string(file, app->animation_id);
    (void)fputs("\n", file);
    (void)fputs("sprite = ", file);
    pr_gui_write_toml_escaped_string(file, app->sprite_id);
    (void)fputs("\n", file);
    (void)fputs("loop = ", file);
    pr_gui_write_toml_escaped_string(file, PR_GUI_LOOP_MODES[app->loop_mode_index]);
    (void)fputs("\n", file);
    (void)fputs("frames = [\n", file);
    for (i = 0; i < app->frame_count; ++i) {
        const pr_gui_frame_t *frame;

        frame = &app->frames[i];
        (void)fprintf(
            file,
            "  { index = %d, ms = %d }%s\n",
            i,
            frame->ms,
            (i + 1 < app->frame_count) ? "," : ""
        );
    }
    (void)fputs("]\n", file);

    if (fclose(file) != 0) {
        pr_gui_set_status(app, 1, "Failed closing manifest file: %s", strerror(errno));
        return PR_STATUS_IO_ERROR;
    }

    pr_gui_set_status(app, 0, "Saved manifest: %s", app->manifest_path);
    return PR_STATUS_OK;
}

static pr_status_t pr_gui_save_and_build_package(pr_gui_app_t *app)
{
    pr_status_t status;
    pr_build_options_t options;
    pr_build_result_t result;
    pr_gui_diag_state_t diag_state;

    if (app == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }

    status = pr_gui_save_manifest(app);
    if (status != PR_STATUS_OK) {
        return status;
    }

    memset(&options, 0, sizeof(options));
    options.manifest_path = app->manifest_path;

    memset(&result, 0, sizeof(result));
    memset(&diag_state, 0, sizeof(diag_state));

    status = pr_build_package(&options, pr_gui_diag_sink, &diag_state, &result);
    if (status != PR_STATUS_OK) {
        if (diag_state.first_error[0] != '\0') {
            pr_gui_set_status(app, 1, "Build failed: %s", diag_state.first_error);
        } else {
            pr_gui_set_status(app, 1, "Build failed: %s", pr_status_string(status));
        }
        return status;
    }

    pr_gui_set_status(
        app,
        0,
        "Built package: %s (%u sprites, %u animations)",
        (result.package_path != NULL) ? result.package_path : "<unknown>",
        result.sprite_count,
        result.animation_count
    );
    return PR_STATUS_OK;
}

static void pr_gui_draw_status(struct nk_context *ctx, pr_gui_app_t *app)
{
    int pushed_color;

    if (ctx == NULL || app == NULL || app->status_text[0] == '\0') {
        return;
    }

    pushed_color = 0;
    if (app->status_is_error != 0) {
        pushed_color = nk_style_push_color(
            ctx,
            &ctx->style.text.color,
            nk_rgba(255, 126, 126, 255)
        );
    }

    nk_layout_row_dynamic(ctx, 34.0f, 1);
    nk_label_wrap(ctx, app->status_text);

    if (pushed_color != 0) {
        (void)nk_style_pop_color(ctx);
    }
}

static void pr_gui_draw_frame_list(struct nk_context *ctx, pr_gui_app_t *app)
{
    int i;

    if (ctx == NULL || app == NULL) {
        return;
    }

    nk_layout_row_dynamic(ctx, 22.0f, 1);
    nk_label(ctx, "Frames", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 180.0f, 1);
    if (nk_group_begin(ctx, "packrat_frames_list", NK_WINDOW_BORDER) != 0) {
        for (i = 0; i < app->frame_count; ++i) {
            char label[96];
            nk_bool selected;

            selected = (i == app->selected_frame) ? nk_true : nk_false;
            (void)snprintf(
                label,
                sizeof(label),
                "#%d  x:%d y:%d w:%d h:%d  %dms",
                i,
                app->frames[i].x,
                app->frames[i].y,
                app->frames[i].w,
                app->frames[i].h,
                app->frames[i].ms
            );

            nk_layout_row_dynamic(ctx, 22.0f, 1);
            if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected) != 0) {
                app->selected_frame = i;
            }
        }

        nk_group_end(ctx);
    }

    nk_layout_row_dynamic(ctx, 26.0f, 3);
    if (nk_button_label(ctx, "Add Full") != 0) {
        if (app->image_loaded == 0) {
            pr_gui_set_status(app, 1, "Load an image first.");
        } else if (pr_gui_add_frame(
                       app,
                       0,
                       0,
                       app->image_width,
                       app->image_height,
                       app->default_frame_ms
                   ) != 0) {
            pr_gui_set_status(app, 0, "Added full-image frame.");
        }
    }

    if (nk_button_label(ctx, "Remove") != 0) {
        if (app->selected_frame >= 0 && app->selected_frame < app->frame_count) {
            pr_gui_remove_selected_frame(app);
            pr_gui_set_status(app, 0, "Removed selected frame.");
        }
    }

    if (nk_button_label(ctx, "Clear") != 0) {
        pr_gui_clear_frames(app);
        pr_gui_set_status(app, 0, "Cleared all frames.");
    }

    if (app->selected_frame >= 0 && app->selected_frame < app->frame_count) {
        pr_gui_frame_t *frame;
        int max_x;
        int max_y;
        int max_w;
        int max_h;

        frame = &app->frames[app->selected_frame];

        nk_layout_row_dynamic(ctx, 22.0f, 1);
        nk_label(ctx, "Selected Frame", NK_TEXT_LEFT);

        max_x = (app->image_width > 0) ? app->image_width - 1 : INT_MAX;
        max_y = (app->image_height > 0) ? app->image_height - 1 : INT_MAX;

        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_property_int(ctx, "x", 0, &frame->x, max_x, 1, 1);

        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_property_int(ctx, "y", 0, &frame->y, max_y, 1, 1);

        max_w = (app->image_width > 0) ? app->image_width - frame->x : INT_MAX;
        max_h = (app->image_height > 0) ? app->image_height - frame->y : INT_MAX;
        if (max_w < 1) {
            max_w = 1;
        }
        if (max_h < 1) {
            max_h = 1;
        }

        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_property_int(ctx, "w", 1, &frame->w, max_w, 1, 1);

        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_property_int(ctx, "h", 1, &frame->h, max_h, 1, 1);

        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_property_int(ctx, "ms", 1, &frame->ms, 60000, 1, 10);

        if (app->image_loaded != 0) {
            pr_gui_frame_clamp_to_image(frame, app->image_width, app->image_height);
        }
    }
}

static void pr_gui_try_upload_preview(
    pr_gui_app_t *app,
    const pr_gui_preview_renderer_t *preview_renderer
)
{
    if (
        app == NULL ||
        app->image_loaded == 0 ||
        app->image_pixels == NULL ||
        app->image_texture_dirty == 0 ||
        preview_renderer == NULL ||
        preview_renderer->upload_rgba8 == NULL
    ) {
        return;
    }

    if (preview_renderer->upload_rgba8(
            preview_renderer->user_data,
            app->image_width,
            app->image_height,
            app->image_pixels,
            &app->image_nk
        ) != 0) {
        app->image_nk_valid = 1;
        app->image_texture_dirty = 0;
    }
}

static void pr_gui_draw_drag_overlay(struct nk_command_buffer *canvas, const struct nk_rect *rect)
{
    if (canvas == NULL || rect == NULL || rect->w <= 0.0f || rect->h <= 0.0f) {
        return;
    }

    nk_stroke_rect(canvas, *rect, 0.0f, 2.0f, nk_rgba(255, 196, 76, 255));
}

static void pr_gui_update_drag_selection(struct nk_context *ctx, pr_gui_app_t *app)
{
    int hovered;
    int mouse_down;
    int mouse_pressed;

    if (
        ctx == NULL ||
        app == NULL ||
        app->image_loaded == 0 ||
        app->image_draw_rect_valid == 0
    ) {
        return;
    }

    hovered = nk_input_is_mouse_hovering_rect(&ctx->input, app->image_draw_rect);
    mouse_down = nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT);
    mouse_pressed = nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT);

    if (mouse_pressed != 0 && hovered != 0) {
        if (pr_gui_map_screen_to_image_edge(
                &app->image_draw_rect,
                app->image_width,
                app->image_height,
                ctx->input.mouse.pos.x,
                ctx->input.mouse.pos.y,
                &app->drag_start_x,
                &app->drag_start_y
            ) != 0) {
            app->drag_active = 1;
            app->drag_current_x = app->drag_start_x;
            app->drag_current_y = app->drag_start_y;
        }
    }

    if (app->drag_active == 0) {
        return;
    }

    if (mouse_down != 0) {
        (void)pr_gui_map_screen_to_image_edge(
            &app->image_draw_rect,
            app->image_width,
            app->image_height,
            ctx->input.mouse.pos.x,
            ctx->input.mouse.pos.y,
            &app->drag_current_x,
            &app->drag_current_y
        );
        return;
    }

    {
        int x0;
        int y0;
        int x1;
        int y1;
        int w;
        int h;

        app->drag_active = 0;

        x0 = (app->drag_start_x < app->drag_current_x) ? app->drag_start_x : app->drag_current_x;
        y0 = (app->drag_start_y < app->drag_current_y) ? app->drag_start_y : app->drag_current_y;
        x1 = (app->drag_start_x > app->drag_current_x) ? app->drag_start_x : app->drag_current_x;
        y1 = (app->drag_start_y > app->drag_current_y) ? app->drag_start_y : app->drag_current_y;

        w = x1 - x0;
        h = y1 - y0;

        if (w > 0 && h > 0) {
            if (pr_gui_add_frame(app, x0, y0, w, h, app->default_frame_ms) != 0) {
                pr_gui_set_status(
                    app,
                    0,
                    "Added frame #%d (%d,%d %dx%d).",
                    app->frame_count - 1,
                    x0,
                    y0,
                    w,
                    h
                );
            }
        }
    }
}

static void pr_gui_draw_preview(struct nk_context *ctx, pr_gui_app_t *app)
{
    struct nk_rect widget_bounds;
    struct nk_rect draw_rect;
    enum nk_widget_layout_states state;
    struct nk_command_buffer *canvas;
    struct nk_rect old_clip;
    int i;

    if (ctx == NULL || app == NULL) {
        return;
    }

    nk_layout_row_dynamic(ctx, 24.0f, 1);
    nk_label(
        ctx,
        "Drag inside the image to add animation frames. Selection is in image pixel space.",
        NK_TEXT_LEFT
    );

    nk_layout_row_dynamic(ctx, nk_window_get_content_region(ctx).h - 28.0f, 1);
    state = nk_widget(&widget_bounds, ctx);
    if (state == NK_WIDGET_INVALID) {
        app->image_draw_rect_valid = 0;
        return;
    }

    canvas = nk_window_get_canvas(ctx);
    if (canvas == NULL) {
        app->image_draw_rect_valid = 0;
        return;
    }

    old_clip = canvas->clip;
    nk_fill_rect(canvas, widget_bounds, 0.0f, nk_rgba(18, 21, 27, 255));
    nk_stroke_rect(canvas, widget_bounds, 0.0f, 1.0f, nk_rgba(54, 62, 74, 255));
    nk_push_scissor(canvas, widget_bounds);

    if (app->image_loaded != 0 && app->image_nk_valid != 0) {
        struct nk_rect content_bounds;

        content_bounds = widget_bounds;
        content_bounds.x += 8.0f;
        content_bounds.y += 8.0f;
        content_bounds.w -= 16.0f;
        content_bounds.h -= 16.0f;

        draw_rect = pr_gui_fit_rect_with_aspect(&content_bounds, app->image_width, app->image_height);
        app->image_draw_rect = draw_rect;
        app->image_draw_rect_valid = 1;

        nk_draw_image(canvas, draw_rect, &app->image_nk, nk_rgba(255, 255, 255, 255));

        for (i = 0; i < app->frame_count; ++i) {
            struct nk_rect frame_rect;
            struct nk_color color;
            float line_thickness;

            frame_rect = pr_gui_frame_to_draw_rect(
                &app->frames[i],
                &app->image_draw_rect,
                app->image_width,
                app->image_height
            );
            color = (i == app->selected_frame)
                ? nk_rgba(107, 214, 255, 255)
                : nk_rgba(132, 197, 120, 235);
            line_thickness = (i == app->selected_frame) ? 2.0f : 1.4f;
            nk_stroke_rect(canvas, frame_rect, 0.0f, line_thickness, color);
        }

        if (app->drag_active != 0) {
            int x0;
            int y0;
            int x1;
            int y1;
            pr_gui_frame_t drag_frame;
            struct nk_rect drag_rect;

            x0 = (app->drag_start_x < app->drag_current_x) ? app->drag_start_x : app->drag_current_x;
            y0 = (app->drag_start_y < app->drag_current_y) ? app->drag_start_y : app->drag_current_y;
            x1 = (app->drag_start_x > app->drag_current_x) ? app->drag_start_x : app->drag_current_x;
            y1 = (app->drag_start_y > app->drag_current_y) ? app->drag_start_y : app->drag_current_y;

            drag_frame.x = x0;
            drag_frame.y = y0;
            drag_frame.w = x1 - x0;
            drag_frame.h = y1 - y0;
            drag_frame.ms = app->default_frame_ms;
            drag_rect = pr_gui_frame_to_draw_rect(
                &drag_frame,
                &app->image_draw_rect,
                app->image_width,
                app->image_height
            );
            pr_gui_draw_drag_overlay(canvas, &drag_rect);
        }
    } else {
        app->image_draw_rect_valid = 0;

        if (ctx->style.font != NULL) {
            struct nk_rect text_bounds;
            const char *message;

            message = "Load a PNG image to begin selecting frames.";
            text_bounds = nk_rect(
                widget_bounds.x + 12.0f,
                widget_bounds.y + widget_bounds.h * 0.5f - ctx->style.font->height,
                widget_bounds.w - 24.0f,
                ctx->style.font->height * 2.0f
            );
            nk_draw_text(
                canvas,
                text_bounds,
                message,
                (int)strlen(message),
                ctx->style.font,
                nk_rgba(18, 21, 27, 255),
                nk_rgba(182, 192, 207, 255)
            );
        }
    }

    nk_push_scissor(canvas, old_clip);

    pr_gui_update_drag_selection(ctx, app);
}

static void pr_gui_draw_authoring_panel(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    int available_width
)
{
    if (ctx == NULL || app == NULL) {
        return;
    }

    nk_layout_row_dynamic(ctx, 22.0f, 1);
    nk_label(ctx, "Image", NK_TEXT_LEFT);

    nk_layout_row_begin(ctx, NK_STATIC, 26.0f, 2);
    nk_layout_row_push(ctx, (float)(available_width - 86));
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->image_path,
        (int)sizeof(app->image_path),
        nk_filter_default
    );
    nk_layout_row_push(ctx, 80.0f);
    if (nk_button_label(ctx, "Load") != 0) {
        (void)pr_gui_app_load_image(app);
    }
    nk_layout_row_end(ctx);

    if (app->image_loaded != 0) {
        nk_layout_row_dynamic(ctx, 20.0f, 1);
        nk_labelf(
            ctx,
            NK_TEXT_LEFT,
            "Loaded: %dx%d (%d frames)",
            app->image_width,
            app->image_height,
            app->frame_count
        );
    }

    nk_layout_row_dynamic(ctx, 8.0f, 1);
    nk_spacing(ctx, 1);

    nk_layout_row_dynamic(ctx, 22.0f, 1);
    nk_label(ctx, "Manifest", NK_TEXT_LEFT);

    nk_layout_row_begin(ctx, NK_STATIC, 26.0f, 2);
    nk_layout_row_push(ctx, (float)(available_width - 86));
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->manifest_path,
        (int)sizeof(app->manifest_path),
        nk_filter_default
    );
    nk_layout_row_push(ctx, 80.0f);
    if (nk_button_label(ctx, "Save") != 0) {
        (void)pr_gui_save_manifest(app);
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "package_name", NK_TEXT_LEFT);
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->package_name,
        (int)sizeof(app->package_name),
        nk_filter_default
    );

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "output", NK_TEXT_LEFT);
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->output_path,
        (int)sizeof(app->output_path),
        nk_filter_default
    );

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "image id", NK_TEXT_LEFT);
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->image_id,
        (int)sizeof(app->image_id),
        nk_filter_default
    );

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "sprite id", NK_TEXT_LEFT);
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->sprite_id,
        (int)sizeof(app->sprite_id),
        nk_filter_default
    );

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "animation id", NK_TEXT_LEFT);
    (void)nk_edit_string_zero_terminated(
        ctx,
        NK_EDIT_FIELD,
        app->animation_id,
        (int)sizeof(app->animation_id),
        nk_filter_default
    );

    nk_layout_row_dynamic(ctx, 24.0f, 2);
    nk_label(ctx, "loop", NK_TEXT_LEFT);
    app->loop_mode_index = nk_combo(
        ctx,
        PR_GUI_LOOP_MODES,
        (int)(sizeof(PR_GUI_LOOP_MODES) / sizeof(PR_GUI_LOOP_MODES[0])),
        app->loop_mode_index,
        24,
        nk_vec2(150.0f, 120.0f)
    );

    nk_layout_row_dynamic(ctx, 24.0f, 1);
    nk_property_int(ctx, "default frame ms", 1, &app->default_frame_ms, 60000, 1, 10);

    nk_layout_row_dynamic(ctx, 26.0f, 2);
    if (nk_button_label(ctx, "Save Manifest") != 0) {
        (void)pr_gui_save_manifest(app);
    }
    if (nk_button_label(ctx, "Save + Build") != 0) {
        (void)pr_gui_save_and_build_package(app);
    }

    nk_layout_row_dynamic(ctx, 8.0f, 1);
    nk_spacing(ctx, 1);

    pr_gui_draw_frame_list(ctx, app);
    pr_gui_draw_status(ctx, app);
}

static void pr_gui_draw_content(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    const pr_gui_preview_renderer_t *preview_renderer
)
{
    struct nk_rect content;
    float left_width;
    float right_width;
    const float gap = 10.0f;

    if (ctx == NULL || app == NULL) {
        return;
    }

    pr_gui_try_upload_preview(app, preview_renderer);
    content = nk_window_get_content_region(ctx);
    if (content.w <= 1.0f || content.h <= 1.0f) {
        return;
    }

    left_width = pr_gui_clamp_float(content.w * 0.34f, 320.0f, 460.0f);
    if (left_width > content.w - 220.0f) {
        left_width = content.w - 220.0f;
    }
    if (left_width < 220.0f) {
        left_width = 220.0f;
    }

    right_width = content.w - left_width - gap;
    if (right_width < 1.0f) {
        right_width = 1.0f;
    }

    nk_layout_row_begin(ctx, NK_STATIC, content.h, 2);
    nk_layout_row_push(ctx, left_width);
    if (nk_group_begin(ctx, "Authoring", NK_WINDOW_BORDER | NK_WINDOW_TITLE) != 0) {
        pr_gui_draw_authoring_panel(ctx, app, (int)left_width - 20);
        nk_group_end(ctx);
    }

    nk_layout_row_push(ctx, right_width);
    if (nk_group_begin(
            ctx,
            "Preview",
            NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR
        ) != 0) {
        pr_gui_draw_preview(ctx, app);
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
}

pr_gui_app_t *pr_gui_app_create(void)
{
    pr_gui_app_t *app;

    app = (pr_gui_app_t *)calloc(1u, sizeof(*app));
    if (app == NULL) {
        return NULL;
    }

    (void)pr_gui_copy_text(app->manifest_path, sizeof(app->manifest_path), "packrat.toml");
    (void)pr_gui_copy_text(app->package_name, sizeof(app->package_name), "sample_assets");
    (void)pr_gui_copy_text(app->output_path, sizeof(app->output_path), "build/assets/sample.prpk");
    (void)pr_gui_copy_text(app->image_id, sizeof(app->image_id), "sprite_sheet");
    (void)pr_gui_copy_text(app->sprite_id, sizeof(app->sprite_id), "sprite");
    (void)pr_gui_copy_text(app->animation_id, sizeof(app->animation_id), "sprite_anim");

    app->loop_mode_index = 1;
    app->default_frame_ms = 100;
    app->selected_frame = -1;

    pr_gui_set_status(app, 0, "Load a PNG image, drag to select frames, then save packrat.toml.");
    return app;
}

void pr_gui_app_destroy(pr_gui_app_t *app)
{
    if (app == NULL) {
        return;
    }

    pr_gui_release_image(app);
    free(app);
}

void pr_gui_app_set_image_path(pr_gui_app_t *app, const char *path)
{
    if (app == NULL || path == NULL) {
        return;
    }

    (void)pr_gui_copy_text(app->image_path, sizeof(app->image_path), path);
}

void pr_gui_app_set_manifest_path(pr_gui_app_t *app, const char *path)
{
    if (app == NULL || path == NULL) {
        return;
    }

    (void)pr_gui_copy_text(app->manifest_path, sizeof(app->manifest_path), path);
}

pr_status_t pr_gui_app_load_image(pr_gui_app_t *app)
{
    unsigned char *pixels;
    size_t pixel_bytes;
    int width;
    int height;

    if (app == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (app->image_path[0] == '\0') {
        pr_gui_set_status(app, 1, "Image path is required.");
        return PR_STATUS_INVALID_ARGUMENT;
    }

    pixels = NULL;
    pixel_bytes = 0u;
    width = 0;
    height = 0;

    if (!pr_gui_decode_png_rgba_file(
            app->image_path,
            &width,
            &height,
            &pixels,
            &pixel_bytes
        )) {
        pr_gui_set_status(app, 1, "Failed to load PNG: %s", app->image_path);
        return PR_STATUS_IO_ERROR;
    }

    pr_gui_release_image(app);

    app->image_pixels = pixels;
    app->image_pixel_bytes = pixel_bytes;
    app->image_width = width;
    app->image_height = height;
    app->image_loaded = 1;
    app->image_texture_dirty = 1;
    app->image_nk_valid = 0;
    app->frame_count = 0;
    app->selected_frame = -1;
    app->drag_active = 0;

    pr_gui_set_status(
        app,
        0,
        "Loaded image: %s (%dx%d). Frame list reset.",
        app->image_path,
        width,
        height
    );
    return PR_STATUS_OK;
}

void pr_gui_app_draw(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    int window_width,
    int window_height,
    const pr_gui_preview_renderer_t *preview_renderer
)
{
    struct nk_rect window_bounds;

    if (ctx == NULL || app == NULL || window_width <= 0 || window_height <= 0) {
        return;
    }

    window_bounds = nk_rect(0.0f, 0.0f, (float)window_width, (float)window_height);

    if (nk_begin(
            ctx,
            "Packrat Asset Tool",
            window_bounds,
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR
        ) == 0) {
        nk_end(ctx);
        return;
    }

    pr_gui_draw_content(ctx, app, preview_renderer);
    nk_end(ctx);
}

void pr_gui_app_draw_embedded(
    struct nk_context *ctx,
    pr_gui_app_t *app,
    const pr_gui_preview_renderer_t *preview_renderer
)
{
    if (ctx == NULL || app == NULL) {
        return;
    }

    pr_gui_draw_content(ctx, app, preview_renderer);
}
