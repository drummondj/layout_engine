# Installs BUILD_DIR into WORK_DIR, checks the bundle has every file it needs,
# then runs the installed le_shell with no -module/-procs flags or
# LE_TCL_MODULE/LE_TCL_PROCS_PATH: it must load the bundle's own le_tcl.so
# and generated procs even though this build tree is still on disk.

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --component bundle --prefix "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${result}):\n${output}")
endif()

foreach(file le_shell le_tcl.so le_tcl_procs.tcl le_tcl_procs_generated.tcl lucide.ttf extensions.json
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
