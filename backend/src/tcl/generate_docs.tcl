# One-shot driver for UPDATES.md item 20's Markdown documentation
# (generate_command_docs, le_tcl_procs.tcl) - run via the
# generate-tcl-docs skill, not part of the regular ctest suite (doc
# generation isn't a regression check, it's a build artifact this writes
# straight to the committed backend/TCL_COMMANDS.md).
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <output .md path>

if {[llength $argv] != 3} {
    puts stderr "usage: generate_docs.tcl <le_tcl.so> <le_tcl_procs.tcl> <output.md>"
    exit 2
}
lassign $argv module_path procs_path output_path

load $module_path le_tcl
source $procs_path

generate_command_docs $output_path
puts "wrote $output_path"
