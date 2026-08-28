# BUGS

- [x] B1. The terminal/route fill pattern does not work at different zoom levels. Not sure how long it has been like this.

# ENHANCEMENTS

- [x] E1. Objects are not selectable in layout view. As dicussed only ojects at the top-level of hierarchy should be selectable, including instances, rows, blockages etc.
- [ ] E2. tracks, rows and gcellgrid puposes should be invisible by default. track visiblity should be able to be toggled by preferred and non-preffered routing direction. Tracks and gcellgrid lines should be dashed and not selectable.
- [ ] E3. Async TCL feature. When executing TCL commands the Flutter UI should not freeze and TCL output should appear immediately after being emitted by the TCL interp. The example fontend/tcl/async_test.tcl should show each puts output every second. Also, when a TCL command is running, all buttons and interactive elements in the Flutter UI should be disabled. This may require wrapping elements in a new Widget that consumes an LeProvider.running field. A small circular progress indicator should be added somewhere in the Terminal widget.
- [ ] E4. Via X patterns should use 3 pixel width so they are visible underneath the routing patttern.
- [ ] E5. When a command fails, it should still be added to the hostory so a user can recal the command and edit it to fix. Also, complete_commands should nto be visibile in the history.
- [ ] E6. TCL output truncating should only truncate long TCL return values. Other output, such as the results of report_properties should not be truncated. This may require pushing down the truncation code into the TCL API instead of performing at the Flutter UI level. Which would also help achieve the same feature in le_shell.
