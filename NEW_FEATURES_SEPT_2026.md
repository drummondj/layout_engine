1. Boolean operations on shapes - DONE

I would like a set of TCL commands to perform boolean operations and conversions on shapes.

1.1 COPY - copies the specified shapes from to another layer
1.2 MOVE - moves the specified shapes to another layer
1.3 OR, AND, NOT etc - boolean operations to create new shapes from existing shapes
1.4 TO POLYGON - convert shapes to polygons
1.5 TO RECTS - convert shapes to rects, either vertical or horizontal fracturing (user decides)
1.6 BBOX - returns bbox of the shapes
1.7 SIZE - sizes shapes in X and Y, or both
1.8 PATH - creates a path following the rect boundary or polygon line shapes for a user specified width

All operations take a list of shapes as input (or two lists for boolean operations).

Shapes do not need to be associated with a particular object, unless the user wants them to be written out with write_def. So the database schema needs to be updated for this.

2. Placement moving - DONE

Please add the ability to move a placement. In move mode, when a placement is selected a secondary toolbar should appear under the main toolbar (this is a pattern that will be re-used across different tools). The secondary toolbar will contain:

- Snapping options, either site, finfet grid, manufacturing grid, or no snapping
- Buttons to rotate the placement orientation, horizontal and vertical flips

Site snapping will snap the ghost view to the row's site grid and make sure the orientation matches the rows symmetry.

3. Shape resizing

Shapes can be resized using a resize tool. A snapping options secondary toolbar will appear with options to:

- Paths - snap center of path to tracks, snap edges to manufacturing grid, snap to user grid, or no snap
- Polygons - snap to user grid, manufacturing grid, finfet grid, or no snap
- Rects - snap to user grid, manufacturing grid, finfet grid, or no snap

- Rectangles can be resized by dragging their edges.
- Polygons segments can be individually moved by dragging their edges.
- Paths segments can be individually moved by dragging anywhere on the segment and the adjacent segment points are moved too.

4. Custom library naming - DONE

Currently, when the user reads LEF, DEF and Verilog a library is created automatically based on the filename. I would like to change this behavour so that a library name is required as a TCL argument.

If a view already exists for the design being read in, then an error should occur but multiple views Abstract, Layout and Schematic can be read into the same design.

If the library name doesn't already exist, then a new library should be created.

5. Flightline display - DONE

Net connections should be drawn between selected placement pins using a light blue line. A separate layer purpose should be used for this, which is invisible by default.





