# Packrat Proposal (Initial)

## Purpose

Packrat should provide a reliable, cross-platform way to author and package visual assets so game code does not hardcode sprite rectangles, frame timings, or animation definitions.

The pack output should be the source of truth for:

- Sprite definitions
- Sprite frame geometry and timing
- Animation clip composition
- Texture atlas locations

## Primary Goals (v0)

1. Support single-image sprites.
2. Support sprite-sheet style assets (grid and explicit frame rects).
3. Support authoring animation clips in asset definitions.
4. Produce deterministic package output from the same inputs.
5. Expose both CLI and embeddable library API entry points.

## Non-Goals (v0)

1. Audio/font/model packing.
2. Runtime streaming/virtual file systems.
3. Lossy texture compression/transcoding matrix.
4. Full editor UI for asset authoring.

## User Workflow

1. Author a pack manifest (`packrat.toml`) in the game project.
2. Define image sources, sprites, and animation clips in the manifest.
3. Run packrat build (`packrat build packrat.toml`).
4. Consume the generated package from game/runtime code.

## v0 Deliverables

1. Manifest schema for images/sheets/animations.
2. Atlas packing pipeline for RGBA images.
3. Package metadata that maps names to sprite frames and clips.
4. Validation pass with actionable diagnostics.
5. Optional debug dump output for inspection.

## Proposed Roadmap

1. Milestone A: Core foundation
2. Milestone B: Image import + atlas packing
3. Milestone C: Animation metadata + package writing
4. Milestone D: Ardent integration path (build + runtime loader contract)

## Design Constraints

1. Portable C/C++ implementation.
2. Deterministic output ordering and IDs.
3. Clear schema versioning in both manifest and output package.
4. Dependency-light default implementation.

## Open Decisions

1. Manifest parser dependency choice (dedicated TOML parser vs minimal internal parser).
2. Package container strategy for v0 (single binary only vs binary + debug sidecar).
3. ID strategy (string keys only vs generated integer handles in metadata).

