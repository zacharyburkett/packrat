#include "gui/nuklear_backend.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_opengl.h>

#define NK_IMPLEMENTATION
#include "fission/nuklear.h"

struct pr_gui_nk_device {
    struct nk_buffer commands;
    struct nk_draw_null_texture null_texture;
    GLuint vbo;
    GLuint vao;
    GLuint ebo;
    GLuint program;
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLint attrib_position;
    GLint attrib_uv;
    GLint attrib_color;
    GLint uniform_texture;
    GLint uniform_projection;
    GLuint font_texture;
};

struct pr_gui_nk_vertex {
    float position[2];
    float uv[2];
    nk_byte color[4];
};

struct pr_gui_nk_backend {
    SDL_Window *window;
    struct nk_context context;
    struct nk_font_atlas atlas;
    struct pr_gui_nk_device device;
    int initialized;
};

static struct pr_gui_nk_backend PR_GUI_NK;

#define PR_GUI_NK_GL_PROC_LIST(X) \
    X(ActiveTexture, PFNGLACTIVETEXTUREPROC, "glActiveTexture") \
    X(AttachShader, PFNGLATTACHSHADERPROC, "glAttachShader") \
    X(BindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer") \
    X(BindVertexArray, PFNGLBINDVERTEXARRAYPROC, "glBindVertexArray") \
    X(BlendEquation, PFNGLBLENDEQUATIONPROC, "glBlendEquation") \
    X(BufferData, PFNGLBUFFERDATAPROC, "glBufferData") \
    X(CompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader") \
    X(CreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram") \
    X(CreateShader, PFNGLCREATESHADERPROC, "glCreateShader") \
    X(DeleteBuffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers") \
    X(DeleteProgram, PFNGLDELETEPROGRAMPROC, "glDeleteProgram") \
    X(DeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader") \
    X(DeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC, "glDeleteVertexArrays") \
    X(DetachShader, PFNGLDETACHSHADERPROC, "glDetachShader") \
    X(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray") \
    X(GenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers") \
    X(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC, "glGenVertexArrays") \
    X(GetAttribLocation, PFNGLGETATTRIBLOCATIONPROC, "glGetAttribLocation") \
    X(GetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv") \
    X(GetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv") \
    X(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation") \
    X(LinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram") \
    X(MapBuffer, PFNGLMAPBUFFERPROC, "glMapBuffer") \
    X(ShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource") \
    X(Uniform1i, PFNGLUNIFORM1IPROC, "glUniform1i") \
    X(UniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC, "glUniformMatrix4fv") \
    X(UnmapBuffer, PFNGLUNMAPBUFFERPROC, "glUnmapBuffer") \
    X(UseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram") \
    X(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer")

typedef struct pr_gui_nk_gl_procs {
#define PR_GUI_NK_GL_FIELD(field_name, field_type, field_symbol) field_type field_name;
    PR_GUI_NK_GL_PROC_LIST(PR_GUI_NK_GL_FIELD)
#undef PR_GUI_NK_GL_FIELD
} pr_gui_nk_gl_procs_t;

static pr_gui_nk_gl_procs_t PR_GUI_NK_GL;

#define glActiveTexture PR_GUI_NK_GL.ActiveTexture
#define glAttachShader PR_GUI_NK_GL.AttachShader
#define glBindBuffer PR_GUI_NK_GL.BindBuffer
#define glBindVertexArray PR_GUI_NK_GL.BindVertexArray
#define glBlendEquation PR_GUI_NK_GL.BlendEquation
#define glBufferData PR_GUI_NK_GL.BufferData
#define glCompileShader PR_GUI_NK_GL.CompileShader
#define glCreateProgram PR_GUI_NK_GL.CreateProgram
#define glCreateShader PR_GUI_NK_GL.CreateShader
#define glDeleteBuffers PR_GUI_NK_GL.DeleteBuffers
#define glDeleteProgram PR_GUI_NK_GL.DeleteProgram
#define glDeleteShader PR_GUI_NK_GL.DeleteShader
#define glDeleteVertexArrays PR_GUI_NK_GL.DeleteVertexArrays
#define glDetachShader PR_GUI_NK_GL.DetachShader
#define glEnableVertexAttribArray PR_GUI_NK_GL.EnableVertexAttribArray
#define glGenBuffers PR_GUI_NK_GL.GenBuffers
#define glGenVertexArrays PR_GUI_NK_GL.GenVertexArrays
#define glGetAttribLocation PR_GUI_NK_GL.GetAttribLocation
#define glGetProgramiv PR_GUI_NK_GL.GetProgramiv
#define glGetShaderiv PR_GUI_NK_GL.GetShaderiv
#define glGetUniformLocation PR_GUI_NK_GL.GetUniformLocation
#define glLinkProgram PR_GUI_NK_GL.LinkProgram
#define glMapBuffer PR_GUI_NK_GL.MapBuffer
#define glShaderSource PR_GUI_NK_GL.ShaderSource
#define glUniform1i PR_GUI_NK_GL.Uniform1i
#define glUniformMatrix4fv PR_GUI_NK_GL.UniformMatrix4fv
#define glUnmapBuffer PR_GUI_NK_GL.UnmapBuffer
#define glUseProgram PR_GUI_NK_GL.UseProgram
#define glVertexAttribPointer PR_GUI_NK_GL.VertexAttribPointer

