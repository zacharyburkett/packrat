#include "packrat/build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pr_emit_diag(
    pr_diag_sink_fn sink,
    void *user_data,
    pr_diag_severity_t severity,
    const char *message,
    const char *file,
    const char *code,
    const char *asset_id
)
{
    pr_diagnostic_t diag;

    if (sink == NULL || message == NULL) {
        return;
    }

    memset(&diag, 0, sizeof(diag));
    diag.severity = severity;
    diag.message = message;
    diag.file = file;
    diag.code = code;
    diag.asset_id = asset_id;
    diag.line = 0;
    diag.column = 0;
    sink(&diag, user_data);
}

static char *pr_read_text_file(const char *path, size_t *out_size)
{
    FILE *file;
    char *buffer;
    size_t read_size;
    long size;

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

    size = ftell(file);
    if (size < 0) {
        (void)fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1u, (size_t)size, file);
    (void)fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[read_size] = '\0';
    *out_size = read_size;
    return buffer;
}

static const char *pr_skip_space(const char *cursor, const char *line_end)
{
    while (cursor < line_end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) {
        cursor += 1;
    }
    return cursor;
}

static int pr_line_matches_key(
    const char *line_start,
    const char *line_end,
    const char *key
)
{
    const char *cursor;
    size_t key_len;

    if (line_start == NULL || line_end == NULL || key == NULL || line_start >= line_end) {
        return 0;
    }

    cursor = pr_skip_space(line_start, line_end);
    if (cursor >= line_end || *cursor == '#') {
        return 0;
    }

    key_len = strlen(key);
    if ((size_t)(line_end - cursor) < key_len) {
        return 0;
    }
    if (strncmp(cursor, key, key_len) != 0) {
        return 0;
    }

    cursor += key_len;
    cursor = pr_skip_space(cursor, line_end);
    return (cursor < line_end && *cursor == '=') ? 1 : 0;
}

static int pr_extract_quoted_string(
    const char *content,
    const char *key,
    char *out_value,
    size_t out_value_size
)
{
    const char *line_start;
    const char *line_end;

    if (
        content == NULL ||
        key == NULL ||
        out_value == NULL ||
        out_value_size == 0u
    ) {
        return -1;
    }

    out_value[0] = '\0';
    line_start = content;
    while (*line_start != '\0') {
        const char *cursor;
        size_t copy_len;

        line_end = line_start;
        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }

        if (!pr_line_matches_key(line_start, line_end, key)) {
            line_start = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }

        cursor = pr_skip_space(line_start, line_end);
        cursor += strlen(key);
        cursor = pr_skip_space(cursor, line_end);
        if (cursor >= line_end || *cursor != '=') {
            return -1;
        }
        cursor += 1;
        cursor = pr_skip_space(cursor, line_end);
        if (cursor >= line_end || *cursor != '"') {
            return -1;
        }
        cursor += 1;

        line_end = cursor;
        while (*line_end != '\0' && *line_end != '"' && *line_end != '\n') {
            line_end += 1;
        }
        if (*line_end != '"') {
            return -1;
        }

        copy_len = (size_t)(line_end - cursor);
        if (copy_len >= out_value_size) {
            copy_len = out_value_size - 1u;
        }
        memcpy(out_value, cursor, copy_len);
        out_value[copy_len] = '\0';
        return 1;
    }

    return 0;
}

static int pr_extract_int(
    const char *content,
    const char *key,
    int *out_value
)
{
    const char *line_start;
    const char *line_end;

    if (content == NULL || key == NULL || out_value == NULL) {
        return -1;
    }

    line_start = content;
    while (*line_start != '\0') {
        const char *cursor;
        char *endptr;
        long value;

        line_end = line_start;
        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }

        if (!pr_line_matches_key(line_start, line_end, key)) {
            line_start = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }

        cursor = pr_skip_space(line_start, line_end);
        cursor += strlen(key);
        cursor = pr_skip_space(cursor, line_end);
        if (cursor >= line_end || *cursor != '=') {
            return -1;
        }
        cursor += 1;
        cursor = pr_skip_space(cursor, line_end);
        if (cursor >= line_end) {
            return -1;
        }

        value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            return -1;
        }
        *out_value = (int)value;
        return 1;
    }

    return 0;
}

