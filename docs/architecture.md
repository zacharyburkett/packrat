# Packrat Architecture (Initial)

## Scope

This document describes the initial architecture for image-focused asset packaging:

- Single images
- Sprite sheets
- Frame-based animation clips

The architecture is intentionally staged so new asset domains can be added without redesigning the whole system.

## System Overview

Packrat is split into five layers:

1. Authoring schema (`packrat.toml`)
2. Build pipeline (parse, validate, normalize, pack, emit)
3. Package format (`.prpk`)
4. Runtime read API (engine-facing)
5. Tooling surfaces (CLI + potential editor integration)

## Data Model

### Source Images

A source image is an input file from disk with optional import options:

- Path
- Color space assumptions
- Premultiply alpha option
- Trimming policy (future)

### Sprites

A sprite is a named renderable asset with one or more frames.

Frame source options:

1. Single-image frame (`source + full rect`)
2. Sheet grid extraction (`cell_w`, `cell_h`, index range)
3. Explicit rect list (`x`, `y`, `w`, `h`)

### Animations

An animation clip is a named ordered frame sequence:

- Sprite reference
- Frame indices or explicit frame refs
- Per-frame duration
- Loop mode (`once`, `loop`, `ping_pong`)

### Package Objects

Compiled package data should minimally expose:

- Texture atlas pages
- Sprite definitions and UV/rect data
- Animation clips with frame timing
- Name lookup/index table

## Manifest Schema (Proposed)

Authoring file: `packrat.toml`

```toml
schema_version = 1
package_name = "sample_game_assets"
output = "build/assets/sample_game.prpk"

[[images]]
id = "boid"
path = "assets/boid.png"

[[images]]
id = "fx_sheet"
path = "assets/fx_sheet.png"

[[sprites]]
id = "boid"
source = "boid"

[[sprites]]
id = "explosion"
source = "fx_sheet"
mode = "grid"
cell_w = 32
cell_h = 32
frame_start = 0
frame_count = 12

[[animations]]
id = "boid_idle"
sprite = "boid"
loop = "loop"
frames = [{ index = 0, ms = 100 }]

[[animations]]
id = "explosion_burst"
sprite = "explosion"
loop = "once"
frames = [
  { index = 0, ms = 40 }, { index = 1, ms = 40 }, { index = 2, ms = 40 },
  { index = 3, ms = 40 }, { index = 4, ms = 40 }, { index = 5, ms = 40 },
  { index = 6, ms = 40 }, { index = 7, ms = 40 }, { index = 8, ms = 40 },
  { index = 9, ms = 40 }, { index = 10, ms = 40 }, { index = 11, ms = 40 }
]
```

## Build Pipeline

1. Parse manifest.
2. Validate IDs, references, frame bounds, durations, and duplicate names.
3. Load images and normalize to a common pixel format (`RGBA8` in v0).
4. Expand sprite frame definitions into concrete rect lists.
5. Pack frames into atlas pages (deterministic sort + rectangle packing).
6. Build animation clip tables.
7. Emit package (`.prpk`) and optional debug dump (`.json`).

## Package Format (Proposed v0)

Primary output: single binary package (`.prpk`) containing:

1. File header
2. Chunk directory
3. Chunk payloads

Core chunk set:

1. `STRS`: string table
2. `TXTR`: atlas page metadata + pixel blobs
3. `SPRT`: sprite/frame records (source rect + atlas rect + pivots)
4. `ANIM`: animation clips and timing data
5. `INDX`: name-to-record lookup tables

Optional chunk:

1. `DBUG`: diagnostic metadata for tooling/debugging

## Runtime API Shape (Proposed)

The runtime-facing API should be read-only and allocation-conscious:

1. Open package from memory/file.
2. Query sprite by name.
3. Query animation clip by name.
4. Access frame data (atlas page, UVs, duration).
5. Close/release package.

This keeps gameplay code data-driven:

- Animation timing and frame ordering live in package data.
- Game code only asks for clip IDs/names and advances clip state.

## Ardent Integration Contract (Initial)

1. Ardent project build flow can call packrat as a build step.
2. Engine runtime can load `.prpk` and expose asset queries to modules.
3. Editor panels can inspect loaded package metadata for debugging.

## Extensibility Plan

Future asset domains should plug in through domain-specific compilers:

1. `packrat_image_compiler`
2. `packrat_audio_compiler`
3. `packrat_font_compiler`

Shared infrastructure remains common:

1. Manifest parsing
2. Validation and diagnostics
3. Package writer/reader
4. Stable naming/indexing

## Error and Diagnostic Strategy

Every build diagnostic should include:

1. Severity (`error`, `warning`, `note`)
2. Manifest location (line/column when available)
3. Asset ID/path context
4. Clear remediation text

Build exits non-zero when any `error` is emitted.