static pr_gui_status_t pr_gui_nk_load_gl_procs(void)
{
    memset(&PR_GUI_NK_GL, 0, sizeof(PR_GUI_NK_GL));

#define PR_GUI_NK_GL_LOAD(field_name, field_type, field_symbol) \
    PR_GUI_NK_GL.field_name = (field_type)SDL_GL_GetProcAddress(field_symbol); \
    if (PR_GUI_NK_GL.field_name == NULL) { \
        return PR_GUI_STATUS_DEPENDENCY_ERROR; \
    }
    PR_GUI_NK_GL_PROC_LIST(PR_GUI_NK_GL_LOAD)
#undef PR_GUI_NK_GL_LOAD

    return PR_GUI_STATUS_OK;
}

#ifdef __APPLE__
#define PR_GUI_NK_SHADER_VERSION "#version 150\n"
#else
#define PR_GUI_NK_SHADER_VERSION "#version 300 es\n"
#endif

static pr_gui_status_t pr_gui_nk_device_create(struct pr_gui_nk_device *device)
{
    GLint status;
    const GLchar *vertex_shader_source =
        PR_GUI_NK_SHADER_VERSION
        "uniform mat4 ProjMtx;\n"
        "in vec2 Position;\n"
        "in vec2 TexCoord;\n"
        "in vec4 Color;\n"
        "out vec2 Frag_UV;\n"
        "out vec4 Frag_Color;\n"
        "void main() {\n"
        "    Frag_UV = TexCoord;\n"
        "    Frag_Color = Color;\n"
        "    gl_Position = ProjMtx * vec4(Position.xy, 0, 1);\n"
        "}\n";
    const GLchar *fragment_shader_source =
        PR_GUI_NK_SHADER_VERSION
        "precision mediump float;\n"
        "uniform sampler2D Texture;\n"
        "in vec2 Frag_UV;\n"
        "in vec4 Frag_Color;\n"
        "out vec4 Out_Color;\n"
        "void main() {\n"
        "    Out_Color = Frag_Color * texture(Texture, Frag_UV.st);\n"
        "}\n";
    GLsizei vertex_size;
    size_t vertex_offset_position;
    size_t vertex_offset_uv;
    size_t vertex_offset_color;

    if (device == NULL) {
        return PR_GUI_STATUS_INVALID_ARGUMENT;
    }

    nk_buffer_init_default(&device->commands);

    device->program = glCreateProgram();
    device->vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    device->fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(device->vertex_shader, 1, &vertex_shader_source, NULL);
    glShaderSource(device->fragment_shader, 1, &fragment_shader_source, NULL);

    glCompileShader(device->vertex_shader);
    glCompileShader(device->fragment_shader);

    glGetShaderiv(device->vertex_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        return PR_GUI_STATUS_RUNTIME_ERROR;
    }
    glGetShaderiv(device->fragment_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        return PR_GUI_STATUS_RUNTIME_ERROR;
    }

    glAttachShader(device->program, device->vertex_shader);
    glAttachShader(device->program, device->fragment_shader);
    glLinkProgram(device->program);

    glGetProgramiv(device->program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        return PR_GUI_STATUS_RUNTIME_ERROR;
    }

    device->uniform_texture = glGetUniformLocation(device->program, "Texture");
    device->uniform_projection = glGetUniformLocation(device->program, "ProjMtx");
    device->attrib_position = glGetAttribLocation(device->program, "Position");
    device->attrib_uv = glGetAttribLocation(device->program, "TexCoord");
    device->attrib_color = glGetAttribLocation(device->program, "Color");

    vertex_size = (GLsizei)sizeof(struct pr_gui_nk_vertex);
    vertex_offset_position = offsetof(struct pr_gui_nk_vertex, position);
    vertex_offset_uv = offsetof(struct pr_gui_nk_vertex, uv);
    vertex_offset_color = offsetof(struct pr_gui_nk_vertex, color);

    glGenBuffers(1, &device->vbo);
    glGenBuffers(1, &device->ebo);
    glGenVertexArrays(1, &device->vao);

    glBindVertexArray(device->vao);
    glBindBuffer(GL_ARRAY_BUFFER, device->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device->ebo);

    glEnableVertexAttribArray((GLuint)device->attrib_position);
    glEnableVertexAttribArray((GLuint)device->attrib_uv);
    glEnableVertexAttribArray((GLuint)device->attrib_color);

    glVertexAttribPointer(
        (GLuint)device->attrib_position,
        2,
        GL_FLOAT,
        GL_FALSE,
        vertex_size,
        (const void *)vertex_offset_position
    );
    glVertexAttribPointer(
        (GLuint)device->attrib_uv,
        2,
        GL_FLOAT,
        GL_FALSE,
        vertex_size,
        (const void *)vertex_offset_uv
    );
    glVertexAttribPointer(
        (GLuint)device->attrib_color,
        4,
        GL_UNSIGNED_BYTE,
        GL_TRUE,
        vertex_size,
        (const void *)vertex_offset_color
    );

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return PR_GUI_STATUS_OK;
}

