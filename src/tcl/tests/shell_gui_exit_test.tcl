# Run by shell_gui_exit_test.cmake through the real le_shell: opens the
# window, optionally lets it run for `delay_ms`, then exits with a status
# the runner checks. The window must close and the process end with that
# status, never hang or crash, and the last `puts` must reach stdout.
lassign $argv delay_ms
show_gui
if {$delay_ms > 0} {
    after $delay_ms
}
puts "shell_gui_exit_test: exiting"
exit 3
