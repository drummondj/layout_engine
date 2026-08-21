# Layout Engine MVP

## Overview

This is a Minimum Viable Product (MVP) and proof of concept to develop a full-stack solution for editing Electronic Design Automation (EDA) Computer Aided Design (CAD) data in LEF/DEF and SystemVerilog formats.

The purpose of this project is to determine the best software architecture to use for editing complex, hierarchical designs with millions of objects.

We will start by implementing a simple LEF editor, which can read multiple LEF files and provide a C API which is used in a Flutter plugin to display a Texture.

The backend project is used to process a database, into a set of skia commands which will create an SkPicture that can be sent to Flutter via a pixel buffer.

## Requirements

- Target platform is linux servers, with little or no GPU support.
- Must be as memory and CPU efficient as possible.
- Tests must be developed in parallel with code.
- Code must be structure in a clean, reusable way, with appropriate use of abstraction.
- Target C++ language is c++23.
- All code must be benchmarked so decisions are data driven.
- Code must be concise and well documented.
- All interactions should be concise and not overly verbose.

## Features

- The pipeline has three stages: shape generation (reads an Abstract into dbu-space `Shape`s), a viewport/sub-pixel-size filter, and a layer-visibility filter. The dbu → pixel transform and rasterization happen after filtering, in `render`, not as pipeline stages — see `CLAUDE.md`.
- Events (not yet built): C-API-originated (load file, toolbar click) and surface-originated (mouse move, object selection, resize/move), routing into `scene`/`database` mutations.
- Property editing (not yet built): a Flutter property editor for selected database objects, via generic get/set access.

## Plan

- [x] CLAUDE.md and skills setup
- [x] Project directory structure setup
- [x] cmake setup — see `CLAUDE.md`'s Build section
- [ ] `events`, `properties`, Flutter-side FFI bindings/plugin

### Module breakdown (dependency order)

See `CLAUDE.md`'s Layout section for how each existing module actually works; this is just the dependency order and build status.

- **`database`** *(exists)* — schema-driven object-pool database. Depends on: nothing.
- **`geometry`** *(exists)* — Boost.Geometry-backed operations over the database's shape types; a hard compile-time dependency of `io`, not just a pipeline-filter nicety. Depends on: `database`.
- **`io`** *(exists)* — format readers (`LEFReader` today; DEF/SystemVerilog land here later). Depends on: `database`, `geometry`.
- **`view_style`** *(exists)* — `ViewLayerSet`/`ViewLayer`, the rendering-purpose layer concept, kept separate from the LEF-mirroring `database`. Depends on: `database`.
- **`scene`** *(exists)* — per-handle mutable view state (displayed Abstract, viewport transform, layer visibility, selection). Depends on: `database`, `view_style`.
- **`pipeline`** *(exists)* — the 3-stage shape generation/filtering pass, each stage self-caching. Depends on: `database`, `geometry`, `scene`, `view_style`.
- **`render`** *(exists)* — `Renderer`: dbu→pixel transform, `SkPicture` generation, pixel-buffer rasterization. Depends on: `pipeline`, `scene`, `view_style`, `database`.
- **`events`** *(new)* — event definitions + dispatch, routing into `scene`/`database` mutations. Depends on: `scene`, `database`.
- **`properties`** *(new)* — generic get/set property access over a selected object. Depends on: `database`.
- **`api`** *(exists, minimal slice)* — the C API surface for Dart FFI: handle lifecycle, file loading, Design selection, viewport control, pixel-buffer rendering. Buffer-based structural-data export (e.g. a library/hierarchy browser) isn't part of this slice — deferred alongside `events`/`properties`. Depends on: everything above.

### Recommended build order (thin vertical slice first)

1. ~~CMake + vendored `lefdef` + `io` + `geometry` compiling, tests passing.~~ **Done.**
2. ~~`scene`.~~ **Done.**
3. ~~One straight-line pipeline function, single-threaded, no caching.~~ **Done.**
4. ~~Minimal `render`: dbu → pixel transform + `SkPicture` generation + pixel-buffer rasterization.~~ **Done.**
5. ~~Minimal `api`: handle lifecycle, load-file, `render_pixel_buffer`.~~ **Done.** Flutter-side FFI bindings/plugin (Dart, not this C surface) still open.
6. Layer in `events`, `properties`, and revisit pipeline/render threading — each backed by a benchmark (see Threading below).

### Open design questions (resolve during implementation)

- **`properties`**: schema-driven reflection metadata (extend `cmg`/`schema.py`) vs. hand-written per-class accessors.
- **Threading**: whether pipeline/render need a task graph vs. plain threads — decide only after a benchmark shows a bottleneck. Reopened once `rasterize()` landed (see `BENCHMARKS.md`): the warm/interactive path now exceeds a 60fps budget for the first time. Not investigated further yet.
- **`SkPicture` recording vs. drawing straight to a canvas**: went with recording because it benchmarks cheap, not because it was compared head-to-head against a straight-to-canvas alternative. Still open if a future benchmark suggests recording overhead matters.
- ~~Rasterizing the `SkPicture` to a pixel buffer~~ — **Done**, `Renderer::rasterize()`. See `CLAUDE.md`/`BENCHMARKS.md`.
- ~~`api` pixel-buffer signature~~ — **Done** as far as this repo goes (`LePixelBuffer` in `src/api/api.hpp`), but not yet confirmed against Flutter's actual texture-ingestion API — no Dart/Flutter-side plugin code exists in this repo yet.