static void pr_gui_nk_device_upload_font_atlas(
    struct pr_gui_nk_device *device,
    const void *image,
    int width,
    int height
)
{
    if (device == NULL || image == NULL || width <= 0 || height <= 0) {
        return;
    }

    glGenTextures(1, &device->font_texture);
    glBindTexture(GL_TEXTURE_2D, device->font_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        (GLsizei)width,
        (GLsizei)height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        image
    );
}

static void pr_gui_nk_device_destroy(struct pr_gui_nk_device *device)
{
    if (device == NULL) {
        return;
    }

    if (device->program != 0u) {
        if (device->vertex_shader != 0u) {
            glDetachShader(device->program, device->vertex_shader);
        }
        if (device->fragment_shader != 0u) {
            glDetachShader(device->program, device->fragment_shader);
        }
    }
    if (device->vertex_shader != 0u) {
        glDeleteShader(device->vertex_shader);
    }
    if (device->fragment_shader != 0u) {
        glDeleteShader(device->fragment_shader);
    }
    if (device->program != 0u) {
        glDeleteProgram(device->program);
    }
    if (device->font_texture != 0u) {
        glDeleteTextures(1, &device->font_texture);
    }
    if (device->vbo != 0u) {
        glDeleteBuffers(1, &device->vbo);
    }
    if (device->ebo != 0u) {
        glDeleteBuffers(1, &device->ebo);
    }
    if (device->vao != 0u) {
        glDeleteVertexArrays(1, &device->vao);
    }
    nk_buffer_free(&device->commands);

    memset(device, 0, sizeof(*device));
}

static void pr_gui_nk_clipboard_paste(nk_handle user_data, struct nk_text_edit *edit)
{
    char *text;

    (void)user_data;
    if (edit == NULL) {
        return;
    }

    text = SDL_GetClipboardText();
    if (text != NULL) {
        nk_textedit_paste(edit, text, nk_strlen(text));
        SDL_free(text);
    }
}

static void pr_gui_nk_clipboard_copy(nk_handle user_data, const char *text, int length)
{
    char *copied;

    (void)user_data;
    if (text == NULL || length <= 0) {
        return;
    }

    copied = (char *)malloc((size_t)length + 1u);
    if (copied == NULL) {
        return;
    }

    memcpy(copied, text, (size_t)length);
    copied[length] = '\0';
    (void)SDL_SetClipboardText(copied);
    free(copied);
}

