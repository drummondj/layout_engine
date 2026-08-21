---
name: build-test
description: Build the backend native code this app links against (via layout_engine_plugin), then run flutter pub get/analyze/test and run/build the macOS app. Use when asked to build, compile, test, or run this frontend.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Build and test the frontend app

## 1. Backend native code

This app depends on `layout_engine_plugin` (`pubspec.yaml`'s
`path: ../flutter_plugin`), whose macOS podspec force-links the backend's
`libapi.a`/`librender.a`/`libio.a` + vendored `liblef.a` directly — build
those first or the macOS build fails at the link step, not compile. The
podspec deliberately links the **Release** build (`backend/build_release`),
not Debug - see its own comment:

```
cmake -S ../backend -B ../backend/build_release -DCMAKE_BUILD_TYPE=Release
cmake --build ../backend/build_release --target api render io -j
```

`../backend/build` (Debug) is a separate tree for backend's own ctest/
development workflow, not what this app actually links - both are
expected to exist and be kept up to date together going forward, not
just Release built on-demand for benchmarking.

See `../backend/CLAUDE.md`'s Build section for Skia/Boost/spdlog/fmt setup,
and `../flutter_plugin/CLAUDE.md`'s Native linking section for the full
recipe this app's `macos/Podfile` pulls in transitively — there is no
separate recipe to maintain here.

## 2. Dart/Flutter side

```
flutter pub get
flutter analyze
flutter test
```

## 3. Run/build the macOS app

```
flutter run -d macos      # or: flutter build macos --debug
```

Requires backend/build_release's `libapi.a`/`librender.a`/`libio.a` (step 1) and a
Skia checkout (`$SKIA_DIR` or backend's own default,
`/Users/john/Projects/synthosilicon/skia/skia`) — the podspec raises a
clear error naming the missing file/command if these aren't there yet.

**If CocoaPods doesn't pick up a podspec/plugin change**: `pod install`
only reruns when Flutter thinks the Podfile/plugin list changed, not on
every change to `../flutter_plugin`. Force it:
```
rm -rf macos/Pods macos/Podfile.lock
flutter run -d macos
```

## 4. Linux — cannot be built or run here

Same upstream blockers as the plugin (CoreText-only font manager, no Linux
Skia checkout, no Linux toolchain/GTK on this dev machine) — see
`../flutter_plugin/CLAUDE.md`'s Open design questions. Don't debug a Linux
build failure here without first checking those gaps have landed *and*
you're actually on a Linux machine.

## 5. Verifying UI changes visually

Driving/screenshotting the running macOS window requires Accessibility
(`osascript`/System Events) and Screen Recording (`screencapture`)
permissions for whatever process is running the command — neither is
guaranteed to be granted in an automated/sandboxed shell. If a screenshot
or simulated click fails with "not allowed assistive access" or "could not
create image from display", say so explicitly rather than claiming the UI
was verified, and ask the user to grant those permissions or verify
manually.

## 6. Reporting

Report failures concisely: file:line and the actual error, not the full
build log (native link failures especially — grep for the actual missing
symbol/file rather than pasting the whole `ld` output).
