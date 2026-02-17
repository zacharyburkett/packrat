#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packrat/build.h"
#include "packrat/runtime.h"

typedef struct pr_cli_diag_context {
    int quiet;
} pr_cli_diag_context_t;

static const char *pr_diag_severity_name(pr_diag_severity_t severity)
{
    switch (severity) {
    case PR_DIAG_ERROR:
        return "error";
    case PR_DIAG_WARNING:
        return "warning";
    case PR_DIAG_NOTE:
        return "note";
    default:
        return "unknown";
    }
}

static void pr_cli_diag_printer(const pr_diagnostic_t *diag, void *user_data)
{
    const pr_cli_diag_context_t *context;
    const char *severity;
    const char *file;
    const char *code;
    const char *asset_id;

    if (diag == NULL) {
        return;
    }

    context = (const pr_cli_diag_context_t *)user_data;
    if (context != NULL && context->quiet != 0 && diag->severity != PR_DIAG_ERROR) {
        return;
    }

    severity = pr_diag_severity_name(diag->severity);
    file = (diag->file != NULL) ? diag->file : "<unknown>";
    code = (diag->code != NULL) ? diag->code : "-";
    asset_id = (diag->asset_id != NULL) ? diag->asset_id : "-";

    if (diag->line > 0 || diag->column > 0) {
        fprintf(
            stderr,
            "%s: %s:%d:%d: %s [code=%s] [asset=%s]\n",
            severity,
            file,
            diag->line,
            diag->column,
            diag->message,
            code,
            asset_id
        );
    } else {
        fprintf(
            stderr,
            "%s: %s: %s [code=%s] [asset=%s]\n",
            severity,
            file,
            diag->message,
            code,
            asset_id
        );
    }
}

static int pr_cli_exit_code_for_status(pr_status_t status)
{
    switch (status) {
    case PR_STATUS_OK:
        return 0;
    case PR_STATUS_INVALID_ARGUMENT:
        return 1;
    case PR_STATUS_VALIDATION_ERROR:
    case PR_STATUS_PARSE_ERROR:
        return 2;
    case PR_STATUS_IO_ERROR:
        return 3;
    default:
        return 4;
    }
}

static int pr_cli_print_usage(FILE *stream)
{
    if (stream == NULL) {
        return 1;
    }

    fprintf(stream, "Usage:\n");
    fprintf(stream, "  packrat validate <manifest>\n");
    fprintf(stream, "  packrat build <manifest> [options]\n");
    fprintf(stream, "  packrat inspect <package> [options]\n");
    fprintf(stream, "\n");
    fprintf(stream, "Build options:\n");
    fprintf(stream, "  --output <path>\n");
    fprintf(stream, "  --debug-output <path>\n");
    fprintf(stream, "  --pretty-debug-json\n");
    fprintf(stream, "  --quiet\n");
    fprintf(stream, "  --strict\n");
    fprintf(stream, "\n");
    fprintf(stream, "Inspect options:\n");
    fprintf(stream, "  --json\n");
    fprintf(stream, "  --verbose\n");
    return 1;
}

static int pr_cli_run_validate(int argc, char **argv)
{
    pr_status_t status;
    pr_cli_diag_context_t diag_context;

    if (argc != 3) {
        return pr_cli_print_usage(stderr);
    }

    memset(&diag_context, 0, sizeof(diag_context));
    status = pr_validate_manifest_file(argv[2], pr_cli_diag_printer, &diag_context);
    if (status == PR_STATUS_OK) {
        fprintf(stdout, "Manifest is valid: %s\n", argv[2]);
    } else {
        fprintf(stderr, "Validate failed: %s\n", pr_status_string(status));
    }
    return pr_cli_exit_code_for_status(status);
}

