#ifndef PACKRAT_BUILD_H
#define PACKRAT_BUILD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pr_status {
    PR_STATUS_OK = 0,
    PR_STATUS_INVALID_ARGUMENT,
    PR_STATUS_IO_ERROR,
    PR_STATUS_PARSE_ERROR,
    PR_STATUS_VALIDATION_ERROR,
    PR_STATUS_ALLOCATION_FAILED,
    PR_STATUS_INTERNAL_ERROR
} pr_status_t;

typedef enum pr_diag_severity {
    PR_DIAG_ERROR = 0,
    PR_DIAG_WARNING,
    PR_DIAG_NOTE
} pr_diag_severity_t;

typedef struct pr_diagnostic {
    pr_diag_severity_t severity;
    const char *message;
    const char *file;
    int line;
    int column;
    const char *code;
    const char *asset_id;
} pr_diagnostic_t;

typedef void (*pr_diag_sink_fn)(const pr_diagnostic_t *diag, void *user_data);

typedef struct pr_build_options {
    const char *manifest_path;
    const char *output_override;
    const char *debug_output_override;
    int pretty_debug_json;
    int strict_mode;
} pr_build_options_t;

typedef struct pr_build_result {
    const char *package_path;
    const char *debug_output_path;
    unsigned int atlas_page_count;
    unsigned int sprite_count;
    unsigned int animation_count;
} pr_build_result_t;

pr_status_t pr_validate_manifest_file(
    const char *manifest_path,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data
);

pr_status_t pr_build_package(
    const pr_build_options_t *options,
    pr_diag_sink_fn diag_sink,
    void *diag_user_data,
    pr_build_result_t *out_result
);

const char *pr_status_string(pr_status_t status);

#ifdef __cplusplus
}
#endif

#endif

