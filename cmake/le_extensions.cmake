# Extensions: builds every extension directory in LE_EXTENSION_DIRS into this
# build (plans/EXTENSION_MECHANISM_RESEARCH.md §2-3, §6). Included by
# CMakeLists.txt after the `api` and `le::extension_sdk` targets exist and
# Tcl/SWIG have been looked for.
#
# Each directory's le_extension.toml has already been read, checked and
# ordered (cmake/le_extension_manifests.cmake, before codegen, which merges
# extension schemas into the database). A compiled extension's
# le_extension.cmake then calls le_add_extension() with its build details.
# The results:
# - `le_extensions`: every extension's core library plus a generated
#   register_all(), linked into le_shell and le_tcl.
# - `le_extensions_tcl` (if Tcl is available): every extension's Tcl
#   library plus a generated init_tcl(), linked into le_tcl; and
#   generated/extensions/le_api_extensions.i, the extensions' SWIG files,
#   %include'd by le_api.i.
# - `le_extensions_gui`: every extension's GUI library plus a generated
#   register_all_gui(), linked into le_shell.
# - extensions.json (build tree, written by the le_extensions_index target)
#   and the bundle's own copy, listing each extension's procs in load order
#   for le_shell.

# le_add_extension(<name>
#     CORE_SOURCES <file>...     the extension's C++, including le_ext_<name>_register
#     [CORE_INCLUDE <dir>...]    public include directories
#     [TESTS <file>...]          GoogleTest sources, built as <name>_tests
#     [TCL_SWIG <file>...]       SWIG declarations of its Tcl commands
#     [TCL_SOURCES <file>...]    their C++ (le/extension_tcl.hpp), built into le_tcl only
#     [TCL_INIT]                 TCL_SOURCES define le_ext_<name>_init_tcl(Tcl_Interp *)
#     [GUI_SOURCES <file>...]    its windows and menu items (le/extension_gui.hpp), built into
#                                le_shell only; they define le_ext_<name>_register_gui(GuiRegistry &)
#     [LINK <target>...])        the extension's own third-party dependencies
# Paths are relative to the extension directory.
function(le_add_extension name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "TCL_INIT" "" "CORE_SOURCES;CORE_INCLUDE;TESTS;TCL_SWIG;TCL_SOURCES;GUI_SOURCES;LINK")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "le_add_extension(${name}): unknown arguments ${ARG_UNPARSED_ARGUMENTS} "
            "(supported: CORE_SOURCES CORE_INCLUDE TESTS TCL_SWIG TCL_SOURCES TCL_INIT GUI_SOURCES LINK)")
    endif()
    if(NOT name STREQUAL LE_CURRENT_EXTENSION)
        message(FATAL_ERROR "le_add_extension(${name}) called from ${LE_CURRENT_EXTENSION}'s le_extension.cmake - "
            "the name must match its le_extension.toml")
    endif()
    if(NOT ARG_CORE_SOURCES)
        message(FATAL_ERROR "le_add_extension(${name}): CORE_SOURCES is required (at least le_ext_${name}_register)")
    endif()
    if((ARG_TCL_SWIG OR ARG_TCL_INIT) AND NOT ARG_TCL_SOURCES)
        message(FATAL_ERROR "le_add_extension(${name}): TCL_SWIG and TCL_INIT need TCL_SOURCES")
    endif()
    set(dir "${LE_EXTENSION_${name}_DIR}")
    foreach(list CORE_SOURCES CORE_INCLUDE TESTS TCL_SWIG TCL_SOURCES GUI_SOURCES)
        list(TRANSFORM ARG_${list} PREPEND "${dir}/")
    endforeach()

    add_library(${name}_core STATIC ${ARG_CORE_SOURCES})
    target_include_directories(${name}_core PUBLIC ${ARG_CORE_INCLUDE})
    target_link_libraries(${name}_core PUBLIC le::extension_sdk ${ARG_LINK})

    if(ARG_TESTS)
        add_executable(${name}_tests ${ARG_TESTS})
        # le_extensions (defined below) holds every core plus register_all().
        target_link_libraries(${name}_tests PRIVATE le_extensions GTest::gtest_main)
        gtest_discover_tests(${name}_tests TEST_PREFIX "${name}.")
    endif()

    if(ARG_TCL_SOURCES)
        if(NOT LE_EXTENSIONS_HAVE_TCL)
            message(STATUS "Extension ${name}: Tcl not found - its Tcl commands won't be built")
        else()
            add_library(${name}_tcl STATIC ${ARG_TCL_SOURCES})
            target_include_directories(${name}_tcl PUBLIC ${ARG_CORE_INCLUDE} ${TCL_INCLUDE_PATH})
            target_link_libraries(${name}_tcl PUBLIC ${name}_core)
            set_property(GLOBAL APPEND PROPERTY LE_EXTENSION_TCL_TARGETS ${name}_tcl)
            set_property(GLOBAL APPEND PROPERTY LE_EXTENSION_SWIG_FILES ${ARG_TCL_SWIG})
            if(ARG_TCL_INIT)
                set_property(GLOBAL APPEND PROPERTY LE_EXTENSION_TCL_INITS ${name})
            endif()
        endif()
    endif()

    if(ARG_GUI_SOURCES)
        add_library(${name}_gui STATIC ${ARG_GUI_SOURCES})
        target_include_directories(${name}_gui PUBLIC ${ARG_CORE_INCLUDE})
        target_link_libraries(${name}_gui PUBLIC ${name}_core gui)
        set_property(GLOBAL APPEND PROPERTY LE_EXTENSION_GUI_TARGETS ${name}_gui)
        set_property(GLOBAL APPEND PROPERTY LE_EXTENSION_GUIS ${name})
    endif()
