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
    /// @brief One physical routing layer's worth of Obstruction geometry -
    /// mirrors GenerateTerminalsOnLayerStage's own comment (same
    /// per-layer-input/no-via-resolution/single-threaded/cache-key
    /// shape, and the same reverted `ShapesByLayerIndex` pre-bucketing
    /// attempt - see that class's own comment and
    /// TASKFLOW_EXPERIMENT.md's "Pre-process step" entry), simpler since
    /// Obstructions have no per-layer label bookkeeping to preserve.
    class GenerateObstructionsOnLayerStage : public le::ShapeGenerationStage
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, AbstractId abstract_id, LayerId physical_layer, const ViewLayerSet &view_layers)
        {
            return stage_.get(std::tuple{abstract_id, view_layers.generation(), root.mutation_version()}, [&]
            {
                std::vector<RenderedShape> shapes;
                const ViewLayerId view_layer = view_layers.find(physical_layer, ViewLayerPurpose::OBSTRUCTION);

                for (auto obstruction_id : root.get_abstract_obstructions(abstract_id))
                {
                    for (const auto &shape_id : root.get_obstruction_shapes(obstruction_id))
                    {
                        const auto *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape || raw_shape->layer != physical_layer)
                            continue;

                        const Shape shape = expand_iterates(*raw_shape);
                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = view_layer, .origin = SelectionRef{obstruction_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
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
