// Benchmarks for the shape_* operations (NEW_FEATURES_SEPT_2026.md item 1)
// and for rendering the free-standing shapes they create:
//
//  - Geometry ops on a synthetic "clustered" layer: C disjoint clusters of
//    4 overlapping rects (N = 4C rects) - merging leaves C separate
//    polygons, the realistic case for a real layer's many disjoint shapes
//    and the one where the merge strategy matters (a result with a single
//    polygon is cheap to fold into no matter how it's done).
//  - BM_Merge_LeftFold (Geometry::union_shapes, one part at a time into a
//    growing accumulator) vs BM_Merge_Balanced (boolean_shapes' balanced
//    pairwise union_all) on identical input.
//  - Rendering overhead of free shapes on the real AES 1x1 fixture, added
//    to the top Layout or to one standard cell's Abstract (so they repeat
//    in every placement of that cell), through HierarchyResolverStage's
//    cold collect and a warm-tier pan frame.

#include "../../core/placement_geometry.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "../view_render_pipeline.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    // C clusters on a pitch-40 grid, each a plus of 4 overlapping rects
    // offset by `shift` - disjoint from every other cluster.
    Shape clustered_layer(int64_t rect_count, int64_t shift)
    {
        const int64_t clusters = std::max<int64_t>(rect_count / 4, 1);
        int64_t side = 1;
        while (side * side < clusters)
            ++side;
        Shape shape;
        shape.rects.reserve(static_cast<size_t>(clusters * 4));
        for (int64_t i = 0; i < clusters; ++i)
        {
            const int64_t x = (i % side) * 40 + shift;
            const int64_t y = (i / side) * 40 + shift;
            shape.rects.push_back(Rect{.ll = {x, y + 8}, .ur = {x + 24, y + 16}});
            shape.rects.push_back(Rect{.ll = {x + 8, y}, .ur = {x + 16, y + 24}});
            shape.rects.push_back(Rect{.ll = {x + 4, y + 4}, .ur = {x + 20, y + 20}});
            shape.rects.push_back(Rect{.ll = {x + 10, y + 2}, .ur = {x + 14, y + 22}});
        }
        return shape;
    }

    void set_result_counters(benchmark::State &state, int64_t rect_count, const AreaGeometry &result)
    {
        state.counters["rects_in"] = static_cast<double>(rect_count);
        state.counters["rects_out"] = static_cast<double>(result.rects.size());
        state.counters["polygons_out"] = static_cast<double>(result.polygons.size());
    }

    void BM_ShapeBoolean(benchmark::State &state, BooleanOp op)
    {
        const int64_t n = state.range(0);
        const Shape a = clustered_layer(n, 0);
        const Shape b = clustered_layer(n, 6);
        AreaGeometry result;
        for (auto _ : state)
        {
            result = Geometry::boolean_shapes({&a}, {&b}, op);
            benchmark::DoNotOptimize(result);
        }
        set_result_counters(state, 2 * n, result);
    }

    void BM_Merge_LeftFold(benchmark::State &state)
    {
        const int64_t n = state.range(0);
        const Shape shape = clustered_layer(n, 0);
        size_t polygons = 0;
        for (auto _ : state)
        {
            const auto merged = Geometry::union_shapes({&shape});
            polygons = merged ? merged->size() : 0;
            benchmark::DoNotOptimize(polygons);
        }
        state.counters["rects_in"] = static_cast<double>(n);
        state.counters["polygons_out"] = static_cast<double>(polygons);
    }

    void BM_Merge_Balanced(benchmark::State &state)
    {
        const int64_t n = state.range(0);
        const Shape shape = clustered_layer(n, 0);
        AreaGeometry result;
        for (auto _ : state)
        {
            result = Geometry::boolean_shapes({&shape}, {}, BooleanOp::Or);
            benchmark::DoNotOptimize(result);
        }
        set_result_counters(state, n, result);
    }

    void BM_ShapeToRects(benchmark::State &state)
    {
        const int64_t n = state.range(0);
        const Shape shape = clustered_layer(n, 0);
        std::vector<Rect> result;
        for (auto _ : state)
        {
            result = Geometry::shape_to_rects(shape, FractureDirection::Horizontal);
            benchmark::DoNotOptimize(result);
        }
        state.counters["rects_in"] = static_cast<double>(n);
        state.counters["rects_out"] = static_cast<double>(result.size());
    }

    // A single region with n/4 holes - the fracture's hardest case, since
    // every hole adds cut lines that split every strip it crosses.
    void BM_ShapeToRects_Holes(benchmark::State &state)
    {
        const int64_t n = state.range(0);
        const Shape holes = clustered_layer(n, 0);
        const std::optional<Rect> box = Geometry::bbox(holes);
        const Shape plate{.rects = {Rect{.ll = {box->ll.x - 10, box->ll.y - 10}, .ur = {box->ur.x + 10, box->ur.y + 10}}}};
        const AreaGeometry holed = Geometry::boolean_shapes({&plate}, {&holes}, BooleanOp::Not);
        const Shape shape{.polygons = holed.polygons, .rects = holed.rects};
        std::vector<Rect> result;
        for (auto _ : state)
        {
            result = Geometry::shape_to_rects(shape, FractureDirection::Horizontal);
            benchmark::DoNotOptimize(result);
        }
        state.counters["holes"] = static_cast<double>(n / 4);
        state.counters["rects_out"] = static_cast<double>(result.size());
    }

    void BM_ShapeSize(benchmark::State &state, int64_t amount)
    {
        const int64_t n = state.range(0);
        const Shape shape = clustered_layer(n, 0);
        AreaGeometry result;
        for (auto _ : state)
        {
            result = *Geometry::size_shape(shape, amount, amount);
            benchmark::DoNotOptimize(result);
        }
        set_result_counters(state, n, result);
    }

    void BM_ShapeOutlinePaths(benchmark::State &state)
    {
        const int64_t n = state.range(0);
        const Shape shape = clustered_layer(n, 0);
        std::vector<Path> result;
        for (auto _ : state)
        {
            result = Geometry::shape_outline_paths(shape, 2);
            benchmark::DoNotOptimize(result);
        }
        state.counters["rects_in"] = static_cast<double>(n);
        state.counters["paths_out"] = static_cast<double>(result.size());
    }

    // --- Rendering free shapes on the real AES fixture ---

    enum class FreeShapeTarget
    {
        Layout, // the top Layout's own free shapes - drawn once
        Cell,   // the most-placed standard cell's Abstract - drawn in every placement of it
    };

    struct FreeShapeFixture
    {
        Root root;
        LayoutId layout_id;
        int64_t placements_of_target = 1;
    };

    // A private copy of the cached AES fixture (other benchmarks share the
    // cached one, so it must never be mutated) with `count` free rects on
    // the design's routing layers.
    const FreeShapeFixture &free_shape_fixture(const TileConfig &config, FreeShapeTarget target, int64_t count)
    {
        static std::map<std::string, FreeShapeFixture> cache;
        const std::string key = std::string(config.label) + (target == FreeShapeTarget::Layout ? "/layout/" : "/cell/") + std::to_string(count);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;

        const AesScalingFixture &aes = cached_aes_scaling_fixture(config);
        FreeShapeFixture fixture{.root = aes.root, .layout_id = aes.layout_id};
        Root &root = fixture.root;

        std::vector<LayerId> routing_layers;
        for (LayerId layer : root.get_technology_layers(root.get_technology_ids().front()))
            if (root.get_layer(layer)->type == "ROUTING" && routing_layers.size() < 4)
                routing_layers.push_back(layer);

        AbstractId cell_abstract;
        Rect area = layout_declared_bbox(root, fixture.layout_id);
        if (target == FreeShapeTarget::Cell)
        {
            std::map<DesignId, int64_t> placements_per_design;
            for (PlacementId placement : root.get_layout_placements(fixture.layout_id))
                ++placements_per_design[root.get_placement(placement)->reference_design];
            const auto most_placed = std::max_element(placements_per_design.begin(), placements_per_design.end(),
                                                      [](const auto &a, const auto &b)
                                                      { return a.second < b.second; });
            cell_abstract = root.get_design_abstract(most_placed->first);
            fixture.placements_of_target = most_placed->second;
            area = *Geometry::bbox(*root.get_shape(root.get_abstract_boundary(cell_abstract)));
        }

        int64_t side = 1;
        while (side * side < count)
            ++side;
        const int64_t step_x = std::max<int64_t>((area.ur.x - area.ll.x) / std::max<int64_t>(side, 1), 2);
        const int64_t step_y = std::max<int64_t>((area.ur.y - area.ll.y) / std::max<int64_t>(side, 1), 2);
        for (int64_t i = 0; i < count; ++i)
        {
            const int64_t x = area.ll.x + (i % side) * step_x;
            const int64_t y = area.ll.y + (i / side) * step_y;
            ShapeData data{
                .layer = routing_layers[static_cast<size_t>(i) % routing_layers.size()],
                .rects = {Rect{.ll = {x, y}, .ur = {x + std::max<int64_t>(step_x * 2 / 3, 1), y + std::max<int64_t>(step_y * 2 / 3, 1)}}},
            };
            if (target == FreeShapeTarget::Layout)
                data.in_layout = fixture.layout_id;
            else
                data.in_abstract = cell_abstract;
            root.create_shape(std::move(data));
        }
        root.bump_mutation_version();
        return cache.emplace(key, std::move(fixture)).first->second;
    }

    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;

    void BM_HierarchyResolver_FreeShapes(benchmark::State &state, TileConfig config, FreeShapeTarget target)
    {
        const FreeShapeFixture &fixture = free_shape_fixture(config, target, state.range(0));
        const ViewLayerSetHandle view_layers = std::make_shared<const ViewLayerSet>(
            ViewLayerSet::build_for_technology(fixture.root, fixture.root.get_technology_ids().front()));
        const ViewRenderOptions options{
            .root = &fixture.root,
            .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id},
            .hierarchy_depth = 1,
        };
        for (auto _ : state)
        {
            HierarchyResolverRunner runner{"bm_hierarchy_resolver_free_shapes"};
            const HierarchyResolverOutput &output = runner.run(view_layers, 0, options);
            benchmark::DoNotOptimize(output.view_data.size());
        }

        // Untimed proof the free shapes were really collected (per distinct
        // Abstract/Layout, not per placement) - not a vacuous benchmark.
        HierarchyResolverRunner check_runner{"bm_hierarchy_resolver_free_shapes_check"};
        const HierarchyResolverOutput &check = check_runner.run(view_layers, 0, options);
        size_t collected = 0;
        for (const auto &[id, data] : check.view_data)
            for (const auto &[view_layer, shapes] : *data.shapes)
                if (const ViewLayerData *layer = view_layers->get(view_layer); layer && layer->purpose == ViewLayerPurpose::CUSTOM_SHAPE)
                    collected += shapes.size();
        state.counters["custom_shapes_collected"] = static_cast<double>(collected);
        state.counters["free_shapes"] = static_cast<double>(state.range(0));
        state.counters["drawn_instances"] = static_cast<double>(state.range(0) * fixture.placements_of_target);
    }

    // Same 16-step pan as BM_WarmTier (warm_tier_benchmark.cpp), whole
    // design framed at 1/10 of its size per step.
    void BM_WarmTier_FreeShapes(benchmark::State &state, TileConfig config, FreeShapeTarget target)
    {
        const FreeShapeFixture &fixture = free_shape_fixture(config, target, state.range(0));
        const Rect design_bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = design_bbox.ur.x - design_bbox.ll.x;
        const int64_t height = design_bbox.ur.y - design_bbox.ll.y;
        const int64_t window_w = width / 10;
        const int64_t window_h = height / 10;
        constexpr int kPanSteps = 16;
        std::vector<Rect> viewports;
        for (int i = 0; i < kPanSteps; ++i)
        {
            const double t = static_cast<double>(i) / (kPanSteps - 1);
            const int64_t cx = design_bbox.ll.x + width / 4 + static_cast<int64_t>(t * static_cast<double>(width) / 2);
            const int64_t cy = design_bbox.ll.y + height / 4 + static_cast<int64_t>(t * static_cast<double>(height) / 2);
            viewports.push_back(Rect{.ll = Point{cx - window_w / 2, cy - window_h / 2}, .ur = Point{cx + window_w / 2, cy + window_h / 2}});
        }
        const double scale = 1000.0 / static_cast<double>(std::max<int64_t>(window_w, 1));
        auto options_for = [&](const Rect &viewport)
        {
            return ViewRenderOptions{
                .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
                .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
                .viewport = viewport, .scale = scale,
            };
        };

        ViewRenderPipeline pipeline{"bm_warm_tier_free_shapes"};
        pipeline.run(&fixture.root, options_for(viewports.front())); // untimed warm-up
        int pan_index = 0;
        for (auto _ : state)
        {
            pan_index = (pan_index + 1) % kPanSteps;
            const ViewRenderPipeline::WarmOutput output = pipeline.run(&fixture.root, options_for(viewports[pan_index]));
            int frame_width = output.frame->buffer.width;
            benchmark::DoNotOptimize(frame_width);
        }

        // Untimed proof the free shapes were really drawn: a checksum of the
        // first pan step's frame, which must differ from the 0-shape run's.
        const ViewRenderPipeline::WarmOutput check = pipeline.run(&fixture.root, options_for(viewports.front()));
        const PixelBuffer &buffer = check.frame->buffer;
        uint32_t checksum = 2166136261u; // FNV-1a
        for (int y = 0; y < buffer.height; ++y)
            for (size_t x = 0; x < static_cast<size_t>(buffer.width) * 4; ++x)
                checksum = (checksum ^ buffer.data[static_cast<size_t>(y) * buffer.row_bytes + x]) * 16777619u;
        state.counters["frame_checksum"] = static_cast<double>(checksum % 1000000007u);
        state.counters["free_shapes"] = static_cast<double>(state.range(0));
        state.counters["drawn_instances"] = static_cast<double>(state.range(0) * fixture.placements_of_target);
    }
}

