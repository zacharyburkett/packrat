# Packrat

Packrat is a C asset packaging library and CLI focused on data-driven 2D image/sprite/animation content.

> Stability notice
> This repository is pre-1.0 and not stable. Manifest schema, package layout, and API contracts may change between commits.

## What Is Implemented

Library target:

- `packrat::packrat`

Optional CLI executable:

- `packrat` (target `packrat_cli`)

Current capabilities:

- Manifest parsing and validation (`schema_version = 1`)
- PNG decode via `libpng`
- Sprite frame expansion (`single`, `grid`, `rects`)
- Atlas page packing
- `.prpk` package build with `STRS` / `TXTR` / `SPRT` / `ANIM` / `INDX` chunks
- Runtime package loading APIs for sprites/animations/atlas pixel pages
- Package inspection in text or JSON output

## Build

Requires `libpng` development headers/libraries.

```sh
cmake -S . -B build
cmake --build build
```

## CLI

```sh
./build/packrat validate packrat.toml
./build/packrat build packrat.toml
./build/packrat inspect build/assets/game.prpk --verbose
```

Build command options:

- `--output <path>`
- `--debug-output <path>`
- `--pretty-debug-json`
- `--quiet`
- `--strict`

Inspect command options:

- `--json`
- `--verbose`

## CMake Options

- `PACKRAT_BUILD_CLI=ON|OFF`

## Consumer Integration

Current integration model is source inclusion:

```cmake
add_subdirectory(/absolute/path/to/packrat ${CMAKE_BINARY_DIR}/_deps/packrat EXCLUDE_FROM_ALL)
target_link_libraries(my_target PRIVATE packrat::packrat)
```

Note: install/export packaging is not wired yet; use `add_subdirectory(...)` for now.

Public headers:

- `include/packrat/build.h`
- `include/packrat/runtime.h`

## Docs

- `docs/proposal.md`
- `docs/architecture.md`
- `docs/manifest_schema.md`
- `docs/api.md`