static int pr_cli_run_build(int argc, char **argv)
{
    pr_status_t status;
    pr_build_options_t options;
    pr_build_result_t result;
    pr_cli_diag_context_t diag_context;
    int i;

    if (argc < 3) {
        return pr_cli_print_usage(stderr);
    }

    memset(&options, 0, sizeof(options));
    options.manifest_path = argv[2];

    memset(&diag_context, 0, sizeof(diag_context));

    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                return pr_cli_print_usage(stderr);
            }
            options.output_override = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--debug-output") == 0) {
            if (i + 1 >= argc) {
                return pr_cli_print_usage(stderr);
            }
            options.debug_output_override = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--pretty-debug-json") == 0) {
            options.pretty_debug_json = 1;
            continue;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            diag_context.quiet = 1;
            continue;
        }
        if (strcmp(argv[i], "--strict") == 0) {
            options.strict_mode = 1;
            continue;
        }

        return pr_cli_print_usage(stderr);
    }

    memset(&result, 0, sizeof(result));
    status = pr_build_package(&options, pr_cli_diag_printer, &diag_context, &result);
    if (status == PR_STATUS_OK) {
        fprintf(stdout, "Build succeeded: %s\n", result.package_path);
    } else {
        fprintf(stderr, "Build failed: %s\n", pr_status_string(status));
    }
    return pr_cli_exit_code_for_status(status);
}

static const char *pr_cli_loop_mode_name(pr_loop_mode_t mode)
{
    switch (mode) {
    case PR_LOOP_ONCE:
        return "once";
    case PR_LOOP_LOOP:
        return "loop";
    case PR_LOOP_PING_PONG:
        return "ping_pong";
    default:
        return "unknown";
    }
}

static void pr_cli_json_escaped(FILE *file, const char *text)
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

static unsigned int pr_cli_animation_total_ms(const pr_animation_t *animation)
{
    unsigned int total;
    unsigned int i;

    if (animation == NULL || animation->frames == NULL) {
        return 0u;
    }

    total = 0u;
    for (i = 0u; i < animation->frame_count; ++i) {
        total += animation->frames[i].duration_ms;
    }
    return total;
}

static void pr_cli_print_inspect_text(
    const char *package_path,
    const pr_package_t *package,
    int verbose
)
{
    unsigned int page_count;
    unsigned int sprite_count;
    unsigned int animation_count;
    unsigned int i;

    page_count = pr_package_atlas_page_count(package);
    sprite_count = pr_package_sprite_count(package);
    animation_count = pr_package_animation_count(package);

    fprintf(stdout, "Package: %s\n", package_path);
    fprintf(stdout, "Atlas pages: %u\n", page_count);
    fprintf(stdout, "Sprites: %u\n", sprite_count);
    fprintf(stdout, "Animations: %u\n", animation_count);

    if (verbose == 0) {
        return;
    }

    fprintf(stdout, "\nAtlas:\n");
    for (i = 0u; i < page_count; ++i) {
        unsigned int width;
        unsigned int height;
        unsigned int stride;
        const unsigned char *pixels;

        width = 0u;
        height = 0u;
        stride = 0u;
        pixels = pr_package_atlas_page_pixels(package, i, &width, &height, &stride);
        fprintf(
            stdout,
            "  [%u] %ux%u stride=%u pixels=%s\n",
            i,
            width,
            height,
            stride,
            (pixels != NULL) ? "yes" : "no"
        );
    }

    fprintf(stdout, "\nSprites:\n");
    for (i = 0u; i < sprite_count; ++i) {
        const pr_sprite_t *sprite;
        unsigned int j;

        sprite = pr_package_sprite_at(package, i);
        if (sprite == NULL) {
            continue;
        }

        fprintf(
            stdout,
            "  [%u] id=%s frames=%u\n",
            i,
            (sprite->id != NULL) ? sprite->id : "<null>",
            sprite->frame_count
        );

        for (j = 0u; j < sprite->frame_count; ++j) {
            const pr_sprite_frame_t *frame;

            frame = &sprite->frames[j];
            fprintf(
                stdout,
                "    frame[%u] page=%u rect=(%u,%u,%u,%u) uv=(%.4f,%.4f)-(%.4f,%.4f)\n",
                j,
                frame->atlas_page,
                frame->x,
                frame->y,
                frame->w,
                frame->h,
                (double)frame->u0,
                (double)frame->v0,
                (double)frame->u1,
                (double)frame->v1
            );
        }
    }

    fprintf(stdout, "\nAnimations:\n");
    for (i = 0u; i < animation_count; ++i) {
        const pr_animation_t *animation;
        unsigned int j;

        animation = pr_package_animation_at(package, i);
        if (animation == NULL) {
            continue;
        }

        fprintf(
            stdout,
            "  [%u] id=%s sprite=%s loop=%s frames=%u total_ms=%u\n",
            i,
            (animation->id != NULL) ? animation->id : "<null>",
            (animation->sprite != NULL && animation->sprite->id != NULL) ?
                animation->sprite->id : "<null>",
            pr_cli_loop_mode_name(animation->loop_mode),
            animation->frame_count,
            pr_cli_animation_total_ms(animation)
        );

        for (j = 0u; j < animation->frame_count; ++j) {
            const pr_anim_frame_t *frame;

            frame = &animation->frames[j];
            fprintf(
                stdout,
                "    key[%u] sprite_frame=%u ms=%u\n",
                j,
                frame->sprite_frame_index,
                frame->duration_ms
            );
        }
    }
}

