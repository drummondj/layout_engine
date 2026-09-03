# This is a test to try and implement async TCL commands
# Users should see messages in the Terminal as they are emitted from the TCL interp
for { set i 0 } { $i < 10 } { incr i } {
    puts "Step $i"
    after 1000
}
