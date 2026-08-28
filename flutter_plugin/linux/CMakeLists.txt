# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "layout_engine_plugin")
project(${PROJECT_NAME} LANGUAGES CXX)

# layout_engine_plugin_plugin below is SHARED (inherently PIC), but the
# FetchContent-built fmt/spdlog fallback further down (LE_LINK_BACKEND
# block) builds STATIC libraries by default - linking a non-PIC static
# archive into a shared object fails on Linux (GNU ld enforces this;
# confirmed via a real "relocation R_X86_64_32S against '.data' can not be
# used when making a shared object; recompile with -fPIC" failure on
# libspdlog.a without this). Must be set before those
# FetchContent_MakeAvailable calls below, which is why it's here at the
# top, not next to them.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Invoke the build for native code shared with the other target platforms.
# This can be changed to accommodate different builds.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")

# --- layout_engine_plugin_plugin: the method-channel/texture side (see
# pubspec.yaml's `pluginClass: LayoutEnginePlugin`), separate from the
# `layout_engine_plugin` FFI shared library above - registers the
# "layout_engine_plugin" method channel and an FlPixelBufferTexture (see
# layout_engine_plugin.cc/lef_texture.cc), mirroring
# ../macos/Classes/LayoutEnginePlugin.swift/LeTexture.swift.
#
# UNVERIFIED: written against the real vendored Flutter Linux embedder
# headers (this repo has no Linux toolchain/GTK - never built here). This
# target must still link successfully with LE_LINK_BACKEND OFF (the
# default, see ../src/CMakeLists.txt's own comment on why) - Flutter's
# generated_plugins.cmake unconditionally does
# `target_link_libraries(${BINARY_NAME} PRIVATE layout_engine_plugin_plugin)`
# once pluginClass is declared, so unlike the FFI shared library (where
# missing backend symbols are only a problem once Dart actually looks them
# up via dlsym), this target has no equivalent "fine to be incomplete"
# state - lef_texture.cc's copy_pixels therefore has a real implementation
# gated behind LE_LINK_BACKEND_ENABLED and a stub (always fails cleanly)
# when it's off, rather than the whole target being conditional.
set(PLUGIN_NAME "${PROJECT_NAME}_plugin")
add_library(${PLUGIN_NAME} SHARED
  "layout_engine_plugin.cc"
  "lef_texture.cc"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# A rootless Rocky 8 build (rocky8-env.sh) compiles the backend/Skia (below,
# LE_LINK_BACKEND block) with a real gcc-toolset-13 g++, for its newer
# libstdc++ (backend's C++23 std::expected usage needs a libstdc++ new
# enough to have <expected> at all - RHEL8's base gcc-toolset-11 doesn't).
# But flutter's own Linux build tooling links this target with a *different*
# compiler on such a machine - a locally-built engine's own clang++ (an
# engine built from source has to be built with a specific clang for the
# engine's own ABI reasons, and flutter_tools matches the app build to it) -
# and that clang++, with no explicit hint, auto-detects whichever GCC
# installation looks like the system default (gcc-toolset-11 here, not the
# rootlessly-extracted gcc-toolset-13), so it resolves against gcc-toolset-11's
# *older* libstdc++. Since this target whole-archives backend/Skia static
# libraries built against gcc-toolset-13's newer one (LE_LINK_BACKEND block
# below), that mismatch is a real, confirmed link failure - "undefined
# reference to std::__cxx11::basic_string<...>::_M_replace_cold(...)" - not
# hypothetical. --gcc-toolchain tells clang which GCC installation's
# headers/libs to actually use, overriding its own auto-detection; only
# applies when this target is genuinely being linked by Clang (Docker CI/
# macOS never hit this, and LE_TOOLCHAIN_ROOT is unset there, so this is a
# no-op off the rootless Rocky 8 path). Mirrored in
# ../../frontend/linux/runner/CMakeLists.txt for the same reason - the
# final runner executable link hits the identical mismatch.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND DEFINED ENV{LE_TOOLCHAIN_ROOT})
  set(LE_GCC13_TOOLCHAIN "$ENV{LE_TOOLCHAIN_ROOT}/root/opt/rh/gcc-toolset-13/root/usr")
  if(EXISTS "${LE_GCC13_TOOLCHAIN}")
    target_link_options(${PLUGIN_NAME} PRIVATE "--gcc-toolchain=${LE_GCC13_TOOLCHAIN}")
  endif()
endif()

if(LE_LINK_BACKEND)
  # LE_LINK_BACKEND/LE_BACKEND_DIR/LE_BACKEND_BUILD/LE_SKIA_DIR are all
  # defined in ../src/CMakeLists.txt, already processed by the
  # add_subdirectory(../src) call above - CMake cache variables are
  # global, not scoped to the directory that defined them. The
  # spdlog::spdlog/fmt::fmt IMPORTED targets those find_package() calls
  # create are NOT global the same way, though - an imported target only
  # propagates to the directory that called find_package() and *its own*
  # subdirectories, never back up to a parent that add_subdirectory()'d
  # it in (confirmed by trying it without this - "layout_engine_plugin_plugin
  # links to spdlog::spdlog but the target was not found", even though
  # ../src's own find_package(spdlog REQUIRED) already ran by this point).
  # This directory needs its own find_package() calls. Same
  # find_package-then-FetchContent fallback as ../src/CMakeLists.txt/
  # backend/CMakeLists.txt (see their own comments) - a rootless Rocky 8
  # build has no system spdlog/fmt, and this directory's own scope can't
  # see ../src's FetchContent-built copy either, for the same reason it
  # can't see its find_package()-found one.
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
  target_compile_definitions(${PLUGIN_NAME} PRIVATE LE_LINK_BACKEND_ENABLED)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    "-Wl,--whole-archive" "${LE_BACKEND_BUILD}/libapi.a" "-Wl,--no-whole-archive"
    "${LE_BACKEND_BUILD}/librender.a"
    "${LE_BACKEND_BUILD}/libio.a"
    "${LE_BACKEND_DIR}/src/lefdef/lef/lib/liblef.a"
    "${LE_SKIA_DIR}/out/Linux/libskia.a"
    ${LE_JPEG_LIBRARIES} ${LE_PNG_LIBRARIES}
    spdlog::spdlog fmt::fmt
    z
  )
  # Mirrors ../src/CMakeLists.txt's own LE_SKIA_VENDORS_THIRD_PARTY gate
  # (see that file's comment) - both targets link the same SKIA_DIR
  # checkout, so both must agree on whether it bundles harfbuzz/icu/webp
  # internally or expects them linked separately.
  if(NOT LE_SKIA_VENDORS_THIRD_PARTY)
    target_link_libraries(${PLUGIN_NAME} PRIVATE
      ${LE_HARFBUZZ_LIBRARIES} ${LE_ICU_LIBRARIES} ${LE_WEBP_LIBRARIES}
    )
  endif()

  # ../src/le_tcl_bridge.{hpp,cpp} - the plain-C++ embedded-Tcl-console
  # core shared verbatim with macOS's own LeTclBridge.mm (a thin
  # Objective-C++ wrapper around the same class) - see that file's own
  # comment and TCL_EXPLORATION.md's show_gui section. Only needed here
  # (not by the FFI shared library in ../src/CMakeLists.txt) since it's
  # this target's own method-channel handler
  # (createTclConsole/evalTclCommand/disposeTclConsole below) that
  # constructs le::TclBridge instances, not the FFI side. le_tcl.so/
  # le_tcl_procs.tcl come from backend's own *Debug* tree (LE_BACKEND_TCL_BUILD,
  # matching build-linux's own naming - see Dockerfile.linux-ci), not
  # LE_BACKEND_BUILD (Release) like everything else above - unlike
  # api/render/io (hot render-path code), le_tcl only interprets a
  # handful of typed commands per keystroke, so Debug-build overhead here
  # is irrelevant, and this avoids needing le_tcl added to the Release
  # tree at all (mirrors ../macos/layout_engine_plugin.podspec's own choice).
  # On a rootless Rocky 8 build, tcl.h lives under the rootlessly-extracted
  # toolchain root ($LE_TOOLCHAIN_ROOT/root/usr/include, once
  # rocky8-bootstrap.sh has actually extracted it there - see that script's
  # own comment on extract_rpm_closure_if_missing for a real case where a
  # stale RPM-database record silently skipped this) - the real system
  # /usr/include has no tcl.h at all (only a base "tcl" runtime package is
  # actually installed there, no -devel headers). find_package(TCL)'s own
  # TCL_INCLUDE_PATH search only ever reaches the rootless one via a
  # "${TCL_TCLSH_PATH_PARENT}/include" hint derived from wherever `tclsh`
  # happens to resolve on PATH (see FindTCL.cmake's own
  # TCLTK_POSSIBLE_INCLUDE_PATHS) - fragile and PATH-order-dependent,
  # confirmed by this exact search flipping from finding tcl.h to not
  # finding it, with no change to this file, just PATH order changing
  # elsewhere (an unrelated compiler-PATH fix) resolving the real system's
  # /bin/tclsh instead of the rootless toolchain's own tclsh. TCL_LIBRARY
  # isn't fragile the same way - the base "tcl" runtime package's .so is
  # already on the real system's default library search path, no hint
  # needed. Set TCL_INCLUDE_PATH explicitly instead of hoping PATH order
  # cooperates - mirrors backend/CMakeLists.txt's own TCL_TK_ROOT override
  # for macOS's Homebrew tcl-tk@8 (same fragile-default-search problem,
  # different platform).
  if(DEFINED ENV{LE_TOOLCHAIN_ROOT})
    set(LE_ROCKY8_TCL_INCLUDE "$ENV{LE_TOOLCHAIN_ROOT}/root/usr/include")
    if(EXISTS "${LE_ROCKY8_TCL_INCLUDE}/tcl.h")
      set(TCL_INCLUDE_PATH "${LE_ROCKY8_TCL_INCLUDE}" CACHE PATH "" FORCE)
    endif()
  endif()
  # Deliberately find_package(TCL) without REQUIRED, then check
  # TCL_LIBRARY/TCL_INCLUDE_PATH ourselves - CMake's bundled FindTCL.cmake
  # always resolves Tcl *and* Tk together (it runs a second, combined
  # TCLTK find_package_handle_standard_args() check requiring
  # TK_LIBRARY/TK_INCLUDE_PATH too, and inherits REQUIRED onto that check
  # from this one - see the module itself). This project only uses plain
  # Tcl (le_tcl_bridge.cpp #includes tcl.h, no Tk widgets), so a machine
  # with Tcl but no Tk headers installed shouldn't hard-fail configure over
  # a dependency this target never links.
  find_package(TCL)
  if(NOT TCL_LIBRARY OR NOT TCL_INCLUDE_PATH)
    message(FATAL_ERROR "Tcl not found (need TCL_LIBRARY and TCL_INCLUDE_PATH) - required by ${PLUGIN_NAME}'s embedded Tcl console (le_tcl_bridge.cpp)")
  endif()
  set(LE_BACKEND_TCL_BUILD "${LE_BACKEND_DIR}/build-linux" CACHE PATH
      "Path to the backend's own Debug CMake build directory, providing le_tcl.so")
  target_sources(${PLUGIN_NAME} PRIVATE "../src/le_tcl_bridge.cpp")
  target_include_directories(${PLUGIN_NAME} PRIVATE ../src ${TCL_INCLUDE_PATH})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${TCL_LIBRARY})

  # le_tcl.so/le_tcl_procs.tcl/le_tcl_procs_generated.tcl need to be
  # bundled into the app's own lib/ directory too, not referenced via a
  # compile-time absolute path baked into the binary the way
  # LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH used to be - a bundle built on
  # one machine and copied to another has no LE_BACKEND_DIR at all, so
  # that path was never anything but this build machine's own local
  # backend/build-linux tree. layout_engine_plugin.cc now resolves the first
  # two at runtime relative to the executable's own directory instead
  # (see its own comment) - confirmed necessary by a real "cannot open
  # shared object file" error running a release build on a different
  # machine than it was built on. le_tcl_procs_generated.tcl doesn't need
  # a matching C++-side change: le_tcl_procs.tcl's own `source` line
  # already tries this flat (no generated/ subdirectory) layout as a
  # fallback after the real source-tree one - see that file's own
  # comment, added for the same reason ("couldn't read file
  # .../generated/le_tcl_procs_generated.tcl" on a release build).
  #
  # The bundled font (backend/assets/fonts/*.ttf, see render.cpp's own
  # default_typeface()) rides along here too - LE_FONT_DIR is exactly the
  # same class of compile-time-build-machine-path problem (confirmed by a
  # real report of every text label rendering blank on a release build),
  # and render.cpp's own runtime fallback (see its comment) looks in this
  # same bundled lib/ directory.
  #
  # Collected here into LE_RUNTIME_BUNDLED_FILES, not appended directly
  # to layout_engine_plugin_bundled_libraries: that variable already gets
  # its one and only `set()` call further down this file (Flutter's own
  # generated template, bundling the separate FFI shared library) -
  # a second, earlier `set(layout_engine_plugin_bundled_libraries ...
  # PARENT_SCOPE)` here would silently overwrite it rather than add to
  # it (confirmed by hitting exactly that - le_tcl.so never made it into
  # a real built bundle until this was combined into the one call below
  # instead). LE_RUNTIME_BUNDLED_FILES stays unset (harmless, expands to
  # nothing) when this if(LE_LINK_BACKEND) block doesn't run at all.
  file(GLOB LE_BUNDLED_FONT_FILES "${LE_BACKEND_DIR}/assets/fonts/*.ttf")
  set(LE_RUNTIME_BUNDLED_FILES
    "${LE_BACKEND_TCL_BUILD}/le_tcl.so"
    "${LE_BACKEND_DIR}/src/tcl/le_tcl_procs.tcl"
    "${LE_BACKEND_DIR}/src/tcl/generated/le_tcl_procs_generated.tcl"
    ${LE_BUNDLED_FONT_FILES}
  )
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
#
# layout_engine_plugin_plugin itself (the target above) is bundled
# automatically by Flutter's own generated_plugins.cmake, which expects
# exactly this variable name for anything *else* that needs bundling - the
# separate FFI shared library, in this plugin's case.
set(layout_engine_plugin_bundled_libraries
  # Defined in ../src/CMakeLists.txt.
  # This can be changed to accommodate different builds.
  $<TARGET_FILE:layout_engine_plugin>
  # le_tcl.so/le_tcl_procs*.tcl/fonts, when LE_LINK_BACKEND is ON - see
  # the LE_RUNTIME_BUNDLED_FILES comment above for why these are added
  # here rather than in a separate set() call of their own.
  ${LE_RUNTIME_BUNDLED_FILES}
  PARENT_SCOPE
)
