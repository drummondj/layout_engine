# Pipeline Refactor

We are going to restart the pipelines module implementation, so it works for a large amount of design data.
I have moved the current pipelines module into pipelines.old.

## Methodology

Implement each stage and pipeline incrementally, benchmarking as we go, and carefully reviewing in small incremental changes. This will take time.

All stages must be implemented with a class from tbb_core.hpp. If the generics in that file are not suitable, we should architect another.

## Benchmarking

Using the aes_scaling data we should measure the performance of each stage and pipeline to determine big-O scaling. I would like to see every benchmark use 1x1, 2x1, 2x2, 3x2 and 3x3 tiling, so we get 5 points of data.

## Structure

### ViewRenderPipeline

The pipeline will be split into 3 parts, cold, warm and hot:

1. Cold - converts the root data into a set of shapes (in dbu) per AbstractId and LayoutId, plus a list of ViewLayers
    Input: Pointer to Root database.
    Options: top_level AbstractId or LayoutId, root mutation_version, hierarchy_depth.
    Output: A vector of shapes per Abstract and Layout, including placement data for each Placement plus a vector of ViewLayers
    Speed requirement: Max 5s
    Stages:
        1. LayerGeneration - generates ViewLayers from Technology data
            Input: Pointer to Root database
            Output: vector of ViewLayers
        2. HierarchyResolver - traverses hierachy from top-level AbstractId or LayoutId until hierarchy_depth is 0.
            Input: Pointer to Root database and LayerGenerationOutput
            Output:
                struct ViewShape
                {
                    Shape shape;
                    ViewLayerId view_layer;
                }

                struct ViewPlacementData {
                    std::variant<AbstractId, LayoutId> id;
                    Point location;
                    Orientation orientation;
                }

                struct ViewData {
                    std::vector<ViewShape> shapes;
                    std::vector<ViewPlacementData> placement_data;
                }

                struct HierarchyResolverOutput {
                    std::unordered_map<std::variant<AbstractId, LayoutId>> view_data;
                }

2. Warm - TBD triggered by viewport changes, converts Cold data into razterized images.
    Speed requirement: 500ms (would prefer as fast as possible)
3. Hot - TBD Mouse movement, selection highlighting and zoom/selection rectangle rendering. Plus a final image composition.
    Speed requirement: 100ms (again as fast as possible)
