# Layout Engine MVP — Flutter Plugin

`layout_engine_plugin`, a `plugin_ffi`-template Flutter plugin that binds to
the backend's C API (`../backend/src/api/api.hpp`) via Dart FFI and renders
`le_render_pixel_buffer`'s output as a platform `Texture`. See
`../backend/README.md` for the project brief and `../backend/CLAUDE.md` for
the backend's own architecture.

## Requirements (inherited from backend/README.md)

- Target platforms: macOS (dev, working end-to-end, texture included) and
  Linux (deploy, blocked upstream — see Open design questions).
- Keep responses and docs concise.
- `backend/src/api/api.hpp` is the single source of truth for the FFI
  surface — never hand-edit generated bindings, regenerate via the `ffigen`
  skill instead.

## Architecture

Two separate paths call into the C API — not one:

- **Dart FFI** (`lib/layout_engine_plugin.dart`'s `LeEditor` class, wrapping
  ffigen-generated `lib/layout_engine_plugin_bindings_generated.dart`) for
  everything except pixel delivery: `le_create`/`le_destroy`, `le_read_lef`,
  flat design enumeration/selection (`le_design_count`/`le_design_name`/
  `le_set_current_design`), the hierarchical Library → Design browser
  (`le_library_count`/`le_library_at`/`le_library_design_count`/
  `le_library_design_at`/`le_set_current_design_by_id` — `LeEditor.library()`/
  `libraryDesign()`/`setCurrentDesignById()`; `LeLibraryId`/`LeDesignId`/
  `LeAbstractId` cross the FFI boundary as `LeLibraryRef`/`LeDesignRef`/
  `LeAbstractRef` — plain index+generation value types, safe to hold
  indefinitely unlike `LeFrame`'s pixel data, since they carry no native
  pointer), `le_set_viewport_size`, `le_zoom`/`le_pan` (`LeEditor.zoom()`/
  `pan()` — the backend owns pan/scale entirely; there is no direct setter
  for either, only these relative nudges), `le_fit_scene`
  (`LeEditor.fitScene()` — fits pan/scale to the selected Design's content
  bbox, see Scene::fit_to_content, and is the only way to reset to a known
  view), plus a `renderPixelBuffer()` that copies a frame into Dart memory
  for previewing/testing without a texture. Cheap, called directly from
  Dart.
- **The layer visibility/selectability widget** is two independent axes,
  not a per-cell grid addressed by id: rows by name
  (`le_layer_count`/`le_layer_at` — `LeEditor.layerCount`/`layer()` —
  `ViewLayerSet`'s rows, e.g. a physical Layer or BOUNDARY, each just a
  name + swatch color, not tied to a physical Layer existing) and columns
  by purpose (`le_purpose_count`/`le_purpose_at` — `LeEditor.purposeCount`/
  `purposeAt()` — `LeLayerPurpose.terminal`/`obstruction`/`boundary`,
  independent of any row). Toggling addresses a whole row or a whole
  column at once - `le_is_layer_name_visible`/`le_set_layer_name_visible`/
  `le_is_layer_name_selectable`/`le_set_layer_name_selectable` by name
  (`LeEditor.isLayerNameVisible()`/`setLayerNameVisible()`/
  `isLayerNameSelectable()`/`setLayerNameSelectable()`), and
  `le_is_purpose_visible`/`le_set_purpose_visible`/
  `le_is_purpose_selectable`/`le_set_purpose_selectable` by purpose
  (`LeEditor.isPurposeVisible()`/`setPurposeVisible()`/
  `isPurposeSelectable()`/`setPurposeSelectable()`) - there is no API for
  one individual (row, column) cell. Visibility affects rendering
  (`Pipeline::filter_by_layer_visibility`, combining both axes - see
  `Scene::is_view_layer_visible`); selectability is interaction-only — no
  hit-testing/click-to-select API exists yet to consult it.
