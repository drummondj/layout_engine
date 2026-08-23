Here are some things I would like updating after reviewing.

1. zoom fit does not work on a layout view, it zooms to the bottom left corner of the layout instead of sizing to it's contents.
2. Rendering a design with lot's of instances is too slow, for example see frontend/tcl/ispd_test.tcl. The main issue seems to be small shapes inside instances are being rendered as single pixels, even if the instance is very small. When an instance size is smaller than a pixel, it's picture should not be drawn, just a pixel in the BOUNDARY layer color.
3. There is no layer to switch on/off TRACKS.
4. Hierarchy depth controls are not exposed in the plugin.