static void pr_cli_print_inspect_json(
    const char *package_path,
    const pr_package_t *package,
    int verbose
)
{
    unsigned int page_count;
    unsigned int sprite_count;
    unsigned int animation_count;
    unsigned int i;

    page_count = pr_package_atlas_page_count(package);
    sprite_count = pr_package_sprite_count(package);
    animation_count = pr_package_animation_count(package);

    (void)fputs("{\"package\":\"", stdout);
    pr_cli_json_escaped(stdout, package_path);
    (void)fprintf(
        stdout,
        "\",\"atlas_pages\":%u,\"sprite_count\":%u,\"animation_count\":%u",
        page_count,
        sprite_count,
        animation_count
    );

    if (verbose == 0) {
        (void)fputs("}\n", stdout);
        return;
    }

    (void)fputs(",\"atlas\":[", stdout);
    for (i = 0u; i < page_count; ++i) {
        unsigned int width;
        unsigned int height;
        unsigned int stride;
        const unsigned char *pixels;

        if (i > 0u) {
            (void)fputc(',', stdout);
        }
        width = 0u;
        height = 0u;
        stride = 0u;
        pixels = pr_package_atlas_page_pixels(package, i, &width, &height, &stride);
        (void)fprintf(
            stdout,
            "{\"index\":%u,\"width\":%u,\"height\":%u,\"stride\":%u,\"has_pixels\":%s}",
            i,
            width,
            height,
            stride,
            (pixels != NULL) ? "true" : "false"
        );
    }
    (void)fputs("]", stdout);

    (void)fputs(",\"sprites\":[", stdout);
    for (i = 0u; i < sprite_count; ++i) {
        const pr_sprite_t *sprite;
        unsigned int j;

        sprite = pr_package_sprite_at(package, i);
        if (i > 0u) {
            (void)fputc(',', stdout);
        }

        (void)fputs("{\"id\":\"", stdout);
        pr_cli_json_escaped(stdout, (sprite != NULL && sprite->id != NULL) ? sprite->id : "");
        if (sprite == NULL) {
            (void)fputs("\",\"frame_count\":0,\"frames\":[]}", stdout);
            continue;
        }

        (void)fprintf(stdout, "\",\"frame_count\":%u,\"frames\":[", sprite->frame_count);
        for (j = 0u; j < sprite->frame_count; ++j) {
            const pr_sprite_frame_t *frame;

            frame = &sprite->frames[j];
            if (j > 0u) {
                (void)fputc(',', stdout);
            }
            (void)fprintf(
                stdout,
                "{\"index\":%u,\"atlas_page\":%u,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,"
                "\"u0\":%.6f,\"v0\":%.6f,\"u1\":%.6f,\"v1\":%.6f,"
                "\"pivot_x\":%.3f,\"pivot_y\":%.3f}",
                j,
                frame->atlas_page,
                frame->x,
                frame->y,
                frame->w,
                frame->h,
                (double)frame->u0,
                (double)frame->v0,
                (double)frame->u1,
                (double)frame->v1,
                (double)frame->pivot_x,
                (double)frame->pivot_y
            );
        }
        (void)fputs("]}", stdout);
    }
    (void)fputs("]", stdout);

    (void)fputs(",\"animations\":[", stdout);
    for (i = 0u; i < animation_count; ++i) {
        const pr_animation_t *animation;
        unsigned int j;

        animation = pr_package_animation_at(package, i);
        if (i > 0u) {
            (void)fputc(',', stdout);
        }
        if (animation == NULL) {
            (void)fputs(
                "{\"id\":\"\",\"sprite\":\"\",\"loop\":\"unknown\","
                "\"frame_count\":0,\"total_ms\":0,\"frames\":[]}",
                stdout
            );
            continue;
        }

        (void)fputs("{\"id\":\"", stdout);
        pr_cli_json_escaped(stdout, (animation->id != NULL) ? animation->id : "");
        (void)fputs("\",\"sprite\":\"", stdout);
        pr_cli_json_escaped(
            stdout,
            (animation->sprite != NULL && animation->sprite->id != NULL) ?
                animation->sprite->id : ""
        );
        (void)fprintf(
            stdout,
            "\",\"loop\":\"%s\",\"frame_count\":%u,\"total_ms\":%u,\"frames\":[",
            pr_cli_loop_mode_name(animation->loop_mode),
            animation->frame_count,
            pr_cli_animation_total_ms(animation)
        );

        for (j = 0u; j < animation->frame_count; ++j) {
            const pr_anim_frame_t *frame;

            frame = &animation->frames[j];
            if (j > 0u) {
                (void)fputc(',', stdout);
            }
            (void)fprintf(
                stdout,
                "{\"index\":%u,\"sprite_frame\":%u,\"ms\":%u}",
                j,
                frame->sprite_frame_index,
                frame->duration_ms
            );
        }
        (void)fputs("]}", stdout);
    }
    (void)fputs("]}\n", stdout);
}

