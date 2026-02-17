#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packrat/build.h"

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

static int pr_cli_run_inspect(int argc, char **argv)
{
    int i;

    if (argc < 3) {
        return pr_cli_print_usage(stderr);
    }

    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "--verbose") == 0) {
            continue;
        }
        return pr_cli_print_usage(stderr);
    }

    fprintf(
        stderr,
        "Inspect is not implemented yet for package: %s\n",
        argv[2]
    );
    return 4;
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