static int pr_has_prpk_extension(const char *path)
{
    size_t length;

    if (path == NULL) {
        return 0;
    }

    length = strlen(path);
    if (length < 5u) {
        return 0;
    }

    return (strcmp(path + (length - 5u), ".prpk") == 0) ? 1 : 0;
}

pr_status_t pr_validate_manifest_file(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    char *content;
    size_t content_size;
    int schema_version;
    int result;
    int error_count;
    char package_name[256];
    char output_path[512];

    if (manifest_path == NULL || manifest_path[0] == '\0') {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Manifest path is required.",
            manifest_path,
            "manifest.path_required",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    content = pr_read_text_file(manifest_path, &content_size);
    if (content == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to read manifest file.",
            manifest_path,
            "manifest.read_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }
    if (content_size == 0u) {
        free(content);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Manifest file is empty.",
            manifest_path,
            "manifest.empty",
            NULL
        );
        return PR_STATUS_VALIDATION_ERROR;
    }

    error_count = 0;

    schema_version = 0;
    result = pr_extract_int(content, "schema_version", &schema_version);
    if (result == 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Missing required key: schema_version.",
            manifest_path,
            "manifest.missing_schema_version",
            NULL
        );
        error_count += 1;
    } else if (result < 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Invalid schema_version value.",
            manifest_path,
            "manifest.invalid_schema_version",
            NULL
        );
        free(content);
        return PR_STATUS_PARSE_ERROR;
    } else if (schema_version != 1) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Unsupported schema_version. Expected 1.",
            manifest_path,
            "manifest.unsupported_schema_version",
            NULL
        );
        error_count += 1;
    }

    result = pr_extract_quoted_string(content, "package_name", package_name, sizeof(package_name));
    if (result == 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Missing required key: package_name.",
            manifest_path,
            "manifest.missing_package_name",
            NULL
        );
        error_count += 1;
    } else if (result < 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Invalid package_name value.",
            manifest_path,
            "manifest.invalid_package_name",
            NULL
        );
        free(content);
        return PR_STATUS_PARSE_ERROR;
    } else if (package_name[0] == '\0') {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "package_name cannot be empty.",
            manifest_path,
            "manifest.empty_package_name",
            NULL
        );
        error_count += 1;
    }

    result = pr_extract_quoted_string(content, "output", output_path, sizeof(output_path));
    if (result == 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Missing required key: output.",
            manifest_path,
            "manifest.missing_output",
            NULL
        );
        error_count += 1;
    } else if (result < 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Invalid output value.",
            manifest_path,
            "manifest.invalid_output",
            NULL
        );
        free(content);
        return PR_STATUS_PARSE_ERROR;
    } else if (output_path[0] == '\0') {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "output cannot be empty.",
            manifest_path,
            "manifest.empty_output",
            NULL
        );
        error_count += 1;
    } else if (!pr_has_prpk_extension(output_path)) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_WARNING,
            "output path does not use .prpk extension.",
            manifest_path,
            "manifest.output_extension",
            NULL
        );
    }

    free(content);

    if (error_count > 0) {
        return PR_STATUS_VALIDATION_ERROR;
    }

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_NOTE,
        "Manifest validated successfully.",
        manifest_path,
        "manifest.valid",
        NULL
    );
    return PR_STATUS_OK;
}

pr_status_t pr_build_package(
    const pr_build_options_t *options,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_build_result_t *out_result
)
{
    pr_status_t status;

    if (options == NULL || out_result == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Build options and result pointer are required.",
            NULL,
            "build.invalid_arguments",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    memset(out_result, 0, sizeof(*out_result));
    status = pr_validate_manifest_file(options->manifest_path, diag_sink, diag_user_data);
    if (status != PR_STATUS_OK) {
        return status;
    }

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_ERROR,
        "Build pipeline is not implemented yet.",
        options->manifest_path,
        "build.not_implemented",
        NULL
    );
    return PR_STATUS_INTERNAL_ERROR;
}