namespace le::benchmarks
{
    void register_shape_ops_benchmarks()
    {
        const std::vector<int64_t> sizes = {1'000, 10'000, 100'000};
        auto with_sizes = [&](benchmark::internal::Benchmark *bm, const std::vector<int64_t> &values)
        {
            for (int64_t n : values)
                bm->Arg(n);
            bm->Unit(benchmark::kMillisecond);
        };

        with_sizes(benchmark::RegisterBenchmark("BM_ShapeBoolean/Or", BM_ShapeBoolean, BooleanOp::Or), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeBoolean/And", BM_ShapeBoolean, BooleanOp::And), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeBoolean/Not", BM_ShapeBoolean, BooleanOp::Not), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_Merge_LeftFold", BM_Merge_LeftFold), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_Merge_Balanced", BM_Merge_Balanced), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeToRects", BM_ShapeToRects), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeToRects_Holes", BM_ShapeToRects_Holes), {1'000, 10'000});
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeSize/Grow", BM_ShapeSize, int64_t{2}), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeSize/Shrink", BM_ShapeSize, int64_t{-2}), sizes);
        with_sizes(benchmark::RegisterBenchmark("BM_ShapeOutlinePaths", BM_ShapeOutlinePaths), sizes);

        const TileConfig config = kAesScalingTileConfigs[0]; // 1x1 - the cost measured is per free shape, not per tile
        with_sizes(benchmark::RegisterBenchmark("BM_HierarchyResolver_FreeShapes/Layout/1x1", BM_HierarchyResolver_FreeShapes, config, FreeShapeTarget::Layout),
                   {0, 10'000, 100'000});
        with_sizes(benchmark::RegisterBenchmark("BM_HierarchyResolver_FreeShapes/Cell/1x1", BM_HierarchyResolver_FreeShapes, config, FreeShapeTarget::Cell),
                   {0, 10, 100});
        with_sizes(benchmark::RegisterBenchmark("BM_WarmTier_FreeShapes/Layout/1x1", BM_WarmTier_FreeShapes, config, FreeShapeTarget::Layout),
                   {0, 10'000, 100'000});
        with_sizes(benchmark::RegisterBenchmark("BM_WarmTier_FreeShapes/Cell/1x1", BM_WarmTier_FreeShapes, config, FreeShapeTarget::Cell),
                   {0, 10, 100});
    }
}
