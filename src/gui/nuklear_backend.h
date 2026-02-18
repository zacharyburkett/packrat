#ifndef PACKRAT_GUI_NUKLEAR_BACKEND_H
#define PACKRAT_GUI_NUKLEAR_BACKEND_H

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;

struct nk_context;
struct nk_font_atlas;

typedef enum pr_gui_status {
    PR_GUI_STATUS_OK = 0,
    PR_GUI_STATUS_INVALID_ARGUMENT,
    PR_GUI_STATUS_DEPENDENCY_ERROR,
    PR_GUI_STATUS_RUNTIME_ERROR
} pr_gui_status_t;

#define PR_GUI_NK_ANTI_ALIASING_OFF 0
#define PR_GUI_NK_ANTI_ALIASING_ON 1

pr_gui_status_t pr_gui_nk_init(SDL_Window *window, struct nk_context **out_ctx);
void pr_gui_nk_font_stash_begin(struct nk_font_atlas **atlas);
void pr_gui_nk_font_stash_end(void);
void pr_gui_nk_input_begin(void);
void pr_gui_nk_input_end(void);
int pr_gui_nk_handle_event(const SDL_Event *event);
void pr_gui_nk_render(int anti_aliasing, int max_vertex_buffer, int max_element_buffer);
void pr_gui_nk_shutdown(void);

#endif
