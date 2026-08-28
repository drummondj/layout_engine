I've been experimenting with oneTBB in a separate Claude session and come up with a set of petterns I want to use in this project.

The source code for these patterns lives in /Volumes/Docking/Projects/synthosilicon/oneTBB_test. The only file you need is core.hpp, so copy that into here. The other classes are examples of how to use the data structs and classes in core.hpp.

I would like to merge the code in the instancing, pipeline and render modules into one module called pipelines.

The first step is to determine the shape of the classes in the pipeline. May first pass outline looks like this:

1. Seperate pipelines for:
   1.1 Abstract view shape generation
   1.2 Layout view shape generation (calls the abstract view pipeline for leaf cells)
   1.3 There is a pipeline per layer:
   - the layout/abstract render view
   - mouse target and movement layer
   - selection and ghost layers

2. Each pipeline has a set of stages which use the classes defined in the oneTBB_test projects code.hpp file. Stages may be shaed between pipelines, but each pipeline and stage has a clear boundary. Each function in the current code based becomes a stage.

3. PipelineOptions is used to hold all data that can change how and if a pipeline stage is re-computed.

Please remove all the previous experiments with taskflow. It wasn't fit for purpose.

Please create a plan and we will go from there. Yes, this is a big undertaking :)
