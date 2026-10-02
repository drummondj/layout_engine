# Runs shell_gui_exit_test.tcl through le_shell ITERATIONS times,
# alternating an immediate exit with one after the window has been open
# for a while. Exiting while the GUI thread is opening or running a window
# is a race, so one run proves little.
#
# Inputs (-D): LE_SHELL, LE_TCL_MODULE, LE_TCL_PROCS, SCRIPT, ITERATIONS.
foreach(i RANGE 1 ${ITERATIONS})
    math(EXPR parity "${i} % 2")
    if(parity)
        set(delay_ms 0)
    else()
        set(delay_ms 300)
    endif()
    execute_process(
        COMMAND ${LE_SHELL} -module ${LE_TCL_MODULE} -procs ${LE_TCL_PROCS} ${SCRIPT} ${delay_ms}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 20
    )
    if(NOT result STREQUAL "3")
        message(FATAL_ERROR "run ${i} (delay ${delay_ms} ms): expected exit status 3, got '${result}'\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    if(NOT output MATCHES "shell_gui_exit_test: exiting")
        message(FATAL_ERROR "run ${i} (delay ${delay_ms} ms): script output lost\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endforeach()