endfunction()

if(SWIG_FOUND AND TCL_FOUND)
    set(LE_EXTENSIONS_HAVE_TCL ON)
else()
    set(LE_EXTENSIONS_HAVE_TCL OFF)
endif()

set(le_extension_cores "")
set(le_extension_declarations "")
set(le_extension_registrations "")
# The two extensions.json variants: the build tree's points at each
# extension's source directory, the bundle's at ext/<name> beside le_shell.
set(build_index "{\"format\": 1, \"extensions\": []}")
string(JSON build_index SET "${build_index}" layout_engine "\"${PROJECT_VERSION}\"")
string(JSON build_index SET "${build_index}" extension_api "${LE_EXTENSION_API_VERSION}")
set(bundle_index "${build_index}")
set(index_position 0)
foreach(name IN LISTS LE_EXTENSIONS)
    set(dir "${LE_EXTENSION_${name}_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LE_EXTENSION_${name}_MANIFEST})
    string(APPEND le_extension_registrations
        "        registry().add({\"${name}\", \"${LE_EXTENSION_${name}_VERSION}\"});\n")
    if(LE_EXTENSION_${name}_CMAKE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LE_EXTENSION_${name}_CMAKE})
        set(LE_CURRENT_EXTENSION ${name})
        include(${LE_EXTENSION_${name}_CMAKE})
        if(NOT TARGET ${name}_core)
            message(FATAL_ERROR "extension ${name}: ${LE_EXTENSION_${name}_CMAKE} must call le_add_extension(${name} ...)")
        endif()
        list(APPEND le_extension_cores ${name}_core)
        string(APPEND le_extension_declarations "void le_ext_${name}_register(le::ext::Registry &);\n")
        string(APPEND le_extension_registrations "        le_ext_${name}_register(registry());\n")
        set(tier compiled)
    elseif(LE_EXTENSION_${name}_SCHEMA)
        set(tier compiled) # its classes are compiled in, though it has no C++ of its own
    else()
        set(tier script)
    endif()

    # This extension's index entry, and its procs and resources in the bundle.
    set(entry "{\"procs\": []}")
    string(JSON entry SET "${entry}" name "\"${name}\"")
    string(JSON entry SET "${entry}" version "\"${LE_EXTENSION_${name}_VERSION}\"")
    string(JSON entry SET "${entry}" tier "\"${tier}\"")
    set(procs_position 0)
    foreach(procs IN LISTS LE_EXTENSION_${name}_TCL_PROCS)
        file(RELATIVE_PATH relative "${dir}" "${procs}")
        string(JSON entry SET "${entry}" procs ${procs_position} "\"${relative}\"")
        math(EXPR procs_position "${procs_position} + 1")
        get_filename_component(relative_dir "${relative}" DIRECTORY)
        install(FILES "${procs}" DESTINATION "ext/${name}/${relative_dir}" COMPONENT bundle)
    endforeach()
    foreach(resource IN LISTS LE_EXTENSION_${name}_RESOURCES)
        file(RELATIVE_PATH relative "${dir}" "${resource}")
        get_filename_component(relative_dir "${relative}" DIRECTORY)
        if(IS_DIRECTORY "${resource}")
            install(DIRECTORY "${resource}" DESTINATION "ext/${name}/${relative_dir}" COMPONENT bundle)
        else()
            install(FILES "${resource}" DESTINATION "ext/${name}/${relative_dir}" COMPONENT bundle)
        endif()
    endforeach()
    string(JSON build_entry SET "${entry}" dir "\"${dir}\"")
    string(JSON bundle_entry SET "${entry}" dir "\"ext/${name}\"")
    string(JSON build_index SET "${build_index}" extensions ${index_position} "${build_entry}")
    string(JSON bundle_index SET "${bundle_index}" extensions ${index_position} "${bundle_entry}")
    math(EXPR index_position "${index_position} + 1")

    # Each tcl_tests script runs through the build tree's le_shell, with
    # every extension loaded; it fails the test by raising an error.
    foreach(test IN LISTS LE_EXTENSION_${name}_TCL_TESTS)
        if(LE_EXTENSIONS_HAVE_TCL)
            get_filename_component(test_name "${test}" NAME_WE)
            add_test(NAME ${name}.${test_name} COMMAND $<TARGET_FILE:le_shell> "${test}")
        endif()
    endforeach()
    # What the extension's tests (ctest -R "^<name>\.") need built: `le test`.
    add_custom_target(${name}_test_deps)
    add_dependencies(${name}_test_deps le_shell)
    if(TARGET ${name}_tests)
        add_dependencies(${name}_test_deps ${name}_tests)
    endif()
    message(STATUS "Extension ${name} ${LE_EXTENSION_${name}_VERSION} (${tier}, ${dir})")
