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
- Minimal `validate` command wired to manifest sanity checks

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
```

Current implementation status:

- `validate`: basic checks implemented (`schema_version`, `package_name`, `output`)
- `build`: command/contract scaffolded, pipeline not implemented yet
- `inspect`: command scaffolded, not implemented yet
