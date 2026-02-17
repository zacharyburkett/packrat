#include "packrat/build.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "manifest.h"

typedef struct pr_build_result_storage {
    char package_path[PR_MANIFEST_PATH_MAX];
    char debug_output_path[PR_MANIFEST_PATH_MAX];
} pr_build_result_storage_t;

static pr_build_result_storage_t PR_BUILD_RESULT_STORAGE;

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
    sink(&diag, user_data);
}

static int pr_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t length;

    if (dst == NULL || dst_size == 0u || src == NULL) {
        return 0;
    }

    length = strlen(src);
    if (length >= dst_size) {
        return 0;
    }

    memcpy(dst, src, length + 1u);
    return 1;
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

static int pr_is_path_separator(char ch)
{
    return (ch == '/' || ch == '\\') ? 1 : 0;
}

static int pr_make_directory_if_missing(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 1;
    }

#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return 1;
    }
#else
    if (mkdir(path, 0755) == 0) {
        return 1;
    }
#endif

    return (errno == EEXIST) ? 1 : 0;
}

static int pr_ensure_parent_directories(const char *file_path)
{
    char path_copy[PR_MANIFEST_PATH_MAX];
    size_t i;
    size_t length;

    if (file_path == NULL || file_path[0] == '\0') {
        return 0;
    }
    length = strlen(file_path);
    if (length >= sizeof(path_copy)) {
        return 0;
    }

    memcpy(path_copy, file_path, length + 1u);
    for (i = 1u; i < length; ++i) {
        char saved;

        if (!pr_is_path_separator(path_copy[i])) {
            continue;
        }
        saved = path_copy[i];
        path_copy[i] = '\0';
        if (path_copy[0] != '\0') {
            if (!pr_make_directory_if_missing(path_copy)) {
                return 0;
            }
        }
        path_copy[i] = saved;
    }

    return 1;
}

static int pr_write_u16_le(FILE *file, uint16_t value)
{
    unsigned char bytes[2];

    if (file == NULL) {
        return 0;
    }

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static int pr_write_u32_le(FILE *file, uint32_t value)
{
    unsigned char bytes[4];

    if (file == NULL) {
        return 0;
    }

    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8u) & 0xFFu);
    bytes[2] = (unsigned char)((value >> 16u) & 0xFFu);
    bytes[3] = (unsigned char)((value >> 24u) & 0xFFu);
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static int pr_write_u64_le(FILE *file, uint64_t value)
{
    unsigned char bytes[8];
    size_t i;

    if (file == NULL) {
        return 0;
    }

    for (i = 0u; i < 8u; ++i) {
        bytes[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    }
    return (fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes)) ? 1 : 0;
}