pr_gui_status_t pr_gui_nk_init(SDL_Window *window, struct nk_context **out_ctx)
{
    pr_gui_status_t status;

    if (window == NULL || out_ctx == NULL) {
        return PR_GUI_STATUS_INVALID_ARGUMENT;
    }

    *out_ctx = NULL;
    memset(&PR_GUI_NK, 0, sizeof(PR_GUI_NK));

    status = pr_gui_nk_load_gl_procs();
    if (status != PR_GUI_STATUS_OK) {
        return status;
    }

    PR_GUI_NK.window = window;
    nk_init_default(&PR_GUI_NK.context, NULL);
    PR_GUI_NK.context.clip.copy = pr_gui_nk_clipboard_copy;
    PR_GUI_NK.context.clip.paste = pr_gui_nk_clipboard_paste;
    PR_GUI_NK.context.clip.userdata = nk_handle_ptr(NULL);

    status = pr_gui_nk_device_create(&PR_GUI_NK.device);
    if (status != PR_GUI_STATUS_OK) {
        nk_free(&PR_GUI_NK.context);
        memset(&PR_GUI_NK, 0, sizeof(PR_GUI_NK));
        return status;
    }

    (void)SDL_StartTextInput(window);

    PR_GUI_NK.initialized = 1;
    *out_ctx = &PR_GUI_NK.context;
    return PR_GUI_STATUS_OK;
}

void pr_gui_nk_font_stash_begin(struct nk_font_atlas **atlas)
{
    if (PR_GUI_NK.initialized == 0 || atlas == NULL) {
        return;
    }

    nk_font_atlas_init_default(&PR_GUI_NK.atlas);
    nk_font_atlas_begin(&PR_GUI_NK.atlas);
    *atlas = &PR_GUI_NK.atlas;
}

void pr_gui_nk_font_stash_end(void)
{
    const void *image;
    int width;
    int height;

    if (PR_GUI_NK.initialized == 0) {
        return;
    }

    image = nk_font_atlas_bake(&PR_GUI_NK.atlas, &width, &height, NK_FONT_ATLAS_RGBA32);
    pr_gui_nk_device_upload_font_atlas(&PR_GUI_NK.device, image, width, height);
    nk_font_atlas_end(
        &PR_GUI_NK.atlas,
        nk_handle_id((int)PR_GUI_NK.device.font_texture),
        &PR_GUI_NK.device.null_texture
    );

    if (PR_GUI_NK.atlas.default_font != NULL) {
        nk_style_set_font(&PR_GUI_NK.context, &PR_GUI_NK.atlas.default_font->handle);
    }
}

void pr_gui_nk_input_begin(void)
{
    if (PR_GUI_NK.initialized == 0) {
        return;
    }
    nk_input_begin(&PR_GUI_NK.context);
}

void pr_gui_nk_input_end(void)
{
    if (PR_GUI_NK.initialized == 0) {
        return;
    }
    nk_input_end(&PR_GUI_NK.context);
}

