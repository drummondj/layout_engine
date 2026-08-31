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
# spdlog/fmt fall back to FetchContent (same pattern as googletest/
# googlebenchmark below) when no system install is found - neither ships in
# RHEL8 BaseOS/AppStream/CRB by default, needed for a rootless Rocky 8
# build with no system package installs available at all (see
# backend/scripts/rocky8-bootstrap.sh). macOS/Ubuntu (where find_package
# already succeeds via Homebrew/apt) are unaffected - this only activates
# when the system copy is missing, on any platform.
find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    FetchContent_Declare(fmt URL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.zip)
    FetchContent_MakeAvailable(fmt)
endif()

find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    # Without this, spdlog vendors its own internal fmt copy, conflicting
    # with the many targets in this file that link fmt::fmt directly.
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip)
    FetchContent_MakeAvailable(spdlog)
endif()

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

# --- oneTBB (backend/ONETBB_INTEGRATION.md migration - src/pipelines
# replaced pipeline/render/instancing at the Phase 5 cutover, and `api`
# now depends on it unconditionally - see api.cpp's own LeHandle comment).
# TBB_DIR is a Homebrew-macOS-specific hint, matching the sibling
# oneTBB_test project's own CMakeLists.txt. REQUIRED (not QUIET/optional
# as it was pre-cutover): api - and therefore le_tcl, the Flutter plugin,
# backend_tests - can no longer build without it. Linux/Rocky8 TBB
# acquisition (the rootless-build target, see the Rocky 8 bullet in
# CLAUDE.md's Open gaps) remains a real, tracked, NOT-yet-solved
# prerequisite for a Linux configure of this project from this point
# forward - a deliberate, accepted consequence of the cutover (see the
# migration plan's decision 3), not an oversight. ---
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
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy
    URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.zip
)
FetchContent_MakeAvailable(tracy)

enable_testing()
include(GoogleTest)

# --- database: header-only, generated from src/database/schema.py via cmg ---
add_library(database INTERFACE)
target_include_directories(database INTERFACE src/database src/database/generated)

# --- geometry: header-only Boost.Geometry wrapper over database types ---
add_library(geometry INTERFACE)
target_include_directories(geometry INTERFACE src)
target_link_libraries(geometry INTERFACE database Boost::boost)

# --- view_style: header-only ViewLayer/purpose/color config, distinct from
# the LEF-mirroring database - see CLAUDE.md for why these aren't schema/cmg-generated ---
add_library(view_style INTERFACE)
target_include_directories(view_style INTERFACE src)
target_link_libraries(view_style INTERFACE database)

# --- scene: header-only per-handle mutable view state (pan/zoom, selection, layer visibility) ---
add_library(scene INTERFACE)
target_include_directories(scene INTERFACE src)
target_link_libraries(scene INTERFACE database view_style)

# --- editing: header-only undo/redo command recording (UPDATES.md item 21) ---
add_library(editing INTERFACE)
target_include_directories(editing INTERFACE src)
target_link_libraries(editing INTERFACE database scene)

# --- core: header-only generic building blocks (RenderedShape/TinyShapeDot,
# VersionedStage) shared by `pipelines` and by HierarchyResolver's own
# recursive core, which stays on VersionedStage rather than MemoizingStage
# (backend/ONETBB_INTEGRATION.md migration plan, decision 1). See
# UPDATES.md item 16. ---
add_library(core INTERFACE)
target_include_directories(core INTERFACE src)
target_link_libraries(core INTERFACE database scene view_style)

# --- Skia (not vendored/built by this project - a pre-built checkout on
# this machine). SKIA_DIR must contain out/MacStatic/{libskia.a,...} on
# macOS or out/Linux/{libskia.a,...} on Linux (see docker-compose.yml /
# Dockerfile.linux-ci for how the Linux checkout is built - raster-only,
# no GPU backend, matching the macOS build), each built with
# is_component_build=false (static). Link set (include path, libskia.a,
# system harfbuzz/icu, plus platform-specific font-manager libs) was
# determined empirically by linking a minimal SkPictureRecorder/SkSurface
# program against the macOS checkout - see BENCHMARKS.md/CLAUDE.md for why
# no GPU (Ganesh/Metal) frameworks are needed: we only use the raster (CPU)
# surface APIs, and static linking never pulls in the unused GPU object
# files. Machine-specific - not portable/committed; override -DSKIA_DIR=...
# to point at a different checkout.
if(APPLE)
    set(SKIA_DIR "/Volumes/Docking/Projects/synthosilicon/skia/skia" CACHE PATH
        "Path to a built Skia checkout (must contain out/MacStatic/libskia.a)")
    set(SKIA_OUT_DIR "${SKIA_DIR}/out/MacStatic")