endforeach()
unset(LE_CURRENT_EXTENSION)

# The build tree's index is staged at configure time and copied into place by
# the build (le_extensions_index, which le_shell depends on), so it never
# lists different extensions from the le_shell beside it - as it would after
# a configure that isn't followed by a build.
file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/build_extensions.json CONTENT "${build_index}\n")
set(LE_EXTENSIONS_INDEX ${PROJECT_BINARY_DIR}/extensions.json)
add_custom_command(
    OUTPUT ${LE_EXTENSIONS_INDEX}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_BINARY_DIR}/generated/extensions/build_extensions.json ${LE_EXTENSIONS_INDEX}
    DEPENDS ${PROJECT_BINARY_DIR}/generated/extensions/build_extensions.json
    VERBATIM)
add_custom_target(le_extensions_index DEPENDS ${LE_EXTENSIONS_INDEX})
file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/extensions.json CONTENT "${bundle_index}\n")
install(FILES ${PROJECT_BINARY_DIR}/generated/extensions/extensions.json DESTINATION . COMPONENT bundle)

# register_all(): each extension's info, then its register function, in
# dependency order. file(CONFIGURE) writes only on a change, so a
# reconfigure doesn't rebuild it.
file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_init.cpp @ONLY CONTENT [=[
// Generated by cmake/le_extensions.cmake from LE_EXTENSION_DIRS - do not edit.
#include "le/extension.hpp"

@le_extension_declarations@
namespace le::ext
{
    void register_all()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
@le_extension_registrations@    }
}
]=])
add_library(le_extensions STATIC ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_init.cpp)
target_link_libraries(le_extensions PUBLIC le::extension_sdk ${le_extension_cores})

if(LE_EXTENSIONS_HAVE_TCL)
    get_property(le_extension_tcl_targets GLOBAL PROPERTY LE_EXTENSION_TCL_TARGETS)
    get_property(le_extension_swig_files GLOBAL PROPERTY LE_EXTENSION_SWIG_FILES)
    get_property(le_extension_tcl_inits GLOBAL PROPERTY LE_EXTENSION_TCL_INITS)
    set(swig_includes "")
    foreach(swig IN LISTS le_extension_swig_files)
        string(APPEND swig_includes "%include \"${swig}\"\n")
    endforeach()
    file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/le_api_extensions.i @ONLY CONTENT [=[
// Generated by cmake/le_extensions.cmake from LE_EXTENSION_DIRS - do not edit.
// Every extension's TCL_SWIG files, %include'd by le_api.i.
@swig_includes@]=])
    set(tcl_declarations "")
    set(tcl_calls "")
    foreach(name IN LISTS le_extension_tcl_inits)
        string(APPEND tcl_declarations "void le_ext_${name}_init_tcl(Tcl_Interp *);\n")
        string(APPEND tcl_calls "        le_ext_${name}_init_tcl(interp);\n")
    endforeach()
    file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_tcl_init.cpp @ONLY CONTENT [=[
// Generated by cmake/le_extensions.cmake from LE_EXTENSION_DIRS - do not edit.
#include "le/extension_tcl.hpp"

@tcl_declarations@
namespace le::ext
{
    void init_tcl([[maybe_unused]] Tcl_Interp *interp)
    {
@tcl_calls@    }
}
]=])
    add_library(le_extensions_tcl STATIC ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_tcl_init.cpp)
    target_include_directories(le_extensions_tcl PUBLIC ${TCL_INCLUDE_PATH})
    target_link_libraries(le_extensions_tcl PUBLIC le_extensions ${le_extension_tcl_targets})
endif()

# register_all_gui(): each extension's GUI registrations, in dependency order.
get_property(le_extension_gui_targets GLOBAL PROPERTY LE_EXTENSION_GUI_TARGETS)
get_property(le_extension_guis GLOBAL PROPERTY LE_EXTENSION_GUIS)
set(gui_declarations "")
set(gui_calls "")
foreach(name IN LISTS le_extension_guis)
    string(APPEND gui_declarations "void le_ext_${name}_register_gui(le::ext::GuiRegistry &);\n")
    string(APPEND gui_calls "        {\n            GuiRegistry registry(\"${name}\");\n            le_ext_${name}_register_gui(registry);\n        }\n")
endforeach()
file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_gui_init.cpp @ONLY CONTENT [=[
// Generated by cmake/le_extensions.cmake from LE_EXTENSION_DIRS - do not edit.
#include "le/extension_gui.hpp"

@gui_declarations@
namespace le::ext
{
    void register_all_gui()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
@gui_calls@    }
}
]=])
add_library(le_extensions_gui STATIC ${PROJECT_BINARY_DIR}/generated/extensions/le_extensions_gui_init.cpp)
target_link_libraries(le_extensions_gui PUBLIC gui le_extensions ${le_extension_gui_targets})
