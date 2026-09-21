1. Boolean operations on shapes

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

2. Placement moving

Please add the ability to move a placement. In move mode, when a placement is selected a secondary toolbar should appear under the main toolbar (this is a pattern that will be re-used across different tools). The secondary toolbar will contain:

2.1 Snapping options, either site, manufacturing grid, or no snapping
2.2 Buttons to rotate the placement orientation, horizontal and vertical flips

Site snapping will snap the ghost view to the row's site grid and make sure the orientation matches the rows symmetry.