else()
    set(SKIA_DIR "/skia/skia" CACHE PATH
        "Path to a built Skia checkout (must contain out/Linux/libskia.a)")
    set(SKIA_OUT_DIR "${SKIA_DIR}/out/Linux")
endif()
# A Rocky-8-rootless-build Skia checkout is configured with
# skia_use_system_harfbuzz=false/skia_use_system_icu=false/
# skia_use_system_libwebp=false/skia_use_system_expat=false (see
# backend/scripts/rocky8-bootstrap.sh) - RHEL8's own harfbuzz (~1.7.5) and
# icu (~60) are old enough relative to a modern Skia commit that linking
# against them as system libs (what the existing Docker/Ubuntu Linux CI
# recipe still does, Dockerfile.linux-ci's skia_use_system_harfbuzz=true
# etc.) is a real version-skew risk there, and libwebp-devel's presence on
# a locked-down RHEL8 box at all is uncertain - so that build instead lets
# libskia.a bundle its own copies of all four internally, statically, and
# this target must not *also* link separate copies of them in that case.
# Default OFF: unconditionally linking these has been this project's own
# working Docker/Ubuntu Linux CI setup all along - don't change that
# path's behavior, only add a new opt-in for the rootless one.
option(LE_SKIA_VENDORS_THIRD_PARTY
    "Skia was built with skia_use_system_harfbuzz/icu/libwebp/expat=false (bundles its own copies into libskia.a) - don't link system harfbuzz/icu/webp separately"
    OFF)

add_library(skia INTERFACE)
target_include_directories(skia INTERFACE ${SKIA_DIR})
target_link_libraries(skia INTERFACE
    ${SKIA_OUT_DIR}/libskia.a
    jpeg
    png
    z
)
if(NOT LE_SKIA_VENDORS_THIRD_PARTY)
    target_link_libraries(skia INTERFACE harfbuzz icuuc icudata webp webpdemux)
endif()
if(APPLE)
    target_link_directories(skia INTERFACE /opt/homebrew/lib /opt/homebrew/opt/icu4c/lib)
    target_link_libraries(skia INTERFACE
        "-framework CoreText"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreServices"
    )
else()
    # Linux Skia is built with skia_use_freetype=true (see
    # Dockerfile.linux-ci / rocky8-bootstrap.sh) - not statically bundled
    # into libskia.a the way the platform frameworks are on macOS, so the
    # final consumer (render, below) needs to link it explicitly.
    # fontconfig is deliberately NOT linked here (both Dockerfile.linux-ci
    # and rocky8-bootstrap.sh now build Skia with skia_use_fontconfig=false
    # too) - default_typeface() (render.cpp) uses a bundled font directory
    # instead, not system fontconfig, so the app doesn't depend on the
    # target machine having any fonts installed at all.
    target_link_libraries(skia INTERFACE freetype)
endif()

# --- pipelines: dbu->pixel transform + Skia SkPicture generation, built on
# oneTBB's flow::graph (backend/ONETBB_INTEGRATION.md migration, Phase 5c
# cutover) - the direct replacement for the former pipeline/render/
# instancing modules (deleted; see git history for their own doc comments,
# preserved in this module's own stage-class comments as "oneTBB port of
# src/render/stages/..." provenance notes). Compiled (not header-only) so
# pipelines.cpp alone pulls in the CoreText font manager's
# ApplicationServices.h - see default_typeface()'s own comment (in
# draw_helpers.hpp) for why that can't be a header-only detail. ---
add_library(pipelines STATIC src/pipelines/pipelines.cpp)
target_include_directories(pipelines PUBLIC src)
target_link_libraries(pipelines PUBLIC database geometry scene view_style core skia fmt::fmt spdlog::spdlog TBB::tbb TracyClient)

# LE_FONT_DIR: Linux's default_typeface() (pipelines.cpp) loads its default
# font from this bundled directory (backend/assets/fonts, committed to the
# repo - DejaVu Sans, Bitstream-Vera licensed) instead of system fontconfig,
# so rendering doesn't depend on the target machine having any fonts
# installed at all - see pipelines.cpp's own comment. Not used on macOS
# (CoreText path). Override for a different bundled font set.
if(NOT APPLE)
    set(LE_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" CACHE PATH
        "Directory of .ttf/.ttc/.otf/.pfb files default_typeface() loads from on Linux")
    target_compile_definitions(pipelines PRIVATE LE_FONT_DIR="${LE_FONT_DIR}")
