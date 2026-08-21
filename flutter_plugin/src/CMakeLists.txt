# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(layout_engine_plugin_library VERSION 0.0.1 LANGUAGES C)

# layout_engine_plugin below is already SHARED (inherently PIC), but the
# FetchContent-built fmt/spdlog fallback further down (LE_LINK_BACKEND
# block) builds STATIC libraries by default - linking a non-PIC static
# archive into a shared object fails on Linux (GNU ld enforces this;
# macOS's ld64 doesn't, so this went unnoticed there) - same class of
# gotcha backend/CMakeLists.txt already works around globally for its own
# targets. Must be set before those FetchContent_MakeAvailable calls
# below, which is why it's here at the top, not next to them.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(layout_engine_plugin SHARED
  "layout_engine_plugin.c"
)

set_target_properties(layout_engine_plugin PROPERTIES
  PUBLIC_HEADER layout_engine_plugin.h
  OUTPUT_NAME "layout_engine_plugin"
)

target_compile_definitions(layout_engine_plugin PUBLIC DART_SHARED_LIB)

if (ANDROID)
  # Support Android 15 16k page size
  target_link_options(layout_engine_plugin PRIVATE "-Wl,-z,max-page-size=16384")
endif()

# --- Backend link (Linux) ---
# le_*() (see layout_engine_plugin.h) is implemented in backend/src/api/api.cpp,
# not in this plugin's own sources - link the backend's own CMake build
# output directly, mirroring what ../macos/layout_engine_plugin.podspec does
# (there's no automated sharing between a podspec and a CMakeLists.txt -
# keep the two in sync by hand; see this plugin's CLAUDE.md).
#
# Default OFF and NOT expected to succeed yet even when turned on: this is
# genuinely blocked upstream, not just an unfinished link recipe -
# backend's Renderer::default_typeface() is CoreText-backed (macOS only,
# see backend/CLAUDE.md's `render` entry - a Linux fontconfig/FreeType
# SkFontMgr is a tracked, not-yet-done gap there), and no Linux Skia
# checkout exists on this dev machine either (see backend/CLAUDE.md's
# "Open gaps"). Turn this on once both of those land, not before -
# leaving it off keeps a plain `flutter build linux` succeeding (with an
# empty shared library, no le_*() symbols) instead of failing on
# pkg-config lookups for a link that can't work yet anyway.
option(LE_LINK_BACKEND "Link this plugin against the backend's api library (see comment above - not functional until backend gets Linux Skia + a fontconfig SkFontMgr)" OFF)
if(LE_LINK_BACKEND)
  enable_language(CXX)
  target_sources(layout_engine_plugin PRIVATE "layout_engine_plugin_force_cxx_link.cpp")

  set(LE_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../backend" CACHE PATH
      "Path to the backend checkout providing libapi.a et al")
  # build_release (Release), not build (Debug) - matches
  # ../macos/layout_engine_plugin.podspec's own choice: this plugin is what a
  # real running app embeds, so it needs actual optimized performance, not
  # debug-build timings. See that podspec's own comment.
  set(LE_BACKEND_BUILD "${LE_BACKEND_DIR}/build_release" CACHE PATH
      "Path to the backend's own Release CMake build directory")
  set(LE_SKIA_DIR "$ENV{SKIA_DIR}" CACHE PATH
      "Path to a built Linux Skia checkout (must contain out/<config>/libskia.a) - unset by default since none exists yet")
  # Mirrors backend/CMakeLists.txt's own LE_SKIA_VENDORS_THIRD_PARTY option
  # (see that file's comment) - both must agree on whether the SKIA_DIR
  # checkout bundles its own harfbuzz/icu/libwebp internally or expects
  # them linked separately. Forwarded from the environment by
  # ../../frontend/linux/CMakeLists.txt when this plugin is consumed by
  # the real app; defaults OFF (unset) here too for a standalone configure.
  option(LE_SKIA_VENDORS_THIRD_PARTY
      "Skia was built with skia_use_system_harfbuzz/icu/libwebp=false (bundles its own copies into libskia.a) - don't link system harfbuzz/icu/webp separately"
      OFF)

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LE_JPEG REQUIRED libjpeg)
  pkg_check_modules(LE_PNG REQUIRED libpng)
  if(NOT LE_SKIA_VENDORS_THIRD_PARTY)
    pkg_check_modules(LE_HARFBUZZ REQUIRED harfbuzz)
    pkg_check_modules(LE_ICU REQUIRED icu-uc)
    pkg_check_modules(LE_WEBP REQUIRED libwebp libwebpdemux)
  endif()

  # Same find_package-then-FetchContent-fallback as backend/CMakeLists.txt
  # (see that file's own comment) - this is a separate CMake configure
  # from backend's own, so it can't discover whatever backend's build
  # produced spdlog/fmt from; it needs its own fallback for a rootless
  # Rocky 8 build with no system spdlog/fmt available.
  include(FetchContent)
  find_package(fmt CONFIG QUIET)
  if(NOT fmt_FOUND)
    FetchContent_Declare(fmt URL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.zip)
    FetchContent_MakeAvailable(fmt)
  endif()
  find_package(spdlog CONFIG QUIET)
  if(NOT spdlog_FOUND)
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip)
    FetchContent_MakeAvailable(spdlog)
  endif()

  # api.cpp's le_*() functions are never referenced by this plugin's own
  # compiled sources (Dart only finds them via dlsym at runtime) - without
  # --whole-archive, the linker drops libapi.a's object file entirely
  # since nothing in this link appears to need it (same reasoning as the
  # macOS podspec's -force_load).
  target_link_libraries(layout_engine_plugin PRIVATE
    "-Wl,--whole-archive" "${LE_BACKEND_BUILD}/libapi.a" "-Wl,--no-whole-archive"
    "${LE_BACKEND_BUILD}/librender.a"
    "${LE_BACKEND_BUILD}/libio.a"
    "${LE_BACKEND_DIR}/src/lefdef/lef/lib/liblef.a"
    "${LE_SKIA_DIR}/out/Linux/libskia.a"
    ${LE_JPEG_LIBRARIES} ${LE_PNG_LIBRARIES}
    spdlog::spdlog fmt::fmt
    z
  )
  if(NOT LE_SKIA_VENDORS_THIRD_PARTY)
    target_link_libraries(layout_engine_plugin PRIVATE
      ${LE_HARFBUZZ_LIBRARIES} ${LE_ICU_LIBRARIES} ${LE_WEBP_LIBRARIES}
    )
  endif()
endif()
