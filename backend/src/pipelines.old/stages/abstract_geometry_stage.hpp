#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "via_shapes.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/pipeline/stages/generate_abstract_shapes_stage.hpp
    /// (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) - collects
    /// every Shape from an Abstract's Terminals' Ports, its Obstructions,
    /// and its boundary polygon, each resolved to its ViewLayerId, in
    /// dbu-space. compute()'s own body is unchanged from the original
    /// stage's run() lambda - see that file's own doc comment for the full
    /// per-Shape/label/path-outline/via-expansion behavior; only the
    /// caching mechanism differs.
    ///
    /// The original stage's key was `{AbstractId, ViewLayerSet::
    /// generation(), Root::mutation_version()}`. MemoizingStage's own
    /// `data_version` only ever gets compared by inequality - `InputData`
    /// (AbstractId here) itself is never inspected - so `data_version_for`
    /// below folds all three of the original key's components together via
    /// combine_versions, the same way every other converted stage in this
    /// module derives its own data_version (see the migration plan's
    /// PipelineOptions design section). options.epoch carries the other
    /// two components; only AbstractId's own {index, generation} needs to
    /// be passed in separately since it's this stage's own InputData, not
    /// part of PipelineOptions.
    class AbstractGeometryStage : public MemoizingStage<AbstractId, std::vector<RenderedShape>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        /// @brief Derives this stage's data_version for a given AbstractId
        /// under the given options.epoch - callers (e.g. AbstractShapePipeline)
        /// compute this once per try_put rather than duplicating the
        /// combine_versions call at every call site.
        static uint64_t data_version_for(AbstractId abstract_id, const PipelineOptions &options)
        {
            return combine_versions(abstract_id.index, abstract_id.generation, options.epoch.view_layers_generation, options.epoch.root_mutation_version);
        }

    protected:
        std::vector<RenderedShape> compute(const AbstractId &abstract_id, const PipelineOptions &options) override
        {
            const Root &root = *options.ctx.root;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            std::vector<RenderedShape> shapes;
            const auto &terminals = root.get_abstract_terminals(abstract_id);
            const auto &obstructions = root.get_abstract_obstructions(abstract_id);
            shapes.reserve(terminals.size() + obstructions.size());

            auto resolve = [&](const Shape &shape, ViewLayerPurpose purpose)
            {
                return view_layers.find(shape.layer, purpose);
            };

            // UPDATES.md 12 Phase 1's ITERATE rework - LEFReader stores
            // RECT/PATH/POLYGON ITERATE statements raw (rect_iterates/
            // path_iterates/polygon_iterates) rather than pre-expanding
            // them at parse time, so LEFWriter can re-emit compact
            // ITERATE syntax on write. This is the one place downstream
            // Pipeline/Renderer code needs to know about that - expands
            // each into concrete Rect/Path/Polygon entries appended to
            // the same Shape's rects/paths/polygons, so everything below
            // (label accumulation, compute_path_outlines, RenderedShape
            // itself) sees a conventional fully-expanded Shape exactly
            // as before this rework. Bounds each statement's own num_x*num_y the same
            // way LEFReader::safe_iteration_count did at parse time -
            // defense in depth, not just trusting the database's
            // already-validated values.
            auto expand_iterates = [](Shape shape)
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
                            shape.paths.push_back(Path{.width = it.path.width, .polygon = Geometry::transform(it.path.polygon, offset)});
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
            };

            // One buffered outline per path, computed once here (see
            // RenderedShape::path_outlines's own comment for why this
            // is the right cache tier, and why it's shared_ptr-wrapped).
            auto compute_path_outlines = [](const Shape &shape)
            {
                std::vector<std::vector<Polygon>> outlines;
                outlines.reserve(shape.paths.size());
                for (const Path &path : shape.paths)
                    outlines.push_back(Geometry::path_to_polygons(path));
                return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
            };

            for (auto terminal_id : terminals)
            {
                // Single pass over this Terminal's Ports/Shapes: push
                // each one into `shapes` immediately (preserving its own
                // layer_name/ViewLayerId - combining shapes across
                // layers would break per-layer visibility for
                // multi-layer Terminals, and coarsen viewport/sub-pixel
                // culling to a combined bbox instead of each piece's
                // own), while also accumulating just the geometry
                // primitives (not whole Shapes) into a per-layer_name
                // combined Shape for label placement only. Each layer's
                // label is attached after the loop, once its location
                // is known, to the first Shape pushed for *that layer*
                // (remembered by index - safe across any reallocation
                // `shapes.push_back` triggers, since vector indices
                // stay valid across growth, only references/iterators
                // taken before it don't).
                struct LabelAccumulator
                {
                    Shape combined;
                    size_t first_shape_index = 0;
                };
                std::unordered_map<LayerId, LabelAccumulator> by_layer;

                for (auto port_id : root.get_terminal_ports(terminal_id))
                {
                    for (const auto &shape_id : root.get_terminal_port_shapes(port_id))
                    {
                        const auto *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape)
                            continue;
                        const Shape shape = expand_iterates(*raw_shape);
                        auto [it, inserted] = by_layer.try_emplace(shape.layer);
                        if (inserted)
                            it->second.first_shape_index = shapes.size();

                        Shape &combined = it->second.combined;
                        combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                        combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                        combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::TERMINAL), .origin = SelectionRef{terminal_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
                        append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes);
                    }
                }

                if (by_layer.empty())
                    continue;

                if (const TerminalData *terminal = root.get_terminal(terminal_id))
                {
                    for (const auto &[layer_id, acc] : by_layer)
                    {
                        const Point location = Geometry::get_label_location(acc.combined);
                        shapes[acc.first_shape_index].shape.texts.push_back(Text{
                            .label = terminal->name,
                            .location = location,
                            .size = Geometry::local_width_at(acc.combined, location),
                        });
                    }
                }
            }

            for (auto obstruction_id : obstructions)
            {
                for (const auto &shape_id : root.get_obstruction_shapes(obstruction_id))
                {
                    const auto *raw_shape = root.get_shape(shape_id);
                    if (!raw_shape)
                        continue;
                    const Shape shape = expand_iterates(*raw_shape);
                    shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::OBSTRUCTION), .origin = SelectionRef{obstruction_id}, .shape_id = shape_id, .path_outlines = compute_path_outlines(shape)});
                    append_via_shapes(root, shape, ViewLayerPurpose::OBSTRUCTION, view_layers, LayoutId{}, shapes);
                }
            }

            if (const Shape *boundary_shape = root.get_shape(root.get_abstract_boundary(abstract_id)))
            {
                shapes.push_back(RenderedShape{
                    .shape = *boundary_shape,
                    .view_layer = view_layers.boundary_view_layer(),
                });
            }

            return shapes;
        }
    };
}
