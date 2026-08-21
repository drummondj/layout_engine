# Regression check for the integrated help system (UPDATES.md item 20):
# the ::command_help registry, help/man/complete_command/
# generate_command_docs (le_tcl_procs.tcl), and the generated -help
# support/registration calls (le_tcl_procs_generated_tcl_j2.py) for
# get_<type>/create_<type>/update_<type>. No LEF fixture needed -
# complete_command's dot-path completion is pure static schema metadata
# (::property_scalars/::property_hops), never a live query, so a
# friendly-id token like "shape:0" works fine as a completion seed even
# though no such Shape actually exists in an empty session.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>

if {[llength $argv] != 2} {
    puts stderr "usage: help_test.tcl <le_tcl.so> <le_tcl_procs.tcl>"
    exit 2
}
lassign $argv module_path procs_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

proc check_true {what actual} {
    if {!$actual} {
        puts stderr "FAIL: $what - expected true, got {$actual}"
        exit 1
    }
    puts "ok: $what"
}

proc check_contains {what haystack needle} {
    check_true "$what (looking for \"$needle\")" [expr {[string first $needle $haystack] >= 0}]
}

load $module_path le_tcl
source $procs_path

# --- help ---

set help_output [help get_*]
check_contains "help get_* mentions get_terminals" $help_output "get_terminals"
check_contains "help get_* mentions get_shapes" $help_output "get_shapes"
check_true "help matches multiple commands" [expr {[llength [split $help_output "\n"]] > 1}]

check "help with no matches" \
    "help: no commands match \"no_such_command_*\"" \
    [help no_such_command_*]

# --- man ---

set man_output [man get_terminals]
check_contains "man get_terminals includes its own usage" $man_output "get_terminals"
check_contains "man get_terminals includes an Options: table" $man_output "Options:"
check_contains "man get_terminals documents -filter" $man_output "-filter"
check_contains "man get_terminals documents -help" $man_output "-help"

if {[catch {man no_such_command} err]} {
    puts "ok: man rejects an unregistered command ($err)"
} else {
    puts stderr "FAIL: man accepted an unregistered command name"
    exit 1
}

# --- generated -help (get_<type>/create_<type>/update_<type>) ---

check_contains "get_terminals -help returns its own usage" [get_terminals -help] "get_terminals"

set create_help [create_terminal -help]
check_contains "create_terminal -help mentions -name" $create_help "-name"
check_contains "create_terminal -help mentions -direction" $create_help "-direction"

set update_help [update_terminal bogus_id -help]
check_contains "update_terminal <id> -help never touches <id>" $update_help "update_terminal"

# Regression: update_<type> takes a *mandatory* leading positional (id),
# so calling it as bare `update_<type> -help` with no id at all binds
# "-help" to id, leaving args empty - a real bug where this fell through
# to "at least one -flag is required" instead of returning the usage.
set update_help_no_id [update_terminal -help]
check_contains "update_terminal -help (no id at all) still returns its own usage" \
    $update_help_no_id "update_terminal"

# Same bug, same fix, in the hand-written open_design (its own leading
# positional is `name`, not `id`).
set open_design_help [open_design -help]
check_contains "open_design -help (no name at all) still returns its own usage" \
    $open_design_help "open_design"

# --- registration covers every generated command ---

check_true "get_terminals is registered with real options" \
    [expr {[llength [dict get $::command_help get_terminals options]] > 0}]
check_true "create_terminal is registered with real options" \
    [expr {[llength [dict get $::command_help create_terminal options]] > 0}]
check_true "update_terminal is registered with real options" \
    [expr {[llength [dict get $::command_help update_terminal options]] > 0}]

# --- complete_command: command-name completion ---

check_true "complete_command completes get_t* to include get_terminals" \
    [expr {"get_terminals" in [complete_command {get_t}]}]
check_true "complete_command with no input suggests every command" \
    [expr {[llength [complete_command {}]] == [llength [dict keys $::command_help]]}]

