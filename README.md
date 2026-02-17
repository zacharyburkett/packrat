# Packrat

Packrat is a standalone asset packaging library for games and interactive tools.

Initial focus:

- Image asset packing
- Single-image sprites
- Sprite-sheet frame extraction
- Data-driven animation clip definitions

Longer-term direction:

- Expand to additional asset domains (audio, fonts, data blobs, etc.)
- Remain portable and easy to embed in engine/tool pipelines

## Current Status

This repository currently contains initial planning and interface/format documentation.

It now also includes an initial C/CMake scaffold:

- `packrat::packrat` static library target
- `packrat` CLI executable
- Manifest parser + schema validator for the v0 image/sprite/animation schema
- Package build path that writes a `.prpk` skeleton file

## Documents

- `docs/proposal.md`
- `docs/architecture.md`
- `docs/manifest_schema.md`
- `docs/api.md`

## Build

```sh
cmake -S . -B build
cmake --build build
```

## CLI (Current)

```sh
./build/packrat validate packrat.toml
./build/packrat build packrat.toml
```

Current implementation status:

- `validate`: parses manifest sections and validates schema/references for v0
- `build`: validates manifest and writes `.prpk` header/chunk-table skeleton (+ optional debug JSON)
- `inspect`: command scaffolded, not implemented yet
