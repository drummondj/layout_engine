# Project-level configuration.
cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES CXX)

# The name of the executable created for the application. Change this to change
# the on-disk name of your application.
set(BINARY_NAME "layout_engine")
# The unique GTK application identifier for this application. See:
# https://wiki.gnome.org/HowDoI/ChooseApplicationID
set(APPLICATION_ID "com.synthosilicon.layout_engine")

# Explicitly opt in to modern CMake behaviors to avoid warnings with recent
# versions of CMake.
cmake_policy(SET CMP0063 NEW)

# layout_engine_plugin's own default for this is OFF (see
# ../../flutter_plugin/src/CMakeLists.txt's own comment) - it was written
# before backend had a working Linux Skia checkout or fontconfig SkFontMgr,
# so a plain `flutter build linux` could still succeed with an empty stub
# rather than failing on a link that couldn't work yet anyway. Both of
# those backend gaps are closed now (see Dockerfile.linux-ci/CI backend
# fixes), so this actual consuming app should always link the real
# backend on Linux, same as ../macos/layout_engine_plugin.podspec always does
# unconditionally. LE_BACKEND_BUILD overridden to build_release-linux -
# the plugin's own default (build_release) is the macOS-native tree name;
# see backend/Dockerfile.linux-ci for why the Linux trees are named
# differently. LE_BACKEND_DIR is *also* overridden, not left at the
# plugin's own default - that default is relative to the plugin's own
# CMAKE_CURRENT_SOURCE_DIR, but Flutter symlinks a consumed plugin in
# under flutter/ephemeral/.plugin_symlinks/layout_engine_plugin/, which has
# no `backend` sibling, so a relative `../../backend` computed from
# *there* resolves to a path that doesn't exist (confirmed: "liblef.a
# ... missing and no known rule to make it"). This file's own
# CMAKE_CURRENT_SOURCE_DIR isn't symlinked, so computing the same
# relative path from here instead works. LE_SKIA_DIR keeps the plugin's
# own default ($ENV{SKIA_DIR}, set by Dockerfile.linux-ci's backend
# stage) - unaffected by this, since it doesn't depend on
# CMAKE_CURRENT_SOURCE_DIR at all.
set(LE_LINK_BACKEND ON CACHE BOOL "" FORCE)
set(LE_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../backend" CACHE PATH "" FORCE)
set(LE_BACKEND_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../../backend/build_release-linux" CACHE PATH "" FORCE)

# Forwards backend/CMakeLists.txt's own LE_SKIA_VENDORS_THIRD_PARTY option
# (see that file's comment) into this plugin's own equivalent - both must
# agree on whether the SKIA_DIR checkout being linked bundles its own
# harfbuzz/icu/libwebp or expects them linked separately as system libs.
# Read from the environment (set by backend/scripts/rocky8-env.sh for a
# rootless Rocky 8 build), not forced ON/OFF here - defaults to OFF
# (unset), matching the Docker/Ubuntu CI Skia checkout's own system-linked
# configuration, same as this file's other env-sourced value (LE_SKIA_DIR,
# via flutter_plugin/src/CMakeLists.txt's own $ENV{SKIA_DIR} default).
set(LE_SKIA_VENDORS_THIRD_PARTY "$ENV{LE_SKIA_VENDORS_THIRD_PARTY}" CACHE BOOL "" FORCE)

# Load bundled libraries from the lib/ directory relative to the binary.
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

# Root filesystem for cross-building.
if(FLUTTER_TARGET_PLATFORM_SYSROOT)
  set(CMAKE_SYSROOT ${FLUTTER_TARGET_PLATFORM_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

# Define build configuration options.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE
    STRING "Flutter build mode" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Profile" "Release")
endif()

# Compilation settings that should be applied to most targets.
#
# Be cautious about adding new options here, as plugins use this function by
# default. In most cases, you should add new options to specific targets instead
# of modifying this function.
function(APPLY_STANDARD_SETTINGS TARGET)
  target_compile_features(${TARGET} PUBLIC cxx_std_14)
  target_compile_options(${TARGET} PRIVATE -Wall -Werror)
  target_compile_options(${TARGET} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:-O3>")
  target_compile_definitions(${TARGET} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>")
endfunction()

# Flutter library and tool build rules.
set(FLUTTER_MANAGED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/flutter")
add_subdirectory(${FLUTTER_MANAGED_DIR})

# System-level dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)

# Application build; see runner/CMakeLists.txt.
add_subdirectory("runner")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# Only the install-generated bundle's copy of the executable will launch
# correctly, since the resources must in the right relative locations. To avoid
# people trying to run the unbundled copy, put it in a subdirectory instead of
# the default top-level location.
set_target_properties(${BINARY_NAME}
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/intermediates_do_not_run"
)


# Generated plugin build rules, which manage building the plugins and adding
# them to the application.
include(flutter/generated_plugins.cmake)


# === Installation ===
# By default, "installing" just makes a relocatable bundle in the build
# directory.
set(BUILD_BUNDLE_DIR "${PROJECT_BINARY_DIR}/bundle")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${BUILD_BUNDLE_DIR}" CACHE PATH "..." FORCE)
endif()

# Start with a clean build bundle directory every time.
install(CODE "
  file(REMOVE_RECURSE \"${BUILD_BUNDLE_DIR}/\")
  " COMPONENT Runtime)

set(INSTALL_BUNDLE_DATA_DIR "${CMAKE_INSTALL_PREFIX}/data")
set(INSTALL_BUNDLE_LIB_DIR "${CMAKE_INSTALL_PREFIX}/lib")

install(TARGETS ${BINARY_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_ICU_DATA_FILE}" DESTINATION "${INSTALL_BUNDLE_DATA_DIR}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_LIBRARY}" DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
  COMPONENT Runtime)

foreach(bundled_library ${PLUGIN_BUNDLED_LIBRARIES})
  install(FILES "${bundled_library}"
    DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
    COMPONENT Runtime)
endforeach(bundled_library)

# Copy the native assets provided by the build.dart from all packages.
set(NATIVE_ASSETS_DIR "${PROJECT_BUILD_DIR}native_assets/linux/")
# This project has no native_assets_cli packages, so flutter assemble's own
# install_code_assets target (which normally creates this directory, even
# empty) gets skipped entirely - confirmed for real ("Skipping target:
# install_code_assets" in a verbose build log) - leaving install(DIRECTORY)
# below pointed at a source that doesn't exist, a hard install-time error
# ("file INSTALL cannot find ... No such file or directory"). Create it
# ourselves (a no-op if flutter assemble did make it) so there's always
# something, possibly empty, to install.
install(CODE "file(MAKE_DIRECTORY \"${NATIVE_ASSETS_DIR}\")" COMPONENT Runtime)
install(DIRECTORY "${NATIVE_ASSETS_DIR}"
   DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
   COMPONENT Runtime)

# Fully re-copy the assets directory on each build to avoid having stale files
# from a previous install.
set(FLUTTER_ASSET_DIR_NAME "flutter_assets")
install(CODE "
  file(REMOVE_RECURSE \"${INSTALL_BUNDLE_DATA_DIR}/${FLUTTER_ASSET_DIR_NAME}\")
  " COMPONENT Runtime)
install(DIRECTORY "${PROJECT_BUILD_DIR}/${FLUTTER_ASSET_DIR_NAME}"
  DESTINATION "${INSTALL_BUNDLE_DATA_DIR}" COMPONENT Runtime)

# Install the AOT library on non-Debug builds only.
if(NOT CMAKE_BUILD_TYPE MATCHES "Debug")
  install(FILES "${AOT_LIBRARY}" DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
    COMPONENT Runtime)
endif()