static pr_status_t pr_write_package_skeleton(
    const char *output_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    FILE *file;
    const uint16_t version_major = 1u;
    const uint16_t version_minor = 0u;
    const uint32_t header_size = 24u;
    const uint32_t chunk_count = 0u;
    const uint64_t chunk_table_offset = 24u;

    if (output_path == NULL || output_path[0] == '\0') {
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (!pr_ensure_parent_directories(output_path)) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to create output directory path.",
            output_path,
            "build.output_dir_create_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    file = fopen(output_path, "wb");
    if (file == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to open package output file.",
            output_path,
            "build.output_open_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    if (
        fwrite("PRPK", 1u, 4u, file) != 4u ||
        !pr_write_u16_le(file, version_major) ||
        !pr_write_u16_le(file, version_minor) ||
        !pr_write_u32_le(file, header_size) ||
        !pr_write_u32_le(file, chunk_count) ||
        !pr_write_u64_le(file, chunk_table_offset)
    ) {
        (void)fclose(file);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to write package skeleton file.",
            output_path,
            "build.output_write_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    if (fclose(file) != 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to finalize package output file.",
            output_path,
            "build.output_close_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    return PR_STATUS_OK;
}

static void pr_write_json_escaped(FILE *file, const char *text)
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

static pr_status_t pr_write_debug_json(
    const char *debug_path,
    const pr_manifest_t *manifest,
    const char *resolved_output_path,
    int pretty_json,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    FILE *file;

    if (debug_path == NULL || debug_path[0] == '\0' || manifest == NULL) {
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (!pr_ensure_parent_directories(debug_path)) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to create debug output directory path.",
            debug_path,
            "build.debug_dir_create_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    file = fopen(debug_path, "wb");
    if (file == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to open debug output file.",
            debug_path,
            "build.debug_open_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    if (pretty_json != 0) {
        (void)fputs("{\n", file);
        (void)fputs("  \"schema_version\": ", file);
        (void)fprintf(file, "%d,\n", manifest->schema_version);
        (void)fputs("  \"package_name\": \"", file);
        pr_write_json_escaped(file, manifest->package_name);
        (void)fputs("\",\n", file);
        (void)fputs("  \"output\": \"", file);
        pr_write_json_escaped(file, resolved_output_path);
        (void)fputs("\",\n", file);
        (void)fputs("  \"counts\": {\n", file);
        (void)fprintf(file, "    \"images\": %u,\n", (unsigned int)manifest->image_count);
        (void)fprintf(file, "    \"sprites\": %u,\n", (unsigned int)manifest->sprite_count);
        (void)fprintf(file, "    \"animations\": %u\n", (unsigned int)manifest->animation_count);
        (void)fputs("  }\n", file);
        (void)fputs("}\n", file);
    } else {
        (void)fputs("{\"schema_version\":", file);
        (void)fprintf(file, "%d", manifest->schema_version);
        (void)fputs(",\"package_name\":\"", file);
        pr_write_json_escaped(file, manifest->package_name);
        (void)fputs("\",\"output\":\"", file);
        pr_write_json_escaped(file, resolved_output_path);
        (void)fputs("\",\"counts\":{\"images\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->image_count);
        (void)fputs(",\"sprites\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->sprite_count);
        (void)fputs(",\"animations\":", file);
        (void)fprintf(file, "%u", (unsigned int)manifest->animation_count);
        (void)fputs("}}\n", file);
    }

    if (fclose(file) != 0) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Failed to finalize debug output file.",
            debug_path,
            "build.debug_close_failed",
            NULL
        );
        return PR_STATUS_IO_ERROR;
    }

    return PR_STATUS_OK;
}

pr_status_t pr_validate_manifest_file(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
)
{
    pr_manifest_t manifest;
    pr_status_t status;
    int error_count;
    int warning_count;

    pr_manifest_init(&manifest);
    status = pr_manifest_load_and_validate(
        manifest_path,
        diag_sink,
        diag_user_data,
        &manifest,
        &error_count,
        &warning_count
    );
    if (status != PR_STATUS_OK) {
        return status;
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

    pr_manifest_free(&manifest);
    return PR_STATUS_OK;
}

pr_status_t pr_build_package(
    const pr_build_options_t *options,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_build_result_t *out_result
)
{
    pr_manifest_t manifest;
    pr_status_t status;
    const char *output_path;
    const char *debug_output_path;
    int validation_errors;
    int validation_warnings;
    int warnings_count;

    if (options == NULL || out_result == NULL || options->manifest_path == NULL) {
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Build options and output result are required.",
            NULL,
            "build.invalid_arguments",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    memset(out_result, 0, sizeof(*out_result));
    memset(&PR_BUILD_RESULT_STORAGE, 0, sizeof(PR_BUILD_RESULT_STORAGE));
    pr_manifest_init(&manifest);

    status = pr_manifest_load_and_validate(
        options->manifest_path,
        diag_sink,
        diag_user_data,
        &manifest,
        &validation_errors,
        &validation_warnings
    );
    if (status != PR_STATUS_OK) {
        return status;
    }
    (void)validation_errors;

    warnings_count = validation_warnings;
    output_path = (
        options->output_override != NULL && options->output_override[0] != '\0'
    ) ? options->output_override : manifest.output;

    if (output_path == NULL || output_path[0] == '\0') {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Resolved output path is empty.",
            options->manifest_path,
            "build.output_missing",
            NULL
        );
        return PR_STATUS_VALIDATION_ERROR;
    }
    if (!pr_copy_string(
            PR_BUILD_RESULT_STORAGE.package_path,
            sizeof(PR_BUILD_RESULT_STORAGE.package_path),
            output_path
        )) {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Resolved output path exceeds max path length.",
            options->manifest_path,
            "build.output_path_too_long",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }
    if (!pr_has_prpk_extension(output_path)) {
        warnings_count += 1;
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_WARNING,
            "Resolved output path does not use .prpk extension.",
            output_path,
            "build.output_extension",
            NULL
        );
    }

    debug_output_path = (
        options->debug_output_override != NULL && options->debug_output_override[0] != '\0'
    ) ? options->debug_output_override :
        ((manifest.has_debug_output != 0) ? manifest.debug_output : NULL);
    if (
        debug_output_path != NULL &&
        debug_output_path[0] != '\0' &&
        !pr_copy_string(
            PR_BUILD_RESULT_STORAGE.debug_output_path,
            sizeof(PR_BUILD_RESULT_STORAGE.debug_output_path),
            debug_output_path
        )
    ) {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Resolved debug output path exceeds max path length.",
            options->manifest_path,
            "build.debug_output_path_too_long",
            NULL
        );
        return PR_STATUS_INVALID_ARGUMENT;
    }

    if (options->strict_mode != 0 && warnings_count > 0) {
        pr_manifest_free(&manifest);
        pr_emit_diag(
            diag_sink,
            diag_user_data,
            PR_DIAG_ERROR,
            "Strict mode failed: warnings were emitted.",
            options->manifest_path,
            "build.strict_warnings",
            NULL
        );
        return PR_STATUS_VALIDATION_ERROR;
    }

    status = pr_write_package_skeleton(
        PR_BUILD_RESULT_STORAGE.package_path,
        diag_sink,
        diag_user_data
    );
    if (status != PR_STATUS_OK) {
        pr_manifest_free(&manifest);
        return status;
    }

    if (PR_BUILD_RESULT_STORAGE.debug_output_path[0] != '\0') {
        int pretty_json;

        pretty_json = (
            options->pretty_debug_json != 0 ||
            manifest.pretty_debug_json != 0
        ) ? 1 : 0;

        status = pr_write_debug_json(
            PR_BUILD_RESULT_STORAGE.debug_output_path,
            &manifest,
            PR_BUILD_RESULT_STORAGE.package_path,
            pretty_json,
            diag_sink,
            diag_user_data
        );
        if (status != PR_STATUS_OK) {
            pr_manifest_free(&manifest);
            return status;
        }
    }

    out_result->package_path = PR_BUILD_RESULT_STORAGE.package_path;
    out_result->debug_output_path = (
        PR_BUILD_RESULT_STORAGE.debug_output_path[0] != '\0'
    ) ? PR_BUILD_RESULT_STORAGE.debug_output_path : NULL;
    out_result->atlas_page_count = 0u;
    out_result->sprite_count = (unsigned int)manifest.sprite_count;
    out_result->animation_count = (unsigned int)manifest.animation_count;

    pr_emit_diag(
        diag_sink,
        diag_user_data,
        PR_DIAG_NOTE,
        "Wrote .prpk package skeleton successfully.",
        PR_BUILD_RESULT_STORAGE.package_path,
        "build.skeleton_written",
        NULL
    );

    pr_manifest_free(&manifest);
    return PR_STATUS_OK;
}