int pr_gui_nk_handle_event(const SDL_Event *event)
{
    struct nk_context *ctx;
    const bool *state;
    int key_down;
    SDL_Keycode key;

    if (PR_GUI_NK.initialized == 0 || event == NULL) {
        return 0;
    }

    ctx = &PR_GUI_NK.context;
    state = SDL_GetKeyboardState(NULL);

    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        key_down = (event->type == SDL_EVENT_KEY_DOWN) ? 1 : 0;
        key = event->key.key;

        if (key == SDLK_RSHIFT || key == SDLK_LSHIFT) {
            nk_input_key(ctx, NK_KEY_SHIFT, key_down);
        } else if (key == SDLK_DELETE) {
            nk_input_key(ctx, NK_KEY_DEL, key_down);
        } else if (key == SDLK_RETURN) {
            nk_input_key(ctx, NK_KEY_ENTER, key_down);
        } else if (key == SDLK_TAB) {
            nk_input_key(ctx, NK_KEY_TAB, key_down);
        } else if (key == SDLK_BACKSPACE) {
            nk_input_key(ctx, NK_KEY_BACKSPACE, key_down);
        } else if (key == SDLK_HOME) {
            nk_input_key(ctx, NK_KEY_TEXT_START, key_down);
            nk_input_key(ctx, NK_KEY_SCROLL_START, key_down);
        } else if (key == SDLK_END) {
            nk_input_key(ctx, NK_KEY_TEXT_END, key_down);
            nk_input_key(ctx, NK_KEY_SCROLL_END, key_down);
        } else if (key == SDLK_PAGEDOWN) {
            nk_input_key(ctx, NK_KEY_SCROLL_DOWN, key_down);
        } else if (key == SDLK_PAGEUP) {
            nk_input_key(ctx, NK_KEY_SCROLL_UP, key_down);
        } else if (key == SDLK_Z) {
            nk_input_key(ctx, NK_KEY_TEXT_UNDO, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_R) {
            nk_input_key(ctx, NK_KEY_TEXT_REDO, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_C) {
            nk_input_key(ctx, NK_KEY_COPY, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_V) {
            nk_input_key(ctx, NK_KEY_PASTE, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_X) {
            nk_input_key(ctx, NK_KEY_CUT, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_B) {
            nk_input_key(ctx, NK_KEY_TEXT_LINE_START, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_E) {
            nk_input_key(ctx, NK_KEY_TEXT_LINE_END, key_down && state[SDL_SCANCODE_LCTRL]);
        } else if (key == SDLK_UP) {
            nk_input_key(ctx, NK_KEY_UP, key_down);
        } else if (key == SDLK_DOWN) {
            nk_input_key(ctx, NK_KEY_DOWN, key_down);
        } else if (key == SDLK_LEFT) {
            if (state[SDL_SCANCODE_LCTRL]) {
                nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, key_down);
            } else {
                nk_input_key(ctx, NK_KEY_LEFT, key_down);
            }
        } else if (key == SDLK_RIGHT) {
            if (state[SDL_SCANCODE_LCTRL]) {
                nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, key_down);
            } else {
                nk_input_key(ctx, NK_KEY_RIGHT, key_down);
            }
        } else {
            return 0;
        }
        return 1;
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        int down;
        int x;
        int y;

        down = (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1 : 0;
        x = (int)event->button.x;
        y = (int)event->button.y;

        if (event->button.button == SDL_BUTTON_LEFT) {
            if (event->button.clicks > 1u) {
                nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, down);
            }
            nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        } else if (event->button.button == SDL_BUTTON_MIDDLE) {
            nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
        } else if (event->button.button == SDL_BUTTON_RIGHT) {
            nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);
        }
        return 1;
    }

    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        int x;
        int y;

        if (ctx->input.mouse.grabbed != 0) {
            x = (int)ctx->input.mouse.prev.x + (int)event->motion.xrel;
            y = (int)ctx->input.mouse.prev.y + (int)event->motion.yrel;
            nk_input_motion(ctx, x, y);
        } else {
            nk_input_motion(ctx, (int)event->motion.x, (int)event->motion.y);
        }
        return 1;
    }

    if (event->type == SDL_EVENT_TEXT_INPUT) {
        nk_glyph glyph;
        size_t length;

        NK_MEMSET(glyph, 0, sizeof(glyph));
        length = strlen(event->text.text);
        if (length > NK_UTF_SIZE) {
            length = NK_UTF_SIZE;
        }
        if (length > 0u) {
            memcpy(glyph, event->text.text, length);
            nk_input_glyph(ctx, glyph);
            return 1;
        }
        return 0;
    }

    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        nk_input_scroll(ctx, nk_vec2(event->wheel.x, event->wheel.y));
        return 1;
    }

    return 0;
}

