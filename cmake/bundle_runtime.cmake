# Install-time part of the `bundle` component (install(SCRIPT), after
# le_shell and le_tcl.so are installed): the shared libraries a host can't be
# relied on to provide, each in runtime/<soname>/ for the launcher
# (src/tcl/le_shell_launcher.sh) to use when the host lacks it.
#
# Inputs, set by an install(CODE) just before:
#   LE_BUNDLE_SHELL, LE_BUNDLE_TCL_MODULE  the build tree's le_shell and le_tcl.so
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

# Only these, and only what le_shell or le_tcl.so actually loads. Never
# glibc (the ABI everything else depends on) or GL/X11 (the host's driver
# must be used). ldd asks the dynamic loader, through the binaries' own
# RPATH, so each is the copy the build tree runs with - not whichever the
# host's system directories happen to hold first.
set(bundled "^(libstdc\\+\\+|libgcc_s|libreadline|libhistory|libtinfo|libncursesw?|libtcl8\\.6|libz)\\.so")
set(resolved "")
foreach(binary "${LE_BUNDLE_SHELL}" "${LE_BUNDLE_TCL_MODULE}")
    execute_process(COMMAND ldd "${binary}" OUTPUT_VARIABLE ldd_output RESULT_VARIABLE ldd_result)
    if(NOT ldd_result EQUAL 0)
        message(FATAL_ERROR "bundle: ldd ${binary} failed:\n${ldd_output}")
    endif()
    string(REPLACE "\n" ";" ldd_lines "${ldd_output}")
    foreach(line IN LISTS ldd_lines)
        if(line MATCHES "^[ \t]*([^ \t]+) => not found")
            message(FATAL_ERROR "bundle: ${binary} needs ${CMAKE_MATCH_1}, which the loader can't find")
        elseif(line MATCHES "^[ \t]*([^ \t]+) => ([^ \t]+) \\(")
            set(soname "${CMAKE_MATCH_1}")
            set(library "${CMAKE_MATCH_2}")
            if(soname MATCHES "${bundled}")
                list(APPEND resolved "${soname}=${library}")
            endif()
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES resolved)

file(REMOVE_RECURSE "${prefix}/runtime")
set(have_tcl FALSE)
foreach(entry IN LISTS resolved)
    string(REGEX MATCH "^([^=]+)=(.*)$" _ "${entry}")
    set(soname "${CMAKE_MATCH_1}")
    set(library "${CMAKE_MATCH_2}")
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
