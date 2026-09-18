cmake_minimum_required(VERSION 3.20)

# Must be set before project() to take effect. Without this, CMake lets
# clang default to targeting whatever macOS SDK/version this machine
# currently runs (drifts upward over time, e.g. macOS 26.0 as of this
# writing) - fine for this backend's own standalone binaries (ctest,
# benchmarks), but a real problem once linked into flutter_plugin's
# macOS app bundle, which targets a much older macOS (11.0, per
# frontend/macos/Podfile's/Runner.xcodeproj's own MACOSX_DEPLOYMENT_TARGET) -
# found via a real crash, not anticipated: Apple's libc++ has real
# __API_AVAILABLE-gated implementation differences for synchronization
# primitives across SDK versions, so object code compiled assuming a much
# newer minimum (this backend's libapi.a/libTracyClient.a, previously
# unset -> 26.0) linked into a binary built for 11.0 can end up with a
# std::mutex whose construction/locking disagrees with what the final
# binary's own runtime expects - manifested as Tracy's own background
# profiler thread (tracy::Profiler::Worker) throwing `std::mutex::lock()
# "Invalid argument"` on startup, only inside the actual Flutter app
# bundle, never in this backend's own plain CLI binaries (which stayed
# internally consistent throughout). 11.0 matches the actual effective
# target the app's own linker already reported building for (`ld:
# warning: building for macOS-11.0`) - keep in sync with frontend/macos's
# own deployment target if that ever changes.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version - must match flutter_plugin's own Flutter-app deployment target, see comment above")
endif()

