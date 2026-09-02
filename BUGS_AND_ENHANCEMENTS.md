# BUGS

- [x] B1. The terminal/route fill pattern does not work at different zoom levels. Not sure how long it has been like this.
- [x] B2. When hovering the mouse in select mode, it highlights the route under the mouse, but when I clock to select, it selects the placement underneath the route. Whatever is highlighted should be selected and routes should hit before the placements underneath.
- [x] B3. Via arrays are not being rendered.
- [x] B4. Running a zoom command before show_gui causes the layout viewer to hang.
- [x] B5. Resizing a sidebar should wait until the resize is finished before rendering a new frame in the layout window.
- [ ] B6. Closing the GUI window still causes a never ending beachball. Force quiting window results in B6_trace.txt report. 


# ENHANCEMENTS

- [x] E1. Objects are not selectable in layout view. As dicussed only ojects at the top-level of hierarchy should be selectable, including instances, rows, blockages etc.
- [x] E2. tracks, rows and gcellgrid puposes should be invisible by default. track visiblity should be able to be toggled by preferred and non-preffered routing direction. Tracks and gcellgrid lines should be dashed and not selectable.
- [x] E3. Async TCL feature. When executing TCL commands the Flutter UI should not freeze and TCL output should appear immediately after being emitted by the TCL interp. The example fontend/tcl/async_test.tcl should show each puts output every second. A small circular progress indicator should be added to the status bar when LeProvider is running anything.
- [x] E4. Via X patterns should use 2 pixel width so they are more visible underneath the routing patttern.
- [x] E5. When a command fails, it should still be added to the history so a user can recal the command and edit it to fix. Also, complete_commands should not be visibile in the history. For example, when I tab complete a command, a "complete_command" is added to the history, which should not happen.
- [x] E6. TCL output truncating should only truncate long TCL return values. Other output, such as the results of report_properties should not be truncated. This may require pushing down the truncation code into the TCL API instead of performing at the Flutter UI level. Which would also help achieve the same feature in le_shell.
- [x] E7. I need a benchmark for a large design that includes NET shapes and multiple levels of hierarchy. Please take the LEF test_data/ISPD22**final_benchmarks/**Nangate/NangateOpenCellLibrary.lef and DEF test_data/ISPD22\_\_final_benchmarks/AES_1/design_original.def, then create a new DEF that instances the aes design 25 times in a 5x5 grid. Call it test_data/aes_5x5.def.
- [x] E8. Please make boundary layer draw below technology layers, but above rows, using one shade lighter than rows.
- [x] E9. When the user recalls a TCL command after they have already started typing a command, then pressing the down arrow after the last command in history will restore the command they started typing. If they didn't start typing a command, then the final down arrow press should return empty text (which is logically the same as restoring the last command they started typing).
- [ ] E10. Max CPU control. When running on machines with 128+ CPUs I need to control the maximum threads avaialbe to layout engine's backend. So I need an arena with a user settable concurrency flag. Minimum should be 2, one for the TCL interface and one for ImGUI? With a default of 8.
- [x] E11. TCL command completion for file names. For certain commands, such as dump_png that have a filename argument, tab completion should match files on the filesystem. If possible this should also work for the TCL source command.
- [x] E12. Duplicate entries in LayerManager. Rows, boundary etc are in the Layer section and their purposes are in the purposes section. Which matches our implementation but doesn't make sense from a user perspective. So, layers that are "purpose" only i.e. not technology layers, should only be in the purposes section.
- [x] E13. Placement names should be drawn as text along the bottom edge of the Placement. If the width of the text is longer than the width of the Placement, it should be truncated by replacing characters at the begining of the name with "...". The font size should scale with the cell height, similar to how terminal text scales.
- [x] E14. Some TCL commands don't help -help options or work with the help command. For example, read_lef and read_def. Please make sure all TCL commands are integrated with the help system.
- [x] E15. The LibraryBrowser widget needs to be updated so the user can open an abstract view or a layout view. Each design node in the tree should have children for each view, if it exists, which can be clicked on to open that view in the LayoutEngine widget (via the open_design TCL command not directly in the API).
- [x] E16. The cross shapes next to terminal text should scale with the text instead of being a fixed height/width. Let's use half the height of the text.
- [x] E17. When interacting with the texture, zoom, pan etc. I would also like the spinner to show in the status bar when a frame is being rendered.
- [x] E18. Psuedo-abstract generation. When a Layout view containts Placements of other Layout views and those designs don't have an abstract view, then nothing is rendered at hierarchy depth 1. Which is expected. But I would like to draw the Placement boundary and placementName if there is no Abstract view.
- [x] E19. The anti-aliasing enable/disable option doesn't work on all text. Some are hardcoded as true. Even though it looks bad, text render is quite expensive, so it would be good to benchmark if this helps or not.
- [x] E20. Prefer TCL commands over directy API calls from LeProvider. Can you review which LeProvider methods call the API directly and if they can be replaced with TCL command calls? Then the user can see which UI action correspond to which TCL commands, which helps them script things. Also, it's better to have one way to execute API commands, and TCL is my prefered way. For example, if I used the UI to select and read a LEF file, I should see a read_lef command in the TCL history.
- [ ] E21. There is an inconsistency between how shape coordinates are displayed in the property viewer, and how TCL command accept coordinates as arguments. All properties and TCL commands should follow this convention:

    - Point: { x y }
    - Rect: { { llx lly } { urx ury } }
    - Polygon: { { x0 y0 } { x1 y1 } ...}
    - Path: {width { { x0 y0 } { x1 y1 } ...} }
- [ ] E22. When Placements with terminal labels are rendered, the text orientation is drawn depending on the orientation of the Placement. Is there anyway to always draw text the right way up without impacting performance? For example, using a custom canvas and overriding onDrawTextBlob.
- [x] E23. I'm wondering if it is efficent to create a SkPicture and replay it each time the zoom level changes. Would it be better to just directly render to the canvas and re-use the canvas across Placements? Is this even possible? Please investigate.
- [x] E24. PlacementNames should be rendered for sub-layout placements in the same way terminal text is for placement abstracts.
- [ ] E25. Test raster performance when recording SkPitcures with a SkRTreeFactory
