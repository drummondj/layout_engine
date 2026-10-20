# Installs BUILD_DIR into WORK_DIR, checks the bundle has every file it needs,
# then runs the installed le_shell with no -module/-procs flags or
# LE_TCL_MODULE/LE_TCL_PROCS_PATH: it must load the bundle's own le_tcl.so
# and generated procs even though this build tree is still on disk. Then the
# runtime libraries: the launcher takes each from runtime/ only when the
# system lacks it or (the C++ runtime) has an older one, and the processes
# le_shell starts get the caller's LD_LIBRARY_PATH back.

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --component bundle --prefix "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${result}):\n${output}")
endif()

foreach(file le_shell le_shell.bin le_tcl.so le_tcl_procs.tcl le_tcl_procs_generated.tcl lucide.ttf extensions.json
        fonts/DejaVuSansMono.ttf fonts/Quicksand-Medium.ttf)
    if(NOT EXISTS "${WORK_DIR}/${file}")
        message(FATAL_ERROR "the bundle is missing ${file}")
    endif()
endforeach()
file(GLOB tbb "${WORK_DIR}/libtbb.so*")
if(NOT tbb)
    message(FATAL_ERROR "the bundle is missing libtbb")
endif()
foreach(unwanted include lib)
    if(EXISTS "${WORK_DIR}/${unwanted}")
        message(FATAL_ERROR "the bundle has a ${unwanted}/ directory - a dependency's install rules leaked into it")
    endif()
endforeach()

file(WRITE "${WORK_DIR}/smoke.tcl" [=[
puts "module [lindex [lindex [info loaded] 0] 0]"
puts "generated $::le_generated_procs_path"
puts "commands [llength [info commands get_*]]"
puts "index $::le_extensions_index"
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=LE_TCL_MODULE --unset=LE_TCL_PROCS_PATH --unset=LD_LIBRARY_PATH
            "${WORK_DIR}/le_shell" "${WORK_DIR}/smoke.tcl"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the installed le_shell failed (${result}):\n${output}\n${errors}")
endif()
file(REAL_PATH "${WORK_DIR}" work_dir)
foreach(expected "module ${work_dir}/le_tcl.so" "generated ${work_dir}/le_tcl_procs_generated.tcl" "index ${work_dir}/extensions.json")
    string(FIND "${output}" "${expected}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "expected \"${expected}\" - the bundle used files outside itself:\n${output}")
    endif()
endforeach()
string(REGEX MATCH "commands ([0-9]+)" _ "${output}")
if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 LESS 100)
    message(FATAL_ERROR "the generated get_* commands didn't load:\n${output}")
endif()

# --- runtime libraries ---
foreach(binary le_shell.bin le_tcl.so)
    file(READ_ELF "${WORK_DIR}/${binary}" RPATH rpath RUNPATH runpath)
    if(NOT "${rpath}${runpath}" STREQUAL "$ORIGIN")
        message(FATAL_ERROR "${binary}'s RPATH/RUNPATH is '${rpath}${runpath}', not just \$ORIGIN - it would look outside the bundle")
    endif()
endforeach()
if(NOT EXISTS "${WORK_DIR}/runtime/libstdc++.so.6/libstdc++.so.6")
    message(FATAL_ERROR "the bundle has no runtime/libstdc++.so.6")
endif()

# Where le_shell's libstdc++ and libtcl were loaded from, and what a process
# it starts sees as LD_LIBRARY_PATH.
file(WRITE "${WORK_DIR}/probe.tcl" [=[
set maps [open /proc/self/maps]
foreach line [split [read $maps] \n] {
    if {[regexp {(/\S*/(libstdc\+\+|libtcl8\.6)\S*)$} $line -> path name] && ![info exists loaded($name)]} {
        set loaded($name) $path
        puts "$name $path"
    }
}
close $maps
puts "child [exec sh -c {echo ${LD_LIBRARY_PATH-unset}}]"
]=])
# run_probe(<out var> <env assignments>...): runs probe.tcl through the bundle.
# With <out var>_MISSING_OK, a host lacking a library le_shell needs sets
# <out var> to MISSING instead of failing.
function(run_probe out)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=LE_BUNDLED_LIBRARIES ${ARGN}
                "${WORK_DIR}/le_shell" "${WORK_DIR}/probe.tcl"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0 AND ${out}_MISSING_OK AND errors MATCHES "error while loading shared libraries")
        set(${out} MISSING PARENT_SCOPE)
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "le_shell (${ARGN}) failed (${result}):\n${output}\n${errors}")
    endif()
    set(${out} "${output}" PARENT_SCOPE)
