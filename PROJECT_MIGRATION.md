I think we have completed most of the basic requirements for this project, so it's time to move on to a full DEF/LEF editor with SystemVerilog read/write capability.

Please copy this layout_engine_mvp project (or start from scratch) to a new project in a parallel directory called "layout_engine".

DEF has many more data structures than LEF and will contain significantly more data to render, so I would like to focus on the rendering performance of hierarchical data. Here are the high level steps:

1. Create a DEF reader and writer, using the same structure as lef_reader.cpp and lef_writer.cpp. This involves adding new Klasses to the database schema.py including a new Layout view which is a child of the Design klass and contains all the objects from from DEF. The test DEF for this work is here: backend/src/lefdef/def/TEST/complete.5.8.def

2. Setup layer generation for custom layers. DEF has different object types, such as rows, tracks and different types of blockages which will require new Technology layers and purposes.

3. Update render pipeline.

3.1. Render objects from the Layout view including cells which have Abstract views. This involves creating Abstract view renderings, which can be drawn in multiple locations in the Layout view with different orientations. Therefore the renderer must create cache images of each instanced Asbtract to re-use across the Layout.

3.2 Layout views can also contain instances of other Layout views. How this is displayed is controlled by a hierarchy depth setting on the Scene. So, if hierarchy depth is 2, then 2 levels of Layout views are used one for the top-level and one for children. Then the children use Abstract views for their children. Any instances without Layout views fallback to Abstract views.

The key part here is rendering performance. So please create a large testcase, i.e. a sub-block with 1 million instances as a child. Then a top-level which has 4 instances of a sub-block, with different orientations.
