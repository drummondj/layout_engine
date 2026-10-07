# Reads every le_extension.toml in LE_EXTENSION_DIRS: codegen.extension_manifest
# checks each (identity, compatibility with this build, dependencies), orders
# them, and writes LE_EXTENSIONS plus LE_EXTENSION_<name>_* variables.
# Included before codegen runs, since extension schemas are merged into the
# generated database; cmake/le_extensions.cmake builds the rest later.

# A relative directory is relative to this source tree's top directory, not
# to wherever cmake happens to run.
set(le_extension_dirs "")
foreach(dir IN LISTS LE_EXTENSION_DIRS)
    get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND le_extension_dirs "${dir}")
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/codegen
        ${Python3_EXECUTABLE} -m codegen.extension_manifest
        --layout-engine-version ${PROJECT_VERSION}
        --extension-api ${LE_EXTENSION_API_VERSION}
        --output ${PROJECT_BINARY_DIR}/generated/extensions/manifests.cmake
        ${le_extension_dirs}
    RESULT_VARIABLE manifest_result
    ERROR_VARIABLE manifest_errors)
if(NOT manifest_result EQUAL 0)
    message(FATAL_ERROR "LE_EXTENSION_DIRS: ${manifest_errors}")
endif()
include(${PROJECT_BINARY_DIR}/generated/extensions/manifests.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/codegen/codegen/extension_manifest.py)

