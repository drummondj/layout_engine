# Install-time part of the `bundle` component (install(SCRIPT), after
# le_shell and le_tcl.so are installed): the shared libraries a host can't be
# relied on to provide, each in runtime/<soname>/ for the launcher
# (src/tcl/le_shell_launcher.sh) to use when the host lacks it.
#
# Inputs, set by an install(CODE) just before:
#   LE_BUNDLE_SHELL, LE_BUNDLE_TCL_MODULE  the build tree's le_shell and le_tcl.so
#   LE_BUNDLE_OBJDUMP                      objdump, to read their dependencies
#   LE_BUNDLE_TCL_LIBRARY                  Tcl's script library matching the linked libtcl, or empty

set(prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")

# The launcher is installed as le_shell next; the binary it starts is le_shell.bin.
file(RENAME "${prefix}/le_shell" "${prefix}/le_shell.bin")

# Exactly $ORIGIN (for libtbb beside them): a toolchain's compiler can add
# its own library directory to the RPATH, which wouldn't exist on another
# machine.
foreach(binary le_shell.bin le_tcl.so)
    file(RPATH_SET FILE "${prefix}/${binary}" NEW_RPATH "$ORIGIN")
endforeach()

# Only these, and only what le_shell or le_tcl.so actually links. Never
# glibc (the ABI everything else depends on) or GL/X11 (the host's driver
# must be used).
set(bundled
    "^libstdc\\+\\+\\.so"
    "^libgcc_s\\.so"
    "^libreadline\\.so"
    "^libhistory\\.so"
    "^libtinfo\\.so"
    "^libncursesw?\\.so"
    "^libtcl8\\.6\\.so"
    "^libz\\.so")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${LE_BUNDLE_OBJDUMP}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${LE_BUNDLE_SHELL}"
    LIBRARIES "${LE_BUNDLE_TCL_MODULE}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_INCLUDE_REGEXES ${bundled}
    PRE_EXCLUDE_REGEXES ".*")
if(unresolved)
    message(FATAL_ERROR "bundle: can't find ${unresolved}, which le_shell or le_tcl.so links")
endif()

file(REMOVE_RECURSE "${prefix}/runtime")
set(have_tcl FALSE)
foreach(library IN LISTS resolved)
    get_filename_component(soname "${library}" NAME)
    file(INSTALL "${library}" DESTINATION "${prefix}/runtime/${soname}" FOLLOW_SYMLINK_CHAIN)
    if(soname MATCHES "^libtcl")
        set(have_tcl TRUE)
    endif()
endforeach()

# libtcl finds its script library (init.tcl and the rest) where it was
# built to look, which is gone on another machine. Without a matching one,
# the host's Tcl has to do.
if(have_tcl)
    if(LE_BUNDLE_TCL_LIBRARY)
        file(INSTALL "${LE_BUNDLE_TCL_LIBRARY}/" DESTINATION "${prefix}/runtime/tcl-library")
    else()
        file(GLOB tcl_runtime "${prefix}/runtime/libtcl*")
        file(REMOVE_RECURSE ${tcl_runtime})
        message(WARNING "bundle: no Tcl script library matching the linked libtcl was found at configure time, "
            "so libtcl isn't bundled - the bundle needs the host's Tcl 8.6")
    endif()
endif()
