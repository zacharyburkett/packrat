#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "fission/nuklear.h"
#include "fission/nuklear_render.h"
#include "fission/nuklear_ui.h"
#include "packrat/gui.h"
#include "gui/nuklear_backend.h"

static int pr_gui_preview_upload_rgba8(
    void *user_data,
    int width,
    int height,
    const unsigned char *pixels,
    struct nk_image *out_image
)
{
    fission_nk_texture_t *texture;

    if (user_data == NULL || pixels == NULL || out_image == NULL || width <= 0 || height <= 0) {
        return 0;
    }

    texture = (fission_nk_texture_t *)user_data;
    return fission_nk_texture_upload_rgba8_image(
        texture,
        width,
        height,
        pixels,
        FISSION_NK_TEXTURE_SAMPLING_PIXEL_ART,
        out_image
    );
}

int main(int argc, char **argv)
{
    const char *initial_image;
    const char *initial_manifest;
    SDL_Window *window;
    SDL_GLContext gl_context;
    struct nk_context *nk_ctx;
    struct nk_font_atlas *atlas;
    SDL_Event event;
    int running;
    int i;
    pr_gui_app_t *app;
    fission_nk_texture_t preview_texture;
    pr_gui_preview_renderer_t preview_renderer;

    initial_image = NULL;
    initial_manifest = NULL;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --image\n");
                return 1;
            }
            initial_image = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--manifest") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --manifest\n");
                return 1;
            }
            initial_manifest = argv[i + 1];
            i += 1;
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        fprintf(stderr, "Usage: %s [--image <png_path>] [--manifest <packrat.toml>]\n", argv[0]);
        return 1;
    }

    window = NULL;
    gl_context = NULL;
    nk_ctx = NULL;
    app = NULL;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    window = SDL_CreateWindow(
        "Packrat Asset Tool",
        1500,
        920,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    gl_context = SDL_GL_CreateContext(window);
    if (gl_context == NULL) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_GL_MakeCurrent(window, gl_context)) {
        fprintf(stderr, "SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        (void)SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    (void)SDL_GL_SetSwapInterval(1);

    if (pr_gui_nk_init(window, &nk_ctx) != PR_GUI_STATUS_OK) {
        fprintf(stderr, "pr_gui_nk_init failed\n");
        (void)SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    atlas = NULL;
    pr_gui_nk_font_stash_begin(&atlas);
    pr_gui_nk_font_stash_end();
    (void)atlas;

    fission_nk_apply_theme(nk_ctx);

    app = pr_gui_app_create();
    if (app == NULL) {
        fprintf(stderr, "Failed to create GUI app state\n");
        pr_gui_nk_shutdown();
        (void)SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (initial_manifest != NULL) {
        pr_gui_app_set_manifest_path(app, initial_manifest);
    }
    if (initial_image != NULL) {
        pr_gui_app_set_image_path(app, initial_image);
        (void)pr_gui_app_load_image(app);
    }

    fission_nk_texture_init(&preview_texture);
    preview_renderer.user_data = &preview_texture;
    preview_renderer.upload_rgba8 = pr_gui_preview_upload_rgba8;

    running = 1;
    while (running != 0) {
        int window_width;
        int window_height;

        pr_gui_nk_input_begin();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = 0;
            }
            (void)pr_gui_nk_handle_event(&event);
        }
        pr_gui_nk_input_end();

        (void)SDL_GetWindowSize(window, &window_width, &window_height);
        pr_gui_app_draw(nk_ctx, app, window_width, window_height, &preview_renderer);

        glClearColor(0.09f, 0.11f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        pr_gui_nk_render(PR_GUI_NK_ANTI_ALIASING_ON, 1024 * 1024, 256 * 1024);
        (void)SDL_GL_SwapWindow(window);
    }

    fission_nk_texture_destroy(&preview_texture);
    pr_gui_app_destroy(app);
    pr_gui_nk_shutdown();
    (void)SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
