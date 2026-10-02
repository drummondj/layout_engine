# Runs interactive le_shell ITERATIONS times with commands on stdin:
# show_gui, an optional wait, then exit 3. The window must close and the
# process end with that status, never hang or crash, and output printed
# just before the exit must reach stdout. Runs alternate an immediate exit
# with one after the window has been open for a while; exiting while the
# GUI thread is opening or running a window is a race, so one run proves
# little. HOME points at WORK_DIR so the user's saved settings and window
# layout are neither read nor written.
#
# Inputs (-D): LE_SHELL, LE_TCL_MODULE, LE_TCL_PROCS, WORK_DIR, ITERATIONS.
file(MAKE_DIRECTORY ${WORK_DIR})
foreach(delay_ms 0 300)
    # ::le_shell_builtin_exit skips the interactive `exit`'s unsaved-changes prompt.
    file(WRITE ${WORK_DIR}/input_${delay_ms}.tcl
        "show_gui\nafter ${delay_ms}\nputs \"shell_gui_exit_test: exiting\"\n::le_shell_builtin_exit 3\n")
endforeach()

foreach(i RANGE 1 ${ITERATIONS})
    math(EXPR parity "${i} % 2")
    if(parity)
        set(delay_ms 0)
    else()
        set(delay_ms 300)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env HOME=${WORK_DIR}
            ${LE_SHELL} -module ${LE_TCL_MODULE} -procs ${LE_TCL_PROCS}
        INPUT_FILE ${WORK_DIR}/input_${delay_ms}.tcl
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 20
    )
    if(NOT result STREQUAL "3")
        message(FATAL_ERROR "run ${i} (delay ${delay_ms} ms): expected exit status 3, got '${result}'\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    if(NOT output MATCHES "shell_gui_exit_test: exiting")
        message(FATAL_ERROR "run ${i} (delay ${delay_ms} ms): output lost\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endforeach()
