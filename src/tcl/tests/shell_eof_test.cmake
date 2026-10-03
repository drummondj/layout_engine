# Runs interactive le_shell with no `exit` in its input, once from a pipe
# and once from a redirected file. Each run must evaluate every line,
# including a last one with no trailing newline, then exit 0 at end of
# input without asking about the unsaved edit it made. HOME points at
# WORK_DIR so the user's saved settings are neither read nor written.
#
# Inputs (-D): LE_SHELL, LE_TCL_MODULE, LE_TCL_PROCS, WORK_DIR.
file(MAKE_DIRECTORY ${WORK_DIR})
set(input ${WORK_DIR}/input.tcl)
file(WRITE ${input} "create_library -name eof_test\nputs shell_eof_test:first\nputs shell_eof_test:last")
set(shell ${CMAKE_COMMAND} -E env HOME=${WORK_DIR} ${LE_SHELL} -module ${LE_TCL_MODULE} -procs ${LE_TCL_PROCS})

foreach(mode pipe file)
    if(mode STREQUAL "pipe")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E cat ${input}
            COMMAND ${shell}
            RESULTS_VARIABLE results
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
            TIMEOUT 20
        )
        list(GET results 1 result)
    else()
        execute_process(
            COMMAND ${shell}
            INPUT_FILE ${input}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
            TIMEOUT 20
        )
    endif()
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${mode}: expected exit status 0, got '${result}'\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    foreach(expected "\nshell_eof_test:first\n" "\nshell_eof_test:last\n")
        string(FIND "${output}" "${expected}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${mode}: missing '${expected}'\nstdout:\n${output}\nstderr:\n${error}")
        endif()
    endforeach()
    if(output MATCHES "Exit anyway")
        message(FATAL_ERROR "${mode}: asked about unsaved changes at end of input\nstdout:\n${output}")
    endif()
endforeach()
