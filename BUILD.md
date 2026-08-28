# Building on Rocky Linux 8 (no root, no Docker)

**Just want to run the app, not build it?** A prebuilt, portable Linux
x86_64 release (glibc 2.28+, no build toolchain needed) is published on
this repo's [GitHub Releases](../../releases) page — see
`.github/workflows/release.yml`/`Dockerfile.linux-release` for how it's
built. It still needs a desktop Linux system with GTK3/X11/Mesa/Tcl-Tk
installed (see that workflow's own release notes) but skips everything
below entirely, and — being built against the same glibc generation as
Rocky 8 — will very likely run directly on this same locked-down machine.
Worth trying first.

This is the build-from-source path for a locked-down machine with **no
root access, no ability to install system packages, and no Docker** —
concretely Rocky Linux 8, though the approach should carry over to other
RPM-based RHEL-8 family distros with adjustment. If you have Docker and
just want a Linux build/test environment, use `docker compose` at the repo
root instead (see `docker-compose.yml`'s own header comment) — that path
is simpler and already verified working; this one is closer to it but
still being iterated on (see the warning below).

**This is still being verified against a real Rocky 8 machine, in
progress as of this writing.** Every RPM name, GN flag, and URL below
started as best-effort reasoning grounded in this repo's actual CMake/GN
files, not empirical verification — several real issues already found and
fixed this way (a redundant `filesystem` RPM extraction, RHEL8's bison
being too old for SWIG's grammar) — but it hasn't reached a full,
confirmed end-to-end run yet. Expect real failures — that's why every step
below is logged. If you hit something you can't resolve yourself, send
back the relevant log file(s) (paths given at each step) rather than just
the terminal output you can see, since the logs capture more than what
scrolls past.

All logs land under one place: `$LE_TOOLCHAIN_ROOT/logs/` (default
`~/.local/layout_engine_toolchain/logs/`, see step 1). Each step below names
its own log file.

## 0. Before you start

You'll need outbound network access to: your configured `dnf` repos
(including `crb`/CodeReady-Builder — see step 1), `github.com`, a Boost
download mirror, `skia.googlesource.com`, and Flutter's engine-artifact CDN
(`storage.googleapis.com`). If any of these are blocked, later steps will
fail with a clear "download failed" — check reachability for that
specific one rather than assuming it's something else.

## 1. Bootstrap the toolchain

```
backend/scripts/rocky8-bootstrap.sh
```

This assembles a compiler (`gcc-toolset-13`), CMake, Ninja, Boost, SWIG,
GTK3 (+ its full build dependency closure), and a from-source Skia build —
all via rootless RPM extraction (`rpm2cpio`/`cpio`, never `dnf install`)
and upstream release tarballs, into `~/.local/layout_engine_toolchain`. It
takes a while (Skia alone is a real build). **No need to add your own
`tee`** — it automatically logs its own full output (still shown live on
screen too) to a timestamped file under
`~/.local/layout_engine_toolchain/logs/bootstrap-<timestamp>.log`, also
symlinked as `latest.log` for convenience; the path is printed at both the
start and end of the run.

It's idempotent and broken into stages (`check-tools`, `rpms`, `cmake`,
`ninja`, `boost`, `swig`, `skia`) — if it fails partway, fix whatever the
log points at and re-run either the whole script (already-done stages
skip themselves) or just the failed stage by name, e.g.:

```
backend/scripts/rocky8-bootstrap.sh skia
```

**If this fails and you're stuck:** send the log file it names in its own
failure message (printed both in the terminal and at the end of the log
itself) — that's the one that actually matters, not just what scrolled by.

## 2. Activate the toolchain

```
source backend/scripts/rocky8-env.sh
```

Not run — **sourced**, every new shell session, before any of the steps
below. This sets `CC`/`CXX`/`PATH`/`PKG_CONFIG_PATH`/`BOOST_ROOT`/
`SKIA_DIR`/etc. to point at what step 1 built. It prints what it set on
success; if it instead prints an error about
`~/.local/layout_engine_toolchain/root` not being found, step 1 didn't
complete — go back and fix that first.

## 3. Generate the database/TCL bindings (codegen)

Step 4 below won't compile without this — `backend/src/database/generated/`
and the TCL-facing generated surface (`backend/src/api/generated_tcl/`,
`backend/src/tcl/generated/`) are `.gitignore`d, produced from
`backend/src/database/schema.py` by this repo's own `codegen` fork (repo
root: `codegen/`), not checked in. There's no combined script for this yet —
run both generation targets by hand:

```
cd codegen
poetry install 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/codegen-poetry-install.log"

poetry run cmg --schema ../backend/src/database/schema.py \
                --output ../backend/src/database/generated \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/codegen-database.log"

poetry run cmg --schema ../backend/src/database/schema.py \
                --output ../backend/src \
                --target tcl \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/codegen-tcl.log"

cd ..
```

This needs Python (`>=3.11,<3.14` per `codegen/pyproject.toml`) and
[Poetry](https://python-poetry.org/) on `PATH` — neither is installed by
step 1's bootstrap script, which only assembles the C++ toolchain.
**Rocky 8's system `python3` is 3.6**, too old for this — you'll need a
newer interpreter available some other way (e.g. an already-installed
`python3.11`+, or `pyenv`/an extracted portable build) before `poetry
install` will succeed; this hasn't been verified on the actual Rocky 8
target machine yet, so treat it the same as everything else in this
doc — expect to hit something here and send back
`codegen-poetry-install.log` if `poetry install` itself is what fails.

See `backend/.claude/skills/regen-database/SKILL.md` and
`regen-tcl/SKILL.md` for what each target actually generates and why
both are needed (one covers `src/database/generated/`, the other covers
the TCL/SWIG-facing surface `src/tcl/` and `src/api/` `#include`). Rerun
both any time `schema.py` changes — those skills are the ones to reach
for then, this section is just the one-time "get from nothing to a
buildable tree" version.

## 4. Build and test the backend

```
cd backend

cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug -DSKIA_DIR="${SKIA_DIR}" -DLE_SKIA_VENDORS_THIRD_PARTY=ON \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/backend-configure-debug.log"

cmake --build build-linux -j \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/backend-build-debug.log"

ctest --test-dir build-linux --output-on-failure \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/backend-ctest.log"

cmake -S . -B build_release-linux -DCMAKE_BUILD_TYPE=Release -DSKIA_DIR="${SKIA_DIR}" -DLE_SKIA_VENDORS_THIRD_PARTY=ON \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/backend-configure-release.log"

cmake --build build_release-linux --target api render io -j \
    2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/backend-build-release.log"
```

`-DLE_SKIA_VENDORS_THIRD_PARTY=ON` must be passed explicitly at configure
time, both times — `rocky8-env.sh` also sets it as a shell `ENV` var, but
that alone does nothing: `backend/CMakeLists.txt`'s `option(...)` never
reads `$ENV{LE_SKIA_VENDORS_THIRD_PARTY}`, only an actual `-D` flag. Left
off, CMake defaults it OFF and tries to link a system `libwebp`/`libjpeg`
that Skia bundled instead — a real `-lwebp: No such file or directory`
link failure, found via the new GitHub Releases build path (see
`.github/workflows/release.yml`) hitting exactly this.

Two trees on purpose: `build-linux` (Debug) is what `ctest` runs against;
`build_release-linux` (Release) is what the actual GUI app links — see
`backend/CLAUDE.md`'s Build section. `flutter_plugin`/`frontend` (steps 5-6
below) need `build_release-linux` to already exist, so don't skip it even
though `ctest` doesn't touch it.

**Expect real test failures here** — beyond the "does it link at all"
question, `ctest`'s actual pass/fail results are the first real signal
about whether the RHEL8-specific choices in `backend/CMakeLists.txt` (the
system-linked vs. Skia-vendored split for freetype/harfbuzz/icu/jpeg/png/
webp/zlib — see that file's own `LE_SKIA_VENDORS_THIRD_PARTY` comment)
actually hold up. `ctest`'s own `--output-on-failure` output goes into
`backend-ctest.log` above; that's the one to send back for a test failure
specifically (not the configure/build logs, unless the failure is a build
error rather than a test result).

## 5. Build and test the Flutter plugin (Dart side only)

```
cd ../flutter_plugin

flutter pub get 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/flutter-plugin-pub-get.log"
dart analyze 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/flutter-plugin-analyze.log"
flutter test 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/flutter-plugin-test.log"
```

This doesn't build the native Linux plugin standalone — see this
package's own `build-test` skill/CLAUDE.md for why that's not a real,
buildable configuration on its own (`apply_standard_settings` only exists
inside a real consuming app's own build). The native link is verified for
real in step 6.

## 6. Build the actual app

```
cd ../frontend

flutter pub get 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/frontend-pub-get.log"
flutter build linux 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/frontend-build-linux.log"
```

This is the real end-to-end check: it configures and builds
`layout_engine_plugin_plugin` (the GTK/method-channel/texture native library)
against everything steps 1-4 produced, and `layout_engine_plugin` (the FFI
shared library) alongside it. A clean run ends with
`✓ Built build/linux/<arch>/release/bundle/layout_engine`.

**If this fails at the link step** specifically (not a Dart compile
error), `flutter build linux`'s own error output is often truncated to a
one-line summary (`clang++: error: linker command failed ...` with no
detail) — if that happens, re-run with `-v` and send the fuller log:

```
flutter build linux -v 2>&1 | tee "$LE_TOOLCHAIN_ROOT/logs/frontend-build-linux-verbose.log"
```

## 7. Run it

```
./build/linux/*/release/bundle/layout_engine
```

You confirmed this machine has a real display, so no `Xvfb`/VNC setup
should be needed (unlike the Docker path's `frontend-gui` stage, which
exists specifically for a headless container).

## After a source change

Steps 1-2 are one-time (until you want to rebuild the toolchain itself).
After editing backend C++ source, re-run step 4's `cmake --build`/`ctest`
lines (no need to reconfigure unless `CMakeLists.txt` itself changed).
After editing `backend/src/database/schema.py`, re-run step 3 (both
`cmg` targets) before step 4 — see the `regen-database`/`regen-tcl`
skills for the fuller regeneration workflow. After editing Dart/plugin
source, re-run step 6. If a build ever looks
inexplicably wrong after switching between this path and something else
(e.g. macOS, or Docker) on the *same* checkout, suspect stale
cross-environment build artifacts in `build*/`, `.dart_tool/`, or
`backend/src/lefdef/lef/lib/` before anything else — `flutter clean`
(Dart-side) or deleting the relevant `build*` directory (CMake-side) is
the fix. This bit us for real during development: a debug build already
compiled by GCC on Linux, or an `.a` archive built by macOS's `ar`, is not
usable by the other platform's toolchain, and the symptom is a confusing
build/link error that has nothing obviously to do with the real cause.