void pr_gui_nk_render(int anti_aliasing, int max_vertex_buffer, int max_element_buffer)
{
    struct pr_gui_nk_device *device;
    enum nk_anti_aliasing anti_aliasing_mode;
    int width;
    int height;
    int display_width;
    int display_height;
    struct nk_vec2 scale;
    GLfloat projection[4][4] = {
        {2.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, -2.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f}
    };
    const struct nk_draw_command *command;
    void *vertices;
    void *elements;
    const nk_draw_index *offset;
    struct nk_buffer vertex_buffer;
    struct nk_buffer element_buffer;

    if (PR_GUI_NK.initialized == 0) {
        return;
    }

    anti_aliasing_mode = (anti_aliasing != 0) ? NK_ANTI_ALIASING_ON : NK_ANTI_ALIASING_OFF;

    device = &PR_GUI_NK.device;
    width = 0;
    height = 0;
    display_width = 0;
    display_height = 0;
    (void)SDL_GetWindowSize(PR_GUI_NK.window, &width, &height);
    (void)SDL_GetWindowSizeInPixels(PR_GUI_NK.window, &display_width, &display_height);
    if (width <= 0 || height <= 0 || display_width <= 0 || display_height <= 0) {
        return;
    }

    projection[0][0] /= (GLfloat)width;
    projection[1][1] /= (GLfloat)height;

    scale.x = (float)display_width / (float)width;
    scale.y = (float)display_height / (float)height;

    glViewport(0, 0, display_width, display_height);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);

    glUseProgram(device->program);
    glUniform1i(device->uniform_texture, 0);
    glUniformMatrix4fv(device->uniform_projection, 1, GL_FALSE, &projection[0][0]);

    glBindVertexArray(device->vao);
    glBindBuffer(GL_ARRAY_BUFFER, device->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device->ebo);

    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)max_vertex_buffer, NULL, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)max_element_buffer, NULL, GL_STREAM_DRAW);

    vertices = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
    if (vertices == NULL || elements == NULL) {
        if (vertices != NULL) {
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        if (elements != NULL) {
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(0);
        nk_clear(&PR_GUI_NK.context);
        nk_buffer_clear(&device->commands);
        return;
    }

    {
        struct nk_convert_config config;
        static const struct nk_draw_vertex_layout_element VERTEX_LAYOUT[] = {
            {
                NK_VERTEX_POSITION,
                NK_FORMAT_FLOAT,
                NK_OFFSETOF(struct pr_gui_nk_vertex, position)
            },
            {
                NK_VERTEX_TEXCOORD,
                NK_FORMAT_FLOAT,
                NK_OFFSETOF(struct pr_gui_nk_vertex, uv)
            },
            {
                NK_VERTEX_COLOR,
                NK_FORMAT_R8G8B8A8,
                NK_OFFSETOF(struct pr_gui_nk_vertex, color)
            },
            {NK_VERTEX_LAYOUT_END}
        };

        NK_MEMSET(&config, 0, sizeof(config));
        config.vertex_layout = VERTEX_LAYOUT;
        config.vertex_size = sizeof(struct pr_gui_nk_vertex);
        config.vertex_alignment = (nk_size)_Alignof(struct pr_gui_nk_vertex);
        config.tex_null = device->null_texture;
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.0f;
        config.shape_AA = anti_aliasing_mode;
        config.line_AA = anti_aliasing_mode;

        nk_buffer_init_fixed(&vertex_buffer, vertices, (nk_size)max_vertex_buffer);
        nk_buffer_init_fixed(&element_buffer, elements, (nk_size)max_element_buffer);
        nk_convert(
            &PR_GUI_NK.context,
            &device->commands,
            &vertex_buffer,
            &element_buffer,
            &config
        );
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);

    offset = NULL;
    nk_draw_foreach(command, &PR_GUI_NK.context, &device->commands) {
        if (command->elem_count == 0u) {
            continue;
        }

        glBindTexture(GL_TEXTURE_2D, (GLuint)command->texture.id);
        glScissor(
            (GLint)(command->clip_rect.x * scale.x),
            (GLint)((height - (int)(command->clip_rect.y + command->clip_rect.h)) * scale.y),
            (GLint)(command->clip_rect.w * scale.x),
            (GLint)(command->clip_rect.h * scale.y)
        );
        glDrawElements(
            GL_TRIANGLES,
            (GLsizei)command->elem_count,
            GL_UNSIGNED_SHORT,
            offset
        );
        offset += command->elem_count;
    }
    nk_clear(&PR_GUI_NK.context);
    nk_buffer_clear(&device->commands);

    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
}

void pr_gui_nk_shutdown(void)
{
    if (PR_GUI_NK.initialized == 0) {
        return;
    }

    (void)SDL_StopTextInput(PR_GUI_NK.window);
    nk_font_atlas_clear(&PR_GUI_NK.atlas);
    nk_free(&PR_GUI_NK.context);
    pr_gui_nk_device_destroy(&PR_GUI_NK.device);
    memset(&PR_GUI_NK, 0, sizeof(PR_GUI_NK));
    memset(&PR_GUI_NK_GL, 0, sizeof(PR_GUI_NK_GL));
}
