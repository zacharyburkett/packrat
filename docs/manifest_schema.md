# Packrat Manifest Schema (v0)

## File

- Default manifest filename: `packrat.toml`
- Current schema version: `1`

## Top-Level Fields

Required:

- `schema_version` (int): must be `1`
- `package_name` (string): logical package identifier
- `output` (string): output `.prpk` path

Optional:

- `debug_output` (string): debug JSON output path
- `pretty_debug_json` (bool, default `false`)

## Atlas Settings

Optional table: `[atlas]`

Fields:

- `max_page_width` (int, default `2048`)
- `max_page_height` (int, default `2048`)
- `padding` (int, default `1`)
- `power_of_two` (bool, default `false`)
- `sampling` (string enum: `pixel`, `linear`; default `pixel`)

## Images

Array table: `[[images]]`

Required fields:

- `id` (string): unique asset id
- `path` (string): image file path

Optional fields:

- `premultiply_alpha` (bool, default `false`)
- `color_space` (string enum: `srgb`, `linear`; default `srgb`)

## Sprites

Array table: `[[sprites]]`

Required fields:

- `id` (string): unique sprite id
- `source` (string): image id from `[[images]]`

Optional common fields:

- `mode` (string enum: `single`, `grid`, `rects`; default `single`)
- `pivot_x` (float, default `0.5`) normalized [0..1]
- `pivot_y` (float, default `0.5`) normalized [0..1]

### `mode = "single"`

Uses one frame. If no rect values are provided, uses the full source image.

Optional rect fields:

- `x` (int, default `0`)
- `y` (int, default `0`)
- `w` (int, default source width)
- `h` (int, default source height)

### `mode = "grid"`

Extracts frames from a uniform cell grid.

Required fields:

- `cell_w` (int, >0)
- `cell_h` (int, >0)

Optional fields:

- `frame_start` (int, default `0`)
- `frame_count` (int, default "to end of available cells")
- `margin_x` (int, default `0`)
- `margin_y` (int, default `0`)
- `spacing_x` (int, default `0`)
- `spacing_y` (int, default `0`)

Frame index order is row-major.

### `mode = "rects"`

Uses explicit rectangles from nested `[[sprites.rects]]`.

Nested frame fields:

- `x` (int, required)
- `y` (int, required)
- `w` (int, required, >0)
- `h` (int, required, >0)
- `label` (string, optional)

At least one `[[sprites.rects]]` entry is required in `rects` mode.

## Animations

Array table: `[[animations]]`

Required fields:

- `id` (string): unique clip id
- `sprite` (string): sprite id from `[[sprites]]`
- `frames` (array of inline tables): ordered frame sequence

Optional fields:

- `loop` (string enum: `once`, `loop`, `ping_pong`; default `loop`)

Each `frames` item:

- `index` (int, required): sprite frame index
- `ms` (int, required): frame duration in milliseconds (>0)

## Validation Rules

1. All ids are unique within their domain.
2. Image/sprite/animation references must resolve.
3. Frame rectangles must stay within source image bounds.
4. Animation frame indices must exist on referenced sprite.
5. Durations must be positive integers.
6. Duplicate output paths in one build invocation are invalid.

## ID Conventions (Recommended)

- Lowercase snake_case ids
- Regex recommendation: `^[a-z][a-z0-9_]*$`

## Full Example

```toml
schema_version = 1
package_name = "sample_game_assets"
output = "build/assets/sample_game.prpk"
debug_output = "build/assets/sample_game.assets.json"
pretty_debug_json = true

[atlas]
max_page_width = 2048
max_page_height = 2048
padding = 1
power_of_two = false
sampling = "pixel"

[[images]]
id = "boid"
path = "assets/boid.png"

[[images]]
id = "fx_sheet"
path = "assets/fx_sheet.png"
premultiply_alpha = false
color_space = "srgb"

[[sprites]]
id = "boid"
source = "boid"
mode = "single"
pivot_x = 0.5
pivot_y = 0.5

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