project(layout_engine_backend VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_policy(SET CMP0135 NEW)

# le_tcl (below) is a shared module (le_tcl.so) that statically links
# io/render/api/lef_lib - on Linux, a static library linked into a .so
# needs to have been compiled with -fPIC, or the final link fails with
# "relocation ... can not be used when making a shared object; recompile
# with -fPIC" (ld64 on macOS doesn't enforce this the same way, so this
# went unnoticed there). Applying it globally is simplest and has no real
# cost - PIC is already macOS's default for everything.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_COVERAGE "Instrument io/backend_tests with Clang source-based line+branch coverage" OFF)

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE uses -fprofile-instr-generate, which requires Clang; found ${CMAKE_CXX_COMPILER_ID}. On GCC/Linux use --coverage + gcov/lcov instead.")
    endif()

    find_program(LLVM_PROFDATA_EXE llvm-profdata)
    find_program(LLVM_COV_EXE llvm-cov)
    if(APPLE AND (NOT LLVM_PROFDATA_EXE OR NOT LLVM_COV_EXE))
        # Xcode's toolchain doesn't put these on PATH directly.
        execute_process(COMMAND xcrun -f llvm-profdata OUTPUT_VARIABLE LLVM_PROFDATA_EXE OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND xcrun -f llvm-cov OUTPUT_VARIABLE LLVM_COV_EXE OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(NOT LLVM_PROFDATA_EXE OR NOT LLVM_COV_EXE)
        message(FATAL_ERROR "ENABLE_COVERAGE requires llvm-profdata and llvm-cov (on PATH, or via xcrun on macOS)")
    endif()
endif()

function(le_enable_coverage_instrumentation target)
    if(ENABLE_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping -g -O0)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endif()
endfunction()

include(ExternalProject)
include(FetchContent)

# --- Vendored LEF parser (src/lefdef/lef) ---
# Built via its own Makefile; produces liblef.a and generates lefdef/lef/include/*.h.
# The DEF parser (src/lefdef/def) is vendored alongside it - see def_lib below.
ExternalProject_Add(
    lef_lib
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef
    CONFIGURE_COMMAND ""
    # -j1 and a cleared MAKEFLAGS are required: this vendored recursive
    # Makefile's `all: install release` traverses the same subdirectories
    # twice and isn't safe under an inherited parallel jobserver (races on
    # shared bison-generated lef.tab.c and on liblef.a itself).
    # CXXFLAGS/CFLAGS=-fPIC: template.mk's Linux branch doesn't set either,
    # so they pass through from this env untouched, same as the
    # CMAKE_POSITION_INDEPENDENT_CODE setting above and for the same
    # reason - le_tcl.so statically links liblef.a, which GNU ld (Linux)
    # refuses without -fPIC object code (ld64/macOS doesn't enforce this).
    BUILD_COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS --unset=MFLAGS CXXFLAGS=-fPIC CFLAGS=-fPIC make -j1
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 1
    # lefdiff (UPDATES.md 12 Phase 1's round-trip fidelity check) is also a
    # side effect of this same build - the vendored top-level Makefile's
    # own BUILD_ORDER already includes it (and lefwrite) alongside lef/
    # clef/lefrw, so no separate build step is needed, just tracking it as
    # a real byproduct.
    BYPRODUCTS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/lib/liblef.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
)

# Wrapping liblef.a as a proper IMPORTED target (rather than a raw file path
# in target_link_libraries) lets CMake re-order/re-emit it correctly for
# each generator's actual linker - a raw path is just inserted verbatim
# wherever it falls in topological order. Apple's ld64 tolerates a static
# archive appearing "out of order" relative to the objects that need its
# symbols; GNU ld (Linux) does one left-to-right pass and does not, so a
# raw path silently linked fine on macOS while failing on Linux with
# "undefined reference" for every LefDefParser:: symbol - found via a real
# Linux build (see docker-compose.yml), not anticipated in advance.
add_library(lef_static STATIC IMPORTED)
set_target_properties(lef_static PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/lib/liblef.a")
add_dependencies(lef_static lef_lib)

# --- Vendored DEF parser (src/lefdef/def) ---
# Mirrors lef_lib above exactly - same vendored Si2 distribution shape
# (def/def/Makefile's own LIBTARGET = libdef.a, installed to
# def/lib/libdef.a + def/include/*.hpp, same -j1/cleared-MAKEFLAGS/-fPIC
# gotcha, same BUILD_ORDER-driven defdiff byproduct at def/bin/defdiff
# for round-trip fidelity testing, same reason for IMPORTED wrapping
# over a raw archive path).
ExternalProject_Add(
    def_lib
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS --unset=MFLAGS CXXFLAGS=-fPIC CFLAGS=-fPIC make -j1
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 1
    BYPRODUCTS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/lib/libdef.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/bin/defdiff"
)

add_library(def_static STATIC IMPORTED)
set_target_properties(def_static PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/lib/libdef.a")
add_dependencies(def_static def_lib)

# --- Third-party dependencies ---
# fmt is pinned via a hard-required exact-version find_package (not the
# "prefer system, else fetch" pattern spdlog below still uses) -
# discovered empirically while adding the slang dependency further down
# (SystemVerilog/Verilog reader, SYSTEMVERILOG.md): a CMake target name
# is process-global, so this project's own fmt::fmt and slang's own
# internal fmt::fmt lookup (it declares its own FetchContent dependency
# on fmt, gated at >= 12.1) must resolve to the *same* target or one
# add_library() collides with the other. An unversioned/loosely-versioned
# find_package(fmt) is at the mercy of whatever happens to be installed
# system-wide (this dev machine's system fmt is 9.1.0) - EXACT here
# guarantees this project's own fmt::fmt is always *this* FetchContent-
# built 11.0.2, regardless of host environment, which is what lets the
# FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER override on the slang block
# below correctly dedupe slang's own fmt redeclaration against this one
# rather than either silently using a wrong system version or trying to
# build a second, colliding copy.
find_package(fmt 11.0.2 EXACT CONFIG QUIET)
if(NOT fmt_FOUND)
    FetchContent_Declare(fmt URL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.zip)
    FetchContent_MakeAvailable(fmt)
endif()

# spdlog falls back to FetchContent (same pattern as googletest/
# googlebenchmark below) when no system install is found - it doesn't
# ship in RHEL8 BaseOS/AppStream/CRB by default, needed for a rootless
# Rocky 8 build with no system package installs available at all (see
# backend/scripts/rocky8-bootstrap.sh). macOS/Ubuntu (where find_package
# already succeeds via Homebrew/apt) are unaffected - this only activates
# when the system copy is missing, on any platform.
#
# Version-gated at >= 1.14 (not a plain unversioned find_package) for a
# reason discovered only after fmt above was pinned EXACT/deterministic
# (see that block's own comment): a *precompiled* system spdlog package
# (this dev machine's is 1.12.0, a real .so) bakes in whichever fmt ABI
# it happened to be built against (this machine's system fmt 9.1.0) into
# its own compiled symbol names (fmt::v9::... vs our fmt::v11::...) -
# linking it against this project's own, differently-versioned fmt::fmt
# fails at *link* time (undefined reference to spdlog::details::log_msg
# constructors, header-level usage compiled fine) even though it never
# had this problem before fmt's own version was pinned deterministically.
# Requiring a floor the system package doesn't meet forces this project's
# own FetchContent-built spdlog every time, built fresh against whatever
# fmt::fmt this project actually uses (SPDLOG_FMT_EXTERNAL=ON below) -
# consistent, reproducible, and no longer at the mercy of whatever
# unrelated fmt version happened to compile the host's system package.
find_package(spdlog 1.14 CONFIG QUIET)
if(NOT spdlog_FOUND)
    # Without this, spdlog vendors its own internal fmt copy, conflicting
    # with the many targets in this file that link fmt::fmt directly.
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip)
    FetchContent_MakeAvailable(spdlog)
endif()

# --- slang (SystemVerilog/Verilog frontend for src/sv's netlist/RTL
# reader - see SYSTEMVERILOG.md). Pinned to the v11.0 tagged release (not
# master) - unlike Blend2D/AsmJit above, slang publishes real tags, so it
# follows fmt/spdlog's pin-to-release-zip convention instead. No
# find_package fallback of our own (unlikely to be a preinstalled system
# package on any target platform, including the Rocky 8 rootless build) -
# straight to FetchContent, same as Blend2D. slang builds one single
# CMake target, slang::slang (its own source/CMakeLists.txt does
# add_library(slang_slang ...) + ALIAS) - there is no separate
# syntax-only sub-target; "syntax-only vs full elaboration" is a choice
# of which slang API src/sv's own reader code calls, not a link-time one.
#
# slang's own external/CMakeLists.txt declares its *own* "fmt"
# FetchContent dependency, gated at >= 12.1 via FIND_PACKAGE_ARGS (CMake
# 3.24+'s FetchContent+find_package integration - tries find_package()
# first, falling back to fetching its own copy). Bumping this whole
# project's fmt to 12.1 to satisfy that floor was tried and reverted: fmt
# 12.x renamed/removed fmt::basic_format_string, which both the system
# spdlog package on this dev machine (1.12.0) *and* the newest spdlog
# release this project could pin to (1.14.1) still reference - a real
# compile break neither this project nor slang's own vendored dependency
# graph has a ready answer for yet. Since slang's actual behavior is to
# *always* end up with some real fmt >= 12.1 one way or another
# (reusing a satisfying system/already-available one, or fetching its
# pinned 12.1.0 otherwise) - there's no lower version we can hand it that
# it will just accept - FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER instead
# forces its FIND_PACKAGE_ARGS-decorated redeclaration of "fmt" back to
# plain FetchContent name-based dedup: since this project's own fmt block
# above is now *guaranteed* (EXACT-pinned find_package) to have already
# declared and populated "fmt" as 11.0.2 by the time this line runs,
# slang's own FetchContent_MakeAvailable(fmt) call becomes a no-op reuse
# of that already-populated 11.0.2, rather than either colliding with it
# or fetching a second, separately-versioned copy. This is a real,
# accepted risk (flagged in the linked plan's own build-spike step):
# slang's source is nominally tested against 12.1, and building it
# against 11.0.2 instead depends on slang's own use of fmt being
# API-compatible across that gap - verified empirically by this build
# actually succeeding, not assumed on paper.
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    slang
    URL https://github.com/MikePopoloski/slang/archive/refs/tags/v11.0.zip
)
FetchContent_MakeAvailable(slang)

# find_package(Boost REQUIRED) runs *after* the slang block above -
# discovered empirically during the slang build spike, the mirror image
# of the fmt situation: slang's own external/CMakeLists.txt does its own
# internal find_package(Boost 1.87.0 CONFIG QUIET), and only when that
# fails (this dev machine's system Boost is 1.83.0 - too old to satisfy
# 1.87.0, though functionally almost certainly fine for what slang
# actually uses it for, an unordered_flat_map-style header) does it
# define its own vendored fallback, add_library(Boost::headers ALIAS
# boost_unordered) - unlike fmt, there's no version we can bump our own
# Boost pin to (it's a real system package, not FetchContent-managed
# here), so the two can't be made to share one real target the way fmt's
# two consumers now do. Running slang's block first lets its own
# fallback claim Boost::headers while nothing else has - the real
# upstream Boost CMake package's own target-definition logic (which our
# own find_package(Boost REQUIRED) below triggers) guards each target
# with if(NOT TARGET ...) before defining it, so it cleanly skips
# redefining the one slang's fallback already created, rather than
# colliding with it the way this failed when the order was reversed.
find_package(Boost REQUIRED)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.zip
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# --- Blend2D (a side-experiment alternative Rasterize backend to Skia's -
# see rasterize_blend2d_stage.hpp's own doc comment; PIPELINE_REFACTOR_BENCHMARK_RESULTS.md
# has the benchmark comparisons this exists to produce) + its own required
# AsmJit dependency (Blend2D's JIT-compiled rendering pipelines need it;
# its own CMakeLists.txt looks for a sibling/vendored `3rdparty/asmjit`
# directory via the ASMJIT_DIR cache variable, auto-detected only if not
# already set - see the block below).
#
# Neither project publishes tagged releases (confirmed: blend2d/blend2d
# and asmjit/asmjit both have zero tags on GitHub, development happens
# directly on `master`) - pinned to a specific commit SHA each instead of
# the `URL .../refs/tags/vX.Y.Z.zip` convention every other FetchContent
# dependency in this file uses, via GitHub's own "archive of one commit"
# zip URL (same mechanism, just a commit hash instead of a tag name).
#
# AsmJit is fetched via FetchContent_Populate (NOT FetchContent_MakeAvailable)
# deliberately - it only needs to be *present on disk* here, not built as
# its own top-level subdirectory: Blend2D's own CMakeLists.txt does its
# own `add_subdirectory(${ASMJIT_DIR} ...)` internally once ASMJIT_DIR is
# set, so calling MakeAvailable(asmjit) ourselves as well would add the
# same source directory to the build twice - a real CMake error, not just
# redundant work.
FetchContent_Declare(
    asmjit
    URL https://github.com/asmjit/asmjit/archive/e24347521858616d2d070375ad0d52512e6e44ba.zip
)
FetchContent_GetProperties(asmjit)
if(NOT asmjit_POPULATED)
    FetchContent_Populate(asmjit)
endif()
set(ASMJIT_DIR "${asmjit_SOURCE_DIR}" CACHE PATH "Location of 'asmjit' (fetched, see the Blend2D block above)")

set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    blend2d
    URL https://github.com/blend2d/blend2d/archive/58ca9460b4138af6e793184682e9324e0a8053b2.zip
)
FetchContent_MakeAvailable(blend2d)

# --- oneTBB (backend/ONETBB_INTEGRATION.md migration - src/pipelines
# replaced pipeline/render/instancing at the Phase 5 cutover, and `api`
# now depends on it unconditionally - see api.cpp's own LeHandle comment).
# TBB_DIR is a Homebrew-macOS-specific hint, matching the sibling
# oneTBB_test project's own CMakeLists.txt. REQUIRED (not QUIET/optional
# as it was pre-cutover): api - and therefore le_tcl, le_shell,
# backend_tests - can no longer build without it. Both Docker-based Linux
# paths now provide it too (Dockerfile.linux-ci's own `libtbb-dev`
# package - Ubuntu 24.04's is modern enough to have real CMake config
# support; Dockerfile.linux-release's own `tbb` stage, oneTBB built from
# source, since Rocky 8's own dnf `tbb-devel` is a 2018-era package
# predating that support entirely) - found the hard way, by actually
# running `docker compose run --rm backend` and hitting the exact
# `find_package(TBB)` failure this comment used to only warn about.
# `scripts/rocky8-bootstrap.sh` (the separate rootless-build target, see
# the Rocky 8 bullet in CLAUDE.md's Open gaps) still doesn't provision
# TBB at all - that one remains real, tracked, NOT-yet-solved. ---
if(APPLE)
    set(TBB_DIR "/opt/homebrew/opt/tbb/lib/cmake/TBB" CACHE PATH
        "Path to a oneTBB CMake package config (Homebrew tbb keg)")
endif()
find_package(TBB REQUIRED CONFIG)

# Tracy - the sibling oneTBB_test project's own src/core.hpp (copied into
# src/pipelines/tbb_core.hpp) hard-depends on its ZoneScoped/ZoneName/
# ZoneText/FrameMark profiling macros. Pinned to v0.13.1 to match that
# project's own CMakeLists.txt (kept in lockstep with the Homebrew-bottled
# Tracy GUI viewer's own protocol version, per that project's own
# CLAUDE.md).
#
# TRACY_ENABLE ON - see this file's own CMAKE_OSX_DEPLOYMENT_TARGET
# comment near the top for the real story: this was briefly set OFF after
# Tracy's own background profiler worker thread crashed on startup inside
# the Flutter macOS app bundle (`std::mutex::lock()` throwing "Invalid
# argument", tracy::Thread::Launch -> tracy::Profiler::Worker, SIGABRT) -
# root-caused to this backend's own object code (including TracyClient's)
# having drifted to targeting a much newer macOS SDK minimum than the
# actual app binary it links into, not a real Tracy bug or a reason to
# keep it disabled. Re-enabled once CMAKE_OSX_DEPLOYMENT_TARGET pinned
# both to the same target.
# LE_ENABLE_TRACY: defaults ON everywhere (unchanged behavior for local
# dev builds), overridable to OFF for a build that shouldn't carry
# profiling instrumentation - Dockerfile.linux-release's own Release-tree
# configure passes -DLE_ENABLE_TRACY=OFF for exactly this.
option(LE_ENABLE_TRACY "Enable Tracy profiling instrumentation" ON)
set(TRACY_ENABLE ${LE_ENABLE_TRACY} CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy
    URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.zip
)
FetchContent_MakeAvailable(tracy)

# --- GLFW + Dear ImGui (Dear ImGui prototype) - a native window showing
# le_shell's own rendered pixel buffer, replacing Flutter for the
# CPU-only-Linux-VM deploy target this was prototyped for (Flutter's own
# GPU-oriented rendering assumptions perform poorly there; ImGui's own
# per-frame draw-call count is tiny by comparison). Unconditional -
# le_shell's own `show_gui` command requires this. GLFW is a normal
# CMake package (find_package fallback to FetchContent, same pattern as
# fmt/spdlog above); Dear ImGui ships no CMake project of its own, so
# it's fetched as plain source and given a hand-declared library target
# (imgui_lib) near the rest of this project's own library targets below,
# not here. ---
find_package(glfw3 3.3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # GLFW_BUILD_WAYLAND OFF - on Linux, GLFW's own CMake unconditionally
    # tries to configure a Wayland backend alongside X11 and hard-errors
    # ("Failed to find wayland-scanner") if wayland-scanner isn't on the
    # system, even though this project only ever needs X11 (le_gui's own
    # GLFW usage has no Wayland-specific code) - confirmed by hitting
    # exactly that configure failure on a minimal X11-only Linux image
    # (Dockerfile.linux-ci/Dockerfile.linux-release, neither of which
    # installs Wayland dev packages on purpose, matching le_gui's own
    # X11-only scope). GLFW_BUILD_X11 stays at its own default (ON).
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw URL https://github.com/glfw/glfw/archive/refs/tags/3.5.1.zip)
    FetchContent_MakeAvailable(glfw)
endif()

# The `docking` branch, not a tagged mainline release - window docking
# (drag a panel to split/tab/rearrange it, matching the Flutter frontend's
# own `docking` package-based layout - see src/gui/le_gui.cpp's own
# dockspace comment) is a long-lived, actively-maintained feature branch
# upstream, not yet merged to master as of this writing, and is what this
# prototype needs to mirror the Flutter UI's left/center/right panel
# layout. Pinned to a specific commit (not a moving branch ref) for
# reproducible builds, same as every other FetchContent dependency above
# being pinned to a specific tag/version - bump this by hand (check
# https://github.com/ocornut/imgui/commits/docking) when a newer commit
# is wanted, same as bumping any other pinned dependency version.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5 # docking branch, 2026-09-01
)
FetchContent_MakeAvailable(imgui)

# Lucide icon font (src/gui/components/ - ModeSelector/ModeToolbar and
# any later toolbar button) - Dear ImGui has no icon-widget concept of
# its own; the standard technique is merging a small icon font's glyphs
# into the same font atlas as the regular text font (ImFontConfig::MergeMode,
# see le_gui.cpp's own font-setup code) and drawing an icon as plain text
# via its own Unicode codepoint. Lucide (MIT) rather than the more common
# Font Awesome: its outline/stroke icon style matches the Flutter
# frontend's own HugeIcons "strokeRounded" look far more closely than
# Font Awesome's free tier, which is mostly filled "Solid" glyphs.
# IconsLucide.h (juliettef/IconFontCppHeaders - ICON_LC_* codepoint
# constants for C/C++) has to be pinned to the *exact* commit that
# generated it against a specific lucide-static release - its own
# generated header comment names the version each regeneration was built
# from (here, "Lucide 1.17.0"); an unpinned `main`/`@latest` fetch of
# either half alone risks the two drifting out of sync (a later Lucide
# release renumbering/removing codepoints this header's own constants
# still point at). DOWNLOAD_NO_EXTRACT - both are a single loose file,
# not an archive with a build system inside like every other
# FetchContent dependency above.
FetchContent_Declare(
    icon_font_header
    URL https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/780f04406084230c1b034c2c34c0c0b8b9b1829b/IconsLucide.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(icon_font_header)

FetchContent_Declare(
    lucide_font
    URL https://unpkg.com/lucide-static@1.17.0/font/lucide.ttf
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(lucide_font)
# Also copied into the build tree next to where le_shell/le_tcl.so land
# (not just left in FetchContent's own cache dir under CMAKE_BINARY_DIR/
# _deps) - LE_LUCIDE_FONT_PATH (below) is this build machine's own
# absolute FetchContent cache path, correct for a local dev/ctest run but
# never valid once le_shell is copied to another machine; le_gui.cpp's
# own runtime fallback looks for a file named exactly "lucide.ttf" right
# next to the running executable when that compile-time path doesn't
# exist (mirroring pipelines.cpp's default_blend2d_font_face() fallback). Having a
# real copy sitting right next to le_shell in the build tree also means
# Dockerfile.linux-release's own bundle stage can just `cp` it alongside
# le_shell the same simple way it already does for every other bundled
# file, rather than needing to know this FetchContent-internal path.
file(COPY "${lucide_font_SOURCE_DIR}/lucide.ttf" DESTINATION "${CMAKE_BINARY_DIR}")

enable_testing()
include(GoogleTest)

# --- database: header-only, generated from src/database/schema.py via cmg ---
add_library(database INTERFACE)
target_include_directories(database INTERFACE src/database src/database/generated)

# --- geometry: header-only Boost.Geometry wrapper over database types ---
#
# Links database's own includes plus Boost_INCLUDE_DIRS directly, not
# Boost::boost - a real, confirmed bug on any platform where
# find_package(Boost REQUIRED) below resolves via CMake's bundled legacy
# FindBoost.cmake MODULE (no real Boost CMake CONFIG package installed,
# e.g. Dockerfile.linux-release/rocky8-bootstrap.sh's raw-source-plus-
# BOOST_ROOT approach - see this file's own find_package(Boost REQUIRED)
# comment above for why slang's own vendored fallback claims
# Boost::headers first). That legacy module, seeing Boost::headers
# already claimed by slang's own incomplete single-header stand-in,
# defines Boost::boost as a bare `ALIAS Boost::headers` instead of an
# independent target pointing at the real BOOST_ROOT-detected includes -
# so geometry/io silently built against slang's own tiny vendored
# boost_unordered shim (no boost/geometry.hpp at all) even though
# find_package's own status message correctly reported finding real
# Boost. Confirmed via a minimal local repro reproducing the exact
# compile line (-DSLANG_BOOST_SINGLE_HEADER, no -I for real Boost) -
# never hit on the macOS/Ubuntu dev machines, which have a real Boost
# CMake CONFIG package installed system-wide (a different, apparently
# collision-free code path). Boost_INCLUDE_DIRS itself is unaffected by
# any of this target-aliasing, so using it directly sidesteps the whole
# landmine on every platform uniformly.
add_library(geometry INTERFACE)
target_include_directories(geometry INTERFACE src ${Boost_INCLUDE_DIRS})
target_link_libraries(geometry INTERFACE database)

# --- view_style: header-only ViewLayer/purpose/color config, distinct from
# the LEF-mirroring database - see CLAUDE.md for why these aren't schema/cmg-generated ---
add_library(view_style INTERFACE)
target_include_directories(view_style INTERFACE src)
target_link_libraries(view_style INTERFACE database)

# --- editing: header-only undo/redo command recording (UPDATES.md item 21) ---
add_library(editing INTERFACE)
target_include_directories(editing INTERFACE src)
target_link_libraries(editing INTERFACE database)

# --- core: header-only generic building blocks (RenderedShape/TinyShapeDot,
# VersionedStage) shared by `pipelines` and by HierarchyResolver's own
# recursive core, which stays on VersionedStage rather than MemoizingStage
# (backend/ONETBB_INTEGRATION.md migration plan, decision 1). See
# UPDATES.md item 16. ---
add_library(core INTERFACE)
target_include_directories(core INTERFACE src)
target_link_libraries(core INTERFACE database view_style)

# --- pipelines: PIPELINE_REFACTOR.md restart - the Cold/Warm/Hot
# ViewRenderPipeline, built on oneTBB's flow::graph (tbb_core.hpp's
# MemoizingStage/FanInCollectStage). The pre-restart module (STATIC,
# compiled for its own Skia/CoreText default_typeface() - see git history)
# has been moved to src/pipelines.old/ and is being rebuilt one stage at a
# time (PIPELINE_REFACTOR.md's own methodology). Was header-only (INTERFACE)
# through the Cold tier and Warm stage 1 (ViewportCullStage, no Skia
# dependency); reverted to STATIC here with the Warm tier's own
# Rasterization/Compose stages, which do need a real compiled TU -
# pipelines.cpp defines default_blend2d_font_face() for the same reason the
# pre-restart module's own compiled TU existed (see that file's own
# comment). No Skia dependency anywhere in this project anymore
# (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md - the earlier Skia-based
# RasterizeStage and its own Skia-based ComposeStage sibling are both
# gone, RasterizeBlend2DStage/Blend2D-native ComposeStage are the only
# Rasterize/Compose implementations now) - le_tcl's own last remaining
# Skia consumer, dump_png_cmd's PNG export, was ported onto Blend2D's own
# built-in PNG codec (BLImage::write_to_file) too, so the `skia` CMake
# target that used to exist here (a pre-built checkout, SKIA_DIR) is gone.
add_library(pipelines STATIC src/pipelines/pipelines.cpp)
target_include_directories(pipelines PUBLIC src)

target_link_libraries(pipelines PUBLIC database view_style core blend2d::blend2d fmt::fmt spdlog::spdlog TBB::tbb TracyClient)

# LE_FONT_DIR: default_blend2d_font_face() (pipelines.cpp/blend2d_font.hpp)
# loads its font from this bundled directory rather than system
# fontconfig, so text labels still render on a locked-down target machine
# with no fonts installed at all, on every platform - Blend2D has no
# font-manager abstraction to fall back on the way Skia's CoreText path
# does on macOS, so it always loads one specific bundled file from here
# directly - see pipelines.cpp's own comment.
set(LE_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" CACHE PATH
    "Directory of bundled fonts default_blend2d_font_face() (pipelines.cpp) loads from")
target_compile_definitions(pipelines PRIVATE LE_FONT_DIR="${LE_FONT_DIR}")

# --- io: LEF/DEF file readers ---
add_library(io STATIC src/io/lef_reader.cpp src/io/lef_writer.cpp src/io/def_reader.cpp src/io/def_writer.cpp)
add_dependencies(io lef_lib def_lib)
target_include_directories(io
    PUBLIC src/io
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/include
)
target_link_libraries(io
    # spdlog PUBLIC (not PRIVATE): lef_reader.hpp - io's own PUBLIC header -
    # #includes spdlog/spdlog.h directly, so any consumer of that header
    # needs spdlog's include path too, not just io's own .cpp files. Went
    # unnoticed on macOS/Ubuntu (system spdlog headers sit on a default
    # compiler search path regardless of CMake link scope); surfaced for
    # real on a FetchContent-built spdlog (Rocky 8, no system package) -
    # lef_roundtrip_diff.cpp failed with spdlog/spdlog.h: No such file or
    # directory until this was PUBLIC. fmt stays PRIVATE - only io's own
    # .cpp files use it directly.
    PUBLIC database geometry spdlog::spdlog
    PRIVATE fmt::fmt lef_static def_static
)
le_enable_coverage_instrumentation(io)

# --- sv: SystemVerilog/Verilog netlist+RTL reader (SYSTEMVERILOG.md) ---
add_library(sv STATIC src/sv/sv_reader.cpp)
target_include_directories(sv PUBLIC src/sv)
# slang::slang PUBLIC (not PRIVATE): sv_reader.hpp - sv's own PUBLIC header -
# #includes slang/ast/SemanticFacts.h directly (SVReader::
# signal_direction_from_parser's public signature takes a real
# slang::ast::ArgumentDirection, matching LEFReader::signal_direction_from_parser's
# own directly-unit-testable pure-conversion convention) - so any
# consumer of that header (api.cpp) needs slang's own include path too,
# not just sv's own .cpp files. Same reasoning as io's own PUBLIC
# spdlog::spdlog dependency above.
target_link_libraries(sv PUBLIC database spdlog::spdlog slang::slang PRIVATE fmt::fmt)
le_enable_coverage_instrumentation(sv)

# --- api: the C API surface a Flutter plugin's Dart FFI binds to (see
# README's `api` module entry). Compiled (not header-only): the opaque
# LeHandle's real definition must stay out of api.hpp, which needs to be a
# plain-C-parseable header for ffigen/Dart FFI - see api.hpp's own comment. ---
add_library(api STATIC src/api/api.cpp)
target_include_directories(api PUBLIC src/api)
target_link_libraries(api PRIVATE database geometry editing view_style pipelines io sv)
le_enable_coverage_instrumentation(api)

# --- gui: Dear ImGui prototype - le_shell's own `show_gui` window,
# unconditional (le_shell requires it). Depends on `api` (LeHandle*/le_*
# calls) plus GLFW/Dear ImGui - deliberately no dependency on Tcl/SWIG at
# all, and no knowledge that le_shell exists (see src/gui/le_gui.hpp's
# own doc comment for the module boundary/threading rationale). Dear
# ImGui ships no CMake project of its own (fetched as plain source
# above), so imgui_lib is a small hand-declared static library wrapping
# its core sources plus the glfw/opengl3 backend files - the standard
# way projects consume it. No external GL loader dependency: every GL
# function this needs (texture upload) is part of OpenGL 1.1's core
# spec, declared directly by the system GL headers on both target
# platforms, and Dear ImGui's own opengl3 backend bundles its own
# minimal loader for the handful of GL3.2 core-profile calls it needs
# internally (no GLEW/GLAD/gl3w dependency needed either, unlike many
# older ImGui integrations). ---
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# Same target name (`glfw`) whether it came from find_package(glfw3) or
# the FetchContent fallback above.
target_link_libraries(imgui_lib PUBLIC glfw)

add_library(gui STATIC
    src/gui/le_gui.cpp
    src/gui/components/status_bar.cpp
    src/gui/components/library_browser.cpp
    src/gui/components/property_viewer.cpp
    src/gui/components/layer_manager.cpp
    src/gui/components/mode_selector.cpp
    src/gui/components/mode_toolbar.cpp
)
target_include_directories(gui PUBLIC src/gui)
target_include_directories(gui PRIVATE ${icon_font_header_SOURCE_DIR})
# le_gui.cpp's own font-setup code loads this from disk at runtime
# (ImGui merges icon glyphs into the text font atlas by loading a real
# .ttf file, not compiling one in) - a build-tree path is fine for this
# local-dev prototype, same as this project's own fixture-path compile
# definitions elsewhere (e.g. API_TEST_FIXTURES_DIR).
target_compile_definitions(gui PRIVATE LE_LUCIDE_FONT_PATH="${lucide_font_SOURCE_DIR}/lucide.ttf")
# spdlog::spdlog directly (not just transitively through api, which links
# pipelines - spdlog's own consumer - PRIVATE, so nothing propagates this
# far): le_gui.cpp uses spdlog/fmt directly, and without this it silently
# picks up whatever system fmt/spdlog headers happen to be on the default
# include path at compile time instead of this project's own pinned
# fmt::fmt (EXACT-pinned, see that block's own CMakeLists.txt comment) -
# a real link-time ABI mismatch (fmt::v9 object code linked against this
# project's own fmt::v11 library), same class of bug io's own PUBLIC
# spdlog::spdlog dependency exists to avoid.
target_link_libraries(gui PUBLIC api imgui_lib spdlog::spdlog)
if(APPLE)
    target_link_libraries(gui PRIVATE "-framework Cocoa" "-framework IOKit" "-framework OpenGL")
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(gui PRIVATE OpenGL::GL)
endif()

# --- le_tcl: Phase 0 SWIG toolchain spike (UPDATES.md item 15 /
# TCL_EXPLORATION.md) - SWIG-wraps src/tcl/le_tcl_shim.{hpp,cpp} (a thin
# Tcl-ergonomics layer over api.hpp - hidden session handle, domain-verb
# command names) into a loadable Tcl extension, proving the build/link
# story before any CRUD/filter-search work exists to wrap for real.
# Optional: only built if SWIG and Tcl are both found, so a machine not
# yet provisioned for this (e.g. CI) still configures/builds everything
# else. Pinned to Homebrew's tcl-tk@8 (Tcl 8.6), not the also-installed
# tcl-tk 9.0 - SWIG 4.5's Tcl support is battle-tested against 8.x, and
# Tcl 9's C ABI (released 2025) isn't yet a safe default target for this
# spike. Override TCL_TK_ROOT for a different Tcl 8.x install. ---
set(TCL_TK_ROOT "/opt/homebrew/opt/tcl-tk@8" CACHE PATH
    "Path to a Tcl 8.6 install (Homebrew tcl-tk@8 keg) used for the SWIG Tcl spike")
if(EXISTS "${TCL_TK_ROOT}/include/tcl-tk/tcl.h")
    set(TCL_INCLUDE_PATH "${TCL_TK_ROOT}/include/tcl-tk" CACHE PATH "" FORCE)
    set(TCL_LIBRARY "${TCL_TK_ROOT}/lib/libtcl8.6.dylib" CACHE FILEPATH "" FORCE)
    set(TCL_TCLSH "${TCL_TK_ROOT}/bin/tclsh8.6" CACHE FILEPATH "" FORCE)
endif()

find_package(SWIG 4.0)
find_package(TCL)

if(SWIG_FOUND AND TCL_FOUND)
    include(UseSWIG)
    set_property(SOURCE src/tcl/le_api.i PROPERTY CPLUSPLUS ON)
    swig_add_library(le_tcl
        LANGUAGE tcl
        SOURCES src/tcl/le_api.i src/tcl/le_tcl_shim.cpp
    )
    set_property(TARGET le_tcl PROPERTY SWIG_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/api ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl)
    target_include_directories(le_tcl PRIVATE src/api src/tcl ${TCL_INCLUDE_PATH})
    # blend2d::blend2d alongside `api` - `pipelines` (api's own dependency)
    # links it too, but only PUBLIC-propagates it as far as api, not this
    # far; le_tcl_shim.cpp needs it directly for dump_png_cmd's own
    # BLImage::write_to_file PNG export (no Skia dependency anywhere in
    # this project anymore - see the `pipelines` target's own comment
    # above).
    target_link_libraries(le_tcl PRIVATE api blend2d::blend2d ${TCL_LIBRARY})
    # Hide this target's own copy of the vendored LEF parser's symbols
    # (everything under the LefDefParser:: namespace -
    # src/tcl/le_tcl_unexported_symbols.txt). Without this, a process
    # that *also* separately links the same static libraries
    # (api/io/render/liblef.a) into another Mach-O image (e.g.
    # layout_engine_plugin.framework, for LeTclBridge's embedded Tcl
    # console - see TCL_EXPLORATION.md's show_gui section) crashes: dyld
    # binds a call inside this module's own copy of the vendored parser
    # (specifically LefDefParser::lefGetKeyword's internal keyword-table
    # std::map, a function-local static) to the *other* image's
    # differently-initialized copy of the same symbol instead of this
    # module's own, reading through a garbage/uninitialized pointer -
    # reproduced headlessly and fixed here after a real crash report,
    # see ReadLefThroughTclFirstDoesNotCrash in
    # src/tcl/tests/session_handle_test.cpp.
    #
    # Deliberately a *linker*-level denylist
    # (-unexported_symbols_list), not CXX_VISIBILITY_PRESET or a
    # blanket -exported_symbols_list allowlist - two things learned by
    # trial, not assumed:
    # - CXX_VISIBILITY_PRESET only sets compile flags for this target's
    #   own two listed sources (le_tcl_shim.cpp, the SWIG wrap file) -
    #   api/io/render/lef_lib are separate CMake targets, already
    #   compiled into .a archives with default (visible) symbols, and a
    #   consuming target's own visibility preset can't retroactively
    #   hide symbols already baked into object code linked in from
    #   elsewhere. Confirmed by trial: with only CXX_VISIBILITY_PRESET
    #   set, the crash reproduced identically.
    # - A blanket -exported_symbols_list allowlist (hide everything
    #   except two named entry points) does stop the original crash, but
    #   breaks something worse: it also hides libc++ typeinfo symbols
    #   that are *supposed* to stay coalescable across dylib boundaries
    #   (weak/vague linkage), so std::system_error thrown deep in the
    #   parser's std::vector/std::map usage stops RTTI-matching a
    #   `catch (const std::exception&)` in the calling image - "unknown
    #   C++ exception" instead of a clean TCL_ERROR. Confirmed by trial
    #   (abi::__cxa_current_exception_type()->name() ==
    #   "NSt3__112system_errorE", uncaught by a std::exception handler).
    #   A denylist scoped to exactly the confirmed culprit
    #   (LefDefParser::) avoids this - everything else, including any
    #   std:: template instantiated with a LefDefParser:: type as a
    #   parameter (e.g. exception guards for vector<lefrOBSSpacing>),
    #   keeps its normal default visibility and stays coalescable.
    #
    # Investigating a reported get_shapes crash (mutex::lock() throwing
    # system_error inside le_get_shapes) found the exact same bug SHAPE in
    # a second place: le::Root (src/database/generated/root.hpp, header-
    # only/inline, so - like LefDefParser:: - compiled separately into
    # this target and into whatever else statically links `api`) has ~10
    # accessors (get_terminal_port_shapes, get_obstruction_shapes,
    # get_abstract_terminals, get_terminal_ports, get_technology_layers,
    # etc.) that return `static const std::vector<X> empty;`/`static
    # const XxxId empty;` as their "not found" sentinel - a function-local
    # magic static, exported with default visibility exactly like
    # lefGetKeyword's keyword table was (confirmed via `nm` that both this
    # target's build/le_tcl.so and build_release/libapi.a export the same
    # symbol names before this fix). `__Z*2le4Root*` closes it the same
    # way - confirmed via `nm` that the ::empty statics and their guard
    # variables are local (t/b) afterward, not just the accessors
    # themselves. `2le4Root` (not e.g. `le4Root`) is the exact Itanium
    # mangling substring for the `le::Root::` scope prefix - confirmed by
    # grepping both built libraries' full symbol dumps that it doesn't
    # false-match symbols merely mentioning Root as a template argument
    # (those use mangling substitution compression instead of re-spelling
    # `2le4Root`) or any typeinfo/vtable symbol (Root isn't part of any
    # exception/RTTI hierarchy). Note: a from-scratch repro (this
    # target's Debug build force-loaded alongside build_release's Release
    # libapi.a in one process, driving get_shapes/get_terminal_ports/
    # get_properties through Tcl repeatedly, including through two
    # independent Tcl_Interps loading the already-resident module) did
    # NOT reproduce an actual crash from this gap even before this fix -
    # most of these sentinels are all-zero-safe (an empty std::vector's
    # default-constructed bit pattern is valid regardless of which
    # image's copy of the guard "wins" a coalescing race), so this closes
    # a real, confirmed-duplicated hazard without itself being confirmed
    # as the original report's actual cause.
    #
    # A third instance of the exact same bug (backend/ONETBB_INTEGRATION.md
    # migration) - `tracy::` (Tracy's own global Profiler singleton and its
    # background worker threads, pulled in transitively via `api` ->
    # `pipelines` -> the STATIC `TracyClient` library - a genuine STATIC
    # library gets its own separate copy of code baked into every image
    # that links it, unlike TBB's own runtime, which stays a single shared
    # instance across images since libtbb is a dylib, not a static lib).
    # This one WAS confirmed as a real crash's actual cause, not just a
    # closed-preemptively gap like le::Root above: a real crash report
    # showed `le_tcl.so` loaded into the same process as
    # flutter_plugin's own main app binary (both independently linking
    # `api`/`pipelines`/`TracyClient`), and Tracy's own background
    # worker thread (tracy::Profiler::Worker, already running against the
    # main binary's own copy of the Profiler singleton) crashed with
    # `std::mutex::lock() "Invalid argument"` at almost exactly the
    # moment le_tcl.so's own Tcl bridge was first invoked (the frontend's
    # "Load Test Data" button, calling into a `read_lef` Tcl command) -
    # dyld's flat-namespace symbol coalescing rebinding some subset of
    # calls to le_tcl.so's own freshly-loaded, differently-initialized
    # copy of the same singleton state out from under the already-running
    # worker thread. `__ZN5tracy*` (not just an exact-namespace match -
    # Tracy vendors its own `tracy::moodycamel::` queue implementation
    # nested inside the same `tracy` namespace, which needs covering too)
    # closes this the same way the other two entries do.
    #
    # A fourth instance (BUGS_AND_ENHANCEMENTS.md E10) - oneapi::tbb::
    # global_control (LeHandle's own concurrency_control_, api.cpp).
    # Unlike tracy:: above, oneTBB's actual runtime state genuinely IS a
    # single shared instance across images (libtbb is a real dylib, not a
    # static library baked into `api` - see this comment block's own
    # opening paragraph) - but tbb::detail::d1::global_control's own
    # constructor/destructor bodies are header-inline (oneapi/tbb/
    # global_control.h), so they still get compiled separately into this
    # target's own object code AND into whatever else statically links
    # `api`, the exact "duplicated code, coalescable across the dylib
    # boundary" shape every entry above shares - confirmed via `nm
    # build/libapi.a`: __ZN3tbb6detail2d114global_controlC1/C2/D1/D2 (and
    # the std::optional<global_control> wrapper functions LeHandle's own
    # std::optional<global_control> member needs) are all default-
    # visibility (T) symbols baked directly into this archive, not merely
    # forwarding calls to libtbb.dylib. This one WAS reproduced as a real
    # crash (not just closed preemptively like le::Root above):
    # SessionHandle.InjectedHandleIsSharedNotFresh/GetShapesThroughTclDoesNotCrash
    # (session_handle_test.cpp, the same "this module + a directly-linked
    # copy of `api` in one process" shape every entry here needs) SIGTRAP'd/
    # segfaulted the moment LeHandle's constructor ran through this
    # module's own copy of global_control's constructor with the other
    # image's copy already live - fixed by adding this pattern here,
    # confirmed clean (5/5) afterward. `2d114global_control` (not e.g.
    # `14global_control` alone) is the exact Itanium mangling substring
    # for the `tbb::detail::d1::global_control` scope prefix - narrow
    # enough to leave `tbb::detail::r1::` (the genuinely-shared-across-
    # images runtime entry points every d1:: call forwards into) and
    # every other tbb:: symbol coalescable as normal.
    #
    # Second gotcha: editing le_tcl_unexported_symbols.txt alone does not
    # reliably trigger a relink - it's passed to the linker as a raw path
    # inside a string here, so CMake/ninja/make don't know to treat it as
    # a build dependency and can report `le_tcl` "up to date" (skipping
    # the link step and its `nm`-visible effect entirely) after only the
    # list file changed. `rm build*/le_tcl.so` (or touch
    # src/tcl/le_tcl_shim.cpp) before rebuilding this target if a change
    # here doesn't seem to take effect.
    #
    # Third gotcha hit while verifying this: running
    # src/tcl/tests/session_handle_test.cpp's binary directly (all 4
    # TEST()s in one process) makes ReadLefThroughTclFirstDoesNotCrash and
    # DirectReadThenSingleTclInterpReadBothSucceed fail with "Unknown C++
    # exception thrown" - looking like this fix broke RTTI coalescing
    # exactly like the "blanket allowlist" mistake above. It didn't - that
    # file's own header comment already explains why: each TEST() is
    # meant to run as its own ctest case via gtest_discover_tests, in its
    # own fresh process, specifically because chaining several of these
    # handle-inject/destroy sequences in one process corrupts the shared
    # session()/injected_handle() state in le_tcl_shim.cpp (an
    # anonymous-namespace static, process-wide, not per-Tcl_Interp) -
    # unrelated to this fix and reproducible identically without it. Use
    # `ctest --test-dir build -R SessionHandle`, not the raw binary, to
    # get a real signal - confirmed clean (4/4) with this fix applied.
    # -unexported_symbols_list is an Apple ld64 flag, and the list file
    # above uses Mach-O's underscore-prefixed symbol names - both specific
    # to the macOS dyld flat-namespace coalescing bug this works around
    # (see the comment block above), which only arises when this module
    # and layout_engine_plugin.framework separately link the same static
    # libraries into one macOS process. Linux CI never does that (le_tcl.so
    # here is only ever loaded standalone, by ctest/tclsh), so there's no
    # GNU-ld equivalent to wire up.
    if(APPLE)
        target_link_options(le_tcl PRIVATE
            "-Wl,-unexported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_unexported_symbols.txt"
        )
    endif()

    # Regression check for the spike itself (src/tcl/tests/smoke_test.tcl) -
    # loads the built module under tclsh, sources le_tcl_procs.tcl, and
    # exercises the ergonomic command surface against the same
    # testcell.lef fixture api_test.cpp's C++ tests already use, asserting
    # the same expected values so this doesn't silently bit-rot.
    add_test(
        NAME le_tcl_smoke
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/smoke_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # Regression check for the integrated help system (UPDATES.md item 20 -
    # src/tcl/tests/help_test.tcl): the ::command_help registry, help/man/
    # complete_command/generate_command_docs, and the generated -help
    # support for get_<type>/create_<type>/update_<type>. No LEF fixture -
    # everything it exercises is either pure static schema metadata
    # (complete_command's dot-path completion) or works before any LEF is
    # ever read (registration itself).
    add_test(
        NAME le_tcl_help
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/help_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
    )

    # Phase 5 regression check (src/tcl/tests/crud_test.tcl) - exercises
    # the full Terminal/TerminalPort/Obstruction/Shape CRUD+search
    # surface and the coordinate-list typemap, against the same
    # testcell.lef fixture.
    add_test(
        NAME le_tcl_crud
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/crud_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/othercell.lef
    )

    # Regression check for the SystemVerilog/Verilog TCL wiring
    # (SYSTEMVERILOG.md, src/tcl/tests/sv_test.tcl) - read_verilog plus
    # the generated get_designs/get_schematics/get_instances/get_ports/
    # get_pins surface, against the same gate_netlist_clean.v fixture
    # src/sv/tests's own C++ tests already use.
    add_test(
        NAME le_tcl_sv
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/sv_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/sv/tests/fixtures/gate_netlist_clean.v
    )

    # Phase 5 mutation side-effect regression check
    # (LINKING_STRATEGY_RESEARCH.md section 5, src/tcl/tests/
    # mutation_cascade_test.tcl) - delete_net cascade and update_net/
    # update_instance rename propagation, through the real TCL layer.
    add_test(
        NAME le_tcl_mutation_cascade
        COMMAND ${TCL_TCLSH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/mutation_cascade_test.tcl
            $<TARGET_FILE:le_tcl>
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # le_shell (src/tcl/le_shell.cpp) - the interactive/batch Tcl console,
    # not linked against le_tcl itself (it dynamically `load`s the built
    # shared module at startup, same as tclsh would) - only needs Tcl
    # directly for that. Links `api` to call le_create()/le_destroy()
    # itself and own the LeHandle it injects into the console via
    # set_session_handle (see le_shell.cpp's own comment), `gui` for its
    # own `show_gui` window, and readline for its own interactive prompt
    # (real line editing/recall history/Tab completion - see
    # le_shell.cpp's own header comment for why this replaced Tcl_Main's
    # own hardcoded REPL) - all three unconditional, not optional
    # features: this shell is the *only* user-facing way to run Tcl
    # commands now that the Flutter Terminal widget isn't the primary
    # interactive surface, so it needs the real window and the real
    # console experience every time, not a degraded fallback.
    # BUGS_AND_ENHANCEMENTS.md E26 - le_shell's own version/build-date
    # banner (le_shell.cpp's own run_interactive()), regenerated fresh on
    # every build (not just every configure) - see
    # cmake/generate_le_shell_version.cmake's own header comment for why
    # this is a custom target (always out of date) rather than a plain
    # configure_file() call.
    add_custom_target(generate_le_shell_version ALL
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPROJECT_VERSION=${PROJECT_VERSION}
            -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/generated/le_shell_version.hpp
            -P ${CMAKE_SOURCE_DIR}/cmake/generate_le_shell_version.cmake
        BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/le_shell_version.hpp
        COMMENT "Generating le_shell version header"
    )

    add_executable(le_shell src/tcl/le_shell.cpp)
    add_dependencies(le_shell generate_le_shell_version)
    target_include_directories(le_shell PRIVATE ${TCL_INCLUDE_PATH} ${CMAKE_BINARY_DIR})
    target_link_libraries(le_shell PRIVATE ${TCL_LIBRARY} api gui)

    # Compile-time fallback for -module/-procs and their LE_TCL_MODULE/
    # LE_TCL_PROCS_PATH env-var equivalents (le_shell.cpp's own
    # resolve_path) - this build tree's own known locations for le_tcl's
    # built module and le_tcl_procs.tcl, same paths the le_tcl_shell/
    # le_tcl_session_test targets above already pass explicitly. Lets a
    # plain `./le_shell` run with no flags or env vars set for the common
    # case (running straight out of this build tree); either override
    # mechanism still works for a packaged/relocated binary.
    target_compile_definitions(le_shell PRIVATE
        LE_TCL_MODULE_DEFAULT_PATH="$<TARGET_FILE:le_tcl>"
        LE_TCL_PROCS_DEFAULT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl"
    )
    add_dependencies(le_shell le_tcl)

    # GNU readline (GPL - this project carries no LICENSE file/
    # redistribution requirement of its own, so that's not a blocker
    # here) rather than macOS's own BSD-licensed libedit: modern macOS no
    # longer ships libedit's readline-compatible headers under
    # /usr/include at all (checked directly on this dev machine), so
    # there's no zero-dependency system option left to prefer over an
    # explicit one either way. REQUIRED, same as e.g. Boost above - a
    # missing readline keg is a configure-time error, not a silent
    # feature downgrade.
    set(READLINE_ROOT "/opt/homebrew/opt/readline" CACHE PATH
        "Path to a readline install (Homebrew readline keg) for le_shell's own interactive editing/history/Tab-completion")
    find_path(READLINE_INCLUDE_DIR readline/readline.h HINTS ${READLINE_ROOT}/include)
    find_library(READLINE_LIBRARY NAMES readline HINTS ${READLINE_ROOT}/lib)
    find_library(READLINE_HISTORY_LIBRARY NAMES history HINTS ${READLINE_ROOT}/lib)
    if(NOT (READLINE_INCLUDE_DIR AND READLINE_LIBRARY AND READLINE_HISTORY_LIBRARY))
        message(FATAL_ERROR "readline not found (checked ${READLINE_ROOT}) - install it (e.g. `brew install readline`) or set READLINE_ROOT")
    endif()
    target_include_directories(le_shell PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(le_shell PRIVATE ${READLINE_LIBRARY} ${READLINE_HISTORY_LIBRARY})

    # Both le_shell and le_tcl (its dynamically `load`ed Tcl module) link
    # TBB::tbb (via api -> pipelines) as a real shared library, not a
    # static one - on Linux this means libtbb.so.12 has to be findable at
    # runtime, not just link time. CMake's own default build-tree RPATH
    # already covers that inside this build tree (an absolute path to
    # wherever find_package(TBB) resolved it, e.g. /opt/tbb/lib64 in the
    # release Docker image - which is why ctest/le_shell both already work
    # fine *inside* that image) - but Dockerfile.linux-release's own
    # `bundle` stage copies just the le_shell/le_tcl.so binaries (plus a
    # copy of libtbb.so.12 itself) into one flat directory, and a plain
    # user checkout on another machine (e.g. the Rocky 8 target machine
    # BUILD.md describes) has no /opt/tbb at all - confirmed via a real
    # "error while loading shared libraries: libtbb.so.12: cannot open
    # shared object file" report running that bundle's own le_shell.
    # BUILD_RPATH (not INSTALL_RPATH/BUILD_WITH_INSTALL_RPATH) ADDS
    # "$ORIGIN" (an ELF/ld.so token, expanded to whichever directory the
    # referencing binary itself lives in, not cwd) to that default
    # build-tree rpath rather than replacing it - so both the in-tree
    # /opt/tbb/lib64 entry ctest still needs today and the bundle's own
    # "look next to me" fallback stay intact simultaneously. $ORIGIN is a
    # Linux/ELF-only token (macOS's Mach-O equivalent is @loader_path,
    # spelled differently), and macOS dev builds never need this at all
    # (le_shell always runs straight from the build tree there) - Apple
    # gated accordingly.
    if(NOT APPLE)
        set_target_properties(le_shell le_tcl PROPERTIES
            BUILD_RPATH "$ORIGIN"
        )
    endif()

    # Batch-mode regression check (src/tcl/tests/shell_test.tcl) - runs
    # through the real compiled le_shell binary (not tclsh directly
    # loading le_tcl, which smoke_test.tcl/crud_test.tcl already cover),
    # verifying its own bootstrap (argv flag handling, `load`, sourcing
    # le_tcl_procs.tcl) actually works end to end.
    add_test(
        NAME le_tcl_shell
        COMMAND $<TARGET_FILE:le_shell>
            -module $<TARGET_FILE:le_tcl>
            -procs ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/tests/shell_test.tcl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures/testcell.lef
    )

    # show_gui design regression check (src/tcl/tests/session_handle_test.cpp) -
    # a plain GTest binary, not a Tcl script: proves set_session_handle()
    # actually redirects le_tcl_shim.cpp's session() to an externally-
    # owned LeHandle*, the mechanism a Flutter-embedded Tcl console
    # relies on (see TCL_EXPLORATION.md's show_gui section). Kept out of
    # backend_tests (unconditional, built even without SWIG/TCL) since
    # this needs both.
    add_executable(le_tcl_session_test src/tcl/tests/session_handle_test.cpp)
    target_include_directories(le_tcl_session_test PRIVATE src/api ${TCL_INCLUDE_PATH})
    target_link_libraries(le_tcl_session_test PRIVATE api ${TCL_LIBRARY} GTest::gtest_main)
    target_compile_definitions(le_tcl_session_test PRIVATE
        API_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures"
        LE_TCL_MODULE_PATH="$<TARGET_FILE:le_tcl>"
        LE_TCL_PROCS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/tcl/le_tcl_procs.tcl"
    )
    add_dependencies(le_tcl_session_test le_tcl)
    gtest_discover_tests(le_tcl_session_test)
else()
    message(STATUS "SWIG and/or Tcl not found - skipping le_tcl spike target (see TCL_EXPLORATION.md Phase 0)")
endif()

# --- tests ---
add_executable(backend_tests
    src/database/tests/database_test.cpp
    src/database/tests/filter_test.cpp
    src/database/tests/hierarchical_resolver_test.cpp
    src/database/tests/schematic_layout_linker_test.cpp
    src/geometry/tests/geometry_test.cpp
    src/view_style/tests/view_style_test.cpp
    src/editing/tests/editing_test.cpp
    src/io/tests/lef_reader_test.cpp
    src/io/tests/lef_reader_utils_test.cpp
    src/io/tests/lef_writer_test.cpp
    src/io/tests/def_reader_test.cpp
    src/io/tests/def_writer_test.cpp
    src/api/tests/api_test.cpp
    src/api/tests/le_handle_test.cpp
    src/api/tests/mutation_cascade_test.cpp
    src/sv/tests/sv_reader_test.cpp
)
target_link_libraries(backend_tests PRIVATE database geometry view_style editing pipelines io api sv fmt::fmt GTest::gtest_main)
target_compile_definitions(backend_tests PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    IO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/io/tests/fixtures"
    API_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures"
    SV_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/sv/tests/fixtures"
    LEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
    DEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/TEST"
    DEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/bin/defdiff"
)
add_dependencies(backend_tests lef_lib def_lib)
le_enable_coverage_instrumentation(backend_tests)
gtest_discover_tests(backend_tests)

# --- pipelines_tests: the new (PIPELINE_REFACTOR.md) pipelines module's own
# test binary, deliberately separate from backend_tests - developed and
# benchmarked standalone against database/view_style alone, integration
# with api/backend_tests deferred until the module is far enough along to
# replace src/pipelines.old's own equivalents there. ---
add_executable(pipelines_tests
    src/pipelines/tests/layer_generation_stage_test.cpp
    src/pipelines/tests/hierarchy_resolver_stage_test.cpp
    src/pipelines/tests/via_shapes_test.cpp
    src/pipelines/tests/view_render_pipeline_test.cpp
    src/pipelines/tests/viewport_cull_stage_test.cpp
    src/pipelines/tests/rasterize_blend2d_stage_test.cpp
    src/pipelines/tests/compose_stage_test.cpp
)
target_link_libraries(pipelines_tests PRIVATE database geometry view_style pipelines blend2d::blend2d GTest::gtest_main)
gtest_discover_tests(pipelines_tests)

# --- benchmarks ---
# --- pipeline_benchmarks: PIPELINE_REFACTOR.md's own per-stage benchmarks,
# standalone against database/view_style/io - see aes_scaling_fixture.hpp
# for the shared 1x1/2x1/2x2/3x2/3x3 tiling fixture every stage benchmark
# uses (PIPELINE_REFACTOR.md's own "Benchmarking" section). Deliberately no
# `scene`/`api` dependency yet - those return once a stage/benchmark
# actually needs them. ---
add_executable(pipeline_benchmarks
    src/pipelines/benchmarks/benchmark_main.cpp
    src/pipelines/benchmarks/layer_generation_benchmark.cpp
    src/pipelines/benchmarks/hierarchy_resolver_benchmark.cpp
    src/pipelines/benchmarks/viewport_cull_benchmark.cpp
    src/pipelines/benchmarks/rasterize_blend2d_benchmark.cpp
    src/pipelines/benchmarks/compose_benchmark.cpp
    src/pipelines/benchmarks/warm_tier_benchmark.cpp
)
target_link_libraries(pipeline_benchmarks PRIVATE database geometry view_style pipelines io benchmark::benchmark)
target_compile_definitions(pipeline_benchmarks PRIVATE
    REAL_DESIGN_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test_data"
)
# Not instrumented for its own coverage (benchmarks aren't run under the
# `coverage` target) - but it links `io`, which IS instrumented when
# ENABLE_COVERAGE is on, and that instrumentation pulls in a profiling
# runtime symbol that only gets linked into the final executable if this
# target also passes -fprofile-instr-generate, or the link fails.
le_enable_coverage_instrumentation(pipeline_benchmarks)

# render_preview (rasterizes a LEF's Designs to PNGs via the real render
# pipeline) is out for now - it depends on the pre-restart FrameRenderPipeline/
# HierarchyResolver (src/pipelines.old/), which PIPELINE_REFACTOR.md's Warm/Hot
# tiers haven't rebuilt yet. Restore once those exist.

# --- generate_tiled_design: dev-only data-generation tool
# (BUGS_AND_ENHANCEMENTS.md E31) - tiles a real LEF+DEF design's own
# Layout content across a grid, producing one large flat DEF file for
# render-performance stress testing/Tracy profiling. Not a benchmark or a
# test - not run by ctest or the coverage target. Only needs database/io
# (no geometry/scene/pipelines) - it never renders anything itself.
add_executable(generate_tiled_design src/pipelines/benchmarks/generate_tiled_design.cpp)
target_link_libraries(generate_tiled_design PRIVATE database io)
le_enable_coverage_instrumentation(generate_tiled_design)

# profile_tiled_render (headless Tracy-profiling driver for
# HierarchyResolver::render_layout_frame) is out for the same reason
# render_preview is, above - restore once the Warm/Hot tiers exist.

# --- spatial_index_benchmark: dev-only benchmark (BUGS_AND_ENHANCEMENTS.md
# E31 zoom/pan-tick follow-up) - quantifies a Boost.Geometry Index rtree's
# query cost against HierarchyResolver::discover_layout_children's own
# current linear per-placement scan, at several simulated zoom levels.
# Not run by ctest or the coverage target.
add_executable(spatial_index_benchmark src/pipelines/benchmarks/spatial_index_benchmark.cpp)
target_link_libraries(spatial_index_benchmark PRIVATE database geometry io)
le_enable_coverage_instrumentation(spatial_index_benchmark)

# --- blend2d_smoke_test: throwaway build/link verification for the
# Blend2D FetchContent chain above - see that block's own comment and
# blend2d_smoke_test.cpp's own header. Not run by ctest.
add_executable(blend2d_smoke_test src/pipelines/benchmarks/blend2d_smoke_test.cpp)
target_link_libraries(blend2d_smoke_test PRIVATE blend2d::blend2d)

# --- lef_roundtrip_diff: dev-only progress-tracking tool for UPDATES.md
# item 12 (LEF syntax completion) - reads a LEF file (default:
# complete.5.8.lef), writes every Design + the Technology's layers back out
# via LEFWriter, and reports what the vendored lefdiff tool's own
# normalized dump still shows as missing after the round trip. Not a
# benchmark or a test - not run by ctest or the coverage target, not a hard
# gate (see src/io/tests/lef_writer_test.cpp's own lefdiff-based CTest for
# the actual gated check, scoped to what this phase supports). ---
add_executable(lef_roundtrip_diff src/pipelines/benchmarks/lef_roundtrip_diff.cpp)
add_dependencies(lef_roundtrip_diff lef_lib)
target_link_libraries(lef_roundtrip_diff PRIVATE database geometry io)
target_compile_definitions(lef_roundtrip_diff PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    LEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
)
le_enable_coverage_instrumentation(lef_roundtrip_diff)

# --- coverage: `cmake --build build --target coverage` (requires -DENABLE_COVERAGE=ON at configure time) ---
if(ENABLE_COVERAGE)
    set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
    # database/generated is cmg output (mostly boilerplate getters/to_string);
    # excluded for now rather than chasing coverage on generated code.
    set(COVERAGE_IGNORE_REGEX "${CMAKE_BINARY_DIR}/_deps|${CMAKE_SOURCE_DIR}/src/lefdef|${CMAKE_SOURCE_DIR}/src/database/generated")

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${COVERAGE_DIR}/backend_tests.profraw $<TARGET_FILE:backend_tests>
        COMMAND ${LLVM_PROFDATA_EXE} merge -sparse ${COVERAGE_DIR}/backend_tests.profraw -o ${COVERAGE_DIR}/backend_tests.profdata
        COMMAND sh -c "${LLVM_COV_EXE} report $<TARGET_FILE:backend_tests> -instr-profile=${COVERAGE_DIR}/backend_tests.profdata --show-branch-summary --ignore-filename-regex='${COVERAGE_IGNORE_REGEX}' | tee ${COVERAGE_DIR}/report.txt"
        COMMAND sh -c "${LLVM_COV_EXE} export $<TARGET_FILE:backend_tests> -instr-profile=${COVERAGE_DIR}/backend_tests.profdata -format=lcov --ignore-filename-regex='${COVERAGE_IGNORE_REGEX}' > ${COVERAGE_DIR}/lcov.info"
        DEPENDS backend_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running backend_tests with coverage instrumentation, then reporting line+branch coverage"
        USES_TERMINAL
        VERBATIM
    )
endif()