endif()

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

# --- api: the C API surface a Flutter plugin's Dart FFI binds to (see
# README's `api` module entry). Compiled (not header-only): the opaque
# LeHandle's real definition must stay out of api.hpp, which needs to be a
# plain-C-parseable header for ffigen/Dart FFI - see api.hpp's own comment. ---
add_library(api STATIC src/api/api.cpp)
target_include_directories(api PUBLIC src/api)
target_link_libraries(api PRIVATE database geometry scene editing view_style pipelines io)
le_enable_coverage_instrumentation(api)

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
    # `skia` alongside `api` - api links pipelines (which owns Skia's own
    # include path) PRIVATE, so it doesn't propagate this far; le_tcl_shim.cpp
    # needs it directly now too (dump_png_cmd's own SkPngEncoder use, same
    # reasoning pipelines.cpp's own direct Skia use has - skia is an
    # INTERFACE library, just the include path, no separate binary to link).
    target_link_libraries(le_tcl PRIVATE api skia ${TCL_LIBRARY})
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

    # Phase 6 batch shell (src/tcl/le_shell.cpp) - a small Tcl_Main-based
    # binary, not linked against le_tcl (it dynamically `load`s the built
    # shared module at startup, same as tclsh would) - only needs Tcl
    # itself.
    add_executable(le_shell src/tcl/le_shell.cpp)
    target_include_directories(le_shell PRIVATE ${TCL_INCLUDE_PATH})
    target_link_libraries(le_shell PRIVATE ${TCL_LIBRARY})

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
    src/geometry/tests/geometry_test.cpp
    src/view_style/tests/view_style_test.cpp
    src/scene/tests/scene_test.cpp
    src/editing/tests/editing_test.cpp
    src/io/tests/lef_reader_test.cpp
    src/io/tests/lef_reader_utils_test.cpp
    src/io/tests/lef_writer_test.cpp
    src/io/tests/def_reader_test.cpp
    src/io/tests/def_writer_test.cpp
    src/api/tests/api_test.cpp
)
target_link_libraries(backend_tests PRIVATE database geometry view_style scene editing io api fmt::fmt GTest::gtest_main)
target_compile_definitions(backend_tests PRIVATE
    LEFDEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/TEST"
    IO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/io/tests/fixtures"
    API_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/api/tests/fixtures"
    LEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/lef/bin/lefdiff"
    DEF_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/TEST"
    DEFDIFF_BIN="${CMAKE_CURRENT_SOURCE_DIR}/src/lefdef/def/bin/defdiff"
)
add_dependencies(backend_tests lef_lib def_lib)
target_sources(backend_tests PRIVATE src/pipelines/tests/pipelines_test.cpp src/pipelines/tests/render_pipelines_test.cpp src/pipelines/tests/hierarchy_resolver_test.cpp src/pipelines/tests/draw_helpers_test.cpp)
target_link_libraries(backend_tests PRIVATE pipelines)
le_enable_coverage_instrumentation(backend_tests)
gtest_discover_tests(backend_tests)

# --- benchmarks ---
add_executable(pipeline_benchmarks src/pipelines/benchmarks/pipeline_benchmark.cpp)
target_link_libraries(pipeline_benchmarks PRIVATE database geometry scene pipelines io api benchmark::benchmark)
target_compile_definitions(pipeline_benchmarks PRIVATE
    BENCHMARK_DATA_DIR="${CMAKE_BINARY_DIR}/benchmark_data"
)
# Not instrumented for its own coverage (benchmarks aren't run under the
# `coverage` target) - but it links `io`, which IS instrumented when
# ENABLE_COVERAGE is on, and that instrumentation pulls in a profiling
# runtime symbol that only gets linked into the final executable if this
# target also passes -fprofile-instr-generate, or the link fails.
le_enable_coverage_instrumentation(pipeline_benchmarks)

# --- render_preview: dev-only tool, rasterizes each given LEF file's Designs
# to PNGs (preview/<lef-stem>__<design-name>.png) so they can be visually
# sanity-checked without waiting for Flutter texture wiring. Not a benchmark
# or a test - not run by ctest or the coverage target. ---
add_executable(render_preview src/pipelines/benchmarks/render_preview.cpp)
target_link_libraries(render_preview PRIVATE database geometry scene pipelines io)
le_enable_coverage_instrumentation(render_preview)

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
