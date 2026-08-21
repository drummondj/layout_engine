---
name: build-test
description: Build the backend native code this plugin links against, then run flutter pub get/analyze/test. Use when asked to build, compile, test, or run the flutter plugin.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Build and test the Flutter plugin

## 1. Backend native code

The plugin links against the backend's `api`/`render`/`io` targets
(`backend/src/api/`, `src/render/`, `src/io/`), which transitively pull in
`database`/`geometry`/`scene`/`view_style`/`pipeline` (all header-only) and
the vendored `liblef.a`. This plugin deliberately links the **Release**
build (`backend/build_release`), not Debug - it's what a real running
Flutter app embeds, so it needs actual optimized performance (see the
podspec's own comment). Build it first so the plugin's native side has
something to link against:

```
cmake -S ../backend -B ../backend/build_release -DCMAKE_BUILD_TYPE=Release
cmake --build ../backend/build_release --target api render io -j
```

`../backend/build` (Debug) is a separate tree used for backend's own
`ctest`/development workflow - it's not what this plugin links, but both
trees are expected to exist and be kept up to date together going
forward (rebuild both after a backend source change), not just Release
built on-demand for benchmarking.

See `../backend/CLAUDE.md`'s Build section for Skia/Boost/spdlog/fmt setup
if this fails to configure.

## 2. Dart/Flutter side

```
flutter pub get
dart analyze
flutter test
```

`dart analyze` excludes `lib/layout_engine_plugin_bindings_generated.dart`
(see `analysis_options.yaml`) — that file has ~60 harmless
`unused_element`/`unused_field` warnings from transitively-included
`<stdint.h>`/`<pthread.h>` cruft, not from anything we wrote.

## 3. Verifying the native link end-to-end

This plugin no longer ships its own example app (removed — `../frontend`
is the real end-user app and the live consumer of this plugin now).
Verify the native link/build via `../frontend`'s own build (`frontend`'s
own skill, once one exists) — `flutter build macos` there succeeds,
`nm -gU` on the built `layout_engine_plugin.framework` shows all nine `le_*`
symbols, and a loaded LEF file's geometry actually renders through the
`LeEditor` → `LeTexture` → `Texture` widget chain (not just "didn't
crash" — a blank white `Texture` widget with no error is the single most
likely failure mode; check the platform log for `Metal texture`/
`CVReturn` lines on macOS, same as any Skia/CVPixelBuffer issue would
produce).

**If CocoaPods doesn't pick up a podspec/Classes change** (e.g. a new file
added to `macos/Classes/`): `pod install` only reruns when Flutter thinks
the Podfile/plugin list changed, not on every glob match. Force it (run
from whatever app consumes this plugin, e.g. `../frontend/macos`):
```
rm -rf macos/Pods macos/Podfile.lock
flutter build macos --debug
```

## 4. Linux — build via Docker CI, not on this machine directly

This dev machine has no Linux toolchain/GTK, so `linux/layout_engine_plugin.cc`/
`lef_texture.cc` can't be compiled here directly. Use the repo root's
`Dockerfile.linux-ci`/`docker-compose.yml` instead:
`docker compose run --rm frontend` runs `frontend`'s own
`flutter pub get && flutter analyze && flutter test && flutter build linux`
inside a real Linux container with the backend already built there (see
`backend`'s own `build-test` skill) — `LE_LINK_BACKEND` is forced `ON` by
`../frontend/linux/CMakeLists.txt` (this plugin's own `LE_LINK_BACKEND`
default is still `OFF`, since a bare checkout has nothing built to link
against). This is build-verified as of this plugin's CLAUDE.md's "Open
design questions" - see that section for the three real bugs (a
standalone-vs-app-subdirectory CMake gotcha, missing `find_package()`
calls, and a Flutter-plugin-symlink path gotcha) that only surfaced by
actually building it, not by reading the CMake files. Don't attempt
`cmake -S linux -B ...` directly against this plugin's own `linux/`
folder — `apply_standard_settings` only exists when this plugin is
configured as a subdirectory of a real app's `flutter build linux`, so a
standalone configure of `linux/` always fails, app build or not.

## 5. Reporting

Report failures concisely: file:line and the actual error, not the full
build log (native link failures especially — grep for the actual missing
symbol/file rather than pasting the whole `ld` output). Per the backend's
own convention, don't guess at performance fixes — this skill is build/test
only.
