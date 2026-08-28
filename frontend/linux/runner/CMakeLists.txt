cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME}
  "main.cc"
  "my_application.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the application ID.
add_definitions(-DAPPLICATION_ID="${APPLICATION_ID}")

# Add dependency libraries. Add any application-specific dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)

target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# See ../../flutter_plugin/linux/CMakeLists.txt's own comment on this same
# block (right before its LE_LINK_BACKEND section) - this executable
# transitively links the same gcc-toolset-13-built backend/Skia static
# libraries (via the plugin's own liblayout_engine_plugin_plugin.so),
# through whatever clang++ a locally-built Flutter engine forces
# flutter_tools to use on a rootless Rocky 8 build, and hits the identical
# libstdc++ symbol mismatch at this final link step specifically (this is
# the executable that actually fails with "undefined reference to ...
# _M_replace_cold").
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND DEFINED ENV{LE_TOOLCHAIN_ROOT})
  set(LE_GCC13_TOOLCHAIN "$ENV{LE_TOOLCHAIN_ROOT}/root/opt/rh/gcc-toolset-13/root/usr")
  if(EXISTS "${LE_GCC13_TOOLCHAIN}")
    target_link_options(${BINARY_NAME} PRIVATE "--gcc-toolchain=${LE_GCC13_TOOLCHAIN}")
  endif()
endif()