endfunction()
# expect_loaded(<output> <library> bundled|system)
function(expect_loaded output library where)
    string(REGEX MATCH "${library} ([^\n]*)" _ "${output}")
    string(FIND "${CMAKE_MATCH_1}" "${work_dir}/runtime/" at)
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "${library} wasn't loaded:\n${output}")
    elseif(where STREQUAL "bundled" AND NOT at EQUAL 0)
        message(FATAL_ERROR "expected the bundle's ${library}, got ${CMAKE_MATCH_1}")
    elseif(where STREQUAL "system" AND at EQUAL 0)
        message(FATAL_ERROR "expected the system's ${library}, got ${CMAKE_MATCH_1}")
    endif()
endfunction()

run_probe(output LE_BUNDLED_LIBRARIES=all LD_LIBRARY_PATH=/caller/lib)
expect_loaded("${output}" "libstdc\\+\\+" bundled)
if(EXISTS "${WORK_DIR}/runtime/libtcl8.6.so")
    expect_loaded("${output}" "libtcl8.6" bundled)
endif()
string(FIND "${output}" "child /caller/lib\n" at)
if(at EQUAL -1)
    message(FATAL_ERROR "a process le_shell starts should get the caller's LD_LIBRARY_PATH back:\n${output}")
endif()
run_probe(output LE_BUNDLED_LIBRARIES=all)
string(FIND "${output}" "child unset\n" at)
if(at EQUAL -1)
    message(FATAL_ERROR "with no LD_LIBRARY_PATH of the caller's, a process le_shell starts should have none:\n${output}")
endif()
# Using only the host's libraries works only on a host that has them all.
set(host_output_MISSING_OK TRUE)
run_probe(host_output LE_BUNDLED_LIBRARIES=none)
if(host_output STREQUAL "MISSING")
    message(STATUS "this host lacks libraries le_shell needs - skipping the checks that use only the host's")
    set(host_has_all FALSE)
else()
    expect_loaded("${host_output}" "libstdc\\+\\+" system)
    set(host_has_all TRUE)
endif()

# The launcher's choice, with a stand-in ldconfig: the system's libstdc++
# (older or newer than the bundle's) and libtcl (absent).
execute_process(COMMAND uname -m OUTPUT_VARIABLE machine OUTPUT_STRIP_TRAILING_WHITESPACE)
if(machine STREQUAL "x86_64")
    set(arch "x86-64")
elseif(machine STREQUAL "aarch64")
    set(arch "AArch64")
else()
    message(FATAL_ERROR "no ldconfig architecture tag for ${machine} - add it here and in le_shell_launcher.sh")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}/fake/bin")
foreach(case older newer)
    if(case STREQUAL "older")
        file(WRITE "${WORK_DIR}/fake/libstdc++.so.6" "GLIBCXX_3.4\nGLIBCXX_3.4.1\n")
    else()
        file(WRITE "${WORK_DIR}/fake/libstdc++.so.6" "GLIBCXX_3.4\nGLIBCXX_3.4.999\n")
    endif()
    file(WRITE "${WORK_DIR}/fake/bin/ldconfig"
        "#!/bin/sh\nprintf '\\tlibstdc++.so.6 (libc6,${arch}) => ${WORK_DIR}/fake/libstdc++.so.6\\n'\n")
    file(CHMOD "${WORK_DIR}/fake/bin/ldconfig" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    # A newer system libstdc++ is believed, so this host's real one has to do.
    if(case STREQUAL "newer" AND NOT host_has_all)
        continue()
    endif()
    run_probe(output "PATH=${WORK_DIR}/fake/bin:$ENV{PATH}")
    if(case STREQUAL "older")
        expect_loaded("${output}" "libstdc\\+\\+" bundled)
    else()
        expect_loaded("${output}" "libstdc\\+\\+" system)
    endif()
    if(EXISTS "${WORK_DIR}/runtime/libtcl8.6.so")
        expect_loaded("${output}" "libtcl8.6" bundled)
    endif()
endforeach()

