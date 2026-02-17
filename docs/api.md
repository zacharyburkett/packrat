# Packrat API Contract (v0)

## Scope

This document defines the initial interface contract for:

1. Command-line usage
2. Build-time library usage
3. Runtime package read usage

This is a stability target for implementation; names and signatures should match this doc unless explicitly revised.

## CLI Contract

Binary name:

- `packrat`

Commands:

1. `packrat validate <manifest>`
2. `packrat build <manifest> [options]`
3. `packrat inspect <package> [options]`

### `validate`

Validates manifest and source references without writing a package.

Example:

```sh
packrat validate packrat.toml
```

### `build`

Builds package output according to manifest.

Options:

- `--output <path>`: override manifest `output`
- `--debug-output <path>`: override manifest `debug_output`
- `--pretty-debug-json`: pretty-print debug JSON if emitted
- `--quiet`: suppress non-error output
- `--strict`: treat warnings as errors

Example:

```sh
packrat build packrat.toml --output build/assets/game.prpk
```

### `inspect`

Inspects a built package for tooling/debugging.

Options:

- `--json`: machine-readable summary output
- `--verbose`: include per-frame/per-clip details

Example:

```sh
packrat inspect build/assets/game.prpk --json
```

## CLI Exit Codes

- `0`: success
- `1`: invalid arguments / usage
- `2`: validation failed
- `3`: I/O or filesystem error
- `4`: internal/runtime error

## Diagnostic Output Format

Default text format:

```text
<severity>: <file>:<line>:<column>: <message> [code=<code>] [asset=<id>]
```

Severity values:

- `error`
- `warning`
- `note`

## C Library: Build API

Header target:

- `packrat/build.h`

### Core Types (Proposed)

```c
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
```

### Functions (Proposed)

```c
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
```

## C Library: Runtime Read API

Header target:

- `packrat/runtime.h`

### Core Types (Proposed)

```c
typedef struct pr_package pr_package_t;

typedef enum pr_loop_mode {
    PR_LOOP_ONCE = 0,
    PR_LOOP_LOOP,
    PR_LOOP_PING_PONG
} pr_loop_mode_t;

typedef struct pr_sprite_frame {
    unsigned int atlas_page;
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    float u0;
    float v0;
    float u1;
    float v1;
    float pivot_x;
    float pivot_y;
} pr_sprite_frame_t;

typedef struct pr_sprite {
    const char *id;
    unsigned int frame_count;
    const pr_sprite_frame_t *frames;
} pr_sprite_t;

typedef struct pr_anim_frame {
    unsigned int sprite_frame_index;
    unsigned int duration_ms;
} pr_anim_frame_t;

typedef struct pr_animation {
    const char *id;
    const pr_sprite_t *sprite;
    pr_loop_mode_t loop_mode;
    unsigned int frame_count;
    const pr_anim_frame_t *frames;
} pr_animation_t;
```

### Functions (Proposed)

```c
pr_status_t pr_package_open_file(const char *path, pr_package_t **out_package);
pr_status_t pr_package_open_memory(
    const void *data,
    size_t size,
    pr_package_t **out_package
);
void pr_package_close(pr_package_t *package);

const pr_sprite_t *pr_package_find_sprite(
    const pr_package_t *package,
    const char *sprite_id
);

const pr_animation_t *pr_package_find_animation(
    const pr_package_t *package,
    const char *animation_id
);

unsigned int pr_package_atlas_page_count(const pr_package_t *package);

unsigned int pr_package_sprite_count(const pr_package_t *package);
const pr_sprite_t *pr_package_sprite_at(
    const pr_package_t *package,
    unsigned int index
);

unsigned int pr_package_animation_count(const pr_package_t *package);
const pr_animation_t *pr_package_animation_at(
    const pr_package_t *package,
    unsigned int index
);
```

## Ownership and Lifetime Rules

1. All pointers returned by `pr_package_find_*` are owned by `pr_package_t`.
2. Returned pointers become invalid after `pr_package_close`.
3. Build APIs do not keep caller-owned pointer references after returning.
4. Strings in `pr_build_result_t` are valid until next build call on same context (or until explicit free API, if introduced).

## Threading Expectations (v0)

1. Build APIs are reentrant but not guaranteed to be internally parallel.
2. Runtime package read APIs are thread-safe for concurrent read access on the same package.
3. No mutable runtime state is stored in query objects.

## Compatibility Rules

1. `schema_version` in manifest controls parse/validate behavior.
2. Package header version controls runtime loader compatibility.
3. Minor format additions must preserve backward compatibility for v0 readers whenever possible.
