# Layout Engine MVP — Frontend

The real end-user desktop app, and the only consumer of `layout_engine_plugin`
now that its own `example/` app has been removed. Loads a user-chosen LEF
file — via a Tcl `read_lef` command (see `LeProvider.readLef`/
`LeEditor.createTclConsole`), not a direct Dart FFI call, so `read_lef` is
the single entry point for reading a LEF file regardless of caller — and
renders it via `layout_engine_plugin`'s `LeEditor`/`LeTexture`, which binds to
the backend's C API (`../backend/src/api/api.hpp`) through Dart FFI. See
`../backend/CLAUDE.md` for the backend architecture and
`../flutter_plugin/CLAUDE.md` for the plugin's Dart FFI / native texture
wiring — neither is duplicated here.

## Requirements (inherited from backend/README.md)

- Target platforms: macOS (dev) and Linux (deploy, blocked upstream — see
  `../flutter_plugin/CLAUDE.md`'s Open design questions). No Windows/iOS/
  Android — those platform directories were never generated.
- Keep responses and docs concise.

## Current state

Early and minimal — not yet wired to the plugin:

- `lib/main.dart` — bare `MaterialApp` shell; `Home` is its only page.
- `lib/pages/home.dart` — one "Open LEF ..." button. `openFilePicker()`
  uses `file_selector` to open a native, `.lef`-filtered dialog and
  currently only `debugPrint`s the chosen path — it does not yet call into
  `LeProvider`/`layout_engine_plugin`'s `LeEditor` to read the file or display
  a `Texture`. `lib/providers/le_provider.dart`'s `readLef` (Tcl
  `read_lef`) → `openDesign` (`setCurrentDesignById`) → `LeEditor.createTexture`
  is the reference wiring once this page grows past the picker.
- `pubspec.yaml` already declares `layout_engine_plugin: path: ../flutter_plugin`,
  so the backend's native code must be built (see the `build-test` skill)
  before `flutter run`/`build macos` will link successfully.
- macOS is sandboxed (`macos/Runner/*.entitlements`,
  `com.apple.security.app-sandbox`); `com.apple.security.files.user-selected.read-only`
  is required for the native open panel and is already set in both Debug
  and Release entitlements.

## Skills

- `build-test` — build the backend native code the plugin links against,
  then `flutter pub get`/`analyze`/`test` and run/build this app.
