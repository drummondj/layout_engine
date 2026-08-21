---
name: ffigen
description: Regenerate the Dart FFI bindings (lib/layout_engine_plugin_bindings_generated.dart) from backend/src/api/api.hpp using the ffigen package. Use whenever api.hpp changes, or the bindings are missing/stale/don't match the header.
user-invocable: true
allowed-tools:
  - Bash
  - Read
  - Edit
  - Write
---

# Regenerate Dart FFI bindings from api.hpp

`backend/src/api/api.hpp` is the only hand-edited source for
`lib/layout_engine_plugin_bindings_generated.dart`. Never edit that file
directly — if it's wrong, fix `api.hpp` (backend side) or `ffigen.yaml`
(config side, plugin root) and regenerate.

## Steps

1. **Run ffigen** from the plugin root:

   ```
   dart run ffigen --config ffigen.yaml
   ```

   Requires libclang (bundled with Xcode Command Line Tools on macOS,
   already present on this dev machine — confirm with `which clang`; on
   Linux install `libclang-dev` if generation fails to find it).

2. **If the output is suspiciously small or missing `le_*` symbols**
   (confirmed to happen silently, no error, twice while first setting this
   up): `ffigen.yaml`'s `compiler-opts: [-x, c]` and `ignore-source-errors:
   true` are both load-bearing, not optional style choices:
   - Without `-x c`, libclang parses `api.hpp` as C++ (its `.hpp`
     extension), turning the header's `extern "C" { ... }` guard into a
     `LinkageSpecDecl`. ffigen's root-cursor walk only visits the
     translation unit's *direct* children, so it silently emits zero
     bindings for anything inside that block — only transitively-included
     `<stdint.h>`/`<pthread.h>` typedefs come out. The top-level
     `language: c` config key does **not** fix this (checked in ffigen
     20.1.1's `parser.dart`: it only ever adds compiler-opts for `language:
     objc`, never for `c`) — it has to be the explicit `-x c` compiler-opt.
   - `api.hpp`'s `#pragma once` triggers a harmless "#pragma once in main
     file" warning (inherent to ffigen parsing any include-guarded header
     directly as a translation unit, not specific to this file) that
     ffigen otherwise treats as fatal and aborts on.
   If you ever need to re-derive this, bisect with a scratch header/config
   pair outside the repo (e.g. under the session scratchpad) rather than
   guessing against the real one — that's how both of these were found.

3. **Verify the output**, don't just trust a clean exit:
   - `LeHandle`, the `LePixelBuffer` struct (with `data`/`width`/`height`/
     `row_bytes`), and every `le_*` function from `api.hpp` appear:
     ```
     grep -c 'le_create\|le_render_pixel_buffer' lib/layout_engine_plugin_bindings_generated.dart
     ```
   - `git diff lib/layout_engine_plugin_bindings_generated.dart` — an
     unexpected change (a function disappearing, a type changing shape)
     means something in `api.hpp` changed in a way you should double check
     was intentional, not just regenerate over.

4. Rebuild/retest afterward (see `build-test` skill) — a signature change
   in the generated bindings almost always requires a matching change in
   `lib/layout_engine_plugin.dart` (the hand-written wrapper around them) and
   possibly the native linking recipes (`macos/layout_engine_plugin.podspec`,
   `src/CMakeLists.txt`) if a function was added/removed/renamed.