- **Grid display and mouse-position tracking** (UPDATES.md 5.1/5.2/5.3/5.4
  all done): `le_minor_grid_spacing`/`le_set_minor_grid_spacing` and the
  `le_major_grid_spacing` equivalents (`LeEditor.minorGridSpacing`/
  `majorGridSpacing` getter+setter pairs, dbu, defaulting to 5/50 — a
  5nm/50nm grid under the common "1 dbu = 1nm" Technology convention), plus
  a fixed cross marker at the Abstract's own origin (5.4 — not
  necessarily dbu (0,0), no dedicated API, always drawn by
  `le_render_pixel_buffer` once a Design's selected), drawn behind the
  design; `le_set_mouse_position`/`le_clear_mouse_position`
  (`LeEditor.setMousePosition()`/`clearMousePosition()`, same pixel space
  as `le_zoom`'s x/y - feed straight from a pointer-move/-leave event)
  drive a grid-snap indicator box also drawn by `le_render_pixel_buffer` -
  cheap to call every pointer-move since it only invalidates the small
  overlay picture, not the potentially design-sized rasterized design
  cache (see `Renderer::compose_with_overlays`); `le_snapped_mouse_position`
  (`LeEditor.snappedMousePosition` — the same minor-grid-snapped point the
  indicator box is centered on, converted dbu → microns, for a Flutter UI
  to display as coordinate text) returns null (not a `LeSnappedMouse`) if
  no position has been set or no Technology has been read yet to convert
  with — `has_position` is checked explicitly rather than a sentinel
  coordinate, since 0/0 is otherwise a legitimate position. As of
  UPDATES.md 7.1 item 1 (no new API surface — same two functions),
  `setMousePosition`/`clearMousePosition` also drive a yellow hover
  outline around whichever selectable shape is under the cursor, via a
  hit-test bounded by the shapes currently on screen (viewport-culled),
  not the whole design.
- **Selection** (UPDATES.md 7.1 items 2-6; 7.2's fuller per-object
  properties API isn't built yet - `le_selection_count`/
  `LeEditor.selectionCount` is a minimal placeholder just to make the
  behavior below independently testable) is driven by three calls, not
  addressed by id like the browsers above: `le_key_down`/`le_key_up`/
  `le_is_key_held` (`LeEditor.keyDown()`/`keyUp()`/`isKeyHeld()`, taking
  the generated `LeKeyCode` enum — re-exported as-is from
  `layout_engine_plugin_bindings_generated.dart` via `export … show
  LeKeyCode` in `lib/layout_engine_plugin.dart`, since unlike
  `LePixelBuffer`/`LeLibraryId`/etc. it's a plain enum with no
  ffi.Struct/native-pointer baggage to wrap first) track modifier keys —
  currently only `LE_KEY_SHIFT`, extend `LeKeyCode` in api.hpp as more
  interaction modes need their own shortcuts; `le_mouse_down`/
  `le_mouse_up` (`LeEditor.mouseDown()`/`mouseUp()`, same pixel space as
  `setMousePosition`) implement click-vs-drag-select purely from the
  down/up pixel distance (small threshold) and shift's currently-held
  state (not a parameter) - click hit-tests one point topmost-layer-first;
  drag-select hit-tests every selectable shape on every layer fully
  enclosed by the down/up rectangle; either way replaces the selection,
  or adds to it if shift is held. A consuming app doesn't have to
  hand-roll this pixel-space/key-code translation itself —
  `lib/layout_engine_input.dart`'s `LeEditorInput` extension
  (`handlePointerEvent`/`handleKeyEvent`) converts standard Flutter
  `PointerEvent`/`KeyEvent` input directly, for wiring straight into
  `Listener`/`MouseRegion`/`Focus`.
- **Native platform texture code** (macOS: `LeApiBridge`/`LeTexture` in
  `macos/Classes/`, implementing `FlutterTexture`; Linux: `FlLeTexture` in
  `linux/lef_texture.cc`, implementing `FlPixelBufferTexture`) calls
  `le_render_pixel_buffer` itself, inside the platform's own pull-based
  texture callback (`copyPixelBuffer` on macOS, `copy_pixels` on Linux) —
  not via Dart FFI. Both platforms only pull a frame after being told a new
  one is available (see below), which matches `le_render_pixel_buffer`'s
  own "cheap unless something changed" caching (see api.hpp), so the native
  callback can call it every pull with no extra throttling logic.
- The Dart-owned `LeHandle*` has to reach that native code too, and Dart FFI
  can't reach the engine's texture registrar itself (only platform embedder
  code can) — so a small `MethodChannel('layout_engine_plugin')` exists purely
  to shuttle a handle address and a texture id back and forth
  (`LeEditor.createTexture()`/`LeTexture` on the Dart side; `LayoutEnginePlugin`
  on macOS, `layout_engine_plugin_register_with_registrar` on Linux — same
  three methods on both: `createTexture`/`markTextureFrameAvailable`/
  `disposeTexture`). The actual per-frame pixel pull never crosses this
  channel.
- `LePixelBuffer.data` is only valid until the next
  `le_render_pixel_buffer`/`le_destroy` call on the same handle (see
  api.hpp's own comment). `LeEditor.renderPixelBuffer()` copies it into a
  Dart `Uint8List` immediately; the Linux texture hands the pointer to
  Flutter directly instead (`FlPixelBufferTexture`'s own contract allows
  this — the buffer only needs to survive until the *next* render-thread
  tick, which matches api.hpp's validity window exactly, see
  `lef_texture.cc`'s comment); the macOS texture instead copies+converts
  into a `CVPixelBuffer` (see Native linking below for why conversion, not
  just a copy, is required there).
- **Lifetime and thread safety are real, open constraints, not just
  disclaimers** — see the doc comment on `LeTexture` in
  `lib/layout_engine_plugin.dart`: a `LeTexture` must be disposed before the
  `LeEditor` it came from, and a `LeEditor` setter (including `fitScene`)
  racing a pending, not-yet-rendered `markFrameAvailable` on the engine's
  raster thread is a genuine data race (backend's Scene/Pipeline/Renderer
  aren't documented as thread-safe). Not solved at the plugin layer — a
  real fix needs a lock inside the C API itself.

## Open design questions

- **Linux native link: now built and verified (via Docker CI), not just
  written.** Both backend gaps that used to block this (CoreText-only
  `default_typeface()`, no Linux Skia checkout) are closed — see
  backend/CLAUDE.md's `pipelines` entry and "Open gaps". `LE_LINK_BACKEND`
  (`src/CMakeLists.txt`) still defaults `OFF` (a plain `flutter build
  linux` from a bare checkout has no backend build/Skia checkout to link
  against), but `../frontend/linux/CMakeLists.txt` forces it `ON` and
  `docker compose run frontend` (repo root `Dockerfile.linux-ci`/
  `docker-compose.yml`) now builds it for real: `flutter build linux`
  succeeds, and `nm -D` on the built `liblayout_engine_plugin_plugin.so`
  shows the full `le_*()` surface, including `le_render_pixel_buffer`
  itself (same verification standard as macOS's own `nm -gU` check below).
  Getting there past "structurally plausible" surfaced three real bugs,
  not found by inspection:
  - `linux/CMakeLists.txt`'s own `apply_standard_settings` call only
    resolves when this plugin is configured as a subdirectory of a real
    app's `flutter build linux` — Flutter's tooling injects that function
    from the app's own `flutter/CMakeLists.txt`, so there's no such thing
    as configuring this plugin's `linux/` folder standalone.
  - `linux/CMakeLists.txt`'s `LE_LINK_BACKEND` block links
    `spdlog::spdlog`/`fmt::fmt` but never called `find_package()` for
    either itself — `../src/CMakeLists.txt`'s own `find_package()` calls
    don't propagate their imported targets back up from the child
    `add_subdirectory` to this parent directory.
  - `src/CMakeLists.txt`'s own `LE_BACKEND_DIR`/`LE_BACKEND_BUILD`
    defaults are relative to *that file's* `CMAKE_CURRENT_SOURCE_DIR`,
    which resolves through Flutter's `flutter/ephemeral/.plugin_symlinks/
    layout_engine_plugin/` symlink when consumed by a real app — a relative
    `../../backend` from there lands on a path that doesn't exist. A real
    consuming app's own `linux/CMakeLists.txt` (not symlinked) has to
    override both to an absolute-from-itself path instead.
  Still true: the frontend app itself doesn't yet call `LeEditor`/display
  a `Texture` from `home.dart` (see `../frontend/CLAUDE.md`'s "Current
  state"), so a built-and-run Linux app has nothing to actually render
  yet — this closes the *link*, not the end-to-end visual.
- **Packaging.** Backend depends on a machine-specific Skia checkout plus
  Homebrew Boost/spdlog/fmt/harfbuzz/icu4c/jpeg/png/webp (see backend
  CLAUDE.md's "Open gaps") — none of that is redistributable as-is, and the
  macOS podspec/Linux CMake both hardcode this machine's paths as defaults.
  Not a blocker for local dev; is a blocker before this plugin could ship
  to a machine that isn't this one.

## Native linking (macOS — done and verified; Linux — done and verified via Docker CI)

`api.hpp`'s `le_*()` functions are implemented in `backend/src/api/api.cpp`
and compiled by backend's *own* CMake build (`../backend/build_release/libapi.a`
etc.) — this plugin links that output directly rather than recompiling it,
mirrored by hand in two places since there's no automated sharing between a
CMakeLists.txt and a podspec.

**Deliberately the Release build (`backend/build_release`), not Debug
(`backend/build`)** - this plugin is what an actual running Flutter app
embeds, so it needs real optimized performance, not debug-build timings.
`backend/build` (Debug) still has to exist too, for backend's own
`ctest`/development workflow - **both trees are expected to exist and be
kept current going forward**, not just Release built on-demand for
benchmarking (see backend's own `build-test` skill/CLAUDE.md). Rebuild
`build_release`'s `api`/`pipelines`/`io` targets after any backend source
change before rebuilding this plugin, the same way `build`'s targets get
rebuilt before running `ctest`.

- `macos/layout_engine_plugin.podspec` — `pod_target_xcconfig`'s
  `OTHER_LDFLAGS` force-loads `libapi.a` (its `le_*()` symbols are never
  referenced by this plugin's own sources — Dart only finds them via
  `dlsym` at runtime — so without `-force_load` the linker drops the whole
  object file) plus `libpipelines.a`/`libio.a`/`liblef.a`/`libskia.a` and the
  Homebrew/system libs backend's own `CMakeLists.txt` needs. Verified
  end-to-end (originally via this plugin's own `example/` app, since
  removed — `../frontend` is the real end-user app and now the live proof
  this link still works): `flutter build macos` succeeds, `nm -gU` on the
  resulting `layout_engine_plugin.framework` shows all nine `le_*` symbols
  exported, and a loaded LEF file's boundary and M1 pin render through the
  full `LeEditor` → `LeTexture` → `Texture` widget chain — confirmed
  visually (screenshot), not just "didn't crash."
  - **Gotcha:** CocoaPods' `OTHER_LDFLAGS` merging silently drops anything
    it recognizes as the C++ runtime library — neither a plain `-lc++` flag
    nor an explicit `libc++.tbd` path survives (no error, just absent from
    the merged `.xcconfig`). The fix ended up being
    `macos/Classes/LeApiBridge.mm` (see below) simply *being* an
    Objective-C++ file — its mere presence makes Xcode pick the C++ linker
    driver for the whole target (this plugin's other sources are plain C).
  - **Gotcha:** `__dir__` inside the podspec resolves through CocoaPods'
    `.symlinks/plugins/...` path, not this repo's real layout — wrap it in
    `File.realpath` before computing `../../backend`, or the relative walk
    silently lands inside `.symlinks` instead.
  - Requires `backend/build_release/{libapi,libpipelines,libio}.a` and
    `backend/src/lefdef/lef/lib/liblef.a` to already be built (`build-test`
    skill, step 1) and a Skia checkout at `$SKIA_DIR` or
    `/Users/john/Projects/synthosilicon/skia/skia` (backend's own default).
- `src/CMakeLists.txt` / `linux/CMakeLists.txt` (Linux) — same recipe via
  `LE_LINK_BACKEND` (`--whole-archive` instead of `-force_load`;
  `pkg_check_modules` instead of hardcoded Homebrew paths; `LE_BACKEND_BUILD`
  also defaults to `build_release`, same reasoning). Build-verified via
  Docker CI (`docker compose run frontend` at the repo root, forcing
  `LE_LINK_BACKEND ON` from `../frontend/linux/CMakeLists.txt`) — see Open
  design questions above for the three real bugs that surfaced getting
  there and aren't visible from reading this file alone.

### Why macOS needs a pixel copy+swizzle, not just a copy

`FlutterTexture.copyPixelBuffer`'s own doc (`FlutterTexture.h`) restricts
the returned `CVPixelBuffer` to `kCVPixelFormatType_32BGRA` or a YpCbCr
variant — never `32RGBA`. api.hpp's `LePixelBuffer` is deliberately
platform-neutral RGBA8888 (see its own comment). So macOS's
`LeApiBridge.mm` swizzles R/B per pixel while copying into a new
`CVPixelBuffer`; Linux's `FlPixelBufferTexture` wants RGBA directly (see
`fl_pixel_buffer_texture.h`'s own doc), so `lef_texture.cc` hands off
api.hpp's buffer pointer as-is, no copy or swizzle needed — found the
asymmetry by reading both platforms' actual headers, not assumed.

Two more macOS specifics, both found by trial (the failure mode gives no
useful compile-time signal):

- `CVPixelBufferCreate` **must** be called with
  `kCVPixelBufferIOSurfacePropertiesKey` in its attributes dictionary — a
  plain (non-IOSurface-backed) pixel buffer isn't rejected by
  `CVPixelBufferCreate` itself, but the engine's Metal compositor can't
  wrap it into a texture; the only symptom is "Could not create Metal
  texture from pixel buffer: CVReturn -6660" in the system log and the
  `Texture` widget staying permanently blank, with `copyPixelBuffer` itself
  reporting success throughout.
- Swift files in a CocoaPods **framework** target do not automatically see
  the target's own C headers the way a bridging header would (and
  `SWIFT_OBJC_BRIDGING_HEADER` is outright rejected for framework targets:
  "using bridging headers with framework targets is unsupported"). Rather
  than fight Clang's module dependency scanner (which also separately
  rejects a `../../` relative include escaping the pod's module root, even
  though the same relative include compiles fine in a plain non-modular
  `.c`/`.mm` translation unit), `LeApiBridge.h`/`.mm` exists specifically
  so `LeTexture.swift` only ever sees a small, clean Objective-C interface
  — ordinary same-target ObjC↔Swift interop, which has always worked with
  no extra plumbing.

## Layout

- `pubspec.yaml`, `ffigen.yaml` — `ffigen.yaml` points `entry-points` at
  `../backend/src/api/api.hpp` directly (not a copy) with `compiler-opts:
  [-x, c]` and `ignore-source-errors: true` — see the `ffigen` skill for
  why both are required, not optional niceties. `pubspec.yaml` declares
  `pluginClass: LayoutEnginePlugin` for both `macos:` and `linux:` alongside
  `ffiPlugin: true` — a plugin can be both at once; Flutter's tool treats
  it as method-channel-registered (bundling the FFI shared library
  alongside it), not as two competing registrations.
- `lib/layout_engine_plugin.dart` — the public Dart API: `LeEditor` (FFI
  wrapper + `createTexture()`), `LeFrame` (a `renderPixelBuffer()` result),
  `LeTexture` (a live platform texture — see its own doc comment for the
  lifetime/thread-safety constraints). `lib/layout_engine_plugin_bindings_generated.dart`
  — ffigen output, never hand-edit.
- `lib/layout_engine_input.dart` — `extension LeEditorInput on LeEditor`
  (re-exported from `lib/layout_engine_plugin.dart` via `export … show
  LeEditorInput`, same pattern as `LeKeyCode`'s own re-export): converts
  standard Flutter `PointerEvent`/`KeyEvent` input directly into the
  Selection API's low-level calls (see Architecture below) —
  `handlePointerEvent()` and `handleKeyEvent()` — so a consumer doesn't
  have to hand-roll a `LogicalKeyboardKey` → `LeKeyCode` mapping or
  pixel-space bookkeeping itself. `../frontend`'s canvas wiring shows the
  intended `Listener`/`MouseRegion`/`Focus` usage.
- `src/layout_engine_plugin.h` — `#include`s `../backend/src/api/api.hpp`
  directly (so this TU fails to compile if that header ever stops being
  C-parseable); `src/layout_engine_plugin.c` — empty, exists only because the
  CMake target needs a source file (see Native linking above for where the
  real symbols come from).
- `macos/Classes/` — `LayoutEnginePlugin.swift` (method channel),
  `LeTexture.swift` (`FlutterTexture`), `LeApiBridge.h`/`.mm` (the
  Objective-C++ shim both depend on — see Native linking above for why it
  exists as its own class rather than Swift calling api.hpp directly),
  `layout_engine_plugin.c` (the FFI forwarder, unrelated to the texture path).
- `linux/` — `layout_engine_plugin.cc`/`include/layout_engine_plugin/layout_engine_plugin.h`
  (method channel), `lef_texture.cc`/`.h` (`FlPixelBufferTexture`) — same
  protocol as macOS, **build-verified via Docker CI** (see Open design
  questions).

## Skills

- `ffigen` — regenerate `lib/layout_engine_plugin_bindings_generated.dart`
  from `backend/src/api/api.hpp`.
- `build-test` — build the backend native code this plugin links against,
  then `flutter pub get`/`analyze`/`test`.