# Command-name completion also has to work right after "[" - Tcl command
# substitution nested inside another command's own argument (e.g.
# `set t [get_`) - both glued (no space after "[") and separated (a
# space after "[" makes it its own token) forms. A glued candidate is
# itself an unbalanced-bracket token (same reasoning as a -filter
# candidate's own leading brace - see the dot-path tests below), so it's
# compared as a plain string via `check`, not list membership.
check "complete_command completes a command name glued right after \[" \
    {[get_technologies} [complete_command "set t \[get_technol"]
check_true "complete_command completes a command name after \[ with a space" \
    [expr {"get_technologies" in [complete_command "set t \[ get_technol"]}]
check_true "complete_command completes a command name nested inside another command's own argument" \
    [expr {"\[get_terminals" in [complete_command "get_properties \[get_ter"]}]

# --- complete_command: flag completion ---

check_true "complete_command completes get_terminals -f* to include -filter" \
    [expr {"-filter" in [complete_command {get_terminals -f}]}]
check "complete_command flag completion on an unregistered command" \
    {} [complete_command {no_such_command -f}]

# Flag/dot-path completion for a *nested* command's own arguments (once
# its name has already been typed, unlike the command-name-completion
# cases above) needs that command's own options, not the outer command's
# - both glued and separated bracket forms, and confirms a *closed*
# bracket correctly falls back to the outer scope again.
check_true "complete_command completes a nested command's own flag (glued bracket)" \
    [expr {"-filter" in [complete_command "set t \[get_terminals -f"]}]
check_true "complete_command completes a nested command's own flag (separated bracket)" \
    [expr {"-filter" in [complete_command "set t \[ get_terminals -f"]}]
# A brace-prefixed candidate (see the top-level -filter tests above) is
# its own unbalanced-bracket-and-brace token, not a well-formed Tcl list
# element - compared as a plain string via `check`, not list membership.
check "complete_command completes a dot-path inside a nested command's own -filter value" \
    "\{.direction" [complete_command "get_properties \[get_terminals -filter \{.dir"]
check "complete_command flag completion returns to the outer scope once its bracket has closed" \
    {} [complete_command "set t \[get_terminals\] -f"]

# --- complete_command: dot-path completion (no LEF/live objects needed -
# see this file's own header comment) ---

check_true "complete_command completes a single-level property path" \
    [expr {".layer_name" in [complete_command {get_properties shape:0 .lay}]}]
check_true "complete_command completes a chained-hop property path" \
    [expr {".terminal_port.terminal" in [complete_command {get_properties shape:0 .terminal_port.te}]}]
check "complete_command dot-path completion outside get_properties/report_properties" \
    {} [complete_command {get_terminals .lay}]
check "complete_command dot-path completion with no resolvable seed token" \
    {} [complete_command {get_properties .lay}]

# --- complete_command: dot-path completion inside a get_<type> command's
# own -filter value (this section's own request) - seeded from
# ::get_command_class (the command's own class), not a friendly-id
# token, since a -filter expression never has one. Every test line here
# is built via `set line "..."` (double-quoted, not {}-quoted) since the
# partial input being completed is deliberately an *unbalanced* brace
# (e.g. "-filter {.dir", the opening brace with no close yet) - writing
# that literally as a {}-quoted Tcl argument wouldn't parse. A returned
# candidate that itself starts with that same unbalanced "{" is, for the
# same reason, not test-inspectable via `in`/`llength`/`lindex` (Tcl
# list operations - it isn't a well-formed list element on its own), so
# these compare complete_command's own result directly as a plain string
# via `check`, not list membership.

check "complete_command completes a property path right after -filter's own opening brace" \
    "{.direction" [complete_command "get_terminals -filter {.dir"]
check "complete_command completes a chained-hop path inside -filter" \
    "{.terminal.name" [complete_command "get_terminal_ports -filter {.terminal.na"]
check "complete_command completes a later segment of one -filter expression" \
    ".direction" [complete_command "get_terminals -filter {.direction == INPUT || .dir"]
check "complete_command -filter dot-path completion is scoped to -filter's own value" \
    {} [complete_command "get_terminals -name IN0 .dir"]
check "complete_command -filter dot-path completion never fires for a command with no -filter flag" \
    {} [complete_command "create_terminal -filter {.dir"]

# --- generate_command_docs ---

set docs [generate_command_docs]
check_contains "generate_command_docs includes a top-level heading" $docs "# TCL Command Reference"
check_contains "generate_command_docs includes get_terminals" $docs "## get_terminals"
check_contains "generate_command_docs includes create_terminal" $docs "## create_terminal"
check_contains "generate_command_docs includes report_properties (hand-written)" $docs "## report_properties"

puts "le_tcl help system test passed"