static int pr_cli_run_inspect(int argc, char **argv)
{
    pr_package_t *package;
    pr_status_t status;
    int json_output;
    int verbose;
    int i;

    if (argc < 3) {
        return pr_cli_print_usage(stderr);
    }

    package = NULL;
    json_output = 0;
    verbose = 0;

    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
            continue;
        }
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            continue;
        }
        return pr_cli_print_usage(stderr);
    }

    status = pr_package_open_file(argv[2], &package);
    if (status != PR_STATUS_OK) {
        fprintf(stderr, "Inspect failed: %s\n", pr_status_string(status));
        return pr_cli_exit_code_for_status(status);
    }

    if (json_output != 0) {
        pr_cli_print_inspect_json(argv[2], package, verbose);
    } else {
        pr_cli_print_inspect_text(argv[2], package, verbose);
    }

    pr_package_close(package);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return pr_cli_print_usage(stderr);
    }

    if (strcmp(argv[1], "validate") == 0) {
        return pr_cli_run_validate(argc, argv);
    }
    if (strcmp(argv[1], "build") == 0) {
        return pr_cli_run_build(argc, argv);
    }
    if (strcmp(argv[1], "inspect") == 0) {
        return pr_cli_run_inspect(argc, argv);
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        return pr_cli_print_usage(stdout);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return pr_cli_print_usage(stderr);
}
