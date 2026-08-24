#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include <memory>
#include <tuple>
#include <vector>

namespace le::flow
{
    /// @brief One physical routing layer's worth of Terminal-Port geometry -
    /// a Shape whose own `.layer` matches `physical_layer` is emitted
    /// directly; everything else is skipped before any real work
    /// (`expand_iterates`, path-outline computation) happens. No via
    /// resolution here at all - see `GenerateViasOnLayerStage`'s own
    /// comment for why that's a separate, cut-layer-keyed task instead.
    ///
    /// One instance per physical layer (owned by `GenerateAndFilterPipeline`),
    /// each single-threaded start to finish - the coarser task granularity
    /// TASKFLOW_EXPERIMENT.md's per-layer redesign asked for, replacing the
    /// earlier design's own internal `flow::parallel_chunks` fan-out.
    ///
    /// Every Terminal in the Abstract is still walked by every one of
    /// these per-layer instances (there's no per-layer index to jump
    /// straight to the matching ones) - a `ShapesByLayerIndex`
    /// pre-bucketing pass was tried here and reverted: it replaced this
    /// redundant-but-cheap (a plain integer compare, run inside an
    /// already-parallel task) walk with a single-threaded pre-pass that
    /// measured as a net wash or worse - see TASKFLOW_EXPERIMENT.md's
    /// "Pre-process step" entry for the real numbers and why.
    ///
    /// Cache key `{abstract_id, view_layers.generation(), root.mutation_version()}` -
    /// identical shape to the real `GenerateAbstractShapesStage`'s own key
    /// (src/pipeline/stages/generate_abstract_shapes_stage.hpp) - pan/zoom
    /// never invalidates this tier, only a real Abstract switch, ViewLayerSet
    /// rebuild, or database mutation does.
    class GenerateTerminalsOnLayerStage : public le::ShapeGenerationStage
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, AbstractId abstract_id, LayerId physical_layer, const ViewLayerSet &view_layers)
        {
            return stage_.get(std::tuple{abstract_id, view_layers.generation(), root.mutation_version()}, [&]
            {
                std::vector<RenderedShape> shapes;
                const ViewLayerId view_layer = view_layers.find(physical_layer, ViewLayerPurpose::TERMINAL);

                for (auto terminal_id : root.get_abstract_terminals(abstract_id))
                {
                    // Single pass over this Terminal's Ports/Shapes - see
                    // the real GenerateAbstractShapesStage's own comment
                    // for why label placement accumulates combined
                    // geometry per layer rather than per Port. Simpler
                    // here than the original: this whole pass only ever
                    // sees one layer's worth of geometry (everything else
                    // was skipped before reaching this point), so there's
                    // only ever one LabelAccumulator-equivalent, not a
                    // map of them.
                    Shape combined_for_label;
                    size_t first_shape_index = shapes.size();
                    bool any_shape_on_this_layer = false;

                    for (auto port_id : root.get_terminal_ports(terminal_id))
                    {
                        for (const auto &shape_id : root.get_terminal_port_shapes(port_id))
                        {
                            const auto *raw_shape = root.get_shape(shape_id);
                            if (!raw_shape || raw_shape->layer != physical_layer)
                                continue;

                            const Shape shape = expand_iterates(*raw_shape);
                            if (!any_shape_on_this_layer)
                            {
                                first_shape_index = shapes.size();
                                any_shape_on_this_layer = true;
                            }

                            combined_for_label.rects.insert(combined_for_label.rects.end(), shape.rects.begin(), shape.rects.end());
                            combined_for_label.polygons.insert(combined_for_label.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                            combined_for_label.paths.insert(combined_for_label.paths.end(), shape.paths.begin(), shape.paths.end());

                            shapes.push_back(RenderedShape{.shape = shape, .view_layer = view_layer, .origin = SelectionRef{terminal_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
                        }
                    }

                    if (!any_shape_on_this_layer)
                        continue;

                    if (const TerminalData *terminal = root.get_terminal(terminal_id))
                    {
                        const Point location = Geometry::get_label_location(combined_for_label);
                        shapes[first_shape_index].shape.texts.push_back(Text{
                            .label = terminal->name,
                            .location = location,
                            .size = Geometry::local_width_at(combined_for_label, location),
                        });
                    }
                }

                return shapes;
            });
        }

        uint64_t version() const override { return stage_.version(); }
        uint64_t call_count() const override { return stage_.call_count(); }

    private:
        // Ported unchanged from the real GenerateAbstractShapesStage - see
        // its own comment (UPDATES.md 12 Phase 1's ITERATE rework).
        static Shape expand_iterates(Shape shape)
        {
            constexpr int kMaxReasonableCount = 1'000'000;

            for (const RectIterate &it : shape.rect_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.rects.reserve(shape.rects.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                        shape.rects.push_back(Rect{
                            .ll = Point{.x = it.rect.ll.x + ix * it.space_x, .y = it.rect.ll.y + iy * it.space_y},
                            .ur = Point{.x = it.rect.ur.x + ix * it.space_x, .y = it.rect.ur.y + iy * it.space_y},
                        });
            }
            shape.rect_iterates.clear();

            for (const PathIterate &it : shape.path_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.paths.reserve(shape.paths.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.paths.push_back(Path{.polygon = Geometry::transform(it.path.polygon, offset), .width = it.path.width});
                    }
            }
            shape.path_iterates.clear();

            for (const PolygonIterate &it : shape.polygon_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.polygons.reserve(shape.polygons.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.polygons.push_back(Geometry::transform(it.polygon, offset));
                    }
            }
            shape.polygon_iterates.clear();

            return shape;
        }

        static std::shared_ptr<const std::vector<std::vector<Polygon>>> compute_path_outlines(const Shape &shape)
        {
            std::vector<std::vector<Polygon>> outlines;
            outlines.reserve(shape.paths.size());
            for (const Path &path : shape.paths)
                outlines.push_back(Geometry::path_to_polygons(path));
            return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
        }

        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> stage_;
    };
}
