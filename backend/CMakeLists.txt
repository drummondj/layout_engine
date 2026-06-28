cmake_minimum_required(VERSION 3.15)
project(layout_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X version" FORCE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Include the ExternalProject module to manage third-party builds
include(ExternalProject)

# --- Build the external LEF library ---
# This command tells CMake to go into the lefdef/lef directory, run 'make',
# and produce a static library (liblef.a).
ExternalProject_Add(
    lef_lib
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/lef
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 1
    # Define the output artifact
    BYPRODUCTS
    "${CMAKE_CURRENT_SOURCE_DIR}/lefdef/lef/lib/liblef.a"
)

# --- Build the external DEF library ---
ExternalProject_Add(
    def_lib
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/def
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 1
    # Define the output artifact
    BYPRODUCTS
    "${CMAKE_CURRENT_SOURCE_DIR}/lefdef/def/lib/libdef.a"
)

# --- Find and configure dependencies ---

set(SKIA_DIR "/Users/john/Projects/synthosilicon/skia/skia")
set(SKIA_OUT_DIR "${SKIA_DIR}/out/MacStatic")

find_package(flatbuffers CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(DATABASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/database")
set(DATABASE_HEADER "${DATABASE_DIR}/database_generated.h")
set(DATABASE_SCHEMA "${DATABASE_DIR}/database.fbs")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/database/database_generated.h"
    COMMAND flatbuffers::flatc --cpp --gen-object-api -o "${DATABASE_DIR}" "${DATABASE_SCHEMA}"
    DEPENDS "${DATABASE_SCHEMA}"
    COMMENT "Compiling FlatBuffers schema: ${DATABASE_SCHEMA}"
)

include_directories(
    ${SKIA_DIR}
    /opt/homebrew/include
    /opt/homebrew/include/harfbuzz
    /opt/homebrew/opt/icu4c/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/lef/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/def/include
)

file(GLOB CPP_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(layout_engine SHARED 
    ${CPP_SOURCES}
    ${DATABASE_HEADER}
)

add_dependencies(layout_engine lef_lib def_lib)

target_link_libraries(layout_engine
    PRIVATE
    "${SKIA_OUT_DIR}/libskia.a"
    # Homebrew libraries we used during your build
    /opt/homebrew/lib/libpng.dylib
    /opt/homebrew/lib/libjpeg.dylib
    /opt/homebrew/lib/libwebp.dylib
    /opt/homebrew/lib/libwebpdemux.dylib 
    /opt/homebrew/lib/libharfbuzz.dylib
    /opt/homebrew/opt/libpng/lib/libpng16.16.dylib
    /opt/homebrew/opt/icu4c/lib/libicuuc.dylib
    # Core macOS Frameworks required by Skia
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreVideo" # Added for CVPixelBufferRef support
    flatbuffers::flatbuffers
    ZLIB::ZLIB
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/lef/lib/liblef.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/def/lib/libdef.a
)


if(APPLE)
    set_target_properties(layout_engine PROPERTIES
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
    find_library(CORE_SERVICES_FRAMEWORK CoreServices)
    
    target_link_libraries(layout_engine PRIVATE ${CORE_SERVICES_FRAMEWORK})
endif()

# include(CTest)
# enable_testing()


# Link your dylib against the static libraries produced by the external projects
target_link_libraries(layout_engine PRIVATE
    # The full paths to the compiled static libraries
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/lef/lib/liblef.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lefdef/def/lib/libdef.a
)
